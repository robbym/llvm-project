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
#include "DSPIC.h"
#include "DSPICMachineFunctionInfo.h"
#include "TargetInfo/DSPICTargetInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/GlobalObject.h"
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

  MCSection *getExplicitSectionGlobal(const GlobalObject *GO, SectionKind Kind,
                                      const TargetMachine &TM) const override {
    // L1e (session 89), explicit-section bss: a zero-initialised global in a named section is
    // BSS. LLVM would emit it @progbits (a DATA section of zeros) for a section it cannot prove
    // nobits; the pic30 linker then classifies it "attributes = data" and cannot place the far
    // `.comm_buffer` (8200 bytes). cc1 emits `.comm_buffer,bss`. Force NOBITS for a zero/absent
    // initializer. (Program-space `__prog__` placement -- code-flagged sections + tbloffset --
    // is the address-space model, tagged for a following increment.)
    StringRef Name = GO->getSection();
    // L1e prog-space: a program-memory global (addrspace 1) goes in a CODE section (cc1's `,code`)
    // so it lands in program memory and the assembler accepts tbloffset on its symbol.
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
    // L1d (trellis session 88): an interrupt function's BODY goes in `.isr.isr.text` (cc1's
    // `,code,keep`), and the vector is wired by the linker script from the function's SYMBOL
    // NAME -- there is no vector table. The retain/keep flag is a link concern (L1g).
    if (const auto *F = dyn_cast<Function>(GO))
      if (F->hasFnAttribute("interrupt"))
        return getContext().getELFSection(".isr.isr.text", ELF::SHT_PROGBITS,
                                          ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    // L1e prog-space: an addrspace(1) global with no explicit section still goes to program memory.
    if (GO->getAddressSpace() == 1 && !isa<Function>(GO))
      return getContext().getELFSection(".const", ELF::SHT_PROGBITS,
                                        ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
    // Functions keep the default `.text` handling.
    if (Kind.isText())
      return TargetLoweringObjectFileELF::SelectSectionForGlobal(GO, Kind, TM);
    // Read-only data -> `.const` (PSV / program memory).
    if (Kind.isReadOnly() || Kind.isMergeableCString() || Kind.isMergeableConst())
      return pic30Section(".const", GO, Kind, TM, ELF::SHT_PROGBITS, ELF::SHF_ALLOC);
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

MachineFunctionInfo *DSPICTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return DSPICMachineFunctionInfo::create<DSPICMachineFunctionInfo>(Allocator,
                                                                      F, STI);
}

void DSPICPassConfig::addIRPasses() {
  addPass(createAtomicExpandLegacyPass());

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
}
