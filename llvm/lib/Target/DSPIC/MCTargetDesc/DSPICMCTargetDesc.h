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

#include "llvm/ADT/StringRef.h"
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

/// trellis session 119: -mcpu=<device> (33CK1024MP705, ...) reaches the backend as the CPU string.
/// The device was validated by the FRONT END against the pack's resource file; here every name
/// that is not one of this target's three processors (DSPIC.td) is the generic "dspic", so the MC
/// layer's "'X' is not a recognized processor for this target" is not printed for a device name.
/// Used at both subtarget-creation sites (the MC layer's and the codegen subtarget's).
StringRef dspicBackendCPU(StringRef CPU);

/// Creates a machine code emitter for DSPIC.
MCCodeEmitter *createDSPICMCCodeEmitter(const MCInstrInfo &MCII,
                                         MCContext &Ctx);

MCAsmBackend *createDSPICMCAsmBackend(const Target &T,
                                       const MCSubtargetInfo &STI,
                                       const MCRegisterInfo &MRI,
                                       const MCTargetOptions &Options);

MCTargetStreamer *
createDSPICObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI);
// trellis session 96 (follow-up 14): prints pic30 section directives; see DSPICELFStreamer.cpp.
// The two parameter types are only named here, so forward declarations are enough -- this header
// is included by every DSPIC .cpp and must not pull in MC/Support headers for one signature.
class formatted_raw_ostream;
class MCInstPrinter;
MCTargetStreamer *dspicAsmTargetStreamerCtor(MCStreamer &S,
                                             formatted_raw_ostream &OS,
                                             MCInstPrinter *IP);

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
