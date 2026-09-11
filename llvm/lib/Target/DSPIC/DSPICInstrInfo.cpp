//===-- DSPICInstrInfo.cpp - DSPIC Instruction Information --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the DSPIC implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "DSPICInstrInfo.h"
#include "DSPIC.h"
#include "DSPICSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineOutliner.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define GET_INSTRINFO_CTOR_DTOR
#include "DSPICGenInstrInfo.inc"

// ---- REMAT-PLAN (trellis session 94): rematerializing a near-global load ------------------
// The two options. The feature is OFF by default, so the default build is byte-identical.
cl::opt<bool> llvm::DSPICEnableRematNearGlobal(
    "dspic-remat-near-global", cl::Hidden, cl::init(false),
    cl::desc("Offer a load of a near global as rematerializable, so the allocator re-reads it "
             "from memory instead of holding it in a register across a call (REMAT-PLAN)"));

// TEMPORARY (REMAT-PLAN Phase 1/2). The Phase-3 IR pass is what may legitimately clear an
// opaque call; this knob asserts the clearance for EVERY near-global load with nothing behind
// it, so that the spiller path can be built and its explicit-store scan validated in isolation.
// It is UNSOUND BY CONSTRUCTION for any program whose callees write the global, and it is
// retired when the pass lands.
static cl::opt<bool> RematForce(
    "dspic-remat-force", cl::Hidden, cl::init(false),
    cl::desc("REMAT-PLAN Phase 1/2 scaffolding: put the IR pass's verdict on every near-global "
             "load without proving it. Unsound for calls; for building the spiller path only"));

// The instrument. A refusal is invisible otherwise -- the output simply does not change -- and
// the first run of Phase 1 refused everything for a reason no print showed.
static cl::opt<bool> RematWhy(
    "dspic-remat-why", cl::Hidden, cl::init(false),
    cl::desc("Print, for each near-global load rematerialization the memory check refuses, the "
             "instruction in the value's live range that vetoed it"));

bool llvm::DSPICIsRematerializableNearGlobalLoad(const Instruction &I) {
  const auto *LI = dyn_cast<LoadInst>(&I);
  if (!LI || !LI->isSimple()) // not volatile, not atomic, unordered
    return false;
  Type *Ty = LI->getType();
  if (!Ty->isIntegerTy(8) && !Ty->isIntegerTy(16))
    return false;
  // The whole object by its own name: a GEP or any computed address is a different class.
  const auto *GVar = dyn_cast<GlobalVariable>(LI->getPointerOperand());
  return GVar && GVar->getAddressSpace() == 0 && !GVar->isConstant() &&
         !GVar->hasSection() && !GVar->hasAttribute("far");
}

MachineMemOperand::Flags llvm::DSPICGetRematMMOFlags(const Instruction &I) {
  if (DSPICEnableRematNearGlobal && RematForce &&
      DSPICIsRematerializableNearGlobalLoad(I))
    return MachineMemOperand::MOTargetFlag1;
  return MachineMemOperand::MONone;
}

// The MIR half of the same class. SelectAddr plants $sr in the base-register slot as the "no
// base register" sentinel for a bare file address, and the displacement is then the symbol.
static bool isNearGlobalLoadOpcode(unsigned Op) {
  switch (Op) {
  case DSPIC::MOV16rm:
  case DSPIC::MOV8rm:
  case DSPIC::MOVZX16rm8:
  // Session 95: the byte read of a near global, as one instruction that uses its own
  // destination as the address scratch. ⚠ MOV8rm and MOVZX16rm8 stay in the list and stay
  // DEAD -- isel never builds the $sr-sentinel base for them (measured: 0 of 55 byte loads on
  // bl_fw) -- because they are the forms the gate would have to accept if it ever did.
  case DSPIC::ZE16f:
  case DSPIC::MOV8f:
    return true;
  default:
    return false;
  }
}

