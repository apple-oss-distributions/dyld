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
#include <strings.h>

#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
#endif

#include "Defines.h"
#include "Loader.h"
#include "JustInTimeLoader.h"
#include "MachOAnalyzer.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"

// mach_o
#include "Header.h"
#include "Version32.h"

using dyld3::MachOAnalyzer;
using dyld3::MachOFile;
using dyld3::FatFile;
using mach_o::Header;
using mach_o::Platform;
using mach_o::Version32;

namespace dyld4 {

//////////////////////// "virtual" functions /////////////////////////////////

const dyld3::MachOFile* JustInTimeLoader::mf(const RuntimeState&) const
{
#if SUPPORT_VM_LAYOUT
    return this->mappedAddress;
#else
    return this->mappedAddress.operator->();
#endif // SUPPORT_VM_LAYOUT
}

#if SUPPORT_VM_LAYOUT
const MachOLoaded* JustInTimeLoader::loadAddress(const RuntimeState&) const
{
    return mappedAddress;
}
#endif // SUPPORT_VM_LAYOUT

const char* JustInTimeLoader::path(const RuntimeState& state) const
{
    return this->pathOffset ? ((char*)this + this->pathOffset) : nullptr;
}

const char* JustInTimeLoader::installName(const RuntimeState& state) const
{
    mach_o::Header* mh = (mach_o::Header*)this->mf(state);
    if ( mh->isDylib() )
        return mh->installName();
    return nullptr;
}

#if BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_UNIT_TESTS
bool JustInTimeLoader::contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const
{
    if ( addr < this->mappedAddress )
        return false;

    if ( pd ) {
        if ( pd->contains(addr) ) {
            // FIXME: We might want a path to find the __TEXT segment, to avoid a
            //        contradiction between the load command in the JITDylib mach
            //        header and the values returned here.
            //        We might also want to punt down to a pseudo-dylib
            //        callback. In some cases it could provide a usable answer.
            segAddr = 0;
            segSize = 0;
            segPerms = 0;
            return true;
        }
    }

    __block bool         result     = false;
    const Header*        hdr         = (const Header*)this->mappedAddress;
    uint64_t             vmTextAddr = hdr->preferredLoadAddress();
    uint64_t             slide      = (uintptr_t)hdr - vmTextAddr;
    uint64_t             targetAddr = (uint64_t)addr;
    hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( ((info.vmaddr + slide) <= targetAddr) && (targetAddr < (info.vmaddr + slide + info.vmsize)) ) {
            *segAddr  = (void*)(info.vmaddr + slide);
            *segSize  = info.vmsize;
            *segPerms = info.initProt;
            result    = true;
            stop      = true;
        }
    });
    return result;
}
#endif // BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_UNIT_TESTS

bool JustInTimeLoader::matchesPath(const RuntimeState& state, const char* path) const
{
    if ( strcmp(path, this->path(state)) == 0 )
        return true;
    if ( this->altInstallName ) {
        if ( strcmp(path, ((const Header*)this->mappedAddress)->installName()) == 0 )
            return true;
    }
    if ( pd ) {
      if ( auto *canonicalPath = pd->loadableAtPath(path) ) {
        // Dispose of canonicalPath if it is different from path
        // (loadableAtPath is allowed to return its argument, which should not be freed).
        if ( canonicalPath != path )
          pd->disposeString(canonicalPath);
        return true;
      }
    }

    return false;
}

FileID JustInTimeLoader::fileID(const RuntimeState& state) const
{
    return this->fileIdent;
}

struct HashPointer {
    static size_t hash(const void* v, void* state) {
        return std::hash<uintptr_t>{}((uintptr_t)v);
    }
};

struct EqualPointer {
    static bool equal(const void* s1, const void* s2, void* state) {
        return s1 == s2;
    }
};

typedef dyld3::Map<const void*, bool, HashPointer, EqualPointer> PointerSet;

// A class in the root can be patched only if the __objc_classlist entry for that class is bind to self.  We need to
// find the class list and check each class.  For meta classes, the ISA in the class should be a bind to self
#if SUPPORT_VM_LAYOUT
static void getObjCPatchClasses(const dyld3::MachOAnalyzer* ma, PointerSet& classPointers)
{
    if ( !ma->hasChainedFixups() )
        return;

    Diagnostics diag;

    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, bindTargets, 32);
    ma->forEachBindTarget(diag, false, ^(const dyld3::MachOAnalyzer::BindTargetInfo& info, bool& stop) {
        if ( diag.hasError() ) {
            stop = true;
            return;
        }

        if ( info.libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
            void* result = nullptr;
            bool  resultPointsToInstructions = false;
            if ( ma->hasExportedSymbol(info.symbolName, nullptr, &result, &resultPointsToInstructions) ) {
                bindTargets.push_back(result);
            } else {
                bindTargets.push_back(nullptr);
            }
        } else {
            bindTargets.push_back(nullptr);
        }
    }, ^(const MachOAnalyzer::BindTargetInfo& info, bool& stop) {
    });

    if ( diag.hasError() )
        return;

    // Find the classlist and see which entries are binds to self
    uint64_t classListRuntimeOffset;
    uint64_t classListSize;
    bool foundSection = ((const Header*)ma)->findObjCDataSection("__objc_classlist", classListRuntimeOffset, classListSize);
    if ( !foundSection )
        return;

    const uint64_t ptrSize = ma->pointerSize();
    if ( (classListSize % ptrSize) != 0 ) {
        diag.error("Invalid objc class section size");
        return;
    }

    uint64_t classListCount = classListSize / ptrSize;
    uint16_t chainedPointerFormat = ma->chainedPointerFormat();

    const uint8_t* arrayBase = (uint8_t*)ma + classListRuntimeOffset;
    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        for ( uint64_t i = 0; i != classListCount; ++i ) {
            const PtrTy* classListEntry = (PtrTy*)(arrayBase + (i * sizeof(PtrTy)));

            // Add the class to the patch list if its a bind to self
            const void* classPtr = nullptr;
            {
                const auto* classFixup = (const dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)classListEntry;
                uint32_t bindOrdinal = 0;
                int64_t unusedAddend = 0;
                if ( classFixup->isBind(chainedPointerFormat, bindOrdinal, unusedAddend) ) {
                    if ( bindOrdinal < bindTargets.count() ) {
                        classPtr = bindTargets[bindOrdinal];
                        // Only non-null entries will be binds to self
                        if ( classPtr != nullptr )
                            classPointers.insert({ classPtr, true });
                    }
                }
            }

            // Add the metaclass to the patch list if its a bind to self
            if ( classPtr != nullptr ) {
                // The metaclass is the class ISA, which  is the first field of the class
                const auto* metaclassFixup = (const dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)classPtr;
                uint32_t bindOrdinal = 0;
                int64_t unusedAddend = 0;
                if ( metaclassFixup->isBind(chainedPointerFormat, bindOrdinal, unusedAddend) ) {
                    if ( bindOrdinal < bindTargets.count() ) {
                        const void* metaclassPtr = bindTargets[bindOrdinal];
                        // Only non-null entries will be binds to self
                        if ( metaclassPtr != nullptr )
                            classPointers.insert({ metaclassPtr, true });
                    }
                }
            }
        }
    } else {
        typedef uint32_t PtrTy;
        for ( uint64_t i = 0; i != classListCount; ++i ) {
            const PtrTy* classListEntry = (PtrTy*)(arrayBase + (i * sizeof(PtrTy)));

            // Add the class to the patch list if its a bind to self
            const void* classPtr = nullptr;
            {
                const auto* classFixup = (const dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)classListEntry;
                uint32_t bindOrdinal = 0;
                int64_t unusedAddend = 0;
                if ( classFixup->isBind(chainedPointerFormat, bindOrdinal, unusedAddend) ) {
                    if ( bindOrdinal < bindTargets.count() ) {
                        classPtr = bindTargets[bindOrdinal];
                        // Only non-null entries will be binds to self
                        if ( classPtr != nullptr )
                            classPointers.insert({ classPtr, true });
                    }
                }
            }

            // Add the metaclass to the patch list if its a bind to self
            if ( classPtr != nullptr ) {
                // The metaclass is the class ISA, which  is the first field of the class
                const auto* metaclassFixup = (const dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)classPtr;
                uint32_t bindOrdinal = 0;
                int64_t unusedAddend = 0;
                if ( metaclassFixup->isBind(chainedPointerFormat, bindOrdinal, unusedAddend) ) {
                    if ( bindOrdinal < bindTargets.count() ) {
                        const void* metaclassPtr = bindTargets[bindOrdinal];
                        // Only non-null entries will be binds to self
                        if ( metaclassPtr != nullptr )
                            classPointers.insert({ metaclassPtr, true });
                    }
                }
            }
        }
    }
}

