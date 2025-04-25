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

const dyld_chained_fixups_header* ChainedFixups::linkeditHeader() const
{
    return _fixupsHeader;
}

const dyld_chained_starts_offsets* ChainedFixups::startsSectionHeader() const
{
    return _chainStartsHeader;
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

void ChainedFixups::forEachFixupChainStartLocation(std::span<const MappedSegment> segments,
                                                   void (^callback)(const void* loc, uint32_t segIndex, uint32_t pageIndex, uint32_t pageSize, const PointerFormat&, bool& stop)) const
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
                        uint16_t       startOffset = (segStarts->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                        const uint8_t* chainStart  = (uint8_t*)(segments[segIndex].content) + pageIndex * segStarts->page_size + startOffset;
                        callback(chainStart, segIndex, pageIndex, segStarts->page_size, pf, stop);
                        ++overflowIndex;
                    }
                }
                else {
                    const uint8_t* chainStart = ((uint8_t*)(segments[segIndex].content)) + pageIndex * segStarts->page_size + offsetInPage;
                    callback(chainStart, segIndex, pageIndex, segStarts->page_size, pf, stop);
                }
                if ( stop )
                    return;
            }
        }
    }
}


Error ChainedFixups::validLinkedit(uint64_t preferredLoadAddress, std::span<const MappedSegment> segments) const
{
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
    if ( (_fixupsHeader->imports_count != 0) && (_fixupsHeader->imports_count >= maxBindOrdinal) )
        return Error("chained fixups, imports_count (%d) exceeds max of %d", _fixupsHeader->imports_count, maxBindOrdinal);

    // validate max_valid_pointer is larger than last segment
    if ( maxValidPointerSeen != 0 ) {
        size_t lastDataSegmentIndex = segments.size() - (segments.back().segName == "__LINKEDIT" ? 2 : 1);
        const MappedSegment& lastDataSegment = segments[lastDataSegmentIndex];
        // note: runtime offset is relative to the load address but max valid pointer encodes an 'absolute' valid pointer
        uint64_t lastDataSegmentLastVMAddr = preferredLoadAddress + lastDataSegment.runtimeOffset + lastDataSegment.runtimeSize;
        if ( maxValidPointerSeen < lastDataSegmentLastVMAddr )
            return Error("chained fixups, max_valid_pointer (0x%x) too small for image last vm address 0x%llx", maxValidPointerSeen, lastDataSegmentLastVMAddr);
    }
    return Error::none();
}

Error ChainedFixups::validStartsSection(std::span<const MappedSegment> segments) const
{
    // validate dyld_chained_starts_offsets
    if ( !PointerFormat::valid(_chainStartsHeader->pointer_format) ) {
        return Error("chained fixups, unknown pointer_format (%d)", _chainStartsHeader->pointer_format);
    }
    return Error::none();
}

Error ChainedFixups::valid(uint64_t preferredLoadAddress, std::span<const MappedSegment> segments,
                           bool startsInSection) const
{
    if ( startsInSection ) {
        return validStartsSection(segments);
    } else {
        return validLinkedit(preferredLoadAddress, segments);
    }
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
                                                                std::span<const uint64_t> segOffsetTable, uint32_t pageIndex, uint32_t pageSize,
                                                                void (^callback)(const Fixup& f, bool& stop)) const
{
    bool         stop    = false;
    const void*  nextLoc = nullptr;
    const void*  endPage = nullptr;
    // note: seg is null for firmware and firmware does not require chains to be limited to one page
    if ( seg != nullptr ) {
        const void*  startPage = (void*)((uint8_t*)seg->content + pageIndex*pageSize);
        endPage = (void*)((uint8_t*)startPage + pageSize);
        if ( (chainStartLoc < startPage) || (chainStartLoc > endPage) )
            return; // error: chain is not on page
    }
    for ( const void* fixupLoc = chainStartLoc; (fixupLoc != nullptr) && !stop; fixupLoc = nextLoc) {
        // get next before calling callback, because callback may update location (change PointerFormat bits into runtime pointer)
        nextLoc = this->nextLocation(fixupLoc);
        callback(this->parseChainEntry(fixupLoc, seg, prefLoadAddr, segOffsetTable), stop);
        if ( (nextLoc != nullptr) && (endPage != nullptr) ) {
            if ( nextLoc > endPage )
                break; // error: chain went off end of page
        }
    }
}


