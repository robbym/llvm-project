//===--- DSPIC.cpp - Implement dsPIC33 target feature support -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis L1f-b (session 86). The predefined macros are the FAMILY ones cc1 sets
// (`__dsPIC30__`, `__dsPIC33C__`) plus this target's own (`__DSPIC__`, `__dsPIC33__`);
// `__XC16__` / `__C30__` name a compiler this is not, and are NOT defined.
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "clang/Basic/MacroBuilder.h"

using namespace clang;
using namespace clang::targets;

const char *const DSPICTargetInfo::GCCRegNames[] = {
    "w0", "w1", "w2",  "w3",  "w4",  "w5",  "w6",  "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15"};

ArrayRef<const char *> DSPICTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(GCCRegNames);
}

void DSPICTargetInfo::getTargetDefines(const LangOptions &Opts,
                                       MacroBuilder &Builder) const {
  Builder.defineMacro("__DSPIC__");
  Builder.defineMacro("__dsPIC33__");
  Builder.defineMacro("__dsPIC33C__");
  Builder.defineMacro("__dsPIC30__");

  // Microchip C SPACE-QUALIFIER keywords (trellis session 88, "run it through"): cc1 knows
  // `__prog__` (program/flash space) and `__eds__` (extended data space) as bare TYPE QUALIFIERS;
  // clang does not and errors ("unknown type name") in qualifier position. In the vendor device
  // headers and firmware these are ALWAYS paired with `__attribute__((space(...)))`, which carries
  // the real placement (clang tolerates the unknown attribute), so recognizing the keyword unblocks
  // compilation. `__pack_upper_byte` is the packed program-pointer qualifier from libpic30.h.
  // (`__sfr__`/`__deprecated__`/`__unsafe__` need nothing -- they appear only inside
  // `__attribute__((...))`, which clang already tolerates.)
  // NOTE: RECOGNIZED FOR COMPILATION ONLY. Program-space PLACEMENT beyond an explicit `section()`
  // and program-space ACCESS (tblrd/PSV, the packed-pointer representation) are the data-model
  // work (backend stage L1e); they are NOT modelled by this recognition.
  Builder.defineMacro("__prog__", "__attribute__((address_space(1)))");
  Builder.defineMacro("__eds__", "");
  Builder.defineMacro("__external__", "");
  Builder.defineMacro("__pack_upper_byte", "");
}
