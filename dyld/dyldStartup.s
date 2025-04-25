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

/*
 * When starting a dynamic process, the kernel maps the main executable and dyld.
 * The kernel the starts the process with program counter set to __dyld_start
 * in dyld with the stack looking like:
 *
 *      | STRING AREA |
 *      +-------------+
 *      |      0      |
 *      +-------------+
 *      |  apple[n]   |
 *      +-------------+
 *             :
 *      +-------------+
 *      |  apple[0]   |
 *      +-------------+
 *      |      0      |
 *      +-------------+
 *      |    env[n]   |
 *      +-------------+
 *             :
 *             :
 *      +-------------+
 *      |    env[0]   |
 *      +-------------+
 *      |      0      |
 *      +-------------+
 *      | arg[argc-1] |
 *      +-------------+
 *             :
 *             :
 *      +-------------+
 *      |    arg[0]   |
 *      +-------------+
 *      |     argc    |
 *      +-------------+
 * sp-> |      mh     | address of where the main program's mach_header (TEXT segment) is loaded
 *      +-------------+
 *
 *    Where arg[i] and env[i] point into the STRING AREA
 */


#include <TargetConditionals.h>

//
// This assembly code just needs to align the stack and jump into the C code for:
//      dyld::start(dyld4::KernelArgs*)
//
// For ExclaveKit, the startup code is defined in crt0_dyld.S provided by ExclavePlatform
//
#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT
    .text
    .align    4
    .globl __dyld_start
__dyld_start:
#if __x86_64__
    movq    %rsp,%rdi       # save pointer to KernelArgs
    andq    $-16,%rsp       # force SSE alignment
    movq    $0,%rbp         # terminate frame pointer chain
    pushq   $0              # simulate a call with a return address of zero
    jmp     start
#elif __arm64__
    mov    x0, sp               // get pointer to KernelArgs into parameter register
    and    sp, x0, #~15         // force 16-byte alignment of stack
    mov    fp, #0               // first frame
    mov    lr, #0               // no return address
    b      start
#else
#error "Unknown architecture"
#endif

// support for old macOS x86_64 binaries (pre LC_MAIN)
#if __x86_64__
    .globl __ZN5dyld412gotoAppStartEmPKNS_10KernelArgsE
__ZN5dyld412gotoAppStartEmPKNS_10KernelArgsE:
    // extern void gotoAppStart(uintptr_t start, const KernelArgs* kernArgs) __attribute__((__noreturn__));
    movq    %rsi, %rsp      # reset stack to what it was on entry from kernel
    addq    $8,%rsp         # remove mach_header of main executable (dyld expects it but app does not)
    jmp     *%rdi           # jump to app's start
#endif

// switch to dyld in the dyld cache
// Note: all archs pass parameters in registers.  dyldOnDisk is in second param register and flows through to start()
    .text
    .globl __ZN5dyld422restartWithDyldInCacheEPKNS_10KernelArgsEPKN6mach_o6HeaderEPK15DyldSharedCachePv
__ZN5dyld422restartWithDyldInCacheEPKNS_10KernelArgsEPKN6mach_o6HeaderEPK15DyldSharedCachePv:
    // void restartWithDyldInCache(const KernelArgs* kernArgs, const Header* dyldOnDisk, void* dyldStart);
#if __x86_64__
    movq    %rdi, %rsp      # reset SP to original kernel args
    jmp     *%rcx           # jump into dyld in cache
#elif __arm64__
    mov     sp, x0          // reset SP to original kernel args
    br      x3
#else
#error "Unknown architecture"
#endif


#endif // !TARGET_OS_SIMULATOR


