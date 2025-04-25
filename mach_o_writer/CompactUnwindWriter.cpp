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

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>

#include <mach-o/compact_unwind_encoding.h>

// mach_o_writer
#include "CompactUnwindWriter.h"

// mach_o
#include "Misc.h"


namespace mach_o {

bool CompactUnwindWriter::encodingMeansUseDwarf(Architecture arch, uint32_t encoding)
{
    if ( arch.usesArm64Instructions() )
        return ((encoding & UNWIND_ARM64_MODE_MASK) == UNWIND_ARM64_MODE_DWARF);
    else if ( arch.usesx86_64Instructions() )
        return ((encoding & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_DWARF);
    assert(0 && "arch not supported for compact unwind");
}

bool CompactUnwindWriter::encodingCannotBeMerged(Architecture arch, uint32_t encoding)
{
    if ( arch.usesx86_64Instructions() )
        return ((encoding & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_STACK_IND);
    return false;
}

// there are two bits in compact unwind that encode which personality function is used
// this function keeps track of which personality functions are used and when their 2-bit index is
void CompactUnwindWriter::updatePersonalityForEntry(WriterUnwindInfo& entry, std::vector<UniquePersonality>& personalities)
{
    if ( (entry.personalityHandle != nullptr) || (entry.personalityOffset != 0) ) {
        std::optional<uint32_t> index;
        for ( const UniquePersonality& personality : personalities ) {
            if ( personality.handle == entry.personalityHandle ) {
                index = &personality - personalities.data();
                break;
            }
            else if ( (personality.handle == 0) && (entry.personalityHandle == 0)
                       && (personality.offset != 0) && (personality.offset == entry.personalityOffset) ) {
                index = &personality - personalities.data();
                break;
            }
        }
        if ( !index.has_value() ) {
            index = personalities.size();
            personalities.push_back({ entry.personalityOffset, entry.personalityHandle });
        }
        // update entry with personality index
        entry.encoding |= ((index.value()+ 1) << (__builtin_ctz(UNWIND_PERSONALITY_MASK)) );
    }
}

void CompactUnwindWriter::compressDuplicates(Architecture arch, std::vector<WriterUnwindInfo>& entries, uint32_t& lsdaCount,
                                       CommonEncodingsMap& commonEncodings, std::vector<UniquePersonality>& personalities)
{
    lsdaCount = 0;
    // build a vector removing entries where next function has same encoding
    WriterUnwindInfo last = { ~0U, ~0U, ~0U, ~0U, nullptr, nullptr, nullptr };
    // encoding frequency to build common encodings
    size_t inEntriesSize = entries.size();
    std::unordered_map<compact_unwind_encoding_t, unsigned int> encodingsUsed;
    std::erase_if(entries, [&](WriterUnwindInfo& entry) {
        this->updatePersonalityForEntry(entry, personalities);
        bool newNeedsDwarf  = encodingMeansUseDwarf(arch, entry.encoding);
        bool cannotBeMerged = encodingCannotBeMerged(arch, entry.encoding);
        bool duplicate      = true;
        // remove entries which have same encoding and personalityPointer as last one
        if ( newNeedsDwarf || (entry.encoding != last.encoding) || (entry.personalityHandle != last.personalityHandle)
            || cannotBeMerged  || (entry.lsdaHandle != nullptr) ) {
            duplicate = false;

            // never put dwarf into common table
            if ( !newNeedsDwarf )
                encodingsUsed[entry.encoding] += 1;
        }
        if ( entry.encoding & UNWIND_HAS_LSDA ) {
            ++lsdaCount;
            assert(entry.lsdaHandle != nullptr);
        }
        last = entry;
        return duplicate;
    });

    using EncodingsAndUsage = std::pair<compact_unwind_encoding_t, unsigned int>;
    // put encodings into a vector and sort them descending by frequency and
    // ascending by the encoding value
    // there's a limited number of unique encodings but many entries so it's
    // faster to use an unordered map for encodings and sort it here
    std::vector<EncodingsAndUsage> encodingsByUsage;
    encodingsByUsage.resize(encodingsUsed.size());
    std::copy(encodingsUsed.begin(), encodingsUsed.end(), encodingsByUsage.begin());
    std::sort(encodingsByUsage.begin(), encodingsByUsage.end(),
            [](const EncodingsAndUsage& l, const EncodingsAndUsage& r) {
                if ( l.second != r.second )
                    return l.second > r.second;
                /* sort by encoding time for same number of usages for deterministic output */
                return l.first < r.first;
            });
    // put the most common encodings into the common table, but at most 127 of them
    uint32_t maxNumCommonEncodings = std::min((uint32_t)encodingsByUsage.size(), 127u);
    for ( uint32_t i = 0; i < maxNumCommonEncodings; ++i ) {
        if ( encodingsByUsage[i].second <= 1 )
            break;
        commonEncodings[encodingsByUsage[i].first] = i;
    }
    if (_verbose) fprintf(stderr, "compressDuplicates() entries.size()=%lu, uniqueEntries.size()=%lu, lsdaCount=%u\n",
                          inEntriesSize, entries.size(), lsdaCount);
    if (_verbose) fprintf(stderr, "compressDuplicates() %lu common encodings found\n", commonEncodings.size());
}

uint8_t CompactUnwindWriter::encodingIndex(uint32_t encoding, const CommonEncodingsMap& commonEncodings, const CommonEncodingsMap& pageSpecificEncodings)
{
    const auto& pos = commonEncodings.find(encoding);
    if ( pos != commonEncodings.end() )
        return pos->second;
    else
        return pageSpecificEncodings.at(encoding);
}

void CompactUnwindWriter::makeRegularSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, uint32_t pageSize,
                                               size_t& curInfosIndex, uint8_t*& pageStart, unwind_info_section_header_lsda_index_entry*& lsdaContent)
{
    const size_t maxEntriesPerPage = (pageSize - sizeof(unwind_info_regular_second_level_page_header))/sizeof(unwind_info_regular_second_level_entry);
    const size_t entriesToAdd      = std::min(maxEntriesPerPage, uniqueInfos.size() - curInfosIndex);

    unwind_info_regular_second_level_page_header* pageHeader = (unwind_info_regular_second_level_page_header*)pageStart;
    pageHeader->kind                = UNWIND_SECOND_LEVEL_REGULAR;
    pageHeader->entryPageOffset     = sizeof(unwind_info_regular_second_level_page_header);
    pageHeader->entryCount          = entriesToAdd;

    unwind_info_regular_second_level_entry* entryArray = (unwind_info_regular_second_level_entry*)((uint8_t*)pageHeader + pageHeader->entryPageOffset);
    for (uint32_t i=0; i < entriesToAdd; ++i) {
        const WriterUnwindInfo& info = uniqueInfos[curInfosIndex + i];
        entryArray[i].functionOffset = info.funcOffset;
        entryArray[i].encoding       = info.encoding;
        uint64_t entrySectionOffset = (uint8_t*)&entryArray[i].functionOffset - (uint8_t*)&_bytes[0];
        this->_imageOffsetFixups.push_back({ info.funcHandle, (uint32_t)entrySectionOffset, false });
        if ( info.encoding & UNWIND_HAS_LSDA ) {
            lsdaContent->functionOffset = info.funcOffset;
            lsdaContent->lsdaOffset     = info.lsdaOffset;
            assert(info.lsdaHandle != nullptr);

            uint64_t sectionOffset = (uint8_t*)&lsdaContent->functionOffset - (uint8_t*)&_bytes[0];
            this->_imageOffsetFixups.push_back({ info.funcHandle, (uint32_t)sectionOffset, false });

            sectionOffset = (uint8_t*)&lsdaContent->lsdaOffset - (uint8_t*)&_bytes[0];
            this->_imageOffsetFixups.push_back({ info.lsdaHandle, (uint32_t)sectionOffset, false });

            ++lsdaContent;
        }
    }

    // update what has been processed
    curInfosIndex += entriesToAdd;
    pageStart     += (pageHeader->entryPageOffset + pageHeader->entryCount *sizeof(unwind_info_regular_second_level_entry));
}

void CompactUnwindWriter::makeCompressedSecondLevelPage(const std::vector<WriterUnwindInfo>& uniqueInfos, const CommonEncodingsMap& commonEncodings,
                                                  uint32_t pageSize, size_t& curInfosIndex, uint8_t*& pageStart, unwind_info_section_header_lsda_index_entry*& lsdaContent)
{
    // first pass calculates how many compressed entries we could fit in this sized page
    // keep adding entries to page until:
    //  1) encoding table plus entry table plus header exceed page size
    //  2) the file offset delta from the first to last function > 24 bits
    //  3) custom encoding index reaches 255
    //  4) run out of uniqueInfos to encode
    CommonEncodingsMap pageSpecificEncodings;
    uint32_t space          = pageSize - sizeof(unwind_info_compressed_second_level_page_header);
    uint32_t entryCount     = 0;
    while ( curInfosIndex + entryCount < uniqueInfos.size() // 4) run out of uniqueInfos to encode
            && space >= sizeof(uint32_t) ) { // 1) enough room to encode a compressed entry
        const WriterUnwindInfo& info = uniqueInfos[curInfosIndex + entryCount];
        if ( commonEncodings.find(info.encoding) == commonEncodings.end() ) {
            if ( pageSpecificEncodings.find(info.encoding) == pageSpecificEncodings.end() ) {
                // 1) enough room for the new encoding and the entry, no point adding the encoding
                // only if there won't be place for the entry
                if ( space < (sizeof(uint32_t) * 2) )
                    break;

                // need to add page specific encoding
                uint32_t nextEncodingIndex = (uint32_t)(commonEncodings.size() + pageSpecificEncodings.size());
                if ( nextEncodingIndex <= 255 ) {
                    pageSpecificEncodings[info.encoding] = nextEncodingIndex;
                    space -= sizeof(uint32_t);
                } else {
                    break; // 3) custom encoding index reaches 255
                }
            }
        }
        // compute function offset
        assert(info.funcOffset >= uniqueInfos[curInfosIndex].funcOffset);
        uint32_t fromOffset = uniqueInfos[curInfosIndex].funcOffset;
        uint32_t targetOffset = info.funcOffset;
        uint32_t funcOffsetWithInPage = targetOffset - fromOffset;
        if ( funcOffsetWithInPage > 0x00FF0000 ) {
            // don't use 0x00FFFFFF because addresses may vary after atoms are laid out again
            break; // 2) the file offset delta from the first to last function > 24 bits
        }

        if ( _arch.usesArm64Instructions() ) {
            // on arm64 there's the 128mb branch distance limit
            // when __text exceeds the limit we insert branch islands at every 124mb interval
            // leaving 4mb available for islands
            // so when start and target functions are located at different 124mb intervals
            // we need to limit their max allowed distance to make sure branch islands don't
            // make the distance between functions exceed the 24-bit limit
            const uint32_t branchIslandDistance = 124*1024*1024;
            const uint32_t branchIslandMaxSize  = 4*1024*1024;
            if ( (fromOffset / branchIslandDistance) != (targetOffset / branchIslandDistance) ) {
                if ( (funcOffsetWithInPage + branchIslandMaxSize ) > 0xFF0000 ) {
                    break; // 2) the file offset delta from the first to last function *might*
                           // exceed 24 bits later after branch islands were added
                }
            }
        }

        ++entryCount;
        space -= sizeof(uint32_t);
    }

    // fallback to regular encoding when eligible compressed entries don't use all the available page space,
    // this isn't the last page and the number of the eligible entries is smaller
    // than the number of regular entries that can be encoded in this page
    if ( space >= minPageSize && (curInfosIndex + entryCount) < uniqueInfos.size() )  {
        const size_t maxEntriesPerPage = (pageSize - sizeof(unwind_info_regular_second_level_page_header))/sizeof(unwind_info_regular_second_level_entry);
        if ( entryCount < maxEntriesPerPage ) {
            makeRegularSecondLevelPage(uniqueInfos, pageSize, curInfosIndex, pageStart, lsdaContent);
            return;
        }
     }

    // second pass fills in page
    unwind_info_compressed_second_level_page_header* pageHeader = (unwind_info_compressed_second_level_page_header*)pageStart;
    pageHeader->kind                = UNWIND_SECOND_LEVEL_COMPRESSED;
    pageHeader->entryPageOffset     = sizeof(unwind_info_compressed_second_level_page_header);
    pageHeader->entryCount          = entryCount;
    pageHeader->encodingsPageOffset = pageHeader->entryPageOffset + entryCount*sizeof(uint32_t);
    pageHeader->encodingsCount      = pageSpecificEncodings.size();
    uint32_t* const entriesArray    = (uint32_t*)((uint8_t*)pageHeader + pageHeader->entryPageOffset);
    uint32_t        firstFuncOffset = uniqueInfos[curInfosIndex].funcOffset;
    const void*     firstFuncHandle = uniqueInfos[curInfosIndex].funcHandle;
    for (uint32_t i=0; i < entryCount; ++i) {
        const WriterUnwindInfo& info  = uniqueInfos[curInfosIndex + i];
        uint32_t          offset      = info.funcOffset - firstFuncOffset;
        uint8_t           eIndex      = encodingIndex(info.encoding, commonEncodings, pageSpecificEncodings);
        entriesArray[i]               = (offset & 0x00FFFFFF) | (eIndex << 24);
        uint64_t sectionOffset        = (uint8_t*)&entriesArray[i] - (uint8_t*)&_bytes[0];
        this->_diff24Fixups.push_back({ info.funcHandle, firstFuncHandle, (uint32_t)sectionOffset });

        if ( info.encoding & UNWIND_HAS_LSDA ) {
            lsdaContent->functionOffset = info.funcOffset;
            lsdaContent->lsdaOffset     = info.lsdaOffset;
            assert(info.lsdaHandle != nullptr);

            sectionOffset = (uint8_t*)&lsdaContent->functionOffset - (uint8_t*)&_bytes[0];
            this->_imageOffsetFixups.push_back({ info.funcHandle, (uint32_t)sectionOffset, false });

            sectionOffset = (uint8_t*)&lsdaContent->lsdaOffset - (uint8_t*)&_bytes[0];
            this->_imageOffsetFixups.push_back({ info.lsdaHandle, (uint32_t)sectionOffset, false });

            ++lsdaContent;
        }
    }
    uint32_t* const encodingsArray      = (uint32_t*)((uint8_t*)pageHeader + pageHeader->encodingsPageOffset);
    uint32_t const  commonEncodingsSize = (uint32_t)commonEncodings.size();
    for (const auto& enc : pageSpecificEncodings) {
        encodingsArray[enc.second - commonEncodingsSize] = enc.first;
    }

    // update what has been processed
    curInfosIndex += entryCount;
    pageStart     += (pageHeader->encodingsPageOffset + pageHeader->encodingsCount *sizeof(uint32_t));
}


//
// FIXME CompactUnwindWriter needs two modes: fast and optimized.
//   Fast uses regular pages every and is easy to size and layout
//   Optimize tries to make the table as small as possible, but that means the size estimation will be expensive
//
size_t CompactUnwindWriter::estimateCompactUnwindTableSize(std::span<const WriterUnwindInfo> unwindInfos)
{
    std::unordered_set<uint32_t> uniqueEncodings;
    unsigned lsdaCount = 0;
    for (const WriterUnwindInfo& entry : unwindInfos) {
        uniqueEncodings.insert(entry.encoding);
        if ( entry.encoding & UNWIND_HAS_LSDA )
            ++lsdaCount;
    }
    //fprintf(stderr, "ext: unwindInfos.size=%lu   uniqueEncodings.size=%lu\n", unwindInfos.size(), uniqueEncodings.size());
    // calculate worst case size where all pages are regular
    return 64 + 20 + unwindInfos.size()*8 + lsdaCount*8 + unwindInfos.size()/32 + uniqueEncodings.size()*4;
}

// Note: unwindInfos must come in sorted by functionOffset
CompactUnwindWriter::CompactUnwindWriter(Architecture arch, std::vector<WriterUnwindInfo> unwindInfos)
    : CompactUnwind(arch, nullptr, 0)
{
    // build new compressed list by removing entries where next function has same encoding
    // put the most common encodings into the common table, but at most 127 of them
    // build up vector of personality functions used, with an index for each
    uint32_t                        lsdaCount;
    CommonEncodingsMap              commonEncodings;
    std::vector<UniquePersonality>  personalities;
    compressDuplicates(arch, unwindInfos, lsdaCount, commonEncodings, personalities);
    // FIXME: need a way to error out if there are more than 3 personality functions used

    // calculate worst case size for all unwind info pages when allocating buffer
    const size_t entriesPerRegularPage = (maxPageSize-sizeof(unwind_info_regular_second_level_page_header))/sizeof(unwind_info_regular_second_level_entry);
    const size_t pageCountUpperBound = ((unwindInfos.size() - 1)/entriesPerRegularPage) + 3;
    _bytes.resize(estimateCompactUnwindTableSize(unwindInfos));

    // fill in section header
    unwind_info_section_header* header = (unwind_info_section_header*)&_bytes[0];
    header->version                           = UNWIND_SECTION_VERSION;
    header->commonEncodingsArraySectionOffset = sizeof(unwind_info_section_header);
    header->commonEncodingsArrayCount         = (uint32_t)commonEncodings.size();
    header->personalityArraySectionOffset     = header->commonEncodingsArraySectionOffset + (uint32_t)(commonEncodings.size()*sizeof(compact_unwind_encoding_t));
    header->personalityArrayCount             = (uint32_t)personalities.size();
    header->indexSectionOffset                = header->personalityArraySectionOffset + (uint32_t)(personalities.size()*sizeof(uint32_t));
    header->indexCount                        = 0;  // fill in after second level pages built

    // fill in commmon encodings
    uint32_t* commonEncodingsArray = (uint32_t*)&_bytes[header->commonEncodingsArraySectionOffset];
    for (const auto& enc : commonEncodings ) {
        assert(enc.second < header->commonEncodingsArrayCount);
        commonEncodingsArray[enc.second] = enc.first;
    }

    // fill in personalities
    uint32_t* personalityArray = (uint32_t*)&_bytes[header->personalityArraySectionOffset];
    for (const auto& p : personalities) {
        size_t index = &p - personalities.data();
        personalityArray[index] = p.offset;

        uint64_t sectionOffset = (uint8_t*)&personalityArray[index] - (uint8_t*)header;
        this->_imageOffsetFixups.push_back({ p.handle, (uint32_t)sectionOffset, false });
    }

    // build second level pages and fill in first level as each is built
    unwind_info_section_header_index_entry*      firstLevelTable    = (unwind_info_section_header_index_entry*)&_bytes[header->indexSectionOffset];
    unwind_info_section_header_lsda_index_entry* lsdaContent        = (unwind_info_section_header_lsda_index_entry*)&_bytes[header->indexSectionOffset+pageCountUpperBound*sizeof(unwind_info_section_header_index_entry)];
    uint8_t*                                     secondLevelContent = (uint8_t*)&lsdaContent[lsdaCount];
    uint8_t* const                               firstSecondContent = secondLevelContent;
    size_t curInfosIndex = 0;
    // reserve approximate buffers for fixup vectors
    this->_imageOffsetFixups.reserve(unwindInfos.size() / 2);
    this->_diff24Fixups.reserve(unwindInfos.size() / 2);

    while (curInfosIndex < unwindInfos.size()) {
        uint64_t sectionOffset = (uint8_t*)&firstLevelTable[header->indexCount].functionOffset - (uint8_t*)header;
        this->_imageOffsetFixups.push_back({ unwindInfos[curInfosIndex].funcHandle,
            (uint32_t)sectionOffset,
            false });

        firstLevelTable[header->indexCount].functionOffset                = unwindInfos[curInfosIndex].funcOffset;
        firstLevelTable[header->indexCount].secondLevelPagesSectionOffset = (uint32_t)(secondLevelContent - &_bytes[0]);
        firstLevelTable[header->indexCount].lsdaIndexArraySectionOffset   = (uint32_t)((uint8_t*)lsdaContent - &_bytes[0]);
        makeCompressedSecondLevelPage(unwindInfos, commonEncodings, maxPageSize, curInfosIndex, secondLevelContent, lsdaContent);

        header->indexCount++;
        // 8-byte align next page
        secondLevelContent = (uint8_t*)(((uintptr_t)secondLevelContent+7) & (-8));
    }
    // add extra top level index to denote the end
    {
        firstLevelTable[header->indexCount].functionOffset                = unwindInfos.back().funcOffset;
        firstLevelTable[header->indexCount].secondLevelPagesSectionOffset = 0;
        firstLevelTable[header->indexCount].lsdaIndexArraySectionOffset   = (uint32_t)(firstSecondContent - &_bytes[0]);

        uint64_t sectionOffset = (uint8_t*)&firstLevelTable[header->indexCount].functionOffset - (uint8_t*)header;
        this->_imageOffsetFixups.push_back({
            unwindInfos.back().funcHandle,
            (uint32_t)sectionOffset,
            true
        });

        header->indexCount++;
    }

    assert(header->indexCount <= pageCountUpperBound && "not enough space reserved for compact unwind first level table");

    // update pointers to the constructed table can be used
    //fprintf(stderr, "est-size=%lu, act-size=%lu, ext2=%lu\n", _bytes.size(), secondLevelContent-&_bytes[0], estimateCompactUnwindTableSize(unwindInfos));
    assert(secondLevelContent <= (_bytes.data() + _bytes.size()));
    _bytes.resize(secondLevelContent-&_bytes[0]);
    _unwindTable     = header;
    _unwindTableSize = _bytes.size();
}




} // namespace mach_o

