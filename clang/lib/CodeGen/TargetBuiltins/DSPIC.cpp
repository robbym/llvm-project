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
  }
}
