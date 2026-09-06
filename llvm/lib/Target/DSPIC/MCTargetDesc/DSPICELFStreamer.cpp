//===-- DSPICELFStreamer.cpp - DSPIC ELF Target Streamer Methods --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides DSPIC specific target streamer methods.
//
//===----------------------------------------------------------------------===//

#include "DSPICMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/MSP430Attributes.h"

using namespace llvm;
using namespace llvm::MSP430Attrs;

namespace llvm {

class DSPICTargetELFStreamer : public MCTargetStreamer {
public:
  MCELFStreamer &getStreamer();
  DSPICTargetELFStreamer(MCStreamer &S, const MCSubtargetInfo &STI);
};

// This part is for ELF object output.
DSPICTargetELFStreamer::DSPICTargetELFStreamer(MCStreamer &S,
                                                 const MCSubtargetInfo &STI)
    : MCTargetStreamer(S) {
  // Emit build attributes section according to
  // DSPIC EABI (slaa534.pdf, part 13).
  MCSection *AttributeSection = getStreamer().getContext().getELFSection(
      ".DSPIC.attributes", ELF::SHT_MSP430_ATTRIBUTES, 0);
  Streamer.switchSection(AttributeSection);

  // Format version.
  Streamer.emitInt8(0x41);
  // Subsection length.
  Streamer.emitInt32(22);
  // Vendor name string, zero-terminated.
  Streamer.emitBytes("mspabi");
  Streamer.emitInt8(0);

  // Attribute vector scope tag. 1 stands for the entire file.
  Streamer.emitInt8(1);
  // Attribute vector length.
  Streamer.emitInt32(11);

  Streamer.emitInt8(TagISA);
  Streamer.emitInt8(STI.hasFeature(DSPIC::FeatureX) ? ISAMSP430X : ISAMSP430);
  Streamer.emitInt8(TagCodeModel);
  Streamer.emitInt8(CMSmall);
  Streamer.emitInt8(TagDataModel);
  Streamer.emitInt8(DMSmall);
  // Don't emit TagEnumSize, for full GCC compatibility.
}

MCELFStreamer &DSPICTargetELFStreamer::getStreamer() {
  return static_cast<MCELFStreamer &>(Streamer);
}

MCTargetStreamer *
createDSPICObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  const Triple &TT = STI.getTargetTriple();
  if (TT.isOSBinFormatELF())
    return new DSPICTargetELFStreamer(S, STI);
  return nullptr;
}

} // namespace llvm
