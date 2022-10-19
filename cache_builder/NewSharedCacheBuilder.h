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

#ifndef NewSharedCacheBuilder_h
#define NewSharedCacheBuilder_h

#include "Defines.h"
#include "BuilderConfig.h"
#include "BuilderOptions.h"
#include "CacheDylib.h"
#include "Chunk.h"
#include "Diagnostics.h"
#include "Error.h"
#include "ImpCachesBuilder.h"
#include "JSONReader.h"
#include "MachOFile.h"
#include "NewAdjustDylibSegments.h"
#include "Optimizers.h"
#include "OptimizerObjC.h"
#include "PerfectHash.h"
#include "SectionCoalescer.h"
#include "SubCache.h"
#include "Timer.h"
#include "Types.h"

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dyld3 {
    namespace closure {
        class FileSystem;
    }
}

namespace cache_builder
{

struct CacheBuffer {
    uint8_t*        bufferData  = nullptr;
    size_t          bufferSize  = 0;
    std::string     cdHash      = "";
    std::string     uuid        = "";

    // Something like .development, .development.data, .symbols, etc
    std::string     cacheFileSuffix;

    // True if customer/universal caches need this buffer
    bool usedByCustomerConfig = false;
    // True if development/universal caches need this buffer
    bool usedByDevelopmentConfig = false;

    // The builder executable also passes back the fd.  This should typically be used instead of the data buffer
#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
    int             fd          = 0;
    std::string     tempPath    = "";
#endif
};

class SharedCacheBuilder
{
public:
    SharedCacheBuilder(BuilderOptions& options, const dyld3::closure::FileSystem& fileSystem);

    void            forEachWarning(void (^callback)(const std::string_view& str)) const;
    void            forEachCacheDylib(void (^callback)(const std::string_view& path)) const;
    void            forEachCacheSymlink(void (^callback)(const std::string_view& path)) const;
    void            addFile(const void* buffer, size_t bufferSize, std::string_view path,
                            uint64_t inode, uint64_t modTime);
    void            setAliases(const std::vector<FileAlias>& aliases,
                               const std::vector<FileAlias>& intermediateAliases);
    error::Error    build();

    void            getResults(std::vector<CacheBuffer>& results) const;
    std::string     getMapFileBuffer() const;

    // All caches have a logging prefix, UUID and JSON map which represents the development cache.
    // Universal caches may also have customer versions of these
    std::string                 developmentLoggingPrefix() const;
    std::string                 developmentJSONMap(std::string_view disposition) const;
    std::string                 developmentCacheUUID() const;
    std::string                 customerLoggingPrefix() const;
    std::optional<std::string>  customerJSONMap(std::string_view disposition) const;
    std::optional<std::string>  customerCacheUUID() const;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    // We need everything public to write tests
public:
#else
private:
#endif

    // Phases of the build() method
    error::Error    calculateInputs();
    error::Error    estimateGlobalOptimizations();
    error::Error    createSubCaches();
    error::Error    preDylibEmitChunks();
    error::Error    runDylibPasses();
    error::Error    postDylibEmitChunks();
    error::Error    finalize();

    // Passes to run before we have cache buffers
    void            categorizeInputs();
    void            verifySelfContained();
    void            calculateDylibAliases();
    void            sortDylibs();
    error::Error    calculateDylibDependents();
    void            categorizeDylibSegments();
    void            categorizeDylibLinkedit();
    void            estimateIMPCaches();
    void            findObjCDylibs();
    void            findCanonicalObjCSelectors();
    void            findCanonicalObjCClassNames();
    void            findCanonicalObjCProtocolNames();
    void            findObjCClasses();
    void            findObjCProtocols();
    void            estimateObjCHashTableSizes();
    void            calculateObjCCanonicalProtocolsSize();
    void            estimateSwiftHashTableSizes();
    void            calculateCacheDylibsTrie();
    void            estimatePatchTableSize();
    void            estimateCacheLoadersSize();
    void            setupStubOptimizer();
    void            addObjCOptimizationsToSubCache(SubCache& subCache);
    void            addGlobalOptimizationsToSubCache(SubCache& subCache);
    void            addFinalChunksToSubCache(SubCache& subCache);
    void            computeSubCaches();
    void            computeRegularSubCache();
    void            computeLargeSubCache();
    void            makeLargeLayoutSubCaches(SubCache* firstSubCache,
                                             std::list<SubCache>& otherCaches);
    void            setSubCacheNames();
    error::Error    calculateSubCacheSymbolStrings();
    error::Error    calculateUniqueGOTs();
    void            sortSubCacheSegments();
    void            calculateSlideInfoSize();
    void            calculateCodeSignatureSize();
    void            printSubCaches() const;
    error::Error    computeSubCacheDiscontiguousSimVMLayout();
    error::Error    computeSubCacheDiscontiguousVMLayout();
    error::Error    computeSubCacheContiguousVMLayout();
    error::Error    computeSubCacheLayout();
    error::Error    allocateSubCacheBuffers();
    void            setupDylibLinkedit();
    void            setupSplitSegAdjustors();
    void            adjustObjCClasses();
    void            adjustObjCProtocols();

