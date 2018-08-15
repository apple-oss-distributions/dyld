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


#include <unistd.h>
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach/shared_region.h>
#include <assert.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonDigestSPI.h>
#include <pthread/pthread.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "MachOParser.h"
#include "CodeSigningTypes.h"
#include "DyldSharedCache.h"
#include "CacheBuilder.h"
#include "FileAbstraction.hpp"
#include "LaunchCacheWriter.h"
#include "Trie.hpp"
#include "Diagnostics.h"
#include "ImageProxy.h"

#if __has_include("dyld_cache_config.h")
    #include "dyld_cache_config.h"
#else
    #define ARM_SHARED_REGION_START    0x1A000000ULL
    #define ARM_SHARED_REGION_SIZE     0x26000000ULL
    #define ARM64_SHARED_REGION_START 0x180000000ULL
    #define ARM64_SHARED_REGION_SIZE   0x40000000ULL
#endif

const CacheBuilder::ArchLayout CacheBuilder::_s_archLayout[] = {
    { 0x7FFF20000000ULL,         0xEFE00000ULL,             0x40000000, 0xFFFF000000000000, "x86_64",  0,          0,          0,          12, true,  true  },
    { 0x7FFF20000000ULL,         0xEFE00000ULL,             0x40000000, 0xFFFF000000000000, "x86_64h", 0,          0,          0,          12, true,  true  },
    { SHARED_REGION_BASE_I386,   SHARED_REGION_SIZE_I386,   0x00200000,                0x0, "i386",    0,          0,          0,          12, false, false },
    { ARM64_SHARED_REGION_START, ARM64_SHARED_REGION_SIZE,  0x02000000, 0x00FFFF0000000000, "arm64",   0x0000C000, 0x00100000, 0x07F00000, 14, false, true  },
    { ARM64_SHARED_REGION_START, ARM64_SHARED_REGION_SIZE,  0x02000000, 0x00FFFF0000000000, "arm64e",  0x0000C000, 0x00100000, 0x07F00000, 14, false, true  },
    { ARM_SHARED_REGION_START,   ARM_SHARED_REGION_SIZE,    0x02000000,         0xE0000000, "armv7s",  0,          0,          0,          14, false, false },
    { ARM_SHARED_REGION_START,   ARM_SHARED_REGION_SIZE,    0x00400000,         0xE0000000, "armv7k",  0,          0,          0,          14, false, false },
    { 0x40000000,                0x40000000,                0x02000000,                0x0, "sim-x86", 0,          0,          0,          14, false, false }
};


// These are dylibs that may be interposed, so stubs calling into them should never be bypassed
const char* const CacheBuilder::_s_neverStubEliminate[] = {
    "/usr/lib/system/libdispatch.dylib",
    nullptr
};


CacheBuilder::CacheBuilder(const DyldSharedCache::CreateOptions& options)
    : _options(options)
    , _buffer(nullptr)
    , _diagnostics(options.loggingPrefix, options.verbose)
    , _archLayout(nullptr)
    , _aliasCount(0)
    , _slideInfoFileOffset(0)
    , _slideInfoBufferSizeAllocated(0)
    , _allocatedBufferSize(0)
    , _currentFileSize(0)
    , _vmSize(0)
    , _branchPoolsLinkEditStartAddr(0)
{

    std::string targetArch = options.archName;
    if ( options.forSimulator && (options.archName == "i386") )
        targetArch = "sim-x86";

    for (const ArchLayout& layout : _s_archLayout) {
        if ( layout.archName == targetArch ) {
            _archLayout = &layout;
            break;
        }
    }
}


std::string CacheBuilder::errorMessage()
{
    return _diagnostics.errorMessage();
}

const std::set<std::string> CacheBuilder::warnings()
{
    return _diagnostics.warnings();
}

const std::set<const mach_header*> CacheBuilder::evictions()
{
    return _evictions;
}

void CacheBuilder::deleteBuffer()
{
    vm_deallocate(mach_task_self(), (vm_address_t)_buffer, _allocatedBufferSize);
    _buffer = nullptr;
    _allocatedBufferSize = 0;
}

std::vector<DyldSharedCache::MappedMachO>
CacheBuilder::makeSortedDylibs(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const std::unordered_map<std::string, unsigned> sortOrder)
{
    std::vector<DyldSharedCache::MappedMachO> sortedDylibs = dylibs;

    std::sort(sortedDylibs.begin(), sortedDylibs.end(), [&](const DyldSharedCache::MappedMachO& a, const DyldSharedCache::MappedMachO& b) {
        const auto& orderA = sortOrder.find(a.runtimePath);
        const auto& orderB = sortOrder.find(b.runtimePath);
        bool foundA = (orderA != sortOrder.end());
        bool foundB = (orderB != sortOrder.end());

        // Order all __DATA_DIRTY segments specified in the order file first, in
        // the order specified in the file, followed by any other __DATA_DIRTY
        // segments in lexicographic order.
        if ( foundA && foundB )
            return orderA->second < orderB->second;
        else if ( foundA )
            return true;
        else if ( foundB )
             return false;
        else
             return a.runtimePath < b.runtimePath;
    });

    return sortedDylibs;
}


inline uint32_t absolutetime_to_milliseconds(uint64_t abstime)
{
    return (uint32_t)(abstime/1000/1000);
}

struct DylibAndSize
{
    const char*     installName;
    uint64_t        size;
};

bool CacheBuilder::cacheOverflow(const dyld_cache_mapping_info regions[3])
{
    if ( _archLayout->sharedRegionsAreDiscontiguous ) {
        // for macOS x86_64 cache, need to check each region for overflow
        return ( (regions[0].size > 0x60000000) || (regions[1].size > 0x40000000) || (regions[2].size > 0x3FE00000) );
    }
    else {
        return (_vmSize > _archLayout->sharedMemorySize);
    }
}

