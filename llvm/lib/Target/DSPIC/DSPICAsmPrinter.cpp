//===-- DSPICAsmPrinter.cpp - DSPIC LLVM assembly writer ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the DSPIC assembly language.
//
//===----------------------------------------------------------------------===//

#include "DSPICAsmPrinter.h"
#include "DSPICMachineFunctionInfo.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "MCTargetDesc/DSPICInstPrinter.h"
// S_HANDLE lives here; DSPICMCInstLower.cpp reaches it the same way.
#include "MCTargetDesc/DSPICMCAsmInfo.h"
#include "DSPICMCInstLower.h"
#include "DSPICTargetMachine.h"
#include "TargetInfo/DSPICTargetInfo.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/PassManager.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Target/TargetLoweringObjectFile.h"  // trellis session 99: the NOBITS arm below
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/IOSandbox.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {

// The config-words row (trellis session 92): the device's configuration-word database -- the
// DFP's xc16/bin/config/<device>/aux_configuration.data -- read at the end of a file that carries
// `#pragma config` markers. Without it a pragma is a fatal error, never silence.
static cl::opt<std::string> DSPICConfigDB(
    "dspic-config-db", cl::init(""),
    cl::desc("dsPIC: the configuration-word database for #pragma config "
             "(the DFP's xc16/bin/config/<device>/aux_configuration.data)"));

// trellis session 108 (post-close): annotate every emitted instruction with the size the backend
// BELIEVES it has (`getInstSizeInBytes`, the number the outliner and the branch selector price
// with) and its opcode, as a trailing comment `; size=N OPCODE`. steps/sizes/audit.py compares
// the claim with the disassembly's actual width. Default off: prints must not change.
static cl::opt<bool> DSPICPrintSizes(
    "dspic-print-sizes", cl::init(false), cl::Hidden,
    cl::desc("dsPIC: annotate each instruction with getInstSizeInBytes and its opcode"));

  class DSPICAsmPrinter : public AsmPrinter {
  public:
    DSPICAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
        : AsmPrinter(TM, std::move(Streamer), ID) {}

    StringRef getPassName() const override { return "DSPIC Assembly Printer"; }

    bool runOnMachineFunction(MachineFunction &MF) override;

    void PrintSymbolOperand(const MachineOperand &MO, raw_ostream &O) override;
    void printOperand(const MachineInstr *MI, int OpNum, raw_ostream &O,
                      bool PrefixHash = true);
    void printSrcMemOperand(const MachineInstr *MI, int OpNum,
                            raw_ostream &O);
    bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                         const char *ExtraCode, raw_ostream &O) override;
    bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                               const char *ExtraCode, raw_ostream &O) override;
    void emitInstruction(const MachineInstr *MI) override;

    /// Session 96: the jump table is a run of `bra` INSTRUCTIONS, so LLVM's own data emission
    /// must not also run -- there is no data.
    void emitJumpTableInfo() override {}

    /// Session 96: a function's address AS DATA is `handle(_f)` -- the address of the veneer the
    /// linker builds, which is what makes a function pointer fit in 16 bits on a part whose
    /// program memory does not. The immediate form is already handled in DSPICMCInstLower; a
    /// global initializer never passes through there, so it is wrapped here.
    const MCExpr *lowerConstant(const Constant *CV, const Constant *BaseCV,
                                uint64_t Offset) override;
    /// trellis session 99: `lowerConstant` RECURSES into a ConstantExpr, so a handle must be
    /// applied only to the OUTERMOST constant. Measured from cc1: a bare label address is
    /// `handle(.L2)` and a DIFFERENCE of two label addresses is `.L3-(.L2)`, BARE -- a difference
    /// of two veneer addresses is not a code offset and means nothing. Without this guard the
    /// first version of the fix emitted `handle(.Ltmp1)-handle(.Ltmp0)`, which the assembler
    /// refused with "junk at end of line".
    bool InLowerConstant = false;

    /// ⛔ trellis session 136: AN ALIAS IS NOT DATA, AND THE ARM ABOVE WAS REACHED ON ONE.
    /// AsmPrinter::emitGlobalAlias lowers the ALIASEE through lowerConstant and hands the result
    /// to emitAssignment, so a C1/C2 constructor alias came out
    /// `__ZN1SC1Ei = handle(__ZN1SC2Ei)` -- which the pic30 assembler refuses ("junk at end of
    /// line, first unrecognized character is `('"), where cc1plus writes
    /// `.set __ZN1SC1Ei,__ZN1SC2Ei`. `handle()` is this port's PROGRAM-ADDRESS operator: on an
    /// alias there is no address to compute, because the alias IS the aliasee.
    /// ⚠ The save/restore is defensive rather than load-bearing, and the distinction is worth
    /// the sentence: AsmPrinter emits every alias in doFinalization, AFTER every global, so
    /// nothing in a TU is emitted between two aliases today. Mutant N6 drops the restore and is
    /// predicted to LIVE for exactly that reason.
    /// ⛔ AsmPrinter::emitGlobalIFunc is lowerConstant's third caller and does the same thing
    /// with the resolver. It is deliberately NOT guarded: clang answers `unknown attribute
    /// 'ifunc' ignored` for this triple (a warning, so the compile exits 0), creates no
    /// GlobalIFunc and leaves the call undefined -- measured, cxxasm-ask4 ARM I, whose banked
    /// print carries that warning verbatim -- so an override would be dead code.
    /// ⚠ The attribute's existsInTarget gate IS TargetInfo::supportsIFunc() (clang Attr.td: the
    /// IFunc attr is TargetSpecificAttr<TargetIFuncSupport>, whose CustomCode is exactly that
    /// call), so the named far side is necessary AND sufficient -- and AVR is in that function's
    /// list by ARCH alone, so a bare-metal ELF target joining it is upstream precedent.
    /// COSTED: one override and ONE CELL -- and the cell must expect up to TWO bare assignments,
    /// because the ELF arm runs the lowered expr through emitAssignment twice, for Name and for
    /// LocalAlias.
    /// ⛔ AND THAT PRICE IS THE PRICE OF NOT EMITTING A MALFORMED ASSIGNMENT. It is NOT the price
    /// of `ifunc` working: the same arm emits `.type f,@gnu_indirect_function`, and the
    /// resolution runs through an IRELATIVE relocation that a static pic30 image has no agent
    /// for. That is a separate, unpriced question, and cell D3 holds the line until someone
    /// asks it.
    bool InGlobalAlias = false;
    void emitGlobalAlias(const Module &M, const GlobalAlias &GA) override {
      bool Saved = InGlobalAlias;
      InGlobalAlias = true;
      AsmPrinter::emitGlobalAlias(M, GA);
      InGlobalAlias = Saved;
    }

    /// L1b: print the frame's accounting as a comment, so a report's frame size is a
    /// line the compiler printed (trellis standing rule 12).
    void emitFunctionBodyStart() override;
    // trellis session 98: the `.user_init` fragment, see the definition below
    void emitUserInitFragment(const MachineFunction &MF);
    // the config-words row: the `#pragma config` markers, collected here and emitted as words
    std::vector<std::string> ConfigPragmas;
    void emitGlobalVariable(const GlobalVariable *GV) override;
    void emitEndOfAsmFile(Module &M) override;
    // trellis session 109: `irq(N)` / `altirq(N)` -- the second global label at the function head
    void emitFunctionEntryLabel() override;

    void EmitInterruptVectorSection(MachineFunction &ISR);

    static char ID;
  };
} // end of anonymous namespace

