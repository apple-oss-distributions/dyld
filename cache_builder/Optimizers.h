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

#ifndef Optimizers_hpp
#define Optimizers_hpp

#include "CachePatching.h"
#include "Chunk.h"
#include "ImpCachesBuilder.h"
#include "Map.h"
#include "PerfectHash.h"
#include "SectionCoalescer.h"

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace dyld4 {
    struct PrebuiltLoaderSet;
}

namespace cache_builder
{

struct BuilderConfig;
struct CacheDylib;

struct HashString {
    size_t operator()(const std::string_view& v) const {
        return std::hash<std::string_view>{}(v);
    }

    static size_t hash(const std::string_view& v) {
        return std::hash<std::string_view>{}(v);
    }
};

struct EqualString {
    bool operator()(std::string_view s1, std::string_view s2) const {
        return s1 == s2;
    }

    static bool equal(std::string_view s1, std::string_view s2) {
        return s1 == s2;
    }
};

// Use our own map as std::unordered_map makes way too many allocations
template<typename V>
using StringMap = dyld3::Map<std::string_view, V, HashString, EqualString>;

// Map from strings to their offsets in to the new string buffer
using SymbolStringMap = StringMap<uint32_t>;

// A poorly named catch-all for the objc stuff which isn't selectors/classes/protocols
struct ObjCOptimizer
{
    // all the dylibs containing objc
    std::vector<CacheDylib*>                objcDylibs;

    // Flags we accumulate during optimization and need to emit in the objc opt header
    bool                                    foundMissingWeakSuperclass = false;

    // How much space we need for the optimization header
    uint64_t                                optsHeaderByteSize = 0;

    // The Chunk in a SubCache which will contain the optimization header
    const ObjCOptsHeaderChunk*              optsHeaderChunk = nullptr;

    // How much space we need for the HeaderInfoRO array
    uint64_t                                headerInfoReadOnlyByteSize = 0;

    // The Chunk in a SubCache which will contain the HeaderInfoRO array
    const ObjCHeaderInfoReadOnlyChunk*      headerInfoReadOnlyChunk = nullptr;

    // How much space we need for the HeaderInfoRW array
    uint64_t                                headerInfoReadWriteByteSize = 0;

    // The Chunk in a SubCache which will contain the HeaderInfoROWarray
    const ObjCHeaderInfoReadWriteChunk*     headerInfoReadWriteChunk = nullptr;

    struct header_info_ro_32_t
    {
        int32_t mhdr_offset;     // offset to mach_header or mach_header_64
        int32_t info_offset;     // offset to objc_image_info *
    };

    struct header_info_ro_64_t
    {
        int64_t mhdr_offset;     // offset to mach_header or mach_header_64
        int64_t info_offset;     // offset to objc_image_info *
    };

    struct header_info_ro_list_t
    {
        uint32_t count;
        uint32_t entsize;
        uint8_t  arrayBase[]; // Really an array of header_info_ro_32_t/header_info_ro_64_t
    };

    struct header_info_rw_32_t
    {
        [[maybe_unused]] uint32_t isLoaded              : 1;
        [[maybe_unused]] uint32_t allClassesRealized    : 1;
        [[maybe_unused]] uint32_t next                  : 30;
    };
    static_assert(sizeof(header_info_rw_32_t) == sizeof(uint32_t));

    struct header_info_rw_64_t
    {
        [[maybe_unused]] uint64_t isLoaded              : 1;
        [[maybe_unused]] uint64_t allClassesRealized    : 1;
        [[maybe_unused]] uint64_t next                  : 62;
    };
    static_assert(sizeof(header_info_rw_64_t) == sizeof(uint64_t));

    struct header_info_rw_list_t
    {
        uint32_t count;
        uint32_t entsize;
        uint8_t  arrayBase[]; // Really an array of header_info_rw_32_t/header_info_rw_64_t
    };
};

struct ObjCIMPCachesOptimizer
{
    struct ClassKey
    {
        std::string_view    name;
        bool                metaclass = false;
    };

    struct ClassKeyHash
    {
        size_t operator()(const ClassKey& value) const
        {
            return std::hash<std::string_view>{}(value.name) ^ std::hash<uint64_t>{}(value.metaclass ? 1 : 0);
        }
    };

    struct ClassKeyEqual
    {
        bool operator()(const ClassKey& a, const ClassKey& b) const
        {
            return (a.metaclass == b.metaclass) && (a.name == b.name);
        }
    };

    typedef std::pair<imp_caches::IMPCache, VMOffset> IMPCacheAndOffset;
    typedef std::unordered_map<ClassKey, IMPCacheAndOffset, ClassKeyHash, ClassKeyEqual> IMPCacheMap;
    typedef std::pair<const CacheDylib*, InputDylibVMAddress> InputDylibLocation;
    typedef std::unordered_map<imp_caches::FallbackClass, InputDylibLocation, imp_caches::FallbackClassHash> ClassMap;
    typedef std::unordered_map<imp_caches::BucketMethod, InputDylibLocation, imp_caches::BucketMethodHash> MethodMap;

    std::vector<imp_caches::Dylib>          dylibs;