// A singleton object can only be patched if it matches the layout/authentication expected by the patcher
// This finds all eligible singleton classes
static void getSingletonPatches(const Header* hdr, PointerSet& objectPointers)
{
    hdr->forEachSingletonPatch(^(uint64_t runtimeOffset) {
        void* value = (uint8_t*)hdr + runtimeOffset;
        objectPointers.insert({ value, true });
    });
}

// Feature flag.  Enable this once we have ld64-804 everywhere
static const bool enableObjCPatching = true;

static const bool enableSingletonPatching = true;

static bool isEligibleForObjCPatching(RuntimeState& state, uint32_t indexOfOverriddenCachedDylib)
{
    const char* path = state.config.dyldCache.addr->getIndexedImagePath(indexOfOverriddenCachedDylib);
    if ( path == nullptr )
        return false;

    // Some dylibs put data next to their classes.  Eg, libdispatch puts a vtable before the class.  We can't
    // make objc patching work in these cases
    if ( strstr(path, "libdispatch.dylib") != nullptr )
        return false;
    if ( strstr(path, "libxpc.dylib") != nullptr )
        return false;
    if ( strcmp(path, "/usr/lib/libodmodule.dylib") == 0 )
        return false;
    if ( strcmp(path, "/usr/lib/log/liblog_odtypes.dylib") == 0 )
        return false;

    return true;
}

#endif // SUPPORT_VM_LAYOUT

const Loader::DylibPatch* JustInTimeLoader::makePatchTable(RuntimeState& state, uint32_t indexOfOverriddenCachedDylib) const
{
    static const bool extra = false;

    const PatchTable& patchTable = state.config.dyldCache.patchTable;
    assert(patchTable.hasValue());
    //const DyldSharedCache* dyldCache = state.config.dyldCache.addr;
    //assert(dyldCache != nullptr);

    if ( extra )
        state.log("Found %s overrides dyld cache index 0x%04X\n", this->path(state), indexOfOverriddenCachedDylib);
    uint32_t patchCount = patchTable.patchableExportCount(indexOfOverriddenCachedDylib);
    if ( patchCount != 0 ) {
        DylibPatch*     table       = (DylibPatch*)state.persistentAllocator.malloc(sizeof(DylibPatch) * (patchCount + 1));;
        __block uint32_t patchIndex = 0;

#if SUPPORT_VM_LAYOUT
        const uint8_t*  thisAddress = (uint8_t*)(this->loadAddress(state));
        const uint8_t*  cacheDylibAddress = (uint8_t*)state.config.dyldCache.addr->getIndexedImageEntry(indexOfOverriddenCachedDylib);

        __block PointerSet eligibleClasses;
        // The cache builder doesn't analyze objc classes as isEligibleForObjCPatching() relies on
        // parsing the on-disk chained fixup format
        if ( isEligibleForObjCPatching(state, indexOfOverriddenCachedDylib) )
            getObjCPatchClasses(this->analyzer(), eligibleClasses);

        __block PointerSet eligibleSingletons;
        getSingletonPatches(((const Header*)this->analyzer()), eligibleSingletons);

        patchTable.forEachPatchableExport(indexOfOverriddenCachedDylib, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                                          PatchKind patchKind) {
            Diagnostics    exportDiag;
            ResolvedSymbol foundSymbolInfo;
            if ( this->hasExportedSymbol(exportDiag, state, exportName, staticLink, skipResolver, &foundSymbolInfo) ) {
                if ( extra )
                    state.log("   will patch cache uses of '%s' %s\n", exportName, PatchTable::patchKindName(patchKind));
                const dyld3::MachOAnalyzer* implMA  = (const dyld3::MachOAnalyzer*)foundSymbolInfo.targetLoader->loadAddress(state);
                uint8_t* newImplAddress = (uint8_t*)implMA + foundSymbolInfo.targetRuntimeOffset;

                bool foundUsableObjCClass = false;
                bool foundSingletonObject = false;
                switch ( patchKind ) {
                    case PatchKind::regular:
                        break;
                    case PatchKind::cfObj2: {
                        if ( !enableSingletonPatching )
                            break;

                        if ( eligibleSingletons.find(newImplAddress) == eligibleSingletons.end() )
                            break;

                        const dyld3::MachOAnalyzer* cacheMA = (const dyld3::MachOAnalyzer*)cacheDylibAddress;
                        const uint8_t* cacheImpl = (uint8_t*)cacheMA + dylibVMOffsetOfImpl;
                        state.patchedSingletons.push_back({ (uintptr_t)cacheImpl, (uintptr_t)newImplAddress });
                        foundSingletonObject = true;
                        break;
                    }
                    case PatchKind::objcClass: {
                        // Check if we can use ObjC patching.  For now this is only for non-swift classes
                        if ( !enableObjCPatching )
                            break;

                        if ( eligibleClasses.find(newImplAddress) == eligibleClasses.end() )
                            break;

                        const dyld3::MachOAnalyzer* cacheMA = (const dyld3::MachOAnalyzer*)cacheDylibAddress;
                        const uint8_t* cacheImpl = (uint8_t*)cacheMA + dylibVMOffsetOfImpl;

                        if ( implMA->isSwiftClass(newImplAddress) )
                            break;

                        if ( cacheMA->isSwiftClass(cacheImpl) )
                            break;

                        // Interpose so that if anyone tries to bind to the class in the root, then they'll instead bind
                        // to the class in the shared cache
                        state.patchedObjCClasses.push_back({ (uintptr_t)cacheImpl, (uintptr_t)newImplAddress });
                        state.objcReplacementClasses.push_back({ cacheMA, (uintptr_t)cacheImpl, implMA, (uintptr_t)newImplAddress });
                        foundUsableObjCClass = true;
                        break;
                    }
                }

                if ( foundUsableObjCClass ) {
                    table[patchIndex].overrideOffsetOfImpl = DylibPatch::objcClass;
                } else if ( foundSingletonObject ) {
                    table[patchIndex].overrideOffsetOfImpl = DylibPatch::singleton;
                } else {
                    // note: we are saving a signed 64-bit offset to the impl.  This is to support re-exported symbols
                    table[patchIndex].overrideOffsetOfImpl = newImplAddress - thisAddress;
                }
            }
            else {
                if ( extra )
                    state.log("   override missing '%s', so uses will be patched to NULL\n", exportName);
                table[patchIndex].overrideOffsetOfImpl = DylibPatch::missingSymbol;
            }
            ++patchIndex;
        });
        // mark end of table
        table[patchIndex].overrideOffsetOfImpl = DylibPatch::endOfPatchTable;
        // record in Loader
        return table;
#else
        CacheVMAddress thisVMAddr(((const Header*)this->mf(state))->preferredLoadAddress());

        // The cache builder doesn't lay out dylibs in VM layout, so we need to use VMAddr/VMOffset everywhere
        patchTable.forEachPatchableExport(indexOfOverriddenCachedDylib, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                                          PatchKind patchKind) {
            Diagnostics    exportDiag;
            ResolvedSymbol foundSymbolInfo;
            if ( this->hasExportedSymbol(exportDiag, state, exportName, staticLink, skipResolver, &foundSymbolInfo) ) {
                if ( extra )
                    state.log("   will patch cache uses of '%s' %s\n", exportName, PatchTable::patchKindName(patchKind));
                CacheVMAddress implBaseVMAddr(((const Header*)foundSymbolInfo.targetLoader->mf(state))->preferredLoadAddress());
                CacheVMAddress newImplVMAddr = implBaseVMAddr + VMOffset(foundSymbolInfo.targetRuntimeOffset);

                // note: we are saving a signed 64-bit offset to the impl.  This is to support re-exported symbols
                VMOffset offsetToImpl = newImplVMAddr - thisVMAddr;
                table[patchIndex].overrideOffsetOfImpl = offsetToImpl.rawValue();
            }
            else {
                if ( extra )
                    state.log("   override missing '%s', so uses will be patched to NULL\n", exportName);
                table[patchIndex].overrideOffsetOfImpl = DylibPatch::missingSymbol;
            }
            ++patchIndex;
        });
        // mark end of table
        table[patchIndex].overrideOffsetOfImpl = DylibPatch::endOfPatchTable;
        // record in Loader
        return table;
