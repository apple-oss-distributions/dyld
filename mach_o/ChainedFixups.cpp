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

#if BUILDING_MACHO_WRITER
#include "Algorithm.h"
#include "Array.h"
#endif

#include "Array.h"

#include "ChainedFixups.h"
#include "Misc.h"
#include "Image.h"

using dyld3::Array;

namespace mach_o {



//
// MARK: --- ChainedFixups inspection methods ---
//

ChainedFixups::ChainedFixups(const dyld_chained_fixups_header* start, size_t size)
: _fixupsHeader(start), _fixupsSize(size)
{
}

const dyld_chained_fixups_header* ChainedFixups::bytes(size_t& size) const
{
    size = _fixupsSize;
    return _fixupsHeader;
}

void ChainedFixups::forEachBindTarget(void (^callback)(const Fixup::BindTarget&, bool& stop)) const
{
    (void)this->forEachBindTarget(^(int libOrdinal, const char* symbolName, int64_t addend, bool weakImport, bool& stop) {
        Fixup::BindTarget target = { symbolName, libOrdinal, weakImport, addend };
        callback(target, stop);
    });
}

uint32_t ChainedFixups::pageSize() const
{
    const dyld_chained_starts_in_image* imageStarts = (dyld_chained_starts_in_image*)((uint8_t*)_fixupsHeader + _fixupsHeader->starts_offset);
    for (int i=0; i < imageStarts->seg_count; ++i) {
        const dyld_chained_starts_in_segment* segStarts = this->startsForSegment(i);
        if ( segStarts->page_size != 0 )
            return segStarts->page_size;
    }
    return 0x1000;
}

Error ChainedFixups::forEachBindTarget(void (^callback)(int libOrdinal, const char* symbolName, int64_t addend, bool weakImport, bool& stop)) const
{
    if ( _fixupsHeader->imports_offset > _fixupsSize )
        return Error("malformed import table, imports_offset too large");
    if ( _fixupsHeader->symbols_offset > _fixupsSize )
        return Error("malformed import table, symbols_offset too large");

    const char*                         symbolsPool     = (char*)_fixupsHeader + _fixupsHeader->symbols_offset;
    size_t                              maxSymbolOffset = _fixupsSize - _fixupsHeader->symbols_offset;
    int                                 libOrdinal;
    bool                                stop            = false;
    const dyld_chained_import*          imports;
    const dyld_chained_import_addend*   importsA32;
    const dyld_chained_import_addend64* importsA64;
    switch (_fixupsHeader->imports_format) {
        case DYLD_CHAINED_IMPORT:
            imports = (dyld_chained_import*)((uint8_t*)_fixupsHeader + _fixupsHeader->imports_offset);
            for (uint32_t i=0; i < _fixupsHeader->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[imports[i].name_offset];
                if ( imports[i].name_offset > maxSymbolOffset )
                    return Error("malformed import table, imports[%d].name_offset (%d) out of range", i, imports[i].name_offset);
                uint8_t libVal = imports[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, 0, imports[i].weak_import, stop);
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND:
            importsA32 = (dyld_chained_import_addend*)((uint8_t*)_fixupsHeader + _fixupsHeader->imports_offset);
            for (uint32_t i=0; i < _fixupsHeader->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA32[i].name_offset];
                if ( importsA32[i].name_offset > maxSymbolOffset )
                    return Error("malformed import table, imports[%d].name_offset (%d) out of range", i, importsA32[i].name_offset);
                uint8_t libVal = importsA32[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA32[i].addend, importsA32[i].weak_import, stop);
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND64:
            importsA64 = (dyld_chained_import_addend64*)((uint8_t*)_fixupsHeader + _fixupsHeader->imports_offset);
            for (uint32_t i=0; i < _fixupsHeader->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA64[i].name_offset];
                if ( importsA64[i].name_offset > maxSymbolOffset )
                    return Error("malformed import table, imports[%d].name_offset (%d) out of range", i, importsA64[i].name_offset);
                uint16_t libVal = importsA64[i].lib_ordinal;
                if ( libVal > 0xFFF0 )
                    libOrdinal = (int16_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA64[i].addend, importsA64[i].weak_import, stop);
            }
            break;
        default:
            return  Error("unknown imports format %d", _fixupsHeader->imports_format);
    }
    return Error::none();
}

const dyld_chained_starts_in_segment* ChainedFixups::startsForSegment(uint32_t segIndex) const
{
    const dyld_chained_starts_in_image* imageStarts = (dyld_chained_starts_in_image*)((uint8_t*)_fixupsHeader + _fixupsHeader->starts_offset);
    if ( segIndex >= imageStarts->seg_count )
        return nullptr;
    uint32_t segInfoOffset = imageStarts->seg_info_offset[segIndex];
    if ( segInfoOffset == 0 )
        return nullptr;
    return (dyld_chained_starts_in_segment*)((uint8_t*)imageStarts + segInfoOffset);
}

const ChainedFixups::PointerFormat& ChainedFixups::pointerFormat() const
{
    const dyld_chained_starts_in_image* imageStarts = (dyld_chained_starts_in_image*)((uint8_t*)_fixupsHeader + _fixupsHeader->starts_offset);
    for (uint32_t segIndex=0; segIndex < imageStarts->seg_count; ++segIndex) {
        uint32_t segInfoOffset = imageStarts->seg_info_offset[segIndex];
        if ( segInfoOffset == 0 )
            continue;
        const dyld_chained_starts_in_segment* segStarts = (dyld_chained_starts_in_segment*)((uint8_t*)imageStarts + segInfoOffset);
        if ( segStarts->pointer_format != 0 )
            return PointerFormat::make(segStarts->pointer_format);
    }
    assert(0 && "can't find pointer format");
}

void ChainedFixups::forEachFixupChainStartLocation(std::span<const MappedSegment> segments,
                                                   void (^callback)(const void* loc, uint32_t segIndex, const PointerFormat&, bool& stop)) const
{
    bool stop = false;
    for (uint32_t segIndex=0; segIndex < segments.size(); ++segIndex) {
        if ( const dyld_chained_starts_in_segment* segStarts = this->startsForSegment(segIndex) ) {
            const PointerFormat& pf = PointerFormat::make(segStarts->pointer_format);
            for (uint32_t pageIndex=0; pageIndex < segStarts->page_count; ++pageIndex) {
                uint16_t offsetInPage = segStarts->page_start[pageIndex];
                if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
                    continue;
                if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
                    // some fixups in the page are too far apart, so page has multiple starts
                    uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
                    bool chainEnd = false;
                    while ( !chainEnd ) {
                        chainEnd = (segStarts->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                        uint16_t  startOffset = (segStarts->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                        uint32_t* chainStart  = (uint32_t*)((uint8_t*)(segments[segIndex].content) + startOffset);
                        callback(chainStart, segIndex, pf, stop);
                        ++overflowIndex;
                    }
                }
                else {
                    const uint8_t* chainStart = ((uint8_t*)(segments[segIndex].content)) + pageIndex * segStarts->page_size + offsetInPage;
                    callback(chainStart, segIndex, pf, stop);
                }
                if ( stop )
                    return;
            }
        }
    }
}


Error ChainedFixups::valid(std::span<const MappedSegment> segments) const
{
#if BUILDING_MACHO_WRITER
    if ( _buildError.hasError() )
        return Error("%s", _buildError.message());
#endif
    // validate dyld_chained_fixups_header
    if ( _fixupsHeader->fixups_version != 0 )
        return Error("chained fixups, unknown header version (%d)", _fixupsHeader->fixups_version);
    if ( _fixupsHeader->starts_offset >= _fixupsSize )
        return Error("chained fixups, starts_offset exceeds LC_DYLD_CHAINED_FIXUPS size");
    if ( _fixupsHeader->imports_offset > _fixupsSize)
        return Error("chained fixups, imports_offset exceeds LC_DYLD_CHAINED_FIXUPS size");
    uint32_t formatEntrySize;
    switch ( _fixupsHeader->imports_format ) {
        case DYLD_CHAINED_IMPORT:
            formatEntrySize = sizeof(dyld_chained_import);
            break;
        case DYLD_CHAINED_IMPORT_ADDEND:
            formatEntrySize = sizeof(dyld_chained_import_addend);
            break;
        case DYLD_CHAINED_IMPORT_ADDEND64:
            formatEntrySize = sizeof(dyld_chained_import_addend64);
            break;
        default:
            return Error("chained fixups, unknown imports_format (%d)", _fixupsHeader->imports_format);
    }
    if ( greaterThanAddOrOverflow(_fixupsHeader->imports_offset, (formatEntrySize * _fixupsHeader->imports_count), _fixupsHeader->symbols_offset) )
        return Error("chained fixups, imports array overlaps symbols");
    if ( _fixupsHeader->symbols_format != 0 )
        return Error("chained fixups, symbols_format unknown (%d)", _fixupsHeader->symbols_format);


    // validate dyld_chained_starts_in_image
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)_fixupsHeader + _fixupsHeader->starts_offset);
    if ( startsInfo->seg_count != segments.size() ) {
        // We can have fewer segments than the count, so long as those we are missing have no relocs
        // This can happen because __CTF is inserted by ctf_insert after linking, and between __DATA and __LINKEDIT, but has no relocs
        // ctf_insert updates the load commands to put __CTF between __DATA and __LINKEDIT, but doesn't update the chained fixups data structures
        if ( startsInfo->seg_count > segments.size() )
            return Error("chained fixups, seg_count exceeds number of segments");

        // We can have fewer segments than the count, so long as those we are missing have no relocs
        const MappedSegment& lastSegInfo = segments.back();
        if ( lastSegInfo.segName != "__CTF" )
            return Error("chained fixups, seg_count does not match number of segments");
    }

