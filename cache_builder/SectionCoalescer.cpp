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

#include "SectionCoalescer.h"
#include "SubCache.h"

//
// MARK: --- CoalescedSection methods ---
//

// TODO

//
// MARK: --- CoalescedStringsSection methods ---
//

// TODO

//
// MARK: --- CoalescedGOTSection methods ---
//

void CoalescedGOTSection::addClientDylibSection(OptimizedGOTSection* section)
{
    this->dylibSections.push_back(section);
}

std::pair<uint32_t, bool> CoalescedGOTSection::addOptimizedOffset(uint32_t pointerSize,
                                                                  CoalescedGOTSection::GOTKey key)
{
    GOTMap& gotMap = key.isFunctionVariant ? fvTargetsToOffsets : gotTargetsToOffsets;
    uint32_t cacheSectionOffset = (uint32_t)(gotMap.size() * pointerSize);
    auto itAndInserted = gotMap.insert({ key, cacheSectionOffset });
    if ( itAndInserted.second ) {
        // We inserted the element, so its offset is already valid.  Nothing else to do
        return { cacheSectionOffset, false };
    } else {
        // Debugging only.  If we didn't include the GOT then we saved that many bytes
        this->savedSpace += pointerSize;
        cacheSectionOffset = itAndInserted.first->second;

        return { cacheSectionOffset, false };
    }
}

void CoalescedGOTSection::addFunctionVariantInfo(CoalescedGOTSection::GOTKey key,
                                                 CoalescedGOTSection::FunctionVariantInfo info)
{
    // store function-variant index in other map
    this->functionVariantIndexes[key] = info;
}

uint64_t CoalescedGOTSection::numSourceGOTs() const
{
    uint64_t totalSourceGOTs = 0;
    for ( const OptimizedSection* section : dylibSections ) {
        totalSourceGOTs += section->numOptimizedEntries();
    }
    return totalSourceGOTs;
}

uint64_t CoalescedGOTSection::numCacheGOTs() const
{
    return this->gotTargetsToOffsets.size() + fvTargetsToOffsets.size();
}

bool CoalescedGOTSection::empty() const
{
    return this->gotTargetsToOffsets.empty() && fvTargetsToOffsets.empty();
}

uint64_t CoalescedGOTSection::gotVMSize(uint32_t pointerSize) const
{
    return this->gotTargetsToOffsets.size() * pointerSize;
}

uint64_t CoalescedGOTSection::fvVMSize(uint32_t pointerSize) const
{
    return this->fvTargetsToOffsets.size() * pointerSize;
}

void CoalescedGOTSection::sort(uint32_t pointerSize, std::string_view sectionName,
                               std::span<OptimizedGOTSection*> dylibSections,
                               bool functionVariants,
                               CoalescedGOTSection::GOTMap& gotMap)
{
    // Sort the coalesced GOTs based on the target install name.  We find GOTs in the order we parse
    // the fixups in the dylibs, but we want the final cache to keep all GOTs for the same target near
    // each other
    typedef CoalescedGOTSection::GOTKey Key;
    std::vector<Key> sortedKeys;
    sortedKeys.reserve(gotMap.size());
    for ( const auto& keyAndValue : gotMap )
        sortedKeys.push_back(keyAndValue.first);

    std::sort(sortedKeys.begin(), sortedKeys.end(), [](const Key& a, const Key& b) {
        // sort all function-variants together at end
        if ( a.isFunctionVariant != b.isFunctionVariant )
            return b.isFunctionVariant;
        // sort first by impl dylib name
        if ( a.targetDylibName != b.targetDylibName )
            return (a.targetDylibName < b.targetDylibName);
        // if install names are the same, sort by symbol name
        return a.targetSymbolName < b.targetSymbolName;
    });

    // Rewrite entries from their original offset to the new offset
    std::unordered_map<uint32_t, uint32_t> oldToNewOffsetMap;
    for ( uint32_t i = 0; i != sortedKeys.size(); ++i ) {
        const Key& key = sortedKeys[i];
        auto it = gotMap.find(key);
        assert(it != gotMap.end());

        uint32_t newCacheSectionOffset = i * pointerSize;

        // Record the offset mapping for updating the dylibs
        oldToNewOffsetMap[it->second] = newCacheSectionOffset;

        const bool log = false;
        if ( log ) {
            printf("%s[%d]: %s\n", sectionName.data(), newCacheSectionOffset, key.targetSymbolName.data());
        }

        it->second = newCacheSectionOffset;
    }

    // Also rewrite entries in each dylib
    for ( OptimizedSection* section : dylibSections )
        section->reassignOffsets(oldToNewOffsetMap, functionVariants);
}

