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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

#include <assert.h>
#include <unistd.h>
#include <sys/types.h>

#include "Defines.h"
#include "Header.h"
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

using mach_o::Header;

#if SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
using dyld3::OverflowSafeArray;
typedef dyld4::PrebuiltObjC::ObjCOptimizerImage ObjCOptimizerImage;

// This holds all the maps we are going to serialize.
namespace prebuilt_objc
{

void forEachSelectorStringEntry(const void* selMap, void (^handler)(const PrebuiltLoader::BindTargetRef& target))
{
    // The on-disk map is really an ObjCSelectorMapOnDisk
    const ObjCSelectorMapOnDisk map(selMap);
    map.forEachEntry(^(const ObjCSelectorMapOnDisk::NodeT& node) {
        handler(node.first.stringTarget);
    });
}

#if SUPPORT_VM_LAYOUT

const char* findSelector(dyld4::RuntimeState* state, const ObjCSelectorMapOnDisk& map,
                         const char* selectorName)
{
    auto it = map.find((void*)state, selectorName);
    if ( it == map.end() )
        return nullptr;

    return (const char*)it->first.stringTarget.value(*state);
}

void forEachClass(dyld4::RuntimeState* state, const ObjCClassMapOnDisk& classMap, const char* className,
                  void (^handler)(const dyld3::Array<const PrebuiltLoader::BindTargetRef*>& values))
{
    classMap.forEachEntry(state, className, ^(const dyld3::Array<const ObjCObjectOnDiskLocation*>& values) {
        if ( values.empty() )
            return;

        STACK_ALLOC_ARRAY(const PrebuiltLoader::BindTargetRef*, newValues, values.count());
        for ( const ObjCObjectOnDiskLocation* value : values )
            newValues.push_back(&value->objectLocation);

        handler(newValues);
    });
}

void forEachProtocol(dyld4::RuntimeState* state, const ObjCProtocolMapOnDisk& protocolMap, const char* protocolName,
                     void (^handler)(const dyld3::Array<const PrebuiltLoader::BindTargetRef*>& values))
{
    protocolMap.forEachEntry(state, protocolName, ^(const dyld3::Array<const ObjCObjectOnDiskLocation*>& values) {
        if ( values.empty() )
            return;

        STACK_ALLOC_ARRAY(const PrebuiltLoader::BindTargetRef*, newValues, values.count());
        for ( const ObjCObjectOnDiskLocation* value : values )
            newValues.push_back(&value->objectLocation);

        handler(newValues);
    });
}

#endif // SUPPORT_VM_LAYOUT

void forEachClass(const void* classMap,
                  void (^handler)(const PrebuiltLoader::BindTargetRef& nameTarget,
                                  const dyld3::Array<const PrebuiltLoader::BindTargetRef*>& values))
{
    // The on-disk map is really an ObjCClassMapOnDisk
    const ObjCClassMapOnDisk map(classMap);
    map.forEachEntry(^(const ObjCStringKeyOnDisk& key, const dyld3::Array<const ObjCObjectOnDiskLocation*>& values) {
        STACK_ALLOC_ARRAY(const PrebuiltLoader::BindTargetRef*, newValues, values.count());
        for ( const ObjCObjectOnDiskLocation* value : values )
            newValues.push_back(&value->objectLocation);
        handler(key.stringTarget, newValues);
    });
}

void forEachProtocol(const void* protocolMap,
                     void (^handler)(const PrebuiltLoader::BindTargetRef& nameTarget,
                                     const dyld3::Array<const PrebuiltLoader::BindTargetRef*>& values))
{
    // The on-disk map is really an ObjCProtocolMapOnDisk
    const ObjCProtocolMapOnDisk map(protocolMap);
    map.forEachEntry(^(const ObjCStringKeyOnDisk& key, const dyld3::Array<const ObjCObjectOnDiskLocation*>& values) {
        STACK_ALLOC_ARRAY(const PrebuiltLoader::BindTargetRef*, newValues, values.count());
        for ( const ObjCObjectOnDiskLocation* value : values )
            newValues.push_back(&value->objectLocation);
        handler(key.stringTarget, newValues);
    });
}

uint64_t hashStringKey(const std::string_view& str)
{
    return murmurHash(str.data(), (int)str.size(), 0);
}

} // namespace prebuilt_objc

