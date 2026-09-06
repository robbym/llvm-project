//===-- DSPICISelLowering.cpp - DSPIC DAG Lowering Implementation  ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the DSPICTargetLowering class.
//
//===----------------------------------------------------------------------===//

#include "DSPICISelLowering.h"
#include "MCTargetDesc/DSPICMCAsmInfo.h"
#include "DSPIC.h"
#include "DSPICMachineFunctionInfo.h"
#include "DSPICSelectionDAGInfo.h"
#include "DSPICSubtarget.h"
#include "DSPICTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "dspic-lower"

static cl::opt<bool>DSPICNoLegalImmediate(
  "dspic-dspic-no-legal-immediate", cl::Hidden,
  cl::desc("Enable non legal immediates (for testing purposes only)"),
  cl::init(false));

DSPICTargetLowering::DSPICTargetLowering(const TargetMachine &TM,
                                           const DSPICSubtarget &STI)
    : TargetLowering(TM, STI) {

  // Set up the register classes.
  addRegisterClass(MVT::i8,  &DSPIC::GR8RegClass);
  addRegisterClass(MVT::i16, &DSPIC::GR16RegClass);

  // Compute derived properties from the register classes
  computeRegisterProperties(STI.getRegisterInfo());

  // Provide all sorts of operation actions
  setStackPointerRegisterToSaveRestore(DSPIC::SP);
  setBooleanContents(ZeroOrOneBooleanContent);
  setBooleanVectorContents(ZeroOrOneBooleanContent); // FIXME: Is this correct?

  // L1f-f (trellis session 87): every addressing mode dsPIC has on a load and on a store --
  // [Wn++], [Wn--], [++Wn], [--Wn] -- is an indexed node; the isel arms print them.
  for (auto AM : {ISD::POST_INC, ISD::POST_DEC, ISD::PRE_INC, ISD::PRE_DEC})
    for (MVT VT : {MVT::i8, MVT::i16}) {
      setIndexedLoadAction(AM, VT, Legal);
      setIndexedStoreAction(AM, VT, Legal);
    }

  for (MVT VT : MVT::integer_valuetypes()) {
    setLoadExtAction(ISD::EXTLOAD,  VT, MVT::i1,  Promote);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i1,  Promote);
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i1,  Promote);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i8,  Expand);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i16, Expand);
  }

  // We don't have any truncstores
  setTruncStoreAction(MVT::i16, MVT::i8, Expand);

  // L1f-c: i8 shifts stay Custom (the fold of an amount >= 8 in LowerShifts) and select
  // through the word forms by pattern (the .td); a lowering through i16 looped with the
  // combiner, and Promote does not reach a shift of a legal type.
  setOperationAction(ISD::SRA,              MVT::i8,    Custom);
  setOperationAction(ISD::SHL,              MVT::i8,    Custom);
  setOperationAction(ISD::SRL,              MVT::i8,    Custom);
  setOperationAction(ISD::SRA,              MVT::i16,   Custom);
  setOperationAction(ISD::SHL,              MVT::i16,   Custom);
  setOperationAction(ISD::SRL,              MVT::i16,   Custom);
  // L1f-c: a rotate by one is `rlnc`/`rrnc`; any other amount falls through to Expand.
  setOperationAction(ISD::ROTL,             MVT::i8,    Custom);
  setOperationAction(ISD::ROTR,             MVT::i8,    Custom);
  setOperationAction(ISD::ROTL,             MVT::i16,   Custom);
  setOperationAction(ISD::ROTR,             MVT::i16,   Custom);
  setOperationAction(ISD::GlobalAddress,    MVT::i16,   Custom);
  // L1e prog-space: an addrspace(1) global address is i32 (page:offset); LowerGlobalAddress
  // splits it into tbloffset/tblpage, so it must be Custom too (else the legalizer expands it).
  setOperationAction(ISD::GlobalAddress,    MVT::i32,   Custom);
  setOperationAction(ISD::ExternalSymbol,   MVT::i16,   Custom);
  setOperationAction(ISD::BlockAddress,     MVT::i16,   Custom);
  setOperationAction(ISD::BR_JT,            MVT::Other, Expand);
  setOperationAction(ISD::BR_CC,            MVT::i8,    Custom);
  setOperationAction(ISD::BR_CC,            MVT::i16,   Custom);
  setOperationAction(ISD::BRCOND,           MVT::Other, Expand);
  setOperationAction(ISD::SETCC,            MVT::i8,    Custom);
  setOperationAction(ISD::SETCC,            MVT::i16,   Custom);
  setOperationAction(ISD::SELECT,           MVT::i8,    Expand);
  setOperationAction(ISD::SELECT,           MVT::i16,   Expand);
  setOperationAction(ISD::SELECT_CC,        MVT::i8,    Custom);
  setOperationAction(ISD::SELECT_CC,        MVT::i16,   Custom);
  setOperationAction(ISD::SIGN_EXTEND,      MVT::i16,   Custom);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i8, Expand);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i16, Expand);
  setOperationAction(ISD::STACKSAVE,        MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE,     MVT::Other, Expand);

  setOperationAction(ISD::CTTZ,             MVT::i8,    Expand);
  setOperationAction(ISD::CTTZ,             MVT::i16,   Expand);
  setOperationAction(ISD::CTLZ,             MVT::i8,    Expand);
  setOperationAction(ISD::CTLZ,             MVT::i16,   Expand);
  // L1f-g: `ff1l`/`ff1r` are one word each (1-based, so a decrement follows); the builtins
  // are undefined at zero, which is exactly the _ZERO_POISON form (upstream's name for _ZERO_UNDEF). CTLZ/CTTZ stay Expand.
  setOperationAction(ISD::CTLZ_ZERO_POISON,  MVT::i16,   Legal);
  setOperationAction(ISD::CTTZ_ZERO_POISON,  MVT::i16,   Legal);
  setOperationAction(ISD::CTPOP,            MVT::i8,    Expand);
  setOperationAction(ISD::CTPOP,            MVT::i16,   Expand);

  setOperationAction(ISD::SHL_PARTS,        MVT::i8,    Expand);
  setOperationAction(ISD::SHL_PARTS,        MVT::i16,   Expand);
  setOperationAction(ISD::SRL_PARTS,        MVT::i8,    Expand);
  setOperationAction(ISD::SRL_PARTS,        MVT::i16,   Expand);
  setOperationAction(ISD::SRA_PARTS,        MVT::i8,    Expand);
  setOperationAction(ISD::SRA_PARTS,        MVT::i16,   Expand);

  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1,   Expand);

  // L1f-d increment 2 (trellis session 86): the glue-carry nodes are Legal, so the integer
  // type legalizer splits an i32 add/sub into add;addc / sub;subb (the .td patterns) rather
  // than a UADDO_CARRY chain that lowers to a compare diamond. The AVR recipe.
  for (MVT VT : {MVT::i8, MVT::i16}) {
    setOperationAction(ISD::ADDC, VT, Legal);
    setOperationAction(ISD::ADDE, VT, Legal);
    setOperationAction(ISD::SUBC, VT, Legal);
    setOperationAction(ISD::SUBE, VT, Legal);
  }

  // FIXME: Implement efficiently multiplication by a constant
  setOperationAction(ISD::MUL,              MVT::i8,    Promote);
  setOperationAction(ISD::MULHS,            MVT::i8,    Promote);
  setOperationAction(ISD::MULHU,            MVT::i8,    Promote);
  setOperationAction(ISD::SMUL_LOHI,        MVT::i8,    Promote);
  setOperationAction(ISD::UMUL_LOHI,        MVT::i8,    Promote);
  // L1f-d: i16 multiply is the hardware `mulw` (one word), not the MSP430 libcall.
  setOperationAction(ISD::MUL,              MVT::i16,   Legal);
  setOperationAction(ISD::MULHS,            MVT::i16,   Expand);
  setOperationAction(ISD::MULHU,            MVT::i16,   Expand);
  // L1f-d increment 3 (trellis session 86): the widening multiply is `mul.ss`/`mul.uu`
  // (a pair result), selected by a custom-inserter pseudo fixing the pair at w0:w1.
  setOperationAction(ISD::SMUL_LOHI,        MVT::i16,   Legal);
  setOperationAction(ISD::UMUL_LOHI,        MVT::i16,   Legal);

  setOperationAction(ISD::UDIV,             MVT::i8,    Promote);
  setOperationAction(ISD::UDIVREM,          MVT::i8,    Promote);
  setOperationAction(ISD::UREM,             MVT::i8,    Promote);
  setOperationAction(ISD::SDIV,             MVT::i8,    Promote);
  setOperationAction(ISD::SDIVREM,          MVT::i8,    Promote);
  setOperationAction(ISD::SREM,             MVT::i8,    Promote);
  // L1f-d increment 4 (trellis session 86): the hardware divide is `repeat`+`div.uw`/`div.sw`
  // (quotient w0, remainder w1); the four div/rem Expand so the combiner forms DIVREM.
  setOperationAction(ISD::UDIV,             MVT::i16,   Expand);
  setOperationAction(ISD::UDIVREM,          MVT::i16,   Custom);
  setOperationAction(ISD::UREM,             MVT::i16,   Expand);
  setOperationAction(ISD::SDIV,             MVT::i16,   Expand);
  setOperationAction(ISD::SDIVREM,          MVT::i16,   Custom);
  setOperationAction(ISD::SREM,             MVT::i16,   Expand);

  // varargs support
  setOperationAction(ISD::VASTART,          MVT::Other, Custom);
  setOperationAction(ISD::VAARG,            MVT::Other, Expand);
  setOperationAction(ISD::VAEND,            MVT::Other, Expand);
  setOperationAction(ISD::VACOPY,           MVT::Other, Expand);
  setOperationAction(ISD::JumpTable,        MVT::i16,   Custom);

  // L1f-f: the i32 shift-by-one pair is recovered from the `or` the type legalizer leaves
  setTargetDAGCombine(ISD::OR);
  // L1e prog-space: fold an addrspace(1) i16 read into the PSV-window node before the wide
  // (i32) program pointer reaches type legalization.
  setTargetDAGCombine(ISD::LOAD);

  setMinFunctionAlignment(Align(2));
  setPrefFunctionAlignment(Align(2));
  setMaxAtomicSizeInBitsSupported(0);
}