void DSPICAsmPrinter::PrintSymbolOperand(const MachineOperand &MO,
                                          raw_ostream &O) {
  uint64_t Offset = MO.getOffset();
  if (Offset)
    O << '(' << Offset << '+';

  getSymbol(MO.getGlobal())->print(O, MAI);

  if (Offset)
    O << ')';
}

void DSPICAsmPrinter::printOperand(const MachineInstr *MI, int OpNum,
                                    raw_ostream &O, bool PrefixHash) {
  const MachineOperand &MO = MI->getOperand(OpNum);
  switch (MO.getType()) {
  default: llvm_unreachable("Not implemented yet!");
  case MachineOperand::MO_Register:
    O << DSPICInstPrinter::getRegisterName(MO.getReg());
    return;
  case MachineOperand::MO_Immediate:
    if (PrefixHash)
      O << '#';
    O << MO.getImm();
    return;
  case MachineOperand::MO_MachineBasicBlock:
    MO.getMBB()->getSymbol()->print(O, MAI);
    return;
  case MachineOperand::MO_GlobalAddress: {
    // If the global address expression is a part of displacement field with a
    // register base, we should not emit any prefix symbol here, e.g.
    //   mov.w glb(r1), r2
    // Otherwise (!) dspic-as will silently miscompile the output :(
    if (PrefixHash)
      O << '#';
    PrintSymbolOperand(MO, O);
    return;
  }
  }
}

void DSPICAsmPrinter::printSrcMemOperand(const MachineInstr *MI, int OpNum,
                                          raw_ostream &O) {
  const MachineOperand &Base = MI->getOperand(OpNum);
  const MachineOperand &Disp = MI->getOperand(OpNum+1);

  // Print displacement first

  // Imm here is in fact global address - print extra modifier.
  if (Disp.isImm() && Base.getReg() == DSPIC::SR)
    O << '&';
  printOperand(MI, OpNum + 1, O, /*PrefixHash=*/false);

  // Print register base field
  if (Base.getReg() != DSPIC::SR && Base.getReg() != DSPIC::PC) {
    O << '(';
    printOperand(MI, OpNum, O);
    O << ')';
  }
}

/// PrintAsmOperand - Print out an operand for an inline asm expression.
///
bool DSPICAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                       const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0])
    return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, O);

  printOperand(MI, OpNo, O);
  return false;
}

bool DSPICAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                             unsigned OpNo,
                                             const char *ExtraCode,
                                             raw_ostream &O) {
  if (ExtraCode && ExtraCode[0]) {
    return true; // Unknown modifier.
  }
  printSrcMemOperand(MI, OpNo, O);
  return false;
}

