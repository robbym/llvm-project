//===-- DSPICFrameLowering.cpp - DSPIC Frame Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dsPIC33 frame: see the header. Every printed form here was accepted by the
// GPL pic30 `as` before it was written (trellis `tools/dspic-llvm/prints/l1b/`).
//
//===----------------------------------------------------------------------===//

#include "DSPICFrameLowering.h"
#include "DSPICInstrInfo.h"
#include "DSPICMachineFunctionInfo.h"
#include "DSPICSubtarget.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

// The assembler's ranges (prints/l1b/probe3.log): `lnk #N` takes an even N up to 16382;
// a word displacement is even and in [-1024, 1022]; `add #lit10,Wn` takes up to 1023.
static const uint64_t MaxLnk = 16382;
static const uint64_t MaxLit10 = 1023;

// The stack grows up; the local area begins 2 bytes past the entry SP (the saved w14).
DSPICFrameLowering::DSPICFrameLowering(const DSPICSubtarget &STI)
    : TargetFrameLowering(TargetFrameLowering::StackGrowsUp, Align(2), 2,
                          Align(2)),
      STI(STI), TII(*STI.getInstrInfo()), TRI(STI.getRegisterInfo()) {}

// A frame is linked (`lnk`/`ulnk`, w14 the base) iff anything is addressed through a
// frame index: a local, a spill, a fixed object (an incoming stack argument, or a tail
// call's outgoing one written over it), the outgoing call area, or a variable-sized object. Callee-saved slots alone are pushes and pops.
// w14 is reserved whatever this returns, so a late spill cannot change the answer for a
// register already handed out.
bool DSPICFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (MF.getTarget().Options.DisableFramePointerElim(MF) ||
      MFI.hasVarSizedObjects() || MFI.isFrameAddressTaken() ||
      MFI.isReturnAddressTaken())
    return true;
  for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); I != E;
       ++I)
    if (!MFI.isDeadObjectIndex(I) && !MFI.isCalleeSavedObjectIndex(I))
      return true;
  return false; // a call frame alone links nothing: the arguments are pushed (L1f-a)
}

// L1f-a: never reserved. Outgoing arguments are PUSHED (the pushes move w15) and popped
// by `sub.w #N,w15` after the call, so a caller with no locals links no frame -- cc1's
// shape -- and the variable-sized-object case is the same path as every other.
bool DSPICFrameLowering::hasReservedCallFrame(const MachineFunction &MF) const {
  return false;
}

int64_t DSPICFrameLowering::frameOffsetFromFP(const MachineFunction &MF,
                                             int FI) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
  return MFI.getObjectOffset(FI) -
         (int64_t)(getOffsetOfLocalArea() +
                   FuncInfo->getCalleeSavedFrameSize());
}

StackOffset
DSPICFrameLowering::getFrameIndexReference(const MachineFunction &MF, int FI,
                                           Register &FrameReg) const {
  FrameReg = DSPIC::R4;
  return StackOffset::getFixed(frameOffsetFromFP(MF, FI));
}

// L1d RCOUNT (trellis session 88): an ISR clobbers the repeat counter if it makes a CALL
// (the opaque callee may run a `repeat`) or its own body has a repeat instruction (the inline
// memcpy/memset block ops, the divide). A non-ISR never saves RCOUNT.
static bool isrSavesRCount(const MachineFunction &MF) {
  if (!MF.getFunction().hasFnAttribute("interrupt"))
    return false;
  if (MF.getFrameInfo().hasCalls())
    return true;
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB)
      switch (MI.getOpcode()) {
      case DSPIC::REPEATdiv:
      case DSPIC::MEMCPYrepW: case DSPIC::MEMCPYrepB:
      case DSPIC::MEMSETrepW: case DSPIC::MEMSETrepB:
        return true;
      default: break;
      }
  return false;
}

static bool isCalleeSavedPush(const MachineInstr &MI) {
  return MI.getFlag(MachineInstr::FrameSetup) &&
         (MI.getOpcode() == DSPIC::PUSH16r || MI.getOpcode() == DSPIC::PUSHD ||
          MI.getOpcode() == DSPIC::PUSHRCOUNT);
}

static bool isCalleeSavedPop(const MachineInstr &MI) {
  return MI.getFlag(MachineInstr::FrameDestroy) &&
         (MI.getOpcode() == DSPIC::POP16r || MI.getOpcode() == DSPIC::POPD ||
          MI.getOpcode() == DSPIC::POPRCOUNT);
}

