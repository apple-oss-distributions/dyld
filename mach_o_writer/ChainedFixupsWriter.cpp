/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "Algorithm.h"
#include "Array.h"

// mach_o
#include "Misc.h"
#include "Image.h"

// mach_o_writer
#include "ChainedFixupsWriter.h"

using dyld3::Array;

namespace mach_o {

//
// MARK: --- ChainedFixupsWriter inspection methods ---
//

const uint8_t* ChainedFixupsWriter::bytes(size_t& size) const
{
    size = _fixupsSize;
    return _bytes.data();
}

Error ChainedFixupsWriter::valid(uint64_t preferredLoadAddress, std::span<const MappedSegment> segments,
                           bool startsInSection) const
{
    if ( _buildError.hasError() )
        return Error("%s", _buildError.message());

    return ChainedFixups::valid(preferredLoadAddress, segments, startsInSection);
}

template <typename T>
static T align8(T value)
{
    return (value + 7) & (-8);
}

template <typename T>
static T align4(T value)
{
    return (value + 3) & (-4);
}

size_t ChainedFixupsWriter::linkeditSize(std::span<const Fixup::BindTarget> bindTargets,
                                         std::span<const SegmentFixupsInfo> segments,
                                         const PointerFormat& pointerFormat, uint32_t pageSize)
{
    // scan binds to figure out which imports table format to use
    uint16_t  imFormat;
    size_t    stringPoolSize;
    if ( Error err = importsFormat(bindTargets, imFormat, stringPoolSize) )
        return 0;

    // allocate space in _bytes for full dyld_chained_fixups data structure
    size_t maxBytesNeeded = align8(sizeof(dyld_chained_fixups_header));
    maxBytesNeeded += offsetof(dyld_chained_starts_in_image,seg_info_offset[segments.size()]);
    for ( const SegmentFixupsInfo& segment : segments ) {
        const MappedSegment& seg = segment.mappedSegment;
        uint32_t extras = segment.numPageExtras;
        std::span<const Fixup> segFixups = segment.fixups;
        if ( seg.writable && (seg.runtimeSize != 0) && !segFixups.empty() ) {
            uint64_t lastFixupSegmentOffset = (uint64_t)segFixups.back().location - (uint64_t)seg.content;
            uint64_t lastFixupPage = (lastFixupSegmentOffset / pageSize) + 1;
            maxBytesNeeded = align8(maxBytesNeeded);
            maxBytesNeeded += offsetof(dyld_chained_starts_in_segment, page_start[lastFixupPage + extras]);
        }
        else if ( pointerFormat.value() == DYLD_CHAINED_PTR_ARM64E_SEGMENTED ) {
            // this format requires an entry for every segment (to get seg's base address)
            maxBytesNeeded = align8(maxBytesNeeded);
            maxBytesNeeded += offsetof(dyld_chained_starts_in_segment, page_start[1]);
        }
    }

    if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND64 ) {
        maxBytesNeeded = align8(maxBytesNeeded);
        maxBytesNeeded += sizeof(dyld_chained_import_addend64) * bindTargets.size();
    } else if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND ) {
        maxBytesNeeded = align4(maxBytesNeeded);
        maxBytesNeeded += sizeof(dyld_chained_import_addend) * bindTargets.size();
    } else {
        maxBytesNeeded = align4(maxBytesNeeded);
        maxBytesNeeded += sizeof(dyld_chained_import) * bindTargets.size();
    }

    maxBytesNeeded += stringPoolSize;
    return align8(maxBytesNeeded);
}

