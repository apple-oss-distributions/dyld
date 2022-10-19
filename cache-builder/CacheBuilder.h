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
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "ClosureFileSystem.h"
#include "DyldSharedCache.h"
#include "Diagnostics.h"
#include "MachOAnalyzer.h"
#include "IMPCaches.hpp"

namespace cache_builder
{
class ASLR_Tracker;
}

struct UnmappedLocalsOptimizer;

class CacheBuilder {
public:
    CacheBuilder(const DyldSharedCache::CreateOptions& options, const dyld3::closure::FileSystem& fileSystem);
    virtual ~CacheBuilder();

    struct InputFile {
        enum State {
            Unset,
            MustBeIncluded,
            MustBeIncludedForDependent,
            MustBeExcludedIfUnused
        };
        InputFile(const char* path, State state) : path(path), state(state) { }
        const char*     path;
        State           state = Unset;
        Diagnostics     diag;

        bool mustBeIncluded() const {
            return (state == MustBeIncluded) || (state == MustBeIncludedForDependent);
        }
    };

    // Contains a MachO which has been loaded from the file system and may potentially need to be unloaded later.
    struct LoadedMachO {
        DyldSharedCache::MappedMachO    mappedFile;
        dyld3::closure::LoadedFileInfo  loadedFileInfo;
        InputFile*                      inputFile;
    };

    std::string                                 errorMessage();

    struct Region
    {
        uint8_t*    buffer                          = nullptr;
        uint64_t    bufferSize                      = 0;
        uint64_t    sizeInUse                       = 0;
        uint64_t    unslidLoadAddress               = 0;
        uint64_t    cacheFileOffset                 = 0;
        uint8_t     initProt                        = 0;
        uint8_t     maxProt                         = 0;
        std::string name;
        uint64_t    index                  = ~0ULL; // The index of this region in the final binary

        // Each region can optionally have its own slide info
        uint8_t*    slideInfoBuffer                 = nullptr;
        uint64_t    slideInfoBufferSizeAllocated    = 0;
        uint64_t    slideInfoFileOffset             = 0;
        uint64_t    slideInfoFileSize               = 0;
    };

    struct SegmentMappingInfo {
        const void*     srcSegment;
        const char*     segName;
        uint8_t*        dstSegment;
        uint64_t        dstCacheUnslidAddress;
        uint32_t        dstCacheFileOffset;
        uint32_t        dstCacheSegmentSize;
        uint32_t        dstCacheFileSize;
        uint32_t        copySegmentSize;
        uint32_t        srcSegmentIndex;
        // Used by the AppCacheBuilder to work out which one of the regions this segment is in
        const Region*   parentRegion            = nullptr;
    };

    struct CoalescedSection
    {
        uint8_t*                                    bufferAddr       = nullptr;
        uint32_t                                    bufferSize       = 0;
        uint64_t                                    bufferVMAddr     = 0;

        // Note this is for debugging only
        uint64_t                                    savedSpace       = 0;
    };

    struct CoalescedStringsSection : CoalescedSection
    {
        CoalescedStringsSection(std::string_view sectionName) : sectionName(sectionName) { }

        void clear()
        {
            *this = CoalescedStringsSection(this->sectionName);
        }

        std::string_view                        sectionName;
        // Map from class strings to offsets in to the strings buffer
        std::map<std::string_view, uint32_t>    stringsToOffsets;
    };

    struct CoalescedGOTSection : CoalescedSection
    {
        struct GOTKey
        {
            std::string_view                    targetSymbolName;
            std::string_view                    targetDylibName;
            dyld3::MachOLoaded::PointerMetaData pmd;
        };

        struct Hash
        {
            size_t operator()(const GOTKey& v) const
            {
                static_assert(sizeof(v.pmd) == sizeof(uint32_t));

                size_t hash = 0;
                hash ^= std::hash<std::string_view>{}(v.targetSymbolName);
                hash ^= std::hash<std::string_view>{}(v.targetDylibName);
                hash ^= std::hash<uint32_t>{}(*(uint32_t*)&v.pmd);
                return hash;
            }
        };

        struct EqualTo
        {
            bool operator()(const GOTKey& a, const GOTKey& b) const
            {
                return (a.targetSymbolName == b.targetSymbolName)
                    && (a.targetDylibName == b.targetDylibName)
                    && (memcmp(&a.pmd, &b.pmd, sizeof(a.pmd)) == 0);
            }
        };

        // Map from bind target to offsets in to the GOTs buffer
        std::unordered_map<GOTKey, uint32_t, Hash, EqualTo> gotTargetsToOffsets;
    };

    struct DylibSectionCoalescer
    {

        typedef std::map<uint32_t, uint32_t> DylibSectionOffsetToCacheSectionOffset;

        // A section may be completely coalesced and removed, eg, strings,
        // or it may be coalesced and copies made elsewhere, eg, GOTs.  In the GOTs case, we
        // don't remove the original section
        struct OptimizedSection
        {
            OptimizedSection(bool sectionWillBeRemoved, bool sectionIsObliterated)
                : sectionWillBeRemoved(sectionWillBeRemoved), sectionIsObliterated(sectionIsObliterated) { }

            void clear()
            {
                this->offsetMap.clear();
                this->subCacheSection = nullptr;
            }

            DylibSectionOffsetToCacheSectionOffset  offsetMap;

            // Some offsets are not in the above offsetMap, even though we'd typically want to know about every
            // reference to the given section.  Eg, we only optimize binds in __got, not rebases.  But we want
            // to track the rebases just so that we know of every element in the section.
            std::set<uint32_t>                      unoptimizedOffsets;

