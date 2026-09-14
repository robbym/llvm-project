//===-- DSPICInstrInfo.td - DSPIC Instruction defs -------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the DSPIC instructions in TableGen format.
//
//===----------------------------------------------------------------------===//

include "DSPICInstrFormats.td"

//===----------------------------------------------------------------------===//
// Type Constraints.
//===----------------------------------------------------------------------===//
class SDTCisI8<int OpNum> : SDTCisVT<OpNum, i8>;
class SDTCisI16<int OpNum> : SDTCisVT<OpNum, i16>;

//===----------------------------------------------------------------------===//
// Type Profiles.
//===----------------------------------------------------------------------===//
def SDT_DSPICCall         : SDTypeProfile<0, -1, [SDTCisVT<0, iPTR>]>;
def SDT_DSPICCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i16>,
                                             SDTCisVT<1, i16>]>;
def SDT_DSPICCallSeqEnd   : SDCallSeqEnd<[SDTCisVT<0, i16>, SDTCisVT<1, i16>]>;
def SDT_DSPICWrapper      : SDTypeProfile<1, 1, [SDTCisSameAs<0, 1>,
                                                  SDTCisPtrTy<0>]>;
def SDT_DSPICCmp          : SDTypeProfile<0, 2, [SDTCisSameAs<0, 1>]>;
def SDT_DSPICBrCC         : SDTypeProfile<0, 2, [SDTCisVT<0, OtherVT>,
                                                  SDTCisVT<1, i8>]>;
def SDT_DSPICSelectCC     : SDTypeProfile<1, 3, [SDTCisSameAs<0, 1>,
                                                  SDTCisSameAs<1, 2>, 
                                                  SDTCisVT<3, i8>]>;
def SDT_DSPICDAdd         : SDTypeProfile<1, 2, [SDTCisSameAs<0, 1>,
                                                  SDTCisSameAs<0, 2>,
                                                  SDTCisInt<0>]>;

