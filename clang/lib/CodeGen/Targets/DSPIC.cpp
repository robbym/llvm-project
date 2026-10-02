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
#include "clang/Basic/DiagnosticSema.h" // trellis session 144: warn_dspic_fillupper_ignored, given here now
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
  // ⛔ trellis session 144 (ITEM 1): A VARIABLE'S dsPIC FACTS ARE FINAL ONLY AT THE END OF THE TRANSLATION UNIT. C,
  // and C++ at file scope, emit a strong definition as it is parsed, and the vendor merges a declaration written AFTER
  // it (trc steps/frontend/LATE-L.banked.txt). So each variable is recorded with the most recent declaration it saw,
  // emitTargetGlobals re-derives one a later declaration reached, and every warning of the variable path is given
  // there, once, from the final view -- a definition-time view could warn for a space a later declaration replaces
  // (`space(auto_psv), shared` and then `extern __attribute__((space(data)))`: the vendor is silent, and a warning
  // given at the definition would refuse the unit under -Werror). ⚠ The price, measured: a unit an UNRECOVERABLE error
  // stops before its end gives none of the three -- where the vendor warns `name` and then refuses, ours gives the
  // error alone (trc steps/frontend/APSV.expected.first addendum 26: six of the row's cells, RA-S-plainobj-first-sy+b/d/x
  // and RB-R4-noload/persist/reverse64-F, a section-type conflict clang raises during CodeGen); a warning -Werror makes
  // an error is recoverable, and the three are given (refutation pass B, W7). A variable is re-derived only when its
  // view CHANGED, from the declaration it was recorded with: an attribute-free later declaration that completes an
  // array's type must not move a declaration-only object from far to near (pass B, X3).
  struct DspicSeen {
    const VarDecl *VD;
    std::string View;
  };
  mutable llvm::MapVector<const VarDecl *, DspicSeen> DspicVars;
  mutable llvm::MapVector<const VarDecl *, SmallVector<std::function<void(DiagnosticsEngine &)>, 1>>
      DspicWarnings;
  void emitTargetGlobals(CodeGen::CodeGenModule &CGM) const override;
  void setTargetAttributes(const Decl *D, llvm::GlobalValue *GV,
                           CodeGen::CodeGenModule &M) const override;
  // ⛔ trellis session 139: the two ABI facts the backend's `__c30_signature` is made of --
  // DSPICAsmPrinter::emitEndOfAsmFile composes cc1's three words from them. They are stated HERE
  // because the IR cannot carry them: by the time the backend runs, a C `double` IS `float`, and a
  // translation unit with no function carries no language and no target feature at all (trc
  // steps/frontend/SIG5.banked.txt ARM L). The WIDTHS, not the flags that set them: the words
  // must describe the object that was actually built. Behaviour Error: two modules that disagree
  // on an ABI fact cannot become one object whose signature is true of both.
  // ⛔ trellis session 140: the size_t width is ALSO TargetLibraryInfo's size_t (getSizeTSize reads
  // this flag), which types the length of every library call the backend and the optimizer build.
  // Under -mlarge-arrays it is 32, as the vendor's is (D6); until then ours stayed 16 (SIG5 ARM Z).
  void emitTargetMetadata(CodeGen::CodeGenModule &CGM,
                          const llvm::MapVector<GlobalDecl, StringRef> &) const override {
    const ASTContext &Ctx = CGM.getContext();
    CGM.getModule().addModuleFlag(llvm::Module::Error, "dspic-size-t-width",
                                  uint32_t(Ctx.getTypeSize(Ctx.getSizeType())));
    CGM.getModule().addModuleFlag(llvm::Module::Error, "dspic-double-width",
                                  uint32_t(Ctx.getTypeSize(Ctx.DoubleTy)));
  }
};

} // namespace

