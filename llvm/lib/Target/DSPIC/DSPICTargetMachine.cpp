//===-- DSPICTargetMachine.cpp - Define TargetMachine for DSPIC ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level implementation for the DSPIC target.
//
//===----------------------------------------------------------------------===//

#include "DSPICTargetMachine.h"
#include "DSPICTargetTransformInfo.h"
#include "DSPIC.h"
#include "DSPICMachineFunctionInfo.h"
#include "TargetInfo/DSPICTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/GlobalObject.h"
#include "llvm/IR/DiagnosticInfo.h"   // trellis session 122: the two-placements warning
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Function.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/SectionKind.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/ADT/SmallString.h"
#include <optional>
using namespace llvm;

// L1f-h (trellis session 88): the pic30 assembler REFUSES the mergeable ELF sections LLVM
// emits for read-only constants and strings (`.rodata.cst16,"aM",@progbits,16`,
// `.rodata.str1.1,"aMS",...`; probe10 shows the bare `.rodata` accepted, the mergeable form
// not). Route every such constant to plain `.rodata`. cc1 places read-only data in
// `.const,psv,page`; the PSV window and the near/far split are L1e's/L1g's -- a `.rodata` that
// ASSEMBLES is this row's minimum.
namespace {
class DSPICTargetObjectFile : public TargetLoweringObjectFileELF {
  // L1e (trellis session 89): the pic30 near/PSV section model. The bl_fw linker script places
  // sections by NAME: the pic30 `as` gives `.const*` PSV/program-memory attributes, `.ndata*`/
  // `.nbss*` NEAR data attributes; plain `.rodata`/`.data`/`.bss` get bare DATA the script cannot
  // allocate. cc1 in the small-data model puts read-only constants in `.const` (PSV window) and
  // ordinary data in `.ndata`/`.nbss` (near). We match the NAMES; the `as` supplies the flags.
  MCSection *ConstSection = nullptr;   // .const   (read-only, in program memory via PSV)
  MCSection *NDataSection = nullptr;   // .ndata   (near initialized data)
  MCSection *NBSSSection = nullptr;    // .nbss    (near zero-initialized data)

  MCSection *pic30Section(StringRef Base, const GlobalObject *GO, SectionKind Kind,
                          const TargetMachine &TM, unsigned Type,
                          unsigned Flags) const {
    // With -fdata-sections the per-global suffix `.symbol` rides on the pic30 prefix (cc1's
    // `.nbss._connected`); the `as` still reads NEAR/PSV from the prefix (probe as-probe2).
    if (TM.getDataSections() && GO) {
      SmallString<128> Name(Base);
      Name += '.';
      Name += GO->getName();
      return getContext().getELFSection(Name, Type, Flags);
    }
    return getContext().getELFSection(Base, Type, Flags);
  }

public:
  void Initialize(MCContext &Ctx, const TargetMachine &TM) override {
    TargetLoweringObjectFileELF::Initialize(Ctx, TM);
    // Constant pools, jump tables and merged constants land here.
    ConstSection = Ctx.getELFSection(".const", ELF::SHT_PROGBITS, ELF::SHF_ALLOC);
    NDataSection = Ctx.getELFSection(".ndata", ELF::SHT_PROGBITS,
                                     ELF::SHF_ALLOC | ELF::SHF_WRITE);
    NBSSSection = Ctx.getELFSection(".nbss", ELF::SHT_NOBITS,
                                    ELF::SHF_ALLOC | ELF::SHF_WRITE);
    ReadOnlySection = ConstSection;
    DataSection = NDataSection;
    // Session 90: BSSSection is a SENTINEL, not .nbss. LLVM's AsmPrinter emits a BSS-local
    // global as `.comm sym,size` (which the pic30 `as` leaves with no `near`/far attribute --
    // the linker overflows the data region placing them) IFF the global's section ==
    // getBSSSection(). Every zero-init global is routed by SelectSectionForGlobal to .nbss
    // (near) or .bss (far); if getBSSSection() equalled either, those would become `.comm`.
    // A section nothing returns keeps every one a DEFINED symbol in its named section (cc1).
    BSSSection = Ctx.getELFSection(".nbss.__llvm_bss_sentinel", ELF::SHT_NOBITS,
                                   ELF::SHF_ALLOC | ELF::SHF_WRITE);
  }

  MCSection *getSectionForConstant(const DataLayout &DL, SectionKind Kind,
                                   const Constant *C, Align &Alignment,
                                   const Function *F) const override {
    // Every read-only constant goes to program memory via `.const` (PSV), never `.rodata`.
    return ConstSection;
  }

  // trellis session 96 (follow-up 14): the pic30 section-attribute suffix for a global, read
  // off cc1 (steps/placement/ask.sh). The attributes travel INSIDE the MCSection name and
  // DSPICTargetAsmStreamer::changeSection prints them verbatim, because `persist`, `noload` and
  // `psv` have no ELF flag and the assembler's name-based inference does not cover the names
  // this firmware uses. ⛔ `persist` REPLACES `data`/`bss` in cc1's output; it is not appended.
  // trellis session 109: `priority(N)` is `,priority(0xNNNN),keep` on both kinds (var.cc1.s
  // v_prio, fn.cc1.s f_prio) -- four hex digits, and it implies keep.
  static std::string pic30Priority(const GlobalObject *GO) {
    StringRef V;
    if (const auto *F = dyn_cast<Function>(GO)) {
      if (!F->hasFnAttribute("dspic-priority"))
        return "";
      V = F->getFnAttribute("dspic-priority").getValueAsString();
    } else if (const auto *GV = dyn_cast<GlobalVariable>(GO)) {
      if (!GV->hasAttribute("dspic-priority"))
        return "";
      V = GV->getAttribute("dspic-priority").getValueAsString();
    } else {
      return "";
    }
    unsigned N = std::stoul(V.str());
    char Buf[32];
    snprintf(Buf, sizeof Buf, ",priority(0x%04X),keep", N);
    return Buf;
  }

