//===-- DSPICRegisterInfo.h - DSPIC Register Information Impl -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DSPIC implementation of the MRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICREGISTERINFO_H
#define LLVM_LIB_TARGET_DSPIC_DSPICREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "DSPICGenRegisterInfo.inc"

namespace llvm {

class DSPICRegisterInfo : public DSPICGenRegisterInfo {
public:
  DSPICRegisterInfo();

  /// Code Generation virtual methods...
  const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;

  BitVector getReservedRegs(const MachineFunction &MF) const override;
  const TargetRegisterClass *
  getPointerRegClass(unsigned Kind = 0) const override;

  /// Frame indices past the 10-bit displacement need a scratch register: a virtual
  /// one is created in eliminateFrameIndex and PEI scavenges it afterwards (L1b).
  /// (session 95) Refuse a coalesce that would give a long-lived value a ONE-REGISTER class.
  /// The WREG-only file forms take GR16_W0/GR8_W0, and merging a copy into one of those spreads
  /// the constraint over the source's whole live range; two such values cannot coexist and the
  /// allocator aborts outright. See crashfix.py for the trace.
  bool shouldCoalesce(MachineInstr *MI, const TargetRegisterClass *SrcRC, unsigned SubReg,
                      const TargetRegisterClass *DstRC, unsigned DstSubReg,
                      const TargetRegisterClass *NewRC, LiveIntervals &LIS) const override;

  bool requiresRegisterScavenging(const MachineFunction &MF) const override;
  bool requiresFrameIndexScavenging(const MachineFunction &MF) const override;

  bool eliminateFrameIndex(MachineBasicBlock::iterator II,
                           int SPAdj, unsigned FIOperandNum,
                           RegScavenger *RS = nullptr) const override;

  // Session 93 (the CSR-vs-remat row): the words a callee-saved register's first save costs --
  // push and pop, or nothing when its push.d partner is already saved -- so RAGreedy
  // rematerializes a cheap constant instead of saving a register for it
  // (RegAllocGreedy.cpp::shouldRematInsteadOfCSR). Under -mllvm -dspic-remat-over-csr=0 it is 0
  // and the rule is off (the row's own red).
  unsigned getCSRFirstUseSizeCost(const MachineFunction &MF, MCRegister PhysReg,
                                  function_ref<bool(MCRegister)> IsUsed) const override;

  // Debug information queries.
  Register getFrameRegister(const MachineFunction &MF) const override;
};

} // end namespace llvm

#endif