bool DSPICInstrInfo::isRematerializableNearGlobalLoad(
    const MachineInstr &MI, const GlobalValue **GVOut) const {
  if (!isNearGlobalLoadOpcode(MI.getOpcode()) || MI.getNumOperands() < 3)
    return false;
  if (!MI.getOperand(0).isReg() || !MI.getOperand(0).isDef() ||
      MI.getOperand(0).getSubReg())
    return false;
  if (MI.mayStore() || MI.hasUnmodeledSideEffects() || MI.isNotDuplicable())
    return false;
  const MachineOperand &Base = MI.getOperand(1);
  const MachineOperand &Disp = MI.getOperand(2);
  if (!Base.isReg() || Base.getReg() != DSPIC::SR || !Disp.isGlobal())
    return false;
  const GlobalValue *GV = Disp.getGlobal();
  const auto *GVar = dyn_cast<GlobalVariable>(GV);
  if (!GVar || GVar->getAddressSpace() != 0 || GVar->isConstant() ||
      GVar->hasSection() || GVar->hasAttribute("far"))
    return false;
  // Exactly one ordinary load memory operand.
  if (MI.memoperands_empty() ||
      std::next(MI.memoperands_begin()) != MI.memoperands_end())
    return false;
  const MachineMemOperand *MMO = *MI.memoperands_begin();
  if (!MMO->isLoad() || MMO->isStore() || MMO->isVolatile() || !MMO->isUnordered())
    return false;
  if (GVOut)
    *GVOut = GV;
  return true;
}

bool DSPICInstrInfo::hasRematVerdict(const MachineInstr &MI) const {
  return !MI.memoperands_empty() &&
         ((*MI.memoperands_begin())->getFlags() & MachineMemOperand::MOTargetFlag1);
}

static cl::opt<bool> EnableRematALU(
    "dspic-remat-alu", cl::Hidden, cl::init(false),
    cl::desc("Offer a three-operand ALU form whose SR def is dead as rematerializable, so a "
             "value that is a one-instruction function of registers already live across a call "
             "is recomputed there instead of occupying one (session 95)"));

bool DSPICInstrInfo::isRematerializableALU(const MachineInstr &MI) const {
  if (!MI.getDesc().isRematerializable() || MI.mayLoadOrStore() ||
      MI.hasUnmodeledSideEffects() || MI.isNotDuplicable() || MI.isInlineAsm())
    return false;
  if (!MI.getNumOperands() || !MI.getOperand(0).isReg() || !MI.getOperand(0).isDef() ||
      !MI.getOperand(0).getReg().isVirtual() || MI.getOperand(0).getSubReg())
    return false;
  Register Def = MI.getOperand(0).getReg();
  for (const MachineOperand &MO : MI.operands()) {
    if (!MO.isReg() || !MO.getReg())
      continue;
    if (MO.getReg().isPhysical()) {
      // The one physical shape allowed: a DEAD def of the status register. A physical USE of
      // SR means the result depends on the carry where the instruction stands (ADDC/SUBC/
      // DADD), and any other physical def would be clobbered at the remat point.
      if (MO.getReg() != DSPIC::SR || !MO.isDef() || !MO.isDead())
        return false;
      continue;
    }
    if (MO.isDef() && MO.getReg() != Def)
      return false;
  }
  return true;
}

// REMAT-PLAN 4.3 -- the gate. The default refuses every non-invariant load; this admits exactly
// the flagged near-global loads and defers on everything else.
bool DSPICInstrInfo::isReMaterializableImpl(const MachineInstr &MI) const {
  if (DSPICEnableRematNearGlobal && isRematerializableNearGlobalLoad(MI) &&
      hasRematVerdict(MI))
    return true;
  if (EnableRematALU && isRematerializableALU(MI))
    return true;
  return TargetInstrInfo::isReMaterializableImpl(MI);
}