void CacheBuilder::build(const std::vector<DyldSharedCache::MappedMachO>& dylibs,
                         const std::vector<DyldSharedCache::MappedMachO>& otherOsDylibsInput,
                         const std::vector<DyldSharedCache::MappedMachO>& osExecutables)
{
    // <rdar://problem/21317611> error out instead of crash if cache has no dylibs
    // FIXME: plist should specify required vs optional dylibs
    if ( dylibs.size() < 30 ) {
        _diagnostics.error("missing required minimum set of dylibs");
        return;
    }
    uint64_t t1 = mach_absolute_time();


    // make copy of dylib list and sort
    std::vector<DyldSharedCache::MappedMachO> sortedDylibs = makeSortedDylibs(dylibs, _options.dylibOrdering);
    std::vector<DyldSharedCache::MappedMachO> otherOsDylibs = otherOsDylibsInput;

    // assign addresses for each segment of each dylib in new cache
    dyld_cache_mapping_info regions[3];
    SegmentMapping segmentMapping = assignSegmentAddresses(sortedDylibs, regions);
    while ( cacheOverflow(regions) ) {
        if ( !_options.evictLeafDylibsOnOverflow ) {
            _diagnostics.error("cache overflow: %lluMB (max %lluMB)", _vmSize / 1024 / 1024, (_archLayout->sharedMemorySize) / 1024 / 1024);
            return;
        }
        // find all leaf (not referenced by anything else in cache) dylibs

        // build count of how many references there are to each dylib
        __block std::map<std::string, unsigned int> referenceCount;
        for (const DyldSharedCache::MappedMachO& dylib : sortedDylibs) {
            dyld3::MachOParser parser(dylib.mh);
            parser.forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool &stop) {
                referenceCount[loadPath] += 1;
            });
        }

        // find all dylibs not referenced
        std::vector<DylibAndSize> unreferencedDylibs;
        for (const DyldSharedCache::MappedMachO& dylib : sortedDylibs) {
            dyld3::MachOParser parser(dylib.mh);
            const char* installName = parser.installName();
            if ( referenceCount.count(installName) == 0 ) {
                // conservative: sum up all segments except LINKEDIT
                __block uint64_t segsSize = 0;
                parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool &stop) {
                    if ( strcmp(segName, "__LINKEDIT") != 0 )
                        segsSize += vmSize;
                });
                unreferencedDylibs.push_back({installName, segsSize});
            }
        }
        // sort leaf dylibs by size
        std::sort(unreferencedDylibs.begin(), unreferencedDylibs.end(), [&](const DylibAndSize& a, const DylibAndSize& b) {
            return ( a.size > b.size );
        });

        // build set of dylibs that if removed will allow cache to build
        uint64_t reductionTarget = _vmSize - _archLayout->sharedMemorySize;
        std::set<std::string> toRemove;
        for (DylibAndSize& dylib : unreferencedDylibs) {
            if ( _options.verbose )
                _diagnostics.warning("to prevent cache overflow, not caching %s", dylib.installName);
            toRemove.insert(dylib.installName);
            if ( dylib.size > reductionTarget )
                break;
            reductionTarget -= dylib.size;
        }
        // transfer overflow dylibs from cached vector to other vector
        for (const std::string& installName : toRemove) {
            for (std::vector<DyldSharedCache::MappedMachO>::iterator it=sortedDylibs.begin(); it != sortedDylibs.end(); ++it) {
                dyld3::MachOParser parser(it->mh);
                if ( installName == parser.installName() ) {
                    _evictions.insert(parser.header());
                    otherOsDylibs.push_back(*it);
                    sortedDylibs.erase(it);
                    break;
                }
            }
        }
        // re-layout cache
        segmentMapping = assignSegmentAddresses(sortedDylibs, regions);
        if ( unreferencedDylibs.size() == 0 && cacheOverflow(regions) ) {
            _diagnostics.error("cache overflow, tried evicting %ld leaf daylibs, but still too big: %lluMB (max %lluMB)",
                               toRemove.size(), _vmSize / 1024 / 1024, (_archLayout->sharedMemorySize) / 1024 / 1024);
            return;
        }
    }

    // allocate buffer for new cache
    _allocatedBufferSize = std::max(_currentFileSize, (uint64_t)0x100000)*1.1; // add 10% to allocation to support large closures
    if ( vm_allocate(mach_task_self(), (vm_address_t*)&_buffer, _allocatedBufferSize, VM_FLAGS_ANYWHERE) != 0 ) {
        _diagnostics.error("could not allocate buffer");
        return;
    }
    _currentFileSize = _allocatedBufferSize;

    // write unoptimized cache
    writeCacheHeader(regions, sortedDylibs, segmentMapping);
    copyRawSegments(sortedDylibs, segmentMapping);
    adjustAllImagesForNewSegmentLocations(sortedDylibs, segmentMapping);
    if ( _diagnostics.hasError() )
        return;

    bindAllImagesInCacheFile(regions);
    if ( _diagnostics.hasError() )
        return;

    // optimize ObjC
    if ( _options.optimizeObjC )
        optimizeObjC(_buffer, _archLayout->is64, _options.optimizeStubs, _pointersForASLR, _diagnostics);
    if ( _diagnostics.hasError() )
        return;

    // optimize away stubs
    std::vector<uint64_t> branchPoolOffsets;
    uint64_t cacheStartAddress = _archLayout->sharedMemoryStart;
    if ( _options.optimizeStubs ) {
        std::vector<uint64_t> branchPoolStartAddrs;
        const uint64_t* p = (uint64_t*)((uint8_t*)_buffer + _buffer->header.branchPoolsOffset);
        for (int i=0; i < _buffer->header.branchPoolsCount; ++i) {
            uint64_t poolAddr = p[i];
            branchPoolStartAddrs.push_back(poolAddr);
            branchPoolOffsets.push_back(poolAddr - cacheStartAddress);
        }
        bypassStubs(_buffer, branchPoolStartAddrs, _s_neverStubEliminate, _diagnostics);
    }
    uint64_t t2 = mach_absolute_time();

    // FIPS seal corecrypto, This must be done after stub elimination (so that
    // __TEXT,__text is not changed after sealing), but before LINKEDIT
    // optimization  (so that we still have access to local symbols)
    fipsSign();

    // merge and compact LINKEDIT segments
    dyld_cache_local_symbols_info* localsInfo = nullptr;
    if ( dylibs.size() == 0 )
        _currentFileSize = 0x1000;
    else
        _currentFileSize = optimizeLinkedit(_buffer, _archLayout->is64, _options.excludeLocalSymbols, _options.optimizeStubs, branchPoolOffsets, _diagnostics, &localsInfo);

    uint64_t t3 = mach_absolute_time();

    // add ImageGroup for all dylibs in cache
    __block std::vector<DyldSharedCache::MappedMachO> cachedDylibs;
    std::unordered_map<std::string, const DyldSharedCache::MappedMachO*> mapIntoSortedDylibs;
    for (const DyldSharedCache::MappedMachO& entry : sortedDylibs) {
        mapIntoSortedDylibs[entry.runtimePath] = &entry;
    }
    _buffer->forEachImage(^(const mach_header* mh, const char* installName) {
        auto pos = mapIntoSortedDylibs.find(installName);
        if ( pos != mapIntoSortedDylibs.end() ) {
            DyldSharedCache::MappedMachO newEntry = *(pos->second);
            newEntry.mh = mh;
            cachedDylibs.push_back(newEntry);
        }
        else {
            bool found = false;
            for (const std::string& prefix :  _options.pathPrefixes) {
                std::string fullPath = prefix + installName;
                char resolvedPath[PATH_MAX];
                if ( realpath(fullPath.c_str(), resolvedPath) != nullptr ) {
                    std::string resolvedUnPrefixed = &resolvedPath[prefix.size()];
                    pos = mapIntoSortedDylibs.find(resolvedUnPrefixed);
                    if ( pos != mapIntoSortedDylibs.end() ) {
                        DyldSharedCache::MappedMachO newEntry = *(pos->second);
                        newEntry.mh = mh;
                        cachedDylibs.push_back(newEntry);
                        found = true;
                   }
                }
            }
            if ( !found )
                fprintf(stderr, "missing mapping for %s\n", installName);
        }
    });
    dyld3::DyldCacheParser dyldCacheParser(_buffer, true);
    dyld3::ImageProxyGroup* dylibGroup = dyld3::ImageProxyGroup::makeDyldCacheDylibsGroup(_diagnostics, dyldCacheParser, cachedDylibs,
                                                                                          _options.pathPrefixes, _patchTable,
                                                                                          _options.optimizeStubs, !_options.dylibsRemovedDuringMastering);
    if ( _diagnostics.hasError() )
        return;
    addCachedDylibsImageGroup(dylibGroup);
    if ( _diagnostics.hasError() )
        return;

    uint64_t t4 = mach_absolute_time();

    // add ImageGroup for other OS dylibs and bundles
    dyld3::ImageProxyGroup* otherGroup = dyld3::ImageProxyGroup::makeOtherOsGroup(_diagnostics, dyldCacheParser, dylibGroup, otherOsDylibs,
                                                                                  _options.inodesAreSameAsRuntime, _options.pathPrefixes);
    if ( _diagnostics.hasError() )
        return;
    addCachedOtherDylibsImageGroup(otherGroup);
    if ( _diagnostics.hasError() )
        return;

    uint64_t t5 = mach_absolute_time();

    // compute and add launch closures
    std::map<std::string, const dyld3::launch_cache::binary_format::Closure*> closures;
    for (const DyldSharedCache::MappedMachO& mainProg : osExecutables) {
        Diagnostics clsDiag;
        const dyld3::launch_cache::binary_format::Closure* cls = dyld3::ImageProxyGroup::makeClosure(clsDiag, dyldCacheParser, dylibGroup, otherGroup, mainProg,
                                                                                                     _options.inodesAreSameAsRuntime, _options.pathPrefixes);
        if ( clsDiag.hasError() ) {
            // if closure cannot be built, silently skip it, unless in verbose mode
            if ( _options.verbose ) {
                _diagnostics.warning("building closure for '%s': %s", mainProg.runtimePath.c_str(), clsDiag.errorMessage().c_str());
                for (const std::string& warn : clsDiag.warnings() )
                    _diagnostics.warning("%s", warn.c_str());
            }
        }
        else {
            closures[mainProg.runtimePath] = cls;
       }
    }
    addClosures(closures);
    if ( _diagnostics.hasError() )
        return;

    uint64_t t6 = mach_absolute_time();

    // fill in slide info at start of region[2]
    // do this last because it modifies pointers in DATA segments
    if ( _options.cacheSupportsASLR ) {
        if ( _archLayout->is64 )
            writeSlideInfoV2<Pointer64<LittleEndian>>();
        else
            writeSlideInfoV2<Pointer32<LittleEndian>>();
    }

    uint64_t t7 = mach_absolute_time();

    // update last region size
    dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    _currentFileSize = align(_currentFileSize, _archLayout->sharedRegionAlignP2);
    mappings[2].size = _currentFileSize - mappings[2].fileOffset;

    // record cache bounds
    _buffer->header.sharedRegionStart = _archLayout->sharedMemoryStart;
    _buffer->header.sharedRegionSize  = _archLayout->sharedMemorySize;
    if ( _archLayout->sharedRegionsAreDiscontiguous ) {
        // special case x86_64 which has three non-contiguous chunks each in their own 1GB regions
        uint64_t maxSlide0 = 0x60000000 - mappings[0].size; // TEXT region has 1.5GB region
        uint64_t maxSlide1 = 0x40000000 - mappings[1].size;
        uint64_t maxSlide2 = 0x3FE00000 - mappings[2].size;
        _buffer->header.maxSlide = std::min(std::min(maxSlide0, maxSlide1), maxSlide2);
    }
    else {
        _buffer->header.maxSlide = (_archLayout->sharedMemoryStart + _archLayout->sharedMemorySize) - (mappings[2].address + mappings[2].size);
    }

    // append "unmapped" local symbols region
    if ( _options.excludeLocalSymbols ) {
        size_t localsInfoSize = align(localsInfo->stringsOffset + localsInfo->stringsSize, _archLayout->sharedRegionAlignP2);
        if ( _currentFileSize + localsInfoSize > _allocatedBufferSize ) {
            _diagnostics.warning("local symbols omitted because cache buffer overflow");
        }
        else {
            memcpy((char*)_buffer+_currentFileSize, localsInfo, localsInfoSize);
            _buffer->header.localSymbolsOffset = _currentFileSize;
            _buffer->header.localSymbolsSize   = localsInfoSize;
            _currentFileSize += localsInfoSize;
        }
        free((void*)localsInfo);
    }

    recomputeCacheUUID();

    // Calculate the VMSize of the resulting cache
    __block uint64_t endAddr = 0;
    _buffer->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
        if (vmAddr+size > endAddr)
            endAddr = vmAddr+size;
    });
    _vmSize = endAddr - cacheStartAddress;

    // last sanity check on size
    if ( _vmSize > _archLayout->sharedMemorySize ) {
        _diagnostics.error("cache overflow after optimizations.  %lluMB (max %lluMB)", _vmSize / 1024 / 1024, (_archLayout->sharedMemorySize) / 1024 / 1024);
        return;
    }

    // codesignature is part of file, but is not mapped
    codeSign();
    if ( _diagnostics.hasError() )
        return;

    uint64_t t8 = mach_absolute_time();

    if ( _options.verbose ) {
        fprintf(stderr, "time to copy and bind cached dylibs: %ums\n", absolutetime_to_milliseconds(t2-t1));
        fprintf(stderr, "time to optimize LINKEDITs: %ums\n", absolutetime_to_milliseconds(t3-t2));
        fprintf(stderr, "time to build ImageGroup of %lu cached dylibs: %ums\n", sortedDylibs.size(), absolutetime_to_milliseconds(t4-t3));
        fprintf(stderr, "time to build ImageGroup of %lu other dylibs: %ums\n", otherOsDylibs.size(), absolutetime_to_milliseconds(t5-t4));
        fprintf(stderr, "time to build %lu closures: %ums\n", osExecutables.size(), absolutetime_to_milliseconds(t6-t5));
        fprintf(stderr, "time to compute slide info: %ums\n", absolutetime_to_milliseconds(t7-t6));
        fprintf(stderr, "time to compute UUID and codesign cache file: %ums\n", absolutetime_to_milliseconds(t8-t7));
    }

    // trim over allocated buffer
    if ( _allocatedBufferSize > _currentFileSize ) {
        uint8_t* startOfUnused  = (uint8_t*)_buffer+_currentFileSize;
        size_t unusedLen = _allocatedBufferSize-_currentFileSize;
        vm_deallocate(mach_task_self(), (vm_address_t)startOfUnused, unusedLen);
        _allocatedBufferSize = _currentFileSize;
    }

    return;
}