//===----------------------------------------------------------------------===//
// DSPIC Specific Node Definitions.
//===----------------------------------------------------------------------===//
def DSPICretglue  : SDNode<"DSPICISD::RET_GLUE", SDTNone,
                       [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
def DSPICretiglue : SDNode<"DSPICISD::RETI_GLUE", SDTNone,
                       [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;


def DSPICcall    : SDNode<"DSPICISD::CALL", SDT_DSPICCall,
                     [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue, SDNPVariadic]>;
def DSPICcallseq_start :
                 SDNode<"ISD::CALLSEQ_START", SDT_DSPICCallSeqStart,
                        [SDNPHasChain, SDNPOutGlue]>;
def DSPICcallseq_end :
                 SDNode<"ISD::CALLSEQ_END",   SDT_DSPICCallSeqEnd,
                        [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;
def DSPICWrapper : SDNode<"DSPICISD::Wrapper", SDT_DSPICWrapper>;

// L1e prog-space (session 89): the PSV-window read of a program-memory (addrspace 1) value.
// Operands: the offset (low 16) and page (high 16) halves of the 24-bit program address.
def SDT_DSPICPsvLd : SDTypeProfile<1, 2, [SDTCisVT<0, i16>, SDTCisVT<1, i16>, SDTCisVT<2, i16>]>;
def DSPICpsvld : SDNode<"DSPICISD::PSVLD", SDT_DSPICPsvLd,
                        [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
// Session 90: the BYTE read through the window (`mov.b [w],w` in the same sequence).
def SDT_DSPICPsvLd8 : SDTypeProfile<1, 2, [SDTCisVT<0, i8>, SDTCisVT<1, i16>, SDTCisVT<2, i16>]>;
def DSPICpsvld8 : SDNode<"DSPICISD::PSVLD8", SDT_DSPICPsvLd8,
                         [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
// Session 90: the zero-/sign-extended byte reads (i16 result, extend in the read).
def DSPICpsvld8z : SDNode<"DSPICISD::PSVLD8Z", SDT_DSPICPsvLd,
                          [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def DSPICpsvld8s : SDNode<"DSPICISD::PSVLD8S", SDT_DSPICPsvLd,
                          [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;

// === session 110: the EDS window, `__eds__` (addrspace 2) ===
// Measured from cc1 (steps/eds/ASK.md section 3), and NOT the PSV sequence one address space over:
//   read  : mov _DSRPAG,wV | movpag wP,DSRPAG | mov [wO],wD | movpag wV,DSRPAG
//   write : mov _DSWPAG,wV | mov wP,_DSWPAG   | mov wS,[wO] | mov wV,_DSWPAG
// Two different page registers and two different setters. ⚠ And there is NO btsts.c/rlc on the
// offset: `edsoffset()` already yields a window-set offset, which is why the pointer ARITHMETIC
// (Phase B) has to clear bit 15 and restore it.
def SDT_DSPICEdsSt : SDTypeProfile<0, 3, [SDTCisVT<0, i16>, SDTCisVT<1, i16>, SDTCisVT<2, i16>]>;
def DSPICedsld : SDNode<"DSPICISD::EDSLD", SDT_DSPICPsvLd,
                        [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def DSPICedsst : SDNode<"DSPICISD::EDSST", SDT_DSPICEdsSt,
                        [SDNPHasChain, SDNPMayStore, SDNPMemOperand]>;
// Phase B(i): the BYTE width. The extension is IN the load (ze/se [w],w) -- L1e's rule, because
// `mov.b [w],w` does not zero the high byte and def8 -> SUBREG_TO_REG drops a following extend.
def SDT_DSPICEdsSt8 : SDTypeProfile<0, 3, [SDTCisVT<0, i8>, SDTCisVT<1, i16>, SDTCisVT<2, i16>]>;
def DSPICedsld8  : SDNode<"DSPICISD::EDSLD8", SDT_DSPICPsvLd8,
                          [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def DSPICedsld8z : SDNode<"DSPICISD::EDSLD8Z", SDT_DSPICPsvLd,
                          [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def DSPICedsld8s : SDNode<"DSPICISD::EDSLD8S", SDT_DSPICPsvLd,
                          [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def DSPICedsst8  : SDNode<"DSPICISD::EDSST8", SDT_DSPICEdsSt8,
                          [SDNPHasChain, SDNPMayStore, SDNPMemOperand]>;
def DSPICcmp     : SDNode<"DSPICISD::CMP", SDT_DSPICCmp, [SDNPOutGlue]>;
// L1f-k (trellis session 92): the borrow compare `cpb`, the second half of a 32-bit compare --
// it takes the first half's flags as glue and hands the pair's on.
def DSPICcmpb    : SDNode<"DSPICISD::CMPB", SDT_DSPICCmp, [SDNPInGlue, SDNPOutGlue]>;
def DSPICbrcc    : SDNode<"DSPICISD::BR_CC", SDT_DSPICBrCC,
                            [SDNPHasChain, SDNPInGlue]>;
def DSPICselectcc: SDNode<"DSPICISD::SELECT_CC", SDT_DSPICSelectCC,
                            [SDNPInGlue]>;
def DSPICdadd    : SDNode<"DSPICISD::DADD", SDT_DSPICDAdd, []>;
// L1f-e: a boolean from the compare flags via `bsw`; the operand is a {flag,invert} imm,
// the flag itself arrives as glue (SDNPInGlue).
def SDT_DSPICF2B : SDTypeProfile<1, 1, [SDTCisVT<0, i16>, SDTCisVT<1, i16>]>;
def DSPICf2b : SDNode<"DSPICISD::FLAG2BOOL", SDT_DSPICF2B, [SDNPInGlue]>;
// L1f-d increment 4: the hardware divide, two results (quotient, remainder).
def SDT_DSPICDivRem : SDTypeProfile<2, 2, [SDTCisSameAs<0, 1>, SDTCisSameAs<0, 2>,
                                           SDTCisSameAs<0, 3>, SDTCisInt<0>]>;
def DSPICudivrem : SDNode<"DSPICISD::UDIVREM", SDT_DSPICDivRem, []>;
def DSPICsdivrem : SDNode<"DSPICISD::SDIVREM", SDT_DSPICDivRem, []>;

//===----------------------------------------------------------------------===//
// DSPIC Operand Definitions.
//===----------------------------------------------------------------------===//

def MemAsmOperand : AsmOperandClass {
  let Name = "Mem";
}

// Address operands
def memsrc : Operand<i16> {
  let PrintMethod = "printSrcMemOperand";
  let MIOperandInfo = (ops GR16, i16imm);
  let ParserMatchClass = MemAsmOperand;
  let EncoderMethod = "getMemOpValue";
  let DecoderMethod = "DecodeMemOperand";
}

def memdst : Operand<i16> {
  let PrintMethod = "printSrcMemOperand";
  let MIOperandInfo = (ops GR16, i16imm);
  let ParserMatchClass = MemAsmOperand;
  let EncoderMethod = "getMemOpValue";
  let DecoderMethod = "DecodeMemOperand";
}

def IndRegAsmOperand : AsmOperandClass {
  let Name = "IndReg";
  let RenderMethod = "addRegOperands";
}

def indreg : Operand<i16> {
  let PrintMethod = "printIndRegOperand";
  let MIOperandInfo = (ops GR16);
  let ParserMatchClass = IndRegAsmOperand;
  let DecoderMethod = "DecodeGR16RegisterClass";
}

def PostIndRegAsmOperand : AsmOperandClass {
  let Name = "PostIndReg";
  let RenderMethod = "addRegOperands";
}

def postreg : Operand<i16> {
  let PrintMethod = "printPostIndRegOperand";
  let MIOperandInfo = (ops GR16);
  let ParserMatchClass = PostIndRegAsmOperand;
  let DecoderMethod = "DecodeGR16RegisterClass";
}

// Short jump targets have OtherVT type and are printed as pcrel imm values.
def jmptarget : Operand<OtherVT> {
  let PrintMethod = "printPCRelImmOperand";
  let EncoderMethod = "getPCRelImmOpValue";
}

// A call or long-branch target: a bare symbol.
def calltarget : Operand<i16> {
  let PrintMethod = "printPCRelImmOperand";
}
// Operand for printing out a condition code.
def cc : Operand<i8> {
  let PrintMethod = "printCCOperand";
  let EncoderMethod = "getCCOpValue";
}

def CGImmAsmOperand : AsmOperandClass {
  let Name = "CGImm";
  let RenderMethod = "addImmOperands";
}

def lit5imm8  : ImmLeaf<i8,  [{return Imm >= 0 && Imm < 32;}]>;
def lit5imm16 : ImmLeaf<i16, [{return Imm >= 0 && Imm < 32;}]>;
def lit8imm8  : ImmLeaf<i8,  [{return Imm >= 0 && Imm < 256;}]>;
def lit8imm16 : ImmLeaf<i16, [{return Imm >= 0 && Imm < 256;}]>;
def cg8imm : Operand<i8>,
             ImmLeaf<i8, [{return Imm >= 0 && Imm < 32;}]> {
  let ParserMatchClass = CGImmAsmOperand;
  let EncoderMethod = "getCGImmOpValue";
  let DecoderMethod = "DecodeCGImm";
}

def cg16imm : Operand<i16>,
              ImmLeaf<i16, [{return Imm >= 0 && Imm < 32;}]> {
  let ParserMatchClass = CGImmAsmOperand;
  let EncoderMethod = "getCGImmOpValue";
  let DecoderMethod = "DecodeCGImm";
}

//===----------------------------------------------------------------------===//
// DSPIC Complex Pattern Definitions.
//===----------------------------------------------------------------------===//

// trellis session 99: WantsParent so SelectAddr can see HOW WIDE the access is. A word
// access encodes its displacement in WORDS and cannot express an odd byte offset at all;
// without the parent this one pattern served the byte and word forms alike and folded a -1
// into `mov.w [w0-1],w1`, which the pic30 assembler refuses (torture pr36339, 20030209-1).
def addr : ComplexPattern<iPTR, 2, "SelectAddr", [], []> { let WantsParent = true; }
// L1f-g: a file-register (near data) address -- a symbol with an optional offset and NO
// register; the bit forms and the file-form bfext take it. Prints as the bare symbol.
def faddr : ComplexPattern<iPTR, 2, "SelectFileAddr", [], []>;
// session 95: the same, but the address must have a single use -- see SelectFileAddr1
def faddr1 : ComplexPattern<iPTR, 2, "SelectFileAddr1", [], []>;

//===----------------------------------------------------------------------===//
// Pattern Fragments
def zextloadi16i8 : PatFrag<(ops node:$ptr), (i16 (zextloadi8 node:$ptr))>;
def  extloadi16i8 : PatFrag<(ops node:$ptr), (i16 ( extloadi8 node:$ptr))>;
def bic : PatFrag<(ops node:$lhs, node:$rhs), (and node:$lhs, (not node:$rhs))>;
def and_su : PatFrag<(ops node:$lhs, node:$rhs), (and node:$lhs, node:$rhs), [{
  return N->hasOneUse();
}]>;
// L1f-k: an `or` whose only use is the compare against zero -- `ior Wb,Ws,[w15]`
def or_su : PatFrag<(ops node:$lhs, node:$rhs), (or node:$lhs, node:$rhs), [{
  return N->hasOneUse();
}]>;
//===----------------------------------------------------------------------===//
// Instruction list..

// ADJCALLSTACKDOWN/UP implicitly use/def SP because they may be expanded into
// a stack adjustment and the codegen must know that they may modify the stack
// pointer before prolog-epilog rewriting occurs.
// Pessimistically assume ADJCALLSTACKDOWN / ADJCALLSTACKUP will become
// sub / add which can clobber SR.
let isCodeGenOnly = 1, Defs = [SP, SR], Uses = [SP] in {
def ADJCALLSTACKDOWN : Pseudo<(outs), (ins i16imm:$amt1, i16imm:$amt2),
                              "#ADJCALLSTACKDOWN $amt1 $amt2",
                              [(DSPICcallseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP   : Pseudo<(outs), (ins i16imm:$amt1, i16imm:$amt2),
                              "#ADJCALLSTACKUP $amt1 $amt2",
                              [(DSPICcallseq_end timm:$amt1, timm:$amt2)]>;
}

let isCodeGenOnly = 1, Defs = [SR], Uses = [SP] in {
def ADDframe : Pseudo<(outs GR16:$dst), (ins i16imm:$base, i16imm:$offset),
                      "# ADDframe PSEUDO", []>;
}

//===----------------------------------------------------------------------===//
// The dsPIC frame (trellis L1b, session 84). Every spelling below was accepted by
// the GPL pic30 `as` before it was written (tools/dspic-llvm/prints/l1b/). The
// encodings are the inherited MSP430 dummies where a class fits, and absent where
// none does: the code emitter generator refuses an encoding field with no operand of
// its name, and nothing emits objects yet (plan L2).
//===----------------------------------------------------------------------===//
class DSPICFrameInst<dag outs, dag ins, int size, string asmstr>
  : DSPICInst<outs, ins, size, asmstr> {
  let Pattern = [];
}

let isCodeGenOnly = 1, hasSideEffects = 0 in {
  // lnk #N: push w14, w14 = w15, w15 += N.   ulnk: w15 = w14, pop w14.
  let Defs = [SP, R4], Uses = [SP, R4], mayStore = 1 in
  def LNK  : II16i<0b100, (outs), (ins i16imm:$imm), "lnk\t$imm", []>;
  let Defs = [SP, R4], Uses = [R4], mayLoad = 1 in
  def ULNK : DSPICFrameInst<(outs), (ins), 2, "ulnk">;

  // Callee-saved pairs: the odd register of the pair rides as an implicit operand.
  let Defs = [SP], Uses = [SP], mayStore = 1 in
  def PUSHD : II16r<0b100, (outs), (ins GR16:$rs), "push.d\t$rs", []>;
  let Defs = [SP], Uses = [SP], mayLoad = 1 in
  def POPD  : IForm16<0b0100, DstReg, SrcPostInc, 2,
                      (outs GR16:$rd), (ins), "pop.d\t$rd", []> {
    let DecoderNamespace = "Delta";
    let rs = 1;
  }

  // The address of a frame slot (ADDframe's expansions): three-operand, untied.
  let Defs = [SR] in {
  def ADD16rri_lea : I16ri<0b0101, (outs GR16:$rd), (ins GR16:$rb, i16imm:$imm),
                           "add.w\t$rb,$imm,$rd", []>;
  def SUB16rri_lea : I16ri<0b1000, (outs GR16:$rd), (ins GR16:$rb, i16imm:$imm),
                           "sub.w\t$rb,$imm,$rd", []>;
  def ADD16rrr     : I16rr<0b0101, (outs GR16:$rd), (ins GR16:$rb, GR16:$rs),
                           "add.w\t$rb,$rs,$rd", []>;
  // Stack-pointer adjustment by a 10-bit literal (call frames beside var-sized objects).
  let Constraints = "$src2 = $rd" in {
  def ADD16ri10 : I16ri<0b0101, (outs GR16:$rd), (ins GR16:$src2, i16imm:$imm),
                        "add.w\t$imm,$rd", []>;
  def SUB16ri10 : I16ri<0b1000, (outs GR16:$rd), (ins GR16:$src2, i16imm:$imm),
                        "sub.w\t$imm,$rd", []>;
  }
  }

  // Register-indexed moves, for a frame slot past the 10-bit displacement:
  // mov #off,wT ; mov.w [w14+wT],wD.
  let mayLoad = 1 in {
  def MOV16rx    : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$base, GR16:$idx), 2,
                                  "mov.w\t[$base+$idx],$rd">;
  def MOV8rx     : DSPICFrameInst<(outs GR8:$rd), (ins GR16:$base, GR16:$idx), 2,
                                  "mov.b\t[$base+$idx],$rd">;
  def MOVZX16rx8 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$base, GR16:$idx), 4,
                                  "mov.b\t[$base+$idx],$rd\n\tze\t$rd,$rd">;
  }
  let mayStore = 1 in {
  def MOV16xr : DSPICFrameInst<(outs), (ins GR16:$base, GR16:$idx, GR16:$rs), 2,
                               "mov.w\t$rs,[$base+$idx]">;
  def MOV8xr  : DSPICFrameInst<(outs), (ins GR16:$base, GR16:$idx, GR8:$rs), 2,
                               "mov.b\t$rs,[$base+$idx]">;
  }
}

let isCodeGenOnly = 1, usesCustomInserter = 1 in {
  let Uses = [SR] in {
  def Select8  : Pseudo<(outs GR8:$dst), (ins GR8:$src, GR8:$src2, i8imm:$cc),
                        "# Select8 PSEUDO",
                        [(set GR8:$dst,
                          (DSPICselectcc GR8:$src, GR8:$src2, imm:$cc))]>;
  def Select16 : Pseudo<(outs GR16:$dst), (ins GR16:$src, GR16:$src2, i8imm:$cc),
                        "# Select16 PSEUDO",
                        [(set GR16:$dst,
                          (DSPICselectcc GR16:$src, GR16:$src2, imm:$cc))]>;
  }
}

//===----------------------------------------------------------------------===//
//  Control Flow Instructions...
//

let isReturn = 1, isTerminator = 1, isBarrier = 1 in {
  def RET  : IForm16<0b0100, DstReg, SrcPostInc, 2,
                     (outs), (ins), "return",  [(DSPICretglue)]> {
    let DecoderNamespace = "Delta";
    let rs = 1;
    let rd = 0;
  }
  def RETI : IIForm16<0b110, SrcReg, 2,
                      (outs), (ins), "retfie", [(DSPICretiglue)]> {
    let rs = 0;
  }
}

let isBranch = 1, isTerminator = 1 in {

// FIXME: expand opcode & cond field for branches!

// Direct branch
let isBarrier = 1 in {
  // Short branch
  def JMP : CJForm<(outs), (ins jmptarget:$dst),
                   "bra\t$dst",
                   [(br bb:$dst)]> {
    let cond = 0b111;
  }
  let isIndirectBranch = 1, rd = 0 in {
    // Long branches
    // trellis session 108 (post-close): `goto #imm` is TWO words (a 24-bit target) -- the branch
    // selector's far branch. I16ri was 4 by MSP430's accident and is one word now; this def says
    // its own size, because an UNDER-sized far branch would let the selector pick a `bra` that
    // cannot reach (a link failure, never a wrong branch -- session 97 -- but a refusal all the same).
    let Size = 4 in
    def Bi  : I16ri<0b0100, (outs), (ins calltarget:$imm),
                    "goto\t$imm",
                    [(brind tblockaddress:$imm)]>;
    def Br  : I16rr<0b0100, (outs), (ins GR16:$rs),
                    "goto\t$rs",
                    [(brind GR16:$rs)]>;
    def Bm  : I16rm<0b0100, (outs), (ins memsrc:$src),
                    "goto\t$src", []>;
  }
}

// Conditional branches
let Uses = [SR] in
  def JCC : CJForm<(outs), (ins jmptarget:$dst, cc:$cond),
                   "bra\t$cond,$dst",
                   [(DSPICbrcc bb:$dst, imm:$cond)]>;
} // isBranch, isTerminator

//===----------------------------------------------------------------------===//
//  Call Instructions...
//
// All calls clobber the non-callee saved registers. SPW is marked as
// a use to prevent stack-pointer assignments that appear immediately
// before calls from potentially appearing dead. Uses for argument
// registers are added manually.
let isCall = 1,
    Defs = [R11, R12, R13, R14, R15, W5, W6, W7, SR],
    Uses = [SP] in {
  // trellis session 108 (post-close): `call` is TWO words (a 24-bit target). II16i is one word
  // since the size table was corrected; this def says its own size.
  let Size = 4 in
  def CALLi     : II16i<0b101,
                        (outs), (ins calltarget:$imm),
                        "call\t$imm", [(DSPICcall imm:$imm)]>;
  def CALLr     : II16r<0b101,
                        (outs), (ins GR16:$rs),
                        "call\t$rs", [(DSPICcall GR16:$rs)]>;
  def CALLm     : II16m<0b101,
                        (outs), (ins memsrc:$src),
                        "call\t$src", []>;
  def CALLn     : II16n<0b101, (outs), (ins indreg:$rs), "call\t$rs", []>;
  def CALLp     : II16p<0b101, (outs), (ins postreg:$rs), "call\t$rs", []>;
}

//===----------------------------------------------------------------------===//
// Tail calls (trellis L1c, session 85): the epilogue runs, then `bra _sym` or `goto wN`,
// and the callee returns to this function's caller. The compatible-frame rule that admits
// one is DSPICISelLowering.cpp's; `goto`'s register is caller-saved (GR16_TC), since the
// pops restore w8-w13 after the target is computed. Both forms accepted by the GPL `as`
// (prints/l1c/probe4.log).
//===----------------------------------------------------------------------===//
// A callee the user marked `near`: it keeps the PC-relative forms whatever the code model.
def tglobaladdrNear : PatLeaf<(tglobaladdr), [{
  const auto *F = dyn_cast<Function>(cast<GlobalAddressSDNode>(N)->getGlobal());
  return F && F->hasFnAttribute("near");
}]>;

// trellis session 96 (follow-up 15): the code model, for the tail call. The ordinary call picks
// its form in LowerCall; a tail call is selected by pattern, so it needs these.
// ⚠ DEFINED HERE, not beside the other Predicate 1000 lines below: TableGen resolves in file
// order and the uses are at line ~425, so a later definition is "Variable not defined" -- the
// same failure `store_1u` hit earlier this session, one file over.
// ⚠ Spelled through MF, not `Subtarget->`: DSPICDAGToDAGISel holds no Subtarget member (the
// target's only other Predicate reads a cl::opt global instead), and SelectionDAGISel's own MF is
// what is in scope in the generated matcher.
def IsSmallCode : Predicate<"!MF->getSubtarget<DSPICSubtarget>().isLargeCode()">;
def IsLargeCode : Predicate<"MF->getSubtarget<DSPICSubtarget>().isLargeCode()">;

def DSPICtcret : SDNode<"DSPICISD::TC_RETURN", SDT_DSPICCall,
                        [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
let isCall = 1, isTerminator = 1, isReturn = 1, isBarrier = 1, isCodeGenOnly = 1,
    Uses = [SP] in {
  def TCRETURNdi : DSPICFrameInst<(outs), (ins calltarget:$dst), 2, "bra\t$dst">;
  def TCRETURNdiL : DSPICFrameInst<(outs), (ins calltarget:$dst), 4, "goto\t$dst">;
  def TCRETURNri : DSPICFrameInst<(outs), (ins GR16_TC:$dst), 2, "goto\t$dst">;
}
//===----------------------------------------------------------------------===//
// The call-shape words (trellis L1f-a, session 85 post-close; prints/l1f/probe5.log):
//   rcall  — one word, PC-relative (±32K words), cc1's default for every direct call;
//            `call` (two words, absolute) under -mattr=+large-code, as cc1's -mlarge-code;
//   push   — an outgoing stack argument is `push wN` (SP moves), and the caller pops
//            with `sub.w #N,w15` after the call; no reserved outgoing area;
//   clr [wN] — a zero stored through a register (no displacement form exists);
//   retlw / mov.d / push.d — built by DSPICPeephole.cpp, never selected.
//===----------------------------------------------------------------------===//
// trellis session 96 (follow-up 18): the computed branch at the head of a jump table. The
// operands are the case index and the jump-table index; the AsmPrinter expands it into `bra Wn`
// plus one `bra MBB` per entry, which is cc1's own construct and stores no address anywhere.
def SDT_DSPICBrJT : SDTypeProfile<0, 2, [SDTCisVT<0, i16>, SDTCisVT<1, i16>]>;
def DSPICbrjt : SDNode<"DSPICISD::BR_JT", SDT_DSPICBrJT, [SDNPHasChain]>;

// ⛔ `bra Wn` is a COMPUTED RELATIVE branch (PC += 2*Wn) and the target did not have it -- only
// `goto Wn`, which is ABSOLUTE. Emitting `goto w0` with the case INDEX in w0 jumps to address 0..n,
// which is what the first version of this change did. The encoding is MEASURED, not guessed:
//     bra  w0 -> 0x010600     bra  w5 -> 0x010605
//     goto w0 -> 0x010400     goto w5 -> 0x010405
// so the 4-bit selector is 0b0110 where the existing `Br` (goto) uses 0b0100.
let isIndirectBranch = 1, isBarrier = 1, isTerminator = 1, rd = 0 in
def BrRel : I16rr<0b0110, (outs), (ins GR16:$rs), "bra\t$rs", []>;

let isTerminator = 1, isBarrier = 1, isIndirectBranch = 1, isNotDuplicable = 1 in
def BR_JT : DSPICFrameInst<(outs), (ins GR16:$idx, i16imm:$jti), 2,
                           "bra\t$idx"> {
  let Pattern = [(DSPICbrjt GR16:$idx, tjumptable:$jti)];
}

def DSPICrcall : SDNode<"DSPICISD::RCALL", SDT_DSPICCall,
                        [SDNPHasChain, SDNPOutGlue, SDNPOptInGlue, SDNPVariadic]>;
def SDT_DSPICPush : SDTypeProfile<0, 1, [SDTCisVT<0, i16>]>;
def DSPICpush : SDNode<"DSPICISD::PUSH", SDT_DSPICPush, [SDNPHasChain, SDNPMayStore]>;

let isCall = 1, isCodeGenOnly = 1,
    Defs = [R11, R12, R13, R14, R15, W5, W6, W7, SR], Uses = [SP] in
def RCALLi : DSPICFrameInst<(outs), (ins calltarget:$dst), 2, "rcall\t$dst">;
def : Pat<(DSPICrcall (i16 tglobaladdr:$dst)),  (RCALLi tglobaladdr:$dst)>;
def : Pat<(DSPICrcall (i16 texternalsym:$dst)), (RCALLi texternalsym:$dst)>;

let isCodeGenOnly = 1, AddedComplexity = 20 in { // mayStore inferred from the pattern
  def CLR8m  : DSPICFrameInst<(outs), (ins GR16:$p), 2, "clr.b\t[$p]"> {
    let Pattern = [(store (i8 0), GR16:$p)];
  }
  def CLR16m : DSPICFrameInst<(outs), (ins GR16:$p), 2, "clr.w\t[$p]"> {
    let Pattern = [(store (i16 0), GR16:$p)];
  }
}
let isCodeGenOnly = 1, isReturn = 1, isTerminator = 1, isBarrier = 1 in {
  def RETLW16 : DSPICFrameInst<(outs GR16:$rd), (ins i16imm:$imm), 2, "retlw\t$imm,$rd">;
  def RETLW8  : DSPICFrameInst<(outs GR8:$rd),  (ins i16imm:$imm), 2, "retlw.b\t$imm,$rd">;
}
let isCodeGenOnly = 1, hasSideEffects = 0 in
def MOVDrr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "mov.d\t$rs,$rd">;

// trellis session 97: the mirror of tglobaladdrNear. A `far` callee is out of PC-relative reach
// by assertion, so its TAIL call takes the two-word absolute form -- which is cc1's own answer
// (steps/callreach/ask.sh: `goto _r_far` at BOTH code models), not a demotion to call+return.
def tglobaladdrFar : PatLeaf<(tglobaladdr), [{
  const auto *F = dyn_cast<Function>(cast<GlobalAddressSDNode>(N)->getGlobal());
  return F && F->hasFnAttribute("far");
}]>;

// An explicitly `far` callee takes the LONG tail form even under small code, exactly as the
// ordinary call already did. AddedComplexity puts it ahead of the generic row, which is how the
// `near` row under large code is already ordered.
let AddedComplexity = 10 in
def : Pat<(DSPICtcret (i16 tglobaladdrFar:$dst)), (TCRETURNdiL tglobaladdr:$dst)>,
      Requires<[IsSmallCode]>;
def : Pat<(DSPICtcret (i16 tglobaladdr:$dst)),  (TCRETURNdi tglobaladdr:$dst)>,
      Requires<[IsSmallCode]>;
def : Pat<(DSPICtcret (i16 texternalsym:$dst)), (TCRETURNdi texternalsym:$dst)>,
      Requires<[IsSmallCode]>;
// Session 96 (follow-up 20): an explicitly `near` callee keeps the ONE-WORD tail form even under
// -mlarge-code. Without this the ordinary call honoured `near` and the tail call did not, which is
// the asymmetry the data side does not have. AddedComplexity puts it ahead of the generic row.
let AddedComplexity = 10 in
def : Pat<(DSPICtcret (i16 tglobaladdrNear:$dst)), (TCRETURNdi tglobaladdr:$dst)>,
      Requires<[IsLargeCode]>;
def : Pat<(DSPICtcret (i16 tglobaladdr:$dst)),  (TCRETURNdiL tglobaladdr:$dst)>,
      Requires<[IsLargeCode]>;
def : Pat<(DSPICtcret (i16 texternalsym:$dst)), (TCRETURNdiL texternalsym:$dst)>,
      Requires<[IsLargeCode]>;
def : Pat<(DSPICtcret GR16_TC:$dst),            (TCRETURNri GR16_TC:$dst)>;

//===----------------------------------------------------------------------===//
//  Miscellaneous Instructions...
//
let Defs = [SP], Uses = [SP], hasSideEffects = 0 in {
let mayLoad = 1 in
def POP16r   : IForm16<0b0100, DstReg, SrcPostInc, 2,
                       (outs GR16:$rd), (ins), "pop\t$rd", []> {
  let DecoderNamespace = "Delta";
  let rs = 1;
}

let mayStore = 1 in {
def PUSH8r :  II8r<0b100, (outs), (ins GR8:$rs), "push.b\t$rs", []>;
def PUSH16r : II16r<0b100, (outs), (ins GR16:$rs), "push\t$rs", []>;
def PUSH16c : II16c<0b100, (outs), (ins cg16imm:$imm), "push\t$imm", []>;
def PUSH16i : II16i<0b100, (outs), (ins i16imm:$imm), "push\t$imm", []>;
}
}

//===----------------------------------------------------------------------===//
// Move Instructions

let hasSideEffects = 0 in {
def MOV8rr  : I8rr<0b0100,
                   (outs GR8:$rd), (ins GR8:$rs),
                   "mov.b\t$rs,$rd",
                   []>;
def MOV16rr : I16rr<0b0100,
                    (outs GR16:$rd), (ins GR16:$rs),
                    "mov.w\t$rs,$rd",
                    []>;
}

let isReMaterializable = 1, isAsCheapAsAMove = 1 in {
def MOV8rc : I8rc<0b0100,
                   (outs GR8:$rd), (ins cg8imm:$imm),
                   "mov.b\t$imm,$rd",
                   [(set GR8:$rd, cg8imm:$imm)]>;
def MOV16rc : I16rc<0b0100,
                    (outs GR16:$rd), (ins cg16imm:$imm),
                    "mov\t$imm,$rd",
                    [(set GR16:$rd, cg16imm:$imm)]>;
def MOV8ri  : I8ri<0b0100,
                   (outs GR8:$rd), (ins i8imm:$imm),
                   "mov.b\t$imm,$rd",
                   [(set GR8:$rd, imm:$imm)]>;
def MOV16ri : I16ri<0b0100,
                    (outs GR16:$rd), (ins i16imm:$imm),
                    "mov\t$imm,$rd",
                    [(set GR16:$rd, imm:$imm)]>;
}

let canFoldAsLoad = 1, isReMaterializable = 1 in {
def MOV8rm  : I8rm<0b0100,
                   (outs GR8:$rd), (ins memsrc:$src),
                   "mov.b\t$src,$rd",
                   [(set GR8:$rd, (load addr:$src))]>;
def MOV16rm : I16rm<0b0100,
                    (outs GR16:$rd), (ins memsrc:$src),
                    "mov.w\t$src,$rd",
                    [(set GR16:$rd, (load addr:$src))]>;
def MOV8rn  : I8rn<0b0100,
                   (outs GR8:$rd), (ins indreg:$rs),
                   "mov.b\t$rs,$rd",
                   [(set GR8:$rd, (load addr:$rs))]>;
def MOV16rn : I16rn<0b0100,
                    (outs GR16:$rd), (ins indreg:$rs),
                    "mov.w\t$rs,$rd",
                    [(set GR16:$rd, (load addr:$rs))]>;
}

let isCodeGenOnly = 1 in {
def MOVZX16rr8 : I8rr<0b0100,
                      (outs GR16:$rd), (ins GR8:$rs),
                      "ze\t$rs,$rd",
                      [(set GR16:$rd, (zext GR8:$rs))]>;
// Session 94 (REMAT-PLAN Phase 0): the MCID flag the remat gate needs. MOV8rm/MOV16rm already
// carry it from the MSP430 table; this one did not. The flag ALONE does nothing -- the default
// isReMaterializableImpl still refuses a non-invariant load -- so this is inert until
// -dspic-remat-near-global.
let isReMaterializable = 1 in
let Size = 4 in  // two instructions, two words (session 108: the audit)
def MOVZX16rm8 : I8rm<0b0100,
                      (outs GR16:$rd), (ins memsrc:$src),
                      "mov.b\t$src,$rd\n\tze\t$rd,$rd",
                      [(set GR16:$rd, (zextloadi16i8 addr:$src))]>;
}

let mayLoad = 1, hasExtraDefRegAllocReq = 1, Constraints = "$rs = $wb" in {
def MOV8rp  : I8rp<0b0100,
                   (outs GR8:$rd, GR16:$wb), (ins postreg:$rs),
                   "mov.b\t$rs,$rd", []>;
def MOV16rp : I16rp<0b0100,
                    (outs GR16:$rd, GR16:$wb), (ins postreg:$rs),
                    "mov.w\t$rs,$rd", []>;
}

// Any instruction that defines a 8-bit result leaves the high half of the
// register. Truncate can be lowered to EXTRACT_SUBREG, and CopyFromReg may
// be copying from a truncate, but any other 8-bit operation will zero-extend
// up to 16 bits.
def def8 : PatLeaf<(i8 GR8:$src), [{
  return N->getOpcode() != ISD::TRUNCATE &&
         N->getOpcode() != TargetOpcode::EXTRACT_SUBREG &&
         N->getOpcode() != ISD::CopyFromReg;
}]>;

// ⛔ trellis session 99: THE PATTERN THAT WAS HERE WAS AN MSP430 FACT, AND IT MISCOMPILED.
// It read:
//     // In the case of a 8-bit def that is known to implicitly zero-extend,
//     // we can use a SUBREG_TO_REG.
//     def : Pat<(i16 (zext def8:$src)), (SUBREG_TO_REG GR8:$src, subreg_8bit)>;
// SUBREG_TO_REG's contract is that the high bits are ALREADY ZERO. On MSP430 they are: a byte
// instruction clears the upper byte of its destination, which is what the comment above `def8`
// still says. ⛔ ON dsPIC A BYTE INSTRUCTION LEAVES THE UPPER BYTE ALONE, so this deleted an
// extension the program needed and let whatever was in the high half reach the result.
//
// Found by EXECUTION, not by reading: steps/exec/shiftexec X5 read 0xC040 where C says 0x0040 --
// a byte-returning callee's result widened to int. Reduced in steps/byteext, where the radius is
// exactly this pattern's admission rule: a CopyFromReg is excluded (so a call result used
// directly was always correct) and a byte ALU or shift result is not (so those were always wrong).
//
// ⚠ NOT REPLACED BY A LONGER EXCLUSION LIST. There is no opcode set on this machine for which the
// claim holds; `ze` is what zero-extends, and where a high half is genuinely known zero the DAG's
// own known-bits machinery drops the redundant extension without the target asserting a falsehood.
// `def8` itself survives for its other user, but nothing may read it as a zero-extension promise.

def MOV8mc  : I8mc<0b0100,
                   (outs), (ins memdst:$dst, cg8imm:$imm),
                   "mov.b\t$imm,$dst",
                   []>;
def MOV16mc : I16mc<0b0100,
                    (outs), (ins memdst:$dst, cg16imm:$imm),
                    "mov\t$imm,$dst",
                    []>;

def MOV8mi  : I8mi<0b0100,
                   (outs), (ins memdst:$dst, i8imm:$imm),
                   "mov.b\t$imm,$dst",
                   []>;
def MOV16mi : I16mi<0b0100,
                    (outs), (ins memdst:$dst, i16imm:$imm),
                    "mov\t$imm,$dst",
                    []>;

def MOV8mr  : I8mr<0b0100,
                   (outs), (ins memdst:$dst, GR8:$rs),
                   "mov.b\t$rs,$dst",
                   [(store GR8:$rs, addr:$dst)]>;
def MOV16mr : I16mr<0b0100,
                    (outs), (ins memdst:$dst, GR16:$rs),
                    "mov.w\t$rs,$dst",
                    [(store GR16:$rs, addr:$dst)]>;

def MOV8mm  : I8mm<0b0100,
                   (outs), (ins memdst:$dst, memsrc:$src),
                   "mov.b\t$src,$dst",
                   []>;
def MOV16mm : I16mm<0b0100,
                    (outs), (ins memdst:$dst, memsrc:$src),
                    "mov\t$src,$dst",
                    []>;

def MOV8mn  : I8mn<0b0100, (outs), (ins memdst:$dst, indreg:$rs),
                   "mov.b\t$rs,$dst", []>;
def MOV16mn : I16mn<0b0100, (outs), (ins memdst:$dst, indreg:$rs),
                    "mov.w\t$rs,$dst", []>;

//===----------------------------------------------------------------------===//
// Arithmetic Instructions

multiclass Arith<bits<4> opcode, string asmstring, SDPatternOperator node,
                 bit commutes, list<Register> uses> {
  // L1f-a: the register and literal forms are UNTIED -- dsPIC's `add Wb,Ws,Wd` and
  // `add Wb,#lit5,Wd` are three-operand, and the inherited tie cost a copy per add.
  let Defs = [SR], Uses = uses in {
  // Session 95: the three-operand register forms are rematerializable -- their result is a
  // function of their two virtual inputs alone, and canRematerializeAt's allUsesAvailableAt
  // already proves those are live and unchanged at the use (upstream PR #124327's condition).
  // ⛔ ONLY for the ops that do not READ the flags: ADDC/SUBC/DADD pass Uses=[SR] and their
  // result depends on the carry where they stand, so they are not reproducible elsewhere.
  // The MCID flag alone is inert -- the default isReMaterializableImpl rejects the [SR] def --
  // until -dspic-remat-alu turns the target override on.
  let isReMaterializable = !empty(uses) in {
  let isCommutable = commutes in {
  def 8rr : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$src2, GR8:$rs), 2,
                           !strconcat(asmstring, ".b\t$src2,$rs,$rd")> {
    let Pattern = [(set GR8:$rd, (node GR8:$src2, GR8:$rs))];
  }
  def 16rr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$src2, GR16:$rs), 2,
                            !strconcat(asmstring, ".w\t$src2,$rs,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$src2, GR16:$rs))];
  }
  }
  }
  let Constraints = "$src2 = $rd" in {
  // Memory-source forms exist so the inherited C++ compiles; they carry no
  // pattern, since a dsPIC ALU op takes registers and a 5-bit literal only.
  def 8rm : I8rm<opcode, (outs GR8:$rd), (ins GR8:$src2, memsrc:$src),
                 !strconcat(asmstring, ".b\t$rd,$src,$rd"), []>;
  def 16rm : I16rm<opcode, (outs GR16:$rd), (ins GR16:$src2, memsrc:$src),
                   !strconcat(asmstring, ".w\t$rd,$src,$rd"), []>;
  def 8rn : I8rn<opcode, (outs GR8:$rd), (ins GR8:$src2, indreg:$rs),
                 !strconcat(asmstring, ".b\t$rd,$rs,$rd"), []>;
  def 16rn : I16rn<opcode, (outs GR16:$rd), (ins GR16:$src2, indreg:$rs),
                   !strconcat(asmstring, ".w\t$rd,$rs,$rd"), []>;
  let mayLoad = 1,
      hasExtraDefRegAllocReq = 1,
      Constraints = "$rs = $wb, $src2 = $rd" in {
  def 8rp : I8rp<opcode, (outs GR8:$rd, GR16:$wb), (ins GR8:$src2, postreg:$rs),
                 !strconcat(asmstring, ".b\t$rd,$rs,$rd"), []>;
  def 16rp : I16rp<opcode, (outs GR16:$rd, GR16:$wb), (ins GR16:$src2, postreg:$rs),
                   !strconcat(asmstring, ".w\t$rd,$rs,$rd"), []>;
  }
  }
  def 8rc : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$src2, cg8imm:$imm), 2,
                           !strconcat(asmstring, ".b\t$src2,$imm,$rd")> {
    let Pattern = [(set GR8:$rd, (node GR8:$src2, cg8imm:$imm))];
  }
  def 16rc : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$src2, cg16imm:$imm), 2,
                            !strconcat(asmstring, ".w\t$src2,$imm,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$src2, cg16imm:$imm))];
  }
  def 8ri : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$src2, i8imm:$imm), 2,
                           !strconcat(asmstring, ".b\t$src2,$imm,$rd")> {
    let Pattern = [(set GR8:$rd, (node GR8:$src2, lit5imm8:$imm))];
  }
  def 16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$src2, i16imm:$imm), 2,
                            !strconcat(asmstring, ".w\t$src2,$imm,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$src2, lit5imm16:$imm))];
  }
  def 8mr : I8mr<opcode, (outs), (ins memdst:$dst, GR8:$rs),
                 !strconcat(asmstring, ".b\t$rs,$dst"), []>;
  def 16mr : I16mr<opcode, (outs), (ins memdst:$dst, GR16:$rs),
                   !strconcat(asmstring, ".w\t$rs,$dst"), []>;
  def 8mc : I8mc<opcode, (outs), (ins memdst:$dst, cg8imm:$imm),
                 !strconcat(asmstring, ".b\t$imm,$dst"), []>;
  def 16mc : I16mc<opcode, (outs), (ins memdst:$dst, cg16imm:$imm),
                   !strconcat(asmstring, ".w\t$imm,$dst"), []>;
  def 8mi : I8mi<opcode, (outs), (ins memdst:$dst, i8imm:$imm),
                 !strconcat(asmstring, ".b\t$imm,$dst"), []>;
  def 16mi : I16mi<opcode, (outs), (ins memdst:$dst, i16imm:$imm),
                   !strconcat(asmstring, ".w\t$imm,$dst"), []>;
  def 8mm : I8mm<opcode, (outs), (ins memdst:$dst, memsrc:$src),
                 !strconcat(asmstring, ".b\t$src,$dst"), []>;
  def 16mm : I16mm<opcode, (outs), (ins memdst:$dst, memsrc:$src),
                   !strconcat(asmstring, ".w\t$src,$dst"), []>;
  def 8mn : I8mn<opcode, (outs), (ins memdst:$dst, indreg:$rs),
                 !strconcat(asmstring, ".b\t$rs,$dst"), []>;
  def 16mn : I16mn<opcode, (outs), (ins memdst:$dst, indreg:$rs),
                   !strconcat(asmstring, ".w\t$rs,$dst"), []>;
  def 8mp : I8mp<opcode, (outs), (ins memdst:$dst, postreg:$rs),
                 !strconcat(asmstring, ".b\t$rs,$dst"), []>;
  def 16mp : I16mp<opcode, (outs), (ins memdst:$dst, postreg:$rs),
                   !strconcat(asmstring, ".w\t$rs,$dst"), []>;
  }
}
defm ADD  : Arith<0b0101, "add",  add,  1, []>;
defm ADDC : Arith<0b0110, "addc", adde, 1, [SR]>;
defm AND  : Arith<0b1111, "and",  and,  1, []>;
defm BIS  : Arith<0b1101, "ior",  or,   1, []>;
defm XOR  : Arith<0b1110, "xor",  xor,  1, []>;
defm SUB  : Arith<0b1000, "sub",  sub,  0, []>;
defm SUBC : Arith<0b0111, "subb", sube, 0, [SR]>;
defm DADD : Arith<0b1010, "dadd", DSPICdadd, 1, [SR]>;


// L1f-e (trellis session 86): the compare family's flag-to-bool. `bsw.z`/`bsw.c Wd,Wd` set bit 0
// of a cleared Wd to the Z/C flag (Wd holds 0, the bit index); `btg Wd,#0` inverts it. The four
// equality/unsigned conditions land in 4 words; the pseudo's inserter emits the exact cc1 sequence.
let isCodeGenOnly = 1, Uses = [SR], Constraints = "$rs = $rd" in {
  def BSWZ16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "bsw.z\t$rd,$rd">;
  def BSWC16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "bsw.c\t$rd,$rd">;
}
let isCodeGenOnly = 1, Constraints = "$rs = $rd" in
  def BTG16i0 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "btg\t$rd,#0">;
let isCodeGenOnly = 1, usesCustomInserter = 1, Uses = [SR] in
  def FLAG2BOOL16 : Pseudo<(outs GR16:$d), (ins i16imm:$k), "# FLAG2BOOL16",
                           [(set GR16:$d, (DSPICf2b imm:$k))]>;

// ── trellis session 103: THE TABLE INSTRUCTIONS. The effective program address is TBLPAG:Ws.
// `tblrdl` reads bits 15:0 of the program word; `tblrdh` reads bits 23:16 into the low byte of the
// destination and zero into its high byte. The writes target the flash write LATCHES, which the
// NVM controller commits -- and which are themselves readable by a table read at TBLPAG 0xFA,
// which is what bl_fw's `mov #250,w0 ; mov.w w0,_TBLPAG` is doing.
//
// ⛔ hasSideEffects ON ALL OF THEM, DELIBERATELY. They replace an `__asm__ volatile`; the page
// comes from TBLPAG, a memory-mapped SFR these instructions do not name, so a reorderable read
// could be hoisted above the `mov.w wN,_TBLPAG` that selects its page. The prediction this buys is
// that both firmwares come out BYTE-IDENTICAL, which is checkable; a faster model is a later row.
//
// ⚠ The spelling is cc1's `.w`, not the shim's bare mnemonic -- the two assemble to the same
// encoding, so the .s differs and the .hex must not. This backend's own convention (no space after
// the comma) is kept over cc1's.
let hasSideEffects = 1 in {
  let mayLoad = 1 in {
  def TBLRDL16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "tblrdl.w\t[$p],$rd">;
  def TBLRDH16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "tblrdh.w\t[$p],$rd">;
  }
  let mayStore = 1 in {
  def TBLWTL16 : DSPICFrameInst<(outs), (ins GR16:$rs, GR16:$p), 2, "tblwtl.w\t$rs,[$p]">;
  def TBLWTH16 : DSPICFrameInst<(outs), (ins GR16:$rs, GR16:$p), 2, "tblwth.w\t$rs,[$p]">;
  // the MEMORY source, which is the row's one measured size opportunity: cc1 emits
  // `tblwtl.w [w0], [w3]` where the shim's `"r"` constraint forced a `mov` first.
  let mayLoad = 1 in {
  def TBLWTL16mn : DSPICFrameInst<(outs), (ins GR16:$src, GR16:$p), 2, "tblwtl.w\t[$src],[$p]">;
  def TBLWTH16mn : DSPICFrameInst<(outs), (ins GR16:$src, GR16:$p), 2, "tblwth.w\t[$src],[$p]">;
  }
  }
}
// The intrinsic's argument order is the VENDOR's -- (offset, value) -- and the instruction prints
// them the other way round. The swap is here, and mutant MT3 undoes it.
def : Pat<(int_dspic_tblrdl GR16:$p), (TBLRDL16 GR16:$p)>;
def : Pat<(int_dspic_tblrdh GR16:$p), (TBLRDH16 GR16:$p)>;
def : Pat<(int_dspic_tblwtl GR16:$p, GR16:$v), (TBLWTL16 GR16:$v, GR16:$p)>;
def : Pat<(int_dspic_tblwth GR16:$p, GR16:$v), (TBLWTH16 GR16:$v, GR16:$p)>;
let AddedComplexity = 12 in {
def : Pat<(int_dspic_tblwtl GR16:$p, (i16 (load GR16:$src))), (TBLWTL16mn GR16:$src, GR16:$p)>;
def : Pat<(int_dspic_tblwth GR16:$p, (i16 (load GR16:$src))), (TBLWTH16mn GR16:$src, GR16:$p)>;
}

def NOP  : InstAlias<"nop",  (MOV16rc CG, 0)>;
def CLR8r   : InstAlias<"clr.b\t$dst",  (MOV8rc    GR8:$dst,     0)>;
def CLR16r  : InstAlias<"clr\t$dst",    (MOV16rc   GR16:$dst,    0)>;
//===----------------------------------------------------------------------===//
// Shifts and rotates (trellis L1f-c, session 86; prints/l1f/probe6.log, cc1's shapes in
// prints/l1f/cc1/corpus.log). `sl/lsr/asr Wb,#lit4,Wd` and `Wb,Wns,Wnd` are three-operand,
// one word, untied; the two-operand `sl Ws,Wd` shifts by ONE (the `.b` form exists for it
// alone -- a byte shift by k is the word form on the extended byte, cc1's `ze w0,w0 /
// sl w0,w1,w0`); `rlnc`/`rrnc` rotate by one without the carry, `rlc`/`rrc` through it (no
// pattern yet: their use is the i32 shift-by-one pair, the carry row). The amount is an i16.
//===----------------------------------------------------------------------===//
def lit4imm16 : ImmLeaf<i16, [{return Imm >= 2 && Imm < 16;}]>;
multiclass Shift<string asm, SDPatternOperator node> {
  let Defs = [SR] in {
  def 16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, i16imm:$imm), 2,
                            !strconcat(asm, "\t$rb,$imm,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$rb, lit4imm16:$imm))];
  }
  def 16rr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, GR16:$rs), 2,
                            !strconcat(asm, "\t$rb,$rs,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$rb, GR16:$rs))];
  }
  def 16r1 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2,
                            !strconcat(asm, "\t$rs,$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$rs, (i16 1)))];
  }
  def 8r1  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rs), 2,
                            !strconcat(asm, ".b\t$rs,$rd")> {
    let Pattern = [(set GR8:$rd, (node GR8:$rs, (i16 1)))];
  }
  }
}
defm SL  : Shift<"sl",  shl>;
defm LSR : Shift<"lsr", srl>;
defm ASR : Shift<"asr", sra>;
let Defs = [SR] in {
  def RLNC16r : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "rlnc\t$rs,$rd"> {
    let Pattern = [(set GR16:$rd, (rotl GR16:$rs, (i16 1)))];
  }
  def RRNC16r : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "rrnc\t$rs,$rd"> {
    let Pattern = [(set GR16:$rd, (rotr GR16:$rs, (i16 1)))];
  }
  def RLNC8r  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rs), 2, "rlnc.b\t$rs,$rd"> {
    let Pattern = [(set GR8:$rd, (rotl GR8:$rs, (i16 1)))];
  }
  def RRNC8r  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rs), 2, "rrnc.b\t$rs,$rd"> {
    let Pattern = [(set GR8:$rd, (rotr GR8:$rs, (i16 1)))];
  }
  let Uses = [SR] in {
  def RLC16r : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "rlc\t$rs,$rd">;
  def RRC16r : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "rrc\t$rs,$rd">;
  def RLC8r  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs),  2, "rlc.b\t$rs,$rd">;
  def RRC8r  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs),  2, "rrc.b\t$rs,$rd">;
  }
}

