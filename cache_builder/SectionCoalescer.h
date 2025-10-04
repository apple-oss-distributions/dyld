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

#ifndef SectionCoalescer_h
#define SectionCoalescer_h

#include "Chunk.h"
#include "MachOFile.h"
#include "Types.h"
#include "dyld_cache_format.h"

// mach_o
#include "Header.h"

#include <string>
#include <unordered_map>
#include <memory>

class OptimizedSection;
class OptimizedStringSection;
class OptimizedGOTSection;

namespace cache_builder
{
struct Region;
struct SubCache;
}

class CoalescedSection
{
public:
    // Note this is for debugging only
    uint64_t              savedSpace = 0;
};

class CoalescedStringsSection : public CoalescedSection
{
public:
    CoalescedStringsSection(std::string_view sectionName) : sectionName(sectionName) { }

    void clear()
    {
        *this = CoalescedStringsSection(this->sectionName);
    }

private:
    friend class OptimizedStringSection;

    std::string_view                                sectionName;
    // Map from class strings to offsets in to the strings buffer
    std::unordered_map<std::string_view, uint32_t>  stringsToOffsets;

    // Points to the chunk in the subCache where these coalesced values live.
    cache_builder::Chunk* cacheChunk = nullptr;
};

class CoalescedGOTSection : public CoalescedSection
{
public:
    struct GOTKey
    {
        std::string_view                    targetSymbolName;
        std::string_view                    targetDylibName;
        dyld3::MachOFile::PointerMetaData   pmd;
        bool                                isWeakImport;
        bool                                isFunctionVariant    = false;
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
            hash ^= std::hash<bool>{}(v.isWeakImport);
            hash ^= std::hash<bool>{}(v.isFunctionVariant);
            return hash;
        }
    };

    struct EqualTo
    {
        bool operator()(const GOTKey& a, const GOTKey& b) const
        {
            return (a.isWeakImport == b.isWeakImport)
                && (a.targetSymbolName == b.targetSymbolName)
                && (a.targetDylibName == b.targetDylibName)
                && (a.isFunctionVariant == b.isFunctionVariant)
                && (memcmp(&a.pmd, &b.pmd, sizeof(a.pmd)) == 0);
        }
    };

    struct FunctionVariantInfo { uint32_t dylibIndex; uint32_t variantIndex; };
    

    void addClientDylibSection(OptimizedGOTSection* section);

    std::pair<uint32_t, bool> addOptimizedOffset(uint32_t pointerSize, GOTKey key);
    void addFunctionVariantInfo(GOTKey key, FunctionVariantInfo info);

    // For printing stats
    uint64_t numSourceGOTs() const;
    uint64_t numCacheGOTs() const;

    bool empty() const;

    void finalize(uint32_t pointerSize, std::string_view sectionName,
                  const cache_builder::BuilderConfig& config,
                  cache_builder::SubCache& subCache, cache_builder::Region& region);

    void forEachFunctionVariant(void (^callback)(const FunctionVariantInfo& tv, uint64_t gotVMAddr,
                                                 dyld3::MachOFile::PointerMetaData pmd)) const;

    void* gotLocation(CacheVMAddress gotVMAddr);
    bool shouldEmitGOT(CacheVMAddress gotVMAddr) const;
    void trackFixup(void* loc);

private:
    friend class OptimizedGOTSection;

    typedef std::unordered_map<GOTKey, uint32_t, Hash, EqualTo> GOTMap;
    typedef std::unordered_map<GOTKey, FunctionVariantInfo, Hash, EqualTo> FVMap;

    // Once all dylibs have been found for this section, sort it
    static void sort(uint32_t pointerSize, std::string_view sectionName,
                     std::span<OptimizedGOTSection*> dylibSections,
                     bool functionVariants,
                     CoalescedGOTSection::GOTMap& gotMap);

    uint64_t gotVMSize(uint32_t pointerSize) const;
    uint64_t fvVMSize(uint32_t pointerSize) const;

    // Map from bind target to offsets in to the GOTs buffer
    GOTMap  gotTargetsToOffsets;
    GOTMap  fvTargetsToOffsets;
    FVMap   functionVariantIndexes;

    // Track all dylib sections coalesced in to this cache section
    std::vector<OptimizedGOTSection*> dylibSections;
    // Points to the chunk in the subCache where these coalesced values live.
    std::unique_ptr<cache_builder::UniquedGOTsChunk> gotChunk;
    // Points to the chunk for function variant GOTs
    std::unique_ptr<cache_builder::UniquedGOTsChunk> fvChunk;
};

