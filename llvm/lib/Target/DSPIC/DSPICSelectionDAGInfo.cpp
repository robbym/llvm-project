//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DSPICSelectionDAGInfo.h"
#include "DSPICISelLowering.h"
#include "MCTargetDesc/DSPICMCAsmInfo.h"   // DSPICII::MO_EDSOFFSET / MO_EDSPAGE (session 110)
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_SDNODE_DESC
#include "DSPICGenSDNodeInfo.inc"

using namespace llvm;

DSPICSelectionDAGInfo::DSPICSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(DSPICGenSDNodeInfo) {}

DSPICSelectionDAGInfo::~DSPICSelectionDAGInfo() = default;

// A constant-length block copy: `repeat #cnt-1 ; mov [Wn++],[Wn++]` (word if the access is
// aligned and the size even, byte otherwise). A variable length returns SDValue() -> the
// named `_memcpy`. Volatile is left to the libcall too (order/width of a volatile block is
// not this row's). trellis session 87 post-close.
SDValue DSPICSelectionDAGInfo::EmitTargetCodeForMemcpy(
    SelectionDAG &DAG, const SDLoc &dl, SDValue Chain, SDValue Dst, SDValue Src, SDValue Size,
    Align DstAlign, Align SrcAlign, bool isVolatile, bool AlwaysInline,
    MachinePointerInfo DstPtrInfo, MachinePointerInfo SrcPtrInfo) const {
  // Session 90: a copy FROM program memory (addrspace 1 source) is cc1's `__memcpy_helper`
  // libcall -- `void _memcpy_helper(unsigned long src, unsigned dst, unsigned n, unsigned 0)`
  // (pic30.c's emit_library_call; measured: w0:w1 = tbloffset:tblpage, w2 = dst, w3 = n,
  // w4 = 0). findOptimalMemOpLowering has already declined the load/store expansion for this
  // source, so every size comes here, constant or not. A copy INTO program memory has no
  // instruction (flash is written by tblwt under NVM control) and is refused, as cc1 refuses it.
  if (DstPtrInfo.getAddrSpace() == 1)
    report_fatal_error("dsPIC: a block copy into program memory (__prog__) is not a store; "
                       "flash is written through the tblwt builtins", false);
  if (SrcPtrInfo.getAddrSpace() == 1) {
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    LLVMContext &Ctx = *DAG.getContext();
    Type *I16 = Type::getInt16Ty(Ctx);
    TargetLowering::ArgListTy Args;
    Args.emplace_back(Src, Type::getInt32Ty(Ctx));
    Args.emplace_back(Dst, I16);
    Args.emplace_back(DAG.getZExtOrTrunc(Size, dl, MVT::i16), I16);
    Args.emplace_back(DAG.getConstant(0, dl, MVT::i16), I16);
    TargetLowering::CallLoweringInfo CLI(DAG);
    CLI.setDebugLoc(dl).setChain(Chain)
        .setLibCallee(CallingConv::C, Type::getVoidTy(Ctx),
                      DAG.getExternalSymbol("_memcpy_helper",
                                            TLI.getPointerTy(DAG.getDataLayout())),
                      std::move(Args))
        .setDiscardResult();
    return TLI.LowerCallTo(CLI).second;
  }
  // trellis session 110: a block copy touching EXTENDED DATA SPACE (addrspace 2) in either
  // direction is cc1's `__memcpy_eds` libcall -- w0:w1 = src as a 32-bit EDS address, w2:w3 = dst
  // likewise, w4 and w5 the size. ⛔ This arm must come BEFORE the inline path below: that path
  // builds a DSPICISD::MEMCPY whose pointer operands are i16, and handing it an addrspace-2 i32
  // pointer produced a node LowerOperation has no case for -- the crash this row repairs.
  // ⚠ The two size registers cannot be told apart from any observable: the only constructs that
  // reach here are whole-object (assignment, return, pass-by-value), where length and both object
  // sizes coincide, and cc1 refuses __builtin_memcpy with an __eds__ pointer (guide 7.7.3). So
  // passing the size twice is cc1's call exactly, for every case that can occur.
  if (DstPtrInfo.getAddrSpace() == 2 || SrcPtrInfo.getAddrSpace() == 2) {
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    LLVMContext &Ctx = *DAG.getContext();
    Type *I16 = Type::getInt16Ty(Ctx);
    Type *I32 = Type::getInt32Ty(Ctx);
    // An operand NOT in addrspace 2 is a 16-bit address and the routine takes 32-bit EDS addresses
    // on BOTH sides, so it has to be widened. ⛔ cc1 widens a near GLOBAL with the relocation pair
    // -- `mov #edsoffset(_ns),w2 ; mov #edspage(_ns),w3` -- not by zero-extending, and the first
    // version of this arm zero-extended (`mov #_ns,w2 ; clr w3`), which the comparer caught against
    // cc1. Matched here rather than argued about: whether a zero page is equivalent depends on what
    // __memcpy_eds does with it, which is not established. A RUNTIME near pointer still zero-extends
    // -- no relocation exists for it -- and that case is NOT witnessed by any fixture.
    auto Wide = [&](SDValue P, unsigned AS) -> SDValue {
      if (AS == 2)
        return P;
      // ⚠ the pointer reaches here EITHER bare or already wrapped, depending on how far the DAG
      // has been built; the first version only looked through Wrapper and silently fell through to
      // the zero-extend, which the comparer caught against cc1.
      {
        SDValue Inner = P.getOpcode() == DSPICISD::Wrapper ? P.getOperand(0) : P;
        if (auto *GA = dyn_cast<GlobalAddressSDNode>(Inner)) {
          SDValue Lo = DAG.getNode(DSPICISD::Wrapper, dl, MVT::i16,
                                   DAG.getTargetGlobalAddress(GA->getGlobal(), dl, MVT::i16,
                                                              GA->getOffset(),
                                                              DSPICII::MO_EDSOFFSET));
          SDValue Hi = DAG.getNode(DSPICISD::Wrapper, dl, MVT::i16,
                                   DAG.getTargetGlobalAddress(GA->getGlobal(), dl, MVT::i16,
                                                              GA->getOffset(),
                                                              DSPICII::MO_EDSPAGE));
          return DAG.getNode(ISD::BUILD_PAIR, dl, MVT::i32, Lo, Hi);
        }
      }
      return DAG.getZExtOrTrunc(P, dl, MVT::i32);
    };
    SDValue SrcA = Wide(Src, SrcPtrInfo.getAddrSpace());
    SDValue DstA = Wide(Dst, DstPtrInfo.getAddrSpace());
    SDValue N = DAG.getZExtOrTrunc(Size, dl, MVT::i16);
    TargetLowering::ArgListTy Args;
    Args.emplace_back(SrcA, I32);
    Args.emplace_back(DstA, I32);
    Args.emplace_back(N, I16);
    Args.emplace_back(N, I16);
    TargetLowering::CallLoweringInfo CLI(DAG);
    CLI.setDebugLoc(dl).setChain(Chain)
        .setLibCallee(CallingConv::C, Type::getVoidTy(Ctx),
                      // ⛔ ONE underscore fewer than the symbol: this target prefixes external
                      // symbols with `_`, so "__memcpy_eds" here emitted `___memcpy_eds` -- three
                      // underscores, a name nothing defines and a LINK failure, not a miscompile.
                      // Session 92 found the same class when every libcall name was MSP430's.
                      DAG.getExternalSymbol("_memcpy_eds",
                                            TLI.getPointerTy(DAG.getDataLayout())),
                      std::move(Args))
        .setDiscardResult();
    return TLI.LowerCallTo(CLI).second;
  }

  auto *C = dyn_cast<ConstantSDNode>(Size);
  if (!C || isVolatile)
    return SDValue();
  uint64_t Bytes = C->getZExtValue();
  if (Bytes == 0)
    return Chain;
  bool Word = (Bytes % 2 == 0) && DstAlign >= Align(2) && SrcAlign >= Align(2);
  uint64_t Cnt = Word ? Bytes / 2 : Bytes;
  SDValue Ops[] = {Chain, Dst, Src, DAG.getTargetConstant(Cnt, dl, MVT::i16),
                   DAG.getTargetConstant(Word ? 0 : 1, dl, MVT::i16)};
  return DAG.getNode(DSPICISD::MEMCPY, dl, MVT::Other, Ops);
}

