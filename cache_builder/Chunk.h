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

#ifndef Chunk_hpp
#define Chunk_hpp

#include "ASLRTracker.h"
#include "Types.h"
#include "fixup-chains.h"

#include <mach-o/nlist.h>
#include <unordered_set>
#include <string>
#include <vector>

namespace dyld3
{
struct MachOFile;
}

namespace cache_builder
{

struct BuilderConfig;
struct InputFile;

struct DylibSegmentChunk;
struct LinkeditDataChunk;
struct CodeSignatureChunk;
struct DynamicConfigChunk;
struct SlidChunk;
struct StubsChunk;
struct UniquedGOTsChunk;

namespace impl
{
    enum class Alignment;
};

// The smallest atom of data within a SubCache.  A Chunk is a contiguous region of memory
// which may point to data from dylibs, cache header, optimization results, etc
struct Chunk
{
    enum class Kind
    {
        // Contains the dyld_cache_header value
        // This is the HeaderChunk below
        cacheHeader,

        // Contains the slide info for a single one of the RW regions.
        // We may have multiple of these in a subCache
        slideInfo,

        // Contains the code signature
        codeSignature,

        // The SwiftOptimizationHeader value
        swiftOptsHeader,

        // A buffer to hold a swift hash table (type, metadata, foreign)
        swiftConformanceHashTable,

        // A buffer to hold the trie for the cache dylib names
        cacheDylibsTrie,

        // A buffer to hold the patch table for the cache dylibs
        cachePatchTable,

        // A buffer to hold the PrebuiltLoaderSet for the cache dylibs
        dylibPrebuiltLoaders,

        // A buffer to hold the PrebuiltLoaderSet for the executables
        executablePrebuiltLoaders,

        // A buffer to hold the trie for the cache dylib names
        cacheExecutablesTrie,

        // In the .symbols file, this is the payload
        unmappedSymbols,

        // Uniqued GOTs
        uniquedGOTs,

        // In a universal cache, we adds stubs every N MB.  This is the stubs for a given dylib
        stubs,

        // __TEXT copied from the source dylib
        dylibText,

        // __DATA copied from the source dylib
        dylibData,

        // __DATA_CONST copied from the source dylib, but the dylib is ineligible for RO __DATA_CONST
        dylibDataConstWorkaround,

        // __DATA_CONST copied from the source dylib, and the dylib is eligible for RO __DATA_CONST
        dylibDataConst,

        // The objc HeaderInfoRW array.  It is before __DATA_DIRTY so that we sort it near
        // the __DATA_DIRTY from libobjc
        objcHeaderInfoRW,

        // A buffer to hold the canonical protocols. This is adjacent to HeaderInfoRW so that the
        // __OBJC_RW segment can cover them
        objcCanonicalProtocols,

        // __DATA_DIRTY copied from the source dylib
        dylibDataDirty,

        // __AUTH copied from the source dylib
        dylibAuth,

        // __AUTH_CONST copied from the source dylib, and the dylib is eligible for RO __DATA_CONST
        dylibAuthConst,

        // __AUTH_CONST copied from the source dylib, but the dylib is ineligible for RO __AUTH_CONST
        dylibAuthConstWorkaround,

        // Read-only segment copied from the source dylib
        dylibReadOnly,

        // __LINKEDIT copied from the source dylib
        dylibLinkedit,

        // Individual pieces of LINKEDIT copied from input files
        linkeditSymbolNList,
        linkeditSymbolStrings,
        linkeditIndirectSymbols,
        linkeditFunctionStarts,
        linkeditDataInCode,
        linkeditExportTrie,

        // Optimized symbols nlist/strings.
        // Note this must be sorted after the above LINKEDIT entries so that
        // offsets from the dylib LINKEDIT work
        optimizedSymbolNList,
        optimizedSymbolStrings,

        // ObjC Optimizations.  These must be after dylibText so that offsets from the libobjc __TEXT
        // are positive if pointing to OBJC_RO

        // The ObjCOptimizationHeader value
        objcOptsHeader,

        // The objc HeaderInfoRO array
        objcHeaderInfoRO,

        // A contiguous buffer of objc strings.  There may be multiple of these, eg, selectors, class names, etc.
        objcStrings,

        // A buffer to hold the selectors hash table
        objcSelectorsHashTable,

        // A buffer to hold the classes hash table
        objcClassesHashTable,

        // A buffer to hold the protocols hash table
        objcProtocolsHashTable,

        // A buffer to hold the imp caches
        objcIMPCaches,