    uint32_t        maxValidPointerSeen     = 0;
    uint16_t        pointer_format_for_all  = 0;
    bool            pointer_format_found    = false;
    const uint8_t*  endOfStarts             = (uint8_t*)_fixupsHeader + _fixupsHeader->imports_offset;
    for (uint32_t i=0; i < startsInfo->seg_count; ++i) {
        uint32_t segInfoOffset = startsInfo->seg_info_offset[i];
        // 0 offset means this segment has no fixups
        if ( segInfoOffset == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + segInfoOffset);
        if ( segInfo->size > (endOfStarts - (uint8_t*)segInfo) )
            return Error("chained fixups, dyld_chained_starts_in_segment for segment #%d overruns imports table", i);

        // validate dyld_chained_starts_in_segment
        if ( (segInfo->page_size != 0x1000) && (segInfo->page_size != 0x4000) )
            return Error("chained fixups, page_size not 4KB or 16KB in segment #%d", i);
        if ( !PointerFormat::valid(segInfo->pointer_format) )
            return Error("chained fixups, unknown pointer_format in segment #%d", i);
        if ( !pointer_format_found ) {
            pointer_format_for_all = segInfo->pointer_format;
            pointer_format_found = true;
        }
        if ( segInfo->pointer_format != pointer_format_for_all )
            return Error("chained fixups, pointer_format not same for all segments %d and %d", segInfo->pointer_format, pointer_format_for_all);
        if ( segInfo->max_valid_pointer != 0 ) {
            if ( maxValidPointerSeen == 0 ) {
                // record max_valid_pointer values seen
                maxValidPointerSeen = segInfo->max_valid_pointer;
            }
            else if ( maxValidPointerSeen != segInfo->max_valid_pointer ) {
                return Error("chained fixups, different max_valid_pointer values seen in different segments");
            }
        }
        // validate starts table in segment
        if ( offsetof(dyld_chained_starts_in_segment, page_start[segInfo->page_count]) > segInfo->size )
            return Error("chained fixups, page_start array overflows size");
        uint32_t maxOverflowIndex = (uint32_t)(segInfo->size - offsetof(dyld_chained_starts_in_segment, page_start[0]))/sizeof(uint16_t);
        for (int pageIndex=0; pageIndex < segInfo->page_count; ++pageIndex) {
            uint16_t offsetInPage = segInfo->page_start[pageIndex];
            if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
                continue;
            if ( (offsetInPage & DYLD_CHAINED_PTR_START_MULTI) == 0 ) {
                // this is the offset into the page where the first fixup is
                if ( offsetInPage > segInfo->page_size ) {
                    return Error("chained fixups, in segment #%d page_start[%d]=0x%04X exceeds page size", i, pageIndex, offsetInPage);
                }
            }
            else {
                // this is actually an index into chain_starts[]
                uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
                // now verify all starts are within the page and in ascending order
                uint16_t lastOffsetInPage = 0;
                do {
                    if ( overflowIndex > maxOverflowIndex )
                        return Error("chain overflow index out of range %d (max=%d) in segment #%d", overflowIndex, maxOverflowIndex, i);
                    offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                    if ( offsetInPage > segInfo->page_size )
                        return Error("chained fixups, in segment #%d overflow page_start[%d]=0x%04X exceeds page size", i, overflowIndex, offsetInPage);
                    if ( (offsetInPage <= lastOffsetInPage) && (lastOffsetInPage != 0) )
                        return Error("chained fixups, in segment #%d overflow page_start[%d]=0x%04X is before previous at 0x%04X\n", i, overflowIndex, offsetInPage, lastOffsetInPage);
                    lastOffsetInPage = offsetInPage;
                    ++overflowIndex;
                } while ( (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST) == 0 );
            }
        }

    }
    // validate import table size can fit
    uint32_t maxBindOrdinal = PointerFormat::make(pointer_format_for_all).maxBindOrdinal(false);
    if ( _fixupsHeader->imports_count >= maxBindOrdinal )
        return Error("chained fixups, imports_count (%d) exceeds max of %d", _fixupsHeader->imports_count, maxBindOrdinal);