SDValue DSPICTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::SHL: // FALLTHROUGH
  case ISD::SRL:
  case ISD::SRA:
  case ISD::ROTL:
  case ISD::ROTR:             return LowerShifts(Op, DAG);
  case ISD::UDIVREM:
  case ISD::SDIVREM:          return LowerDivRem(Op, DAG);
  case ISD::GlobalAddress:    return LowerGlobalAddress(Op, DAG);
  case ISD::BlockAddress:     return LowerBlockAddress(Op, DAG);
  case ISD::ExternalSymbol:   return LowerExternalSymbol(Op, DAG);
  case ISD::SETCC:            return LowerSETCC(Op, DAG);
  case ISD::BR_CC:            return LowerBR_CC(Op, DAG);
  case ISD::SELECT_CC:        return LowerSELECT_CC(Op, DAG);
  case ISD::SIGN_EXTEND:      return LowerSIGN_EXTEND(Op, DAG);
  case ISD::RETURNADDR:       return LowerRETURNADDR(Op, DAG);
  case ISD::FRAMEADDR:        return LowerFRAMEADDR(Op, DAG);
  case ISD::VASTART:          return LowerVASTART(Op, DAG);
  case ISD::JumpTable:        return LowerJumpTable(Op, DAG);
  default:
    llvm_unreachable("unimplemented operand");
  }
}

// Define non profitable transforms into shifts
bool DSPICTargetLowering::shouldAvoidTransformToShift(EVT VT,
                                                       unsigned Amount) const {
  // L1f-c: every shift by a literal is one word; nothing is unprofitable.
  return false;
}

// Implemented to verify test case assertions in
// tests/codegen/dspic/shift-amount-threshold-b.ll
bool DSPICTargetLowering::isLegalICmpImmediate(int64_t Immed) const {
  if (DSPICNoLegalImmediate)
    return Immed >= -32 && Immed < 32;
  return TargetLowering::isLegalICmpImmediate(Immed);
}

//===----------------------------------------------------------------------===//
//                       DSPIC Inline Assembly Support
//===----------------------------------------------------------------------===//

/// getConstraintType - Given a constraint letter, return the type of
/// constraint it is for this target.
TargetLowering::ConstraintType
DSPICTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return C_RegisterClass;
    default:
      break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
DSPICTargetLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  if (Constraint.size() == 1) {
    // GCC Constraint Letters
    switch (Constraint[0]) {
    default: break;
    case 'r':   // GENERAL_REGS
      if (VT == MVT::i8)
        return std::make_pair(0U, &DSPIC::GR8RegClass);

      return std::make_pair(0U, &DSPIC::GR16RegClass);
    }
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

//===----------------------------------------------------------------------===//
//                      Calling Convention Implementation
//===----------------------------------------------------------------------===//

#define GET_CALLING_CONV_IMPL
#include "DSPICGenCallingConv.inc"

/// For each argument in a function store the number of pieces it is composed
/// of.
template<typename ArgT>
static void ParseFunctionArgs(const SmallVectorImpl<ArgT> &Args,
                              SmallVectorImpl<unsigned> &Out) {
  unsigned CurrentArgIndex;

  if (Args.empty())
    return;

  CurrentArgIndex = Args[0].OrigArgIndex;
  Out.push_back(0);

  for (auto &Arg : Args) {
    if (CurrentArgIndex == Arg.OrigArgIndex) {
      Out.back() += 1;
    } else {
      Out.push_back(1);
      CurrentArgIndex = Arg.OrigArgIndex;
    }
  }
}

//===----------------------------------------------------------------------===//
// The dsPIC33 calling convention (trellis L1c, session 85), MEASURED from the pic30 cc1
// (tools/dspic-llvm/prints/l1c/cc1/: abi.c, abi2.c and their -O2/-O0 .s), never designed:
//
//   * w0-w7 carry arguments. An argument of P words (P = 1, 2, 4) takes the LOWEST free run
//     of P registers whose first index is a multiple of P: an i16 or i8 the lowest free
//     register, an i32 the lowest free even pair, an i64 the lowest free quad. A hole an
//     aligned argument skipped is filled by a later smaller one (`l1(int, long, int)`:
//     w0, w2:w3, w1), and an argument that finds no run goes to the stack without closing
//     the registers to those after it (`tail_fit`: the long on the stack, the int after
//     it in w7). Little-endian parts: the low word in the even register.
//   * A byte travels in the low byte of its register or slot, UNEXTENDED: the callee
//     `se`s or `ze`s it (`bytes`: `se w1,w1 / ze w2,w2`; `call_bytes`: `mov.b #97,w1`).
//   * Stack arguments sit just below the return address, the FIRST stack argument nearest
//     it: an argument of S bytes whose predecessors on the stack total O bytes occupies
//     [entrySP-4-O-S, entrySP-4-O). The caller lays the same bytes out at [w15-N, w15), N
//     the total, the LAST stack argument at w15-N (`call_ten` pushes 10 then 9; `ten_def`
//     reads 9 at [entrySP-6] and 10 at [entrySP-8]), and pops N after the call. A word
//     slot holds a byte in its low half (`nine_bytes`: `mov.b [w15-8],w2`).
//   * Results: w0, w0:w1, w0:w3; a byte unextended in w0. A struct result goes through a
//     pointer passed in w0 as the first argument, and the callee returns that pointer in w0
//     (`fwd`: `mov.w w0,w8 ... rcall _mk3 / mov.w w8,w0`).
//
// Here a location's LocMemOffset is the REAL offset in the caller's outgoing area (0 = the
// last stack argument), so the callee's fixed object for it sits at LocMemOffset - N - 4
// from the entry w15, and the caller stores it at w15 + LocMemOffset - N.
//
// The domain of the measurement (the refuter's row, session 85): -mcpu=33CK256MP508, the
// default memory model, over i8/i16/pointer/i32/i64 scalars with at most one stack
// argument or two i16 stack arguments; all-int structs of 2, 3 and 5 words by value
// starting at w0; 4- and 6-byte struct results; no varargs, no floats, no i64 in first
// position, no struct that does not fit. Outside it the rule is this code's extrapolation,
// and the fatal errors below are where it refuses to guess. Which stack slot is the 9th
// argument is decided by `ten_to_two` (`two(i, j)`: `[w15-8]` -> w0) and `ten_def` at -O0,
// not by `ten_def` at -O2, whose add is commutative.
//
// A tail call is `bra _sym` / `goto wN` -- the frame does not grow -- iff (the compatible-
// frame rule, written before it was coded; BACKEND-PLAN.md L1c):
//   1. it is in tail position (LLVM's `musttail`, or `tail` found in position);
//   2. caller and callee use the C convention (`ccc`, `fastcc` and `tailcc` are ONE
//      convention here; `tailcc` exists because the IR verifier admits `musttail` between
//      different prototypes only under it) and neither is variadic;
//   3. no argument is passed `byval`;
//   4. the callee's stack-argument bytes do not exceed the caller's INCOMING stack-argument
//      bytes: the return address stays where the caller's caller put it, and that caller
//      pops exactly what it pushed, so the callee's arguments are written over the top of
//      the caller's own incoming area -- each slot read before it is overwritten (the DAG
//      chains every store after every argument load; the -O2 text is per slot) -- and
//      nothing below moves;
//      (no cc1 output exhibits this overwrite -- cc1 never sibcalls out of a function with
//      incoming stack arguments -- so its correctness rests on this arithmetic, the fixture's
//      hand trace and the assembler's acceptance, not on a reference);
//   5. an indirect target has a caller-saved register to live in across the epilogue's
//      pops, i.e. fewer than eight words of register arguments.
// Then: register arguments into w0-w7; stack arguments into the caller's incoming slots;
// the epilogue (`ulnk`, the pops); `bra`/`goto`. A `musttail` failing a clause is a fatal
// error naming it, never a frame.
//===----------------------------------------------------------------------===//

// The eight argument registers in w order, and the byte registers under them.
static const MCPhysReg ArgRegs16[8] = {DSPIC::R12, DSPIC::R13, DSPIC::R14,
                                       DSPIC::R15, DSPIC::R11, DSPIC::W5,
                                       DSPIC::W6,  DSPIC::W7};
static const MCPhysReg ArgRegs8[8] = {DSPIC::R12B, DSPIC::R13B, DSPIC::R14B,
                                      DSPIC::R15B, DSPIC::R11B, DSPIC::W5B,
                                      DSPIC::W6B,  DSPIC::W7B};

// The lowest free run of Parts registers aligned to Parts, or -1.
static int lowestFreeRun(unsigned Used, unsigned Parts) {
  for (unsigned I = 0; I + Parts <= 8; I += Parts) {
    unsigned Mask = ((1u << Parts) - 1) << I;
    if ((Used & Mask) == 0)
      return I;
  }
  return -1;
}

template <typename ArgT>
static void AnalyzeArguments(CCState &State,
                             SmallVectorImpl<CCValAssign> &ArgLocs,
                             const SmallVectorImpl<ArgT> &Args) {
  if (State.isVarArg())
    report_fatal_error("dspic: variadic arguments have no measured convention "
                       "(L1c; cc1 was not asked)",
                       /*gen_crash_diag=*/false);

  SmallVector<unsigned, 8> ArgsParts;
  ParseFunctionArgs(Args, ArgsParts);

  // Pass 1: registers by the lowest-free-run rule; what finds none is a stack argument.
  struct Plan {
    unsigned FirstVal, Parts;
    int Reg;           // index into ArgRegs16, or -1 for the stack
    unsigned StackOff; // real offset in the outgoing area, when Reg < 0
  };
  SmallVector<Plan, 8> Plans;
  unsigned Used = 0, ValNo = 0;
  for (unsigned Parts : ArgsParts) {
    if (Args[ValNo].Flags.isByVal())
      report_fatal_error("dspic: a byval argument has no lowering (L1c; cc1 "
                         "passes a struct by value word by word in registers)",
                         /*gen_crash_diag=*/false);
    if (Parts != 1 && Parts != 2 && Parts != 4)
      report_fatal_error("dspic: an argument of " + Twine(Parts) +
                         " words has no measured convention",
                         /*gen_crash_diag=*/false);
    int Run = lowestFreeRun(Used, Parts);
    if (Run >= 0)
      Used |= ((1u << Parts) - 1) << Run;
    Plans.push_back({ValNo, Parts, Run, 0});
    ValNo += Parts;
  }

  // Pass 2: the stack, from the LAST stack argument (real offset 0) to the first (at the
  // top, just below the return address).
  for (Plan &P : llvm::reverse(Plans))
    if (P.Reg < 0)
      P.StackOff = State.AllocateStack(2 * P.Parts, Align(2));

  // The locations, in argument order; a multi-part argument little-endian.
  for (const Plan &P : Plans) {
    for (unsigned J = 0; J < P.Parts; ++J) {
      unsigned V = P.FirstVal + J;
      MVT VT = Args[V].VT;
      if (VT != MVT::i8 && VT != MVT::i16)
        report_fatal_error("dspic: an argument part of " +
                           Twine(VT.getSizeInBits()) + " bits",
                           /*gen_crash_diag=*/false);
      if (P.Reg >= 0) {
        MCPhysReg Reg =
            VT == MVT::i8 ? ArgRegs8[P.Reg + J] : ArgRegs16[P.Reg + J];
        MCRegister Got = State.AllocateReg(Reg);
        assert(Got.id() == Reg && "the lowest-free-run rule reused a register");
        (void)Got;
        State.addLoc(CCValAssign::getReg(V, VT, Reg, VT, CCValAssign::Full));
      } else {
        State.addLoc(CCValAssign::getMem(V, VT, P.StackOff + 2 * J, VT,
                                         CCValAssign::Full));
      }
    }
  }
}

static void AnalyzeRetResult(CCState &State,
                             const SmallVectorImpl<ISD::InputArg> &Ins) {
  State.AnalyzeCallResult(Ins, RetCC_DSPIC);
}

static void AnalyzeRetResult(CCState &State,
                             const SmallVectorImpl<ISD::OutputArg> &Outs) {
  State.AnalyzeReturn(Outs, RetCC_DSPIC);
}

template<typename ArgT>
static void AnalyzeReturnValues(CCState &State,
                                SmallVectorImpl<CCValAssign> &RVLocs,
                                const SmallVectorImpl<ArgT> &Args) {
  AnalyzeRetResult(State, Args);
}

SDValue DSPICTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {

  switch (CallConv) {
  default:
    report_fatal_error("Unsupported calling convention");
  case CallingConv::C:
  case CallingConv::Fast:
  case CallingConv::Tail:
    return LowerCCCArguments(Chain, CallConv, isVarArg, Ins, dl, DAG, InVals);
  case CallingConv::MSP430_INTR:
    if (Ins.empty())
      return Chain;
    report_fatal_error("ISRs cannot have arguments");
  }
}

// Clauses 2-5 of the compatible-frame rule (clause 1, tail position, is the DAG builder's).
bool DSPICTargetLowering::isEligibleForTailCall(
    const TargetLowering::CallLoweringInfo &CLI, std::string &Why) const {
  MachineFunction &MF = CLI.DAG.getMachineFunction();
  const Function &Caller = MF.getFunction();
  const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();

  auto IsC = [](CallingConv::ID CC) {
    return CC == CallingConv::C || CC == CallingConv::Fast ||
           CC == CallingConv::Tail;
  };
  if (!IsC(CLI.CallConv) || !IsC(Caller.getCallingConv())) {
    Why = "the caller or the callee is not under the C convention";
    return false;
  }
  if (CLI.IsVarArg || Caller.isVarArg()) {
    Why = "the caller or the callee is variadic";
    return false;
  }
  for (const ISD::OutputArg &A : CLI.Outs)
    if (A.Flags.isByVal()) {
      Why = "an argument is passed byval";
      return false;
    }

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CLI.CallConv, CLI.IsVarArg, MF, ArgLocs,
                 *CLI.DAG.getContext());
  AnalyzeArguments(CCInfo, ArgLocs, CLI.Outs);
  unsigned Need = CCInfo.getStackSize();
  unsigned Have = FuncInfo->getIncomingArgBytes();
  if (Need > Have) {
    Why = ("the callee needs " + Twine(Need) +
           " bytes of stack arguments and the caller receives " + Twine(Have))
              .str();
    return false;
  }

  bool Direct = isa<GlobalAddressSDNode>(CLI.Callee) ||
                isa<ExternalSymbolSDNode>(CLI.Callee);
  if (!Direct) {
    unsigned RegWords = 0;
    for (const CCValAssign &VA : ArgLocs)
      if (VA.isRegLoc())
        ++RegWords;
    if (RegWords >= 8) {
      Why = "an indirect target with all eight argument registers in use has "
            "no caller-saved register to live in across the epilogue";
      return false;
    }
  }
  return true;
}

