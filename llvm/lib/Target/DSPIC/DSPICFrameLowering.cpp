//===-- DSPICFrameLowering.cpp - DSPIC Frame Information ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The dsPIC33 frame: see the header. Every printed form here was accepted by the
// GPL pic30 `as` before it was written (trellis `tools/dspic-llvm/prints/l1b/`).
//
//===----------------------------------------------------------------------===//

#include "DSPICFrameLowering.h"
#include "DSPICInstrInfo.h"
#include "DSPICMachineFunctionInfo.h"
#include "DSPICSubtarget.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

// The assembler's ranges (prints/l1b/probe3.log): `lnk #N` takes an even N up to 16382;
// a word displacement is even and in [-1024, 1022]; `add #lit10,Wn` takes up to 1023.
static const uint64_t MaxLnk = 16382;
static const uint64_t MaxLit10 = 1023;

// The stack grows up; the local area begins 2 bytes past the entry SP (the saved w14).
DSPICFrameLowering::DSPICFrameLowering(const DSPICSubtarget &STI)
    : TargetFrameLowering(TargetFrameLowering::StackGrowsUp, Align(2), 2,
                          Align(2)),
      STI(STI), TII(*STI.getInstrInfo()), TRI(STI.getRegisterInfo()) {}

// Session 94: frame-pointer elimination is OFF by default. Measured a net +16 bytes on bl_fw
// (LLVM's allocator spills where cc1 does not even with w14 free, and w14's save is then not
// repaid); the implementation is correct and kept behind the flag for revival.
static cl::opt<bool> EnableFPElim(
    "dspic-frame-pointer-elim", cl::Hidden, cl::init(false),
    cl::desc("dsPIC: free w14 as a 15th register for functions whose stack pointer is fixed"));

bool DSPICFrameLowering::isFramePointerElimEnabled() const { return EnableFPElim; }

// Session 94: the body moves w15 iff it PUSHES a stack argument. A PUSH with no FrameSetup
// flag is such a push; the callee-saved pushes PEI adds carry FrameSetup, so this answer is
// the same before and after PEI (getMaxCallFrameSize reads 0 until PEI, which would flip
// hasFP mid-pipeline and hand w14 out, then reserve it -- a miscompile).
static bool functionMovesSP(const MachineFunction &MF) {
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB) {
      if (MI.getFlag(MachineInstr::FrameSetup))
        continue;
      switch (MI.getOpcode()) {
      case DSPIC::PUSH16r: case DSPIC::PUSH8r:
      case DSPIC::PUSH16i: case DSPIC::PUSH16c:
        return true;
      default: break;
      }
    }
  return false;
}

// w14 is the frame pointer (`lnk`/`ulnk`, and reserved) ONLY when a stable base is needed:
// the body moves w15 (an argument push -- then a spill's distance from w15 varies), or a
// var-sized object / a frame-or-return-address builtin / -fno-omit-frame-pointer demands it.
// Otherwise w15 is fixed after the prologue, locals and spills are w15-relative at a fixed
// offset, and w14 is a free allocatable register (session 93's NextChunk shape; the register
// class already lists R4 last, so the allocator reaches it only under pressure).
bool DSPICFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (MF.getTarget().Options.DisableFramePointerElim(MF) ||
      MFI.hasVarSizedObjects() || MFI.isFrameAddressTaken() ||
      MFI.isReturnAddressTaken())
    return true;
  if (!EnableFPElim) {
    // The pre-session-94 rule: any non-callee-saved stack object links a `lnk` frame (and w14
    // is reserved by getReservedRegs whatever this returns), so the default is unchanged.
    for (int I = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd(); I != E; ++I)
      if (!MFI.isDeadObjectIndex(I) && !MFI.isCalleeSavedObjectIndex(I))
        return true;
    return false;
  }
  return functionMovesSP(MF);
}