    // validate max_valid_pointer is larger than last segment
    //if ( maxValidPointerSeen != 0 ) {
    //    uint64_t lastSegmentLastVMAddr = segmentsInfo[leInfo.layout.linkeditSegIndex-1].vmAddr + segmentsInfo[leInfo.layout.linkeditSegIndex-1].vmSize;
    //    if ( maxValidPointerSeen < lastSegmentLastVMAddr ) {
    //        return Error("chained fixups, max_valid_pointer too small for image");
    //    }
    //}
    return Error::none();
}


const char* ChainedFixups::importsFormatName(uint32_t format)
{
    switch (format) {
        case DYLD_CHAINED_IMPORT:
            return "DYLD_CHAINED_IMPORT";
        case DYLD_CHAINED_IMPORT_ADDEND:
            return "DYLD_CHAINED_IMPORT_ADDEND";
        case DYLD_CHAINED_IMPORT_ADDEND64:
            return "DYLD_CHAINED_IMPORT_ADDEND64";
    }
    return "unknown";
}

const char* ChainedFixups::importsFormatName() const
{
    return importsFormatName(_fixupsHeader->imports_format);
}

void ChainedFixups::PointerFormat::forEachFixupLocationInChain(const void* chainStartLoc, uint64_t prefLoadAddr, const MappedSegment* seg,
                                                               void (^callback)(const Fixup& info, bool& stop)) const
{
    bool stop = false;
    const void* nextLoc = nullptr;
    for ( const void* fixupLoc = chainStartLoc; (fixupLoc != nullptr) && !stop; fixupLoc = nextLoc) {
        // get next before calling callback, because callback may update location (set runtime pointer)
        nextLoc = this->nextLocation(fixupLoc);
        callback(this->parseChainEntry(fixupLoc, seg, prefLoadAddr), stop);
    }
}


#if BUILDING_MACHO_WRITER


template <typename T>
static T align8(T value)
{
    return (value + 7) & (-8);
}

size_t ChainedFixups::linkeditSize(std::span<const Fixup::BindTarget> bindTargets,
                                   std::span<const SegmentFixupsInfo> segments,
                                   uint32_t pageSize)
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
            size_t segInfoSize = align8(offsetof(dyld_chained_starts_in_segment, page_start[lastFixupPage + extras]));
            maxBytesNeeded += segInfoSize;
        }
    }

    maxBytesNeeded = align8(maxBytesNeeded);

    size_t importTableSize = 0;
    if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND64 )
        importTableSize = align8(sizeof(dyld_chained_import_addend64) * bindTargets.size());
    else if ( imFormat ==  DYLD_CHAINED_IMPORT_ADDEND )
        importTableSize = align8(sizeof(dyld_chained_import_addend) * bindTargets.size());
    else
        importTableSize = align8(sizeof(dyld_chained_import) * bindTargets.size());

    maxBytesNeeded += importTableSize;
    maxBytesNeeded += align8(stringPoolSize);

    return maxBytesNeeded;
}