void CacheBuilder::writeCacheHeader(const dyld_cache_mapping_info regions[3], const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping& segmentMappings)
{
    // "dyld_v1" + spaces + archName(), with enough spaces to pad to 15 bytes
    std::string magic = "dyld_v1";
    magic.append(15 - magic.length() - _options.archName.length(), ' ');
    magic.append(_options.archName);
    assert(magic.length() == 15);

    // fill in header
    memcpy(_buffer->header.magic, magic.c_str(), 16);
    _buffer->header.mappingOffset      = sizeof(dyld_cache_header);
    _buffer->header.mappingCount       = 3;
    _buffer->header.imagesOffset       = (uint32_t)(_buffer->header.mappingOffset + 3*sizeof(dyld_cache_mapping_info) + sizeof(uint64_t)*_branchPoolStarts.size());
    _buffer->header.imagesCount        = (uint32_t)dylibs.size() + _aliasCount;
    _buffer->header.dyldBaseAddress    = 0;
    _buffer->header.codeSignatureOffset= 0;
    _buffer->header.codeSignatureSize  = 0;
    _buffer->header.slideInfoOffset    = _slideInfoFileOffset;
    _buffer->header.slideInfoSize      = _slideInfoBufferSizeAllocated;
    _buffer->header.localSymbolsOffset = 0;
    _buffer->header.localSymbolsSize   = 0;
    _buffer->header.cacheType          = _options.optimizeStubs ? kDyldSharedCacheTypeProduction : kDyldSharedCacheTypeDevelopment;
    _buffer->header.accelerateInfoAddr = 0;
    _buffer->header.accelerateInfoSize = 0;
    bzero(_buffer->header.uuid, 16);    // overwritten later by recomputeCacheUUID()
    _buffer->header.branchPoolsOffset    = _buffer->header.mappingOffset + 3*sizeof(dyld_cache_mapping_info);
    _buffer->header.branchPoolsCount     = (uint32_t)_branchPoolStarts.size();
    _buffer->header.imagesTextOffset     = _buffer->header.imagesOffset + sizeof(dyld_cache_image_info)*_buffer->header.imagesCount;
    _buffer->header.imagesTextCount      = dylibs.size();
    _buffer->header.platform             = (uint8_t)_options.platform;
    _buffer->header.formatVersion        = dyld3::launch_cache::binary_format::kFormatVersion;
    _buffer->header.dylibsExpectedOnDisk = !_options.dylibsRemovedDuringMastering;
    _buffer->header.simulator            = _options.forSimulator;

    // fill in mappings
    dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    mappings[0] = regions[0];
    mappings[1] = regions[1];
    mappings[2] = regions[2];

    // fill in branch pool addresses
    uint64_t* p = (uint64_t*)((char*)_buffer + _buffer->header.branchPoolsOffset);
    for (uint64_t pool : _branchPoolStarts) {
        *p++ = pool;
    }

    // fill in image table
    dyld_cache_image_info* images = (dyld_cache_image_info*)((char*)_buffer + _buffer->header.imagesOffset);
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        const std::vector<SegmentMappingInfo>& segs = segmentMappings.at(dylib.mh);
        dyld3::MachOParser parser(dylib.mh);
        const char* installName = parser.installName();
        images->address = segs[0].dstCacheAddress;
        if ( _options.dylibsRemovedDuringMastering ) {
            images->modTime = 0;
            images->inode   = pathHash(installName);
        }
        else {
            images->modTime = dylib.modTime;
            images->inode   = dylib.inode;
        }
        uint32_t installNameOffsetInTEXT =  (uint32_t)(installName - (char*)dylib.mh);
        images->pathFileOffset = (uint32_t)segs[0].dstCacheOffset + installNameOffsetInTEXT;
        ++images;
    }
    // append aliases image records and strings
/*
    for (auto &dylib : _dylibs) {
        if (!dylib->installNameAliases.empty()) {
            for (const std::string& alias : dylib->installNameAliases) {
                images->set_address(_segmentMap[dylib][0].address);
                if (_manifest.platform() == "osx") {
                    images->modTime = dylib->lastModTime;
                    images->inode = dylib->inode;
                }
                else {
                    images->modTime = 0;
                    images->inode = pathHash(alias.c_str());
                }
                images->pathFileOffset = offset;
                //fprintf(stderr, "adding alias %s for %s\n", alias.c_str(), dylib->installName.c_str());
                ::strcpy((char*)&_buffer[offset], alias.c_str());
                offset += alias.size() + 1;
                ++images;
            }
        }
    }
*/
    // calculate start of text image array and trailing string pool
    dyld_cache_image_text_info* textImages = (dyld_cache_image_text_info*)((char*)_buffer + _buffer->header.imagesTextOffset);
    uint32_t stringOffset = (uint32_t)(_buffer->header.imagesTextOffset + sizeof(dyld_cache_image_text_info) * dylibs.size());

    // write text image array and image names pool at same time
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        const std::vector<SegmentMappingInfo>& segs = segmentMappings.at(dylib.mh);
        dyld3::MachOParser parser(dylib.mh);
        parser.getUuid(textImages->uuid);
        textImages->loadAddress     = segs[0].dstCacheAddress;
        textImages->textSegmentSize = (uint32_t)segs[0].dstCacheSegmentSize;
        textImages->pathOffset      = stringOffset;
        const char* installName = parser.installName();
        ::strcpy((char*)_buffer + stringOffset, installName);
        stringOffset += (uint32_t)strlen(installName)+1;
        ++textImages;
    }

    // make sure header did not overflow into first mapped image
    const dyld_cache_image_info* firstImage = (dyld_cache_image_info*)((char*)_buffer + _buffer->header.imagesOffset);
    assert(stringOffset <= (firstImage->address - mappings[0].address));
}


void CacheBuilder::copyRawSegments(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping& mapping)
{
    uint8_t* cacheBytes = (uint8_t*)_buffer;
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        auto pos = mapping.find(dylib.mh);
        assert(pos != mapping.end());
        for (const SegmentMappingInfo& info : pos->second) {
            //fprintf(stderr, "copy %s segment %s (0x%08X bytes) from %p to %p (logical addr 0x%llX) for %s\n", _options.archName.c_str(), info.segName, info.copySegmentSize, info.srcSegment, &cacheBytes[info.dstCacheOffset], info.dstCacheAddress, dylib.runtimePath.c_str());
            ::memcpy(&cacheBytes[info.dstCacheOffset], info.srcSegment, info.copySegmentSize);
        }
    }
}

void CacheBuilder::adjustAllImagesForNewSegmentLocations(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping& mapping)
{
    uint8_t* cacheBytes = (uint8_t*)_buffer;
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        auto pos = mapping.find(dylib.mh);
        assert(pos != mapping.end());
        mach_header* mhInCache = (mach_header*)&cacheBytes[pos->second[0].dstCacheOffset];
        adjustDylibSegments(_buffer, _archLayout->is64, mhInCache, pos->second, _pointersForASLR, _diagnostics);
        if ( _diagnostics.hasError() )
            break;
    }
}

struct Counts {
    unsigned long lazyCount    = 0;
    unsigned long nonLazyCount = 0;
};