// $sr in the base slot of a bare file address is a SENTINEL, not a read: the encoded
// instruction names only the symbol. Without this, allUsesAvailableAt refuses every
// rematerialization of a near-global load on the strength of a physreg use that is not there.
bool DSPICInstrInfo::isIgnorableUse(const MachineInstr &MI, unsigned OpIdx) const {
  if (!DSPICEnableRematNearGlobal || OpIdx != 1)
    return false;
  const MachineOperand &MO = MI.getOperand(OpIdx);
  if (!MO.isReg() || MO.getReg() != DSPIC::SR)
    return false;
  return isRematerializableNearGlobalLoad(MI);
}

bool DSPICInstrInfo::isMemoryRematCandidate(const MachineInstr &MI) const {
  return DSPICEnableRematNearGlobal && isRematerializableNearGlobalLoad(MI);
}

// REMAT-PLAN 4.4 -- the judgement CodeGen asks for at every instruction the value is live
// across. TRUE means "may write the global the candidate reads", and the caller refuses on it.
// Print a refusal under -dspic-remat-why and return true (the caller refuses on true), so every
// veto in this function reads as `return report(...)`.
static bool reportImpl(const MachineInstr &Orig, const MachineInstr &MI,
                       const GlobalValue *GV, const char *Why, StringRef OpName) {
  if (RematWhy)
    errs() << "dspic-remat: " << Orig.getMF()->getName() << ": refuse re-reading @"
           << (GV ? GV->getName() : "?") << " -- " << Why << " (" << OpName << ")\n";
  return true;
}

bool DSPICInstrInfo::isMemoryRematClobber(const MachineInstr &Orig,
                                          const MachineInstr &MI) const {
  auto report = [&](const MachineInstr &O, const MachineInstr &M,
                    const GlobalValue *G, const char *Why) {
    return reportImpl(O, M, G, Why, getName(M.getOpcode()));
  };
  const GlobalValue *GV = nullptr;
  if (!isRematerializableNearGlobalLoad(Orig, &GV))
    return true; // not the class this reasoning covers

  // Instructions that are not code, and the call-frame pseudos, which only move w15.
  if (MI.isDebugInstr() || MI.isPosition() || MI.isImplicitDef() || MI.isKill() ||
      MI.isCFIInstruction() || MI.isLabel() || isFrameInstr(MI))
    return false;

  // An opaque call may write ANY global; nothing at MIR can see through one. The IR pass is the
  // only thing that can, and it says so through the load's MMO flag. Without that flag, refuse.
  if (MI.isCall() || MI.isInlineAsm())
    return hasRematVerdict(Orig)
               ? false
               : report(Orig, MI, GV, "an opaque call, with no IR-level verdict on the load");

  if (MI.hasUnmodeledSideEffects())
    return report(Orig, MI, GV, "unmodeled side effects");
  if (!MI.mayStore())
    return false;

  // An explicit store. Name its destination, or refuse. (No AAResults is threaded in here --
  // REMAT-PLAN 9.2 -- so a pointer or indexed store is a clobber even when it is not one.)
  bool Named = false;
  for (const MachineMemOperand *MMO : MI.memoperands()) {
    if (!MMO->isStore())
      continue;
    Named = true;
    if (const PseudoSourceValue *PSV = MMO->getPseudoValue()) {
      switch (PSV->kind()) {
      case PseudoSourceValue::Stack:
      case PseudoSourceValue::FixedStack:
      case PseudoSourceValue::ConstantPool:
      case PseudoSourceValue::JumpTable:
        continue; // the frame and read-only pools are not this data global
      default:
        return report(Orig, MI, GV, "a store to a pseudo source this scan does not model");
      }
    }
    const Value *V = MMO->getValue();
    if (!V)
      return report(Orig, MI, GV, "a store with an unnamed destination");
    // `sink[1] = x` names `getelementptr (@sink, 2)`, a ConstantExpr, not `@sink`. The object
    // an offset is taken into is the object the NoAlias comparison is about.
    V = V->stripInBoundsConstantOffsets();
    if (isa<AllocaInst>(V))
      continue; // a stack object cannot be the global
    const auto *DstGV = dyn_cast<GlobalValue>(V);
    if (!DstGV)
      return report(Orig, MI, GV, "a store through a pointer this scan cannot name");
    if (DstGV == GV)
      return report(Orig, MI, GV, "a store to the same global");
  }
  // A store whose destination no memory operand names says nothing; refuse.
  return Named ? false : report(Orig, MI, GV, "a store with no memory operand at all");
}