void ChainedFixups::calculateSegmentPageExtras(std::span<SegmentFixupsInfo> segments,
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

Error ChainedFixups::importsFormat(std::span<const Fixup::BindTarget> bindTargets, uint16_t& importsFormat, size_t& stringPoolSize)
{
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

#if BUILDING_UNIT_TESTS
ChainedFixups::ChainedFixups(std::span<const Fixup::BindTarget> bindTargets,
                             std::span<const Fixup> fixups,
                             std::span<const MappedSegment> segments,
                             uint64_t preferredLoadAddress,
                             const PointerFormat& pointerFormat, uint32_t pageSize, bool setDataChains)
{
    std::vector<std::vector<Fixup>> fixupsInSegments;
    fixupsInSegments.reserve(segments.size());

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

    buildFixups(bindTargets, segmentFixupInfos, preferredLoadAddress, pointerFormat, pageSize, setDataChains);
}
#endif


ChainedFixups::ChainedFixups(std::span<const Fixup::BindTarget> bindTargets,
                             std::span<const SegmentFixupsInfo> segments,
                             uint64_t preferredLoadAddress,
                             const PointerFormat& pointerFormat, uint32_t pageSize, bool setDataChains)
{
    buildFixups(bindTargets, segments, preferredLoadAddress, pointerFormat, pageSize, setDataChains);
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


void ChainedFixups::buildFixups(std::span<const Fixup::BindTarget> bindTargets,
                                std::span<const SegmentFixupsInfo> segments,
                                uint64_t preferredLoadAddress,
                                const PointerFormat& pointerFormat, uint32_t pageSize, bool setDataChains)
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

    // for 32-bit archs, compute maxRebaseAddress value
    uint64_t maxRebaseAddress = 0;
    if ( !pointerFormat.is64() ) {
        for ( const SegmentFixupsInfo& segment : segments ) {
            const MappedSegment& seg = segment.mappedSegment;
            if ( seg.segName == "__LINKEDIT" ) {
                uint64_t baseAddress = preferredLoadAddress;
                if ( baseAddress == 0x4000 )
                    baseAddress = 0; // 32-bit main executables have rebase targets that are zero based
                maxRebaseAddress = (seg.runtimeOffset + 0x00100000-1) & -0x00100000; // align to 1MB
            }
        }
    }

    // allocate space in _bytes for full dyld_chained_fixups data structure
    size_t maxBytesNeeded = linkeditSize(bindTargets, segments, pageSize);
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
        uint32_t segInfoOffset = (uint32_t)align8(offsetof(dyld_chained_starts_in_image,seg_info_offset[segments.size()]));
        for ( uint32_t segIndex = 0; segIndex != segments.size(); ++segIndex ) {
            const MappedSegment& segment = segments[segIndex].mappedSegment;
            const std::span<const Fixup> fixupsInSegment = segments[segIndex].fixups;

            // don't make dyld_chained_starts_in_segment for segments with no fixups
            if ( !segment.writable || (segment.runtimeSize == 0) || fixupsInSegment.empty() ) {
                startsInfo->seg_info_offset[segIndex] = 0;
                continue;
            }

            startsInfo->seg_info_offset[segIndex] = segInfoOffset;
            dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)(&_bytes[header->starts_offset+segInfoOffset]);
            segInfo->size               = 0; // filled in later
            segInfo->page_size          = pageSize;
            segInfo->pointer_format     = pointerFormat.value();
            segInfo->segment_offset     = segment.runtimeOffset;
            segInfo->max_valid_pointer  = (uint32_t)maxRebaseAddress;
            segInfo->page_count         = 0; // fill in later, may be trailing pages with no fixups
            segInfo->page_start[0]      = DYLD_CHAINED_PTR_START_NONE;

            uint64_t lastFixupSegmentOffset = (uint64_t)fixupsInSegment.back().location - (uint64_t)segment.content;
            uint64_t lastFixupPage = (lastFixupSegmentOffset / pageSize) + 1;

            segInfo->page_count = lastFixupPage;
            segInfo->size = (uint32_t)offsetof(dyld_chained_starts_in_segment, page_start[segInfo->page_count]);

            // adjust segment size info to include overflow entries
            segInfo->size += segments[segIndex].numPageExtras * sizeof(uint16_t);

            segInfoOffset += segInfo->size;
            segInfoOffset = align8(segInfoOffset);
        }

        header->imports_offset = align8(header->starts_offset + segInfoOffset);
        header->symbols_offset = (uint32_t)align8(header->imports_offset + importsTableSize);
    }

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
                            pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress);
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
                            pointerFormat.writeChainEntry(*prevFixup, chain, preferredLoadAddress);
                        }
                    }
                    else {
                        // prev/next are too far apart for chain to span, instead terminate chain at prevFixup
                        if ( setDataChains )
                            pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress);
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
                pointerFormat.writeChainEntry(*prevFixup, nullptr, preferredLoadAddress);
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
                            pointerFormat.writeChainEntry(*prev, chain, preferredLoadAddress);
                        }

                        // set end of chain
                        pointerFormat.writeChainEntry(*end, nullptr, preferredLoadAddress);
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


uint32_t ChainedFixups::addSymbolString(CString symbolName, std::vector<char>& pool)
{
    uint32_t symbolOffset = (uint32_t)pool.size();
    // end+1 to copy also the null-terminator
    pool.insert(pool.end(), symbolName.begin(), symbolName.end()+1);
    return symbolOffset;
}


#endif


