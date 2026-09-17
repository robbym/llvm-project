//===--- DSPIC.cpp - Implement dsPIC33 target feature support -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis L1f-b (session 86). The predefined macros are the FAMILY ones cc1 sets
// (`__dsPIC30__`, `__dsPIC33C__`) plus this target's own (`__DSPIC__`, `__dsPIC33__`);
// `__XC16__` / `__C30__` name a compiler this is not, and are NOT defined.
//
//===----------------------------------------------------------------------===//

#include "DSPIC.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/MacroBuilder.h"
#include "clang/Basic/TargetBuiltins.h"

using namespace clang;
using namespace clang::targets;

// trellis session 103: the table read/write builtins, the AVR `.def` shape.
static constexpr int NumBuiltins =
    clang::DSPIC::LastTSBuiltin - Builtin::FirstTSBuiltin;

static constexpr llvm::StringTable BuiltinStrings =
    CLANG_BUILTIN_STR_TABLE_START
#define BUILTIN CLANG_BUILTIN_STR_TABLE
#include "clang/Basic/BuiltinsDSPIC.def"
    ;

static constexpr auto BuiltinInfos = Builtin::MakeInfos<NumBuiltins>({
#define BUILTIN CLANG_BUILTIN_ENTRY
#include "clang/Basic/BuiltinsDSPIC.def"
});

llvm::SmallVector<Builtin::InfosShard>
DSPICTargetInfo::getTargetBuiltins() const {
  return {{&BuiltinStrings, BuiltinInfos}};
}

// trellis session 119: the resource-file reader's includes (below the builtin shard on purpose --
// see steps/frontend/mcpu-edit.py on why they are not with the file's other includes).
#include "clang/Basic/Diagnostic.h"
#include "llvm/Support/IOSandbox.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstring>

const char *const DSPICTargetInfo::GCCRegNames[] = {
    "w0", "w1", "w2",  "w3",  "w4",  "w5",  "w6",  "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15"};

ArrayRef<const char *> DSPICTargetInfo::getGCCRegNames() const {
  return llvm::ArrayRef(GCCRegNames);
}