// Pin the vtable to this file.
void DSPICInstrInfo::anchor() {}

DSPICInstrInfo::DSPICInstrInfo(const DSPICSubtarget &STI)
    : DSPICGenInstrInfo(STI, RI, DSPIC::ADJCALLSTACKDOWN,
                         DSPIC::ADJCALLSTACKUP),
      RI() {}

void DSPICInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool isKill, int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end()) DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOStore, MFI.getObjectSize(FrameIdx),
      MFI.getObjectAlign(FrameIdx));

  if (DSPIC::GR16RegClass.hasSubClassEq(RC)) // GR16 or GR16_TC (L1c)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV16mr))
      .addFrameIndex(FrameIdx).addImm(0)
      .addReg(SrcReg, getKillRegState(isKill)).addMemOperand(MMO);
  else if (DSPIC::GR8RegClass.hasSubClassEq(RC)) // GR8 or GR8_W0 (L1f-g)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV8mr))
      .addFrameIndex(FrameIdx).addImm(0)
      .addReg(SrcReg, getKillRegState(isKill)).addMemOperand(MMO);
  else
    llvm_unreachable("Cannot store this register to stack slot!");
}

void DSPICInstrInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MI,
                                           Register DestReg, int FrameIdx,
                                           const TargetRegisterClass *RC,
                                           Register VReg, unsigned SubReg,
                                           MachineInstr::MIFlag Flags) const {
  DebugLoc DL;
  if (MI != MBB.end()) DL = MI->getDebugLoc();
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOLoad, MFI.getObjectSize(FrameIdx),
      MFI.getObjectAlign(FrameIdx));

  if (DSPIC::GR16RegClass.hasSubClassEq(RC)) // GR16 or GR16_TC (L1c)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV16rm))
      .addReg(DestReg, getDefRegState(true)).addFrameIndex(FrameIdx)
      .addImm(0).addMemOperand(MMO);
  else if (DSPIC::GR8RegClass.hasSubClassEq(RC)) // GR8 or GR8_W0 (L1f-g)
    BuildMI(MBB, MI, DL, get(DSPIC::MOV8rm))
      .addReg(DestReg, getDefRegState(true)).addFrameIndex(FrameIdx)
      .addImm(0).addMemOperand(MMO);
  else
    llvm_unreachable("Cannot store this register to stack slot!");
}

void DSPICInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator I,
                                  const DebugLoc &DL, Register DestReg,
                                  Register SrcReg, bool KillSrc,
                                  bool RenamableDest, bool RenamableSrc) const {
  unsigned Opc;
  if (DSPIC::GR16RegClass.contains(DestReg, SrcReg))
    Opc = DSPIC::MOV16rr;
  else if (DSPIC::GR8RegClass.contains(DestReg, SrcReg))
    Opc = DSPIC::MOV8rr;
  else
    llvm_unreachable("Impossible reg-to-reg copy");

  BuildMI(MBB, I, DL, get(Opc), DestReg)
    .addReg(SrcReg, getKillRegState(KillSrc));
}