#endif // SUPPORT_VM_LAYOUT
    }
    return nullptr;
}

void JustInTimeLoader::loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options)
{
    if ( dependentsSet )
        return;

    // add first level of dependents
    __block int                 depIndex = 0;
    const mach_o::MachOFileRef& mf  = this->mappedAddress;
    const Header*               hdr = (const Header*)(mf); // Better way?
    hdr->forEachLinkedDylib(^(const char* loadPath, LinkedDylibAttributes depAttrs, Version32 compatVersion, Version32 curVersion, bool synthesizedLink, bool& stop) {
        // fix illegal combinations of dylib attributes
        if ( depAttrs.reExport && depAttrs.delayInit )
            depAttrs.delayInit = false;
        if ( depAttrs.reExport && depAttrs.weakLink )
            depAttrs.weakLink = false;
        if ( !this->allDepsAreNormal )
            dependentAttrs(depIndex) = depAttrs;

        // If this is a shared cache JITLoader then there's likely a root installed and we
        // had to invalidate the prebuilt loaders.  This shared cache dylib may have weakly linked
        // something outside the cache, and the cache builder would break that weak edge.  We
        // want to mimic that behaviour to ensure consistency
        if ( this->dylibInDyldCache && depAttrs.weakLink ) {
            // FIXME: Could we ever not have a cache here, given that we aren't an app loader?
            const DyldSharedCache* cache = state.config.dyldCache.addr;
            __block uint32_t unusedDylibInCacheIndex;
            if ( (cache != nullptr) && !state.config.dyldCache.indexOfPath(loadPath, unusedDylibInCacheIndex) ) {
                if ( state.config.log.loaders )
                    state.log("Skipping shared cache weak-linked dylib '%s' from '%s'\n", loadPath, this->path(state));
                dependents[depIndex] = nullptr;
                depIndex++;
                return;
            }
        }

        const Loader* depLoader = nullptr;
        // for absolute paths, do a quick check if this is already loaded with exact match
        if ( loadPath[0] == '/' ) {
            for ( const Loader* ldr : state.loaded ) {
                if ( ldr->matchesPath(state, loadPath) ) {
                    depLoader = ldr;
                    break;
                }
            }
        }
        if ( depLoader == nullptr ) {
            // first load, so do full search
            LoadChain   nextChain { options.rpathStack, this };
            Diagnostics depDiag;
            LoadOptions depOptions  = options;
            depOptions.requestorNeedsFallbacks = this->pre2022Binary;
            depOptions.rpathStack   = &nextChain;
            depOptions.canBeMissing = depAttrs.weakLink;
            depLoader               = options.finder ? options.finder(depDiag, state.config.process.platform, loadPath, depOptions) : getLoader(depDiag, state, loadPath, depOptions);
            if ( depDiag.hasError() ) {
                char  fromUuidStr[64];
                this->getUuidStr(fromUuidStr);
                // rdar://15648948 (On fatal errors, check binary's min-OS version and note if from the future)
                Diagnostics tooNewBinaryDiag;
                this->tooNewErrorAddendum(tooNewBinaryDiag, state);
                diag.error("Library not loaded: %s\n  Referenced from: <%s> %s%s\n  Reason: %s",
                           loadPath, fromUuidStr, this->path(state), tooNewBinaryDiag.errorMessageCStr(), depDiag.errorMessageCStr());
#if BUILDING_DYLD
                if ( options.launching ) {
                    state.setLaunchMissingDylib(loadPath, this->path(state));
                }
#endif
                stop = true;
            }
        }
        dependents[depIndex] = (Loader*)depLoader;
        depIndex++;
    });
    dependentsSet = true;
    if ( diag.hasError() )
        return;

    // breadth first recurse
    LoadChain   nextChain { options.rpathStack, this };
    LoadOptions depOptions = options;
    depOptions.rpathStack  = &nextChain;
    for ( depIndex = 0; depIndex < this->depCount; ++depIndex ) {
        if ( Loader* depLoader = dependents[depIndex] ) {
            depLoader->loadDependents(diag, state, depOptions);
        }
    }

    // if this image overrides something in the dyld cache, build a table of its patches for use by other dylibs later
    if ( this->overridesCache ) {
        this->overridePatches = makePatchTable(state, this->overrideIndex);

        // Also build patches for overrides of unzippered twins
        // The above case handled an iOSMac dylib rooting an iOSMac unzippered twin.  This handles the iOSMac dylib
        // overriding the macOS unzippered twin
        this->overridePatchesCatalystMacTwin = nullptr;
        if ( state.config.process.catalystRuntime ) {
            // Find the macOS twin overridden index
            uint16_t macOSTwinIndex = Loader::indexOfUnzipperedTwin(state, this->overrideIndex);
            if ( macOSTwinIndex != kNoUnzipperedTwin )
                this->overridePatchesCatalystMacTwin = makePatchTable(state, macOSTwinIndex);
        }
    }
}

uint32_t JustInTimeLoader::dependentCount() const
{
    return this->depCount;
}

JustInTimeLoader::LinkedDylibAttributes& JustInTimeLoader::dependentAttrs(uint32_t depIndex)
{
    assert(depIndex < this->depCount);
    assert(!this->allDepsAreNormal);

    // Dependent kinds are after the dependent loaders
    uint8_t* firstDepKind = (uint8_t*)&dependents[this->depCount];
    return ((JustInTimeLoader::LinkedDylibAttributes*)firstDepKind)[depIndex];
}

Loader* JustInTimeLoader::dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* depAttrs) const
{
    assert(depIndex < this->depCount);
    if ( depAttrs != nullptr ) {
        if ( this->allDepsAreNormal )
            *depAttrs = LinkedDylibAttributes::regular;
        else
            *depAttrs = ((JustInTimeLoader*)this)->dependentAttrs(depIndex);
    }

    return dependents[depIndex];
}

bool JustInTimeLoader::getExportsTrie(uint64_t& runtimeOffset, uint32_t& size) const
{
    if ( this->exportsTrieRuntimeOffset != 0 ) {
        runtimeOffset = this->exportsTrieRuntimeOffset;
        size          = this->exportsTrieSize;
        return true;
    }
    return false;
}

bool JustInTimeLoader::hiddenFromFlat(bool forceGlobal) const
{
    if ( forceGlobal )
        this->hidden = false;
    return this->hidden;
}

