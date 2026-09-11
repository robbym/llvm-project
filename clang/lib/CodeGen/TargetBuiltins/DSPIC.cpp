//===------ DSPIC.cpp - Emit LLVM Code for dsPIC33 builtins ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 103: the table read/write builtins. Modelled on TargetBuiltins/AVR.cpp, which is
// the smallest complete example of this path in the tree.
// trellis session 108: the vendor set that reaches instructions no C expression spells -- the
// byte table forms, fbcl, the four widening multiplies, the swaps and the four control
// instructions. Names and signatures are the vendor's (BuiltinsDSPIC.def says where they were read).
//
//===----------------------------------------------------------------------===//

#include "CGBuiltin.h"
#include "clang/Basic/TargetBuiltins.h"
#include "llvm/IR/IntrinsicsDSPIC.h"

using namespace clang;
using namespace CodeGen;
using namespace llvm;

Value *CodeGenFunction::EmitDSPICBuiltinExpr(unsigned BuiltinID,
                                             const CallExpr *E) {
  switch (BuiltinID) {
  default:
    return nullptr;
  case DSPIC::BI__builtin_tblrdl:
  case DSPIC::BI__builtin_tblrdh: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblrdl
                       ? Intrinsic::dspic_tblrdl
                       : Intrinsic::dspic_tblrdh;
    Value *Off = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(IID), Off);
  }
  case DSPIC::BI__builtin_tblwtl:
  case DSPIC::BI__builtin_tblwth: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblwtl
                       ? Intrinsic::dspic_tblwtl
                       : Intrinsic::dspic_tblwth;
    // The vendor's argument order is (offset, value) and the intrinsic keeps it; the INSTRUCTION
    // prints them the other way round (`tblwtl.w Ws, [Wd]`), and the pattern in
    // DSPICInstrInfo.td is where that swap happens. Mutant MT3 exchanges them here.
    Value *Off = EmitScalarExpr(E->getArg(0));
    Value *Val = EmitScalarExpr(E->getArg(1));
    return Builder.CreateCall(CGM.getIntrinsic(IID), {Off, Val});
  }

  // ── trellis session 108 ───────────────────────────────────────────────────────────────────────
  case DSPIC::BI__builtin_tblrdlb:
  case DSPIC::BI__builtin_tblrdhb: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblrdlb
                       ? Intrinsic::dspic_tblrdlb
                       : Intrinsic::dspic_tblrdhb;
    Value *Off = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(IID), Off);
  }
  case DSPIC::BI__builtin_tblwtlb:
  case DSPIC::BI__builtin_tblwthb: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblwtlb
                       ? Intrinsic::dspic_tblwtlb
                       : Intrinsic::dspic_tblwthb;
    Value *Off = EmitScalarExpr(E->getArg(0));
    Value *Val = EmitScalarExpr(E->getArg(1));
    return Builder.CreateCall(CGM.getIntrinsic(IID), {Off, Val});
  }
  case DSPIC::BI__builtin_fbcl: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_fbcl), X);
  }
  case DSPIC::BI__builtin_mulss:
  case DSPIC::BI__builtin_muluu:
  case DSPIC::BI__builtin_mulsu:
  case DSPIC::BI__builtin_mulus: {
    // ⛔ NO INTRINSIC. The product of two extended i16s is exactly the IR shape the backend already
    // selects `mul.ss`/`mul.uu` from (SMUL_LOHI/UMUL_LOHI, L1f-d) and `mul.su`/`mul.us` from since
    // this session (combineMixedWideningMul on the i32 MUL). The extension of each operand is its
    // own signedness in the vendor's signature: ss = (int, int), uu = (unsigned, unsigned),
    // su = (int, unsigned), us = (unsigned, int). Mutant MM2 exchanges the two extensions of mulsu.
    bool S0 = BuiltinID == DSPIC::BI__builtin_mulss ||
              BuiltinID == DSPIC::BI__builtin_mulsu;
    bool S1 = BuiltinID == DSPIC::BI__builtin_mulss ||
              BuiltinID == DSPIC::BI__builtin_mulus;
    llvm::Type *ResTy = ConvertType(E->getType()); // long: i32 on this target
    Value *A = EmitScalarExpr(E->getArg(0));
    Value *B = EmitScalarExpr(E->getArg(1));
    A = S0 ? Builder.CreateSExt(A, ResTy) : Builder.CreateZExt(A, ResTy);
    B = S1 ? Builder.CreateSExt(B, ResTy) : Builder.CreateZExt(B, ResTy);
    return Builder.CreateMul(A, B);
  }
  case DSPIC::BI__builtin_swap: {
    // The word swap is ISD::BSWAP, which the backend already prints as `swap Wn`.
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::bswap, X->getType()), X);
  }
  case DSPIC::BI__builtin_swap_byte: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_swapb), X);
  }
  case DSPIC::BI__builtin_disi:
  case DSPIC::BI__builtin_pwrsav: {
    // The `I` in the builtin's type string makes a non-constant argument a Sema error before this
    // runs; the RANGE is checked here and refused with a diagnostic at the call, because the
    // encoding has 14 (disi) or 1 (pwrsav) bits for it and a value past that would be silently
    // truncated by the assembler. Mutant MD1 drops this check.
    bool Disi = BuiltinID == DSPIC::BI__builtin_disi;
    llvm::APSInt V = E->getArg(0)->EvaluateKnownConstInt(getContext());
    int64_t N = V.getSExtValue();
    int64_t Max = Disi ? 16383 : 1;
    if (N < 0 || N > Max) {
      CGM.Error(E->getArg(0)->getExprLoc(),
                Disi ? "__builtin_disi: the cycle count must be 0..16383 (disi #lit14)"
                     : "__builtin_pwrsav: the mode must be 0 (Sleep) or 1 (Idle) (pwrsav #lit1)");
      return nullptr;
    }
    Value *C = llvm::ConstantInt::get(Int16Ty, N);
    return Builder.CreateCall(
        CGM.getIntrinsic(Disi ? Intrinsic::dspic_disi : Intrinsic::dspic_pwrsav), C);
  }
  case DSPIC::BI__builtin_clrwdt:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_clrwdt));
  case DSPIC::BI__builtin_nop:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_nop));
  }
}