// A section may be completely coalesced and removed, eg, strings,
// or it may be coalesced and copies made elsewhere, eg, GOTs.  In the GOTs case, we
// don't remove the original section
class OptimizedSection
{
protected:
    OptimizedSection(bool sectionWillBeRemoved, const char* name)
        : sectionWillBeRemoved(sectionWillBeRemoved), name(name) { }

public:
    // Whether or not this section will be removed.  Eg, GOTs aren't currently removed from
    // their original binary
    bool sectionWillBeRemoved;

    void setSourceSectionInfo(const mach_o::Header::SectionInfo& info);

    // Gets the cache VM address for the value at the given section offset in the original section
    virtual std::optional<uint64_t> cacheVMAddress(uint32_t originalDylibSectionOffset) const = 0;

    bool sectionWasRemoved() const;
    bool sectionWasOptimized() const;

    void addUnoptimizedOffset(uint32_t sourceSectionOffset);

    void reassignOffsets(const std::unordered_map<uint32_t, uint32_t>& oldToNewOffsetMap,
                         bool functionVariants);

    uint64_t numOptimizedEntries() const;

protected:

    struct OffsetInfo
    {
        uint32_t cacheSectionOffset;
        bool isFunctionVariant;
    };

    // Map from offsets in the original dylib section to the cache offset and info
    std::unordered_map<uint32_t, OffsetInfo>    offsetMap;

    // Some offsets are not in the above offsetMap, even though we'd typically want to know about every
    // reference to the given section.  Eg, we only optimize binds in __got, not rebases.  But we want
    // to track the rebases just so that we know of every element in the section.
    std::set<uint32_t>                          unoptimizedOffsets;

    const char* name;

    std::optional<mach_o::Header::SectionInfo> sourceSectionInfo;
};

class OptimizedStringSection : public OptimizedSection
{
public:
    OptimizedStringSection(const char* name) : OptimizedSection(true, name)
    {
    }

    void setSubCacheSection(CoalescedStringsSection* section);

    std::optional<uint64_t> cacheVMAddress(uint32_t originalDylibSectionOffset) const override;

private:

    // Different subCache's may contain their own GOTs/strings.  We can't deduplicate
    // cache-wide in to a single buffer due to constraints such as 32-bit offsets
    // This points to the cache section we coalesced into, for this section in this dylib
    CoalescedStringsSection* subCacheSection = nullptr;
};

class OptimizedGOTSection : public OptimizedSection
{
public:
    OptimizedGOTSection(const char* name) : OptimizedSection(false, name)
    {
    }

    void setSubCacheSection(CoalescedGOTSection* section);
    
    std::optional<uint64_t> cacheVMAddress(uint32_t originalDylibSectionOffset) const override;

    bool addOptimizedOffset(uint32_t sourceSectionOffset, uint32_t pointerSize,
                            CoalescedGOTSection::GOTKey key);

    void addFunctionVariantInfo(CoalescedGOTSection::GOTKey key,
                                CoalescedGOTSection::FunctionVariantInfo info);

    typedef std::unordered_map<const InputDylibVMAddress, cache_builder::ChunkPlusOffset, InputDylibVMAddressHash, InputDylibVMAddressEqual> CoalescedGOTsMap;
    CoalescedGOTsMap getCoalescedGOTsMap() const;

    void forEachCacheGOTChunk(void (^callback)(const cache_builder::Chunk* cacheGOTChunk)) const;

private:

    // Different subCache's may contain their own GOTs/strings.  We can't deduplicate
    // cache-wide in to a single buffer due to constraints such as 32-bit offsets
    // This points to the cache section we coalesced into, for this section in this dylib
    CoalescedGOTSection* subCacheSection = nullptr;
};

struct DylibSectionCoalescer
{
    bool                    sectionWasRemoved(std::string_view segmentName, std::string_view sectionName) const;
    bool                    sectionWasOptimized(std::string_view segmentName, std::string_view sectionName) const;
    OptimizedSection*       getSection(std::string_view segmentName, std::string_view sectionName);
    const OptimizedSection* getSection(std::string_view segmentName, std::string_view sectionName) const;

    void                    forEachCacheGOTChunk(void (^callback)(const cache_builder::Chunk* cacheGOTChunk)) const;

    OptimizedStringSection objcClassNames = { "objc class names" };
    OptimizedStringSection objcMethNames  = { "objc method names" };
    OptimizedStringSection objcMethTypes  = { "objc method types" };
    OptimizedGOTSection gots              = { "gots" };
    OptimizedGOTSection auth_gots         = { "auth gots" };
    OptimizedGOTSection auth_ptrs         = { "auth ptrs" };
};

#endif /* SectionCoalescer_h */
