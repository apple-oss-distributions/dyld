/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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

#ifndef SubCache_hpp
#define SubCache_hpp

#include "Defines.h"
#include "Chunk.h"
#include "MachOFile.h"
#include "Optimizers.h"
#include "Types.h"

#include <uuid/uuid.h>
#include <vector>

namespace cache_builder
{

struct Region
{
    // Note the order of this enum is the order in the final cache binary.
    // It is sorted to try keep page tables to a minimum, ie, keeping similar regions adjacent
    enum class Kind : uint32_t
    {
        text,

        // Rosetta expects __DATA_CONST after __TEXT, as we currently sort using this enum
        dataConst,

        data,
        auth,

        // FIXME: Move this to be after DATA_CONST to reduce page tables
        // Needs rdar://96315050
        authConst,

        linkedit,
        unmapped,
        dynamicConfig,
        codeSignature,

        numKinds
    };

    Region(Kind kind) : kind(kind) { }

    uint32_t initProt() const;
    uint32_t maxProt() const;
    bool     canContainAuthPointers() const;
    bool     needsSharedCacheMapping() const;
    bool     needsSharedCacheReserveAddressSpace() const;

    bool needsRegionPadding(const Region& next) const;

    Kind                                kind;

    // The chunks from dylibs, optimzations, etc, which make up this Region
    std::vector<Chunk*>                 chunks;

    CacheFileOffset                     subCacheFileOffset;
    CacheFileSize                       subCacheFileSize;
    CacheVMAddress                      subCacheVMAddress;
    CacheVMSize                         subCacheVMSize;
    uint8_t*                            subCacheBuffer = nullptr;
};

struct SubCache
{
private:
    enum class Kind
    {
        mainDevelopment,
        stubsDevelopment,

        mainCustomer,
        stubsCustomer,

        // If we aren't the main cache, or a stubs cache, then the remainder is a universal "sub" cache
        // which is typically TEXT/DATA/LINKEDIT
        subUniversal,

        symbols
    };

    SubCache(Kind kind);

public:
    SubCache() = delete;
    ~SubCache() = default;
    SubCache(const SubCache&) = delete;
    SubCache& operator=(const SubCache&) = delete;
    SubCache(SubCache&&) = default;
    SubCache& operator=(SubCache&&) = default;

    // Helper methods to build the various kinds of subCache
    static SubCache makeMainCache(const BuilderOptions& options, bool isDevelopment);
    static SubCache makeSubCache(const BuilderOptions& options);
    static SubCache makeStubsCache(const BuilderOptions& options, bool isDevelopment);
    static SubCache makeSymbolsCache();

    // These methods are called by computeSubCaches() to add Chunk's to the subCache
    void addDylib(CacheDylib& cacheDylib, bool addLinkedit);
    void addLinkeditFromDylib(CacheDylib& cacheDylib);
    void addCacheHeaderChunk(const std::span<CacheDylib> cacheDylibs);
    void addObjCHeaderInfoReadWriteChunk(const BuilderConfig& config, ObjCOptimizer& objcOptimizer);
    void addCodeSignatureChunk();
    void addObjCOptsHeaderChunk(ObjCOptimizer& objcOptimizer);
    void addObjCHeaderInfoReadOnlyChunk(ObjCOptimizer& objcOptimizer);
    void addObjCSelectorStringsChunk(ObjCSelectorOptimizer& objCSelectorOptimizer);
    void addObjCSelectorHashTableChunk(ObjCSelectorOptimizer& objCSelectorOptimizer);
    void addObjCClassNameStringsChunk(ObjCClassOptimizer& objcClassOptimizer);
    void addObjCClassHashTableChunk(ObjCClassOptimizer& objcClassOptimizer);
    void addObjCProtocolNameStringsChunk(ObjCProtocolOptimizer& objcProtocolOptimizer);
    void addObjCProtocolHashTableChunk(ObjCProtocolOptimizer& objcProtocolOptimizer);
    void addObjCProtocolSwiftDemangledNamesChunk(ObjCProtocolOptimizer& objcProtocolOptimizer);
    void addObjCIMPCachesChunk(ObjCIMPCachesOptimizer& objcIMPCachesOptimizer);
    void addObjCCanonicalProtocolsChunk(const BuilderConfig& config,
                                        ObjCProtocolOptimizer& objcProtocolOptimizer);
    void addCacheTrieChunk(DylibTrieOptimizer& dylibTrieOptimizer);
    void addPatchTableChunk(PatchTableOptimizer& patchTableOptimizer);
    void addCacheDylibsLoaderChunk(PrebuiltLoaderBuilder& builder);
    void addExecutableLoaderChunk(PrebuiltLoaderBuilder& builder);
    void addExecutablesTrieChunk(PrebuiltLoaderBuilder& builder);
    void addSwiftOptsHeaderChunk(SwiftProtocolConformanceOptimizer& opt);
    void addSwiftTypeHashTableChunk(SwiftProtocolConformanceOptimizer& opt);
    void addSwiftMetadataHashTableChunk(SwiftProtocolConformanceOptimizer& opt);
    void addSwiftForeignHashTableChunk(SwiftProtocolConformanceOptimizer& opt);
    void addUnmappedSymbols(const BuilderConfig& config, UnmappedSymbolsOptimizer& opt);
    void addDynamicConfigChunk();
    void addSlideInfoChunks();
    void removeEmptyRegions();