// L1f-a: never reserved. Outgoing arguments are PUSHED (the pushes move w15) and popped
// by `sub.w #N,w15` after the call, so a caller with no locals links no frame -- cc1's
// shape -- and the variable-sized-object case is the same path as every other.
bool DSPICFrameLowering::hasReservedCallFrame(const MachineFunction &MF) const {
  return false;
}

int64_t DSPICFrameLowering::frameOffsetFromFP(const MachineFunction &MF,
                                             int FI) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
  return MFI.getObjectOffset(FI) -
         (int64_t)(getOffsetOfLocalArea() +
                   FuncInfo->getCalleeSavedFrameSize());
}

// Session 94 (no frame pointer): an object's true address is entrySP + ObjectOffset (the
// identity the FP form above rests on), and after the prologue w15 = entrySP + StackSize, so
// the object sits at [w15 + (ObjectOffset - StackSize)] -- a fixed negative displacement,
// because w15 does not move in the body (functionMovesSP is false here).
int64_t DSPICFrameLowering::frameOffsetFromSP(const MachineFunction &MF,
                                             int FI) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  // The objects start at getOffsetOfLocalArea() (the gap the FP form spends on the saved w14);
  // w15 is bumped past Locals + that gap, so the object at ObjectOffset sits this far below it.
  return MFI.getObjectOffset(FI) -
         (int64_t)(MFI.getStackSize() + getOffsetOfLocalArea());
}

StackOffset
DSPICFrameLowering::getFrameIndexReference(const MachineFunction &MF, int FI,
                                           Register &FrameReg) const {
  if (hasFP(MF)) {
    FrameReg = DSPIC::R4;
    return StackOffset::getFixed(frameOffsetFromFP(MF, FI));
  }
  FrameReg = DSPIC::SP;
  return StackOffset::getFixed(frameOffsetFromSP(MF, FI));
}

// L1d RCOUNT (trellis session 88): an ISR clobbers the repeat counter if it makes a CALL
// (the opaque callee may run a `repeat`) or its own body has a repeat instruction (the inline
// memcpy/memset block ops, the divide). A non-ISR never saves RCOUNT.
static bool isrSavesRCount(const MachineFunction &MF) {
  if (!MF.getFunction().hasFnAttribute("interrupt"))
    return false;
  if (MF.getFrameInfo().hasCalls())
    return true;
  for (const MachineBasicBlock &MBB : MF)
    for (const MachineInstr &MI : MBB)
      switch (MI.getOpcode()) {
      // trellis session 99: the divide's repeat is now fused into the divide itself, so the
      // opcode this scan must recognise is the FUSED one. Leaving only REPEATdiv here would have
      // made an ISR that divides stop saving RCOUNT -- silently, and only in an ISR.
      case DSPIC::REPEATdiv:
      case DSPIC::DIVUWrep: case DSPIC::DIVSWrep:
      case DSPIC::MEMCPYrepW: case DSPIC::MEMCPYrepB:
      case DSPIC::MEMSETrepW: case DSPIC::MEMSETrepB:
        return true;
      default: break;
      }
  return false;
}

static bool isCalleeSavedPush(const MachineInstr &MI) {
  return MI.getFlag(MachineInstr::FrameSetup) &&
         (MI.getOpcode() == DSPIC::PUSH16r || MI.getOpcode() == DSPIC::PUSHD ||
          MI.getOpcode() == DSPIC::PUSHRCOUNT);
}

static bool isCalleeSavedPop(const MachineInstr &MI) {
  return MI.getFlag(MachineInstr::FrameDestroy) &&
         (MI.getOpcode() == DSPIC::POP16r || MI.getOpcode() == DSPIC::POPD ||
          MI.getOpcode() == DSPIC::POPRCOUNT);
}