// Format-II (Single Operand) Instruction
// Register mode
let Constraints = "$rs = $rd" in {

let Defs = [SR] in {
def SEXT16r : II16r<0b011,
                    (outs GR16:$rd), (ins GR16:$rs),
                    "se\t$rd,$rd",
                    [(set GR16:$rd, (sext_inreg GR16:$rs, i8))]>;

} // Defs = [SR]

let isCodeGenOnly = 1 in
def ZEXT16r : I8rr<0b0100,
                   (outs GR16:$rd), (ins GR16:$rs),
                   "ze\t$rs,$rd",
                   [(set GR16:$rd, (zext (trunc GR16:$rs)))]>;

def SWPB16r : II16r<0b001,
                    (outs GR16:$rd), (ins GR16:$rs),
                    "swap\t$rd",
                    [(set GR16:$rd, (bswap GR16:$rs))]>;

} // Constraints = "$src = $dst"


// Integer comparisons
let Defs = [SR] in {
def CMP8rr  : I8rr<0b1001,
                   (outs), (ins GR8:$rd, GR8:$rs),
                   "cp.b\t$rd,$rs",
                   [(DSPICcmp GR8:$rd, GR8:$rs)]>;
def CMP16rr : I16rr<0b1001,
                    (outs), (ins GR16:$rd, GR16:$rs),
                    "cp\t$rd,$rs",
                    [(DSPICcmp GR16:$rd, GR16:$rs)]>;

def CMP8rc  : I8rc<0b1001,
                   (outs), (ins GR8:$rd, cg8imm:$imm),
                   "cp.b\t$rd,$imm",
                   [(DSPICcmp GR8:$rd, cg8imm:$imm)]>;
def CMP16rc : I16rc<0b1001,
                    (outs), (ins GR16:$rd, cg16imm:$imm),
                    "cp\t$rd,$imm",
                    [(DSPICcmp GR16:$rd, cg16imm:$imm)]>;

def CMP8ri  : I8ri<0b1001,
                   (outs), (ins GR8:$rd, i8imm:$imm),
                   "cp.b\t$rd,$imm",
                   [(DSPICcmp GR8:$rd, lit8imm8:$imm)]>;
def CMP16ri : I16ri<0b1001,
                    (outs), (ins GR16:$rd, i16imm:$imm),
                    "cp\t$rd,$imm",
                    [(DSPICcmp GR16:$rd, lit8imm16:$imm)]>;

def CMP8mc  : I8mc<0b1001,
                   (outs), (ins memsrc:$dst, cg8imm:$imm),
                   "cmp.b\t$imm,$dst",
                   []>;
def CMP16mc : I16mc<0b1001,
                    (outs), (ins memsrc:$dst, cg16imm:$imm),
                    "cmp\t$imm,$dst",
                    []>;

def CMP8mi  : I8mi<0b1001,
                   (outs), (ins memsrc:$dst, i8imm:$imm),
                   "cmp.b\t$imm,$dst",
                   []>;
def CMP16mi : I16mi<0b1001,
                    (outs), (ins memsrc:$dst, i16imm:$imm),
                    "cmp\t$imm,$dst",
                     []>;

def CMP8rm  : I8rm<0b1001,
                   (outs), (ins GR8:$rd, memsrc:$src),
                   "cmp.b\t$src,$rd",
                   []>;
def CMP16rm : I16rm<0b1001,
                    (outs), (ins GR16:$rd, memsrc:$src),
                    "cmp\t$src,$rd",
                    []>;

def CMP8rn  : I8rn<0b1001,
                   (outs), (ins GR8:$rd, indreg:$rs), "cmp.b\t$rs,$rd", []>;
def CMP16rn : I16rn<0b1001,
                    (outs), (ins GR16:$rd, indreg:$rs), "cmp\t$rs,$rd", []>;

def CMP8rp  : I8rp<0b1001,
                   (outs), (ins GR8:$rd, postreg:$rs), "cmp.b\t$rs,$rd", []>;
def CMP16rp : I16rp<0b1001,
                    (outs), (ins GR16:$rd, postreg:$rs), "cmp\t$rs,$rd", []>;

def CMP8mr  : I8mr<0b1001,
                   (outs), (ins memsrc:$dst, GR8:$rs),
                   "cmp.b\t$rs,$dst",
                   []>;
def CMP16mr : I16mr<0b1001,
                    (outs), (ins memsrc:$dst, GR16:$rs),
                    "cmp\t$rs,$dst",
                    []>;
def CMP8mm  : I8mm<0b1001,
                   (outs), (ins memdst:$dst, memsrc:$src),
                   "cmp.b\t$src,$dst",
                   []>;
def CMP16mm : I16mm<0b1001, (outs), (ins memdst:$dst, memsrc:$src),
                    "cmp\t$src,$dst",
                    []>;

def CMP8mn  : I8mn<0b1001, (outs), (ins memsrc:$dst, indreg:$rs),
                   "cmp.b\t$rs,$dst", []>;
def CMP16mn : I16mn<0b1001, (outs), (ins memsrc:$dst, indreg:$rs),
                    "cmp\t$rs,$dst", []>;

def CMP8mp  : I8mp<0b1001, (outs), (ins memsrc:$dst, postreg:$rs),
                   "cmp.b\t$rs,$dst", []>;
def CMP16mp : I16mp<0b1001, (outs), (ins memsrc:$dst, postreg:$rs),
                    "cmp\t$rs,$dst", []>;

// BIT TESTS, just sets condition codes
// Note that the C condition is set differently than when using CMP.
let isCommutable = 1 in {
def BIT8rr  : I8rr<0b1011,
                   (outs), (ins GR8:$rd, GR8:$rs),
                   "and.b\t$rd,$rs,[w15]",
                   [(DSPICcmp (and_su GR8:$rd, GR8:$rs), 0)]>;
def BIT16rr : I16rr<0b1011,
                    (outs), (ins GR16:$rd, GR16:$rs),
                    "and\t$rd,$rs,[w15]",
                    [(DSPICcmp (and_su GR16:$rd, GR16:$rs), 0)]>;
}
def BIT8rc  : I8rc<0b1011,
                   (outs), (ins GR8:$rd, cg8imm:$imm),
                   "bit.b\t$imm,$rd",
                   []>;
def BIT16rc : I16rc<0b1011,
                    (outs), (ins GR16:$rd, cg16imm:$imm),
                    "bit\t$imm,$rd",
                    []>;

def BIT8ri  : I8ri<0b1011,
                   (outs), (ins GR8:$rd, i8imm:$imm),
                   "bit.b\t$imm,$rd",
                   []>;
def BIT16ri : I16ri<0b1011,
                    (outs), (ins GR16:$rd, i16imm:$imm),
                    "bit\t$imm,$rd",
                    []>;

def BIT8rm  : I8rm<0b1011,
                   (outs), (ins GR8:$rd, memdst:$src),
                   "bit.b\t$src,$rd",
                   []>;
def BIT16rm : I16rm<0b1011,
                    (outs), (ins GR16:$rd, memdst:$src),
                    "bit\t$src,$rd",
                    []>;

def BIT8rn  : I8rn<0b1011, (outs), (ins GR8:$rd, indreg:$rs),
                   "bit.b\t$rs,$rd", []>;
def BIT16rn : I16rn<0b1011, (outs), (ins GR16:$rd, indreg:$rs),
                    "bit\t$rs,$rd", []>;

def BIT8rp  : I8rp<0b1011, (outs), (ins GR8:$rd, postreg:$rs),
                   "bit.b\t$rs,$rd", []>;
def BIT16rp : I16rp<0b1011, (outs), (ins GR16:$rd, postreg:$rs),
                    "bit\t$rs,$rd", []>;

def BIT8mr  : I8mr<0b1011,
                  (outs), (ins memsrc:$dst, GR8:$rs),
                  "bit.b\t$rs,$dst",
                  []>;
def BIT16mr : I16mr<0b1011,
                    (outs), (ins memsrc:$dst, GR16:$rs),
                    "bit\t$rs,$dst",
                    []>;

def BIT8mc  : I8mc<0b1011,
                   (outs), (ins memsrc:$dst, cg8imm:$imm),
                   "bit.b\t$imm,$dst",
                   []>;
def BIT16mc : I16mc<0b1011,
                    (outs), (ins memdst:$dst, cg16imm:$imm),
                    "bit\t$imm,$dst",
                    []>;

def BIT8mi  : I8mi<0b1011,
                   (outs), (ins memsrc:$dst, i8imm:$imm),
                   "bit.b\t$imm,$dst",
                   []>;
def BIT16mi : I16mi<0b1011,
                    (outs), (ins memsrc:$dst, i16imm:$imm),
                    "bit\t$imm,$dst",
                    []>;

def BIT8mm  : I8mm<0b1011,
                   (outs), (ins memsrc:$dst, memsrc:$src),
                   "bit.b\t$src,$dst",
                   []>;
def BIT16mm : I16mm<0b1011,
                    (outs), (ins memsrc:$dst, memsrc:$src),
                    "bit\t$src,$dst",
                    []>;
def BIT8mn  : I8mn<0b1011, (outs), (ins memsrc:$dst, indreg:$rs),
                   "bit.b\t$rs,$dst", []>;
def BIT16mn : I16mn<0b1011, (outs), (ins memsrc:$dst, indreg:$rs),
                    "bit\t$rs,$dst", []>;

def BIT8mp  : I8mp<0b1011, (outs), (ins memsrc:$dst, postreg:$rs),
                   "bit.b\t$rs,$dst", []>;
def BIT16mp : I16mp<0b1011, (outs), (ins memsrc:$dst, postreg:$rs),
                    "bit\t$rs,$dst", []>;

} // Defs = [SR]

def TST8r   : InstAlias<"cp0.b\t$dst",  (CMP8rc    GR8:$dst,     0)>;
def TST16r  : InstAlias<"cp0\t$dst",    (CMP16rc   GR16:$dst,    0)>;

//===----------------------------------------------------------------------===//
// Non-Instruction Patterns

// extload
def : Pat<(extloadi16i8 addr:$src), (MOVZX16rm8 addr:$src)>;

// anyext
def : Pat<(i16 (anyext GR8:$src)),
          (SUBREG_TO_REG GR8:$src, subreg_8bit)>;

// truncs
def : Pat<(i8 (trunc GR16:$src)),
          (EXTRACT_SUBREG GR16:$src, subreg_8bit)>;

// GlobalAddress, ExternalSymbol
def : Pat<(i16 (DSPICWrapper tglobaladdr:$dst)), (MOV16ri tglobaladdr:$dst)>;
def : Pat<(i16 (DSPICWrapper texternalsym:$dst)), (MOV16ri texternalsym:$dst)>;
def : Pat<(i16 (DSPICWrapper tblockaddress:$dst)), (MOV16ri tblockaddress:$dst)>;
def : Pat<(i16 (DSPICWrapper tjumptable:$dst)), (MOV16ri tjumptable:$dst)>;  // L1d follow-on (session 88): switch jump tables

// calls
def : Pat<(DSPICcall (i16 tglobaladdr:$dst)),
          (CALLi tglobaladdr:$dst)>;
def : Pat<(DSPICcall (i16 texternalsym:$dst)),
          (CALLi texternalsym:$dst)>;
// an outgoing stack argument (L1f-a): PUSH16r is defined above this point
def : Pat<(DSPICpush GR16:$r), (PUSH16r GR16:$r)>;

// add and sub always produce carry
def : Pat<(addc GR16:$src, GR16:$src2), (ADD16rr GR16:$src, GR16:$src2)>;
def : Pat<(addc GR16:$src, lit5imm16:$src2), (ADD16ri GR16:$src, imm:$src2)>;
def : Pat<(addc GR8:$src, GR8:$src2), (ADD8rr GR8:$src, GR8:$src2)>;
def : Pat<(addc GR8:$src, lit5imm8:$src2), (ADD8ri GR8:$src, imm:$src2)>;
def : Pat<(subc GR16:$src, GR16:$src2), (SUB16rr GR16:$src, GR16:$src2)>;
def : Pat<(subc GR16:$src, lit5imm16:$src2), (SUB16ri GR16:$src, imm:$src2)>;
def : Pat<(subc GR8:$src, GR8:$src2), (SUB8rr GR8:$src, GR8:$src2)>;
def : Pat<(subc GR8:$src, lit5imm8:$src2), (SUB8ri GR8:$src, imm:$src2)>;
// peephole patterns
def : Pat<(and GR16:$src, 255), (ZEXT16r GR16:$src)>;
def : Pat<(DSPICcmp (trunc (and_su GR16:$src, GR16:$src2)), 0),
          (BIT8rr (EXTRACT_SUBREG GR16:$src, subreg_8bit),
                  (EXTRACT_SUBREG GR16:$src2, subreg_8bit))>;


// L1f-d increment 4 (trellis session 86): the hardware divide. `repeat #__TARGET_DIVIDE_CYCLES`
// (an assembler symbol the GPL `as` resolves) prefixes `div.uw`/`div.sw w0,Wdivisor`; the dividend
// is in w0, quotient -> w0, remainder -> w1. The pseudos carry the two-result pattern; the inserter
// copies the dividend to w0 and the pair out.
let isCodeGenOnly = 1, hasSideEffects = 1 in
def REPEATdiv : DSPICFrameInst<(outs), (ins), 2, "repeat\t#__TARGET_DIVIDE_CYCLES">;

// L1d RCOUNT (trellis session 88): save/restore the hardware repeat counter in an ISR that
// could be interrupted mid-repeat. _RCOUNT is the device SFR (an external symbol); the forms
// are `push f`/`pop f` with a fixed operand, so no operand is modelled.
// trellis session 98: the managed-PSV ISR model. `push f`/`pop f` on the two page registers, and
// the setup block that points DSRPAG at the const page. See steps/isrpsv/psv-edit.py.
let Defs = [SP], Uses = [SP], mayStore = 1, isCodeGenOnly = 1 in {
  def PUSHDSRPAG : DSPICFrameInst<(outs), (ins), 2, "push\t_DSRPAG">;
  def PUSHDSWPAG : DSPICFrameInst<(outs), (ins), 2, "push\t_DSWPAG">;
}
let Defs = [SP], Uses = [SP], mayLoad = 1, isCodeGenOnly = 1 in {
  def POPDSRPAG  : DSPICFrameInst<(outs), (ins), 2, "pop\t_DSRPAG">;
  def POPDSWPAG  : DSPICFrameInst<(outs), (ins), 2, "pop\t_DSWPAG">;
}
// ⛔ FIVE WORDS: mov.w #1,$s (1) + mov $s,_DSWPAG (1) + mov #__const_psvpage,$s (TWO -- a 16-bit
// symbol immediate) + movpag $s,DSRPAG (1). The DSWPAG constant is 1, read off cc1 and identical
// in all three of its handlers.
let isCodeGenOnly = 1 in
// ⚠ $s is an OUTPUT, not an input: the pseudo WRITES the scratch and the value is dead after
// the movpag. Declared as `(ins ...)` first, the machine verifier refused it -- "Explicit
// operand marked as def" -- which is the verifier doing exactly its job on a model that did
// not match the machine.
def SETPSVPAGE : DSPICFrameInst<(outs GR16:$s), (ins), 8,   // four one-word lines (session 108: the audit said 10)
  "mov.w\t#1,$s\n\tmov\t$s,_DSWPAG\n\tmov\t#__const_psvpage,$s\n\tmovpag\t$s,DSRPAG">;