static std::string calleeName(SDValue Callee) {
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    return ("_" + G->getGlobal()->getName()).str();
  if (auto *E = dyn_cast<ExternalSymbolSDNode>(Callee))
    return ("_" + StringRef(E->getSymbol())).str();
  return "an indirect callee";
}

SDValue
DSPICTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG                     = CLI.DAG;
  SDLoc &dl                             = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals     = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins   = CLI.Ins;
  SDValue Chain                         = CLI.Chain;
  SDValue Callee                        = CLI.Callee;
  bool &isTailCall                      = CLI.IsTailCall;
  CallingConv::ID CallConv              = CLI.CallConv;
  bool isVarArg                         = CLI.IsVarArg;
  bool IsMustTail = CLI.CB && CLI.CB->isMustTailCall();

  switch (CallConv) {
  default:
    report_fatal_error("Unsupported calling convention");
  case CallingConv::MSP430_INTR:
    report_fatal_error("ISRs cannot be called directly");
  case CallingConv::Fast:
  case CallingConv::Tail:
  case CallingConv::C:
    break;
  }

  // The compatible-frame rule: a musttail it refuses is an error; a `tail` a plain call.
  std::string Name = calleeName(Callee);
  if (IsMustTail && !isTailCall)
    report_fatal_error("dspic: musttail call to " + Twine(Name) +
                       " cannot be honoured: the DAG builder took it out of "
                       "tail position (a demoted struct return)",
                       /*gen_crash_diag=*/false);
  if (isTailCall) {
    std::string Why;
    if (!isEligibleForTailCall(CLI, Why)) {
      if (IsMustTail)
        report_fatal_error("dspic: musttail call to " + Twine(Name) +
                           " cannot be honoured: " + Twine(Why),
                           /*gen_crash_diag=*/false);
      isTailCall = false;
    }
  }

  return LowerCCCCallTo(Chain, Callee, CallConv, isVarArg, isTailCall,
                        Outs, OutVals, Ins, dl, DAG, InVals);
}

/// LowerCCCArguments - transform physical registers into virtual registers and
/// generate load operations for arguments placed on the stack. A stack argument is a
/// fixed object BELOW the entry w15 (a growing-up stack): at LocMemOffset - N - 4.
SDValue DSPICTargetLowering::LowerCCCArguments(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  DSPICMachineFunctionInfo *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, MF, ArgLocs, *DAG.getContext());
  AnalyzeArguments(CCInfo, ArgLocs, Ins);

  unsigned N = CCInfo.getStackSize();
  FuncInfo->setIncomingArgBytes(N);

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    if (VA.isRegLoc()) {
      MVT RegVT = VA.getLocVT();
      const TargetRegisterClass *RC =
          RegVT == MVT::i8 ? &DSPIC::GR8RegClass : &DSPIC::GR16RegClass;
      Register VReg = RegInfo.createVirtualRegister(RC);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
    } else {
      assert(VA.isMemLoc());
      unsigned ObjSize = VA.getLocVT().getStoreSize();
      int FI = MFI.CreateFixedObject(
          ObjSize, (int64_t)VA.getLocMemOffset() - (int64_t)N - 4,
          /*IsImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, MVT::i16);
      InVals.push_back(DAG.getLoad(VA.getLocVT(), dl, Chain, FIN,
                                   MachinePointerInfo::getFixedStack(MF, FI)));
    }
  }

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    if (Ins[i].Flags.isSRet()) {
      Register Reg = FuncInfo->getSRetReturnReg();
      if (!Reg) {
        Reg = MF.getRegInfo().createVirtualRegister(
            getRegClassFor(MVT::i16));
        FuncInfo->setSRetReturnReg(Reg);
      }
      SDValue Copy = DAG.getCopyToReg(DAG.getEntryNode(), dl, Reg, InVals[i]);
      Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Copy, Chain);
    }
  }

  return Chain;
}

bool
DSPICTargetLowering::CanLowerReturn(CallingConv::ID CallConv,
                                     MachineFunction &MF,
                                     bool IsVarArg,
                                     const SmallVectorImpl<ISD::OutputArg> &Outs,
                                     LLVMContext &Context,
                                     const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_DSPIC);
}

SDValue
DSPICTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                  bool isVarArg,
                                  const SmallVectorImpl<ISD::OutputArg> &Outs,
                                  const SmallVectorImpl<SDValue> &OutVals,
                                  const SDLoc &dl, SelectionDAG &DAG) const {

  MachineFunction &MF = DAG.getMachineFunction();

  // CCValAssign - represent the assignment of the return value to a location
  SmallVector<CCValAssign, 16> RVLocs;

  // ISRs cannot return any value.
  if (CallConv == CallingConv::MSP430_INTR && !Outs.empty())
    report_fatal_error("ISRs cannot return any value");

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analize return values.
  AnalyzeReturnValues(CCInfo, RVLocs, Outs);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");

    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(),
                             OutVals[i], Glue);

    // Guarantee that all emitted copies are stuck together,
    // avoiding something bad.
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  if (MF.getFunction().hasStructRetAttr()) {
    DSPICMachineFunctionInfo *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
    Register Reg = FuncInfo->getSRetReturnReg();

    if (!Reg)
      llvm_unreachable("sret virtual register not created in entry block");

    MVT PtrVT = getFrameIndexTy(DAG.getDataLayout());
    SDValue Val =
      DAG.getCopyFromReg(Chain, dl, Reg, PtrVT);
    unsigned R12 = DSPIC::R12;

    Chain = DAG.getCopyToReg(Chain, dl, R12, Val, Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(R12, PtrVT));
  }

  // L1d (trellis session 88): the C `interrupt` attribute sets the fn-attribute, not the
  // MSP430_INTR calling convention, so the return kind keys off the attribute too.
  bool IsInterrupt = CallConv == CallingConv::MSP430_INTR ||
                     MF.getFunction().hasFnAttribute("interrupt");
  unsigned Opc = (IsInterrupt ? DSPICISD::RETI_GLUE : DSPICISD::RET_GLUE);

  RetOps[0] = Chain;  // Update chain.

  // Add the glue if we have it.
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(Opc, dl, MVT::Other, RetOps);
}

/// LowerCCCCallTo - the arguments go to their registers and to the outgoing area at
/// [w15-N, w15) (a tail call: to this function's own incoming slots); CALLSEQ_START/END
/// bracket a plain call; a tail call ends the function with TC_RETURN.
SDValue DSPICTargetLowering::LowerCCCCallTo(
    SDValue Chain, SDValue Callee, CallingConv::ID CallConv, bool isVarArg,
    bool isTailCall, const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, MF, ArgLocs, *DAG.getContext());
  AnalyzeArguments(CCInfo, ArgLocs, Outs);

  unsigned NumBytes = CCInfo.getStackSize();
  MVT PtrVT = getFrameIndexTy(DAG.getDataLayout());

  if (!isTailCall)
    Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, dl);
  // L1f-a: a plain call PUSHES its stack arguments, the last stack argument first (real
  // offset 0 is the bottom), and pops them with `sub.w #N,w15` after the call; the
  // pushes are chained in order. A tail call still writes its caller's incoming slots.
  SmallVector<std::pair<int64_t, SDValue>, 8> Pushes;
  // A tail call writes over this function's incoming slots: every load from them first.
  SDValue StoreChain = (isTailCall && NumBytes)
                           ? DAG.getStackArgumentTokenFactor(Chain)
                           : Chain;

  SmallVector<std::pair<unsigned, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 12> MemOpChains;
  SDValue StackPtr;

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[i];
    assert(VA.getLocInfo() == CCValAssign::Full && "nothing is promoted here");
    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      continue;
    }
    assert(VA.isMemLoc());
    assert(!Outs[i].Flags.isByVal() && "refused by AnalyzeArguments");
    int64_t Real = VA.getLocMemOffset();
    SDValue MemOp;
    if (isTailCall) {
      int FI = MFI.CreateFixedObject(VA.getLocVT().getStoreSize(),
                                     Real - (int64_t)NumBytes - 4,
                                     /*IsImmutable=*/false);
      MemOp = DAG.getStore(StoreChain, dl, Arg, DAG.getFrameIndex(FI, PtrVT),
                           MachinePointerInfo::getFixedStack(MF, FI));
    } else {
      SDValue Word = Arg;
      if (Word.getValueType() == MVT::i8) // a byte rides in the low half of a word
        Word = DAG.getNode(ISD::ANY_EXTEND, dl, MVT::i16, Word);
      Pushes.push_back({Real, Word});
      continue;
    }
    MemOpChains.push_back(MemOp);
  }
  (void)StackPtr;
  llvm::sort(Pushes, [](const auto &A, const auto &B) { return A.first < B.first; });
  for (const auto &[Off, Word] : Pushes)
    Chain = DAG.getNode(DSPICISD::PUSH, dl, MVT::Other, Chain, Word);

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);

  SDValue InGlue;
  for (const auto &[Reg, N] : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, dl, Reg, N, InGlue);
    InGlue = Chain.getValue(1);
  }

  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, MVT::i16);
  else if (ExternalSymbolSDNode *E = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(E->getSymbol(), MVT::i16);

  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  for (const auto &[Reg, N] : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg, N.getValueType()));
  if (InGlue.getNode())
    Ops.push_back(InGlue);

  if (isTailCall) {
    // Nothing returns here: the callee returns to this function's caller.
    MFI.setHasTailCall();
    return DAG.getNode(DSPICISD::TC_RETURN, dl, MVT::Other, Ops);
  }

  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  // rcall (one word, PC-relative) by default, as cc1's small-code model; `call` under
  // +large-code. An indirect call is `call wN` either way.
  bool Direct = isa<GlobalAddressSDNode>(Callee) || isa<ExternalSymbolSDNode>(Callee);
  bool Large = MF.getSubtarget<DSPICSubtarget>().isLargeCode();
  Chain = DAG.getNode(Direct && !Large ? DSPICISD::RCALL : DSPICISD::CALL, dl, NodeTys, Ops);
  InGlue = Chain.getValue(1);
  Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, InGlue, dl);
  InGlue = Chain.getValue(1);
  return LowerCallResult(Chain, InGlue, CallConv, isVarArg, Ins, dl, DAG,
                         InVals);
}

