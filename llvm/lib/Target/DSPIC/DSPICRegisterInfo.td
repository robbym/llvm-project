//===-- DSPICRegisterInfo.td - dsPIC33 register file -----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Milestone 1a of the trellis dsPIC spike (session 83). The MSP430 backend's
// internal register NAMES are kept so its C++ compiles unchanged; what changes is
// which dsPIC W register each one prints as and the caller/callee split:
//
//   MSP430 R12..R15 (argument / caller-saved)  ->  w0..w3
//   MSP430 R11      (caller-saved)             ->  w4
//   W5, W6, W7      (new, caller-saved)        ->  w5..w7
//   MSP430 R10..R5  (callee-saved)             ->  w8..w13
//   MSP430 R4       (frame pointer)            ->  w14   (dsPIC's frame pointer)
//   MSP430 SP                                  ->  w15   (dsPIC's stack pointer)
//   PC, SR, CG stay reserved and are never printed.
//
// Byte registers alias the same W register: a dsPIC byte op names w0 and carries
// a .b suffix, exactly as MSP430's r12 does, so the GR8/GR16 subregister
// structure is kept as is.
//===----------------------------------------------------------------------===//

class DSPICReg<bits<4> num, string n, list<string> alt = []> : Register<n> {
  field bits<4> Num = num;
  let Namespace = "DSPIC";
  let HWEncoding{3-0} = num;
  let AltNames = alt;
  let DwarfNumbers = [num];
}

class DSPICRegWithSubregs<bits<4> num, string n, list<Register> subregs,
                           list<string> alt = []>
  : RegisterWithSubRegs<n, subregs> {
  field bits<4> Num = num;
  let Namespace = "DSPIC";
  let HWEncoding{3-0} = num;
  let AltNames = alt;
  let DwarfNumbers = [num];
}

//===----------------------------------------------------------------------===//
//  Registers
//===----------------------------------------------------------------------===//

def PCB  : DSPICReg<0,  "pc">;
def SPB  : DSPICReg<15, "w15", ["sp"]>;
def SRB  : DSPICReg<2,  "SR">;
def CGB  : DSPICReg<3,  "cg">;
def R4B  : DSPICReg<14, "w14", ["fp"]>;
def R5B  : DSPICReg<13, "w13">;
def R6B  : DSPICReg<12, "w12">;
def R7B  : DSPICReg<11, "w11">;
def R8B  : DSPICReg<10, "w10">;
def R9B  : DSPICReg<9,  "w9">;
def R10B : DSPICReg<8,  "w8">;
def R11B : DSPICReg<4,  "w4">;
def R12B : DSPICReg<0,  "w0">;
def R13B : DSPICReg<1,  "w1">;
def R14B : DSPICReg<2,  "w2">;
def R15B : DSPICReg<3,  "w3">;
def W5B  : DSPICReg<5,  "w5">;
def W6B  : DSPICReg<6,  "w6">;
def W7B  : DSPICReg<7,  "w7">;

def subreg_8bit : SubRegIndex<8> { let Namespace = "DSPIC"; }

let SubRegIndices = [subreg_8bit] in {
def PC  : DSPICRegWithSubregs<0,  "pc",  [PCB]>;
def SP  : DSPICRegWithSubregs<15, "w15", [SPB], ["sp"]>;
def SR  : DSPICRegWithSubregs<2,  "SR",  [SRB]>;
def CG  : DSPICRegWithSubregs<3,  "cg",  [CGB]>;
def R4  : DSPICRegWithSubregs<14, "w14", [R4B], ["fp"]>;
def R5  : DSPICRegWithSubregs<13, "w13", [R5B]>;
def R6  : DSPICRegWithSubregs<12, "w12", [R6B]>;
def R7  : DSPICRegWithSubregs<11, "w11", [R7B]>;
def R8  : DSPICRegWithSubregs<10, "w10", [R8B]>;
def R9  : DSPICRegWithSubregs<9,  "w9",  [R9B]>;
def R10 : DSPICRegWithSubregs<8,  "w8",  [R10B]>;
def R11 : DSPICRegWithSubregs<4,  "w4",  [R11B]>;
def R12 : DSPICRegWithSubregs<0,  "w0",  [R12B]>;
def R13 : DSPICRegWithSubregs<1,  "w1",  [R13B]>;
def R14 : DSPICRegWithSubregs<2,  "w2",  [R14B]>;
def R15 : DSPICRegWithSubregs<3,  "w3",  [R15B]>;
def W5  : DSPICRegWithSubregs<5,  "w5",  [W5B]>;
def W6  : DSPICRegWithSubregs<6,  "w6",  [W6B]>;
def W7  : DSPICRegWithSubregs<7,  "w7",  [W7B]>;
}

def GR8 : RegisterClass<"DSPIC", [i8], 8,
   // Caller-saved: w0..w7
  (add R12B, R13B, R14B, R15B, R11B, W5B, W6B, W7B,
   // Callee-saved: w8..w13
   R10B, R9B, R8B, R7B, R6B, R5B,
   // Frame pointer w14, sometimes allocable
   R4B,
   // Reserved
   PCB, SPB, SRB, CGB)>;

def GR16 : RegisterClass<"DSPIC", [i16], 16,
  (add R12, R13, R14, R15, R11, W5, W6, W7,
   R10, R9, R8, R7, R6, R5,
   R4,
   PC, SP, SR, CG)>;

// The register an indirect tail call's target may live in across the epilogue: the
// caller-saved w0-w7 only (trellis L1c, session 85).
def GR16_TC : RegisterClass<"DSPIC", [i16], 16,
  (add R12, R13, R14, R15, R11, W5, W6, W7)>;

// The even W registers (w0 w2 w4 w6 w8 w10 w12), the destination of a hardware multiply
// (`mulw.ss Wb,Ws,Wd` with Wd even -- W_MUL_DST_REG; probe: w1/w9 refused, w0/w2/w8 OK;
// trellis L1f-d, session 86). w14 (fp) and w15 (sp) are reserved and excluded.
def GR16_EVEN : RegisterClass<"DSPIC", [i16], 16, (add R12, R14, R11, W6, R10, R8, R6)>;

// W0 alone, as a byte: the only register a byte DIRECT move reaches (`mov.b f,WREG` /
// `mov.b WREG,f`; probe7: `mov.b _x,w1` refused). Trellis L1f-g/h, session 87.
def GR8_W0 : RegisterClass<"DSPIC", [i8], 8, (add R12B)>;

// W0 alone, as a word: the implied WREG of the file-register forms (`add.w _g` is
// `_g = _g + W0`; cc1 moves the operand to w0 first -- `_addw2` does `mov.w w1,w0 ; add _g`).
// Also the destination of the WREG-result forms (`inc.w _g,WREG`). Trellis L1f-h, session 88.
def GR16_W0 : RegisterClass<"DSPIC", [i16], 16, (add R12)>;