    // When "kind == sub", sets the suffix on this subCache
    // This has to be done after creating things like stubs sub caches, which might move the indices
    void setSuffix(dyld3::Platform platform, bool forceDevelopmentSubCacheSuffix,
                   size_t subCacheIndex);

    void setCodeSignatureSize(const BuilderOptions& options, const BuilderConfig& config,
                              CacheFileSize estimatedSize);

    error::Error computeSlideInfo(const BuilderConfig& config);

    // Emits a dyld_cache_header for this subCache
    void writeCacheHeader(const BuilderOptions& options, const BuilderConfig& config,
                          const std::span<CacheDylib> cacheDylibs);

    // Adds any additional fields which are set only on the main subCache(s)
    void addMainCacheHeaderInfo(const BuilderOptions& options, const BuilderConfig& config,
                                const std::span<CacheDylib> cacheDylibs,
                                CacheVMSize totalVMSize, uint64_t maxSlide,
                                uint32_t osVersion, uint32_t altPlatform, uint32_t altOsVersion,
                                CacheVMAddress dyldInCacheUnslidAddr,
                                CacheVMAddress dyldInCacheEntryUnslidAddr,
                                const DylibTrieOptimizer& dylibTrieOptimizer,
                                const ObjCOptimizer& objcOpt,
                                const SwiftProtocolConformanceOptimizer& swiftProtocolConformanceOpt,
                                const PatchTableOptimizer& patchTableOptimizer,
                                const PrebuiltLoaderBuilder& prebuiltLoaderBuilder);

    // Adds any additional fields which are set only on the .symbols subCache
    void addSymbolsCacheHeaderInfo(const UnmappedSymbolsOptimizer& unmappedSymbolsOptimizer);

    void codeSign(Diagnostics& diag, const BuilderOptions& options, const BuilderConfig& config);

    bool isMainCache() const;
    bool isMainDevelopmentCache() const;
    bool isMainCustomerCache() const;
    bool isSubCache() const;
    bool isStubsCache() const;
    bool isStubsDevelopmentCache() const;
    bool isStubsCustomerCache() const;
    bool isSymbolsCache() const;

    void addStubsChunk(Chunk* chunk);

    bool shouldKeepCache(bool keepDevelopmentCaches, bool keepCustomerCaches) const;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    // We need everything public to write tests
public:
#else
private:
#endif

    // Adds the given chunk to the given region
    void addTextChunk(Chunk* chunk);
    void addDataChunk(Chunk* chunk);
    void addDataConstChunk(Chunk* chunk);
    void addAuthChunk(Chunk* chunk);
    void addAuthConstChunk(Chunk* chunk);
    void addLinkeditChunk(Chunk* chunk);
    void addUnmappedChunk(Chunk* chunk);
    void addCodeSignatureChunk(Chunk* chunk);
    void addObjCTextChunk(Chunk* chunk);
    void addObjCReadOnlyChunk(Chunk* chunk);
    void addObjCReadWriteChunk(const BuilderConfig& config, Chunk* chunk);

    // Returns true if the cache header on this subCache needs an image list
    // The symbols cache and stubs caches, for example, don't need this
    bool needsCacheHeaderImageList() const;

    // Add image info to the subCache header, if it needs it
    void addCacheHeaderImageInfo(const BuilderOptions& options,
                                 const std::span<CacheDylib> cacheDylibs);

    static uint64_t getCacheType(const BuilderOptions& options);
    uint32_t        getCacheSubType() const;
    void            writeCacheHeaderMappings();

    static error::Error convertChainsToVMAddresses(const BuilderConfig& config, Region& region);
    static error::Error computeSlideInfoV1(const BuilderConfig& config,
                                           SlideInfoChunk* slideChunk,
                                           Region& region);
    static error::Error computeSlideInfoV2(const BuilderConfig& config,
                                           SlideInfoChunk* slideChunk,
                                           Region& region);
    static error::Error computeSlideInfoV3(const BuilderConfig& config,
                                           SlideInfoChunk* slideChunk,
                                           Region& region);
    static error::Error computeSlideInfoForRegion(const BuilderConfig& config,
                                                  SlideInfoChunk* slideChunk,
                                                  Region& region);

public:
    Kind                kind;
    std::vector<Region> regions;

