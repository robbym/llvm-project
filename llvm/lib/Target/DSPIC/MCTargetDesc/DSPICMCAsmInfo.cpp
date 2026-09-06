//===-- DSPICMCAsmInfo.cpp - dsPIC asm properties -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// What the pic30 assembler (binutils 2.32, Microchip's port) accepts, measured at
// trellis session 83: `.text`/`.data`/`.bss` bare, `.align N` in bytes, `.globl`,
// `.type`, `.size`; `;` comments. It refuses `.p2align`. No CFI, no DWARF here.
//===----------------------------------------------------------------------===//

#include "DSPICMCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

void DSPICMCAsmInfo::anchor() { }

void DSPICMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                        const MCSpecifierExpr &Expr) const {
  const char *Fn = nullptr;
  switch (Expr.getSpecifier()) {
  case DSPIC::S_HANDLE:    Fn = "handle";    break;
  case DSPIC::S_TBLOFFSET: Fn = "tbloffset"; break;
  case DSPIC::S_TBLPAGE:   Fn = "tblpage";   break;
  default: llvm_unreachable("unknown DSPIC specifier");
  }
  OS << Fn << '(';
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}

DSPICMCAsmInfo::DSPICMCAsmInfo(const Triple &TT,
                               const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  CodePointerSize = 2;
  CalleeSaveStackSlotSize = 2;
  CommentString = ";";
  SeparatorString = "{";
  AlignmentIsInBytes = true;
  UseByteAlignDirective = true;
  UsesELFSectionDirectiveForBSS = false;
  SupportsDebugInformation = false;
  ExceptionsType = ExceptionHandling::None;
  // L1f-b: inline asm is emitted verbatim, never parsed by the inherited AsmParser.
  UseIntegratedAssembler = false;
  // L1f-h (trellis session 88): the pic30 `as` REFUSES `.zero N` (probe10) -- its spelling
  // for a zero-fill is `.space N` (a `const` table's padding and a zero-init aggregate both
  // emit it). `.bss`/`.data`/`.rodata` all take `.space`.
  ZeroDirective = "\t.space\t";
}
