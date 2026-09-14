//===- IntrinsicsDSPIC.td - Defines dsPIC33 intrinsics ------*- tablegen -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 103: the table read/write instructions.
// trellis session 108: the byte table forms, fbcl, swap.b and the four control instructions.
// ⚠ The widening multiplies (__builtin_mulss/muluu/mulsu/mulus) have NO intrinsic: they are
// ordinary IR multiplies of extended operands, selected by the backend's own lowering.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "dspic" in { // All intrinsics start with "llvm.dspic.".

  // ⛔ EVERY TYPE IS i16 BECAUSE `unsigned int` IS SIXTEEN BITS ON THIS TARGET. Session 99 found
  // three clang builtins in this port handing a 16-bit int to an i32 intrinsic parameter; the
  // symptom is a type mismatch at the call site, and it is this trap rather than a new one.

  // ⛔ AND ALL FOUR CARRY IntrHasSideEffects, WHICH IS A DELIBERATE REFUSAL TO IMPROVE ANYTHING
  // YET. They replace an `__asm__ volatile`, which is neither deletable nor reorderable against
  // other volatile accesses. A table read's page comes from TBLPAG -- a memory-mapped SFR these
  // intrinsics do not name -- so a "pure" read could be hoisted above the `mov.w wN,_TBLPAG` that
  // selects its page, and two reads of the same offset on different pages could be CSE'd into one.
  // Matching the existing semantics exactly is what makes the row's prediction checkable: both
  // firmwares must come out BYTE-IDENTICAL. Relaxing this is a separate row with its own evidence.

  // Read the low 16 bits / the high 8 bits of the program word at TBLPAG:offset.
  def int_dspic_tblrdl : Intrinsic<[llvm_i16_ty], [llvm_i16_ty], [IntrHasSideEffects]>;
  def int_dspic_tblrdh : Intrinsic<[llvm_i16_ty], [llvm_i16_ty], [IntrHasSideEffects]>;

  // Write the flash write latch at TBLPAG:offset. (offset, value), the vendor's argument order.
  def int_dspic_tblwtl : Intrinsic<[], [llvm_i16_ty, llvm_i16_ty], [IntrHasSideEffects]>;
  def int_dspic_tblwth : Intrinsic<[], [llvm_i16_ty, llvm_i16_ty], [IntrHasSideEffects]>;

  // ── trellis session 108 ───────────────────────────────────────────────────────────────────────
  // The BYTE table forms, the same TBLPAG argument for the same side effects. The result and the
  // written value are i8 (`unsigned char` in the vendor's signature).
  def int_dspic_tblrdlb : Intrinsic<[llvm_i8_ty], [llvm_i16_ty], [IntrHasSideEffects]>;
  def int_dspic_tblrdhb : Intrinsic<[llvm_i8_ty], [llvm_i16_ty], [IntrHasSideEffects]>;
  def int_dspic_tblwtlb : Intrinsic<[], [llvm_i16_ty, llvm_i8_ty], [IntrHasSideEffects]>;
  def int_dspic_tblwthb : Intrinsic<[], [llvm_i16_ty, llvm_i8_ty], [IntrHasSideEffects]>;

  // fbcl Ws,Wnd: pure -- a function of its one operand (it also writes C, which no consumer of
  // the intrinsic reads; the instruction def carries Defs = [SR]).
  def int_dspic_fbcl : Intrinsic<[llvm_i16_ty], [llvm_i16_ty], [IntrNoMem, IntrSpeculatable]>;

  // swap.b Wn: the nibbles of the low byte exchanged; pure.
  def int_dspic_swapb : Intrinsic<[llvm_i8_ty], [llvm_i8_ty], [IntrNoMem, IntrSpeculatable]>;

  // The control instructions. disi #lit14 and pwrsav #lit1 take a LITERAL, so the argument is an
  // ImmArg (a TargetConstant in the DAG, matched by `timm`); the range is checked in clang's
  // CGBuiltin before the intrinsic is formed. All four have side effects the optimiser must not
  // see through: a `disi` that moved would protect the wrong region.
  def int_dspic_disi   : Intrinsic<[], [llvm_i16_ty], [IntrHasSideEffects, ImmArg<ArgIndex<0>>]>;
  def int_dspic_pwrsav : Intrinsic<[], [llvm_i16_ty], [IntrHasSideEffects, ImmArg<ArgIndex<0>>]>;
  def int_dspic_clrwdt : Intrinsic<[], [], [IntrHasSideEffects]>;
  def int_dspic_nop    : Intrinsic<[], [], [IntrHasSideEffects]>;

  // ── trellis session 118 ───────────────────────────────────────────────────────────────────────
  // ⚠ NOTHING HERE FOR btg: `__builtin_btg` is `*p ^= (1u << n)` in CGBuiltin and the backend
  // already selects `btg` from it. An intrinsic would have COST capability -- it cannot fold the
  // place into the instruction the way the existing L1f-g patterns do.

  // ff1l / ff1r Ws,Wnd: pure. The hardware result IS the vendor builtin's value (0 when no bit is
  // set, else the shift count starting at 1 -- DS70000157G pp.257/259), so no adjustment is owed.
  // Going through ctlz/cttz instead costs six instructions to cc1's two: this backend's pattern is
  // `DEC16rr (FF1L16 x)` plus a zero test the builtin does not need.
  def int_dspic_ff1l : Intrinsic<[llvm_i16_ty], [llvm_i16_ty], [IntrNoMem, IntrSpeculatable]>;
  def int_dspic_ff1r : Intrinsic<[llvm_i16_ty], [llvm_i16_ty], [IntrNoMem, IntrSpeculatable]>;

  // The global interrupt enable, and the two machine-control instructions. Each carries
  // IntrHasSideEffects for the same reason disi does: a `bclr _INTCON2,#15` that moved would
  // protect the wrong region, and a `reset` that was deleted would not reset.
  def int_dspic_disable_interrupts : Intrinsic<[], [], [IntrHasSideEffects]>;
  def int_dspic_enable_interrupts  : Intrinsic<[], [], [IntrHasSideEffects]>;
  def int_dspic_software_reset     : Intrinsic<[], [], [IntrHasSideEffects]>;
  def int_dspic_software_breakpoint: Intrinsic<[], [], [IntrHasSideEffects]>;

  // `repeat #n ; nop`. The count is NOT an ImmArg: the vendor accepts a variable as well as a
  // literal (`repeat Wn` is a real form), so both are patterns on the same intrinsic.
  def int_dspic_repeat_nop : Intrinsic<[], [llvm_i16_ty], [IntrHasSideEffects]>;
}