bool JustInTimeLoader::representsCachedDylibIndex(uint16_t dylibIndex) const
{
    // check if this is an override of the specified cached dylib
    if ( this->overridesCache && (this->overrideIndex == dylibIndex) )
        return true;

    // check if this is the specified dylib in the cache
    if ( this->dylibInDyldCache && (this->ref.index == dylibIndex) )
        return true;

    return false;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void JustInTimeLoader::logFixup(RuntimeState& state, uint64_t fixupLocRuntimeOffset, uintptr_t newValue, PointerMetaData pmd, const Loader::ResolvedSymbol& target) const
{
    const MachOAnalyzer* ma       = this->analyzer();
    uintptr_t*           fixupLoc = (uintptr_t*)((uint8_t*)ma + fixupLocRuntimeOffset);
    switch ( target.kind ) {
        case Loader::ResolvedSymbol::Kind::rebase:
#if BUILDING_DYLD && __has_feature(ptrauth_calls)
            if ( pmd.authenticated )
                state.log("rebase: *0x%012lX = 0x%012lX (*%s+0x%012lX = 0x%012lX+0x%012lX) (JOP: diversity=0x%04X, addr-div=%d, key=%s)\n",
                          (long)fixupLoc, newValue,
                          this->leafName(state), (long)fixupLocRuntimeOffset,
                          (uintptr_t)ma, (long)target.targetRuntimeOffset,
                          pmd.diversity, pmd.usesAddrDiversity, MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::keyName(pmd.key));
            else
#endif // BUILDING_DYLD && __has_feature(ptrauth_calls)
                state.log("rebase: *0x%012lX = 0x%012lX (*%s+0x%012lX = 0x%012lX+0x%012lX)\n",
                          (long)fixupLoc, newValue,
                          this->leafName(state), (long)fixupLocRuntimeOffset,
                          (uintptr_t)ma, (long)target.targetRuntimeOffset);
            break;
        case Loader::ResolvedSymbol::Kind::bindToImage:
#if BUILDING_DYLD && __has_feature(ptrauth_calls)
            if ( pmd.authenticated )
                state.log("bind:   *0x%012lX = 0x%012lX (*%s+0x%012lX = %s/%s) (JOP: diversity=0x%04X, addr-div=%d, key=%s)\n",
                          (long)fixupLoc, newValue,
                          this->leafName(state), (long)fixupLocRuntimeOffset,
                          target.targetLoader->leafName(state), target.targetSymbolName,
                          pmd.diversity, pmd.usesAddrDiversity, MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::keyName(pmd.key));
            else
#endif // BUILDING_DYLD && __has_feature(ptrauth_calls)
                state.log("bind:   *0x%012lX = 0x%012lX (*%s+0x%012lX = %s/%s)\n",
                          (long)fixupLoc, newValue,
                          this->leafName(state), (long)fixupLocRuntimeOffset,
                          target.targetLoader->leafName(state), target.targetSymbolName);
            break;
        case Loader::ResolvedSymbol::Kind::bindAbsolute:
            state.log("bind:   *0x%012lX = 0x%012lX (*%s+0x%012lX = 0x%012lX(%s))\n",
                      (long)fixupLoc, newValue,
                      this->leafName(state), (long)fixupLocRuntimeOffset,
                      (long)target.targetRuntimeOffset, target.targetSymbolName);
            break;
    }
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

bool JustInTimeLoader::overridesDylibInCache(const DylibPatch*& patchTable, uint16_t& cacheDylibOverriddenIndex) const
{
    if ( !this->overridesCache )
        return false;

    patchTable                = this->overridePatches;
    cacheDylibOverriddenIndex = this->overrideIndex;
    return true;
}

void JustInTimeLoader::withLayout(Diagnostics &diag, const RuntimeState& state,
                        void (^callback)(const mach_o::Layout &layout)) const
{
#if SUPPORT_VM_LAYOUT
    this->analyzer()->withVMLayout(diag, callback);
#else
    // In the cache builder, we must have set a layout if this is a cache dylib
    if ( this->dylibInDyldCache ) {
        assert(this->nonRuntimeLayout != nullptr);
        callback(*this->nonRuntimeLayout);
        return;
    }

    // Not in the cache, but the cache builder never uses MachOAnalyzer, so use the MachOFile layout
    mach_o::MachOFileRef fileRef = this->mf(state);
    fileRef->withFileLayout(diag, callback);
#endif // SUPPORT_VM_LAYOUT
}

bool JustInTimeLoader::dyldDoesObjCFixups() const
{
    // JustInTimeLoaders do not do objc fixups, except for dylibs in dyld cache (which we fixed up at cache build time)
    return this->dylibInDyldCache;
}

const SectionLocations* JustInTimeLoader::getSectionLocations() const
{
    return &this->sectionLocations;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void JustInTimeLoader::handleStrongWeakDefOverrides(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst)
{
    CacheWeakDefOverride cacheWeakDefFixup = ^(uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const ResolvedSymbol& target) {
        JustInTimeLoader::cacheWeakDefFixup(state, cacheDataConst, cachedDylibIndex, cachedDylibVMOffset, target);
    };

    // Find an on-disk dylib with weak-defs, if one exists.  If we find one, look for strong overrides of all the special weak symbols
    // On all platforms we look in the main executable for strong symbols
    const Loader* weakDefLoader = nullptr;
    if ( state.mainExecutableLoader->hasWeakDefs )
        weakDefLoader = state.mainExecutableLoader;

    // On macOS, we also allow check on-disk dylibs for strong symbols
#if TARGET_OS_OSX
    if ( weakDefLoader == nullptr ) {
        for (const Loader* loader : state.loaded) {
            if ( !loader->dylibInDyldCache ) {
                const dyld3::MachOAnalyzer *ma = loader->analyzer(state);
                if ( loader->hasWeakDefs && ma->hasOpcodeFixups() ) {
                    weakDefLoader = loader;
                    break;
                }
            }
        }
    }
#endif // TARGET_OS_OSX

    if ( weakDefLoader != nullptr ) {
        MachOAnalyzer::forEachTreatAsWeakDef(^(const char* symbolName) {
            Diagnostics weakBindDiag; // ignore failures here
            (void)weakDefLoader->resolveSymbol(weakBindDiag, state, BIND_SPECIAL_DYLIB_WEAK_LOOKUP, symbolName, true, false, cacheWeakDefFixup);
        });
    }
}

void JustInTimeLoader::cacheWeakDefFixup(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst,
                                         uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const ResolvedSymbol& target) {
    const DyldSharedCache* dyldcache = state.config.dyldCache.addr;

    //state.log("cache patch: dylibIndex=%d, exportCacheOffset=0x%08X, target=%s\n", cachedDylibIndex, exportCacheOffset,target.targetSymbolName);
    dyldcache->forEachPatchableUseOfExport(cachedDylibIndex, cachedDylibVMOffset,
                                           ^(uint64_t cacheVMOffset,
                                             dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                             bool isWeakImport) {
        uintptr_t* loc     = (uintptr_t*)(((uint8_t*)dyldcache) + cacheVMOffset);
        uintptr_t  newImpl = (uintptr_t)(Loader::resolvedAddress(state, target) + addend);
#if __has_feature(ptrauth_calls)
        if ( pmd.authenticated )
            newImpl = MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newImpl, loc, pmd.usesAddrDiversity, pmd.diversity, pmd.key);
#endif
        // ignore duplicate patch entries
        if ( *loc != newImpl ) {
            cacheDataConst.makeWriteable();
            if ( state.config.log.fixups )
                state.log("cache patch: %p = 0x%0lX\n", loc, newImpl);
            *loc = newImpl;
        }
    });
}

void JustInTimeLoader::applyFixups(Diagnostics& diag, RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst, bool allowLazyBinds,
                                   lsl::Vector<PseudoDylibSymbolToMaterialize>* materializingSymbols) const
{
    //state.log("applyFixups: %s\n", this->path(state));

    // check if we need to patch the cache
    this->applyFixupsCheckCachePatching(state, cacheDataConst);

    // images in shared cache don't need any more fixups
    if ( this->dylibInDyldCache ) {
        // update any internal pointers to function variants
        this->applyFunctionVariantFixups(diag, state);
#if TARGET_OS_EXCLAVEKIT
        // exclavekit is special in that page-in linking for for the dyld cache can be disabled
        if ( state.config.process.sharedCachePageInLinking )
#endif
        {
            this->fixUpsApplied = true;
            return;
        }
    }

    if ( this->pd ) {
        // FIXME: Do we need to handle anything here? We probably do if we want
        // to support things like extending the main executable with JIT'd code.
        return;
    }

    CacheWeakDefOverride cacheWeakDefFixup = ^(uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const ResolvedSymbol& target) {
        JustInTimeLoader::cacheWeakDefFixup(state, cacheDataConst, cachedDylibIndex, cachedDylibVMOffset, target);
    };

    // build targets table
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, bindTargets, 512);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, overrideTargetAddrs, 32);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(MissingFlatLazySymbol, missingFlatLazySymbols, 4);
    this->forEachBindTarget(diag, state, cacheWeakDefFixup, allowLazyBinds, ^(const ResolvedSymbol& target, bool& stop) {
        const void* targetAddr = (const void*)Loader::interpose(state, Loader::resolvedAddress(state, target), this);
        if ( state.config.log.fixups ) {
            const char* targetLoaderName = target.targetLoader ? target.targetLoader->leafName(state) : "<none>";
            state.log("<%s/bind#%llu> -> %p (%s/%s)\n", this->leafName(state), bindTargets.count(), targetAddr, targetLoaderName, target.targetSymbolName);
        }

        // Record missing flat-namespace lazy symbols
        if ( target.isMissingFlatLazy )
            missingFlatLazySymbols.push_back({ target.targetSymbolName, (uint32_t)bindTargets.count() });
        // Record pseudo dylib symbols we need to materialize
        if ( target.isMaterializing && materializingSymbols )
            materializingSymbols->push_back({ target.targetLoader, target.targetSymbolName });

        bindTargets.push_back(targetAddr);
    }, ^(const ResolvedSymbol& target, bool& stop) {
        // Missing weak binds need placeholders to make the target indices line up, but we should otherwise ignore them
        if ( (target.kind == Loader::ResolvedSymbol::Kind::bindToImage) && (target.targetLoader == nullptr) ) {
            if ( state.config.log.fixups )
                state.log("<%s/bind#%llu> -> missing-weak-bind (%s)\n", this->leafName(state), overrideTargetAddrs.count(), target.targetSymbolName);

            overrideTargetAddrs.push_back((const void*)UINTPTR_MAX);
        } else {
            const void* targetAddr = (const void*)Loader::interpose(state, Loader::resolvedAddress(state, target), this);
            if ( state.config.log.fixups ) {
                const char* targetLoaderName = target.targetLoader ? target.targetLoader->leafName(state) : "<none>";
                state.log("<%s/bind#%llu> -> %p (%s/%s)\n", this->leafName(state), overrideTargetAddrs.count(), targetAddr, targetLoaderName, target.targetSymbolName);
            }

            // Record missing flat-namespace lazy symbols
            if ( target.isMissingFlatLazy )
                missingFlatLazySymbols.push_back({ target.targetSymbolName, (uint32_t)overrideTargetAddrs.count() });
            overrideTargetAddrs.push_back(targetAddr);
        }
    });
    if ( diag.hasError() )
        return;

    // do fixups using bind targets table
    this->applyFixupsGeneric(diag, state, this->sliceOffset, bindTargets, overrideTargetAddrs, true, missingFlatLazySymbols);

#if SUPPPORT_PRE_LC_MAIN
    // some old macOS games need __dyld section set up in dylibs too.  Main executable with __dyld section set up in prepare()
    if ( (state.config.process.platform == Platform::macOS) && (state.libdyldLoader != nullptr) && (this != state.mainExecutableLoader) ) {
        const MachOAnalyzer* ma = this->analyzer();
        if ( !ma->inDyldCache() ) {
            ((mach_o::Header*)ma)->platformAndVersions().unzip(^(mach_o::PlatformAndVersions pvs) {
                // rdar://84760053 (SEED: Web: Crash in libobjc.A.dylib's load_images when loading certain bundles in Monterey)
                if ( (pvs.platform == Platform::macOS) && (pvs.minOS <= Version32(0x000A0900)) ) {
                    struct DATAdyld { void* dyldLazyBinder; FuncLookup dyldFuncLookup; };
                    uint64_t  sectSize;
                    if ( DATAdyld* dyldSect = (DATAdyld*)ma->findSectionContent("__DATA", "__dyld", sectSize) ) {
                        //state.log("found __dyld section in %s\n", this->path());
                        // dyld and libdyld have not been wired together yet, so peek into libdyld
                        // if libdyld.dylib is a root, it may not have been rebased yet
                        if ( state.libdyldLoader->hasBeenFixedUp(state) ) {
                            const Header*               libdyldHdr    = state.libdyldLoader->header(state);
                            std::span<const uint8_t>    helperSection = libdyldHdr->findSectionContent("__DATA_CONST", "__helper", true/*vm layout*/);
                            if ( helperSection.size() == sizeof(void*) ) {
                                const LibdyldHelperSection* section = (LibdyldHelperSection*)helperSection.data();
                                LibSystemHelpersWrapper  myLibSystemHelpers;
                                myLibSystemHelpers = { &section->helper, &lsl::MemoryManager::memoryManager() };
                                dyldSect->dyldLazyBinder = nullptr;
                                dyldSect->dyldFuncLookup = myLibSystemHelpers.legacyDyldFuncLookup();
                            }
                        }
                    }
                }
            });
        }
    }
#endif

    // update any internal pointers to function variants
    this->applyFunctionVariantFixups(diag, state);

    // mark any __DATA_CONST segments read-only
    if ( this->hasConstantSegmentsToProtect() )
        this->makeSegmentsReadOnly(state);

    if ( diag.noError() )
        this->fixUpsApplied = true;
}