// `; frame _f: locals=N csr=C fp=F ra=4 total=T` — N is the `lnk` operand (0 without a
// linked frame), C the callee-saved bytes pushed, F 2 when w14 is saved by `lnk` else 0,
// and 4 the two-word return address `call` pushed; T is their sum: the bytes this
// function adds to the stack above the caller's w15 at the call.

// The config-words row (trellis session 92). A `#pragma config NAME = VALUE` reaches the backend
// as a marker global in section `.dspic.config` whose initializer is "file:line:NAME=VALUE" (the
// clang handler, ParsePragma.cpp). The markers are collected here and never emitted as data.
// trellis session 109: `irq(N)` / `altirq(N)`. cc1 (isr.cc1.s my_handler_52):
//     _my_handler_52:
//         .global __Interrupt52
//     __Interrupt52:
// -- the handler keeps its own exported name and gains the vector's, which the linker script wires
// by name exactly as it wires `__T1Interrupt`. The label PRECEDES the preprologue (my_everything).
void DSPICAsmPrinter::emitFunctionEntryLabel() {
  AsmPrinter::emitFunctionEntryLabel();
  const Function &F = MF->getFunction();
  for (const char *Key : {"dspic-irq", "dspic-altirq"})
    if (F.hasFnAttribute(Key)) {
      std::string Sym = (StringRef(Key) == "dspic-irq" ? "__Interrupt" : "__AltInterrupt") +
                        F.getFnAttribute(Key).getValueAsString().str();
      OutStreamer->emitRawText(Twine("\t.global\t") + Sym);
      OutStreamer->emitRawText(Twine(Sym) + ":");
    }
}

void DSPICAsmPrinter::emitGlobalVariable(const GlobalVariable *GV) {
  // trellis session 109: `fillupper(V)` on a program-memory object -- cc1 (var.cc1.s v_fill):
  //     .section *_hash,code
  //     .fillupper 0x12
  //     ... the object ...
  //     .fillupper 0x00
  // The directive is emitted INSIDE the object's section, so the section is switched first (the
  // generic emission then switches to the same one, a no-op). Mutant MP3 drops the closing one.
  if (GV->hasAttribute("dspic-fillupper")) {
    OutStreamer->switchSection(getObjFileLowering().SectionForGlobal(GV, TM));
    OutStreamer->emitRawText(Twine("\t.fillupper 0x") +
                             Twine::utohexstr(std::stoul(
                                 GV->getAttribute("dspic-fillupper").getValueAsString().str())));
    AsmPrinter::emitGlobalVariable(GV);
    OutStreamer->emitRawText(StringRef("\t.fillupper 0x00"));
    return;
  }
  if (GV->hasSection() && GV->getSection() == ".dspic.config") {
    if (GV->hasInitializer())
      if (auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer()))
        if (CDA->isCString())
          ConfigPragmas.push_back(CDA->getAsCString().str());
    return;
  }
  // trellis session 99: a zero initialiser in a NOBITS section is RESERVED, not written. The
  // pic30 assembler answers `.short 0` under `.section .nbss,...,@nobits` with "Initial values
  // are not supported in bss section '.nbss'" -- 36 times in one bl_fw build -- and cc1 writes
  // `.space`. LLVM already does the right thing for an aggregate zero (emitGlobalConstant sends
  // a ConstantAggregateZero to emitZeros), so only SCALARS take the typed path and only they are
  // wrong here. ⚠ The assembler still advances the location counter, so this is cosmetic TODAY,
  // measured symbol by symbol -- but it is a directive the assembler says it does not support,
  // and a non-zero value on that path would be dropped in silence.
  // trellis session 134: ⛔ AND NOT ON A GLOBAL THE BASE CLASS WOULD HAVE SKIPPED. A
  // zero-length appending array -- what -Oz's GlobalOpt leaves behind when it evaluates a global
  // constructor away, `[0 x {i32,ptr,ptr}] zeroinitializer` -- IS a null initializer and lands
  // in .bss, so it entered this arm and handed AppendingLinkage to emitLinkage, which answers
  // llvm_unreachable("Should never emit this"). One linkage over, available_externally trips
  // getKindForGlobal's "Can only be used for global definitions" assertion in SectionForGlobal
  // on the line below, BEFORE linkage is ever consulted. AsmPrinter::emitGlobalVariable skips
  // both via emitSpecialLLVMGlobal before it does anything else; this arm ran first.
  // ⚠ ExternalWeak is the third linkage emitLinkage refuses and needs no clause: it is a
  // declaration, so hasInitializer() is already false.
  if (GV->hasInitializer() && GV->getInitializer()->isNullValue() &&
      !GV->hasCommonLinkage() && !GV->isThreadLocal() &&
      !GV->hasAppendingLinkage() && !GV->hasAvailableExternallyLinkage()) {
    MCSection *S = getObjFileLowering().SectionForGlobal(GV, TM);
    if (S && S->isBssSection()) {
      // ⚠ The ORDER and the metadata are AsmPrinter's own, deliberately: the first version of
      // this arm emitted `.globl` before switching section and dropped `.type ,@object`
      // altogether, so the only intended difference from the standard path -- `.space` in place
      // of a typed zero -- came with two unintended ones.
      MCSymbol *Sym = getSymbol(GV);
      const DataLayout &DL = GV->getDataLayout();
      uint64_t Size = DL.getTypeAllocSize(GV->getValueType());
      Align Alignment = getGVAlignment(GV, DL);
      emitVisibility(Sym, GV->getVisibility(), !GV->isDeclaration());
      OutStreamer->switchSection(S);
      OutStreamer->emitSymbolAttribute(Sym, MCSA_ELF_TypeObject);
      emitLinkage(GV, Sym);
      emitAlignment(Alignment, GV);
      OutStreamer->emitLabel(Sym);
      OutStreamer->emitZeros(Size ? Size : 1);
      OutStreamer->emitELFSize(Sym, MCConstantExpr::create(Size, OutContext));
      return;
    }
  }
  AsmPrinter::emitGlobalVariable(GV);
}