/// LowerCallResult - Lower the result values of a call into the
/// appropriate copies out of appropriate physical registers.
///
SDValue DSPICTargetLowering::LowerCallResult(
    SDValue Chain, SDValue InGlue, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  AnalyzeReturnValues(CCInfo, RVLocs, Ins);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    Chain = DAG.getCopyFromReg(Chain, dl, RVLocs[i].getLocReg(),
                               RVLocs[i].getValVT(), InGlue).getValue(1);
    InGlue = Chain.getValue(2);
    InVals.push_back(Chain.getValue(0));
  }

  return Chain;
}

// L1f-d increment 4: UDIVREM/SDIVREM -> the target 2-result node (selected to the divide
// pseudo). A lone div or rem reached here through the combiner's DIVREM formation.
SDValue DSPICTargetLowering::LowerDivRem(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  unsigned N = Op.getOpcode() == ISD::SDIVREM ? DSPICISD::SDIVREM : DSPICISD::UDIVREM;
  SDVTList VTs = DAG.getVTList(MVT::i16, MVT::i16);
  SDValue R = DAG.getNode(N, dl, VTs, Op.getOperand(0), Op.getOperand(1));
  return R;
}



// L1f-f (trellis session 87): an i32 shift by ONE arrives from the type legalizer already
// expanded (SHL_PARTS is never asked for a constant amount):
//   x >> 1: hi' = srl hi,1 ; lo' = (srl lo,1) | (shl hi,15)      (sra for a signed x)
//   x << 1: lo' = shl lo,1 ; hi' = (shl hi,1) | (srl lo,15)
// The `or` is the signature. It becomes one result of the carry pair's pseudo (lsr/asr hi ;
// rrc lo -- add lo,lo ; addc hi,hi) and the other half's shift, found among that half's uses,
// takes the other result. Two words for the expansion's four; cc1's own shapes.

// L1f-h bfins (trellis session 88): recognize a bit-field insert `or` and form DSPICbfins. A
// wrong bfins silently corrupts (session 87's lesson), so the validation is exhaustive and the
// node is produced ONLY for an `or` that is exactly (BASE & ~field) | ((V<<k) & field).
static SDValue combineBitfieldInsert(SDNode *N, SelectionDAG &DAG) {
  if (N->getValueType(0) != MVT::i16)
    return SDValue();
  auto tryKeep = [&](SDValue A, SDValue B) -> SDValue {
    if (A.getOpcode() != ISD::AND)
      return SDValue();
    auto *KC = dyn_cast<ConstantSDNode>(A.getOperand(1));
    if (!KC)
      return SDValue();
    uint16_t keep = (uint16_t)KC->getZExtValue();
    uint16_t field = (uint16_t)~keep;
    if (field == 0 || !isShiftedMask_32(field))
      return SDValue();
    unsigned k = llvm::countr_zero((uint32_t)field);
    unsigned n = llvm::popcount((uint32_t)field);
    if (n < 1 || n >= 16)
      return SDValue();
    SDValue BASE = A.getOperand(0);
    SDValue V;
    if (B.getOpcode() == ISD::AND) {
      auto *FC = dyn_cast<ConstantSDNode>(B.getOperand(1));
      if (!FC || (uint16_t)FC->getZExtValue() != field)
        return SDValue();
      SDValue inner = B.getOperand(0);
      if (k == 0) {
        V = inner;
      } else {
        if (inner.getOpcode() != ISD::SHL)
          return SDValue();
        auto *SC = dyn_cast<ConstantSDNode>(inner.getOperand(1));
        if (!SC || SC->getZExtValue() != k)
          return SDValue();
        V = inner.getOperand(0);
      }
    } else if (B.getOpcode() == ISD::SHL) {
      // an unmasked shift is a bit-field insert only when the field reaches bit 15
      if (k + n != 16)
        return SDValue();
      auto *SC = dyn_cast<ConstantSDNode>(B.getOperand(1));
      if (!SC || SC->getZExtValue() != k)
        return SDValue();
      V = B.getOperand(0);
    } else {
      return SDValue();
    }
    SDLoc dl(N);
    return DAG.getNode(DSPICISD::BFINS, dl, MVT::i16, BASE, V,
                       DAG.getTargetConstant(k, dl, MVT::i16),
                       DAG.getTargetConstant(n, dl, MVT::i16));
  };
  if (SDValue R = tryKeep(N->getOperand(0), N->getOperand(1)))
    return R;
  return tryKeep(N->getOperand(1), N->getOperand(0));
}

static SDValue combineOrIntoShift32(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  SelectionDAG &DAG = DCI.DAG;
  if (N->getValueType(0) != MVT::i16)
    return SDValue();
  auto shiftBy = [](SDValue V, unsigned Opc, unsigned K) -> SDValue {
    if (V.getOpcode() != Opc || !V.hasOneUse())
      return SDValue();
    auto *C = dyn_cast<ConstantSDNode>(V.getOperand(1));
    return (C && C->getZExtValue() == K) ? V.getOperand(0) : SDValue();
  };
  auto shiftUser = [](SDValue V, unsigned K, bool Left) -> SDNode * {
    for (SDNode *U : V.getNode()->users()) {
      bool Ok = Left ? U->getOpcode() == ISD::SHL
                     : (U->getOpcode() == ISD::SRL || U->getOpcode() == ISD::SRA);
      if (!Ok || U->getValueType(0) != MVT::i16 || U->getOperand(0) != V)
        continue;
      auto *C = dyn_cast<ConstantSDNode>(U->getOperand(1));
      if (C && C->getZExtValue() == K)
        return U;
    }
    return nullptr;
  };
  SDLoc dl(N);
  SDVTList VTs = DAG.getVTList(MVT::i16, MVT::i16);
  for (int Swap = 0; Swap < 2; ++Swap) {
    SDValue X = N->getOperand(Swap), Y = N->getOperand(1 - Swap);
    // right by one
    if (SDValue Lo = shiftBy(X, ISD::SRL, 1)) {
      if (SDValue Hi = shiftBy(Y, ISD::SHL, 15)) {
        if (SDNode *U = shiftUser(Hi, 1, false)) {
          unsigned Opc = U->getOpcode() == ISD::SRA ? DSPICISD::SRA32_1 : DSPICISD::SRL32_1;
          SDValue R = DAG.getNode(Opc, dl, VTs, Lo, Hi);
          DCI.CombineTo(U, R.getValue(1));
          return R.getValue(0);
        }
      }
    }
    // left by one
    if (SDValue Hi = shiftBy(X, ISD::SHL, 1)) {
      if (SDValue Lo = shiftBy(Y, ISD::SRL, 15)) {
        if (SDNode *U = shiftUser(Lo, 1, true)) {
          SDValue R = DAG.getNode(DSPICISD::SHL32_1, dl, VTs, Lo, Hi);
          DCI.CombineTo(U, R.getValue(0));
          return R.getValue(1);
        }
      }
    }
  }
  return SDValue();
}

