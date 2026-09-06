//===-- DSPICRegisterInfo.cpp - DSPIC Register Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DSPIC implementation of the TargetRegisterInfo class.
//
// Frame indices (trellis L1b, session 84): every one is w14-relative — see
// DSPICFrameLowering.h for the layout. A `mov` with a displacement inside the
// assembler's range ([-1024, 1022] even for words, [-512, 511] for bytes) keeps its
// form; past it the offset is materialized (`mov #off,wT`) and the access becomes
// register-indexed (`[w14+wT]`), wT a virtual register PEI scavenges afterwards. An
// address-of (ADDframe) is `add.w w14,#lit5,wD`, `sub.w w14,#lit5,wD`, a plain move
// at offset 0, or `mov #off,wD; add.w w14,wD,wD`.
//
//===----------------------------------------------------------------------===//

#include "DSPICRegisterInfo.h"
#include "DSPICFrameLowering.h"
#include "DSPICMachineFunctionInfo.h"
#include "DSPICTargetMachine.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

#define DEBUG_TYPE "dspic-reg-info"

#define GET_REGINFO_TARGET_DESC
#include "DSPICGenRegisterInfo.inc"

DSPICRegisterInfo::DSPICRegisterInfo()
  : DSPICGenRegisterInfo(DSPIC::PC) {}

// w8..w13 (internal R10..R5) are callee-saved; w14 is the frame pointer and always
// reserved, so it is never in the list. The interrupt list is stage L1d's to revise.
const MCPhysReg*
DSPICRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  const Function* F = &MF->getFunction();
  static const MCPhysReg CalleeSavedRegs[] = {
    DSPIC::R5, DSPIC::R6, DSPIC::R7,
    DSPIC::R8, DSPIC::R9, DSPIC::R10,
    0
  };
  // L1d (trellis session 88): an ISR may be interrupted at any point, so every GPR it
  // CLOBBERS must be saved -- getCalleeSavedRegs offers ALL of w0..w13 and PEI spills only
  // the used subset (a leaf saves its few; an ISR that calls saves the whole caller-saved
  // set, the callee's clobbers unknown). w14 (FP) and w15 (SP) are handled by the frame.
  // The inherited list was MISSING W5/W6/W7 (w5/w6/w7) -- a correctness hole.
  static const MCPhysReg CalleeSavedRegsIntr[] = {
    DSPIC::R12, DSPIC::R13, DSPIC::R14, DSPIC::R15,  // w0..w3
    DSPIC::R11, DSPIC::W5,  DSPIC::W6,  DSPIC::W7,   // w4..w7
    DSPIC::R10, DSPIC::R9,  DSPIC::R8,               // w8..w10
    DSPIC::R7,  DSPIC::R6,  DSPIC::R5,               // w11..w13
    0
  };
  return ((F->getCallingConv() == CallingConv::MSP430_INTR ||
           F->hasFnAttribute("interrupt")) ?
          CalleeSavedRegsIntr : CalleeSavedRegs);
}

// w14 is reserved in every function (COSTED: one register lost to frameless functions;
// the alternative, allocating it when no frame is linked, needs hasFP decided before
// register allocation, which spills created during allocation can falsify).
BitVector DSPICRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());

  Reserved.set(DSPIC::PCB);
  Reserved.set(DSPIC::SPB);
  Reserved.set(DSPIC::SRB);
  Reserved.set(DSPIC::CGB);
  Reserved.set(DSPIC::PC);
  Reserved.set(DSPIC::SP);
  Reserved.set(DSPIC::SR);
  Reserved.set(DSPIC::CG);
  Reserved.set(DSPIC::R4B);
  Reserved.set(DSPIC::R4);

  return Reserved;
}

const TargetRegisterClass *
DSPICRegisterInfo::getPointerRegClass(unsigned Kind) const {
  return &DSPIC::GR16RegClass;
}

bool DSPICRegisterInfo::requiresRegisterScavenging(
    const MachineFunction &MF) const {
  return true;
}

bool DSPICRegisterInfo::requiresFrameIndexScavenging(
    const MachineFunction &MF) const {
  return true;
}