namespace dyld4 {

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
                            diag.error("out of range bind ordinal %d (max %llu)", bindOrdinal, bindTargetsAreWeakImports.count());
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
                        diag.error("out of range bind ordinal %d (max %llu)", targetIndex, bindTargetsAreWeakImports.count());
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
                        diag.error("out of range bind ordinal %d (max %llu)", overrideBindTargetIndex, overrideBindTargetsAreWeakImports.count());
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
                        diag.error("out of range bind ordinal %d (max %llu)", targetIndex, bindTargetsAreWeakImports.count());
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
                                                      PrebuiltObjC::SelectorMapTy& appSelectorMap,
                                                      VMOffset selectorReferenceRuntimeOffset, VMOffset selectorStringRuntimeOffset,
                                                      const char* selectorString)
{

    // fprintf(stderr, "selector: %p -> %p %s\n", (void*)selectorReferenceRuntimeOffset, (void*)selectorStringRuntimeOffset, selectorString);
    if ( const char* sharedCacheSelector = objcSelOpt->get(selectorString) ) {
        // We got the selector from the cache so add a fixup to point there.
        // We use an absolute bind here, to reference the offset from the shared cache selector table base
        uint64_t sharedCacheOffset = (uint64_t)sharedCacheSelector - (uint64_t)objcSelOpt;
        PrebuiltLoader::BindTargetRef bindTarget = PrebuiltLoader::BindTargetRef::makeAbsolute(sharedCacheOffset);

        //printf("Overriding fixup at 0x%08llX to cache offset 0x%08llX\n", selectorUseImageOffset, (uint64_t)objcSelOpt->getEntryForIndex(cacheSelectorIndex) - (uint64_t)state.config.dyldCache());
        selectorFixups.push_back(bindTarget);
        return;
    }

    // See if this selector is already in the app map from a previous image
    prebuilt_objc::ObjCStringKey selectorMapKey { selectorString };
    auto appSelectorIt = appSelectorMap.find(selectorMapKey);
    if ( appSelectorIt != appSelectorMap.end() ) {
        // This selector was found in a previous image, so use it here.

        //printf("Overriding fixup at 0x%08llX to other image\n", selectorUseImageOffset);
        selectorFixups.push_back(PrebuiltLoader::BindTargetRef(appSelectorIt->second.nameLocation));
        return;
    }

    // See if this selector is already in the map for this image
    prebuilt_objc::ObjCSelectorLocation selectorMapValue = { Loader::BindTarget() };
    auto itAndInserted = selectorMap.insert({ selectorMapKey, selectorMapValue });
    if ( itAndInserted.second ) {
        // We added the selector so its pointing in to our own image.
        Loader::BindTarget target;
        target.loader               = jitLoader;
        target.runtimeOffset        = selectorStringRuntimeOffset.rawValue();
        itAndInserted.first->second.nameLocation = target;

        // We'll add a fixup anyway as we want a sel ref fixup for every entry in the sel refs section

        //printf("Fixup at 0x%08llX to '%s' offset 0x%08llX\n", selectorUseImageOffset, findLoadedImage(target.image.imageNum).path(), target.image.offset);
        selectorFixups.push_back(PrebuiltLoader::BindTargetRef(target));
        return;
    }

    // This selector was found elsewhere in our image.  As we want a fixup for every selref, we'll
    // add one here too
    Loader::BindTarget& target = itAndInserted.first->second.nameLocation;

    //printf("Overriding fixup at 0x%08llX to '%s' offset 0x%08llX\n", selectorUseImageOffset, findLoadedImage(target.image.imageNum).path(), target.image.offset);
    selectorFixups.push_back(PrebuiltLoader::BindTargetRef(target));
}

