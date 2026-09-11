//===-- DSPICPeephole.cpp - the fused forms cc1 prints ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Trellis L1f-a (session 85, post-close). Three rewrites over physical-register code,
// each one word for two, each a form the GPL pic30 `as` accepted before it was printed
// (tools/dspic-llvm/prints/l1f/probe5.log):
//
//   mov #lit10,wN ; return          ->  retlw #lit10,wN      (retlw.b for a byte move)
//   mov.w wS,wD ; mov.w wS+1,wD+1   ->  mov.d wS,wD          (S, D even; D != S+1)
//   push wE ; push wE+1             ->  push.d wE            (E even)
//
// The pair rewrites read the PRINTED register number (the encoding), since the internal
// names inherited from MSP430 do not run in w order. Runs after PEI, before the branch
// selector, so the pushes of a call sequence and the return of every block are physical.
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "DSPICInstrInfo.h"
#include "DSPICSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h" // trellis session 103: dbgs(), for -dspic-cmp-fuse-why
#include "llvm/IR/Function.h"     // trellis session 104: hasOptSize(), the level policy
#include "llvm/IR/Analysis.h"

using namespace llvm;

#define DEBUG_TYPE "dspic-peephole"

STATISTIC(NumRetlw, "Number of mov+return fused into retlw");
STATISTIC(NumMovd, "Number of register pairs fused into mov.d");
STATISTIC(NumPushd, "Number of push pairs fused into push.d");
STATISTIC(NumSkip, "Number of bit test + branch pairs fused into btsc/btss");
STATISTIC(NumMovdMem, "Number of word-pair loads/stores fused into mov.d with memory");
STATISTIC(NumByteFile, "Number of byte global load/store materialize+indirect fused into the direct WREG form");
STATISTIC(NumTailBra, "Number of rcall+return fused into a tail bra");
STATISTIC(NumFileALU, "Number of materialize+memory-ALU pairs fused into the direct f,WREG form");

namespace {

class DSPICPeepholeImpl {
  const DSPICInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;

  unsigned wNumber(Register R) const { return TRI->getEncodingValue(R); }
  // The word register printed as w<N+1>, given the one printed as w<N>; w14/w15 never.
  Register nextWord(Register R) const {
    unsigned N = wNumber(R);
    if (N >= 13)
      return Register();
    static const MCPhysReg Words[] = {
        DSPIC::R12, DSPIC::R13, DSPIC::R14, DSPIC::R15, DSPIC::R11,
        DSPIC::W5,  DSPIC::W6,  DSPIC::W7,  DSPIC::R10, DSPIC::R9,
        DSPIC::R8,  DSPIC::R7,  DSPIC::R6,  DSPIC::R5};
    for (MCPhysReg Cand : Words)
      if (wNumber(Cand) == N + 1)
        return Cand;
    return Register();
  }

