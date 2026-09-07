//===-- DSPICInstrInfo.h - DSPIC Instruction Information ------*- C++ -*-===//
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

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICINSTRINFO_H
#define LLVM_LIB_TARGET_DSPIC_DSPICINSTRINFO_H

#include "DSPICRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "DSPICGenInstrInfo.inc"

namespace llvm {

class DSPICSubtarget;

class DSPICInstrInfo : public DSPICGenInstrInfo {
  const DSPICRegisterInfo RI;
  virtual void anchor();
public:
  explicit DSPICInstrInfo(const DSPICSubtarget &STI);

  /// getRegisterInfo - TargetInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  ///
  const DSPICRegisterInfo &getRegisterInfo() const { return RI; }

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  void storeRegToStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
      bool isKill, int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;
  void loadRegFromStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
      int FrameIdx, const TargetRegisterClass *RC, Register VReg,
      unsigned SubReg = 0,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  unsigned getInstSizeInBytes(const MachineInstr &MI) const override;

  // Branch folding goodness
  bool
  reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const override;
  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify) const override;

  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;

  bool isFunctionSafeToOutlineFrom(MachineFunction &MF,
                                   bool OutlineFromLinkOnceODRs) const override;
  bool isMBBSafeToOutlineFrom(MachineBasicBlock &MBB, unsigned &Flags) const override;
  bool shouldOutlineFromFunctionByDefault(MachineFunction &MF) const override;
  std::optional<std::unique_ptr<outliner::OutlinedFunction>> getOutliningCandidateInfo(
      const MachineModuleInfo &MMI,
      std::vector<outliner::Candidate> &RepeatedSequenceLocs,
      unsigned MinRepeats) const override;
  outliner::InstrType getOutliningTypeImpl(const MachineModuleInfo &MMI,
                                           MachineBasicBlock::iterator &MBBI,
                                           unsigned Flags) const override;
  void buildOutlinedFrame(MachineBasicBlock &MBB, MachineFunction &MF,
                          const outliner::OutlinedFunction &OF) const override;
  MachineBasicBlock::iterator
  insertOutlinedCall(Module &M, MachineBasicBlock &MBB,
                     MachineBasicBlock::iterator &It, MachineFunction &MF,
                     outliner::Candidate &C) const override;

  // ---- REMAT-PLAN (trellis session 94): rematerializing a near-global load ----------------
  /// Is MI a load of a whole near, non-far, non-volatile, non-atomic, mutable data global by
  /// its bare file address? That is the class REMAT-PLAN section 3 restricts remat to. GVOut,
  /// when given, receives the global the load reads.
  bool isRematerializableNearGlobalLoad(const MachineInstr &MI,
                                        const GlobalValue **GVOut = nullptr) const;
  /// Does MI's memory operand carry the IR pass's verdict (MOTargetFlag1)? The ONE place that
  /// verdict is consulted; -dspic-remat-force stands in for the pass until Phase 3.
  bool hasRematVerdict(const MachineInstr &MI) const;

  /// (session 95) A three-operand ALU form whose ONLY physical operand is a DEAD def of SR.
  /// Its value is a function of its virtual inputs alone; allUsesAvailableAt proves those are
  /// live and unchanged at the use, and LiveRangeEdit checks the flag clobber at the remat
  /// point, which is where `dead at the original site` stops meaning anything.
  bool isRematerializableALU(const MachineInstr &MI) const;

  bool isReMaterializableImpl(const MachineInstr &MI) const override;
  bool isIgnorableUse(const MachineInstr &MI, unsigned OpIdx) const override;
  bool isMemoryRematCandidate(const MachineInstr &MI) const override;
  bool isMemoryRematClobber(const MachineInstr &Orig,
                            const MachineInstr &MI) const override;

  int64_t getFramePoppedByCallee(const MachineInstr &I) const {
    assert(isFrameInstr(I) && "Not a frame instruction");
    assert(I.getOperand(1).getImm() >= 0 && "Size must not be negative");
    return I.getOperand(1).getImm();
  }
};

/// (REMAT-PLAN 4.2, trellis session 94) The IR-level twin of the MIR class above: the same
/// restrictions, stated on a LoadInst. Phase 3's annotation pass will add the MemorySSA proof
/// on top of this; today -dspic-remat-force stands in for it.
bool DSPICIsRematerializableNearGlobalLoad(const Instruction &I);

/// The getTargetMMOFlags body, kept beside the class it belongs to.
MachineMemOperand::Flags DSPICGetRematMMOFlags(const Instruction &I);

}

#endif
