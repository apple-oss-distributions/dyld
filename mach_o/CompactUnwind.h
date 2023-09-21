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


#ifndef mach_o_CompactUnwind_h
#define mach_o_CompactUnwind_h

#include <span>
#include <stdint.h>

#if BUILDING_MACHO_WRITER
  #include <vector>
  #include <unordered_map>
#endif

#include "Defines.h"
#include "Error.h"
#include "Architecture.h"

namespace mach_o {

/*!
 * @class CompactUnwind
 *
 * @abstract
 *      Abstraction `__TEXT,__unwind_info` section
 */
class VIS_HIDDEN CompactUnwind
{
public:
                        // construct from a mach-o __TEXT,__unwind_info section
                        CompactUnwind(Architecture, const uint8_t* start, size_t size);

    struct UnwindInfo { uint32_t funcOffset; uint32_t encoding=0; uint32_t lsdaOffset=0; uint32_t personalityOffset=0; };
    Error               valid() const;
    void                forEachUnwindInfo(void (^callback)(const UnwindInfo&)) const;
    bool                findUnwindInfo(uint32_t funcOffset, UnwindInfo& info) const;
    void                encodingToString(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;

#if BUILDING_MACHO_WRITER
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
                        CompactUnwind(Architecture, std::vector<WriterUnwindInfo> unwindInfos);

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
#endif

    static uint32_t       compactUnwindEntrySize(bool is64);
    static bool           encodingMeansUseDwarf(Architecture, uint32_t encoding);

private:
    Error               forEachFirstLevelTableEntry(void (^callback)(uint32_t funcsStartOffset, uint32_t funcsEndOffset, uint32_t secondLevelOffset, uint32_t lsdaIndexOffset)) const;
    Error               forEachSecondLevelRegularTableEntry(const struct unwind_info_regular_second_level_page_header*, void (^callback)(const UnwindInfo&)) const;
    Error               forEachSecondLevelCompressedTableEntry(const struct unwind_info_compressed_second_level_page_header*, uint32_t pageFunsOffset, void (^callback)(const UnwindInfo&)) const;
    void                encodingToString_arm64(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;
    void                encodingToString_x86_64(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;
    uint32_t            findLSDA(uint32_t funcOffset) const;

#if BUILDING_MACHO_WRITER
    static size_t       estimateCompactUnwindTableSize(std::span<const WriterUnwindInfo> unwindInfos);
    typedef std::unordered_map<uint32_t, uint32_t> CommonEncodingsMap;
    void                makeCompressedSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, const CommonEncodingsMap& commonEncodings,
                                                      uint32_t pageSize, size_t& curInfosIndex, uint8_t*& pageStart, struct unwind_info_section_header_lsda_index_entry*& lsdaContent);
    void                makeRegularSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, uint32_t pageSize, size_t& curInfosIndex,
                                                   uint8_t*& pageStart, unwind_info_section_header_lsda_index_entry*& lsdaContent);
    uint8_t             encodingIndex(uint32_t encoding, const CommonEncodingsMap& commonEncodings, const CommonEncodingsMap& pageSpecificEncodings);

    static bool           encodingCannotBeMerged(Architecture, uint32_t encoding);
    struct UniquePersonality { uint32_t offset; const void* handle; };
    void                  compressDuplicates(Architecture, std::vector<WriterUnwindInfo>& entries, uint32_t& lsdaCount,
                                             CommonEncodingsMap& commonEncodings, std::vector<UniquePersonality>& personalities);
    void                  updatePersonalityForEntry(WriterUnwindInfo& entry, std::vector<UniquePersonality>& personalities);

#endif

    Architecture                                _arch;
    const struct unwind_info_section_header*    _unwindTable      = nullptr;
    size_t                                      _unwindTableSize  = 0;
#if BUILDING_MACHO_WRITER
    std::vector<uint8_t>            _bytes;
    std::vector<ImageOffsetFixup>   _imageOffsetFixups;
    std::vector<Diff24Fixup>        _diff24Fixups;
    Error                           _buildError;
    static const bool               _verbose = false;
#endif
};


} // namespace mach_o

#endif // mach_o_CompactUnwind_h