// Prologue: the callee-saved pushes are already at the block's head (PEI inserted them
// through spillCalleeSavedRegisters); `lnk #locals` follows them.
void DSPICFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  assert(&MF.front() == &MBB && "Shrink-wrapping not yet supported");
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();

  MachineBasicBlock::iterator MBBI = MBB.begin();
  while (MBBI != MBB.end() && isCalleeSavedPush(*MBBI))
    ++MBBI;
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  if (!hasFP(MF))
    return;

  uint64_t Locals = MFI.getStackSize() - FuncInfo->getCalleeSavedFrameSize();
  if (Locals % 2 != 0 || Locals > MaxLnk)
    report_fatal_error("dspic: frame of " + Twine(Locals) +
                       " bytes is not an even number of bytes up to 16382");
  BuildMI(MBB, MBBI, DL, TII.get(DSPIC::LNK))
      .addImm(Locals)
      .setMIFlag(MachineInstr::FrameSetup);

  for (MachineBasicBlock &MBBJ : llvm::drop_begin(MF))
    MBBJ.addLiveIn(DSPIC::R4);
}

// Epilogue: `ulnk` restores w15 to the top of the pushes and pops w14; then the pops
// PEI inserted through restoreCalleeSavedRegisters; then `return`/`retfie`, or a tail
// call's `bra`/`goto` (L1c), which leaves the frame exactly as `return` would.
void DSPICFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  switch (MBBI->getOpcode()) {
  case DSPIC::RET:
  case DSPIC::RETI:
  case DSPIC::TCRETURNdi: // L1c: `bra _sym` after the epilogue
  case DSPIC::TCRETURNri: // L1c: `goto wN`, wN caller-saved
    break;
  default:
    llvm_unreachable("Can only insert epilog into returning blocks");
  }

  // Back up over the callee-saved pops.
  MachineBasicBlock::iterator I = MBBI;
  while (I != MBB.begin() && isCalleeSavedPop(*std::prev(I)))
    --I;

  if (hasFP(MF))
    BuildMI(MBB, I, DL, TII.get(DSPIC::ULNK))
        .setMIFlag(MachineInstr::FrameDestroy);
}

// The callee-saved pairs `push.d`/`pop.d` can take: (w8,w9) (w10,w11) (w12,w13), by the
// internal names the register file keeps (DSPICRegisterInfo.td: R10 = w8 ... R5 = w13).
namespace {
struct CSPair {
  MCPhysReg Even, Odd;
};
} // namespace
static const CSPair CSPairs[] = {
    {DSPIC::R12, DSPIC::R13}, {DSPIC::R14, DSPIC::R15}, {DSPIC::R11, DSPIC::W5}, {DSPIC::W6, DSPIC::W7}, {DSPIC::R10, DSPIC::R9}, {DSPIC::R8, DSPIC::R7}, {DSPIC::R6, DSPIC::R5}};
// Singles, in ascending w order, for what is left over.
static const MCPhysReg CSSingles[] = {
    DSPIC::R12, DSPIC::R13, DSPIC::R14, DSPIC::R15, DSPIC::R11, DSPIC::W5, DSPIC::W6,
    DSPIC::W7,  DSPIC::R10, DSPIC::R9,  DSPIC::R8,   DSPIC::R7,  DSPIC::R6, DSPIC::R5};

// Plan the pushes in ascending w order: a pair where both registers are saved, a
// single otherwise. Pops are the reverse of this list.
static void
planCalleeSaves(ArrayRef<CalleeSavedInfo> CSI,
                SmallVectorImpl<std::pair<MCPhysReg, MCPhysReg>> &Plan) {
  SmallSet<MCPhysReg, 8> Saved;
  for (const CalleeSavedInfo &I : CSI)
    Saved.insert(I.getReg());
  SmallSet<MCPhysReg, 8> Done;
  for (const CSPair &P : CSPairs)
    if (Saved.count(P.Even) && Saved.count(P.Odd)) {
      Plan.push_back({P.Even, P.Odd});
      Done.insert(P.Even);
      Done.insert(P.Odd);
    }
  for (MCPhysReg R : CSSingles)
    if (Saved.count(R) && !Done.count(R))
      Plan.push_back({R, 0});
  // Ascending w order across pairs and singles. The internal numbering runs the other
  // way (R10 = w8 ... R5 = w13), so sort by the internal number descending.
  llvm::stable_sort(Plan, [](const std::pair<MCPhysReg, MCPhysReg> &A,
                             const std::pair<MCPhysReg, MCPhysReg> &B) {
    return A.first > B.first;
  });
}