// Check if the given class is in an image loaded in the shared cache.
// If so, add the class to the duplicate map
static void checkForDuplicateClass(const VMAddress dyldCacheBaseAddress,
                                   const char* className, const objc::ClassHashTable* objcClassOpt,
                                   PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                   PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClasses,
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
                                    SharedCacheImagesMapTy& sharedCacheImagesMap,
                                    DuplicateClassesMapTy& duplicateSharedCacheClasses,
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
                                    PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap)
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
                                       SharedCacheImagesMapTy& sharedCacheImagesMap,
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

    const DyldSharedCache* dyldCache = (const DyldSharedCache*)state.config.dyldCache.addr;
    uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
    objc_visitor::Visitor objcVisitor(dyldCache, dylibMA, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
    return objcVisitor;
#elif SUPPORT_VM_LAYOUT
    const dyld3::MachOAnalyzer* dylibMA = ldr->analyzer(state);

    objc_visitor::Visitor objcVisitor(dylibMA);
    return objcVisitor;
#else
    const dyld3::MachOFile* dylibMF = ldr->mf(state);
    return dylibMF->makeObjCVisitor(diag);
#endif
}

static void optimizeObjCSelectors(RuntimeState& state,
                                  const objc::SelectorHashTable* objcSelOpt,
                                  PrebuiltObjC::SelectorMapTy& appSelectorMap,
                                  ObjCOptimizerImage&                image)
{

    const Header*   hdr         = (const Header*)image.jitLoader->mf(state);
    uint32_t        pointerSize = hdr->pointerSize();

    // The legacy (objc1) codebase uses a bunch of sections we don't want to reason about.  If we see them just give up.
    __block bool foundBadSection = false;
    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.segmentName != "__OBJC" )
            return;
        if ( sectInfo.sectionName == "__module_info" ) {
            foundBadSection = true;
            stop            = true;
            return;
        }
        if ( sectInfo.sectionName == "__protocol" ) {
            foundBadSection = true;
            stop            = true;
            return;
        }
        if ( sectInfo.sectionName == "__message_refs" ) {
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
    if ( hdr->isArch("x86_64") || hdr->isArch("x86_64h") ) {
        if ( hdr->hasObjCMessageReferences() ) {
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
        image.binaryInfo.hasCategoryMethodListsToUnique     = hasPointerBasedMethodList;
        image.binaryInfo.hasCategoryMethodListsToSetUniqued = hasPointerBasedMethodList;
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
        image.binaryInfo.hasProtocolMethodListsToUnique     = hasPointerBasedMethodList;
        image.binaryInfo.hasProtocolMethodListsToSetUniqued = hasPointerBasedMethodList;
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
                                PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClasses,
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
                image.diag.error("Missing weak superclass of class %s in %s", className, image.jitLoader->path(state));
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
                                  PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
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

static void optimizeObjCProtocolReferences(RuntimeState& state,
                                           const objc::ProtocolHashTable* objcProtocolOpt,
                                           PrebuiltObjC::SharedCacheImagesMapTy& sharedCacheImagesMap,
                                           PrebuiltObjC::ProtocolMapTy& protocolMap,
                                           ObjCOptimizerImage& image)
{
    if ( image.binaryInfo.protocolRefsCount == 0 )
        return;

    image.protocolFixups.reserve(image.binaryInfo.protocolRefsCount);

    // FIXME: Don't make a duplicate one of these if we can pass one in instead
    __block objc_visitor::Visitor objcVisitor = makeObjCVisitor(image.diag, state, image.jitLoader);
    if ( image.diag.hasError() )
        return;

    objcVisitor.forEachProtocolReference(^(metadata_visitor::ResolvedValue& protocolRefValue) {
        if ( image.diag.hasError() )
            return;

        // Follow the protocol reference to get to the actual protocol
        metadata_visitor::ResolvedValue protocolValue = objcVisitor.resolveRebase(protocolRefValue);
        objc_visitor::Protocol objcProtocol(protocolValue);

        const char* protocolName = objcProtocol.getName(objcVisitor);

        // Check if this protocol is in the map in the shared cache.  If so use that one
        __block std::optional<uint64_t> protocolCacheOffset;
        objcProtocolOpt->forEachProtocol(protocolName,
                                         ^(uint64_t classCacheOffset, uint16_t dylibObjCIndex, bool& stopObjects) {
            // Check if this image is loaded
            if ( auto cacheIt = sharedCacheImagesMap.find(dylibObjCIndex); cacheIt != sharedCacheImagesMap.end() ) {
                protocolCacheOffset = classCacheOffset;
                stopObjects = true;
            }
        });
        if ( protocolCacheOffset.has_value() ) {
            // We use an absolute bind to point in to the shared cache protocols
            PrebuiltLoader::BindTargetRef bindTarget = PrebuiltLoader::BindTargetRef::makeAbsolute(protocolCacheOffset.value());
            image.protocolFixups.push_back(bindTarget);
            return;
        }

        // Not using the shared cache, so we should find the protocol in the map in the closure
        prebuilt_objc::ObjCStringKey key = { protocolName };
        auto nameIt = protocolMap.find(key);
        if ( nameIt == protocolMap.end() ) {
            // FIXME: What do we do here?  The protocols are wrong?  Skip this image for now.
            image.diag.error("Could not find protocol '%s'", protocolName);
            return;
        }
        const prebuilt_objc::ObjCObjectLocation& protocolLocation = nameIt->value;
        image.protocolFixups.push_back(protocolLocation.objectLocation);
    });
}


static void
generateClassOrProtocolHashTable(PrebuiltObjC::ObjCStructKind objcKind,
                                 Array<ObjCOptimizerImage>& objcImages,
                                 const PrebuiltObjC::DuplicateClassesMapTy& duplicateSharedCacheClassMap,
                                 PrebuiltObjC::ObjectMapTy& objectMap, bool& hasDuplicates)
{
    // Note we walk the images backwards as we want them in load order to match the order they are registered with objc
    for ( uint64_t imageIndex = 0, reverseIndex = (objcImages.count() - 1); imageIndex != objcImages.count(); ++imageIndex, --reverseIndex ) {
        if ( objcImages[reverseIndex].diag.hasError() )
            continue;
        ObjCOptimizerImage& image = objcImages[reverseIndex];

        if ( objcKind == PrebuiltObjC::ObjCStructKind::classes ) {
            for ( const ObjCOptimizerImage::ObjCObject& classLocation : image.classLocations ) {
                //uint64_t nameVMAddr     = ma->preferredLoadAddress() + classImage.offsetOfClassNames + classNameTarget.classNameImageOffset;
                //printf("%s: 0x%08llx = '%s'\n", li.path(), nameVMAddr, className);

                // Also track the name
                PrebuiltLoader::BindTarget nameTarget  = { image.jitLoader, classLocation.nameRuntimeOffset.rawValue() };
                PrebuiltLoader::BindTarget valueTarget = { image.jitLoader, classLocation.valueRuntimeOffset.rawValue() };
                prebuilt_objc::ObjCStringKey key = { classLocation.name };
                prebuilt_objc::ObjCObjectLocation value = {
                    nameTarget,
                    valueTarget
                };
                bool alreadyHaveNodeWithKey = false;
                auto objectIt = objectMap.insert({ key, value }, alreadyHaveNodeWithKey);
                if ( !alreadyHaveNodeWithKey ) {
                    // Check if we have a duplicate.  If we do, it will be on the last image which had a duplicate class name,
                    // but as we walk images backwards, we'll see this before all other images with duplicates.
                    // Note we only check for duplicates when we know we just inserted the object name in to the map, as this
                    // ensure's that we only insert each duplicate once
                    auto duplicateClassIt = duplicateSharedCacheClassMap.find(classLocation.name);
                    if ( duplicateClassIt != duplicateSharedCacheClassMap.end() ) {
                        // This is gross.  Change this entry to the duplicate, and add a new one
                        objectIt->value = { nameTarget, duplicateClassIt->second };

                        bool unusedAlreadyHaveNodeWithKey;
                        objectMap.insert({ key, value }, unusedAlreadyHaveNodeWithKey);
                        hasDuplicates = true;
                    }
                } else {
                    // We didn't add the node, so we have duplicates
                    hasDuplicates = true;
                }
            }
        }

        if ( objcKind == PrebuiltObjC::ObjCStructKind::protocols ) {
            for ( const ObjCOptimizerImage::ObjCObject& protocolLocation : image.protocolLocations ) {
                // Also track the name
                PrebuiltLoader::BindTarget nameTarget  = { image.jitLoader, protocolLocation.nameRuntimeOffset.rawValue() };
                PrebuiltLoader::BindTarget valueTarget = { image.jitLoader, protocolLocation.valueRuntimeOffset.rawValue() };
                prebuilt_objc::ObjCStringKey key = { protocolLocation.name };
                prebuilt_objc::ObjCObjectLocation value = {
                    nameTarget,
                    valueTarget
                };
                bool alreadyHaveNodeWithKey = false;
                objectMap.insert({ key, value }, alreadyHaveNodeWithKey);
                if ( !alreadyHaveNodeWithKey ) {
                    // We are processing protocols, and this is the first one we've seen, so track its ISA to be fixed up
                    auto protocolIndexIt = image.protocolIndexMap.find(protocolLocation.valueRuntimeOffset);
                    assert(protocolIndexIt != image.protocolIndexMap.end());
                    image.protocolISAFixups[protocolIndexIt->second] = true;
                }
            }
        }
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
        this->selectorMap[stringAndTarget.first] = stringAndTarget.second;
    }
}

uint32_t PrebuiltObjC::serializeSelectorMap(dyld4::BumpAllocator& alloc) const
{
    // The key on the new map is the name bind target
    typedef prebuilt_objc::ObjCSelectorMapOnDisk::KeyType (^KeyFuncTy)(const SelectorMapTy::KeyType&, const SelectorMapTy::ValueType&);
    KeyFuncTy convertKey = ^(const SelectorMapTy::KeyType& key, const SelectorMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCSelectorMapOnDisk::KeyType){ PrebuiltLoader::BindTargetRef(value.nameLocation) };
    };

    // The value on the new map is unused
    typedef prebuilt_objc::ObjCSelectorMapOnDisk::ValueType (^ValueFuncTy)(const SelectorMapTy::KeyType&, const SelectorMapTy::ValueType&);
    ValueFuncTy convertValue = ^(const SelectorMapTy::KeyType& key, const SelectorMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCSelectorMapOnDisk::ValueType)0;
    };

    uint32_t offset = (uint32_t)alloc.size();
    this->selectorMap.serialize(alloc, convertKey, convertValue);
    return offset;
}

uint32_t PrebuiltObjC::serializeClassMap(dyld4::BumpAllocator& alloc) const
{
    // The key on the new map is the name bind taret
    typedef prebuilt_objc::ObjCClassMapOnDisk::KeyType (^KeyFuncTy)(const ClassMapTy::KeyType&, const ClassMapTy::ValueType&);
    KeyFuncTy convertKey = ^(const ClassMapTy::KeyType& key, const ClassMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCClassMapOnDisk::KeyType){ PrebuiltLoader::BindTargetRef(value.nameLocation) };
    };

    // The value on the new map is just the class impl
    typedef prebuilt_objc::ObjCClassMapOnDisk::ValueType (^ValueFuncTy)(const ClassMapTy::KeyType&, const ClassMapTy::ValueType&);
    ValueFuncTy convertValue = ^(const ClassMapTy::KeyType& key, const ClassMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCClassMapOnDisk::ValueType){ PrebuiltLoader::BindTargetRef(value.objectLocation) };
    };

    uint32_t offset = (uint32_t)alloc.size();
    this->classMap.serialize(alloc, convertKey, convertValue);
    return offset;
}

