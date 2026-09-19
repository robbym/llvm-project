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
    std::string T;
    SmallVector<StringRef, 8> Parts;
    Attrs.split(Parts, ',');
    for (StringRef P : Parts) {
      P = P.trim();
      if (P == "near" || P == "far" || P == "noload")
        T += (P + ",").str();
      else if (P.starts_with("address("))
        T += "address,";
    }
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
    std::string S;
    // cc1 puts address() FIRST, ahead of the space attribute, and in decimal.
    if (GV && GV->hasAttribute("dspic-address"))
      S += ",address(" +
           GV->getAttribute("dspic-address").getValueAsString().str() + ")";
    // trellis session 109: `reverse(N)` next (var.cc1.s: `reverse(64),bss`), then priority.
    if (GV && GV->hasAttribute("dspic-reverse"))
      S += ",reverse(" + GV->getAttribute("dspic-reverse").getValueAsString().str() + ")";
    S += pic30Priority(GO);
    if (GV && GV->hasAttribute("dspic-persistent")) {
      if (Near)
        S += ",near";
      S += ",persist";
    } else if (Space == "prog") {
      S += ",code";
    } else if (Space == "psv" || Space == "auto_psv") {
      S += ",psv,page";
    } else {
      bool Zero = !GV || !GV->hasInitializer() ||
                  (GV->getInitializer() && GV->getInitializer()->isNullValue());
      S += Zero ? ",bss" : ",data";
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
    // own default -- `.prog,code` for space(prog), and for a bare `persistent` a per-object
    // section, where cc1 generates a hashed name and this uses the symbol, which the linker
    // script's `*(.pbss*)` rule collects the same way.
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
        std::string Base = GVar->hasAttribute("dspic-persistent")
                               ? (".pbss." + GO->getName()).str()
                               : (Space == "prog" ? std::string(".prog")
                                  : (Space == "psv" || Space == "auto_psv") ? std::string(".const")
                                  : Space == "dma" ? (".dma." + GO->getName()).str()
                                  : (Space == "xmemory" || Space == "ymemory" || Space == "eds")
                                      ? ("." + Space + "." + GO->getName()).str()
                                  : !PerObject ? std::string(".ndata")
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
