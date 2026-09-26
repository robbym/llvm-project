//===--- DSPIC.h - Declare dsPIC33 target feature support -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The clang target for the experimental dsPIC33 backend (trellis L1f-b, session 86):
// the type widths the pic30 cc1 uses (`xgcc -dM -E -mcpu=33CK256MP508`, banked in
// tools/dspic-llvm/prints/l1f/clang/cc1-macros.txt): int 16, long 32, long long 64,
// pointer 16, float 32, DOUBLE 32 (cc1's -fshort-double default; long double 64),
// every alignment 16, char signed. The data layout is the backend's, taken from
// TargetDataLayout.cpp by the triple. Register names w0-w15 (sp = w15, fp = w14).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_DSPIC_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_DSPIC_H

#include "clang/Basic/AddressSpaces.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

class LLVM_LIBRARY_VISIBILITY DSPICTargetInfo : public TargetInfo {
public:
  // ⛔ trellis session 112: ask the type printer for the vendor's qualifier spellings. This is the
  // whole of the target-keying -- the flag defaults OFF and only this target turns it on, so the
  // shared arm in Qualifiers::print is unreachable from any other triple.
  void adjust(DiagnosticsEngine &Diags, LangOptions &Opts,
              const TargetInfo *Aux) override {
    TargetInfo::adjust(Diags, Opts, Aux);
    Opts.DSPICAddressSpaceNames = true;
    // ⛔ trellis session 136: cc1plus's `double` IS 64-BIT WHERE cc1's IS 32-BIT, and ours was
    // 32-bit in both. Measured: __DBL_MANT_DIG__ / __SIZEOF_DOUBLE__ are 24 / 4 from the vendor's
    // C and 53 / 8 from its C++, with `long double` 53 / 8 on every side. One entry of a
    // sixteen-entry four-way table, and an ABI rather than a type: a C++ program built against the
    // vendor's headers and linked against its libstdc++.a passed 32-bit doubles to functions
    // expecting 64-bit ones -- no diagnostic, no crash, wrong numbers. It is also the entire cause
    // of six of the seven standard-header refusals, because <xc-dsc>/include/math.h:146 declares
    // its whole double block only under `__DBL_MANT_DIG__ != __FLT_MANT_DIG__`.
    // ⛔ IT IS A cc1plus BUILT-IN DEFAULT AND NOT A FLAG. The vendor's cc1 and cc1plus command
    // lines are IDENTICAL in flags and neither mentions short-double, so nothing the driver passes
    // can discover it -- which is why this is keyed on the LANGUAGE here rather than on an option.
    // ⚠ SAFE BECAUSE THE BACKEND ALREADY DOES f64, AND THAT WAS MEASURED, NOT ASSUMED: this
    // target's `long double` is already 64-bit, compiles, assembles, and emits ___adddf3 /
    // ___muldf3 / ___ltdf2 -- the same names the vendor emits -- and the data layout carries
    // f64:16 with no language dependence at all, so the front-end type cannot disagree with it.
    // ⚠ DoubleAlign is deliberately left at 16, and that is REQUIRED rather than incumbent:
    // LongDoubleAlign, this target's existing 64-bit float, is 16 too, and mutant M4 -- which sets
    // it to one byte -- breaks assembly, the libcall set and every header cell, not just the
    // alignment one. Measured, after I predicted it would be inert.
    // The row is steps/frontend/dblwidth-compare.sh; PPCTargetInfo::adjust is the in-tree
    // precedent for mutating a float format here.
    if (Opts.CPlusPlus) {
      DoubleWidth = 64;
      DoubleFormat = &llvm::APFloat::IEEEdouble();
    }
  }

