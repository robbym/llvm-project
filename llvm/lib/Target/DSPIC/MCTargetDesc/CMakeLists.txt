add_llvm_component_library(LLVMDSPICDesc
  DSPICAsmBackend.cpp
  DSPICELFObjectWriter.cpp
  DSPICELFStreamer.cpp
  DSPICInstPrinter.cpp
  DSPICMCAsmInfo.cpp
  DSPICMCCodeEmitter.cpp
  DSPICMCTargetDesc.cpp

  LINK_COMPONENTS
  MC
  DSPICInfo
  Support

  ADD_TO_COMPONENT
  DSPIC
  )
