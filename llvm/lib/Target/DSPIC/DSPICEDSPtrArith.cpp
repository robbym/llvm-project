//===-- DSPICEDSPtrArith.cpp - carry EDS pointer arithmetic into the page --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 112, on the operator's ruling "match cc1, page:offset".
// Written-first expectation: steps/eds/XPAGEFIX.expected.first.
// The defect this fixes has an EXECUTED witness: steps/exec/xpageexec.c.
//
// ⛔ WHAT WAS WRONG. An `__eds__` (addrspace 2) pointer is a page:offset pair in one i32, and
// bit 15 of the offset is the HARDWARE's window select -- the EDS window is the address range
// 0x8000..0xFFFF (vendor guide DS-50003589D section 7.7, p.103, read as a rendered page). Adding a
// byte displacement as a FLAT 32-bit add carries out of bit 14 straight THROUGH the window bit and
// clears it, and an address with bit 15 clear is not a windowed access at all: it is a direct
// access to the low 32 K. So an IN-BOUNDS store to a straddling `space(eds)` array silently wrote
// into NEAR data. Measured on the MPLAB simulator at session 112: `&big[6051]` came out
// (page 3, offset 0x1022) where cc1 gives (page 3, offset 0x9022), and the store changed a near
// variable the program never assigns from 0x1234 to 0xBEEF.
//
// ⛔ WHY THE FIX IS AN IR PASS, AND WHY THE TWO OBVIOUS HOOKS DO NOT WORK. Both were rejected on
// measured grounds before any code was written (XPAGEFIX.expected.first, F0):
//
//   * ISD::PTRADD + TargetLowering::shouldPreservePtrArith -- the AMDGPU precedent -- discriminates
//     by pointer EVT, and this target's __prog__ (addrspace 1), __eds__ (2) and __pack_upper_byte
//     (4) pointers are ALL i32. The node carries no address space, so such a lowering could not
//     tell an EDS pointer from a linear packed-flash one and would corrupt the other two spaces.
//
//   * The load/store combines (combineEdsLoad / combineEdsStore) already know the address space,
//     but they see only memory operations. The pointer-as-VALUE forms -- `p + i` returned, and
//     `&big[i]`, which is the fixture's own observable -- never reach them.
//
// The address space survives in the TYPE, and only in the IR. So this runs there.
//
// ⛔ AND A CONSTANT INDEX IS SKIPPED BY CONSTRUCTION, NOT BY A SPECIAL CASE. Measured with
// -emit-llvm before the pass was written: a constant index on a global is a GEP CONSTANT
// EXPRESSION folded into the load (`load ... getelementptr inbounds nuw (i8, ptr addrspace(2)
// @bigs, i32 12102)`), while a runtime index is a GEP INSTRUCTION. This pass walks instructions,
// so the constant path keeps folding into the symbol's relocation -- which the LINKER resolves
// correctly across pages, in TWO instructions where cc1 spends fourteen. Session 112 measured that
// advantage and it must not be given away.
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/Utils/Local.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "dspic-eds-ptr-arith"

STATISTIC(NumEDSPtrArith, "Number of __eds__ GEPs given a paging carry");

// The port's convention: every behaviour change gets a hidden switch so an A/B is one flag away.
// ⛔ NOT static, and the name is the target's: DSPICISelLowering.cpp's edsPagedAdd reads the SAME
// option, because the paging carry is ONE RULE AT TWO SITES. This pass fixes every address a GEP
// computes; the `+2` between the two halves of an i32 is created in the DAG, where no IR pass can
// see it. One switch so that "the defect, on demand" means the whole defect and not half of it.
cl::opt<bool>
    DSPICEnableEDSPtrArith("dspic-eds-ptr-arith", cl::Hidden, cl::init(true),
                      cl::desc("Carry __eds__ pointer arithmetic into the page word "
                               "instead of through the hardware window-select bit"));

// The EDS address space. 1 is __prog__, 3 is __external__, 4 is __pack_upper_byte -- all of them
// LINEAR, and none of them may be touched here.
static constexpr unsigned EDSAddrSpace = 2;

// The offset field is 15 bits; bit 15 selects the window.
static constexpr uint32_t OffsetBits = 15;
static constexpr uint32_t OffsetMask = (1u << OffsetBits) - 1; // 0x7FFF
static constexpr uint32_t WindowBit = 1u << OffsetBits;        // 0x8000

// The one implementation. ⛔ TWO ENTRY POINTS ARE REQUIRED, and finding that out cost a build:
// this target registers BOTH a legacy DSPICPassConfig and a new-PM DSPICCodeGenPassBuilder, and
// `llc`/`clang` run the NEW-PM one. A pass added only to DSPICPassConfig::addIRPasses is dead
// code that never executes and never says so -- the first version of this row was exactly that,
// and the written-first expectation is what caught it (two rows were predicted to go red and did
// not). `-print-after-all` naming ObjCARCContractPass / AtomicExpandPass / GCLoweringPass -- all
// new-PM spellings -- is the check that settles which pipeline is live.
static bool runEDSPtrArith(Function &F);

