//===-- DSPICBranchSelector.cpp - Emit long conditional branches ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a pass that scans a machine function to determine which
// conditional branches need more than 10 bits of displacement to reach their
// target basic block.  It does this in two passes; a calculation of basic block
// positions pass, and a branch pseudo op to machine branch opcode pass.  This
// pass should be run last, just before the assembly printer.
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "DSPICInstrInfo.h"
#include "DSPICSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/IR/Analysis.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetMachine.h"
using namespace llvm;

#define DEBUG_TYPE "dspic-branch-select"

static cl::opt<bool>
    BranchSelectEnabled("dspic-dspic-branch-select", cl::Hidden, cl::init(true),
                        cl::desc("Expand out of range branches"));

// trellis session 97: the branch displacement, in BITS of signed WORD offset. 16 is the dsPIC's
// own, measured against the GPL linker for `bra`, `bra z` and `bra nz` alike -- 32,767 words links
// and 32,768 is refused "out of range" (steps/brreach/reach.sh). The inherited value was 10,
// MSP430's, which is 64x too tight and cost bl_fw 132 progbytes in branches expanded for nothing.
// ⚠ The knob exists because J9 in steps/jumptable is the only observable for the BR_JT size arm,
// and it works by pushing a branch past this threshold; at the correct value no realistic jump
// table can. J9 passes -dspic-branch-reach-bits=10 to keep its witness.
static cl::opt<unsigned>
    BranchReachBits("dspic-branch-reach-bits", cl::Hidden, cl::init(16),
                    cl::desc("Bits of signed word displacement a branch can reach"));

STATISTIC(NumSplit, "Number of machine basic blocks split");
STATISTIC(NumExpanded, "Number of branches expanded to long format");

namespace {
class DSPICBSelImpl {

  typedef SmallVector<int, 16> OffsetVector;

  MachineFunction *MF;
  const DSPICInstrInfo *TII;

  unsigned measureFunction(OffsetVector &BlockOffsets,
                           MachineBasicBlock *FromBB = nullptr);
  bool expandBranches(OffsetVector &BlockOffsets);

public:
  bool runOnMachineFunction(MachineFunction &MF);
};

class DSPICBranchSelectLegacyPass : public MachineFunctionPass {
public:
  static char ID;
  DSPICBranchSelectLegacyPass() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoVRegs();
  }

  StringRef getPassName() const override { return "DSPIC Branch Selector"; }
};

char DSPICBranchSelectLegacyPass::ID = 0;
} // namespace

static bool isInRage(int DistanceInBytes) {
  // trellis session 97: MEASURED, not inherited. The comment here used to cite the CC430 Family
  // User's Guide for a signed 10-bit word offset -- MSP430's field, carried over with the rest of
  // the backend. Against the GPL pic30 linker, `bra Expr`, `bra z,Expr` and `bra nz,Expr` all link
  // at 32,767 words and are all refused "out of range" at 32,768 (steps/brreach/reach.sh, which
  // crosses the boundary so that the table is a measurement and not a row of "links").
  // ⛔ And the ASSEMBLER is not the oracle for this: it range-checks no branch at all, emitting a
  // PC-relative relocation at any distance. Only `ld` refuses.
  const int WordSize = 2;

  assert((DistanceInBytes % WordSize == 0) &&
         "Branch offset should be word aligned!");

  int Words = DistanceInBytes / WordSize;
  return BranchReachBits >= 32 ||
         (Words >= -(1 << (BranchReachBits - 1)) &&
          Words < (1 << (BranchReachBits - 1)));
}

/// Measure each basic block, fill the BlockOffsets, and return the size of
/// the function, starting with BB
unsigned DSPICBSelImpl::measureFunction(OffsetVector &BlockOffsets,
                                         MachineBasicBlock *FromBB) {
  // Give the blocks of the function a dense, in-order, numbering.
  MF->RenumberBlocks(FromBB);

  MachineFunction::iterator Begin;
  if (FromBB == nullptr) {
    Begin = MF->begin();
  } else {
    Begin = FromBB->getIterator();
  }

  BlockOffsets.resize(MF->getNumBlockIDs());

  unsigned TotalSize = BlockOffsets[Begin->getNumber()];
  for (auto &MBB : make_range(Begin, MF->end())) {
    BlockOffsets[MBB.getNumber()] = TotalSize;
    for (MachineInstr &MI : MBB) {
      TotalSize += TII->getInstSizeInBytes(MI);
    }
  }
  return TotalSize;
}