// copy of dyld_chained_ptr_arm64e_rebase that allows 4-byte strides
struct __attribute__((packed)) unaligned_dyld_chained_ptr_arm64e_rebase
{
    uint64_t    target   : 43,
                high8    :  8,
                next     : 11,    // 4 or 8-byte stide
                bind     :  1,    // == 0
                auth     :  1;    // == 0
};

// copy of dyld_chained_ptr_arm64e_auth_rebase that allows 4-byte strides
struct __attribute__((packed)) unaligned_dyld_chained_ptr_arm64e_auth_rebase
{
    uint64_t    target    : 32,   // runtimeOffset
                diversity : 16,
                addrDiv   :  1,
                key       :  2,
                next      : 11,    // 4 or 8-byte stide
                bind      :  1,    // == 0
                auth      :  1;    // == 1
};


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
        const unaligned_dyld_chained_ptr_arm64e_rebase* ptr = (unaligned_dyld_chained_ptr_arm64e_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * stride());
    }
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        if ( ((unaligned_dyld_chained_ptr_arm64e_rebase*)loc)->bind ) {
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
            const unaligned_dyld_chained_ptr_arm64e_auth_rebase* authRebasePtr = (unaligned_dyld_chained_ptr_arm64e_auth_rebase*)loc;
            const dyld_chained_ptr_arm64e_rebase*                rebasePtr     = (dyld_chained_ptr_arm64e_rebase*)loc;
            if ( authRebasePtr->auth )
               return Fixup(loc, seg, authRebasePtr->target, authRebasePtr->key, authRebasePtr->addrDiv, authRebasePtr->diversity);
            else if ( unauthRebaseIsVmAddr() )
                return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | (rebasePtr->target - preferedLoadAddress));
            else
                return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | rebasePtr->target);
        }
    }

    static int64_t   signExtendedAddend(dyld_chained_ptr_arm64e_bind24* bind)
    {
        uint64_t addend19 = bind->addend;
        if ( addend19 & 0x40000 )
            return addend19 | 0xFFFFFFFFFFFC0000ULL;
        else
            return addend19;
    }

    static int64_t   signExtendedAddend(dyld_chained_ptr_arm64e_bind* bind)
    {
        uint64_t addend27     = bind->addend;
        uint64_t top8Bits     = addend27 & 0x00007F80000ULL;
        uint64_t bottom19Bits = addend27 & 0x0000007FFFFULL;
        uint64_t newValue     = (top8Bits << 13) | (((uint64_t)(bottom19Bits << 37) >> 37) & 0x00FFFFFFFFFFFFFF);
        return newValue;
    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
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
                    assert(signExtendedAddend(bind24Ptr) == fixup.bind.embeddedAddend);
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
                    assert(signExtendedAddend(bindPtr) == fixup.bind.embeddedAddend);
                    assert(bindPtr->next*stride() == delta);
                    assert(bindPtr->ordinal == fixup.bind.bindOrdinal);
                }
            }
        }
        else {
            if ( fixup.authenticated ) {
                unaligned_dyld_chained_ptr_arm64e_auth_rebase* authRebasePtr = (unaligned_dyld_chained_ptr_arm64e_auth_rebase*)fixup.location;
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
                unaligned_dyld_chained_ptr_arm64e_rebase* rebasePtr = (unaligned_dyld_chained_ptr_arm64e_rebase*)fixup.location;
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
    uint32_t        ptrAlignmentSize() const override      { return 8; } // arm64e userspace requires 8-byte ptr alignment
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 8; }
    bool            unauthRebaseIsVmAddr() const override  { return true; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL ---
//
class VIS_HIDDEN  __attribute__((__packed__)) PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL : public PointerFormat_Generic_arm64e
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
    uint32_t        ptrAlignmentSize() const override      { return 8; } // arm64e userspace requires 8-byte ptr alignment
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
    uint32_t        ptrAlignmentSize() const override      { return 8; } // arm64e userspace requires 8-byte ptr alignment
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
    const char*      description() const override          { return "authenticated arm64e, 4-byte stride, target vmaddr"; }
    bool             is64() const override                 { return true; }
protected:
    uint32_t        bindBitCount() const override          { return 16; }
    uint32_t        stride() const override                { return 4; }
    bool            unauthRebaseIsVmAddr() const override  { return true; }
};


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE : public PointerFormat_Generic_arm64e
{
public:
    bool            supportsBinds() const override         { return false; }
    uint16_t        value() const override                 { return DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE; }
    const char*     name() const override                  { return "PointerFormat_DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE"; }
    const char*     description() const override           { return "arm64e shared cache, 8-byte stride, target vmoffset"; }
    uint32_t        ptrAlignmentSize() const override      { return 8; } // arm64e userspace requires 8-byte ptr alignment
    uint64_t        maxRebaseTargetOffset(bool auth) const override    { return 0x3FFFFFFFFULL; }
    void            writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override;
protected:
    uint32_t        bindBitCount() const override          { return 0; }
    uint32_t        stride() const override                { return 8; }
    bool            unauthRebaseIsVmAddr() const override  { return false; }

    const void*     nextLocation(const void* loc) const override {
        const dyld_chained_ptr_arm64e_shared_cache_rebase* ptr = (dyld_chained_ptr_arm64e_shared_cache_rebase*)loc;
        if ( ptr->next == 0 )
            return nullptr;
        return (void*)((uint8_t*)loc + ptr->next * stride());
    }

    Fixup           parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_arm64e_shared_cache_auth_rebase*     authRebasePtr = (dyld_chained_ptr_arm64e_shared_cache_auth_rebase*)loc;
        const dyld_chained_ptr_arm64e_shared_cache_rebase*          rebasePtr     = (dyld_chained_ptr_arm64e_shared_cache_rebase*)loc;
        if ( authRebasePtr->auth ) {
            uint8_t key = (authRebasePtr->keyIsData ? ptrauth_key_asda : ptrauth_key_asia);
            return Fixup(loc, seg, authRebasePtr->runtimeOffset, key, authRebasePtr->addrDiv, authRebasePtr->diversity);
        }
        else {
            return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | rebasePtr->runtimeOffset);
        }
    }

};

