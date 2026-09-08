//===-- DSPIC.td - Describe the DSPIC Target Machine -----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This is the top level entry point for the DSPIC target.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target-independent interfaces
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

//===----------------------------------------------------------------------===//
// Subtarget Features. 
//===----------------------------------------------------------------------===//
// trellis session 96 (follow-up 15): the data-model features exist so the driver has somewhere
// to put them and `llc` can be told; the PLACEMENT decision itself is clang's, stamped per global
// as `near`/`far`, so these are declarations rather than switches the backend reads.
def FeatureLargeScalar
 : SubtargetFeature<"large-scalar", "LargeScalar", "true",
                    "Place scalars in far data">;
def FeatureSmallAggregate
 : SubtargetFeature<"small-aggregate", "SmallAggregate", "true",
                    "Place aggregates in near data">;
def FeatureConstInData
 : SubtargetFeature<"const-in-data", "ConstInData", "true",
                    "Place constants in data memory">;
def FeatureLargeArrays
 : SubtargetFeature<"large-arrays", "LargeArrays", "true",
                    "Allow arrays larger than 32K">;

def FeatureLargeCode
 : SubtargetFeature<"large-code", "LargeCode", "true",
                    "call (24-bit absolute) for every direct call; the default is rcall, as cc1's small-code model (trellis L1f-a)">;

def FeatureX
 : SubtargetFeature<"ext", "ExtendedInsts", "true",
                    "Enable DSPIC-X extensions">;

def FeatureHWMult16
 : SubtargetFeature<"hwmult16", "HWMultMode", "HWMult16",
                    "Enable 16-bit hardware multiplier">;

def FeatureHWMult32
 : SubtargetFeature<"hwmult32", "HWMultMode", "HWMult32",
                    "Enable 32-bit hardware multiplier">;

def FeatureHWMultF5
 : SubtargetFeature<"hwmultf5", "HWMultMode", "HWMultF5",
                    "Enable F5 series hardware multiplier">;

//===----------------------------------------------------------------------===//
// DSPIC supported processors.
//===----------------------------------------------------------------------===//
class Proc<string Name, list<SubtargetFeature> Features>
 : Processor<Name, NoItineraries, Features>;

def : Proc<"generic",         []>;
def : Proc<"dspic",          []>;
def : Proc<"dspicx",         [FeatureX]>;

//===----------------------------------------------------------------------===//
// Register File Description
//===----------------------------------------------------------------------===//

include "DSPICRegisterInfo.td"

//===----------------------------------------------------------------------===//
// Calling Convention Description
//===----------------------------------------------------------------------===//

include "DSPICCallingConv.td"

//===----------------------------------------------------------------------===//
// Instruction Descriptions
//===----------------------------------------------------------------------===//

include "DSPICInstrInfo.td"

defm : RemapAllTargetPseudoPointerOperands<GR16>;

def DSPICInstrInfo : InstrInfo;

//===---------------------------------------------------------------------===//
// Assembly Printers
//===---------------------------------------------------------------------===//

def DSPICAsmWriter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
}

//===---------------------------------------------------------------------===//
// Assembly Parsers
//===---------------------------------------------------------------------===//

def DSPICAsmParser : AsmParser {
  let AllowDuplicateRegisterNames = 1;
  let ShouldEmitMatchRegisterAltName = 1;
}

//===----------------------------------------------------------------------===//
// Target Declaration
//===----------------------------------------------------------------------===//

def DSPIC : Target {
  let InstructionSet = DSPICInstrInfo;
  let AssemblyParsers = [DSPICAsmParser];
}