// combineProgLoad handles i16 and i32 (session 89): fold an addrspace(1) read into PSV-window
// word reads. Each word is an independent PSVLD (offset in i16, page in i16); the i32 read does
// two, at the address and address+2 (the +2 in i32 so it carries into the page).
static SDValue psvWord(SelectionDAG &DAG, const SDLoc &dl, SDValue Chain, SDValue Addr32,
                       MachineMemOperand *MMO) {
  SDValue Off  = DAG.getNode(ISD::TRUNCATE, dl, MVT::i16, Addr32);
  SDValue Hi   = DAG.getNode(ISD::SRL, dl, MVT::i32, Addr32, DAG.getConstant(16, dl, MVT::i32));
  SDValue Page = DAG.getNode(ISD::TRUNCATE, dl, MVT::i16, Hi);
  SDValue Ops[] = { Chain, Off, Page };
  return DAG.getMemIntrinsicNode(DSPICISD::PSVLD, dl,
             DAG.getVTList(MVT::i16, MVT::Other), Ops, MVT::i16, MMO);
}

// Session 90: the byte read (PSVLD8 raw, PSVLD8Z zero-extended, PSVLD8S sign-extended -- the
// extension is in the windowed instruction because `mov.b [w],w` does not zero the high byte).
static SDValue psvByte(SelectionDAG &DAG, const SDLoc &dl, SDValue Chain, SDValue Addr32,
                       MachineMemOperand *MMO, unsigned Node, EVT ResVT) {
  SDValue Off  = DAG.getNode(ISD::TRUNCATE, dl, MVT::i16, Addr32);
  SDValue Hi   = DAG.getNode(ISD::SRL, dl, MVT::i32, Addr32, DAG.getConstant(16, dl, MVT::i32));
  SDValue Page = DAG.getNode(ISD::TRUNCATE, dl, MVT::i16, Hi);
  SDValue Ops[] = { Chain, Off, Page };
  return DAG.getMemIntrinsicNode(Node, dl, DAG.getVTList(ResVT, MVT::Other), Ops, MVT::i8, MMO);
}

static SDValue combineProgLoad(SDNode *N, TargetLowering::DAGCombinerInfo &DCI) {
  LoadSDNode *LD = cast<LoadSDNode>(N);
  if (LD->getAddressSpace() != 1)
    return SDValue();
  EVT VT = LD->getMemoryVT();
  ISD::LoadExtType Ext = LD->getExtensionType();
  SelectionDAG &DAG = DCI.DAG;
  SDLoc dl(N);
  SDValue Ptr = LD->getBasePtr();  // i32 program address: page:offset

  // Session 90: a BYTE read. `mov.b [w],w` does NOT zero the high byte on dsPIC, so an
  // extending load must extend IN the windowed read (`ze`/`se`), never via a following op the
  // def8 -> SUBREG_TO_REG rewrite would drop. NON_EXTLOAD (a `char` used as a byte) keeps the
  // raw `mov.b`; the consumer truncates.
  if (VT == MVT::i8) {
    unsigned Node = Ext == ISD::ZEXTLOAD ? DSPICISD::PSVLD8Z
                  : Ext == ISD::SEXTLOAD ? DSPICISD::PSVLD8S : DSPICISD::PSVLD8;
    EVT ResVT = Ext == ISD::NON_EXTLOAD ? MVT::i8 : MVT::i16;
    SDValue B = psvByte(DAG, dl, LD->getChain(), Ptr, LD->getMemOperand(), Node, ResVT);
    SDValue Val = B;
    if (Ext != ISD::NON_EXTLOAD && LD->getValueType(0) != MVT::i16)
      Val = DAG.getNode(ISD::ANY_EXTEND, dl, LD->getValueType(0), B);
    DCI.CombineTo(N, Val, B.getValue(1));
    return SDValue(N, 0);
  }
  if (Ext != ISD::NON_EXTLOAD)
    return SDValue();

  if (VT == MVT::i16) {
    SDValue W = psvWord(DAG, dl, LD->getChain(), Ptr, LD->getMemOperand());
    DCI.CombineTo(N, W, W.getValue(1));
    return SDValue(N, 0);
  }
  if (VT == MVT::i32) {
    SDValue Lo = psvWord(DAG, dl, LD->getChain(), Ptr, LD->getMemOperand());
    SDValue Ptr2 = DAG.getNode(ISD::ADD, dl, MVT::i32, Ptr, DAG.getConstant(2, dl, MVT::i32));
    SDValue Hi = psvWord(DAG, dl, Lo.getValue(1), Ptr2, LD->getMemOperand());
    SDValue Val = DAG.getNode(ISD::BUILD_PAIR, dl, MVT::i32, Lo, Hi);
    DCI.CombineTo(N, Val, Hi.getValue(1));
    return SDValue(N, 0);
  }
  // Wider scalars (i64, float) never reach here from the firmware corpus; an aggregate read is
  // a memcpy and goes to EmitTargetCodeForMemcpy (session 90: the `__memcpy_helper` libcall).
  return SDValue();
}

// Session 90: a block copy touching PROGRAM memory (addrspace 1) is never expanded into loads
// and stores -- those would be data reads of a program address, which the pre-session-90
// backend crashed on ("Do not know how to expand this operator's operand"). Declining sends
// it to EmitTargetCodeForMemcpy, which emits cc1's `__memcpy_helper` libcall.
bool DSPICTargetLowering::findOptimalMemOpLowering(
    LLVMContext &Context, std::vector<EVT> &MemOps, unsigned Limit, const MemOp &Op,
    unsigned DstAS, unsigned SrcAS, const AttributeList &FuncAttributes, EVT *LargestVT) const {
  if (SrcAS == 1 || DstAS == 1)
    return false;
  return TargetLowering::findOptimalMemOpLowering(Context, MemOps, Limit, Op, DstAS, SrcAS,
                                                  FuncAttributes, LargestVT);
}

// Session 90: an inline-asm "i"/"s" operand naming a PROGRAM-memory global (addrspace 1, an
// i32 address) stays SYMBOLIC -- an i16 target address the printer renders as the bare symbol
// (`%c1`), so `mov #tblpage(%c1),%0` reaches the assembler with the symbol and the LINKER
// computes the page/offset (binutils include/elf/pic30.h). The default would hand the type
// legalizer an i32 TargetGlobalAddress it cannot expand (session 89's crash, the reason the
// shim was pure casts). Constant offsets fold as in the default ((GA+C), (GA-C), ...).
void DSPICTargetLowering::LowerAsmOperandForConstraint(SDValue Op, StringRef Constraint,
                                                        std::vector<SDValue> &Ops,
                                                        SelectionDAG &DAG) const {
  if (Constraint.size() == 1 && (Constraint[0] == 'i' || Constraint[0] == 's' ||
                                 Constraint[0] == 'X') && Op.getValueType() == MVT::i32) {
    int64_t Offset = 0;
    SDValue V = Op;
    while (true) {
      if (auto *GA = dyn_cast<GlobalAddressSDNode>(V)) {
        Ops.push_back(DAG.getTargetGlobalAddress(GA->getGlobal(), SDLoc(Op), MVT::i16,
                                                 Offset + GA->getOffset()));
        return;
      }
      unsigned Opc = V.getOpcode();
      if (Opc != ISD::ADD && Opc != ISD::SUB)
        break;
      ConstantSDNode *C;
      if ((C = dyn_cast<ConstantSDNode>(V.getOperand(0))))
        V = V.getOperand(1);
      else if (Opc == ISD::ADD && (C = dyn_cast<ConstantSDNode>(V.getOperand(1))))
        V = V.getOperand(0);
      else
        break;
      Offset += (Opc == ISD::ADD ? 1 : -1) * C->getSExtValue();
    }
  }
  TargetLowering::LowerAsmOperandForConstraint(Op, Constraint, Ops, DAG);
}

SDValue DSPICTargetLowering::PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const {
  if (N->getOpcode() == ISD::LOAD)
    return combineProgLoad(N, DCI);
  if (N->getOpcode() == ISD::OR) {
    if (SDValue R = combineBitfieldInsert(N, DCI.DAG))
      return R;
    return combineOrIntoShift32(N, DCI);
  }
  return SDValue();
}

SDValue DSPICTargetLowering::LowerShifts(SDValue Op,
                                          SelectionDAG &DAG) const {
  // L1f-c (trellis session 86). A shift is one word for any amount in a register or in
  // 1..15 (`sl Wb,#lit4,Wd`, `sl Wb,Wns,Wnd`, `sl Ws,Wd`; a byte through the word forms by
  // pattern); an amount >= the width is folded (zero, or the sign for sra). A rotate by
  // one is legal and any other rotate returns nothing, falling through to Expand.
  unsigned Opc = Op.getOpcode();
  SDNode *N = Op.getNode();
  EVT VT = Op.getValueType();
  SDLoc dl(N);
  SDValue Val = N->getOperand(0);
  SDValue Amt = N->getOperand(1);
  auto *CAmt = dyn_cast<ConstantSDNode>(Amt);

  // L1f-f: a rotate by width-1 is the rotate by one the other way (the combiner spells a
  // rotate right by one as `rotl x, 15`; L1f-c's ror1 was three words for that reason)
  if (Opc == ISD::ROTL || Opc == ISD::ROTR) {
    if (!CAmt)
      return SDValue();
    uint64_t K = CAmt->getZExtValue();
    if (K == 1)
      return Op;
    if (K == VT.getSizeInBits() - 1)
      return DAG.getNode(Opc == ISD::ROTL ? ISD::ROTR : ISD::ROTL, dl, VT, Val,
                         DAG.getConstant(1, dl, MVT::i16));
    return SDValue();
  }

  if (CAmt) {
    uint64_t K = CAmt->getZExtValue();
    unsigned Bits = VT.getSizeInBits();
    if (K == 0)
      return Val;
    if (K >= Bits)
      return Opc == ISD::SRA
                 ? DAG.getNode(ISD::SRA, dl, VT, Val,
                               DAG.getConstant(Bits - 1, dl, MVT::i16))
                 : DAG.getConstant(0, dl, VT);
    return DAG.getNode(Opc, dl, VT, Val, DAG.getConstant(K, dl, MVT::i16));
  }
  return Op;
}