//
// MARK: --- PointerFormat_Generic_arm64e ---
//
class VIS_HIDDEN PointerFormat_Generic_arm64e : public ChainedFixups::PointerFormat
{
public:
    bool             is64() const override                                       { return true; }
    bool             supportsAuth() const override                               { return true; }
    uint32_t         minNext() const override                                    { return stride(); }
    uint32_t         maxNext() const override                                    { return stride()*0x7FF; }  // 11-bits
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return (authenticated ? 0xFFFFFFFF : 0x7FFFFFFFFFFULL); }
    bool             supportsBinds() const override                              { return true; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return (1 << bindBitCount()) - 1; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return (authenticated ? 0 : 0x3FFFF); }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return (authenticated ? 0 : -0x3FFFF); }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_arm64e_rebase* ptr = (dyld_chained_ptr_arm64e_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * stride());
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
       if ( ((dyld_chained_ptr_arm64e_rebase*)loc)->bind ) {
            if ( bindBitCount() == 24 ) {
                const dyld_chained_ptr_arm64e_auth_bind24* authBind24Ptr = (dyld_chained_ptr_arm64e_auth_bind24*)loc;
                const dyld_chained_ptr_arm64e_bind24*      bind24Ptr     = (dyld_chained_ptr_arm64e_bind24*)loc;
                if ( authBind24Ptr->auth )
                    return Fixup(loc, seg, authBind24Ptr->ordinal, 0, authBind24Ptr->key, authBind24Ptr->addrDiv, authBind24Ptr->diversity);
                else
                   return Fixup(loc, seg, bind24Ptr->ordinal, bind24Ptr->addend);
            }
            else {
                const dyld_chained_ptr_arm64e_auth_bind*   authBindPtr   = (dyld_chained_ptr_arm64e_auth_bind*)loc;
                const dyld_chained_ptr_arm64e_bind*        bindPtr       = (dyld_chained_ptr_arm64e_bind*)loc;
                if ( authBindPtr->auth )
                    return Fixup(loc, seg, authBindPtr->ordinal, 0, authBindPtr->key, authBindPtr->addrDiv, authBindPtr->diversity);
                else
                    return Fixup(loc, seg, bindPtr->ordinal, bindPtr->addend);
            }
        }
        else {
            const dyld_chained_ptr_arm64e_auth_rebase* authRebasePtr = (dyld_chained_ptr_arm64e_auth_rebase*)loc;
            const dyld_chained_ptr_arm64e_rebase*      rebasePtr     = (dyld_chained_ptr_arm64e_rebase*)loc;
            if ( authRebasePtr->auth )
               return Fixup(loc, seg, authRebasePtr->target, authRebasePtr->key, authRebasePtr->addrDiv, authRebasePtr->diversity);
            else if ( unauthRebaseIsVmAddr() )
                return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | (rebasePtr->target - preferedLoadAddress));
            else
                return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | rebasePtr->target);
        }
    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        if ( fixup.isBind ) {
            if ( bindBitCount() == 24 ) {
                if ( fixup.authenticated ) {
                    dyld_chained_ptr_arm64e_auth_bind24* authBind24Ptr = (dyld_chained_ptr_arm64e_auth_bind24*)fixup.location;
                    authBind24Ptr->auth      = true;
                    authBind24Ptr->bind      = true;
                    authBind24Ptr->next      = delta/stride();
                    authBind24Ptr->key       = fixup.auth.key;
                    authBind24Ptr->addrDiv   = fixup.auth.usesAddrDiversity;
                    authBind24Ptr->diversity = fixup.auth.diversity;
                    authBind24Ptr->zero      = 0;
                    authBind24Ptr->ordinal   = fixup.bind.bindOrdinal;
                    assert(authBind24Ptr->next*stride() == delta);
                    assert(authBind24Ptr->ordinal == fixup.bind.bindOrdinal);
                    assert(fixup.bind.embeddedAddend == 0);
                }
                else {
                    dyld_chained_ptr_arm64e_bind24* bind24Ptr = (dyld_chained_ptr_arm64e_bind24*)fixup.location;
                    bind24Ptr->auth     = false;
                    bind24Ptr->bind     = true;
                    bind24Ptr->next     = delta/stride();
                    bind24Ptr->addend   = fixup.bind.embeddedAddend;
                    bind24Ptr->zero     = 0;
                    bind24Ptr->ordinal  = fixup.bind.bindOrdinal;
                    assert(bind24Ptr->addend == fixup.bind.embeddedAddend);
                    assert(bind24Ptr->next*stride() == delta);
                    assert(bind24Ptr->ordinal == fixup.bind.bindOrdinal);
                }
            }
            else {
                if ( fixup.authenticated ) {
                    dyld_chained_ptr_arm64e_auth_bind* authBindPtr = (dyld_chained_ptr_arm64e_auth_bind*)fixup.location;
                    authBindPtr->auth      = true;
                    authBindPtr->bind      = true;
                    authBindPtr->next      = delta/stride();
                    authBindPtr->key       = fixup.auth.key;
                    authBindPtr->addrDiv   = fixup.auth.usesAddrDiversity;
                    authBindPtr->diversity = fixup.auth.diversity;
                    authBindPtr->zero      = 0;
                    authBindPtr->ordinal   = fixup.bind.bindOrdinal;
                    assert(authBindPtr->next*stride() == delta);
                    assert(authBindPtr->ordinal == fixup.bind.bindOrdinal);
                    assert(fixup.bind.embeddedAddend == 0);
                }
                else {
                    dyld_chained_ptr_arm64e_bind* bindPtr = (dyld_chained_ptr_arm64e_bind*)fixup.location;
                    bindPtr->auth     = false;
                    bindPtr->bind     = true;
                    bindPtr->next     = delta/stride();
                    bindPtr->addend   = fixup.bind.embeddedAddend;
                    bindPtr->zero     = 0;
                    bindPtr->ordinal  = fixup.bind.bindOrdinal;
                    assert(bindPtr->addend == fixup.bind.embeddedAddend);
                    assert(bindPtr->next*stride() == delta);
                    assert(bindPtr->ordinal == fixup.bind.bindOrdinal);
                }
            }
        }
        else {
            if ( fixup.authenticated ) {
                dyld_chained_ptr_arm64e_auth_rebase* authRebasePtr = (dyld_chained_ptr_arm64e_auth_rebase*)fixup.location;
                authRebasePtr->auth      = true;
                authRebasePtr->bind      = false;
                authRebasePtr->next      = delta/stride();
                authRebasePtr->key       = fixup.auth.key;
                authRebasePtr->addrDiv   = fixup.auth.usesAddrDiversity;
                authRebasePtr->diversity = fixup.auth.diversity;
                authRebasePtr->target    = fixup.rebase.targetVmOffset;
                assert(authRebasePtr->next*stride() == delta);
                assert(authRebasePtr->target == fixup.rebase.targetVmOffset);
            }
            else {
                dyld_chained_ptr_arm64e_rebase* rebasePtr = (dyld_chained_ptr_arm64e_rebase*)fixup.location;
                uint8_t   high8 = (fixup.rebase.targetVmOffset >> 56);
                uint64_t  low56 = (fixup.rebase.targetVmOffset & 0x00FFFFFFFFFFFFFFULL);
                rebasePtr->auth     = false;
                rebasePtr->bind     = false;
                rebasePtr->next     = delta/stride();
                rebasePtr->high8    = high8;
                rebasePtr->target   = low56 + (this->unauthRebaseIsVmAddr() ? preferedLoadAddress : 0);
                assert(rebasePtr->next*stride() == delta);
                assert(rebasePtr->target == (low56 + (this->unauthRebaseIsVmAddr() ? preferedLoadAddress : 0)));
            }
        }
    }
