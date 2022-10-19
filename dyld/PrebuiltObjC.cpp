/*
 * Copyright (c) 2019-2020 Apple Inc. All rights reserved.
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

#include <assert.h>
#include <unistd.h>
#include <sys/types.h>

#include "Defines.h"
#include "Loader.h"
#include "PrebuiltLoader.h"
#include "JustInTimeLoader.h"
#include "MachOFile.h"
#include "BumpAllocator.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "OptimizerObjC.h"
#include "ObjCVisitor.h"
#include "PerfectHash.h"
#include "PrebuiltObjC.h"
#include "objc-shared-cache.h"


using dyld3::OverflowSafeArray;
typedef dyld4::PrebuiltObjC::ObjCOptimizerImage ObjCOptimizerImage;

namespace dyld4 {

////////////////////////////  ObjCStringTable ////////////////////////////////////////

uint32_t ObjCStringTable::hash(const char* key, size_t keylen) const
{
    uint64_t val   = objc::lookup8((uint8_t*)key, keylen, salt);
    uint32_t index = (uint32_t)((shift == 64) ? 0 : (val >> shift)) ^ scramble[tab[val & mask]];
    return index;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
const char* ObjCStringTable::getString(const char* selName, RuntimeState& state) const
{
    std::optional<PrebuiltLoader::BindTargetRef> target = getPotentialTarget(selName);
    if ( !target.has_value() )
        return nullptr;

    const PrebuiltLoader::BindTargetRef& nameTarget = *target;
    const PrebuiltLoader::BindTargetRef  sentinel   = getSentinel();

    if ( memcmp(&nameTarget, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0 )
        return nullptr;

    const char* stringValue = (const char*)target->value(state);
    if ( !strcmp(selName, stringValue) )
        return stringValue;
    return nullptr;
}
#endif

size_t ObjCStringTable::size(const objc::PerfectHash& phash)
{
    // Round tab[] to at least 8 in length to ensure the BindTarget's after are aligned
    uint32_t roundedTabSize        = std::max(phash.mask + 1, 8U);
    uint32_t roundedCheckBytesSize = std::max(phash.capacity, 8U);
    size_t   tableSize             = 0;
    tableSize += sizeof(ObjCStringTable);
    tableSize += roundedTabSize * sizeof(uint8_t);
    tableSize += roundedCheckBytesSize * sizeof(uint8_t);
    tableSize += phash.capacity * sizeof(PrebuiltLoader::BindTargetRef);
    return (size_t)align(tableSize, 3);
}

void ObjCStringTable::write(const objc::PerfectHash& phash, const Array<StringToTargetMapNodeT>& strings)
{
    // Set header
    capacity              = phash.capacity;
    occupied              = phash.occupied;
    shift                 = phash.shift;
    mask                  = phash.mask;
    roundedTabSize        = std::max(phash.mask + 1, 8U);
    roundedCheckBytesSize = std::max(phash.capacity, 8U);
    salt                  = phash.salt;

    // Set hash data
    for ( uint32_t i = 0; i < 256; i++ ) {
        scramble[i] = phash.scramble[i];
    }
    for ( uint32_t i = 0; i < phash.mask + 1; i++ ) {
        tab[i] = phash.tab[i];
    }

    dyld3::Array<PrebuiltLoader::BindTargetRef> targetsArray    = targets();
    dyld3::Array<uint8_t>                       checkBytesArray = checkBytes();

    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();

    // Set offsets to the sentinel
    for ( uint32_t i = 0; i < phash.capacity; i++ ) {
        targetsArray[i] = sentinel;
    }
    // Set checkbytes to 0
    for ( uint32_t i = 0; i < phash.capacity; i++ ) {
        checkBytesArray[i] = 0;
    }

    // Set real string offsets and checkbytes
    for ( const auto& s : strings ) {
        assert(memcmp(&s.second, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) != 0);
        uint32_t h         = hash(s.first);
        targetsArray[h]    = s.second;
        checkBytesArray[h] = checkbyte(s.first);
    }
}

////////////////////////////  ObjCSelectorOpt ////////////////////////////////////////
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
const char* ObjCSelectorOpt::getStringAtIndex(uint32_t index, RuntimeState& state) const
{
    if ( index >= capacity )
        return nullptr;

    PrebuiltLoader::BindTargetRef       target   = targets()[index];
    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();
    if ( memcmp(&target, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0 )
        return nullptr;

    const char* stringValue = (const char*)target.value(state);
    return stringValue;
}
#endif

void ObjCSelectorOpt::forEachString(void (^callback)(const PrebuiltLoader::BindTargetRef& target)) const
{
    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();

    dyld3::Array<PrebuiltLoader::BindTargetRef> stringTargets = targets();
    for ( const PrebuiltLoader::BindTargetRef& target : stringTargets ) {
        if ( memcmp(&target, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0 )
            continue;
        callback(target);
    }
}

////////////////////////////  ObjCClassOpt ////////////////////////////////////////

// Returns true if the class was found and the callback said to stop
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
bool ObjCClassOpt::forEachClass(const char* className, RuntimeState& state,
                                void (^callback)(void* classPtr, bool isLoaded, bool* stop)) const
{
    uint32_t index = getIndex(className);
    if ( index == ObjCStringTable::indexNotFound )
        return false;

    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();

    const PrebuiltLoader::BindTargetRef& nameTarget = targets()[index];
    if ( memcmp(&nameTarget, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0 )
        return false;

    const char* nameStringValue = (const char*)nameTarget.value(state);
    if ( strcmp(className, nameStringValue) != 0 )
        return false;

    // The name matched so now call the handler on all the classes for this name
    const Array<PrebuiltLoader::BindTargetRef> classes    = classTargets();
    const Array<PrebuiltLoader::BindTargetRef> duplicates = duplicateTargets();

    const PrebuiltLoader::BindTargetRef& classTarget = classes[index];
    if ( !classTarget.isAbsolute() ) {
        // A regular target points to the single class implementation
        // This class has a single implementation
        void* classImpl = (void*)classTarget.value(state);
        bool  stop      = false;
        callback(classImpl, true, &stop);
        return stop;
    }
    else {
        // This class has mulitple implementations.
        // The absolute value of the class target is the index in to the duplicates table
        // The first entry we point to is the count of duplicates for this class
        size_t                              duplicateStartIndex  = (size_t)classTarget.value(state);
        const PrebuiltLoader::BindTargetRef duplicateCountTarget = duplicates[duplicateStartIndex];
        ++duplicateStartIndex;
        assert(duplicateCountTarget.isAbsolute());
        uint64_t duplicateCount = duplicateCountTarget.value(state);

        for ( size_t dupeIndex = 0; dupeIndex != duplicateCount; ++dupeIndex ) {
            const PrebuiltLoader::BindTargetRef& duplicateTarget = duplicates[duplicateStartIndex + dupeIndex];

            void* classImpl = (void*)duplicateTarget.value(state);
            bool  stop      = false;
            callback(classImpl, true, &stop);
            if ( stop )
                return true;
        }
    }
    return false;
}
#endif

void ObjCClassOpt::forEachClass(RuntimeState& state,
                                void (^callback)(const PrebuiltLoader::BindTargetRef&        nameTarget,
                                                 const Array<PrebuiltLoader::BindTargetRef>& implTargets)) const
{

    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();

    Array<PrebuiltLoader::BindTargetRef> stringTargets = targets();
    Array<PrebuiltLoader::BindTargetRef> classes       = classTargets();
    Array<PrebuiltLoader::BindTargetRef> duplicates    = duplicateTargets();
    for ( unsigned i = 0; i != capacity; ++i ) {
        const PrebuiltLoader::BindTargetRef& nameTarget = stringTargets[i];
        if ( memcmp(&nameTarget, &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0 )
            continue;

        // Walk each class for this key
        PrebuiltLoader::BindTargetRef classTarget = classes[i];
        if ( !classTarget.isAbsolute() ) {
            // A regular target points to the single class implementation
            // This class has a single implementation
            const Array<PrebuiltLoader::BindTargetRef> implTarget(&classTarget, 1, 1);
            callback(nameTarget, implTarget);
        }
        else {
            // This class has mulitple implementations.
            // The absolute value of the class target is the index in to the duplicates table
            // The first entry we point to is the count of duplicates for this class
            uintptr_t                           duplicateStartIndex  = (uintptr_t)classTarget.absValue();
            const PrebuiltLoader::BindTargetRef duplicateCountTarget = duplicates[duplicateStartIndex];
            ++duplicateStartIndex;
            assert(duplicateCountTarget.isAbsolute());
            uintptr_t duplicateCount = (uintptr_t)duplicateCountTarget.absValue();

            callback(nameTarget, duplicates.subArray(duplicateStartIndex, duplicateCount));
        }
    }
}

size_t ObjCClassOpt::size(const objc::PerfectHash& phash, uint32_t numClassesWithDuplicates,
                          uint32_t totalDuplicates)
{
    size_t tableSize = 0;
    tableSize += ObjCStringTable::size(phash);
    tableSize += phash.capacity * sizeof(PrebuiltLoader::BindTargetRef);                               // classTargets
    tableSize += sizeof(uint32_t);                                                                     // duplicateCount
    tableSize += (numClassesWithDuplicates + totalDuplicates) * sizeof(PrebuiltLoader::BindTargetRef); // duplicateTargets
    return (size_t)align(tableSize, 3);
}

void ObjCClassOpt::write(const objc::PerfectHash& phash, const Array<StringToTargetMapNodeT>& strings,
                         const dyld3::CStringMultiMapTo<PrebuiltLoader::BindTarget>& classes,
                         uint32_t numClassesWithDuplicates, uint32_t totalDuplicates)
{
    ObjCStringTable::write(phash, strings);
    duplicateCount() = numClassesWithDuplicates + totalDuplicates;

    __block dyld3::Array<PrebuiltLoader::BindTargetRef> classTargets     = this->classTargets();
    __block dyld3::Array<PrebuiltLoader::BindTargetRef> duplicateTargets = this->duplicateTargets();

    const PrebuiltLoader::BindTargetRef sentinel = getSentinel();

    // Set class offsets to 0
    for ( uint32_t i = 0; i < capacity; i++ ) {
        classTargets[i] = sentinel;
    }

    // Empty the duplicate targets array so that we can push elements in to it.  It already has the correct capacity
    duplicateTargets.resize(0);

    classes.forEachEntry(^(const char* const& key, const PrebuiltLoader::BindTarget** values, uint64_t valuesCount) {
        uint32_t keyIndex = getIndex(key);
        assert(keyIndex != indexNotFound);
        assert(memcmp(&classTargets[keyIndex], &sentinel, sizeof(PrebuiltLoader::BindTargetRef)) == 0);

        if ( valuesCount == 1 ) {
            // Only one entry so write it in to the class offsets directly
            const PrebuiltLoader::BindTarget& classTarget = *(values[0]);
            classTargets[keyIndex]                        = PrebuiltLoader::BindTargetRef(classTarget);
            return;
        }

        // We have more than one value.  We add a placeholder to the class offsets which tells us the head
        // of the linked list of classes in the duplicates array

        PrebuiltLoader::BindTargetRef classTargetPlaceholder = PrebuiltLoader::BindTargetRef::makeAbsolute(duplicateTargets.count());
        classTargets[keyIndex]                               = classTargetPlaceholder;

        // The first value we push in to the duplicates array for this class is the count
        // of how many duplicates for this class we have
        duplicateTargets.push_back(PrebuiltLoader::BindTargetRef::makeAbsolute(valuesCount));
        for ( size_t i = 0; i != valuesCount; ++i ) {
            PrebuiltLoader::BindTarget classTarget = *(values[i]);
            duplicateTargets.push_back(PrebuiltLoader::BindTargetRef(classTarget));
        }
    });

    assert(duplicateTargets.count() == duplicateCount());
}

//////////////////////// ObjCOptimizerImage /////////////////////////////////

ObjCOptimizerImage::ObjCOptimizerImage(const JustInTimeLoader* jitLoader, uint64_t loadAddress, uint32_t pointerSize)
    : jitLoader(jitLoader)
    , pointerSize(pointerSize)
    , loadAddress(loadAddress)
{
}

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
void ObjCOptimizerImage::calculateMissingWeakImports(RuntimeState& state)
{
    const mach_o::MachOFileRef& mf = jitLoader->mf(state);

    // build targets table
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(bool, bindTargetsAreWeakImports, 512);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(bool, overrideBindTargetsAreWeakImports, 16);
    __block bool                           foundMissingWeakImport = false;
    bool                                   allowLazyBinds         = false;
    JustInTimeLoader::CacheWeakDefOverride cacheWeakDefFixup      = ^(uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const JustInTimeLoader::ResolvedSymbol& target) {};
    jitLoader->forEachBindTarget(diag, state, cacheWeakDefFixup, allowLazyBinds, ^(const JustInTimeLoader::ResolvedSymbol& target, bool& stop) {
        if ( (target.kind == Loader::ResolvedSymbol::Kind::bindAbsolute) && (target.targetRuntimeOffset == 0) ) {
            foundMissingWeakImport = true;
            bindTargetsAreWeakImports.push_back(true);
        }
        else {
            bindTargetsAreWeakImports.push_back(false);
        }
    }, ^(const JustInTimeLoader::ResolvedSymbol& target, bool& stop) {
        if ( (target.kind == Loader::ResolvedSymbol::Kind::bindAbsolute) && (target.targetRuntimeOffset == 0) ) {
            foundMissingWeakImport = true;
            overrideBindTargetsAreWeakImports.push_back(true);
        }
        else {
            overrideBindTargetsAreWeakImports.push_back(false);
        }
    });
    if ( diag.hasError() )
        return;

    if ( foundMissingWeakImport ) {
        jitLoader->withLayout(diag, state, ^(const mach_o::Layout& layout) {
            mach_o::Fixups fixups(layout);

            if ( mf->hasChainedFixups() ) {
                // walk all chains
                auto handler = ^(dyld3::MachOFile::ChainedFixupPointerOnDisk *fixupLocation,
                                 InputDylibVMAddress fixupVMAddr, uint16_t pointerFormat,
                                 bool &stopChain) {
                    uint32_t bindOrdinal;
                    int64_t  addend;
                    if ( fixupLocation->isBind(pointerFormat, bindOrdinal, addend) ) {
                        if ( bindOrdinal < bindTargetsAreWeakImports.count() ) {
                            if ( bindTargetsAreWeakImports[bindOrdinal] )
                                missingWeakImports.insert(fixupVMAddr);
                        }
                        else {
                            diag.error("out of range bind ordinal %d (max %lu)", bindOrdinal, bindTargetsAreWeakImports.count());
                            stopChain = true;
                        }
                    }
                };

                fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* startsInfo) {
                    fixups.forEachFixupChainSegment(diag, startsInfo, ^(const dyld_chained_starts_in_segment *segInfo,
                                                                        uint32_t segIndex, bool &stopSegment) {
                        InputDylibVMAddress segmentVMAddr(layout.segments[segIndex].vmAddr);
                        auto adaptor = ^(dyld3::MachOFile::ChainedFixupPointerOnDisk *fixupLocation,
                                         uint64_t fixupSegmentOffset,
                                         bool &stopChain) {
                            InputDylibVMAddress fixupVMAddr = segmentVMAddr + VMOffset(fixupSegmentOffset);
                            handler(fixupLocation, fixupVMAddr, segInfo->pointer_format, stopChain);
                        };
                        fixups.forEachFixupInSegmentChains(diag, segInfo, segIndex, true, adaptor);
                    });
                });
                if ( diag.hasError() )
                    return;
            } else if ( mf->hasOpcodeFixups() ) {
                // process all bind opcodes
                fixups.forEachBindLocation_Opcodes(diag, ^(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                           unsigned targetIndex, bool& fixupsStop) {
                    if ( targetIndex < bindTargetsAreWeakImports.count() ) {
                        if ( bindTargetsAreWeakImports[targetIndex] ) {
                            InputDylibVMAddress fixupVMAddr(layout.textUnslidVMAddr() + runtimeOffset);
                            missingWeakImports.insert(fixupVMAddr);
                        }
                    }
                    else {
                        diag.error("out of range bind ordinal %d (max %lu)", targetIndex, bindTargetsAreWeakImports.count());
                        fixupsStop = true;
                    }
                }, ^(uint64_t runtimeOffset, uint32_t segmentIndex,
                     unsigned overrideBindTargetIndex, bool& fixupsStop) {
                    if ( overrideBindTargetIndex < overrideBindTargetsAreWeakImports.count() ) {
                        if ( overrideBindTargetsAreWeakImports[overrideBindTargetIndex] ) {
                            InputDylibVMAddress fixupVMAddr(layout.textUnslidVMAddr() + runtimeOffset);
                            missingWeakImports.insert(fixupVMAddr);
                        }
                    }
                    else {
                        diag.error("out of range bind ordinal %d (max %lu)", overrideBindTargetIndex, overrideBindTargetsAreWeakImports.count());
                        fixupsStop = true;
                    }
                });
                if ( diag.hasError() )
                    return;
            }
            else {
                // process external relocations
                fixups.forEachBindLocation_Relocations(diag, ^(uint64_t runtimeOffset, unsigned targetIndex, bool& fixupsStop) {
                    if ( targetIndex < bindTargetsAreWeakImports.count() ) {
                        if ( bindTargetsAreWeakImports[targetIndex] ) {
                            InputDylibVMAddress fixupVMAddr(layout.textUnslidVMAddr() + runtimeOffset);
                            missingWeakImports.insert(fixupVMAddr);
                        }
                    }
                    else {
                        diag.error("out of range bind ordinal %d (max %lu)", targetIndex, bindTargetsAreWeakImports.count());
                        fixupsStop = true;
                    }
                });
                if ( diag.hasError() )
                    return;
            }
        });
    }
}
#endif // (BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL)

bool ObjCOptimizerImage::isNull(InputDylibVMAddress vmAddr, const void* address) const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
    return (missingWeakImports.find(vmAddr) != missingWeakImports.end());
#elif BUILDING_DYLD
    // In dyld, we are live, so we can just check if we point to a null value
    uintptr_t* pointer = (uintptr_t*)address;
    return (*pointer == 0);
#else
    // FIXME: Have we been slide or not in the non-dyld case?
    assert(0);
    return false;
#endif
}

void ObjCOptimizerImage::visitReferenceToObjCSelector(const objc::SelectorHashTable* objcSelOpt,
                                                      const PrebuiltObjC::SelectorMapTy& appSelectorMap,
                                                      VMOffset selectorReferenceRuntimeOffset, VMOffset selectorStringRuntimeOffset,
                                                      const char* selectorString)
{

    // fprintf(stderr, "selector: %p -> %p %s\n", (void*)selectorReferenceRuntimeOffset, (void*)selectorStringRuntimeOffset, selectorString);
    if ( std::optional<uint32_t> cacheSelectorIndex = objcSelOpt->tryGetIndex(selectorString) ) {
        // We got the selector from the cache so add a fixup to point there.
        // We use an absolute bind here, to reference the index in to the shared cache table
        PrebuiltLoader::BindTargetRef bindTarget = PrebuiltLoader::BindTargetRef::makeAbsolute(*cacheSelectorIndex);

        //printf("Overriding fixup at 0x%08llX to cache offset 0x%08llX\n", selectorUseImageOffset, (uint64_t)objcSelOpt->getEntryForIndex(cacheSelectorIndex) - (uint64_t)state.config.dyldCache());
        selectorFixups.push_back(bindTarget);
        return;
    }

    // See if this selector is already in the app map from a previous image
    auto appSelectorIt = appSelectorMap.find(selectorString);
    if ( appSelectorIt != appSelectorMap.end() ) {
        // This selector was found in a previous image, so use it here.

        //printf("Overriding fixup at 0x%08llX to other image\n", selectorUseImageOffset);
        selectorFixups.push_back(PrebuiltLoader::BindTargetRef(appSelectorIt->second));
        return;
    }

    // See if this selector is already in the map for this image
    auto itAndInserted = selectorMap.insert({ selectorString, Loader::BindTarget() });
    if ( itAndInserted.second ) {
        // We added the selector so its pointing in to our own image.
        Loader::BindTarget target;
        target.loader               = jitLoader;
        target.runtimeOffset        = selectorStringRuntimeOffset.rawValue();
        itAndInserted.first->second = target;

        // We'll add a fixup anyway as we want a sel ref fixup for every entry in the sel refs section

        //printf("Fixup at 0x%08llX to '%s' offset 0x%08llX\n", selectorUseImageOffset, findLoadedImage(target.image.imageNum).path(), target.image.offset);
        selectorFixups.push_back(PrebuiltLoader::BindTargetRef(target));
        return;
    }

    // This selector was found elsewhere in our image.  As we want a fixup for every selref, we'll
    // add one here too
    Loader::BindTarget& target = itAndInserted.first->second;

    //printf("Overriding fixup at 0x%08llX to '%s' offset 0x%08llX\n", selectorUseImageOffset, findLoadedImage(target.image.imageNum).path(), target.image.offset);
    selectorFixups.push_back(PrebuiltLoader::BindTargetRef(target));
}

// Check if the given class is in an image loaded in the shared cache.
// If so, add the class to the duplicate map
static void checkForDuplicateClass(const VMAddress dyldCacheBaseAddress,
                                   const char* className, const objc::ClassHashTable* objcClassOpt,
                                   const PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                   const PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClasses,
                                   ObjCOptimizerImage& image)
{
    objcClassOpt->forEachClass(className,
                               ^(uint64_t classCacheOffset, uint16_t dylibObjCIndex, bool &stopObjects) {
        // Check if this image is loaded
        if ( auto cacheIt = sharedCacheImagesMap.find(dylibObjCIndex); cacheIt != sharedCacheImagesMap.end() ) {
            const Loader* ldr = cacheIt->second.second;

            // We have a duplicate class, so check if we've already got it in our map.
            if ( duplicateSharedCacheClasses.find(className) == duplicateSharedCacheClasses.end() ) {
                // We haven't seen this one yet, so record it in the map for this image
                VMAddress cacheDylibUnslidVMAddr = cacheIt->second.first;
                VMAddress          classVMAddr = dyldCacheBaseAddress + VMOffset(classCacheOffset);
                VMOffset           classDylibVMOffset = classVMAddr - cacheDylibUnslidVMAddr;
                Loader::BindTarget classTarget   = { ldr, classDylibVMOffset.rawValue() };
                image.duplicateSharedCacheClassMap.insert({ className, classTarget });
            }

            stopObjects = true;
        }
    });
}

void ObjCOptimizerImage::visitClass(const VMAddress dyldCacheBaseAddress,
                                    const objc::ClassHashTable* objcClassOpt,
                                    const SharedCacheImagesMapTy& sharedCacheImagesMap,
                                    const DuplicateClassesMapTy& duplicateSharedCacheClasses,
                                    InputDylibVMAddress classVMAddr, InputDylibVMAddress classNameVMAddr, const char* className)
{

    // If the class also exists in a shared cache image which is loaded, then objc
    // would have found that one, regardless of load order.
    // In that case, we still add this class to the map, but also track which shared cache class it is a duplicate of
    checkForDuplicateClass(dyldCacheBaseAddress, className, objcClassOpt, sharedCacheImagesMap,
                           duplicateSharedCacheClasses, *this);

    VMOffset classNameVMOffset   = classNameVMAddr - loadAddress;
    VMOffset classObjectVMOffset = classVMAddr - loadAddress;
    classLocations.push_back({ className, classNameVMOffset, classObjectVMOffset });
}

static bool protocolIsInSharedCache(const char* protocolName,
                                    const objc::ProtocolHashTable* objcProtocolOpt,
                                    const PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap)
{
    __block bool foundProtocol = false;
    objcProtocolOpt->forEachProtocol(protocolName,
                                     ^(uint64_t classCacheOffset, uint16_t dylibObjCIndex, bool &stopObjects) {
        // Check if this image is loaded
        if ( auto cacheIt = sharedCacheImagesMap.find(dylibObjCIndex); cacheIt != sharedCacheImagesMap.end() ) {
            foundProtocol = true;
            stopObjects = true;
        }
    });
    return foundProtocol;
}

void ObjCOptimizerImage::visitProtocol(const objc::ProtocolHashTable* objcProtocolOpt,
                                       const SharedCacheImagesMapTy& sharedCacheImagesMap,
                                       InputDylibVMAddress protocolVMAddr, InputDylibVMAddress protocolNameVMAddr,
                                       const char* protocolName)
{

    uint32_t protocolIndex = (uint32_t)protocolISAFixups.count();
    protocolISAFixups.push_back(false);

    // If the protocol also exists in a shared cache image which is loaded, then objc
    // would have found that one, regardless of load order.  So we can just skip this one.
    if ( protocolIsInSharedCache(protocolName, objcProtocolOpt, sharedCacheImagesMap) )
        return;

    VMOffset protocolNameVMOffset   = protocolNameVMAddr - loadAddress;
    VMOffset protocolObjectVMOffset = protocolVMAddr - loadAddress;
    protocolLocations.push_back({ protocolName, protocolNameVMOffset, protocolObjectVMOffset });

    // Record which index this protocol uses in protocolISAFixups.  Later we can change its entry if we
    // choose this protocol as the canonical definition.
    protocolIndexMap[protocolObjectVMOffset] = protocolIndex;
}

//////////////////////// ObjC Optimisations /////////////////////////////////

// HACK!: dyld3 used to know if each image in a closure has been rebased or not when it was building the closure
// Now we try to make good guesses based on whether its the shared cache or not, and which binary is executing this code
#if 0
static bool hasBeenRebased(const Loader* ldr)
{
#if BUILDING_DYLD
    // In dyld, we always run this analysis after everything has already been fixed up
    return true;
#elif BUILDING_CLOSURE_UTIL
    // dyld_closure_util assumes that on disk binaries haven't had fixups applied
    return false;
#else
    // In the shared cache builder, nothing has been rebased yet
    return false;
#endif
}
#endif

static objc_visitor::Visitor makeObjCVisitor(Diagnostics& diag, RuntimeState& state,
                                             const Loader* ldr)
{

#if POINTERS_ARE_UNSLID
    const dyld3::MachOAnalyzer* dylibMA = ldr->analyzer(state);

    objc_visitor::Visitor objcVisitor(state.config.dyldCache.addr, dylibMA);
    return objcVisitor;
#elif SUPPORT_VM_LAYOUT
    const dyld3::MachOAnalyzer* dylibMA = ldr->analyzer(state);

    objc_visitor::Visitor objcVisitor(dylibMA);
    return objcVisitor;
#else
    const dyld3::MachOFile* dylibMF = ldr->mf(state);
    VMAddress dylibBaseAddress(dylibMF->preferredLoadAddress());

    __block std::vector<metadata_visitor::Segment> segments;
    __block std::vector<uint64_t> bindTargets;
    ldr->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        for ( uint32_t segIndex = 0; segIndex != layout.segments.size(); ++segIndex ) {
            const auto& layoutSegment = layout.segments[segIndex];
            metadata_visitor::Segment segment {
                .startVMAddr = VMAddress(layoutSegment.vmAddr),
                .endVMAddr = VMAddress(layoutSegment.vmAddr + layoutSegment.vmSize),
                .bufferStart = (uint8_t*)layoutSegment.buffer,
                .onDiskDylibChainedPointerFormat = 0,
                .segIndex = segIndex
            };
            segments.push_back(std::move(segment));
        }

        // Add chained fixup info to each segment, if we have it
        if ( dylibMF->hasChainedFixups() ) {
            mach_o::Fixups fixups(layout);
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                mach_o::Fixups::forEachFixupChainSegment(diag, starts,
                                                         ^(const dyld_chained_starts_in_segment *segInfo, uint32_t segIndex, bool &stop) {
                    segments[segIndex].onDiskDylibChainedPointerFormat = segInfo->pointer_format;
                });
            });
        }

        // ObjC patching needs the bind targets for interposable references to the classes
        // build targets table
        if ( dylibMF->hasChainedFixupsLoadCommand() ) {
            mach_o::Fixups fixups(layout);
            fixups.forEachBindTarget_ChainedFixups(diag, ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                if ( info.libOrdinal != BIND_SPECIAL_DYLIB_SELF ) {
                    bindTargets.push_back(0);
                    return;
                }

                mach_o::Layout::FoundSymbol foundInfo;
                if ( !layout.findExportedSymbol(diag, info.symbolName, info.weakImport, foundInfo) ) {
                    bindTargets.push_back(0);
                    return;
                }

                // We only support header offsets in this dylib, as we are looking for self binds
                // which are likely only to classes
                if ( (foundInfo.kind != mach_o::Layout::FoundSymbol::Kind::headerOffset)
                    || (foundInfo.foundInDylib.value() != dylibMF) ) {
                    bindTargets.push_back(0);
                    return;
                }

                uint64_t vmAddr = layout.textUnslidVMAddr() + foundInfo.value;
                bindTargets.push_back(vmAddr);
            });
        }
    });

    std::optional<VMAddress> selectorStringsBaseAddress;
    objc_visitor::Visitor objcVisitor(dylibBaseAddress, dylibMF,
                                      std::move(segments), selectorStringsBaseAddress,
                                      std::move(bindTargets));
    return objcVisitor;
#endif
}

static void optimizeObjCSelectors(RuntimeState& state,
                                  const objc::SelectorHashTable* objcSelOpt,
                                  const PrebuiltObjC::SelectorMapTy& appSelectorMap,
                                  ObjCOptimizerImage&                image)
{

    const mach_o::MachOFileRef mf       = image.jitLoader->mf(state);
    uint32_t                pointerSize = mf->pointerSize();

    // The legacy (objc1) codebase uses a bunch of sections we don't want to reason about.  If we see them just give up.
    __block bool foundBadSection = false;
    mf->forEachSection(^(const MachOAnalyzer::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( strcmp(sectInfo.segInfo.segName, "__OBJC") != 0 )
            return;
        if ( strcmp(sectInfo.sectName, "__module_info") == 0 ) {
            foundBadSection = true;
            stop            = true;
            return;
        }
        if ( strcmp(sectInfo.sectName, "__protocol") == 0 ) {
            foundBadSection = true;
            stop            = true;
            return;
        }
        if ( strcmp(sectInfo.sectName, "__message_refs") == 0 ) {
            foundBadSection = true;
            stop            = true;
            return;
        }
    });
    if ( foundBadSection ) {
        image.diag.error("Old objc section");
        return;
    }

    // Visit the message refs
    // Note this isn't actually supported in libobjc any more.  Its logic for deciding whether to support it is if this is true:
    // #if (defined(__x86_64__) && (TARGET_OS_OSX || TARGET_OS_SIMULATOR))
    // So to keep it simple, lets only do this walk if we are x86_64
    if ( mf->isArch("x86_64") || mf->isArch("x86_64h") ) {
        if ( mf->hasObjCMessageReferences() ) {
            image.diag.error("Cannot handle message refs");
            return;
        }
    }

    // FIXME: Don't make a duplicate one of these if we can pass one in instead
    __block objc_visitor::Visitor objcVisitor = makeObjCVisitor(image.diag, state, image.jitLoader);
    if ( image.diag.hasError() )
        return;

    // We only record selector references for __objc_selrefs and pointer based method lists.
    // If we find a relative method list pointing outside of __objc_selrefs then we give up for now
    uint64_t selRefsStartRuntimeOffset = image.binaryInfo.selRefsRuntimeOffset;
    uint64_t selRefsEndRuntimeOffset   = selRefsStartRuntimeOffset + (pointerSize * image.binaryInfo.selRefsCount);
    auto     visitRelativeMethod = ^(const objc_visitor::Method& method, bool& stop) {
        VMAddress selectorRefVMAddress = method.getNameSelRefVMAddr(objcVisitor);
        VMOffset selectorReferenceRuntimeOffset = selectorRefVMAddress - VMAddress(image.loadAddress.rawValue());
        if ( (selectorReferenceRuntimeOffset.rawValue() < selRefsStartRuntimeOffset)
            || (selectorReferenceRuntimeOffset.rawValue() >= selRefsEndRuntimeOffset) ) {
            image.diag.error("Cannot handle relative method list pointing outside of __objc_selrefs");
            stop = true;
        }
    };

    auto visitMethodList = ^(const objc_visitor::MethodList& methodList,
                             bool& hasPointerBasedMethodList, bool &stop) {
        if ( methodList.numMethods() == 0 )
            return;

        if ( methodList.usesRelativeOffsets() ) {
            // Check relative method lists
            uint32_t numMethods = methodList.numMethods();
            for ( uint32_t i = 0; i != numMethods; ++i ) {
                const objc_visitor::Method& method = methodList.getMethod(objcVisitor, i);
                visitRelativeMethod(method, stop);
            }
        } else {
            // Record if we found a pointer based method list.  This lets us skip walking method lists later if
            // they are all relative method lists
            hasPointerBasedMethodList = true;
        }
    };

    if ( image.binaryInfo.classListCount != 0 ) {
        __block bool hasPointerBasedMethodList = false;
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class &objcClass, bool &stopClass) {
            objc_visitor::MethodList methodList = objcClass.getBaseMethods(objcVisitor);
            visitMethodList(methodList, hasPointerBasedMethodList, stopClass);
        });
        image.binaryInfo.hasClassMethodListsToUnique     = hasPointerBasedMethodList;
        image.binaryInfo.hasClassMethodListsToSetUniqued = hasPointerBasedMethodList;
    }

    if ( image.binaryInfo.categoryCount != 0 ) {
        __block bool hasPointerBasedMethodList = false;
        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool &stopCategory) {
            objc_visitor::MethodList instanceMethodList = objcCategory.getInstanceMethods(objcVisitor);
            objc_visitor::MethodList classMethodList    = objcCategory.getClassMethods(objcVisitor);

            visitMethodList(instanceMethodList, hasPointerBasedMethodList, stopCategory);
            if ( stopCategory )
                return;

            visitMethodList(classMethodList, hasPointerBasedMethodList, stopCategory);
        });
        image.binaryInfo.hasClassMethodListsToUnique     = hasPointerBasedMethodList;
        image.binaryInfo.hasClassMethodListsToSetUniqued = hasPointerBasedMethodList;
    }

    if ( image.binaryInfo.protocolListCount != 0 ) {
        __block bool hasPointerBasedMethodList = false;
        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
            objc_visitor::MethodList instanceMethodList         = objcProtocol.getInstanceMethods(objcVisitor);
            objc_visitor::MethodList classMethodList            = objcProtocol.getClassMethods(objcVisitor);
            objc_visitor::MethodList optionalInstanceMethodList = objcProtocol.getOptionalInstanceMethods(objcVisitor);
            objc_visitor::MethodList optionalClassMethodList    = objcProtocol.getOptionalClassMethods(objcVisitor);

            visitMethodList(instanceMethodList, hasPointerBasedMethodList, stopProtocol);
            if ( stopProtocol )
                return;

            visitMethodList(classMethodList, hasPointerBasedMethodList, stopProtocol);
            if ( stopProtocol )
                return;

            visitMethodList(optionalInstanceMethodList, hasPointerBasedMethodList, stopProtocol);
            if ( stopProtocol )
                return;

            visitMethodList(optionalClassMethodList, hasPointerBasedMethodList, stopProtocol);
        });
        image.binaryInfo.hasClassMethodListsToUnique     = hasPointerBasedMethodList;
        image.binaryInfo.hasClassMethodListsToSetUniqued = hasPointerBasedMethodList;
    }

    auto visitSelRef = ^(uint64_t selectorReferenceRuntimeOffset, uint64_t selectorStringRuntimeOffset,
                         const char* selectorString) {
        // Note we don't check if the string is printable.  We already checked earlier that this image doesn't have
        // Fairplay or protected segments, which would prevent seeing the strings.
        image.visitReferenceToObjCSelector(objcSelOpt, appSelectorMap,
                                           VMOffset(selectorReferenceRuntimeOffset),
                                           VMOffset(selectorStringRuntimeOffset), selectorString);
    };

    PrebuiltObjC::forEachSelectorReferenceToUnique(state, image.jitLoader, image.loadAddress.rawValue(), image.binaryInfo, visitSelRef);
}

static void optimizeObjCClasses(RuntimeState& state,
                                const objc::ClassHashTable* objcClassOpt,
                                const PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                const PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClasses,
                                ObjCOptimizerImage& image)
{
    if ( image.binaryInfo.classListCount == 0 )
        return;

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
    image.calculateMissingWeakImports(state);
    if ( image.diag.hasError() )
        return;
#endif

    // FIXME: Don't make a duplicate one of these if we can pass one in instead
    __block objc_visitor::Visitor objcVisitor = makeObjCVisitor(image.diag, state, image.jitLoader);
    if ( image.diag.hasError() )
        return;

    VMAddress dyldCacheBaseAddress(state.config.dyldCache.unslidLoadAddress);

    // Note we skip metaclasses
    objcVisitor.forEachClass(^(const objc_visitor::Class& objcClass, bool &stopClass) {
        // Make sure the superclass pointer is not nil.  Unless we are a root class as those don't have a superclass
        if ( !objcClass.isRootClass(objcVisitor) ) {
            metadata_visitor::ResolvedValue classSuperclassField = objcClass.getSuperclassField(objcVisitor);
            InputDylibVMAddress superclassFieldVMAddr(classSuperclassField.vmAddress().rawValue());
            if ( image.isNull(superclassFieldVMAddr, classSuperclassField.value()) ) {
                const char* className = objcClass.getName(objcVisitor);
                image.diag.error("Missing weak superclass of class %s in %s", className, image.jitLoader->path());
                return;
            }
        }

        // Does this class need to be fixed up for stable Swift ABI.
        // Note the order matches the objc runtime in that we always do this fix before checking for dupes,
        // but after excluding classes with missing weak superclasses.
        if ( objcClass.isUnfixedBackwardDeployingStableSwift(objcVisitor) ) {
            // Class really is stable Swift, pretending to be pre-stable.
            image.binaryInfo.hasClassStableSwiftFixups = true;
        }

        VMAddress classVMAddr = objcClass.getVMAddress();
        VMAddress classNameVMAddr = objcClass.getNameVMAddr(objcVisitor);
        // Note we don't check if the string is printable.  We already checked earlier that this image doesn't have
        // Fairplay or protected segments, which would prevent seeing the strings.
        const char* className = objcClass.getName(objcVisitor);
        image.visitClass(dyldCacheBaseAddress, objcClassOpt, sharedCacheImagesMap, duplicateSharedCacheClasses,
                         InputDylibVMAddress(classVMAddr.rawValue()), InputDylibVMAddress(classNameVMAddr.rawValue()),
                         className);
    });
}

static void optimizeObjCProtocols(RuntimeState& state,
                                  const objc::ProtocolHashTable* objcProtocolOpt,
                                  const PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                  ObjCOptimizerImage& image)
{
    if ( image.binaryInfo.protocolListCount == 0 )
        return;

    image.protocolISAFixups.reserve(image.binaryInfo.protocolListCount);

    // FIXME: Don't make a duplicate one of these if we can pass one in instead
    __block objc_visitor::Visitor objcVisitor = makeObjCVisitor(image.diag, state, image.jitLoader);
    if ( image.diag.hasError() )
        return;

    objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
        std::optional<VMAddress> isaVMAddr = objcProtocol.getISAVMAddr(objcVisitor);
        if ( isaVMAddr.has_value() ) {
            // We can't optimize this protocol if it has an ISA as we want to override it
            image.diag.error("Protocol ISA must be null");
            stopProtocol = true;
            return;
        }

        VMAddress protocolVMAddr = objcProtocol.getVMAddress();
        VMAddress protocolNameVMAddr = objcProtocol.getNameVMAddr(objcVisitor);
        // Note we don't check if the string is printable.  We already checked earlier that this image doesn't have
        // Fairplay or protected segments, which would prevent seeing the strings.
        const char* protocolName = objcProtocol.getName(objcVisitor);

        image.visitProtocol(objcProtocolOpt, sharedCacheImagesMap, InputDylibVMAddress(protocolVMAddr.rawValue()),
                            InputDylibVMAddress(protocolNameVMAddr.rawValue()), protocolName);
    });
}

static void
writeClassOrProtocolHashTable(RuntimeState& state, bool classes,
                              Array<ObjCOptimizerImage>& objcImages,
                              OverflowSafeArray<uint8_t>& hashTable,
                              const PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClassMap,
                              PrebuiltObjC::ClassMapTy& seenObjectsMap)
{

    dyld3::CStringMapTo<PrebuiltLoader::BindTarget>      objectNameMap;
    OverflowSafeArray<const char*>                       objectNames;

    // Note we walk the images backwards as we want them in load order to match the order they are registered with objc
    for ( size_t imageIndex = 0, reverseIndex = (objcImages.count() - 1); imageIndex != objcImages.count(); ++imageIndex, --reverseIndex ) {
        if ( objcImages[reverseIndex].diag.hasError() )
            continue;
        ObjCOptimizerImage& image = objcImages[reverseIndex];

        const OverflowSafeArray<ObjCOptimizerImage::ObjCObject>& objectLocations = classes ? image.classLocations : image.protocolLocations;

        for ( const ObjCOptimizerImage::ObjCObject& objectLocation : objectLocations ) {
            //uint64_t nameVMAddr     = ma->preferredLoadAddress() + classImage.offsetOfClassNames + classNameTarget.classNameImageOffset;
            //printf("%s: 0x%08llx = '%s'\n", li.path(), nameVMAddr, className);

            // Also track the name
            PrebuiltLoader::BindTarget nameTarget    = { image.jitLoader, objectLocation.nameRuntimeOffset.rawValue() };
            auto                       itAndInserted = objectNameMap.insert({ objectLocation.name, nameTarget });
            if ( itAndInserted.second ) {
                // We inserted the class name so we need to add it to the strings for the closure hash table
                objectNames.push_back(objectLocation.name);

                // If we are processing protocols, and this is the first one we've seen, then track its ISA to be fixed up
                if ( !classes ) {
                    auto protocolIndexIt = image.protocolIndexMap.find(objectLocation.valueRuntimeOffset);
                    assert(protocolIndexIt != image.protocolIndexMap.end());
                    image.protocolISAFixups[protocolIndexIt->second] = true;
                }

                // Check if we have a duplicate.  If we do, it will be on the last image which had a duplicate class name,
                // but as we walk images backwards, we'll see this before all other images with duplicates.
                // Note we only check for duplicates when we know we just inserted the object name in to the map, as this
                // ensure's that we only insert each duplicate once
                if ( classes ) {
                    auto duplicateClassIt = duplicateSharedCacheClassMap.find(objectLocation.name);
                    if ( duplicateClassIt != duplicateSharedCacheClassMap.end() ) {
                        seenObjectsMap.insert({ objectLocation.name, duplicateClassIt->second });
                    }
                }
            }

            PrebuiltLoader::BindTarget valueTarget = { image.jitLoader, objectLocation.valueRuntimeOffset.rawValue() };
            seenObjectsMap.insert({ objectLocation.name, valueTarget });
        }
    }

    __block uint32_t numClassesWithDuplicates = 0;
    __block uint32_t totalDuplicates          = 0;
    seenObjectsMap.forEachEntry(^(const char* const& key, const PrebuiltLoader::BindTarget** values,
                                  uint64_t valuesCount) {
        if ( valuesCount != 1 ) {
            ++numClassesWithDuplicates;
            totalDuplicates += valuesCount;
        }
    });

    // If we have closure class names, we need to make a hash table for them.
    if ( !objectNames.empty() ) {
        objc::PerfectHash phash;
        objc::PerfectHash::make_perfect(objectNames, phash);
        size_t size = ObjCClassOpt::size(phash, numClassesWithDuplicates, totalDuplicates);
        hashTable.resize(size);
        //printf("Class table size: %lld\n", size);
        ObjCClassOpt* resultHashTable = (ObjCClassOpt*)hashTable.begin();
        resultHashTable->write(phash, objectNameMap.array(), seenObjectsMap,
                               numClassesWithDuplicates, totalDuplicates);
    }
}

//////////////////////// PrebuiltObjC /////////////////////////////////

void PrebuiltObjC::commitImage(const ObjCOptimizerImage& image)
{
    // As this image is still valid, then add its intermediate results to the main tables
    for ( const auto& stringAndDuplicate : image.duplicateSharedCacheClassMap ) {
        // Note we want to overwrite any existing entries here.  We want the last seen
        // class with a duplicate to be in the map as writeClassOrProtocolHashTable walks the images
        // from back to front.
        duplicateSharedCacheClassMap[stringAndDuplicate.first] = stringAndDuplicate.second;
    }

    // Selector results
    // Note we don't need to add the selector binds here.  Its easier just to process them later from each image
    for ( const auto& stringAndTarget : image.selectorMap ) {
        closureSelectorMap[stringAndTarget.first] = stringAndTarget.second;
        closureSelectorStrings.push_back(stringAndTarget.first);
    }
}

void PrebuiltObjC::generateHashTables(RuntimeState& state)
{
    // Write out the class table
    writeClassOrProtocolHashTable(state, true, objcImages, classesHashTable, duplicateSharedCacheClassMap, classMap);

    // Write out the protocol table
    writeClassOrProtocolHashTable(state, false, objcImages, protocolsHashTable, duplicateSharedCacheClassMap, protocolMap);

    // If we have closure selectors, we need to make a hash table for them.
    if ( !closureSelectorStrings.empty() ) {
        objc::PerfectHash phash;
        objc::PerfectHash::make_perfect(closureSelectorStrings, phash);
        size_t size = ObjCStringTable::size(phash);
        selectorsHashTable.resize(size);
        //printf("Selector table size: %lld\n", size);
        selectorStringTable = (ObjCStringTable*)selectorsHashTable.begin();
        selectorStringTable->write(phash, closureSelectorMap.array());
    }
}

void PrebuiltObjC::generatePerImageFixups(RuntimeState& state, uint32_t pointerSize)
{
    // Find the largest JIT loader index so that we know how many images we might serialize
    uint16_t largestLoaderIndex = 0;
    for ( const Loader* l : state.loaded ) {
        if ( !l->isPrebuilt ) {
            JustInTimeLoader* jl = (JustInTimeLoader*)l;
            assert(jl->ref.app);
            largestLoaderIndex = std::max(largestLoaderIndex, jl->ref.index);
        }
    }
    ++largestLoaderIndex;

    imageFixups.reserve(largestLoaderIndex);
    for ( uint16_t i = 0; i != largestLoaderIndex; ++i ) {
        imageFixups.default_constuct_back();
    }

    // Add per-image fixups
    for ( ObjCOptimizerImage& image : objcImages ) {
        if ( image.diag.hasError() )
            continue;

        ObjCImageFixups& fixups = imageFixups[image.jitLoader->ref.index];

        // Copy all the binary info for use later when applying fixups
        fixups.binaryInfo = image.binaryInfo;

        // Protocol ISA references
        // These are a single boolean value for each protocol to identify if it is canonical or not
        // We convert from bool to uint8_t as that seems better for saving to disk.
        if ( !image.protocolISAFixups.empty() ) {
            fixups.protocolISAFixups.reserve(image.protocolISAFixups.count());
            for ( bool isCanonical : image.protocolISAFixups )
                fixups.protocolISAFixups.push_back(isCanonical ? 1 : 0);
        }

        // Selector references.
        // These are a BindTargetRef for every selector reference to fixup
        if ( !image.selectorFixups.empty() ) {
            fixups.selectorReferenceFixups.reserve(image.selectorFixups.count());
            for ( const PrebuiltLoader::BindTargetRef& target : image.selectorFixups ) {
                fixups.selectorReferenceFixups.push_back(target);
            }
        }
    }
}

// Visits each selector reference once, in order.  Note the order this visits selector references has to
// match for serializing/deserializing the PrebuiltLoader.
void PrebuiltObjC::forEachSelectorReferenceToUnique(RuntimeState&           state,
                                                    const Loader*           ldr,
                                                    uint64_t                loadAddress,
                                                    const ObjCBinaryInfo&   binaryInfo,
                                                    void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                                     uint64_t selectorStringRuntimeOffset,
                                                                     const char* selectorString))

{
    // FIXME: Don't make a duplicate one of these if we can pass one in instead
    Diagnostics diag;
    __block objc_visitor::Visitor objcVisitor = makeObjCVisitor(diag, state, ldr);
    assert(!diag.hasError());

    if ( binaryInfo.selRefsCount != 0 ) {
        objcVisitor.forEachSelectorReference(^(VMAddress selRefVMAddr, VMAddress selRefTargetVMAddr,
                                               const char* selectorString) {
            VMOffset selectorReferenceRuntimeOffset = selRefVMAddr - VMAddress(loadAddress);
            VMOffset selectorStringRuntimeOffset    = selRefTargetVMAddr - VMAddress(loadAddress);
            callback(selectorReferenceRuntimeOffset.rawValue(), selectorStringRuntimeOffset.rawValue(),
                     selectorString);
        });
    }

    // We only make the callback for method list selrefs which are not already covered by the __objc_selrefs section.
    // For pointer based method lists, this is all sel ref pointers.
    // For relative method lists, we should always point to the __objc_selrefs section.  This was checked earlier, so
    // we skip this callback on relative method lists as we know here they must point to the (already uniqied) __objc_selrefs.
    auto visitPointerBasedMethod = ^(const objc_visitor::Method& method) {
        VMAddress nameVMAddr = method.getNameVMAddr(objcVisitor);
        VMAddress nameLocationVMAddr = method.getNameField(objcVisitor).vmAddress();
        const char* selectorString = method.getName(objcVisitor);

        VMOffset selectorStringRuntimeOffset    = nameVMAddr - VMAddress(loadAddress);
        VMOffset selectorReferenceRuntimeOffset = nameLocationVMAddr - VMAddress(loadAddress);
        callback(selectorReferenceRuntimeOffset.rawValue(), selectorStringRuntimeOffset.rawValue(), selectorString);
    };

    auto visitMethodList = ^(const objc_visitor::MethodList& methodList) {
        if ( methodList.numMethods() == 0 )
            return;
        if ( methodList.usesRelativeOffsets() )
            return;

        // Check pointer based method lists
        uint32_t numMethods = methodList.numMethods();
        for ( uint32_t i = 0; i != numMethods; ++i ) {
            const objc_visitor::Method& method = methodList.getMethod(objcVisitor, i);
            visitPointerBasedMethod(method);
        }
    };

    if ( binaryInfo.hasClassMethodListsToUnique && (binaryInfo.classListCount != 0) ) {
        // FIXME: Use binaryInfo.classListRuntimeOffset and binaryInfo.classListCount
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class &objcClass, bool &stopClass) {
            objc_visitor::MethodList methodList = objcClass.getBaseMethods(objcVisitor);
            visitMethodList(methodList);
        });
    }

    if ( binaryInfo.hasCategoryMethodListsToUnique && (binaryInfo.categoryCount != 0) ) {
        // FIXME: Use binaryInfo.categoryListRuntimeOffset and binaryInfo.categoryCount
        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool &stopCategory) {
            objc_visitor::MethodList instanceMethodList = objcCategory.getInstanceMethods(objcVisitor);
            objc_visitor::MethodList classMethodList    = objcCategory.getClassMethods(objcVisitor);

            visitMethodList(instanceMethodList);
            visitMethodList(classMethodList);
        });
    }

    if ( binaryInfo.hasProtocolMethodListsToUnique && (binaryInfo.protocolListCount != 0) ) {
        // FIXME: Use binaryInfo.protocolListRuntimeOffset and binaryInfo.protocolListCount
        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
            objc_visitor::MethodList instanceMethodList         = objcProtocol.getInstanceMethods(objcVisitor);
            objc_visitor::MethodList classMethodList            = objcProtocol.getClassMethods(objcVisitor);
            objc_visitor::MethodList optionalInstanceMethodList = objcProtocol.getOptionalInstanceMethods(objcVisitor);
            objc_visitor::MethodList optionalClassMethodList    = objcProtocol.getOptionalClassMethods(objcVisitor);

            visitMethodList(instanceMethodList);
            visitMethodList(classMethodList);
            visitMethodList(optionalInstanceMethodList);
            visitMethodList(optionalClassMethodList);
        });
    }
}

static std::optional<VMOffset> getImageInfo(Diagnostics& diag, RuntimeState& state,
                                            const Loader* ldr, const mach_o::MachOFileRef& mf)
{
    __block std::optional<VMOffset> objcImageInfoRuntimeOffset;
    mf->forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& sectionInfo, bool malformedSectionRange, bool& stop) {
        if ( strncmp(sectionInfo.segInfo.segName, "__DATA", 6) != 0 )
            return;
        if (strcmp(sectionInfo.sectName, "__objc_imageinfo") != 0)
            return;
        if ( malformedSectionRange ) {
            stop = true;
            return;
        }
        if ( sectionInfo.sectSize != 8 ) {
            stop = true;
            return;
        }

        // We can't just access the image info directly from the MachOFile.  Instead we have to
        // use the layout to find the actual location of the segment, as we might be in the cache builder
        ldr->withLayout(diag, state, ^(const mach_o::Layout& layout) {
            const mach_o::SegmentLayout& segment = layout.segments[sectionInfo.segInfo.segIndex];
            uint64_t offsetInSegment = sectionInfo.sectAddr - segment.vmAddr;
            const auto* imageInfo = (MachOAnalyzer::ObjCImageInfo*)(segment.buffer + offsetInSegment);

            if ( (imageInfo->flags & MachOAnalyzer::ObjCImageInfo::dyldPreoptimized) != 0 )
                return;

            objcImageInfoRuntimeOffset = VMOffset(sectionInfo.sectAddr - layout.textUnslidVMAddr());
        });
        stop = true;
    });

    return objcImageInfoRuntimeOffset;
}

static std::optional<VMOffset> getProtocolClassCacheOffset(RuntimeState& state)
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
        assert(state.config.dyldCache.objcProtocolClassCacheOffset != 0);
        return VMOffset(state.config.dyldCache.objcProtocolClassCacheOffset);
#else
        // Make sure we have the pointers section with the pointer to the protocol class
        const void* objcOptPtrs = state.config.dyldCache.addr->objcOptPtrs();
        if ( objcOptPtrs == nullptr )
            return { };

        uint32_t pointerSize = state.mainExecutableLoader->loadAddress(state)->pointerSize();
        uint64_t classProtocolVMAddr = (pointerSize == 8) ? *(uint64_t*)objcOptPtrs : *(uint32_t*)objcOptPtrs;

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
        // As we are running in dyld/tests, the cache is live

#if __has_feature(ptrauth_calls)
        // If we are on arm64e, the protocol ISA in the shared cache was signed.  We don't
        // want the signature bits in the encoded value
        classProtocolVMAddr = (uint64_t)__builtin_ptrauth_strip((void*)classProtocolVMAddr, ptrauth_key_asda);
#endif // __has_feature(ptrauth_calls)

        return VMOffset(classProtocolVMAddr - (uint64_t)state.config.dyldCache.addr);
#elif BUILDING_CLOSURE_UTIL
        // FIXME: This assumes an on-disk cache
        classProtocolVMAddr          = state.config.dyldCache.addr->makeVMAddrConverter(false).convertToVMAddr(classProtocolVMAddr);
        return VMOffset(classProtocolVMAddr - state.config.dyldCache.addr->unslidLoadAddress());
#else
        // Running offline so the cache is not live
        //objcProtocolClassCacheOffset = classProtocolVMAddr - dyldCache->unslidLoadAddress();
#error Unknown tool
#endif // BUILDING_DYLD

#endif // BUILDING_CACHE_BUILDER
}

void PrebuiltObjC::make(Diagnostics& diag, RuntimeState& state)
{

    // If we have the read only data, make sure it has a valid selector table inside.
    const objc::ClassHashTable*    objcClassOpt             = state.config.dyldCache.objcClassHashTable;
    const objc::SelectorHashTable* objcSelOpt               = state.config.dyldCache.objcSelectorHashTable;
    const objc::ProtocolHashTable* objcProtocolOpt          = state.config.dyldCache.objcProtocolHashTable;
    const void*                    headerInfoRO             = state.config.dyldCache.objcHeaderInfoRO;
    const void*                    headerInfoRW             = state.config.dyldCache.objcHeaderInfoRW;
    VMAddress                      headerInfoROUnslidVMAddr(state.config.dyldCache.objcHeaderInfoROUnslidVMAddr);

    if ( !objcClassOpt || !objcSelOpt || !objcProtocolOpt )
        return;

    if ( std::optional<VMOffset> offset = getProtocolClassCacheOffset(state); offset.has_value() )
        objcProtocolClassCacheOffset = offset.value();

    STACK_ALLOC_ARRAY(const Loader*, jitLoaders, state.loaded.size());
    for (const Loader* ldr : state.loaded)
        jitLoaders.push_back(ldr);

    // Find all the images with valid objc info
    SharedCacheImagesMapTy sharedCacheImagesMap;
    for ( const Loader* ldr : jitLoaders ) {
        const mach_o::MachOFileRef mf = ldr->mf(state);
        uint32_t pointerSize = mf->pointerSize();

        std::optional<VMOffset> objcImageInfoRuntimeOffset = getImageInfo(diag, state, ldr, mf);

        if ( !objcImageInfoRuntimeOffset.has_value() )
            continue;

        if ( ldr->dylibInDyldCache ) {
            // Add shared cache images to a map so that we can see them later for looking up classes
            uint64_t dylibUnslidVMAddr = mf->preferredLoadAddress();

            std::optional<uint16_t> objcIndex;
            objcIndex = objc::getPreoptimizedHeaderRWIndex(headerInfoRO, headerInfoRW,
                                                           headerInfoROUnslidVMAddr.rawValue(),
                                                           dylibUnslidVMAddr,
                                                           mf->is64());
            if ( !objcIndex.has_value() )
                return;
            sharedCacheImagesMap.insert({ *objcIndex, { VMAddress(dylibUnslidVMAddr), ldr } });
            continue;
        }

        // If we have a root of libobjc, just give up for now
        if ( ldr->matchesPath("/usr/lib/libobjc.A.dylib") )
            return;

        // dyld can see the strings in Fairplay binaries and protected segments, but other tools cannot.
        // Skip generating the PrebuiltObjC in these other cases
#if !BUILDING_DYLD
        // Find FairPlay encryption range if encrypted
        uint32_t fairPlayFileOffset;
        uint32_t fairPlaySize;
        if ( mf->isFairPlayEncrypted(fairPlayFileOffset, fairPlaySize) )
            return;

        __block bool hasProtectedSegment = false;
        mf->forEachSegment(^(const dyld3::MachOAnalyzer::SegmentInfo& segInfo, bool& stop) {
            if ( segInfo.isProtected ) {
                hasProtectedSegment = true;
                stop                = true;
            }
        });
        if ( hasProtectedSegment )
            return;
#endif

        // This image is good so record it for use later.
        objcImages.emplace_back((const JustInTimeLoader*)ldr, mf->preferredLoadAddress(), pointerSize);
        ObjCOptimizerImage& image = objcImages.back();
        image.jitLoader           = (const JustInTimeLoader*)ldr;

        // Set the offset to the objc image info
        image.binaryInfo.imageInfoRuntimeOffset = objcImageInfoRuntimeOffset->rawValue();

        // Get the range of a section which is required to contain pointers, i.e., be pointer sized.
        auto getPointerBasedSection = ^(const char* name, uint64_t& runtimeOffset, uint32_t& pointerCount) {
            uint64_t offset;
            uint64_t count;
            if ( mf->findObjCDataSection(name, offset, count) ) {
                if ( (count % pointerSize) != 0 ) {
                    image.diag.error("Invalid objc pointer section size");
                    return;
                }
                runtimeOffset = offset;
                pointerCount  = (uint32_t)count / pointerSize;
            }
            else {
                runtimeOffset = 0;
                pointerCount  = 0;
            }
        };

        // Find the offsets to all other sections we need for the later optimizations
        getPointerBasedSection("__objc_selrefs", image.binaryInfo.selRefsRuntimeOffset, image.binaryInfo.selRefsCount);
        getPointerBasedSection("__objc_classlist", image.binaryInfo.classListRuntimeOffset, image.binaryInfo.classListCount);
        getPointerBasedSection("__objc_catlist", image.binaryInfo.categoryListRuntimeOffset, image.binaryInfo.categoryCount);
        getPointerBasedSection("__objc_protolist", image.binaryInfo.protocolListRuntimeOffset, image.binaryInfo.protocolListCount);
    }

    for ( ObjCOptimizerImage& image : objcImages ) {
        if ( image.diag.hasError() )
            continue;

        optimizeObjCClasses(state, objcClassOpt, sharedCacheImagesMap, duplicateSharedCacheClassMap, image);
        if ( image.diag.hasError() )
            continue;

        optimizeObjCProtocols(state, objcProtocolOpt, sharedCacheImagesMap, image);
        if ( image.diag.hasError() )
            continue;

        optimizeObjCSelectors(state, objcSelOpt, closureSelectorMap, image);
        if ( image.diag.hasError() )
            continue;

        commitImage(image);
    }

    // If we successfully analyzed the classes and selectors, we can now emit their data
    generateHashTables(state);

    uint32_t pointerSize = state.mainExecutableLoader->mf(state)->pointerSize();
    generatePerImageFixups(state, pointerSize);

    builtObjC = true;
}

uint32_t PrebuiltObjC::serializeFixups(const Loader& jitLoader, BumpAllocator& allocator) const
{
    if ( !builtObjC )
        return 0;

    assert(jitLoader.ref.app);
    uint16_t index = jitLoader.ref.index;

    const ObjCImageFixups& fixups = imageFixups[index];

    if ( fixups.binaryInfo.imageInfoRuntimeOffset == 0 ) {
        // No fixups to apply
        return 0;
    }

    uint32_t                         serializationStart = (uint32_t)allocator.size();
    BumpAllocatorPtr<ObjCBinaryInfo> fixupInfo(allocator, serializationStart);

    allocator.append(&fixups.binaryInfo, sizeof(fixups.binaryInfo));

    // Protocols
    if ( !fixups.protocolISAFixups.empty() ) {
        // If we have protocol fixups, then we must have 1 for every protocol in this image.
        assert(fixups.protocolISAFixups.count() == fixups.binaryInfo.protocolListCount);

        uint16_t protocolArrayOff       = allocator.size() - serializationStart;
        fixupInfo->protocolFixupsOffset = protocolArrayOff;
        allocator.zeroFill(fixups.protocolISAFixups.count() * sizeof(uint8_t));
        allocator.align(8);
        BumpAllocatorPtr<uint8_t> protocolArray(allocator, serializationStart + protocolArrayOff);
        memcpy(protocolArray.get(), fixups.protocolISAFixups.begin(), fixups.protocolISAFixups.count() * sizeof(uint8_t));
    }

    // Selector references
    if ( !fixups.selectorReferenceFixups.empty() ) {
        uint16_t selectorsArrayOff                = allocator.size() - serializationStart;
        fixupInfo->selectorReferencesFixupsOffset = selectorsArrayOff;
        fixupInfo->selectorReferencesFixupsCount  = (uint32_t)fixups.selectorReferenceFixups.count();
        allocator.zeroFill(fixups.selectorReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef));
        BumpAllocatorPtr<uint8_t> selectorsArray(allocator, serializationStart + selectorsArrayOff);
        memcpy(selectorsArray.get(), fixups.selectorReferenceFixups.begin(), fixups.selectorReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef));
    }

    return serializationStart;
}

} // namespace dyld4


// Temporary copy of the old hash tables, to let the split cache branch load old hash tables
namespace legacy_objc_opt
{

uint32_t objc_stringhash_t::hash(const char *key, size_t keylen) const
{
    uint64_t val = objc::lookup8((uint8_t*)key, keylen, salt);
    uint32_t index = (uint32_t)(val>>shift) ^ scramble[tab[val&mask]];
    return index;
}


const header_info_rw *getPreoptimizedHeaderRW(const struct header_info *const hdr,
                                              void* headerInfoRO, void* headerInfoRW)
{
    const objc_headeropt_ro_t* hinfoRO = (const objc_headeropt_ro_t*)headerInfoRO;
    const objc_headeropt_rw_t* hinfoRW = (const objc_headeropt_rw_t*)headerInfoRW;
    int32_t index = hinfoRO->index(hdr);
    assert(hinfoRW->entsize == sizeof(header_info_rw));
    return &hinfoRW->headers[index];
}

}