void CacheBuilder::bindAllImagesInCacheFile(const dyld_cache_mapping_info regions[3])
{
    const bool log = false;
    __block std::unordered_map<std::string, Counts> useCounts;

    // build map of install names to mach_headers
    __block std::unordered_map<std::string, const mach_header*> installNameToMH;
    __block std::vector<const mach_header*> dylibMHs;
    _buffer->forEachImage(^(const mach_header* mh, const char* installName) {
        installNameToMH[installName] = mh;
        dylibMHs.push_back(mh);
    });

    __block Diagnostics parsingDiag;
    bool (^dylibFinder)(uint32_t, const char*, void* , const mach_header**, void**) = ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        auto pos = installNameToMH.find(depLoadPath);
        if ( pos != installNameToMH.end() ) {
            *foundMH = pos->second;
            *foundExtra = nullptr;
            return true;
        }
        parsingDiag.error("dependent dylib %s not found", depLoadPath);
        return false;
    };
    if ( parsingDiag.hasError() ) {
        _diagnostics.error("%s", parsingDiag.errorMessage().c_str());
        return;
    }

    // bind every dylib in cache
    for (const mach_header* mh : dylibMHs) {
        dyld3::MachOParser parser(mh, true);
        bool is64 = parser.is64();
        const char* depPaths[256];
        const char** depPathsArray = depPaths;
        __block int depIndex = 1;
        parser.forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
            depPathsArray[depIndex++] = loadPath;
        });
        uint8_t*  segCacheStarts[10];
        uint64_t  segCacheAddrs[10];
        uint8_t** segCacheStartsArray = segCacheStarts;
        uint64_t* segCacheAddrsArray  = segCacheAddrs;
        __block int segIndex = 0;
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
             segCacheStartsArray[segIndex] = (segIndex == 0) ? (uint8_t*)mh : (uint8_t*)_buffer + fileOffset;
             segCacheAddrsArray[segIndex] = vmAddr;
             ++segIndex;
        });
        __block Diagnostics bindingDiag;
        parser.forEachBind(bindingDiag, ^(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal, uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop) {
            if ( log ) {
                if ( lazy )
                    useCounts[symbolName].lazyCount += 1;
                else
                    useCounts[symbolName].nonLazyCount += 1;
            }
            const mach_header* targetMH = nullptr;
            if ( libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
                targetMH = mh;
            }
            else if ( libOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
                parsingDiag.error("bind ordinal BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE not supported in dylibs in dyld shared cache (found in %s)", parser.installName());
                stop = true;
                return;
            }
            else if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
                parsingDiag.error("bind ordinal BIND_SPECIAL_DYLIB_FLAT_LOOKUP not supported in dylibs in dyld shared cache (found in %s)", parser.installName());
                stop = true;
                return;
            }
            else {
                const char* fromPath = depPathsArray[libOrdinal];
                auto pos = installNameToMH.find(fromPath);
                if (pos == installNameToMH.end()) {
                    if (!weakImport) {
                        _diagnostics.error("dependent dylib %s not found", fromPath);
                    }
                    return;
                }
                targetMH = pos->second;
            }
            dyld3::MachOParser targetParser(targetMH, true);
            dyld3::MachOParser::FoundSymbol foundInfo;
            uint64_t targetValue = 0;
            uint8_t* fixupLoc = segCacheStartsArray[dataSegIndex] + dataSegOffset;
            if ( targetParser.findExportedSymbol(parsingDiag, symbolName, nullptr, foundInfo, dylibFinder) ) {
                const mach_header* foundInMH = foundInfo.foundInDylib;
                dyld3::MachOParser foundInParser(foundInMH, true);
                uint64_t foundInBaseAddress = foundInParser.preferredLoadAddress();
                switch ( foundInfo.kind ) {
                    case dyld3::MachOParser::FoundSymbol::Kind::resolverOffset:
                        // Bind to the target stub for resolver based functions.
                        // There may be a later optimization to alter the client
                        // stubs to directly to the target stub's lazy pointer.
                    case dyld3::MachOParser::FoundSymbol::Kind::headerOffset:
                        targetValue = foundInBaseAddress + foundInfo.value + addend;
                        _pointersForASLR.push_back((void*)fixupLoc);
                        if ( foundInMH != mh ) {
                            uint32_t mhVmOffset                 = (uint32_t)((uint8_t*)foundInMH - (uint8_t*)_buffer);
                            uint32_t definitionCacheVmOffset    = (uint32_t)(mhVmOffset + foundInfo.value);
                            uint32_t referenceCacheDataVmOffset = (uint32_t)(segCacheAddrsArray[dataSegIndex] + dataSegOffset - regions[1].address);
                            assert(referenceCacheDataVmOffset < (1<<30));
                            dyld3::launch_cache::binary_format::PatchOffset entry;
                            entry.last              = false;
                            entry.hasAddend         = (addend != 0);
                            entry.dataRegionOffset  = referenceCacheDataVmOffset;
                            _patchTable[foundInMH][definitionCacheVmOffset].insert(*((uint32_t*)&entry));
                        }
                       break;
                    case dyld3::MachOParser::FoundSymbol::Kind::absolute:
                        // pointers set to absolute values are not slid
                        targetValue = foundInfo.value + addend;
                        break;
                }
            }
            else if ( weakImport ) {
                // weak pointers set to zero are not slid
                targetValue = 0;
            }
            else {
                parsingDiag.error("cannot find symbol %s, needed in dylib %s", symbolName, parser.installName());
                stop = true;
            }
            switch ( type ) {
                case BIND_TYPE_POINTER:
                    if ( is64 )
                        *((uint64_t*)fixupLoc) = targetValue;
                    else
                        *((uint32_t*)fixupLoc) = (uint32_t)targetValue;
                    break;
                case BIND_TYPE_TEXT_ABSOLUTE32:
                case BIND_TYPE_TEXT_PCREL32:
                    parsingDiag.error("text relocs not supported for shared cache binding in %s", parser.installName());
                    stop = true;
                    break;
                default:
                    parsingDiag.error("bad bind type (%d) in %s", type, parser.installName());
                    stop = true;
                    break;

            }
        });
        if ( bindingDiag.hasError() ) {
            parsingDiag.error("%s in dylib %s", bindingDiag.errorMessage().c_str(), parser.installName());
        }
        if ( parsingDiag.hasError() )
            break;
        // also need to add patch locations for weak-binds that point within same image, since they are not captured by binds above
        parser.forEachWeakDef(bindingDiag, ^(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset, uint64_t addend, const char* symbolName, bool &stop) {
            if ( strongDef )
                return;
            uint8_t* fixupLoc = segCacheStartsArray[dataSegIndex] + dataSegOffset;
            dyld3::MachOParser::FoundSymbol weakFoundInfo;
            Diagnostics weakLookupDiag;
            if ( parser.findExportedSymbol(weakLookupDiag, symbolName, nullptr, weakFoundInfo, nullptr) ) {
                // this is an interior pointing (rebased) pointer
                uint64_t targetValue;
                if ( is64 )
                    targetValue = *((uint64_t*)fixupLoc);
                else
                    targetValue = *((uint32_t*)fixupLoc);
                uint32_t definitionCacheVmOffset    = (uint32_t)(targetValue - regions[0].address);
                uint32_t referenceCacheDataVmOffset = (uint32_t)(segCacheAddrsArray[dataSegIndex] + dataSegOffset - regions[1].address);
                assert(referenceCacheDataVmOffset < (1<<30));
                dyld3::launch_cache::binary_format::PatchOffset entry;
                entry.last              = false;
                entry.hasAddend         = (addend != 0);
                entry.dataRegionOffset  = referenceCacheDataVmOffset;
                _patchTable[mh][definitionCacheVmOffset].insert(*((uint32_t*)&entry));
            }
        });
        if ( bindingDiag.hasError() ) {
            parsingDiag.error("%s in dylib %s", bindingDiag.errorMessage().c_str(), parser.installName());
        }
        if ( parsingDiag.hasError() )
            break;
    }

    if ( log ) {
        unsigned lazyCount = 0;
        unsigned nonLazyCount = 0;
        std::unordered_set<std::string> lazyTargets;
        for (auto entry : useCounts) {
            fprintf(stderr, "% 3ld      % 3ld     %s\n", entry.second.lazyCount, entry.second.nonLazyCount, entry.first.c_str());
            lazyCount += entry.second.lazyCount;
            nonLazyCount += entry.second.nonLazyCount;
            if ( entry.second.lazyCount != 0 )
                lazyTargets.insert(entry.first);
        }
        fprintf(stderr, "lazyCount = %d\n", lazyCount);
        fprintf(stderr, "nonLazyCount = %d\n", nonLazyCount);
        fprintf(stderr, "unique lazys = %ld\n", lazyTargets.size());
    }
    
    if ( parsingDiag.hasError() )
        _diagnostics.error("%s", parsingDiag.errorMessage().c_str());
}


void CacheBuilder::recomputeCacheUUID(void)
{
    // Clear existing UUID, then MD5 whole cache buffer.
    uint8_t* uuidLoc = _buffer->header.uuid;
    bzero(uuidLoc, 16);
    CC_MD5(_buffer, (unsigned)_currentFileSize, uuidLoc);
    // <rdar://problem/6723729> uuids should conform to RFC 4122 UUID version 4 & UUID version 5 formats
    uuidLoc[6] = ( uuidLoc[6] & 0x0F ) | ( 3 << 4 );
    uuidLoc[8] = ( uuidLoc[8] & 0x3F ) | 0x80;
}