// ── trellis session 119: THE RESOURCE FILE, <dfp>/bin/c30_device.info ─────────────────────────
// The format is the vendor's own (c30_resource/src/generator/resource.c, XC-DSC 4.00 GPL sources;
// session 83 read it first, session 119's device-ask.py reproduces cc1's six device macros from it
// 48 of 48 at five devices): a bit-and-nybble-swapped NUL-terminated tool name, u16 major, u16
// minor, u8 increment, u32 field count, u32 field sizes, then bytes until the accumulated marker
// 0xFFFFFFFF, then fixed-size records -- field 0 the packed name, field 1 a flags word whose top
// nybble is the record type, field 2 the device id (a device record) or the device a vector
// belongs to (a vector record). Only what the device macros need is here, cited by line into
// c30_flag_definitions.h.
namespace {
enum : unsigned {
  P30F = 1u << 0, P30FSMPS = 1u << 1, P33F = 1u << 2, P24F = 1u << 3, P24H = 1u << 4,
  P24FK = 1u << 5, P33E = 1u << 6, P24E = 1u << 7,                                  // :31-38
  HAS_DSP = 1u << 8, HAS_EEDATA = 1u << 9, HAS_DMA = 1u << 10, HAS_CODEGUARD = 1u << 11,
  HAS_PMP = 1u << 12, HAS_EDS = 1u << 13, HAS_PMPV2 = 1u << 14, HAS_DMAV2 = 1u << 16,
  HAS_AUXFLASH = 1u << 17, HAS_5VOLTS = 1u << 18, HAS_ISAV4 = 1u << 24,
  HAS_ISA32V0 = 1u << 25,                                                            // :46-63
  RECORD_TYPE_MASK = 0xF0000000u, IS_VECTOR_ID = 0x20000000u, IS_DEVICE_ID = 0x40000000u, // :15,:125-126
};

// resource.c resource_pack_string: swap every bit pair, then the nybbles. An involution, so the
// same function packs a name for comparison and unpacks a stored one.
static unsigned char packByte(unsigned char C) {
  unsigned char L = (unsigned char)(((C << 1) & 0xAA) | ((C >> 1) & 0x55));
  return (unsigned char)((L >> 4) | (L << 4));
}
static uint32_t rd32(const unsigned char *P) {
  return (uint32_t)P[0] | ((uint32_t)P[1] << 8) | ((uint32_t)P[2] << 16) | ((uint32_t)P[3] << 24);
}

struct DeviceRecord { unsigned Flags = 0; unsigned Id = 0; unsigned IVT = 0; };

// "" on success; "open" / "format" / "cpu" otherwise, each a different sentence at the caller.
static std::string readDevice(StringRef Path, StringRef CPU, DeviceRecord &Out) {
  auto BypassSandbox = llvm::sys::sandbox::scopedDisable();
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return "open";
  StringRef S = (*Buf)->getBuffer();
  const unsigned char *B = (const unsigned char *)S.data();
  size_t N = S.size(), P = 0;
  std::string Tool;
  while (P < N && B[P])
    Tool.push_back((char)packByte(B[P++]));
  if (Tool != "C30" || P + 10 > N)
    return "format";
  P += 1 + 2 + 2 + 1; // the NUL, major, minor, increment
  uint32_t FC = rd32(B + P); P += 4;
  if (FC < 3 || FC > 64 || P + 4 * FC > N)
    return "format";
  llvm::SmallVector<uint32_t, 8> Sizes;
  uint32_t Rec = 0;
  for (uint32_t I = 0; I < FC; ++I) { Sizes.push_back(rd32(B + P)); Rec += Sizes.back(); P += 4; }
  uint32_t Acc = 0;
  while (P < N && Acc != 0xFFFFFFFFu)
    Acc = (Acc << 8) | B[P++];
  if (Acc != 0xFFFFFFFFu || Rec == 0)
    return "format";
  const size_t Off1 = Sizes[0], Off2 = Sizes[0] + Sizes[1];
  // the packed name to look for, NUL-terminated, at most the name field's width
  std::string Want;
  for (char C : CPU) Want.push_back((char)packByte((unsigned char)C));
  Want.push_back('\0');
  if (Want.size() > Sizes[0])
    return "cpu";
  bool Found = false;
  for (size_t R = P; R + Rec <= N; R += Rec) {
    uint32_t F = rd32(B + R + Off1);
    if ((F & RECORD_TYPE_MASK) == IS_DEVICE_ID && memcmp(B + R, Want.data(), Want.size()) == 0) {
      Out.Flags = F; Out.Id = rd32(B + R + Off2); Found = true; break;
    }
  }
  if (!Found)
    return "cpu";
  // pic30.c:1817-1831: a vector belongs to this device if it names the device's id, or names no
  // device and its flags meet the device's mask. (Measured: in every installed pack every vector
  // record names a device, so the second clause contributes 0 -- it is transcribed, not relied on.)
  const unsigned Mask = Out.Flags & ~IS_DEVICE_ID;
  for (size_t R = P; R + Rec <= N; R += Rec) {
    uint32_t F = rd32(B + R + Off1);
    if ((F & RECORD_TYPE_MASK) != IS_VECTOR_ID)
      continue;
    uint32_t Dev = rd32(B + R + Off2);
    if ((Dev && Dev == Out.Id) || (Dev == 0 && (F & Mask)))
      ++Out.IVT;
  }
  return "";
}
} // namespace

