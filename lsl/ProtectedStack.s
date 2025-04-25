/*
 * Copyright (c) 1999-2019 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include "Defines.h"

//
// Moves from the current (non-TPRO) stack, to the TPRO-stack given by 'nextStackPtr'.
// Saves the current stack pointer to 'prevStackPtr' so that it can be used later if we need to
// transition back to the regular stack in some nested withReadableMemory block
// Finally calls the callback function once we are on the TPRO stack.
//
// void callWithProtectedStack(void* nextStackPtr, void* __ptrauth_dyld_tpro_stack* prevStackPtr, void (^callback)(void)) __asm("_callWithProtectedStack");
//
#if DYLD_FEATURE_USE_HW_TPRO
    .text
    .align    4
    .globl _callWithProtectedStack
_callWithProtectedStack:
.cfi_startproc
pacibsp
mov       x16, sp
ldr       x8,  [x1]                // load the old value in prevStackPtr
mov       x17, x1
movk      x17, #0x2ebb, lsl #48    // make the key to sign next value in prevStackPtr
pacda     x16, x17                 // sign next value prevStackPtr
str       x16, [x1]                // save next stack value to prevStackPtr
mov       x17, x0
mov       x16, sp
pacdb     x16, x17                 // sign the old sp
sub       x17, x17, #0x30          // subtract space from stack
stp       x1, x8,   [x17, #0x00]   // save prevStackPtr and its old target value
stp       xzr, x16, [x17, #0x10]   // save old sp
stp       x29, x30, [x17, #0x20]   // save fp, lr
.cfi_def_cfa w29, 16
.cfi_offset w30, -8                // lr
.cfi_offset w29, -16               // fp
mov       sp, x17                  // switch to new stack
add       x29, x17, #0x20          // switch to new frame
mov       x0, x2                   // move the block pointer to x0 as blocks requires it for context
add       x2, x0, #16              // get the address of the block pointer to auth later
ldr       x1, [x0, #16]            // load the function pointer from the block
blraa     x1, x2                   // call the function
ldp       x1, x8,   [sp, #0x00]    // load prevStackPtr and its current value when we started this function
ldp       x29, x30, [sp, #0x20]    // restore fp, lr
ldp       xzr, x16, [sp, #0x10]    // load old sp
add       sp, sp, #0x30            // move the stack back up before the auth
autdb     x16, sp                  // auth old sp
mov       sp, x16                  // restore old sp
str       x8, [x1]                 // restore the old value in prevStackPtr
retab
.cfi_endproc

//
// Moves from the current (TPRO) stack, to the non-TPRO-stack given by 'nextStackPtr'.
// Saves the current stack pointer to 'prevStackPtr' so that it can be used later if we need to
// transition back to the regular stack in some nested withWritableMemory block.
// Note the 'prevStackPtr' is saved on the current (TPRO) stack to ensure it cannot be tampered with.
// Finally calls the callback function once we are on the TPRO stack.
//
// void callWithRegularStack(void* nextStackPtr, void* __ptrauth_dyld_tpro_stack* prevStackPtr, void (^callback)(void)) __asm("_callWithRegularStack");
//
.align    4
.globl _callWithRegularStack
_callWithRegularStack:
.cfi_startproc
pacibsp
sub       sp, sp, #0x10            // make space to store the prevStackPtr data in the current TPRO stack
mov       x16, sp
ldr       x8,  [x1]                // load the old value in prevStackPtr
mov       x17, x1
movk      x17, #0x2ebb, lsl #48    // make the key to sign next value in prevStackPtr
pacda     x16, x17                 // sign next value prevStackPtr
str       x16, [x1]                // save next stack value to prevStackPtr
mov       x17, x0
mov       x16, sp
stp       x1, x8,   [sp, #0x00]    // save prevStackPtr and its old target value to the TPRO stack
mov       x16, sp
pacdb     x16, x17                 // sign the old sp
sub       x17, x17, #0x20          // subtract space from stack
stp       xzr, x16, [x17, #0x00]   // save old sp
stp       x29, x30, [x17, #0x10]   // save fp, lr
mov       sp, x17                  // switch to new stack
add       x29, x17, #0x10          // switch to new frame
.cfi_def_cfa w29, 16
.cfi_offset w30, -8                // lr
.cfi_offset w29, -16               // fp
mov       x0, x2                   // move the block pointer to x0 as blocks requires it for context
add       x2, x0, #16              // get the address of the block pointer to auth later
ldr       x1, [x0, #16]            // load the function pointer from the block
blraa     x1, x2                   // call the function
ldp       x29, x30, [sp, #0x10]    // restore fp, lr
ldp       xzr, x16, [sp, #0x00]    // load old sp
add       sp, sp, #0x20            // move the stack back up before the auth
autdb     x16, sp                  // auth old sp
mov       sp, x16                  // restore old sp
ldp       x1, x8,   [sp, #0x00]    // load prevStackPtr and its current value when we started this function
str       x8, [x1]                 // restore the old value in prevStackPtr
add       sp, sp, #0x10            // remove space used for prevStackPtr
retab
.cfi_endproc

#endif // DYLD_FEATURE_USE_HW_TPRO


