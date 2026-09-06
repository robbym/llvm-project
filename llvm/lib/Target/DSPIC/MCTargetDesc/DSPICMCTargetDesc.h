//===-- DSPICMCTargetDesc.h - DSPIC Target Descriptions -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides DSPIC specific target descriptions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_MCTARGETDESC_DSPICMCTARGETDESC_H
#define LLVM_LIB_TARGET_DSPIC_MCTARGETDESC_DSPICMCTARGETDESC_H

#include "llvm/Support/DataTypes.h"
#include <memory>

namespace llvm {
class Target;
class MCAsmBackend;
class MCCodeEmitter;
class MCInstrInfo;
class MCSubtargetInfo;
class MCRegisterInfo;
class MCContext;
class MCTargetOptions;
class MCObjectTargetWriter;
class MCStreamer;
class MCTargetStreamer;

/// Creates a machine code emitter for DSPIC.
MCCodeEmitter *createDSPICMCCodeEmitter(const MCInstrInfo &MCII,
                                         MCContext &Ctx);

MCAsmBackend *createDSPICMCAsmBackend(const Target &T,
                                       const MCSubtargetInfo &STI,
                                       const MCRegisterInfo &MRI,
                                       const MCTargetOptions &Options);

MCTargetStreamer *
createDSPICObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI);

std::unique_ptr<MCObjectTargetWriter>
createDSPICELFObjectWriter(uint8_t OSABI);

} // End llvm namespace

// Defines symbolic names for DSPIC registers.
// This defines a mapping from register name to register number.
#define GET_REGINFO_ENUM
#include "DSPICGenRegisterInfo.inc"

// Defines symbolic names for the DSPIC instructions.
#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "DSPICGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "DSPICGenSubtargetInfo.inc"

#endif