        // This is a placeholder for empty address space that can be used at runtime
        dynamicConfig,
    };

protected:
    Kind          kind;

public:
    // Where are we in the subCache buffer (set by computeSubCacheFileLayout() and allocateSubCacheBuffers())
    CacheFileOffset     subCacheFileOffset;
    CacheFileSize       subCacheFileSize;
    uint8_t*            subCacheBuffer      = nullptr;

    // Where are we in the cache, ie, in memory layout.  Set by computeSubCacheFileLayout().
    CacheVMAddress      cacheVMAddress;
    CacheVMSize         cacheVMSize;

private:
    uint64_t            minAlignment = 1;

public:
    Chunk(Kind kind, uint64_t minAlignment);
    Chunk(Kind kind, impl::Alignment minAlignment);
    virtual ~Chunk();
    Chunk(const Chunk&) = delete;
    Chunk(Chunk&&) = default;
    Chunk& operator=(const Chunk&) = delete;
    Chunk& operator=(Chunk&&) = default;

    uint32_t sortOrder() const;
    uint64_t alignment() const;

    // Abstract methods
    virtual const char* name() const = 0;

    virtual bool isZeroFill() const;
    virtual SlidChunk* isSlidChunk();
    virtual const DylibSegmentChunk* isDylibSegmentChunk() const;
    virtual const LinkeditDataChunk* isLinkeditDataChunk() const;
    virtual StubsChunk* isStubsChunk();
    virtual UniquedGOTsChunk* isUniquedGOTsChunk();

private:
    __attribute__((used))
    virtual void dump() const;
};

struct CacheHeaderChunk : Chunk
{
public:
    CacheHeaderChunk();
    virtual ~CacheHeaderChunk();
    CacheHeaderChunk(const CacheHeaderChunk&) = delete;
    CacheHeaderChunk(CacheHeaderChunk&&) = delete;
    CacheHeaderChunk& operator=(const CacheHeaderChunk&) = delete;
    CacheHeaderChunk& operator=(CacheHeaderChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct SlideInfoChunk : Chunk
{
public:
    SlideInfoChunk();
    virtual ~SlideInfoChunk();
    SlideInfoChunk(const SlideInfoChunk&) = delete;
    SlideInfoChunk(SlideInfoChunk&&) = delete;
    SlideInfoChunk& operator=(const SlideInfoChunk&) = delete;
    SlideInfoChunk& operator=(SlideInfoChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

    // We allocate space for N-bytes per page, but for the V1 format, we may use less
    // This tracks the size we actually use, which is what we'll then wire to the kernel
    CacheFileSize usedFileSize;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct CodeSignatureChunk : Chunk
{

public:
    CodeSignatureChunk();
    virtual ~CodeSignatureChunk();
    CodeSignatureChunk(const CodeSignatureChunk&) = delete;
    CodeSignatureChunk(CodeSignatureChunk&&) = delete;
    CodeSignatureChunk& operator=(const CodeSignatureChunk&) = delete;
    CodeSignatureChunk& operator=(CodeSignatureChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// A Chunk which may contain slid values, ie, rebases/binds
struct SlidChunk : Chunk
{
    ASLR_Tracker tracker;

public:
    SlidChunk(Kind kind, uint64_t minAlignment);
    virtual ~SlidChunk();
    SlidChunk(const SlidChunk&) = delete;
    SlidChunk(SlidChunk&&) = default;
    SlidChunk& operator=(const SlidChunk&) = delete;
    SlidChunk& operator=(SlidChunk&&) = default;

    // Virtual methods (will be overridden by subclasses)
    //const char* name() const override final;
    SlidChunk* isSlidChunk() override final;

private:
    // Note, not final as SlidChunk's are subclassesed, eg, by SegmentInfo
    __attribute__((used))
    virtual void dump() const override;
};

struct ObjCOptsHeaderChunk : Chunk
{
public:
    ObjCOptsHeaderChunk();
    virtual ~ObjCOptsHeaderChunk();
    ObjCOptsHeaderChunk(const ObjCOptsHeaderChunk&) = delete;
    ObjCOptsHeaderChunk(ObjCOptsHeaderChunk&&) = delete;
    ObjCOptsHeaderChunk& operator=(const ObjCOptsHeaderChunk&) = delete;
    ObjCOptsHeaderChunk& operator=(ObjCOptsHeaderChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCHeaderInfoReadOnlyChunk : Chunk
{
public:
    ObjCHeaderInfoReadOnlyChunk();
    virtual ~ObjCHeaderInfoReadOnlyChunk();
    ObjCHeaderInfoReadOnlyChunk(const ObjCHeaderInfoReadOnlyChunk&) = delete;
    ObjCHeaderInfoReadOnlyChunk(ObjCHeaderInfoReadOnlyChunk&&) = delete;
    ObjCHeaderInfoReadOnlyChunk& operator=(const ObjCHeaderInfoReadOnlyChunk&) = delete;
    ObjCHeaderInfoReadOnlyChunk& operator=(ObjCHeaderInfoReadOnlyChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCHeaderInfoReadWriteChunk : SlidChunk
{
public:
    ObjCHeaderInfoReadWriteChunk();
    virtual ~ObjCHeaderInfoReadWriteChunk();
    ObjCHeaderInfoReadWriteChunk(const ObjCHeaderInfoReadWriteChunk&) = delete;
    ObjCHeaderInfoReadWriteChunk(ObjCHeaderInfoReadWriteChunk&&) = delete;
    ObjCHeaderInfoReadWriteChunk& operator=(const ObjCHeaderInfoReadWriteChunk&) = delete;
    ObjCHeaderInfoReadWriteChunk& operator=(ObjCHeaderInfoReadWriteChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCStringsChunk : Chunk
{
public:
    ObjCStringsChunk();
    virtual ~ObjCStringsChunk();
    ObjCStringsChunk(const ObjCStringsChunk&) = delete;
    ObjCStringsChunk(ObjCStringsChunk&&) = delete;
    ObjCStringsChunk& operator=(const ObjCStringsChunk&) = delete;
    ObjCStringsChunk& operator=(ObjCStringsChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCSelectorHashTableChunk : Chunk
{
public:
    ObjCSelectorHashTableChunk();
    virtual ~ObjCSelectorHashTableChunk();
    ObjCSelectorHashTableChunk(const ObjCSelectorHashTableChunk&) = delete;
    ObjCSelectorHashTableChunk(ObjCSelectorHashTableChunk&&) = delete;
    ObjCSelectorHashTableChunk& operator=(const ObjCSelectorHashTableChunk&) = delete;
    ObjCSelectorHashTableChunk& operator=(ObjCSelectorHashTableChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCClassHashTableChunk : Chunk
{
public:
    ObjCClassHashTableChunk();
    virtual ~ObjCClassHashTableChunk();
    ObjCClassHashTableChunk(const ObjCClassHashTableChunk&) = delete;
    ObjCClassHashTableChunk(ObjCClassHashTableChunk&&) = delete;
    ObjCClassHashTableChunk& operator=(const ObjCClassHashTableChunk&) = delete;
    ObjCClassHashTableChunk& operator=(ObjCClassHashTableChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCProtocolHashTableChunk : Chunk
{
public:
    ObjCProtocolHashTableChunk();
    virtual ~ObjCProtocolHashTableChunk();
    ObjCProtocolHashTableChunk(const ObjCProtocolHashTableChunk&) = delete;
    ObjCProtocolHashTableChunk(ObjCProtocolHashTableChunk&&) = delete;
    ObjCProtocolHashTableChunk& operator=(const ObjCProtocolHashTableChunk&) = delete;
    ObjCProtocolHashTableChunk& operator=(ObjCProtocolHashTableChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCCanonicalProtocolsChunk : SlidChunk
{
public:
    ObjCCanonicalProtocolsChunk();
    virtual ~ObjCCanonicalProtocolsChunk();
    ObjCCanonicalProtocolsChunk(const ObjCCanonicalProtocolsChunk&) = delete;
    ObjCCanonicalProtocolsChunk(ObjCCanonicalProtocolsChunk&&) = delete;
    ObjCCanonicalProtocolsChunk& operator=(const ObjCCanonicalProtocolsChunk&) = delete;
    ObjCCanonicalProtocolsChunk& operator=(ObjCCanonicalProtocolsChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct ObjCIMPCachesChunk : Chunk
{
public:
    ObjCIMPCachesChunk();
    virtual ~ObjCIMPCachesChunk();
    ObjCIMPCachesChunk(const ObjCIMPCachesChunk&) = delete;
    ObjCIMPCachesChunk(ObjCIMPCachesChunk&&) = delete;
    ObjCIMPCachesChunk& operator=(const ObjCIMPCachesChunk&) = delete;
    ObjCIMPCachesChunk& operator=(ObjCIMPCachesChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct SwiftOptsHeaderChunk : Chunk
{
public:
    SwiftOptsHeaderChunk();
    virtual ~SwiftOptsHeaderChunk();
    SwiftOptsHeaderChunk(const SwiftOptsHeaderChunk&) = delete;
    SwiftOptsHeaderChunk(SwiftOptsHeaderChunk&&) = delete;
    SwiftOptsHeaderChunk& operator=(const SwiftOptsHeaderChunk&) = delete;
    SwiftOptsHeaderChunk& operator=(SwiftOptsHeaderChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct SwiftProtocolConformancesHashTableChunk : Chunk
{
public:
    SwiftProtocolConformancesHashTableChunk();
    virtual ~SwiftProtocolConformancesHashTableChunk();
    SwiftProtocolConformancesHashTableChunk(const SwiftProtocolConformancesHashTableChunk&) = delete;
    SwiftProtocolConformancesHashTableChunk(SwiftProtocolConformancesHashTableChunk&&) = delete;
    SwiftProtocolConformancesHashTableChunk& operator=(const SwiftProtocolConformancesHashTableChunk&) = delete;
    SwiftProtocolConformancesHashTableChunk& operator=(SwiftProtocolConformancesHashTableChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct CacheTrieChunk : Chunk
{
public:
    CacheTrieChunk(Kind kind);
    virtual ~CacheTrieChunk();
    CacheTrieChunk(const CacheTrieChunk&) = delete;
    CacheTrieChunk(CacheTrieChunk&&) = delete;
    CacheTrieChunk& operator=(const CacheTrieChunk&) = delete;
    CacheTrieChunk& operator=(CacheTrieChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct PatchTableChunk : Chunk
{
public:
    PatchTableChunk();
    virtual ~PatchTableChunk();
    PatchTableChunk(const PatchTableChunk&) = delete;
    PatchTableChunk(PatchTableChunk&&) = delete;
    PatchTableChunk& operator=(const PatchTableChunk&) = delete;
    PatchTableChunk& operator=(PatchTableChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct PrebuiltLoaderChunk : Chunk
{
public:
    PrebuiltLoaderChunk(Kind kind);
    virtual ~PrebuiltLoaderChunk();
    PrebuiltLoaderChunk(const PrebuiltLoaderChunk&) = delete;
    PrebuiltLoaderChunk(PrebuiltLoaderChunk&&) = delete;
    PrebuiltLoaderChunk& operator=(const PrebuiltLoaderChunk&) = delete;
    PrebuiltLoaderChunk& operator=(PrebuiltLoaderChunk&&) = delete;

    // Virtual methods
    const char* name() const override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// In the .symbols file, this dyld_cache_local_symbols_info and dyld_cache_local_symbols_entry_64
// The rest is the nlist and symbol strings chunks
struct UnmappedSymbolsChunk : Chunk
{
public:
    UnmappedSymbolsChunk();
    virtual ~UnmappedSymbolsChunk();
    UnmappedSymbolsChunk(const UnmappedSymbolsChunk&) = delete;
    UnmappedSymbolsChunk(UnmappedSymbolsChunk&&) = default;
    UnmappedSymbolsChunk& operator=(const UnmappedSymbolsChunk&) = delete;
    UnmappedSymbolsChunk& operator=(UnmappedSymbolsChunk&&) = default;

    // Virtual methods
    const char* name() const override final;

    // FIXME: We really don't want to make kind public
    using Chunk::kind;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

struct DylibSegmentChunk : SlidChunk
{
    std::string_view        segmentName; // Points to the LC_SEGMENT in the input dylib
    const InputFile*        inputFile;

    // Where are we in the input file (set by categorizeDylibSegments())
    InputDylibFileOffset    inputFileOffset;
    InputDylibFileSize      inputFileSize;
    InputDylibVMAddress     inputVMAddress;
    InputDylibVMSize        inputVMSize;

public:
    DylibSegmentChunk(Kind kind, uint64_t minAlignment);
    virtual ~DylibSegmentChunk();
    DylibSegmentChunk(const DylibSegmentChunk&) = delete;
    DylibSegmentChunk(DylibSegmentChunk&&) = default;
    DylibSegmentChunk& operator=(const DylibSegmentChunk&) = delete;
    DylibSegmentChunk& operator=(DylibSegmentChunk&&) = default;

    // Virtual methods
    const char* name() const override final;
    const DylibSegmentChunk* isDylibSegmentChunk() const override final;

    // FIXME: We really don't want to make kind public
    using Chunk::kind;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// An individual piece of LINKEDIT, eg, an export trie, or function starts
struct LinkeditDataChunk : Chunk
{
    const InputFile*        inputFile;

    // Where are we in the input file (set by categorizeDylibLinkedit())
    InputDylibFileOffset    inputFileOffset;
    InputDylibFileSize      inputFileSize;

public:
    LinkeditDataChunk(Kind kind, uint64_t minAlignment);
    virtual ~LinkeditDataChunk();
    LinkeditDataChunk(const LinkeditDataChunk&) = delete;
    LinkeditDataChunk(LinkeditDataChunk&&) = default;
    LinkeditDataChunk& operator=(const LinkeditDataChunk&) = delete;
    LinkeditDataChunk& operator=(LinkeditDataChunk&&) = default;

    // Virtual methods
    const char* name() const override final;
    const LinkeditDataChunk* isLinkeditDataChunk() const override final;

    bool isIndirectSymbols() const;
    bool isNList() const;
    bool isNSymbolStrings() const;

    // FIXME: We really don't want to make kind public
    using Chunk::kind;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// The optimizd nlists (local, global, undef) for a given dylib
struct NListChunk : Chunk
{
public:
    NListChunk();
    virtual ~NListChunk();
    NListChunk(const NListChunk&) = delete;
    NListChunk(NListChunk&&) = default;
    NListChunk& operator=(const NListChunk&) = delete;
    NListChunk& operator=(NListChunk&&) = default;

    // Virtual methods
    const char* name() const override final;

    std::vector<struct nlist>       nlist32;
    std::vector<struct nlist_64>    nlist64;

    uint32_t localsStartIndex   = 0;
    uint32_t localsCount        = 0;
    uint32_t globalsStartIndex  = 0;
    uint32_t globalsCount       = 0;
    uint32_t undefsStartIndex   = 0;
    uint32_t undefsCount        = 0;

    // FIXME: We really don't want to make kind public
    using Chunk::kind;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// The optimized symbols strings for a given subCache
struct SymbolStringsChunk : Chunk
{
public:
    SymbolStringsChunk();
    virtual ~SymbolStringsChunk();
    SymbolStringsChunk(const SymbolStringsChunk&) = delete;
    SymbolStringsChunk(SymbolStringsChunk&&) = default;
    SymbolStringsChunk& operator=(const SymbolStringsChunk&) = delete;
    SymbolStringsChunk& operator=(SymbolStringsChunk&&) = default;

    // Virtual methods
    const char* name() const override final;

    // FIXME: We really don't want to make kind public
    using Chunk::kind;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// The uniqued GOTs for a given subCache
struct UniquedGOTsChunk : SlidChunk
{
public:
    UniquedGOTsChunk();
    virtual ~UniquedGOTsChunk();
    UniquedGOTsChunk(const UniquedGOTsChunk&) = delete;
    UniquedGOTsChunk(UniquedGOTsChunk&&) = default;
    UniquedGOTsChunk& operator=(const UniquedGOTsChunk&) = delete;
    UniquedGOTsChunk& operator=(UniquedGOTsChunk&&) = default;

    // Virtual methods
    const char* name() const override final;
    UniquedGOTsChunk* isUniquedGOTsChunk() override final;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// The stubs for a universal cache.  One StubsChunk per source dylib stubs section
struct StubsChunk : Chunk
{
public:
    StubsChunk();
    virtual ~StubsChunk();
    StubsChunk(const StubsChunk&) = delete;
    StubsChunk(StubsChunk&&) = default;
    StubsChunk& operator=(const StubsChunk&) = delete;
    StubsChunk& operator=(StubsChunk&&) = default;

    // Virtual methods
    const char* name() const override final;
    StubsChunk* isStubsChunk() override final;

    // A dylib might have multiple segment/section's with stubs.  This is to track which one
    // this stubs chunk corresponds to
    // Note we use strings, because forEachSegment/Section might return pointers to temporary strings
    std::string segmentName;
    std::string sectionName;

private:
    __attribute__((used))
    virtual void dump() const override final;
};

// Space reserved for dynamic content generated at runtime
struct DynamicConfigChunk : Chunk
{
public:
    DynamicConfigChunk();
    virtual ~DynamicConfigChunk();
    DynamicConfigChunk(const DynamicConfigChunk&) = delete;
    DynamicConfigChunk(DynamicConfigChunk&&) = default;
    DynamicConfigChunk& operator=(const DynamicConfigChunk&) = delete;
    DynamicConfigChunk& operator=(DynamicConfigChunk&&) = default;

    // Virtual methods
    const char* name() const override final;
    bool isZeroFill() const override final;
private:
    __attribute__((used))
    virtual void dump() const override final;
};

} // namespace cache_builder

#endif /* Chunk_hpp */