// ⛔ trellis session 144 (ITEM 1): THE VENDOR'S VIEW OF A VARIABLE -- the space it finds and the auto_psv family --
// read off the merged list Sema keeps on the most recent declaration (DSPICMergedAttrs: SemaDecl.cpp,
// mergeDeclAttributes, says how it is built). With one declaration there is no list and its own attributes are read:
// its LAST space, which SemaDeclAttr.cpp's handleDSPICSpaceAttr makes the vendor's. `Space` is a space() attribute
// written somewhere among the declarations whose space is that one, so the code below reads it as it read the
// definition's first.
namespace {
struct DspicVendorView {
  const DSPICSpaceAttr *Space = nullptr;
  bool Noload = false, Persistent = false, Shared = false;
  std::optional<unsigned> Fill;     // the seventh build: the merged list's first fillupper (the vendor's lookup)
};
} // namespace
static DspicVendorView dspicVendorView(const VarDecl *VD) {
  DspicVendorView V;
  const auto *MA = VD->getMostRecentDecl()->getAttr<DSPICMergedAttrsAttr>();
  if (!MA) {
    for (const auto *Sp : VD->specific_attrs<DSPICSpaceAttr>())
      V.Space = Sp;
    V.Noload = VD->hasAttr<DSPICNoloadAttr>();
    V.Persistent = VD->hasAttr<DSPICPersistentAttr>();
    V.Shared = VD->hasAttr<DSPICSharedAttr>();
    if (const auto *FA = VD->getAttr<DSPICFillupperAttr>())
      V.Fill = FA->getValue();
    return V;
  }
  StringRef Name;
  for (StringRef E : MA->entries()) {
    if (Name.empty() && E.starts_with("space(") && E.ends_with(")"))
      Name = E.drop_front(6).drop_back(1);
    // a fillupper entry is `fillupper(N)`, N its value in decimal (SemaDecl.cpp, dspicParsedKey)
    // the tenth build: an entry whose NAME is fillupper -- not one whose string argument holds the text
    // (refutation pass E's F1-F8: `deprecated("fillupper(34)")` was read as one); a fallback list prints it whole
    if (StringRef FE = E; !V.Fill && (FE.consume_front("fillupper(") ||
                                      (FE.consume_front("__attribute__((") && FE.consume_front("fillupper(")))) {
      unsigned Val;
      if (!FE.take_until([](char C) { return C == ')'; }).getAsInteger(10, Val))
        V.Fill = Val;
    }
    V.Noload = V.Noload || E == "noload";
    V.Persistent = V.Persistent || E == "persistent";
    V.Shared = V.Shared || E == "shared";
  }
  if (!Name.empty())
    for (const VarDecl *R : VD->redecls())
      for (const auto *Sp : R->specific_attrs<DSPICSpaceAttr>())
        if (!V.Space && Sp->getSpace()->getName() == Name)
          V.Space = Sp;
  return V;
}