  bool fuseRetlw(MachineBasicBlock &MBB);
  bool fusePairs(MachineBasicBlock &MBB);
  bool fuseSkip(MachineFunction &MF);
  bool fuseMovdMem(MachineBasicBlock &MBB);
  bool fuseByteFile(MachineBasicBlock &MBB);
  bool fuseTailCall(MachineBasicBlock &MBB);
  bool fuseFileALU(MachineBasicBlock &MBB);

public:
  bool runOnMachineFunction(MachineFunction &MF);
};

class DSPICPeepholeLegacyPass : public MachineFunctionPass {
public:
  static char ID;
  DSPICPeepholeLegacyPass() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override {
    return DSPICPeepholeImpl().runOnMachineFunction(MF);
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
  StringRef getPassName() const override {
    return "DSPIC Peephole (fused forms)";
  }
};
char DSPICPeepholeLegacyPass::ID = 0;

} // namespace

// mov #imm,wN immediately before `return`, imm in the 10-bit literal (a byte move's
// immediate masked to 8 bits): retlw sets wN and returns in one word.
bool DSPICPeepholeImpl::fuseRetlw(MachineBasicBlock &MBB) {
  if (MBB.size() < 2)
    return false;
  MachineInstr &Ret = MBB.back();
  if (Ret.getOpcode() != DSPIC::RET)
    return false;
  MachineInstr &Mov = *std::prev(Ret.getIterator());
  // the register-immediate move or MSP430's constant-generator form (0, 1, 2, 4, 8, -1)
  unsigned Op = Mov.getOpcode();
  bool Byte = Op == DSPIC::MOV8ri || Op == DSPIC::MOV8rc;
  if (!Byte && Op != DSPIC::MOV16ri && Op != DSPIC::MOV16rc)
    return false;
  if (!Mov.getOperand(1).isImm())
    return false;
  int64_t Imm = Mov.getOperand(1).getImm();
  if (Byte)
    Imm &= 0xff;
  if (Imm < 0 || Imm > 1023)
    return false;
  Register Rd = Mov.getOperand(0).getReg();
  MachineInstrBuilder B =
      BuildMI(MBB, Ret, Ret.getDebugLoc(),
              TII->get(Byte ? DSPIC::RETLW8 : DSPIC::RETLW16), Rd)
          .addImm(Imm);
  // the return's implicit uses ride along, except the return value retlw itself defines
  // (an implicit use of an undefined register is a verifier error)
  for (const MachineOperand &MO : Ret.implicit_operands())
    if (!MO.isReg() || !TRI->regsOverlap(MO.getReg(), Rd))
      B.add(MO);
  Mov.eraseFromParent();
  Ret.eraseFromParent();
  ++NumRetlw;
  return true;
}

// Two adjacent word moves of a register pair -> mov.d (S and D even, D != S+1 so the first
// move does not feed the second; mov.d reads both sources first); two adjacent pushes of a
// pair -> push.d (the pair is pushed low word first, the order two pushes give).
bool DSPICPeepholeImpl::fusePairs(MachineBasicBlock &MBB) {
  bool Changed = false;
  auto I = MBB.begin();
  while (I != MBB.end()) {
    auto J = std::next(I);
    if (J == MBB.end())
      break;
    MachineInstr *Fused = nullptr;
    if (I->getOpcode() == DSPIC::MOV16rr && J->getOpcode() == DSPIC::MOV16rr) {
      // the even (low) move may come first or second; the FIRST move must not write
      // the SECOND move's source, since mov.d reads both sources before writing
      auto *Lo = &*I, *Hi = &*J;
      if (wNumber(Lo->getOperand(0).getReg()) % 2)
        std::swap(Lo, Hi);
      Register D = Lo->getOperand(0).getReg(), S = Lo->getOperand(1).getReg();
      Register D2 = Hi->getOperand(0).getReg(), S2 = Hi->getOperand(1).getReg();
      Register FirstDef = I->getOperand(0).getReg(), SecondSrc = J->getOperand(1).getReg();
      if (wNumber(D) % 2 == 0 && wNumber(S) % 2 == 0 && D2 == nextWord(D) &&
          S2 == nextWord(S) && D2.isValid() && S2.isValid() && FirstDef != SecondSrc) {
        bool Kill = Lo->getOperand(1).isKill() && Hi->getOperand(1).isKill();
        Fused = BuildMI(MBB, I, I->getDebugLoc(), TII->get(DSPIC::MOVDrr), D)
                    .addReg(S, getKillRegState(Kill))
                    .addReg(D2, RegState::ImplicitDefine)
                    .addReg(S2, RegState::Implicit | getKillRegState(Kill));
        ++NumMovd;
      }
    } else if (I->getOpcode() == DSPIC::PUSH16r &&
               J->getOpcode() == DSPIC::PUSH16r) {
      Register A = I->getOperand(0).getReg(), B = J->getOperand(0).getReg();
      if (wNumber(A) % 2 == 0 && B == nextWord(A) && B.isValid()) {
        bool Kill = I->getOperand(0).isKill() && J->getOperand(0).isKill();
        Fused = BuildMI(MBB, I, I->getDebugLoc(), TII->get(DSPIC::PUSHD))
                    .addReg(A, getKillRegState(Kill))
                    .addReg(B, RegState::Implicit | getKillRegState(Kill));
        ++NumPushd;
      }
    }
    if (!Fused) {
      ++I;
      continue;
    }
    auto K = std::next(J);
    I->eraseFromParent();
    J->eraseFromParent();
    I = K;
    Changed = true;
  }
  return Changed;
}

// L1f-g (trellis session 87): a bit test whose branch jumps over exactly ONE instruction is
// the skip form -- `btst wN,#k ; bra z,.L ; <one instruction> ; .L:` becomes `btsc wN,#k ; <one
// instruction>` (`bra z` jumps when the bit is CLEAR, so the instruction runs when it is SET:
// skip it if clear), and `bra nz` becomes `btss`. Only the test+branch pair is rewritten: the
// skipped instruction stays in its own block and the CFG is unchanged (the block still reaches
// both successors), so the branch target must be the block after the skipped one in layout, the
// skipped block must have no other predecessor, and it must reach the target -- by falling
// through, or by never continuing (a return, a tail call, a jump). The skipped instruction is
// one word (the hardware skips one word) and reads no flag (the skip forms write none, where
// the test wrote Z). Every pair the GPL `as` took: prints/l1f/probe7.log.
bool DSPICPeepholeImpl::fuseSkip(MachineFunction &MF) {
  bool Changed = false;
  for (auto AI = MF.begin(); AI != MF.end(); ++AI) {
    MachineBasicBlock &A = *AI;
    if (A.size() < 2)
      continue;
    MachineInstr &Br = A.back();
    if (Br.getOpcode() != DSPIC::JCC)
      continue;
    unsigned CC = Br.getOperand(1).getImm();
    if (CC != DSPICCC::COND_E && CC != DSPICCC::COND_NE)
      continue;
    bool Clear = CC == DSPICCC::COND_E; // `bra z`: taken when the bit is clear
    MachineInstr &Tst = *std::prev(Br.getIterator());
    unsigned Skip = 0;
    switch (Tst.getOpcode()) {
    case DSPIC::BTST16ri: Skip = Clear ? DSPIC::BTSC16ri : DSPIC::BTSS16ri; break;
    case DSPIC::BTST16n:  Skip = Clear ? DSPIC::BTSC16n  : DSPIC::BTSS16n;  break;
    case DSPIC::BTST16f:  Skip = Clear ? DSPIC::BTSC16f  : DSPIC::BTSS16f;  break;
    case DSPIC::BTST8f:   Skip = Clear ? DSPIC::BTSC8f   : DSPIC::BTSS8f;   break;
    default: break;
    }
    if (!Skip)
      continue;
    auto BI = std::next(AI);
    if (BI == MF.end())
      continue;
    MachineBasicBlock &B = *BI;
    auto LI = std::next(BI);
    if (LI == MF.end() || Br.getOperand(0).getMBB() != &*LI)
      continue;
    if (!A.isSuccessor(&B) || B.pred_size() != 1 || B.size() != 1)
      continue;
    MachineInstr &One = B.front();
    if (One.isPseudo() || TII->getInstSizeInBytes(One) != 2 ||
        One.readsRegister(DSPIC::SR, TRI))
      continue;
    if (!One.isBarrier() && !B.isSuccessor(&*LI))
      continue;
    MachineInstrBuilder S = BuildMI(A, Tst, Br.getDebugLoc(), TII->get(Skip));
    for (const MachineOperand &MO : Tst.explicit_operands())
      S.add(MO);
    Tst.eraseFromParent();
    Br.eraseFromParent();
    ++NumSkip;
    Changed = true;
  }
  return Changed;
}

// L1f-f (trellis session 87): two adjacent word moves of a register PAIR to or from
// [Wp] and [Wp+2] -> mov.d (the pair even, the base outside it; either order; the load's
// base not written by the first move). `mov.w [w0],w2 ; mov.w [w0+2],w3` -> `mov.d [w0],w2`.
bool DSPICPeepholeImpl::fuseMovdMem(MachineBasicBlock &MBB) {
  bool Changed = false;
  auto memOf = [&](MachineInstr &MI, bool &IsLoad, Register &R, Register &Base, int64_t &Off) {
    unsigned Op = MI.getOpcode();
    if (Op == DSPIC::MOV16rm || Op == DSPIC::MOV16rn) {
      IsLoad = true; R = MI.getOperand(0).getReg(); Base = MI.getOperand(1).getReg();
      Off = Op == DSPIC::MOV16rm ? (MI.getOperand(2).isImm() ? MI.getOperand(2).getImm() : -1) : 0;
      return Base.isPhysical() && Off >= 0;
    }
    if (Op == DSPIC::MOV16mr || Op == DSPIC::MOV16mn) {
      IsLoad = false; Base = MI.getOperand(0).getReg();
      Off = Op == DSPIC::MOV16mr ? (MI.getOperand(1).isImm() ? MI.getOperand(1).getImm() : -1) : 0;
      R = MI.getOperand(Op == DSPIC::MOV16mr ? 2 : 1).getReg();
      return Base.isPhysical() && Off >= 0;
    }
    return false;
  };
  auto I = MBB.begin();
  while (I != MBB.end()) {
    auto J = std::next(I);
    if (J == MBB.end())
      break;
    bool L1, L2; Register R1, R2, B1, B2; int64_t O1, O2;
    if (memOf(*I, L1, R1, B1, O1) && memOf(*J, L2, R2, B2, O2) && L1 == L2 && B1 == B2 &&
        B1 != DSPIC::SR && !(I->getOperand(0).isReg() && I->getOperand(0).isDef() && I->getOperand(0).getReg() == B1 && L1)) {
      // the even (low) half may come first or second
      Register Lo = R1, Hi = R2; int64_t OLo = O1, OHi = O2;
      if (wNumber(Lo) % 2) { std::swap(Lo, Hi); std::swap(OLo, OHi); }
      if (wNumber(Lo) % 2 == 0 && Hi == nextWord(Lo) && Hi.isValid() && OHi == OLo + 2 &&
          Lo != B1 && Hi != B1 && OLo == 0) {
        bool Kill = !L1 && I->getOperand(I->getOpcode() == DSPIC::MOV16mr ? 2 : 1).isKill() &&
                    J->getOperand(J->getOpcode() == DSPIC::MOV16mr ? 2 : 1).isKill();
        MachineInstrBuilder F;
        if (L1)
          F = BuildMI(MBB, I, I->getDebugLoc(), TII->get(DSPIC::MOVD16nr), Lo)
                  .addReg(B1).addReg(Hi, RegState::ImplicitDefine);
        else
          F = BuildMI(MBB, I, I->getDebugLoc(), TII->get(DSPIC::MOVD16rn))
                  .addReg(Lo, getKillRegState(Kill)).addReg(B1)
                  .addReg(Hi, RegState::Implicit | getKillRegState(Kill));
        auto K = std::next(J);
        I->eraseFromParent();
        J->eraseFromParent();
        I = K;
        ++NumMovdMem;
        Changed = true;
        continue;
      }
    }
    ++I;
  }
  return Changed;
}

// Session 90: recover the direct byte-file move the session-88 RA fix gave up. isel materializes
// a near-global byte access as `MOV16ri wN,<global> ; MOV8{m,r}... [wN+0]` (2 words). When the
// byte value/result is ALREADY in w0 (WREG) -- a call return, a first arg, a fused chain -- the
// direct `mov.b WREG,_g` / `mov.b _g,WREG` is 1 word. Fire ONLY then (the `as` accepts no other
// register), and only when the address reg dies at the byte op, so it is a strict 2->1 win that
// never over-constrains RA (this runs after allocation) and never pessimizes (if the value is
// not in w0 the materialize form stands).
// trellis session 96 (follow-up 15): a global the 13-bit byte file forms may name. A `far`
// object lies outside that field, and one with an explicit section may be placed anywhere -- the
// same two conditions DSPICInstrInfo.cpp already applies to the near-global remat class.
static bool isNearFileGlobal(const MachineOperand &MO) {
  if (!MO.isGlobal())
    return MO.isSymbol();
  const auto *GVar = dyn_cast<GlobalVariable>(MO.getGlobal());
  return GVar && !GVar->hasAttribute("far") && !GVar->hasSection();
}

bool DSPICPeepholeImpl::fuseByteFile(MachineBasicBlock &MBB) {
  bool Changed = false;
  auto I = MBB.begin();
  while (I != MBB.end()) {
    auto J = std::next(I);
    if (J == MBB.end())
      break;
    // I: MOV16ri wN, <global/external symbol>   (materialize a near-global address)
    // Session 95: a `MOV8f wD, _sym` -- the byte read of a near global emitted as ONE
    // rematerializable instruction (`mov #_sym,wD ; mov.b [wD],wD`) -- collapses to the 1-word
    // direct form when wD is w0, exactly as the two-instruction pair below did. Only MOV8f: the
    // zero-extending ZE16f must keep its high half zero and `mov.b _sym,WREG` does not.
    if (I->getOpcode() == DSPIC::MOV8f && I->getOperand(0).isReg() &&
        wNumber(I->getOperand(0).getReg()) == 0 && I->getOperand(2).isGlobal() &&
        isNearFileGlobal(I->getOperand(2))) {
      Register D8 = TRI->getSubReg(I->getOperand(0).getReg(), DSPIC::subreg_8bit);
      if (D8) {
        BuildMI(MBB, *I, I->getDebugLoc(), TII->get(DSPIC::MOV8fW), D8)
            .addReg(DSPIC::SR)
            .add(I->getOperand(2));
        auto K = std::next(I);
        I->eraseFromParent();
        I = K;
        ++NumByteFile;
        Changed = true;
        continue;
      }
    }
    if (I->getOpcode() != DSPIC::MOV16ri || !I->getOperand(0).isReg() ||
        !(I->getOperand(1).isGlobal() || I->getOperand(1).isSymbol()) ||
        !isNearFileGlobal(I->getOperand(1))) {
      ++I; continue;
    }
    Register Addr = I->getOperand(0).getReg();
    unsigned Op = J->getOpcode();
    MachineInstr *Built = nullptr;
    if (Op == DSPIC::MOV8mr) {
      // store: MOV8mr [base + off], val  -> operands base(0) off(1) val(2)
      Register Base = J->getOperand(0).getReg();
      Register Val = J->getOperand(2).getReg();
      if (Base == Addr && J->getOperand(0).isKill() && J->getOperand(1).isImm() &&
          J->getOperand(1).getImm() == 0 && wNumber(Val) == 0) {
        Built = BuildMI(MBB, *J, J->getDebugLoc(), TII->get(DSPIC::MOV8Wf))
                    .addReg(DSPIC::SR)
                    .add(I->getOperand(1))
                    .addReg(Val, getKillRegState(J->getOperand(2).isKill()));
      }
    } else if (Op == DSPIC::MOV8rm) {
      // load: MOV8rm dst, [base + off]  -> operands dst(0) base(1) off(2)
      Register Dst = J->getOperand(0).getReg();
      Register Base = J->getOperand(1).getReg();
      if (Base == Addr && J->getOperand(1).isKill() && J->getOperand(2).isImm() &&
          J->getOperand(2).getImm() == 0 && wNumber(Dst) == 0) {
        Built = BuildMI(MBB, *J, J->getDebugLoc(), TII->get(DSPIC::MOV8fW), Dst)
                    .addReg(DSPIC::SR)
                    .add(I->getOperand(1));
      }
    }
    if (!Built) { ++I; continue; }
    auto K = std::next(J);
    I->eraseFromParent();
    J->eraseFromParent();
    I = K;
    ++NumByteFile;
    Changed = true;
  }
  return Changed;
}

// trellis session 99: the WREG-result two-operand file forms, `and.w _g,WREG` and family --
// class (b') in DSPICInstrInfo.td, and the PREP's rank-1 shape-gap cluster.
//
// isel gives a near global fed to an ALU op the materialize-plus-memory-operand pair:
//     mov #_g,w1 ; and w0,[w1],w0                             3 words
// and when the value AND the result are both w0 -- an incoming argument, a returned value, a
// fused chain -- the whole thing is one 1-word instruction:
//     and.w _g,WREG                                           1 word
//
// ⛔ WHY HERE AND NOT IN ISEL: the file form's operand and result are both WREG, a one-register
// class, and asking the allocator to arrange that before it has allocated anything makes it fail
// outright on the first function where the value is not already in w0 (see the edit script's
// header). After allocation "it is in w0" is something to READ. Same trade as fuseByteFile.
//
// ⛔ THE SUBTRACTION MAP IS INVERTED AND THAT IS THE WHOLE CONTENT OF TWO OF THE TWELVE ROWS.
// `sub Wb,Ws,Wd` is Wb - Ws, so `sub w0,[w1],w0` is w0 - g; the file form computing w0 - g is
// `subr f,WREG`, because `subr f,WREG` is WREG - f. The assembler takes either spelling and
// cannot tell them apart -- session 84's lesson, and mutant MW1 swaps exactly this.
// trellis session 99: the SHIFT and ROTATE half of the same family. These consume a different
// shape from the binary ops -- a LOAD of the near file symbol into w0 followed by a one-operand
// shift, rather than a materialize followed by a memory-operand ALU op -- so they are a second
// table and a second matcher, and the two never overlap.
static unsigned fileWREGShift(unsigned Op) {
  switch (Op) {
  case DSPIC::LSR16r1:  return DSPIC::LSR16fWr;
  case DSPIC::ASR16r1:  return DSPIC::ASR16fWr;
  case DSPIC::RLNC16r:  return DSPIC::RLNC16fWr;
  case DSPIC::RRNC16r:  return DSPIC::RRNC16fWr;
  case DSPIC::SL8r1:    return DSPIC::SL8fWr;
  case DSPIC::LSR8r1:   return DSPIC::LSR8fWr;
  case DSPIC::ASR8r1:   return DSPIC::ASR8fWr;
  case DSPIC::RLNC8r:   return DSPIC::RLNC8fWr;
  case DSPIC::RRNC8r:   return DSPIC::RRNC8fWr;
  // ⚠ SL16r1 is absent on purpose: shl-by-one never reaches this shape (it takes a
  // memory-operand shift), so a row here would be dead. Mutant MS4 adds it and must find nothing.
  default:              return 0;
  }
}

static unsigned fileWREGForm(unsigned Op) {
  switch (Op) {
  case DSPIC::ADDM16rn:  return DSPIC::ADD16fWr;
  case DSPIC::ANDM16rn:  return DSPIC::AND16fWr;
  case DSPIC::IORM16rn:  return DSPIC::IOR16fWr;
  case DSPIC::XORM16rn:  return DSPIC::XOR16fWr;
  case DSPIC::SUBM16rn:  return DSPIC::SUBR16fWr; // rb - [p]  =  WREG - f  =  subr f,WREG
  case DSPIC::SUBRM16rn: return DSPIC::SUB16fWr;  // [p] - rb  =  f - WREG  =  sub  f,WREG
  case DSPIC::ADDM8rn:   return DSPIC::ADD8fWr;
  case DSPIC::ANDM8rn:   return DSPIC::AND8fWr;
  case DSPIC::IORM8rn:   return DSPIC::IOR8fWr;
  case DSPIC::XORM8rn:   return DSPIC::XOR8fWr;
  case DSPIC::SUBM8rn:   return DSPIC::SUBR8fWr;
  case DSPIC::SUBRM8rn:  return DSPIC::SUB8fWr;
  default:               return 0;
  }
}

bool DSPICPeepholeImpl::fuseFileALU(MachineBasicBlock &MBB) {
  bool Changed = false;
  auto I = MBB.begin();
  while (I != MBB.end()) {
    auto J = std::next(I);
    if (J == MBB.end())
      break;
    // I: MOV16ri wA, <near global>      J: OP wD, wB, [wA]   with wD == wB == w0
    // trellis session 99, the shift/rotate shape: a LOAD of the symbol into w0 (the word load
    // through the $sr no-base sentinel, or session 90's direct byte move) followed by a
    // one-operand shift w0 -> w0 that kills it.
    if (unsigned Sh = fileWREGShift(J->getOpcode())) {
      bool WordLoad = I->getOpcode() == DSPIC::MOV16rm && I->getNumOperands() >= 3 &&
                      I->getOperand(1).isReg() && I->getOperand(1).getReg() == DSPIC::SR &&
                      (I->getOperand(2).isGlobal() || I->getOperand(2).isSymbol()) &&
                      isNearFileGlobal(I->getOperand(2));
      bool ByteLoad = I->getOpcode() == DSPIC::MOV8fW && I->getNumOperands() >= 3 &&
                      (I->getOperand(2).isGlobal() || I->getOperand(2).isSymbol()) &&
                      isNearFileGlobal(I->getOperand(2));
      if ((WordLoad || ByteLoad) && I->getOperand(0).isReg() && J->getNumOperands() >= 2 &&
          J->getOperand(0).isReg() && J->getOperand(1).isReg()) {
        Register Ld = I->getOperand(0).getReg();
        Register Sd = J->getOperand(0).getReg();
        Register Ss = J->getOperand(1).getReg();
        // the loaded value must BE the shift's source, must DIE there, and both it and the
        // result must live in w0 -- the file form writes WREG and nothing else.
        if (Ss == Ld && J->getOperand(1).isKill() && wNumber(Sd) == 0 && wNumber(Ss) == 0) {
          BuildMI(MBB, *J, J->getDebugLoc(), TII->get(Sh), Sd)
              .addReg(DSPIC::SR)
              .add(I->getOperand(2));
          auto K = std::next(J);
          I->eraseFromParent();
          J->eraseFromParent();
          I = K;
          ++NumFileALU;
          Changed = true;
          continue;
        }
      }
    }
    // ⛔ THE BINARY SHAPE'S REJECTION COMES AFTER THIS ARM, and the first version of this edit
    // put the arm after it -- which discards every load-shaped pair, because the shift shape
    // begins with a LOAD (MOV16rm / MOV8fW) and not a MOV16ri. Nothing fused, and the comment
    // added beside it asserted the ordering it did not have.
    if (I->getOpcode() != DSPIC::MOV16ri || !I->getOperand(0).isReg() ||
        !(I->getOperand(1).isGlobal() || I->getOperand(1).isSymbol()) ||
        !isNearFileGlobal(I->getOperand(1))) {
      ++I;
      continue;
    }
    unsigned New = fileWREGForm(J->getOpcode());
    if (!New || J->getNumOperands() < 3 || !J->getOperand(0).isReg() ||
        !J->getOperand(1).isReg() || !J->getOperand(2).isReg()) {
      ++I;
      continue;
    }
    Register Addr = I->getOperand(0).getReg();
    Register Rd = J->getOperand(0).getReg();
    Register Rb = J->getOperand(1).getReg();
    // ⚠ the address register must DIE here: anything else still wants the materialized address,
    // and dropping the `mov` would be a miscompile rather than a saving.
    if (J->getOperand(2).getReg() != Addr || !J->getOperand(2).isKill() ||
        wNumber(Rd) != 0 || wNumber(Rb) != 0) {
      ++I;
      continue;
    }
    BuildMI(MBB, *J, J->getDebugLoc(), TII->get(New), Rd)
        .addReg(DSPIC::SR)
        .add(I->getOperand(1))
        .addReg(Rb, getKillRegState(J->getOperand(1).isKill()));
    auto K = std::next(J);
    I->eraseFromParent();
    J->eraseFromParent();
    I = K;
    ++NumFileALU;
    Changed = true;
  }
  return Changed;
}

// (session 92, the libcalls row) `rcall SYM ; return` ending a block -> `bra SYM`: the routine
// returns straight to our caller, one word for two -- cc1's shape for every runtime-library call
// in tail position, which LLVM's tail-call lowering never marks (it marks C-level calls only).
// Adjacency is the whole safety argument: a call with stack arguments is followed by the caller's
// `sub.w #N,w15` pop, an epilogue with anything to restore by its pops or `ulnk`, and a result
// that is not the function's own by a move -- any of which sits between the two and blocks the
// fusion. The rcall's implicit operands (the argument registers it uses, the registers it clobbers)
// and the return's (the result registers it uses) ride along on the branch.
bool DSPICPeepholeImpl::fuseTailCall(MachineBasicBlock &MBB) {
  if (MBB.size() < 2)
    return false;
  MachineInstr &Ret = MBB.back();
  if (Ret.getOpcode() != DSPIC::RET)
    return false;
  MachineInstr &Call = *std::prev(Ret.getIterator());
  if (Call.getOpcode() != DSPIC::RCALLi)
    return false;
  const MachineOperand &Target = Call.getOperand(0);
  if (!Target.isGlobal() && !Target.isSymbol())
    return false;
  MachineInstrBuilder B = BuildMI(MBB, Ret, Ret.getDebugLoc(), TII->get(DSPIC::TCRETURNdi));
  B.add(Target);
  for (const MachineOperand &MO : Call.implicit_operands())
    B.add(MO);
  // trellis session 97: and NOTHING from the RET. Its implicit operands are the RETURN-VALUE
  // registers it USES; on a tail branch the CALLEE defines them, so copying them claims they are
  // live INTO the branch -- false, and "Using an undefined physical register" wherever a returned
  // register is not also live-in as an argument. RET (DSPICInstrInfo.td) declares no Uses of its
  // own and TCRETURNdi already declares `Uses = [SP]`, so there is nothing else there to carry.
  // ⛔ THE FILTERED FORM ("copy what the call does not define", mirroring fuseRetlw above) WAS
  // WRITTEN FIRST, and BOTH of its mutants LIVED -- on the fixture AND on the whole corpus. That
  // priced the filter's precision at zero everywhere measured, so the simpler code is the honest
  // one. steps/tailfix/mutant.sh carries the measurement.
  Call.eraseFromParent();
  Ret.eraseFromParent();
  ++NumTailBra;
  return true;
}

bool DSPICPeepholeImpl::runOnMachineFunction(MachineFunction &MF) {
  TII = MF.getSubtarget<DSPICSubtarget>().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  bool Changed = fuseSkip(MF);
  for (MachineBasicBlock &MBB : MF) {
    Changed |= fuseMovdMem(MBB);
    Changed |= fuseByteFile(MBB);
    Changed |= fuseFileALU(MBB);
    Changed |= fusePairs(MBB);
    Changed |= fuseTailCall(MBB);
    Changed |= fuseRetlw(MBB);
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// The compare fusion (trellis session 103): `cp Wb,Wn` + `bra cc,L` -> one instruction.
//
// Runs AFTER the branch selector so block offsets are settled. See DSPIC.h and
// tools/dspic-llvm/steps/cmpfuse/cmpfuse-edit.py for the four measured facts it rests on; the
// two that decide the code below are:
//   - the fused forms WRITE NO FLAGS (measured), so SR must be dead after the branch;
//   - the displacement is a signed 6-bit WORD field the LINKER checks, and it is numerically
//     equal to the original branch's own displacement.
//===----------------------------------------------------------------------===//

// ⛔ A LEVEL POLICY, NOT A SWITCH (trellis session 104 post-close; the operator's ruling, "on at
// both"): the fusion is ON for a function carrying optsize or minsize (-Os, -Oz) and OFF
// otherwise, because on the dsPIC33C device model it is never slower -- steps/exec/cycleprobe.c:
// cp+bra 2 cycles not taken / 5 taken, cpbeq 1 / 5 -- and always one word smaller, so it is a
// size decision and the level is where size decisions are made (session 93's CSR-remat gate).
// The option is an OVERRIDE in either direction for A/B runs and fixtures: `-dspic-cmp-fuse`
// forces on at any level, `-dspic-cmp-fuse=false` forces off. Unset means the policy.
static cl::opt<cl::boolOrDefault> CmpFuse("dspic-cmp-fuse", cl::Hidden,
    cl::desc("Fuse a register compare and the branch after it into cpbeq/cpbne/cpblt "
             "(unset: on at -Os/-Oz; =false forces off; set forces on)"));

static bool cmpFuseEnabled(const MachineFunction &MF) {
  switch (CmpFuse) {
  case cl::boolOrDefault::BOU_TRUE:  return true;
  case cl::boolOrDefault::BOU_FALSE: return false;
  default:            return MF.getFunction().hasOptSize(); // optsize OR minsize: -Os and -Oz
  }
}

// ⛔ WHY WAS A SITE REFUSED? Session 94's -dspic-remat-why is the precedent, and this one earned
// its keep the same way: a fixture built specifically to exercise the DISPLACEMENT guard did not
// fuse, the range check was removed by a mutant and it STILL did not fuse, and three explanations
// were reasoned out in a row without one of them being right. A pass with four guards has to be
// able to say which one fired, or every refusal looks like every other refusal.
static cl::opt<bool> CmpFuseWhy("dspic-cmp-fuse-why", cl::Hidden, cl::init(false),
    cl::desc("Print, per candidate compare+branch site, why the fusion was or was not made"));

static void why(const MachineBasicBlock &MBB, const char *What, int Detail = 0) {
  if (!CmpFuseWhy)
    return;
  dbgs() << "cmp-fuse: " << MBB.getParent()->getName() << " bb." << MBB.getNumber() << ": "
         << What;
  if (Detail)
    dbgs() << " (" << Detail << ")";
  dbgs() << "\n";
}

STATISTIC(NumCmpFuse, "Number of compare+branch pairs fused into cpb forms");

namespace {
class DSPICCmpFuseImpl {
  MachineFunction *MF = nullptr;
  const DSPICInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  SmallVector<int, 16> Off;

  unsigned measure();

public:
  bool runOnMachineFunction(MachineFunction &MF);
};

class DSPICCmpFuseLegacyPass : public MachineFunctionPass {
public:
  static char ID;
  DSPICCmpFuseLegacyPass() : MachineFunctionPass(ID) {}
  bool runOnMachineFunction(MachineFunction &MF) override {
    return DSPICCmpFuseImpl().runOnMachineFunction(MF);
  }
  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }
  StringRef getPassName() const override { return "DSPIC Compare Fusion"; }
};
char DSPICCmpFuseLegacyPass::ID = 0;
} // namespace

// ⛔ MAY SR BE READ, starting at the top of this block, before something redefines it?
//
// This does NOT consult block live-in lists. Whether SR appears in a live-in set at pre-emit time
// is a property of what earlier passes chose to maintain, and a fusion that is wrong when that
// bookkeeping is stale is wrong SILENTLY -- the flags simply are not there any more. So the
// question is answered by WALKING: each successor is scanned from its first instruction, and the
// first instruction that reads SR refuses the fusion while the first that redefines it settles
// that path. Anything the walk cannot settle -- a budget exhausted, a block with no successors
// that is not a return -- refuses. Conservative in every direction that matters.
// ⛔ AN EXPLICIT SR OPERAND IS NOT A FLAG READ -- IT IS AN ADDRESSING SENTINEL, AND READING IT AS
// ONE MADE THIS PASS REFUSE EVERY COMPARE FOLLOWED BY A NEAR-GLOBAL LOAD.
//
// A near-global load is `%1:gr16 = MOV16rm $sr, @g16`, where $sr in the BASE slot is SelectAddr's
// "no base register" sentinel. Session 94 hit the same operand from the other side and needed an
// isIgnorableUse override for it (DSPICInstrInfo.cpp:176). Here, taking
// MI.readsRegister(DSPIC::SR) literally refused far_eq in steps/cmpfuse/farbranch.c -- a site
// whose whole purpose was to exercise the DISPLACEMENT guard -- and it survived a mutant that
// removed the displacement check, because a different guard was doing the refusing.
// ⚠ THREE EXPLANATIONS WERE REASONED OUT BEFORE THIS ONE AND ALL THREE WERE WRONG;
// -dspic-cmp-fuse-why answered it in a single run. That is what the knob is for.
//
// The discriminator is principled rather than a special case for one opcode: every instruction in
// DSPICInstrInfo.td that genuinely reads the flags declares `Uses = [SR]`, which makes the operand
// IMPLICIT, and NO instruction lists SR in an `ins` list at all. So an explicit SR use is always
// the sentinel, and an implicit one is always a real read.
static bool readsFlags(const MachineInstr &MI) {
  for (const MachineOperand &MO : MI.operands())
    if (MO.isReg() && MO.isUse() && MO.getReg() == DSPIC::SR && MO.isImplicit())
      return true;
  return false;
}

static bool srMayBeRead(MachineBasicBlock *Start, const TargetRegisterInfo *TRI) {
  SmallPtrSet<MachineBasicBlock *, 8> Seen;
  SmallVector<MachineBasicBlock *, 8> Work;
  Work.push_back(Start);
  unsigned Budget = 64;
  while (!Work.empty()) {
    MachineBasicBlock *B = Work.pop_back_val();
    if (!Seen.insert(B).second)
      continue;
    if (Budget-- == 0)
      return true;
    bool Settled = false;
    for (MachineInstr &MI : *B) {
      // inline assembly is opaque: it may read the flags without saying so.
      if (MI.isInlineAsm() || readsFlags(MI))
        return true;
      if (MI.definesRegister(DSPIC::SR, TRI)) {
        Settled = true;
        break;
      }
    }
    if (Settled)
      continue;
    if (B->succ_empty()) {
      // a block that returns observes no flag; anything else that simply stops is unknown.
      if (!B->isReturnBlock())
        return true;
      continue;
    }
    for (MachineBasicBlock *S : B->successors())
      Work.push_back(S);
  }
  return false;
}

STATISTIC(NumCmpSkip, "Number of compare+branch pairs fused into cps skip forms");

// the skip forms take no target: the skipped instruction is the layout successor's one word.
static bool isSkipForm(unsigned Opc) {
  switch (Opc) {
  case DSPIC::CPSEQ16: case DSPIC::CPSNE16: case DSPIC::CPSLT16: case DSPIC::CPSGT16:
  case DSPIC::CPSEQ8:  case DSPIC::CPSNE8:  case DSPIC::CPSLT8:  case DSPIC::CPSGT8:
    return true;
  default:
    return false;
  }
}

unsigned DSPICCmpFuseImpl::measure() {
  MF->RenumberBlocks();
  Off.assign(MF->getNumBlockIDs(), 0);
  unsigned Total = 0;
  for (MachineBasicBlock &MBB : *MF) {
    Off[MBB.getNumber()] = Total;
    for (MachineInstr &MI : MBB)
      Total += TII->getInstSizeInBytes(MI);
  }
  return Total;
}

bool DSPICCmpFuseImpl::runOnMachineFunction(MachineFunction &mf) {
  if (!cmpFuseEnabled(mf))
    return false;
  MF = &mf;
  TII = static_cast<const DSPICInstrInfo *>(MF->getSubtarget().getInstrInfo());
  TRI = MF->getSubtarget().getRegisterInfo();
  measure();

  SmallVector<std::pair<MachineInstr *, MachineInstr *>, 8> Work;
  SmallVector<unsigned, 8> Opc;

  for (MachineBasicBlock &MBB : *MF) {
    if (MBB.size() < 2)
      continue;
    MachineInstr *Br = &MBB.back();
    if (Br->getOpcode() != DSPIC::JCC)
      continue;
    MachineInstr *Cmp = &*std::prev(Br->getIterator());
    bool Byte;
    if (Cmp->getOpcode() == DSPIC::CMP16rr)
      Byte = false;
    else if (Cmp->getOpcode() == DSPIC::CMP8rr)
      Byte = true;
    else
      continue;

    // the condition. ⛔ ONLY FOUR MAP. The unsigned conditions (COND_HS/COND_LO, printed `c`/`nc`
    // and spelled geu/ltu by our own printer) have NO cpb form in the ISA, and COND_GE has none
    // either -- cpb offers only > and <, and "branch if not less" is not among them; inverting it
    // means swapping the successors, which is a layout change and not a peephole. Both are
    // refused here BY NAME so a later session does not close the gap by reaching for cpblt.
    unsigned CC = Br->getOperand(1).getImm();
    unsigned Fused;
    switch (CC) {
    case DSPICCC::COND_E:  Fused = Byte ? DSPIC::CPBEQ8 : DSPIC::CPBEQ16; break;
    case DSPICCC::COND_NE: Fused = Byte ? DSPIC::CPBNE8 : DSPIC::CPBNE16; break;
    case DSPICCC::COND_L:  Fused = Byte ? DSPIC::CPBLT8 : DSPIC::CPBLT16; break;
    default:
      // COND_HS, COND_LO: unsigned, no form. COND_GE, COND_N: no direct form.
      why(MBB, "REFUSED: condition has no cpb form", (int)CC);
      continue;
    }

    // ⛔ SR MUST BE DEAD AFTER THE BRANCH -- the one guard this row cannot do without. The
    // compare defines the flags and the branch consumes them; the fused form defines NOTHING
    // (measured, steps/exec/cmpprobe.c), so a later reader would see whatever came before.
    bool MayRead = false;
    for (MachineBasicBlock *S : MBB.successors())
      if (srMayBeRead(S, TRI))
        MayRead = true;
    if (MayRead) {
      why(MBB, "REFUSED: SR may be read after the branch");
      continue;
    }

    // ⛔ THE ASSEMBLER REFUSES A cpb THAT FOLLOWS A `repeat` -- F_CANNOT_FOLLOW_REPEAT in the
    // vendor's own opcode table (pic30-opc.c:6209), diagnosed BY NAME in tc-pic30.c:9937. Our own
    // repeats are fused two-line pseudos (the divide, the block copy) so nothing can land between
    // them, and the compare is never the repeated instruction -- but an inline-asm `repeat` is
    // opaque, and the guard costs one comparison against a build failure on real firmware.
    if (Cmp != &MBB.front()) {
      MachineInstr &Prev = *std::prev(Cmp->getIterator());
      if (Prev.isInlineAsm() || Prev.getOpcode() == DSPIC::REPEATdiv) {
        why(MBB, "REFUSED: the compare follows inline asm or a repeat");
        continue;
      }
    }

    // the displacement, in WORDS, of the original branch -- which is the fused form's own.
    MachineBasicBlock *Dest = Br->getOperand(0).getMBB();
    int End = Off[MBB.getNumber()];
    for (MachineInstr &MI : MBB)
      End += TII->getInstSizeInBytes(MI);
    int Bytes = Off[Dest->getNumber()] - End;
    // ⚠ FUSION ONLY SHRINKS, so a distance in reach now is in reach after every other fusion in
    // this function; no fixed point is needed. Measured bounds: +31 links, +32 is refused by the
    // linker (steps/cmpfuse/as-range2.sh).
    int Words = Bytes / 2;
    if (Words < -32 || Words > 31) {
      why(MBB, "REFUSED: displacement out of the 6-bit reach, words", Words);
      continue;
    }
    // ⛔ THE SKIP ARM (trellis session 105): a branch over exactly ONE word is the skip shape.
    // `cpbXX Wb,Wn,L ; <one word> ; L:` becomes `cpsXX Wb,Wn ; <one word>`.
    // ⛔ AND IT IS A SPELLING, MEASURED AFTER IT WAS BUILT: the LINKER resolves a one-word cpbeq to
    // `11 90 e7`, the identical word the assembler gives cpseq (a two-word cpbeq is `21 90 e7`) --
    // so both firmwares are BYTE-IDENTICAL under this arm at 20 changed sites, and the cycle
    // difference the prediction claimed (cycleprobe: cpbeq "taken 5", cpseq "skip 2") compared a
    // displacement-ZERO branch against this very word. What the arm buys is the assembler's six
    // cps rows in the inventory and an honest mnemonic for what the hardware does; what it costs
    // is nothing the machine sees. Same guard as the branch forms, because the skip forms write
    // no flags either (cmpprobe F3). The rule is fuseSkip's (the btsc/btss arm, L1f-g), mirrored:
    // the layout successor B is ONE instruction, not a pseudo, not inline asm, has this block as
    // its only predecessor, and reaches the target by falling through or by never continuing.
    // B's own flag reads are already refused above: B is a successor, and the walk starts at its
    // first instruction. ⚠ fuseSkip's explicit "two bytes" test is NOT repeated here because
    // `Words == 1` with B the only block between already says it -- mutant MS3 (the test dropped)
    // LIVED on its two-word fixture for exactly that reason, and a guard that cannot fire is
    // documentation pretending to be a check. skip2.c pins the two-word case at Words == 2.
    unsigned SkipOpc = 0;
    if (Words == 1) {
      auto BI = std::next(MBB.getIterator());
      if (BI != MF->end()) {
        MachineBasicBlock &B = *BI;
        auto LI = std::next(BI);
        if (LI != MF->end() && &*LI == Dest && MBB.isSuccessor(&B) && B.pred_size() == 1 &&
            B.size() == 1) {
          MachineInstr &One = B.front();
          if (!One.isPseudo() && !One.isInlineAsm() &&
              (One.isBarrier() || B.isSuccessor(Dest))) {
            switch (CC) {
            case DSPICCC::COND_E:  SkipOpc = Byte ? DSPIC::CPSEQ8 : DSPIC::CPSEQ16; break;
            case DSPICCC::COND_NE: SkipOpc = Byte ? DSPIC::CPSNE8 : DSPIC::CPSNE16; break;
            case DSPICCC::COND_L:  SkipOpc = Byte ? DSPIC::CPSLT8 : DSPIC::CPSLT16; break;
            default: break;
            }
          }
        }
      }
    }
    if (SkipOpc) {
      why(MBB, "FUSED, as a skip over one word");
      Fused = SkipOpc;
    } else {
      why(MBB, "FUSED, displacement words", Words);
    }

    Work.push_back({Cmp, Br});
    Opc.push_back(Fused);
  }

  if (Work.empty())
    return false;
  for (unsigned i = 0; i != Work.size(); ++i) {
    MachineInstr *Cmp = Work[i].first, *Br = Work[i].second;
    MachineInstrBuilder MIB =
        BuildMI(*Br->getParent(), *Cmp, Br->getDebugLoc(), TII->get(Opc[i]))
            .add(Cmp->getOperand(0))
            .add(Cmp->getOperand(1));
    if (isSkipForm(Opc[i]))
      ++NumCmpSkip;
    else
      MIB.addMBB(Br->getOperand(0).getMBB());
    Cmp->eraseFromParent();
    Br->eraseFromParent();
    ++NumCmpFuse;
  }
  return true;
}

FunctionPass *llvm::createDSPICCmpFuseLegacyPass() {
  return new DSPICCmpFuseLegacyPass();
}

PreservedAnalyses DSPICCmpFusePass::run(MachineFunction &MF,
                                        MachineFunctionAnalysisManager &MFAM) {
  if (!DSPICCmpFuseImpl().runOnMachineFunction(MF))
    return PreservedAnalyses::all();
  return getMachineFunctionPassPreservedAnalyses();
}

PreservedAnalyses DSPICPeepholePass::run(MachineFunction &MF,
                                         MachineFunctionAnalysisManager &MFAM) {
  return DSPICPeepholeImpl().runOnMachineFunction(MF)
             ? getMachineFunctionPassPreservedAnalyses()
             : PreservedAnalyses::all();
}

FunctionPass *llvm::createDSPICPeepholeLegacyPass() {
  return new DSPICPeepholeLegacyPass();
}
