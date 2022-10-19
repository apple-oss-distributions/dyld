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

#include <assert.h>

#include "ASLRTracker.h"
#include "MachOFileAbstraction.hpp"
#include "DyldSharedCache.h"
#include "CacheBuilder.h"
#include "Diagnostics.h"
#include "IMPCaches.hpp"

CacheBuilder::CacheBuilder(const DyldSharedCache::CreateOptions& options, const dyld3::closure::FileSystem& fileSystem)
    : _options(options)
    , _fileSystem(fileSystem)
    , _fullAllocatedBuffer(0)
    , _diagnostics(options.loggingPrefix, options.verbose)
    , _allocatedBufferSize(0)
{
}

CacheBuilder::~CacheBuilder() {
}


std::string CacheBuilder::errorMessage()
{
    return _diagnostics.errorMessage();
}

void CacheBuilder::copyRawSegments()
{
    const bool log = false;

    forEachDylibInfo(^(const DylibInfo& dylib, Diagnostics& dylibDiag, cache_builder::ASLR_Tracker& dylibASLRTracker,
                       const CacheBuilder::DylibSectionCoalescer* sectionCoalescer) {
        for (const SegmentMappingInfo& info : dylib.cacheLocation) {
            if (log) fprintf(stderr, "copy %s segment %15s (0x%08X bytes) from %p to %p (logical addr 0x%llX) for %s\n",
                             _options.archs->name(), info.segName, info.copySegmentSize, info.srcSegment, info.dstSegment, info.dstCacheUnslidAddress, dylib.input->mappedFile.runtimePath.c_str());
            ::memcpy(info.dstSegment, info.srcSegment, info.copySegmentSize);
        }
    });

    // Copy the coalesced __TEXT sections
    for ( const auto* coalescedSection : { &_objcCoalescedClassNames, &_objcCoalescedMethodNames, &_objcCoalescedMethodTypes } ) {
        if ( coalescedSection->bufferSize == 0 )
            continue;

        if (log) {
            fprintf(stderr, "copy %s __TEXT_COAL section %s (0x%08X bytes) to %p (logical addr 0x%llX)\n",
                    _options.archs->name(), coalescedSection->sectionName.data(),
                    coalescedSection->bufferSize, coalescedSection->bufferAddr, coalescedSection->bufferVMAddr);
        }
        for (const auto& stringAndOffset : coalescedSection->stringsToOffsets) {
            ::memcpy(coalescedSection->bufferAddr + stringAndOffset.second,
                     stringAndOffset.first.data(), stringAndOffset.first.size() + 1);
        }
    }
}

void CacheBuilder::adjustAllImagesForNewSegmentLocations(uint64_t cacheBaseAddress,
                                                         LOH_Tracker* lohTracker)
{
    // Note this cannot to be done in parallel because the LOH Tracker and aslr tracker are not thread safe
    __block bool badDylib = false;
    forEachDylibInfo(^(const DylibInfo& dylib, Diagnostics& dylibDiag,
                       cache_builder::ASLR_Tracker& dylibASLRTracker,
                       const CacheBuilder::DylibSectionCoalescer* sectionCoalescer) {
        if ( dylibDiag.hasError() )
            return;
        adjustDylibSegments(dylib, dylibDiag, cacheBaseAddress, dylibASLRTracker,
                            lohTracker, sectionCoalescer);
        if ( dylibDiag.hasError() )
            badDylib = true;
    });

    if ( badDylib && !_diagnostics.hasError() ) {
        _diagnostics.error("One or more binaries has an error which prevented linking.  See other errors.");
    }
}

////////////////////////////  CacheBuilder::DylibSectionCoalescer ////////////////////////////////////

// Returns true if the section was removed from the source dylib after being optimized
bool CacheBuilder::DylibSectionCoalescer::sectionWasRemoved(std::string_view segmentName,
                                                            std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) ) {
        // Some sections, eg, GOTs, are optimized but not removed
        if ( !section->sectionWillBeRemoved )
            return false;

        return !section->offsetMap.empty();
    }

    return false;
}

// Returns true if the section was totally removed, and hasn't been redirected to some coalesced or
// optimized location
bool CacheBuilder::DylibSectionCoalescer::sectionWasObliterated(std::string_view segmentName,
                                                                std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) ) {
        return section->sectionIsObliterated;
    }

    return false;
}

// Returns true if the section was optimized.  It may or may not have been removed too, see sectionWasRemoved().
bool CacheBuilder::DylibSectionCoalescer::sectionWasOptimized(std::string_view segmentName,
                                                              std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) )
        return !section->offsetMap.empty() || section->sectionIsObliterated;

    return false;
}

CacheBuilder::DylibSectionCoalescer::OptimizedSection*
CacheBuilder::DylibSectionCoalescer::getSection(std::string_view segmentName, std::string_view sectionName)
{
    if (segmentName.size() > 16)
        segmentName = segmentName.substr(0, 16);
    if (sectionName.size() > 16)
        sectionName = sectionName.substr(0, 16);

    if ( segmentName == "__TEXT" ) {
        if ( sectionName == "__objc_classname" )
            return &this->objcClassNames;
        if ( sectionName == "__objc_methname" )
            return &this->objcMethNames;
        if ( sectionName == "__objc_methtype" )
            return &this->objcMethTypes;
    } else if ( segmentName == "__TEXT_EXEC" ) {
        if ( sectionName == "__auth_stubs" )
            return &this->auth_stubs;
    } else if ( segmentName == "__DATA_CONST" ) {
        if ( sectionName == "__got" )
            return &this->gots;
    } else if ( segmentName == "__AUTH_CONST" ) {
        if ( sectionName == "__auth_got" )
            return &this->auth_gots;
    }

    return nullptr;
}

const CacheBuilder::DylibSectionCoalescer::OptimizedSection*
CacheBuilder::DylibSectionCoalescer::getSection(std::string_view segmentName, std::string_view sectionName) const
{
    return ((CacheBuilder::DylibSectionCoalescer*)this)->getSection(segmentName, sectionName);
}

void CacheBuilder::DylibSectionCoalescer::clear()
{
    objcClassNames.clear();
    objcMethNames.clear();
    objcMethTypes.clear();
    auth_stubs.clear();
    gots.clear();
    auth_gots.clear();
}