// Prologue: the callee-saved pushes are already at the block's head (PEI inserted them
// through spillCalleeSavedRegisters); `lnk #locals` follows them.
// trellis session 98: does this handler manage the PSV page registers?
// cc1's rule, measured: every interrupt handler that does not carry `no_auto_psv`. A non-ISR never
// does. ⚠ `auto_psv` needs no test -- it asks for the default, so its absence and its presence are
// the same answer; only the suppression is carried as a string.
static bool isrManagesPSV(const MachineFunction &MF) {
  const Function &F = MF.getFunction();
  return F.hasFnAttribute("interrupt") && !F.hasFnAttribute("dspic-no-auto-psv");
}

void DSPICFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  assert(&MF.front() == &MBB && "Shrink-wrapping not yet supported");
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();

  MachineBasicBlock::iterator MBBI = MBB.begin();

  // trellis session 96 (follow-up 13): `interrupt(preprologue("..."))`. cc1 emits the text as the
  // very first thing in the function, BEFORE the ISR prologue -- the firmware's stack-error trap
  // captures w15 before the compiler's own push moves it, then branches past the prologue. By the
  // time this runs the callee-saved pushes are already at MBB.begin(), so inserting here puts the
  // text ahead of them. An INLINEASM MachineInstr rather than AsmPrinter raw text: raw text is an
  // error on an object streamer, and AsmPrinter::emitInlineAsm(StringRef,...) is private.
  if (MF.getFunction().hasFnAttribute("dspic-preprologue")) {
    StringRef Pre =
        MF.getFunction().getFnAttribute("dspic-preprologue").getValueAsString();
    // Indent the continuation lines. The inline-asm printer tabs the FIRST line only, so a
    // two-line preprologue would otherwise put `bra ...` in column 0 -- which the assembler
    // accepts, but cc1 indents and a column-0 mnemonic reads like a label.
    std::string Text;
    for (char Ch : Pre) {
      Text.push_back(Ch);
      if (Ch == '\n')
        Text.push_back('\t');
    }
    BuildMI(MBB, MBBI, DebugLoc(),
            MF.getSubtarget().getInstrInfo()->get(TargetOpcode::INLINEASM))
        .addExternalSymbol(MF.createExternalSymbolName(Text))
        .addImm(InlineAsm::Extra_HasSideEffects);
  }
  while (MBBI != MBB.end() && isCalleeSavedPush(*MBBI))
    ++MBBI;
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();

  uint64_t Locals = MFI.getStackSize() - FuncInfo->getCalleeSavedFrameSize();
  if (Locals % 2 != 0 || Locals > MaxLnk)
    report_fatal_error("dspic: frame of " + Twine(Locals) +
                       " bytes is not an even number of bytes up to 16382");

  if (!hasFP(MF)) {
    // Session 94: no w14. If there are locals/spills, grow w15 past them once (`add #N,w15`);
    // w15 then holds still for the body (no argument pushes here). No local area -> nothing.
    if (Locals == 0)
      return;
    uint64_t Bump = Locals + getOffsetOfLocalArea();
    if (Bump > MaxLit10)
      report_fatal_error("dspic: a frameless local area of " + Twine(Bump) +
                         " bytes exceeds the 10-bit `add #N,w15`");
    BuildMI(MBB, MBBI, DL, TII.get(DSPIC::ADD16ri10), DSPIC::SP)
        .addReg(DSPIC::SP)
        .addImm(Bump)
        ->getOperand(3).setIsDead(); // SR
    return;
  }

  BuildMI(MBB, MBBI, DL, TII.get(DSPIC::LNK))
      .addImm(Locals)
      .setMIFlag(MachineInstr::FrameSetup);

  for (MachineBasicBlock &MBBJ : llvm::drop_begin(MF))
    MBBJ.addLiveIn(DSPIC::R4);
}

