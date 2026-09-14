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
#include "clang/Basic/Builtins.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetBuiltins.h"

using namespace clang;
using namespace clang::targets;

// trellis session 103: the table read/write builtins, the AVR `.def` shape.
static constexpr int NumBuiltins =
    clang::DSPIC::LastTSBuiltin - Builtin::FirstTSBuiltin;

static constexpr llvm::StringTable BuiltinStrings =
    CLANG_BUILTIN_STR_TABLE_START
#define BUILTIN CLANG_BUILTIN_STR_TABLE
#include "clang/Basic/BuiltinsDSPIC.def"
    ;

static constexpr auto BuiltinInfos = Builtin::MakeInfos<NumBuiltins>({
#define BUILTIN CLANG_BUILTIN_ENTRY
#include "clang/Basic/BuiltinsDSPIC.def"
});

llvm::SmallVector<Builtin::InfosShard>
DSPICTargetInfo::getTargetBuiltins() const {
  return {{&BuiltinStrings, BuiltinInfos}};
}

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
  // session 110: `__eds__` is MODELLED, not silenced. It was defined EMPTY here, so
  // `__eds__ int gv; return gv;` compiled to a direct near access ignoring the address space,
  // silently, where cc1 emits the DSRPAG-windowed read. The operator: "any program the user
  // writes targetting the xc-dsc compiler needs to work here".
  Builder.defineMacro("__eds__", "__attribute__((address_space(2)))");
  // session 110: `__external__` is MODELLED. It was defined EMPTY, so a direct read compiled to
  // an ordinary near access -- silently the wrong memory. Address space 3; every load and store
  // in it is refused in cc1's own words (SemaExpr.cpp), which is what cc1 does too.
  Builder.defineMacro("__external__", "__attribute__((address_space(3)))");
  // session 111: `__pack_upper_byte` is MODELLED. It was defined EMPTY, so a packed-flash object
  // was an ordinary near data object read from RAM -- silently the wrong memory. Address space 4:
  // a 32-bit linear pointer, placement in `,packedflash`, every read through the vendor's own
  // ___P32DFrd, the record laid out packed (SemaType.cpp), writes refused in cc1's sentence.
  Builder.defineMacro("__pack_upper_byte", "__attribute__((address_space(4)))");

  // ── trellis session 118: THE VENDOR'S FAMILY-WIDE PREDEFINED MACROS ──────────────────────
  // Every name and value below is read off `xc-dsc-gcc -dM -E` at FOUR devices
  // (steps/frontend/macro-ask.sh; banked at prints/l1f/frontend/macro/), and every one of them is
  // identical at all four -- 33CK1024MP705, 33CK256MP508, 33EP256MU806 and 30F6014A. The macros
  // that MOVE with the device (__dsPIC<part>__, __IVT_NUM, __HAS_DMA__, __HAS_DMAV2__,
  // __HAS_EDS__, __HAS_PMP__) are deliberately NOT here: they need the device on the command line
  // and the pack's database, and this target takes no -mcpu.
  //
  // ⛔ WHY THE COMPILER AND NOT A BUILD SCRIPT. These were compensated by eleven hand-written -D
  // flags in each of two build scripts. A missing macro is the silent kind of divergence -- the
  // `#if` takes the other arm and the two compilers build different programs from one source --
  // and 54 firmware files test __XC16__, 52 test __dsPIC33C__, 166 pack headers test XC16.
  Builder.defineMacro("__C30__");
  Builder.defineMacro("__C30");
  Builder.defineMacro("C30");
  Builder.defineMacro("__C30ELF__");
  Builder.defineMacro("__C30ELF");
  Builder.defineMacro("__XC__");
  Builder.defineMacro("__XC16__");
  Builder.defineMacro("__XC16");
  Builder.defineMacro("XC16");
  Builder.defineMacro("__XC16ELF__");
  Builder.defineMacro("__XC16ELF");
  Builder.defineMacro("__XC_DSC__");
  Builder.defineMacro("__XC_DSC");
  Builder.defineMacro("XC_DSC");
  Builder.defineMacro("__XC_DSCELF__");
  Builder.defineMacro("__XC_DSCELF");
  Builder.defineMacro("__XC_DSC_MUSL__");
  Builder.defineMacro("__dsPIC30ELF__");
  Builder.defineMacro("__dsPIC30ELF");
  Builder.defineMacro("__dsPIC30");
  Builder.defineMacro("dsPIC30");
  Builder.defineMacro("__BUILTIN_ITTYPE");
  Builder.defineMacro("__HAS_BUILTINS_16__");
  Builder.defineMacro("__HAS_CODEGUARD__");
  Builder.defineMacro("__HAS_DSP__");
  Builder.defineMacro("__LONG_LONG_WIDTH__", "64");
  // ⚠ THE VERSION MACROS ARE ZERO BECAUSE THE VENDOR'S OWN SHIPPED COMPILER REPORTS ZERO. Asked
  // of xc-dsc-gcc v4.00 directly: __C30_VERSION__ 0, __XC16_VERSION__ 0, __XC_DSC_VERSION__ 0.
  // The record already establishes why (the install's __VERSION__ carries the unsubstituted token
  // MCHP_VERSION -- its version macros are broken, not authoritative), and steps/stn3255/build.sh
  // deliberately passes -D__C30_VERSION__=4000 over it, measured from the vendor's own ELF. A
  // command-line -D wins over a target define, so that override still stands.
  Builder.defineMacro("__C30_VERSION__", "0");
  Builder.defineMacro("__XC16_VERSION__", "0");
  Builder.defineMacro("__XC16_VERSION", "0");
  Builder.defineMacro("__XC_DSC_VERSION__", "0");
  // __XC16_BUILD_DATE__ / __XC_DSC_BUILD_DATE__ are COSTED, not implemented: a vendor build date
  // ("Aug 31 2026" on this install), with zero customers measured in the vendor libc headers, the
  // device pack's support headers or either firmware tree. Inventing one is a claim about a build
  // that did not happen.
  //
  // __LARGE_ARRAYS__ is EXACT, from the target feature this port already carries. Measured through
  // cc1: 0 by default, 1 under -menable-large-arrays, and it does NOT move with -mlarge-code or
  // -mlarge-data.
  Builder.defineMacro("__LARGE_ARRAYS__", LargeArrays ? "1" : "0");
  // __OPTIMIZATION_LEVEL__ is NOT here: getTargetDefines is handed LangOptions, which carries no
  // -O number. It is defined exactly, from CGOpts.OptimizationLevel, in InitPreprocessor.cpp --
  // three lines from where clang already defines __OPTIMIZE__ from the same field.
}