CacheBuilder::SegmentMapping CacheBuilder::assignSegmentAddresses(const std::vector<DyldSharedCache::MappedMachO>& dylibs, dyld_cache_mapping_info regions[3])
{
    // calculate size of header info and where first dylib's mach_header should start
    size_t startOffset = sizeof(dyld_cache_header) + 3*sizeof(dyld_cache_mapping_info);
    size_t maxPoolCount = 0;
    if (  _archLayout->branchReach != 0 )
        maxPoolCount = (_archLayout->sharedMemorySize / _archLayout->branchReach);
    startOffset += maxPoolCount * sizeof(uint64_t);
    startOffset += sizeof(dyld_cache_image_info) * dylibs.size();
    startOffset += sizeof(dyld_cache_image_text_info) * dylibs.size();
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh);
        startOffset += (strlen(parser.installName()) + 1);
    }
    //fprintf(stderr, "%s total header size = 0x%08lX\n", _options.archName.c_str(), startOffset);
    startOffset = align(startOffset, 12);

    _branchPoolStarts.clear();
    __block uint64_t addr = _archLayout->sharedMemoryStart;
    __block SegmentMapping result;

    // assign TEXT segment addresses
    regions[0].address      = addr;
    regions[0].fileOffset   = 0;
    regions[0].initProt     = VM_PROT_READ | VM_PROT_EXECUTE;
    regions[0].maxProt      = VM_PROT_READ | VM_PROT_EXECUTE;
    addr += startOffset; // header

    __block uint64_t lastPoolAddress = addr;
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
             if ( protections != (VM_PROT_READ | VM_PROT_EXECUTE) )
                return;
            // Insert branch island pools every 128MB for arm64
            if ( (_archLayout->branchPoolTextSize != 0) && ((addr + vmSize - lastPoolAddress) > _archLayout->branchReach) ) {
                _branchPoolStarts.push_back(addr);
                _diagnostics.verbose("adding branch pool at 0x%llX\n", addr);
                lastPoolAddress = addr;
                addr += _archLayout->branchPoolTextSize;
            }
            // Keep __TEXT segments 4K or more aligned
            addr = align(addr, std::max(p2align, (uint8_t)12));
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[0].address + regions[0].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)align(sizeOfSections, 12);
            info.copySegmentSize     = (uint32_t)align(sizeOfSections, 12);
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }
    // align TEXT region end
    uint64_t endTextAddress = align(addr, _archLayout->sharedRegionAlignP2);
    regions[0].size         = endTextAddress - regions[0].address;

    // assign __DATA* addresses
    if ( _archLayout->sharedRegionsAreDiscontiguous )
        addr = _archLayout->sharedMemoryStart + 0x60000000;
    else
        addr = align((addr + _archLayout->sharedRegionPadding), _archLayout->sharedRegionAlignP2);
    regions[1].address      = addr;
    regions[1].fileOffset   = regions[0].fileOffset + regions[0].size;
    regions[1].initProt     = VM_PROT_READ | VM_PROT_WRITE;
    regions[1].maxProt      = VM_PROT_READ | VM_PROT_WRITE;

    // layout all __DATA_CONST segments
    __block int dataConstSegmentCount = 0;
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
            if ( protections != (VM_PROT_READ | VM_PROT_WRITE) )
                return;
            if ( strcmp(segName, "__DATA_CONST") != 0 )
                return;
            ++dataConstSegmentCount;
            // Pack __DATA_CONST segments
            addr = align(addr, p2align);
            size_t copySize = std::min((size_t)fileSize, (size_t)sizeOfSections);
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[1].address + regions[1].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)sizeOfSections;
            info.copySegmentSize     = (uint32_t)copySize;
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }

    // layout all __DATA segments (and other r/w non-dirty, non-const) segments
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
            if ( protections != (VM_PROT_READ | VM_PROT_WRITE) )
                return;
            if ( strcmp(segName, "__DATA_CONST") == 0 )
                return;
            if ( strcmp(segName, "__DATA_DIRTY") == 0 )
                return;
            if ( dataConstSegmentCount > 10 ) {
                // Pack __DATA segments only if we also have __DATA_CONST segments
                addr = align(addr, p2align);
            }
            else {
                // Keep __DATA segments 4K or more aligned
                addr = align(addr, std::max(p2align, (uint8_t)12));
            }
            size_t copySize = std::min((size_t)fileSize, (size_t)sizeOfSections);
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[1].address + regions[1].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)sizeOfSections;
            info.copySegmentSize     = (uint32_t)copySize;
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }

    // layout all __DATA_DIRTY segments, sorted
    addr = align(addr, 12);
    std::vector<DyldSharedCache::MappedMachO> dirtyDataDylibs = makeSortedDylibs(dylibs, _options.dirtyDataSegmentOrdering);
    for (const DyldSharedCache::MappedMachO& dylib : dirtyDataDylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
            if ( protections != (VM_PROT_READ | VM_PROT_WRITE) )
                return;
            if ( strcmp(segName, "__DATA_DIRTY") != 0 )
                return;
            // Pack __DATA_DIRTY segments
            addr = align(addr, p2align);
            size_t copySize = std::min((size_t)fileSize, (size_t)sizeOfSections);
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[1].address + regions[1].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)sizeOfSections;
            info.copySegmentSize     = (uint32_t)copySize;
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }

    // align DATA region end
    uint64_t endDataAddress = align(addr, _archLayout->sharedRegionAlignP2);
    regions[1].size         = endDataAddress - regions[1].address;

    // start read-only region
    if ( _archLayout->sharedRegionsAreDiscontiguous )
        addr = _archLayout->sharedMemoryStart + 0xA0000000;
    else
        addr = align((addr + _archLayout->sharedRegionPadding), _archLayout->sharedRegionAlignP2);
    regions[2].address    = addr;
    regions[2].fileOffset = regions[1].fileOffset + regions[1].size;
    regions[2].maxProt    = VM_PROT_READ;
    regions[2].initProt   = VM_PROT_READ;

    // reserve space for kernel ASLR slide info at start of r/o region
    if ( _options.cacheSupportsASLR ) {
        _slideInfoBufferSizeAllocated = align((regions[1].size/4096) * 4, _archLayout->sharedRegionAlignP2); // only need 2 bytes per page
        _slideInfoFileOffset = regions[2].fileOffset;
        addr += _slideInfoBufferSizeAllocated;
    }

    // layout all read-only (but not LINKEDIT) segments
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
            if ( protections != VM_PROT_READ )
                return;
            if ( strcmp(segName, "__LINKEDIT") == 0 )
                return;
            // Keep segments segments 4K or more aligned
            addr = align(addr, std::max(p2align, (uint8_t)12));
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[2].address + regions[2].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)align(sizeOfSections, 12);
            info.copySegmentSize     = (uint32_t)sizeOfSections;
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }
    // layout all LINKEDIT segments (after other read-only segments)
    for (const DyldSharedCache::MappedMachO& dylib : dylibs) {
        dyld3::MachOParser parser(dylib.mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop) {
            if ( protections != VM_PROT_READ )
                return;
            if ( strcmp(segName, "__LINKEDIT") != 0 )
                return;
            // Keep segments segments 4K or more aligned
            addr = align(addr, std::max(p2align, (uint8_t)12));
            SegmentMappingInfo info;
            info.srcSegment          = (uint8_t*)dylib.mh + fileOffset;
            info.segName             = segName;
            info.dstCacheAddress     = addr;
            info.dstCacheOffset      = (uint32_t)(addr - regions[2].address + regions[2].fileOffset);
            info.dstCacheSegmentSize = (uint32_t)align(sizeOfSections, 12);
            info.copySegmentSize     = (uint32_t)align(fileSize, 12);
            info.srcSegmentIndex     = segIndex;
            result[dylib.mh].push_back(info);
            addr += info.dstCacheSegmentSize;
        });
    }
    // add room for branch pool linkedits
    _branchPoolsLinkEditStartAddr = addr;
    addr += (_branchPoolStarts.size() * _archLayout->branchPoolLinkEditSize);

    // align r/o region end
    uint64_t endReadOnlyAddress = align(addr, _archLayout->sharedRegionAlignP2);
    regions[2].size = endReadOnlyAddress - regions[2].address;
    _currentFileSize = regions[2].fileOffset + regions[2].size;

    // FIXME: Confirm these numbers for all platform/arch combos
    // assume LINKEDIT optimzation reduces LINKEDITs to %40 of original size
    if ( _options.excludeLocalSymbols ) {
        _vmSize = regions[2].address + (regions[2].size * 2 / 5) - regions[0].address;
    }
    else {
        _vmSize = regions[2].address + (regions[2].size * 9 / 10) - regions[0].address;
    }

    // sort SegmentMappingInfo for each image to be in the same order as original segments
    for (auto& entry : result) {
        std::vector<SegmentMappingInfo>& infos = entry.second;
        std::sort(infos.begin(), infos.end(), [&](const SegmentMappingInfo& a, const SegmentMappingInfo& b) {
            return a.srcSegmentIndex < b.srcSegmentIndex;
        });
    }

    return result;
}

uint64_t CacheBuilder::pathHash(const char* path)
{
    uint64_t sum = 0;
    for (const char* s=path; *s != '\0'; ++s)
        sum += sum*4 + *s;
    return sum;
}


void CacheBuilder::findDylibAndSegment(const void* contentPtr, std::string& foundDylibName, std::string& foundSegName)
{
    foundDylibName = "???";
    foundSegName   = "???";
    uint32_t cacheOffset = (uint32_t)((uint8_t*)contentPtr - (uint8_t*)_buffer);
    _buffer->forEachImage(^(const mach_header* mh, const char* installName) {
        dyld3::MachOParser parser(mh, true);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
            if ( (cacheOffset > fileOffset) && (cacheOffset < (fileOffset+vmSize)) ) {
                foundDylibName = installName;
                foundSegName = segName;
            }
        });
    });
 }


template <typename P>
bool CacheBuilder::makeRebaseChain(uint8_t* pageContent, uint16_t lastLocationOffset, uint16_t offset, const dyld_cache_slide_info2* info)
{
    typedef typename P::uint_t     pint_t;

    const pint_t   deltaMask    = (pint_t)(info->delta_mask);
    const pint_t   valueMask    = ~deltaMask;
    const pint_t   valueAdd     = (pint_t)(info->value_add);
    const unsigned deltaShift   = __builtin_ctzll(deltaMask) - 2;
    const uint32_t maxDelta     = (uint32_t)(deltaMask >> deltaShift);

    pint_t* lastLoc = (pint_t*)&pageContent[lastLocationOffset+0];
    pint_t lastValue = (pint_t)P::getP(*lastLoc);
    if ( (lastValue - valueAdd) & deltaMask ) {
        std::string dylibName;
        std::string segName;
        findDylibAndSegment((void*)pageContent, dylibName, segName);
        _diagnostics.error("rebase pointer does not point within cache. lastOffset=0x%04X, seg=%s, dylib=%s\n",
                            lastLocationOffset, segName.c_str(), dylibName.c_str());
        return false;
    }
    if ( offset <= (lastLocationOffset+maxDelta) ) {
        // previous location in range, make link from it
        // encode this location into last value
        pint_t delta = offset - lastLocationOffset;
        pint_t newLastValue = ((lastValue - valueAdd) & valueMask) | (delta << deltaShift);
        //warning("  add chain: delta = %d, lastOffset=0x%03X, offset=0x%03X, org value=0x%08lX, new value=0x%08lX",
        //                    offset - lastLocationOffset, lastLocationOffset, offset, (long)lastValue, (long)newLastValue);
        P::setP(*lastLoc, newLastValue);
        return true;
    }
    //warning("  too big delta = %d, lastOffset=0x%03X, offset=0x%03X", offset - lastLocationOffset, lastLocationOffset, offset);

    // distance between rebase locations is too far
    // see if we can make a chain from non-rebase locations
    uint16_t nonRebaseLocationOffsets[1024];
    unsigned nrIndex = 0;
    for (uint16_t i = lastLocationOffset; i < offset-maxDelta; ) {
        nonRebaseLocationOffsets[nrIndex] = 0;
        for (int j=maxDelta; j > 0; j -= 4) {
            pint_t value = (pint_t)P::getP(*(pint_t*)&pageContent[i+j]);
            if ( value == 0 ) {
                // Steal values of 0 to be used in the rebase chain
                nonRebaseLocationOffsets[nrIndex] = i+j;
                break;
            }
        }
        if ( nonRebaseLocationOffsets[nrIndex] == 0 ) {
            lastValue = (pint_t)P::getP(*lastLoc);
            pint_t newValue = ((lastValue - valueAdd) & valueMask);
            //warning("   no way to make non-rebase delta chain, terminate off=0x%03X, old value=0x%08lX, new value=0x%08lX", lastLocationOffset, (long)value, (long)newValue);
            P::setP(*lastLoc, newValue);
            return false;
        }
        i = nonRebaseLocationOffsets[nrIndex];
        ++nrIndex;
    }

    // we can make chain. go back and add each non-rebase location to chain
    uint16_t prevOffset = lastLocationOffset;
    pint_t* prevLoc = (pint_t*)&pageContent[prevOffset];
    for (int n=0; n < nrIndex; ++n) {
        uint16_t nOffset = nonRebaseLocationOffsets[n];
        assert(nOffset != 0);
        pint_t* nLoc = (pint_t*)&pageContent[nOffset];
        uint32_t delta2 = nOffset - prevOffset;
        pint_t value = (pint_t)P::getP(*prevLoc);
        pint_t newValue;
        if ( value == 0 )
            newValue = (delta2 << deltaShift);
        else
            newValue = ((value - valueAdd) & valueMask) | (delta2 << deltaShift);
        //warning("    non-rebase delta = %d, to off=0x%03X, old value=0x%08lX, new value=0x%08lX", delta2, nOffset, (long)value, (long)newValue);
        P::setP(*prevLoc, newValue);
        prevOffset = nOffset;
        prevLoc = nLoc;
    }
    uint32_t delta3 = offset - prevOffset;
    pint_t value = (pint_t)P::getP(*prevLoc);
    pint_t newValue;
    if ( value == 0 )
        newValue = (delta3 << deltaShift);
    else
        newValue = ((value - valueAdd) & valueMask) | (delta3 << deltaShift);
    //warning("    non-rebase delta = %d, to off=0x%03X, old value=0x%08lX, new value=0x%08lX", delta3, offset, (long)value, (long)newValue);
    P::setP(*prevLoc, newValue);

    return true;
}


