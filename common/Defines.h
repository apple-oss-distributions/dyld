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

// Centralize convenience macros and configuration options

#ifndef DYLD_DEFINES_H
#define DYLD_DEFINES_H

#if __has_include(<AppleFeatures/AppleFeatures.h>)
#include <AppleFeatures/AppleFeatures.h>
#endif

#include <TargetConditionals.h>

#ifndef TARGET_OS_EXCLAVEKIT
//TODO: EXCLAVES remove
#define TARGET_OS_EXCLAVEKIT 0
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

#ifndef BUILDING_ALLOCATOR_UNIT_TESTS
#define BUILDING_ALLOCATOR_UNIT_TESTS 0
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

#define VIS_HIDDEN      __attribute__((visibility("hidden")))
#define TRIVIAL_ABI     [[clang::trivial_abi]]
#define NO_DEBUG        [[gnu::nodebug]]
#define ALWAYS_INLINE   __attribute__((always_inline))

#if __x86_64__
    #define DYLD_PAGE_SIZE (4096)
#else
    #define DYLD_PAGE_SIZE (16384)
#endif
#define DYLD_PAGE_MASK  (DYLD_PAGE_SIZE-1)

#define SUPPORT_IMAGE_UNLOADING (BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT)

// Rosetta support is defined by whether or not a platform has librosetta_trap
#define SUPPORT_ROSETTA (__has_include(<Rosetta/Dyld/Traps.h>) && (__x86_64__ || __arm64e__))


#define SUPPORT_PRIVATE_EXTERNS_WORKAROUND (BUILDING_DYLD && TARGET_OS_OSX && __x86_64__)

// The cache builder and associated tests don't support MachOAnalyzer or anything assuming
// VM layout of binaries/caches
#define SUPPORT_VM_LAYOUT (!BUILDING_CACHE_BUILDER && !BUILDING_CACHE_BUILDER_UNIT_TESTS)

// The cache builder either mmap()s output buffers, or vm_allocate()s them.  This tracks which one
#define SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS ( BUILDING_MRM_CACHE_BUILDER || BUILDING_SIM_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS )

#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
// Host tools can only use introspection APIs starting in macOS 12
#define SUPPORT_HOST_INTROSPECTION (__MAC_OS_X_VERSION_MIN_REQUIRED >= 120000)
#else
// Assume introspection is available when building for native targets
#define SUPPORT_HOST_INTROSPECTION (1)
#endif

#define HAS_EXTERNAL_STATE (BUILDING_DYLD)

#define SUPPORT_PREBUILTLOADERS ((BUILDING_DYLD && !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT) || BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_CLOSURE_UTIL)
#define SUPPORT_ON_DISK_PREBUILTLOADERS (SUPPORT_PREBUILTLOADERS && BUILDING_DYLD && !TARGET_OS_OSX)
#define SUPPORT_CREATING_PREMAPPEDLOADERS (BUILDING_DYLD && TARGET_OS_EXCLAVEKIT)

// Controls the creation of the modern atlas based process informtation
#define DYLD_FEATURE_ATLAS_GENERATION (BUILDING_DYLD && !(TARGET_OS_SIMULATOR || TARGET_OS_EXCLAVEKIT))

#define DYLD_FEATURE_COMPACT_INFO_GENERATION (BUILDING_DYLD && !(TARGET_OS_SIMULATOR || TARGET_OS_EXCLAVEKIT))

// Controls whether or to update the legacy all image info
#define DYLD_FEATURE_LEGACY_IMAGE_INFO (BUILDING_DYLD)

// Controls whether or not the breakpoint based notifiers hould be called after image list changes
#define DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS (BUILDING_DYLD)

// Controls whether or not the mach port base remote notifiers should be called when dyld's state changes
#define DYLD_FEATURE_MACH_PORT_NOTIFICATIONS (BUILDING_DYLD && !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT)

// Controls whether legacy data carrying mach message notifiers are active
#define DYLD_FEATURE_LEGACY_MACH_PORT_NOTIFICATIONS (BUILDING_DYLD && !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT)

// Controls whether or not the notifications should be sent through the simulator interfaces to a host dyld
#define DYLD_FEATURE_SIMULATOR_NOTIFICATIONS (BUILDING_DYLD && TARGET_OS_SIMULATOR)

// Controls whether or not
#define DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT (BUILDING_DYLD && TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT)

#if __cplusplus

#if DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS
    static_assert(DYLD_FEATURE_LEGACY_IMAGE_INFO, "DYLD_FEATURE_LEGACY_IMAGE_INFO depends on DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS");
