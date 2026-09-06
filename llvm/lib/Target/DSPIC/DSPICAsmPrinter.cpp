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
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

namespace {
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