bool DSPICFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction &MF = *MBB.getParent();
  bool SaveRC = isrSavesRCount(MF);
  if (CSI.empty() && !SaveRC)
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
  FuncInfo->setCalleeSavedFrameSize(CSI.size() * 2);
  // RCOUNT first (deepest), before the GPRs -- cc1's order. It self-balances with the pop and
  // is not counted in the frame size (pushed before `lnk`, transparent to w14-relative slots).
  if (SaveRC)
    BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHRCOUNT)).setMIFlag(MachineInstr::FrameSetup);

  SmallVector<std::pair<MCPhysReg, MCPhysReg>, 4> Plan;
  planCalleeSaves(CSI, Plan);
  for (const auto &[Even, Odd] : Plan) {
    MBB.addLiveIn(Even);
    if (Odd) {
      MBB.addLiveIn(Odd);
      BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHD))
          .addReg(Even, RegState::Kill)
          .addReg(Odd, RegState::Implicit | RegState::Kill)
          .setMIFlag(MachineInstr::FrameSetup);
    } else {
      BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSH16r))
          .addReg(Even, RegState::Kill)
          .setMIFlag(MachineInstr::FrameSetup);
    }
  }
  return true;
}

bool DSPICFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction &MF = *MBB.getParent();
  bool SaveRC = isrSavesRCount(MF);
  if (CSI.empty() && !SaveRC)
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();

  SmallVector<std::pair<MCPhysReg, MCPhysReg>, 4> Plan;
  planCalleeSaves(CSI, Plan);
  for (const auto &[Even, Odd] : llvm::reverse(Plan)) {
    if (Odd)
      BuildMI(MBB, MI, DL, TII.get(DSPIC::POPD), Even)
          .addReg(Odd, RegState::ImplicitDefine)
          .setMIFlag(MachineInstr::FrameDestroy);
    else
      BuildMI(MBB, MI, DL, TII.get(DSPIC::POP16r), Even)
          .setMIFlag(MachineInstr::FrameDestroy);
  }
  // RCOUNT last -- restored after every GPR, cc1's order.
  if (SaveRC)
    BuildMI(MBB, MI, DL, TII.get(DSPIC::POPRCOUNT)).setMIFlag(MachineInstr::FrameDestroy);
  return true;
}

// The setup pseudo emits nothing (the pushes move w15); the destroy pseudo pops the
// pushed bytes with `sub.w #N,w15` (a 10-bit literal; past 1023 a fatal error, as
// cc1's `sub` would need a register).
MachineBasicBlock::iterator DSPICFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  const DSPICInstrInfo &TII =
      *static_cast<const DSPICInstrInfo *>(MF.getSubtarget().getInstrInfo());
  {
    MachineInstr &Old = *I;
    uint64_t Amount = alignTo(TII.getFrameSize(Old), getStackAlign());
    bool IsSetup = Old.getOpcode() == TII.getCallFrameSetupOpcode();
    if (!IsSetup)
      Amount -= TII.getFramePoppedByCallee(Old);
    if (Amount != 0 && !IsSetup) {
      if (Amount > MaxLit10)
        report_fatal_error("dspic: " + Twine(Amount) +
                               " bytes of pushed arguments exceed the 10-bit "
                               "literal of the pop after the call",
                           /*gen_crash_diag=*/false);
      MachineInstr *New =
          BuildMI(MF, Old.getDebugLoc(), TII.get(DSPIC::SUB16ri10), DSPIC::SP)
              .addReg(DSPIC::SP)
              .addImm(Amount);
      New->getOperand(3).setIsDead(); // SR
      MBB.insert(I, New);
    }
  }
  return MBB.erase(I);
}

// An emergency spill slot for frame-index scavenging, when a local may sit past the
// 10-bit word displacement (prints/l1b/probe3.log: [-1024, 1022]). It is placed first,
// at [w14 + 0], so it is always reachable. The price: 2 bytes in a frame over ~1 KB.
void DSPICFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *RS) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  if (RS && MFI.estimateStackSize(MF) >= 1000)
    RS->addScavengingFrameIndex(MFI.CreateStackObject(2, Align(2), false));
}