  static const char *const GCCRegNames[];

public:
  DSPICTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    TLSSupported = false;
    IntWidth = 16;
    IntAlign = 16;
    LongWidth = 32;
    LongLongWidth = 64;
    LongAlign = LongLongAlign = 16;
    FloatWidth = 32;
    FloatAlign = 16;
    // cc1's default is -fshort-double: double is the 32-bit format; long double is 64.
    DoubleWidth = 32;
    DoubleAlign = 16;
    DoubleFormat = &llvm::APFloat::IEEEsingle();
    LongDoubleWidth = 64;
    LongDoubleAlign = 16;
    LongDoubleFormat = &llvm::APFloat::IEEEdouble();
    PointerWidth = 16;
    PointerAlign = 16;
    SuitableAlign = 16;
    SizeType = UnsignedInt;
    IntMaxType = SignedLongLong;
    IntPtrType = SignedInt;
    PtrDiffType = SignedInt;
    WCharType = SignedInt;
    WIntType = SignedInt;
    SigAtomicType = SignedInt;
    resetDataLayout();
  }
  // Program-memory pointers (`__prog__` = address space 1) are 4 bytes: a 24-bit program
  // address held page:offset (measured: sizeof(__prog__ int*) == 4 under cc1). Data pointers
  // stay 2 bytes.
  uint64_t getPointerWidthV(LangAS AS) const override {
    // session 110: address space 2 is `__eds__`, whose pointer is also 4 bytes (offset:page).
    // Measured from cc1: sizeof(__eds__ int *) == 4, sizeof(int *) == 2 (steps/eds/ASK.md).
    unsigned TAS = toTargetAddressSpace(AS);
    // 3 is `__external__`: measured sizeof(__external__ int *) == 4, the same as eds and prog.
    // 4 is `__pack_upper_byte` (session 111): measured sizeof(__pack_upper_byte char *) == 4.
    return (AS != LangAS::Default && (TAS == 1 || TAS == 2 || TAS == 3 || TAS == 4)) ? 32 : PointerWidth;
  }
  uint64_t getPointerAlignV(LangAS AS) const override {
    return (AS != LangAS::Default && toTargetAddressSpace(AS) == 1) ? 16 : PointerAlign;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  // trellis session 103: the table read/write builtins. Defined in DSPIC.cpp because the
  // string table and the info array are file-scope constants built from BuiltinsDSPIC.def.
  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override;

  bool allowsLargerPreferedTypeAlignment() const override { return false; }

  // trellis session 96 (follow-up 15): the code and data models. The DEFAULTS are cc1's own,
  // measured (steps/models/ask.sh) and confirmed in the vendor's source: small code, scalars near,
  // aggregates far, constants in program memory.
  bool LargeCode = false;
  bool LargeScalar = false;
  bool SmallAggregate = false;
  bool ConstInData = false;
  bool LargeArrays = false;

  bool handleTargetFeatures(std::vector<std::string> &Features,
                            DiagnosticsEngine &Diags) override {
    for (StringRef F : Features) {
      bool On = F[0] == '+';
      StringRef Name = F.drop_front();
      if (Name == "large-code") LargeCode = On;
      else if (Name == "large-scalar") LargeScalar = On;
      else if (Name == "small-aggregate") SmallAggregate = On;
      else if (Name == "const-in-data") ConstInData = On;
      else if (Name == "large-arrays") LargeArrays = On;
    }
    // ⛔ trellis session 140: under -mlarge-arrays / -menable-large-arrays the vendor's size_t is
    // `long unsigned int`, in C and C++, and ours stayed 16 bits (the flag moved only
    // __LARGE_ARRAYS__), so the two compilers built one program with two size_t ABIs; the operator
    // ruled "match vendor" (D6). Measured of the vendor (trc steps/frontend/SZT2.banked.txt): the
    // flag moves EXACTLY size_t's macros -- ptrdiff_t, intptr_t and pointers stay 16 bits -- and a
    // size_t travels in a register pair (w2:w3 after a pointer). So only SizeType moves here; the
    // signature's bit 0, TargetLibraryInfo's size_t and the mem* libcall's length all follow from it
    // through the `dspic-size-t-width` module flag. Row: steps/frontend/szt-compare.sh.
    SizeType = LargeArrays ? UnsignedLong : UnsignedInt;
    return true;
  }

  bool hasFeature(StringRef Feature) const override {
    return Feature == "dspic" ||
           (Feature == "large-code" && LargeCode) ||
           (Feature == "large-scalar" && LargeScalar) ||
           (Feature == "small-aggregate" && SmallAggregate) ||
           (Feature == "const-in-data" && ConstInData) ||
           (Feature == "large-arrays" && LargeArrays);
  }

  ArrayRef<const char *> getGCCRegNames() const override;

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    static const TargetInfo::GCCRegAlias GCCRegAliases[] = {
        {{"sp"}, "w15"},
        {{"fp"}, "w14"},
    };
    return llvm::ArrayRef(GCCRegAliases);
  }

  // ── trellis session 119: THE DEVICE, THE VENDOR'S WAY ──────────────────────────────────────
  // `-mcpu=<device> -mdfp=<pack>/xc16` is the vendor driver's own spelling; until this session
  // ours refused `-mcpu=` outright. The device is read from ONE file, <dfp>/bin/c30_device.info,
  // the same file cc1 reads (pic30.c:1637 validate_target_id), and everything the device macros
  // need comes out of it -- steps/frontend/device-ask.py reproduces them against cc1's own -dM -E
  // at five devices. The members are `mutable` because validateTarget, the one hook that has a
  // DiagnosticsEngine AND runs after TargetOpts is attached, is const.
  std::string CPU;                              // as written, uppercased (cc1 TOUPPERs it too)
  mutable bool HaveDevice = false;
  mutable std::string DeviceMacro, FamilyMacro; // __dsPIC33CK1024MP705__, __dsPIC33C__
  mutable unsigned DeviceFlags = 0;             // the record's flags word (c30_flag_definitions.h)
  mutable unsigned IVTNum = 0;                  // __IVT_NUM: the vector records for this device
  mutable bool IsGeneric = false;               // session 120: one of the nine GENERIC-* names
  bool setCPU(StringRef Name) override { CPU = Name.upper(); return true; }
  bool validateTarget(DiagnosticsEngine &Diags) const override;

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &info) const override {
    // No target constraints: inline asm is L1f-i's / a later row's (COSTED).
    return false;
  }

  std::string_view getClobbers() const override { return ""; }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::CharPtrBuiltinVaList;
  }
};

} // namespace targets
} // namespace clang
#endif // LLVM_CLANG_LIB_BASIC_TARGETS_DSPIC_H