  // trellis session 122: ProgDefault -- the space to assume when the object names none. A `__prog__`
  // object lives in program memory whether or not it says space(prog), and both of its section
  // arms spell their attributes through here so that noload / keep / psv,page are not lost.
  // ⛔ trellis session 124 (P3). THE SECTION TYPE, as cc1 distinguishes it from a section
  // ATTRIBUTE, measured before this was written (steps/frontend/order-ask.sh): cc1 REFUSES to
  // reconcile near-vs-far, address-present-vs-absent and noload-vs-absent -- "'a' causes a section
  // type conflict with 'b'", an ERROR -- and silently RESOLVES two different address VALUES,
  // keep-vs-absent and two priorities. So the TYPE is those three facts and the address VALUE is
  // deliberately NOT part of it. Mutants MO2 and MO3 take a fact back out.
  // ⚠ `page` vs absent was not asked; it is not classified, and nothing here quantifies over it.
  static std::string dspicSecType(StringRef Attrs) {
    // ⛔ trellis session 125: AN EXCLUSION LIST, NOT AN INCLUSION LIST, and the direction of the
    // default is the repair. Session 124 kept four tokens and thereby DEFINED every other token --
    // including every one nobody had asked cc1 about -- to be a reconcilable attribute, so an
    // unreconcilable pair was silently unified into one `.section` line that the assembler then
    // ACCEPTED, where before P3 two conflicting lines made it refuse the file. Measured regression
    // on three classes (space(prog)/space(psv) vs far, persistent vs plain) with cc1 refusing all
    // three; the only signal was a warning, and `-w` removes it.
    //
    // ⛔ THE FOUR BELOW ARE FITTED FROM cc1, WHICH PRINTS ITS OWN PREDICATE on a conflict
    // ("note: a variable flags: near, persist" / "note: sy section flags: near").
    // steps/frontend/sectype-ask.sh asks 21 attributes against ABSENCE and 10 pairs of differing
    // VALUES, plus the bss/data axis: cc1 RESOLVES exactly address(), priority(), reverse()
    // and keep, and REFUSES everything else -- bss vs data, persist, xmemory vs absent, xmemory
    // vs ymemory, dma vs eds among them. Mutant MS1 takes it back to the inclusion list.
    //
    // ⛔ trellis session 127: `address` IS MASKED OUT WHOLE, PRESENCE AND VALUE ALIKE, and that
    // is READ OFF cc1's OWN PREDICATE rather than fitted from verdicts. pic30.c:28153
    // pic30_check_section_flags_save computes `f1 = flag1 & ~IGNORE; f2 = flag2 & ~IGNORE;
    // return f1 != f2;` with IGNORE = SECTION_CONST_NAME | SECTION_DECLARED | SECTION_ADDRESS |
    // SECTION_REVERSE | SECTION_KEEP | SECTION_ALIGN.
    // ⛔ Session 125 kept the PRESENCE because its ladder asked address(0x2000) AND NOTHING ELSE
    // -- a literal OUTSIDE the near data range, where cc1 refuses for an entirely different
    // reason (it has cleared `near`, pic30.c:2984). Re-asked at 0x100 the same upper triangle
    // gives cc1=accept and ours=REFUSE on TEN partners. A shape asked at one literal is a shape
    // not asked. Mutant MA1 restores the presence token and dies on those ten.
    // ⚠ The same mask ALSO ignores SECTION_ALIGN, which is not in this list: `aligned(N)` reaches
    // no token in pic30Attrs at all, so the two agree by two different routes. Recorded because a
    // reader comparing this list with the mask will notice, and not relied on.
    //
    // ⚠ Complete over pic30Attrs's TOKEN UNIVERSE, read from that function rather than from the
    // cells I thought of: address(N) reverse(N) priority(0xNNNN) keep near persist code psv page
    // bss data xmemory ymemory dma eds noload -- sixteen, all sixteen asked.
    // ⚠ `far` is not in it: far is the ABSENCE of near, so session 124's `P == "far"` arm was dead.
    SmallVector<StringRef, 8> Parts;
    Attrs.split(Parts, ',');
    SmallVector<std::string, 8> Keep;
    for (StringRef P : Parts) {
      P = P.trim();
      if (P.empty())
        continue;
      if (P == "keep" || P.starts_with("priority(") || P.starts_with("reverse("))
        continue;                      // cc1 resolves these outright
      if (P.starts_with("address("))
        continue;                      // session 127: masked out WHOLE -- see the block above
      Keep.push_back(P.str());
    }
    // ⚠ A SET comparison, as cc1's is. ⛔ THE SORT IS UNWITNESSED: pic30Attrs emits in a fixed
    // sequence, so no two reachable attribute sets give the same kept SET in two ORDERS, and
    // mutant MS4 is PREDICTED TO LIVE. Kept as defence -- relying on emission order would couple
    // this function to pic30Attrs's statement order, which nothing checks.
    llvm::sort(Keep);
    std::string T;
    for (const std::string &K : Keep)
      T += K + ",";
    return T;
  }

  // The object whose placement a shared section line carries, plus cc1's refusal where the objects
  // cannot be reconciled. ⛔ Ours takes the LAST in module order = cc1's answer AT -Os; matching
  // -O0 as well is impossible, because cc1's deciding object is the FIRST it emits and its emission
  // order REVERSES above -O0 (order-ask.sh a1/a2/a3, the swap being the single axis). Mutant MO5
  // takes the first instead. ⚠ Module order is CREATION order, which is source order except under a
  // forward reference -- P1, inherited here and not fixed.
  const GlobalVariable *dspicDecider(const GlobalVariable *PV, SectionKind Kind,
                                     StringRef Name) const {
    const GlobalVariable *Last = PV;
    for (const GlobalVariable &G : PV->getParent()->globals())
      if (G.getAddressSpace() == PV->getAddressSpace() && G.hasSection() &&
          G.getSection() == Name && !G.isDeclaration())
        Last = &G;
    if (PV != Last)
      return Last;                     // once per name: when the deciding object is itself placed
    std::string LA = pic30Attrs(Last, Kind), LT = dspicSecType(LA);
    for (const GlobalVariable &G : PV->getParent()->globals()) {
      if (&G == Last || G.getAddressSpace() != PV->getAddressSpace() || !G.hasSection() ||
          G.getSection() != Name || G.isDeclaration())
        continue;
      std::string GA = pic30Attrs(&G, Kind);
      if (dspicSecType(GA) != LT)
        G.getContext().diagnose(DiagnosticInfoGeneric(
            ("'" + G.getName() + "' causes a section type conflict with '" + Last->getName() +
             "': section '" + Name + "' is asked for '" + StringRef(GA).ltrim(',') +
             "' and for '" + StringRef(LA).ltrim(',') +
             "', and these are different KINDS of section, not two placements of one -- the "
             "vendor compiler refuses this too").str(),
            DS_Error));
      else if (GA != LA)
        G.getContext().diagnose(DiagnosticInfoGeneric(
            ("section '" + Name + "' is given two placements: '" + G.getName() + "' asks for '" +
             StringRef(GA).ltrim(',') + "' and '" + Last->getName() + "' for '" +
             StringRef(LA).ltrim(',') +
             "'; a section has one, and the second object's is used. NOTE: the deciding object "
             "here is the last in MODULE order, which is CREATION order, and the vendor compiler "
             "decides by its own emission order -- source order at -O0 and REVERSE source order "
             "above it -- so the two agree at -Os and differ at -O0 (trellis session 124)").str(),
            DS_Warning));
    }
    return Last;
  }