namespace {

class DSPICEDSPtrArith : public FunctionPass {
public:
  static char ID;
  DSPICEDSPtrArith() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override { return runEDSPtrArith(F); }
  StringRef getPassName() const override {
    return "DSPIC __eds__ pointer arithmetic";
  }
};

char DSPICEDSPtrArith::ID = 0;

} // namespace

// base is page:offset in one i32. Return base + disp with the carry landing in the PAGE word.
//
//   page = base >> 16
//   off  = base & 0x7FFF                 -- strip the window bit
//   lin  = (page << 15) | off            -- the 31-bit LINEAR address
//   lin2 = lin + disp
//   res  = (lin2 >> 15) << 16 | (lin2 & 0x7FFF) | (page' != 0 ? 0x8000 : 0)
//
// ⛔ This is cc1's sequence in a different spelling, not a different rule. cc1 shifts both halves
// LEFT by one so the 15-bit field fills 16 bits, lets add/addc carry into the page word, and
// rotates back with `bit 15 := (page != 0)` -- the `bset _SR,#1` / sticky-Z / `rrc` idiom, banked
// verbatim at prints/l1f/eds/xpage-seq.txt. Converting to linear, adding, and converting back is
// the same function of the same inputs.
//
// ⚠ A BARE `__eds__` OBJECT IS CORRECT BY THIS FORMULA RATHER THAN BY A SPECIAL CASE. Such an
// object is placed NEAR, at page 0, with the window bit already clear; then lin == off, and any
// in-bounds displacement keeps page' == 0, so the window bit stays clear and the access stays
// direct. That is exactly why cc1's restored bit is `(page != 0)` and not a constant 1.
static Value *emitPagedAdd(IRBuilder<> &B, Value *Base, Value *Disp) {
  Type *I32 = B.getInt32Ty();
  Value *Page = B.CreateLShr(Base, 16, "eds.page");
  Value *Off = B.CreateAnd(Base, OffsetMask, "eds.off");
  Value *Lin = B.CreateOr(B.CreateShl(Page, OffsetBits), Off, "eds.lin");
  Value *Sum = B.CreateAdd(Lin, Disp, "eds.lin.add");
  Value *NewPage = B.CreateLShr(Sum, OffsetBits, "eds.page.new");
  Value *NewOff = B.CreateAnd(Sum, OffsetMask, "eds.off.new");
  Value *InWindow = B.CreateICmpNE(NewPage, ConstantInt::get(I32, 0));
  Value *Window = B.CreateSelect(InWindow, ConstantInt::get(I32, WindowBit),
                                 ConstantInt::get(I32, 0), "eds.window");
  Value *Hi = B.CreateShl(NewPage, 16);
  return B.CreateOr(B.CreateOr(Hi, NewOff), Window, "eds.ptr");
}

static bool runEDSPtrArith(Function &F) {
  if (!DSPICEnableEDSPtrArith || F.isDeclaration())
    return false;

  const DataLayout &DL = F.getParent()->getDataLayout();

  // Collect first: the transform erases the GEPs it replaces.
  SmallVector<GetElementPtrInst *, 8> Work;
  for (Instruction &I : instructions(F))
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
      if (GEP->getPointerAddressSpace() == EDSAddrSpace)
        Work.push_back(GEP);

  bool Changed = false;
  for (GetElementPtrInst *GEP : Work) {
    IRBuilder<> B(GEP);
    Value *Disp = emitGEPOffset(&B, DL, GEP);

    // A zero-offset GEP is a no-op; paying for a carry there would be a pure regression.
    if (auto *C = dyn_cast<ConstantInt>(Disp))
      if (C->isZero()) {
        GEP->replaceAllUsesWith(GEP->getPointerOperand());
        GEP->eraseFromParent();
        Changed = true;
        continue;
      }

    Type *PtrTy = GEP->getType();
    Value *Base = B.CreatePtrToInt(GEP->getPointerOperand(), B.getInt32Ty(), "eds.base");
    Value *Res = emitPagedAdd(B, Base, Disp);
    Value *NewPtr = B.CreateIntToPtr(Res, PtrTy);
    NewPtr->takeName(GEP);
    GEP->replaceAllUsesWith(NewPtr);
    GEP->eraseFromParent();
    ++NumEDSPtrArith;
    Changed = true;
  }
  return Changed;
}

FunctionPass *llvm::createDSPICEDSPtrArithPass() {
  return new DSPICEDSPtrArith();
}

PreservedAnalyses DSPICEDSPtrArithPass::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  if (!runEDSPtrArith(F))
    return PreservedAnalyses::all();
  // The pass replaces GEPs with integer arithmetic and inttoptr; it does not change the CFG.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
