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

#ifndef CacheDylib_hpp
#define CacheDylib_hpp

#include "CachePatching.h"
#include "Chunk.h"
#include "Diagnostics.h"
#include "MachOFile.h"
#include "NewAdjustDylibSegments.h"
#include "ObjCVisitor.h"
#include "SectionCoalescer.h"
#include "SwiftVisitor.h"
#include "Timer.h"

#include <list>

namespace cache_builder
{

struct BuilderConfig;
struct InputFile;
struct ObjCIMPCachesOptimizer;
struct ObjCSelectorOptimizer;
struct ObjCCategoryOptimizer;
struct StubOptimizer;
struct UnmappedSymbolsOptimizer;
struct FunctionVariantsOptimizer;

// A dylib which will be included in the cache
struct CacheDylib
{
#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    CacheDylib();
#else
    CacheDylib() = delete;
#endif
    CacheDylib(InputFile& inputFile);
    // create a CacheDylib *placeholder* with an install name only and no input mach-o
    CacheDylib(std::string_view installName);
    ~CacheDylib() = default;

    CacheDylib(const CacheDylib&) = delete;
    CacheDylib& operator=(const CacheDylib&) = delete;

    CacheDylib& operator=(CacheDylib&&) = default;
    CacheDylib(CacheDylib&&) = default;

    // Passes to create a valid CacheDylib from an input file
    void categorizeSegments(const BuilderConfig& config,
                            objc_visitor::Visitor& objcVisitor);
    void categorizeLinkedit(const BuilderConfig& config);

    // Passes to run to add a CacheDylib to a SubCache
    void copyRawSegments(const BuilderConfig& config, Timer::AggregateTimer& timer);
    void applySplitSegInfo(Diagnostics& diag, const BuilderOptions& options,
                           const BuilderConfig& config, Timer::AggregateTimer& timer,
                           UnmappedSymbolsOptimizer& unmappedSymbolsOptimizer);
    void updateSymbolTables(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer);
    std::vector<error::Error> calculateBindTargets(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
                                                   const std::vector<const CacheDylib *>& cacheDylibs,
                                                   PatchInfo& dylibPatchInfo);
    void bind(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
              PatchInfo& dylibPatchInfo, FunctionVariantsOptimizer& functionVariantsOptimizer);
    void updateObjCSelectorReferences(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
                                      ObjCSelectorOptimizer& objcSelectorOptimizer);
    void convertObjCMethodListsToOffsets(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
                                         const Chunk* selectorStringsChunk);
    void sortObjCMethodLists(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
                             const Chunk* selectorStringsChunk);
    void optimizeLoadsFromConstants(const BuilderConfig& config, Timer::AggregateTimer& timer,
                                    const ObjCStringsChunk* selectorStringsChunk);
    error::Error emitObjCIMPCaches(const BuilderConfig& config, Timer::AggregateTimer& timer,
                                   const ObjCIMPCachesOptimizer& objcIMPCachesOptimizer,
                                   const ObjCStringsChunk* selectorStringsChunk);
    void optimizeStubs(const BuilderOptions& options, const BuilderConfig& config,
                       Timer::AggregateTimer& timer, const StubOptimizer& stubOptimizer,
                       const PatchInfo& dylibPatchInfo);
    void fipsSign(Timer::AggregateTimer& timer);
    void addObjcSegments(Diagnostics& diag, Timer::AggregateTimer& timer,
                         const ObjCHeaderInfoReadOnlyChunk* headerInfoReadOnlyChunk,
                         const ObjCImageInfoChunk* imageInfoChunk,
                         const ObjCProtocolHashTableChunk* protocolHashTableChunk,
                         const ObjCPreAttachedCategoriesChunk* preAttachedCategoriesChunk,
                         const ObjCHeaderInfoReadWriteChunk* headerInfoReadWriteChunk,
                         const ObjCCanonicalProtocolsChunk* canonicalProtocolsChunk);
    // remove all dylib link load commands, except libSystem
    void removeLinkedDylibs(Diagnostics& diag);
    void addLinkedDylib(Diagnostics& diag, const CacheDylib& dylib);