bool DSPICTargetInfo::validateTarget(DiagnosticsEngine &Diags) const {
  if (CPU.empty())
    return true;
  // the backend's own three processors (DSPIC.td) pass through: no device, no device macros
  if (CPU == "GENERIC" || CPU == "DSPIC" || CPU == "DSPICX")
    return true;
  auto Err = [&](const Twine &Msg) {
    Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error, "%0")) << Msg.str();
    return false;
  };
  // Every refusal below carries cc1's OWN sentence for the case (elf-cc1: "Invalid -mcpu option.
  // CPU X not recognized." / "Could not open resource file: P"), never a silent default.
  const std::string &DFP = getTargetOpts().DFP;
  if (DFP.empty())
    return Err("Invalid -mcpu option.  CPU " + CPU + " not recognized: no device family pack -- "
               "pass -mdfp=<pack>/xc16 (the vendor driver's own -mdfp), whose bin/c30_device.info "
               "names the devices");
  std::string Path = DFP + "/bin/c30_device.info";
  DeviceRecord R;
  std::string E = readDevice(Path, CPU, R);
  if (E == "open")
    return Err("Could not open resource file: " + Path);
  if (E == "format")
    return Err(Path + " is not a C30 resource file");
  if (E == "cpu")
    return Err("Invalid -mcpu option.  CPU " + CPU + " not recognized.");
  // COSTED, not parity: cc1 compiles the dsPIC33A/PIC32A parts; this backend is the 16-bit dsPIC
  // and a silent 16-bit compile of a 32-bit part is the wrong-answer class.
  if (R.Flags & HAS_ISA32V0)
    return Err("device " + CPU + " is a 32-bit dsPIC33A/PIC32A part (HAS_ISA32V0 in the resource "
               "file); this target is the 16-bit dsPIC");
  // pic30.c:1789-1798 sets the architecture from the record's FLAGS -- ten overwriting ifs, later
  // wins -- and ~4255-4280 spells the family macro from it. The flags are cc1's mechanism; a name
  // rule would agree over every installed pack (device-ask.py P6b) but is not what cc1 does.
  const char *Fam = "__dsPIC30F__";
  if (R.Flags & P30F) Fam = "__dsPIC30F__";
  if (R.Flags & P33E) Fam = "__dsPIC33E__";
  if (R.Flags & P33F) Fam = "__dsPIC33F__";
  if (R.Flags & P30FSMPS) Fam = "__dsPIC30F__";
  if (R.Flags & P24F) Fam = "__PIC24F__";
  if (R.Flags & P24E) Fam = "__PIC24E__";
  if (R.Flags & P24H) Fam = "__PIC24H__";
  if (R.Flags & P24FK) Fam = "__PIC24FK__";
  if (R.Flags & HAS_ISAV4) Fam = "__dsPIC33C__";
  FamilyMacro = Fam;
  // pic30.c:4229 spells "__dsPIC<CPU>__" and :4262 respells it "__PIC<CPU>__" for the PIC24
  // families -- measured at 24F16KA102: cc1 defines __PIC24F16KA102__.
  DeviceMacro = (StringRef(Fam).starts_with("__PIC24") ? "__PIC" : "__dsPIC") + CPU + "__";
  DeviceFlags = R.Flags;
  IVTNum = R.IVT;
  HaveDevice = true;
  return true;
}

