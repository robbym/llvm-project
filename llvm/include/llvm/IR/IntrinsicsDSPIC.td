//===- IntrinsicsDSPIC.td - Defines dsPIC33 intrinsics ------*- tablegen -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// trellis session 103: the table read/write instructions.
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
}
