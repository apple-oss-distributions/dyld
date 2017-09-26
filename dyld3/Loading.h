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



#ifndef __DYLD_LOADING_H__
#define __DYLD_LOADING_H__

#include <string.h>
#include <stdint.h>
#include <mach/mach.h>
#include <_simple.h>
#include "LaunchCache.h"
#include "LaunchCacheFormat.h"
#include "MachOParser.h"
#include "ClosureBuffer.h"



namespace dyld3 {

ClosureBuffer closured_CreateImageGroup(const ClosureBuffer& input);

namespace loader {

struct ImageInfo
{
    const launch_cache::binary_format::Image*   imageData;
    const mach_header*                          loadAddress;
    uint32_t                                    groupNum;
    uint32_t                                    indexInGroup;
    bool                                        previouslyFixedUp;
    bool                                        justMapped;
    bool                                        justUsedFromDyldCache;
    bool                                        neverUnload;
};


#if DYLD_IN_PROCESS

typedef bool (*LogFunc)(const char*, ...) __attribute__((format(printf, 1, 2)));

void mapAndFixupImages(Diagnostics& diag, launch_cache::DynArray<ImageInfo>& images, const uint8_t* cacheLoadAddress,
                       LogFunc log_loads, LogFunc log_segments, LogFunc log_fixups, LogFunc log_dofs) VIS_HIDDEN;


void unmapImage(const launch_cache::binary_format::Image* image, const mach_header* loadAddress) VIS_HIDDEN;

#if BUILDING_DYLD
bool bootArgsContains(const char* arg) VIS_HIDDEN;
bool internalInstall();
void forEachLineInFile(const char* path, void (^lineHandler)(const char* line, bool& stop));
#endif
#endif

} // namespace loader
} // namespace dyld3


#endif // __DYLD_LOADING_H__


