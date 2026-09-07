//===-- DSPICISelLowering.h - DSPIC DAG Lowering Interface ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that DSPIC uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICISELLOWERING_H
#define LLVM_LIB_TARGET_DSPIC_DSPICISELLOWERING_H

#include "DSPIC.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {
  class DSPICSubtarget;
  class DSPICTargetLowering : public TargetLowering {
  public:
    explicit DSPICTargetLowering(const TargetMachine &TM,
                                  const DSPICSubtarget &STI);

    // L1f-c: the shift amount is a W register (`sl Wb,Wns,Wnd`), an i16.
    MVT getScalarShiftAmountTy(const DataLayout &, EVT) const override {
      return MVT::i16;
    }

    MVT::SimpleValueType getCmpLibcallReturnType() const override {
      return MVT::i16;
    }

    /// LowerOperation - Provide custom lowering hooks for some operations.
    SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;
    // L1e prog-space: an addrspace(1) global address is an illegal i32 (page:offset); the type
    // legalizer expands it here into the tbloffset/tblpage pair.
    void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue> &Results,
                            SelectionDAG &DAG) const override;

    SDValue LowerShifts(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerDivRem(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerExternalSymbol(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerBR_CC(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerSETCC(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerSIGN_EXTEND(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;
    SDValue LowerJumpTable(SDValue Op, SelectionDAG &DAG) const;
    SDValue getReturnAddressFrameIndex(SelectionDAG &DAG) const;

    TargetLowering::ConstraintType
    getConstraintType(StringRef Constraint) const override;
    std::pair<unsigned, const TargetRegisterClass *>
    getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                 StringRef Constraint, MVT VT) const override;

    /// isTruncateFree - Return true if it's free to truncate a value of type
    /// Ty1 to type Ty2. e.g. On dspic it's free to truncate a i16 value in
    /// register R15W to i8 by referencing its sub-register R15B.
    bool isTruncateFree(Type *Ty1, Type *Ty2) const override;
    bool isTruncateFree(EVT VT1, EVT VT2) const override;

    /// isZExtFree - Return true if any actual instruction that defines a value
    /// of type Ty1 implicit zero-extends the value to Ty2 in the result
    /// register. This does not necessarily include registers defined in unknown
    /// ways, such as incoming arguments, or copies from unknown virtual
    /// registers. Also, if isTruncateFree(Ty2, Ty1) is true, this does not
    /// necessarily apply to truncate instructions. e.g. on dspic, all
    /// instructions that define 8-bit values implicit zero-extend the result
    /// out to 16 bits.
    bool isZExtFree(Type *Ty1, Type *Ty2) const override;
    bool isZExtFree(EVT VT1, EVT VT2) const override;

    bool isLegalICmpImmediate(int64_t) const override;
    bool shouldAvoidTransformToShift(EVT VT, unsigned Amount) const override;

    MachineBasicBlock *
    EmitInstrWithCustomInserter(MachineInstr &MI,
                                MachineBasicBlock *BB) const override;

  private:
    /// Clauses 2-5 of the compatible-frame rule (L1c); Why names the failing one.
    bool isEligibleForTailCall(const TargetLowering::CallLoweringInfo &CLI,
                               std::string &Why) const;

    SDValue LowerCCCCallTo(SDValue Chain, SDValue Callee,
                           CallingConv::ID CallConv, bool isVarArg,
                           bool isTailCall,
                           const SmallVectorImpl<ISD::OutputArg> &Outs,
                           const SmallVectorImpl<SDValue> &OutVals,
                           const SmallVectorImpl<ISD::InputArg> &Ins,
                           const SDLoc &dl, SelectionDAG &DAG,
                           SmallVectorImpl<SDValue> &InVals) const;

    SDValue LowerCCCArguments(SDValue Chain, CallingConv::ID CallConv,
                              bool isVarArg,
                              const SmallVectorImpl<ISD::InputArg> &Ins,
                              const SDLoc &dl, SelectionDAG &DAG,
                              SmallVectorImpl<SDValue> &InVals) const;

    SDValue LowerCallResult(SDValue Chain, SDValue InGlue,
                            CallingConv::ID CallConv, bool isVarArg,
                            const SmallVectorImpl<ISD::InputArg> &Ins,
                            const SDLoc &dl, SelectionDAG &DAG,
                            SmallVectorImpl<SDValue> &InVals) const;

    SDValue
    LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
                         const SmallVectorImpl<ISD::InputArg> &Ins,
                         const SDLoc &dl, SelectionDAG &DAG,
                         SmallVectorImpl<SDValue> &InVals) const override;
    SDValue
      LowerCall(TargetLowering::CallLoweringInfo &CLI,
                SmallVectorImpl<SDValue> &InVals) const override;

    bool CanLowerReturn(CallingConv::ID CallConv,
                        MachineFunction &MF,
                        bool IsVarArg,
                        const SmallVectorImpl<ISD::OutputArg> &Outs,
                        LLVMContext &Context, const Type *RetTy) const override;

    SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
                        const SmallVectorImpl<ISD::OutputArg> &Outs,
                        const SmallVectorImpl<SDValue> &OutVals,
                        const SDLoc &dl, SelectionDAG &DAG) const override;

    /// L1f-g: a `tail call` is a candidate (CodeGenPrepare then duplicates a return into
    /// the call's block); whether it BECOMES `bra` is the L1c compatible-frame rule's word,
    /// isEligibleForTailCall. Without this the default (false) kept every guarded call at
    /// `rcall ; return`.
    bool mayBeEmittedAsTailCall(const CallInst *CI) const override {
      return CI->isTailCall();
    }
    /// L1f-g: never -- a load is one word at either width, and a narrowed load loses the
    /// bit forms on memory (`btst [Wn],#k`); a volatile place is never narrowed anyway.
    bool shouldReduceLoadWidth(SDNode *Load, ISD::LoadExtType ExtTy, EVT NewVT,
                               std::optional<unsigned> ByteOffset) const override {
      return false;
    }
    SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;
    // L1f-k: FLAG2BOOL is a 0/1 word; SELECT_CC knows what both arms know
    void computeKnownBitsForTargetNode(const SDValue Op, KnownBits &Known,
                                       const APInt &DemandedElts, const SelectionDAG &DAG,
                                       unsigned Depth = 0) const override;
    // Session 90 (the data model): no load/store expansion of a program-memory block copy, and
    // a program-memory global in an asm "i" operand stays the symbol (an i16 target address).
    bool findOptimalMemOpLowering(LLVMContext &Context, std::vector<EVT> &MemOps,
                                  unsigned Limit, const MemOp &Op, unsigned DstAS,
                                  unsigned SrcAS, const AttributeList &FuncAttributes,
                                  EVT *LargestVT = nullptr) const override;
    void LowerAsmOperandForConstraint(SDValue Op, StringRef Constraint,
                                      std::vector<SDValue> &Ops,
                                      SelectionDAG &DAG) const override;
    bool getPreIndexedAddressParts(SDNode *N, SDValue &Base, SDValue &Offset,
                                   ISD::MemIndexedMode &AM,
                                   SelectionDAG &DAG) const override;
    bool getPostIndexedAddressParts(SDNode *N, SDNode *Op, SDValue &Base,
                                    SDValue &Offset, ISD::MemIndexedMode &AM,
                                    SelectionDAG &DAG) const override;
  };
} // namespace llvm

#endif
