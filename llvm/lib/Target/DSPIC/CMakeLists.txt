add_llvm_component_group(DSPIC)

set(LLVM_TARGET_DEFINITIONS DSPIC.td)

tablegen(LLVM DSPICGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM DSPICGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM DSPICGenCallingConv.inc -gen-callingconv)
tablegen(LLVM DSPICGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM DSPICGenDisassemblerTables.inc -gen-disassembler)
tablegen(LLVM DSPICGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM DSPICGenMCCodeEmitter.inc -gen-emitter)
tablegen(LLVM DSPICGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM DSPICGenSDNodeInfo.inc -gen-sd-node-info)
tablegen(LLVM DSPICGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(DSPICCommonTableGen)

add_llvm_target(DSPICCodeGen
  DSPICBranchSelector.cpp
  DSPICCodeGenPassBuilder.cpp
  DSPICISelDAGToDAG.cpp
  DSPICISelLowering.cpp
  DSPICInstrInfo.cpp
  DSPICFrameLowering.cpp
  DSPICMachineFunctionInfo.cpp
  DSPICPeephole.cpp
  DSPICRegisterInfo.cpp
  DSPICSelectionDAGInfo.cpp
  DSPICSubtarget.cpp
  DSPICTargetMachine.cpp
  DSPICAsmPrinter.cpp
  DSPICMCInstLower.cpp

  LINK_COMPONENTS
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  MC
  DSPICDesc
  DSPICInfo
  Passes
  SelectionDAG
  Support
  Target
  TargetParser

  ADD_TO_COMPONENT
  DSPIC
  )

add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
