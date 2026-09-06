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
    return (AS != LangAS::Default && toTargetAddressSpace(AS) == 1) ? 32 : PointerWidth;
  }
  uint64_t getPointerAlignV(LangAS AS) const override {
    return (AS != LangAS::Default && toTargetAddressSpace(AS) == 1) ? 16 : PointerAlign;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }

  bool allowsLargerPreferedTypeAlignment() const override { return false; }

  bool hasFeature(StringRef Feature) const override {
    return Feature == "dspic";
  }

  ArrayRef<const char *> getGCCRegNames() const override;

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    static const TargetInfo::GCCRegAlias GCCRegAliases[] = {
        {{"sp"}, "w15"},
        {{"fp"}, "w14"},
    };
    return llvm::ArrayRef(GCCRegAliases);
  }

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
