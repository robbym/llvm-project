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
#include "DSPICMCInstLower.h"
#include "DSPICTargetMachine.h"
#include "TargetInfo/DSPICTargetInfo.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/AsmPrinterAnalysis.h"
#include "llvm/CodeGen/MachineConstantPool.h"
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

    /// L1b: print the frame's accounting as a comment, so a report's frame size is a
    /// line the compiler printed (trellis standing rule 12).
    void emitFunctionBodyStart() override;
    // the config-words row: the `#pragma config` markers, collected here and emitted as words
    std::vector<std::string> ConfigPragmas;
    void emitGlobalVariable(const GlobalVariable *GV) override;
    void emitEndOfAsmFile(Module &M) override;

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
void DSPICAsmPrinter::emitGlobalVariable(const GlobalVariable *GV) {
  if (GV->hasSection() && GV->getSection() == ".dspic.config") {
    if (GV->hasInitializer())
      if (auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer()))
        if (CDA->isCString())
          ConfigPragmas.push_back(CDA->getAsCString().str());
    return;
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
void DSPICAsmPrinter::emitInstruction(const MachineInstr *MI) {
  DSPIC_MC::verifyInstructionPredicates(MI->getOpcode(),
                                         getSubtargetInfo().getFeatureBits());

  DSPICMCInstLower MCInstLowering(OutContext, *this);

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
