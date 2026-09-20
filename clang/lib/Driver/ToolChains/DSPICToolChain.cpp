//===--- DSPICToolChain.cpp - dsPIC ToolChain Implementation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 133, ROUTE A -- the external assembler. The header carries the why; this file
// carries the argv, and every clause of it is a line the VENDOR DRIVER printed about itself
// (steps/frontend/asdrv-ask.sh) rather than a flag anybody thought sensible.
//
//===----------------------------------------------------------------------===//

// ⚠ `clang/Driver/CommonArgs.h` and `clang/Options/Options.h`, NOT the "CommonArgs.h" /
// "clang/Driver/Options.h" spellings a reader carries from older LLVM: both moved, and the first
// build of this file failed on exactly that. XCore.cpp is the live model for the include block.
#include "DSPICToolChain.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

DSPICToolChain::DSPICToolChain(const Driver &D, const llvm::Triple &Triple,
                               const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {}

Tool *DSPICToolChain::buildAssembler() const {
  return new tools::dspic::Assembler(*this);
}

// ⛔ --relax TRACKS THE SOURCE LANGUAGE, NOT THE -O LEVEL, AND THAT DISTINCTION WAS MEASURED.
// The vendor passes --relax for a C unit at -O0 and with no -O at all, and NOT for a hand-written
// .s even at -O1 (asdrv-ask.sh arms B2/B3/B4). It is also not "cc1 ran": a .S goes through
// `cc1 -E -lang-asm` and gets no --relax (arm A4). What separates them is whether the assembler's
// input was PRODUCED BY A COMPILE, which is exactly what this asks.
//
// ⚠ Reading arms A1-vs-A3 alone would have supported "the C path" OR "the -O level" OR "the -I
// flags", three axes moving at once. The B arms exist because a domain you chose is not a domain
// you measured.
static bool inputCameFromACompile(const JobAction &JA) {
  for (const Action *Input : JA.getInputs())
    if (isa<BackendJobAction>(Input) || isa<CompileJobAction>(Input))
      return true;
  return false;
}

void tools::dspic::Assembler::ConstructJob(Compilation &C, const JobAction &JA,
                                           const InputInfo &Output,
                                           const InputInfoList &Inputs,
                                           const ArgList &Args,
                                           const char *LinkingOutput) const {
  claimNoWarnArgs(Args);
  ArgStringList CmdArgs;

  // The user's -I directories, as separate `-I <dir>` pairs. The vendor forwards these on BOTH
  // the compile-and-assemble path and the assemble-only path (arm B1), which is why this is not
  // guarded on where the input came from: bl_fw's hand-written aes.s uses .include.
  for (const Arg *A : Args.filtered(options::OPT_I)) {
    A->claim();
    CmdArgs.push_back("-I");
    CmdArgs.push_back(A->getValue());
  }

  if (inputCameFromACompile(JA))
    CmdArgs.push_back("--relax");

  // -p<CPU>, from -mcpu=, with no space -- the vendor's own spelling, e.g. -p33CK1024MP705.
  // ⚠ Absent when -mcpu is absent. The vendor driver always has a device, so it never shows us
  // that case; this port's row PRINTS what a device-less assemble does rather than asserting a
  // value nobody measured.
  if (const Arg *A = Args.getLastArg(options::OPT_mcpu_EQ))
    CmdArgs.push_back(Args.MakeArgString(Twine("-p") + A->getValue()));

  if (const Arg *A = Args.getLastArg(options::OPT_mdfp_EQ))
    CmdArgs.push_back(Args.MakeArgString(Twine("-mdfp=") + A->getValue()));

  if (Args.hasArg(options::OPT_v))
    CmdArgs.push_back("-v");

  // ⛔ AFTER --relax AND AFTER -mdfp=, which is where the vendor puts it (arm B6): the assembler
  // takes the last of --relax/--no-relax, so a user `-Wa,--no-relax` must be able to win.
  Args.AddAllArgValues(CmdArgs, options::OPT_Wa_COMMA, options::OPT_Xassembler);

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  for (const auto &II : Inputs)
    CmdArgs.push_back(II.getFilename());

  const char *Exec =
      Args.MakeArgString(getToolChain().GetProgramPath("pic30-elf-as"));
  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Exec, CmdArgs, Inputs, Output));
}
