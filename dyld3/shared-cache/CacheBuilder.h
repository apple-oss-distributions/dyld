/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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


#ifndef CacheBuilder_h
#define CacheBuilder_h

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "DyldSharedCache.h"
#include "Diagnostics.h"
#include "ImageProxy.h"


namespace  dyld3 {
  namespace launch_cache {
    namespace binary_format {
      struct ImageGroup;
      struct Closure;
    }
  }
}


struct CacheBuilder {

                                        CacheBuilder(const DyldSharedCache::CreateOptions& options);

    void                                build(const std::vector<DyldSharedCache::MappedMachO>&  dylibsToCache,
                                              const std::vector<DyldSharedCache::MappedMachO>&  otherOsDylibs,
                                              const std::vector<DyldSharedCache::MappedMachO>&  osExecutables);
    void                                deleteBuffer();
    const DyldSharedCache*              buffer() { return _buffer; }
    size_t                              bufferSize() { return (size_t)_allocatedBufferSize; }
    std::string                         errorMessage();
    const std::set<std::string>         warnings();
    const std::set<const mach_header*>  evictions();
    const bool                          agileSignature();
    const std::string                   cdHashFirst();
    const std::string                   cdHashSecond();

    struct SegmentMappingInfo {
        const void*     srcSegment;
        const char*     segName;
        uint64_t        dstCacheAddress;
        uint32_t        dstCacheOffset;
        uint32_t        dstCacheSegmentSize;
        uint32_t        copySegmentSize;
        uint32_t        srcSegmentIndex;
    };

private:

    typedef std::unordered_map<const mach_header*, std::vector<SegmentMappingInfo>> SegmentMapping;

    struct ArchLayout
    {
        uint64_t    sharedMemoryStart;
        uint64_t    sharedMemorySize;
        uint64_t    sharedRegionPadding;
        uint64_t    pointerDeltaMask;
        const char* archName;
        uint32_t    branchPoolTextSize;
        uint32_t    branchPoolLinkEditSize;
        uint32_t    branchReach;
        uint8_t     sharedRegionAlignP2;
        bool        sharedRegionsAreDiscontiguous;
        bool        is64;
    };

    static const ArchLayout  _s_archLayout[];
    static const char* const _s_neverStubEliminate[];

    std::vector<DyldSharedCache::MappedMachO> makeSortedDylibs(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const std::unordered_map<std::string, unsigned> sortOrder);

    SegmentMapping assignSegmentAddresses(const std::vector<DyldSharedCache::MappedMachO>& dylibs, struct dyld_cache_mapping_info regions[3]);

    bool        cacheOverflow(const dyld_cache_mapping_info regions[3]);
    void        adjustImageForNewSegmentLocations(const std::vector<uint64_t>& segNewStartAddresses,
                                                 const std::vector<uint64_t>& segCacheFileOffsets,
                                                 const std::vector<uint64_t>& segCacheSizes, std::vector<void*>& pointersForASLR);

    void        fipsSign();
    void        codeSign();
    uint64_t    pathHash(const char* path);
    void        writeCacheHeader(const struct dyld_cache_mapping_info regions[3], const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping&);
    void        copyRawSegments(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping& mapping);
    void        adjustAllImagesForNewSegmentLocations(const std::vector<DyldSharedCache::MappedMachO>& dylibs, const SegmentMapping& mapping);
    void        bindAllImagesInCacheFile(const dyld_cache_mapping_info regions[3]);
    void        writeSlideInfoV1();
    void        recomputeCacheUUID(void);
    void        findDylibAndSegment(const void* contentPtr, std::string& dylibName, std::string& segName);

    void        addCachedDylibsImageGroup(dyld3::ImageProxyGroup*);
    void        addCachedOtherDylibsImageGroup(dyld3::ImageProxyGroup*);
    void        addClosures(const std::map<std::string, const dyld3::launch_cache::binary_format::Closure*>& closures);

    template <typename P> void writeSlideInfoV2();
    template <typename P> bool makeRebaseChain(uint8_t* pageContent, uint16_t lastLocationOffset, uint16_t newOffset, const struct dyld_cache_slide_info2* info);
    template <typename P> void addPageStarts(uint8_t* pageContent, const bool bitmap[], const struct dyld_cache_slide_info2* info,
                                             std::vector<uint16_t>& pageStarts, std::vector<uint16_t>& pageExtras);


    const DyldSharedCache::CreateOptions&       _options;
    DyldSharedCache*                            _buffer;
    Diagnostics                                 _diagnostics;
    std::set<const mach_header*>               _evictions;
    const ArchLayout*                           _archLayout;
    uint32_t                                    _aliasCount;
    uint64_t                                    _slideInfoFileOffset;
    uint64_t                                    _slideInfoBufferSizeAllocated;
    uint64_t                                    _allocatedBufferSize;
    uint64_t                                    _currentFileSize;
    uint64_t                                    _vmSize;
    std::unordered_map<std::string, uint32_t>   _dataDirtySegsOrder;
    std::vector<void*>                          _pointersForASLR;
    dyld3::ImageProxyGroup::PatchTable          _patchTable;
    std::vector<uint64_t>                       _branchPoolStarts;
    uint64_t                                    _branchPoolsLinkEditStartAddr;
    uint8_t                                     _cdHashFirst[20];
    uint8_t                                     _cdHashSecond[20];
};


// implemented in AdjustDylibSegemnts.cpp
void        adjustDylibSegments(DyldSharedCache* cache, bool is64, mach_header* mhInCache, const std::vector<CacheBuilder::SegmentMappingInfo>& mappingInfo, std::vector<void*>& pointersForASLR, Diagnostics& diag);

// implemented in OptimizerLinkedit.cpp
uint64_t    optimizeLinkedit(DyldSharedCache* cache, bool is64, bool dontMapLocalSymbols, bool addAcceleratorTables, const std::vector<uint64_t>& branchPoolOffsets, Diagnostics& diag, dyld_cache_local_symbols_info** localsInfo);

// implemented in OptimizerBranches.cpp
void        bypassStubs(DyldSharedCache* cache, const std::vector<uint64_t>& branchPoolStartAddrs, const char* const alwaysUsesStubsTo[], Diagnostics& diag);

// implemented in OptimizerObjC.cpp
void        optimizeObjC(DyldSharedCache* cache, bool is64, bool customerCache, std::vector<void*>& pointersForASLR, Diagnostics& diag);



inline uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}



#endif /* CacheBuilder_h */
