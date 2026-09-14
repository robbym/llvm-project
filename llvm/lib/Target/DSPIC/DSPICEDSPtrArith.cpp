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
#include "llvm/IR/GlobalVariable.h"
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
// session 114 (post-close): the pair -> LINEAR conversion, at ONE site. `emitPagedAdd` computed
// this inline and the pointer-difference rewrite needs the same function of the same input; two
// copies of one formula is how combineEdsLoad and combineProgLoad came to share two defects.
//
//   page = base >> 16 ; off = base & 0x7FFF (the window bit stripped) ; lin = page << 15 | off
//
// ⚠ The window bit is DISCARDED here, not preserved: it is the hardware's window-select flag, not
// part of the address, and a placed object carries it set on every offset. Keeping it would add a
// constant 0x8000 to both operands of a difference -- harmless there and wrong everywhere else.
static Value *emitToLinear(IRBuilder<> &B, Value *Base) {
  Value *Page = B.CreateLShr(Base, 16, "eds.page");
  Value *Off = B.CreateAnd(Base, OffsetMask, "eds.off");
  return B.CreateOr(B.CreateShl(Page, OffsetBits), Off, "eds.lin");
}

static Value *emitPagedAdd(IRBuilder<> &B, Value *Base, Value *Disp) {
  Type *I32 = B.getInt32Ty();
  Value *Lin = emitToLinear(B, Base);
  Value *Sum = B.CreateAdd(Lin, Disp, "eds.lin.add");
  Value *NewPage = B.CreateLShr(Sum, OffsetBits, "eds.page.new");
  Value *NewOff = B.CreateAnd(Sum, OffsetMask, "eds.off.new");
  Value *InWindow = B.CreateICmpNE(NewPage, ConstantInt::get(I32, 0));
  Value *Window = B.CreateSelect(InWindow, ConstantInt::get(I32, WindowBit),
                                 ConstantInt::get(I32, 0), "eds.window");
  Value *Hi = B.CreateShl(NewPage, 16);
  return B.CreateOr(B.CreateOr(Hi, NewOff), Window, "eds.ptr");
}

// session 114 (post-close): is V `trunc (ptrtoint <addrspace 2 ptr>)`? clang lowers `a - b` to
// exactly that on both operands, then one `sub` at ptrdiff width and an exact `ashr` for the
// element size. The PAIR value is handed back so the caller can convert it.
static Value *edsPtrDiffOperand(Value *V) {
  auto *T = dyn_cast<TruncInst>(V);
  if (!T)
    return nullptr;
  auto *P = dyn_cast<PtrToIntInst>(T->getOperand(0));
  if (!P || P->getPointerOperand()->getType()->getPointerAddressSpace() != EDSAddrSpace)
    return nullptr;
  return P;
}

// ⛔ trellis session 112: IS THIS BASE AT PAGE 0, KNOWABLY? A bare `__eds__` object -- one without
// `space(eds)` -- is placed in ordinary data, where the window bit is already clear, so the paging
// carry computes the same address a flat add does and costs 11 -> 23 instructions to do it.
//
// ⛔ THAT IS THE LINKER'S ANSWER, NOT AN ARGUMENT FROM THE SCRIPT. The device's own .gld gives ONE
// data region (0x1000 + 0x1F000) shared by ordinary and EDS data, and caps ordinary data NOWHERE in
// its MEMORY block. Asked directly and bisected (prints/l1f/eds/nearcarry.txt): the largest bare
// `__eds__` array that links is 28638 bytes at 0x01002..0x07FDF, and one byte more is a LINK ERROR.
// Ordinary data tops out at 0x7FFF -- one below the EDS window -- and the toolchain REFUSES rather
// than crossing. So no in-bounds index on such an object can reach bit 15.
//
// ⛔ THE ATTRIBUTE IS THE PLACEMENT'S OWN. `"dspic-space"="eds"` is the string
// DSPICTargetMachine.cpp reads to put `,eds` on the section; re-deriving "is this in EDS" a second
// way invites the two to disagree, and a disagreement here is a miscompile.
//
// ⚠ Conservative in both directions that matter: an unknown base (a pointer PARAMETER, a computed
// address, an external declaration whose space we cannot see) returns false and pays the carry.
static bool baseIsKnownPageZero(const Value *Ptr) {
  const Value *V = Ptr->stripPointerCasts()->stripInBoundsConstantOffsets();
  const auto *GV = dyn_cast<GlobalVariable>(V);
  if (!GV)
    return false;
  // A declaration is defined elsewhere and may carry space(eds) there; do not guess.
  if (GV->isDeclaration())
    return false;
  return !(GV->hasAttribute("dspic-space") &&
           GV->getAttribute("dspic-space").getValueAsString() == "eds");
}

static bool runEDSPtrArith(Function &F) {
  if (!DSPICEnableEDSPtrArith || F.isDeclaration())
    return false;

  const DataLayout &DL = F.getParent()->getDataLayout();

  // Collect first: the transform erases the GEPs it replaces.
  SmallVector<BinaryOperator *, 4> Diffs;
  for (Instruction &I : instructions(F))
    if (auto *BO = dyn_cast<BinaryOperator>(&I))
      if (BO->getOpcode() == Instruction::Sub && edsPtrDiffOperand(BO->getOperand(0)) &&
          edsPtrDiffOperand(BO->getOperand(1)))
        Diffs.push_back(BO);

  SmallVector<GetElementPtrInst *, 8> Work;
  for (Instruction &I : instructions(F))
    if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
      if (GEP->getPointerAddressSpace() == EDSAddrSpace &&
          // trellis session 112: a base that is knowably at page 0 keeps the flat add, which is
          // the SAME address there and eleven instructions cheaper. See baseIsKnownPageZero.
          !baseIsKnownPageZero(GEP->getPointerOperand()))
        Work.push_back(GEP);

  bool Changed = false;

  // ⛔ session 114 (post-close): the difference is taken on LINEAR addresses. The operands arrive
  // as `trunc(ptrtoint p)` -- the OFFSET WORD alone -- and subtracting those discards the page,
  // which is the whole defect (steps/exec/edsdiff.c: -385 where 15999 is owed, across an array the
  // linker straddled). ⚠ The SUB is replaced, never the truncs: a trunc may have other users and
  // rewriting it in place would change their values too.
  // ⚠ The subtraction is done at 32 bits and truncated to the difference's own type, which is what
  // cc1 does -- its ptrdiff_t here is 16 bits, measured (steps/eds/ptrarith-ask.sh), so this is not
  // a widening. Correct for any distance representable in that type, which is every distance inside
  // one `space(eds)` object: cc1 caps such an object at 32 767 bytes.
  for (BinaryOperator *Sub : Diffs) {
    IRBuilder<> B(Sub);
    Value *LA = emitToLinear(B, edsPtrDiffOperand(Sub->getOperand(0)));
    Value *LB = emitToLinear(B, edsPtrDiffOperand(Sub->getOperand(1)));
    Value *D = B.CreateTrunc(B.CreateSub(LA, LB, "eds.diff"), Sub->getType());
    Sub->replaceAllUsesWith(D);
    Sub->eraseFromParent();
    Changed = true;
  }

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
