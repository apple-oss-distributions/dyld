/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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

#ifndef NewAdjustDylibSegments_hpp
#define NewAdjustDylibSegments_hpp

#include "SectionCoalescer.h"
#include "Types.h"

#include <stdint.h>
#include <unordered_map>
#include <string>
#include <vector>

namespace dyld3 {

struct MachOFile;

};

class Diagnostics;

// Represents a segment in a dylib/kext which is going to be moved in to a cache buffer
struct MovedSegment
{
    // Where is this segment in the source file
    InputDylibVMAddress inputVMAddress;
    // TODO: See if we need this.  In theory the inputVMSize might be greater that the cacheVMSize
    // if we remove sections from the segment, eg, deduplicating strings/GOTs/etc
    InputDylibVMSize inputVMSize;

    // Where is this segment in the cache
    uint8_t*        cacheLocation = nullptr;
    CacheVMAddress  cacheVMAddress;
    CacheVMSize     cacheVMSize;
    CacheFileOffset cacheFileOffset;
    CacheFileSize   cacheFileSize;

    // Each segment has its own ASLRTracker
    cache_builder::ASLR_Tracker* aslrTracker = nullptr;
};

// Represents a piece of LINKEDIT in a dylib/kext which is going to be moved in to a cache buffer
struct MovedLinkedit
{
    enum class Kind
    {
        symbolNList,
        symbolStrings,
        indirectSymbols,
        functionStarts,
        dataInCode,
        exportTrie,

        numKinds
    };

    Kind            kind;
    CacheFileOffset dataOffset;
    CacheFileSize   dataSize;
    uint8_t*        cacheLocation = nullptr;
};

struct NListInfo
{
    uint32_t localsStartIndex   = 0;
    uint32_t localsCount        = 0;
    uint32_t globalsStartIndex  = 0;
    uint32_t globalsCount       = 0;
    uint32_t undefsStartIndex   = 0;
    uint32_t undefsCount        = 0;
};

struct DylibSegmentsAdjustor
{
    DylibSegmentsAdjustor(std::vector<MovedSegment>&&                              movedSegments,
                          std::unordered_map<MovedLinkedit::Kind, MovedLinkedit>&& movedLinkedit,
                          NListInfo&                                               nlistInfo);

    // Map from input dylib VMAddr to cache dylib VMAddr
    CacheVMAddress adjustVMAddr(InputDylibVMAddress inputVMAddr) const;

    void adjustDylib(Diagnostics& diag, CacheVMAddress cacheBaseAddress,
                     dyld3::MachOFile* cacheMF, std::string_view dylibID,
                     const uint8_t* chainedFixupsStart, const uint8_t* chainedFixupsEnd,
                     const uint8_t* splitSegInfoStart, const uint8_t* splitSegInfoEnd,
                     const uint8_t* rebaseOpcodesStart, const uint8_t* rebaseOpcodesEnd,
                     const DylibSectionCoalescer* sectionCoalescer);

    const std::vector<MovedSegment>                              movedSegments;
    const std::unordered_map<MovedLinkedit::Kind, MovedLinkedit> movedLinkedit;
    const NListInfo                                              nlistInfo;
};

#endif /* NewAdjustDylibSegments_hpp */