namespace {
struct CfgValue { std::string Name; uint32_t Val; };
struct CfgSetting { std::string Name; uint32_t Mask; std::vector<CfgValue> Values; };
struct CfgWord {
  uint32_t Addr, Mask, Default, Value;
  bool Primary;
  bool Touched = false;
  std::string TouchedBy; // the first pragma's file:line, for the repeat refusal
  std::vector<CfgSetting> Settings;
};
} // namespace

// At the end of the file: the database is read (CWORD:addr:mask:default[:type] opens a word;
// CSETTING:mask:name:desc a field in it; CVALUE:value:name:desc an option of the field), each
// pragma is resolved -- the setting by name over the PRIMARY words (type 01, or every word when
// the file has no type column), the value by option name or as a number shifted into the field --
// and cc1's shape is printed for every primary word that has at least one setting (cc1 skips the
// settingless ones), highest address first: the value is the word's default with each set field
// masked in. An unknown setting or value is a fatal error naming the pragma's file:line -- cc1
// refuses those too, and silence was the defect this row repairs.
void DSPICAsmPrinter::emitEndOfAsmFile(Module &M) {
  // trellis session 122 (ROW U): A REFERENCE TO A VENDOR BUILTIN THIS COMPILER DOES NOT IMPLEMENT.
  // Since the session-121 version mirror, <xc.h> opens the vendor's <builtins.h>, which DECLARES
  // every vendor builtin -- so a call to one this compiler lacks stopped being clang's "use of
  // unknown builtin" and became an ordinary external call (`bra ___builtin_divsd`), silent at
  // -Wall, failing only at the link. The test is made HERE because the property is "an undefined
  // reference is EMITTED": this is the OPTIMISED module, so an optimiser-dead call, an uncalled
  // inline and a function defined later in the unit are all fine, as they are for the vendor's
  // toolchain (the first landing tested at the call in Sema, the second on clang's
  // pre-optimisation module; both refused programs the vendor links). A WEAK declaration is the
  // program's own guard. And only a name THE VENDOR REGISTERS (-mprint-builtins, identical over
  // eight installed 16-bit device families; trc steps/frontend/vendor-names.sh) is refused: a
  // user's own `__builtin_mine`, defined in another unit, is the linker's business and links.
  // Mutants MU1' (isDeclaration), MU5 (weak), MU6 (the vendor table), MU3' (the whole check).
  {
    static const char *const VendorBuiltins[] = {
        "ACCH", "ACCL", "ACCU", "add", "add_16", "addab", "addr", "addr_high", "addr_low",
        "ashiftrt_32_16", "bitcopy", "btg", "btg_16", "btg_32", "btg_8", "clr", "clr_prefetch",
        "clrwdt", "dataflashoffset", "disable_interrupts", "disi", "divf", "divmodsd",
        "divmodud", "divsd", "divud", "dmaoffset", "dmapage", "ed", "edac", "edsoffset",
        "edspage", "enable_interrupts", "fbcl", "fbcl_16", "ff1l", "ff1l_16", "ff1r",
        "ff1r_16", "flim", "flim_16", "flim_excess", "flim_excess_16", "flimv_excess",
        "flimv_excess_16", "get_isr_state", "lac", "lac_16", "lac_32", "lacd",
        "lshiftrt_32_16", "mac", "mac_16", "max", "max_excess", "maxv_excess", "min",
        "min_excess", "minv_excess", "modsd", "modud", "movsac", "mpy", "mpy_16", "mpyn",
        "mpyn_16", "msc", "msc_16", "mulss", "mulsu", "mulus", "muluu", "nop", "psvoffset",
        "psvpage", "pwrsav", "readsfr", "repeat_nop", "sac", "sac_16", "sac_32", "sacd",
        "sacr", "sacr_16", "sat_abs_s16", "sat_add_s16", "sat_sub_s16", "section_begin",
        "section_end", "section_size", "set_isr_state", "sftac", "software_breakpoint",
        "software_reset", "subab", "swap", "swap_16", "swap_8", "swap_byte", "tbladdress",
        "tbloffset", "tblpage", "tblrdh", "tblrdhb", "tblrdl", "tblrdlb", "tblwth", "tblwthb",
        "tblwtl", "tblwtlb", "vector_offset", "write_CRYOTP", "write_DATAFLASH",
        "write_DATAFLASH_secure", "write_DISICNT", "write_NVM", "write_NVM_secure",
        "write_OSCCONH", "write_OSCCONL", "write_PWMSFR", "write_RPCON", "write_RTCC_WRLOCK",
        "write_RTCWEN", "writesfr",
    };
    for (const Function &F : M) {
      if (!F.isDeclaration() || F.use_empty() || F.hasExternalWeakLinkage())
        continue;
      StringRef N = F.getName();
      if (!N.consume_front("__builtin_") ||
          llvm::none_of(VendorBuiltins, [&](const char *V) { return N == V; }))
        continue;
      std::string At = F.hasFnAttribute("dspic-builtin-ref")
                           ? (" (called at " +
                              F.getFnAttribute("dspic-builtin-ref").getValueAsString() + ")")
                                 .str()
                           : std::string();
      M.getContext().emitError(
          "'" + F.getName() + "'" + At +
          " is a vendor builtin this compiler does not implement: it is only DECLARED (the "
          "vendor's <builtins.h> declares every one), so this reference would fail at link as an "
          "undefined '_" + F.getName() + "'");
    }
  }
  // trellis session 109: `sfr(ADDR)` on an extern declaration is an ABSOLUTE symbol -- cc1
  // (var.cc1.s): `.equ _v_sfr_at,512`. A declaration emits nothing else, so this is where it goes.
  for (const GlobalVariable &GV : M.globals())
    if (GV.hasAttribute("dspic-sfr-address"))
      OutStreamer->emitRawText(Twine("\t.equ\t") + getSymbol(&GV)->getName() + "," +
                               GV.getAttribute("dspic-sfr-address").getValueAsString());
  // ⛔ trellis session 139: THE OBJECT SIGNATURE -- `__c30_signature`, the section the pic30 linker
  // picks EVERY library member by. It ORs each loaded object's words into one link-wide state
  // (the vendor binutils' elflink.c:4050) and chooses each member against it
  // (pic30_elf32.em:9116-9173), so an object without one leaves every choice to a tie-break: a C++
  // `sqrt(2.25)` linked the single-precision sqrt.CH_lo and returned 0 (session 138). cc1's words
  // (pic30.c:23531): 0x0001, the MASK of bits this object cares about, and MASK & SET --
  //     bit 0 unsigned_long_size_t    bit 1 unified_memory    bit 2 no_short_double.
  // The two facts come from the FRONT END as module flags (clang Targets/DSPIC.cpp), because the IR
  // does not carry them. Neither flag -> no section: a hand-written .ll states no ABI, and none is
  // invented here (cell G14a).
  // MASK is 7 for every object. cc1's is 7 in every TU on a classic device with a 16-bit size_t:
  // the public `int ()` libfuncs GCC and pic30 declare at init (17 or more: optabs-libfuncs.c:920-
  // 935, pic30.c:30030-30042) reach type_refers_to_size_t, where `int` is size_t's signed twin (SIG4).
  // ⚠ Under -menable-large-arrays cc1's bit 0 follows each public decl that reaches codegen --
  // written, implicit or pic30's own -- so a TU with no size_t in its interface is 6/0 there. These
  // words describe OUR object, whose size_t is 32 bits under the flag as cc1's is (session 140, D6):
  // every such object is 7/1 (7/5 in C++), and a pure-ours link takes the CH_lo members.
  // SET: bit 0 iff size_t is 32 bits (-mlarge-arrays), bit 1 never (this port has no unified model),
  // bit 2 iff double is 64 bits (C++; C under -mdouble=64, cell G17). Row: sig-compare.sh.
  // ⛔ ONLY WHERE THE STREAMER TAKES RAW TEXT. cc1's spelling (`info, data`) exists only as text for
  // the pic30 assembler. An object streamer (`-fintegrated-as`, Route B, not started: the operator,
  // session 133) aborts on emitRawText, and without this test every TU did (mcpu-compare S2). Via
  // -save-temps the integrated assembler still parses this text and rejects `info, data` -- every
  // object on that path is EM_MSP430, refused by ld-new; the signature there is Route B's to owe.
  {
    auto Flag = [&](StringRef Key) -> uint64_t {
      if (auto *CI = mdconst::extract_or_null<ConstantInt>(M.getModuleFlag(Key)))
        return CI->getZExtValue();
      return 0;
    };
    uint64_t SizeT = Flag("dspic-size-t-width"), Dbl = Flag("dspic-double-width");
    if (SizeT && Dbl && OutStreamer->hasRawTextSupport()) {
      unsigned Mask = 0x7, Set = (SizeT == 32 ? 0x1 : 0x0) | (Dbl == 64 ? 0x4 : 0x0);
      auto Hex4 = [](unsigned V) {
        std::string H = llvm::utohexstr(V, /*LowerCase=*/true);
        return std::string(H.size() < 4 ? 4 - H.size() : 0, '0') + H;
      };
      OutStreamer->emitRawText(StringRef("\n\t.section __c30_signature, info, data"));
      OutStreamer->emitRawText(StringRef("\t.word 0x0001"));
      OutStreamer->emitRawText("\t.word 0x" + Hex4(Mask));
      OutStreamer->emitRawText("\t.word 0x" + Hex4(Mask & Set));
    }
  }
  if (ConfigPragmas.empty())
    return;
  if (DSPICConfigDB.empty())
    report_fatal_error(Twine("#pragma config at ") + ConfigPragmas.front().substr(0, ConfigPragmas.front().rfind(':', ConfigPragmas.front().find('='))) +
                           ": no configuration database -- pass -mllvm -dspic-config-db=<the DFP's "
                           "xc16/bin/config/<device>/aux_configuration.data>",
                       /*gen_crash_diag=*/false);
  // the codegen pipeline runs under an IO sandbox (IOSandbox.h); the database is the one file
  // this backend reads, and it is read here under the sandbox's own scoped escape
  auto BypassSandbox = sys::sandbox::scopedDisable();
  auto Buf = MemoryBuffer::getFile(DSPICConfigDB);
  if (!Buf)
    report_fatal_error(Twine("#pragma config: cannot read the configuration database ") + DSPICConfigDB,
                       /*gen_crash_diag=*/false);
  std::vector<CfgWord> Words;
  SmallVector<StringRef, 512> Lines;
  (*Buf)->getBuffer().split(Lines, '\n');
  auto Hex = [](StringRef S) { uint32_t V = 0; S.trim().getAsInteger(16, V); return V; };
  for (StringRef Line : Lines) {
    Line = Line.rtrim("\r");
    SmallVector<StringRef, 6> F;
    Line.split(F, ':');
    if (F.size() < 3)
      continue;
    if (F[0] == "CWORD" && F.size() >= 4) {
      CfgWord W;
      W.Addr = Hex(F[1]);
      W.Mask = Hex(F[2]);
      W.Default = Hex(F[3]);
      W.Value = W.Default;
      // every word but the type-02 second-partition copies (the refuter, session 92: the
      // 33CK-MC/33E/33F/33EV/PIC24F-KA databases type every word 00; FBOOT here is 00)
      W.Primary = F.size() < 5 || F[4].trim() != "02";
      Words.push_back(W);
    } else if (F[0] == "CSETTING" && !Words.empty()) {
      Words.back().Settings.push_back({F[2].trim().str(), Hex(F[1]), {}});
    } else if (F[0] == "CVALUE" && !Words.empty() && !Words.back().Settings.empty()) {
      Words.back().Settings.back().Values.push_back({F[2].trim().str(), Hex(F[1])});
    }
  }
  for (const std::string &P : ConfigPragmas) {
    size_t Eq = P.find('=');
    size_t Colon = P.rfind(':', Eq);
    if (Eq == std::string::npos || Colon == std::string::npos)
      continue;
    StringRef Where(P.data(), Colon), Name(P.data() + Colon + 1, Eq - Colon - 1), Val(P.data() + Eq + 1);
    CfgWord *W = nullptr;
    CfgSetting *S = nullptr;
    for (CfgWord &Cand : Words) {
      if (!Cand.Primary)
        continue;
      for (CfgSetting &SC : Cand.Settings)
        if (SC.Name == Name) { W = &Cand; S = &SC; break; }
      if (S)
        break;
    }
    if (!S)
      report_fatal_error(Twine(Where) + ": #pragma config: unknown configuration setting '" + Name +
                             "' for this device (" + DSPICConfigDB + ")",
                         /*gen_crash_diag=*/false);
    bool Found = false;
    uint32_t V = 0;
    for (const CfgValue &CV : S->Values)
      if (CV.Name == Val) { V = CV.Val; Found = true; break; }
    if (!Found) {
      uint32_t N = 0;
      if (Val.getAsInteger(0, N))
        report_fatal_error(Twine(Where) + ": #pragma config: unknown value '" + Val + "' for setting '" +
                               Name + "'",
                           /*gen_crash_diag=*/false);
      V = (N << llvm::countr_zero(S->Mask)) & S->Mask;
    }
    // cc1 refuses a repeated setting outright, even at the same value ("multiple definitions
    // for configuration setting"); so does this, naming both pragmas
    for (const std::string &Q : ConfigPragmas) {
      if (&Q == &P)
        break;
      size_t QEq = Q.find('='), QColon = Q.rfind(':', QEq);
      if (QEq != std::string::npos && QColon != std::string::npos &&
          StringRef(Q.data() + QColon + 1, QEq - QColon - 1) == Name)
        report_fatal_error(Twine(Where) + ": #pragma config: multiple definitions for configuration setting '" +
                               Name + "' (first at " + StringRef(Q.data(), QColon) + ")",
                           /*gen_crash_diag=*/false);
    }
    W->Value = (W->Value & ~S->Mask) | (V & S->Mask);
    W->Touched = true;
  }
  std::vector<const CfgWord *> Out;
  for (const CfgWord &W : Words)
    if (W.Primary && W.Touched && !W.Settings.empty()) // cc1 emits the words a pragma touched
      Out.push_back(&W);
  llvm::sort(Out, [](const CfgWord *A, const CfgWord *B) { return A->Addr > B->Addr; });
  OutStreamer->emitRawText("; MCHP configuration words");
  for (const CfgWord *W : Out) {
    const std::string &Last = W->Settings.back().Name;
    std::string A = llvm::utohexstr(W->Addr, /*LowerCase=*/true);
    OutStreamer->emitRawText(Twine("; Configuration word @ 0x") + A);
    OutStreamer->emitRawText(Twine("\t.section\t.config_") + Last + ", code, address(0x" + A + "), keep");
    OutStreamer->emitRawText(Twine("__config_") + Last + ":");
    OutStreamer->emitRawText(Twine("\t.pword\t") + Twine(W->Value));
  }
}

