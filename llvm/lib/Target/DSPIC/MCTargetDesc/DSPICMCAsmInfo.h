//===-- DSPICMCAsmInfo.h - DSPIC asm properties --------------*- C++ -*--===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the declaration of the DSPICMCAsmInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_MCTARGETDESC_DSPICMCASMINFO_H
#define LLVM_LIB_TARGET_DSPIC_MCTARGETDESC_DSPICMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {
class Triple;
class MCSpecifierExpr;

namespace DSPIC {
// The pic30 assembler's `handle(sym)`: the 16-bit handle of a function's program
// address, required wherever a code symbol is used as data (trellis L1c; the assembler
// refuses `mov #_f,w0`: "Cannot reference executable symbol (_f) in a data context").
enum { S_HANDLE = 1, S_TBLOFFSET = 2, S_TBLPAGE = 3 };
} // namespace DSPIC

namespace DSPICII {
// Machine-operand target flags: the two halves of a 24-bit program address (L1e prog-space).
enum TOF { MO_NONE = 0, MO_TBLOFFSET, MO_TBLPAGE };
} // namespace DSPICII

class DSPICMCAsmInfo : public MCAsmInfoELF {
  void anchor() override;

public:
  explicit DSPICMCAsmInfo(const Triple &TT, const MCTargetOptions &Options);
  void printSpecifierExpr(raw_ostream &OS,
                          const MCSpecifierExpr &Expr) const override;
};

} // namespace llvm

#endif
