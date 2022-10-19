/*
* Copyright (c) 2019 Apple Inc. All rights reserved.
*
* @APPLE_LICENSE_HEADER_START@
*
* "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
* Reserved.  This file contains Original Code and/or Modifications of
* Original Code as defined in and that are subject to the Apple Public
* Source License Version 1.0 (the 'License').  You may not use this file
* except in compliance with the License.  Please obtain a copy of the
* License at http://www.apple.com/publicsource and read it before using
* this file.
*
* The Original Code and all software distributed under the License are
* distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
* INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
* License for the specific language governing rights and limitations
* under the License."
*
* @APPLE_LICENSE_HEADER_END@
*/

// For now just for compact info, but in the long run we can use this to centralize convenience macros and configuration options

#ifndef DYLD_DEFINES_H
#define DYLD_DEFINES_H

#include <TargetConditionals.h>

#define VIS_HIDDEN      __attribute__((visibility("hidden")))
#define TRIVIAL_ABI     [[clang::trivial_abi]]
#define NO_DEBUG        [[gnu::nodebug]]
#define ALWAYS_INLINE   __attribute__((always_inline))

#define SUPPORT_ROSETTA (TARGET_OS_OSX && __x86_64__)


#define SUPPORT_PRIVATE_EXTERNS_WORKAROUND (BUILDING_DYLD && TARGET_OS_OSX && __x86_64__)

// The cache builder and associated tests don't support MachOAnalyzer or anything assuming
// VM layout of binaries/caches
#define SUPPORT_VM_LAYOUT (!BUILDING_CACHE_BUILDER && !BUILDING_CACHE_BUILDER_UNIT_TESTS)

// The cache builder either mmap()s output buffers, or vm_allocate()s them.  This tracks which one
#define SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS ( BUILDING_MRM_CACHE_BUILDER || BUILDING_SIM_CACHE_BUILDER )

#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
// Host tools can only use introspection APIs starting in macOS 12
#define SUPPORT_HOST_INTROSPECTION (__MAC_OS_X_VERSION_MIN_REQUIRED >= 120000)
#else
// Assume introspection is available when building for native targets
#define SUPPORT_HOST_INTROSPECTION (1)
#endif

// Default to a 16MB pool for most uses cases, but we want a smaller 256KB pool when:
// 1. We are building dyld, since most of the time we only allocate 2-3 pages and we can hand pack a bunch of virtual address spacew
// OR
// 2. We are running anything on 32 bit hardware where we have limited address space
#if BUILDING_DYLD || !__LP64__
#define ALLOCATOR_DEFAULT_POOL_SIZE (256*1024)
#else
#define ALLOCATOR_DEFAULT_POOL_SIZE (16*1024*1024)
#endif

#if TARGET_OS_OSX && defined(__x86_64__)
#define SUPPPORT_PRE_LC_MAIN (1)
#else
#define SUPPPORT_PRE_LC_MAIN (0)
#endif

#ifndef BUILD_FOR_TESTING
#define BUILD_FOR_TESTING 0
#endif

#ifndef BUILDING_UNIT_TESTS
#define BUILDING_UNIT_TESTS 0
#endif

#ifndef BUILDING_DYLD
#define BUILDING_DYLD 0
#endif

#ifndef BUILDING_LIBDYLD
#define BUILDING_LIBDYLD 0
#endif

#ifndef BUILDING_LIBDYLD_INTROSPECTION_STATIC
#define BUILDING_LIBDYLD_INTROSPECTION_STATIC 0
#endif

#ifndef BUILDING_CACHE_BUILDER
#define BUILDING_CACHE_BUILDER 0
#endif

#if !TARGET_OS_DRIVERKIT && (BUILDING_LIBDYLD || BUILDING_DYLD || BUILDING_SHARED_CACHE_EXTRACTOR || BUILDING_SHARED_CACHE_UTIL || BUILDING_LIBDSC)
  #include <CrashReporterClient.h>
#else
  #define CRSetCrashLogMessage(x)
  #define CRSetCrashLogMessage2(x)
#endif

#if defined(DEBUG) && DEBUG
#define contract assert
#else
#define contract __builtin_assume
#endif

#if BUILDING_DYLD
#define CONCURRENT_ALLOCATOR_SUPPORT        (0)
#else
#define CONCURRENT_ALLOCATOR_SUPPORT        (1)
#endif


#endif /* DYLD_DEFINES_H */