void CoalescedGOTSection::finalize(uint32_t pointerSize, std::string_view sectionName,
                                   const cache_builder::BuilderConfig& config,
                                   cache_builder::SubCache& subCache, cache_builder::Region& region)
{
    CoalescedGOTSection::sort(pointerSize, sectionName, this->dylibSections, false, this->gotTargetsToOffsets);
    CoalescedGOTSection::sort(pointerSize, sectionName, this->dylibSections, true, this->fvTargetsToOffsets);

    if ( !this->gotTargetsToOffsets.empty() ) {
        this->gotChunk = std::make_unique<cache_builder::UniquedGOTsChunk>();
        this->gotChunk->cacheVMSize       = CacheVMSize(this->gotVMSize(pointerSize));
        this->gotChunk->subCacheFileSize  = CacheFileSize(this->gotVMSize(pointerSize));

        region.chunks.push_back(this->gotChunk.get());
    }

    if ( !this->fvTargetsToOffsets.empty() ) {
        this->fvChunk = std::make_unique<cache_builder::UniquedGOTsChunk>();
        this->fvChunk->cacheVMSize       = CacheVMSize(this->fvVMSize(pointerSize));
        this->fvChunk->subCacheFileSize  = CacheFileSize(this->fvVMSize(pointerSize));

        // The function variants go in TPRO
        subCache.addTPROConstChunk(config, this->fvChunk.get());
    }
}

void CoalescedGOTSection::forEachFunctionVariant(void (^callback)(const CoalescedGOTSection::FunctionVariantInfo& tv, uint64_t gotVMAddr,
                                                                  dyld3::MachOFile::PointerMetaData pmd)) const
{
    for (const auto& fv : this->functionVariantIndexes) {
        uint32_t offsetInGOTSection = this->fvTargetsToOffsets.at(fv.first);
        uint64_t cacheVMAddr = this->fvChunk->cacheVMAddress.rawValue() + offsetInGOTSection;
        callback(fv.second, cacheVMAddr, fv.first.pmd);
    }
}

void* CoalescedGOTSection::gotLocation(CacheVMAddress gotVMAddr)
{
    if ( gotChunk ) {
        if ( (gotVMAddr >= gotChunk->cacheVMAddress) && (gotVMAddr < (gotChunk->cacheVMAddress + gotChunk->cacheVMSize))) {
            VMOffset cacheSectionVMOffset = gotVMAddr - gotChunk->cacheVMAddress;
            return gotChunk->subCacheBuffer + cacheSectionVMOffset.rawValue();
        }
    }

#if 0
    // Enable this if we ever need to write out the function variant GOTs
    if ( fvChunk ) {
        if ( (gotVMAddr >= fvChunk->cacheVMAddress) && (gotVMAddr < (fvChunk->cacheVMAddress + fvChunk->cacheVMSize))) {
            VMOffset cacheSectionVMOffset = gotVMAddr - fvChunk->cacheVMAddress;
            return fvChunk->subCacheBuffer + cacheSectionVMOffset.rawValue();
        }
    }
#endif

    assert(0 && "unreachable");
}

bool CoalescedGOTSection::shouldEmitGOT(CacheVMAddress gotVMAddr) const
{
    if ( gotChunk ) {
        if ( (gotVMAddr >= gotChunk->cacheVMAddress) && (gotVMAddr < (gotChunk->cacheVMAddress + gotChunk->cacheVMSize))) {
            return true;
        }
    }

    if ( fvChunk ) {
        if ( (gotVMAddr >= fvChunk->cacheVMAddress) && (gotVMAddr < (fvChunk->cacheVMAddress + fvChunk->cacheVMSize))) {
            return false;
        }
    }

    assert(0 && "unreachable");
}