template <typename P>
void CacheBuilder::addPageStarts(uint8_t* pageContent, const bool bitmap[], const dyld_cache_slide_info2* info,
                                std::vector<uint16_t>& pageStarts, std::vector<uint16_t>& pageExtras)
{
    typedef typename P::uint_t     pint_t;

    const pint_t   deltaMask    = (pint_t)(info->delta_mask);
    const pint_t   valueMask    = ~deltaMask;
    const uint32_t pageSize     = info->page_size;
    const pint_t   valueAdd     = (pint_t)(info->value_add);

    uint16_t startValue = DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE;
    uint16_t lastLocationOffset = 0xFFFF;
    for(int i=0; i < pageSize/4; ++i) {
        unsigned offset = i*4;
        if ( bitmap[i] ) {
            if ( startValue == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE ) {
                // found first rebase location in page
                startValue = i;
            }
            else if ( !makeRebaseChain<P>(pageContent, lastLocationOffset, offset, info) ) {
                // can't record all rebasings in one chain
                if ( (startValue & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA) == 0 ) {
                    // switch page_start to "extras" which is a list of chain starts
                    unsigned indexInExtras = (unsigned)pageExtras.size();
                    if ( indexInExtras > 0x3FFF ) {
                        _diagnostics.error("rebase overflow in page extras");
                        return;
                    }
                    pageExtras.push_back(startValue);
                    startValue = indexInExtras | DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA;
                }
                pageExtras.push_back(i);
            }
            lastLocationOffset = offset;
        }
    }
    if ( lastLocationOffset != 0xFFFF ) {
        // mark end of chain
        pint_t* lastLoc = (pint_t*)&pageContent[lastLocationOffset];
        pint_t lastValue = (pint_t)P::getP(*lastLoc);
        pint_t newValue = ((lastValue - valueAdd) & valueMask);
        P::setP(*lastLoc, newValue);
    }
    if ( startValue & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA ) {
        // add end bit to extras
        pageExtras.back() |= DYLD_CACHE_SLIDE_PAGE_ATTR_END;
    }
    pageStarts.push_back(startValue);
}

template <typename P>
void CacheBuilder::writeSlideInfoV2()
{
    typedef typename P::uint_t    pint_t;
    typedef typename P::E         E;
    const uint32_t pageSize = 4096;

    // build one 1024/4096 bool bitmap per page (4KB/16KB) of DATA
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    uint8_t* const dataStart = (uint8_t*)_buffer + mappings[1].fileOffset;
    uint8_t* const dataEnd   = dataStart + mappings[1].size;
    unsigned pageCount = (unsigned)(mappings[1].size+pageSize-1)/pageSize;
    const long bitmapSize = pageCount*(pageSize/4)*sizeof(bool);
    bool* bitmap = (bool*)calloc(bitmapSize, 1);
    for (void* p : _pointersForASLR) {
        if ( (p < dataStart) || ( p > dataEnd) ) {
            _diagnostics.error("DATA pointer for sliding, out of range\n");
            free(bitmap);
            return;
        }
        long byteOffset = (long)((uint8_t*)p - dataStart);
        if ( (byteOffset % 4) != 0 ) {
            _diagnostics.error("pointer not 4-byte aligned in DATA offset 0x%08lX\n", byteOffset);
            free(bitmap);
            return;
        }
        long boolIndex = byteOffset / 4;
        // work around <rdar://24941083> by ignoring pointers to be slid that are NULL on disk
        if ( *((pint_t*)p) == 0 ) {
            std::string dylibName;
            std::string segName;
            findDylibAndSegment(p, dylibName, segName);
            _diagnostics.warning("NULL pointer asked to be slid in %s at DATA region offset 0x%04lX of %s", segName.c_str(), byteOffset, dylibName.c_str());
            continue;
        }
        bitmap[boolIndex] = true;
    }

    // fill in fixed info
    assert(_slideInfoFileOffset != 0);
    dyld_cache_slide_info2* info = (dyld_cache_slide_info2*)((uint8_t*)_buffer + _slideInfoFileOffset);
    info->version    = 2;
    info->page_size  = pageSize;
    info->delta_mask = _archLayout->pointerDeltaMask;
    info->value_add  = (sizeof(pint_t) == 8) ? 0 : _archLayout->sharedMemoryStart;  // only value_add for 32-bit archs

    // set page starts and extras for each page
    std::vector<uint16_t> pageStarts;
    std::vector<uint16_t> pageExtras;
    pageStarts.reserve(pageCount);
    uint8_t* pageContent = dataStart;;
    const bool* bitmapForPage = bitmap;
    for (unsigned i=0; i < pageCount; ++i) {
        //warning("page[%d]", i);
        addPageStarts<P>(pageContent, bitmapForPage, info, pageStarts, pageExtras);
        if ( _diagnostics.hasError() ) {
            free(bitmap);
            return;
        }
        pageContent += pageSize;
        bitmapForPage += (sizeof(bool)*(pageSize/4));
    }
    free((void*)bitmap);

    // fill in computed info
    info->page_starts_offset = sizeof(dyld_cache_slide_info2);
    info->page_starts_count  = (unsigned)pageStarts.size();
    info->page_extras_offset = (unsigned)(sizeof(dyld_cache_slide_info2)+pageStarts.size()*sizeof(uint16_t));
    info->page_extras_count  = (unsigned)pageExtras.size();
    uint16_t* pageStartsBuffer = (uint16_t*)((char*)info + info->page_starts_offset);
    uint16_t* pageExtrasBuffer = (uint16_t*)((char*)info + info->page_extras_offset);
    for (unsigned i=0; i < pageStarts.size(); ++i)
        pageStartsBuffer[i] = pageStarts[i];
    for (unsigned i=0; i < pageExtras.size(); ++i)
        pageExtrasBuffer[i] = pageExtras[i];
    // update header with final size
    _buffer->header.slideInfoSize = align(info->page_extras_offset + pageExtras.size()*sizeof(uint16_t), _archLayout->sharedRegionAlignP2);
    if ( _buffer->header.slideInfoSize > _slideInfoBufferSizeAllocated ) {
        _diagnostics.error("kernel slide info overflow buffer");
    }
    //warning("pageCount=%u, page_starts_count=%lu, page_extras_count=%lu", pageCount, pageStarts.size(), pageExtras.size());
}


/*
void CacheBuilder::writeSlideInfoV1()
{
    // build one 128-byte bitmap per page (4096) of DATA
    uint8_t* const dataStart = (uint8_t*)_buffer.get() + regions[1].fileOffset;
    uint8_t* const dataEnd   = dataStart + regions[1].size;
    const long bitmapSize = (dataEnd - dataStart)/(4*8);
    uint8_t* bitmap = (uint8_t*)calloc(bitmapSize, 1);
    for (void* p : _pointersForASLR) {
        if ( (p < dataStart) || ( p > dataEnd) )
            terminate("DATA pointer for sliding, out of range\n");
        long offset = (long)((uint8_t*)p - dataStart);
        if ( (offset % 4) != 0 )
            terminate("pointer not 4-byte aligned in DATA offset 0x%08lX\n", offset);
        long byteIndex = offset / (4*8);
        long bitInByte =  (offset % 32) >> 2;
        bitmap[byteIndex] |= (1 << bitInByte);
    }

    // allocate worst case size block of all slide info
    const unsigned entry_size = 4096/(8*4); // 8 bits per byte, possible pointer every 4 bytes.
    const unsigned toc_count = (unsigned)bitmapSize/entry_size;
    dyld_cache_slide_info* slideInfo = (dyld_cache_slide_info*)((uint8_t*)_buffer + _slideInfoFileOffset);
    slideInfo->version          = 1;
    slideInfo->toc_offset       = sizeof(dyld_cache_slide_info);
    slideInfo->toc_count        = toc_count;
    slideInfo->entries_offset   = (slideInfo->toc_offset+2*toc_count+127)&(-128);
    slideInfo->entries_count    = 0;
    slideInfo->entries_size     = entry_size;
    // append each unique entry
    const dyldCacheSlideInfoEntry* bitmapAsEntries = (dyldCacheSlideInfoEntry*)bitmap;
    dyldCacheSlideInfoEntry* const entriesInSlidInfo = (dyldCacheSlideInfoEntry*)((char*)slideInfo+slideInfo->entries_offset());
    int entry_count = 0;
    for (int i=0; i < toc_count; ++i) {
        const dyldCacheSlideInfoEntry* thisEntry = &bitmapAsEntries[i];
        // see if it is same as one already added
        bool found = false;
        for (int j=0; j < entry_count; ++j) {
            if ( memcmp(thisEntry, &entriesInSlidInfo[j], entry_size) == 0 ) {
                slideInfo->set_toc(i, j);
                found = true;
                break;
            }
        }
        if ( !found ) {
            // append to end
            memcpy(&entriesInSlidInfo[entry_count], thisEntry, entry_size);
            slideInfo->set_toc(i, entry_count++);
        }
    }
    slideInfo->entries_count  = entry_count;
    ::free((void*)bitmap);

    _buffer.header->slideInfoSize = align(slideInfo->entries_offset + entry_count*entry_size, _archLayout->sharedRegionAlignP2);
}

*/