#if SUPPORT_IMAGE_UNLOADING
void JustInTimeLoader::unmap(RuntimeState& state, bool force) const
{
    if ( this->dylibInDyldCache )
        return;
    if ( this->pd )
        return;
    if ( !force && this->neverUnload )
        state.log("trying to unmap %s\n", this->path(state));
    assert(force || !this->neverUnload);
    size_t vmSize  = (size_t)this->analyzer()->mappedSize();
    void*  vmStart = (void*)(this->loadAddress(state));
    state.config.syscall.munmap(vmStart, vmSize);
    if ( state.config.log.segments )
        state.log("unmapped 0x%012lX->0x%012lX for %s\n", (long)vmStart, (long)vmStart + (long)vmSize, this->path(state));
}
#endif // SUPPORT_IMAGE_UNLOADING

#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

bool JustInTimeLoader::hasBeenFixedUp(RuntimeState&) const
{
    return fixUpsApplied;
}

bool JustInTimeLoader::beginInitializers(RuntimeState&)
{
    // do nothing if already initializers already run
    if ( inited )
        return true;

    // switch to being-inited state
    inited = true;
    return false;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void JustInTimeLoader::runInitializers(RuntimeState& state) const
{
    this->findAndRunAllInitializers(state);
    // FIXME: Should we run "JIT" initializers *after* regular initializers, or
    //        should it be either/or?
    // The main use-case for extending an existing image with JIT'd code is the
    // main executable (for previews), but there may be others.
    // FIXME: Error plumbing?
    if ( pd ) {
        if ( char *errMsg = pd->initialize() ) {
            state.log("error running pseudo-dylib initializers: %s", errMsg);
            pd->disposeString(errMsg);
        }
    }
}
#endif

bool JustInTimeLoader::isDelayInit(RuntimeState&) const
{
    return this->delayInit;
}

void JustInTimeLoader::setDelayInit(RuntimeState&, bool value) const
{
    if ( value ) {
        // "mark" phase
        // if this image has already been initialized, then there is no point in re-evaluting if it is not-delayed
        if ( !inited )
            this->delayInit = value;
    }
    else {
        // "sweep" phase
        this->delayInit = value;
    }
}

////////////////////////  other functions /////////////////////////////////

static bool hasDataConst(const Header* hdr)
{
    __block bool result = false;
    hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.readOnlyData() )
            result = true;
    });
    return result;
}