uint32_t PrebuiltObjC::serializeProtocolMap(dyld4::BumpAllocator& alloc) const
{
    // The key on the new map is the name bind taret
    typedef prebuilt_objc::ObjCProtocolMapOnDisk::KeyType (^KeyFuncTy)(const ProtocolMapTy::KeyType&, const ProtocolMapTy::ValueType&);
    KeyFuncTy convertKey = ^(const ProtocolMapTy::KeyType& key, const ProtocolMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCProtocolMapOnDisk::KeyType){ PrebuiltLoader::BindTargetRef(value.nameLocation) };
    };

    // The value on the new map is just the protocol impl
    typedef prebuilt_objc::ObjCProtocolMapOnDisk::ValueType (^ValueFuncTy)(const ProtocolMapTy::KeyType&, const ProtocolMapTy::ValueType&);
    ValueFuncTy convertValue = ^(const ProtocolMapTy::KeyType& key, const ProtocolMapTy::ValueType& value) {
        return (prebuilt_objc::ObjCProtocolMapOnDisk::ValueType){ PrebuiltLoader::BindTargetRef(value.objectLocation) };
    };

    uint32_t offset = (uint32_t)alloc.size();
    this->protocolMap.serialize(alloc, convertKey, convertValue);
    return offset;
}

void PrebuiltObjC::generateHashTables()
{
    generateClassOrProtocolHashTable(PrebuiltObjC::ObjCStructKind::classes, objcImages, 
                                     duplicateSharedCacheClassMap, classMap, this->hasClassDuplicates);

    bool unusedHasProtocolDuplicates = false;
    generateClassOrProtocolHashTable(PrebuiltObjC::ObjCStructKind::protocols, objcImages, 
                                     duplicateSharedCacheClassMap, protocolMap, unusedHasProtocolDuplicates);
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

        // Protocol references.
        // These are a BindTargetRef for every protocol reference to fixup
        if ( !image.protocolFixups.empty() ) {
            fixups.protocolReferenceFixups.reserve(image.protocolFixups.count());
            for ( const PrebuiltLoader::BindTargetRef& target : image.protocolFixups ) {
                fixups.protocolReferenceFixups.push_back(target);
            }
        }
    }
}