let Defs = [SP], Uses = [SP], mayStore = 1, isCodeGenOnly = 1 in
def PUSHRCOUNT : DSPICFrameInst<(outs), (ins), 2, "push\t_RCOUNT">;
let Defs = [SP], Uses = [SP], mayLoad = 1, isCodeGenOnly = 1 in
def POPRCOUNT  : DSPICFrameInst<(outs), (ins), 2, "pop\t_RCOUNT">;
// trellis session 109: the `shadow` handler's fast context save -- `push.s` / `pop.s` swap
// w0..w3, SR<DC,N,OV,Z,C> and CORCON<IPL3> with their shadows in one word each (DS70000157G).
// push.s is the FIRST instruction of the handler and pop.s the LAST before retfie (cc1 isr.cc1.s),
// and neither appears when none of w0..w3 needs saving (T4). The words the handler no longer pushes
// are w0..w3, which DSPICRegisterInfo drops from the shadow ISR's callee-saved list.
let isCodeGenOnly = 1, hasSideEffects = 1 in {
  def PUSHS : DSPICFrameInst<(outs), (ins), 2, "push.s">;
  def POPS  : DSPICFrameInst<(outs), (ins), 2, "pop.s">;
}
// `save(var)`: one WORD of the named global pushed / popped through the scratch register, cc1's
// exact forms (`mov.w [wS++],[w15++]` / `mov.w [--w15],[wS--]`); the scratch advances, so it is
// tied in and out. One word each.
let Defs = [SP], Uses = [SP], mayLoad = 1, mayStore = 1, isCodeGenOnly = 1,
    Constraints = "$s = $d" in {
  def SAVEWORD : DSPICFrameInst<(outs GR16:$d), (ins GR16:$s), 2, "mov.w\t[${s}++],[w15++]">;
  def RESTWORD : DSPICFrameInst<(outs GR16:$d), (ins GR16:$s), 2, "mov.w\t[--w15],[${s}--]">;
}
let isCodeGenOnly = 1, Defs = [R12, R13, SR], Uses = [R12] in {
  def DIVUW : DSPICFrameInst<(outs), (ins GR16_DIV:$d), 2, "div.uw\tw0,$d">;
  def DIVSW : DSPICFrameInst<(outs), (ins GR16_DIV:$d), 2, "div.sw\tw0,$d">;
  // ⛔ trellis session 99: the `repeat` and the divide it drives are ONE instruction of two asm
  // lines (Size 4), exactly as the block-copy forms below are and for exactly the same reason --
  // so that NO PASS can get between them. They were two instructions until now, and the
  // MachineOutliner duly split them: scal-to-vec1 produced an _OUTLINED_FUNCTION holding
  // `mov #2,w0 ; repeat` with the `div.uw` left behind in the caller, which the assembler
  // refused with "'return' cannot follow a REPEAT". An outlined region ending in anything the
  // assembler accepts would have divided wrongly in silence.
  def DIVUWrep : DSPICFrameInst<(outs), (ins GR16_DIV:$d), 4,
                                "repeat\t#__TARGET_DIVIDE_CYCLES\n\tdiv.uw\tw0,$d">;
  def DIVSWrep : DSPICFrameInst<(outs), (ins GR16_DIV:$d), 4,
                                "repeat\t#__TARGET_DIVIDE_CYCLES\n\tdiv.sw\tw0,$d">;
}
let isCodeGenOnly = 1, usesCustomInserter = 1, Defs = [SR] in {
  def UDIVREM16 : Pseudo<(outs GR16:$q, GR16:$r), (ins GR16:$n, GR16_DIV:$d), "# UDIVREM16",
                         [(set GR16:$q, GR16:$r, (DSPICudivrem GR16:$n, GR16_DIV:$d))]>;
  def SDIVREM16 : Pseudo<(outs GR16:$q, GR16:$r), (ins GR16:$n, GR16_DIV:$d), "# SDIVREM16",
                         [(set GR16:$q, GR16:$r, (DSPICsdivrem GR16:$n, GR16_DIV:$d))]>;
}

// L1f-d increment 3 (trellis session 86): the widening i16xi16->i32 multiply. `mul.ss`/`mul.uu`
// write the product to an even:odd pair; cc1 fixes it at w0:w1. The pseudos carry the two-result
// pattern and a custom inserter emits the instruction + copies the pair out of w0:w1.
let isCodeGenOnly = 1, Defs = [R12, R13, SR] in {
  def MULSSpair : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "mul.ss\t$a,$b,w0">;
  def MULUUpair : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "mul.uu\t$a,$b,w0">;
  // trellis session 101: the same product against a 5-bit LITERAL, saving the materialisation.
  // ⛔ THE SIGNED CASE IS `mul.su`, NOT `mul.ss`, AND THAT IS NOT A TYPO. A lit5 operand is
  // unsigned by the instruction's definition, so a signed multiplicand against a literal is
  // signed-times-unsigned; cc1 emits `mul.su w0,#3,w0` for `(long)a * 3`. Using `mul.ss` here
  // assembles clean and produces a wrong product whenever the register operand has bit 15 set --
  // mutant M3 in steps/isaops/.
  def MULSUlit  : DSPICFrameInst<(outs), (ins GR16:$a, i16imm:$b), 2, "mul.su\t$a,$b,w0">;
  def MULUUlit  : DSPICFrameInst<(outs), (ins GR16:$a, i16imm:$b), 2, "mul.uu\t$a,$b,w0">;
}
let isCodeGenOnly = 1, usesCustomInserter = 1, Defs = [SR] in {
  def SMULLOHI16 : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, GR16:$b), "# SMULLOHI16",
                          [(set GR16:$lo, GR16:$hi, (smullohi GR16:$a, GR16:$b))]>;
  def UMULLOHI16 : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, GR16:$b), "# UMULLOHI16",
                          [(set GR16:$lo, GR16:$hi, (umullohi GR16:$a, GR16:$b))]>;
  // the literal twins. AddedComplexity so they win over the register form once the constant is
  // small enough to be an operand; lit5imm16 is the same predicate the three-operand ALU forms use.
  let AddedComplexity = 10 in {
  def SMULLOHI16i : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, i16imm:$b), "# SMULLOHI16i",
                          [(set GR16:$lo, GR16:$hi, (smullohi GR16:$a, lit5imm16:$b))]>;
  def UMULLOHI16i : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, i16imm:$b), "# UMULLOHI16i",
                          [(set GR16:$lo, GR16:$hi, (umullohi GR16:$a, lit5imm16:$b))]>;
  }
}


//===----------------------------------------------------------------------===//
// trellis session 108: MORE ISA CAPABILITIES, INCLUDING THOSE UNREPRESENTABLE BY PLAIN C.
// The asks are steps/isaops/mulf.c and vbi.c (prints/l1f/isaops/*-ask-pre.txt); the written-first
// expectations prints/l1f/mulf.expected.first and vbi.expected.first; execution on the device
// model steps/exec/vbiexec.c. Every value is checked by EXECUTION where the instruction computes
// one -- the assembler is a control for FORMS only (sessions 84, 85, 98, 101).
//===----------------------------------------------------------------------===//

// (0) `mul.b f` -- "MUL: Integer Unsigned Multiply f and WREG", byte mode: (WREG)<7:0> * (f)<7:0>
// -> W2, no flags. The operands are UNSIGNED; a signed byte product never reaches this. The
// registers are FIXED by the instruction (W0 in, W2 out), so the shape is the MULSSpair one: a
// pseudo carries the pattern, the inserter copies the byte into W0, emits the instruction and
// copies W2 out. cc1: `mul.b _gub ; mov.w w2,w0` for `(u16)a * gub` where ours spent five words.
// ⛔ MUL8f USES W0's LOW BYTE AND DEFS W2 AS IMPLICIT OPERANDS; mutant MB2 drops the copy into W0,
// which leaves the text of the argument-in-w0 shape unchanged and computes a wrong product wherever
// the byte lives elsewhere (m_ubub, executed).
let isCodeGenOnly = 1, mayLoad = 1, Uses = [R12B], Defs = [R14] in
def MUL8f : DSPICFrameInst<(outs), (ins memsrc:$f), 2, "mul.b\t$f">;
let isCodeGenOnly = 1, usesCustomInserter = 1, AddedComplexity = 20 in {
  def MULBF16 : Pseudo<(outs GR16:$rd), (ins GR8:$a, memsrc:$f), "# MULBF16",
                       [(set GR16:$rd, (mul (i16 (zext GR8:$a)), (zextloadi16i8 faddr:$f)))]>;
  // the same product when the combiner left the extension outside the load
  def MULBF16z : Pseudo<(outs GR16:$rd), (ins GR8:$a, memsrc:$f), "# MULBF16z",
                        [(set GR16:$rd, (mul (i16 (zext GR8:$a)), (i16 (zext (i8 (load faddr:$f))))))]>;
}

// (1) the MIXED widening multiply, `mul.su Wb,Ws,Wnd` (signed x unsigned) and `mul.us` (unsigned x
// signed), the product to the pair fixed at w0:w1 as MULSSpair. The node is formed by
// combineMixedWideningMul in DSPICISelLowering.cpp from an i32 MUL of a sign-extended and a
// zero-extended i16 -- the shape __builtin_mulsu/mulus emit and the shape `(long)a * (long)b`
// has from plain C. ⛔ WHICH OPERAND IS SIGNED IS THE INSTRUCTION: `mul.su w0,w1` treats w0 as
// signed; mutant MM1 exchanges the two mnemonics and the product is wrong whenever the operand
// that is really unsigned has bit 15 set (executed: -3 x 60000).
def SDT_DSPICMulMixed : SDTypeProfile<2, 2, [SDTCisVT<0, i16>, SDTCisVT<1, i16>,
                                              SDTCisVT<2, i16>, SDTCisVT<3, i16>]>;
def DSPICmulsu : SDNode<"DSPICISD::MULSU", SDT_DSPICMulMixed, []>;
def DSPICmulus : SDNode<"DSPICISD::MULUS", SDT_DSPICMulMixed, []>;
let isCodeGenOnly = 1, Defs = [R12, R13, SR] in {
  def MULSUpair : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "mul.su\t$a,$b,w0">;
  def MULUSpair : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "mul.us\t$a,$b,w0">;
}
let isCodeGenOnly = 1, usesCustomInserter = 1, Defs = [SR] in {
  def MULSU16 : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, GR16:$b), "# MULSU16",
                       [(set GR16:$lo, GR16:$hi, (DSPICmulsu GR16:$a, GR16:$b))]>;
  def MULUS16 : Pseudo<(outs GR16:$lo, GR16:$hi), (ins GR16:$a, GR16:$b), "# MULUS16",
                       [(set GR16:$lo, GR16:$hi, (DSPICmulus GR16:$a, GR16:$b))]>;
}

// (2) fbcl Ws,Wnd -- find first bit change from left; writes C (set iff no change is found).
// swap.b Wn -- the nibbles of the low byte exchanged, in place (one operand, tied).
let Defs = [SR] in
def FBCL16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "fbcl\t$rs,$rd">;
let Constraints = "$rs = $rd" in
def SWAP8r : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rs), 2, "swap.b\t$rd">;
def : Pat<(int_dspic_fbcl GR16:$rs), (FBCL16 GR16:$rs)>;
def : Pat<(int_dspic_swapb GR8:$rs), (SWAP8r GR8:$rs)>;

// (3) the BYTE table forms: session 103's block one width down, the same TBLPAG reasoning and the
// same hasSideEffects. tblrdl.b at an odd offset reads PM<15:8>; tblrdh.b at an odd offset reads
// ZERO (the phantom byte) -- both executed in steps/exec/vbiexec.c. Mutant MT4 makes tblrdh.b
// select the tblrdl.b instruction: 0x56 where 0x12 is owed.
let hasSideEffects = 1 in {
  let mayLoad = 1 in {
  def TBLRDL8 : DSPICFrameInst<(outs GR8:$rd), (ins GR16:$p), 2, "tblrdl.b\t[$p],$rd">;
  def TBLRDH8 : DSPICFrameInst<(outs GR8:$rd), (ins GR16:$p), 2, "tblrdh.b\t[$p],$rd">;
  }
  let mayStore = 1 in {
  def TBLWTL8 : DSPICFrameInst<(outs), (ins GR8:$rs, GR16:$p), 2, "tblwtl.b\t$rs,[$p]">;
  def TBLWTH8 : DSPICFrameInst<(outs), (ins GR8:$rs, GR16:$p), 2, "tblwth.b\t$rs,[$p]">;
  }
}
def : Pat<(int_dspic_tblrdlb GR16:$p), (TBLRDL8 GR16:$p)>;
def : Pat<(int_dspic_tblrdhb GR16:$p), (TBLRDH8 GR16:$p)>;
def : Pat<(int_dspic_tblwtlb GR16:$p, GR8:$v), (TBLWTL8 GR8:$v, GR16:$p)>;
def : Pat<(int_dspic_tblwthb GR16:$p, GR8:$v), (TBLWTH8 GR8:$v, GR16:$p)>;

// (4) the control instructions. The literal of disi/pwrsav is a TargetConstant (ImmArg) matched
// by timm; its range was checked in clang. These are in the inventory's INTRINSIC / asm-only
// exclusion: they move the coverage figure by ZERO and exist for the firmware that calls them.
// ⚠ `nop` had been a static inline over `__asm__ volatile("nop")` in dspic-builtins.h since
// session 88; it is a real builtin now and the shim's definition is gone (the two cannot coexist).
let hasSideEffects = 1 in {
  def DISI    : DSPICFrameInst<(outs), (ins i16imm:$n), 2, "disi\t$n">;
  def PWRSAV  : DSPICFrameInst<(outs), (ins i16imm:$m), 2, "pwrsav\t$m">;
  def CLRWDT  : DSPICFrameInst<(outs), (ins), 2, "clrwdt">;
  def NOPinst : DSPICFrameInst<(outs), (ins), 2, "nop">;
}
def : Pat<(int_dspic_disi timm:$n), (DISI timm:$n)>;
def : Pat<(int_dspic_pwrsav timm:$m), (PWRSAV timm:$m)>;
def : Pat<(int_dspic_clrwdt), (CLRWDT)>;
def : Pat<(int_dspic_nop), (NOPinst)>;

// L1f-c: a byte shifted by k > 1 or by a register is the WORD form on the byte register,
// cc1's own shape: the extension is free for shl (`sl w0,#2,w0`), `ze` for lsr, `se` for
// asr. These are patterns, not a lowering through i16: a Custom i8 shift rebuilt as
// trunc(shl(ext)) LOOPED with the combiner narrowing it back (`slb`, the first run,
// prints/l1f/clang/shifts-hang.log), and Promote does not reach shifts of a legal type
// (the second run: "Cannot select").
// ── trellis session 118: the rest of the vendor's machine-control builtins ─────────────────────
// ⛔ EVERY ASM STRING BELOW IS cc1's OWN, banked at prints/l1f/frontend/builtin2-ask.small.s. The
// two nops after the INTCON2 write are part of the vendor's definition of the builtin, not
// padding -- the same shape as __builtin_write_NVM's two nops, which the vendor's machine
// description hard-codes in the insn template.
// ⚠ `_INTCON2` is an EXTERNAL SFR SYMBOL named inside the instruction's own text, exactly as
// session 88's RCOUNT save names `_RCOUNT`. Nothing declares it: the assembler resolves it from
// the device's linker script.
// ⚠ isCodeGenOnly: a multi-line asm string is not something the ASM PARSER can read back, and
// TableGen refuses it by name ("multiline instruction is not valid for the asmparser, mark it
// isCodeGenOnly") -- which is how REPEATdiv and the divide pair are already marked.
let isCodeGenOnly = 1, hasSideEffects = 1 in {
  def DISABLEINT : DSPICFrameInst<(outs), (ins), 6,
                                  "bclr\t_INTCON2,#15\n\tnop\n\tnop">;
  def ENABLEINT  : DSPICFrameInst<(outs), (ins), 6,
                                  "bset\t_INTCON2,#15\n\tnop\n\tnop">;
  def SWRESET    : DSPICFrameInst<(outs), (ins), 2, "reset">;
  def SWBREAK    : DSPICFrameInst<(outs), (ins), 2, "break">;
  // ⛔ ONE INSTRUCTION OF TWO ASM LINES (Size 4). A `repeat` and the instruction it drives must
  // stay adjacent; session 99 measured a pass splitting a repeat from its divide and the fix was
  // exactly this -- make the pair one MachineInstr. Both operand forms exist because the vendor
  // accepts a variable count as well as a literal.
  // ⚠ `$n` and NOT `#$n`: an i16imm operand prints its own `#`, as DISI's "disi\t$n" does. The
  // first spelling emitted `repeat ##4`, which the GPL assembler would have had to refuse.
  def REPEATNOPi : DSPICFrameInst<(outs), (ins i16imm:$n), 4, "repeat\t$n\n\tnop">;
  def REPEATNOPr : DSPICFrameInst<(outs), (ins GR16:$n), 4, "repeat\t$n\n\tnop">;
}
def : Pat<(int_dspic_disable_interrupts), (DISABLEINT)>;
def : Pat<(int_dspic_enable_interrupts), (ENABLEINT)>;
def : Pat<(int_dspic_software_reset), (SWRESET)>;
def : Pat<(int_dspic_software_breakpoint), (SWBREAK)>;
def : Pat<(int_dspic_repeat_nop imm:$n), (REPEATNOPi imm:$n)>;
def : Pat<(int_dspic_repeat_nop GR16:$n), (REPEATNOPr GR16:$n)>;
// ⚠ the ff1l/ff1r patterns are NOT here: FF1L16 is defined ~160 lines further down and TableGen
// wants the def before the use. They sit beside the ctlz/cttz patterns that share those
// instructions, which is also where a reader looking for them would go.

def : Pat<(shl GR8:$x, lit4imm16:$k),
          (EXTRACT_SUBREG (SL16ri (SUBREG_TO_REG GR8:$x, subreg_8bit), imm:$k), subreg_8bit)>;
def : Pat<(srl GR8:$x, lit4imm16:$k),
          (EXTRACT_SUBREG (LSR16ri (MOVZX16rr8 GR8:$x), imm:$k), subreg_8bit)>;
def : Pat<(sra GR8:$x, lit4imm16:$k),
          (EXTRACT_SUBREG (ASR16ri (SEXT16r (SUBREG_TO_REG GR8:$x, subreg_8bit)), imm:$k), subreg_8bit)>;
def : Pat<(shl GR8:$x, GR16:$n),
          (EXTRACT_SUBREG (SL16rr (SUBREG_TO_REG GR8:$x, subreg_8bit), GR16:$n), subreg_8bit)>;
def : Pat<(srl GR8:$x, GR16:$n),
          (EXTRACT_SUBREG (LSR16rr (MOVZX16rr8 GR8:$x), GR16:$n), subreg_8bit)>;
def : Pat<(sra GR8:$x, GR16:$n),
          (EXTRACT_SUBREG (ASR16rr (SEXT16r (SUBREG_TO_REG GR8:$x, subreg_8bit)), GR16:$n), subreg_8bit)>;
// clang often shifts the promoted word and truncates; the by-one `.b` forms match that too.
def : Pat<(i8 (trunc (shl (i16 (anyext GR8:$x)), (i16 1)))), (SL8r1  GR8:$x)>;
def : Pat<(i8 (trunc (srl (i16 (zext   GR8:$x)), (i16 1)))), (LSR8r1 GR8:$x)>;
def : Pat<(i8 (trunc (sra (i16 (sext   GR8:$x)), (i16 1)))), (ASR8r1 GR8:$x)>;

// L1f-d (trellis session 86): i16 hardware multiply, the low word. `mulw.ss` serves signed
// and unsigned (the low 16 bits agree); the lit5 form is `mulw.su`, cc1's spelling. The
// destination is EVEN (W_MUL_DST_REG); the widening pair forms are a later increment.
let Defs = [SR] in {
def MULWSS16 : DSPICFrameInst<(outs GR16_EVEN:$rd), (ins GR16:$rb, GR16:$rs), 2, "mulw.ss\t$rb,$rs,$rd"> {
  let Pattern = [(set GR16_EVEN:$rd, (mul GR16:$rb, GR16:$rs))];
  let isCommutable = 1;
}
def MULWSU16i : DSPICFrameInst<(outs GR16_EVEN:$rd), (ins GR16:$rb, i16imm:$imm), 2, "mulw.su\t$rb,$imm,$rd"> {
  let Pattern = [(set GR16_EVEN:$rd, (mul GR16:$rb, lit5imm16:$imm))];
}
}