  static std::string pic30Attrs(const GlobalObject *GO, SectionKind Kind,
                                StringRef ProgDefault = StringRef()) {
    if (const auto *F = dyn_cast<Function>(GO)) {
      // trellis session 109: the function placement attributes, cc1's forms (fn.cc1.s):
      // `address(4096),code` / `priority(0x0003),keep,code` / `code,noload` / `code,keep`.
      std::string S;
      if (F->hasFnAttribute("dspic-address"))
        S += ",address(" + F->getFnAttribute("dspic-address").getValueAsString().str() + ")";
      S += pic30Priority(GO);
      S += ",code";
      if (F->hasFnAttribute("dspic-noload"))
        S += ",noload";
      if (F->hasFnAttribute("dspic-keep") && !F->hasFnAttribute("dspic-priority"))
        S += ",keep";
      return S;
    }
    const auto *GV = dyn_cast<GlobalVariable>(GO);
    bool Near = !(GV && GV->hasAttribute("far"));
    bool Noload = GV && GV->hasAttribute("dspic-noload");
    StringRef Space =
        GV && GV->hasAttribute("dspic-space")
            ? GV->getAttribute("dspic-space").getValueAsString()
            : ProgDefault;
    // trellis session 129 (ITEM 2): THE NAME'S OWN TOKENS. Measured from cc1 over the vendor's
    // valid_section_flags[] table, 78 cells across three asks banked under
    // prints/l1f/frontend/seckind/. FIVE shapes assemble under cc1 and not under ours today
    // (code psv dma info persist) and three more place the object elsewhere in silence
    // (xmemory ymemory, and `near` written on a far object).
    // `bss` FORCES the kind and `data` DOES NOT -- BSS beats WRITE in the printed kind, so
    // `int a sec("sy,data");` is `sy,data,bss,near` in cc1, not `...,data,near`. Mutant MK3 makes
    // them symmetric and dies on that one cell.
    // No precedence rule: of fifteen multi-token names cc1 compile-errors on twelve and the
    // three it accepts are order-independent. Measured, not assumed.
    // ⛔ THE NAME IS READ OFF THE OBJECT, NOT PASSED IN, and that is a decision with two
    // grounds rather than a convenience. (1) THE CALL SITES ALL AGREE: the explicit addrspace-0
    // site hands `Dec`, the DECIDING object, whose section equals `GO`'s by construction (the
    // decider is found by name equality), and dspicDecider's own two calls hand objects its scan
    // has already filtered on `getSection() == Name`. So a parameter would carry the same string
    // every time. (2) IT IS ALSO RIGHT ON THE PATH WHERE THE NAME IS DISCARDED, which is the
    // cell that decides it: ASKED of cc1 (seckind2-ask4), `__attribute__((unordered,
    // section("sy,code"))) int a = 41;` is `*_<hex>,code` -- cc1 throws the NAME away and KEEPS
    // its tokens -- against `*_<hex>,data,near` for the same object with `section("sy")`. So
    // session 128's delegation path wants these tokens too, and an unnamed object has no section
    // at all, so every other caller is unaffected by construction.
    // ⚠ And it is what let this row leave session 128's edit block alone: `roundtrip.py` REFUSED
    // a first version that rewrote `std::string Name = (GO->getSection() + pic30Attrs(...))`,
    // which is the last line of discard-edit.py's `new` -- the prep's must-not 2, caught before
    // a commit rather than after one.
    StringRef WrittenName = GO->getSection();
    bool NameBss = false, NameNear = false, NamePersist = false;
    StringRef NameSpace;
    size_t NameComma = WrittenName.find(',');
    if (NameComma != StringRef::npos) {
      SmallVector<StringRef, 8> Toks;
      WrittenName.substr(NameComma + 1).split(Toks, ',');
      for (StringRef T : Toks) {
        T = T.trim();
        if (T == "bss")
          NameBss = true;
        else if (T == "near")
          NameNear = true;
        else if (T == "persist")
          NamePersist = true;
        else if (T == "code")
          NameSpace = "prog";      // cc1 spells the prog space `code` in a section line
        else if (T == "psv" || T == "eedata" || T == "dma" || T == "info" ||
                 T == "xmemory" || T == "ymemory")
          NameSpace = T;
      }
    }
    // The name's space wins over the object's own. Untested where the two DISAGREE and both are
    // written (`space(psv)` plus a `,code` name); cc1 errors on every incompatible pair it was
    // asked, so that cell is expected to be a cc1 error and is not claimed here.
    if (!NameSpace.empty())
      Space = NameSpace;
    // `near` in the name forces near ON even where the object is `far`: cc1 gives
    // `__attribute__((far, section("sy,near")))` the line `sy,near,data,near` (B-near-on-far)
    // and ours dropped it. Both assemble, so this one was SILENT. Mutant MK4.
    if (NameNear)
      Near = true;
    // `info` suppresses `near` exactly as `dma` does -- cc1 writes `sy,info,data` and
    // `sy,info,bss` and never a `near` beside either, and both are listed incompatible with
    // SECTION_NEAR in the vendor's own table. ⚠ IT IS DONE HERE, by clearing Near, rather than by
    // widening the `Space != "dma"` guard further down: that line is the last line of session
    // 110's edsplace-edit.py `new` string, and roundtrip.py REFUSED the version that rewrote it.
    // The two spellings are equivalent -- mutant MK5 removes this clause and dies on the info
    // cells alone -- and this one leaves another script's block whole.
    if (NameSpace == "info")
      Near = false;
    std::string S;
    // cc1 puts address() FIRST, ahead of the space attribute, and in decimal.
    if (GV && GV->hasAttribute("dspic-address"))
      S += ",address(" +
           GV->getAttribute("dspic-address").getValueAsString().str() + ")";
    // trellis session 109: `reverse(N)` next (var.cc1.s: `reverse(64),bss`), then priority.
    if (GV && GV->hasAttribute("dspic-reverse"))
      S += ",reverse(" + GV->getAttribute("dspic-reverse").getValueAsString().str() + ")";
    S += pic30Priority(GO);
    if ((GV && GV->hasAttribute("dspic-persistent")) || NamePersist) {
      if (Near)
        S += ",near";
      S += ",persist";
    } else if (Space == "prog") {
      S += ",code";
    } else if (Space == "psv" || Space == "auto_psv") {
      S += ",psv,page";
    } else if (Space == "eedata") {
      // trellis session 129: a name-borne `eedata` prints the space ALONE -- no kind, no near
      // (A-eedata `sy,eedata,eedata`). The pic30 assembler REFUSES cc1's own line here, so this
      // is parity with an output the vendor toolchain does not accept either; both compilers
      // refused it before this row and both refuse it after, and only the TEXT moves.
      S += ",eedata";
    } else {
      bool Zero = !GV || !GV->hasInitializer() ||
                  (GV->getInitializer() && GV->getInitializer()->isNullValue());
      S += (Zero || NameBss) ? ",bss" : ",data";
      // trellis session 109: space(xmemory|ymemory) keep `near`; space(dma) has none (measured,
      // space-*.cc1.s: `data,xmemory,near` / `data,dma`); page/reverse objects have none either.
      // trellis session 110: `eds` joins the spelled-out spaces. Measured at all three memory
      // models: cc1 emits `bss,eds` / `bss,eds,page` and NEVER `near` on an EDS object.
      if (Space == "xmemory" || Space == "ymemory" || Space == "dma" || Space == "eds")
        S += "," + Space.str();
      // ⛔ NO `Space != "eds"` GUARD HERE, and that is a measured decision, not an omission.
      // The first version had one; mutant MP2 removed it and the comparer stayed 96/96, because an
      // EDS object is already forced FAR in clang/lib/CodeGen/Targets/DSPIC.cpp (`ForcedFar = IsEds
      // || ...`), so `Near` is false by the time this runs. A second guard that can never fire
      // reads as load-bearing to the next person and is not. Session 97's precedent: a fix its own
      // mutants showed to be inert was simplified away rather than kept for comfort.
      if (Near && Space != "dma")
        S += ",near";
    }
    // trellis session 109: page (`bss,page`), keep (`data,near,keep`), noload -- the trailing set.
    // trellis session 122: ...once. space(psv|auto_psv) has already written `psv,page`, and a
    // page attribute on top of it printed `psv,page,page` (cc1: `psv,page`). Mutant MP7.
    if (GV && GV->hasAttribute("dspic-page") && Space != "psv" && Space != "auto_psv")
      S += ",page";
    if (Noload)
      S += ",noload";
    if (GV && GV->hasAttribute("dspic-keep") && !GV->hasAttribute("dspic-priority"))
      S += ",keep";
    return S;
  }

