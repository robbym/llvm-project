//===-- DSPICFrameLowering.h - Define frame lowering for DSPIC --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dsPIC33 stack frame (trellis backend plan, stage L1b; session 84).
//
// The stack grows UP and w15 points at the next free word. A function's frame:
//
//   [entry w15 - 4 .. -1]   the two-word return address, pushed by `call`
//   [entry w15 ..]          callee-saved registers, `push`/`push.d` (CSSize bytes)
//   [.. +2]                 the caller's w14, pushed by `lnk`; then w14 = w15
//   [w14 + 0 ..]            locals, spills, the emergency slot (outgoing arguments are
//                           PUSHED above w15 at each call and popped after it -- L1f-a)
//   w15 = w14 + N           after `lnk #N`
//
// LLVM's model has the local area begin 2 bytes past the entry SP (the saved w14),
// so every frame index is addressed as  w14 + (ObjectOffset - (2 + CSSize)).
// The callee-saved spill slots PEI creates are at model offsets [2, 2+CSSize) while the
// pushes really sit at [0, CSSize); no frame index ever addresses a callee-saved slot, so
// the two-byte discrepancy is invisible to code generation and only the total matters.
//
// The pushes come BEFORE `lnk` (the pic30 cc1 pushes after it): `ulnk` alone then restores
// w15 even when the frame has variable-sized objects, and LLVM's callee-saved-then-locals
// slot order is the real one.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICFRAMELOWERING_H
#define LLVM_LIB_TARGET_DSPIC_DSPICFRAMELOWERING_H

#include "DSPIC.h"
#include "llvm/CodeGen/TargetFrameLowering.h"

namespace llvm {

class DSPICSubtarget;
class DSPICInstrInfo;
class DSPICRegisterInfo;

class DSPICFrameLowering : public TargetFrameLowering {
protected:
  bool hasFPImpl(const MachineFunction &MF) const override;

public:
  DSPICFrameLowering(const DSPICSubtarget &STI);

  const DSPICSubtarget &STI;
  const DSPICInstrInfo &TII;
  const DSPICRegisterInfo *TRI;

  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;

  MachineBasicBlock::iterator
  eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                                MachineBasicBlock::iterator I) const override;

  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 ArrayRef<CalleeSavedInfo> CSI,
                                 const TargetRegisterInfo *TRI) const override;
  bool
  restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MI,
                              MutableArrayRef<CalleeSavedInfo> CSI,
                              const TargetRegisterInfo *TRI) const override;

  bool hasReservedCallFrame(const MachineFunction &MF) const override;
  /// L1f-a: PEI eliminates the call-frame pseudos inside frame-index resolution, which
  /// it skips for a function with no stack objects; a frameless caller with pushed
  /// arguments still needs its `sub.w #N,w15` after each call.
  bool needsFrameIndexResolution(const MachineFunction &MF) const override {
    return MF.getFrameInfo().hasStackObjects() || MF.getFrameInfo().adjustsStack();
  }
  void processFunctionBeforeFrameFinalized(MachineFunction &MF,
                                     RegScavenger *RS = nullptr) const override;

  StackOffset getFrameIndexReference(const MachineFunction &MF, int FI,
                                     Register &FrameReg) const override;

  /// The emergency spill slot goes right after the callee-saved slots, i.e. at
  /// [w14 + 0]: always inside the 10-bit displacement whatever the frame's size.
  bool allocateScavengingFrameIndexesNearIncomingSP(
      const MachineFunction &MF) const override {
    return true;
  }

  /// Bytes a frame index sits from w14: ObjectOffset - (2 + CSSize).
  int64_t frameOffsetFromFP(const MachineFunction &MF, int FI) const;
};

} // End llvm namespace

#endif
