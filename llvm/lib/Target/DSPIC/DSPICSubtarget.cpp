//===-- DSPICSubtarget.cpp - DSPIC Subtarget Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DSPIC specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "DSPICSubtarget.h"
#include "DSPICSelectionDAGInfo.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "dspic-subtarget"

static cl::opt<DSPICSubtarget::HWMultEnum>
HWMultModeOption("dspic-mhwmult", cl::Hidden,
           cl::desc("Hardware multiplier use mode for DSPIC"),
           cl::init(DSPICSubtarget::NoHWMult),
           cl::values(
             clEnumValN(DSPICSubtarget::NoHWMult, "none",
                "Do not use hardware multiplier"),
             clEnumValN(DSPICSubtarget::HWMult16, "16bit",
                "Use 16-bit hardware multiplier"),
             clEnumValN(DSPICSubtarget::HWMult32, "32bit",
                "Use 32-bit hardware multiplier"),
             clEnumValN(DSPICSubtarget::HWMultF5, "f5series",
                "Use F5 series hardware multiplier")));

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "DSPICGenSubtargetInfo.inc"

void DSPICSubtarget::anchor() { }

DSPICSubtarget &
DSPICSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS) {
  ExtendedInsts = false;
  LargeCode = false;
  HWMultMode = NoHWMult;

  StringRef CPUName = CPU;
  if (CPUName.empty())
    CPUName = "dspic";

  ParseSubtargetFeatures(CPUName, /*TuneCPU*/ CPUName, FS);

  if (HWMultModeOption != NoHWMult)
    HWMultMode = HWMultModeOption;

  return *this;
}

DSPICSubtarget::DSPICSubtarget(const Triple &TT, const std::string &CPU,
                                 const std::string &FS, const TargetMachine &TM)
    : DSPICGenSubtargetInfo(TT, CPU, /*TuneCPU*/ CPU, FS),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      FrameLowering(*this) {
  TSInfo = std::make_unique<DSPICSelectionDAGInfo>();
}

DSPICSubtarget::~DSPICSubtarget() = default;

const SelectionDAGTargetInfo *DSPICSubtarget::getSelectionDAGInfo() const {
  return TSInfo.get();
}

void DSPICSubtarget::initLibcallLoweringInfo(LibcallLoweringInfo &Info) const {
  // The memcpy/memset row (trellis session 87 post-close): a block op that is not inlined
  // (a variable length, a memmove, a nonzero memset) falls to a NAMED libcall. Without a
  // name the fallback read getTargetExternalSymbol(nullptr) and aborted -- the memset
  // refuter's crash. cc1 calls the same routines (`bra _memcpy`/`_memmove`/`_memset`).
  Info.setLibcallImpl(RTLIB::MEMCPY, RTLIB::impl_memcpy);
  Info.setLibcallImpl(RTLIB::MEMSET, RTLIB::impl_memset);
  Info.setLibcallImpl(RTLIB::MEMMOVE, RTLIB::impl_memmove);
  // (session 92) The rest of the inherited MSP430 EABI table -- the `__mspabi_*` names for every
  // multiply/divide/remainder past the hardware forms, the i32 shifts by a register, and every
  // soft-float routine and comparison -- is GONE: the vendor's libc99-pic30-elf.a exports the
  // STANDARD libgcc names (`___muldi3`, `___divsi3`, `___addsf3`, `___fixsfsi`,
  // `___ltsf2`, ...; steps/l1f/libcalls-names.txt), which are LLVM's defaults. Found when a
  // closed-formed loop sum came out `rcall ___mspabi_mpyll_hw`, a symbol no library here has.
}