//===----------------------------------------------------------------------===//
// L1f-g (trellis session 87): bit operations on registers, through a pointer, and on a file
// register at the DECLARED width; the two-operand literal ALU forms cc1 prefers to a bit form
// when the literal fits; bfext for a bit field as a value; ff1l/ff1r for clz/ctz. Every
// spelling below was accepted by the GPL pic30 `as` before it was written (prints/l1f/
// probe6.log, probe7.log); cc1's shapes are prints/l1f/cc1/bits-Os.s.
// ⛔ The width rule (IR-DESIGN.md §7; overlays.md's one-access guarantee): a file-register form
// is selected only at the access's own width -- an i16 load/store pair is `bclr.w _f,#k`, ONE
// word, one read and one write of the whole place, and NEVER cc1's `bclr.b _f+1,#k-8`, which
// narrows a word place to a byte. The i8 forms take an i8 pair only.
//===----------------------------------------------------------------------===//
def pow2imm16  : ImmLeaf<i16, [{return isPowerOf2_32(Imm & 0xffff);}]>;
def pow2imm8   : ImmLeaf<i8,  [{return isPowerOf2_32(Imm & 0xff);}]>;
// a single bit at or above bit 10 on a register: below it cc1 takes the one-word `ior #lit10`
def pow2hi16   : ImmLeaf<i16, [{return isPowerOf2_32(Imm & 0xffff) && (Imm & 0xffff) >= 1024;}]>;
def npow2imm16 : ImmLeaf<i16, [{return isPowerOf2_32(~Imm & 0xffff);}]>;
def npow2imm8  : ImmLeaf<i8,  [{return isPowerOf2_32(~Imm & 0xff);}]>;
def bitidx16  : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(Log2_32(N->getZExtValue() & 0xffff), SDLoc(N), MVT::i16);}]>;
def bitidx8   : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(Log2_32(N->getZExtValue() & 0xff), SDLoc(N), MVT::i16);}]>;
def nbitidx16 : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(Log2_32(~N->getZExtValue() & 0xffff), SDLoc(N), MVT::i16);}]>;
def nbitidx8  : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(Log2_32(~N->getZExtValue() & 0xff), SDLoc(N), MVT::i16);}]>;
// the two-operand literal forms: lit10 on a word (the untied three-operand lit5 form keeps
// 0..31), lit8 on a byte, both UNSIGNED (probe6: `add #-1,w0` refused); 255 stays `ze`
def lit10imm16    : ImmLeaf<i16, [{return Imm >= 32 && Imm < 1024;}]>;
def lit10and16    : ImmLeaf<i16, [{return Imm >= 32 && Imm < 1024 && Imm != 255;}]>;
def neglit10imm16 : ImmLeaf<i16, [{return -Imm >= 32 && -Imm < 1024;}]>;
def lit8imm8hi    : ImmLeaf<i8,  [{return (Imm & 0xff) >= 32;}]>;
def negimm16 : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(-N->getSExtValue(), SDLoc(N), MVT::i16);}]>;
def lit8mask : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(N->getZExtValue() & 0xff, SDLoc(N), MVT::i16);}]>;
// a bit field: the mask after a shift is 2^n - 1 (n < 16); `bfext #k,#n` extracts it in one word
def bfmask16 : ImmLeaf<i16, [{unsigned V = Imm & 0xffff; return V != 0 && isMask_32(V) && V != 0xffff;}]>;
def bfwid16  : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(llvm::popcount(N->getZExtValue() & 0xffff), SDLoc(N), MVT::i16);}]>;

// bit set / clear / toggle on a register: no flag is written
let Constraints = "$rs = $rd" in {
  def BSET16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$k), 2, "bset\t$rd,$k">;
  def BCLR16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$k), 2, "bclr\t$rd,$k">;
  def BTG16ri  : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$k), 2, "btg\t$rd,$k">;
  def BCLR8ri  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs,  i16imm:$k), 2, "bclr.b\t$rd,$k">;
}
// the two-operand literal ALU forms (flags written, as every ALU op)
let Defs = [SR], Constraints = "$rs = $rd" in {
  def ADD16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "add\t$imm,$rd">;
  def SUB16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "sub\t$imm,$rd">;
  def AND16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "and\t$imm,$rd">;
  def IOR16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "ior\t$imm,$rd">;
  def XOR16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "xor\t$imm,$rd">;
  def ADD8rl  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs,  i16imm:$imm), 2, "add.b\t$imm,$rd">;
  def AND8rl  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs,  i16imm:$imm), 2, "and.b\t$imm,$rd">;
  def IOR8rl  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs,  i16imm:$imm), 2, "ior.b\t$imm,$rd">;
  def XOR8rl  : DSPICFrameInst<(outs GR8:$rd),  (ins GR8:$rs,  i16imm:$imm), 2, "xor.b\t$imm,$rd">;
}
// trellis session 102: the same two-operand literal forms for the ops that READ the carry.
// ⛔ Uses=[SR] AS WELL AS Defs=[SR]. The block above is add/sub/and/ior/xor, which write the flags
// and read none; addc and subb read C where they stand, and without the Uses the scheduler is free
// to move one away from the instruction that produced its carry -- session 101's SUBBRM16rn
// carries the same warning, and its mutant is the reason it is spelled out here.
let Defs = [SR], Uses = [SR], Constraints = "$rs = $rd" in {
  def ADDC16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "addc\t$imm,$rd">;
  def SUBC16rl : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$imm), 2, "subb\t$imm,$rd">;
}
// the read-modify-write forms: one instruction, one read and one write of the place, at the
// width of the access that selected it
let mayLoad = 1, mayStore = 1 in {
  def BSET16f : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "bset.w\t$f,$k">;
  def BCLR16f : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "bclr.w\t$f,$k">;
  def BTG16f  : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "btg.w\t$f,$k">;
  def BSET8f  : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "bset.b\t$f,$k">;
  def BCLR8f  : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "bclr.b\t$f,$k">;
  def BTG8f   : DSPICFrameInst<(outs), (ins memdst:$f, i16imm:$k), 2, "btg.b\t$f,$k">;
  def BSET16n : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "bset\t[$p],$k">;
  def BCLR16n : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "bclr\t[$p],$k">;
  def BTG16n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "btg\t[$p],$k">;
  def BSET8n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "bset.b\t[$p],$k">;
  def BCLR8n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "bclr.b\t[$p],$k">;
  def BTG8n   : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "btg.b\t[$p],$k">;
}
// the tests: `btst` (= btst.z) writes Z = NOT bit, the same Z `cp (x & mask),#0` writes; the
// byte register test is the word form on the byte's register (no `btst.b Wn` exists)
let Defs = [SR] in {
  def BTST16ri : DSPICFrameInst<(outs), (ins GR16:$x, i16imm:$k), 2, "btst\t$x,$k">;
  def BTST16n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "btst\t[$p],$k">;
  let mayLoad = 1 in {
  def BTST16f  : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btst.w\t$f,$k">;
  def BTST8f   : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btst.b\t$f,$k">;
  }
}
// the skip forms, built by DSPICPeephole from a test + a branch over ONE instruction; they
// write no flag, and the instruction after them is the one skipped (never moved: side effects).
// A TERMINATOR that is not a branch: the block keeps both successors, and analyzeBranch declines
// it ("a terminator that isn't a branch"), which is what keeps the machine verifier from
// demanding a branch target for the second successor (the first build: "MBB has unexpected
// successors which are not branch targets, fallthrough ...").
let hasSideEffects = 1, isTerminator = 1 in {
  def BTSC16ri : DSPICFrameInst<(outs), (ins GR16:$x, i16imm:$k), 2, "btsc\t$x,$k">;
  def BTSS16ri : DSPICFrameInst<(outs), (ins GR16:$x, i16imm:$k), 2, "btss\t$x,$k">;
  def BTSC16n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "btsc\t[$p],$k">;
  def BTSS16n  : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$k), 2, "btss\t[$p],$k">;
  let mayLoad = 1 in {
  def BTSC16f  : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btsc.w\t$f,$k">;
  def BTSS16f  : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btss.w\t$f,$k">;
  def BTSC8f   : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btsc.b\t$f,$k">;
  def BTSS8f   : DSPICFrameInst<(outs), (ins memsrc:$f, i16imm:$k), 2, "btss.b\t$f,$k">;
  }
}
// trellis session 103: the compare-and-branch forms. `cp Wb,Wn` + `bra cc,L` in one word.
//
// ⛔ NO Defs AND NO Uses OF SR, AND THAT IS MEASURED, NOT ASSUMED. steps/exec/cmpprobe.c on the
// device model reads f_cpbne = f_cpbgt = 0 where a plain `cp` in the identical harness reads 1:
// these instructions leave the status flags exactly as they found them. Since the CMP16rr they
// replace carries `Defs = [SR]`, the fusion REMOVES a flag definition, and DSPICCmpFuse refuses
// any site where SR is live out of the branch.
//
// The operands are the compare's own, in the compare's own order: `cpblt $a,$b,L` branches when
// $a < $b SIGNED, measured with 0x8000 against 0x0001 (the pair whose signed and unsigned
// orderings disagree). ⛔ THE ISA HAS NO UNSIGNED TWIN -- there is deliberately no CPBLTU here.
// The `.b` forms compare the LOW BYTE only (0x1234 against 0xff34 branches; measured).
//
// isTerminator without isBranch, on the BTSC precedent above: the block already carries both
// successors from the JCC this replaces, and analyzeBranch declining ("a terminator that isn't a
// branch") is what keeps the verifier from demanding a branch target for the second successor.
let hasSideEffects = 0, isTerminator = 1 in {
  def CPBEQ16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b, jmptarget:$dst), 2, "cpbeq\t$a,$b,$dst">;
  def CPBNE16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b, jmptarget:$dst), 2, "cpbne\t$a,$b,$dst">;
  def CPBLT16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b, jmptarget:$dst), 2, "cpblt\t$a,$b,$dst">;
  def CPBGT16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b, jmptarget:$dst), 2, "cpbgt\t$a,$b,$dst">;
  def CPBEQ8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b,  jmptarget:$dst), 2, "cpbeq.b\t$a,$b,$dst">;
  def CPBNE8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b,  jmptarget:$dst), 2, "cpbne.b\t$a,$b,$dst">;
  def CPBLT8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b,  jmptarget:$dst), 2, "cpblt.b\t$a,$b,$dst">;
  def CPBGT8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b,  jmptarget:$dst), 2, "cpbgt.b\t$a,$b,$dst">;
}

// trellis session 105: the compare-and-SKIP forms. `cpsXX Wb,Wn` skips the next instruction when
// the condition holds. Like the cpb forms they take two registers and nothing else, the ordering
// forms are SIGNED, and they WRITE NO FLAGS (steps/exec/cmpprobe.c F3, measured), so DSPICCmpFuse
// emits one only where it would have emitted the cpb form -- the same SR-deadness walk -- and
// only when the branch's displacement is exactly the one word the skip covers. Same terminator
// convention as the cpb forms: the block keeps both successors from the JCC this replaces, and
// the skipped instruction stays in its own block. One word each.
let hasSideEffects = 0, isTerminator = 1 in {
  def CPSEQ16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "cpseq\t$a,$b">;
  def CPSNE16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "cpsne\t$a,$b">;
  def CPSLT16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "cpslt\t$a,$b">;
  def CPSGT16 : DSPICFrameInst<(outs), (ins GR16:$a, GR16:$b), 2, "cpsgt\t$a,$b">;
  def CPSEQ8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b),  2, "cpseq.b\t$a,$b">;
  def CPSNE8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b),  2, "cpsne.b\t$a,$b">;
  def CPSLT8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b),  2, "cpslt.b\t$a,$b">;
  def CPSGT8  : DSPICFrameInst<(outs), (ins GR8:$a,  GR8:$b),  2, "cpsgt.b\t$a,$b">;
}

// a bit field as a value, and the find-first-one pair
let Defs = [SR] in {
  def BFEXT16rr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$k, i16imm:$n), 4, "bfext\t$k,$n,$rs,$rd">;  // two words (session 108: the audit)
  let mayLoad = 1 in
  def BFEXT16fr : DSPICFrameInst<(outs GR16:$rd), (ins memsrc:$f, i16imm:$k, i16imm:$n), 4, "bfext\t$k,$n,$f,$rd">;  // two words (session 108: the audit)
  def FF1L16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "ff1l\t$rs,$rd">;
  def FF1R16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "ff1r\t$rs,$rd">;
}

// registers
let AddedComplexity = 2 in {
def : Pat<(or  GR16:$x, pow2hi16:$k),   (BSET16ri GR16:$x, (bitidx16 imm:$k))>;
def : Pat<(xor GR16:$x, pow2hi16:$k),   (BTG16ri  GR16:$x, (bitidx16 imm:$k))>;
def : Pat<(and GR16:$x, npow2imm16:$k), (BCLR16ri GR16:$x, (nbitidx16 imm:$k))>;
def : Pat<(and GR8:$x,  npow2imm8:$k),  (BCLR8ri  GR8:$x,  (nbitidx8 imm:$k))>;
}
// the literal two-operand forms
def : Pat<(add GR16:$x, lit10imm16:$i),    (ADD16rl GR16:$x, imm:$i)>;
def : Pat<(add GR16:$x, neglit10imm16:$i), (SUB16rl GR16:$x, (negimm16 imm:$i))>;
def : Pat<(sub GR16:$x, lit10imm16:$i),    (SUB16rl GR16:$x, imm:$i)>;
def : Pat<(and GR16:$x, lit10and16:$i),    (AND16rl GR16:$x, imm:$i)>;
def : Pat<(or  GR16:$x, lit10imm16:$i),    (IOR16rl GR16:$x, imm:$i)>;
def : Pat<(xor GR16:$x, lit10imm16:$i),    (XOR16rl GR16:$x, imm:$i)>;
// trellis session 102, THE LITERAL CARRY CHAIN. A constant half in 32..1023 is an operand of the
// TIED two-operand form; without these four it was a `mov` into a register that the untied
// three-operand lit5 form could not take. lit10imm16 is 32..1023 and lit5imm16 is 0..31, so these
// never overlap the patterns the Arith multiclass already carries.
def : Pat<(addc GR16:$x, lit10imm16:$i), (ADD16rl  GR16:$x, imm:$i)>;
def : Pat<(subc GR16:$x, lit10imm16:$i), (SUB16rl  GR16:$x, imm:$i)>;
// ⛔ AND THE LITERAL FORMS MUST OUTRANK THE MEMORY-SOURCE ONE, which the written-first expectation
// is what caught. Session 101 gave `sube` a memory-source form, `subbr Wb,[Ws],Wd` at
// AddedComplexity 12, for the shape `memory MINUS register`. Once the chain flips, `gl -= 1000`
// offers it `sube(load _gl+2, 0)` -- and it wins on complexity, materialising the ZERO into a
// register with a `clr` and the ADDRESS with a `mov`, three words where `subb.w wN,#0,wN` is one.
// k_gsub came out at 8 words against cc1's 7 and a prediction of 7. A literal never needs a
// register, so a literal pattern must outrank a form that would put it in one.
let AddedComplexity = 14 in {
def : Pat<(adde GR16:$x, lit5imm16:$i),  (ADDC16ri GR16:$x, imm:$i)>;
def : Pat<(sube GR16:$x, lit5imm16:$i),  (SUBC16ri GR16:$x, imm:$i)>;
def : Pat<(adde GR16:$x, lit10imm16:$i), (ADDC16rl GR16:$x, imm:$i)>;
def : Pat<(sube GR16:$x, lit10imm16:$i), (SUBC16rl GR16:$x, imm:$i)>;
}
def : Pat<(add GR8:$x, lit8imm8hi:$i), (ADD8rl GR8:$x, (lit8mask imm:$i))>;
def : Pat<(and GR8:$x, lit8imm8hi:$i), (AND8rl GR8:$x, (lit8mask imm:$i))>;
def : Pat<(or  GR8:$x, lit8imm8hi:$i), (IOR8rl GR8:$x, (lit8mask imm:$i))>;
def : Pat<(xor GR8:$x, lit8imm8hi:$i), (XOR8rl GR8:$x, (lit8mask imm:$i))>;
// read-modify-write of a place: the file forms at the access's width, the pointer forms
let AddedComplexity = 30 in {
def : Pat<(store (or  (i16 (load faddr:$f)), pow2imm16:$k),  faddr:$f), (BSET16f faddr:$f, (bitidx16 imm:$k))>;
def : Pat<(store (xor (i16 (load faddr:$f)), pow2imm16:$k),  faddr:$f), (BTG16f  faddr:$f, (bitidx16 imm:$k))>;
def : Pat<(store (and (i16 (load faddr:$f)), npow2imm16:$k), faddr:$f), (BCLR16f faddr:$f, (nbitidx16 imm:$k))>;
def : Pat<(store (or  (i8 (load faddr:$f)), pow2imm8:$k),    faddr:$f), (BSET8f  faddr:$f, (bitidx8 imm:$k))>;
def : Pat<(store (xor (i8 (load faddr:$f)), pow2imm8:$k),    faddr:$f), (BTG8f   faddr:$f, (bitidx8 imm:$k))>;
def : Pat<(store (and (i8 (load faddr:$f)), npow2imm8:$k),   faddr:$f), (BCLR8f  faddr:$f, (nbitidx8 imm:$k))>;
def : Pat<(store (or  (i16 (load GR16:$p)), pow2imm16:$k),  GR16:$p), (BSET16n GR16:$p, (bitidx16 imm:$k))>;
def : Pat<(store (xor (i16 (load GR16:$p)), pow2imm16:$k),  GR16:$p), (BTG16n  GR16:$p, (bitidx16 imm:$k))>;
def : Pat<(store (and (i16 (load GR16:$p)), npow2imm16:$k), GR16:$p), (BCLR16n GR16:$p, (nbitidx16 imm:$k))>;
def : Pat<(store (or  (i8 (load GR16:$p)), pow2imm8:$k),    GR16:$p), (BSET8n  GR16:$p, (bitidx8 imm:$k))>;
def : Pat<(store (xor (i8 (load GR16:$p)), pow2imm8:$k),    GR16:$p), (BTG8n   GR16:$p, (bitidx8 imm:$k))>;
def : Pat<(store (and (i8 (load GR16:$p)), npow2imm8:$k),   GR16:$p), (BCLR8n  GR16:$p, (nbitidx8 imm:$k))>;
}
// the tests, from `cp (x & 1<<k),#0` (and from the combiner's `(x >> k) & 1` spelling of it)
let AddedComplexity = 10 in {
def : Pat<(DSPICcmp (and_su GR16:$x, pow2imm16:$k), 0), (BTST16ri GR16:$x, (bitidx16 imm:$k))>;
def : Pat<(DSPICcmp (and_su GR8:$x, pow2imm8:$k), 0),
          (BTST16ri (SUBREG_TO_REG GR8:$x, subreg_8bit), (bitidx8 imm:$k))>;
def : Pat<(DSPICcmp (and_su (srl GR16:$x, (i16 imm:$k)), 1), 0), (BTST16ri GR16:$x, imm:$k)>;
def : Pat<(DSPICcmp (and_su (i16 (load faddr:$f)), pow2imm16:$k), 0), (BTST16f faddr:$f, (bitidx16 imm:$k))>;
def : Pat<(DSPICcmp (and_su (i8 (load faddr:$f)), pow2imm8:$k), 0),   (BTST8f  faddr:$f, (bitidx8 imm:$k))>;
def : Pat<(DSPICcmp (and_su (srl (i16 (load faddr:$f)), (i16 imm:$k)), 1), 0), (BTST16f faddr:$f, imm:$k)>;
def : Pat<(DSPICcmp (and_su (i16 (load GR16:$p)), pow2imm16:$k), 0), (BTST16n GR16:$p, (bitidx16 imm:$k))>;
def : Pat<(DSPICcmp (and_su (srl (i16 (load GR16:$p)), (i16 imm:$k)), 1), 0), (BTST16n GR16:$p, imm:$k)>;
}
// a bit field as a value: (x >> k) & (2^n - 1)
let AddedComplexity = 10 in {
def : Pat<(and (srl GR16:$x, (i16 imm:$k)), bfmask16:$m), (BFEXT16rr GR16:$x, imm:$k, (bfwid16 imm:$m))>;
def : Pat<(and (srl (i16 (load faddr:$f)), (i16 imm:$k)), bfmask16:$m), (BFEXT16fr faddr:$f, imm:$k, (bfwid16 imm:$m))>;
}
// clz / ctz: ff1l / ff1r are 1-based, so decrement; zero is undefined for both builtins
// (the ctlz/cttz patterns are at the end of the file since L1f-f: their decrement is `dec.w`)

// L1f-g, second run: `(x & 1<<k) == 0` as a value reaches isel as ((~x) >> k) & 1 (the combiner
// hoists the xor through the shift into a `not`, bits-dag.log); cc1's shape is the mask xor'd
// first, then the extract -- `xor #128,w0 ; bfext #7,#1,w0,w0` (btg for a bit above 9), and
// from a file place the extract first, then `xor #1`.
def bitmask16 : SDNodeXForm<imm, [{return CurDAG->getTargetConstant(1u << (N->getZExtValue() & 15), SDLoc(N), MVT::i16);}]>;
def shamtlo16 : ImmLeaf<i16, [{return Imm >= 0 && Imm <= 9;}]>;
def shamthi16 : ImmLeaf<i16, [{return Imm >= 10 && Imm <= 15;}]>;
let AddedComplexity = 12 in {
def : Pat<(and (srl (not GR16:$x), shamtlo16:$k), 1), (BFEXT16rr (XOR16rl GR16:$x, (bitmask16 imm:$k)), imm:$k, 1)>;
def : Pat<(and (srl (not GR16:$x), shamthi16:$k), 1), (BFEXT16rr (BTG16ri GR16:$x, imm:$k), imm:$k, 1)>;
def : Pat<(and (srl (not (i16 (load faddr:$f))), (i16 imm:$k)), 1), (XOR16ri (BFEXT16fr faddr:$f, imm:$k, 1), 1)>;
}


//===----------------------------------------------------------------------===//
// L1f-f (trellis session 87): memory-operand ALU and moves. dsPIC's Ws/Wd addressing on the
// ALU ops as source and as destination, the one-operand family on memory, memory-to-memory
// mov and the [Wn+Wb] index on mov, the indexed load/store modes, mov.d by peephole, the i32
// shift-by-one pair, and rrnc for a rotate by 15. Every spelling was accepted by the GPL
// pic30 `as` before it was written (prints/l1f/probe8.log); cc1's shapes are
// prints/l1f/cc1/mem-Os.s. The width is the access's own, as at L1f-g.
//===----------------------------------------------------------------------===//
def SDT_DSPICSh32 : SDTypeProfile<2, 2, [SDTCisVT<0, i16>, SDTCisVT<1, i16>,
                                          SDTCisVT<2, i16>, SDTCisVT<3, i16>]>;
def DSPICshl32_1 : SDNode<"DSPICISD::SHL32_1", SDT_DSPICSh32, []>;
def DSPICsrl32_1 : SDNode<"DSPICISD::SRL32_1", SDT_DSPICSh32, []>;
def DSPICsra32_1 : SDNode<"DSPICISD::SRA32_1", SDT_DSPICSh32, []>;

def neglit5imm16 : ImmLeaf<i16, [{return Imm < 0 && -Imm < 32;}]>;
def inc1f  : PatFrag<(ops node:$x), (add node:$x, (i16 1))>;
def inc2f  : PatFrag<(ops node:$x), (add node:$x, (i16 2))>;
def dec1f  : PatFrag<(ops node:$x), (add node:$x, (i16 -1))>;
def dec2f  : PatFrag<(ops node:$x), (add node:$x, (i16 -2))>;
def negf   : PatFrag<(ops node:$x), (sub (i16 0), node:$x)>;
def comf   : PatFrag<(ops node:$x), (xor node:$x, (i16 -1))>;
def sl1f   : PatFrag<(ops node:$x), (shl node:$x, (i16 1))>;
def inc1f8 : PatFrag<(ops node:$x), (add node:$x, (i8 1))>;
def dec1f8 : PatFrag<(ops node:$x), (add node:$x, (i8 -1))>;
def negf8  : PatFrag<(ops node:$x), (sub (i8 0), node:$x)>;
def comf8  : PatFrag<(ops node:$x), (xor node:$x, (i8 -1))>;
// trellis session 101: the fragments class (a) was missing. inc2/dec2 at byte width, and the two
// rotate-by-one directions, which arrive as NATIVE rotl/rotr nodes at both widths. The shift-by-one
// fragments are NOT duplicated at byte width -- see steps/filerm/filerm-edit.py: i8 SRL is not
// promoted here, so lsr1ff/asr1ff/sl1f serve both widths as they stand.
def inc2f8  : PatFrag<(ops node:$x), (add node:$x, (i8 2))>;
def dec2f8  : PatFrag<(ops node:$x), (add node:$x, (i8 -2))>;
def rotl1f  : PatFrag<(ops node:$x), (rotl node:$x, (i16 1))>;
def rotr1f  : PatFrag<(ops node:$x), (rotr node:$x, (i16 1))>;

