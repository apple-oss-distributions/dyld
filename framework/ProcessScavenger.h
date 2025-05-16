/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#ifndef ProcessScavenger_h
#define ProcessScavenger_h

#include <stdint.h>
#include <stdbool.h>
#include <mach/mach_types.h>

#include "Defines.h"

// Allocator cannot be imported into Swift currently, so expose C entry points here. These functions allocate the buffer and it
// is the callers responsibility to free it
#if __cplusplus
extern "C" {
#endif
VIS_HIDDEN bool scavengeProcess(task_read_t task, void** buffer, uint64_t* bufferSize);
VIS_HIDDEN void* scavengeCache(const char* path, uint64_t* bufferSize);
#if __cplusplus
}
#endif
#endif /* ProcessScavenger_h */
