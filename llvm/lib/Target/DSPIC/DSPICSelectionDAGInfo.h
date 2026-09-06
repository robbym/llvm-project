//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_DSPIC_DSPICSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_DSPIC_DSPICSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "DSPICGenSDNodeInfo.inc"

namespace llvm {

class DSPICSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  DSPICSelectionDAGInfo();

  ~DSPICSelectionDAGInfo() override;

  // The memcpy/memset row (trellis session 87 post-close): a CONSTANT length becomes an
  // inline `repeat` block; anything else returns SDValue() and falls to the named libcall.
  SDValue EmitTargetCodeForMemcpy(SelectionDAG &DAG, const SDLoc &dl, SDValue Chain,
                                  SDValue Dst, SDValue Src, SDValue Size, Align DstAlign,
                                  Align SrcAlign, bool isVolatile, bool AlwaysInline,
                                  MachinePointerInfo DstPtrInfo,
                                  MachinePointerInfo SrcPtrInfo) const override;
  SDValue EmitTargetCodeForMemset(SelectionDAG &DAG, const SDLoc &dl, SDValue Chain,
                                  SDValue Dst, SDValue Val, SDValue Size, Align Alignment,
                                  bool isVolatile, bool AlwaysInline,
                                  MachinePointerInfo DstPtrInfo) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_DSPIC_DSPICSELECTIONDAGINFO_H