    std::unique_ptr<imp_caches::Builder>    builder;

    // One map per dylib, of all IMP caches in that dylib
    std::vector<IMPCacheMap>                dylibIMPCaches;

    // Map of class locations so that we can find fallback classes
    ClassMap                                classMap;

    // Map of method locations so that we can find bucket methods
    MethodMap                               methodMap;

    // How much space we need for the imp caches
    uint64_t                                impCachesTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the imp caches
    const ObjCIMPCachesChunk*               impCachesChunk = nullptr;

    // Constants for the magic section in libobjc where we need to store offsets
    const std::string_view                  sharedCacheOffsetsSegmentName = "__DATA_CONST";
    const std::string_view                  sharedCacheOffsetsSectionName = "__objc_scoffs";
    const std::string_view                  sharedCacheOffsetsSymbolName = "_objc_opt_offsets";
};

struct ObjCSelectorOptimizer
{
    // Map from selector string to offset in to the selector buffer
    StringMap<VMOffset>                 selectorsMap;

    // Holds all the selectors in the order they'll be emitted in to the final binary.
    // This is to give a deterministic input to the perfect hash
    // to the perfect hash
    std::vector<objc::ObjCString>       selectorsArray;

    // How much space we need for all the selectors in the new contiguous buffer
    uint64_t                            selectorStringsTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the selector strings
    const ObjCStringsChunk*             selectorStringsChunk = nullptr;

    // How much space we need for the selector hash table
    uint64_t                            selectorHashTableTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the selector hash table
    const ObjCSelectorHashTableChunk*   selectorHashTableChunk = nullptr;
};

struct ObjCClassOptimizer
{
    // Map from class name string to offset in to the class names buffer
    StringMap<VMOffset>                 namesMap;

    // Holds all the class names in the order we visited them, to give a deterministic input
    // to the perfect hash
    std::vector<objc::ObjCString>       namesArray;

    // How much space we need for all the class names in the new contiguous buffer
    uint64_t                            nameStringsTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the class name strings
    const ObjCStringsChunk*             classNameStringsChunk = nullptr;

    // How much space we need for the class hash table
    uint64_t                            classHashTableTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the class hash table
    const ObjCClassHashTableChunk*      classHashTableChunk = nullptr;

    // Map of all classes in all dylibs.  Can contain duplicates with the same name
    objc::class_map                     classes;
};

struct ObjCProtocolOptimizer
{
    // Map from protocol name string to offset in to the protocol names buffer
    StringMap<VMOffset>                     namesMap;

    // Holds all the protocol names in the order we visited them, to give a deterministic input
    // to the perfect hash
    std::vector<objc::ObjCString>           namesArray;

    // How much space we need for all the protocol names in the new contiguous buffer
    uint64_t                                nameStringsTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the protocol name strings
    const ObjCStringsChunk*                 protocolNameStringsChunk = nullptr;

    // How much space we need for the protocol hash table
    uint64_t                                protocolHashTableTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the protocol hash table
    const ObjCProtocolHashTableChunk*       protocolHashTableChunk = nullptr;

    // How much space we need for the canonical protocol definitions
    uint64_t                                canonicalProtocolsTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the canonical protocol definitions
    ObjCCanonicalProtocolsChunk*            canonicalProtocolsChunk = nullptr;

    // Map from swift demangled name string to offset in to the string buffer
    StringMap<VMOffset>                     swiftDemangledNamesMap;

    // Holds all the swift demangled names in the order we visited them
    std::list<std::string>                  swiftDemangledNames;

    // How much space we need for all the swift demangled names in the new contiguous buffer
    uint64_t                                swiftDemangledNameStringsTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the demangled name strings
    const ObjCStringsChunk*                 swiftDemangledNameStringsChunk = nullptr;

    // Map of all protocols in all dylibs.  Can contain duplicates with the same name
    objc::protocol_map                      protocols;
};

struct SwiftProtocolConformanceOptimizer
{
    // How much space we need for the Swift optimization header
    uint64_t                                    optsHeaderByteSize = 0;

    // The Chunk in a SubCache which will contain the Swift optimization header
    SwiftOptsHeaderChunk*                       optsHeaderChunk = nullptr;

    // How much space we need for the type conformances hash table
    uint64_t                                    typeConformancesHashTableSize = 0;

    // The Chunk in a SubCache which will contain the type conformances hash table
    SwiftProtocolConformancesHashTableChunk*    typeConformancesHashTable = nullptr;

    // How much space we need for the metadata conformances hash table
    uint64_t                                    metadataConformancesHashTableSize = 0;

    // The Chunk in a SubCache which will contain the metadata conformances hash table
    SwiftProtocolConformancesHashTableChunk*    metadataConformancesHashTable = nullptr;

    // How much space we need for the foreignType conformances hash table
    uint64_t                                    foreignTypeConformancesHashTableSize = 0;

    // The Chunk in a SubCache which will contain the foreignType conformances hash table
    SwiftProtocolConformancesHashTableChunk*    foreignTypeConformancesHashTable = nullptr;
};

struct DylibTrieOptimizer
{
    // The actual trie buffer.
    std::vector<uint8_t>            dylibsTrie;