/// Do expand branches and split the basic blocks if necessary.
/// Returns true if made any change.
bool DSPICBSelImpl::expandBranches(OffsetVector &BlockOffsets) {
  // For each conditional branch, if the offset to its destination is larger
  // than the offset field allows, transform it into a long branch sequence
  // like this:
  //   short branch:
  //     bCC MBB
  //   long branch:
  //     b!CC $PC+6
  //     b MBB
  //
  bool MadeChange = false;
  for (auto MBB = MF->begin(), E = MF->end(); MBB != E; ++MBB) {
    unsigned MBBStartOffset = 0;
    for (auto MI = MBB->begin(), EE = MBB->end(); MI != EE; ++MI) {
      MBBStartOffset += TII->getInstSizeInBytes(*MI);

      // If this instruction is not a short branch then skip it.
      if (MI->getOpcode() != DSPIC::JCC && MI->getOpcode() != DSPIC::JMP) {
        continue;
      }

      MachineBasicBlock *DestBB = MI->getOperand(0).getMBB();
      // Determine the distance from the current branch to the destination
      // block. MBBStartOffset already includes the size of the current branch
      // instruction.
      int BlockDistance =
          BlockOffsets[DestBB->getNumber()] - BlockOffsets[MBB->getNumber()];
      int BranchDistance = BlockDistance - MBBStartOffset;

      // If this branch is in range, ignore it.
      if (isInRage(BranchDistance)) {
        continue;
      }

      LLVM_DEBUG(dbgs() << "  Found a branch that needs expanding, "
                        << printMBBReference(*DestBB) << ", Distance "
                        << BranchDistance << "\n");

      // If JCC is not the last instruction we need to split the MBB.
      if (MI->getOpcode() == DSPIC::JCC && std::next(MI) != EE) {

        LLVM_DEBUG(dbgs() << "  Found a basic block that needs to be split, "
                          << printMBBReference(*MBB) << "\n");

        // Create a new basic block.
        MachineBasicBlock *NewBB =
            MF->CreateMachineBasicBlock(MBB->getBasicBlock());
        MF->insert(std::next(MBB), NewBB);

        // Splice the instructions following MI over to the NewBB.
        NewBB->splice(NewBB->end(), &*MBB, std::next(MI), MBB->end());

        // Update the successor lists.
        for (MachineBasicBlock *Succ : MBB->successors()) {
          if (Succ == DestBB) {
            continue;
          }
          MBB->replaceSuccessor(Succ, NewBB);
          NewBB->addSuccessor(Succ);
        }

        // We introduced a new MBB so all following blocks should be numbered
        // and measured again.
        measureFunction(BlockOffsets, &*MBB);

        ++NumSplit;

        // It may be not necessary to start all over at this point, but it's
        // safer do this anyway.
        return true;
      }

      MachineInstr &OldBranch = *MI;
      DebugLoc dl = OldBranch.getDebugLoc();
      int InstrSizeDiff = -TII->getInstSizeInBytes(OldBranch);

      if (MI->getOpcode() == DSPIC::JCC) {
        MachineBasicBlock *NextMBB = &*std::next(MBB);
        assert(MBB->isSuccessor(NextMBB) &&
               "This block must have a layout successor!");

        // The BCC operands are:
        // 0. Target MBB
        // 1. DSPIC branch predicate
        SmallVector<MachineOperand, 1> Cond;
        Cond.push_back(MI->getOperand(1));

        // Jump over the long branch on the opposite condition
        TII->reverseBranchCondition(Cond);
        MI = BuildMI(*MBB, MI, dl, TII->get(DSPIC::JCC))
                 .addMBB(NextMBB)
                 .add(Cond[0]);
        InstrSizeDiff += TII->getInstSizeInBytes(*MI);
        ++MI;
      }

      // Unconditional branch to the real destination.
      MI = BuildMI(*MBB, MI, dl, TII->get(DSPIC::Bi)).addMBB(DestBB);
      InstrSizeDiff += TII->getInstSizeInBytes(*MI);

      // Remove the old branch from the function.
      OldBranch.eraseFromParent();

      // The size of a new instruction is different from the old one, so we need
      // to correct all block offsets.
      for (int i = MBB->getNumber() + 1, e = BlockOffsets.size(); i < e; ++i) {
        BlockOffsets[i] += InstrSizeDiff;
      }
      MBBStartOffset += InstrSizeDiff;

      ++NumExpanded;
      MadeChange = true;
    }
  }
  return MadeChange;
}

bool DSPICBSelImpl::runOnMachineFunction(MachineFunction &mf) {
  MF = &mf;
  TII = static_cast<const DSPICInstrInfo *>(MF->getSubtarget().getInstrInfo());

  // If the pass is disabled, just bail early.
  if (!BranchSelectEnabled)
    return false;

  LLVM_DEBUG(dbgs() << "\n********** " << DEBUG_TYPE << " **********\n");

  // BlockOffsets - Contains the distance from the beginning of the function to
  // the beginning of each basic block.
  OffsetVector BlockOffsets;

  unsigned FunctionSize = measureFunction(BlockOffsets);
  // If the entire function is smaller than the displacement of a branch field,
  // we know we don't need to expand any branches in this
  // function. This is a common case.
  if (isInRage(FunctionSize)) {
    return false;
  }

  // Iteratively expand branches until we reach a fixed point.
  bool MadeChange = false;
  while (expandBranches(BlockOffsets))
    MadeChange = true;

  return MadeChange;
}

bool DSPICBranchSelectLegacyPass::runOnMachineFunction(MachineFunction &MF) {
  return DSPICBSelImpl().runOnMachineFunction(MF);
}

PreservedAnalyses
DSPICBranchSelectPass::run(MachineFunction &MF,
                            MachineFunctionAnalysisManager &MFAM) {
  return DSPICBSelImpl().runOnMachineFunction(MF)
             ? getMachineFunctionPassPreservedAnalyses()
             : PreservedAnalyses::all();
}

/// Returns an instance of the Branch Selection Pass
FunctionPass *llvm::createDSPICBranchSelectLegacyPass() {
  return new DSPICBranchSelectLegacyPass();
}