  MCSection *getExplicitSectionGlobal(const GlobalObject *GO, SectionKind Kind,
                                      const TargetMachine &TM) const override {
    // trellis session 96 (follow-up 14). A named section on a FUNCTION was emitted `"a"` --
    // allocatable but not executable -- and the pic30 as refused four of stn3255's files with
    // "Cannot locate executable code in a data section". Anything with a pic30 placement
    // attribute takes the attribute-carrying name; everything else keeps the L1e/session-90
    // path below, unchanged.
    {
      // ⛔ EVERY explicit section takes the pic30 spelling, not only the ones carrying a
      // placement attribute. cc1 does: a plain `section(".can_buffers")` on initialised data is
      // `.can_buffers,data,near`, and the `near` is load-bearing under -msmall-data. The first
      // spelling of this gated on the attributes and left ordinary named sections on the ELF
      // path, where they came out `"aw",@progbits` with no near.
      if (GO->getAddressSpace() != 1) {
        const auto *GV = dyn_cast<GlobalVariable>(GO);
        // ⛔ trellis session 124 (P3): THE DECIDING OBJECT'S attributes, not this object's. This
        // arm spelled each object's own, so two globals sharing an explicit section name emitted
        // TWO `.section` lines -- and with two different address() values the pic30 assembler
        // REFUSES the file where cc1's assembles, with no compiler diagnostic. A FUNCTION keeps
        // its own (it is not a GlobalVariable and no scan can see it; a function sharing a name
        // with a variable is refused by clang itself, on every target). Mutant MO4 restricts the
        // scan back to addrspace 1.
        const GlobalObject *Dec = GV ? cast<GlobalObject>(dspicDecider(GV, Kind, GO->getSection()))
                                     : GO;
        // ⛔ trellis session 128: reverse(N), unordered and space(auto_psv) make cc1 IGNORE a
        // written section name. Delegation is right because the NAME TARGET coincides -- over
        // every named/unnamed pair measured, the name cc1 gives a triggering object is the name
        // its UNNAMED path gives it: the same generated `*_<hex>` for reverse and unordered, the
        // same fixed `.const` for space(auto_psv). So this delegates rather than reimplementing: the
        // `Placed` block in SelectSectionForGlobal already handles all three, and has said in its
        // own comment since session 109 that cc1 ignores an explicit section for space(auto_psv).
        //
        // ⛔ IT IS NOT "THE OBJECT BEHAVES AS THOUGH NO NAME HAD BEEN WRITTEN", and this comment
        // asserted exactly that until a refutation pass broke it. `pic30.c:3415` clears
        // `implied_psv` INSIDE the named branch and the unnamed path does not, so for a READ-ONLY
        // object carrying no explicit `space()` cc1's two paths DIVERGE:
        //     unordered + const    named `*_hex,data`   unnamed `.const,psv,page`
        //     reverse(64) + const  named `...,data`     unnamed `...,psv,page`
        // RAM against program memory, at both levels and every memory model. cc1 discards the
        // NAME; the PSV decision stays the NAMED path's.
        // ⚠ Ours agrees with cc1 on that shape anyway -- measured pre-row and post-row -- because
        // ours implements neither `implied_psv` nor cc1's `if (u && psv) u = 0`, so ours' named
        // and unnamed answers coincide. But the GROUND is the name target, not the behaviour: the
        // day ours grows an implied-psv rule, this delegation must be re-asked, and
        // discard-compare's `const` cells are what would say so.
        //
        // ⛔ AFTER the dspicDecider call above, NOT before, and that ordering is the fix's second
        // half. The conflict diagnostic fires once per section name, when the LAST object
        // carrying it is placed; returning early would skip it whenever that last object is the
        // triggering one -- and cc1 reports the conflict for exactly that case (it warns
        // "Ignoring explicit section name" and then errors). `Dec` is deliberately computed and
        // discarded on this path: the call is wanted for its diagnostic, not its result.
        //
        // The trigger set is NOT repeated here. clang marks the object
        // (Targets/DSPIC.cpp) and this reads the marker, so the three attributes are named in one
        // place and cannot drift apart in two.
        if (GV && GV->hasAttribute("dspic-ignore-section-name"))
          return SelectSectionForGlobal(GO, Kind, TM);
        std::string Name = (GO->getSection() + pic30Attrs(Dec, Kind)).str();
        unsigned Flags = ELF::SHF_ALLOC;
        unsigned Type = ELF::SHT_PROGBITS;
        if (isa<Function>(GO))
          Flags |= ELF::SHF_EXECINSTR;
        else {
          if (!Kind.isReadOnly())
            Flags |= ELF::SHF_WRITE;
          // L1e (session 89): a zero-initialised global in a named section is BSS, or the pic30
          // linker classifies it "attributes = data" and cannot place it.
          if (!GV || !GV->hasInitializer() ||
              (GV->getInitializer() && GV->getInitializer()->isNullValue()))
            Type = ELF::SHT_NOBITS;
        }
        return getContext().getELFSection(Name, Type, Flags);
      }
    }
    // L1e (session 89), explicit-section bss: a zero-initialised global in a named section is
    // BSS. LLVM would emit it @progbits (a DATA section of zeros) for a section it cannot prove
    // nobits; the pic30 linker then classifies it "attributes = data" and cannot place the far
    // `.comm_buffer` (8200 bytes). cc1 emits `.comm_buffer,bss`. Force NOBITS for a zero/absent
    // initializer. (Program-space `__prog__` placement -- code-flagged sections + tbloffset --
    // is the address-space model, tagged for a following increment.)
    StringRef Name = GO->getSection();
    // L1e prog-space: a program-memory global (addrspace 1) goes in a CODE section (cc1's `,code`)
    // so it lands in program memory and the assembler accepts tbloffset on its symbol.
    // trellis session 122: ...and when it also carries address() / noload / keep, the section line
    // must say so -- cc1: `mysec,address(75520),code`. This arm returned the bare name, so the
    // address was dropped without a word (the same defect as the default-section arm below,
    // behind the same `!= 1` guard; a refutation pass found the second copy). Mutant MP2.
    // ⛔ AND TWO `__prog__` OBJECTS UNDER ONE NAME (the fourth version): cc1 writes ONE `.section`
    // line per name and it is the LAST object's -- two address()es: the last; two priorities: the
    // last; `keep` on the first only: no keep; on the second only: keep for both (progaddr-ask
    // d1-d8). Spelling each object's own attributes gave the assembler two lines for one name,
    // and it REFUSES two addresses ("conflicts with previous value") where the vendor's output
    // assembles, and silently takes the FIRST priority where the vendor's says the last. So the
    // attributes come from the last addrspace(1) object the module places under this name.
    // Mutant MP6 removes the scan.
    // ⛔ THE FIFTH VERSION, after another refutation pass. "The last object's" is cc1's answer AT
    // -Os, not its rule: cc1's deciding object is the first one it EMITS, and it emits in reverse
    // declaration order at -O1 and above and forward at -O0 (asked: the `## cc1-O0` block). The
    // source is CONTRADICTORY -- one section cannot sit at two addresses -- so the unit gets a
    // WARNING that names the section, both objects and both placements and says which is used.
    // And a DECLARATION places nothing: the scan counted an undefined `extern`, which then decided
    // a defined object's address (a regression the pass found; mutant MP8).
    if (const auto *PV = dyn_cast<GlobalVariable>(GO); PV && GO->getAddressSpace() == 1) {
      const GlobalVariable *Last = PV;
      for (const GlobalVariable &G : PV->getParent()->globals())
        if (G.getAddressSpace() == 1 && G.hasSection() && G.getSection() == Name &&
            !G.isDeclaration())
          Last = &G;
      if (GO == Last)   // once per name: when the deciding object itself is placed. Mutant MP9.
        for (const GlobalVariable &G : PV->getParent()->globals())
          if (&G != Last && G.getAddressSpace() == 1 && G.hasSection() &&
              G.getSection() == Name && !G.isDeclaration() &&
              pic30Attrs(&G, Kind, "prog") != pic30Attrs(Last, Kind, "prog"))
            GO->getContext().diagnose(DiagnosticInfoGeneric(
                "section '" + Name + "' is given two placements: '" + G.getName() + "' asks for '" +
                    StringRef(pic30Attrs(&G, Kind, "prog")).ltrim(',') + "' and '" +
                    Last->getName() + "' for '" +
                    StringRef(pic30Attrs(Last, Kind, "prog")).ltrim(',') +
                    "'; a section has one, and the second object's is used. NOTE: the "
                    "deciding object here is the last in MODULE order, which is CREATION order -- "
                    "an extern referenced above the definitions moves its object to the front -- "
                    "and the vendor compiler decides by its own emission order, so the two can "
                    "differ on exactly that shape (trellis session 123)",
                DS_Warning));
      // ⛔ trellis session 123: `dspic-space` WAS MISSING FROM THIS LIST, so an object with
      // `space(psv)` and an explicit `section()` and nothing else fell past it and was written
      // `sq1,"ax",@progbits` -- READONLY, PSV and PAGE dropped, with NO diagnostic -- where cc1
      // writes `sq1,psv,page`. Adding an unrelated `keep` restored all three, which is the single
      // axis that names the cause. PAGE is the vendor's own cross-page guard, so this is the
      // founding defect class of this arm (an attribute dropped without a word) one attribute
      // over. Found by a refutation pass. Mutant MP10.
      if (Last->hasAttribute("dspic-address") || Last->hasAttribute("dspic-noload") ||
          Last->hasAttribute("dspic-keep") || Last->hasAttribute("dspic-page") ||
          Last->hasAttribute("dspic-space") ||
          Last->hasAttribute("dspic-priority"))
        return getContext().getELFSection((Name + pic30Attrs(Last, Kind, "prog")).str(),
                                          ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    }
    if (GO->getAddressSpace() == 1)
      return getContext().getELFSection(Name, ELF::SHT_PROGBITS,
                                        ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    unsigned Flags = ELF::SHF_ALLOC;
    if (!Kind.isReadOnly() && !Kind.isText())
      Flags |= ELF::SHF_WRITE;
    unsigned Type = ELF::SHT_PROGBITS;
    if (const auto *GV = dyn_cast<GlobalVariable>(GO))
      if (!GV->hasInitializer() ||
          (GV->getInitializer() && GV->getInitializer()->isNullValue()))
        Type = ELF::SHT_NOBITS;
    return getContext().getELFSection(Name, Type, Flags);
  }

  MCSection *SelectSectionForGlobal(const GlobalObject *GO, SectionKind Kind,
                                    const TargetMachine &TM) const override {
    // trellis session 111: a packed-flash object (`__pack_upper_byte`, addrspace 4) goes in its own
    // section carrying the `packedflash` attribute -- cc1's `*_<hash>,packedflash`, ours named by
    // the symbol, which the GPL assembler gives the same flags (prints/l1f/eds/pack-ask2.txt:
    // CONTENTS, ALLOC, LOAD, PACKEDFLASH). Always PROGBITS: it is flash, and cc1 emits `.skip` for
    // a zero-initialised packed object rather than a bss section. Read-only for the assembler's
    // purposes (no SHF_WRITE): every write is refused in Sema.
    if (GO->getAddressSpace() == 4 && !isa<Function>(GO))
      return getContext().getELFSection((".packed." + GO->getName()).str() + ",packedflash",
                                        ELF::SHT_PROGBITS, ELF::SHF_ALLOC);
    // L1d (trellis session 88): an interrupt function's BODY goes in `.isr.isr.text` (cc1's
    // `,code,keep`), and the vector is wired by the linker script from the function's SYMBOL
    // NAME -- there is no vector table. The retain/keep flag is a link concern (L1g).
    if (const auto *F = dyn_cast<Function>(GO)) {
      if (F->hasFnAttribute("interrupt"))
        return getContext().getELFSection(".isr.isr.text", ELF::SHT_PROGBITS,
                                          ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
      // trellis session 109: a function with a placement attribute gets its OWN section carrying
      // it (cc1's `*_hash,...,code`; ours `.text.<name>` -- the name-derived attributes agree).
      if (F->hasFnAttribute("dspic-address") || F->hasFnAttribute("dspic-noload") ||
          F->hasFnAttribute("dspic-keep") || F->hasFnAttribute("dspic-priority"))
        return getContext().getELFSection((".text." + GO->getName()).str() + pic30Attrs(GO, Kind),
                                          ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    }
    // trellis session 96 (follow-up 14): a placement attribute with NO section() names cc1's
    // own default -- and for a bare `persistent` a per-object section, where cc1 generates a
    // hashed name and this uses the symbol.
    // ⛔ trellis session 130 corrects the rest of this comment, which had been here since 96 and
    // which nothing had checked. There is NO `*(.pbss*)` rule in p33CK1024MP705.gld and none in
    // any of the pack's 82 dsPIC33C scripts: `.pbss.<sym>` is placed by its ATTRIBUTES, like
    // every other data section. And `.prog,code` is NOT cc1's default for space(prog) -- cc1
    // gives such an object its own section (~60 cells in perobj-ask.py; `.prog` was never once
    // cc1's answer), which is what the chain below now does.
    if (const auto *GVar = dyn_cast<GlobalVariable>(GO)) {
      bool Placed = GVar->hasAttribute("dspic-space") ||
                    GVar->hasAttribute("dspic-persistent") ||
                    GVar->hasAttribute("dspic-noload") ||
                    GVar->hasAttribute("dspic-address") ||
                    // trellis session 109: each of these is a per-object section in cc1
                    GVar->hasAttribute("dspic-keep") || GVar->hasAttribute("dspic-page") ||
                    GVar->hasAttribute("dspic-reverse") ||
                    GVar->hasAttribute("dspic-unordered") ||
                    GVar->hasAttribute("dspic-priority");
      if (Placed && GO->getAddressSpace() != 1) {
        StringRef Space = GVar->hasAttribute("dspic-space")
                              ? GVar->getAttribute("dspic-space").getValueAsString()
                              : StringRef();
        // trellis session 109: the per-object base is the object's DEFAULT section's name plus
        // the symbol (`.ndata.<sym>` / `.nbss.<sym>` / `.data.<sym>` / `.bss.<sym>`), so the
        // attributes the pic30 assembler derives from the NAME agree with the ones spelled out.
        // space(auto_psv) is the shared `.const` (cc1 ignores an explicit section for it).
        bool Zero = !GVar->hasInitializer() ||
                    (GVar->getInitializer() && GVar->getInitializer()->isNullValue());
        bool Near = !GVar->hasAttribute("far");
        bool PerObject = !GVar->hasAttribute("dspic-space") && !GVar->hasAttribute("dspic-persistent");
        // ⛔ trellis session 130: THE BASE FOLLOWS cc1's LADDER, WHICH TESTS THE u/a/r ARMS
        // BEFORE THE SPACE.  This block used to test the space first, so an object with an
        // explicit space landed in the SHARED `.const` / `.prog` / `.ndata` where cc1 gives it
        // its own section -- and that is not a placement difference but a BUILD FAILURE and a
        // SILENT MERGE.  Measured on the banked pre-change binary (perobj-compare.py, 303/61):
        // the spelled tokens contradict the implied attributes of the EXACT names `.const` and
        // `.ndata`, so the pic30 assembler REFUSED ours' output for `auto_psv+address`,
        // `psv+address`, `data+reverse`, `data+page` and `prog+page` -- all of which cc1
        // compiles; and in eight further shapes ours emitted TWO CONTRADICTORY `.section` LINES
        // NAMING ONE SECTION, which the assembler ACCEPTS (`.prog,code` beside
        // `.prog,code,noload`, where noload means do not load into the device).
        //
        // WHEN cc1 USES A SHARED BASE AT ALL, measured in perobj-ask2.py over the eight accepted
        // spaces and every attribute in the `Placed` predicate, at -Os and -O0:
        //    `.const` iff space(auto_psv) and no reverse and no address -- keep, page, priority,
        //             unordered and noload all STAY shared (cc1 reaches its `else if (psv)` arm)
        //    `.ndata` iff space(data) and NO other placement attribute at all (cc1 reaches its
        //             final `else`, where each of those clears `is_default`)
        // ⛔ The two arms do NOT have one trigger set -- `keep` sends space(data) per-object and
        // leaves space(auto_psv) shared -- which is why the rule cannot be written from either.
        //
        // THE NAME COSTS NOTHING HERE, AND THAT IS MEASURED, NOT ASSUMED: pic30's implied
        // attributes are matched EXACTLY (bfd/pic30-attributes.c:83 compares strlen AND strcmp
        // against 14 MASK4 names), so `.const.<sym>`, `.psv.<sym>` and `.nbss.<sym>` are
        // byte-identical to each other and to cc1's own `*_<hex>` over 26 token strings, and the
        // device linker has no name rule for any data section in any of the pack's 82 scripts.
        // ⛔ TWO NAMES ARE EXCLUDED AND THE REASON IS A SECOND MECHANISM: BFD's own
        // _bfd_elf_get_special_section (bfd/elf.c:3110) is PREFIX-matched on a dot boundary, so
        // `.text.<sym>` derives CODE and `.data.<sym>` derives DATA, and it UNIONS with the
        // spelled token instead of yielding to it (`.data.a,bss` is DATA).  The tail arm below
        // still spells `.data.`, and it is safe only because `.data.` is reached exactly when
        // !Zero and `,bss` exactly when Zero -- a coincidence between two decisions, asserted by
        // a cell rather than left to hold by luck.
        // ⛔ AND THE SHARED `.const` MUST KEEP EXISTING: pic30_elf32.em:5291 computes
        // `__const_psvpage` / `__const_length` from the output section of EXACTLY that name and
        // falls back to page 1 when it is absent, and our codegen and the vendor libc read it.
        // space(auto_psv) without reverse/address still lands there, and no ordinary `const`
        // object reaches this block at all.
        bool HasRev = GVar->hasAttribute("dspic-reverse");
        bool HasAddr = GVar->hasAttribute("dspic-address");
        bool OnlySpace = !GVar->hasAttribute("dspic-persistent") &&
                         !GVar->hasAttribute("dspic-noload") && !HasAddr && !HasRev &&
                         !GVar->hasAttribute("dspic-keep") && !GVar->hasAttribute("dspic-page") &&
                         !GVar->hasAttribute("dspic-unordered") &&
                         !GVar->hasAttribute("dspic-priority");
        // ⛔ trellis session 130, THE SHAPE AXIS -- and this is a correction to the first version
        // of this very edit, found by `steps/placement/compare.sh`, an instrument I had not run.
        // The asks that produced the rule above varied every ATTRIBUTE against every SPACE and
        // held the OBJECT SHAPE fixed at one literal each, and cc1's answer depends on it
        // (perobj-ask4.py, 8 spaces x 6 shapes):
        //    space(psv)      per-object in ALL SIX shapes
        //    space(auto_psv) `.const`    in ALL SIX shapes
        //    space(prog)     per-object when INITIALISED, shared `.prog` when NOT -- which is
        //                    what steps/placement/ask.c:24 has asserted since session 96
        //    space(data)     the object's OWN DEFAULT BASE, by zero-ness and near-ness:
        //                    `.ndata` / `.nbss` / `.data` / `.bss`, never a fixed `.ndata`
        // "A shape asked at one literal is a shape not asked" is session 127's own must-not,
        // and the first version of this block committed it.
        bool SharedConst = Space == "auto_psv" && !HasRev && !HasAddr;
        bool SharedProg = Space == "prog" && Zero;
        bool SharedNData = Space == "data" && OnlySpace;
        // the default base for this object, WITHOUT the symbol -- what cc1's own default section
        // machinery picks, and what `space(data)` alone therefore gets.
        std::string DefBase = Zero ? (Near ? ".nbss" : ".bss") : (Near ? ".ndata" : ".data");
        std::string Base = GVar->hasAttribute("dspic-persistent")
                               ? (".pbss." + GO->getName()).str()
                               : (SharedConst ? std::string(".const")
                                  : SharedProg ? std::string(".prog")
                                  : SharedNData ? DefBase
                                  : Space == "prog" ? (".prog." + GO->getName()).str()
                                  : (Space == "psv" || Space == "auto_psv")
                                      ? (".const." + GO->getName()).str()
                                  : Space == "dma" ? (".dma." + GO->getName()).str()
                                  : (Space == "xmemory" || Space == "ymemory" || Space == "eds")
                                      ? ("." + Space + "." + GO->getName()).str()
                                  : ((Zero ? (Near ? ".nbss." : ".bss.") : (Near ? ".ndata." : ".data.")) +
                                     GO->getName()).str());
        unsigned Flags = ELF::SHF_ALLOC;
        if (Space == "prog" || Space == "psv" || Space == "auto_psv")
          Flags |= ELF::SHF_EXECINSTR;
        else
          Flags |= ELF::SHF_WRITE;
        unsigned Type = (PerObject && Zero) ? ELF::SHT_NOBITS : ELF::SHT_PROGBITS;
        return getContext().getELFSection(Base + pic30Attrs(GO, Kind), Type, Flags);
      }
    }
    // trellis session 122: ...unless it carries address(N). The placement block above is guarded
    // `getAddressSpace() != 1`, so a `__prog__` object reached the shared `.const` below with its
    // address() never read -- dropped without a diagnostic, where cc1 writes
    // `*_<hash>,address(N),code` (prints/l1f/frontend/progaddr/ask.txt). Found by EXECUTION
    // against the vendor's image of the same source (relopexec X11-X13). A section PER OBJECT: the
    // assembler refuses two addresses under one section name. The attributes come from
    // pic30Attrs, so `noload`, `keep` and space(psv)'s `psv,page` survive. Mutant MP1.
    if (const auto *PV = dyn_cast<GlobalVariable>(GO);
        PV && GO->getAddressSpace() == 1 &&
        (PV->hasAttribute("dspic-address") || PV->hasAttribute("dspic-noload") ||
         PV->hasAttribute("dspic-keep") || PV->hasAttribute("dspic-page") ||
         PV->hasAttribute("dspic-priority")))
      return getContext().getELFSection(
          (".prog." + GO->getName() + pic30Attrs(GO, Kind, "prog")).str(), ELF::SHT_PROGBITS,
          ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    // L1e prog-space: an addrspace(1) global with no explicit section still goes to program memory.
    if (GO->getAddressSpace() == 1 && !isa<Function>(GO))
      return getContext().getELFSection(".const", ELF::SHT_PROGBITS,
                                        ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    // Functions keep the default `.text` handling.
    if (Kind.isText())
      return TargetLoweringObjectFileELF::SelectSectionForGlobal(GO, Kind, TM);
    // Read-only data -> the SHARED .const (never per-object). The pic30 as special-cases the
    // exact name .const to give it the psv/code attribute AND allow near data access; a
    // per-object .const.<sym> (from -fdata-sections) gets neither (probe psvprobe: alloc-only
    // refuses tbloffset, exec refuses a data reference), so const cannot be split on this
    // assembler. Dead const is therefore not stripped, but dead functions (.text.*) and near
    // data (.ndata/.nbss.*) still are -- which is where the size is.
    // trellis session 96 (follow-up 15): a `const` object under -mconst-in-data leaves program
    // memory for ordinary data placement, near or far by the same rule as any other object.
    // ⛔ BEFORE the readonly return, not after. The first spelling of this sat below it and was
    // DEAD CODE -- every const had already left for ConstSection.
    if (const auto *GVc = dyn_cast<GlobalVariable>(GO))
      if (GVc->hasAttribute("dspic-const-in-data")) {
        bool Near = !GVc->hasAttribute("far");
        return pic30Section(Near ? ".ndata" : ".data", GO, Kind, TM,
                            ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_WRITE);
      }
    if (Kind.isReadOnly() || Kind.isMergeableCString() || Kind.isMergeableConst())
      return ConstSection;
    // Session 90: `far` data (the attribute clang forwards) leaves the near 4 KB: cc1 places it
    // in plain `.bss`/`.data` (bss/data attributes, no `near`), which a near-tight script
    // allocates anywhere in data memory. The 13-bit file forms are refused on it in isel.
    if (const auto *GV = dyn_cast<GlobalVariable>(GO))
      if (GV->hasAttribute("far")) {
        if (Kind.isBSS())
          return pic30Section(".bss", GO, Kind, TM, ELF::SHT_NOBITS,
                              ELF::SHF_ALLOC | ELF::SHF_WRITE);
        return pic30Section(".data", GO, Kind, TM, ELF::SHT_PROGBITS,
                            ELF::SHF_ALLOC | ELF::SHF_WRITE);
      }
    // Zero-initialised data -> `.nbss` (near).
    if (Kind.isBSS())
      return pic30Section(".nbss", GO, Kind, TM, ELF::SHT_NOBITS,
                          ELF::SHF_ALLOC | ELF::SHF_WRITE);
    // Everything else (initialised data) -> `.ndata` (near).
    return pic30Section(".ndata", GO, Kind, TM, ELF::SHT_PROGBITS,
                        ELF::SHF_ALLOC | ELF::SHF_WRITE);
  }
};
} // namespace

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeDSPICTarget() {
  // Register the target.
  RegisterTargetMachine<DSPICTargetMachine> X(getTheDSPICTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeDSPICAsmPrinterPass(PR);
  initializeDSPICDAGToDAGISelLegacyPass(PR);
}

static Reloc::Model getEffectiveRelocModel(std::optional<Reloc::Model> RM) {
  return RM.value_or(Reloc::Static);
}

DSPICTargetMachine::DSPICTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         std::optional<Reloc::Model> RM,
                                         std::optional<CodeModel::Model> CM,
                                         CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               getEffectiveRelocModel(RM),
                               getEffectiveCodeModel(CM, CodeModel::Small), OL),
      TLOF(std::make_unique<DSPICTargetObjectFile>()),
      Subtarget(TT, std::string(CPU), std::string(FS), *this) {
  // L1f-b: the pic30 `as` has no `.addrsig`; clang asks for it by default.
  this->Options.EmitAddrsig = false;
  this->Options.EnableMachineOutliner = true;    // outer gate
  this->Options.SupportsDefaultOutlining = true; // inner gate (setMachineOutliner did not stick)
  initAsmInfo();
}

DSPICTargetMachine::~DSPICTargetMachine() = default;

namespace {
/// DSPIC Code Generator Pass Configuration Options.
class DSPICPassConfig : public TargetPassConfig {
public:
  DSPICPassConfig(DSPICTargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  DSPICTargetMachine &getDSPICTargetMachine() const {
    return getTM<DSPICTargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPreEmitPass() override;
};
} // namespace

TargetPassConfig *DSPICTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new DSPICPassConfig(*this, PM);
}

TargetTransformInfo
DSPICTargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<DSPICTTIImpl>(this, F));
}

MachineFunctionInfo *DSPICTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return DSPICMachineFunctionInfo::create<DSPICMachineFunctionInfo>(Allocator,
                                                                      F, STI);
}

void DSPICPassConfig::addIRPasses() {
  addPass(createAtomicExpandLegacyPass());

  // trellis session 112: the `__eds__` paging carry. BEFORE the base addIRPasses() on
  // purpose -- CodeGenPrepare runs inside it and rewrites a load's addressing into integer
  // arithmetic, leaving no GetElementPtrInst for this pass to find.
  // NOTE this pipeline is NOT the one llc/clang use; DSPICCodeGenPassBuilder is.
  addPass(createDSPICEDSPtrArithPass());
  TargetPassConfig::addIRPasses();
}

bool DSPICPassConfig::addInstSelector() {
  // Install an instruction selector.
  addPass(createDSPICISelDag(getDSPICTargetMachine(), getOptLevel()));
  return false;
}

void DSPICPassConfig::addPreEmitPass() {
  // Must run branch selection immediately preceding the asm printer.
  addPass(createDSPICPeepholeLegacyPass());
  addPass(createDSPICBranchSelectLegacyPass());
  // trellis session 103: the compare fusion runs AFTER the branch selector, not before. It needs
  // settled block offsets to know whether a 6-bit displacement reaches, and it is safe there
  // because fusing only removes words -- every distance the selector already judged can shrink
  // and none can grow.
  addPass(createDSPICCmpFuseLegacyPass());
}