size_t ChainedFixupsWriter::startsSectionSize( std::span<const SegmentFixupsInfo> segments, const PointerFormat& pointerFormat)
{
    uint64_t expectedDelta = 0;
    switch ( pointerFormat.value() ) {
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
            expectedDelta = 0x1FFF;
            break;
        case DYLD_CHAINED_PTR_64_OFFSET:
        case DYLD_CHAINED_PTR_ARM64E_SEGMENTED:
            expectedDelta = 0x3FFF;
            break;
        default:
            assert(false && "unknown pointer format for chain starts");
            break;
    }
    // allocate space in _bytes for full dyld_chained_starts_offsets data structure
    size_t chainsCount = 0;
    for (const SegmentFixupsInfo& segment : segments) {
        const MappedSegment& seg = segment.mappedSegment;
        std::span<const Fixup> segFixups = segment.fixups;
        if ( !seg.writable || seg.runtimeSize == 0 || segFixups.empty() )
            continue;
        //fprintf(stderr, "seg: %s\n", seg.segName.data());
        size_t segChains = 1;
        uint64_t lastFixupLoc = (uint64_t)segFixups.front().location;
        for (const Fixup& fixup : segFixups) {
            uint64_t curFixupLoc = (uint64_t)fixup.location;
            uint64_t delta = curFixupLoc - lastFixupLoc;
            if ( delta >= expectedDelta )
                segChains++;
            lastFixupLoc = curFixupLoc;
            //fprintf(stderr, "fixup: 0x%llx\n", (uint64_t)fixup.location);
        }
        chainsCount += segChains;
    }
    size_t maxBytesNeeded = offsetof(dyld_chained_starts_offsets,chain_starts[chainsCount]);
    maxBytesNeeded = align8(maxBytesNeeded);
    return maxBytesNeeded;
}

void ChainedFixupsWriter::calculateSegmentPageExtras(std::span<SegmentFixupsInfo> segments,
                                                     const PointerFormat& pointerFormat,
                                                     uint32_t pageSize)
{
    for ( SegmentFixupsInfo& segmentFixupInfo : segments ) {
        const MappedSegment& segment = segmentFixupInfo.mappedSegment;
        const std::span<const Fixup> fixupsInSegment = segmentFixupInfo.fixups;
        uint32_t numExtras = 0;

        // skip segments with no fixups
        if ( !segment.writable || (segment.runtimeSize == 0) || fixupsInSegment.empty() )
            continue;

        int             curPageIndex    = -1;
        const Fixup*    prevFixup       = nullptr;
        bool            pageHasExtras   = false;
        for ( const Fixup& fixup : fixupsInSegment ) {
            uint64_t offset = (uint8_t*)fixup.location - (uint8_t*)segment.content;
            int pageIndex = (int)(offset/pageSize);
            if ( pageIndex != curPageIndex ) {
                curPageIndex = pageIndex;
                prevFixup = nullptr;
                pageHasExtras = false;
            }
            if ( prevFixup != nullptr ) {
                intptr_t delta = (uint8_t*)fixup.location - (uint8_t*)(prevFixup->location);
                if ( delta > pointerFormat.maxNext() ) {
                    // prev/next are too far apart for chain to span, instead terminate chain at prevFixup
                    // then start new overflow chain
                    if ( !pageHasExtras ) {
                        // A page with extras needs a start and end of the chain too
                        numExtras += 2;
                        pageHasExtras = true;
                    }
                    ++numExtras;
                }
            }
            prevFixup = &fixup;
        }

        segmentFixupInfo.numPageExtras = numExtras;
    }
}

Error ChainedFixupsWriter::importsFormat(std::span<const Fixup::BindTarget> bindTargets, uint16_t& importsFormat, size_t& stringPoolSize)
{
    importsFormat  = DYLD_CHAINED_IMPORT;
    stringPoolSize = 0;

    bool     hasLargeOrdinal = false;
    bool     has32bitAddend  = false;
    bool     has64bitAddend  = false;
    stringPoolSize = 1;
    for (const Fixup::BindTarget& bind : bindTargets) {
        stringPoolSize += (bind.symbolName.size() + 1);
        if ( bind.libOrdinal < -15 ) {
            // TODO: currently only -1, -2, and -3 have meaning.  Should we error here for < -3 ?
            return Error("special libOrdinal (%d) too small", bind.libOrdinal);
        }
        if ( bind.libOrdinal > 240 ) {
            hasLargeOrdinal = true;
            if ( bind.libOrdinal > 65520 ) {
                return Error("libOrdinal (%d) too large", bind.libOrdinal);
            }
        }
        if ( bind.addend != 0 ) {
            int32_t addend32 = (int32_t)bind.addend;
            if ( (int64_t)addend32 == bind.addend )
                has32bitAddend = true;
            else
                has64bitAddend = true;
        }
    }
    bool hasLargeStringOffsets = dyld_chained_import{.name_offset=(uint32_t)stringPoolSize}.name_offset != stringPoolSize;

    if ( hasLargeStringOffsets || has64bitAddend || hasLargeOrdinal )
        importsFormat = DYLD_CHAINED_IMPORT_ADDEND64;
    else if ( has32bitAddend )
        importsFormat = DYLD_CHAINED_IMPORT_ADDEND;
    else
        importsFormat = DYLD_CHAINED_IMPORT;

    if ( stringPoolSize > 0xFFFFFFFF )
        return Error("imports string pool > 4GB");

    return Error::none();
}