void PointerFormat_DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE::writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const
{
    assert(!fixup.isBind && "shared cache does not support binds");
    intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
    if ( fixup.authenticated ) {
        dyld_chained_ptr_arm64e_shared_cache_auth_rebase* authRebasePtr = (dyld_chained_ptr_arm64e_shared_cache_auth_rebase*)fixup.location;
        authRebasePtr->auth          = 1;
        authRebasePtr->next          = (uint32_t)(delta/8);
        authRebasePtr->keyIsData     = (fixup.auth.key == ptrauth_key_asia ? 0 : 1);
        authRebasePtr->addrDiv       = fixup.auth.usesAddrDiversity;
        authRebasePtr->diversity     = fixup.auth.diversity;
        authRebasePtr->runtimeOffset = (fixup.rebase.targetVmOffset & 0x3FFFFFFFFULL);
        assert(authRebasePtr->next*8 == delta);
        assert(authRebasePtr->runtimeOffset == fixup.rebase.targetVmOffset);
    }
    else {
        dyld_chained_ptr_arm64e_shared_cache_rebase* rebasePtr = (dyld_chained_ptr_arm64e_shared_cache_rebase*)fixup.location;
        rebasePtr->auth          = 0;
        rebasePtr->next          = (uint32_t)(delta/8);
        rebasePtr->unused        = 0;
        rebasePtr->high8         = ((fixup.rebase.targetVmOffset >> 56) & 0xFF);
        rebasePtr->runtimeOffset = (fixup.rebase.targetVmOffset & 0x3FFFFFFFFULL);
        assert(rebasePtr->next*8 == delta);
    }
}


//
// MARK: --- PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED ---
//
class VIS_HIDDEN PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED : public PointerFormat_Generic_arm64e
{
public:
    bool             supportsBinds() const override        { return false; }
    uint16_t         value() const override                { return DYLD_CHAINED_PTR_ARM64E_SEGMENTED; }
    const char*      name() const override                 { return "DYLD_CHAINED_PTR_ARM64E_SEGMENTED"; }
    const char*      description() const override          { return "authenticated arm64e, 8-byte stride, target segIndex/offset8"; }
    bool             is64() const override                 { return true; }
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override;
    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override;
    const void*      nextLocation(const void* loc) const override;
    uint64_t         maxRebaseTargetOffset(bool authenticated) const override    { return 0x0FFFFFFF;  }
protected:
    uint32_t        bindBitCount() const override          { return 0; }
    uint32_t        stride() const override                { return 4; }
    bool            unauthRebaseIsVmAddr() const override  { return true; }
};