unsigned DSPICInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                       int *BytesRemoved) const {
  assert(!BytesRemoved && "code size not handled");

  MachineBasicBlock::iterator I = MBB.end();
  unsigned Count = 0;

  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->getOpcode() != DSPIC::JMP &&
        I->getOpcode() != DSPIC::JCC &&
        I->getOpcode() != DSPIC::Bi &&
        I->getOpcode() != DSPIC::Br &&
        I->getOpcode() != DSPIC::Bm)
      break;
    // Remove the branch.
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
  }

  return Count;
}

bool DSPICInstrInfo::
reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Invalid Xbranch condition!");

  DSPICCC::CondCodes CC = static_cast<DSPICCC::CondCodes>(Cond[0].getImm());

  switch (CC) {
  default: llvm_unreachable("Invalid branch condition!");
  case DSPICCC::COND_E:
    CC = DSPICCC::COND_NE;
    break;
  case DSPICCC::COND_NE:
    CC = DSPICCC::COND_E;
    break;
  case DSPICCC::COND_L:
    CC = DSPICCC::COND_GE;
    break;
  case DSPICCC::COND_GE:
    CC = DSPICCC::COND_L;
    break;
  case DSPICCC::COND_HS:
    CC = DSPICCC::COND_LO;
    break;
  case DSPICCC::COND_LO:
    CC = DSPICCC::COND_HS;
    break;
  }

  Cond[0].setImm(CC);
  return false;
}

bool DSPICInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                    MachineBasicBlock *&TBB,
                                    MachineBasicBlock *&FBB,
                                    SmallVectorImpl<MachineOperand> &Cond,
                                    bool AllowModify) const {
  // Start from the bottom of the block and work up, examining the
  // terminator instructions.
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;

    // Working from the bottom, when we see a non-terminator
    // instruction, we're done.
    if (!isUnpredicatedTerminator(*I))
      break;

    // A terminator that isn't a branch can't easily be handled
    // by this analysis.
    if (!I->isBranch())
      return true;

    // Cannot handle indirect branches.
    if (I->getOpcode() == DSPIC::Br ||
        I->getOpcode() == DSPIC::Bm)
      return true;

    // Handle unconditional branches.
    if (I->getOpcode() == DSPIC::JMP || I->getOpcode() == DSPIC::Bi) {
      if (!AllowModify) {
        TBB = I->getOperand(0).getMBB();
        continue;
      }

      // If the block has any instructions after a JMP, delete them.
      MBB.erase(std::next(I), MBB.end());
      Cond.clear();
      FBB = nullptr;

      // Delete the JMP if it's equivalent to a fall-through.
      if (MBB.isLayoutSuccessor(I->getOperand(0).getMBB())) {
        TBB = nullptr;
        I->eraseFromParent();
        I = MBB.end();
        continue;
      }

      // TBB is used to indicate the unconditinal destination.
      TBB = I->getOperand(0).getMBB();
      continue;
    }

    // Handle conditional branches.
    assert(I->getOpcode() == DSPIC::JCC && "Invalid conditional branch");
    DSPICCC::CondCodes BranchCode =
      static_cast<DSPICCC::CondCodes>(I->getOperand(1).getImm());
    if (BranchCode == DSPICCC::COND_INVALID)
      return true;  // Can't handle weird stuff.

    // Working from the bottom, handle the first conditional branch.
    if (Cond.empty()) {
      FBB = TBB;
      TBB = I->getOperand(0).getMBB();
      Cond.push_back(MachineOperand::CreateImm(BranchCode));
      continue;
    }

    // Handle subsequent conditional branches. Only handle the case where all
    // conditional branches branch to the same destination.
    assert(Cond.size() == 1);
    assert(TBB);

    // Only handle the case where all conditional branches branch to
    // the same destination.
    if (TBB != I->getOperand(0).getMBB())
      return true;

    DSPICCC::CondCodes OldBranchCode = (DSPICCC::CondCodes)Cond[0].getImm();
    // If the conditions are the same, we can leave them alone.
    if (OldBranchCode == BranchCode)
      continue;

    return true;
  }

  return false;
}