#endif /* DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS */

#if DYLD_FEATURE_SIMULATOR_NOTIFICATIONS
    static_assert(DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS, "DYLD_FEATURE_SIMULATOR_NOTIFICATIONS depends on DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS");
#endif /* DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS */


static_assert((DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT && DYLD_FEATURE_SIMULATOR_NOTIFICATIONS) == 0,
              "A single build cannot both send simulator notifications and host simulator notifications");

#endif // __cplusplus

// Default to a 16MB pool for most uses cases, but we want a smaller 256KB pool when:
// 1. We are building dyld, since most of the time we only allocate 2-3 pages and we can hand pack a bunch of virtual address spacew
// OR
// 2. We are running anything on 32 bit hardware where we have limited address space
#define ALLOCATOR_DEFAULT_POOL_SIZE (128*1024)

// Exclaves does not have access to vm_allocate, but it also use a smaller fixed set of libraries, so dyld can embed a simple page allocator as a replacement
#if TARGET_OS_EXCLAVEKIT
#define DYLD_FEATURE_EMBEDDED_PAGE_ALLOCATOR (1)
#define DYLD_FEATURE_EMBEDDED_PAGE_ALLOCATOR_PAGE_COUNT (34)
#else
#define DYLD_FEATURE_EMBEDDED_PAGE_ALLOCATOR (0)
#endif

#define SUPPORT_CLASSIC_RELOCS (!TARGET_OS_EXCLAVEKIT && (!BUILDING_DYLD || TARGET_OS_OSX) )

#if TARGET_OS_DRIVERKIT || TARGET_OS_EXCLAVEKIT || BUILDING_ALLOCATOR_UNIT_TESTS
    #define ENABLE_CRASH_REPORTER (0)
    #define CRSetCrashLogMessage(x)
    #define CRSetCrashLogMessage2(x)
#else
    #define ENABLE_CRASH_REPORTER (1)
#endif

#define DYLD_FEATURE_USE_INTERNAL_ALLOCATOR (BUILDING_DYLD || BUILDING_ALLOCATOR_UNIT_TESTS)

#define ALLOCATOR_LOGGING_ENABLED (0)
#define ALLOCATOR_MAKE_TRACE (0)

#if defined(DEBUG) && DEBUG
#define contract assert
#define ALLOCATOR_VALIDATION    (0)
#define BTREE_VALIDATION        (0)
#else
#define contract __builtin_assume
#define ALLOCATOR_VALIDATION    (0)
#define BTREE_VALIDATION        (0)
#endif

// note: keep in sync with ProtectedStack.s
#if (BUILDING_DYLD || BUILDING_UNIT_TESTS || BUILDING_ALLOCATOR_UNIT_TESTS) && __arm64e__ && !TARGET_OS_EXCLAVEKIT && !TARGET_OS_SIMULATOR
#define DYLD_FEATURE_USE_HW_TPRO (1)
#else
#define DYLD_FEATURE_USE_HW_TPRO (0)
#endif /* (BUILDING_DYLD || BUILDING_UNIT_TESTS) && __arm64e__ && !TARGET_OS_EXCLAVEKIT && !TARGET_OS_SIMULATOR */

#if (BUILDING_DYLD || BUILDING_UNIT_TESTS || BUILDING_ALLOCATOR_UNIT_TESTS) && !TARGET_OS_EXCLAVEKIT
#define DYLD_FEATURE_MPROTECT_ALLOCATOR (1)
#else
#define DYLD_FEATURE_MPROTECT_ALLOCATOR (0)
#endif

#if DYLD_FEATURE_USE_HW_TPRO
#define TPRO_SECTION(_name, _align) __attribute__((section("__TPRO_CONST," #_name),aligned(_align))) static
#else
#define TPRO_SECTION(_name, _align) static __attribute__((aligned(_align)))
#endif

#define TPRO_SEGMENT(_align) TPRO_SECTION(__data, _align)


#ifndef PATH_MAX
  #define PATH_MAX 1024
#endif

#if TARGET_OS_EXCLAVEKIT
   #define PAGE_SIZE (16384)
   #define trunc_page(x)   ((x) & (~(PAGE_SIZE - 1)))
   #define round_page(x)   trunc_page((x) + (PAGE_SIZE - 1))
   #define bzero(p, s) memset(p, 0, s)
#endif

// INTERNAL_BUILD is set on command line building ld or dyld_info for OS toolchains
#ifndef INTERNAL_BUILD
    #define INTERNAL_BUILD DEBUG
#endif

#endif /* DYLD_DEFINES_H */