// Epilogue: `ulnk` restores w15 to the top of the pushes and pops w14; then the pops
// PEI inserted through restoreCalleeSavedRegisters; then `return`/`retfie`, or a tail
// call's `bra`/`goto` (L1c), which leaves the frame exactly as `return` would.
void DSPICFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = MBB.getLastNonDebugInstr();
  DebugLoc DL = MBBI->getDebugLoc();
  switch (MBBI->getOpcode()) {
  case DSPIC::RET:
  case DSPIC::RETI:
  case DSPIC::TCRETURNdi: // L1c: `bra _sym` after the epilogue
  case DSPIC::TCRETURNdiL: // session 96: `goto _sym`, the same under -mlarge-code
  case DSPIC::TCRETURNri: // L1c: `goto wN`, wN caller-saved
    break;
  default:
    llvm_unreachable("Can only insert epilog into returning blocks");
  }

  // Back up over the callee-saved pops.
  MachineBasicBlock::iterator I = MBBI;
  while (I != MBB.begin() && isCalleeSavedPop(*std::prev(I)))
    --I;

  if (hasFP(MF)) {
    BuildMI(MBB, I, DL, TII.get(DSPIC::ULNK))
        .setMIFlag(MachineInstr::FrameDestroy);
  } else {
    // Session 94: undo the frameless local-area bump before the callee-saved pops (`pop.d`
    // reads [--w15], so w15 must be back at the top of the pushes).
    const auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
    uint64_t Locals =
        MF.getFrameInfo().getStackSize() - FuncInfo->getCalleeSavedFrameSize();
    if (Locals != 0)
      BuildMI(MBB, I, DL, TII.get(DSPIC::SUB16ri10), DSPIC::SP)
          .addReg(DSPIC::SP)
          .addImm(Locals + getOffsetOfLocalArea())
          ->getOperand(3).setIsDead(); // SR
  }
}

// The callee-saved pairs `push.d`/`pop.d` can take: (w8,w9) (w10,w11) (w12,w13), by the
// internal names the register file keeps (DSPICRegisterInfo.td: R10 = w8 ... R5 = w13).
namespace {
struct CSPair {
  MCPhysReg Even, Odd;
};
} // namespace
static const CSPair CSPairs[] = {
    {DSPIC::R12, DSPIC::R13}, {DSPIC::R14, DSPIC::R15}, {DSPIC::R11, DSPIC::W5}, {DSPIC::W6, DSPIC::W7}, {DSPIC::R10, DSPIC::R9}, {DSPIC::R8, DSPIC::R7}, {DSPIC::R6, DSPIC::R5}};
// Singles, in ascending w order, for what is left over.
static const MCPhysReg CSSingles[] = {
    DSPIC::R12, DSPIC::R13, DSPIC::R14, DSPIC::R15, DSPIC::R11, DSPIC::W5, DSPIC::W6,
    DSPIC::W7,  DSPIC::R10, DSPIC::R9,  DSPIC::R8,   DSPIC::R7,  DSPIC::R6, DSPIC::R5,
    DSPIC::R4};  // session 94: w14, when it is not the frame pointer, is a callee-saved single

// Plan the pushes in ascending w order: a pair where both registers are saved, a
// single otherwise. Pops are the reverse of this list.
static void
planCalleeSaves(ArrayRef<CalleeSavedInfo> CSI,
                SmallVectorImpl<std::pair<MCPhysReg, MCPhysReg>> &Plan) {
  SmallSet<MCPhysReg, 8> Saved;
  for (const CalleeSavedInfo &I : CSI)
    Saved.insert(I.getReg());
  SmallSet<MCPhysReg, 8> Done;
  for (const CSPair &P : CSPairs)
    if (Saved.count(P.Even) && Saved.count(P.Odd)) {
      Plan.push_back({P.Even, P.Odd});
      Done.insert(P.Even);
      Done.insert(P.Odd);
    }
  for (MCPhysReg R : CSSingles)
    if (Saved.count(R) && !Done.count(R))
      Plan.push_back({R, 0});
  // Ascending w order across pairs and singles. The internal numbering runs the other
  // way (R10 = w8 ... R5 = w13), so sort by the internal number descending.
  llvm::stable_sort(Plan, [](const std::pair<MCPhysReg, MCPhysReg> &A,
                             const std::pair<MCPhysReg, MCPhysReg> &B) {
    return A.first > B.first;
  });
}

