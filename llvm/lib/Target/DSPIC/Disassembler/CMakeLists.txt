add_llvm_component_library(LLVMDSPICDisassembler
  DSPICDisassembler.cpp

  LINK_COMPONENTS
  MCDisassembler
  MC
  DSPICInfo
  Support

  ADD_TO_COMPONENT
  DSPIC
  )