void DSPICTargetInfo::getTargetDefines(const LangOptions &Opts,
                                       MacroBuilder &Builder) const {
  Builder.defineMacro("__DSPIC__");
  Builder.defineMacro("__dsPIC33__");
  // ── trellis session 119: THE DEVICE MACROS, from the pack's resource file ────────────────
  // The FAMILY macro comes from the device, exactly as cc1's does, and with no device there is
  // none -- measured: the vendor driver given -mdfp and no -mcpu defines no family and no device
  // macro. Before this session __dsPIC33C__ was defined UNCONDITIONALLY here, wrong for every
  // non-33C device and for the no-device case alike. The __HAS_* set is pic30-c.c:215-230 in its
  // own order; __HAS_DSP__ and __HAS_CODEGUARD__ were session 118's unconditional "family" macros
  // and are the device's -- their two lines in the session-118 block below are a comment now.
  if (HaveDevice) {
    Builder.defineMacro(DeviceMacro);
    Builder.defineMacro(FamilyMacro);
    Builder.defineMacro("__IVT_NUM", Twine(IVTNum));
    if (DeviceFlags & HAS_DSP) Builder.defineMacro("__HAS_DSP__");
    if (DeviceFlags & HAS_EEDATA) Builder.defineMacro("__HAS_EEDATA__");
    if (DeviceFlags & (HAS_DMA | HAS_DMAV2)) Builder.defineMacro("__HAS_DMA__");
    if (DeviceFlags & HAS_AUXFLASH) Builder.defineMacro("__HAS_AUXFLASH__");
    if (DeviceFlags & HAS_DMAV2) Builder.defineMacro("__HAS_DMAV2__");
    if (DeviceFlags & HAS_CODEGUARD) Builder.defineMacro("__HAS_CODEGUARD__");
    if (DeviceFlags & (HAS_PMP | HAS_PMPV2)) Builder.defineMacro("__HAS_PMP__");
    if (DeviceFlags & HAS_PMPV2) Builder.defineMacro("__HAS_PMPV2__");
    if (DeviceFlags & HAS_EDS) Builder.defineMacro("__HAS_EDS__");
    // pic30-c.c:228: HAS_5VOLTS, or the dsPIC30F architecture itself -- TARGET_ARCH(PIC30F) is
    // MASK_ARCH_PIC30FXXXX | MASK_ARCH_PIC30F202X (pic30.h:478), set from P30F and from P30FSMPS
    // (pic30.c:1789,1792). ⚠ The first landing tested P30F alone; a refuter read the second bit
    // out of the source. Inert over every installed pack (0 P30FSMPS records lack HAS_5VOLTS).
    if ((DeviceFlags & HAS_5VOLTS) || (DeviceFlags & (P30F | P30FSMPS)))
      Builder.defineMacro("__HAS_5VOLTS__");
  } else {
    // no device: cc1's own no-device set has __HAS_DSP__ and nothing else of this family
    Builder.defineMacro("__HAS_DSP__");
  }
  Builder.defineMacro("__dsPIC30__");

  // Microchip C SPACE-QUALIFIER keywords (trellis session 88, "run it through"): cc1 knows
  // `__prog__` (program/flash space) and `__eds__` (extended data space) as bare TYPE QUALIFIERS;
  // clang does not and errors ("unknown type name") in qualifier position. In the vendor device
  // headers and firmware these are ALWAYS paired with `__attribute__((space(...)))`, which carries
  // the real placement (clang tolerates the unknown attribute), so recognizing the keyword unblocks
  // compilation. `__pack_upper_byte` is the packed program-pointer qualifier from libpic30.h.
  // (`__sfr__`/`__deprecated__`/`__unsafe__` need nothing -- they appear only inside
  // `__attribute__((...))`, which clang already tolerates.)
  // NOTE: RECOGNIZED FOR COMPILATION ONLY. Program-space PLACEMENT beyond an explicit `section()`
  // and program-space ACCESS (tblrd/PSV, the packed-pointer representation) are the data-model
  // work (backend stage L1e); they are NOT modelled by this recognition.
  Builder.defineMacro("__prog__", "__attribute__((address_space(1)))");
  // session 110: `__eds__` is MODELLED, not silenced. It was defined EMPTY here, so
  // `__eds__ int gv; return gv;` compiled to a direct near access ignoring the address space,
  // silently, where cc1 emits the DSRPAG-windowed read. The operator: "any program the user
  // writes targetting the xc-dsc compiler needs to work here".
  Builder.defineMacro("__eds__", "__attribute__((address_space(2)))");
  // session 110: `__external__` is MODELLED. It was defined EMPTY, so a direct read compiled to
  // an ordinary near access -- silently the wrong memory. Address space 3; every load and store
  // in it is refused in cc1's own words (SemaExpr.cpp), which is what cc1 does too.
  Builder.defineMacro("__external__", "__attribute__((address_space(3)))");
  // session 111: `__pack_upper_byte` is MODELLED. It was defined EMPTY, so a packed-flash object
  // was an ordinary near data object read from RAM -- silently the wrong memory. Address space 4:
  // a 32-bit linear pointer, placement in `,packedflash`, every read through the vendor's own
  // ___P32DFrd, the record laid out packed (SemaType.cpp), writes refused in cc1's sentence.
  Builder.defineMacro("__pack_upper_byte", "__attribute__((address_space(4)))");

  // ── trellis session 118: THE VENDOR'S FAMILY-WIDE PREDEFINED MACROS ──────────────────────
  // Every name and value below is read off `xc-dsc-gcc -dM -E` at FOUR devices
  // (steps/frontend/macro-ask.sh; banked at prints/l1f/frontend/macro/), and every one of them is
  // identical at all four -- 33CK1024MP705, 33CK256MP508, 33EP256MU806 and 30F6014A. The macros
  // that MOVE with the device (__dsPIC<part>__, __IVT_NUM, __HAS_DMA__, __HAS_DMAV2__,
  // __HAS_EDS__, __HAS_PMP__) are deliberately NOT here: they need the device on the command line
  // and the pack's database, and this target takes no -mcpu.
  //
  // ⛔ WHY THE COMPILER AND NOT A BUILD SCRIPT. These were compensated by eleven hand-written -D
  // flags in each of two build scripts. A missing macro is the silent kind of divergence -- the
  // `#if` takes the other arm and the two compilers build different programs from one source --
  // and 54 firmware files test __XC16__, 52 test __dsPIC33C__, 166 pack headers test XC16.
  Builder.defineMacro("__C30__");
  Builder.defineMacro("__C30");
  Builder.defineMacro("C30");
  Builder.defineMacro("__C30ELF__");
  Builder.defineMacro("__C30ELF");
  Builder.defineMacro("__XC__");
  Builder.defineMacro("__XC16__");
  Builder.defineMacro("__XC16");
  Builder.defineMacro("XC16");
  Builder.defineMacro("__XC16ELF__");
  Builder.defineMacro("__XC16ELF");
  Builder.defineMacro("__XC_DSC__");
  Builder.defineMacro("__XC_DSC");
  Builder.defineMacro("XC_DSC");
  Builder.defineMacro("__XC_DSCELF__");
  Builder.defineMacro("__XC_DSCELF");
  Builder.defineMacro("__XC_DSC_MUSL__");
  Builder.defineMacro("__dsPIC30ELF__");
  Builder.defineMacro("__dsPIC30ELF");
  Builder.defineMacro("__dsPIC30");
  Builder.defineMacro("dsPIC30");
  Builder.defineMacro("__BUILTIN_ITTYPE");
  Builder.defineMacro("__HAS_BUILTINS_16__");
  // __HAS_CODEGUARD__ / __HAS_DSP__: mask-derived since session 119 -- the device block above
  Builder.defineMacro("__LONG_LONG_WIDTH__", "64");
  // ⚠ THE VERSION MACROS ARE ZERO BECAUSE THE VENDOR'S OWN SHIPPED COMPILER REPORTS ZERO. Asked
  // of xc-dsc-gcc v4.00 directly: __C30_VERSION__ 0, __XC16_VERSION__ 0, __XC_DSC_VERSION__ 0.
  // The record already establishes why (the install's __VERSION__ carries the unsubstituted token
  // MCHP_VERSION -- its version macros are broken, not authoritative), and steps/stn3255/build.sh
  // deliberately passes -D__C30_VERSION__=4000 over it, measured from the vendor's own ELF. A
  // command-line -D wins over a target define, so that override still stands.
  Builder.defineMacro("__C30_VERSION__", "0");
  Builder.defineMacro("__XC16_VERSION__", "0");
  Builder.defineMacro("__XC16_VERSION", "0");
  Builder.defineMacro("__XC_DSC_VERSION__", "0");
  // __XC16_BUILD_DATE__ / __XC_DSC_BUILD_DATE__ are COSTED, not implemented: a vendor build date
  // ("Aug 31 2026" on this install), with zero customers measured in the vendor libc headers, the
  // device pack's support headers or either firmware tree. Inventing one is a claim about a build
  // that did not happen.
  //
  // __LARGE_ARRAYS__ is EXACT, from the target feature this port already carries. Measured through
  // cc1: 0 by default, 1 under -menable-large-arrays, and it does NOT move with -mlarge-code or
  // -mlarge-data.
  Builder.defineMacro("__LARGE_ARRAYS__", LargeArrays ? "1" : "0");
  // __OPTIMIZATION_LEVEL__ is NOT here: getTargetDefines is handed LangOptions, which carries no
  // -O number. It is defined exactly, from CGOpts.OptimizationLevel, in InitPreprocessor.cpp --
  // three lines from where clang already defines __OPTIMIZE__ from the same field.
}