// trellis session 98: the `.user_init` fragment for a function marked
// __attribute__((user_init)). cc1's rule, pic30.c:26151-26156:
//
//     .pushsection .user_init,code,keep
//     <rcall|call> _<name>
//     .popsection
//
// ⛔ THE MNEMONIC IS THE CODE MODEL'S, measured from cc1 at both models. It is taken from the
// port's OWN call-reach rule rather than re-derived: `far` forces the long form under any model,
// `near` keeps the short one under any model, and the model decides the rest -- the same sentence
// DSPICISelLowering::LowerCall works from. Mutant MU1 hardcodes `rcall` and must die at
// -mlarge-code.
void DSPICAsmPrinter::emitUserInitFragment(const MachineFunction &MF) {
  const Function &F = MF.getFunction();
  if (!F.hasFnAttribute("dspic-user-init"))
    return;
  bool Large = (MF.getSubtarget<DSPICSubtarget>().isLargeCode() &&
                !F.hasFnAttribute("near")) ||
               F.hasFnAttribute("far");
  OutStreamer->emitRawText(StringRef("\t.pushsection .user_init,code,keep"));
  OutStreamer->emitRawText(Twine("\t") + (Large ? "call" : "rcall") + " " +
                           getSymbol(&F)->getName());
  OutStreamer->emitRawText(StringRef("\t.popsection"));
}