SDValue DSPICTargetLowering::LowerGlobalAddress(SDValue Op,
                                                 SelectionDAG &DAG) const {
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  int64_t Offset = cast<GlobalAddressSDNode>(Op)->getOffset();
  EVT PtrVT = Op.getValueType();

  // L1e prog-space (session 89): a program-memory global (addrspace 1) has a 24-bit address held
  // page:offset in 4 bytes -- `mov #tbloffset(sym),wLo ; mov #tblpage(sym),wHi`, cc1's shape.
  if (GV->getAddressSpace() == 1) {
    SDLoc dl(Op);
    SDValue Lo = DAG.getTargetGlobalAddress(GV, dl, MVT::i16, Offset, DSPICII::MO_TBLOFFSET);
    SDValue Hi = DAG.getTargetGlobalAddress(GV, dl, MVT::i16, Offset, DSPICII::MO_TBLPAGE);
    Lo = DAG.getNode(DSPICISD::Wrapper, dl, MVT::i16, Lo);
    Hi = DAG.getNode(DSPICISD::Wrapper, dl, MVT::i16, Hi);
    return DAG.getNode(ISD::BUILD_PAIR, dl, MVT::i32, Lo, Hi);
  }

  // Create the TargetGlobalAddress node, folding in the constant offset.
  SDValue Result = DAG.getTargetGlobalAddress(GV, SDLoc(Op), PtrVT, Offset);
  return DAG.getNode(DSPICISD::Wrapper, SDLoc(Op), PtrVT, Result);
}

void DSPICTargetLowering::ReplaceNodeResults(SDNode *N,
                                             SmallVectorImpl<SDValue> &Results,
                                             SelectionDAG &DAG) const {
  // L1e prog-space (session 89): the address of a program-memory (addrspace 1) global is an
  // illegal i32 held page:offset. LowerGlobalAddress builds it as tbloffset:tblpage; hand the
  // i32 back so the integer legalizer expands the BUILD_PAIR into the two i16 halves.
  switch (N->getOpcode()) {
  case ISD::GlobalAddress:
    Results.push_back(LowerGlobalAddress(SDValue(N, 0), DAG));
    break;
  default:
    llvm_unreachable("Do not know how to custom expand this result");
  }
}

SDValue DSPICTargetLowering::LowerExternalSymbol(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDLoc dl(Op);
  const char *Sym = cast<ExternalSymbolSDNode>(Op)->getSymbol();
  EVT PtrVT = Op.getValueType();
  SDValue Result = DAG.getTargetExternalSymbol(Sym, PtrVT);

  return DAG.getNode(DSPICISD::Wrapper, dl, PtrVT, Result);
}

SDValue DSPICTargetLowering::LowerBlockAddress(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc dl(Op);
  const BlockAddress *BA = cast<BlockAddressSDNode>(Op)->getBlockAddress();
  EVT PtrVT = Op.getValueType();
  SDValue Result = DAG.getTargetBlockAddress(BA, PtrVT);

  return DAG.getNode(DSPICISD::Wrapper, dl, PtrVT, Result);
}

static SDValue EmitCMP(SDValue &LHS, SDValue &RHS, SDValue &TargetCC,
                       ISD::CondCode CC, const SDLoc &dl, SelectionDAG &DAG) {
  // FIXME: Handle bittests someday
  assert(!LHS.getValueType().isFloatingPoint() && "We don't handle FP yet");

  // FIXME: Handle jump negative someday
  DSPICCC::CondCodes TCC = DSPICCC::COND_INVALID;
  switch (CC) {
  default: llvm_unreachable("Invalid integer condition!");
  case ISD::SETEQ:
    TCC = DSPICCC::COND_E;     // aka COND_Z
    // Minor optimization: if LHS is a constant, swap operands, then the
    // constant can be folded into comparison.
    if (LHS.getOpcode() == ISD::Constant)
      std::swap(LHS, RHS);
    break;
  case ISD::SETNE:
    TCC = DSPICCC::COND_NE;    // aka COND_NZ
    // Minor optimization: if LHS is a constant, swap operands, then the
    // constant can be folded into comparison.
    if (LHS.getOpcode() == ISD::Constant)
      std::swap(LHS, RHS);
    break;
  case ISD::SETULE:
    std::swap(LHS, RHS);
    [[fallthrough]];
  case ISD::SETUGE:
    // Turn lhs u>= rhs with lhs constant into rhs u< lhs+1, this allows us to
    // fold constant into instruction.
    if (const ConstantSDNode * C = dyn_cast<ConstantSDNode>(LHS)) {
      LHS = RHS;
      RHS =
          DAG.getSignedConstant(C->getSExtValue() + 1, dl, C->getValueType(0));
      TCC = DSPICCC::COND_LO;
      break;
    }
    TCC = DSPICCC::COND_HS;    // aka COND_C
    break;
  case ISD::SETUGT:
    std::swap(LHS, RHS);
    [[fallthrough]];
  case ISD::SETULT:
    // Turn lhs u< rhs with lhs constant into rhs u>= lhs+1, this allows us to
    // fold constant into instruction.
    if (const ConstantSDNode * C = dyn_cast<ConstantSDNode>(LHS)) {
      LHS = RHS;
      RHS =
          DAG.getSignedConstant(C->getSExtValue() + 1, dl, C->getValueType(0));
      TCC = DSPICCC::COND_HS;
      break;
    }
    TCC = DSPICCC::COND_LO;    // aka COND_NC
    break;
  case ISD::SETLE:
    std::swap(LHS, RHS);
    [[fallthrough]];
  case ISD::SETGE:
    // Turn lhs >= rhs with lhs constant into rhs < lhs+1, this allows us to
    // fold constant into instruction.
    if (const ConstantSDNode * C = dyn_cast<ConstantSDNode>(LHS)) {
      LHS = RHS;
      RHS =
          DAG.getSignedConstant(C->getSExtValue() + 1, dl, C->getValueType(0));
      TCC = DSPICCC::COND_L;
      break;
    }
    TCC = DSPICCC::COND_GE;
    break;
  case ISD::SETGT:
    std::swap(LHS, RHS);
    [[fallthrough]];
  case ISD::SETLT:
    // Turn lhs < rhs with lhs constant into rhs >= lhs+1, this allows us to
    // fold constant into instruction.
    if (const ConstantSDNode * C = dyn_cast<ConstantSDNode>(LHS)) {
      LHS = RHS;
      RHS =
          DAG.getSignedConstant(C->getSExtValue() + 1, dl, C->getValueType(0));
      TCC = DSPICCC::COND_GE;
      break;
    }
    TCC = DSPICCC::COND_L;
    break;
  }

  TargetCC = DAG.getConstant(TCC, dl, MVT::i8);
  return DAG.getNode(DSPICISD::CMP, dl, MVT::Glue, LHS, RHS);
}


SDValue DSPICTargetLowering::LowerBR_CC(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(1))->get();
  SDValue LHS   = Op.getOperand(2);
  SDValue RHS   = Op.getOperand(3);
  SDValue Dest  = Op.getOperand(4);
  SDLoc dl  (Op);

  // L1f-g (trellis session 87): a sign test against zero is a test of the top bit, and a bit
  // test can become the skip form (`btsc w0,#15 ; rcall`, 2 words for `cp0 ; bra ; rcall`'s
  // 3); as a plain branch the two are the same two words. The combiner spells a single-bit
  // test of bit 7 or 15 this way, so this is where the bit test is recovered.
  // InstCombine spells the same test `x > -1` / `x <= -1` (bits-dag.log): both forms are caught.
  bool SignLT = isNullConstant(RHS) && (CC == ISD::SETLT || CC == ISD::SETGE);
  bool SignGT = isAllOnesConstant(RHS) && (CC == ISD::SETGT || CC == ISD::SETLE);
  if ((SignLT || SignGT) &&
      (LHS.getValueType() == MVT::i8 || LHS.getValueType() == MVT::i16)) {
    EVT XVT = LHS.getValueType();
    bool Negative = CC == ISD::SETLT || CC == ISD::SETLE; // the branch when the top bit is set
    LHS = DAG.getNode(ISD::AND, dl, XVT, LHS,
                      DAG.getConstant(XVT == MVT::i8 ? 0x80 : 0x8000, dl, XVT));
    RHS = DAG.getConstant(0, dl, XVT);
    CC = Negative ? ISD::SETNE : ISD::SETEQ;
  }

  SDValue TargetCC;
  SDValue Flag = EmitCMP(LHS, RHS, TargetCC, CC, dl, DAG);

  return DAG.getNode(DSPICISD::BR_CC, dl, Op.getValueType(),
                     Chain, Dest, TargetCC, Flag);
}

SDValue DSPICTargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  SDValue LHS   = Op.getOperand(0);
  SDValue RHS   = Op.getOperand(1);
  SDLoc dl  (Op);

  // If we are doing an AND and testing against zero, then the CMP
  // will not be generated.  The AND (or BIT) will generate the condition codes,
  // but they are different from CMP.
  // FIXME: since we're doing a post-processing, use a pseudoinstr here, so
  // lowering & isel wouldn't diverge.
  bool andCC = isNullConstant(RHS) && LHS.hasOneUse() &&
               (LHS.getOpcode() == ISD::AND ||
                (LHS.getOpcode() == ISD::TRUNCATE &&
                 LHS.getOperand(0).getOpcode() == ISD::AND));
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  // L1f-g (trellis session 87): a single-bit test as a VALUE. (x & 1<<k) != 0 is (x >> k) & 1
  // and == 0 is ((x ^ 1<<k) >> k) & 1 -- cc1's `bfext #k,#1` in one word (`xor #m` first for
  // == 0), which the .td selects from the shift-and-mask shape, from a register or from a
  // file place at word width. Never through the flags: the FLAG2BOOL form is three words.
  if (isNullConstant(RHS) && LHS.getOpcode() == ISD::AND &&
      (CC == ISD::SETNE || CC == ISD::SETEQ)) {
    if (auto *C = dyn_cast<ConstantSDNode>(LHS.getOperand(1))) {
      EVT XVT = LHS.getValueType();
      uint64_t M = C->getZExtValue() & (XVT == MVT::i8 ? 0xff : 0xffff);
      if (isPowerOf2_64(M)) {
        SDValue Sh = DAG.getNode(ISD::SRL, dl, XVT, LHS.getOperand(0),
                                 DAG.getConstant(Log2_64(M), dl, MVT::i16));
        SDValue Bit = DAG.getNode(ISD::AND, dl, XVT, Sh, DAG.getConstant(1, dl, XVT));
        if (CC == ISD::SETEQ)
          Bit = DAG.getNode(ISD::XOR, dl, XVT, Bit, DAG.getConstant(1, dl, XVT));
        return DAG.getZExtOrTrunc(Bit, dl, Op.getValueType());
      }
    }
  }
  SDValue TargetCC;
  SDValue Flag = EmitCMP(LHS, RHS, TargetCC, CC, dl, DAG);

  // L1f-b: a boolean is materialized through SELECT_CC (the Select pseudo's branch
  // diamond), never by reading SR — SR is a FILE register on dsPIC, not a W register, and
  // the inherited `SR >> 1 & 1` idiom printed `xor.w SR,w2,w2`, which no ALU form encodes.
  // The one-word forms (`bsw`, `btst`, the compare-and-skip family) are L1f-e's.
  (void)andCC;
  EVT VT = Op.getValueType();
  // L1f-e: the four equality/unsigned conditions are `bsw` flag-to-bool (4 words, cc1's
  // shape); the six order conditions stay the SELECT_CC branch-select (5 words). The
  // {flag,invert} imm: bit0 = flag (0=Z,1=C), bit1 = invert (the `btg`).
  unsigned Cond = TargetCC->getAsZExtVal();
  int Kind = -1;
  switch (Cond) {
  case DSPICCC::COND_NE: Kind = 0; break;        // bsw.z
  case DSPICCC::COND_E:  Kind = 0 | 2; break;    // bsw.z ; btg
  case DSPICCC::COND_HS: Kind = 1; break;        // bsw.c        (unsigned >=)
  case DSPICCC::COND_LO: Kind = 1 | 2; break;    // bsw.c ; btg  (unsigned <)
  default: break;
  }
  if (Kind >= 0)
    return DAG.getNode(DSPICISD::FLAG2BOOL, dl, VT,
                       DAG.getConstant(Kind, dl, VT), Flag);
  SDValue One  = DAG.getConstant(1, dl, VT);
  SDValue Zero = DAG.getConstant(0, dl, VT);
  SDValue Ops[] = {One, Zero, TargetCC, Flag};
  return DAG.getNode(DSPICISD::SELECT_CC, dl, Op.getValueType(), Ops);
}

