//===-- DSPICMCTargetDesc.cpp - DSPIC Target Descriptions ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides DSPIC specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "DSPICMCTargetDesc.h"
#include "DSPICInstPrinter.h"
#include "DSPICMCAsmInfo.h"
#include "TargetInfo/DSPICTargetInfo.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "DSPICGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "DSPICGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "DSPICGenRegisterInfo.inc"

static MCInstrInfo *createDSPICMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitDSPICMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createDSPICMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitDSPICMCRegisterInfo(X, DSPIC::PC);
  return X;
}

static MCAsmInfo *createDSPICMCAsmInfo(const MCRegisterInfo &MRI,
                                        const Triple &TT,
                                        const MCTargetOptions &Options) {
  MCAsmInfo *MAI = new DSPICMCAsmInfo(TT, Options);

  // Initialize initial frame state.
  int stackGrowth = -2;

  // Initial state of the frame pointer is sp+ptr_size.
  MCCFIInstruction Inst = MCCFIInstruction::cfiDefCfa(
      nullptr, MRI.getDwarfRegNum(DSPIC::SP, true), -stackGrowth);
  MAI->addInitialFrameState(Inst);

  // Add return address to move list
  MCCFIInstruction Inst2 = MCCFIInstruction::createOffset(
      nullptr, MRI.getDwarfRegNum(DSPIC::PC, true), stackGrowth);
  MAI->addInitialFrameState(Inst2);

  return MAI;
}

static MCSubtargetInfo *
createDSPICMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  return createDSPICMCSubtargetInfoImpl(TT, CPU, /*TuneCPU*/ CPU, FS);
}

static MCInstPrinter *createDSPICMCInstPrinter(const Triple &T,
                                                unsigned SyntaxVariant,
                                                const MCAsmInfo &MAI,
                                                const MCInstrInfo &MII,
                                                const MCRegisterInfo &MRI) {
  if (SyntaxVariant == 0)
    return new DSPICInstPrinter(MAI, MII, MRI);
  return nullptr;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeDSPICTargetMC() {
  Target &T = getTheDSPICTarget();

  TargetRegistry::RegisterMCAsmInfo(T, createDSPICMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createDSPICMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createDSPICMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createDSPICMCSubtargetInfo);
  TargetRegistry::RegisterMCInstPrinter(T, createDSPICMCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createDSPICMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createDSPICMCAsmBackend);
  TargetRegistry::RegisterObjectTargetStreamer(
      T, createDSPICObjectTargetStreamer);
}