// A constant-length ZERO fill: `repeat #cnt-1 ; clr [Wn++]`. A nonzero value or a variable
// length returns SDValue() -> the named `_memset` (cc1 inlines a nonzero constant memset;
// ours calls -- a tagged size gap). trellis session 87 post-close.
SDValue DSPICSelectionDAGInfo::EmitTargetCodeForMemset(
    SelectionDAG &DAG, const SDLoc &dl, SDValue Chain, SDValue Dst, SDValue Val, SDValue Size,
    Align Alignment, bool isVolatile, bool AlwaysInline, MachinePointerInfo DstPtrInfo) const {
  auto *C = dyn_cast<ConstantSDNode>(Size);
  auto *V = dyn_cast<ConstantSDNode>(Val);
  if (!C || isVolatile || !V || V->getZExtValue() != 0)
    return SDValue();
  uint64_t Bytes = C->getZExtValue();
  if (Bytes == 0)
    return Chain;
  bool Word = (Bytes % 2 == 0) && Alignment >= Align(2);
  uint64_t Cnt = Word ? Bytes / 2 : Bytes;
  SDValue Ops[] = {Chain, Dst, DAG.getTargetConstant(Cnt, dl, MVT::i16),
                   DAG.getTargetConstant(Word ? 0 : 1, dl, MVT::i16)};
  return DAG.getNode(DSPICISD::MEMSETZ, dl, MVT::Other, Ops);
}