void CoalescedGOTSection::trackFixup(void* loc)
{
    uint64_t rawLoc = (uint64_t)loc;
    if ( gotChunk ) {
        if ( (rawLoc >= (uint64_t)gotChunk->subCacheBuffer) && (rawLoc < ((uint64_t)gotChunk->subCacheBuffer + gotChunk->cacheVMSize.rawValue()))) {
            gotChunk->tracker.add(loc);
            return;
        }
    }

#if 0
    // Enable this if we ever need to write out the function variant GOTs
    if ( fvChunk ) {
        if ( (rawLoc >= (uint64_t)fvChunk->subCacheBuffer) && (rawLoc < ((uint64_t)fvChunk->subCacheBuffer + fvChunk->cacheVMSize.rawValue()))) {
            fvChunk->tracker.add(loc);
            return;
        }
    }
#endif

    assert(0 && "unreachable");
}

//
// MARK: --- DylibSectionCoalescer methods ---
//

// Returns true if the section was removed from the source dylib after being optimized
bool DylibSectionCoalescer::sectionWasRemoved(std::string_view segmentName,
                                              std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) ) {
        return section->sectionWasRemoved();
    }

    return false;
}

// Returns true if the section was optimized.  It may or may not have been removed too, see sectionWasRemoved().
bool DylibSectionCoalescer::sectionWasOptimized(std::string_view segmentName,
                                                std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) )
        return section->sectionWasOptimized();

    return false;
}

OptimizedSection* DylibSectionCoalescer::getSection(std::string_view segmentName,
                                                    std::string_view sectionName)
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
    } else if ( segmentName == "__DATA_CONST" ) {
        if ( sectionName == "__got" )
            return &this->gots;
    } else if ( segmentName == "__AUTH_CONST" ) {
        if ( sectionName == "__auth_got" )
            return &this->auth_gots;
        if ( sectionName == "__auth_ptr" )
            return &this->auth_ptrs;
    }

    return nullptr;
}

const OptimizedSection* DylibSectionCoalescer::getSection(std::string_view segmentName,
                                                          std::string_view sectionName) const
{
    return ((DylibSectionCoalescer*)this)->getSection(segmentName, sectionName);
}

void DylibSectionCoalescer::forEachCacheGOTChunk(void (^callback)(const cache_builder::Chunk* cacheGOTChunk)) const
{
    gots.forEachCacheGOTChunk(callback);
    auth_gots.forEachCacheGOTChunk(callback);
    auth_ptrs.forEachCacheGOTChunk(callback);
}

//
// MARK: --- OptimizedSection methods ---
//

bool OptimizedSection::sectionWasRemoved() const
{
    // Some sections, eg, GOTs, are optimized but not removed
    if ( !sectionWillBeRemoved )
        return false;

    return !offsetMap.empty();
}

bool OptimizedSection::sectionWasOptimized() const
{
    return !offsetMap.empty();
}

void OptimizedSection::addUnoptimizedOffset(uint32_t sourceSectionOffset)
{
    this->unoptimizedOffsets.insert(sourceSectionOffset);
}

void OptimizedSection::setSourceSectionInfo(const mach_o::Header::SectionInfo& info)
{
    assert(!this->sourceSectionInfo.has_value());
    this->sourceSectionInfo = info;
}

void OptimizedSection::reassignOffsets(const std::unordered_map<uint32_t, uint32_t>& oldToNewOffsetMap,
                                       bool functionVariants)
{
    for ( auto& keyAndCacheOffset : offsetMap ) {
        if ( keyAndCacheOffset.second.isFunctionVariant != functionVariants )
            continue;
        auto it = oldToNewOffsetMap.find(keyAndCacheOffset.second.cacheSectionOffset);
        assert(it != oldToNewOffsetMap.end());
        keyAndCacheOffset.second.cacheSectionOffset = it->second;
    }
}

uint64_t OptimizedSection::numOptimizedEntries() const
{
    return this->offsetMap.size();
}

//
// MARK: --- OptimizedStringsSection methods ---
//

void OptimizedStringSection::setSubCacheSection(CoalescedStringsSection* section)
{
    assert(this->subCacheSection == nullptr);
    this->subCacheSection = section;
}

