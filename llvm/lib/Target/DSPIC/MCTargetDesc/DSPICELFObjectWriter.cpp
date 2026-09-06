//===-- DSPICELFObjectWriter.cpp - DSPIC ELF Writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DSPICFixupKinds.h"
#include "MCTargetDesc/DSPICMCTargetDesc.h"

#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {
class DSPICELFObjectWriter : public MCELFObjectTargetWriter {
public:
  DSPICELFObjectWriter(uint8_t OSABI)
    : MCELFObjectTargetWriter(false, OSABI, ELF::EM_MSP430,
                              /*HasRelocationAddend*/ true) {}

  ~DSPICELFObjectWriter() override = default;

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &,
                        bool IsPCRel) const override {
    // Translate fixup kind to ELF relocation type.
    switch (Fixup.getKind()) {
    case FK_Data_1:                   return ELF::R_MSP430_8;
    case FK_Data_2:                   return ELF::R_MSP430_16_BYTE;
    case FK_Data_4:                   return ELF::R_MSP430_32;
    case DSPIC::fixup_32:            return ELF::R_MSP430_32;
    case DSPIC::fixup_10_pcrel:      return ELF::R_MSP430_10_PCREL;
    case DSPIC::fixup_16:            return ELF::R_MSP430_16;
    case DSPIC::fixup_16_pcrel:      return ELF::R_MSP430_16_PCREL;
    case DSPIC::fixup_16_byte:       return ELF::R_MSP430_16_BYTE;
    case DSPIC::fixup_16_pcrel_byte: return ELF::R_MSP430_16_PCREL_BYTE;
    case DSPIC::fixup_2x_pcrel:      return ELF::R_MSP430_2X_PCREL;
    case DSPIC::fixup_rl_pcrel:      return ELF::R_MSP430_RL_PCREL;
    case DSPIC::fixup_8:             return ELF::R_MSP430_8;
    case DSPIC::fixup_sym_diff:      return ELF::R_MSP430_SYM_DIFF;
    default:
      llvm_unreachable("Invalid fixup kind");
    }
  }
};
} // end of anonymous namespace

std::unique_ptr<MCObjectTargetWriter>
llvm::createDSPICELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<DSPICELFObjectWriter>(OSABI);
}
