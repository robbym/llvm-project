//===--- DSPICToolChain.h - dsPIC ToolChain Implementation ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 133, ROUTE A. Until this file existed the `dspic` triple had NO ToolChain of
// its own: Driver::getToolChain's default arm fell through to `Generic_ELF`, whose assembler is
// gnutools::Assembler, which forks the HOST's `/usr/bin/as`. So `clang -c` for dsPIC either wrote
// MSP430 bytes through the integrated assembler (32 files, all EM_MSP430, ZERO usable -- session
// 126's census) or, under -fno-integrated-as, handed dsPIC assembly to an x86 assembler.
//
// The vendor has no integrated assembler either: `xc-dsc-gcc -c` forks cc1 and then
// xc-dsc-as.exe. The standing vendor-compatibility rule binds on ACCEPTANCE, not on the
// pipeline's shape, so forking a real pic30 assembler discharges it without writing a single
// instruction encoding.
//
// ⛔ THE ARGV IS THE VENDOR'S OWN, ASKED AND BANKED, NOT RECONSTRUCTED. See
// trc/tools/dspic-llvm/steps/frontend/asdrv-ask.sh; the shape is
//
//     as [-I <dir>]* [--relax] -p<CPU> -mdfp=<DFP> [<-Wa passthrough>] -o <out> <in>
//
// ⚠ THE FILE IS `DSPICToolChain` AND NOT `DSPIC`, which is what upstream would call it, because
// clang/lib/Basic/Targets/DSPIC.cpp and llvm/lib/Target/DSPIC/ already exist and this port's
// row-selection instrument resolves a query by BASENAME. A third `DSPIC.cpp` would make every
// `whichrows.py DSPIC.cpp` ambiguous across three layers. `CSKYToolChain.cpp` and
// `VEToolchain.cpp` are upstream precedents for the spelling.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_DSPICTOOLCHAIN_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_DSPICTOOLCHAIN_H

#include "Gnu.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace tools {
namespace dspic {

/// The pic30 assembler, forked the way the vendor driver forks it.
class LLVM_LIBRARY_VISIBILITY Assembler final : public Tool {
public:
  Assembler(const ToolChain &TC)
      : Tool("dspic::Assembler", "pic30-elf-as", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

/// The pic30 linker, forked the way the vendor driver forks its own -- session 137, and the
/// same decision session 133 made for the assembler: the vendor has no integrated linker either,
/// and the standing vendor-compatibility rule binds on ACCEPTANCE, not on the pipeline's shape.
class LLVM_LIBRARY_VISIBILITY Linker final : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("dspic::Linker", "pic30-elf-ld", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

} // end namespace dspic
} // end namespace tools

namespace toolchains {

class LLVM_LIBRARY_VISIBILITY DSPICToolChain : public Generic_ELF {
public:
  DSPICToolChain(const Driver &D, const llvm::Triple &Triple,
                 const llvm::opt::ArgList &Args);

  // ⛔ trellis session 138 (post-close), the operator's "vendor parity": -fno-use-cxa-atexit is
  // this toolchain's DEFAULT, as the vendor's cc1plus is configured -- its cc1plus line carries no
  // cxa option, it registers a function-local static with atexit, and an explicit
  // -fuse-cxa-atexit sends every static to __cxa_atexit, which does not link on this install.
  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args, BoundArch BA,
                             Action::OffloadKind DeviceOffloadKind) const override;

  // ⛔ FALSE, AND THE PRICE WAS MEASURED BEFORE IT WAS CHOSEN. This flag is read when the cc1
  // line is built, not only when the assemble step is: it adds -no-integrated-as and
  // -fno-dwarf-directory-asm and drops -faddrsig. Over the two firmwares' 103 units, at each
  // build script's own flags, the TEXT cc1 emits is byte-identical either way
  // (steps/frontend/asdrv-probe.sh, P2 and P2b). Leaving the integrated assembler the default
  // keeps `-c` broken by default, which is the state sessions 88, 92, 96, 98 and 126 each
  // re-found.
  bool IsIntegratedAssemblerDefault() const override { return false; }

  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return true; }

  // trellis session 135: the VENDOR-INSTALL half of cc1's include list, named by --sysroot.
  // Measured: the vendor's C++ search list is its C list plus exactly ONE directory,
  // <install>/include/c++, and it is INSTALL-keyed -- the device pack carries no C++ headers at
  // all. cc1plus reaches it from its own -iprefix, which our clang has no equivalent of, so the
  // build names the install and the driver spells out cc1's order.
  void AddClangSystemIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                 llvm::opt::ArgStringList &CC1Args) const override;
  void AddClangCXXStdlibIncludeArgs(const llvm::opt::ArgList &DriverArgs,
                                    llvm::opt::ArgStringList &CC1Args) const override;

protected:
  Tool *buildAssembler() const override;
  Tool *buildLinker() const override;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_DSPICTOOLCHAIN_H