void CacheBuilder::fipsSign() {
    __block bool found = false;
    _buffer->forEachImage(^(const mach_header* mh, const char* installName) {
        __block void *hash_location = nullptr;
        // Return if this is not corecrypto
        if (strcmp(installName, "/usr/lib/system/libcorecrypto.dylib") != 0) {
            return;
        }
        found = true;
        auto parser = dyld3::MachOParser(mh, true);
        parser.forEachLocalSymbol(_diagnostics, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
            if (strcmp(symbolName, "_fipspost_precalc_hmac") != 0)
                return;
            hash_location = (void *)(n_value - _archLayout->sharedMemoryStart + (uintptr_t)_buffer);
            stop = true;
        });

        // Bail out if we did not find the symbol
        if (hash_location == nullptr) {
            _diagnostics.warning("Could not find _fipspost_precalc_hmac, skipping FIPS sealing");
            return;
        }

        parser.forEachSection(^(const char *segName, const char *sectionName, uint32_t flags, const void *content, size_t size, bool illegalSectionSize, bool &stop) {
            // FIXME: If we ever implement userspace __TEXT_EXEC this will need to be updated
            if ( (strcmp(segName, "__TEXT" ) != 0) || (strcmp(sectionName, "__text") != 0) )  {
                return;
            }

            if (illegalSectionSize) {
                _diagnostics.error("FIPS section %s/%s extends beyond the end of the segment", segName, sectionName);
                return;
            }

            //We have _fipspost_precalc_hmac and __TEXT,__text, seal it
            unsigned char hmac_key = 0;
            CCHmac(kCCHmacAlgSHA256, &hmac_key, 1, content, size, hash_location);
            stop = true;
        });
    });

    if (!found) {
        _diagnostics.warning("Could not find /usr/lib/system/libcorecrypto.dylib, skipping FIPS sealing");
    }
}

void CacheBuilder::codeSign()
{
    uint8_t  dscHashType;
    uint8_t  dscHashSize;
    uint32_t dscDigestFormat;
    bool agile = false;

    // select which codesigning hash
    switch (_options.codeSigningDigestMode) {
        case DyldSharedCache::Agile:
            agile = true;
            // Fall through to SHA1, because the main code directory remains SHA1 for compatibility.
        case DyldSharedCache::SHA1only:
            dscHashType     = CS_HASHTYPE_SHA1;
            dscHashSize     = CS_HASH_SIZE_SHA1;
            dscDigestFormat = kCCDigestSHA1;
            break;
        case DyldSharedCache::SHA256only:
            dscHashType     = CS_HASHTYPE_SHA256;
            dscHashSize     = CS_HASH_SIZE_SHA256;
            dscDigestFormat = kCCDigestSHA256;
            break;
        default:
            _diagnostics.error("codeSigningDigestMode has unknown, unexpected value %d, bailing out.",
                               _options.codeSigningDigestMode);
            return;
    }

    std::string cacheIdentifier = "com.apple.dyld.cache." + _options.archName;
    if ( _options.dylibsRemovedDuringMastering ) {
        if ( _options.optimizeStubs  )
            cacheIdentifier = "com.apple.dyld.cache." + _options.archName + ".release";
        else
            cacheIdentifier = "com.apple.dyld.cache." + _options.archName + ".development";
    }
    // get pointers into shared cache buffer
    size_t          inBbufferSize = _currentFileSize;
    const uint8_t*  inBuffer = (uint8_t*)_buffer;
    uint8_t*        csBuffer = (uint8_t*)_buffer+inBbufferSize;

    // layout code signature contents
    uint32_t blobCount     = agile ? 4 : 3;
    size_t   idSize        = cacheIdentifier.size()+1; // +1 for terminating 0
    uint32_t slotCount     = (uint32_t)((inBbufferSize + CS_PAGE_SIZE - 1) / CS_PAGE_SIZE);
    uint32_t xSlotCount    = CSSLOT_REQUIREMENTS;
    size_t   idOffset      = offsetof(CS_CodeDirectory, end_withExecSeg);
    size_t   hashOffset    = idOffset+idSize + dscHashSize*xSlotCount;
    size_t   hash256Offset = idOffset+idSize + CS_HASH_SIZE_SHA256*xSlotCount;
    size_t   cdSize        = hashOffset + (slotCount * dscHashSize);
    size_t   cd256Size     = agile ? hash256Offset + (slotCount * CS_HASH_SIZE_SHA256) : 0;
    size_t   reqsSize      = 12;
    size_t   cmsSize       = sizeof(CS_Blob);
    size_t   cdOffset      = sizeof(CS_SuperBlob) + blobCount*sizeof(CS_BlobIndex);
    size_t   cd256Offset   = cdOffset + cdSize;
    size_t   reqsOffset    = cd256Offset + cd256Size; // equals cdOffset + cdSize if not agile
    size_t   cmsOffset     = reqsOffset + reqsSize;
    size_t   sbSize        = cmsOffset + cmsSize;
    size_t   sigSize       = align(sbSize, 14);       // keep whole cache 16KB aligned

    if ( _currentFileSize+sigSize > _allocatedBufferSize ) {
        _diagnostics.error("cache buffer too small to hold code signature (buffer size=%lldMB, signature size=%ldMB, free space=%lldMB)",
                            _allocatedBufferSize/1024/1024, sigSize/1024/1024, (_allocatedBufferSize-_currentFileSize)/1024/1024);
        return;
    }

    // create overall code signature which is a superblob
    CS_SuperBlob* sb = reinterpret_cast<CS_SuperBlob*>(csBuffer);
    sb->magic           = htonl(CSMAGIC_EMBEDDED_SIGNATURE);
    sb->length          = htonl(sbSize);
    sb->count           = htonl(blobCount);
    sb->index[0].type   = htonl(CSSLOT_CODEDIRECTORY);
    sb->index[0].offset = htonl(cdOffset);
    sb->index[1].type   = htonl(CSSLOT_REQUIREMENTS);
    sb->index[1].offset = htonl(reqsOffset);
    sb->index[2].type   = htonl(CSSLOT_CMS_SIGNATURE);
    sb->index[2].offset = htonl(cmsOffset);
    if ( agile ) {
        sb->index[3].type = htonl(CSSLOT_ALTERNATE_CODEDIRECTORIES + 0);
        sb->index[3].offset = htonl(cd256Offset);
    }

    // fill in empty requirements
    CS_RequirementsBlob* reqs = (CS_RequirementsBlob*)(((char*)sb)+reqsOffset);
    reqs->magic  = htonl(CSMAGIC_REQUIREMENTS);
    reqs->length = htonl(sizeof(CS_RequirementsBlob));
    reqs->data   = 0;

    // initialize fixed fields of Code Directory
    CS_CodeDirectory* cd = (CS_CodeDirectory*)(((char*)sb)+cdOffset);
    cd->magic           = htonl(CSMAGIC_CODEDIRECTORY);
    cd->length          = htonl(cdSize);
    cd->version         = htonl(0x20400);               // supports exec segment
    cd->flags           = htonl(kSecCodeSignatureAdhoc);
    cd->hashOffset      = htonl(hashOffset);
    cd->identOffset     = htonl(idOffset);
    cd->nSpecialSlots   = htonl(xSlotCount);
    cd->nCodeSlots      = htonl(slotCount);
    cd->codeLimit       = htonl(inBbufferSize);
    cd->hashSize        = dscHashSize;
    cd->hashType        = dscHashType;
    cd->platform        = 0;                            // not platform binary
    cd->pageSize        = __builtin_ctz(CS_PAGE_SIZE);  // log2(CS_PAGE_SIZE);
    cd->spare2          = 0;                            // unused (must be zero)
    cd->scatterOffset   = 0;                            // not supported anymore
    cd->teamOffset      = 0;                            // no team ID
    cd->spare3          = 0;                            // unused (must be zero)
    cd->codeLimit64     = 0;                            // falls back to codeLimit

    // executable segment info
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    cd->execSegBase     = htonll(mappings[0].fileOffset); // base of TEXT segment
    cd->execSegLimit    = htonll(mappings[0].size);     // size of TEXT segment
    cd->execSegFlags    = 0;                            // not a main binary

    // initialize dynamic fields of Code Directory
    strcpy((char*)cd + idOffset, cacheIdentifier.c_str());

    // add special slot hashes
    uint8_t* hashSlot = (uint8_t*)cd + hashOffset;
    uint8_t* reqsHashSlot = &hashSlot[-CSSLOT_REQUIREMENTS*dscHashSize];
    CCDigest(dscDigestFormat, (uint8_t*)reqs, sizeof(CS_RequirementsBlob), reqsHashSlot);

    CS_CodeDirectory* cd256;
    uint8_t* hash256Slot;
    uint8_t* reqsHash256Slot;
    if ( agile ) {
        // Note that the assumption here is that the size up to the hashes is the same as for
        // sha1 code directory, and that they come last, after everything else.

        cd256 = (CS_CodeDirectory*)(((char*)sb)+cd256Offset);
        cd256->magic           = htonl(CSMAGIC_CODEDIRECTORY);
        cd256->length          = htonl(cd256Size);
        cd256->version         = htonl(0x20400);               // supports exec segment
        cd256->flags           = htonl(kSecCodeSignatureAdhoc);
        cd256->hashOffset      = htonl(hash256Offset);
        cd256->identOffset     = htonl(idOffset);
        cd256->nSpecialSlots   = htonl(xSlotCount);
        cd256->nCodeSlots      = htonl(slotCount);
        cd256->codeLimit       = htonl(inBbufferSize);
        cd256->hashSize        = CS_HASH_SIZE_SHA256;
        cd256->hashType        = CS_HASHTYPE_SHA256;
        cd256->platform        = 0;                            // not platform binary
        cd256->pageSize        = __builtin_ctz(CS_PAGE_SIZE);  // log2(CS_PAGE_SIZE);
        cd256->spare2          = 0;                            // unused (must be zero)
        cd256->scatterOffset   = 0;                            // not supported anymore
        cd256->teamOffset      = 0;                            // no team ID
        cd256->spare3          = 0;                            // unused (must be zero)
        cd256->codeLimit64     = 0;                            // falls back to codeLimit

        // executable segment info
        cd256->execSegBase     = cd->execSegBase;
        cd256->execSegLimit    = cd->execSegLimit;
        cd256->execSegFlags    = cd->execSegFlags;

        // initialize dynamic fields of Code Directory
        strcpy((char*)cd256 + idOffset, cacheIdentifier.c_str());

        // add special slot hashes
        hash256Slot = (uint8_t*)cd256 + hash256Offset;
        reqsHash256Slot = &hash256Slot[-CSSLOT_REQUIREMENTS*CS_HASH_SIZE_SHA256];
        CCDigest(kCCDigestSHA256, (uint8_t*)reqs, sizeof(CS_RequirementsBlob), reqsHash256Slot);
    }
    else {
        cd256 = NULL;
        hash256Slot = NULL;
        reqsHash256Slot = NULL;
    }

    // fill in empty CMS blob for ad-hoc signing
    CS_Blob* cms = (CS_Blob*)(((char*)sb)+cmsOffset);
    cms->magic  = htonl(CSMAGIC_BLOBWRAPPER);
    cms->length = htonl(sizeof(CS_Blob));

    // alter header of cache to record size and location of code signature
    // do this *before* hashing each page
    _buffer->header.codeSignatureOffset = inBbufferSize;
    _buffer->header.codeSignatureSize   = sigSize;

    // compute hashes
    const uint8_t* code = inBuffer;
    for (uint32_t i=0; i < slotCount; ++i) {
        CCDigest(dscDigestFormat, code, CS_PAGE_SIZE, hashSlot);
        hashSlot += dscHashSize;

        if ( agile ) {
            CCDigest(kCCDigestSHA256, code, CS_PAGE_SIZE, hash256Slot);
            hash256Slot += CS_HASH_SIZE_SHA256;
        }
        code += CS_PAGE_SIZE;
    }

    // hash of entire code directory (cdHash) uses same hash as each page
    uint8_t fullCdHash[dscHashSize];
    CCDigest(dscDigestFormat, (const uint8_t*)cd, cdSize, fullCdHash);
    // Note: cdHash is defined as first 20 bytes of hash
    memcpy(_cdHashFirst, fullCdHash, 20);
    if ( agile ) {
        uint8_t fullCdHash256[CS_HASH_SIZE_SHA256];
        CCDigest(kCCDigestSHA256, (const uint8_t*)cd256, cd256Size, fullCdHash256);
        // Note: cdHash is defined as first 20 bytes of hash, even for sha256
        memcpy(_cdHashSecond, fullCdHash256, 20);
    }
    else {
        memset(_cdHashSecond, 0, 20);
    }

    // increase file size to include newly append code signature
    _currentFileSize += sigSize;
}