SDValue DSPICTargetLowering::LowerSELECT_CC(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDValue LHS    = Op.getOperand(0);
  SDValue RHS    = Op.getOperand(1);
  SDValue TrueV  = Op.getOperand(2);
  SDValue FalseV = Op.getOperand(3);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(4))->get();
  SDLoc dl   (Op);

  SDValue TargetCC;
  SDValue Flag = EmitCMP(LHS, RHS, TargetCC, CC, dl, DAG);

  SDValue Ops[] = {TrueV, FalseV, TargetCC, Flag};

  return DAG.getNode(DSPICISD::SELECT_CC, dl, Op.getValueType(), Ops);
}

SDValue DSPICTargetLowering::LowerSIGN_EXTEND(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDValue Val = Op.getOperand(0);
  EVT VT      = Op.getValueType();
  SDLoc dl(Op);

  assert(VT == MVT::i16 && "Only support i16 for now!");

  return DAG.getNode(ISD::SIGN_EXTEND_INREG, dl, VT,
                     DAG.getNode(ISD::ANY_EXTEND, dl, VT, Val),
                     DAG.getValueType(Val.getValueType()));
}

SDValue
DSPICTargetLowering::getReturnAddressFrameIndex(SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  DSPICMachineFunctionInfo *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
  int ReturnAddrIndex = FuncInfo->getRAIndex();
  MVT PtrVT = getFrameIndexTy(MF.getDataLayout());

  if (ReturnAddrIndex == 0) {
    // Set up a frame object for the return address.
    uint64_t SlotSize = PtrVT.getStoreSize();
    ReturnAddrIndex = MF.getFrameInfo().CreateFixedObject(SlotSize, -SlotSize,
                                                           true);
    FuncInfo->setRAIndex(ReturnAddrIndex);
  }

  return DAG.getFrameIndex(ReturnAddrIndex, PtrVT);
}

SDValue DSPICTargetLowering::LowerRETURNADDR(SDValue Op,
                                              SelectionDAG &DAG) const {
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  MFI.setReturnAddressIsTaken(true);

  unsigned Depth = Op.getConstantOperandVal(0);
  SDLoc dl(Op);
  EVT PtrVT = Op.getValueType();

  if (Depth > 0) {
    SDValue FrameAddr = LowerFRAMEADDR(Op, DAG);
    SDValue Offset =
      DAG.getConstant(PtrVT.getStoreSize(), dl, MVT::i16);
    return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(),
                       DAG.getNode(ISD::ADD, dl, PtrVT, FrameAddr, Offset),
                       MachinePointerInfo());
  }

  // Just load the return address.
  SDValue RetAddrFI = getReturnAddressFrameIndex(DAG);
  return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), RetAddrFI,
                     MachinePointerInfo());
}

SDValue DSPICTargetLowering::LowerFRAMEADDR(SDValue Op,
                                             SelectionDAG &DAG) const {
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  MFI.setFrameAddressIsTaken(true);

  EVT VT = Op.getValueType();
  SDLoc dl(Op);  // FIXME probably not meaningful
  unsigned Depth = Op.getConstantOperandVal(0);
  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl,
                                         DSPIC::R4, VT);
  while (Depth--)
    FrameAddr = DAG.getLoad(VT, dl, DAG.getEntryNode(), FrameAddr,
                            MachinePointerInfo());
  return FrameAddr;
}

SDValue DSPICTargetLowering::LowerVASTART(SDValue Op,
                                           SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  DSPICMachineFunctionInfo *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();

  SDValue Ptr = Op.getOperand(1);
  EVT PtrVT = Ptr.getValueType();

  // Frame index of first vararg argument
  SDValue FrameIndex =
      DAG.getFrameIndex(FuncInfo->getVarArgsFrameIndex(), PtrVT);
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();

  // Create a store of the frame index to the location operand
  return DAG.getStore(Op.getOperand(0), SDLoc(Op), FrameIndex, Ptr,
                      MachinePointerInfo(SV));
}

SDValue DSPICTargetLowering::LowerJumpTable(SDValue Op,
                                             SelectionDAG &DAG) const {
    JumpTableSDNode *JT = cast<JumpTableSDNode>(Op);
    EVT PtrVT = Op.getValueType();
    SDValue Result = DAG.getTargetJumpTable(JT->getIndex(), PtrVT);
    return DAG.getNode(DSPICISD::Wrapper, SDLoc(JT), PtrVT, Result);
}

/// getPostIndexedAddressParts - returns true by value, base pointer and
/// offset pointer and addressing mode by reference if this node can be
/// combined with a load / store to form a post-indexed load / store.
bool DSPICTargetLowering::getPostIndexedAddressParts(SDNode *N, SDNode *Op,
                                                      SDValue &Base,
                                                      SDValue &Offset,
                                                      ISD::MemIndexedMode &AM,
                                                      SelectionDAG &DAG) const {

  // L1f-f (trellis session 87): a plain load or store of i8/i16 whose pointer is then moved
  // by exactly the access width, either way: [Wn++] / [Wn--].
  auto *LS = cast<LSBaseSDNode>(N);
  if (auto *LD = dyn_cast<LoadSDNode>(N)) {
    if (LD->getExtensionType() != ISD::NON_EXTLOAD)
      return false;
  } else if (cast<StoreSDNode>(N)->isTruncatingStore())
    return false;
  EVT VT = LS->getMemoryVT();
  if (VT != MVT::i8 && VT != MVT::i16)
    return false;
  if (Op->getOpcode() != ISD::ADD && Op->getOpcode() != ISD::SUB)
    return false;
  auto *RHS = dyn_cast<ConstantSDNode>(Op->getOperand(1));
  if (!RHS)
    return false;
  int64_t C = RHS->getSExtValue();
  if (Op->getOpcode() == ISD::SUB)
    C = -C;
  int64_t W = VT == MVT::i16 ? 2 : 1;
  if (C != W && C != -W)
    return false;
  Base = Op->getOperand(0);
  Offset = DAG.getConstant(W, SDLoc(N), MVT::i16);
  AM = C > 0 ? ISD::POST_INC : ISD::POST_DEC;
  return true;
}

/// getPreIndexedAddressParts (L1f-f): a load or store whose address is `p +- width` and
/// whose moved pointer is used again: [++Wn] / [--Wn]. The combiner asks; it only forms the
/// node when the add has other uses, so a plain displacement stays [Wn+d].
bool DSPICTargetLowering::getPreIndexedAddressParts(SDNode *N, SDValue &Base,
                                                     SDValue &Offset,
                                                     ISD::MemIndexedMode &AM,
                                                     SelectionDAG &DAG) const {
  auto *LS = cast<LSBaseSDNode>(N);
  if (auto *LD = dyn_cast<LoadSDNode>(N)) {
    if (LD->getExtensionType() != ISD::NON_EXTLOAD)
      return false;
  } else if (cast<StoreSDNode>(N)->isTruncatingStore())
    return false;
  EVT VT = LS->getMemoryVT();
  if (VT != MVT::i8 && VT != MVT::i16)
    return false;
  SDValue Ptr = LS->getBasePtr();
  if (Ptr.getOpcode() != ISD::ADD && Ptr.getOpcode() != ISD::SUB)
    return false;
  auto *RHS = dyn_cast<ConstantSDNode>(Ptr.getOperand(1));
  if (!RHS)
    return false;
  int64_t C = RHS->getSExtValue();
  if (Ptr.getOpcode() == ISD::SUB)
    C = -C;
  int64_t W = VT == MVT::i16 ? 2 : 1;
  if (C != W && C != -W)
    return false;
  Base = Ptr.getOperand(0);
  Offset = DAG.getConstant(W, SDLoc(N), MVT::i16);
  AM = C > 0 ? ISD::PRE_INC : ISD::PRE_DEC;
  return true;
}

bool DSPICTargetLowering::isTruncateFree(Type *Ty1,
                                          Type *Ty2) const {
  if (!Ty1->isIntegerTy() || !Ty2->isIntegerTy())
    return false;

  return (Ty1->getPrimitiveSizeInBits().getFixedValue() >
          Ty2->getPrimitiveSizeInBits().getFixedValue());
}

bool DSPICTargetLowering::isTruncateFree(EVT VT1, EVT VT2) const {
  if (!VT1.isInteger() || !VT2.isInteger())
    return false;

  return (VT1.getFixedSizeInBits() > VT2.getFixedSizeInBits());
}

bool DSPICTargetLowering::isZExtFree(Type *Ty1, Type *Ty2) const {
  // DSPIC implicitly zero-extends 8-bit results in 16-bit registers.
  return false && Ty1->isIntegerTy(8) && Ty2->isIntegerTy(16);
}

bool DSPICTargetLowering::isZExtFree(EVT VT1, EVT VT2) const {
  // DSPIC implicitly zero-extends 8-bit results in 16-bit registers.
  return false && VT1 == MVT::i8 && VT2 == MVT::i16;
}

//===----------------------------------------------------------------------===//
//  Other Lowering Code
//===----------------------------------------------------------------------===//

