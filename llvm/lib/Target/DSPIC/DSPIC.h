//==-- DSPIC.h - Top-level interface for DSPIC representation --*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in
// the LLVM DSPIC backend.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPIC_H
#define LLVM_LIB_TARGET_DSPIC_DSPIC_H

#include "MCTargetDesc/DSPICMCTargetDesc.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Target/TargetMachine.h"

namespace DSPICCC {
  // DSPIC specific condition code.
  enum CondCodes {
    COND_E  = 0,  // aka COND_Z
    COND_NE = 1,  // aka COND_NZ
    COND_HS = 2,  // aka COND_C
    COND_LO = 3,  // aka COND_NC
    COND_GE = 4,
    COND_L  = 5,
    COND_N  = 6,  // jump if negative
    COND_NONE,    // unconditional

    COND_INVALID = -1
  };
}

namespace llvm {
class FunctionPass;
class DSPICTargetMachine;
class PassRegistry;

class DSPICISelDAGToDAGPass : public SelectionDAGISelPass {
public:
  DSPICISelDAGToDAGPass(DSPICTargetMachine &TM, CodeGenOptLevel OptLevel);
};

FunctionPass *createDSPICISelDag(DSPICTargetMachine &TM,
                                  CodeGenOptLevel OptLevel);

class DSPICBranchSelectPass
    : public RequiredPassInfoMixin<DSPICBranchSelectPass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createDSPICBranchSelectLegacyPass();

/// The fused forms (retlw, mov.d, push.d) -- trellis L1f-a.
class DSPICPeepholePass : public RequiredPassInfoMixin<DSPICPeepholePass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

FunctionPass *createDSPICPeepholeLegacyPass();

void initializeDSPICAsmPrinterPass(PassRegistry &);
void initializeDSPICDAGToDAGISelLegacyPass(PassRegistry &);

} // namespace llvm

#endif