const bool CacheBuilder::agileSignature()
{
    return _options.codeSigningDigestMode == DyldSharedCache::Agile;
}

static const std::string cdHash(uint8_t hash[20])
{
    char buff[48];
    for (int i = 0; i < 20; ++i)
        sprintf(&buff[2*i], "%2.2x", hash[i]);
    return buff;
}

const std::string CacheBuilder::cdHashFirst()
{
    return cdHash(_cdHashFirst);
}

const std::string CacheBuilder::cdHashSecond()
{
    return cdHash(_cdHashSecond);
}

void CacheBuilder::addCachedDylibsImageGroup(dyld3::ImageProxyGroup* dylibGroup)
{
    const dyld3::launch_cache::binary_format::ImageGroup* groupBinary = dylibGroup->makeImageGroupBinary(_diagnostics, _s_neverStubEliminate);
    if (!groupBinary)
        return;

    dyld3::launch_cache::ImageGroup group(groupBinary);
    size_t groupSize = group.size();

    if ( _currentFileSize+groupSize > _allocatedBufferSize ) {
        _diagnostics.error("cache buffer too small to hold group[0] info (buffer size=%lldMB, group size=%ldMB, free space=%lldMB)",
                            _allocatedBufferSize/1024/1024, groupSize/1024/1024, (_allocatedBufferSize-_currentFileSize)/1024/1024);
        return;
    }

    // append ImageGroup data to read-only region of cache
    uint8_t* loc = (uint8_t*)_buffer + _currentFileSize;
    memcpy(loc, groupBinary, groupSize);
    dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    _buffer->header.dylibsImageGroupAddr = mappings[2].address + (_currentFileSize - mappings[2].fileOffset);
    _buffer->header.dylibsImageGroupSize = (uint32_t)groupSize;
    _currentFileSize += groupSize;
    free((void*)groupBinary);
}


void CacheBuilder::addCachedOtherDylibsImageGroup(dyld3::ImageProxyGroup* otherGroup)
{
    const dyld3::launch_cache::binary_format::ImageGroup* groupBinary = otherGroup->makeImageGroupBinary(_diagnostics);
    if (!groupBinary)
        return;

    dyld3::launch_cache::ImageGroup group(groupBinary);
    size_t groupSize = group.size();

    if ( _currentFileSize+groupSize > _allocatedBufferSize ) {
        _diagnostics.error("cache buffer too small to hold group[1] info (buffer size=%lldMB, group size=%ldMB, free space=%lldMB)",
                            _allocatedBufferSize/1024/1024, groupSize/1024/1024, (_allocatedBufferSize-_currentFileSize)/1024/1024);
        return;
    }

    // append ImageGroup data to read-only region of cache
    uint8_t* loc = (uint8_t*)_buffer + _currentFileSize;
    memcpy(loc, groupBinary, groupSize);
    dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    _buffer->header.otherImageGroupAddr = mappings[2].address + (_currentFileSize - mappings[2].fileOffset);
    _buffer->header.otherImageGroupSize = (uint32_t)groupSize;
    _currentFileSize += groupSize;
    free((void*)groupBinary);
}

void CacheBuilder::addClosures(const std::map<std::string, const dyld3::launch_cache::binary_format::Closure*>& closures)
{
    // preflight space needed
    size_t closuresSpace = 0;
    for (const auto& entry : closures) {
        dyld3::launch_cache::Closure closure(entry.second);
        closuresSpace += closure.size();
    }
    size_t freeSpace = _allocatedBufferSize - _currentFileSize;
    if ( closuresSpace > freeSpace ) {
        _diagnostics.error("cache buffer too small to hold all closures (buffer size=%lldMB, closures size=%ldMB, free space=%ldMB)",
                            _allocatedBufferSize/1024/1024, closuresSpace/1024/1024, freeSpace/1024/1024);
        return;
    }

    dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)_buffer + _buffer->header.mappingOffset);
    _buffer->header.progClosuresAddr = mappings[2].address + (_currentFileSize - mappings[2].fileOffset);
    uint8_t* closuresBase = (uint8_t*)_buffer + _currentFileSize;
    std::vector<DylibIndexTrie::Entry> closureEntrys;
    uint32_t currentClosureOffset = 0;
    for (const auto& entry : closures) {
        const dyld3::launch_cache::binary_format::Closure* closBuf = entry.second;
        closureEntrys.push_back(DylibIndexTrie::Entry(entry.first, DylibIndex(currentClosureOffset)));
        dyld3::launch_cache::Closure closure(closBuf);
        size_t size = closure.size();
        assert((size % 4) == 0);
        memcpy(closuresBase+currentClosureOffset, closBuf, size);
        currentClosureOffset += size;
        freeSpace -= size;
        free((void*)closBuf);
    }
    _buffer->header.progClosuresSize = currentClosureOffset;
    _currentFileSize += currentClosureOffset;
    freeSpace = _allocatedBufferSize - _currentFileSize;

    // build trie of indexes into closures list
    DylibIndexTrie closureTrie(closureEntrys);
    std::vector<uint8_t> trieBytes;
    closureTrie.emit(trieBytes);
    while ( (trieBytes.size() % 8) != 0 )
        trieBytes.push_back(0);
    if ( trieBytes.size() > freeSpace ) {
        _diagnostics.error("cache buffer too small to hold all closures trie (buffer size=%lldMB, trie size=%ldMB, free space=%ldMB)",
                            _allocatedBufferSize/1024/1024, trieBytes.size()/1024/1024, freeSpace/1024/1024);
        return;
    }
    memcpy((uint8_t*)_buffer + _currentFileSize, &trieBytes[0], trieBytes.size());
    _buffer->header.progClosuresTrieAddr = mappings[2].address + (_currentFileSize - mappings[2].fileOffset);
    _buffer->header.progClosuresTrieSize = trieBytes.size();
    _currentFileSize += trieBytes.size();
}


