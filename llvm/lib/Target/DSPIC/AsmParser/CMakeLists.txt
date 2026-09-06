add_llvm_component_library(LLVMDSPICAsmParser
  DSPICAsmParser.cpp

  LINK_COMPONENTS
  CodeGenTypes
  MC
  MCParser
  DSPICDesc
  DSPICInfo
  Support

  ADD_TO_COMPONENT
  DSPIC
  )