std::optional<uint64_t> OptimizedStringSection::cacheVMAddress(uint32_t originalDylibSectionOffset) const
{
    auto offsetIt = offsetMap.find(originalDylibSectionOffset);
    if ( sectionWillBeRemoved ) {
        // If the section was removed then we have to find an entry for every atom in there
        assert(offsetIt != offsetMap.end());
    } else {
        // Not all GOTs are optimized, but we should find the element somewhere
        assert((offsetIt != offsetMap.end()) || unoptimizedOffsets.count(originalDylibSectionOffset));
    }

    if ( offsetIt == offsetMap.end() ) {
        // To was not fully optimized/coalesced so we have no element
        return std::nullopt;
    } else {
        assert(!offsetIt->second.isFunctionVariant);
        uint64_t baseVMAddr = subCacheSection->cacheChunk->cacheVMAddress.rawValue();
        return baseVMAddr + offsetIt->second.cacheSectionOffset;
    }
}

//
// MARK: --- OptimizedGOTSection methods ---
//

void OptimizedGOTSection::setSubCacheSection(CoalescedGOTSection* section)
{
    assert(this->subCacheSection == nullptr);
    this->subCacheSection = section;

    this->subCacheSection->addClientDylibSection(this);
}

std::optional<uint64_t> OptimizedGOTSection::cacheVMAddress(uint32_t originalDylibSectionOffset) const
{
    auto offsetIt = offsetMap.find(originalDylibSectionOffset);
    if ( sectionWillBeRemoved ) {
        // If the section was removed then we have to find an entry for every atom in there
        assert(offsetIt != offsetMap.end());
    } else {
        // Not all GOTs are optimized, but we should find the element somewhere
        assert((offsetIt != offsetMap.end()) || unoptimizedOffsets.count(originalDylibSectionOffset));
    }

    if ( offsetIt == offsetMap.end() ) {
        // To was not fully optimized/coalesced so we have no element
        return std::nullopt;
    } else {
        uint64_t baseVMAddr = 0;
        if ( offsetIt->second.isFunctionVariant )
            baseVMAddr = subCacheSection->fvChunk->cacheVMAddress.rawValue();
        else
            baseVMAddr = subCacheSection->gotChunk->cacheVMAddress.rawValue();
        return baseVMAddr + offsetIt->second.cacheSectionOffset;
    }
}

bool OptimizedGOTSection::addOptimizedOffset(uint32_t sourceSectionOffset, uint32_t pointerSize,
                                             CoalescedGOTSection::GOTKey key)
{
    auto cacheSectionOffsetAndAdded = subCacheSection->addOptimizedOffset(pointerSize, key);

    // Now keep track of this offset in our source dylib as pointing to this offset
    offsetMap[sourceSectionOffset] = { cacheSectionOffsetAndAdded.first, key.isFunctionVariant };

    return cacheSectionOffsetAndAdded.second;
}

void OptimizedGOTSection::addFunctionVariantInfo(CoalescedGOTSection::GOTKey key,
                                                 CoalescedGOTSection::FunctionVariantInfo info)
{
    // store function-variant index in other map
    subCacheSection->addFunctionVariantInfo(key, info);
}

OptimizedGOTSection::CoalescedGOTsMap OptimizedGOTSection::getCoalescedGOTsMap() const
{
    if ( this->offsetMap.empty() )
        return { };

    assert(this->sourceSectionInfo.has_value());
    InputDylibVMAddress dylibGOTBaseVMAddr(this->sourceSectionInfo->address);

    OptimizedGOTSection::CoalescedGOTsMap coalescedGOTs;
    for ( const auto& dylibOffsetAndCacheOffset : this->offsetMap ) {
        VMOffset dylibSectionOffset((uint64_t)dylibOffsetAndCacheOffset.first);
        VMOffset cacheSectionOffset((uint64_t)dylibOffsetAndCacheOffset.second.cacheSectionOffset);
        if ( dylibOffsetAndCacheOffset.second.isFunctionVariant )
            coalescedGOTs[dylibGOTBaseVMAddr + dylibSectionOffset] = { this->subCacheSection->fvChunk.get(), cacheSectionOffset };
        else
            coalescedGOTs[dylibGOTBaseVMAddr + dylibSectionOffset] = { this->subCacheSection->gotChunk.get(), cacheSectionOffset };
    }
    return coalescedGOTs;
}

void OptimizedGOTSection::forEachCacheGOTChunk(void (^callback)(const cache_builder::Chunk* cacheGOTChunk)) const
{
    if ( this->subCacheSection == nullptr )
        return;

    if ( this->subCacheSection->gotChunk != nullptr )
        callback(this->subCacheSection->gotChunk.get());

    if ( this->subCacheSection->fvChunk != nullptr )
        callback(this->subCacheSection->fvChunk.get());
}