    // The Chunk in a SubCache which will contain the dylib trie
    const CacheTrieChunk*           dylibsTrieChunk = nullptr;
};

struct SymbolStringsOptimizer
{
    struct LocalSymbolInfo
    {
        uint64_t    dylibOffset;
        uint32_t    nlistStartIndex;
        uint32_t    nlistCount;
    };

    SymbolStringMap             stringMap;

    // The Chunk in a SubCache which will contain the symbol strings
    const SymbolStringsChunk*   symbolStringsChunk = nullptr;
};

struct UnmappedSymbolsOptimizer
{
    struct LocalSymbolInfo
    {
        uint32_t    nlistStartIndex     = 0;
        uint32_t    nlistCount          = 0;
    };

    // On embedded, locals are unmapped and stored in a .symols file.  This is the map
    // of those strings
    SymbolStringMap                 stringMap;
    uint32_t                        stringBufferSize    = 0;

    // Each dylib has an entry tracking its unmapped locals in the .symbol file nlist
    std::vector<LocalSymbolInfo>    symbolInfos;

    // The header for the unmapped locals data structure
    UnmappedSymbolsChunk            unmappedSymbolsChunk;

    // The Chunk in the .symbols subCache which will contain the strings
    SymbolStringsChunk              symbolStringsChunk;

    // The Chunk in the .symbols subCache which will contain the nlist
    NListChunk                      symbolNlistChunk;
};

struct UniquedGOTsOptimizer
{
    CoalescedGOTSection         regularGOTs;
    CoalescedGOTSection         authGOTs;

    // The Chunk in a SubCache which will contain the uniqued GOTs
    UniquedGOTsChunk*     regularGOTsChunk = nullptr;
    UniquedGOTsChunk*     authGOTsChunk = nullptr;
};

struct StubOptimizer
{

    void addDefaultSymbols();

    static uint64_t gotAddrFromArm64Stub(Diagnostics& diag, std::string_view dylibID,
                                         const uint8_t* stubInstructions, uint64_t stubVMAddr);
    static uint64_t gotAddrFromArm64_32Stub(Diagnostics& diag, std::string_view dylibID,
                                            const uint8_t* stubInstructions, uint64_t stubVMAddr);
    static uint64_t gotAddrFromArm64eStub(Diagnostics& diag, std::string_view dylibID,
                                          const uint8_t* stubInstructions, uint64_t stubVMAddr);

    static void generateArm64StubTo(uint8_t* stubBuffer,
                                    uint64_t stubVMAddr, uint64_t targetVMAddr);
    static void generateArm64eStubTo(uint8_t* stubBuffer,
                                     uint64_t stubVMAddr, uint64_t targetVMAddr);
    static void generateArm64_32StubTo(uint8_t* stubBuffer,
                                       uint64_t stubVMAddr, uint64_t targetVMAddr);

    static void generateArm64StubToGOT(uint8_t* stubBuffer,
                                       uint64_t stubVMAddr, uint64_t targetVMAddr);
    static void generateArm64eStubToGOT(uint8_t* stubBuffer,
                                        uint64_t stubVMAddr, uint64_t targetVMAddr);
    static void generateArm64_32StubToGOT(uint8_t* stubBuffer,
                                          uint64_t stubVMAddr, uint64_t targetVMAddr);

    // Some never eliminate symbols are parsed from export tries.  This owns those strings
    // as we can't point to them with a std::string_view
    std::vector<std::string> neverStubEliminateStrings;

    std::unordered_set<std::string_view> neverStubEliminate;
};

struct PatchTableOptimizer
{
    // How much space we need for the patch table
    uint64_t                    patchTableTotalByteSize = 0;

    // The Chunk in a SubCache which will contain the dylib patch table
    const PatchTableChunk*      patchTableChunk = nullptr;
    
    // One PatchInfo for each cache dylib.
    // After bind(), each dylib will have a list of all the locations it used in other dylibs.
    // There will be one list of locations for each bindTargets[] entry in the dylib
    std::vector<PatchInfo>      patchInfos;
};

struct PrebuiltLoaderBuilder
{
    // How much space we need for the cache dylibs PrebuiltLoader's
    uint64_t                        cacheDylibsLoaderSize = 0;

    // The Chunk in a SubCache which will contain the cache dylibs PrebuiltLoader's
    const PrebuiltLoaderChunk*      cacheDylibsLoaderChunk = nullptr;

    // How much space we need for the executables PrebuiltLoader's
    uint64_t                        executablesLoaderSize = 0;

    // The Chunk in a SubCache which will contain the executables PrebuiltLoader's
    const PrebuiltLoaderChunk*      executablesLoaderChunk = nullptr;

    const dyld4::PrebuiltLoaderSet* cachedDylibsLoaderSet = nullptr;

    // How much space we need for the executables trie
    uint64_t                        executablesTrieSize = 0;

    // The Chunk in a SubCache which will contain the executable trie
    const CacheTrieChunk*           executableTrieChunk = nullptr;
};

} // namespace cache_builder

#endif /* Optimizers_hpp */