// A. the two-input ALU ops with a memory operand. Source: `op Wb,[Wn],Wd` (untied); the
// post-increment/decrement source forms carry the pointer's writeback and are selected in
// C++ (tryIndexedBinOp). Destination: the read-modify-write `op Wb,[Wn],[Wn]`, and the store
// of a computed value `op Wb,Ws,[Wn]` / `op Wb,#lit5,[Wn]`.
// Session 96: a memory-destination read-modify-write consumes the arithmetic node and produces NO
// value, so folding one whose result is ALSO used elsewhere forces the computation to be redone --
// and, for the [Wn] forms, an address materialisation besides. Two instructions spent to lose the
// result. Require the stored value to have exactly one use (the store itself); isel then falls
// through to the file-to-WREG form plus a direct store, which is cc1's own pair for `++g`.
// A StoreSDNode's operands are (chain, value, ptr), so operand 1 is the value.
def store_1u : PatFrag<(ops node:$val, node:$ptr), (store node:$val, node:$ptr), [{
  return N->getOperand(1)->hasOneUse();
}]>;

multiclass MemALU<string asmw, string asmb, SDPatternOperator node> {
  let Defs = [SR] in {
  let mayLoad = 1 in {
  def 16rn : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, GR16:$p), 2, !strconcat(asmw, "\t$rb,[$p],$rd")> {
    let Pattern = [(set GR16:$rd, (node GR16:$rb, (i16 (load GR16:$p))))]; }
  def 8rn  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rb, GR16:$p), 2, !strconcat(asmb, "\t$rb,[$p],$rd")> {
    let Pattern = [(set GR8:$rd, (node GR8:$rb, (i8 (load GR16:$p))))]; }
  let hasExtraDefRegAllocReq = 1, Constraints = "$p = $wb" in {
  def 16rpu : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, !strconcat(asmw, "\t$rb,[${p}++],$rd")>;
  def 8rpu  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, !strconcat(asmb, "\t$rb,[${p}++],$rd")>;
  def 16rpd : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, !strconcat(asmw, "\t$rb,[${p}--],$rd")>;
  def 8rpd  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, !strconcat(asmb, "\t$rb,[${p}--],$rd")>;
  }
  }
  let mayLoad = 1, mayStore = 1 in {
  def 16nrn : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$p), 2, !strconcat(asmw, "\t$rb,[$p],[$p]")> {
    let Pattern = [(store_1u (node (i16 (load GR16:$p)), GR16:$rb), GR16:$p)]; }
  def 8nrn  : DSPICFrameInst<(outs), (ins GR8:$rb, GR16:$p), 2, !strconcat(asmb, "\t$rb,[$p],[$p]")> {
    let Pattern = [(store_1u (node (i8 (load GR16:$p)), GR8:$rb), GR16:$p)]; }
  }
  let mayStore = 1 in {
  def 16rrn : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$rs, GR16:$p), 2, !strconcat(asmw, "\t$rb,$rs,[$p]")> {
    let Pattern = [(store_1u (node GR16:$rb, GR16:$rs), GR16:$p)]; }
  def 8rrn  : DSPICFrameInst<(outs), (ins GR8:$rb, GR8:$rs, GR16:$p), 2, !strconcat(asmb, "\t$rb,$rs,[$p]")> {
    let Pattern = [(store_1u (node GR8:$rb, GR8:$rs), GR16:$p)]; }
  def 16rin : DSPICFrameInst<(outs), (ins GR16:$rb, i16imm:$imm, GR16:$p), 2, !strconcat(asmw, "\t$rb,$imm,[$p]")> {
    let Pattern = [(store_1u (node GR16:$rb, lit5imm16:$imm), GR16:$p)]; }
  }
  }
}
// ── trellis session 101: `subbr` WITH A MEMORY SOURCE -- the one carry/borrow form that buys a
// codegen row. `subbr Wb,Ws,Wd` is Ws - Wb - borrow: memory MINUS register, the shape
// `subb Wb,[Ws],Wd` cannot express and the reason cc1 emits subbr only against `[wN]`. This port
// had emitted the mnemonic zero times before this def.
//
// ⛔ Defs AND Uses. MemALU declares Defs=[SR] with no Uses -- right for add/and/ior/xor, a silent
// miscompile here: without Uses=[SR] the scheduler may move this away from the `sub` that produced
// the borrow. The register form already says so (`defm SUBC : Arith<..., sube, 0, [SR]>`), and the
// Arith multiclass's own comment is "ONLY for the ops that do not READ the flags". Mutant M2 in
// steps/carry/ removes the Uses.
//
// ⚠ The addc/subb memory-source twins were implemented, MEASURED and dropped: `P_SRC_REG` already
// admits an indirect operand, so ADDC_W/SUBB_W were covered from the register form and those defs
// bought no row -- while costing stn3255 12 bytes, because `[Wn]` has no displacement and a 32-bit
// high word at +2 must have its address materialised to fold. See the header of this script.
// ⚠ The memory-DESTINATION form is absent because a glued node cannot fold both ends through this
// pattern shape (ScheduleDAGSDNodes' "Node already inserted!"), not because the machine lacks it.
let Defs = [SR], Uses = [SR], AddedComplexity = 12, mayLoad = 1 in {
  def SUBBRM16rn : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, GR16:$p), 2, "subbr\t$rb,[$p],$rd"> {
    let Pattern = [(set GR16:$rd, (sube (i16 (load GR16:$p)), GR16:$rb))]; }
  def SUBBRM8rn  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rb, GR16:$p), 2, "subbr.b\t$rb,[$p],$rd"> {
    let Pattern = [(set GR8:$rd, (sube (i8 (load GR16:$p)), GR8:$rb))]; }
}

let AddedComplexity = 12 in {
defm ADDM : MemALU<"add.w", "add.b", add>;
defm ANDM : MemALU<"and.w", "and.b", and>;
defm IORM : MemALU<"ior", "ior.b", or>;
defm XORM : MemALU<"xor", "xor.b", xor>;
}
// sub is not commutative: `sub Wb,Ws,Wd` is Wb - Ws and `subr Wb,Ws,Wd` is Ws - Wb, so the
// memory operand's side picks the mnemonic
let Defs = [SR], AddedComplexity = 12 in {
  let mayLoad = 1 in {
  def SUBM16rn  : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, GR16:$p), 2, "sub\t$rb,[$p],$rd"> {
    let Pattern = [(set GR16:$rd, (sub GR16:$rb, (i16 (load GR16:$p))))]; }
  def SUBM8rn   : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rb, GR16:$p), 2, "sub.b\t$rb,[$p],$rd"> {
    let Pattern = [(set GR8:$rd, (sub GR8:$rb, (i8 (load GR16:$p))))]; }
  def SUBRM16rn : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, GR16:$p), 2, "subr\t$rb,[$p],$rd"> {
    let Pattern = [(set GR16:$rd, (sub (i16 (load GR16:$p)), GR16:$rb))]; }
  def SUBRM8rn  : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rb, GR16:$p), 2, "subr.b\t$rb,[$p],$rd"> {
    let Pattern = [(set GR8:$rd, (sub (i8 (load GR16:$p)), GR8:$rb))]; }
  let hasExtraDefRegAllocReq = 1, Constraints = "$p = $wb" in {
  def SUBM16rpu : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, "sub\t$rb,[${p}++],$rd">;
  def SUBM8rpu  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, "sub.b\t$rb,[${p}++],$rd">;
  def SUBM16rpd : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, "sub\t$rb,[${p}--],$rd">;
  def SUBM8rpd  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, "sub.b\t$rb,[${p}--],$rd">;
  def SUBRM16rpu : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, "subr\t$rb,[${p}++],$rd">;
  def SUBRM8rpu  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, "subr.b\t$rb,[${p}++],$rd">;
  def SUBRM16rpd : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rb, GR16:$p), 2, "subr\t$rb,[${p}--],$rd">;
  def SUBRM8rpd  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR8:$rb, GR16:$p), 2, "subr.b\t$rb,[${p}--],$rd">;
  }
  }
  let mayLoad = 1, mayStore = 1 in {
  def SUBM16nrn  : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$p), 2, "sub\t$rb,[$p],[$p]"> {
    let Pattern = [(store_1u (sub GR16:$rb, (i16 (load GR16:$p))), GR16:$p)]; }
  def SUBRM16nrn : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$p), 2, "subr\t$rb,[$p],[$p]"> {
    let Pattern = [(store_1u (sub (i16 (load GR16:$p)), GR16:$rb), GR16:$p)]; }
  def SUBM8nrn   : DSPICFrameInst<(outs), (ins GR8:$rb, GR16:$p), 2, "sub.b\t$rb,[$p],[$p]"> {
    let Pattern = [(store_1u (sub GR8:$rb, (i8 (load GR16:$p))), GR16:$p)]; }
  def SUBRM8nrn  : DSPICFrameInst<(outs), (ins GR8:$rb, GR16:$p), 2, "subr.b\t$rb,[$p],[$p]"> {
    let Pattern = [(store_1u (sub (i8 (load GR16:$p)), GR8:$rb), GR16:$p)]; }
  }
  let mayStore = 1 in {
  def SUBM16rrn : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$rs, GR16:$p), 2, "sub\t$rb,$rs,[$p]"> {
    let Pattern = [(store_1u (sub GR16:$rb, GR16:$rs), GR16:$p)]; }
  def SUBM8rrn  : DSPICFrameInst<(outs), (ins GR8:$rb, GR8:$rs, GR16:$p), 2, "sub.b\t$rb,$rs,[$p]"> {
    let Pattern = [(store_1u (sub GR8:$rb, GR8:$rs), GR16:$p)]; }
  // `x - 5` reaches isel as `x + (-5)`: sub.w Wb,#5,[Wn]
  def SUBM16rin : DSPICFrameInst<(outs), (ins GR16:$rb, i16imm:$imm, GR16:$p), 2, "sub.w\t$rb,$imm,[$p]">;
  }
  // subr Wb,#lit5,Wd: lit - Wb
  def SUBR16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, i16imm:$imm), 2, "subr\t$rb,$imm,$rd"> {
    let Pattern = [(set GR16:$rd, (sub lit5imm16:$imm, GR16:$rb))]; }
  // trellis session 102: the borrow-reading twin. `-a` on 32 bits is sub(0, a), which reaches isel
  // as subc(0, lo) / sube(0, hi) -- a constant on the LEFT, which no `sub` form takes. Ours spent
  // a `clr` on a register to hold the zero; cc1 writes `subr w0,#0,w0 ; subbr w1,#0,w1`.
  let Uses = [SR] in
  def SUBBR16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rb, i16imm:$imm), 2, "subbr\t$rb,$imm,$rd"> {
    let Pattern = [(set GR16:$rd, (sube lit5imm16:$imm, GR16:$rb))]; }
  // mulw with a memory source (the destination even, as at L1f-d)
  let mayLoad = 1 in
  def MULWM16rn : DSPICFrameInst<(outs GR16_EVEN:$rd), (ins GR16:$rb, GR16:$p), 2, "mulw.ss\t$rb,[$p],$rd"> {
    let Pattern = [(set GR16_EVEN:$rd, (mul GR16:$rb, (i16 (load GR16:$p))))]; }
}
let AddedComplexity = 12 in
def : Pat<(store (add GR16:$rb, neglit5imm16:$imm), GR16:$p), (SUBM16rin GR16:$rb, (negimm16 imm:$imm), GR16:$p)>;
// trellis session 102: the borrow-PRODUCING low half of a 32-bit negate. `subr Wb,#lit5,Wd`
// already exists, but its pattern is on plain ISD::SUB, which sub(0, a) on 32 bits never reaches:
// the type legaliser splits it into subc(0, lo) / sube(0, hi) first.
def : Pat<(subc lit5imm16:$i, GR16:$x), (SUBR16ri GR16:$x, imm:$i)>;

// B. the one-operand family: register to register (untied), from memory, into memory, and
// on memory. inc/inc2/dec/dec2 outrank the literal add (the same word; cc1's spelling).
multiclass Unary16<string asm, PatFrag f, int cx = 12> {
  let Defs = [SR], AddedComplexity = cx in {
  def rr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, !strconcat(asm, "\t$rs,$rd")> {
    let Pattern = [(set GR16:$rd, (f GR16:$rs))]; }
  let mayLoad = 1 in
  def nr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, !strconcat(asm, "\t[$p],$rd")> {
    let Pattern = [(set GR16:$rd, (f (i16 (load GR16:$p))))]; }
  let mayStore = 1 in
  def rn : DSPICFrameInst<(outs), (ins GR16:$rs, GR16:$p), 2, !strconcat(asm, "\t$rs,[$p]")> {
    let Pattern = [(store_1u (f GR16:$rs), GR16:$p)]; }
  let mayLoad = 1, mayStore = 1 in
  def nn : DSPICFrameInst<(outs), (ins GR16:$p), 2, !strconcat(asm, "\t[$p],[$p]")> {
    let Pattern = [(store_1u (f (i16 (load GR16:$p))), GR16:$p)]; }
  }
}
multiclass Unary8<string asm, PatFrag f, int cx = 12> {
  let Defs = [SR], AddedComplexity = cx in {
  def rr : DSPICFrameInst<(outs GR8:$rd), (ins GR8:$rs), 2, !strconcat(asm, "\t$rs,$rd")> {
    let Pattern = [(set GR8:$rd, (f GR8:$rs))]; }
  let mayLoad = 1 in
  def nr : DSPICFrameInst<(outs GR8:$rd), (ins GR16:$p), 2, !strconcat(asm, "\t[$p],$rd")> {
    let Pattern = [(set GR8:$rd, (f (i8 (load GR16:$p))))]; }
  let mayStore = 1 in
  def rn : DSPICFrameInst<(outs), (ins GR8:$rs, GR16:$p), 2, !strconcat(asm, "\t$rs,[$p]")> {
    let Pattern = [(store_1u (f GR8:$rs), GR16:$p)]; }
  let mayLoad = 1, mayStore = 1 in
  def nn : DSPICFrameInst<(outs), (ins GR16:$p), 2, !strconcat(asm, "\t[$p],[$p]")> {
    let Pattern = [(store_1u (f (i8 (load GR16:$p))), GR16:$p)]; }
  }
}
defm INC16  : Unary16<"inc.w",  inc1f, 13>;
defm INC216 : Unary16<"inc2.w", inc2f, 13>;
defm DEC16  : Unary16<"dec.w",  dec1f, 13>;
defm DEC216 : Unary16<"dec2.w", dec2f, 13>;
defm NEG16  : Unary16<"neg",    negf>;
defm COM16  : Unary16<"com",    comf>;
defm SL116  : Unary16<"sl",     sl1f, 13>;
defm INC8   : Unary8<"inc.b",   inc1f8, 13>;
defm DEC8   : Unary8<"dec.b",   dec1f8, 13>;
defm NEG8   : Unary8<"neg.b",   negf8>;
defm COM8   : Unary8<"com.b",   comf8>;
// setm: all ones, to a register or a place; clr/setm/mov into a post-incremented place are
// the indexed-store arms (C++)
let AddedComplexity = 20 in {
  // Session 94: `setm.w` produces -1 with no inputs and no flag effect -- rematerialize it at a
  // use instead of spilling the constant (the NextChunk spill debug: -1 held in a slot across
  // eight calls). The MOV*rc constant generators carry the same two flags.
  let isReMaterializable = 1, isAsCheapAsAMove = 1 in
  def SETM16r : DSPICFrameInst<(outs GR16:$rd), (ins), 2, "setm.w\t$rd"> {
    let Pattern = [(set GR16:$rd, (i16 -1))]; }
  let mayStore = 1 in {
  def SETM16n : DSPICFrameInst<(outs), (ins GR16:$p), 2, "setm.w\t[$p]"> {
    let Pattern = [(store (i16 -1), GR16:$p)]; }
  def SETM8n  : DSPICFrameInst<(outs), (ins GR16:$p), 2, "setm.b\t[$p]"> {
    let Pattern = [(store (i8 -1), GR16:$p)]; }
  }
}
// C. extending loads, ff1l and the compares from memory
let AddedComplexity = 20 in {
  let mayLoad = 1 in {
  def SE16n : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "se\t[$p],$rd"> {
    let Pattern = [(set GR16:$rd, (sext_inreg (extloadi16i8 GR16:$p), i8))]; }
  def ZE16n : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "ze\t[$p],$rd"> {
    let Pattern = [(set GR16:$rd, (zextloadi16i8 GR16:$p))]; }
  def FF1L16n : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "ff1l\t[$p],$rd">;
  }
  // a displaced byte one add away: `add.w Wp,#d,Wt ; se [Wt],Wd` (no `se [Wn+d]` exists)
  def : Pat<(sext_inreg (extloadi16i8 (add GR16:$p, lit5imm16:$d)), i8), (SE16n (ADD16rri_lea GR16:$p, imm:$d))>;
  def : Pat<(zextloadi16i8 (add GR16:$p, lit5imm16:$d)), (ZE16n (ADD16rri_lea GR16:$p, imm:$d))>;
}