unsigned DSPICInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *TBB,
                                       MachineBasicBlock *FBB,
                                       ArrayRef<MachineOperand> Cond,
                                       const DebugLoc &DL,
                                       int *BytesAdded) const {
  // Shouldn't be a fall through.
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 1 || Cond.size() == 0) &&
         "DSPIC branch conditions have one component!");
  assert(!BytesAdded && "code size not handled");

  if (Cond.empty()) {
    // Unconditional branch?
    assert(!FBB && "Unconditional branch with multiple successors!");
    BuildMI(&MBB, DL, get(DSPIC::JMP)).addMBB(TBB);
    return 1;
  }

  // Conditional branch.
  unsigned Count = 0;
  BuildMI(&MBB, DL, get(DSPIC::JCC)).addMBB(TBB).addImm(Cond[0].getImm());
  ++Count;

  if (FBB) {
    // Two-way Conditional branch. Insert the second branch.
    BuildMI(&MBB, DL, get(DSPIC::JMP)).addMBB(FBB);
    ++Count;
  }
  return Count;
}

/// GetInstSize - Return the number of bytes of code the specified
/// instruction may be.  This returns the maximum number of bytes.
///
unsigned DSPICInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  const MCInstrDesc &Desc = MI.getDesc();

  switch (Desc.getOpcode()) {
  case TargetOpcode::CFI_INSTRUCTION:
  case TargetOpcode::EH_LABEL:
  case TargetOpcode::IMPLICIT_DEF:
  case TargetOpcode::KILL:
  case TargetOpcode::DBG_VALUE:
    return 0;
  case TargetOpcode::INLINEASM:
  case TargetOpcode::INLINEASM_BR: {
    const MachineFunction *MF = MI.getParent()->getParent();
    const TargetInstrInfo &TII = *MF->getSubtarget().getInstrInfo();
    return TII.getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                                  MF->getTarget().getMCAsmInfo());
  }
  case TargetOpcode::BUNDLE:
    return getInstBundleSize(MI);
  case DSPIC::BR_JT: {
    // Session 96: the entries are emitted INLINE after the computed branch, so this instruction
    // occupies 2 bytes plus 2 per case. A wrong size here mis-sizes every branch that spans the
    // table, which DSPICBranchSelector then gets wrong silently.
    const MachineFunction *MF = MI.getParent()->getParent();
    const MachineJumpTableInfo *MJTI = MF->getJumpTableInfo();
    unsigned N = MJTI->getJumpTables()[MI.getOperand(1).getIndex()].MBBs.size();
    return 2 + 2 * N;
  }
  }

  return Desc.getSize();
}

namespace { enum DSPICOutlinerConstruction { MachineOutlinerDefault }; }

bool DSPICInstrInfo::isFunctionSafeToOutlineFrom(MachineFunction &MF,
                                                 bool OutlineFromLinkOnceODRs) const {
  const Function &F = MF.getFunction();
  if (!OutlineFromLinkOnceODRs && F.hasLinkOnceODRLinkage())
    return false;
  if (F.hasFnAttribute("interrupt"))
    return false;
  return true;
}

bool DSPICInstrInfo::isMBBSafeToOutlineFrom(MachineBasicBlock &MBB, unsigned &Flags) const {
  return true;
}

bool DSPICInstrInfo::shouldOutlineFromFunctionByDefault(MachineFunction &MF) const {
  return true;
}

