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
bool DSPICPeepholeImpl::fuseByteFile(MachineBasicBlock &MBB) {
  bool Changed = false;
  auto I = MBB.begin();
  while (I != MBB.end()) {
    auto J = std::next(I);
    if (J == MBB.end())
      break;
    // I: MOV16ri wN, <global/external symbol>   (materialize a near-global address)
    if (I->getOpcode() != DSPIC::MOV16ri || !I->getOperand(0).isReg() ||
        !(I->getOperand(1).isGlobal() || I->getOperand(1).isSymbol())) {
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
  for (const MachineOperand &MO : Ret.implicit_operands())
    B.add(MO);
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
    Changed |= fusePairs(MBB);
    Changed |= fuseTailCall(MBB);
    Changed |= fuseRetlw(MBB);
  }
  return Changed;
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
