/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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


#ifndef Tracing_h
#define Tracing_h

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <uuid/uuid.h>
#include <mach-o/loader.h>
#include <System/sys/kdebug.h>

#ifndef DBG_DYLD_SIGNPOST
    #define DBG_DYLD_SIGNPOST (6)
#endif

#ifndef DBG_DYLD_TIMING
    #define DBG_DYLD_TIMING (7)
#endif

#ifndef DBG_DYLD_PRINT
    #define DBG_DYLD_PRINT (8)
#endif

#ifndef DBG_DYLD_SIGNPOST_START_DYLD
    #define DBG_DYLD_SIGNPOST_START_DYLD (0)
#endif

#ifndef DBG_DYLD_SIGNPOST_START_MAIN
    #define DBG_DYLD_SIGNPOST_START_MAIN (1)
#endif

#ifndef DBG_DYLD_SIGNPOST_START_MAIN_DYLD2
    #define DBG_DYLD_SIGNPOST_START_MAIN_DYLD2 (2)
#endif

#ifndef DBG_DYLD_TIMING_STATIC_INITIALIZER
    #define DBG_DYLD_TIMING_STATIC_INITIALIZER (0)
#endif

#ifndef DBG_DYLD_PRINT_GENERIC
    #define DBG_DYLD_PRINT_GENERIC (0)
#endif


#define VIS_HIDDEN __attribute__((visibility("hidden")))

namespace dyld3 {

VIS_HIDDEN
void kdebug_trace_dyld_image(const uint32_t code,
                       const uuid_t* uuid_bytes,
                       const fsobj_id_t fsobjid,
                       const fsid_t fsid,
                       const mach_header* load_addr);

VIS_HIDDEN
void kdebug_trace_dyld_signpost(const uint32_t code, uint64_t data1, uint64_t data2);

VIS_HIDDEN
void kdebug_trace_dyld_duration(const uint32_t code, uint64_t data1, uint64_t data2, void (^block)());

VIS_HIDDEN
void kdebug_trace_print(const uint32_t code, const char *string);
}

#endif /* Tracing_h */
