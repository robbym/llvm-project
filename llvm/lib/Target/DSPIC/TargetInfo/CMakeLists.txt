add_llvm_component_library(LLVMDSPICInfo
  DSPICTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  DSPIC
  )