            // Different subCache's may contain their own GOTs/strings.  We can't deduplicate
            // cache-wide in to a single buffer due to constraints such as 32-bit offsets
            // This points to the cache section we coalesced into, for this section in this dylib
            CoalescedSection*                       subCacheSection = nullptr;

            // Whether or not this section will be removed.  Eg, GOTs aren't currently removed from
            // their original binary
            bool sectionWillBeRemoved;

            // Whether this section was totally destroyed, ie, is not present in any form in the final binary
            // This corresponds to stubs which aren't just removed but are also not coalesced or merged in to
            // some other section.  The final binary just won't have stubs
            bool sectionIsObliterated;
        };

        bool                    sectionWasRemoved(std::string_view segmentName, std::string_view sectionName) const;
        bool                    sectionWasObliterated(std::string_view segmentName, std::string_view sectionName) const;
        bool                    sectionWasOptimized(std::string_view segmentName, std::string_view sectionName) const;
        OptimizedSection*       getSection(std::string_view segmentName, std::string_view sectionName);
        const OptimizedSection* getSection(std::string_view segmentName, std::string_view sectionName) const;
        void                    clear();

        OptimizedSection objcClassNames = { true,  false };
        OptimizedSection objcMethNames  = { true,  false };
        OptimizedSection objcMethTypes  = { true,  false };
        OptimizedSection auth_stubs     = { false, false };
        OptimizedSection gots           = { false, false };
        OptimizedSection auth_gots      = { false, false };
    };

    typedef std::map<uint64_t, std::set<void*>> LOH_Tracker;

    // For use by the LinkeditOptimizer to work out which symbols to strip on each binary
    enum class DylibStripMode {
        stripNone,
        stripLocals,
        stripExports,
        stripAll
    };

    struct DylibInfo
    {
        const LoadedMachO*              input;
        std::string                     dylibID;
        std::vector<SegmentMappingInfo> cacheLocation;
    };

    struct StubOptimizerInfo
    {
        const mach_header*                              mh          = nullptr;
        const char*                                     dylibID     = nullptr;
        const DylibSectionCoalescer::OptimizedSection*  gots        = nullptr;
        const DylibSectionCoalescer::OptimizedSection*  auth_gots   = nullptr;
    };

protected:

    struct UnmappedRegion
    {
        uint8_t*    buffer                 = nullptr;
        uint64_t    bufferSize             = 0;
        uint64_t    sizeInUse              = 0;
    };

    // Virtual methods overridden by the shared cache builder and app cache builder
    virtual void forEachDylibInfo(void (^callback)(const DylibInfo& dylib, Diagnostics& dylibDiag,
                                                   cache_builder::ASLR_Tracker& dylibASLRTracker,
                                                   const CacheBuilder::DylibSectionCoalescer* sectionCoalescer)) = 0;

    void        copyRawSegments();
    void        adjustAllImagesForNewSegmentLocations(uint64_t cacheBaseAddress,
                                                      LOH_Tracker* lohTracker);

    // implemented in AdjustDylibSegemnts.cpp
    void        adjustDylibSegments(const DylibInfo& dylib, Diagnostics& diag,
                                    uint64_t cacheBaseAddress,
                                    cache_builder::ASLR_Tracker& aslrTracker,
                                    CacheBuilder::LOH_Tracker* lohTracker,
                                    const CacheBuilder::DylibSectionCoalescer* sectionCoalescer) const;

    // implemented in OptimizerLinkedit.cpp
    void        optimizeLinkedit(CacheBuilder::Region& readOnlyRegion,
                                 uint64_t nonLinkEditReadOnlySize,
                                 UnmappedLocalsOptimizer* localSymbolsOptimizer,
                                 const std::vector<std::tuple<const mach_header*, const char*, DylibStripMode>>& images);

    UnmappedLocalsOptimizer* createLocalsOptimizer(uint64_t numDylibs);
    void destroyLocalsOptimizer(UnmappedLocalsOptimizer* locals);
    void emitLocalSymbols(UnmappedLocalsOptimizer* locals);

    // implemented in OptimizerBranches.cpp
    void        optimizeAwayStubs(const std::vector<StubOptimizerInfo>& images,
                                  int64_t cacheSlide, const DyldSharedCache* dyldCache,
                                  const std::unordered_map<uint64_t, std::pair<uint64_t, uint8_t*>>& stubsToIslandAddr,
                                  const char* const neverStubEliminateSymbols[]);

    const DyldSharedCache::CreateOptions&       _options;
    const dyld3::closure::FileSystem&           _fileSystem;
    UnmappedRegion                              _localSymbolsRegion;
    vm_address_t                                _fullAllocatedBuffer;
    Diagnostics                                 _diagnostics;
    TimeRecorder                                _timeRecorder;
    uint64_t                                    _allocatedBufferSize;
    std::vector<CoalescedGOTSection>            _subCacheCoalescedGOTs;
    CoalescedStringsSection                     _objcCoalescedClassNames    = { "objc class names" };
    CoalescedStringsSection                     _objcCoalescedMethodNames   = { "objc method names" };;
    CoalescedStringsSection                     _objcCoalescedMethodTypes   = { "objc method types" };;
    bool                                        _is64                       = false;
    mutable LOH_Tracker                         _lohTracker;
};

inline uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}

inline uint8_t* align_buffer(uint8_t* addr, uint8_t p2)
{
    return (uint8_t *)align((uintptr_t)addr, p2);
}


#endif /* CacheBuilder_h */