    // Final passes to run, after dylib passes
    void            emitObjCSelectorStrings();
    void            emitObjCClassNameStrings();
    void            emitObjCProtocolNameStrings();
    void            emitObjCSwiftDemangledNameStrings();
    void            emitObjCHashTables();
    void            emitObjCHeaderInfo();
    void            emitObjCOptsHeader();
    error::Error    emitSwiftHashTables();
    void            optimizeTLVs();
    error::Error    emitUniquedGOTs();
    error::Error    emitCanonicalObjCProtocols();
    error::Error    computeObjCClassLayout();
    void            computeSlideInfo();
    void            emitCacheDylibsTrie();
    error::Error    emitPatchTable();
    error::Error    emitCacheDylibsPrebuiltLoaders();
    error::Error    emitExecutablePrebuiltLoaders();
    void            emitSymbolTable();
    void            emitUnmappedLocalSymbols();
    void            writeSubCacheHeader(SubCache& subCache);
    uint64_t        getMaxSlide() const;
    void            addObjcSegments();
    void            computeCacheHeaders();
    static bool     regionIsSharedCacheMapping(const Region& region);
    void            codeSign();

    std::string     generateJSONMap(std::string_view disposition,
                                    const SubCache& mainSubCache) const;

    typedef std::unordered_map<const InputFile*, CacheDylib*> FileToDylibMap;
    typedef std::unordered_map<const InputFile*, UnmappedSymbolsOptimizer::LocalSymbolInfo*> FileToSymbolInfoMap;
    typedef std::unordered_map<uint32_t, uint32_t> OldToNewIndicesMap;
    error::Error copyLocalSymbols(SubCache& subCache,
                                  const std::span<LinkeditDataChunk*> symbolStringChunks,
                                  const FileToDylibMap& fileToDylibMap,
                                  const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                  const uint32_t redactedStringOffset,
                                  uint32_t& stringBufferSize,
                                  uint32_t& sourceStringSize,
                                  uint32_t& sourceStringCount);
    error::Error copyExportedSymbols(SubCache& subCache,
                                     const std::span<LinkeditDataChunk*> symbolStringChunks,
                                     const FileToDylibMap& fileToDylibMap,
                                     const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                     std::vector<OldToNewIndicesMap>& oldToNewIndicesMaps,
                                     const uint32_t redactedStringOffset,
                                     uint32_t& stringBufferSize,
                                     uint32_t& sourceStringSize,
                                     uint32_t& sourceStringCount);
    error::Error copyImportedSymbols(SubCache& subCache,
                                     const std::span<LinkeditDataChunk*> symbolStringChunks,
                                     const FileToDylibMap& fileToDylibMap,
                                     const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                     std::vector<OldToNewIndicesMap>& oldToNewIndicesMaps,
                                     const uint32_t redactedStringOffset,
                                     uint32_t& stringBufferSize,
                                     uint32_t& sourceStringSize,
                                     uint32_t& sourceStringCount);

    __attribute__((format(printf, 2, 3)))
    void            warning(const char* format, ...);

    void            debug(const char* installName) const;

    const BuilderOptions                            options;
    const dyld3::closure::FileSystem&               fileSystem;
    BuilderConfig                                   config;
    std::vector<InputFile>                          allInputFiles;
    std::vector<FileAlias>                          inputAliases;
    std::vector<FileAlias>                          inputIntermediateAliases;
    std::vector<CacheDylib>                         cacheDylibs;
    std::vector<InputFile*>                         exeInputFiles;
    std::vector<InputFile*>                         nonCacheDylibInputFiles;
    std::vector<SubCache>                           subCaches;
    CacheVMSize                                     totalVMSize;
    std::unordered_map<std::string, CacheDylib*>    dylibAliases;
    bool                                            dylibHasMissingDependency = false;
    std::vector<std::string>                        warnings;

    // Some optimizers are run just once per cache, so live at the top level here
    ObjCOptimizer                        objcOptimizer;
    ObjCIMPCachesOptimizer               objcIMPCachesOptimizer;
    ObjCSelectorOptimizer                objcSelectorOptimizer;
    ObjCClassOptimizer                   objcClassOptimizer;
    ObjCProtocolOptimizer                objcProtocolOptimizer;
    SwiftProtocolConformanceOptimizer    swiftProtocolConformanceOptimizer;
    DylibTrieOptimizer                   dylibTrieOptimizer;
    PatchTableOptimizer                  patchTableOptimizer;
    PrebuiltLoaderBuilder                prebuiltLoaderBuilder;
    UnmappedSymbolsOptimizer             unmappedSymbolsOptimizer;
    StubOptimizer                        stubOptimizer;
};

} // namespace cache_builder

#endif /* NewSharedCacheBuilder_h */
