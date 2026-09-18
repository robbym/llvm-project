//===------ DSPIC.cpp - Emit LLVM Code for dsPIC33 builtins ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 103: the table read/write builtins. Modelled on TargetBuiltins/AVR.cpp, which is
// the smallest complete example of this path in the tree.
// trellis session 108: the vendor set that reaches instructions no C expression spells -- the
// byte table forms, fbcl, the four widening multiplies, the swaps and the four control
// instructions. Names and signatures are the vendor's (BuiltinsDSPIC.def says where they were read).
//
//===----------------------------------------------------------------------===//

#include "CGBuiltin.h"
#include "clang/Basic/TargetBuiltins.h"
#include "clang/Basic/DSPICDevice.h"   // session 122: __builtin_vector_offset asks the device
#include "clang/Basic/TargetOptions.h" // session 122: ... by the -mcpu / -mdfp it was given
#include "llvm/IR/IntrinsicsDSPIC.h"
#include "llvm/IR/InlineAsm.h"  // session 121: the address operators emit inline asm

using namespace clang;
using namespace CodeGen;
using namespace llvm;

Value *CodeGenFunction::EmitDSPICBuiltinExpr(unsigned BuiltinID,
                                             const CallExpr *E) {
  // trellis session 122: every custom-typechecked dsPIC builtin takes exactly ONE argument, and
  // custom typechecking means Sema counted nothing. Without this, `__builtin_tblpage()` asserted
  // in getArg(0) (session 121's nine) and a second argument was silently dropped. cc1's sentences.
  if (getContext().BuiltinInfo.hasCustomTypechecking(BuiltinID) && E->getNumArgs() != 1) {
    CGM.Error(E->getExprLoc(),
              (llvm::Twine(E->getNumArgs() < 1 ? "too few" : "too many") +
               " arguments to function '" + getContext().BuiltinInfo.getName(BuiltinID) + "'")
                  .str());
    return llvm::PoisonValue::get(ConvertType(E->getType()));
  }
  switch (BuiltinID) {
  default:
    return nullptr;
  case DSPIC::BI__builtin_tblrdl:
  case DSPIC::BI__builtin_tblrdh: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblrdl
                       ? Intrinsic::dspic_tblrdl
                       : Intrinsic::dspic_tblrdh;
    Value *Off = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(IID), Off);
  }
  case DSPIC::BI__builtin_tblwtl:
  case DSPIC::BI__builtin_tblwth: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblwtl
                       ? Intrinsic::dspic_tblwtl
                       : Intrinsic::dspic_tblwth;
    // The vendor's argument order is (offset, value) and the intrinsic keeps it; the INSTRUCTION
    // prints them the other way round (`tblwtl.w Ws, [Wd]`), and the pattern in
    // DSPICInstrInfo.td is where that swap happens. Mutant MT3 exchanges them here.
    Value *Off = EmitScalarExpr(E->getArg(0));
    Value *Val = EmitScalarExpr(E->getArg(1));
    return Builder.CreateCall(CGM.getIntrinsic(IID), {Off, Val});
  }

  // ── trellis session 108 ───────────────────────────────────────────────────────────────────────
  case DSPIC::BI__builtin_tblrdlb:
  case DSPIC::BI__builtin_tblrdhb: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblrdlb
                       ? Intrinsic::dspic_tblrdlb
                       : Intrinsic::dspic_tblrdhb;
    Value *Off = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(IID), Off);
  }
  case DSPIC::BI__builtin_tblwtlb:
  case DSPIC::BI__builtin_tblwthb: {
    unsigned IID = BuiltinID == DSPIC::BI__builtin_tblwtlb
                       ? Intrinsic::dspic_tblwtlb
                       : Intrinsic::dspic_tblwthb;
    Value *Off = EmitScalarExpr(E->getArg(0));
    Value *Val = EmitScalarExpr(E->getArg(1));
    return Builder.CreateCall(CGM.getIntrinsic(IID), {Off, Val});
  }
  case DSPIC::BI__builtin_fbcl: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_fbcl), X);
  }
  case DSPIC::BI__builtin_mulss:
  case DSPIC::BI__builtin_muluu:
  case DSPIC::BI__builtin_mulsu:
  case DSPIC::BI__builtin_mulus: {
    // ⛔ NO INTRINSIC. The product of two extended i16s is exactly the IR shape the backend already
    // selects `mul.ss`/`mul.uu` from (SMUL_LOHI/UMUL_LOHI, L1f-d) and `mul.su`/`mul.us` from since
    // this session (combineMixedWideningMul on the i32 MUL). The extension of each operand is its
    // own signedness in the vendor's signature: ss = (int, int), uu = (unsigned, unsigned),
    // su = (int, unsigned), us = (unsigned, int). Mutant MM2 exchanges the two extensions of mulsu.
    bool S0 = BuiltinID == DSPIC::BI__builtin_mulss ||
              BuiltinID == DSPIC::BI__builtin_mulsu;
    bool S1 = BuiltinID == DSPIC::BI__builtin_mulss ||
              BuiltinID == DSPIC::BI__builtin_mulus;
    llvm::Type *ResTy = ConvertType(E->getType()); // long: i32 on this target
    Value *A = EmitScalarExpr(E->getArg(0));
    Value *B = EmitScalarExpr(E->getArg(1));
    A = S0 ? Builder.CreateSExt(A, ResTy) : Builder.CreateZExt(A, ResTy);
    B = S1 ? Builder.CreateSExt(B, ResTy) : Builder.CreateZExt(B, ResTy);
    return Builder.CreateMul(A, B);
  }
  case DSPIC::BI__builtin_swap: {
    // The word swap is ISD::BSWAP, which the backend already prints as `swap Wn`.
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::bswap, X->getType()), X);
  }
  case DSPIC::BI__builtin_swap_byte: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_swapb), X);
  }
  case DSPIC::BI__builtin_disi:
  case DSPIC::BI__builtin_pwrsav: {
    // The `I` in the builtin's type string makes a non-constant argument a Sema error before this
    // runs; the RANGE is checked here and refused with a diagnostic at the call, because the
    // encoding has 14 (disi) or 1 (pwrsav) bits for it and a value past that would be silently
    // truncated by the assembler. Mutant MD1 drops this check.
    bool Disi = BuiltinID == DSPIC::BI__builtin_disi;
    llvm::APSInt V = E->getArg(0)->EvaluateKnownConstInt(getContext());
    int64_t N = V.getSExtValue();
    int64_t Max = Disi ? 16383 : 1;
    if (N < 0 || N > Max) {
      CGM.Error(E->getArg(0)->getExprLoc(),
                Disi ? "__builtin_disi: the cycle count must be 0..16383 (disi #lit14)"
                     : "__builtin_pwrsav: the mode must be 0 (Sleep) or 1 (Idle) (pwrsav #lit1)");
      return nullptr;
    }
    Value *C = llvm::ConstantInt::get(Int16Ty, N);
    return Builder.CreateCall(
        CGM.getIntrinsic(Disi ? Intrinsic::dspic_disi : Intrinsic::dspic_pwrsav), C);
  }
  case DSPIC::BI__builtin_clrwdt:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_clrwdt));
  case DSPIC::BI__builtin_nop:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_nop));

  // ── trellis session 118 ───────────────────────────────────────────────────────────────────────
  case DSPIC::BI__builtin_btg:
  case DSPIC::BI__builtin_btg_8:
  case DSPIC::BI__builtin_btg_16:
  case DSPIC::BI__builtin_btg_32: {
    // ⛔ NO INTRINSIC. `*p ^= (1 << n)` is already the IR shape the backend selects `btg` from --
    // measured through this very compiler before the builtin was written: `g16 ^= 1<<3` gives
    // `btg.w _g16,#3`, `g8 ^= 1<<5` gives `btg.b _g8,#5`, `g32 ^= 1L<<20` gives `btg.w _g32+2,#4`
    // and `*p ^= 1` gives `btg [w0],#0`. An intrinsic would have COST that folding.
    // ⚠ OURS EMITS THE WORD FORM ON A WORD PLACE WHERE cc1 NARROWS TO A BYTE (`btg.b _g16,#3`).
    // That is L1f-g's width rule and the promise it realizes (`guarantees.md`'s sixth member,
    // `overlays.md` §3), and it is a DELIBERATE departure, the same one session 87 costed.
    unsigned Width = BuiltinID == DSPIC::BI__builtin_btg_8    ? 8
                     : BuiltinID == DSPIC::BI__builtin_btg_32 ? 32
                                                              : 16;
    llvm::APSInt V = E->getArg(1)->EvaluateKnownConstInt(getContext());
    int64_t N = V.getSExtValue();
    if (N < 0 || N >= (int64_t)Width) {
      // The bit number reaches the instruction's #bit4 field through a mask; a value past the
      // place's width would toggle nothing and be silently accepted. Mutant MB6 drops this.
      CGM.Error(E->getArg(1)->getExprLoc(),
                "__builtin_btg: the bit number is out of range for the pointee's width");
      return nullptr;
    }
    Address P = EmitPointerWithAlignment(E->getArg(0));
    llvm::Type *T = P.getElementType();
    // ⚠ THE ACCESS IS NOT VOLATILE even though three of the four vendor signatures take a
    // `volatile` pointer, and cc1 is the authority: it emits ONE `btg.b _g32+2,#20-16` for the
    // 32-bit case. A volatile access of the whole pointee makes LLVM keep both halves, and the
    // first build of this row emitted `mov.w _g32,w0 ; btg.w _g32+2,#4 ; mov.w w0,_g32` -- three
    // accesses where the builtin means ONE bit toggle. The single `btg` IS the narrow access an
    // SFR wants; widening it would be the opposite of what `volatile` is asked for here.
    Value *Old = Builder.CreateLoad(P);
    Value *Mask = llvm::ConstantInt::get(T, (uint64_t)1 << N);
    // ⚠ THE STORE IS THE RETURN VALUE. Returning nullptr from here is how clang says "not
    // handled", and it comes out as "error: cannot compile this builtin function yet" at the call
    // -- which is what the first build of this row did, on a builtin whose type is `void`.
    return Builder.CreateStore(Builder.CreateXor(Old, Mask), P);
  }
  case DSPIC::BI__builtin_disable_interrupts:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_disable_interrupts));
  case DSPIC::BI__builtin_enable_interrupts:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_enable_interrupts));
  case DSPIC::BI__builtin_software_reset:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_software_reset));
  case DSPIC::BI__builtin_software_breakpoint:
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_software_breakpoint));
  case DSPIC::BI__builtin_repeat_nop: {
    Value *N = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_repeat_nop), N);
  }
  case DSPIC::BI__builtin_ff1l:
  case DSPIC::BI__builtin_ff1l_16:
  case DSPIC::BI__builtin_ff1r:
  case DSPIC::BI__builtin_ff1r_16: {
    // The `_16` spellings are the SAME instruction: cc1 emits `ff1l w1,w0` for both. Mutant MB3
    // makes ff1l select the ff1r instruction.
    bool L = BuiltinID == DSPIC::BI__builtin_ff1l ||
             BuiltinID == DSPIC::BI__builtin_ff1l_16;
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(
        CGM.getIntrinsic(L ? Intrinsic::dspic_ff1l : Intrinsic::dspic_ff1r), X);
  }
  // ── trellis session 122: the eight RELOCATION builtins ──────────────────────────────────────
  case DSPIC::BI__builtin_addr:
  case DSPIC::BI__builtin_addr_low:
  case DSPIC::BI__builtin_addr_high:
  case DSPIC::BI__builtin_dataflashoffset: {
    const char *Name = BuiltinID == DSPIC::BI__builtin_addr        ? "__builtin_addr"
                     : BuiltinID == DSPIC::BI__builtin_addr_low    ? "__builtin_addr_low"
                     : BuiltinID == DSPIC::BI__builtin_addr_high   ? "__builtin_addr_high"
                     :                                               "__builtin_dataflashoffset";
    // (the argument COUNT is checked once, at the head of this function, for every
    // custom-typechecked builtin)
    const Expr *A = E->getArg(0);
    bool IsArray = A->getType()->isArrayType() || A->getType()->isFunctionType();
    if (BuiltinID != DSPIC::BI__builtin_dataflashoffset) {
      // pic30.c:10021-10035: the argument must be ADDR_EXPR of a VAR_DECL -- `&obj` or an array's
      // name. `&arr[3]`, `&s.b`, a function and a runtime pointer are each refused, in one sentence.
      const Expr *S = A->IgnoreParens();
      if (const auto *UO = dyn_cast<UnaryOperator>(S))
        S = UO->getOpcode() == UO_AddrOf ? UO->getSubExpr()->IgnoreParens() : nullptr;
      else if (!S->getType()->isArrayType())
        S = nullptr;
      const auto *DRE = S ? dyn_cast<DeclRefExpr>(S) : nullptr;
      const auto *VD = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
      if (!VD || !VD->hasGlobalStorage()) {
        CGM.Error(A->getExprLoc(), (llvm::Twine("Argument to ") + Name +
                                    " is not the address of an object not in automatic scope; "
                                    "the object must not be qualified with any form of index").str());
        return llvm::PoisonValue::get(ConvertType(E->getType()));
      }
      // pic30.c:10039: addr_high needs a pointer wider than a word -- a `__prog__`, `__eds__` or
      // `__pack_upper_byte` object. Mutant MR4 removes this test.
      if (BuiltinID == DSPIC::BI__builtin_addr_high &&
          getTarget().getPointerWidth(VD->getType().getAddressSpace()) <= 16) {
        CGM.Error(A->getExprLoc(), (llvm::Twine("Argument to ") + Name +
                                    " does not have an address high.").str());
        return llvm::PoisonValue::get(ConvertType(E->getType()));
      }
    }
    Value *Ptr = IsArray ? EmitLValue(A).getPointer(*this) : EmitScalarExpr(A);
    llvm::Type *I16 = Builder.getInt16Ty();
    if (BuiltinID == DSPIC::BI__builtin_addr) {
      // cc1: `mov #addr_lo(_x),w0 ; mov #addr_hi(_x),w1`. Mutant MR1 exchanges the two operators.
      llvm::Type *I32 = Builder.getInt32Ty();
      llvm::StructType *STy = llvm::StructType::get(I16, I16);
      llvm::FunctionType *FT = llvm::FunctionType::get(STy, {Ptr->getType()}, false);
      llvm::InlineAsm *IA = llvm::InlineAsm::get(
          FT, "mov\t#addr_lo(${2:c}),$0\n\tmov\t#addr_hi(${2:c}),$1", "=r,=r,i",
          /*hasSideEffects=*/false);
      Value *Pair = Builder.CreateCall(IA, {Ptr});
      Value *Lo = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 0), I32);
      Value *Hi = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 1), I32);
      return Builder.CreateOr(Builder.CreateShl(Hi, llvm::ConstantInt::get(I32, 16)), Lo);
    }
    const char *Op = BuiltinID == DSPIC::BI__builtin_addr_low    ? "addr_lo"
                   : BuiltinID == DSPIC::BI__builtin_addr_high   ? "addr_hi"
                   :                                               "tbloffset";
    llvm::FunctionType *FT = llvm::FunctionType::get(I16, {Ptr->getType()}, false);
    std::string Tmpl = (llvm::Twine("mov\t#") + Op + "(${1:c}),$0").str();
    return Builder.CreateCall(
        llvm::InlineAsm::get(FT, Tmpl, "=r,i", /*hasSideEffects=*/false), {Ptr});
  }
  case DSPIC::BI__builtin_section_begin:
  case DSPIC::BI__builtin_section_end:
  case DSPIC::BI__builtin_section_size: {
    // pic30.c:9883-9925 and the three section_*_16 insns (pic30-classic.md:50765): the NAME is
    // spliced into `mov.w #.startof.(name),w0 ; mov.w #.startof_hi.(name),w1`, the LINKER's
    // operators. cc1 accepts a string literal, or `&"lit"[k]` -- and IGNORES k (it takes the
    // literal whole). Transcribed because cc1 accepts it; `"lit" + 1` and everything else is refused.
    const Expr *A = E->getArg(0)->IgnoreParenImpCasts();
    const auto *SL = dyn_cast<StringLiteral>(A);
    if (!SL)
      if (const auto *UO = dyn_cast<UnaryOperator>(A); UO && UO->getOpcode() == UO_AddrOf)
        if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParens()))
          SL = dyn_cast<StringLiteral>(ASE->getBase()->IgnoreParenImpCasts());
    if (!SL || !SL->isOrdinary()) {
      CGM.Error(E->getArg(0)->getExprLoc(),
                "__builtin_section_* requires a string literal for the section name");
      return llvm::PoisonValue::get(ConvertType(E->getType()));
    }
    // Mutant MR2 makes section_end say `.sizeof.`.
    const char *Op = BuiltinID == DSPIC::BI__builtin_section_begin ? "startof"
                   : BuiltinID == DSPIC::BI__builtin_section_end   ? "endof"
                   :                                                 "sizeof";
    std::string Sec;
    for (char C : SL->getString()) {   // `$` is the inline-asm operand sigil
      if (C == '$') Sec.push_back('$');
      Sec.push_back(C);
    }
    std::string Tmpl = (llvm::Twine("mov.w\t#.") + Op + ".(" + Sec + "),$0\n\tmov.w\t#." + Op +
                        "_hi.(" + Sec + "),$1").str();
    llvm::Type *I16 = Builder.getInt16Ty();
    llvm::Type *I32 = Builder.getInt32Ty();
    llvm::FunctionType *FT =
        llvm::FunctionType::get(llvm::StructType::get(I16, I16), {}, false);
    Value *Pair = Builder.CreateCall(
        llvm::InlineAsm::get(FT, Tmpl, "=r,=r", /*hasSideEffects=*/false), {});
    Value *Lo = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 0), I32);
    Value *Hi = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 1), I32);
    return Builder.CreateOr(Builder.CreateShl(Hi, llvm::ConstantInt::get(I32, 16)), Lo);
  }
  case DSPIC::BI__builtin_vector_offset: {
    // pic30.c:10771-10795: a string literal ONLY (`&"lit"[0]`, which section_* accepts, is refused
    // here), and the value is a CONSTANT -- the slot of the device's vector record of that name,
    // -1 when the device has none or there is no device. The device is the one -mcpu / -mdfp named
    // (session 119's channel); the reader is the front end's own (clang::dspic, DSPICDevice.h).
    const auto *SL = dyn_cast<StringLiteral>(E->getArg(0)->IgnoreParenImpCasts());
    if (!SL || !SL->isOrdinary()) {
      CGM.Error(E->getArg(0)->getExprLoc(), "__builtin_vector_offset requires a string literal");
      return llvm::PoisonValue::get(ConvertType(E->getType()));
    }
    const TargetOptions &TO = getTarget().getTargetOpts();
    int Slot = clang::dspic::vectorSlot(TO.DFP, StringRef(TO.CPU).upper(), SL->getString());
    return llvm::ConstantInt::get(Builder.getInt32Ty(), (uint64_t)(int64_t)Slot, /*isSigned=*/true);
  }
  case DSPIC::BI__builtin_fbcl_16: {
    // The vendor spells fbcl twice; one instruction, no new intrinsic.
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_fbcl), X);
  }
  case DSPIC::BI__builtin_swap_16: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::bswap, X->getType()), X);
  }
  case DSPIC::BI__builtin_swap_8: {
    Value *X = EmitScalarExpr(E->getArg(0));
    return Builder.CreateCall(CGM.getIntrinsic(Intrinsic::dspic_swapb), X);
  }
  // ── trellis session 121: the nine vendor ADDRESS OPERATORS ──────────────────────────────────
  // Real builtins now (BuiltinsDSPIC.def), lowered to the SAME inline asm the shim (dspic-builtins.h)
  // emitted, so the code is byte-identical: `mov #<op>(${1:c}),$0`, "=r,i". The pointer reaches the
  // asm through an "i" (immediate/symbol) constraint, which is what refuses a runtime pointer --
  // the shim's own refusal ("invalid operand for inline asm constraint 'i'"), one layer down.
  case DSPIC::BI__builtin_tblpage:
  case DSPIC::BI__builtin_tbloffset:
  case DSPIC::BI__builtin_psvpage:
  case DSPIC::BI__builtin_psvoffset:
  case DSPIC::BI__builtin_edspage:
  case DSPIC::BI__builtin_edsoffset:
  case DSPIC::BI__builtin_dmapage:
  case DSPIC::BI__builtin_dmaoffset: {
    const char *Op =
        BuiltinID == DSPIC::BI__builtin_tblpage   ? "tblpage"
      : BuiltinID == DSPIC::BI__builtin_tbloffset ? "tbloffset"
      : BuiltinID == DSPIC::BI__builtin_psvpage   ? "psvpage"
      : BuiltinID == DSPIC::BI__builtin_psvoffset ? "psvoffset"
      : BuiltinID == DSPIC::BI__builtin_edspage   ? "edspage"
      : BuiltinID == DSPIC::BI__builtin_edsoffset ? "edsoffset"
      : BuiltinID == DSPIC::BI__builtin_dmapage   ? "dmapage"
      :                                             "dmaoffset";
    const Expr *A = E->getArg(0);
    // custom-typechecked builtins skip array/function decay, so an array or function
    // argument (bl_fw passes gFlashEepromDataStore, _flashEeprom, gCanMessageBuffers)
    // reaches here as an lvalue; take its address rather than EmitScalarExpr (which
    // asserts on a non-scalar type).
    Value *Ptr = (A->getType()->isArrayType() || A->getType()->isFunctionType())
                     ? EmitLValue(A).getPointer(*this)
                     : EmitScalarExpr(A);
    llvm::Type *I16 = Builder.getInt16Ty();
    llvm::FunctionType *FT = llvm::FunctionType::get(I16, {Ptr->getType()}, false);
    std::string Tmpl = (llvm::Twine("mov\t#") + Op + "(${1:c}),$0").str();
    llvm::InlineAsm *IA =
        llvm::InlineAsm::get(FT, Tmpl, "=r,i", /*hasSideEffects=*/false);
    return Builder.CreateCall(IA, {Ptr});
  }
  case DSPIC::BI__builtin_tbladdress: {
    // The 24-bit program address as an unsigned long: the shim's two-output asm, then (hi<<16)|lo.
    const Expr *A = E->getArg(0);
    // custom-typechecked builtins skip array/function decay, so an array or function
    // argument (bl_fw passes gFlashEepromDataStore, _flashEeprom, gCanMessageBuffers)
    // reaches here as an lvalue; take its address rather than EmitScalarExpr (which
    // asserts on a non-scalar type).
    Value *Ptr = (A->getType()->isArrayType() || A->getType()->isFunctionType())
                     ? EmitLValue(A).getPointer(*this)
                     : EmitScalarExpr(A);
    llvm::Type *I16 = Builder.getInt16Ty();
    llvm::Type *I32 = Builder.getInt32Ty();
    llvm::StructType *STy = llvm::StructType::get(I16, I16);
    llvm::FunctionType *FT = llvm::FunctionType::get(STy, {Ptr->getType()}, false);
    llvm::InlineAsm *IA = llvm::InlineAsm::get(
        FT, "mov\t#tbloffset(${2:c}),$0\n\tmov\t#tblpage(${2:c}),$1", "=r,=r,i",
        /*hasSideEffects=*/false);
    Value *Pair = Builder.CreateCall(IA, {Ptr});
    Value *Lo = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 0), I32);
    Value *Hi = Builder.CreateZExt(Builder.CreateExtractValue(Pair, 1), I32);
    return Builder.CreateOr(Builder.CreateShl(Hi, llvm::ConstantInt::get(I32, 16)), Lo);
  }
  }
}
