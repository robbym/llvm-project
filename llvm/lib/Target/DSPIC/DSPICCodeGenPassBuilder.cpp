//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "DSPICAsmPrinter.h"
#include "DSPICTargetMachine.h"

#include "llvm/CodeGen/AtomicExpand.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Passes/CodeGenPassBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Target/CGPassBuilderOption.h"

using namespace llvm;

namespace {

class DSPICCodeGenPassBuilder : public CodeGenPassBuilder {
  using Base = CodeGenPassBuilder;

  DSPICTargetMachine &getTM() const {
    return static_cast<DSPICTargetMachine &>(TM);
  }

public:
  explicit DSPICCodeGenPassBuilder(DSPICTargetMachine &TM,
                                    const CGPassBuilderOption &Opts,
                                    PassInstrumentationCallbacks *PIC)
      : CodeGenPassBuilder(TM, Opts, PIC) {}

  void addIRPasses(PassManagerWrapper &PMW) override;
  Error addInstSelector(PassManagerWrapper &PMW) override;
  void addPreEmitPass(PassManagerWrapper &PMW) override;
  void addAsmPrinterBegin(PassManagerWrapper &PMW) override;
  void addAsmPrinter(PassManagerWrapper &PMW) override;
  void addAsmPrinterEnd(PassManagerWrapper &PMW) override;
};

void DSPICCodeGenPassBuilder::addIRPasses(PassManagerWrapper &PMW) {
  addFunctionPass(AtomicExpandPass(TM), PMW);

  Base::addIRPasses(PMW);
}

Error DSPICCodeGenPassBuilder::addInstSelector(PassManagerWrapper &PMW) {
  addMachineFunctionPass(DSPICISelDAGToDAGPass(getTM(), getOptLevel()), PMW);
  return Error::success();
}

void DSPICCodeGenPassBuilder::addPreEmitPass(PassManagerWrapper &PMW) {
  addMachineFunctionPass(DSPICPeepholePass(), PMW);
  addMachineFunctionPass(DSPICBranchSelectPass(), PMW);
}

void DSPICCodeGenPassBuilder::addAsmPrinterBegin(PassManagerWrapper &PMW) {
  addModulePass(DSPICAsmPrinterBeginPass(), PMW, /*Force=*/true);
}

void DSPICCodeGenPassBuilder::addAsmPrinter(PassManagerWrapper &PMW) {
  addMachineFunctionPass(DSPICAsmPrinterPass(), PMW);
}

void DSPICCodeGenPassBuilder::addAsmPrinterEnd(PassManagerWrapper &PMW) {
  addModulePass(DSPICAsmPrinterEndPass(), PMW);
}

} // namespace

void DSPICTargetMachine::registerPassBuilderCallbacks(PassBuilder &PB){
#define GET_PASS_REGISTRY "DSPICPassRegistry.def"
#include "llvm/Passes/TargetPassRegistry.inc"
}

Error DSPICTargetMachine::buildCodeGenPipeline(
    ModulePassManager &MPM, ModuleAnalysisManager &MAM, raw_pwrite_stream &Out,
    raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
    const CGPassBuilderOption &Opt, MCContext &Ctx,
    PassInstrumentationCallbacks *PIC) {
  auto CGPB = DSPICCodeGenPassBuilder(*this, Opt, PIC);
  return CGPB.buildPipeline(MPM, MAM, Out, DwoOut, FileType, Ctx);
}
