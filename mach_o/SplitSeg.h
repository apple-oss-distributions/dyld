/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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


#ifndef mach_o_SplitSeg_h
#define mach_o_SplitSeg_h

#include <span>
#include <stdint.h>

#if BUILDING_MACHO_WRITER
  #include <vector>
  #include <unordered_map>
#endif

#include "Defines.h"
#include "Error.h"

#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

#define DYLD_CACHE_ADJ_V2_POINTER_32                0x01
#define DYLD_CACHE_ADJ_V2_POINTER_64                0x02
#define DYLD_CACHE_ADJ_V2_DELTA_32                  0x03
#define DYLD_CACHE_ADJ_V2_DELTA_64                  0x04
#define DYLD_CACHE_ADJ_V2_ARM64_ADRP                0x05
#define DYLD_CACHE_ADJ_V2_ARM64_OFF12               0x06
#define DYLD_CACHE_ADJ_V2_ARM64_BR26                0x07
#define DYLD_CACHE_ADJ_V2_ARM_MOVW_MOVT             0x08
#define DYLD_CACHE_ADJ_V2_ARM_BR24                  0x09
#define DYLD_CACHE_ADJ_V2_THUMB_MOVW_MOVT           0x0A
#define DYLD_CACHE_ADJ_V2_THUMB_BR22                0x0B
#define DYLD_CACHE_ADJ_V2_IMAGE_OFF_32              0x0C
#define DYLD_CACHE_ADJ_V2_THREADED_POINTER_64       0x0D

namespace mach_o {

/*!
 * @class SplitSegInfo
 *
 * @abstract
 *      Class to encapsulate accessing and building split seg info
 */
class VIS_HIDDEN SplitSegInfo
{
public:
                        // construct from a chunk of LINKEDIT
                        SplitSegInfo(const uint8_t* start, size_t size);

    struct Entry { uint8_t kind; uint64_t fromSectionIndex; uint64_t fromSectionOffset; uint64_t toSectionIndex; uint64_t toSectionOffset; };

    Error   valid() const;
    bool    isV1() const;
    bool    isV2() const;

    Error   forEachReferenceV2(void (^callback)(const Entry& entry, bool& stop)) const;

#if BUILDING_MACHO_WRITER
                        // used build split seg info
                        // Note: entries so not need to be sorted
                        SplitSegInfo(std::span<const Entry> entries);
    static size_t       estimateSplitSegInfoSize(std::span<const Entry> entries);

    std::span<const uint8_t>  bytes() const { return _bytes; }
#endif

    static uint32_t     splitSegInfoSize(bool is64);

private:

    const uint8_t*       _infoStart;
    const uint8_t*       _infoEnd;
#if BUILDING_MACHO_WRITER
    std::vector<uint8_t> _bytes;
    Error                _buildError;
    static const bool    _verbose = false;
#endif
};


} // namespace mach_o

#endif // mach_o_CompactUnwind_h
