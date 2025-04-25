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


#ifndef mach_o_writer_CompactUnwind_h
#define mach_o_writer_CompactUnwind_h

#include <span>
#include <stdint.h>
#include <vector>
#include <unordered_map>

// mach_o
#include "CompactUnwind.h"
#include "Error.h"
#include "Architecture.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class CompactUnwind
 *
 * @abstract
 *      Abstraction building the `__TEXT,__unwind_info` section
 */
class VIS_HIDDEN CompactUnwindWriter : public CompactUnwind
{
public:
    struct WriterUnwindInfo
    {
        uint32_t    funcOffset;
        uint32_t    encoding;
        uint32_t    lsdaOffset          = 0;
        uint32_t    personalityOffset   = 0;
        const void* funcHandle          = nullptr;
        const void* lsdaHandle          = nullptr;
        const void* personalityHandle   = nullptr;
    };

    // maximum size of a compact unwind page
    constexpr static uint32_t maxPageSize = 0x1000;

    // minimum size of a compact unwind page
    constexpr static uint32_t minPageSize = 128;


    // used build a compact unwind table
    // Note: unwindInfos must be sorted by funcOffset
    CompactUnwindWriter(Architecture, std::vector<WriterUnwindInfo> unwindInfos);

    // raw bytes, used for mocking dummy compact unwind content
    CompactUnwindWriter(std::vector<uint8_t> mockBytes) : CompactUnwind()
    {
        _bytes = std::move(mockBytes);
    }

    std::span<const uint8_t>  bytes() const { return _bytes; }

    struct ImageOffsetFixup
    {
        const void* handle                      = nullptr;
        uint32_t    compactUnwindSectionOffset  = 0;
        bool        includeTargetSizeInAddend   = false;
    };

    struct Diff24Fixup
    {
        const void* targetHandle = nullptr;
        const void* fromTargetHandle = nullptr;
        uint32_t    compactUnwindSectionOffset = 0;
        uint32_t    addend = 0; // TODO: 1 for thumb
    };

    std::span<const ImageOffsetFixup> imageOffsetFixups() const { return _imageOffsetFixups; }
    std::span<const Diff24Fixup>      diff24Fixups() const { return _diff24Fixups; }

    static bool                       encodingMeansUseDwarf(Architecture, uint32_t encoding);

private:
    static size_t       estimateCompactUnwindTableSize(std::span<const WriterUnwindInfo> unwindInfos);
    typedef std::unordered_map<uint32_t, uint32_t> CommonEncodingsMap;
    void                makeCompressedSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, const CommonEncodingsMap& commonEncodings,
                                                      uint32_t pageSize, size_t& curInfosIndex, uint8_t*& pageStart, struct unwind_info_section_header_lsda_index_entry*& lsdaContent);
    void                makeRegularSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, uint32_t pageSize, size_t& curInfosIndex,
                                                   uint8_t*& pageStart, unwind_info_section_header_lsda_index_entry*& lsdaContent);
    uint8_t             encodingIndex(uint32_t encoding, const CommonEncodingsMap& commonEncodings, const CommonEncodingsMap& pageSpecificEncodings);

    static bool         encodingCannotBeMerged(Architecture, uint32_t encoding);
    struct UniquePersonality { uint32_t offset; const void* handle; };
    void                compressDuplicates(Architecture, std::vector<WriterUnwindInfo>& entries, uint32_t& lsdaCount,
                                             CommonEncodingsMap& commonEncodings, std::vector<UniquePersonality>& personalities);
    void                updatePersonalityForEntry(WriterUnwindInfo& entry, std::vector<UniquePersonality>& personalities);

    std::vector<uint8_t>            _bytes;
    std::vector<ImageOffsetFixup>   _imageOffsetFixups;
    std::vector<Diff24Fixup>        _diff24Fixups;
    Error                           _buildError;
    static const bool               _verbose = false;
};


} // namespace mach_o

#endif // mach_o_writer_CompactUnwind_h
