//===-- DSPICInstPrinter.cpp - Convert DSPIC MCInst to assembly syntax ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Prints the pic30 assembler's operand syntax: `#lit` immediates, `[wN+disp]`
// register-offset memory, bare `sym` for direct (file-register) addressing,
// `[wN]` / `[wN++]` indirect, and dsPIC's branch condition mnemonics.
//===----------------------------------------------------------------------===//

#include "DSPICInstPrinter.h"
#include "DSPIC.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Support/ErrorHandling.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated portion of the assembly writer.
#define PRINT_ALIAS_INSTR
#include "DSPICGenAsmWriter.inc"

void DSPICInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void DSPICInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                 StringRef Annot, const MCSubtargetInfo &STI,
                                 raw_ostream &O) {
  if (!printAliasInstr(MI, Address, O))
    printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

// A branch or call target: a bare symbol or label, never `#`-prefixed.
void DSPICInstPrinter::printPCRelImmOperand(const MCInst *MI, unsigned OpNo,
                                            raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isImm()) {
    int64_t Imm = Op.getImm() * 2 + 2;
    O << ".";
    if (Imm >= 0)
      O << '+';
    O << Imm;
  } else {
    assert(Op.isExpr() && "unknown pcrel immediate operand");
    MAI.printExpr(O, *Op.getExpr());
  }
}

void DSPICInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                    raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    O << getRegisterName(Op.getReg());
  } else if (Op.isImm()) {
    O << '#' << Op.getImm();
  } else {
    assert(Op.isExpr() && "unknown operand kind in printOperand");
    O << '#';
    MAI.printExpr(O, *Op.getExpr());
  }
}

// memsrc/memdst: (base register, displacement). Base == SR encodes an absolute
// address (the MSP430 backend's `&sym` form), which on dsPIC is direct
// file-register addressing and prints as the bare symbol or number.
void DSPICInstPrinter::printSrcMemOperand(const MCInst *MI, unsigned OpNo,
                                          raw_ostream &O) {
  const MCOperand &Base = MI->getOperand(OpNo);
  const MCOperand &Disp = MI->getOperand(OpNo+1);

  if (Base.getReg() == DSPIC::SR || Base.getReg() == DSPIC::PC) {
    if (Disp.isExpr())
      MAI.printExpr(O, *Disp.getExpr());
    else
      O << Disp.getImm();
    return;
  }

  O << '[' << getRegisterName(Base.getReg());
  if (Disp.isExpr()) {
    O << '+';
    MAI.printExpr(O, *Disp.getExpr());
  } else {
    assert(Disp.isImm() && "Expected immediate in displacement field");
    int64_t D = Disp.getImm();
    if (D > 0)
      O << '+' << D;
    else if (D < 0)
      O << '-' << -D;
  }
  O << ']';
}

void DSPICInstPrinter::printIndRegOperand(const MCInst *MI, unsigned OpNo,
                                          raw_ostream &O) {
  const MCOperand &Base = MI->getOperand(OpNo);
  O << '[' << getRegisterName(Base.getReg()) << ']';
}

void DSPICInstPrinter::printPostIndRegOperand(const MCInst *MI, unsigned OpNo,
                                              raw_ostream &O) {
  const MCOperand &Base = MI->getOperand(OpNo);
  O << '[' << getRegisterName(Base.getReg()) << "++]";
}

// MSP430's condition codes, in dsPIC's spelling. Both compare by subtracting
// src from dst, so the mapping is direct: HS (carry set) is unsigned >=, LO is
// unsigned <.
void DSPICInstPrinter::printCCOperand(const MCInst *MI, unsigned OpNo,
                                      raw_ostream &O) {
  unsigned CC = MI->getOperand(OpNo).getImm();
  switch (CC) {
  default:
   llvm_unreachable("Unsupported CC code");
  case DSPICCC::COND_E:
   O << "z";
   break;
  case DSPICCC::COND_NE:
   O << "nz";
   break;
  case DSPICCC::COND_HS:
   O << "geu";
   break;
  case DSPICCC::COND_LO:
   O << "ltu";
   break;
  case DSPICCC::COND_GE:
   O << "ge";
   break;
  case DSPICCC::COND_L:
   O << "lt";
   break;
  case DSPICCC::COND_N:
   O << "n";
   break;
  }
}