JustInTimeLoader::JustInTimeLoader(const MachOFile* mh, const Loader::InitialOptions& options,
                                   const FileID& fileID, const mach_o::Layout* layout, bool isPremapped)
    : Loader(options, false, false, 0, isPremapped), mappedAddress((const dyld3::MachOLoaded*)mh), fileIdent(fileID)
{
}

JustInTimeLoader* JustInTimeLoader::make(RuntimeState& state, const MachOFile* mh, const char* path, const FileID& fileID, uint64_t sliceOffset,
                                         bool willNeverUnload, bool leaveMapped, bool overridesCache, uint16_t overridesDylibIndex,
                                         const mach_o::Layout* layout)
{
    //state.log("JustInTimeLoader::make(%s) willNeverUnload=%d\n", path, willNeverUnload);
    // use malloc and placement new to create object big enough for all info
    const Header*          hdr                  = (Header*)mh;
    bool                   allDepsAreNormal     = true;
    uint32_t               depCount             = hdr->linkedDylibCount(&allDepsAreNormal);
    uint32_t               minDepCount          = (depCount ? depCount - 1 : 1);
    size_t                 sizeNeeded           = sizeof(JustInTimeLoader) + (minDepCount * sizeof(AuthLoader)) + (allDepsAreNormal ? 0 : depCount) + strlen(path) + 1;
    void*                  storage              = state.persistentAllocator.malloc(sizeNeeded);
    uuid_t                 uuid;

    Loader::InitialOptions options;
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    options.inDyldCache     = mh->inDyldCache();
#else
    options.inDyldCache     = DyldSharedCache::inDyldCache(state.config.dyldCache.addr, mh);
#endif
    options.hasObjc         = mh->hasObjC();
    options.mayHavePlusLoad = hdr->hasPlusLoadMethod();
    options.roData          = hasDataConst(hdr);
    options.neverUnloaded   = willNeverUnload || overridesCache; // dylibs in cache never unload, be consistent and don't unload roots either
    options.leaveMapped     = leaveMapped;
    options.roObjC          = options.hasObjc && mh->hasConstObjCSection();
    options.pre2022Binary   = !mh->enforceFormat(MachOAnalyzer::Malformed::sdkOnOrAfter2022);
    options.hasUUID         = hdr->getUuid(uuid);
    options.hasWeakDefs     = mh->hasWeakDefs();
    options.hasTLVs         = hdr->hasThreadLocalVariables();
    options.belowLibSystem  = hdr->isDylib() && (strncmp(hdr->installName(), "/usr/lib/system/lib", 19) == 0);
    options.hasFuncVarFixups= hdr->hasFunctionVariantFixups();
    JustInTimeLoader* p     = new (storage) JustInTimeLoader(mh, options, fileID, layout, false);

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    p->nonRuntimeLayout     = layout;
#endif

    // fill in extra data
    p->pathOffset           = sizeof(JustInTimeLoader) + (minDepCount * sizeof(AuthLoader)) + (allDepsAreNormal ? 0 : depCount);
    p->dependentsSet        = false;
    p->fixUpsApplied        = false;
    p->inited               = false;
    p->hidden               = false;
    p->altInstallName       = ((Header*)mh)->isDylib() && (strcmp(((Header*)mh)->installName(), path) != 0);
    p->lateLeaveMapped      = false;
    p->allDepsAreNormal     = allDepsAreNormal;
    p->padding              = 0;
    p->sliceOffset          = sliceOffset;

    if ( options.hasUUID ) {
        memcpy(p->uuid, uuid, sizeof(uuid_t));
    } else {
        // for reproducibility
        bzero(p->uuid, sizeof(uuid_t));
    }

    p->cpusubtype   = mh->cpusubtype;

    parseSectionLocations((const Header*)mh, p->sectionLocations);

    if ( !mh->hasExportTrie(p->exportsTrieRuntimeOffset, p->exportsTrieSize) ) {
        p->exportsTrieRuntimeOffset = 0;
        p->exportsTrieSize          = 0;
    }
    p->overridePatches = nullptr;
    p->pd = nullptr;
    p->overridesCache  = overridesCache;
    p->overrideIndex   = overridesDylibIndex;
    p->depCount        = depCount;
    for ( unsigned i = 0; i < depCount; ++i ) {
        new (&p->dependents[i]) (AuthLoader) { nullptr };
        if ( !allDepsAreNormal )
            p->dependentAttrs(i) = LinkedDylibAttributes::regular; // will be set to correct kind in loadDependents()
    }
    strlcpy(((char*)p) + p->pathOffset, path, PATH_MAX);
    //state.log("JustInTimeLoader::make(%p, %s) => %p\n", ma, path, p);
    p->delayInit       = false;

    state.add(p);
#if BUILDING_DYLD
    if ( overridesCache ) {
        // The only case where a library in the dyld cache overrides another library in the cache is when an unzippered twin overrides its macOS counterpart.
        // We don't want hasOverriddenCachedDylib to be set in such case.
        if ( options.inDyldCache ) {
            state.setHasOverriddenUnzipperedTwin();
        } else {
            state.setHasOverriddenCachedDylib();
        }
    }
    if ( state.config.log.loaders )
        state.log("using JustInTimeLoader %p for %s\n", p, path);
#endif

    return p;
}