// trellis session 98: guarantee a scratch for the PSV setup.
// ⛔ PEI CALLS spillCalleeSavedRegisters ONLY WHEN THERE IS SOMETHING TO SAVE. A handler that
// clobbers no callee-saved register has an empty CSI, the hook is never reached, and the page
// management silently does not happen -- measured on a handler with an empty body, which emitted a
// bare `retfie`. cc1 has the same problem and solves it the same way: its `isr_plain` PUSHES w8
// purely to have a scratch. Reserving one here puts us on the path PEI already drives.
void DSPICFrameLowering::determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                                              RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  if (isrManagesPSV(MF) && SavedRegs.none())
    SavedRegs.set(DSPIC::R12);
}

bool DSPICFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction &MF = *MBB.getParent();
  bool SaveRC = isrSavesRCount(MF);
  // trellis session 98: a handler that manages PSV must reach this code even when it saves no
  // GPR and no RCOUNT -- cc1's `isr_plain` is exactly that shape.
  bool ManagePSV = isrManagesPSV(MF);
  if (CSI.empty() && !SaveRC && !ManagePSV)
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  auto *FuncInfo = MF.getInfo<DSPICMachineFunctionInfo>();
  FuncInfo->setCalleeSavedFrameSize(CSI.size() * 2);
  // RCOUNT first (deepest), before the GPRs -- cc1's order. It self-balances with the pop and
  // is not counted in the frame size (pushed before `lnk`, transparent to w14-relative slots).
  if (SaveRC)
    BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHRCOUNT)).setMIFlag(MachineInstr::FrameSetup);

  SmallVector<std::pair<MCPhysReg, MCPhysReg>, 4> Plan;
  planCalleeSaves(CSI, Plan);
  for (const auto &[Even, Odd] : Plan) {
    MBB.addLiveIn(Even);
    if (Odd) {
      MBB.addLiveIn(Odd);
      BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHD))
          .addReg(Even, RegState::Kill)
          .addReg(Odd, RegState::Implicit | RegState::Kill)
          .setMIFlag(MachineInstr::FrameSetup);
    } else {
      BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSH16r))
          .addReg(Even, RegState::Kill)
          .setMIFlag(MachineInstr::FrameSetup);
    }
  }
  if (ManagePSV) {
    BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHDSRPAG)).setMIFlag(MachineInstr::FrameSetup);
    BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSHDSWPAG)).setMIFlag(MachineInstr::FrameSetup);
    // ⚠ THE SCRATCH REGISTER, and clobbering one the interrupted code owns is SILENT corruption.
    // Where the handler already saves a GPR, the first one is on the stack by now and is free.
    // Where it saves none we push one and pop it back before the body -- exactly what cc1 does in
    // `isr_plain` (`push w8 ... mov.w [--w15],w8`). There is no third option: "w0 is probably
    // free" is not a reason, and mutant MP4 is built on that sentence.
    Register Scratch = CSI.empty() ? Register(DSPIC::R12) : Register(CSI[0].getReg());
    bool Borrowed = CSI.empty();
    if (Borrowed) {
      MBB.addLiveIn(Scratch);
      BuildMI(MBB, MI, DL, TII.get(DSPIC::PUSH16r))
          .addReg(Scratch, RegState::Kill)
          .setMIFlag(MachineInstr::FrameSetup);
    }
    BuildMI(MBB, MI, DL, TII.get(DSPIC::SETPSVPAGE))
        .addReg(Scratch, RegState::Define | RegState::Dead)
        .setMIFlag(MachineInstr::FrameSetup);
    if (Borrowed)
      BuildMI(MBB, MI, DL, TII.get(DSPIC::POP16r), Scratch)
          .setMIFlag(MachineInstr::FrameSetup);
  }
  return true;
}

