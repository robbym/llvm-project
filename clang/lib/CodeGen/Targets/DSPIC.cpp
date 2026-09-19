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
// trellis session 127: the address-over-rides-near warning is emitted from CodeGen, as
// upstream AArch64.cpp emits its own; this is the header that route needs.
#include "clang/Basic/DiagnosticFrontend.h"
#include "llvm/ADT/StringExtras.h"

using namespace clang;
using namespace clang::CodeGen;

// trellis session 99: cc1's DEFAULT placement rule for an aggregate, MEASURED and not assumed.
// NEAR iff the object's size is one the machine has a load for AND its alignment lets that load
// be used: size in {1, 2, 4, 8}, and alignment >= 2 unless the size is 1.
// The ladder it is fitted to, the two independent signals it was read on, and the single-axis
// alignment test that picked alignment out of the candidates are in
// steps/aggnear/AGGNEAR.expected.first. ⚠ It is a FIT to 23 measured objects, not a reading of
// pic30's source.
static bool dspicAggregateFitsNear(const ASTContext &Ctx, QualType T) {
  if (T->isIncompleteType())
    return false;   // no size to test, and the far form is always legal
  uint64_t Size = Ctx.getTypeSizeInChars(T).getQuantity();
  uint64_t Align = Ctx.getTypeAlignInChars(T).getQuantity();
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return false;
  return Size == 1 || Align >= 2;
}

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
      // ⛔ trellis session 99: CanBeFlattened=false. The default TRUE turns this struct into N
      // separate one-word IR arguments, and the backend then cannot tell that they are one
      // argument: it reverses them with everything else when it lays out the stack (so the words
      // arrive backwards) and it never applies the register alignment cc1 applies to a P-word
      // argument. Measured against cc1: `g8(1..8, struct{1,2,3,4})` put 03 04 below 01 02, and
      // `b2(int, struct w2)` used w1,w2 where cc1 uses w2,w3.
      return ABIArgInfo::getDirect(Coerce, /*Offset=*/0, /*Padding=*/nullptr,
                                   /*CanBeFlattened=*/false);
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
    // ⛔ trellis session 99: an AGGREGATE cannot go through the generic emitter at all.
    // EmitVAArgInstr asserts `isDirect() && !getCoerceToType()`, and classifyArgumentType above
    // returns getDirect(<N x i16>) for every by-value struct (session 86) -- so every struct
    // va_arg ABORTED the front end. Sixteen of GCC's torture suite's compile failures were this.
    //
    // MEASURED FROM cc1 (steps/varargs/agg.c): the list steps DOWN by the size rounded up to a
    // word and the object sits AT the new pointer -- the SAME rule the scalars obey, with the
    // aggregate travelling in the list BY VALUE. An odd-sized struct is padded at the HIGH end,
    // so its first byte is at the LOW address of its slot, which falls out of taking the new
    // pointer as the object's address.
    //
    // ⚠ The scalar path below is deliberately untouched: it is correct, its output is pinned by
    // steps/varargs/types-compare.sh, and routing it through this arithmetic instead would make
    // the backend's own LowerVAARG dead and rewrite every fixture in this port.
    if (isAggregateTypeForABI(Ty)) {
      CharUnits SlotSize = CharUnits::fromQuantity(2);
      TypeInfoChars TI = getContext().getTypeInfoInChars(Ty);
      CharUnits Step = TI.Width.alignTo(SlotSize);

      Address Ap = VAListAddr;
      if (Ap.getElementType() != CGF.Int8PtrTy)
        Ap = Ap.withElementType(CGF.Int8PtrTy);
      llvm::Value *Cur = CGF.Builder.CreateLoad(Ap, "ap.cur");
      Address CurAddr(Cur, CGF.Int8Ty, SlotSize);
      Address NextAddr = CGF.Builder.CreateConstInBoundsByteGEP(
          CurAddr, CharUnits::fromQuantity(-Step.getQuantity()), "ap.next");
      llvm::Value *Next = NextAddr.emitRawPointer(CGF);
      CGF.Builder.CreateStore(Next, Ap);

      Address Obj(Next, CGF.ConvertTypeForMem(Ty), TI.Align);
      return CGF.EmitLoadOfAnyValue(CGF.MakeAddrLValue(Obj, Ty), Slot);
    }
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
        // trellis session 99, the operator's "match vendor". The three configurations were
        // each measured through cc1 (steps/aggnear/ask.cc1.*.s) and they are three different
        // rules, not one rule with a knob:
        //   -msmall-data   every object near, aggregate or not, at every size
        //   default        scalars near; an aggregate near IFF dspicAggregateFitsNear
        //   -mlarge-data   every object far
        bool Near;
        if (!Aggregate)
          Near = !M.getTarget().hasFeature("large-scalar");
        else if (M.getTarget().hasFeature("small-aggregate"))
          Near = true;
        else if (M.getTarget().hasFeature("large-scalar"))
          Near = false;
        else
          Near = dspicAggregateFitsNear(M.getContext(), T);
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
      // trellis session 109: the placement attributes, each one pic30 section attribute in the
      // TLOF (DSPICTargetMachine.cpp pic30Attrs), each measured from cc1 (var.cc1.s).
      if (VD->hasAttr<DSPICKeepAttr>())
        GVar->addAttribute("dspic-keep");
      if (VD->hasAttr<DSPICUnorderedAttr>())
        GVar->addAttribute("dspic-unordered");
      if (const auto *PA = VD->getAttr<DSPICPriorityAttr>())
        GVar->addAttribute("dspic-priority", std::to_string(PA->getLevel()));
      if (const auto *FA = VD->getAttr<DSPICFillupperAttr>())
        GVar->addAttribute("dspic-fillupper", std::to_string(FA->getValue()));
      if (const auto *SA = VD->getAttr<DSPICSfrAttr>())
        if (SA->getAddr() != 0)
          GVar->addAttribute("dspic-sfr-address", std::to_string(SA->getAddr()));
      // ⛔ page and space(dma) objects are FAR, whatever the model said, ON THE ASSEMBLER'S
      // AUTHORITY: both the GPL `as` and the vendor's shipped xc-dsc-as REFUSE `page`+`near`
      // and `dma`+`near` ("invalid attribute combination", prints/l1f/frontend/as-probe.txt and
      // the refutation pass's 22 probes) -- cc1 emits `data,page,near` for a scalar page object,
      // a line its OWN driver cannot assemble, and addresses a dma scalar far
      // (`mov #_d_s,w0 ; inc.w [w0],[w0]`). ⚠ cc1's CODEGEN treats a page scalar near (`inc _p_s`)
      // while its section cannot be: ours departs there, COSTED. ⚠ `reverse(N)` is NOT here:
      // reverse+near assembles and is cc1's own placement for a reverse scalar
      // (`reverse(64),data,near`); the banked `reverse(64),bss` with no near was a 32-byte array
      // the aggregate rule places far. Three rounds on this joint -- FRONTEND.expected.first
      // C8, C10, C11, C13, C14: two of them my own rules read off the wrong object, and the
      // third my withdrawal of a right one on a form that pins nothing.
      if (const auto *RA = VD->getAttr<DSPICReverseAttr>())
        GVar->addAttribute("dspic-reverse", std::to_string(RA->getAlign()));
      const auto *SpA = VD->getAttr<DSPICSpaceAttr>();
      // trellis session 110: an EDS object, by either spelling. It is FAR (cc1 emits `bss,eds`
      // with no `near`, measured at all three memory models), and it carries `page` exactly when
      // the object cannot straddle a 32K boundary -- by the rule FITTED from cc1 in
      // steps/eds/page-ladder.sh, which is VERBATIM session 99's near/far rule, so the predicate
      // is reused rather than duplicated.
      bool IsEds = VD->hasAttr<DSPICEdsAttr>() || (SpA && SpA->getSpace()->isStr("eds"));
      if (IsEds) {
        GVar->addAttribute("dspic-space", "eds");
        if (dspicAggregateFitsNear(M.getContext(), VD->getType()))
          GVar->addAttribute("dspic-page");
      }
      bool ForcedFar = IsEds || VD->hasAttr<DSPICPageAttr>() ||
                       (SpA && SpA->getSpace()->isStr("dma"));
      // ⛔ trellis session 127: AN address() OUTSIDE THE NEAR DATA RANGE CLEARS `near`.
      // pic30.c:2984 -- boundary 0x1FFF (0xFFFF under pic30_isa32_target()), START ADDRESS ONLY
      // and size-independent: measured, a `long long` at 0x1FFC ENDS at 0x2004 and cc1 keeps near.
      //
      // ⛔ IT IS HERE AND NOT IN pic30Attrs, WHERE THE SESSION-127 PREP PUTS IT, BUT NOT FOR THE
      // REASON FIRST WRITTEN -- a refutation pass broke that one, and steps/frontend/
      // addrnear-edit.py records it as broken. `near` is read at twelve sites in seven files (the
      // section line in pic30Attrs, the section-name arms, isNearFileSymbol and SelectFileAddr in
      // DSPICISelDAGToDAG.cpp, session 94's remat, the byte-file peephole); clearing it in
      // pic30Attrs alone moves ONE. It would NOT be silent -- the 13-bit file form carries a
      // FILE REG relocation and the pic30 linker range-checks it at exactly 8192 ("Cannot access
      // symbol with file register addressing") -- so the ground is not danger but these three: a
      // compiler should not emit an instruction it knows the linker will reject; this is the only
      // site that can still tell an EXPLICIT `near` from the data model's default, which is what
      // cc1 warns on; and ForcedFar above already clears near for eds/page/dma, so this is a
      // fourth condition and not a new mechanism. Told `far` at address(0x14000), ours ALREADY
      // matched cc1 on both observables -- that control is why one attribute is enough.
      //
      // ⚠ It is OR-ed in below rather than written into the statement above, which belongs to
      // session 110's edsplace-edit.py: splitting another script's `new` string makes its
      // --revert a silent no-op, and steps/roundtrip.py caught exactly that here.
      // ⚠ Domain: addrspace-0 VARIABLES. A function's address() section carries no near, and
      // __prog__/space(eds) objects are already far. ⚠ An ODD address() never reaches here (cc1
      // discards it at any width), so 0x1FFF is unaskable and a strict and a non-strict
      // comparison are extensionally equal over the reachable domain. ⚠ The ISA32 boundary is
      // deliberately unwritten: every HAS_ISA32V0 device is refused at validateTarget, so that
      // branch would be a second, unexecutable model of this rule. COSTED, far side named.
      const auto *AddrA = VD->getAttr<DSPICAddressAttr>();
      bool AddrOutsideNear = AddrA && AddrA->getAddr() > 0x1FFFu;
      // cc1 warns only when `near` was WRITTEN; the data model's default is silent. Emitted from
      // CodeGen as upstream AArch64.cpp does, and guarded on the definition so a declaration of
      // the same object does not report it twice.
      if (AddrOutsideNear && VD->hasAttr<DSPICNearAttr>() &&
          VD->isThisDeclarationADefinition())
        M.getDiags().Report(VD->getLocation(), diag::warn_dspic_address_overrides_near) << VD;
      ForcedFar = ForcedFar || AddrOutsideNear;
      if (ForcedFar) {
        GVar->setAttributes(GVar->getAttributes().removeAttribute(GVar->getContext(), "near"));
        GVar->addAttribute("far");
        if (VD->hasAttr<DSPICPageAttr>())
          GVar->addAttribute("dspic-page");
      }
      // ⛔ trellis session 128: THREE ATTRIBUTES MAKE cc1 IGNORE A WRITTEN SECTION NAME, and
      // ours honoured it in silence -- so the object landed somewhere the vendor compiler would
      // not have put it, with no word from the compiler, the assembler or the linker. The set is
      // reverse(N), unordered and space(auto_psv), MEASURED from cc1 over every attribute and
      // every space argument the device accepts (steps/frontend/discard-ask.sh), and it is the
      // vendor's `if (r || u || psv)` where `psv` is IDENT_CONST -- which matches the `auto_psv`
      // space argument and NOT the `psv` one. space(psv) KEEPS its name; mutant MD4 is the
      // mistake of reading that C variable's name for its meaning, and it dies on one cell.
      //
      // ⛔ THE SECTION STRING IS LEFT ALONE AND A MARKER IS ADDED INSTEAD. The first landing of
      // this row cleared it here (`GVar->setSection("")`), which is semantically "the name is
      // ignored" and passed every row of the comparer and every mutant -- and was still wrong:
      // dspicDecider finds siblings by `hasSection() && getSection() == Name`, so an erased
      // object LEFT THE CONFLICT SCAN and a space(auto_psv)+plain pair sharing a name went from
      // REFUSED (cc1's answer) to silently accepted, one object in program memory and the other
      // in data. The ask had already measured the right answer: cc1 warns AND THEN REPORTS THE
      // CONFLICT ANYWAY. It discards the name when it COMPUTES it, not by forgetting there was
      // one. sectype-compare is what named it, 40/0 -> 38/2.
      //
      // The marker also keeps the trigger set in ONE place: the TLOF reads the marker and never
      // these three attributes, so there is no second copy to drift.
      bool TrigReverse = VD->hasAttr<DSPICReverseAttr>();
      bool TrigUnordered = VD->hasAttr<DSPICUnorderedAttr>();
      bool TrigAutoPsv = SpA && SpA->getSpace()->isStr("auto_psv");
      if (GVar->hasSection() && (TrigReverse || TrigUnordered || TrigAutoPsv)) {
        // ⛔ THE TWO VENDOR SITES ARE NOT ONE CONDITION, and the session-128 PREP's warning about
        // that is RIGHT -- on an axis neither the prep nor this row's first landing considered.
        // They compare the written name against DIFFERENT STRINGS:
        //     pic30.c:3203   strcmp(name, pic30_unique_section_name(decl))   -> WARN
        //     pic30.c:3422   strcmp(name, pic30_default_section)             -> DISCARD
        // and `pic30_default_section` is the one-character string "*" (pic30.c:402). So for the
        // written name "*" cc1 WARNS AND KEEPS. Measured: `space(auto_psv), section("*")` is
        // `*,psv,page` in cc1, which the banked pre-row binary MATCHED and the first landing of
        // this row broke to `.const,psv,page` -- a regression, in the direction the standing
        // vendor rule binds, found by a refutation pass.
        //
        // ⚠ It bites ONLY the auto_psv arm, and that is the vendor ladder rather than a guess:
        // after 3422, `if (u) ... else if (a) ... else if (r) ...` all land on the generated name
        // WHATEVER the surviving name is, and only the psv case falls through to
        // `else if (pszSectionName)` and keeps it. Measured on all three triggers.
        //
        // The warning is UNCONDITIONAL because cc1's is: 3203 compares against the unique name,
        // which a written name never equals, so cc1 warns in the "*" cases too (W=2, measured).
        // ⚠ cc1 warns twice, from two call sites; ours warns once and the count is not asserted.
        // Guarded on the definition so a declaration of the same object does not report it twice
        // -- session 127's guard, same reason.
        if (VD->isThisDeclarationADefinition())
          M.getDiags().Report(VD->getLocation(), diag::warn_dspic_section_name_ignored) << VD;
        if (TrigReverse || TrigUnordered || GVar->getSection() != "*")
          GVar->addAttribute("dspic-ignore-section-name");
      }
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
  // trellis session 109: the function-side facts. Placement (address / noload / keep / priority)
  // becomes a per-function section with cc1's attributes (fn.cc1.s); the ISR facts are read by
  // DSPICFrameLowering (shadow, save, context) and DSPICAsmPrinter (irq / altirq).
  if (const auto *AA = FD->getAttr<DSPICAddressAttr>())
    F->addFnAttr("dspic-address", std::to_string(AA->getAddr()));
  if (FD->hasAttr<DSPICNoloadAttr>())
    F->addFnAttr("dspic-noload");
  if (FD->hasAttr<DSPICKeepAttr>())
    F->addFnAttr("dspic-keep");
  if (const auto *PA = FD->getAttr<DSPICPriorityAttr>())
    F->addFnAttr("dspic-priority", std::to_string(PA->getLevel()));
  if (FD->hasAttr<DSPICShadowAttr>())
    F->addFnAttr("dspic-shadow");
  if (FD->hasAttr<DSPICContextAttr>())
    F->addFnAttr("dspic-context");
  if (const auto *IQ = FD->getAttr<DSPICIrqAttr>())
    F->addFnAttr("dspic-irq", std::to_string(IQ->getNumber()));
  if (const auto *IQ = FD->getAttr<DSPICAltIrqAttr>())
    F->addFnAttr("dspic-altirq", std::to_string(IQ->getNumber()));
  {
    // the save list, from the modifier and from the bare attribute alike, as the globals' IR
    // names in WRITTEN order (the frame lowering reverses, as cc1 does)
    std::string Names;
    auto AddVars = [&](ArrayRef<Expr *> Vars) {
      for (const Expr *E : Vars) {
        const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
        const auto *VD = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
        if (!VD)
          continue;
        if (!Names.empty())
          Names += ",";
        Names += M.getMangledName(GlobalDecl(VD)).str();
      }
    };
    if (const auto *IA = FD->getAttr<DSPICInterruptAttr>())
      AddVars(ArrayRef<Expr *>(IA->save_begin(), IA->save_size()));
    if (const auto *SA = FD->getAttr<DSPICSaveAttr>())
      AddVars(ArrayRef<Expr *>(SA->vars_begin(), SA->vars_size()));
    if (!Names.empty())
      F->addFnAttr("dspic-save", Names);
  }
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
  // trellis session 98: only the SUPPRESSION travels. The save is the default for a handler
  // that says nothing, so `auto_psv` needs no string -- it asks for what it would get anyway.
  if (FD->hasAttr<DSPICNoAutoPsvAttr>())
    F->addFnAttr("dspic-no-auto-psv");
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