// the view as a key: what the record compares at the end of the unit
static std::string dspicViewKey(const DspicVendorView &V) {
  return (V.Space ? V.Space->getSpace()->getName().str() : std::string("-")) + (V.Noload ? "|N" : "|") +
         (V.Persistent ? "P" : "") + (V.Shared ? "S" : "") + (V.Fill ? "|F" + std::to_string(*V.Fill) : "");
}

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
      // ⛔ trellis session 144 (ITEM 1): the vendor's view, and the record emitTargetGlobals re-reads (the class says
      // why). A declaration's call after the definition's does not replace the definition's record.
      const DspicVendorView DspicView = dspicVendorView(VD);
      const VarDecl *DspicCanon = VD->getCanonicalDecl();
      {
        auto DspicIt = DspicVars.find(DspicCanon);
        if (VD->isThisDeclarationADefinition() || DspicIt == DspicVars.end() ||
            !DspicIt->second.VD->isThisDeclarationADefinition()) {
          DspicVars[DspicCanon] = {VD, dspicViewKey(DspicView)};
          DspicWarnings[DspicCanon].clear();
        }
      }
      auto DspicWarnLater = [this, DspicCanon](std::function<void(DiagnosticsEngine &)> W) {
        DspicWarnings[DspicCanon].push_back(std::move(W));
      };
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
      // ⛔ trellis session 144 (ITEM 1): THE SPACE AND THE FAMILY ARE THE VENDOR'S VIEW'S (dspicVendorView), written over
      // what the chain above read off the definition alone -- its FIRST space and its own and earlier declarations'
      // flags; the statements there stay as written (models/model-edit.py's text).
      GVar->setAttributes(GVar->getAttributes()
                              .removeAttribute(GVar->getContext(), "dspic-space")
                              .removeAttribute(GVar->getContext(), "dspic-persistent")
                              .removeAttribute(GVar->getContext(), "dspic-noload"));
      if (const auto *SA = DspicView.Space)
        GVar->addAttribute("dspic-space", SA->getSpace()->getName());
      if (DspicView.Persistent)
        GVar->addAttribute("dspic-persistent");
      if (DspicView.Noload)
        GVar->addAttribute("dspic-noload");
      // trellis session 109: the placement attributes, each one pic30 section attribute in the
      // TLOF (DSPICTargetMachine.cpp pic30Attrs), each measured from cc1 (var.cc1.s).
      if (VD->hasAttr<DSPICKeepAttr>())
        GVar->addAttribute("dspic-keep");
      // trellis session 141: preserved / update / shared, each a pic30 section flag (the TLOF's
      // pic30Attrs, in the vendor's positions).
      if (VD->hasAttr<DSPICPreservedAttr>())
        GVar->addAttribute("dspic-preserved");
      if (VD->hasAttr<DSPICUpdateAttr>())
        GVar->addAttribute("dspic-update");
      if (DspicView.Shared) {       // trellis session 144: from the vendor's view
        GVar->addAttribute("dspic-shared");
        // A const in program memory keeps the SHARED `.const`: the vendor's
        // `.const,psv,page,shared` (ATTR1A V28). Its space is named here -- space(auto_psv), the
        // default const model's -- so the TLOF's per-object chain gives it that base and those
        // flags. ⚠ Done here and not by a new arm before the TLOF's ConstSection return: that
        // return sits inside models/model-edit.py's text, and the first spelling of this row
        // split it (roundtrip.py).
        if (VD->getType().isConstQualified() && !GVar->hasAttribute("dspic-space") &&
            !M.getTarget().hasFeature("const-in-data"))
          GVar->addAttribute("dspic-space", "auto_psv");
      }
      if (VD->hasAttr<DSPICUnorderedAttr>())
        GVar->addAttribute("dspic-unordered");
      if (const auto *PA = VD->getAttr<DSPICPriorityAttr>())
        GVar->addAttribute("dspic-priority", std::to_string(PA->getLevel()));
      // trellis session 144 (ITEM 1): `dspic-fillupper` is decided on the vendor's view, below (after DspicWarnHere).
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
      const auto *SpA = DspicView.Space;       // trellis session 144: the vendor's space, not the first written
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
      // trellis session 144: given at the end of the translation unit (the class's record, F1 in late-edit.py).
      if (AddrOutsideNear && VD->hasAttr<DSPICNearAttr>() &&
          VD->isThisDeclarationADefinition())
        DspicWarnLater([VD](DiagnosticsEngine &DE) {
          DE.Report(VD->getLocation(), diag::warn_dspic_address_overrides_near) << VD;
        });
      ForcedFar = ForcedFar || AddrOutsideNear;
      if (ForcedFar) {
        GVar->setAttributes(GVar->getAttributes().removeAttribute(GVar->getContext(), "near"));
        GVar->addAttribute("far");
        if (VD->hasAttr<DSPICPageAttr>())
          GVar->addAttribute("dspic-page");
      }
      // ⛔ trellis session 142 (ITEM 1): A BLOCK-SCOPE OBJECT'S WRITTEN SECTION IS ATTACHED HERE, BEFORE THE
      // DISCARD BELOW READS IT. For a static local clang attaches it only after this function has run --
      // CodeGenModule::getOrCreateStaticVarDecl calls setTargetAttributes (CGDecl.cpp:308), and
      // CodeGenFunction::EmitStaticVarDecl calls setSection afterwards (:463-464) -- so the discard's
      // `GVar->hasSection()` was false for every static local, and one carrying space(auto_psv), reverse,
      // unordered or `address` + "*" KEPT its written name where the vendor warns and discards it: session
      // 132's D4. Beside a 30000-byte `.const` that name is a separate page-attributed PSV section the linker
      // puts on another page, and the object is read through the page crt0's `__psv_init` sets from
      // `__const_psvpage`: 0x0000 where the vendor reads 0x5A, no diagnostic (trc steps/frontend/
      // MISREAD-X.banked.txt). The name is the one EmitStaticVarDecl attaches later -- the same string twice --
      // and from here a static local takes the file-scope path whole: the warning, the marker, the TLOF's
      // delegation, and the conflict scan, which still reads the string. ⚠ Static locals only: a file-scope
      // definition and a block-scope `extern` carry their section before this runs.
      // ⛔ trellis session 143 (ITEM 1): SESSION 142'S EXCLUSION IS LIFTED -- every static local's written section is
      // attached here. It kept a space(auto_psv) static local carrying noload, persistent or a comma-bearing name on its
      // pre-landing `sy,<tokens>` path, because ours' file-scope path then kept what the vendor ignores; the family block
      // below now ignores them as the vendor does, and the exclusion's own path was the worse one: of the 33 written
      // names the vendor accepts at block scope in trc steps/frontend/APSV-T.banked.txt, ours kept all 33 -- right only
      // for the bare "*" -- and the assembler refused 12 (apsv-counts.py), and D4's silent misread survived for the
      // kept PSV names: `sy,psv`, `sy,keep`, `sy,page`, `sy,info`, `sy,reverse(64)` and `*,psv` read 0x0000 beside a
      // 30000-byte `.const` where the vendor reads 0x5A (APSV-X3.banked.txt).
      if (!GVar->hasSection() && VD->isStaticLocal())
        if (const auto *SecA = VD->getAttr<SectionAttr>())
          GVar->setSection(SecA->getName());
      // ⛔ AND THE VENDOR NEVER GIVES THE DISCARD WARNING INSIDE A TEMPLATE INSTANTIATION. Its C++ front end
      // carries no written section name onto an instantiated declaration, and pic30.c's warning
      // (validate_decl_attributes, :3202-3209) fires only when DECL_SECTION_NAME is set -- measured SILENT on a
      // function template's static local, a class template member's, an explicit instantiation's, a generic
      // lambda's, the static local of a lambda, a local class and a nested class inside a template, a class
      // template's static data member and a variable template; and WARNING on an explicit specialization and
      // a non-template member (MISREAD-M/P.banked.txt). A warning the vendor does not give is a refusal under
      // -Werror. ⚠ The PLACEMENT half of that behaviour -- a templated object with a written name and NO trigger
      // lands where no name was written (MISREAD-P T1-T3, T5, T18) -- is NOT modelled: a different answer where
      // both accept, the operator's.
      // Two terms: the object's OWN kind (a class template's static member, a variable template, an explicit
      // instantiation's member; AM24 drops it) and its enclosing FUNCTION's (AM43 drops it). ⚠ No walk further out:
      // clang marks a lambda's call operator, a local class's and a nested class's member inside an instantiation as
      // instantiations themselves, so the enclosing function decides every static local asked -- a parent walk
      // written first was never reached, and its mutant LIVED (MISREAD.mutants.txt, MR5). Under the gate below, a
      // static local reaches this warning only when clang makes it `weak` (getLLVMLinkageForDeclarator); any other
      // linkage clang gives a static local under the vendor's flags is discardable (ASTContext's static-local GVA
      // linkage, read; refK's probe). The vendor refuses a `weak` it applies itself to a static local ("weak
      // declaration of 'x' must be public"; refI, trc APSV.expected.first addendum 19). It never applies one that
      // `#pragma clang attribute` gives: it ignores the pragma, warns only -Wunknown-pragmas, gives no family
      // warning, and under -Werror with -Wno-unknown-pragmas accepts (refJ, addenda 21 and 22). There this term keeps
      // ours silent too, and accepting. The operator ruled it removed (addendum 18 R2) on the premise that no cell
      // could kill it, and ruled it restored on refJ's witness (addendum 21).
      bool DspicInInstantiation = isTemplateInstantiation(VD->getTemplateSpecializationKind());
      if (const auto *FD = dyn_cast<FunctionDecl>(VD->getDeclContext()))
        DspicInInstantiation = DspicInInstantiation || FD->isTemplateInstantiation();
      // ⛔ AND THE GATE IS "THE OBJECT CERTAINLY REACHES OUTPUT" (session 142's C2, narrowed at session 143 by two
      // refutation passes aimed at the landed diff). The vendor warns from encode_section_info, so only for an object
      // that reaches OUTPUT: at -Os it is silent for an unreferenced or folded object and warns for one that survives
      // (MISREAD-U; trc steps/frontend/APSV-N.banked.txt, the U cells). This runs at CodeGen time, before anything is
      // folded, so it warns only for an object that cannot be folded away: a definition whose linkage is not
      // discardable -- an external or weak one, or an explicit instantiation's. ⚠ NOT "at -O0 as well", which the
      // first landing had: clang emits at -O0 objects the vendor never does -- an unreferenced or folded static local
      // of an inline or in-class function the unit calls, and the static locals of a callee reached only from
      // `while (0)` -- so its warning there was one the vendor does not give, a refusal under -Werror
      // (APSV.expected.first, addendum 7: W-inline-unref-noload). And session 142's gate (`!isStaticLocal() || -O0`)
      // warned for a FILE-SCOPE static a fold removes (APSV-N N-U-bss-fold-FS). The price, declared in apsv-compare.py
      // and misread-compare.py: a MISSING warning for every static local and every internal, inline or implicitly
      // instantiated object, at every level. The far side -- warn
      // after optimization, for the objects that survive -- joins the prep's ITEM on the vendor's warnings ours never
      // gives (the SESSION 144 PREP's ITEM 12), which carries the static-local discard warning's.
      // ⛔ AND THE LINKAGE IS READ AS THE VENDOR'S C++ FRONT END -- GCC 8.3.1's -- READS IT, where the two differ (a third
      // refutation pass, trc steps/frontend/APSV.expected.first addendum 11: refC F1). A non-volatile const at
      // namespace scope that no namespace-scope declaration calls `extern` has internal linkage by C++'s own rule, and
      // clang gives two shapes EXTERNAL linkage instead: an explicit specialization of a const variable template
      // (`template <> const char vt<char>[2]`), and a const defined after a block-scope `extern` of its name. The vendor
      // drops an unreferenced one at -Os in silence, and ours warned: a refusal under -Werror. So they count as
      // discardable. ⚠ Not a static data member (`S::v<int>` is external on both; its context is its class, so the
      // file-context test excludes it) and not an explicit instantiation (`template const int vt<int>[2]`: the vendor
      // warns at both levels). What counts as `extern` is GCC's, read in the vendor's source (cp/parser.c, cp/decl.c)
      // after two more refutation passes broke two narrower readings -- clang's isExternC() (addendum 13, refF 1b: a
      // block-scope `extern` inside `extern "C" {}` gave the whole chain C language linkage) and [dcl.link]p7's, read
      // over every redeclaration (addendum 14, refG):
      //   - the declarations UP TO THE DEFINITION, never a later one: GCC gives a later declaration the earlier one's
      //     linkage (decl.c:2355, `TREE_PUBLIC (newdecl) = TREE_PUBLIC (olddecl)`). Inside a namespace or a braced
      //     linkage spec clang hands CodeGen the block at its closing brace, so a later `extern` stood in the chain
      //     here and read external where the vendor's object is internal -- an extra warning at -Os, a refusal under
      //     -Werror (refG's HL cells); at file scope the definition is emitted before a later line is parsed;
      //   - on a declaration not at block scope (a block-scope `extern` leaves the later const internal on GCC), the
      //     storage class `extern`, or ANY enclosing linkage spec written WITHOUT braces: GCC's parser sets `extern` on
      //     each declaration it parses under one that writes no storage class of its own (parser.c:19726-19727; one
      //     that writes one is refused, :28232) and resets that only for a parameter list, a class body and a named
      //     function's body (:21436, :22634, :26964 -- not a lambda's, :10699), so it reaches through a namespace body
      //     and a braced spec nested in the brace-less one -- wider than [dcl.link]p7 (refG's MS cells: the vendor
      //     warns, and exports the object; refH).
      // C is not read at all: every C object at file scope has external linkage unless `static`, and a static one is
      // discardable already. The price: a MISSING warning where the vendor emits the internal object -- referenced,
      // or at -O0 -- and where GCC's flag makes external an object clang keeps internal and drops (`extern "C"
      // namespace M { __attribute__((space(auto_psv), noload)) const int a[2] = {41, 42}; }`: the vendor warns and
      // exports `a` -- the linkage itself, carried).
      bool DspicGccInternal = false;
      if (M.getLangOpts().CPlusPlus && VD->getDeclContext()->getRedeclContext()->isFileContext() &&
          VD->getType().isConstQualified() && !VD->getType().isVolatileQualified() &&
          VD->getTemplateSpecializationKind() != TSK_ExplicitInstantiationDefinition) {
        DspicGccInternal = true;
        for (const VarDecl *R = VD; R; R = R->getPreviousDecl()) {
          bool DspicUnbraced = false;
          for (const DeclContext *DC = R->getLexicalDeclContext(); DC; DC = DC->getLexicalParent())
            if (const auto *LinkSpec = dyn_cast<LinkageSpecDecl>(DC))
              DspicUnbraced = DspicUnbraced || !LinkSpec->hasBraces();
          if (!R->isLocalExternDecl() && (R->getStorageClass() == SC_Extern || DspicUnbraced))
            DspicGccInternal = false;
        }
      }
      bool DspicReachesOutput = !GVar->isDiscardableIfUnused() && !DspicGccInternal;
      // ⛔ trellis session 144 (ITEM 1): THE SPACE IS THE VENDOR'S NOW, SO IT IS NEVER IN DOUBT. Session 143 gated these
      // warnings on every space() of every declaration saying auto_psv (DspicSpaceAgrees), because ours read the
      // definition's FIRST space and the vendor the last of one declaration and gcc's merged order over several: after
      // `extern __attribute__((space(data))) const int a[2];` a space(auto_psv) definition named "sy,data" is accepted
      // in silence by the vendor (addendum 7, D-redecl-data-then-apsv-F) -- the shorter declaration's space, data, is
      // first. SpA is the vendor's own space since session 144 (dspicVendorView), so the agreement would only silence
      // warnings the vendor gives -- `extern __attribute__((space(psv), noload))` before a space(auto_psv) definition:
      // the vendor's space is auto_psv and it warns noload (trc steps/frontend/LATE-P.banked.txt, P11) -- and it is
      // removed. A later declaration is seen too: the class records the view and re-derives it at the end of the unit.
      bool DspicWarnHere = !DspicInInstantiation && DspicReachesOutput;
      // ⛔ trellis session 144 (ITEM 1): FILLUPPER ON THE FINAL VIEW, as the vendor decides it at emission
      // (pic30_emit_fillupper, pic30.c:22633-22670): a definition whose section is code -- its space prog, or a type in
      // program memory -- gets `.fillupper`; any other is ignored with the vendor's -Wattributes warning, where it
      // reaches output, given at the end of the unit with the rest. Sema decided it at the definition, so a later
      // declaration moving the space left `.fillupper` on a data object our assembler refuses (trc steps/frontend/
      // refs144/C/REFC-FU.banked.txt) and dropped it where a later space(prog) made the object program memory
      // (refs144/B's F9). A declaration never carries it: the AsmPrinter switches to the object's section first, and
      // on a declaration that asserted (refs144/C/REFC-crash.banked.txt; the vendor accepts).
      // The seventh build: the value and the presence are the merged list's (the view's Fill; refutation pass D's F1,
      // F1b, F2).
      if (DspicView.Fill && !GVar->isDeclaration()) {
        if ((DspicView.Space && DspicView.Space->getSpace()->isStr("prog")) ||
            VD->getType().getAddressSpace() == LangAS::FirstTargetAddressSpace)
        {
          if (*DspicView.Fill)                 // the tenth build: fillupper(0) writes no line (pic30_emit_fillupper)
            GVar->addAttribute("dspic-fillupper", std::to_string(*DspicView.Fill));
        }
        else if (VD->isThisDeclarationADefinition() && DspicWarnHere)
          DspicWarnLater([VD](DiagnosticsEngine &DE) {
            DE.Report(VD->getLocation(), diag::warn_dspic_fillupper_ignored) << VD;
          });
      }
      // ⛔ trellis session 143 (ITEM 1): THE auto_psv IGNORE FAMILY. For an object whose OWN written space is auto_psv --
      // pic30.c:2674's `auto_psv` is the space attribute, not the const default and not the auto_psv this file names
      // above for a `shared` const -- the vendor IGNORES `shared`, `noload` and `persistent`, each with "%D Ignoring <x>
      // attribute for '%s'" (pic30.c:2768-2826, in that order), and a discarded name's tokens go with the name. Ours
      // kept all of it: `noload` on the SHARED `.const` left EVERY const of the unit out of the device -- executed, ours
      // read 0x00FF from the object and from an unrelated table (trc steps/frontend/APSV-X1.banked.txt, X-noload-F/B)
      // -- and `persistent` moved the object to uninitialised RAM (0x0000; X-persistent-F/B).
      // The three attributes are taken back off here, after the chain above added them, so the lines that add them
      // stay as written; ALWAYS, on ours' own reading of the space, because an object ours places in the shared
      // `.const` by a WRITTEN space(auto_psv) must never carry `noload` whatever the warning gate says -- the vendor
      // ignores it there. ⚠ Two other roads still reach a NEVER_LOAD shared `.const`, and neither is this family's:
      // a `shared` const with `noload` and no written space, where the vendor ITSELF marks the shared `.const`
      // NEVER_LOAD when nothing else holds it and refuses the unit beside another const (see below the block); and a
      // section NAMED `.const` written on a space(psv) object with `noload`, which the vendor refuses on the name's own
      // flags (refD 2, corrected by refF) -- both carried. `shared` no longer leaves Sema early
      // (SemaDeclAttr.cpp): Sema cannot tell whether the object reaches output, and its warning fired for an
      // unreferenced or folded object the vendor is silent on (APSV-N N-U-shared-*). ⚠ The warnings are NOT silenced
      // inside a template instantiation, where the vendor gives them too (addendum 7: an explicit instantiation's
      // static member, `S<int>::v Ignoring noload attribute for 'v'`) and is silent only for the discarded NAME; the
      // vendor's %D is the QUALIFIED name, the second the identifier.
      if (SpA && SpA->getSpace()->isStr("auto_psv")) {
        static const char *const DspicIgnored[] = {"dspic-shared", "dspic-noload", "dspic-persistent"};
        for (unsigned K = 0; K != 3; ++K)
          if (GVar->hasAttribute(DspicIgnored[K])) {
            GVar->setAttributes(GVar->getAttributes().removeAttribute(GVar->getContext(), DspicIgnored[K]));
            if (VD->isThisDeclarationADefinition() && DspicReachesOutput) {
              std::string DspicShown;
              llvm::raw_string_ostream DspicOS(DspicShown);
              VD->getNameForDiagnostic(DspicOS, M.getContext().getPrintingPolicy(), /*Qualified=*/true);
              DspicWarnLater([VD, DspicName = DspicOS.str(), K](DiagnosticsEngine &DE) {
                DE.Report(VD->getLocation(), diag::warn_dspic_auto_psv_ignores) << DspicName << K << VD->getName();
              });
            }
          }
        // A discarded name's tokens go with it: every token the vendor accepts lands in `.const,psv,page` for an
        // object alone under its name, at both scopes (APSV-T). The TLOF's pic30Attrs reads the marker; the kept "*"
        // holds none. ⚠ Except a name that BEGINS with `#`: the vendor keeps it, without the `#`, with its tokens
        // (pic30.c:3345, :22228-22230 -- `#sy,keep` is `sy,keep,psv,page`), and ours discards it like any other: a
        // different answer where both accept, nothing misread (refC F5), the answer class.
        // ⛔ AND A NAME THE VENDOR REFUSES IS ACCEPTED HERE, ITS TOKENS IGNORED ALIKE: a kind (`sy,bss`) or
        // a pair its table marks incompatible (`sy,psv,near`). The first landing refused them with the vendor's text
        // (D5), and two refutation passes broke it where the vendor accepts (APSV.expected.first addendum 7 counts
        // them): inside template instantiations, which carry no written name on the vendor; at -O0 for objects clang
        // emits and the vendor does not; under two space() attributes; and after a declaration sharing the name
        // string, which the vendor's parse writes into (pic30.c:2131-2132, :2145), where its answer turns on whether
        // the other declaration is used. Two of those are decidable here and one lies outside the gate now; the
        // refusal was WITHDRAWN rather than narrowed shape by shape, because each pass found shapes the last had not,
        // a refusal of what the vendor accepts is a breach, and accepting more is the operator's call (addendum 7, R1).
        if (VD->hasAttr<SectionAttr>())
          GVar->addAttribute("dspic-ignore-name-tokens");
      }
      // ⛔ AND A `shared` CONST WITH `noload` AND NO WRITTEN SPACE KEEPS `noload` -- THE VENDOR'S OWN ANSWER (a fourth
      // refutation pass, refF 2, withdrew the strip the third build had put here). It takes the implied auto_psv set
      // above, the vendor's `.const,psv,page,shared`, and where nothing else holds the unit's `.const` the vendor marks
      // it NEVER_LOAD too -- executed, its image loses every const, another unit's too -- and beside another const it
      // refuses the unit (a section type conflict). The third build took `noload` off here and so answered
      // DIFFERENTLY where both accept -- in 23 cells of refF's sample, 40 cell-levels (14 of those cells, 24 cell-levels,
      // where clang-s143b had matched the vendor exactly; refG) -- the safer answer, and one no rule of this project
      // lets ours choose alone: a different answer where both accept is the operator's -- and for THIS road, after
      // session 143, the operator ruled "do what vendor does": ours keeps NEVER_LOAD here alone, as the vendor
      // does (FOR THE OPERATOR 16; APSV.expected.first addendum 18 R1, read narrowly -- whether it reaches DOTNAME or
      // a plain `noload` const is the operator's; refF's executed witnesses).
      // What the strip was written for is closed since session 144: `shared, noload` and then a LATER
      // `extern __attribute__((space(auto_psv)))`, which the vendor merges (ignoring noload, 0x5A) and clang's Sema
      // dropped ("attribute declaration must precede definition") -- ours' shared `.const` NEVER_LOAD, 0x00FF. Sema now
      // keeps the later space() and the merged list, the view reads auto_psv, and the family above takes noload off.
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
      bool TrigAddress = VD->hasAttr<DSPICAddressAttr>();
      if (GVar->hasSection() &&
          (TrigReverse || TrigUnordered || TrigAutoPsv || TrigAddress)) {
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
        // ⛔ trellis session 130: THE WARNING KEEPS THE OLD TRIGGER SET. `address` now enters
        // this block so that a `"*"` name can be DISCARDED, and cc1 discards it SILENTLY there
        // -- 0 warnings at space(psv) and at no space, against 4 at space(auto_psv), measured
        // through the vendor driver at -Wall. Warning on the address-only cells would be a
        // diagnostic cc1 does not issue, on code it compiles.
        // trellis session 142 (ITEM 1): and never inside a template instantiation, where the vendor is
        // silent; since session 143, only where the gate says the object reaches output (DspicWarnHere, computed
        // before this block) -- never for a static local unless clang makes it `weak` (refI's W10, refJ's P9 and
        // T2; refD: this line still said "above -O0").
        // trellis session 144: given at the end of the translation unit (the class's record).
        if (VD->isThisDeclarationADefinition() && DspicWarnHere &&
            (TrigReverse || TrigUnordered || TrigAutoPsv))
          DspicWarnLater([VD](DiagnosticsEngine &DE) {
            DE.Report(VD->getLocation(), diag::warn_dspic_section_name_ignored) << VD;
          });
        // trellis session 129 (ITEM 0), TWO CORRECTIONS, both found by a refutation pass sent at
        // the LANDED DIFF rather than at a claim -- the first pass ever aimed at this code -- and
        // both REPRODUCED here against the row's own banked pre-row binary before being recorded.
        //
        // THE VENDOR LADDER, read at pic30.c and verified at the source this session:
        //     3421  if (r || u || psv) {
        //     3422    if (pszSectionName && strcmp(pszSectionName, pic30_default_section))
        //     3423      pszSectionName = 0;
        //     3425    if (u && psv) u = 0;            <-- NOT IMPLEMENTED BY THE FIRST LANDING
        //     3429  if (u)            -> generated
        //     3432  else if (a)       -> generated_at_address        (D5, see below)
        //     3468  else if (r)       -> generated + reverse
        //     3491  else if (name)    -> THE NAME IS KEPT
        //           else if (psv)     -> .const
        // with `pic30_default_section` the one-character string "*" (pic30.c:402).
        //
        // (1) ⛔ A REGRESSION THE ROW INTRODUCED. `unordered + space(auto_psv) + section("*")`:
        // cc1 CLEARS `u` when `psv` is set, so its `if (u)` arm does not fire, the ladder falls
        // through to `else if (pszSectionName)` and "*" IS KEPT. Measured on one axis:
        //     cc1  *,psv,page     pre-row  *,psv,page  (MATCHED)   post-row  .const,psv,page
        // drop `unordered` and all three agree; drop `space(auto_psv)` and the cell is fine.
        // `section("*")` is the vendor's own spelling for GIVE ME MY OWN SECTION, and the first
        // landing merged such an object into the shared `.const`.
        bool UnordEff = TrigUnordered && !TrigAutoPsv;      // pic30.c:3425
        // ⛔ trellis session 130 (D5): `address` BELONGS IN THE `"*"` ARM AND ONLY THERE, and
        // it is not a psv question -- which is how the record carried it. Measured over the
        // whole named cross product (perobj-ask3.py, 48 cells): the three cells where ours
        // wrongly keeps `*` are space(auto_psv), space(psv) AND space(none), so the trigger is
        // the NAME's spelling and nothing else. cc1's `else if (a)` arm (pic30.c:3455)
        // substitutes its generated name only when the written name is absent or is itself the
        // default `"*"`; with any other name it keeps it, which is why all EIGHT
        // `section("sy")` + address cells agree today and must keep agreeing. Putting the
        // trigger in both arms would break those eight -- mutant N3m, not a sentence.
        bool NameDiscarded = GVar->getSection() != "*"
                                 ? (TrigReverse || TrigUnordered || TrigAutoPsv)
                                 : (UnordEff || TrigReverse || TrigAddress);
        // (2) ⛔ A SECOND REGRESSION, AND THIS ONE IS NARROWED RATHER THAN FIXED, DELIBERATELY.
        // The delegation's stated ground is that "the name cc1 gives a triggering object is the
        // name its UNNAMED path gives it". That is FALSE for `reverse` + `space(auto_psv)`: cc1's
        // ladder reaches `else if (r)` and gives a PER-OBJECT generated section, while ours'
        // unnamed path tests the space first and gives the SHARED `.const`. Measured:
        //     cc1      .const,psv,page          + *_<hex>,reverse(64),psv,page   (two sections)
        //     pre-row  sy,reverse(64),psv,page  + .const                         (two sections)
        //     post-row .const,reverse(64),psv,page + .const                      (ONE section)
        // so an unrelated `const int plain` -- and in a real translation unit every string
        // literal and const table -- was dragged into a reverse(64), 64-byte-aligned section.
        // ⛔ THE FIX IS NOT ATTEMPTED HERE and that is a scope decision, not an oversight: the
        // root cause is that ours' per-object base chain consults the SPACE before the u/a/r arms
        // where cc1's ladder does the reverse, and choosing the per-object base for a psv object
        // needs its own ask (the pic30 assembler derives attributes from the NAME too -- session
        // 128's own M-ladder finding -- so `.nbss.a` and `.const.a` are not interchangeable).
        // Landing a guess beside a verified fix is how this row shipped wrong code five times.
        // What IS done is to stop the harm: the shape keeps its named section, which is ALSO
        // divergent from cc1 and is at least ISOLATED to the object that asked for it.
        // ⚠ COSTED, not decided, and it is in the close question with its measurement.
        // ⛔ trellis session 130 (D2): `DelegationAgrees` IS DELETED, NOT WEAKENED. Session
        // 129 suppressed the discard for `reverse` + `space(auto_psv)` because ours' unnamed
        // path tested the SPACE before the u/a/r arms and so answered with the shared `.const`,
        // where cc1 gives a per-object section -- delegating would have dragged every plain
        // const in the translation unit into a reverse(64), 64-byte-aligned section. That base
        // chain now follows cc1's ladder (perobj-edit.py), so delegation answers with a
        // per-object section and there is no shape left for which it disagrees. The suppression
        // was COSTED at its site, not decided, and its cost is now paid rather than carried.
        if (NameDiscarded)
          GVar->addAttribute("dspic-ignore-section-name");
        // ⚠ D5, MEASURED AND NOT THIS ROW'S REGRESSION: `space(auto_psv) + address(0x2000) +
        // section("*")` is `*_<hex>_at_address_...` in cc1 and `*,address(8192),psv,page` in
        // ours, IDENTICALLY before and after this row -- the 3432 `else if (a)` arm, which the
        // guard does not consult. Adding `a` to NameDiscarded would delegate into the same
        // shared-`.const` base that (2) is about, so it waits on the same ask.
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
  // trellis session 141: `shared` -- its own section, the flag last.
  if (FD->hasAttr<DSPICSharedAttr>())
    F->addFnAttr("dspic-shared");
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

// ⛔ trellis session 144 (ITEM 1): THE END OF THE TRANSLATION UNIT. A recorded variable whose view -- the space and
// the family, read off the most recent declaration's list -- is not the one its record saw was reached by a later
// declaration: its dsPIC attributes -- every "dspic-*" one, and "near" / "far", the only keys setTargetAttributes
// writes on a variable -- are cleared and derived again, from the declaration it was recorded with (its type stays the
// one CodeGen used; refutation pass B's X3). A static local has no later declaration. Then every recorded warning is
// given, once, in the order the variables were first seen.
void DSPICTargetCodeGenInfo::emitTargetGlobals(CodeGen::CodeGenModule &CGM) const {
  SmallVector<const VarDecl *, 4> Redo;
  for (const auto &E : DspicVars)
    if (!E.second.VD->isStaticLocal() && dspicViewKey(dspicVendorView(E.second.VD)) != E.second.View)
      Redo.push_back(E.first);
  for (const VarDecl *Canon : Redo) {
    const VarDecl *Use = DspicVars[Canon].VD;
    auto *GVar =
        dyn_cast_or_null<llvm::GlobalVariable>(CGM.GetGlobalValue(CGM.getMangledName(GlobalDecl(Use))));
    if (!GVar)
      continue;
    llvm::AttributeSet AS = GVar->getAttributes();
    for (const llvm::Attribute &A : GVar->getAttributes())
      if (A.isStringAttribute() && (A.getKindAsString().starts_with("dspic-") ||
                                    A.getKindAsString() == "near" || A.getKindAsString() == "far"))
        AS = AS.removeAttribute(GVar->getContext(), A.getKindAsString());
    GVar->setAttributes(AS);
    setTargetAttributes(Use, GVar, CGM);
  }
  for (auto &E : DspicWarnings)
    for (auto &W : E.second)
      W(CGM.getDiags());
  DspicWarnings.clear();
}

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createDSPICTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<DSPICTargetCodeGenInfo>(CGM.getTypes());
}