// The assembler's displacement ranges (tools/dspic-llvm/prints/l1b/probe3.log).
static bool displacementFits(int64_t Off, bool Byte) {
  if (Byte)
    return Off >= -512 && Off <= 511;
  return Off >= -1024 && Off <= 1022 && (Off % 2) == 0;
}

// The register-indexed twin of a frame-addressing `mov`, or 0 when there is none.
static unsigned indexedForm(unsigned Opc) {
  switch (Opc) {
  case DSPIC::MOV16rm: return DSPIC::MOV16rx;
  case DSPIC::MOV16mr: return DSPIC::MOV16xr;
  case DSPIC::MOV8rm: return DSPIC::MOV8rx;
  case DSPIC::MOV8mr: return DSPIC::MOV8xr;
  case DSPIC::MOVZX16rm8: return DSPIC::MOVZX16rx8;
  default: return 0;
  }
}

static bool isByteAccess(unsigned Opc) {
  return Opc == DSPIC::MOV8rm || Opc == DSPIC::MOV8mr ||
         Opc == DSPIC::MOVZX16rm8;
}

bool
DSPICRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                        int SPAdj, unsigned FIOperandNum,
                                        RegScavenger *RS) const {
  // SPAdj is PEI's running w15 adjustment inside a call sequence. Every frame index
  // here is w14-relative and w14 never moves inside one (a reserved call frame moves
  // nothing; beside variable-sized objects only w15 moves), so it is not consulted.
  (void)SPAdj;

  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  const DSPICFrameLowering *TFI = getFrameLowering(MF);
  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  DebugLoc dl = MI.getDebugLoc();
  int FrameIndex = MI.getOperand(FIOperandNum).getIndex();

  assert(TFI->hasFP(MF) && "a frame index in a function with no linked frame");
  int64_t Offset = TFI->frameOffsetFromFP(MF, FrameIndex) +
                   MI.getOperand(FIOperandNum + 1).getImm();

  if (MI.getOpcode() == DSPIC::ADDframe) {
    // "load effective address" of the slot into $dst.
    Register DstReg = MI.getOperand(0).getReg();
    if (Offset == 0) {
      MI.setDesc(TII.get(DSPIC::MOV16rr));
      MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
      MI.removeOperand(FIOperandNum + 1);
    } else if (Offset > 0 && Offset < 32) {
      MI.setDesc(TII.get(DSPIC::ADD16rri_lea));
      MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
    } else if (Offset < 0 && Offset > -32) {
      MI.setDesc(TII.get(DSPIC::SUB16rri_lea));
      MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
      MI.getOperand(FIOperandNum + 1).ChangeToImmediate(-Offset);
    } else {
      BuildMI(MBB, II, dl, TII.get(DSPIC::MOV16ri), DstReg).addImm(Offset);
      MI.setDesc(TII.get(DSPIC::ADD16rrr));
      MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
      MI.getOperand(FIOperandNum + 1).ChangeToRegister(DstReg, false, false,
                                                       /*isKill=*/true);
    }
    return false;
  }

  bool Byte = isByteAccess(MI.getOpcode());
  if (displacementFits(Offset, Byte)) {
    MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
    MI.getOperand(FIOperandNum + 1).ChangeToImmediate(Offset);
    return false;
  }

  if (!Byte && (Offset % 2) != 0)
    report_fatal_error("dspic: a word access at the odd frame offset " +
                       Twine(Offset));

  unsigned Indexed = indexedForm(MI.getOpcode());
  if (!Indexed)
    report_fatal_error("dspic: frame offset " + Twine(Offset) +
                       " is past the 10-bit displacement in an instruction "
                       "with no register-indexed form: " +
                       TII.getName(MI.getOpcode()));

  // mov #off,wT ; ... [w14+wT] — wT is scavenged by PEI after this pass.
  Register Tmp = MF.getRegInfo().createVirtualRegister(&DSPIC::GR16RegClass);
  BuildMI(MBB, II, dl, TII.get(DSPIC::MOV16ri), Tmp).addImm(Offset);
  MI.setDesc(TII.get(Indexed));
  MI.getOperand(FIOperandNum).ChangeToRegister(DSPIC::R4, false);
  MI.getOperand(FIOperandNum + 1).ChangeToRegister(Tmp, false, false,
                                                   /*isKill=*/true);
  return false;
}

Register DSPICRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  return DSPIC::R4;
}