__attribute__((noinline))
static void forEachSelectorReferenceToUnique(objc_visitor::Visitor& objcVisitor,
                                             uint64_t                loadAddress,
                                             const ObjCBinaryInfo&   binaryInfo,
                                             void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                              uint64_t selectorStringRuntimeOffset,
                                                              const char* selectorString))
{
    if ( binaryInfo.selRefsCount != 0 ) {
        objcVisitor.forEachSelectorReference(^(VMAddress selRefVMAddr, VMAddress selRefTargetVMAddr,
                                               const char* selectorString) {
            VMOffset selectorReferenceRuntimeOffset = selRefVMAddr - VMAddress(loadAddress);
            VMOffset selectorStringRuntimeOffset    = selRefTargetVMAddr - VMAddress(loadAddress);
            callback(selectorReferenceRuntimeOffset.rawValue(), selectorStringRuntimeOffset.rawValue(),
                     selectorString);
        });
    }
}

__attribute__((noinline))
static void forEachClassSelectorReferenceToUnique(objc_visitor::Visitor& objcVisitor,
                                                  uint64_t                loadAddress,
                                                  const ObjCBinaryInfo&   binaryInfo,
                                                  void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                                   uint64_t selectorStringRuntimeOffset,
                                                                   const char* selectorString))
{

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
}