// Session 95 -- a BYTE read of a near global as ONE rematerializable instruction.
//
// dsPIC has no `mov.b _sym,Wn` for an arbitrary Wn (the file byte forms reach WREG only, which is
// why session 88 routes every byte global access through a materialised address), so a byte global
// read is `MOV16ri @sym` + `MOV8rm/ZE16n [vreg]` -- two instructions with a register between them.
// The spiller clones ONE instruction, so such a value can never be rematerialized and instead
// rides a callee-saved register across every call. In NextChunk that register is the difference
// between seven call-surviving values (cc1's, no frame) and eight (ours, a `lnk` frame).
//
// ⛔ The pair needs no scratch: `mov #_sym,wD ; ze [wD],wD` uses its own DESTINATION, so as one
// instruction it has NO register inputs and is trivially rematerializable. Same two words, same
// text. `faddr` (SelectFileAddr) supplies the `$sr`-sentinel base + symbol pair that session 94's
// isNearGlobalLoadOpcode/isIgnorableUse already understand, so the remat side gains one opcode
// entry and no new reasoning. AddedComplexity beats the 20 on the [Wn] forms above.
// isCodeGenOnly: the asm parser cannot read a two-instruction mnemonic (TableGen refuses it
// otherwise) -- MOVZX16rm8 carries the same mark for the same reason.
// session 95: formation is gated with the rematerialization it exists to enable -- measured a
// small net loss on bl_fw and a win on an isolated spill, so it rides the flag, not the default.
def HasNearByteRemat : Predicate<"DSPICEnableRematNearGlobal">;
let Predicates = [HasNearByteRemat],
    AddedComplexity = 30, mayLoad = 1, isReMaterializable = 1, isCodeGenOnly = 1 in {
// the zero-extending read: the high half MUST be zero, so this one keeps `ze` and is never
// rewritten to the byte-WREG form by the peephole.
def ZE16f : DSPICFrameInst<(outs GR16:$rd), (ins memsrc:$src), 4,
                           "mov\t#$src,$rd\n\tze\t[$rd],$rd"> {
  let Pattern = [(set GR16:$rd, (zextloadi16i8 faddr1:$src))];
}
// the plain byte read: `mov.b`, leaving the high half undefined exactly as the MOV16ri+MOV8rm
// pair it replaces did. fuseByteFile rewrites this back to the 1-word `mov.b _sym,WREG` when the
// destination is w0 -- session 90's win, which the first build of this row had displaced.
def MOV8f : DSPICFrameInst<(outs GR16:$rd), (ins memsrc:$src), 4,
                           "mov\t#$src,$rd\n\tmov.b\t[$rd],$rd">;
}
let Predicates = [HasNearByteRemat], AddedComplexity = 30 in {
  // an any-extending byte read is the plain read (the high half is don't-care)
  def : Pat<(i16 (extloadi16i8 faddr1:$src)), (MOV8f faddr1:$src)>;
  def : Pat<(i8 (load faddr1:$src)), (EXTRACT_SUBREG (MOV8f faddr1:$src), subreg_8bit)>;
}
let Defs = [SR], mayLoad = 1 in {
  def CP016n : DSPICFrameInst<(outs), (ins GR16:$p), 2, "cp0.w\t[$p]"> {
    let Pattern = [(DSPICcmp (i16 (load GR16:$p)), 0)]; }
  def CP08n  : DSPICFrameInst<(outs), (ins GR16:$p), 2, "cp0.b\t[$p]"> {
    let Pattern = [(DSPICcmp (i8 (load GR16:$p)), 0)]; }
  def CP16rn : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$p), 2, "cp\t$rb,[$p]"> {
    let Pattern = [(DSPICcmp GR16:$rb, (i16 (load GR16:$p)))]; }
  def CP8rn  : DSPICFrameInst<(outs), (ins GR8:$rb, GR16:$p), 2, "cp.b\t$rb,[$p]"> {
    let Pattern = [(DSPICcmp GR8:$rb, (i8 (load GR16:$p)))]; }
}
// D. mov: memory to memory, and the [Wn+Wb] index forms (mov's alone). The index form takes
// two REGISTERS: a constant, a frame slot or a symbol on either side stays a displacement.
def addrr : PatFrag<(ops node:$b, node:$i), (add node:$b, node:$i), [{
  for (unsigned k = 0; k < 2; ++k) {
    SDValue O = N->getOperand(k);
    if (isa<ConstantSDNode>(O) || isa<FrameIndexSDNode>(O) || O.getOpcode() == DSPICISD::Wrapper)
      return false;
  }
  return true;
}]>;
// Session 93 (item 4): the memory-to-memory move must NOT take a near file symbol as its
// destination -- `mov.w [w1+4],w2 ; mov.w w2,_LATB` (2 words) beats `add.w w1,#4,w2 ; mov #_LATB,w3 ;
// mov.w [w2],[w3]` (3) and ties the plain-pointer case, while a FAR symbol (no file form) keeps
// the two-word `mov #_far,w3 ; mov.w [w1],[w3]`. isNearFileSymbol is DSPICISelDAGToDAG.cpp's.
def store_nn : PatFrag<(ops node:$v, node:$q), (store node:$v, node:$q), [{
  return !isNearFileSymbol(cast<StoreSDNode>(N)->getBasePtr());
}]>;
let AddedComplexity = 20 in {
  let mayLoad = 1, mayStore = 1 in {
  def MOV16nn : DSPICFrameInst<(outs), (ins GR16:$p, GR16:$q), 2, "mov.w\t[$p],[$q]"> {
    let Pattern = [(store_nn (i16 (load GR16:$p)), GR16:$q)]; }
  def MOV8nn  : DSPICFrameInst<(outs), (ins GR16:$p, GR16:$q), 2, "mov.b\t[$p],[$q]"> {
    let Pattern = [(store_nn (i8 (load GR16:$p)), GR16:$q)]; }
  def MOV16xn : DSPICFrameInst<(outs), (ins GR16:$b, GR16:$i, GR16:$q), 2, "mov.w\t[${b}+${i}],[$q]"> {
    let Pattern = [(store_nn (i16 (load (addrr GR16:$b, GR16:$i))), GR16:$q)]; }
  }
  def : Pat<(i16 (load (addrr GR16:$b, GR16:$i))), (MOV16rx GR16:$b, GR16:$i)>;
  def : Pat<(i8 (load (addrr GR16:$b, GR16:$i))), (MOV8rx GR16:$b, GR16:$i)>;
  def : Pat<(zextloadi16i8 (addrr GR16:$b, GR16:$i)), (MOVZX16rx8 GR16:$b, GR16:$i)>;
  def : Pat<(store GR16:$v, (addrr GR16:$b, GR16:$i)), (MOV16xr GR16:$b, GR16:$i, GR16:$v)>;
  def : Pat<(store GR8:$v, (addrr GR16:$b, GR16:$i)), (MOV8xr GR16:$b, GR16:$i, GR8:$v)>;
}
// E. the indexed load and store forms, selected in C++ from the indexed nodes (four modes)
let mayLoad = 1, hasExtraDefRegAllocReq = 1, Constraints = "$rs = $wb" in {
  def MOV16rpd  : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.w\t[${rs}--],$rd">;
  def MOV16rpi  : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.w\t[++$rs],$rd">;
  def MOV16rpdd : DSPICFrameInst<(outs GR16:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.w\t[--$rs],$rd">;
  def MOV8rpd   : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.b\t[${rs}--],$rd">;
  def MOV8rpi   : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.b\t[++$rs],$rd">;
  def MOV8rpdd  : DSPICFrameInst<(outs GR8:$rd, GR16:$wb), (ins GR16:$rs), 2, "mov.b\t[--$rs],$rd">;
}
let mayStore = 1, hasExtraDefRegAllocReq = 1, Constraints = "$p = $wb" in {
  def MOV16pr   : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$rs, GR16:$p), 2, "mov.w\t$rs,[${p}++]">;
  def MOV16prd  : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$rs, GR16:$p), 2, "mov.w\t$rs,[${p}--]">;
  def MOV16pri  : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$rs, GR16:$p), 2, "mov.w\t$rs,[++$p]">;
  def MOV16prdd : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$rs, GR16:$p), 2, "mov.w\t$rs,[--$p]">;
  def MOV8pr    : DSPICFrameInst<(outs GR16:$wb), (ins GR8:$rs, GR16:$p), 2, "mov.b\t$rs,[${p}++]">;
  def MOV8prd   : DSPICFrameInst<(outs GR16:$wb), (ins GR8:$rs, GR16:$p), 2, "mov.b\t$rs,[${p}--]">;
  def MOV8pri   : DSPICFrameInst<(outs GR16:$wb), (ins GR8:$rs, GR16:$p), 2, "mov.b\t$rs,[++$p]">;
  def MOV8prdd  : DSPICFrameInst<(outs GR16:$wb), (ins GR8:$rs, GR16:$p), 2, "mov.b\t$rs,[--$p]">;
  def CLR16p    : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$p), 2, "clr.w\t[${p}++]">;
  def CLR8p     : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$p), 2, "clr.b\t[${p}++]">;
  def SETM16p   : DSPICFrameInst<(outs GR16:$wb), (ins GR16:$p), 2, "setm.w\t[${p}++]">;
}
// memory to memory with both pointers moving: the copy loops
let mayLoad = 1, mayStore = 1, hasExtraDefRegAllocReq = 1, Constraints = "$s = $wbs, $d = $wbd" in {
  def MOV16pp   : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.w\t[${s}++],[${d}++]">;
  def MOV8pp    : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.b\t[${s}++],[${d}++]">;
  def MOV16ppdd : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.w\t[--$s],[--$d]">;
  def MOV8ppdd  : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.b\t[--$s],[--$d]">;
  def MOV16ppd  : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.w\t[${s}--],[${d}--]">;
  def MOV8ppd   : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.b\t[${s}--],[${d}--]">;
  def MOV16ppii : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.w\t[++$s],[++$d]">;
  def MOV8ppii  : DSPICFrameInst<(outs GR16:$wbs, GR16:$wbd), (ins GR16:$s, GR16:$d), 2, "mov.b\t[++$s],[++$d]">;
}
// F. mov.d to and from memory, built by DSPICPeephole from an adjacent word pair
let isCodeGenOnly = 1 in {
  let mayLoad = 1 in
  def MOVD16nr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "mov.d\t[$p],$rd">;
  let mayStore = 1 in
  def MOVD16rn : DSPICFrameInst<(outs), (ins GR16:$rs, GR16:$p), 2, "mov.d\t$rs,[$p]">;
}
// G. the i32 shift by one, the carry pair: custom-inserter pseudos (the carry never leaves
// the pair)
let isCodeGenOnly = 1, usesCustomInserter = 1, Defs = [SR] in {
  def SHL32x1 : Pseudo<(outs GR16:$lo2, GR16:$hi2), (ins GR16:$lo, GR16:$hi), "# SHL32x1",
                       [(set GR16:$lo2, GR16:$hi2, (DSPICshl32_1 GR16:$lo, GR16:$hi))]>;
  def SRL32x1 : Pseudo<(outs GR16:$lo2, GR16:$hi2), (ins GR16:$lo, GR16:$hi), "# SRL32x1",
                       [(set GR16:$lo2, GR16:$hi2, (DSPICsrl32_1 GR16:$lo, GR16:$hi))]>;
  def SRA32x1 : Pseudo<(outs GR16:$lo2, GR16:$hi2), (ins GR16:$lo, GR16:$hi), "# SRA32x1",
                       [(set GR16:$lo2, GR16:$hi2, (DSPICsra32_1 GR16:$lo, GR16:$hi))]>;
}
// H. a rotate by 15 is the rotate by one the other way (LowerShifts turns it around; these
// are the patterns the turned node meets)
def : Pat<(ctlz_zero_poison GR16:$x), (DEC16rr (FF1L16 GR16:$x))>;
def : Pat<(cttz_zero_poison GR16:$x), (DEC16rr (FF1R16 GR16:$x))>;
// trellis session 118: the vendor builtins onto the SAME two instructions, with NO adjustment.
// ⛔ THE CONTRAST WITH THE TWO LINES ABOVE IS THE POINT: ctlz is ff1l MINUS ONE and needs the
// DEC plus a zero test the caller supplies, where the vendor's __builtin_ff1l IS the hardware
// result (DS70000157G p.257: 0 when no bit is set, else the shift count starting at 1). Routing
// the builtin through ctlz costs `cp0 ; bra z ; ff1l ; dec ; inc` -- six instructions to cc1's
// two -- which is measured, not argued.
def : Pat<(int_dspic_ff1l GR16:$rs), (FF1L16 GR16:$rs)>;
def : Pat<(int_dspic_ff1r GR16:$rs), (FF1R16 GR16:$rs)>;


//===----------------------------------------------------------------------===//
// The memcpy/memset row (trellis session 87 post-close; the atomic + writeback fix, Fable review):
// a constant-length block copy or zero fill is `repeat #cnt-1` then the single move/clr the
// hardware repeats. ⛔ The two are ONE instruction of two asm lines (Size 4), so no pass can slip
// a scratch copy BETWEEN the `repeat` and the instruction it drives (a live pointer's scratch copy
// did exactly that -- the repeat drove the copy, and one word was moved instead of cnt). ⛔ And it
// carries a tied WRITEBACK ($s=$ws, $d=$wd), modelling the pointer advance the hardware makes, so
// LLVM does not coalesce the scratch away and corrupt a pointer used after the copy. Every spelling
// was accepted by the GPL `as` (prints/l1f/probe9.log); cc1's shapes are prints/l1f/cc1/copy-Os.s.
//===----------------------------------------------------------------------===//
let isCodeGenOnly = 1, mayLoad = 1, mayStore = 1, Constraints = "$s = $ws, $d = $wd" in {
  def MEMCPYrepW : DSPICFrameInst<(outs GR16:$ws, GR16:$wd), (ins GR16:$s, GR16:$d, i16imm:$n), 4,
                                  "repeat\t$n\n\tmov.w\t[${s}++],[${d}++]">;
  def MEMCPYrepB : DSPICFrameInst<(outs GR16:$ws, GR16:$wd), (ins GR16:$s, GR16:$d, i16imm:$n), 4,
                                  "repeat\t$n\n\tmov.b\t[${s}++],[${d}++]">;
}
let isCodeGenOnly = 1, mayStore = 1, Constraints = "$d = $wd" in {
  def MEMSETrepW : DSPICFrameInst<(outs GR16:$wd), (ins GR16:$d, i16imm:$n), 4,
                                  "repeat\t$n\n\tclr.w\t[${d}++]">;
  def MEMSETrepB : DSPICFrameInst<(outs GR16:$wd), (ins GR16:$d, i16imm:$n), 4,
                                  "repeat\t$n\n\tclr.b\t[${d}++]">;
}
// the nodes EmitTargetCodeForMem* builds: a chain in and out, the pointers and a {count,byte} imm
def SDT_DSPICMemcpy : SDTypeProfile<0, 4, [SDTCisPtrTy<0>, SDTCisPtrTy<1>, SDTCisVT<2, i16>, SDTCisVT<3, i16>]>;
def SDT_DSPICMemset : SDTypeProfile<0, 3, [SDTCisPtrTy<0>, SDTCisVT<1, i16>, SDTCisVT<2, i16>]>;
def DSPICmemcpy : SDNode<"DSPICISD::MEMCPY", SDT_DSPICMemcpy, [SDNPHasChain, SDNPMayLoad, SDNPMayStore]>;
def DSPICmemsetz : SDNode<"DSPICISD::MEMSETZ", SDT_DSPICMemset, [SDNPHasChain, SDNPMayStore]>;
let isCodeGenOnly = 1, usesCustomInserter = 1 in {
  def MEMCPYinline : Pseudo<(outs), (ins GR16:$d, GR16:$s, i16imm:$cnt, i16imm:$byte), "# MEMCPY",
                            [(DSPICmemcpy GR16:$d, GR16:$s, timm:$cnt, timm:$byte)]>;
  def MEMSETZinline : Pseudo<(outs), (ins GR16:$d, i16imm:$cnt, i16imm:$byte), "# MEMSETZ",
                             [(DSPICmemsetz GR16:$d, timm:$cnt, timm:$byte)]>;
}
// ff1l from memory is 1-based too: the decrement follows (the second run: `ff1l [w0],w0` alone)
let AddedComplexity = 20 in
def : Pat<(ctlz_zero_poison (i16 (load GR16:$p))), (DEC16rr (FF1L16n GR16:$p))>;

// L1f-h's byte direct moves, landed at L1f-g (trellis session 87): `mov.b f,WREG` / `mov.b WREG,f`
// reach W0 ONLY (the 1a defect: MOV8rm printed `mov.b _f,w1`, which the assembler refuses --
// probe7). bits.c's byte SFR is read and written by plain moves at -O0, where no load folds.
// ⛔ RA fix (trellis session 88): byte FILE access goes INDIRECT (materialize the symbol into a
// register, then `[Wn]`), NOT a w0-only direct byte form. `mov.b wN,_f` assembles ONLY for WREG
// (w0), so the direct byte forms forced their operand into a 1-register class (GR8_W0/GR16_W0),
// which collides with w0's argument/return role -- "ran out of registers" on real firmware
// (StnBlUserFw, StnBlCmdHandler). Materializing uses any register and assembles; the WORD file
// forms are unaffected (`mov.w wN,_g` assembles for any register). Size: +1 word per byte file
// access (tagged) -- correctness over the byte-SFR micro-optimization.
let AddedComplexity = 5 in {
  def : Pat<(i8 (load (DSPICWrapper tglobaladdr:$g))),      (MOV8rm (MOV16ri tglobaladdr:$g), 0)>;
  def : Pat<(i8 (load (DSPICWrapper texternalsym:$g))),     (MOV8rm (MOV16ri texternalsym:$g), 0)>;
  def : Pat<(store GR8:$v, (DSPICWrapper tglobaladdr:$g)),  (MOV8mr (MOV16ri tglobaladdr:$g), 0, GR8:$v)>;
  def : Pat<(store GR8:$v, (DSPICWrapper texternalsym:$g)), (MOV8mr (MOV16ri texternalsym:$g), 0, GR8:$v)>;
  def : Pat<(zextloadi16i8 (DSPICWrapper tglobaladdr:$g)),  (MOVZX16rm8 (MOV16ri tglobaladdr:$g), 0)>;
  def : Pat<(zextloadi16i8 (DSPICWrapper texternalsym:$g)), (MOVZX16rm8 (MOV16ri texternalsym:$g), 0)>;
  def : Pat<(extloadi16i8 (DSPICWrapper tglobaladdr:$g)),   (MOVZX16rm8 (MOV16ri tglobaladdr:$g), 0)>;
  def : Pat<(extloadi16i8 (DSPICWrapper texternalsym:$g)),  (MOVZX16rm8 (MOV16ri texternalsym:$g), 0)>;
}

// Session 90: the DIRECT byte-file move forms, `mov.b _g,WREG` / `mov.b WREG,_g` (the ONLY
// byte-file move spellings the pic30 `as` accepts -- WREG literal, no wN). Formed only by the
// post-RA peephole (fuseByteFile) when the byte is already in w0, so no isel pattern and no RA
// pressure: they recover the 1-word form the session-88 materialize fix gave up, without its
// truncstore blowup. A `mov` writes no flags (no Defs=[SR]).
let isCodeGenOnly = 1 in {
  def MOV8fW : DSPICFrameInst<(outs GR8_W0:$rd), (ins memsrc:$f), 2, "mov.b\t$f,WREG"> { let mayLoad = 1; }
  def MOV8Wf : DSPICFrameInst<(outs), (ins memdst:$f, GR8_W0:$w), 2, "mov.b\tWREG,$f"> { let mayStore = 1; }
}


//===----------------------------------------------------------------------===//
// L1f-h bfins (trellis session 88, *"implement bfins"*): the bit-field STORE. DSPICbfins(BASE, V,
// k, n) = insert V's low n bits into BASE at offset k -- formed only by combineBitfieldInsert, so
// the shape is correct by construction. Three consumers: a register (Wd read+written), a file
// place, a pointer. cc1's shapes are prints/l1f/cc1/data-Os.s; every form accepted by the `as`.
//===----------------------------------------------------------------------===//
def SDT_DSPICBfins : SDTypeProfile<1, 4, [SDTCisVT<0, i16>, SDTCisVT<1, i16>, SDTCisVT<2, i16>,
                                          SDTCisVT<3, i16>, SDTCisVT<4, i16>]>;
def DSPICbfins : SDNode<"DSPICISD::BFINS", SDT_DSPICBfins>;
// trellis session 101: the literal source of BFINSL is an UNSIGNED 8-bit field of the instruction.
// The value reaching here is already `C >> k` -- the field's contents, right-justified -- so the
// test is on that, not on the original constant.
def bfinsimm8 : PatLeaf<(imm), [{
  return !N->getAPIntValue().isNegative() && N->getZExtValue() <= 255;
}]>;

let AddedComplexity = 25 in {
  def BFINS16rr : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$old, GR16:$v, i16imm:$k, i16imm:$n),
                                 4, "bfins\t$k,$n,$v,$rd"> {
    let Constraints = "$old = $rd";
    let Pattern = [(set GR16:$rd, (DSPICbfins GR16:$old, GR16:$v, timm:$k, timm:$n))];
  }
  // trellis session 101: the LITERAL-source form, BFINSL, for a register destination. The ISA
  // gives BFINSL the `P_SRC_REG` destination class -- a register or an indirect, and NOT a file
  // register -- so a near global still materialises its value first, exactly as cc1 does
  // (`mov #9,w0 ; bfins #4,#5,w0,_bk`). ⚠ AddedComplexity ABOVE the enclosing 25 so a constant
  // that fits eight bits takes this form instead of being put in a register.
  // ⚠ cc1 does NOT take this one: it keeps and/ior for `(x & ~0x1F0) | 0x90` in a register, three
  // words, where `bfins #4,#5,#9,w0` is two.
  let AddedComplexity = 30 in
  def BFINSL16ri : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$old, i16imm:$v, i16imm:$k, i16imm:$n),
                                  4, "bfins\t$k,$n,$v,$rd"> {
    let Constraints = "$old = $rd";
    let Pattern = [(set GR16:$rd, (DSPICbfins GR16:$old, bfinsimm8:$v, timm:$k, timm:$n))];
  }
  let mayLoad = 1, mayStore = 1 in {
  def BFINS16fr : DSPICFrameInst<(outs), (ins memdst:$f, GR16:$v, i16imm:$k, i16imm:$n),
                                 4, "bfins\t$k,$n,$v,$f"> {
    let Pattern = [(store (DSPICbfins (i16 (load faddr:$f)), GR16:$v, timm:$k, timm:$n), faddr:$f)];
  }
  def BFINS16nr : DSPICFrameInst<(outs), (ins GR16:$p, GR16:$v, i16imm:$k, i16imm:$n),
                                 4, "bfins\t$k,$n,$v,[$p]"> {
    let Pattern = [(store (DSPICbfins (i16 (load GR16:$p)), GR16:$v, timm:$k, timm:$n), GR16:$p)];
  }
  // trellis session 101: and with an INDIRECT destination, which is where the row pays most --
  // `p->mid = 9` is ONE word here against four for the and/ior pair.
  let AddedComplexity = 30 in
  def BFINSL16ni : DSPICFrameInst<(outs), (ins GR16:$p, i16imm:$v, i16imm:$k, i16imm:$n),
                                  4, "bfins\t$k,$n,$v,[$p]"> {
    let Pattern = [(store (DSPICbfins (i16 (load GR16:$p)), bfinsimm8:$v, timm:$k, timm:$n),
                          GR16:$p)];
  }
  }
}



//===----------------------------------------------------------------------===//
// L1f-h (trellis session 88): the DIRECT (file-register) forms. Every global is assumed NEAR
// (session 87 close item 3), so a scalar global is read, written and updated in place by its bare
// symbol -- `inc.w _g`, `add.w _g`, `cp0.w _g`, `clr.w _g` -- one word where the address-then-[Wn]
// shape was three. The `faddr` ComplexPattern (a symbol + offset, no register) selects the place;
// `memdst`/`memsrc` print it as the bare symbol (an SR base, printSrcMemOperand). The far side --
// a global past the 13-bit near window -- is a LINK-time range error, tagged, owed to L1e's near/
// far model. Every form was accepted by the GPL `as` (prints/l1f/probe10.log).
//===----------------------------------------------------------------------===//

// the shift-by-one-into-a-place fragments (sl1f is defined already; lsr/asr are not)
def lsr1ff : PatFrag<(ops node:$x), (srl node:$x, (i16 1))>;
def asr1ff : PatFrag<(ops node:$x), (sra node:$x, (i16 1))>;

// (a) the one-operand forms into the place itself (f = op f), implied WREG, no register operand
class FileU16<string asm, SDPatternOperator op>
  : DSPICFrameInst<(outs), (ins memdst:$f), 2, !strconcat(asm, "\t$f")> {
  let mayLoad = 1; let mayStore = 1; let Defs = [SR];
  let Pattern = [(store (op (i16 (load faddr:$f))), faddr:$f)];
}
class FileU8<string asm, SDPatternOperator op>
  : DSPICFrameInst<(outs), (ins memdst:$f), 2, !strconcat(asm, "\t$f")> {
  let mayLoad = 1; let mayStore = 1; let Defs = [SR];
  let Pattern = [(store (op (i8 (load faddr:$f))), faddr:$f)];
}
let AddedComplexity = 20 in {
  def INC16f  : FileU16<"inc.w",  inc1f>;
  def INC216f : FileU16<"inc2.w", inc2f>;
  def DEC16f  : FileU16<"dec.w",  dec1f>;
  def DEC216f : FileU16<"dec2.w", dec2f>;
  def COM16f  : FileU16<"com.w",  comf>;
  def SL116f  : FileU16<"sl.w",   sl1f>;
  def LSR16f  : FileU16<"lsr.w",  lsr1ff>;
  def ASR16f  : FileU16<"asr.w",  asr1ff>;
  def INC8f   : FileU8<"inc.b",   inc1f8>;
  def DEC8f   : FileU8<"dec.b",   dec1f8>;
  def COM8f   : FileU8<"com.b",   comf8>;
  // ── trellis session 101: class (a)'s missing members. ────────────────────────────────────────
  // The byte twins of the word members above, plus `neg` and the two rotates, which were absent at
  // BOTH widths. Same pattern shape, same AddedComplexity; measured against cc1 on byteops.c,
  // where `g >>= 1` cost four words to cc1's one.
  def INC28f  : FileU8<"inc2.b",  inc2f8>;
  def DEC28f  : FileU8<"dec2.b",  dec2f8>;
  def NEG16f  : FileU16<"neg.w",  negf>;
  def NEG8f   : FileU8<"neg.b",   negf8>;
  def RLNC16f : FileU16<"rlnc.w", rotl1f>;
  def RRNC16f : FileU16<"rrnc.w", rotr1f>;
  def RLNC8f  : FileU8<"rlnc.b",  rotl1f>;
  def RRNC8f  : FileU8<"rrnc.b",  rotr1f>;
  // ⛔ THE BYTE SHIFTS TAKE THE PLAIN i8 NODE, AND THE FIRST VERSION OF THIS ROW GOT THAT WRONG.
  // Reasoning from the register patterns at DSPICInstrInfo.td:1183-1185, which are written
  //     (i8 (trunc (srl (i16 (zext GR8:$x)), (i16 1))))
  // I wrote the file patterns in that widened shape, and they matched NOTHING: the defs existed,
  // TableGen accepted them, the build was clean, and `lsr.b _g` never appeared. Dumping the DAG
  // (-mllvm -debug-only=isel) settles it in one command -- i8 SRL is NOT promoted here:
  //     t7: i8 = srl t4, Constant:i16<1>
  // so the shift-by-one fragments already in this file serve both widths unchanged, the amount
  // being i16 in each. That is mutant M2: the widened shape, which assembles and is inert.
  def SL18f   : FileU8<"sl.b",    sl1f>;
  def LSR8f   : FileU8<"lsr.b",   lsr1ff>;
  def ASR8f   : FileU8<"asr.b",   asr1ff>;
  // clr / setm write a constant to the place -- no load
  let mayStore = 1 in {
  def CLR16f  : DSPICFrameInst<(outs), (ins memdst:$f), 2, "clr.w\t$f"> {
    let Pattern = [(store (i16 0), faddr:$f)]; }
  def CLR8f   : DSPICFrameInst<(outs), (ins memdst:$f), 2, "clr.b\t$f"> {
    let Pattern = [(store (i8 0), faddr:$f)]; }
  def SETM16f : DSPICFrameInst<(outs), (ins memdst:$f), 2, "setm.w\t$f"> {
    let Pattern = [(store (i16 -1), faddr:$f)]; }
  def SETM8f  : DSPICFrameInst<(outs), (ins memdst:$f), 2, "setm.b\t$f"> {
    let Pattern = [(store (i8 -1), faddr:$f)]; }
  }
}

