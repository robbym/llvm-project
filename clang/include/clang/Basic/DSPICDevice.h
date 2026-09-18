//===--- DSPICDevice.h - the dsPIC device, for the front end AND the driver -*- C++ -*-===//
//
// trellis session 120. The device the vendor's -mcpu=<name> -mdfp=<pack> names, resolved the way
// cc1 resolves it (pic30.c:1637 validate_target_id): first against the nine in-source GENERIC-*
// names (pic30.c:1655-1690 generic_devices[], which never reach the file), then against the
// pack's <dfp>/bin/c30_device.info. Declared here because TWO consumers need the FAMILY: the front
// end spells the family macro from it (DSPIC.cpp validateTarget) and the driver spells the pack's
// per-family include directory from it (Clang.cpp AddDSPICTargetArgs, pic30.c:25960
// pic30_default_include_path). One reader, in DSPIC.cpp; two callers; no second parser.
//
//===----------------------------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_DSPICDEVICE_H
#define LLVM_CLANG_BASIC_DSPICDEVICE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang {
namespace dspic {

struct Device {
  bool Known = false;    // describeDevice returned ""
  bool Generic = false;  // one of the nine GENERIC-* names: no record, no __IVT_NUM, no device macro
  unsigned Flags = 0;    // the record's flags word (c30_flag_definitions.h), or the table's device mask
  unsigned Id = 0;       // the device id (0 for a generic)
  unsigned IVT = 0;      // the vector records naming this device (0 for a generic)
  std::string Macro;     // a generic name's macro, `__GENERIC_16DSP__` (pic30.c:4319-4331); else ""
};

/// Resolve CPU (as written, uppercased -- cc1 TOUPPERs it) against the generic table and then the
/// resource file <DFP>/bin/c30_device.info. Returns "" on success (Out.Known set), or one of
/// "nodfp" (a non-generic name with no pack), "open", "format", "cpu" -- the caller owns the sentence.
std::string describeDevice(llvm::StringRef DFP, llvm::StringRef CPU, Device &Out);

/// cc1's family macro from the flags -- pic30.c:1789-1798's ten overwriting ifs, later wins, then
/// :4255-4280's spelling. "" for a generic (its family slot is Macro).
const char *familyMacro(const Device &D);

/// The pack-relative include directories cc1 derives (pic30.c:25960-26075, c30_flag_definitions.h
/// :249-335), in cc1's order: "include", "support/generic/h", then for a KNOWN non-generic device
/// "support/<Fam>/h" and, where the family has one, "support/peripheral_<x>". A generic name and
/// an unknown name get the common two only.
void packIncludeDirs(const Device &D, llvm::SmallVectorImpl<std::string> &Out);

} // namespace dspic
} // namespace clang

#endif // LLVM_CLANG_BASIC_DSPICDEVICE_H