void DSPICAsmPrinter::emitFunctionBodyStart() {
  const MachineFrameInfo &MFI = MF->getFrameInfo();
  const auto *FuncInfo = MF->getInfo<DSPICMachineFunctionInfo>();
  bool FP = MF->getSubtarget().getFrameLowering()->hasFP(*MF);
  unsigned CS = FuncInfo->getCalleeSavedFrameSize();
  unsigned Locals = FP ? MFI.getStackSize() - CS : 0;
  unsigned Fp = FP ? 2 : 0;
  // L1c: ` out=N` when the linked frame carries an outgoing call area of N bytes (part
  // of locals, the `lnk` operand); absent otherwise, so the L1b prints are unchanged.
  unsigned Out = MFI.isMaxCallFrameSizeComputed() ? MFI.getMaxCallFrameSize() : 0;
  std::string Line = (" frame " + CurrentFnSym->getName() + ": locals=" +
                      Twine(Locals) + " csr=" + Twine(CS) + " fp=" + Twine(Fp) +
                      " ra=4 total=" + Twine(Locals + CS + Fp + 4))
                         .str();
  if (Out)
    Line += (" args=" + Twine(Out)).str(); // L1f-a: pushed at the largest call, popped after it
  OutStreamer->emitRawComment(Line);
}

