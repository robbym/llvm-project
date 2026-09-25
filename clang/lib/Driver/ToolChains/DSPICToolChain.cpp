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
// trellis session 137: FileSystem.h for directory_iterator, which enumerates the pack's
// support/<FAMILY>/gld directories, and STLExtras.h for llvm::sort over them.
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

DSPICToolChain::DSPICToolChain(const Driver &D, const llvm::Triple &Triple,
                               const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {}

// ⛔ trellis session 138 (post-close): VENDOR PARITY for static destructors. Measured:
// xc-dsc-g++'s cc1plus line carries no cxa option at all, so atexit is its CONFIGURED default --
// a function-local static registers with atexit (runs on exit(), not when main returns), and a
// namespace-scope object goes to .dtors (ItaniumCXXABI::registerGlobalDtor's dspic arm, gated on
// exactly this flag). An explicit -fuse-cxa-atexit is honoured, as the vendor honours it: every
// static then goes to __cxa_atexit, and both compilers fail to link on ___dso_handle.
// Generic_ELF's hook is called first: it forwards an explicit -fno-use-init-array, and nothing
// else in the driver does. AVR's and XCore's shape. The row is steps/frontend/dtor-compare.sh.
void DSPICToolChain::addClangTargetOptions(const ArgList &DriverArgs,
                                           ArgStringList &CC1Args, BoundArch BA,
                                           Action::OffloadKind DeviceOffloadKind) const {
  Generic_ELF::addClangTargetOptions(DriverArgs, CC1Args, BA, DeviceOffloadKind);
  if (!DriverArgs.hasFlag(options::OPT_fuse_cxa_atexit,
                          options::OPT_fno_use_cxa_atexit, false))
    CC1Args.push_back("-fno-use-cxa-atexit");
}

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

Tool *DSPICToolChain::buildLinker() const {
  return new tools::dspic::Linker(*this);
}

// ⛔ THE ORDER IS A SEQUENCE, IT IS THE VENDOR'S OWN, AND IT WAS READ OFF ITS -### LINE RATHER
// THAN COMPOSED (steps/frontend/CXXLD.banked.txt ARM F). Over FOUR devices in THREE families and
// BOTH drivers the -l sequence is identical in all eight readings; C++ differs from C by exactly
// the two prepended tokens.
//
// ⚠ -lgcc APPEARS TWICE, OUTSIDE THE GROUP ON BOTH SIDES -- in C as well as C++. A single -lgcc
// inside the group is the natural spelling and is not what the vendor does; mutants N2 and N3
// apply it deliberately, and the pair separates "twice" from "outside".
// ⚠ -lstdc++ SITS BEFORE THE GROUP, NOT INSIDE IT. Session 136 put it inside and X8 and XC
// failed to link, which is the measured cost of reading this as a set; mutant N1 is that spelling.
// ⛔ AND NO CRT OBJECT IS ON THE LINE AT ALL -- no crt0.o, no crtbegin/crtend. The pic30 linker
// pulls crt0 from the archive through the linker script, so MSP430's GetFilePath("crt0.o") would
// look right here and fail to find a file.
static void addDefaultLibs(ArgStringList &CmdArgs, bool CXX) {
  if (CXX) {
    CmdArgs.push_back("-lstdc++");
    CmdArgs.push_back("-lm");
  }
  CmdArgs.push_back("-lgcc");
  CmdArgs.push_back("--start-group");
  CmdArgs.push_back("-lc99-pic30-elf");
  CmdArgs.push_back("-lm-elf");
  CmdArgs.push_back("-lc99-elf");
  CmdArgs.push_back("--end-group");
  CmdArgs.push_back("-lgcc");
}

// ⛔ EVERY support/<FAMILY>/gld THE PACK HAS, AND NOT THE DEVICE'S FAMILY. Two installed packs
// carry two families each -- dsPIC33E-GM-GP-MC-GU-MU has PIC24E and dsPIC33E, dsPIC33F-GP-MC has
// PIC24H and dsPIC33F -- and the vendor's line carries BOTH of a pack's directories, in order,
// for a device of either family (CXXLD2 ARM M, CXXLD3 ARM M2). The single-family packs cannot
// tell that rule from the family-derived one, which is why the two-family packs were read.
// ⚠ clang::dspic::packIncludeDirs answers the FAMILY question for include paths and is the
// obvious thing to reuse here. It is the wrong answer for this one: it yields exactly one family,
// so on those two packs it would emit one -L where the vendor emits two. Mutant N6 is that reuse.
// ⚠ SORTED. On the two packs that can show an order, sorted order and the vendor's agree; nothing
// installed here distinguishes "sorted" from "whatever order the vendor uses", and if a pack ever
// does, cell L4 is where it surfaces.
static void addPackLibraryPaths(const ArgList &Args, ArgStringList &CmdArgs) {
  const Arg *A = Args.getLastArg(options::OPT_mdfp_EQ);
  if (!A)
    return;
  SmallString<128> Support(A->getValue());
  llvm::sys::path::append(Support, "support");
  llvm::SmallVector<std::string, 4> Dirs;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator I(Support, EC), E; I != E && !EC;
       I.increment(EC)) {
    SmallString<128> Gld(I->path());
    llvm::sys::path::append(Gld, "gld");
    if (llvm::sys::fs::is_directory(Gld))
      Dirs.push_back(std::string(Gld));
  }
  llvm::sort(Dirs);
  for (const std::string &D : Dirs)
    CmdArgs.push_back(Args.MakeArgString(Twine("-L") + D));
}

