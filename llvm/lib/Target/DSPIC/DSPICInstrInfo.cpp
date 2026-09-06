//===-- DSPICInstrInfo.cpp - DSPIC Instruction Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DSPIC implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "DSPICInstrInfo.h"
#include "DSPIC.h"
#include "DSPICSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOutliner.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "DSPICGenInstrInfo.inc"

// Pin the vtable to this file.
void DSPICInstrInfo::anchor() {}

DSPICInstrInfo::DSPICInstrInfo(const DSPICSubtarget &STI)
    : DSPICGenInstrInfo(STI, RI, DSPIC::ADJCALLSTACKDOWN,
                         DSPIC::ADJCALLSTACKUP),
      RI() {}

void DSPICInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool isKill, int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end()) DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOStore, MFI.getObjectSize(FrameIdx),
      MFI.getObjectAlign(FrameIdx));

  if (DSPIC::GR16RegClass.hasSubClassEq(RC)) // GR16 or GR16_TC (L1c)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV16mr))
      .addFrameIndex(FrameIdx).addImm(0)
      .addReg(SrcReg, getKillRegState(isKill)).addMemOperand(MMO);
  else if (DSPIC::GR8RegClass.hasSubClassEq(RC)) // GR8 or GR8_W0 (L1f-g)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV8mr))
      .addFrameIndex(FrameIdx).addImm(0)
      .addReg(SrcReg, getKillRegState(isKill)).addMemOperand(MMO);
  else
    llvm_unreachable("Cannot store this register to stack slot!");
}

void DSPICInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIdx,
                                           const TargetRegisterClass *RC,
                                           Register VReg, unsigned SubReg,
                                           MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end()) DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOLoad, MFI.getObjectSize(FrameIdx),
      MFI.getObjectAlign(FrameIdx));

  if (DSPIC::GR16RegClass.hasSubClassEq(RC)) // GR16 or GR16_TC (L1c)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV16rm))
      .addReg(DestReg, getDefRegState(true)).addFrameIndex(FrameIdx)
      .addImm(0).addMemOperand(MMO);
  else if (DSPIC::GR8RegClass.hasSubClassEq(RC)) // GR8 or GR8_W0 (L1f-g)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV8rm))
      .addReg(DestReg, getDefRegState(true)).addFrameIndex(FrameIdx)
      .addImm(0).addMemOperand(MMO);
  else
    llvm_unreachable("Cannot store this register to stack slot!");
}

void DSPICInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest, bool RenamableSrc) const {
  unsigned Opc;
  if (DSPIC::GR16RegClass.contains(DestReg, SrcReg))
    Opc = DSPIC::MOV16rr;
  else if (DSPIC::GR8RegClass.contains(DestReg, SrcReg))
    Opc = DSPIC::MOV8rr;
  else
    llvm_unreachable("Impossible reg-to-reg copy");

  BuildMI(MBB, I, DL, get(Opc), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc));
}

unsigned DSPICInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  assert(!BytesRemoved && "code size not handled");

  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;

  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != DSPIC::JMP &&
        I->getOpcode() != DSPIC::JCC &&
        I->getOpcode() != DSPIC::Bi &&
        I->getOpcode() != DSPIC::Br &&
        I->getOpcode() != DSPIC::Bm)
      break;
    // Remove the branch.
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }

  return Count;
}

bool DSPICInstrInfo::
reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Invalid Xbranch condition!");

  DSPICCC::CondCodes CC = static_cast<DSPICCC::CondCodes>(Cond[0].getImm());

  switch (CC) {
  default: llvm_unreachable("Invalid branch condition!");
  case DSPICCC::COND_E:
    CC = DSPICCC::COND_NE;
    break;
  case DSPICCC::COND_NE:
    CC = DSPICCC::COND_E;
    break;
  case DSPICCC::COND_L:
    CC = DSPICCC::COND_GE;
    break;
  case DSPICCC::COND_GE:
    CC = DSPICCC::COND_L;
    break;
  case DSPICCC::COND_HS:
    CC = DSPICCC::COND_LO;
    break;
  case DSPICCC::COND_LO:
    CC = DSPICCC::COND_HS;
    break;
  }

  Cond[0].setImm(CC);
  return false;
}