//===----------------------------------------------------------------------===//
// Session 96: `.word handle(_f)` for a function's address in data, cc1's own spelling. A data
// symbol is an ordinary 16-bit address and takes no handle.
// ⛔ trellis session 99: A BLOCK ADDRESS TAKES ONE TOO, and this comment used to deny it --
// "a block label (a jump-table entry) is not a symbol the linker builds a veneer for" -- which
// conflates two different things. A JUMP-TABLE entry is indeed not data any more: session 96
// replaced the `.short .LBB` table with inline relative branches, and that was right. A BLOCK
// ADDRESS (`&&label`, GCC's computed goto) IS stored as data, and cc1 emits `.word handle(.L11)`
// for it -- measured, both code models. Left bare it produced the assembler's "Cannot reference
// executable symbol (.Ltmp0) in a data context" on two of the torture suite's four refusals.
const MCExpr *DSPICAsmPrinter::lowerConstant(const Constant *CV,
                                             const Constant *BaseCV,
                                             uint64_t Offset) {
  // ⛔ ONLY THE OUTERMOST CONSTANT. This function recurses through AsmPrinter::lowerConstant
  // into a ConstantExpr's operands, so without this guard a `&&b - &&a` table came out as
  // `handle(.Ltmp1)-handle(.Ltmp0)` -- two veneer addresses subtracted, which is not a code
  // offset and which the assembler refused outright ("junk at end of line"). cc1 emits
  // `.word .L3-(.L2)` there, BARE: a bare label address takes a handle, a DIFFERENCE does not.
  bool Top = !InLowerConstant;
  InLowerConstant = true;
  const MCExpr *E = AsmPrinter::lowerConstant(CV, BaseCV, Offset);
  InLowerConstant = !Top;
  const Constant *S = CV->stripPointerCasts();
  // trellis session 136: ...and NOT when the result is about to be an ASSIGNMENT rather than
  // data. See InGlobalAlias, declared above, for why the caller decides this and not the value.
  if (Top && !InGlobalAlias && (isa<Function>(S) || isa<BlockAddress>(S)))
    E = MCSpecifierExpr::create(E, DSPIC::S_HANDLE, OutContext);
  return E;
}