std::optional<std::unique_ptr<outliner::OutlinedFunction>>
DSPICInstrInfo::getOutliningCandidateInfo(
    const MachineModuleInfo &MMI,
    std::vector<outliner::Candidate> &RepeatedSequenceLocs,
    unsigned MinRepeats) const {
  if (RepeatedSequenceLocs.size() < MinRepeats)
    return std::nullopt;
  unsigned SequenceSize = 0;
  for (auto &MI : RepeatedSequenceLocs[0])
    SequenceSize += getInstSizeInBytes(MI);
  // trellis session 108: the call's cost follows the CODE MODEL the call itself follows
  // (insertOutlinedCall below, session 97): `call` under -mlarge-code is TWO words, `rcall` one.
  // This was a constant one word, so under the large model every outlined call was priced at half
  // its size and a two-instruction sequence at four sites (`bclr.w _IEC1bits,#2 ; nop` in stn3255)
  // was outlined for a net +3 words -- seen the day __builtin_nop became a real instruction rather
  // than an inline-asm block the outliner may not touch (steps/vbi/CORRECTIONS.md section 5).
  bool LargeCode =
      RepeatedSequenceLocs[0].getMF()->getSubtarget<DSPICSubtarget>().isLargeCode();
  unsigned CallOverhead = LargeCode ? 4 : 2;
  unsigned FrameOverhead = 2;
  for (outliner::Candidate &C : RepeatedSequenceLocs)
    C.setCallInfo(MachineOutlinerDefault, CallOverhead);
  return std::make_unique<outliner::OutlinedFunction>(
      RepeatedSequenceLocs, SequenceSize, FrameOverhead, MachineOutlinerDefault);
}

outliner::InstrType
DSPICInstrInfo::getOutliningTypeImpl(const MachineModuleInfo &MMI,
                                     MachineBasicBlock::iterator &MBBI,
                                     unsigned Flags) const {
  MachineInstr &MI = *MBBI;
  if (MI.isCFIInstruction() || MI.isCall() || MI.isReturn() || MI.isBranch() ||
      MI.isIndirectBranch() || MI.isTerminator() || isFrameInstr(MI) || MI.isInlineAsm() ||
      MI.isPosition())
    return outliner::InstrType::Illegal;
  if (getInstSizeInBytes(MI) == 0 && !MI.isMetaInstruction())
    return outliner::InstrType::Illegal;
  const TargetRegisterInfo *TRI = MI.getMF()->getSubtarget().getRegisterInfo();
  if (MI.modifiesRegister(DSPIC::SP, TRI) || MI.readsRegister(DSPIC::SP, TRI))
    return outliner::InstrType::Illegal;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isJTI() || MO.isBlockAddress() || MO.isCPI())
      return outliner::InstrType::Illegal;
  return outliner::InstrType::Legal;
}

void DSPICInstrInfo::buildOutlinedFrame(MachineBasicBlock &MBB, MachineFunction &MF,
                                        const outliner::OutlinedFunction &OF) const {
  MBB.insert(MBB.end(), BuildMI(MF, DebugLoc(), get(DSPIC::RET)));
}

MachineBasicBlock::iterator
DSPICInstrInfo::insertOutlinedCall(Module &M, MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator &It, MachineFunction &MF,
                                   outliner::Candidate &C) const {
  // trellis session 97: the outliner obeys the CODE MODEL, as LowerCCCCallTo does. RCALLi is
  // the one-word PC-relative call (reach +/-32,767 words, measured -- steps/brreach); under
  // -mlarge-code an outlined function may sit beyond it, and the link then fails with "PC Relative
  // branch out of range. Suggest large-code model". This built RCALLi unconditionally, so in
  // stn3255 -- which builds with the vendor's own -mlarge-code -- every one of 730 outlined calls
  // disobeyed the model while all 1797 ordinary calls obeyed it.
  // ⚠ Small code must keep `rcall`: making every outlined call two words would tax every build
  // that does not need it. Hence the model, not a constant (see mutant MO1).
  bool Large = MF.getSubtarget<DSPICSubtarget>().isLargeCode();
  It = MBB.insert(It, BuildMI(MF, DebugLoc(),
                              get(Large ? DSPIC::CALLi : DSPIC::RCALLi))
                          .addGlobalAddress(M.getNamedValue(MF.getName()), 0, 0));
  return It;
}