ChainedFixupsWriter::ChainedFixupsWriter(std::span<const Fixup::BindTarget> bindTargets,
                                         std::span<const Fixup> fixups,
                                         std::span<const MappedSegment> segments,
                                         uint64_t preferredLoadAddress,
                                         const PointerFormat& pointerFormat, uint32_t pageSize,
                                         bool setDataChains,
                                         bool startsInSection, bool useFileOffsets)
    : ChainedFixups()
{
    std::vector<std::vector<Fixup>> fixupsInSegments;
    fixupsInSegments.resize(segments.size());

    {
        // unify and sort fixups to make chains
        std::vector<Fixup> sortedFixups(fixups.begin(), fixups.end());
        std::sort(sortedFixups.begin(), sortedFixups.end());

        // verify there are no locations with multiple fixups
        if ( sortedFixups.size() > 1 ) {
            Fixup lastLoc = sortedFixups.back();
            for (const Fixup& f : sortedFixups) {
                if ( f.location == lastLoc.location ) {
                    _buildError = Error("multiple fixups at same location in %.*s at offset=0x%lX",
                                        (int)f.segment->segName.size(), f.segment->segName.data(), (uint8_t*)f.location - (uint8_t*)(f.segment->content));
                    return;
                }
                lastLoc = f;
            }
        }

        for ( const Fixup fixup : sortedFixups) {
            uint64_t segmentIndex = fixup.segment - &segments.front();
            fixupsInSegments[segmentIndex].push_back(fixup);
        }
    }

    std::vector<SegmentFixupsInfo> segmentFixupInfos;
    for ( uint32_t segIndex = 0; segIndex != segments.size(); ++segIndex ) {
        segmentFixupInfos.push_back({ segments[segIndex], fixupsInSegments[segIndex], 0 });
    }

    calculateSegmentPageExtras(segmentFixupInfos, pointerFormat, pageSize);

    if ( startsInSection ) {
        buildStartsSectionFixups(segmentFixupInfos, pointerFormat, useFileOffsets, preferredLoadAddress);
    } else {
        buildLinkeditFixups(bindTargets, segmentFixupInfos, preferredLoadAddress, pointerFormat, pageSize, setDataChains);
    }

}


ChainedFixupsWriter::ChainedFixupsWriter(std::span<const Fixup::BindTarget> bindTargets,
                                         std::span<const SegmentFixupsInfo> segments,
                                         uint64_t preferredLoadAddress,
                                         const PointerFormat& pointerFormat, uint32_t pageSize,
                                         bool setDataChains,
                                         bool startsInSection, bool useFileOffsets)
{
    if ( startsInSection ) {
        buildStartsSectionFixups(segments, pointerFormat, useFileOffsets, preferredLoadAddress);
    } else {
        buildLinkeditFixups(bindTargets, segments, preferredLoadAddress, pointerFormat, pageSize, setDataChains);
    }
}

template<typename T, typename U>
void atomic_min(std::atomic<T>& location, U value, const T defaultValue = nullptr) {
    // If we manage to swap with the default value, then no other thread had set the value, and we're done
    T expected = defaultValue;
    while ( !location.compare_exchange_weak(expected, value, std::memory_order::release, std::memory_order_relaxed) ) {
        // Value change before the store, if new value is smaller (but not null) then there's no need to store
        if ( expected != defaultValue && expected <= value )
            break;
    }
}

template<typename T, typename U>
void atomic_max(std::atomic<T>& location, U value) {
    // If we manage to swap with nullptr, then no other thread had set the value, and we're done
    T expected = nullptr;
    while ( !location.compare_exchange_weak(expected, value, std::memory_order::release, std::memory_order_relaxed) ) {
        // Value change before the store, if new value is larger then there's no need to store
        if ( expected >= value )
            break;
    }
}