// (b) the WREG-result one-operand forms (WREG = op f, f unchanged): the result is W0. RA fix: the
// result is wrapped OUT of GR16_W0 by COPY_TO_REGCLASS, so it is not pinned to the 1-register class
// for its whole life.
class FileToW16<string asm>
  : DSPICFrameInst<(outs GR16_W0:$rd), (ins memsrc:$f), 2, !strconcat(asm, "\t$f,WREG")> {
  let mayLoad = 1; let Defs = [SR];
}
class FileToW8<string asm>
  : DSPICFrameInst<(outs GR8_W0:$rd), (ins memsrc:$f), 2, !strconcat(asm, "\t$f,WREG")> {
  let mayLoad = 1; let Defs = [SR];
}
let AddedComplexity = 20 in {
  def INC16fW : FileToW16<"inc.w">;
  def DEC16fW : FileToW16<"dec.w">;
  def NEG16fW : FileToW16<"neg">;
  def COM16fW : FileToW16<"com">;
  def INC8fW  : FileToW8<"inc.b">;
  // trellis session 107: the eight f,WREG one-operand forms cc1 spells in one word (or that ours
  // takes though cc1 does not: com.b, sl.w). Same class (b) shape as INC8fW above; the isel
  // patterns are below with the inc1f8 one. The result is born in GR8_W0/GR16_W0 and copied out
  // at once, and session 101's shouldCoalesce guard keeps that one-register range short. Byte:
  def COM8fW  : FileToW8<"com.b">;
  def DEC8fW  : FileToW8<"dec.b">;
  def NEG8fW  : FileToW8<"neg.b">;
  def INC28fW : FileToW8<"inc2.b">;
  def DEC28fW : FileToW8<"dec2.b">;
  // Word (INC16fW/DEC16fW/NEG16fW/COM16fW are already above; these are the by-two and shift-left):
  def INC216fW : FileToW16<"inc2.w">;
  def DEC216fW : FileToW16<"dec2.w">;
  // ⚠ sl.w f,WREG: the fileshift comment (session 99) warned shl-by-one reaches a MEMORY-OPERAND
  // shift, so the FUSED SL16fWr was dead. An isel pattern for `(shl (load f), 1)` is a different
  // question -- whether it matches before the load folds -- and wreg.expected.first predicts it
  // does. If it does not, the def is dead and the expectation is corrected at its site.
  def SL16fW   : FileToW16<"sl.w">;
  // trellis session 99: the shift and rotate members of the same class. No isel pattern for the
  // same reason as the rest of class (b'); DSPICPeephole::fuseFileALU forms them after
  // allocation. ⚠ `sl.w` is absent DELIBERATELY: shl-by-one reaches a memory-operand shift, not
  // the load-then-shift shape the fusion matches, so the def would be dead. rlc/rrc are absent
  // because they rotate through the CARRY and no C expression can name an incoming carry.
  def SL8fWr   : FileToW8<"sl.b">;
  def LSR8fWr  : FileToW8<"lsr.b">;
  def ASR8fWr  : FileToW8<"asr.b">;
  def LSR16fWr : FileToW16<"lsr.w">;
  def ASR16fWr : FileToW16<"asr.w">;
  def RLNC16fWr : FileToW16<"rlnc.w">;
  def RRNC16fWr : FileToW16<"rrnc.w">;
  def RLNC8fWr  : FileToW8<"rlnc.b">;
  def RRNC8fWr  : FileToW8<"rrnc.b">;
  def : Pat<(inc1f  (i16 (load faddr:$f))), (COPY_TO_REGCLASS (INC16fW faddr:$f), GR16)>;
  def : Pat<(dec1f  (i16 (load faddr:$f))), (COPY_TO_REGCLASS (DEC16fW faddr:$f), GR16)>;
  def : Pat<(negf   (i16 (load faddr:$f))), (COPY_TO_REGCLASS (NEG16fW faddr:$f), GR16)>;
  def : Pat<(comf   (i16 (load faddr:$f))), (COPY_TO_REGCLASS (COM16fW faddr:$f), GR16)>;
  def : Pat<(inc1f8 (i8  (load faddr:$f))), (COPY_TO_REGCLASS (INC8fW  faddr:$f), GR8)>;
  // trellis session 107: the eight new forms' patterns, the same shape.
  def : Pat<(comf8  (i8  (load faddr:$f))), (COPY_TO_REGCLASS (COM8fW  faddr:$f), GR8)>;
  def : Pat<(dec1f8 (i8  (load faddr:$f))), (COPY_TO_REGCLASS (DEC8fW  faddr:$f), GR8)>;
  def : Pat<(negf8  (i8  (load faddr:$f))), (COPY_TO_REGCLASS (NEG8fW  faddr:$f), GR8)>;
  def : Pat<(inc2f8 (i8  (load faddr:$f))), (COPY_TO_REGCLASS (INC28fW faddr:$f), GR8)>;
  def : Pat<(dec2f8 (i8  (load faddr:$f))), (COPY_TO_REGCLASS (DEC28fW faddr:$f), GR8)>;
  def : Pat<(inc2f  (i16 (load faddr:$f))), (COPY_TO_REGCLASS (INC216fW faddr:$f), GR16)>;
  def : Pat<(dec2f  (i16 (load faddr:$f))), (COPY_TO_REGCLASS (DEC216fW faddr:$f), GR16)>;
  def : Pat<(sl1f   (i16 (load faddr:$f))), (COPY_TO_REGCLASS (SL16fW   faddr:$f), GR16)>;
}

// (b') the WREG-result TWO-OPERAND file forms: WREG = f op WREG, and the PLACE IS UNCHANGED.
// A third form, distinct from both of its neighbours: (b) above is one-operand (`com.w f,WREG`)
// and (c) below writes the place back (`and.w f`). This one reads the file and W0 and writes
// only W0 -- `w0 = g & w0`, an argument in w0, a near global, a result in w0.
//
// ⛔ THESE CARRY NO ISEL PATTERN ON PURPOSE. A pattern needs a GR16_W0 operand and a GR16_W0
// result, a one-register class on both sides, decided before allocation -- and the allocator
// fails outright on the first function where the operand is not already in w0 and another value
// must survive ("ran out of registers", steps/filew/filew-edit.py's header). DSPICPeephole's
// fuseFileALU forms them AFTER allocation, where "the value is in w0" is a fact to be read
// rather than a constraint to be imposed. That is fuseByteFile's argument, at session 90, for
// the byte move; this is the same trade for the ALU.
//
// ⛔ THE ORACLE IS BANKED AND cc1 IS INCONSISTENT WITH ITSELF (steps/filew/ask.cc1.{small,large}.s,
// byte-identical at both code models). cc1 takes this form for and/ior/xor at word width and for
// ior/xor/sub at byte width, and spends THREE or FOUR words on the other six:
//     cc1  `g + x`   mov #_g,w1 ; add.w w0,[w1],w0                          3 words
//     cc1  `gb & x`  mov #_gb,w1 ; mov.b [w1],w1 ; and.b w0,w1,w0           4 words
// Ours takes the whole family, so six of the eleven are one word where cc1 spends three or four.
// A departure from cc1 in our favour, stated as one.
class FileBinToW16<string asm>
  : DSPICFrameInst<(outs GR16_W0:$rd), (ins memsrc:$f, GR16_W0:$w), 2,
                   !strconcat(asm, "\t$f,WREG")> {
  let mayLoad = 1; let Defs = [SR];
}
class FileBinToW8<string asm>
  : DSPICFrameInst<(outs GR8_W0:$rd), (ins memsrc:$f, GR8_W0:$w), 2,
                   !strconcat(asm, "\t$f,WREG")> {
  let mayLoad = 1; let Defs = [SR];
}
// ⛔ sub and subr are each other's opposite here, and the fusion INVERTS them: the register form
// `sub Wb,Ws,Wd` is Wb - Ws, so `sub w0,[w1],w0` computes x - g, and the file form that computes
// x - g is `subr f,WREG` (WREG - f). Read the comment on each def, not the mnemonic.
def ADD16fWr  : FileBinToW16<"add.w">;
def AND16fWr  : FileBinToW16<"and.w">;
def IOR16fWr  : FileBinToW16<"ior.w">;
def XOR16fWr  : FileBinToW16<"xor.w">;
def SUB16fWr  : FileBinToW16<"sub.w">;    // WREG = f - WREG
def SUBR16fWr : FileBinToW16<"subr.w">;   // WREG = WREG - f
def ADD8fWr   : FileBinToW8<"add.b">;
def AND8fWr   : FileBinToW8<"and.b">;
def IOR8fWr   : FileBinToW8<"ior.b">;
def XOR8fWr   : FileBinToW8<"xor.b">;
def SUB8fWr   : FileBinToW8<"sub.b">;     // WREG = f - WREG
def SUBR8fWr  : FileBinToW8<"subr.b">;    // WREG = WREG - f

// (c) the implied-WREG two-operand forms into the place (f = f op W0). The register operand is
// forced to W0 and not printed. add/and/ior/xor commute; sub is f - W0, subr is W0 - f.
class FileBinW16<string asm>
  : DSPICFrameInst<(outs), (ins memdst:$f, GR16_W0:$w), 2, !strconcat(asm, "\t$f")> {
  let mayLoad = 1; let mayStore = 1; let Defs = [SR];
}
class FileBinW8<string asm>
  : DSPICFrameInst<(outs), (ins memdst:$f, GR8_W0:$w), 2, !strconcat(asm, "\t$f")> {
  let mayLoad = 1; let mayStore = 1; let Defs = [SR];
}
let AddedComplexity = 20 in {
  def ADD16fW  : FileBinW16<"add.w">;
  def AND16fW  : FileBinW16<"and.w">;
  def IOR16fW  : FileBinW16<"ior.w">;
  def XOR16fW  : FileBinW16<"xor.w">;
  def SUB16fW  : FileBinW16<"sub.w">;    // f - W0
  def SUBR16fW : FileBinW16<"subr.w">;   // W0 - f
  def ADD8fW   : FileBinW8<"add.b">;
  def AND8fW   : FileBinW8<"and.b">;
  def SUB8fW   : FileBinW8<"sub.b">;
  // trellis session 101: the three members class (c) had at word width and not at byte width.
  // `gb |= x` was `mov #_gb,w1 ; ior.b w0,[w1],[w1]` against cc1's one-word `ior.b _gb`.
  // ⚠ The mnemonic must be the BYTE one: `ior.w` on a byte place assembles and writes two bytes
  // past the end of the global (mutant M2 -- the assembler is a control for forms, not widths).
  def IOR8fW   : FileBinW8<"ior.b">;
  def XOR8fW   : FileBinW8<"xor.b">;
  def SUBR8fW  : FileBinW8<"subr.b">;   // W0 - f, written back to f
  // f = f op W0 -- the value wrapped into GR16_W0 by COPY_TO_REGCLASS (short-lived, RA fix)
  def : Pat<(store (add (i16 (load faddr:$f)), GR16:$w), faddr:$f), (ADD16fW  faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (and (i16 (load faddr:$f)), GR16:$w), faddr:$f), (AND16fW  faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (or  (i16 (load faddr:$f)), GR16:$w), faddr:$f), (IOR16fW  faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (xor (i16 (load faddr:$f)), GR16:$w), faddr:$f), (XOR16fW  faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (sub (i16 (load faddr:$f)), GR16:$w), faddr:$f), (SUB16fW  faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (sub GR16:$w, (i16 (load faddr:$f))), faddr:$f), (SUBR16fW faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(store (add (i8 (load faddr:$f)), GR8:$w), faddr:$f), (ADD8fW faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
  def : Pat<(store (and (i8 (load faddr:$f)), GR8:$w), faddr:$f), (AND8fW faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
  def : Pat<(store (sub (i8 (load faddr:$f)), GR8:$w), faddr:$f), (SUB8fW faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
  def : Pat<(store (or  (i8 (load faddr:$f)), GR8:$w), faddr:$f), (IOR8fW  faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
  def : Pat<(store (xor (i8 (load faddr:$f)), GR8:$w), faddr:$f), (XOR8fW  faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
  def : Pat<(store (sub GR8:$w, (i8 (load faddr:$f))), faddr:$f), (SUBR8fW faddr:$f, (COPY_TO_REGCLASS GR8:$w, GR8_W0))>;
}

// (d) the compares against a file place: cp0.w f (f vs 0), cp.w f (W0 - f). No dest.
let AddedComplexity = 20, Defs = [SR], mayLoad = 1 in {
  def CP016f : DSPICFrameInst<(outs), (ins memsrc:$f), 2, "cp0.w\t$f"> {
    let Pattern = [(DSPICcmp (i16 (load faddr:$f)), 0)]; }
  def CP08f  : DSPICFrameInst<(outs), (ins memsrc:$f), 2, "cp0.b\t$f"> {
    let Pattern = [(DSPICcmp (i8 (load faddr:$f)), 0)]; }  def CP16fW : DSPICFrameInst<(outs), (ins memsrc:$f, GR16_W0:$w), 2, "cp.w\t$f">;
  def CP8fW  : DSPICFrameInst<(outs), (ins memsrc:$f, GR8_W0:$w), 2, "cp.b\t$f">;
}

// Session 96: comparing a zero-extended byte load against zero is comparing the BYTE against zero
// -- the extension cannot change whether it is zero -- so it takes the same one-word direct form.
// Without this, testing a near `bool` global cost three instructions (materialise the address,
// `ze [Wn],Wd`, `cp0 Wd`) where cc1 spends one. `faddr` and not `faddr1`: a compare produces no
// value and needs no scratch register, so it does not care whether the address has other uses.
// The `[Wn]` variant covers the case where the address is not a file address (a far global).
let AddedComplexity = 25 in {
def : Pat<(DSPICcmp (i16 (zextloadi16i8 faddr:$f)), 0), (CP08f faddr:$f)>;
def : Pat<(DSPICcmp (i16 (zextloadi16i8 GR16:$p)), 0), (CP08n GR16:$p)>;
}
// cp.w f = f - W0 (the value wrapped into GR16_W0 by COPY_TO_REGCLASS -- short-lived, RA fix)
let AddedComplexity = 20 in {
  def : Pat<(DSPICcmp (i16 (load faddr:$f)), GR16:$w), (CP16fW faddr:$f, (COPY_TO_REGCLASS GR16:$w, GR16_W0))>;
  def : Pat<(DSPICcmp (i8  (load faddr:$f)), GR8:$w),  (CP8fW  faddr:$f, (COPY_TO_REGCLASS GR8:$w,  GR8_W0))>;
}

// === L1e prog-space (session 89): the PSV-window read of a __prog__ (addrspace 1) value ===
// cc1's sequence for `return prog_scalar` (progcmp.s):
//   mov _DSRPAG,wSave ; btsts.c wOff,#15 ; rlc wPage,wPage ; movpag wPage,DSRPAG ;
//   mov [wOff],wDst ; movpag wSave,DSRPAG
// The offset gets bit 15 set (so [wOff] hits the 0x8000 PSV window) and its old bit 15 rotates
// into the page LSB; DSRPAG selects the page and is saved/restored.
let hasSideEffects = 1 in {
  def MOVFROMDSR : DSPICFrameInst<(outs GR16:$rd), (ins), 2, "mov\t_DSRPAG,$rd">;
  def MOVPAGDSR  : DSPICFrameInst<(outs), (ins GR16:$src), 2, "movpag\t$src,DSRPAG">;
}
let mayLoad = 1 in
  def MOVINDLD16 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "mov\t[$p],$rd">;
let mayLoad = 1 in {
  def MOVINDLD8  : DSPICFrameInst<(outs GR8:$rd),  (ins GR16:$p), 2, "mov.b\t[$p],$rd">;
  // Session 90: the extending windowed byte loads. `mov.b [w],w` does NOT zero the high byte
  // on dsPIC, so an unsigned/signed byte prog read extends in the SAME instruction (the data
  // path's `ze [w],w` / `se [w],w`), never by a following op the def8 rewrite would drop.
  def MOVINDLDZ8 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "ze\t[$p],$rd">;
  def MOVINDLDS8 : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$p), 2, "se\t[$p],$rd">;
}
let Defs = [SR], Constraints = "$rs = $rd" in
  def BTSTSC16   : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs, i16imm:$k), 2, "btsts.c\t$rd,$k">;
let Uses = [SR], Defs = [SR] in
  def RLC16      : DSPICFrameInst<(outs GR16:$rd), (ins GR16:$rs), 2, "rlc\t$rs,$rd">;

let usesCustomInserter = 1, mayLoad = 1, Defs = [SR] in
  def PSVLD16 : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# PSVLD16",
                       [(set GR16:$dst, (DSPICpsvld GR16:$off, GR16:$page))]>;

// === session 110: the EDS window's own pieces ===
// DSWPAG is written with a PLAIN `mov`, not `movpag` -- cc1's asymmetry, measured, not inferred.
let hasSideEffects = 1 in {
  def MOVFROMDSW : DSPICFrameInst<(outs GR16:$rd), (ins), 2, "mov\t_DSWPAG,$rd">;
  def MOVTODSW   : DSPICFrameInst<(outs), (ins GR16:$src), 2, "mov\t$src,_DSWPAG">;
}
let mayStore = 1 in {
  def MOVINDST16 : DSPICFrameInst<(outs), (ins GR16:$rs, GR16:$p), 2, "mov\t$rs,[$p]">;
  def MOVINDST8  : DSPICFrameInst<(outs), (ins GR8:$rs, GR16:$p), 2, "mov.b\t$rs,[$p]">;
}
// ⛔ No Defs = [SR] on either pseudo: unlike the PSV read, the EDS sequence has no btsts.c/rlc and
// therefore clobbers no flags. Claiming SR would pessimise every caller for nothing.
let usesCustomInserter = 1, mayLoad = 1 in
  def EDSLD16 : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# EDSLD16",
                       [(set GR16:$dst, (DSPICedsld GR16:$off, GR16:$page))]>;
let usesCustomInserter = 1, mayStore = 1 in
  def EDSST16 : Pseudo<(outs), (ins GR16:$src, GR16:$off, GR16:$page), "# EDSST16",
                       [(DSPICedsst GR16:$src, GR16:$off, GR16:$page)]>;
// Phase B(i): the byte forms of both directions.
let usesCustomInserter = 1, mayLoad = 1 in {
  def EDSLD8  : Pseudo<(outs GR8:$dst),  (ins GR16:$off, GR16:$page), "# EDSLD8",
                       [(set GR8:$dst,  (DSPICedsld8 GR16:$off, GR16:$page))]>;
  def EDSLD8Z : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# EDSLD8Z",
                       [(set GR16:$dst, (DSPICedsld8z GR16:$off, GR16:$page))]>;
  def EDSLD8S : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# EDSLD8S",
                       [(set GR16:$dst, (DSPICedsld8s GR16:$off, GR16:$page))]>;
}
let usesCustomInserter = 1, mayStore = 1 in
  def EDSST8 : Pseudo<(outs), (ins GR8:$src, GR16:$off, GR16:$page), "# EDSST8",
                      [(DSPICedsst8 GR8:$src, GR16:$off, GR16:$page)]>;
// Session 90: the byte read (same sequence, `mov.b`).
let usesCustomInserter = 1, mayLoad = 1, Defs = [SR] in {
  def PSVLD8 : Pseudo<(outs GR8:$dst), (ins GR16:$off, GR16:$page), "# PSVLD8",
                      [(set GR8:$dst, (DSPICpsvld8 GR16:$off, GR16:$page))]>;
  def PSVLD8Z : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# PSVLD8Z",
                       [(set GR16:$dst, (DSPICpsvld8z GR16:$off, GR16:$page))]>;
  def PSVLD8S : Pseudo<(outs GR16:$dst), (ins GR16:$off, GR16:$page), "# PSVLD8S",
                       [(set GR16:$dst, (DSPICpsvld8s GR16:$off, GR16:$page))]>;
}

// L1f-k (trellis session 92): the 32-bit compare. `cp lo,lo' ; cpb hi,hi'` sets C/N/OV from the
// 32-bit difference and Z STICKY (`cpb` clears Z on a nonzero result and leaves it otherwise; the
// vendor's cc1 branches `leu` after `subb`, the same reliance) -- one word each, and no throwaway
// store where cc1 writes `[w15]`. The register and lit8 forms are the ones the assembler takes
// (steps/l1f/as-probe-11.sh: `cpb w1,#255` yes, `#256` and `#-1` no). And the flag-setting
// `ior Wb,Ws,[w15]` for a 32-bit test against zero -- the `and ...,[w15]` idiom above, on `or`.
let Defs = [SR], Uses = [SR] in {
  def CPB16rr : DSPICFrameInst<(outs), (ins GR16:$rb, GR16:$rs), 2, "cpb\t$rb,$rs"> {
    let Pattern = [(DSPICcmpb GR16:$rb, GR16:$rs)]; }
  def CPB16ri : DSPICFrameInst<(outs), (ins GR16:$rb, i16imm:$imm), 2, "cpb\t$rb,$imm"> {
    let Pattern = [(DSPICcmpb GR16:$rb, lit8imm16:$imm)]; }
}
let Defs = [SR], isCommutable = 1 in
  def IORF16rr : DSPICFrameInst<(outs), (ins GR16:$rd, GR16:$rs), 2, "ior\t$rd,$rs,[w15]"> {
    let Pattern = [(DSPICcmp (or_su GR16:$rd, GR16:$rs), 0)]; }