__attribute__((noinline))
static void forEachCategorySelectorReferenceToUnique(objc_visitor::Visitor& objcVisitor,
                                                     uint64_t                loadAddress,
                                                     const ObjCBinaryInfo&   binaryInfo,
                                                     void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                                      uint64_t selectorStringRuntimeOffset,
                                                                      const char* selectorString))
{
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

    if ( binaryInfo.hasCategoryMethodListsToUnique && (binaryInfo.categoryCount != 0) ) {
        // FIXME: Use binaryInfo.categoryListRuntimeOffset and binaryInfo.categoryCount
        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool &stopCategory) {
            objc_visitor::MethodList instanceMethodList = objcCategory.getInstanceMethods(objcVisitor);
            objc_visitor::MethodList classMethodList    = objcCategory.getClassMethods(objcVisitor);

            visitMethodList(instanceMethodList);
            visitMethodList(classMethodList);
        });
    }
}

__attribute__((noinline))
static void forEachProtocolSelectorReferenceToUnique(objc_visitor::Visitor& objcVisitor,
                                                     uint64_t                loadAddress,
                                                     const ObjCBinaryInfo&   binaryInfo,
                                                     void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                                      uint64_t selectorStringRuntimeOffset,
                                                                      const char* selectorString))
{
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

    dyld4::forEachSelectorReferenceToUnique(objcVisitor, loadAddress, binaryInfo, callback);
    dyld4::forEachClassSelectorReferenceToUnique(objcVisitor, loadAddress, binaryInfo, callback);
    dyld4::forEachCategorySelectorReferenceToUnique(objcVisitor, loadAddress, binaryInfo, callback);
    dyld4::forEachProtocolSelectorReferenceToUnique(objcVisitor, loadAddress, binaryInfo, callback);
}