void DSPICAsmPrinter::emitInstruction(const MachineInstr *MI) {
  DSPIC_MC::verifyInstructionPredicates(MI->getOpcode(),
                                         getSubtargetInfo().getFeatureBits());

  // trellis session 108 (post-close): the size claim, on the instruction's own line.
  if (DSPICPrintSizes) {
    const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
    OutStreamer->AddComment("size=" + Twine(TII->getInstSizeInBytes(*MI)) + " " +
                            TII->getName(MI->getOpcode()));
  }

  DSPICMCInstLower MCInstLowering(OutContext, *this);

  // Session 96: BR_JT expands to cc1's construct -- the computed branch, then one one-word
  // relative branch per case, inline in .text. No address is stored anywhere, so the table works
  // at any address; the previous `.short .LBB` table only worked while the function stayed
  // under 64K, which is what the stn3255 link refused ten times.
  if (MI->getOpcode() == DSPIC::BR_JT) {
    MCInst Bra;
    Bra.setOpcode(DSPIC::BrRel);   // `bra Wn`, RELATIVE -- not `goto Wn`, which is absolute
    Bra.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    EmitToStreamer(*OutStreamer, Bra);
    const MachineJumpTableInfo *MJTI = MF->getJumpTableInfo();
    const std::vector<MachineBasicBlock *> &Blocks =
        MJTI->getJumpTables()[MI->getOperand(1).getIndex()].MBBs;
    for (const MachineBasicBlock *MBB : Blocks) {
      MCInst Entry;
      Entry.setOpcode(DSPIC::JMP);
      Entry.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(MBB->getSymbol(), OutContext)));
      EmitToStreamer(*OutStreamer, Entry);
    }
    return;
  }

  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

void DSPICAsmPrinter::EmitInterruptVectorSection(MachineFunction &ISR) {
  MCSection *Cur = OutStreamer->getCurrentSectionOnly();
  const auto *F = &ISR.getFunction();
  if (F->getCallingConv() != CallingConv::MSP430_INTR) {
    report_fatal_error("dspic: an 'interrupt' function has no lowering before stage L1d "
                       "(BACKEND-PLAN.md); the attribute reached the backend and is refused here",
                       /*gen_crash_diag=*/false);
  }
  StringRef IVIdx = F->getFnAttribute("interrupt").getValueAsString();
  MCSection *IV = OutStreamer->getContext().getELFSection(
    "__interrupt_vector_" + IVIdx,
    ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR);
  OutStreamer->switchSection(IV);

  const MCSymbol *FunctionSymbol = getSymbol(F);
  OutStreamer->emitSymbolValue(FunctionSymbol, TM.getProgramPointerSize());
  OutStreamer->switchSection(Cur);
}

bool DSPICAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  // ⛔ BEFORE AsmPrinter::runOnMachineFunction, which emits the function's own
  // `.global`/`.type`/label: that is where cc1 puts the fragment (its output has the pushsection
  // block at line 4 and the label at line 12).
  emitUserInitFragment(MF);
  // L1d (trellis session 88): an ISR needs NO vector-table entry here -- its body goes in
  // `.isr.isr.text` (the TLOF) and the linker wires the vector from the symbol name, exactly
  // as cc1 does (it emits no table). The old EmitInterruptVectorSection is retired.
  SetupMachineFunction(MF);
  emitFunctionBody();
  return false;
}

char DSPICAsmPrinter::ID = 0;

INITIALIZE_PASS(DSPICAsmPrinter, "dspic-asm-printer",
                "DSPIC Assembly Printer", false, false)

// Force static initialization.
extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeDSPICAsmPrinter() {
  RegisterAsmPrinter<DSPICAsmPrinter> X(getTheDSPICTarget());
}

PreservedAnalyses DSPICAsmPrinterBeginPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  DSPICAsmPrinter &AsmPrinter = static_cast<DSPICAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doInitialization(M);
  return PreservedAnalyses::all();
}

PreservedAnalyses
DSPICAsmPrinterPass::run(MachineFunction &MF,
                          MachineFunctionAnalysisManager &MFAM) {
  DSPICAsmPrinter &AsmPrinter = static_cast<DSPICAsmPrinter &>(
      MFAM.getResult<ModuleAnalysisManagerMachineFunctionProxy>(MF)
          .getCachedResult<AsmPrinterAnalysis>(*MF.getFunction().getParent())
          ->getPrinter());
  setupMachineFunctionAsmPrinter(MFAM, MF, AsmPrinter);
  AsmPrinter.runOnMachineFunction(MF);
  return PreservedAnalyses::all();
}

PreservedAnalyses DSPICAsmPrinterEndPass::run(Module &M,
                                               ModuleAnalysisManager &MAM) {
  DSPICAsmPrinter &AsmPrinter = static_cast<DSPICAsmPrinter &>(
      MAM.getResult<AsmPrinterAnalysis>(M).getPrinter());
  setupModuleAsmPrinter(M, MAM, AsmPrinter);
  AsmPrinter.doFinalization(M);
  return PreservedAnalyses::all();
}
