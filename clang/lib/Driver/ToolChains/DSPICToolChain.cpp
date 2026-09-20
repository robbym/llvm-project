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
#include "llvm/Support/Path.h"

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

// ⛔ THE C++ HEADERS ARE ON THE INSTALL AXIS, NOT THE DEVICE AXIS, AND THAT WAS MEASURED BEFORE
// IT WAS DESIGNED (steps/frontend/CXXINC.banked.txt). cc1plus searches, in this order, the two
// PACK directories -- which Clang.cpp's dspic block already adds from -mcpu/-mdfp, session 120 --
// then <install>/include/c++, then <install>/include, then <install>/support/generic/h. The C
// driver's list is the same MINUS include/c++: exactly one directory separates them. That
// directory does not move when -mcpu changes, does not move when -mdfp is dropped, and does not
// exist inside any device pack, so the mechanism that supplies it cannot be -mdfp.
//
// It is --sysroot, because the install IS a sysroot layout (<root>/include, <root>/include/c++,
// <root>/lib) and clang already threads --sysroot everywhere. ⚠ It changed NOTHING for this
// triple before this function existed, in C or C++, which is the control the row's cells rest on.
//
// ⚠ <sysroot>/support/generic/h is deliberately NOT here. Its builtins.h has to beat clang's
// empty stub and session 120 measured that this requires a USER -I; an -internal-isystem lands
// behind the resource directory and would silently take that header's ownership. A build that
// wants it says -I, as steps/config/blfw-build.sh does and says why.
void DSPICToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                               ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdlibinc))
    return;
  // getDriver().SysRoot and not computeSysRoot(): the latter is Generic_GCC's, and its answer
  // depends on GCC-installation detection that finds nothing for this triple. One value, from
  // the flag the build passed.
  StringRef SysRoot = getDriver().SysRoot;
  if (SysRoot.empty())
    return;
  SmallString<128> P(SysRoot);
  llvm::sys::path::append(P, "include");
  // Passed whether or not it exists, the way Clang.cpp's dspic block passes the four pack
  // candidates no installed pack has: a nonexistent -internal-isystem is dropped at lookup, and
  // GCC drops its own the same way. Cell E11 pins that.
  addSystemInclude(DriverArgs, CC1Args, P);
}

void DSPICToolChain::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                                  ArgStringList &CC1Args) const {
  // The same three suppressions Generic_GCC's own override honours, so -nostdinc++ removes the
  // C++ directory and leaves the C one -- which is what the vendor's C driver produces.
  if (DriverArgs.hasArg(options::OPT_nostdinc, options::OPT_nostdincxx,
                        options::OPT_nostdlibinc))
    return;
  StringRef SysRoot = getDriver().SysRoot;
  if (SysRoot.empty())
    return;
  SmallString<128> P(SysRoot);
  llvm::sys::path::append(P, "include", "c++");
  addSystemInclude(DriverArgs, CC1Args, P);
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