static std::optional<VMOffset> getImageInfo(Diagnostics& diag, RuntimeState& state,
                                            const Loader* ldr, const Header* hdr)
{
    __block std::optional<VMOffset> objcImageInfoRuntimeOffset;
    hdr->forEachSection(^(const Header::SectionInfo& sectionInfo, bool& stop) {
        if ( !sectionInfo.segmentName.starts_with("__DATA") )
            return;
        if ( sectionInfo.sectionName != "__objc_imageinfo" )
            return;
        if ( sectionInfo.size != 8 ) {
            stop = true;
            return;
        }

        // We can't just access the image info directly from the MachOFile.  Instead we have to
        // use the layout to find the actual location of the segment, as we might be in the cache builder
        ldr->withLayout(diag, state, ^(const mach_o::Layout& layout) {
            const mach_o::SegmentLayout& segment = layout.segments[sectionInfo.segIndex];
            uint64_t offsetInSegment = sectionInfo.address - segment.vmAddr;
            const auto* imageInfo = (MachOAnalyzer::ObjCImageInfo*)(segment.buffer + offsetInSegment);

            if ( (imageInfo->flags & MachOAnalyzer::ObjCImageInfo::dyldPreoptimized) != 0 )
                return;

            objcImageInfoRuntimeOffset = VMOffset(sectionInfo.address - layout.textUnslidVMAddr());
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

    if ( !objcClassOpt || !objcSelOpt || !objcProtocolOpt)
        return;

    if ( std::optional<VMOffset> offset = getProtocolClassCacheOffset(state); offset.has_value() )
        objcProtocolClassCacheOffset = offset.value();

    for ( const Loader* ldr : state.delayLoaded ) {
        if ( ldr->isJustInTimeLoader() ) {
            // TODO: Handle apps which delay-init on-disk dylibs
            // This will lead to the closure not optimizing the objc, and libobjc will do it instead
            // in map_images().  This is safe as we tell objc (via dyldDoesObjCFixups()) whether we optimized or not
            return;
        }
    }

    // Find all the images with valid objc info
    SharedCacheImagesMapTy sharedCacheImagesMap;

    // Note we have done the delay-init partitioning by this point, so state.loaded is just the loaders
    // we known we need at launch.  This is important for the shared cache in particular as the shared cache
    // classes/protocols are always preferred over the app ones, so a shared cache image being delayed or not
    // impacts the choice of classes/protocols.  See protocolIsInSharedCache() for example.
    for ( const Loader* ldr : state.loaded ) {
        const Header* hdr = (const Header*)ldr->mf(state);
        uint32_t pointerSize = hdr->pointerSize();

        std::optional<VMOffset> objcImageInfoRuntimeOffset = getImageInfo(diag, state, ldr, hdr);

        if ( !objcImageInfoRuntimeOffset.has_value() )
            continue;

        if ( ldr->dylibInDyldCache ) {
            // Add shared cache images to a map so that we can see them later for looking up classes
            uint64_t dylibUnslidVMAddr = hdr->preferredLoadAddress();

            std::optional<uint16_t> objcIndex;
            objcIndex = objc::getPreoptimizedHeaderROIndex(headerInfoRO, headerInfoRW,
                                                           headerInfoROUnslidVMAddr.rawValue(),
                                                           dylibUnslidVMAddr,
                                                           hdr->is64());
            if ( !objcIndex.has_value() )
                return;
            sharedCacheImagesMap.insert({ *objcIndex, { VMAddress(dylibUnslidVMAddr), ldr } });
            continue;
        }

        // If we have a root of libobjc, just give up for now
        if ( ldr->matchesPath(state, "/usr/lib/libobjc.A.dylib") )
            return;

        // dyld can see the strings in Fairplay binaries and protected segments, but other tools cannot.
        // Skip generating the PrebuiltObjC in these other cases
#if !BUILDING_DYLD
        // Find FairPlay encryption range if encrypted
        uint32_t fairPlayFileOffset;
        uint32_t fairPlaySize;
        if ( hdr->isFairPlayEncrypted(fairPlayFileOffset, fairPlaySize) )
            return;

        __block bool hasProtectedSegment = false;
        hdr->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
            if ( segInfo.isProtected() ) {
                hasProtectedSegment = true;
                stop                = true;
            }
        });
        if ( hasProtectedSegment )
            return;
#endif

        // This image is good so record it for use later.
        objcImages.emplace_back((const JustInTimeLoader*)ldr, hdr->preferredLoadAddress(), pointerSize);
        ObjCOptimizerImage& image = objcImages.back();
        image.jitLoader           = (const JustInTimeLoader*)ldr;

        // Set the offset to the objc image info
        image.binaryInfo.imageInfoRuntimeOffset = objcImageInfoRuntimeOffset->rawValue();

        // Get the range of a section which is required to contain pointers, i.e., be pointer sized.
        auto getPointerBasedSection = ^(const char* name, uint64_t& runtimeOffset, uint32_t& pointerCount) {
            uint64_t offset;
            uint64_t count;
            if ( hdr->findObjCDataSection(name, offset, count) ) {
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
        getPointerBasedSection("__objc_protorefs", image.binaryInfo.protocolRefsRuntimeOffset, image.binaryInfo.protocolRefsCount);
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

        optimizeObjCSelectors(state, objcSelOpt, selectorMap, image);
        if ( image.diag.hasError() )
            continue;

        commitImage(image);
    }

    // If we successfully analyzed the classes and selectors, we can now make the maps
    generateHashTables();

    // Once we have the hash tables with the canonical protocols, we can generate the fixups
    // for the protorefs, which need to point to the canonical protocol
    for ( ObjCOptimizerImage& image : objcImages ) {
        if ( image.diag.hasError() )
            continue;

        optimizeObjCProtocolReferences(state, objcProtocolOpt, sharedCacheImagesMap, protocolMap, image);
    }

    uint32_t pointerSize = state.mainExecutableLoader->mf(state)->pointerSize();
    generatePerImageFixups(state, pointerSize);

    builtObjC = true;
}

uint32_t PrebuiltObjC::serializeFixups(const Loader& jitLoader, BumpAllocator& allocator)
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
        memcpy(protocolArray.get(), fixups.protocolISAFixups.data(), (size_t)(fixups.protocolISAFixups.count() * sizeof(uint8_t)));
    }

    // Selector references
    if ( !fixups.selectorReferenceFixups.empty() ) {
        uint64_t selectorsArrayOff                = allocator.size() - serializationStart;
        fixupInfo->selectorReferencesFixupsOffset = (uint32_t)selectorsArrayOff;
        fixupInfo->selectorReferencesFixupsCount  = (uint32_t)fixups.selectorReferenceFixups.count();
        allocator.zeroFill(fixups.selectorReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef));
        BumpAllocatorPtr<uint8_t> selectorsArray(allocator, serializationStart + selectorsArrayOff);
        memcpy(selectorsArray.get(), fixups.selectorReferenceFixups.data(), (size_t)(fixups.selectorReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef)));
    }

    // Protocol references
    if ( !fixups.protocolReferenceFixups.empty() ) {
        uint64_t protocolsArrayOff                = allocator.size() - serializationStart;
        fixupInfo->protocolReferencesFixupsOffset = (uint32_t)protocolsArrayOff;
        fixupInfo->protocolReferencesFixupsCount  = (uint32_t)fixups.protocolReferenceFixups.count();
        allocator.zeroFill(fixups.protocolReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef));
        BumpAllocatorPtr<uint8_t> protocolsArray(allocator, serializationStart + protocolsArrayOff);
        memcpy(protocolsArray.get(), fixups.protocolReferenceFixups.data(), (size_t)(fixups.protocolReferenceFixups.count() * sizeof(PrebuiltLoader::BindTargetRef)));
    }

    return serializationStart;
}

} // namespace dyld4
#endif // SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

#endif // !TARGET_OS_EXCLAVEKIT