    void forEachCacheSection(void (^callback)(std::string_view segmentName,
                                              std::string_view sectionName,
                                              uint8_t* sectionBuffer,
                                              CacheVMAddress sectionVMAddr,
                                              CacheVMSize sectionVMSize,
                                              bool& stop));

    struct DependentDylib
    {
        enum class Kind
        {
            normal,
            weakLink,
            reexport,
            upward
        };

        Kind              kind  = Kind::normal;
        const CacheDylib* dylib = nullptr;
    };

    struct BindTarget
    {
        enum class Kind
        {
            absolute,
            inputImage,
            cacheImage
        };

        struct Absolute
        {
            uint64_t value;
        };

        struct InputImage
        {
            VMOffset            targetRuntimeOffset;
            const CacheDylib*   targetDylib;
            bool                isWeakDef;
            bool                isFunctionVariant;
            uint16_t            functionVariantTableIndex;
        };

        struct CacheImage
        {
            VMOffset            targetRuntimeOffset;
            const CacheDylib*   targetDylib;
            bool                isWeakDef;
            bool                isFunctionVariant;
            uint16_t            functionVariantTableIndex;
        };

        Kind        kind    = Kind::absolute;
        union
        {
            // Just to give a default constructor...
            uint64_t    unusedRawValue = 0;

            Absolute    absolute;
            InputImage  inputImage;
            CacheImage  cacheImage;
        };
        uint64_t    addend  = 0;
        bool        isWeakImport;
#if DEBUG
        CString     name;
#endif
    };

    typedef std::pair<BindTarget, std::string> BindTargetAndName;
    enum class SearchMode
    {
        onlySelf,
        selfAndReexports
    };

    std::optional<BindTargetAndName>    hasExportedSymbol(Diagnostics& diag, const char* symbolName, SearchMode mode) const;

    objc_visitor::Visitor makeCacheObjCVisitor(const BuilderConfig& config,
                                               const Chunk* selectorStringsChunk,
                                               const ObjCCanonicalProtocolsChunk* canonicalProtocolsChunk,
                                               const ObjCPreAttachedCategoriesChunk* categoriesChunk) const;

    metadata_visitor::SwiftVisitor makeCacheSwiftVisitor(const BuilderConfig& config,
                                                         std::span<metadata_visitor::Segment> extraRegions) const;

    metadata_visitor::Visitor makeCacheVisitor(const BuilderConfig& config) const;

private:

    // Helper method for calculateBindTargets()
    BindTargetAndName                   resolveSymbol(Diagnostics& diag, int libOrdinal, const char* symbolName, bool weakImport,
                                                      const std::vector<const CacheDylib*>& cacheDylibs) const;
    std::optional<BindTargetAndName>    findDyldMagicSymbolAddress(const char* fullSymbolName, std::string_view name) const;

    // Map from where the GOT is located in the dylib to where its located in the coalesced section
    typedef std::unordered_map<const CacheVMAddress, CacheVMAddress, CacheVMAddressHash, CacheVMAddressEqual> CoalescedGOTMap;

    void bindLocation(Diagnostics& diag, const BuilderConfig& config,
                      const BindTarget& bindTarget, uint64_t addend,
                      uint32_t bindOrdinal, uint32_t segIndex,
                      dyld3::MachOFile::ChainedFixupPointerOnDisk* fixupLoc,
                      CacheVMAddress fixupVMAddr, dyld3::MachOFile::PointerMetaData pmd,
                      CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                      CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                      FunctionVariantsOptimizer& functionVariantsOptimizer);
    void bindWithChainedFixups(Diagnostics& diag, const BuilderConfig& config,
                               CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                               CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                               FunctionVariantsOptimizer& functionVariantsOptimizer);
    void bindWithOpcodeFixups(Diagnostics& diag, const BuilderConfig& config,
                              CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                              CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                              FunctionVariantsOptimizer& functionVariantsOptimizer);

    void forEachReferenceToASelRef(Diagnostics &diags,
                                   void (^handler)(uint64_t kind, uint32_t* instrPtr, uint64_t selRefVMAddr)) const;

    // Part of IMP caches
    error::Error setObjCImpCachesPointers(const BuilderConfig& config,
                                          const ObjCIMPCachesOptimizer& objcIMPCachesOptimizer,
                                          const ObjCStringsChunk* selectorStringsChunk);

