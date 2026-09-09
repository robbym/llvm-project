//===-- DSPICTargetTransformInfo.h - dsPIC specific TTI ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The dsPIC TargetTransformInfo. Its ONE job is to tell the vectorizers that
/// this machine has no vector register and no vector instruction.
///
/// Before it existed, DSPICTargetMachine did not override
/// getTargetTransformInfo, so the generic implementation answered every cost
/// question the SLP and loop vectorizers asked. At -Os and -Oz the SLP
/// vectorizer took that as licence and packed two independent
/// (shift, divide-by-constant) chains into a <2 x i16>, which the backend
/// cannot legalize: `llc` aborts in DAGCombiner::visitUDIV ->
/// TargetLoweringBase::getSetCCResultType, "No default SetCC type for
/// vectors!". That was live on real firmware source -- stn3255,
/// StnElmNoInitCrippler.c, _GetCrippleLevel, compiled WITHOUT -D__DEBUG,
/// which is the configuration in which that file has any code at all -- and is
/// reduced to tools/dspic-llvm/steps/reach/vecudiv.ll.
///
/// The same reduced module aborts `llc -mtriple=msp430` identically -- MSP430
/// registers no TTI either -- so the legalization hole is inherited from
/// upstream and this file is the local answer to it: refuse the vector at the
/// point where it would be created, rather than teach the backend to lower a
/// type the machine does not have.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_DSPIC_DSPICTARGETTRANSFORMINFO_H

#include "DSPIC.h"
#include "DSPICTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class DSPICTTIImpl final : public BasicTTIImplBase<DSPICTTIImpl> {
  typedef BasicTTIImplBase<DSPICTTIImpl> BaseT;
  typedef TargetTransformInfo TTI;
  friend BaseT;

  const DSPICSubtarget *ST;
  const DSPICTargetLowering *TLI;

  const DSPICSubtarget *getST() const { return ST; }
  const DSPICTargetLowering *getTLI() const { return TLI; }

public:
  explicit DSPICTTIImpl(const DSPICTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  /// Register class 1 is the vector class. There is not one register in it.
  unsigned getNumberOfRegisters(unsigned ClassID) const override {
    bool Vector = (ClassID == 1);
    if (Vector)
      return 0;
    return 16; // w0..w15
  }

  /// A vector register is zero bits wide, because there is no such register.
  /// A scalar one is a W register: 16 bits.
  ///
  /// ⚠ MEASURED, NOT ASSUMED: this override is NOT what stops the vectorizer.
  /// Mutant MV2 (steps/reach/vec-mutant.sh) answers 16 here while
  /// getNumberOfRegisters still answers zero, and the whole comparer stays
  /// green -- the register COUNT alone gates it. The override is kept because
  /// zero is TRUE of this machine and the next caller deserves the true
  /// answer, but it does no work against the defect above and this comment
  /// exists so nobody reads it as half the fix.
  TypeSize
  getRegisterBitWidth(TargetTransformInfo::RegisterKind K) const override {
    switch (K) {
    case TargetTransformInfo::RGK_Scalar:
      return TypeSize::getFixed(16);
    case TargetTransformInfo::RGK_FixedWidthVector:
    case TargetTransformInfo::RGK_ScalableVector:
      return TypeSize::getFixed(0);
    }
    llvm_unreachable("unhandled register kind");
  }

  // getMaxInterleaveFactor is NOT overridden: TargetTransformInfoImpl's own default
  // already returns 1 (read at the source, not assumed), so an override here would be a
  // second statement of the same thing -- and its signature upstream takes a second
  // argument, so the redundant copy did not even compile.
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_DSPIC_DSPICTARGETTRANSFORMINFO_H
