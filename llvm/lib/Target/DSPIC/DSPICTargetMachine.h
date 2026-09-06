//===-- DSPICTargetMachine.h - Define TargetMachine for DSPIC -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the DSPIC specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//


#ifndef LLVM_LIB_TARGET_DSPIC_DSPICTARGETMACHINE_H
#define LLVM_LIB_TARGET_DSPIC_DSPICTARGETMACHINE_H

#include "DSPICSubtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include <optional>

namespace llvm {
class StringRef;

/// DSPICTargetMachine
///
class DSPICTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  DSPICSubtarget Subtarget;

public:
  DSPICTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                      StringRef FS, const TargetOptions &Options,
                      std::optional<Reloc::Model> RM,
                      std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                      bool JIT);
  ~DSPICTargetMachine() override;

  const DSPICSubtarget *getSubtargetImpl(const Function &F) const override {
    return &Subtarget;
  }
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override;

  void registerPassBuilderCallbacks(PassBuilder &PB) override;

  Error buildCodeGenPipeline(ModulePassManager &MPM, ModuleAnalysisManager &MAM,
                             raw_pwrite_stream &Out, raw_pwrite_stream *DwoOut,
                             CodeGenFileType FileType,
                             const CGPassBuilderOption &Opt, MCContext &Ctx,
                             PassInstrumentationCallbacks *PIC) override;

  bool shouldDefaultToNewPM() const override { return true; }
}; // DSPICTargetMachine.

} // end namespace llvm

#endif