    // Part of the stub optimization
    typedef std::unordered_map<CacheVMAddress, CacheVMAddress,
                               CacheVMAddressHash, CacheVMAddressEqual> OldToNewStubMap;
    CacheDylib::OldToNewStubMap buildStubMaps(const BuilderConfig& config,
                                              const StubOptimizer& stubOptimizer,
                                              const PatchInfo& dylibPatchInfo);

    typedef bool (^CallSiteHandler)(uint8_t callSiteKind, uint64_t callSiteAddr,
                                    uint64_t stubAddr, uint32_t& instruction);
    void forEachCallSiteToAStub(Diagnostics& diag, const CallSiteHandler handler);

    typedef std::unordered_map<CacheVMAddress, CacheVMAddress,
                               CacheVMAddressHash, CacheVMAddressEqual> GOTToTargetMap;
    GOTToTargetMap getUniquedGOTTargets(const PatchInfo& dylibPatchInfo) const;

#if DEBUG
    // Helper method to watch a memory location while building the cache.  Called from copyRawSegments().
    void watchMemory(const DylibSegmentChunk& segment, std::string_view dylibInstallName,
                     std::string_view dylibSegmentName, uint64_t dylibAddressInSegment) const;
#endif


public:
    InputFile*                              inputFile               = nullptr;
    const dyld3::MachOFile*                 inputMF                 = nullptr;
    const mach_o::Header*                   inputHdr                = nullptr;
    InputDylibVMAddress                     inputLoadAddress;
    std::string_view                        installName;
    uint32_t                                cacheIndex;
    bool                                    needsPatchTable         = true;
    dyld3::MachOFile*                       cacheMF                 = nullptr;
    const mach_o::Header*                   cacheHdr                = nullptr;
    CacheVMAddress                          cacheLoadAddress;
    std::vector<DylibSegmentChunk>          segments;
    // This is a list due to iterator invalidation, ie, calculateSubCacheSymbolStrings() deletes
    // elements and Region holds pointers to them
    std::list<LinkeditDataChunk>            linkeditChunks;
    std::vector<DependentDylib>             dependents;
    // an unmodified list of linked libraries, used for symbol resolution
    std::vector<DependentDylib>             inputDependents;
    std::vector<BindTarget>                 bindTargets;
    std::optional<uint32_t>                 weakBindTargetsStartIndex;
    std::unique_ptr<DylibSegmentsAdjustor>  adjustor;

    // The final cache won't have split seg, but we need it in multiple places during building.  So keep a copy
    const uint8_t*                          inputDylibSplitSegStart = nullptr;
    const uint8_t*                          inputDylibSplitSegEnd   = nullptr;

    // The final cache won't have dyld info, but we need it in multiple places during building.  So keep a copy
    const uint8_t*                          inputDylibRebaseStart   = nullptr;
    const uint8_t*                          inputDylibRebaseEnd     = nullptr;
    const uint8_t*                          inputDylibBindStart     = nullptr;
    const uint8_t*                          inputDylibBindEnd       = nullptr;
    const uint8_t*                          inputDylibLazyBindStart = nullptr;
    const uint8_t*                          inputDylibLazyBindEnd   = nullptr;
    const uint8_t*                          inputDylibWeakBindStart = nullptr;
    const uint8_t*                          inputDylibWeakBindEnd   = nullptr;

    // calculateSubCacheSymbolStrings() builds new nlists.  We need to store them here until we can copy them in to the
    // final cache binary
    NListChunk                              optimizedSymbols;
    const SymbolStringsChunk*               subCacheSymbolStrings   = nullptr;
    std::vector<uint32_t>                   indirectSymbolTable;

    // calculateUniqueGOTs() uniques GOTs across dylibs.  This stores the results for this dylib
    DylibSectionCoalescer                   optimizedSections;

    // In Universal caches, this dylib will be redirected to use these alternative stubs
    StubsChunk                              developmentStubs;
    StubsChunk                              customerStubs;
};
static_assert(std::is_move_constructible<CacheDylib>::value, "CacheDylib needs to be move constructible");

} // namespace cache_builder

#endif /* CacheDylib_hpp */
