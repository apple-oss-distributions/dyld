/*
 * Copyright (c) 2010-2013 Apple Inc. All rights reserved.
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

#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <System/machine/cpu_capabilities.h>
#endif

// bool save_xxm = (*((uint32_t*)_COMM_PAGE_CPU_CAPABILITIES) & kHasAVX1_0) != 0;

#if __x86_64__

#define RDI_SAVE_RBP			-8
#define RSI_SAVE_RBP			-16
#define RDX_SAVE_RBP			-24
#define RCX_SAVE_RBP			-32
#define RBX_SAVE_RBP			-40
#define R8_SAVE_RBP 			-48
#define R9_SAVE_RBP 			-56
#define R10_SAVE_RBP			-64
#define R11_SAVE_RBP			-72
#define STATIC_STACK_SIZE		256	// extra padding to allow it to be 64-byte aligned

#define XMM0_SAVE_RSP			0x00
#define XMM1_SAVE_RSP			0x10
#define XMM2_SAVE_RSP			0x20
#define XMM3_SAVE_RSP			0x30
#define XMM4_SAVE_RSP			0x40
#define XMM5_SAVE_RSP			0x50
#define XMM6_SAVE_RSP			0x60
#define XMM7_SAVE_RSP			0x70


// Note: dyld cache builder finds __tlv_get_addr as _tlv_bootstrap+8
    .globl          __tlv_bootstrap
#if TARGET_OS_DRIVERKIT
    .private_extern __tlv_bootstrap
#endif
__tlv_bootstrap:
    jmp   __tlv_bootstrap_error
    nop
    nop
    nop

	// returns address of TLV in %rax, all other registers preserved
	.globl          __tlv_get_addr
	.private_extern __tlv_get_addr
    .alt_entry      __tlv_get_addr
__tlv_get_addr:
	movl	8(%rdi),%eax			// get 32-bit key from descriptor (TLV_Thunkv2.key)
	movq	%gs:0x0(,%rax,8),%rax	// get thread value
	testq	%rax,%rax				// if NULL, lazily allocate
	je		LlazyAllocate
    pushq   %rdi
    movl    12(%rdi),%edi           // load 32-bit offset from descriptor (TLV_Thunkv2.offset)
	addq	%rdi,%rax               // add offset to allocation base
    popq    %rdi
	ret
LlazyAllocate:
	pushq		%rbp
	movq		%rsp,%rbp
	subq		$STATIC_STACK_SIZE,%rsp
	movq		%rdi,RDI_SAVE_RBP(%rbp)	# save registers that might be used as parameters
	movq		%rsi,RSI_SAVE_RBP(%rbp)
	movq		%rdx,RDX_SAVE_RBP(%rbp)
	movq		%rcx,RCX_SAVE_RBP(%rbp)
	movq		%rbx,RBX_SAVE_RBP(%rbp)
	movq		%r8,  R8_SAVE_RBP(%rbp)
	movq		%r9,  R9_SAVE_RBP(%rbp)
	movq		%r10,R10_SAVE_RBP(%rbp)
	movq		%r11,R11_SAVE_RBP(%rbp)

	cmpl		$0, _inited(%rip)
	jne			Linited
	movl		$0x01,%eax
	cpuid		# get cpu features to check on xsave instruction support
	andl		$0x08000000,%ecx		# check OSXSAVE bit
	movl		%ecx,_hasXSave(%rip)
	cmpl		$0, %ecx
	jne			LxsaveInfo
	movl		$1, _inited(%rip)
	jmp			Lsse

LxsaveInfo:
	movl		$0x0D,%eax
	movl		$0x00,%ecx
	cpuid		# get xsave parameter info
	movl		%eax,_features_lo32(%rip)
	movl		%edx,_features_hi32(%rip)
	movl		%ecx,_bufferSize32(%rip)
	movl		$1, _inited(%rip)

Linited:
	cmpl		$0, _hasXSave(%rip)
	jne			Lxsave

Lsse:
	subq		$128, %rsp
	movdqa      %xmm0, XMM0_SAVE_RSP(%rsp)
	movdqa      %xmm1, XMM1_SAVE_RSP(%rsp)
	movdqa      %xmm2, XMM2_SAVE_RSP(%rsp)
	movdqa      %xmm3, XMM3_SAVE_RSP(%rsp)
	movdqa      %xmm4, XMM4_SAVE_RSP(%rsp)
	movdqa      %xmm5, XMM5_SAVE_RSP(%rsp)
	movdqa      %xmm6, XMM6_SAVE_RSP(%rsp)
	movdqa      %xmm7, XMM7_SAVE_RSP(%rsp)
	jmp			Lalloc

Lxsave:
	movl		_bufferSize32(%rip),%eax
	movq		%rsp, %rdi
	subq		%rax, %rdi				# stack alloc buffer
	andq		$-64, %rdi				# 64-byte align stack
	movq		%rdi, %rsp
	# xsave requires buffer to be zero'ed out
	movq		$0, %rcx
	movq		%rdi, %r8
	movq		%rdi, %r9
	addq		%rax, %r9
Lz:	movq		%rcx, (%r8)
	addq		$8, %r8
	cmpq		%r8,%r9
	ja			Lz

	movl		_features_lo32(%rip),%eax
	movl		_features_hi32(%rip),%edx
	# call xsave with buffer on stack and eax:edx flag bits
	# note: do not use xsaveopt, it assumes you are using the same
	# buffer as previous xsaves, and this thread is on the same cpu.
	xsave		(%rsp)

Lalloc:
	movq		RDI_SAVE_RBP(%rbp),%rdi
    call        __ZN4dyld20ThreadLocalVariables19instantiateVariableERKNS0_5ThunkE  // ThreadLocalVariables::instantiateVariable(ThreadLocalVariables::Thunk&)

	cmpl		$0, _hasXSave(%rip)
	jne			Lxrstror

	movdqa      XMM0_SAVE_RSP(%rsp),%xmm0
	movdqa      XMM1_SAVE_RSP(%rsp),%xmm1
	movdqa      XMM2_SAVE_RSP(%rsp),%xmm2
	movdqa      XMM3_SAVE_RSP(%rsp),%xmm3
	movdqa      XMM4_SAVE_RSP(%rsp),%xmm4
	movdqa      XMM5_SAVE_RSP(%rsp),%xmm5
	movdqa      XMM6_SAVE_RSP(%rsp),%xmm6
	movdqa      XMM7_SAVE_RSP(%rsp),%xmm7
	jmp			Ldone

Lxrstror:
	movq		%rax,%r11
	movl		_features_lo32(%rip),%eax
	movl		_features_hi32(%rip),%edx
	# call xsave with buffer on stack and eax:edx flag bits
	xrstor		(%rsp)
	movq		%r11,%rax

Ldone:
	movq		RDI_SAVE_RBP(%rbp),%rdi
    movl        12(%rdi),%esi               // load 32-bit offset from descriptor (TLV_Thunkv2.offset)
    addq        %rsi,%rax                   // add offset to allocation base
	movq		RSI_SAVE_RBP(%rbp),%rsi
	movq		RDX_SAVE_RBP(%rbp),%rdx
	movq		RCX_SAVE_RBP(%rbp),%rcx
	movq		RBX_SAVE_RBP(%rbp),%rbx
	movq		R8_SAVE_RBP(%rbp),%r8
	movq		R9_SAVE_RBP(%rbp),%r9
	movq		R10_SAVE_RBP(%rbp),%r10
	movq		R11_SAVE_RBP(%rbp),%r11
	movq		%rbp,%rsp
	popq		%rbp
	ret

	.data
# Cached info from cpuid.
_inited:			.long 0
_features_lo32:		.long 0
_features_hi32:		.long 0
_bufferSize32:		.long 0
_hasXSave:			.long 0

#endif // __x86_64__

#if __arm64__

    // Note: dyld cache builder finds __tlv_get_addr as _tlv_bootstrap+8
    .globl          __tlv_bootstrap
#if TARGET_OS_DRIVERKIT
    .private_extern __tlv_bootstrap
#endif
__tlv_bootstrap:
    b   __tlv_bootstrap_error
    nop

	// Parameters: X0 = descriptor
	// Result:  X0 = address of TLV
	// Note: all registers except X0, x16, and x17 are preserved
	.align 2
    .globl          __tlv_get_addr
    .private_extern __tlv_get_addr
    .alt_entry      __tlv_get_addr
__tlv_get_addr:
#if __LP64__
	ldr		w16, [x0, #8]			// get key from descriptor (TLV_Thunkv2.key)
#else
	ldrh    w16, [x0, #4]			// get key from descriptor (TLV_Thunkv2_32.key)
#endif
#if !TARGET_OS_EXCLAVEKIT
	mrs		x17, TPIDRRO_EL0
	and		x17, x17, #-8			// clear low 3 bits???
#if __LP64__
	ldr		x17, [x17, x16, lsl #3]	// get thread allocation address for this key
#else
	ldr		w17, [x17, x16, lsl #2]	// get thread allocation address for this key
#endif
	cbz		x17, LlazyAllocate		// if NULL, lazily allocate
#if __LP64__
	ldr		w16, [x0, #12]			// get offset from descriptor (TLV_Thunkv2.offset)
#else
	ldrh 	w16, [x0, #6]			// get offset from descriptor (TLV_Thunkv2_32.offset)
#endif
	add		x0, x17, x16			// return allocation+offset
	ret		lr
#endif // !TARGET_OS_EXCLAVEKIT

LlazyAllocate:
#if __has_feature(ptrauth_returns)
	pacibsp
#endif
	stp		fp, lr, [sp, #-16]!
	mov		fp, sp
	sub		sp, sp, #288
	stp		x1, x2, [sp, #-16]!		// save all registers that C function might trash
	stp		x3, x4, [sp, #-16]!
	stp		x5, x6, [sp, #-16]!
	stp		x7, x8, [sp, #-16]!
	stp		x9, x10,  [sp, #-16]!
	stp		x11, x12, [sp, #-16]!
	stp		x13, x14, [sp, #-16]!
	stp		x15, x16, [sp, #-16]!
	stp		q0,  q1,  [sp, #-32]!
	stp		q2,  q3,  [sp, #-32]!
	stp		q4,  q5,  [sp, #-32]!
	stp		q6,  q7,  [sp, #-32]!
	stp		x0, x17,  [sp, #-16]!	// save descriptor

	bl		__ZN4dyld20ThreadLocalVariables19instantiateVariableERKNS0_5ThunkE  // ThreadLocalVariables::instantiateVariable(ThreadLocalVariables::Thunk&)
	ldp		x16, x17, [sp], #16		// pop descriptor
#if __LP64__
	ldr		w16, [x16, #12]			// get offset from descriptor (TLV_Thunkv2.offset)
#else
	ldrh	w16, [x16, #6]			// get offset from descriptor (TLV_Thunkv2_32.offset)
#endif
	add		x0, x0, x16				// return allocation+offset

	ldp		q6,  q7,  [sp], #32
	ldp		q4,  q5,  [sp], #32
	ldp		q2,  q3,  [sp], #32
	ldp		q0,  q1,  [sp], #32
	ldp		x15, x16, [sp], #16
	ldp		x13, x14, [sp], #16
	ldp		x11, x12, [sp], #16
	ldp		x9, x10,  [sp], #16
	ldp		x7, x8, [sp], #16
	ldp		x5, x6, [sp], #16
	ldp		x3, x4, [sp], #16
	ldp		x1, x2, [sp], #16

	mov		sp, fp
	ldp		fp, lr, [sp], #16
#if __has_feature(ptrauth_returns)
	retab
#else
	ret
#endif

#endif // __arm64__

#if !TARGET_OS_EXCLAVEKIT
     // dyld_stub_binder is no longer used, but needed by old binaries to link
    .align 4
    .globl dyld_stub_binder
dyld_stub_binder:
#if __x86_64__
    jmp _abort
#else
    b   _abort
#endif
#endif

    // debug builds of libc++ headers generate calls to libcpp_verbose_abort()
#if DEBUG
    .private_extern __ZNSt3__122__libcpp_verbose_abortEPKcz
__ZNSt3__122__libcpp_verbose_abortEPKcz:
#if __x86_64__
    jmp _abort_report_np
#else
    b   _abort_report_np
#endif
#endif // DEBUG


	.subsections_via_symbols
	

