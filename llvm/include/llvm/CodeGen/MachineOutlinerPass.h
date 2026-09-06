//===- MachineOutlinerPass.h - Outliner (new PM) ----------------*- C++ -*-===//
//
// New pass manager wrapper for the MachineOutliner. Ported so new-PM codegen
// pipelines (CodeGenPassBuilder) actually run the outliner instead of a stub.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_MACHINEOUTLINERPASS_H
#define LLVM_CODEGEN_MACHINEOUTLINERPASS_H

#include "llvm/IR/PassManager.h"
#include "llvm/Target/CGPassBuilderOption.h"

namespace llvm {

class Module;

class MachineOutlinerPass : public OptionalPassInfoMixin<MachineOutlinerPass> {
  RunOutliner Mode;

public:
  MachineOutlinerPass(RunOutliner Mode = RunOutliner::TargetDefault)
      : Mode(Mode) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

};

} // namespace llvm

#endif // LLVM_CODEGEN_MACHINEOUTLINERPASS_H