void JustInTimeLoader::parseSectionLocations(const Header* hdr, SectionLocations& metadata)
{
    for ( uint32_t i = 0; i < SectionLocations::count; ++i ) {
        metadata.offsets[i] = 0;
        metadata.sizes[i] = 0;
    }

    uint64_t baseAddress = hdr->preferredLoadAddress();
    auto setSectionOffset = ^(uint32_t sectionKind, const Header::SectionInfo& sectInfo) {
        uint64_t sectionOffset = sectInfo.address - baseAddress;
        metadata.offsets[sectionKind] = sectionOffset;
        metadata.sizes[sectionKind] = sectInfo.size;
    };

    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.segmentName == "__TEXT" ) {
            if ( sectInfo.sectionName == "__swift5_protos" )
                setSectionOffset(_dyld_section_location_text_swift5_protos, sectInfo);
            else if ( sectInfo.sectionName == "__swift5_proto" )
                setSectionOffset(_dyld_section_location_text_swift5_proto, sectInfo);
            else if ( sectInfo.sectionName == "__swift5_types" )
                setSectionOffset(_dyld_section_location_text_swift5_types, sectInfo);
            else if ( sectInfo.sectionName == "__swift5_replace" )
                setSectionOffset(_dyld_section_location_text_swift5_replace, sectInfo);
            else if ( sectInfo.sectionName == "__swift5_replac2" )
                setSectionOffset(_dyld_section_location_text_swift5_replace2, sectInfo);
            else if ( sectInfo.sectionName == "__swift5_acfuncs" )
                setSectionOffset(_dyld_section_location_text_swift5_ac_funcs, sectInfo);
            return;
        }

        if ( sectInfo.segmentName.starts_with("__DATA") ) {
            if ( sectInfo.sectionName == "__objc_imageinfo" )
                setSectionOffset(_dyld_section_location_objc_image_info, sectInfo);
            else if ( sectInfo.sectionName == "__objc_selrefs" )
                setSectionOffset(_dyld_section_location_data_sel_refs, sectInfo);
            else if ( sectInfo.sectionName == "__objc_msgrefs" )
                setSectionOffset(_dyld_section_location_data_msg_refs, sectInfo);
            else if ( sectInfo.sectionName == "__objc_classrefs" )
                setSectionOffset(_dyld_section_location_data_class_refs, sectInfo);
            else if ( sectInfo.sectionName == "__objc_superrefs" )
                setSectionOffset(_dyld_section_location_data_super_refs, sectInfo);
            else if ( sectInfo.sectionName == "__objc_protorefs" )
                setSectionOffset(_dyld_section_location_data_protocol_refs, sectInfo);
            else if ( sectInfo.sectionName == "__objc_classlist" )
                setSectionOffset(_dyld_section_location_data_class_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_stublist" )
                setSectionOffset(_dyld_section_location_data_stub_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_nlclslist" )
                setSectionOffset(_dyld_section_location_data_non_lazy_class_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_catlist" )
                setSectionOffset(_dyld_section_location_data_category_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_catlist2" )
                setSectionOffset(_dyld_section_location_data_category_list2, sectInfo);
            else if ( sectInfo.sectionName == "__objc_nlcatlist" )
                setSectionOffset(_dyld_section_location_data_non_lazy_category_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_protolist" )
                setSectionOffset(_dyld_section_location_data_protocol_list, sectInfo);
            else if ( sectInfo.sectionName == "__objc_fork_ok" )
                setSectionOffset(_dyld_section_location_data_objc_fork_ok, sectInfo);
            else if ( sectInfo.sectionName == "__objc_rawisa" )
                setSectionOffset(_dyld_section_location_data_raw_isa, sectInfo);
            return;
        }
    });
}

Loader::FileValidationInfo JustInTimeLoader::getFileValidationInfo(RuntimeState& state) const
{
    __block FileValidationInfo result;
    // set checkInodeMtime and checkCdHash to false by default
    bzero(&result, sizeof(FileValidationInfo));
    if ( this->fileIdent.valid() ) {
        result.checkInodeMtime = true;
        result.sliceOffset     = this->sliceOffset;
        result.inode           = this->fileIdent.inode();
        result.mtime           = this->fileIdent.mtime();
    }
    if ( !this->dylibInDyldCache ) {
#if SUPPORT_VM_LAYOUT
        const MachOAnalyzer* ma = this->analyzer();
        ma->forEachCDHash(^(const uint8_t aCdHash[20]) {
            result.checkCdHash = true;
            memcpy(&result.cdHash[0], &aCdHash[0], 20);
        });
#else
        uint32_t codeSignFileOffset = 0;
        uint32_t codeSignFileSize   = 0;
        const mach_o::MachOFileRef& ref = this->mappedAddress;
        if ( ((const Header*)ref)->hasCodeSignature(codeSignFileOffset, codeSignFileSize) ) {
            ref->forEachCDHashOfCodeSignature(ref.getOffsetInToFile(codeSignFileOffset), codeSignFileSize,
                                              ^(const uint8_t aCdHash[20]) {
                result.checkCdHash = true;
                memcpy(&result.cdHash[0], &aCdHash[0], 20);
            });
        }
#endif

#if !SUPPORT_CREATING_PREMAPPEDLOADERS
        result.deviceID = this->fileIdent.device();
#endif // !SUPPORT_CREATING_PREMAPPEDLOADERS
    }
    return result;
}

const Loader::DylibPatch* JustInTimeLoader::getCatalystMacTwinPatches() const
{
    return this->overridePatchesCatalystMacTwin;
}

void JustInTimeLoader::withRegions(const MachOFile* mf, void (^callback)(const Array<Region>& regions))
{
    const Header* hdr   = (const Header*)mf;
    uint64_t vmTextAddr = hdr->preferredLoadAddress();
    uint32_t segCount   = hdr->segmentCount();
    STACK_ALLOC_ARRAY(Region, regions, segCount * 2);
    hdr->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
        Region region;
        if ( !segInfo.hasZeroFill() || (segInfo.fileSize != 0) ) {
            // add region for content that is not wholely zerofill
            region.vmOffset     = segInfo.vmaddr - vmTextAddr;
            region.perms        = segInfo.initProt;
            region.readOnlyData = segInfo.readOnlyData();
            region.isZeroFill   = false;
            region.fileOffset   = (uint32_t)segInfo.fileOffset;
            region.fileSize     = (uint32_t)segInfo.fileSize;
            // special case LINKEDIT, the vmsize is often larger than the filesize
            // but we need to mmap off end of file, otherwise we may have r/w pages at end
            if ( (segInfo.segmentIndex == segCount - 1) && (segInfo.initProt == VM_PROT_READ) ) {
                region.fileSize = (uint32_t)segInfo.vmsize;
            }
            regions.push_back(region);
        }
        if ( segInfo.hasZeroFill() ) {
            Region fill;
            fill.vmOffset     = segInfo.vmaddr - vmTextAddr + segInfo.fileSize;
            fill.perms        = segInfo.initProt;
            fill.readOnlyData = false;
            fill.isZeroFill   = true;
            fill.fileOffset   = 0;
            fill.fileSize     = (uint32_t)(segInfo.vmsize - segInfo.fileSize);
            regions.push_back(fill);
        }
    });
    callback(regions);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
JustInTimeLoader* JustInTimeLoader::makeJustInTimeLoaderDyldCache(RuntimeState& state, const MachOFile* mf, const char* installName,
                                                                  uint32_t dylibCacheIndex, const FileID& fileID, bool catalystTwin, uint32_t twinIndex,
                                                                  const mach_o::Layout* layout)
{
    bool cacheOverride = catalystTwin;
    JustInTimeLoader* jitLoader = JustInTimeLoader::make(state, mf, installName, fileID, 0, true, false, cacheOverride, twinIndex, layout);
    jitLoader->ref.app   = false;
    jitLoader->ref.index = dylibCacheIndex;
    return jitLoader;
}
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

#if BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_UNIT_TESTS
JustInTimeLoader* JustInTimeLoader::makeJustInTimeLoader(RuntimeState& state, const MachOFile* mf, const char* installName)
{
    const mach_o::Layout* layout = nullptr;
    JustInTimeLoader* jitLoader = JustInTimeLoader::make(state, mf, installName, FileID::none(), 0, true, false, false, 0, layout);
    return jitLoader;
}
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

Loader* JustInTimeLoader::makeJustInTimeLoaderDyldCache(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options, uint32_t dylibCacheIndex,
                                                        const mach_o::Layout* layout)
{
    uint64_t mtime = 0;
    uint64_t inode = 0;
    const Header* cacheMH = (const Header*)state.config.dyldCache.getIndexedImageEntry(dylibCacheIndex, mtime, inode);

    bool fileIDValid = state.config.dyldCache.dylibsExpectedOnDisk;
    uint64_t device = 0;

#if TARGET_OS_SIMULATOR
    if ( fileIDValid ) {
        // We need to get the simulator dylib device ID.  This is required if we later want to match this loader by fileID
        device = state.config.process.dyldSimFSID;
    }
#endif

    UUID fsUUID;
    FileID fileID(inode, device, mtime, fileIDValid);
    if ( !cacheMH->loadableIntoProcess(state.config.process.platform, loadPath, state.config.security.isInternalOS) ) {
        diag.error("wrong platform to load into process");
        return nullptr;
    }
    bool     catalystOverrideOfMacSide = false;
    uint32_t catalystOverideDylibIndex = 0;
    if ( strncmp(loadPath, "/System/iOSSupport/", 19) == 0 ) {
        uint32_t macIndex;
        if ( state.config.dyldCache.indexOfPath(&loadPath[18], macIndex) ) {
            catalystOverrideOfMacSide = true;
            catalystOverideDylibIndex = macIndex;
        }
    }
    JustInTimeLoader* result =
        JustInTimeLoader::make(state, (const MachOFile*)cacheMH, loadPath, fileID, 0, true, false, catalystOverrideOfMacSide, catalystOverideDylibIndex, layout);
    result->ref.index = dylibCacheIndex;
#if BUILDING_DYLD
    if ( state.config.log.segments )
        result->logSegmentsFromSharedCache(state);
    if ( state.config.log.libraries )
        result->logLoad(state, loadPath);
#endif
    return result;
}

#if !SUPPORT_CREATING_PREMAPPEDLOADERS
Loader* JustInTimeLoader::makeJustInTimeLoaderDisk(Diagnostics& diag, RuntimeState& state, const char* loadPath,
                                                   const LoadOptions& options, bool overridesCache, uint32_t overridesCacheIndex,
                                                   const mach_o::Layout* layout)
{
    __block Loader* result          = nullptr;
    bool            checkIfOSBinary = state.config.process.archs->checksOSBinary();

    int      fileDescriptor = -1;
    size_t   mappedSize;
    FileID   fileID = FileID::none();
    bool     isOSBinary = false;
    char realerPath[PATH_MAX];
    if (const void* mapping = state.config.syscall.mapFileReadOnly(diag, loadPath, &fileDescriptor, &mappedSize, &fileID, (checkIfOSBinary ? &isOSBinary : nullptr), realerPath)) {
        uint64_t mhSliceOffset = 0;
        uint64_t sliceSize = 0;
        if ( const MachOFile* mf = MachOFile::compatibleSlice(diag, mhSliceOffset, sliceSize, mapping, mappedSize, loadPath, state.config.process.platform, isOSBinary, *state.config.process.archs, state.config.security.internalInstall) ) {
            // verify the filetype is loadable in this context
            if ( mf->isDylib() ) {
                if ( !options.canBeDylib ) {
                    diag.error("cannot load dylib '%s'", loadPath);
                }
            }
            else if ( mf->isBundle() ) {
                if ( !options.canBeBundle ) {
                    diag.error("cannot link against bundle '%s'", loadPath);
                }
            }
            else if ( mf->isMainExecutable() ) {
                if ( !options.canBeExecutable ) {
                    if ( options.staticLinkage )
                        diag.error("cannot link against a main executable '%s'", loadPath);
                    else
                        diag.error("cannot dlopen a main executable '%s'", loadPath);
                }
            }
            else {
                diag.error("unloadable mach-o file type %d '%s'", mf->filetype, loadPath);
            }

            if ( diag.hasError() ) {
#if !BUILDING_CACHE_BUILDER
                state.config.syscall.unmapFile(mapping, mappedSize);
                ::close(fileDescriptor);
#endif
                return result;
            }
#if !BUILDING_CACHE_BUILDER
            // do a deep inspection of the binary, looking for invalid mach-o constructs
            if ( mach_o::Error err = ((Header*)mf)->valid(mappedSize) ) {
                diag.error("%s", err.message());
                state.config.syscall.unmapFile(mapping, mappedSize);
                ::close(fileDescriptor);
                return result;
            }
#endif
            const MachOAnalyzer* ma          = (MachOAnalyzer*)mf;
            bool                 leaveMapped = options.rtldNoDelete;
            bool                 neverUnload;
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
            // The cache builder only builds executable and shared cache loaders, which are always never unloadable
            neverUnload = true;
#else
            neverUnload = !options.forceUnloadable && (options.launching || ma->neverUnload());
#endif
            uint64_t             vmSpace     = ma->mappedSize();
            FileValidationInfo   fileValidation;

            fileValidation.checkInodeMtime = fileID.valid();
            if ( fileValidation.checkInodeMtime ) {
                fileValidation.inode = fileID.inode();
                fileValidation.mtime = fileID.mtime();
            }
            fileValidation.sliceOffset     = (uint8_t*)mf - (uint8_t*)mapping;

            // Check code signature
            CodeSignatureInFile  codeSignature;
            bool hasCodeSignature = ((const Header*)ma)->hasCodeSignature(codeSignature.fileOffset, codeSignature.size);
#if BUILDING_DYLD
            if ( hasCodeSignature && codeSignature.size != 0 ) {
                uuid_t uuid;
                ((Header*)ma)->getUuid(uuid);
                char uuidStr[64];
                uuidToStr(uuid, uuidStr);
                if ( !state.config.syscall.registerSignature(diag, realerPath, uuidStr, fileDescriptor, fileValidation.sliceOffset, codeSignature.fileOffset, codeSignature.size) ) {
                    state.config.syscall.unmapFile(mapping, mappedSize);
                    ::close(fileDescriptor);
                    return result;
                }

                // Map file again after code signature registration
                state.config.syscall.unmapFile(mapping, mappedSize);
                mapping = state.config.syscall.mmap(nullptr, mappedSize, PROT_READ, MAP_PRIVATE, fileDescriptor, 0);
                if ( mapping == MAP_FAILED ) {
                    diag.error("mmap for %s (size=0x%0lX) failed with errno=%d", loadPath, mappedSize, errno);
                    ::close(fileDescriptor);
                    return result;
                }
                ma = (const MachOAnalyzer*)((uint64_t)mapping + mhSliceOffset);
                if ( ma == nullptr ) {
                    state.config.syscall.unmapFile(mapping, mappedSize);
                    ::close(fileDescriptor);
                    return result;
                }
            }
#endif

            char* canonicalPath = realerPath;
            JustInTimeLoader::withRegions(ma, ^(const Array<Region>& regions) {
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
                // in cache builder, files are already mapped
                (void)vmSpace;
                (void)hasCodeSignature;
                result = JustInTimeLoader::make(state, ma, canonicalPath, FileID::none(), fileValidation.sliceOffset, neverUnload, leaveMapped, overridesCache, overridesCacheIndex, layout);
#else
                if ( const MachOAnalyzer* realMA = Loader::mapSegments(diag, state, canonicalPath, fileDescriptor, vmSpace, codeSignature, hasCodeSignature, regions, neverUnload, false, fileValidation) ) {
                    result = JustInTimeLoader::make(state, realMA, canonicalPath, fileID, fileValidation.sliceOffset, neverUnload, leaveMapped, overridesCache, overridesCacheIndex, layout);
#if BUILDING_DYLD
                    if ( state.config.log.libraries )
                        result->logLoad(state, canonicalPath);
#endif
                    if ( options.rtldLocal )
                        ((JustInTimeLoader*)result)->hidden = true;
                }
#endif
            });
        }
#if !BUILDING_CACHE_BUILDER
        state.config.syscall.unmapFile(mapping, mappedSize);
        ::close(fileDescriptor);
#endif
    }
    return result;
}

Loader* JustInTimeLoader::makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOAnalyzer* mainExe, const char* mainExePath,
                                           const mach_o::Layout* layout)
{
    FileID   mainFileID = FileID::none();
    uint64_t mainSliceOffset = Loader::getOnDiskBinarySliceOffset(state, mainExe, mainExePath);
#if !BUILDING_CACHE_BUILDER
    state.config.syscall.fileExists(mainExePath, &mainFileID);
#endif // !BUILDING_CACHE_BUILDER
    return JustInTimeLoader::make(state, mainExe, mainExePath, mainFileID, mainSliceOffset, true, false, false, 0, layout);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
Loader* JustInTimeLoader::makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOFile* mainExe, const char* mainExePath,
                                           const mach_o::Layout* layout)
{
    FileID   mainFileID = FileID::none();
    uint64_t mainSliceOffset = 0; // FIXME
    return JustInTimeLoader::make(state, mainExe, mainExePath, mainFileID, mainSliceOffset, true, false, false, 0, layout);
}
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

#endif // !SUPPORT_CREATING_PREMAPPEDLOADERS

const Loader* JustInTimeLoader::makePseudoDylibLoader(Diagnostics& diag, RuntimeState &state, const char* path, const LoadOptions& options, const PseudoDylib* pd) {
    const Header* pseudoDylibMH = (const Header*)pd->getAddress();
    FileID fileID = FileID::none();
    if (!pseudoDylibMH->loadableIntoProcess(state.config.process.platform, path)) {
        diag.error("wrong platform to load into process");
        return nullptr;
    }
    JustInTimeLoader* result =
        JustInTimeLoader::make(state, (const MachOFile*)pseudoDylibMH, path, fileID, 0, false, false, false, 0, nullptr);
    result->pd = pd;
    return result;
}

} // namespace