bool DSPICInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool AllowModify) const {
  // Start from the bottom of the block and work up, examining the
  // terminator instructions.
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;

    // Working from the bottom, when we see a non-terminator
    // instruction, we're done.
    if (!isUnpredicatedTerminator(*I))
      break;

    // A terminator that isn't a branch can't easily be handled
    // by this analysis.
    if (!I->isBranch())
      return true;

    // Cannot handle indirect branches.
    if (I->getOpcode() == DSPIC::Br ||
        I->getOpcode() == DSPIC::Bm)
      return true;

    // Handle unconditional branches.
    if (I->getOpcode() == DSPIC::JMP || I->getOpcode() == DSPIC::Bi) {
      if (!AllowModify) {
        TBB = I->getOperand(0).getMBB();
        continue;
      }

      // If the block has any instructions after a JMP, delete them.
      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;

      // Delete the JMP if it's equivalent to a fall-through.
      if (MBB.isLayoutSuccessor(I->getOperand(0).getMBB())) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }

      // TBB is used to indicate the unconditinal destination.
      TBB = I->getOperand(0).getMBB();
      continue;
    }

    // Handle conditional branches.
    assert(I->getOpcode() == DSPIC::JCC && "Invalid conditional branch");
    DSPICCC::CondCodes BranchCode =
      static_cast<DSPICCC::CondCodes>(I->getOperand(1).getImm());
    if (BranchCode == DSPICCC::COND_INVALID)
      return true;  // Can't handle weird stuff.

    // Working from the bottom, handle the first conditional branch.
    if (Cond.empty()) {
      FBB = TBB;
      TBB = I->getOperand(0).getMBB();
      Cond.push_back(MachineOperand::CreateImm(BranchCode));
      continue;
    }

    // Handle subsequent conditional branches. Only handle the case where all
    // conditional branches branch to the same destination.
    assert(Cond.size() == 1);
    assert(TBB);

    // Only handle the case where all conditional branches branch to
    // the same destination.
    if (TBB != I->getOperand(0).getMBB())
      return true;

    DSPICCC::CondCodes OldBranchCode = (DSPICCC::CondCodes)Cond[0].getImm();
    // If the conditions are the same, we can leave them alone.
    if (OldBranchCode == BranchCode)
      continue;

    return true;
  }

  return false;
}

unsigned DSPICInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *TBB,
                                       MachineBasicBlock *FBB,
                                       ArrayRef<MachineOperand> Cond,
                                       const DebugLoc &DL,
                                       int *BytesAdded) const {
  // Shouldn't be a fall through.
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 1 || Cond.size() == 0) &&
         "DSPIC branch conditions have one component!");
  assert(!BytesAdded && "code size not handled");

  if (Cond.empty()) {
    // Unconditional branch?
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(DSPIC::JMP)).addMBB(TBB);
    return 1;
  }

  // Conditional branch.
  unsigned Count = 0;
  BuildMI(&MBB, DL, get(DSPIC::JCC)).addMBB(TBB).addImm(Cond[0].getImm());
  ++Count;

  if (FBB) {
    // Two-way Conditional branch. Insert the second branch.
    BuildMI(&MBB, DL, get(DSPIC::JMP)).addMBB(FBB);
    ++Count;
  }
  return Count;
}

/// GetInstSize - Return the number of bytes of code the specified
/// instruction may be.  This returns the maximum number of bytes.
///
unsigned DSPICInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  const MCInstrDesc &Desc = MI.getDesc();

  switch (Desc.getOpcode()) {
  case TargetOpcode::CFI_INSTRUCTION:
  case TargetOpcode::EH_LABEL:
  case TargetOpcode::IMPLICIT_DEF:
  case TargetOpcode::KILL:
  case TargetOpcode::DBG_VALUE:
    return 0;
  case TargetOpcode::INLINEASM:
  case TargetOpcode::INLINEASM_BR: {
    const MachineFunction *MF = MI.getParent()->getParent();
    const TargetInstrInfo &TII = *MF->getSubtarget().getInstrInfo();
    return TII.getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                                  MF->getTarget().getMCAsmInfo());
  }
  case TargetOpcode::BUNDLE:
    return getInstBundleSize(MI);
  }

  return Desc.getSize();
}

