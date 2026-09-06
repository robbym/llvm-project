//==- DSPICCallingConv.td - Calling Conventions for DSPIC -*- tablegen -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This describes the calling conventions for DSPIC architecture.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// DSPIC Return Value Calling Convention
//===----------------------------------------------------------------------===//
def RetCC_DSPIC : CallingConv<[
  // i8 are returned in registers R12B, R13B, R14B, R15B
  CCIfType<[i8], CCAssignToReg<[R12B, R13B, R14B, R15B]>>,

  // i16 are returned in registers R12, R13, R14, R15
  CCIfType<[i16], CCAssignToReg<[R12, R13, R14, R15]>>
]>;

//===----------------------------------------------------------------------===//
// DSPIC Argument Calling Conventions: none here. The argument convention is C++
// (DSPICISelLowering.cpp, AnalyzeArguments), measured from the pic30 cc1 at trellis L1c;
// TableGen's flat register/stack rules cannot say "the lowest free run aligned to P".
//===----------------------------------------------------------------------===//