void ChainedFixupsWriter::buildLinkeditFixups(std::span<const Fixup::BindTarget> bindTargets,
                                              std::span<const SegmentFixupsInfo> segments,
                                              uint64_t preferredLoadAddress,
                                              const PointerFormat& pointerFormat, uint32_t pageSize,
                                              bool setDataChains)
{
    // scan binds to figure out which imports table format to use
    uint16_t  imFormat;
    size_t    stringPoolSize;
    _buildError = importsFormat(bindTargets, imFormat, stringPoolSize);
    if ( _buildError.hasError() )
        return;


    // build imports table
    std::vector<char>                         stringPool;
    size_t                                    importsTableSize = 0;
    const void*                               importsTableStart = nullptr;
    std::vector<dyld_chained_import>          imports ;
    std::vector<dyld_chained_import_addend>   importsAddend;
    std::vector<dyld_chained_import_addend64> importsAddend64;
    stringPool.reserve(stringPoolSize);
    stringPool.push_back('\0'); // so that zero is never a legal string offset
    if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND64 ) {
        importsAddend64.reserve(bindTargets.size());
        for (const Fixup::BindTarget& bind : bindTargets) {
            importsAddend64.push_back({(uint16_t)bind.libOrdinal, bind.weakImport, 0, addSymbolString(bind.symbolName, stringPool), (uint64_t)bind.addend});
        }
        importsTableSize = sizeof(dyld_chained_import_addend64) * importsAddend64.size();
        if ( !importsAddend64.empty() )
            importsTableStart = &importsAddend64[0];
    }
    else if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND ) {
        importsAddend.reserve(bindTargets.size());
        for (const Fixup::BindTarget& bind : bindTargets) {
            importsAddend.push_back({(uint8_t)bind.libOrdinal, bind.weakImport, addSymbolString(bind.symbolName, stringPool), (int32_t)bind.addend});
        }
        importsTableSize = sizeof(dyld_chained_import_addend) * importsAddend.size();
        if ( !importsAddend.empty() )
            importsTableStart = &importsAddend[0];
    }
    else {
        // can use most compact imports encoding
        imports.reserve(bindTargets.size());
        for (const Fixup::BindTarget& bind : bindTargets) {
            imports.push_back({(uint8_t)bind.libOrdinal, bind.weakImport, addSymbolString(bind.symbolName, stringPool)});
        }
        importsTableSize = sizeof(dyld_chained_import) * imports.size();
        if ( !imports.empty() )
            importsTableStart = &imports[0];
    }

    // for 32-bit archs, compute max valid pointer value
    uint64_t maxValidPointer = 0;
    if ( !pointerFormat.is64() ) {
        uint64_t lastDataSegmentIndex = segments.size() - (segments.back().mappedSegment.segName == "__LINKEDIT" ? 2 : 1);
        const MappedSegment& lastDataSegment = segments[lastDataSegmentIndex].mappedSegment;
        // for 32-bit binaries rebase targets are 0 based, so load address needs to be included in max pointer computation
        uint64_t lastDataSegmentLastVMAddr = preferredLoadAddress + lastDataSegment.runtimeOffset + lastDataSegment.runtimeSize;
        maxValidPointer = (lastDataSegmentLastVMAddr + 0x00100000-1) & -0x00100000; // align to 1MB
    }

    // allocate space in _bytes for full dyld_chained_fixups data structure
    size_t maxBytesNeeded = linkeditSize(bindTargets, segments, pointerFormat, pageSize);
    _bytes.resize(maxBytesNeeded, 0); // ensure alignment padding is zeroed out

    // build dyld_chained_fixups data structure
    dyld_chained_fixups_header* header = (dyld_chained_fixups_header*)(&_bytes[0]);
    header->fixups_version = 0; // version 0
    header->starts_offset  = (uint32_t)align8(sizeof(dyld_chained_fixups_header)); // 8-byte align dyld_chained_starts_in_image
    header->imports_offset = 0; // filled in later
    header->symbols_offset = 0; // filled in later
    header->imports_count  = (uint32_t)bindTargets.size();
    header->imports_format = imFormat;
    header->symbols_format = 0; // raw strings
    dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)(&_bytes[header->starts_offset]);
    startsInfo->seg_count  = (uint32_t)segments.size();

    // create dyld_chained_starts_in_segment for each segment
    {
        uint32_t segInfoOffset = (uint32_t)offsetof(dyld_chained_starts_in_image,seg_info_offset[segments.size()]);
        for ( uint32_t segIndex = 0; segIndex != segments.size(); ++segIndex ) {
            const MappedSegment& segment = segments[segIndex].mappedSegment;
            const std::span<const Fixup> fixupsInSegment = segments[segIndex].fixups;

            // don't make dyld_chained_starts_in_segment for segments with no fixups
            if ( !segment.writable || (segment.runtimeSize == 0) || fixupsInSegment.empty() ) {
                // segmented chain format needs seg info for all segments so each base addr is known
                if ( pointerFormat.value() != DYLD_CHAINED_PTR_ARM64E_SEGMENTED ) {
                    startsInfo->seg_info_offset[segIndex] = 0;
                    continue;
                }
            }

            uint32_t absOffset = header->starts_offset + segInfoOffset;
            segInfoOffset += align8(absOffset) - absOffset;

            startsInfo->seg_info_offset[segIndex] = segInfoOffset;
            dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)(&_bytes[header->starts_offset+segInfoOffset]);
            segInfo->size               = (uint32_t)offsetof(dyld_chained_starts_in_segment, page_start[1]);
            segInfo->page_size          = pageSize;
            segInfo->pointer_format     = pointerFormat.value();
            segInfo->segment_offset     = segment.runtimeOffset;
            segInfo->max_valid_pointer  = (uint32_t)maxValidPointer;
            segInfo->page_count         = 0; // fill in later, may be trailing pages with no fixups
            segInfo->page_start[0]      = DYLD_CHAINED_PTR_START_NONE;

            if ( !fixupsInSegment.empty() ) {
                uint64_t lastFixupSegmentOffset = (uint64_t)fixupsInSegment.back().location - (uint64_t)segment.content;
                uint64_t lastFixupPage = (lastFixupSegmentOffset / pageSize) + 1;

                segInfo->page_count = lastFixupPage;
                segInfo->size = (uint32_t)offsetof(dyld_chained_starts_in_segment, page_start[segInfo->page_count]);
                // adjust segment size info to include overflow entries
                segInfo->size += segments[segIndex].numPageExtras * sizeof(uint16_t);
           }
            segInfoOffset += segInfo->size;
        }

        if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND64 )
            header->imports_offset = align8(header->starts_offset + segInfoOffset);
        else
            header->imports_offset = align4(header->starts_offset + segInfoOffset);

        header->symbols_offset = (uint32_t)(header->imports_offset + importsTableSize);
    }

    std::vector<const MappedSegment*> mappedSegmentsBuffer;
    mappedSegmentsBuffer.reserve(segments.size());
    for ( const SegmentFixupsInfo& segInfo : segments )
        mappedSegmentsBuffer.push_back(&segInfo.mappedSegment);
    std::span<const MappedSegment*> mappedSegments = mappedSegmentsBuffer;

    // For segments, we're going to try do each page in parallel when possible
    // First this means computing the range of fixups for every page.  We can do that in parallel
    // Then walk those ranges in parallel.
    // For segments with pageExtras, its too hard to do pages in parallel so we'll go serially
    for ( uint32_t segIndex = 0; segIndex != segments.size(); ++segIndex ) {
        uint32_t segInfoOffset = startsInfo->seg_info_offset[segIndex];
        if ( segInfoOffset == 0 )
            continue;

        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)(&_bytes[header->starts_offset + segInfoOffset]);

        const MappedSegment&         segment   = segments[segIndex].mappedSegment;
        const std::span<const Fixup> segFixups = segments[segIndex].fixups;
        uint32_t                     segExtras = segments[segIndex].numPageExtras;

        std::span<uint16_t> pageStarts = { (uint16_t*)&segInfo->page_start[0], segInfo->page_count };
        const uint32_t      minNext    = pointerFormat.minNext();

        if ( segExtras != 0 ) {
            // Segment has extras.  Take the slow path
            std::span<uint16_t> extras = { (uint16_t*)&segInfo->page_start[segInfo->page_count], segExtras };

            int       curPageIndex      = -1;
            int       curExtrasIndex    = -1;
            const Fixup* prevFixup      = nullptr;
            for ( const Fixup& fixup : segFixups ) {
                uint64_t segOffset = (uint8_t*)fixup.location - (uint8_t*)segment.content;
                int pageIndex = (int)(segOffset/pageSize);
                if ( pageIndex != curPageIndex ) {
                    // End the previous chain if we have one
                    if ( prevFixup != nullptr ) {
                        if ( (pageStarts[curPageIndex] & DYLD_CHAINED_PTR_START_MULTI) != 0 ) {
                            // Mark the end of this extras chain
                            extras[curExtrasIndex] |= DYLD_CHAINED_PTR_START_LAST;
                        }

                        if ( setDataChains ) {
                            // set end of chain for this page
                            pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress, mappedSegments);
                        }
                    }
                    while (curPageIndex < pageIndex) {
                        ++curPageIndex;
                        pageStarts[curPageIndex] = DYLD_CHAINED_PTR_START_NONE;
                    }
                    pageStarts[curPageIndex] = (segOffset - (curPageIndex*pageSize));
                    prevFixup = nullptr;
                }

                // Found a previous fixup on this page, so make a chain from it to this fixup
                if ( prevFixup != nullptr ) {
                    uint8_t* chain = (uint8_t*)fixup.location;
                    intptr_t delta = chain - (uint8_t*)(prevFixup->location);
                    if ( delta <= pointerFormat.maxNext() ) {
                        if ( (delta % minNext) != 0 ) {
                            _buildError = Error("pointer not %d-byte aligned at %.*s+0x%llX, fix alignment or disable chained fixups",
                                                minNext, (int)segment.segName.size(), segment.segName.data(), segOffset);
                            break;
                        }
                        else if ( setDataChains ) {
                            pointerFormat.writeChainEntry(*prevFixup, chain, preferredLoadAddress, mappedSegments);
                        }
                    }
                    else {
                        // prev/next are too far apart for chain to span, instead terminate chain at prevFixup
                        if ( setDataChains )
                            pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress, mappedSegments);
                        // then start new overflow chain
                        if ( (pageStarts[curPageIndex] & DYLD_CHAINED_PTR_START_MULTI) == 0 ) {
                            ++curExtrasIndex;
                            // move first start to overflow array
                            extras[curExtrasIndex] = pageStarts[curPageIndex];
                            // change first page start to point into overflow array
                            pageStarts[curPageIndex] = DYLD_CHAINED_PTR_START_MULTI | (segInfo->page_count + curExtrasIndex);
                        }
                        uint16_t pageOffset = segOffset % pageSize;
                        ++curExtrasIndex;
                        extras[curExtrasIndex] = pageOffset;
                    }
                }
                prevFixup = &fixup;
            }
            // if this page required multiple starts, mark last one
            if ( (pageStarts[curPageIndex] & DYLD_CHAINED_PTR_START_MULTI) != 0 ) {
                extras[curExtrasIndex] |= DYLD_CHAINED_PTR_START_LAST;
            }
            if ( setDataChains && (prevFixup != nullptr) ) {
                // set end of chain
                pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress, mappedSegments);
            }
        } else {
            // No extras, so use parallelism
            typedef std::pair<std::atomic<const Fixup*>, std::atomic<const Fixup*>> FixupRange;
            // use up to 128kb on stack, main thread has 8mb large stack by default
            STACK_ALLOC_OVERFLOW_SAFE_ARRAY(FixupRange, fixupRangesStorage, 0x2000);
            fixupRangesStorage.resize(segInfo->page_count);
            // array ::resize doesn't initialize new elements, so do it here
            bzero(&fixupRangesStorage[0], sizeof(FixupRange) * segInfo->page_count);
            std::span<FixupRange> fixupRanges = { &fixupRangesStorage[0], segInfo->page_count };

            // Walk all fixups and get the range for each page
            mapReduce(segFixups, ^(size_t, int&, std::span<const Fixup> fixups) {

                int curPageIndex = -1;
                const Fixup* endFixup = nullptr;

                // The very first fixup we process might be the first on its page, or might be
                // somewhere in the middle.  So it needs as atomic min to make sure its safe with other threads
                {
                    const Fixup& fixup = fixups[0];
                    uint64_t segOffset = (uint8_t*)fixup.location - (uint8_t*)segment.content;
                    int pageIndex = (int)(segOffset/pageSize);
                    atomic_min(fixupRanges[pageIndex].first, &fixup);

                    curPageIndex = pageIndex;
                    endFixup = &fixup;
                }

                fixups = fixups.subspan(1);

                for ( const Fixup& fixup : fixups ) {
                    uint64_t segOffset = (uint8_t*)fixup.location - (uint8_t*)segment.content;
                    int pageIndex = (int)(segOffset/pageSize);

                    if ( pageIndex != curPageIndex ) {
                        // Crossing in to a new page.  As fixups are sorted, we know for sure the
                        // last fixup we processed must be on the end of its page
                        fixupRanges[curPageIndex].second.store(endFixup, std::memory_order_relaxed);

                        // Also the new fixup we have must be the first on its page
                        fixupRanges[pageIndex].first.store(&fixup, std::memory_order_relaxed);

                        curPageIndex = pageIndex;
                    }

                    endFixup = &fixup;
                }

                // The last fixup we have is somewhere in a page, but we don't know if its the end
                // of that page or not.  Try set it as the max
                atomic_max(fixupRanges[curPageIndex].second, endFixup);
            });

            // If there's an unaligned fixup, this will store if offset in the segment
            std::atomic<uint64_t> unalignedFixupOffset = ~0ULL;
            std::atomic<uint64_t>& unalignedFixupOffsetRef = unalignedFixupOffset;

            // Now process all pages in parallel
            mapReduce(fixupRanges, std::max(fixupRanges.size() / 64, 32ul), ^(size_t, int&, std::span<FixupRange> ranges) {
                for ( const FixupRange& fixupRange : ranges ) {
                    size_t pageIndex = &fixupRange - fixupRanges.data();
                    const Fixup* start = fixupRange.first.load(std::memory_order_relaxed);
                    const Fixup* end = fixupRange.second.load(std::memory_order_relaxed);
                    if ( start == nullptr ) {
                        assert(end == nullptr);
                        pageStarts[pageIndex] = DYLD_CHAINED_PTR_START_NONE;
                        continue;
                    }

                    assert(end != nullptr);
                    assert(start <= end);
                    uint64_t startSegOffset = (uint8_t*)start->location - (uint8_t*)segment.content;
                    pageStarts[pageIndex] = (startSegOffset - (pageIndex * pageSize));

                    if ( setDataChains ) {
                        const Fixup* fixup = start;
                        while ( fixup != end ) {
                            const Fixup* prev = fixup;
                            ++fixup;
                            uint8_t* chain = (uint8_t*)fixup->location;
                            intptr_t delta = chain - (uint8_t*)(prev->location);
                            if ( (delta % minNext) != 0 ) {
                                uint64_t segOffset = (uint8_t*)fixup->location - (uint8_t*)segment.content;
                                atomic_min(unalignedFixupOffsetRef, segOffset, ~0ULL);
                                break;
                            }
                            pointerFormat.writeChainEntry(*prev, chain, preferredLoadAddress, mappedSegments);
                        }

                        // set end of chain
                        pointerFormat.writeChainEntry(*end, nullptr, preferredLoadAddress, mappedSegments);
                    }
                }
            });

            uint64_t segOffset = unalignedFixupOffset.load(std::memory_order_relaxed);
            if ( (segOffset != ~0ULL) && !_buildError.hasError() ) {
                _buildError = Error("pointer not %d-byte aligned at %.*s+0x%llX, fix alignment or disable chained fixups",
                                    minNext, (int)segment.segName.size(), segment.segName.data(), segOffset);
            }
        }
    }

    // append import table and string pool
    memcpy(&_bytes[header->imports_offset], importsTableStart, importsTableSize);
    memcpy(&_bytes[header->symbols_offset], &stringPool[0], stringPool.size());

    _fixupsHeader = (dyld_chained_fixups_header*)(&_bytes[0]);
    _fixupsSize   = _bytes.size();
}


