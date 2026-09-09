//===- DSPIC.cpp ----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dsPIC33 ABI as clang hands it to the backend (trellis L1f-b, session 86). The
// convention itself is the backend's, MEASURED from the pic30 cc1 at L1c
// (DSPICISelLowering.cpp's header); this file only decides what SHAPE each C type
// reaches the IR in, so that the backend's rule then places it as cc1 does:
//
//   * a scalar (integer, pointer, enum, _Bool, float) is passed and returned DIRECT
//     and NEVER `signext`/`zeroext`: cc1 moves a byte unextended (`call_bytes`:
//     `mov.b #97,w1`) and the callee extends it (`bytes`: `se w1,w1 / ze w2,w2`), so an
//     extension promise here would let the backend skip the callee's `se`/`ze`;
//   * a struct or union BY VALUE is coerced to `{i16, i16, ...}` of ceil(size/2) words,
//     which clang flattens into that many i16 arguments, each taking the lowest free
//     register in the backend's rule: `sv(struct S3 s, int y)` → w0, w1, w2 then y in w3
//     (cc1: `add.w w3,w1,w0`); `sv5(struct S5 s, int y)` → w0-w4 then w5 (cc1's
//     `call_sv5` fills w1-w4 from w0 and puts 4 in w5). The backend never sees `byval`,
//     which it refuses by design (L1c clause 3). ⚠ cc1 was measured on ALL-INT structs of
//     2, 3 and 5 words starting at w0 (the L1c domain row); a struct with a `long` member
//     or an odd byte size is this file's extrapolation — words, padded up;
//   * a struct or union RESULT is `sret` (a pointer in w0, returned in w0 — the backend's
//     L1c rule) for every size, as cc1 does for 4- and 6-byte results (`mk2`, `mk3`).
//
//===----------------------------------------------------------------------===//

#include "ABIInfoImpl.h"
#include "TargetInfo.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace clang::CodeGen;

namespace {

class DSPICABIInfo : public DefaultABIInfo {
public:
  DSPICABIInfo(CodeGenTypes &CGT) : DefaultABIInfo(CGT) {}

  ABIArgInfo classifyReturnType(QualType RetTy) const {
    if (RetTy->isVoidType())
      return ABIArgInfo::getIgnore();
    if (isAggregateTypeForABI(RetTy))
      return getNaturalAlignIndirect(RetTy, getDataLayout().getAllocaAddrSpace());
    if (const auto *ED = RetTy->getAsEnumDecl())
      RetTy = ED->getIntegerType();
    // Unextended: the value's own width, whatever the register carries above it.
    return ABIArgInfo::getDirect();
  }

  ABIArgInfo classifyArgumentType(QualType Ty) const {
    Ty = useFirstFieldIfTransparentUnion(Ty);

    if (isAggregateTypeForABI(Ty)) {
      if (CGCXXABI::RecordArgABI RAA = getRecordArgABI(Ty, getCXXABI()))
        return getNaturalAlignIndirect(Ty, getDataLayout().getAllocaAddrSpace(),
                                       RAA == CGCXXABI::RAA_DirectInMemory);
      uint64_t Bits = getContext().getTypeSize(Ty);
      if (Bits == 0)
        return ABIArgInfo::getIgnore();
      unsigned Words = (Bits + 15) / 16;
      llvm::Type *I16 = llvm::Type::getInt16Ty(getVMContext());
      llvm::SmallVector<llvm::Type *, 8> Elems(Words, I16);
      llvm::Type *Coerce = llvm::StructType::get(getVMContext(), Elems);
      return ABIArgInfo::getDirect(Coerce);
    }

    if (const auto *ED = Ty->getAsEnumDecl())
      Ty = ED->getIntegerType();
    return ABIArgInfo::getDirect();
  }

  void computeInfo(CGFunctionInfo &FI) const override {
    if (!getCXXABI().classifyReturnType(FI))
      FI.getReturnInfo() = classifyReturnType(FI.getReturnType());
    for (auto &I : FI.arguments())
      I.info = classifyArgumentType(I.type);
  }

  RValue EmitVAArg(CodeGenFunction &CGF, Address VAListAddr, QualType Ty,
                   AggValueSlot Slot) const override {
    return CGF.EmitLoadOfAnyValue(
        CGF.MakeAddrLValue(
            EmitVAArgInstr(CGF, VAListAddr, Ty, classifyArgumentType(Ty)), Ty),
        Slot);
  }
};

class DSPICTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  DSPICTargetCodeGenInfo(CodeGenTypes &CGT)
      : TargetCodeGenInfo(std::make_unique<DSPICABIInfo>(CGT)) {}
  void setTargetAttributes(const Decl *D, llvm::GlobalValue *GV,
                           CodeGen::CodeGenModule &M) const override;
};

} // namespace