void tools::dspic::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                        const InputInfo &Output,
                                        const InputInfoList &Inputs,
                                        const ArgList &Args,
                                        const char *LinkingOutput) const {
  const ToolChain &TC = getToolChain();
  ArgStringList CmdArgs;

  // -p<CPU> and the pack, GUARDED as the Assembler guards them -- absent when the flag is absent,
  // rather than defaulted to a value nobody measured -- but ⛔ NOT SPELLED AS THE ASSEMBLER SPELLS
  // THEM. The vendor writes `-mdfp=` to the assembler and `--mdfp=` TO THE LINKER, two dashes
  // (CXXLD.banked.txt ARM F). The first version of this file copied the assembler's one-dash
  // spelling; ld-new accepts it through getopt_long_only, so nothing failed to link and only cell
  // L6 -- which compares the token against the vendor's -- said so.
  if (const Arg *A = Args.getLastArg(options::OPT_mcpu_EQ))
    CmdArgs.push_back(Args.MakeArgString(Twine("-p") + A->getValue()));

  if (const Arg *A = Args.getLastArg(options::OPT_mdfp_EQ))
    CmdArgs.push_back(Args.MakeArgString(Twine("--mdfp=") + A->getValue()));

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // -u before the -L set, and a user -L before the driver's own: the vendor's own slots, read
  // from a line carrying all four of -L, -l, -Wl, and -u at once (ARM U).
  Args.addAllArgs(CmdArgs, {options::OPT_u});
  Args.AddAllArgs(CmdArgs, options::OPT_L);

  // <install>/bin and <install>/lib, from --sysroot -- session 135's mechanism for naming the
  // vendor install, unchanged and not re-derived. Absent when --sysroot is.
  StringRef SysRoot = TC.getDriver().SysRoot;
  if (!SysRoot.empty()) {
    SmallString<128> P(SysRoot);
    llvm::sys::path::append(P, "bin");
    CmdArgs.push_back(Args.MakeArgString(Twine("-L") + P));
  }
  addPackLibraryPaths(Args, CmdArgs);
  if (!SysRoot.empty()) {
    SmallString<128> P(SysRoot);
    llvm::sys::path::append(P, "lib");
    CmdArgs.push_back(Args.MakeArgString(Twine("-L") + P));
  }

  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // ⛔ -nostdlib AND -nodefaultlibs EACH strip every -l token while leaving an ld line, which is
  // MEASURED on the vendor's driver (ARM N) rather than taken from GCC's documentation.
  // -nostartfiles, -r, -static and -fno-exceptions change nothing there, and change nothing here
  // because this port passes no crt object at all.
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs))
    addDefaultLibs(CmdArgs,
                   TC.getDriver().CCCIsCXX() && TC.ShouldLinkCXXStdlib(Args));

  // ⚠ A DELIBERATE DIFFERENCE. The vendor puts a user -T between -lm and -lgcc; this puts it
  // last, which is MSP430's spelling. Measured: the two orders produce the SAME .hex and
  // identical nm output, and the ELF files differ in five .symtab bytes -- a transposition of two
  // absolute-SFR symbol pairs. Cell U4 asserts that AS a difference, not as a match.
  Args.AddAllArgs(CmdArgs, options::OPT_T);

  // ⛔ NO IMPLICIT LINKER SCRIPT, and the reason is in this file's header comment block in
  // cxxld-edit.py: OUTPUT_ARCH lives in the pack's .gld, the .gld needs a preprocessor, and the
  // thing that preprocesses it is the xc-dsc-ld.exe SHELL rather than the linker. An implicit -T
  // would work through one binary and fail through another for the same driver output.
  const char *Exec = Args.MakeArgString(TC.GetProgramPath("pic30-elf-ld"));
  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Exec, CmdArgs, Inputs, Output));
}
