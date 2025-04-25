/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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


//
// included by:
//   ld to convert function variant name(s) to required flag bits
//   dyld to initialize actual flags bits
//
// FUNCTION_VARIANT_X86_64(_reqFlagBitNum, _name, _flagBitsInitialization)
//


//
//  x86_64 processor specific function variants
//
FUNCTION_VARIANT_X86_64( 0, "default",    true  )

// These were added in Spring 2025
FUNCTION_VARIANT_X86_64( 1, "sse41",      (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasSSE4_1)
FUNCTION_VARIANT_X86_64( 2, "fma",        (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasFMA)
FUNCTION_VARIANT_X86_64( 3, "avx",        (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasAVX1_0)
FUNCTION_VARIANT_X86_64( 4, "avx2",       (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasAVX2_0)
FUNCTION_VARIANT_X86_64( 5, "avx512f",    (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasAVX512F)
FUNCTION_VARIANT_X86_64( 6, "avx512bw",   (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasAVX512BW)
FUNCTION_VARIANT_X86_64( 7, "bmi1",       (*(uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasBMI1)
FUNCTION_VARIANT_X86_64( 8, "rosetta",    config.process.isTranslated)
FUNCTION_VARIANT_X86_64( 9, "haswell",    (_COMM_PAGE_CPUFAMILY == CPUFAMILY_INTEL_HASWELL))
FUNCTION_VARIANT_X86_64(10, "ivybridge",  true)
FUNCTION_VARIANT_X86_64(11, "nehalem",    true)

// These were added in Fall 2025