namespace { enum DSPICOutlinerConstruction { MachineOutlinerDefault }; }

bool DSPICInstrInfo::isFunctionSafeToOutlineFrom(MachineFunction &MF,
                                                 bool OutlineFromLinkOnceODRs) const {
  const Function &F = MF.getFunction();
  if (!OutlineFromLinkOnceODRs && F.hasLinkOnceODRLinkage())
    return false;
  if (F.hasFnAttribute("interrupt"))
    return false;
  return true;
}

bool DSPICInstrInfo::isMBBSafeToOutlineFrom(MachineBasicBlock &MBB, unsigned &Flags) const {
  return true;
}

bool DSPICInstrInfo::shouldOutlineFromFunctionByDefault(MachineFunction &MF) const {
  return true;
}

std::optional<std::unique_ptr<outliner::OutlinedFunction>>
DSPICInstrInfo::getOutliningCandidateInfo(
    const MachineModuleInfo &MMI,
    std::vector<outliner::Candidate> &RepeatedSequenceLocs,
    unsigned MinRepeats) const {
  if (RepeatedSequenceLocs.size() < MinRepeats)
    return std::nullopt;
  unsigned SequenceSize = 0;
  for (auto &MI : RepeatedSequenceLocs[0])
    SequenceSize += getInstSizeInBytes(MI);
  unsigned CallOverhead = 2;
  unsigned FrameOverhead = 2;
  for (outliner::Candidate &C : RepeatedSequenceLocs)
    C.setCallInfo(MachineOutlinerDefault, CallOverhead);
  return std::make_unique<outliner::OutlinedFunction>(
      RepeatedSequenceLocs, SequenceSize, FrameOverhead, MachineOutlinerDefault);
}

outliner::InstrType
DSPICInstrInfo::getOutliningTypeImpl(const MachineModuleInfo &MMI,
                                     MachineBasicBlock::iterator &MBBI,
                                     unsigned Flags) const {
  MachineInstr &MI = *MBBI;
  if (MI.isCFIInstruction() || MI.isCall() || MI.isReturn() || MI.isBranch() ||
      MI.isIndirectBranch() || MI.isTerminator() || isFrameInstr(MI) || MI.isInlineAsm() ||
      MI.isPosition())
    return outliner::InstrType::Illegal;
  if (getInstSizeInBytes(MI) == 0 && !MI.isMetaInstruction())
    return outliner::InstrType::Illegal;
  const TargetRegisterInfo *TRI = MI.getMF()->getSubtarget().getRegisterInfo();
  if (MI.modifiesRegister(DSPIC::SP, TRI) || MI.readsRegister(DSPIC::SP, TRI))
    return outliner::InstrType::Illegal;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isJTI() || MO.isBlockAddress() || MO.isCPI())
      return outliner::InstrType::Illegal;
  return outliner::InstrType::Legal;
}

void DSPICInstrInfo::buildOutlinedFrame(MachineBasicBlock &MBB, MachineFunction &MF,
                                        const outliner::OutlinedFunction &OF) const {
  MBB.insert(MBB.end(), BuildMI(MF, DebugLoc(), get(DSPIC::RET)));
}

MachineBasicBlock::iterator
DSPICInstrInfo::insertOutlinedCall(Module &M, MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator &It, MachineFunction &MF,
                                   outliner::Candidate &C) const {
  It = MBB.insert(It, BuildMI(MF, DebugLoc(), get(DSPIC::RCALLi))
                          .addGlobalAddress(M.getNamedValue(MF.getName()), 0, 0));
  return It;
}