    // This buffer is vm_allocated (or points to a file on disk).  Either way, the builder
    // creates it but doesn't destroy it.  Its ownership will move out to the calling code via
    // getResults(), and the caller will deallocate/unmap as needed
    uint8_t*            buffer      = nullptr;
    uint64_t            bufferSize  = 0;
    CacheVMAddress      subCacheVMAddress;
#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
    int                 fd          = 0;
    std::string         tempPath;
#endif
    uint8_t             cdHash[20];
    uuid_string_t       uuidString;
    std::string         fileSuffix;

    // Some Chunk instances are owned by the SubCache.  Eg, it owns its own header
    std::unique_ptr<CacheHeaderChunk>                           cacheHeader;
    std::unique_ptr<SlideInfoChunk>                             dataSlideInfo;
    std::unique_ptr<SlideInfoChunk>                             dataConstSlideInfo;
    std::unique_ptr<SlideInfoChunk>                             authSlideInfo;
    std::unique_ptr<SlideInfoChunk>                             authConstSlideInfo;
    std::unique_ptr<CodeSignatureChunk>                         codeSignature;
    std::unique_ptr<ObjCOptsHeaderChunk>                        objcOptsHeader;
    std::unique_ptr<ObjCHeaderInfoReadOnlyChunk>                objcHeaderInfoRO;
    std::unique_ptr<ObjCHeaderInfoReadWriteChunk>               objcHeaderInfoRW;
    std::unique_ptr<ObjCStringsChunk>                           objcSelectorStrings;
    std::unique_ptr<ObjCSelectorHashTableChunk>                 objcSelectorsHashTable;
    std::unique_ptr<ObjCStringsChunk>                           objcClassNameStrings;
    std::unique_ptr<ObjCClassHashTableChunk>                    objcClassesHashTable;
    std::unique_ptr<ObjCStringsChunk>                           objcProtocolNameStrings;
    std::unique_ptr<ObjCProtocolHashTableChunk>                 objcProtocolsHashTable;
    std::unique_ptr<ObjCCanonicalProtocolsChunk>                objcCanonicalProtocols;
    std::unique_ptr<ObjCStringsChunk>                           objcSwiftDemangledNameStrings;
    std::unique_ptr<ObjCIMPCachesChunk>                         objcIMPCaches;
    std::unique_ptr<SwiftOptsHeaderChunk>                       swiftOptsHeader;
    std::unique_ptr<SwiftProtocolConformancesHashTableChunk>    swiftTypeHashTable;
    std::unique_ptr<SwiftProtocolConformancesHashTableChunk>    swiftMetadataHashTable;
    std::unique_ptr<SwiftProtocolConformancesHashTableChunk>    swiftForeignTypeHashTable;
    std::unique_ptr<CacheTrieChunk>                             cacheDylibsTrie;
    std::unique_ptr<PatchTableChunk>                            patchTable;
    std::unique_ptr<DynamicConfigChunk>                         dynamicConfig;
    std::unique_ptr<PrebuiltLoaderChunk>                        cacheDylibsLoaders;
    std::unique_ptr<PrebuiltLoaderChunk>                        executableLoaders;
    std::unique_ptr<CacheTrieChunk>                             executablesTrie;
    std::unique_ptr<SymbolStringsChunk>                         optimizedSymbolStrings;
    std::unique_ptr<UniquedGOTsChunk>                           uniquedGOTs;
    std::unique_ptr<UniquedGOTsChunk>                           uniquedAuthGOTs;

    // Each subCache has its own Linkedit so needs its own optimizer
    SymbolStringsOptimizer                                      symbolStringsOptimizer;

    // Each subCache can have its own uniqued GOTs
    UniquedGOTsOptimizer                                        uniquedGOTsOptimizer;

    // FIXME: Should these be zero-sized Chunk's instead?
    uint64_t rosettaReadOnlyAddr    = 0;
    uint64_t rosettaReadOnlySize    = 0;
    uint64_t rosettaReadWriteAddr   = 0;
    uint64_t rosettaReadWriteSize   = 0;

    // The following is only used depending on the kind field

    // Main sub caches have a list of the other sub caches (indices are in to the builder subCaches array)
    std::vector<SubCache*> subCaches;
};

} // namespace cache_builder

#endif /* SubCache_hpp */