bool DSPICFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  MachineFunction &MF = *MBB.getParent();
  bool SaveRC = isrSavesRCount(MF);
  if (CSI.empty() && !SaveRC && !isrManagesPSV(MF))
    return false;

  DebugLoc DL;
  if (MI != MBB.end())
    DL = MI->getDebugLoc();

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();

  // trellis session 98: the page registers come back FIRST -- before the GPR pops -- which is
  // cc1's order and the reverse of the push side.
  if (isrManagesPSV(MF)) {
    BuildMI(MBB, MI, DL, TII.get(DSPIC::POPDSWPAG)).setMIFlag(MachineInstr::FrameDestroy);
    BuildMI(MBB, MI, DL, TII.get(DSPIC::POPDSRPAG)).setMIFlag(MachineInstr::FrameDestroy);
  }

  SmallVector<std::pair<MCPhysReg, MCPhysReg>, 4> Plan;
  planCalleeSaves(CSI, Plan);
  for (const auto &[Even, Odd] : llvm::reverse(Plan)) {
    if (Odd)
      BuildMI(MBB, MI, DL, TII.get(DSPIC::POPD), Even)
          .addReg(Odd, RegState::ImplicitDefine)
          .setMIFlag(MachineInstr::FrameDestroy);
    else
      BuildMI(MBB, MI, DL, TII.get(DSPIC::POP16r), Even)
          .setMIFlag(MachineInstr::FrameDestroy);
  }
  // RCOUNT last -- restored after every GPR, cc1's order.
  if (SaveRC)
    BuildMI(MBB, MI, DL, TII.get(DSPIC::POPRCOUNT)).setMIFlag(MachineInstr::FrameDestroy);
  return true;
}

// The setup pseudo emits nothing (the pushes move w15); the destroy pseudo pops the
// pushed bytes with `sub.w #N,w15` (a 10-bit literal; past 1023 a fatal error, as
// cc1's `sub` would need a register).
MachineBasicBlock::iterator DSPICFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  const DSPICInstrInfo &TII =
      *static_cast<const DSPICInstrInfo *>(MF.getSubtarget().getInstrInfo());
  {
    MachineInstr &Old = *I;
    uint64_t Amount = alignTo(TII.getFrameSize(Old), getStackAlign());
    bool IsSetup = Old.getOpcode() == TII.getCallFrameSetupOpcode();
    if (!IsSetup)
      Amount -= TII.getFramePoppedByCallee(Old);
    if (Amount != 0 && !IsSetup) {
      if (Amount > MaxLit10)
        report_fatal_error("dspic: " + Twine(Amount) +
                               " bytes of pushed arguments exceed the 10-bit "
                               "literal of the pop after the call",
                           /*gen_crash_diag=*/false);
      MachineInstr *New =
          BuildMI(MF, Old.getDebugLoc(), TII.get(DSPIC::SUB16ri10), DSPIC::SP)
              .addReg(DSPIC::SP)
              .addImm(Amount);
      New->getOperand(3).setIsDead(); // SR
      MBB.insert(I, New);
    }
  }
  return MBB.erase(I);
}

// An emergency spill slot for frame-index scavenging, when a local may sit past the
// 10-bit word displacement (prints/l1b/probe3.log: [-1024, 1022]). It is placed first,
// at [w14 + 0], so it is always reachable. The price: 2 bytes in a frame over ~1 KB.
void DSPICFrameLowering::processFunctionBeforeFrameFinalized(
    MachineFunction &MF, RegScavenger *RS) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  if (RS && MFI.estimateStackSize(MF) >= 1000)
    RS->addScavengingFrameIndex(MFI.CreateStackObject(2, Align(2), false));
}