void ChainedFixupsWriter::buildStartsSectionFixups(std::span<const SegmentFixupsInfo> segments,
                                                   const PointerFormat& pointerFormat,
                                                   bool useFileOffsets, uint64_t preferredLoadAddress)
{
    // Allocate space in _bytes for dyld_chained_starts_offsets data structure
    size_t maxBytesNeeded = startsSectionSize(segments, pointerFormat);
    _bytes.resize(maxBytesNeeded, 0); // ensure alignment padding is zeroed out

    // Build dyld_chained_starts_offsets data structure
    dyld_chained_starts_offsets* header = (dyld_chained_starts_offsets*)(&_bytes[0]);
    header->pointer_format   = pointerFormat.value();
    header->starts_count     = 0; // filled in later
    header->chain_starts[0]  = 0; // filled in later

    // make span for use by writeChainEntry()
    std::vector<const MappedSegment*> mappedSegmentsBuffer;
    mappedSegmentsBuffer.reserve(segments.size());
    for ( const SegmentFixupsInfo& segInfo : segments )
        mappedSegmentsBuffer.push_back(&segInfo.mappedSegment);
    std::span<const MappedSegment*> mappedSegments = mappedSegmentsBuffer;

    std::vector<uint32_t> startsOffsets;
    for (const SegmentFixupsInfo& segmentFixups : segments) {
        const MappedSegment& segment = segmentFixups.mappedSegment;
        const std::span<const Fixup> fixups = segmentFixups.fixups;
        // Don't make chain_starts for segments with no fixups
        if ( !segment.writable || (segment.runtimeSize == 0) || fixups.empty() )
            continue;

        uint64_t maxDelta = 0;
        switch ( pointerFormat.value() ) {
            case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                maxDelta = 0x1FFF;
                break;
            case DYLD_CHAINED_PTR_64_OFFSET:
                maxDelta = 0x3FFF;
                break;
            default:
                assert(false && "unknown pointer format for chain starts");
                break;
        }
        const Fixup* prevFixup = nullptr;
        for (const Fixup& fixup : fixups) {
            // -fixup_chains_section_vm
            uint64_t segmentOffset = segment.runtimeOffset;
            if ( useFileOffsets ) {
                // -fixup_chains_section
                segmentOffset = segment.fileOffset;
            }
            uint64_t offsetInSegment = (uint64_t)fixup.location - (uint64_t)segment.content;
            uint64_t fixupOffset = segmentOffset + offsetInSegment;
            assert(fixupOffset == (uint32_t)fixupOffset);

            if ( prevFixup == nullptr ) {
                // First fixup
                startsOffsets.push_back((uint32_t)fixupOffset);
                prevFixup = &fixup;
                continue;
            }
            uint8_t* chain = (uint8_t*)fixup.location;
            uint64_t delta = (uint64_t)chain - (uint64_t)(prevFixup->location);
            if ( delta < maxDelta ) {
                pointerFormat.writeChainEntry(*prevFixup, chain, 0 /* preferedLoadAddress */, mappedSegments);
            } else {
                // prev/next are too far apart for chain to span, instead terminate chain at prevFixup
                pointerFormat.writeChainEntry(*prevFixup, nullptr /* nextLoc */, 0 /* preferedLoadAddress */, mappedSegments);
                startsOffsets.push_back((uint32_t)fixupOffset);
            }
            prevFixup = &fixup;
        }
        // Terminate last chain
        pointerFormat.writeChainEntry(*prevFixup, nullptr /* nextLoc */, 0 /* preferedLoadAddress */, mappedSegments);

        uint32_t startsCount = (uint32_t)startsOffsets.size();
        assert(startsCount == startsOffsets.size());
        header->starts_count = startsCount;
        for (uint32_t i = 0; i < startsCount; i++) {
            header->chain_starts[i] = startsOffsets[i];
        }
    }
    _chainStartsHeader = (dyld_chained_starts_offsets*)(&_bytes[0]);
    _fixupsSize = _bytes.size();
}

uint32_t ChainedFixupsWriter::addSymbolString(CString symbolName, std::vector<char>& pool)
{
    uint32_t symbolOffset = (uint32_t)pool.size();
    // end+1 to copy also the null-terminator
    pool.insert(pool.end(), symbolName.begin(), symbolName.end()+1);
    return symbolOffset;
}

} // namespace mach_o