// `__attribute__((interrupt))` reaches the backend as the string attribute
// "interrupt" (plus noinline); what the backend does with it is L1d's (BACKEND-PLAN.md),
// and until then it is carried and ignored.
void DSPICTargetCodeGenInfo::setTargetAttributes(
    const Decl *D, llvm::GlobalValue *GV, CodeGen::CodeGenModule &M) const {
  if (!D)
    return;
  // Session 90: `far`/`near` placement rides on DECLARATIONS too -- a use in another TU must
  // know the object is far (no 13-bit file forms), which is why CodeGenModule hands
  // declaration-only variables here as well. The backend reads the "far" global attribute
  // (section placement and the file-address refusal); "near" is the default, carried for
  // the record.
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (auto *GVar = dyn_cast<llvm::GlobalVariable>(GV)) {
      // trellis session 98: an `sfr` object sits at a silicon-fixed address in SFR space,
      // which IS near space -- pic30.c:4923, "it is also marked NEAR", and pic30.c builds
      // its own SFR refs with PIC30_NEAR_FLAG. The data model has no say over it, so this
      // arm comes BEFORE the model's chain and that chain becomes its `else`.
      // ⛔ PLACED HERE, OUTSIDE steps/models/model-edit.py's inserted text, and not inside
      // it: an edit that splits another script's `new` string makes that script's --revert
      // a silent no-op. steps/roundtrip.py exists to catch that and DID catch it when this
      // arm was first written into the middle of the chain.
      if (VD->hasAttr<DSPICSfrAttr>())
        GVar->addAttribute("near");
      else
      // trellis session 96 (follow-up 15): EXPLICIT beats IMPLICIT, and every global now gets
      // one or the other, so the placement decision is made in exactly one place. A user's
      // attribute is explicit; otherwise the data model supplies it, by the vendor's own rule
      // (pic30.c:21878): an aggregate takes the aggregate model, everything else the scalar one.
      // ⚠ Ours reads "aggregate" as array/struct/vector where GCC reads it as BLKmode, which
      // additionally makes a 2/4/8-byte array a scalar -- a placement difference at one shape,
      // recorded in steps/models/place.expected.first rather than chased.
      if (VD->hasAttr<DSPICFarAttr>())
        GVar->addAttribute("far");
      else if (VD->hasAttr<DSPICNearAttr>())
        GVar->addAttribute("near");
      else {
        QualType T = VD->getType();
        bool Aggregate = T->isArrayType() || T->isRecordType() || T->isVectorType();
        bool Near = Aggregate ? M.getTarget().hasFeature("small-aggregate")
                              : !M.getTarget().hasFeature("large-scalar");
        GVar->addAttribute(Near ? "near" : "far");
      }
      if (VD->getType().isConstQualified() &&
          M.getTarget().hasFeature("const-in-data"))
        GVar->addAttribute("dspic-const-in-data");
      // trellis session 96 (follow-up 14): the placement attributes ride to the TLOF, which
      // turns them into pic30 section attributes. See DSPICTargetMachine.cpp's pic30Attrs.
      if (const auto *SA = VD->getAttr<DSPICSpaceAttr>())
        GVar->addAttribute("dspic-space", SA->getSpace()->getName());
      if (VD->hasAttr<DSPICPersistentAttr>())
        GVar->addAttribute("dspic-persistent");
      if (VD->hasAttr<DSPICNoloadAttr>())
        GVar->addAttribute("dspic-noload");
      if (const auto *AA = VD->getAttr<DSPICAddressAttr>())
        GVar->addAttribute("dspic-address", std::to_string(AA->getAddr()));
    }
    return;
  }
  const FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
  llvm::Function *F = dyn_cast<llvm::Function>(GV);
  if (!FD || !F)
    return;
  // A far FUNCTION is carried as the "far" fn-attribute: the call reach, not yet lowered
  // (every call is `rcall` today; a link-time reach question, tagged).
  if (FD->hasAttr<DSPICFarAttr>())
    F->addFnAttr("far");
  // trellis session 96 (follow-up 20): and `near`, which was being DROPPED -- an explicit near on
  // a function must beat -mlarge-code the way an explicit near on a datum beats -mlarge-data.
  else if (FD->hasAttr<DSPICNearAttr>())
    F->addFnAttr("near");
  if (GV->isDeclaration())
    return;
  // trellis session 98: `user_init` rides as its own fn-attribute; DSPICAsmPrinter emits the
  // `.user_init` fragment that the linker and the C runtime turn into a call before main.
  if (FD->hasAttr<DSPICUserInitAttr>()) {
    F->addFnAttr("dspic-user-init");
    // ⛔ AND IT MUST SURVIVE. A user_init function is reachable ONLY through the linker's
    // `.user_init` section; nothing in the translation unit need call it. Without this, our clang
    // INLINED the fixture's `static` one away entirely and emitted no fragment at all -- measured,
    // 0 mentions in our output against 5 in cc1's. That is the very failure this change exists to
    // fix, reintroduced one level down. `used` keeps the out-of-line body.
    M.addUsedGlobal(F);
  }
  const auto *IA = FD->getAttr<DSPICInterruptAttr>();
  if (!IA)
    return;
  F->addFnAttr(llvm::Attribute::NoInline);
  F->addFnAttr("interrupt");
  // trellis session 96 (follow-up 13): the preprologue text rides to the backend as its own fn
  // attribute; DSPICFrameLowering::emitPrologue turns it into an INLINEASM at the top of the
  // entry block, ahead of the callee-saved pushes, which is where cc1 puts it.
  if (!IA->getPreprologue().empty())
    F->addFnAttr("dspic-preprologue", IA->getPreprologue());
}

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createDSPICTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<DSPICTargetCodeGenInfo>(CGM.getTypes());
}