#endif

protected:
    virtual uint32_t bindBitCount() const = 0;
    virtual uint32_t stride() const = 0;
    virtual bool     unauthRebaseIsVmAddr() const = 0;
};

//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E : public PointerFormat_Generic_arm64e
{
public:
    uint16_t        value() const override                 { return DYLD_CHAINED_PTR_ARM64E; }
    const char*     name() const override                  { return "DYLD_CHAINED_PTR_ARM64E"; }
    const char*     description() const override           { return "authenticated arm64e, 8-byte stride, target vmadddr"; }
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 8; }
    bool            unauthRebaseIsVmAddr() const override  { return true; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL : public PointerFormat_Generic_arm64e
{
public:
    uint16_t        value() const override                 { return DYLD_CHAINED_PTR_ARM64E_KERNEL; }
    const char*     name() const override                  { return "DYLD_CHAINED_PTR_ARM64E_KERNEL"; }
    const char*     description() const override           { return "authenticated arm64e, 4-byte stride, target vmoffset"; }
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 4; }
    bool            unauthRebaseIsVmAddr() const override  { return false; }
};



//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND : public PointerFormat_Generic_arm64e
{
public:
    uint16_t        value() const override                 { return DYLD_CHAINED_PTR_ARM64E_USERLAND; }
    const char*     name() const override                  { return "DYLD_CHAINED_PTR_ARM64E_USERLAND"; }
    const char*     description() const override           { return "authenticated arm64e, 8-byte stride, target vmoffset"; }
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 8; }
    bool            unauthRebaseIsVmAddr() const override  { return false; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND24 ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND24 : public PointerFormat_Generic_arm64e
{
public:
    uint16_t        value() const override                 { return DYLD_CHAINED_PTR_ARM64E_USERLAND24; }
    const char*     name() const override                  { return "DYLD_CHAINED_PTR_ARM64E_USERLAND24"; }
    const char*     description() const override           { return "authenticated arm64e, 8-byte stride, target vmoffset"; }
protected:
    uint32_t        bindBitCount() const override          { return 24; }
    uint32_t        stride() const override                { return 8; }
    bool            unauthRebaseIsVmAddr() const override  { return false; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_FIRMWARE ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_FIRMWARE : public PointerFormat_Generic_arm64e
{
public:
    uint16_t         value() const override                { return DYLD_CHAINED_PTR_ARM64E_FIRMWARE; }
    const char*      name() const override                 { return "DYLD_CHAINED_PTR_ARM64E_FIRMWARE"; }
    const char*      description() const override          { return "authenticated arm64e, 4-byte stride, target vmoffset"; }
    bool             is64() const override                 { return true; }
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 4; }
    bool            unauthRebaseIsVmAddr() const override  { return true; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_64 ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_64 : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_64; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_64"; }
    const char*      description() const override                                { return "generic 64-bit, 4-byte stride, target vmadddr"; }
    bool             is64() const override                                       { return true; }
    bool             supportsAuth() const override                               { return false; }
    uint32_t         minNext() const override                                    { return 4; }
    uint32_t         maxNext() const override                                    { return 4*0xFFF; }
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0xFFFFFFFFFULL; }
    bool             supportsBinds() const override                              { return true; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0x00FFFFFF; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 255; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_64_rebase* ptr = (dyld_chained_ptr_64_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * 4);
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_64_rebase*  rebasePtr = (dyld_chained_ptr_64_rebase*)loc;
        const dyld_chained_ptr_64_bind*    bindPtr   = (dyld_chained_ptr_64_bind*)loc;
        if ( bindPtr->bind )
            return Fixup(loc, seg, bindPtr->ordinal, bindPtr->addend);
        else if ( unauthRebaseIsVmAddr() )
            return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | (rebasePtr->target-preferedLoadAddress));
        else
            return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | rebasePtr->target);
    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        if ( fixup.isBind ) {
            dyld_chained_ptr_64_bind*  bindPtr = (dyld_chained_ptr_64_bind*)fixup.location;
            bindPtr->bind     = true;
            bindPtr->next     = delta/4;
            bindPtr->reserved = 0;
            bindPtr->addend   = fixup.bind.embeddedAddend;
            bindPtr->ordinal  = fixup.bind.bindOrdinal;
            assert(bindPtr->addend == fixup.bind.embeddedAddend);
            assert(bindPtr->next*4 == delta);
            assert(bindPtr->ordinal == fixup.bind.bindOrdinal);
        }
        else if ( unauthRebaseIsVmAddr() ) {
            dyld_chained_ptr_64_rebase* rebasePtr = (dyld_chained_ptr_64_rebase*)fixup.location;
            uint8_t   high8 = (fixup.rebase.targetVmOffset >> 56);
            uint64_t  low56 = (fixup.rebase.targetVmOffset & 0x00FFFFFFFFFFFFFFULL);
            rebasePtr->bind     = false;
            rebasePtr->next     = delta/4;
            rebasePtr->reserved = 0;
            rebasePtr->high8    = high8;
            rebasePtr->target   = low56+preferedLoadAddress;
            assert(rebasePtr->next*4 == delta);
            assert(rebasePtr->target == (low56+preferedLoadAddress));
        }
        else {
            dyld_chained_ptr_64_rebase* rebasePtr = (dyld_chained_ptr_64_rebase*)fixup.location;
            uint8_t   high8 = (fixup.rebase.targetVmOffset >> 56);
            uint64_t  low56 = (fixup.rebase.targetVmOffset & 0x00FFFFFFFFFFFFFFULL);
            rebasePtr->bind     = false;
            rebasePtr->next     = delta/4;
            rebasePtr->reserved = 0;
            rebasePtr->high8    = high8;
            rebasePtr->target   = low56;
            assert(rebasePtr->next*4 == delta);
            assert(rebasePtr->target == low56);
        }
    }
#endif
protected:
    virtual bool    unauthRebaseIsVmAddr() const  { return true; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_32
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_32 : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_32; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_32"; }
    const char*      description() const override                                { return "generic 32-bit, 4-byte stride"; }
    bool             is64() const override                                       { return false; }
    bool             supportsAuth() const override                               { return false; }
    uint32_t         minNext() const override                                    { return 4; }
    uint32_t         maxNext() const override                                    { return 4*0x1F; }
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x03FFFFFF; }
    bool             supportsBinds() const override                              { return true; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0x000FFFFF; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 63; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_32_rebase* ptr = (dyld_chained_ptr_32_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * 4);
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_32_rebase*  rebasePtr = (dyld_chained_ptr_32_rebase*)loc;
        const dyld_chained_ptr_32_bind*    bindPtr   = (dyld_chained_ptr_32_bind*)loc;
        if ( bindPtr->bind )
            return Fixup(loc, seg, bindPtr->ordinal, bindPtr->addend);
        else
            return Fixup(loc, seg, rebasePtr->target);
    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        if ( fixup.isBind ) {
            dyld_chained_ptr_32_bind* bindPtr = (dyld_chained_ptr_32_bind*)fixup.location;
            bindPtr->bind     = true;
            bindPtr->next     = (uint32_t)(delta/4);
            bindPtr->addend   = fixup.bind.embeddedAddend;
            bindPtr->ordinal  = fixup.bind.bindOrdinal;
            assert(bindPtr->next*4 == delta);
            assert(bindPtr->addend == fixup.bind.embeddedAddend);
            assert(bindPtr->ordinal == fixup.bind.bindOrdinal);
        }
        else {
            dyld_chained_ptr_32_rebase*  rebasePtr = (dyld_chained_ptr_32_rebase*)fixup.location;
            rebasePtr->bind     = false;
            rebasePtr->next     = (uint32_t)(delta/4);
            uint64_t target = fixup.rebase.targetVmOffset+preferedLoadAddress;
            rebasePtr->target   = (uint32_t)target;
            assert(rebasePtr->next*4 == delta);
            assert(rebasePtr->target == target);
        }
    }
#endif
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_32_CACHE
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_32_CACHE : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_32_CACHE; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_32_CACHE"; }
    const char*      description() const override                                { return "generic 32-bit, 4-byte stride"; }
    bool             is64() const override                                       { return false; }
    bool             supportsAuth() const override                               { return false; }
    uint32_t         minNext() const override                                    { return 4; }
    uint32_t         maxNext() const override                                    { return 4*3; }
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x3FFFFFFF; }
    bool             supportsBinds() const override                              { return false; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 0; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_32_cache_rebase* ptr = (dyld_chained_ptr_32_cache_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * 4);
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_32_cache_rebase*  rebasePtr = (dyld_chained_ptr_32_cache_rebase*)loc;
        return Fixup(loc, seg, rebasePtr->target);
    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_32_cache_rebase*  rebasePtr = (dyld_chained_ptr_32_cache_rebase*)fixup.location;
        rebasePtr->next     = (uint32_t)(delta/4);
        rebasePtr->target   = (uint32_t)fixup.rebase.targetVmOffset;
        assert(rebasePtr->next*4 == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
#endif
};



//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_32_FIRMWARE
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_32_FIRMWARE : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_32_FIRMWARE; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_32_FIRMWARE"; }
    const char*      description() const override                                { return "generic 32-bit, 4-byte stride"; }
    bool             is64() const override                                       { return false; }
    bool             supportsAuth() const override                               { return false; }
    uint32_t         minNext() const override                                    { return 4; }
    uint32_t         maxNext() const override                                    { return 4*0x1F; }
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x03FFFFFF; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0x000FFFFF; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 0; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }
    bool             supportsBinds() const override                              { return false; }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_32_firmware_rebase* ptr = (dyld_chained_ptr_32_firmware_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * 4);
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_32_firmware_rebase*  rebasePtr = (dyld_chained_ptr_32_firmware_rebase*)loc;
        return Fixup(loc, seg, rebasePtr->target);
    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_32_firmware_rebase*  rebasePtr = (dyld_chained_ptr_32_firmware_rebase*)fixup.location;
        rebasePtr->next     = (uint32_t)(delta/4);
        rebasePtr->target   = (uint32_t)fixup.rebase.targetVmOffset;
        assert(rebasePtr->next*4 == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
#endif
};