MachineBasicBlock *
DSPICTargetLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                  MachineBasicBlock *BB) const {
  unsigned Opc = MI.getOpcode();

  const TargetInstrInfo &TII = *BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc dl = MI.getDebugLoc();

  // L1e prog-space (session 89): expand the PSV-window read to cc1's sequence.
  if (Opc == DSPIC::PSVLD16) {
    MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
    Register Dst  = MI.getOperand(0).getReg();
    Register Off  = MI.getOperand(1).getReg();
    Register Page = MI.getOperand(2).getReg();
    Register Save  = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register Off2  = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register Page2 = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVFROMDSR), Save);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::BTSTSC16), Off2).addReg(Off).addImm(15);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::RLC16), Page2).addReg(Page);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVPAGDSR)).addReg(Page2);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVINDLD16), Dst).addReg(Off2);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVPAGDSR)).addReg(Save);
    MI.eraseFromParent();
    return BB;
  }

  // Session 90: the byte read -- raw (`mov.b`), zero-extended (`ze`) or sign-extended (`se`),
  // the extension in the windowed load itself.
  if (Opc == DSPIC::PSVLD8 || Opc == DSPIC::PSVLD8Z || Opc == DSPIC::PSVLD8S) {
    unsigned LdOpc = Opc == DSPIC::PSVLD8Z ? DSPIC::MOVINDLDZ8
                   : Opc == DSPIC::PSVLD8S ? DSPIC::MOVINDLDS8 : DSPIC::MOVINDLD8;
    MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
    Register Dst  = MI.getOperand(0).getReg();
    Register Off  = MI.getOperand(1).getReg();
    Register Page = MI.getOperand(2).getReg();
    Register Save  = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register Off2  = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register Page2 = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVFROMDSR), Save);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::BTSTSC16), Off2).addReg(Off).addImm(15);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::RLC16), Page2).addReg(Page);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVPAGDSR)).addReg(Page2);
    BuildMI(*BB, MI, dl, TII.get(LdOpc), Dst).addReg(Off2);
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOVPAGDSR)).addReg(Save);
    MI.eraseFromParent();
    return BB;
  }

  // L1f-e: the compare flag-to-bool, `bsw.z`/`bsw.c` + optional `btg`.
  if (Opc == DSPIC::FLAG2BOOL16) {
    // Distinct vregs per step: `bsw`/`btg` are two-address (the constraint is resolved by the
    // two-address pass), so the def and the tied use must be SEPARATE vregs here, not one
    // (else getVRegDef sees multiple defs).
    int64_t K = MI.getOperand(1).getImm();
    MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
    Register Dst = MI.getOperand(0).getReg();
    Register V0 = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register V1 = (K & 2) ? MRI.createVirtualRegister(&DSPIC::GR16RegClass) : Dst;
    BuildMI(*BB, MI, dl, TII.get(DSPIC::MOV16rc), V0).addImm(0);
    unsigned Bsw = (K & 1) ? DSPIC::BSWC16 : DSPIC::BSWZ16;
    BuildMI(*BB, MI, dl, TII.get(Bsw), V1).addReg(V0);
    if (K & 2)
      BuildMI(*BB, MI, dl, TII.get(DSPIC::BTG16i0), Dst).addReg(V1);
    MI.eraseFromParent();
    return BB;
  }

  // (L1f-f, session 87: a dead second copy of the L1f-e arm -- its first, one-vreg form --
  // stood here, shadowed by the corrected arm above; removed.)
  // The memcpy/memset row (session 87 post-close; the pointer-preservation fix, Fable review).
  // The `mov`/`clr [Wn++]` under `repeat` ADVANCES the pointer register by cnt elements, but the
  // machine model does not track that. So the loop must run on SCRATCH copies of the incoming
  // pointers, or a use of the pointer AFTER the copy reads the advanced value (`memcpy(d,s,8);
  // use(d)` called use(d+16)). The COPY coalesces away when the incoming pointer is dead, so this
  // is `repeat`+move when the pointer is dead (smaller than cc1) and copy+`repeat`+move when it is
  // live (the same size as cc1's restore). {byte} imm selects `.w` or `.b`.
  // The moving-pointer instruction must MODEL the advance (a tied writeback def), or LLVM, seeing
  // no def, coalesces the scratch copy away and the original pointer is corrupted anyway. So the
  // loop runs on scratch copies (SD/SS) with `MOV16pp`/`CLR16p` (L1f-f's writeback forms); the
  // writeback vregs are dead. The scratch copy coalesces away when the incoming pointer is dead.
  if (Opc == DSPIC::MEMCPYinline) {
    MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
    Register Dst = MI.getOperand(0).getReg(), Src = MI.getOperand(1).getReg();
    int64_t Cnt = MI.getOperand(2).getImm(), Byte = MI.getOperand(3).getImm();
    Register SD = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register SS = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register WbS = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register WbD = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), SD).addReg(Dst);
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), SS).addReg(Src);
    BuildMI(*BB, MI, dl, TII.get(Byte ? DSPIC::MEMCPYrepB : DSPIC::MEMCPYrepW), WbS)
        .addReg(WbD, RegState::Define).addReg(SS).addReg(SD).addImm(Cnt - 1);
    MI.eraseFromParent();
    return BB;
  }
  if (Opc == DSPIC::MEMSETZinline) {
    MachineRegisterInfo &MRI = BB->getParent()->getRegInfo();
    Register Dst = MI.getOperand(0).getReg();
    int64_t Cnt = MI.getOperand(1).getImm(), Byte = MI.getOperand(2).getImm();
    Register SD = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    Register WbD = MRI.createVirtualRegister(&DSPIC::GR16RegClass);
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), SD).addReg(Dst);
    BuildMI(*BB, MI, dl, TII.get(Byte ? DSPIC::MEMSETrepB : DSPIC::MEMSETrepW), WbD)
        .addReg(SD).addImm(Cnt - 1);
    MI.eraseFromParent();
    return BB;
  }

  // L1f-f: the i32 shift by one -- the carry pair, adjacent, the carry never leaving it.
  if (Opc == DSPIC::SHL32x1 || Opc == DSPIC::SRL32x1 || Opc == DSPIC::SRA32x1) {
    Register Lo2 = MI.getOperand(0).getReg(), Hi2 = MI.getOperand(1).getReg();
    Register Lo = MI.getOperand(2).getReg(), Hi = MI.getOperand(3).getReg();
    if (Opc == DSPIC::SHL32x1) {
      BuildMI(*BB, MI, dl, TII.get(DSPIC::ADD16rr), Lo2).addReg(Lo).addReg(Lo);
      BuildMI(*BB, MI, dl, TII.get(DSPIC::ADDC16rr), Hi2).addReg(Hi).addReg(Hi);
    } else {
      BuildMI(*BB, MI, dl, TII.get(Opc == DSPIC::SRL32x1 ? DSPIC::LSR16r1 : DSPIC::ASR16r1), Hi2)
          .addReg(Hi);
      BuildMI(*BB, MI, dl, TII.get(DSPIC::RRC16r), Lo2).addReg(Lo);
    }
    MI.eraseFromParent();
    return BB;
  }

  // L1f-d increment 4: the hardware divide, dividend to w0, the pair out of w0:w1.
  if (Opc == DSPIC::UDIVREM16 || Opc == DSPIC::SDIVREM16) {
    unsigned DivOpc = Opc == DSPIC::SDIVREM16 ? DSPIC::DIVSW : DSPIC::DIVUW;
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), DSPIC::R12)
        .addReg(MI.getOperand(2).getReg());
    BuildMI(*BB, MI, dl, TII.get(DSPIC::REPEATdiv));
    BuildMI(*BB, MI, dl, TII.get(DivOpc)).addReg(MI.getOperand(3).getReg());
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), MI.getOperand(0).getReg())
        .addReg(DSPIC::R12);
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), MI.getOperand(1).getReg())
        .addReg(DSPIC::R13);
    MI.eraseFromParent();
    return BB;
  }

  // L1f-d increment 3: the widening multiply, its pair fixed at w0:w1.
  if (Opc == DSPIC::SMULLOHI16 || Opc == DSPIC::UMULLOHI16) {
    unsigned MulOpc = Opc == DSPIC::SMULLOHI16 ? DSPIC::MULSSpair : DSPIC::MULUUpair;
    BuildMI(*BB, MI, dl, TII.get(MulOpc))
        .addReg(MI.getOperand(2).getReg())
        .addReg(MI.getOperand(3).getReg());
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), MI.getOperand(0).getReg())
        .addReg(DSPIC::R12);
    BuildMI(*BB, MI, dl, TII.get(TargetOpcode::COPY), MI.getOperand(1).getReg())
        .addReg(DSPIC::R13);
    MI.eraseFromParent();
    return BB;
  }

  assert((Opc == DSPIC::Select16 || Opc == DSPIC::Select8) &&
         "Unexpected instr type to insert");

  // To "insert" a SELECT instruction, we actually have to insert the diamond
  // control-flow pattern.  The incoming instruction knows the destination vreg
  // to set, the condition code register to branch on, the true/false values to
  // select between, and a branch opcode to use.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator I = ++BB->getIterator();

  //  thisMBB:
  //  ...
  //   TrueVal = ...
  //   cmpTY ccX, r1, r2
  //   jCC copy1MBB
  //   fallthrough --> copy0MBB
  MachineBasicBlock *thisMBB = BB;
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *copy0MBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *copy1MBB = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(I, copy0MBB);
  F->insert(I, copy1MBB);
  // Update machine-CFG edges by transferring all successors of the current
  // block to the new block which will contain the Phi node for the select.
  copy1MBB->splice(copy1MBB->begin(), BB,
                   std::next(MachineBasicBlock::iterator(MI)), BB->end());
  copy1MBB->transferSuccessorsAndUpdatePHIs(BB);
  // Next, add the true and fallthrough blocks as its successors.
  BB->addSuccessor(copy0MBB);
  BB->addSuccessor(copy1MBB);

  BuildMI(BB, dl, TII.get(DSPIC::JCC))
      .addMBB(copy1MBB)
      .addImm(MI.getOperand(3).getImm());

  //  copy0MBB:
  //   %FalseValue = ...
  //   # fallthrough to copy1MBB
  BB = copy0MBB;

  // Update machine-CFG edges
  BB->addSuccessor(copy1MBB);

  //  copy1MBB:
  //   %Result = phi [ %FalseValue, copy0MBB ], [ %TrueValue, thisMBB ]
  //  ...
  BB = copy1MBB;
  BuildMI(*BB, BB->begin(), dl, TII.get(DSPIC::PHI), MI.getOperand(0).getReg())
      .addReg(MI.getOperand(2).getReg())
      .addMBB(copy0MBB)
      .addReg(MI.getOperand(1).getReg())
      .addMBB(thisMBB);

  MI.eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}