const void* PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED::nextLocation(const void* loc) const
{
    const dyld_chained_ptr_arm64e_segmented_rebase* ptr = (dyld_chained_ptr_arm64e_segmented_rebase*)loc;
    if ( ptr->next == 0 )
        return nullptr;
    return (void*)((uint8_t*)loc + ptr->next * stride());
}

Fixup PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED::parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const
{
    const dyld_chained_ptr_arm64e_auth_segmented_rebase* authSegRebasePtr = (dyld_chained_ptr_arm64e_auth_segmented_rebase*)loc;
    const dyld_chained_ptr_arm64e_segmented_rebase*      segRebasePtr     = (dyld_chained_ptr_arm64e_segmented_rebase*)loc;
    if ( authSegRebasePtr->auth ) {
        uint64_t targetVMOffset = segOffsetTable[authSegRebasePtr->targetSegIndex] + authSegRebasePtr->targetSegOffset;
        return Fixup(loc, seg, targetVMOffset, authSegRebasePtr->key, authSegRebasePtr->addrDiv, authSegRebasePtr->diversity);
    }
    else {
        uint64_t targetVMOffset = segOffsetTable[segRebasePtr->targetSegIndex] + segRebasePtr->targetSegOffset;
        return Fixup(loc, seg, targetVMOffset);
    }
}

static bool findSegIndexAndOffset(std::span<const MappedSegment*> segments, uint64_t vmOffset, uint8_t& segIndex, uint64_t& segOffset)
{
    int index=0;
    for (const MappedSegment* seg : segments) {
        if ( (seg->runtimeOffset <= vmOffset) && (vmOffset <= (seg->runtimeOffset+seg->runtimeSize)) ) {
            segIndex  = index;
            segOffset = vmOffset - seg->runtimeOffset;
            return true;
        }
        ++index;
    }
    return false;
}


void PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED::writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*> segments) const
{
    intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
    assert(!fixup.isBind && "firmware format does not support binds");
    uint8_t  segIndex;
    uint64_t segOffset;
    bool found = findSegIndexAndOffset(segments, fixup.rebase.targetVmOffset, segIndex, segOffset);
    assert(found && "target vm address not in any segment");

    if ( fixup.authenticated ) {
        //fprintf(stderr, "key=%d, addr=%d, div=0x%04X\n", fixup.auth.key, fixup.auth.usesAddrDiversity, fixup.auth.diversity);
        dyld_chained_ptr_arm64e_auth_segmented_rebase* authRebasePtr = (dyld_chained_ptr_arm64e_auth_segmented_rebase*)fixup.location;
        authRebasePtr->auth             = true;
        authRebasePtr->next             = (uint32_t)(delta/stride());
        authRebasePtr->key              = fixup.auth.key;
        authRebasePtr->addrDiv          = fixup.auth.usesAddrDiversity;
        authRebasePtr->diversity        = fixup.auth.diversity;
        authRebasePtr->targetSegIndex   = segIndex;
        authRebasePtr->targetSegOffset  = (uint32_t)segOffset;
        assert(authRebasePtr->next*stride()   == delta);
        assert(authRebasePtr->targetSegIndex  == segIndex);
        assert(authRebasePtr->targetSegOffset == segOffset);
    }
    else {
        //fprintf(stderr, "segIndex=%d, segOffset=0x%0llX\n", segIndex, segOffset);
        dyld_chained_ptr_arm64e_segmented_rebase* rebasePtr = (dyld_chained_ptr_arm64e_segmented_rebase*)fixup.location;
        rebasePtr->auth             = false;
        rebasePtr->next             = (uint32_t)(delta/stride());
        rebasePtr->padding          = 0;
        rebasePtr->targetSegIndex   = segIndex;
        rebasePtr->targetSegOffset  = (uint32_t)segOffset;
        assert(rebasePtr->next*stride()   == delta);
        assert(rebasePtr->targetSegIndex  == segIndex);
        assert(rebasePtr->targetSegOffset == segOffset);
    }
}


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
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_64_rebase*  rebasePtr = (dyld_chained_ptr_64_rebase*)loc;
        const dyld_chained_ptr_64_bind*    bindPtr   = (dyld_chained_ptr_64_bind*)loc;
        if ( bindPtr->bind )
            return Fixup(loc, seg, bindPtr->ordinal, bindPtr->addend);
        else if ( unauthRebaseIsVmAddr() )
            return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | (rebasePtr->target-preferedLoadAddress));
        else
            return Fixup(loc, seg, ((uint64_t)(rebasePtr->high8) << 56) | rebasePtr->target);
    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
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
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_32_rebase*  rebasePtr = (dyld_chained_ptr_32_rebase*)loc;
        const dyld_chained_ptr_32_bind*    bindPtr   = (dyld_chained_ptr_32_bind*)loc;
        if ( bindPtr->bind )
            return Fixup(loc, seg, bindPtr->ordinal, bindPtr->addend);
        else
            return Fixup(loc, seg, rebasePtr->target);
    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
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
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_32_cache_rebase*  rebasePtr = (dyld_chained_ptr_32_cache_rebase*)loc;
        return Fixup(loc, seg, rebasePtr->target);
    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_32_cache_rebase*  rebasePtr = (dyld_chained_ptr_32_cache_rebase*)fixup.location;
        rebasePtr->next     = (uint32_t)(delta/4);
        rebasePtr->target   = (uint32_t)fixup.rebase.targetVmOffset;
        assert(rebasePtr->next*4 == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
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
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_32_firmware_rebase*  rebasePtr = (dyld_chained_ptr_32_firmware_rebase*)loc;
        return Fixup(loc, seg, rebasePtr->target - preferedLoadAddress);
    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
        intptr_t delta = (nextLoc == nullptr) ? 0 : ((uint8_t*)nextLoc - (uint8_t*)fixup.location);
        dyld_chained_ptr_32_firmware_rebase*  rebasePtr = (dyld_chained_ptr_32_firmware_rebase*)fixup.location;
        rebasePtr->next     = (uint32_t)(delta/4);
        rebasePtr->target   = (uint32_t)fixup.rebase.targetVmOffset;
        assert(rebasePtr->next*4 == delta);
        assert(rebasePtr->target == fixup.rebase.targetVmOffset);
    }
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
    
    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
       if ( rebasePtr->isAuth )
           return Fixup(loc, seg, rebasePtr->target, rebasePtr->key, rebasePtr->addrDiv, rebasePtr->diversity);
       else
           return Fixup(loc, seg, rebasePtr->target);

    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
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

    Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress, std::span<const uint64_t> segOffsetTable) const override {
        const dyld_chained_ptr_64_kernel_cache_rebase* rebasePtr = (dyld_chained_ptr_64_kernel_cache_rebase*)loc;
       if ( rebasePtr->isAuth )
           return Fixup(loc, seg, rebasePtr->target, rebasePtr->key, rebasePtr->addrDiv, rebasePtr->diversity);
       else
           return Fixup(loc, seg, rebasePtr->target);

    }

    void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress, std::span<const MappedSegment*>) const override {
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
};


bool ChainedFixups::PointerFormat::valid(uint16_t pointer_format)
{
    return (pointer_format <= DYLD_CHAINED_PTR_ARM64E_SEGMENTED);
}

uint32_t ChainedFixups::PointerFormat::ptrAlignmentSize() const
{
    // most formats, including 64-bit, allow a 4-byte pointer alignment
    return 4;
}

static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E              p1;
static const constinit PointerFormat_DYLD_CHAINED_PTR_64                  p2;
static const constinit PointerFormat_DYLD_CHAINED_PTR_32                  p3;
static const constinit PointerFormat_DYLD_CHAINED_PTR_32_CACHE            p4;
static const constinit PointerFormat_DYLD_CHAINED_PTR_32_FIRMWARE         p5;
static const constinit PointerFormat_DYLD_CHAINED_PTR_64_OFFSET           p6;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_KERNEL       p7;
static const constinit PointerFormat_DYLD_CHAINED_PTR_64_KERNEL_CACHE     p8;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND     p9;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_FIRMWARE     p10;
static const constinit PointerFormat_DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE p11;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_USERLAND24   p12;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE p13;
static const constinit PointerFormat_DYLD_CHAINED_PTR_ARM64E_SEGMENTED    p14;


const ChainedFixups::PointerFormat& ChainedFixups::PointerFormat::make(uint16_t pointer_format)
{
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
        case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
            return p13;
        case DYLD_CHAINED_PTR_ARM64E_SEGMENTED:
            return p14;
    }
    assert("unknown pointer format");
    return p1;
}

} // namespace mach_o