//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_64_OFFSET ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_64_OFFSET : public PointerFormat_DYLD_CHAINED_PTR_64
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_64_OFFSET; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_64_OFFSET"; }
    const char*      description() const override                                { return "generic 64-bit, 4-byte stride, target vmoffset"; }
    bool             unauthRebaseIsVmAddr() const override                       { return false; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_64_KERNEL_CACHE ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_64_KERNEL_CACHE : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_64_KERNEL_CACHE; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_64_KERNEL_CACHE"; }
    const char*      description() const override                                { return "authenticated arm64e, 4-byte stride, for kernel cache"; }
    bool             is64() const override                                       { return true; }
    bool             supportsAuth() const override                               { return true; }
    uint32_t         minNext() const override                                    { return 4; }
    uint32_t         maxNext() const override                                    { return 4*0xFFF; }  // 12-bits
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x3FFFFFFF;  }
    bool             supportsBinds() const override                              { return false; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 0; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }
    
    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* ptr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * 4);
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
       if ( rebasePtr->isAuth )
           return Fixup(loc, seg, rebasePtr->target, rebasePtr->key, rebasePtr->addrDiv, rebasePtr->diversity);
       else
           return Fixup(loc, seg, rebasePtr->target);

    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)fixup.location;
      
        rebasePtr->isAuth     = fixup.authenticated ;
        rebasePtr->next       = delta/4;
        rebasePtr->key        = fixup.auth.key;
        rebasePtr->addrDiv    = fixup.auth.usesAddrDiversity;
        rebasePtr->diversity  = fixup.auth.diversity;
        rebasePtr->cacheLevel = 0;  // FIXME
        rebasePtr->target     = fixup.rebase.targetVmOffset;
        assert(rebasePtr->next*4 == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
#endif
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE : public ChainedFixups::PointerFormat
{
public:
    uint16_t         value() const override                                      { return DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE; }
    const char*      name() const override                                       { return "DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE"; }
    const char*      description() const override                                { return "1-byte stride, for x86_64 kernel cache"; }
    bool             is64() const override                                       { return true; }
    bool             supportsAuth() const override                               { return true; }
    uint32_t         minNext() const override                                    { return 1; }
    uint32_t         maxNext() const override                                    { return 1*0xFFF; }  // 12-bits
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x3FFFFFFF;  }
    bool             supportsBinds() const override                              { return false; }
    uint32_t         maxBindOrdinal(bool authenticated) const override           { return 0; }
    int32_t          bindMaxEmbeddableAddend(bool authenticated) const override  { return 0; }
    int32_t          bindMinEmbeddableAddend(bool authenticated) const override  { return 0; }

    const void*      nextLocation(const void* loc) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* ptr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next);
    }

    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
       if ( rebasePtr->isAuth )
           return Fixup(loc, seg, rebasePtr->target, rebasePtr->key, rebasePtr->addrDiv, rebasePtr->diversity);
       else
           return Fixup(loc, seg, rebasePtr->target);

    }
#if BUILDING_MACHO_WRITER
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)fixup.location;

        rebasePtr->isAuth     = false;
        rebasePtr->next       = delta;
        rebasePtr->key        = 0;
        rebasePtr->addrDiv    = 0;
        rebasePtr->diversity  = 0;
        rebasePtr->cacheLevel = 0;  // FIXME
        rebasePtr->target     = fixup.rebase.targetVmOffset;
        assert(rebasePtr->next == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
#endif
};


bool ChainedFixups::PointerFormat::valid(uint16_t pointer_format)
{
    return (pointer_format <= DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE);
}


const ChainedFixups::PointerFormat& ChainedFixups::PointerFormat::make(uint16_t pointer_format)
{
    static const PointerFormat_DYLD_CHAINED_PTR_ARM64E              p1;
    static const PointerFormat_DYLD_CHAINED_PTR_64                  p2;
    static const PointerFormat_DYLD_CHAINED_PTR_32                  p3;
    static const PointerFormat_DYLD_CHAINED_PTR_32_CACHE            p4;
    static const PointerFormat_DYLD_CHAINED_PTR_32_FIRMWARE         p5;
    static const PointerFormat_DYLD_CHAINED_PTR_64_OFFSET           p6;
    static const PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL       p7;
    static const PointerFormat_DYLD_CHAINED_PTR_64_KERNEL_CACHE     p8;
    static const PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND     p9;
    static const PointerFormat_DYLD_CHAINED_PTR_ARM64E_FIRMWARE     p10;
    static const PointerFormat_DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE p11;
    static const PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND24   p12;

    switch (pointer_format) {
        case DYLD_CHAINED_PTR_ARM64E:
            return p1;
        case DYLD_CHAINED_PTR_64 :
            return p2;
        case DYLD_CHAINED_PTR_32:
            return p3;
        case DYLD_CHAINED_PTR_32_CACHE:
            return p4;
        case DYLD_CHAINED_PTR_32_FIRMWARE:
            return p5;
        case DYLD_CHAINED_PTR_64_OFFSET:
            return p6;
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
            return p7;
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
            return p8;
        case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
            return p9;
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
            return p10;
        case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
            return p11;
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            return p12;
    }
    assert("unknown pointer format");
    return p1;
}


} // namespace mach_o
