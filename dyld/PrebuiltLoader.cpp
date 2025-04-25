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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/fcntl.h>

#include "Defines.h"
#include "Header.h"
#include "UUID.h"
#include "Loader.h"
#include "PrebuiltLoader.h"
#include "JustInTimeLoader.h"
#include "BumpAllocator.h"
#include "MachOAnalyzer.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "PrebuiltObjC.h"
#include "PrebuiltSwift.h"
#include "ObjCVisitor.h"
#include "OptimizerObjC.h"
#include "objc-shared-cache.h"

// mach_o
#include "Header.h"

#if SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

// HACK: This is just to avoid building the version in the unit tests
#if BUILDING_CACHE_BUILDER_UNIT_TESTS
#define PREBUILTLOADER_VERSION 0x0
#else
#include "PrebuiltLoader_version.h"
#endif


#define DYLD_CLOSURE_XATTR_NAME "com.apple.dyld"

using dyld3::MachOAnalyzer;
using dyld3::MachOFile;
using dyld3::OverflowSafeArray;
using mach_o::Header;
using mach_o::Platform;
using mach_o::Header;
using mach_o::FunctionVariants;

namespace dyld4 {

class PrebuiltLoader;

//
// MARK: --- PrebuiltLoader::BindTargetRef methods ---
//

PrebuiltLoader::BindTargetRef::BindTargetRef(Diagnostics& diag, const RuntimeState& state, const ResolvedSymbol& targetSymbol)
{
    uint64_t high8;
    uint64_t low54;
    uint64_t low38;
    switch ( targetSymbol.kind ) {
        case ResolvedSymbol::Kind::bindAbsolute: {
            high8      = targetSymbol.targetRuntimeOffset >> 56;
            low54      = targetSymbol.targetRuntimeOffset & 0x003FFFFFFFFFFFFFULL;
            _abs.kind  = Kind::absolute;
            _abs.high8 = high8;
            _abs.low54 = low54;
            if ( unpackAbsoluteValue() != targetSymbol.targetRuntimeOffset ) {
                diag.error("unencodeable absolute value (0x%llx) for symbol '%s'", targetSymbol.targetRuntimeOffset, targetSymbol.targetSymbolName);
                return;
            }
            break;
        }
        case ResolvedSymbol::Kind::bindToImage: {
            LoaderRef loaderRef = (targetSymbol.targetLoader != nullptr) ? targetSymbol.targetLoader->ref : LoaderRef::missingWeakImage();
            if ( targetSymbol.isFunctionVariant ) {
                assert(targetSymbol.targetLoader != nullptr);
                uint64_t fvTableOffset     = targetSymbol.targetLoader->functionVariantTableVMOffset(state);
                _funcVariant.kind          = Kind::imageFunctionVariant;
                _funcVariant.loaderRef     = *(uint16_t*)(&loaderRef);
                _funcVariant.variantIndex  = targetSymbol.variantIndex;
                _funcVariant.fvTableOffset = fvTableOffset;
                assert(_funcVariant.variantIndex  == targetSymbol.variantIndex && "too many function variants in image");
                assert(_funcVariant.fvTableOffset == fvTableOffset             && "zerofill padding placed function variants table too far from mach_header");
            }
            else {
                high8              = targetSymbol.targetRuntimeOffset >> 56;
                low38              = targetSymbol.targetRuntimeOffset & 0x3FFFFFFFFFULL;
                _regular.kind      = Kind::imageOffset;
                _regular.loaderRef = *(uint16_t*)(&loaderRef);
                _regular.high8     = high8;
                _regular.low38     = low38;
                assert((offset() == targetSymbol.targetRuntimeOffset) && "large offset not support");
            }
            break;
        }
        case ResolvedSymbol::Kind::rebase:
            assert("rebase not a valid bind target");
            break;
    }
}

uint64_t PrebuiltLoader::BindTargetRef::unpackAbsoluteValue() const
{
    // sign extend
    uint64_t result = _abs.low54;
    if ( result & 0x0020000000000000ULL )
        result |= 0x00C0000000000000ULL;
    result |= ((uint64_t)_abs.high8 << 56);
    return result;
}

#if SUPPORT_VM_LAYOUT
uint64_t PrebuiltLoader::BindTargetRef::value(RuntimeState& state) const
{
    switch ( (Kind)_abs.kind ) {
        case Kind::absolute:
            return unpackAbsoluteValue();
        case Kind::imageOffset: {
            const Loader* ldr    = this->loaderRef().loader(state);
            uint64_t      ldAddr = (uint64_t)(ldr->loadAddress(state));
            return ldAddr + this->offset();
        }
        case Kind::imageFunctionVariant: {
            const Loader*              ldr    = this->loaderRef().loader(state);
            uint64_t                   ldAddr = (uint64_t)(ldr->loadAddress(state));
            std::span<const uint8_t>   fvRange((uint8_t*)(ldAddr+_funcVariant.fvTableOffset), 0x4000); // FIXME, we don't have size
            FunctionVariants           fvt(fvRange);
            uint64_t                   implOffset = state.config.process.selectFromFunctionVariants(fvt, _funcVariant.variantIndex);
            return ldAddr + implOffset;
        }
    }
}
#endif

uint64_t PrebuiltLoader::BindTargetRef::absValue() const
{
    if ( _abs.kind != Kind::absolute )
        assert("Must be absolute");

    return unpackAbsoluteValue();
}

uint64_t PrebuiltLoader::BindTargetRef::absValueOrOffset() const
{
    if ( _abs.kind == Kind::absolute )
        return unpackAbsoluteValue();
    else
        return offset();
}

PrebuiltLoader::LoaderRef PrebuiltLoader::BindTargetRef::loaderRef() const
{
    assert(_regular.kind != Kind::absolute);
    uint16_t t = _regular.loaderRef;
    return *((LoaderRef*)&t);
}

uint64_t PrebuiltLoader::BindTargetRef::offset() const
{
    if ( _abs.kind == Kind::imageOffset )
        assert("kind not imageOffset");

    uint64_t signedOffset = _regular.low38;
    if ( signedOffset & 0x0000002000000000ULL )
        signedOffset |= 0x00FFFFC000000000ULL;
    return ((uint64_t)_regular.high8 << 56) | signedOffset;
}

const char* PrebuiltLoader::BindTargetRef::loaderLeafName(RuntimeState& state) const
{
    if ( _abs.kind == Kind::absolute ) {
        return "<absolute>";
    }
    else {
        return this->loaderRef().loader(state)->leafName(state);
    }
}

PrebuiltLoader::BindTargetRef PrebuiltLoader::BindTargetRef::makeAbsolute(uint64_t value) {
    return PrebuiltLoader::BindTargetRef(value);
}

PrebuiltLoader::BindTargetRef::BindTargetRef(uint64_t absoluteValue) {
    uint64_t low54 = absoluteValue & 0x003FFFFFFFFFFFFFULL;
    uint64_t high8 = absoluteValue >> 56;
    _abs.kind  = 1;
    _abs.high8 = high8;
    _abs.low54 = low54;
    assert(unpackAbsoluteValue() == absoluteValue && "unencodeable absolute symbol value");
}

PrebuiltLoader::BindTargetRef::BindTargetRef(const BindTarget& bindTarget) {
    LoaderRef loaderRef = (bindTarget.loader != nullptr) ? bindTarget.loader->ref : LoaderRef::missingWeakImage();
    uint64_t  high8     = bindTarget.runtimeOffset >> 56;
    uint64_t  low38     = bindTarget.runtimeOffset & 0x3FFFFFFFFFULL;
    _regular.kind       = 0;
    _regular.loaderRef  = *(uint16_t*)(&loaderRef);
    _regular.high8      = high8;
    _regular.low38      = low38;
    assert((offset() == bindTarget.runtimeOffset) && "large offset not support");
}

bool PrebuiltLoader::BindTargetRef::isFunctionVariant(uint64_t& fvTableOff, uint16_t& variantIdx) const
{
    if ( _funcVariant.kind != imageFunctionVariant )
        return false;
    fvTableOff = _funcVariant.fvTableOffset;
    variantIdx = _funcVariant.variantIndex;
    return true;
}

//
// MARK: --- PrebuiltLoader methods ---
//

////////////////////////   "virtual" functions /////////////////////////////////

const dyld3::MachOFile* PrebuiltLoader::mf(const RuntimeState& state) const
{
#if SUPPORT_VM_LAYOUT
    return this->loadAddress(state);
#else
    if ( this->ref.app )
        return state.appMF(this->ref.index);
    else
        return state.cachedDylibMF(this->ref.index);
#endif
}

const char* PrebuiltLoader::path(const RuntimeState& state) const
{
    // note: there is a trick here when prebuiltLoaderSetRealPaths is built,
    // we need this to return the initial path, and we know the override paths are built in order,
    // so we only return the override path if the index is in the vector.
    if ( !this->dylibInDyldCache && (this->ref.index < state.prebuiltLoaderSetRealPaths.size()) )
        return state.prebuiltLoaderSetRealPaths[this->ref.index];
    else
        return this->pathOffset ? ((char*)this + this->pathOffset) : nullptr;
}

const char* PrebuiltLoader::installName(const RuntimeState& state) const
{
    if ( this->dylibInDyldCache ) {
        // in normal case where special loaders are Prebuilt and in dyld cache
        // improve performance by not accessing load commands of dylib (may not be paged-in)
        return this->path(state);
    }

    // TODO: We could also check on-disk prebuilt loaders, but the benefit might be small
    // Either their path is equal to the install name, or we'd have recorded an altPath
    // which is the install name
    const Header* hdr = (const Header*)this->mf(state);
    if ( hdr->isDylib() )
        return hdr->installName();
    return nullptr;
}

#if SUPPORT_VM_LAYOUT
const MachOLoaded* PrebuiltLoader::loadAddress(const RuntimeState& state) const
{
    if ( this->ref.app )
        return state.appLoadAddress(this->ref.index);
    else
        return state.cachedDylibLoadAddress(this->ref.index);
}

bool PrebuiltLoader::contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const
{
    const uint8_t* loadAddr = (uint8_t*)(this->loadAddress(state));
    if ( (uint8_t*)addr < loadAddr )
        return false;
    size_t targetOffset = (uint8_t*)addr - loadAddr;
    for ( const Region& seg : this->segments() ) {
        if ( (targetOffset >= seg.vmOffset) && (targetOffset < (seg.vmOffset + seg.fileSize)) ) {
            *segAddr  = (void*)(loadAddr + seg.vmOffset);
            *segSize  = seg.fileSize;
            *segPerms = seg.perms;
            return true;
        }
    }
    return false;
}
#endif

bool PrebuiltLoader::matchesPath(const RuntimeState& state, const char* path) const
{
    if ( strcmp(path, this->path(state)) == 0 )
        return true;
    if ( altPathOffset != 0 ) {
        const char* altPath = (char*)this + this->altPathOffset;
        if ( strcmp(path, altPath) == 0 )
            return true;
    }
    return false;
}

FileID PrebuiltLoader::fileID(const RuntimeState& state) const
{
    if ( const FileValidationInfo* fvi = fileValidationInfo() )
        return FileID(fvi->inode, fvi->deviceID, fvi->mtime, fvi->checkInodeMtime);
    return FileID::none();
}

uint32_t PrebuiltLoader::dependentCount() const
{
    return this->depCount;
}

bool PrebuiltLoader::recordedCdHashIs(const uint8_t expectedCdHash[20]) const
{
    if ( const FileValidationInfo* fvi = fileValidationInfo() ) {
        if ( fvi->checkCdHash )
            return (::memcmp(fvi->cdHash, expectedCdHash, 20) == 0);
    }
    return false;
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void PrebuiltLoader::withCDHash(void (^callback)(const uint8_t cdHash[20])) const
{
    if ( const FileValidationInfo* fvi = fileValidationInfo() ) {
        if ( fvi->checkCdHash )
            callback(fvi->cdHash);
    }
}
#endif

void PrebuiltLoader::map(Diagnostics& diag, RuntimeState& state, const LoadOptions& options, bool parentIsPrebuilt) const
{
    State& ldrState = this->loaderState(state);

    // only map once
    if ( ldrState >= State::mapped )
        return;

#if BUILDING_DYLD
    if ( state.config.log.searching && parentIsPrebuilt ) {
        const char* path = this->path(state);
        state.log("find path \"%s\"\n", path);
        state.log("  found: prebuilt-loader-dylib matching path\n");
    }
    if ( state.config.log.loaders)
        state.log("using PrebuiltLoader %p for %s\n", this, this->path(state));
#endif

    if ( this->dylibInDyldCache ) {
        // dylibs in cache already mapped, just need to update its state
        ldrState = State::mapped;
#if BUILDING_DYLD
        if ( state.config.log.segments )
            this->logSegmentsFromSharedCache(state);
        if ( state.config.log.libraries )
            this->logLoad(state, this->path(state));

        if ( state.config.process.catalystRuntime && this->isCatalystOverride )
            state.setHasOverriddenUnzipperedTwin();
#endif
    }
    else if ( this == state.mainExecutableLoader ) {
#if SUPPORT_VM_LAYOUT
        // main executable is mapped the the kernel, we need to jump ahead to that state
        if ( ldrState < State::mapped )
            ldrState = State::mapped;
        this->setLoadAddress(state, state.config.process.mainExecutableMF);
#else
        assert(0);
#endif
    }
    else {
#if SUPPORT_VM_LAYOUT
        const char* path = this->path(state);
        // open file
        int fd = state.config.syscall.openFileReadOnly(diag, path);
        if ( fd == -1 )
            return;
        const MachOLoaded* ml = Loader::mapSegments(diag, state, path, fd, this->vmSpace, this->codeSignature, true,
                                                    this->segments(), this->neverUnload, true, *this->fileValidationInfo());
        state.config.syscall.close(fd);
        if ( diag.hasError() )
            return;
        this->setLoadAddress(state, ml);
        ldrState = State::mapped;
#else
        assert(0);
#endif // SUPPORT_VM_LAYOUT

#if BUILDING_DYLD
        if ( state.config.log.libraries )
            this->logLoad(state, this->path(state));
#endif
    }

    // add to `state.loaded` but avoid duplicates with inserted dyld cache dylibs
    if ( state.config.pathOverrides.hasInsertedDylibs() ) {
        for (const Loader* ldr : state.loaded) {
            if ( ldr == this )
                return;
        }
    }
    state.add((Loader*)this);
}

void PrebuiltLoader::loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options)
{
    State& ldrState = this->loaderState(state);

    // mmap() this image if needed
    this->map(diag, state, options, false);

    // break cycles
    if ( ldrState >= State::mappingDependents )
        return;

    // breadth-first map all dependents
    ldrState = State::mappingDependents;
    const int count = this->depCount;
    PrebuiltLoader* deps[count];
    for ( int depIndex = 0; depIndex < count; ++depIndex ) {
        PrebuiltLoader* child = (PrebuiltLoader*)dependent(state, depIndex);
        deps[depIndex]        = child;
        if ( child != nullptr )
            child->map(diag, state, options, true);
        else if ( state.config.log.searching ) {
            // prebuilt loader has recorded that this linked dylib should be missing
            const Header* hdr       = (Header*)this->mf(state);
            const char*   childPath = hdr->linkedDylibLoadPath(depIndex);
            state.log("find path \"%s\"\n", childPath);
            state.log("  not found: weak-linked and pre-built-as-missing dylib\n");
        }
    }
    LoadChain   nextChain { options.rpathStack, this };
    LoadOptions depOptions = options;
    depOptions.requestorNeedsFallbacks = this->pre2022Binary;
    depOptions.rpathStack  = &nextChain;
    for ( int depIndex = 0; depIndex < count; ++depIndex ) {
        if ( deps[depIndex] != nullptr )
            deps[depIndex]->loadDependents(diag, state, depOptions);
    }
    ldrState = State::dependentsMapped;
}

#if SUPPORT_IMAGE_UNLOADING
void PrebuiltLoader::unmap(RuntimeState& state, bool force) const
{
    // only called during a dlopen() failure, roll back state 
    State& ldrState = this->loaderState(state);
    ldrState = State::notMapped;
}
#endif

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void PrebuiltLoader::applyFixups(Diagnostics& diag, RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst, bool allowLazyBinds,
                                 lsl::Vector<PseudoDylibSymbolToMaterialize>* materializingSymbols) const
{
    //state.log("PrebuiltLoader::applyFixups: %s\n", this->path());

    // check if we need to patch the cache
    this->applyFixupsCheckCachePatching(state, cacheDataConst);

    // no fixups for dylibs in dyld cache if the Loader is in the shared cache too
    State& ldrState = this->loaderState(state);
    if ( this->dylibInDyldCache && !this->ref.app ) {
#if TARGET_OS_EXCLAVEKIT
        // exclavekit is special in that page-in linking for for the dyld cache can be disabled
        if ( state.config.process.sharedCachePageInLinking )
#endif
        {
            // update any internal pointers to function variants
            this->applyFunctionVariantFixups(diag, state);
            ldrState = PrebuiltLoader::State::fixedUp;
            return;
        }
    }

    // build targets table
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, targetAddrs, 512);
    for ( const BindTargetRef& target : this->bindTargets() ) {
        void* value = (void*)(long)target.value(state);
        if ( state.config.log.fixups ) {
            if ( target.isAbsolute() )
                state.log("<%s/bind#%llu> -> %p\n", this->leafName(state), targetAddrs.count(), value);
            else
                state.log("<%s/bind#%llu> -> %p (%s+0x%08llX)\n", this->leafName(state), targetAddrs.count(), value, target.loaderRef().loader(state)->leafName(state), target.offset());
        }
        targetAddrs.push_back(value);
    }
    if ( diag.hasError() )
        return;

    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, overrideTargetAddrs, 32);
    for ( const BindTargetRef& target : this->overrideBindTargets() ) {
        // Missing weak binds need placeholders to make the target indices line up, but we should otherwise ignore them
        if ( !target.isAbsolute() && target.loaderRef().isMissingWeakImage() ) {
            if ( state.config.log.fixups )
                state.log("<%s/bind#%llu> -> missing-weak-bind\n", this->leafName(state), overrideTargetAddrs.count());

            overrideTargetAddrs.push_back((const void*)UINTPTR_MAX);
        } else {
            void* value = (void*)(long)target.value(state);
            if ( state.config.log.fixups ) {
                if ( target.isAbsolute() )
                    state.log("<%s/bind#%llu> -> %p\n", this->leafName(state), overrideTargetAddrs.count(), value);
                else
                    state.log("<%s/bind#%llu> -> %p (%s+0x%08llX)\n", this->leafName(state), overrideTargetAddrs.count(), value, target.loaderRef().loader(state)->leafName(state), target.offset());
            }
            overrideTargetAddrs.push_back(value);
        }
    }
    if ( diag.hasError() )
        return;

    // do fixups using bind targets table
    uint64_t sliceOffset = ~0ULL;
    if ( const FileValidationInfo* fvi = fileValidationInfo()) {
        // FIXME: Check that this 'check' variable guards sliceOffset being set.  The JSON printer thinks so
        if ( fvi->checkInodeMtime )
            sliceOffset = fvi->sliceOffset;
    }

    if ( sliceOffset == ~0ULL )
        sliceOffset = Loader::getOnDiskBinarySliceOffset(state, this->analyzer(state), this->path(state));

    this->applyFixupsGeneric(diag, state, sliceOffset, targetAddrs, overrideTargetAddrs, true, {});

    // update and internal pointers to function variants
    this->applyFunctionVariantFixups(diag, state);

    // ObjC may have its own fixups which override those we just applied
    applyObjCFixups(state);

    // mark any __DATA_CONST segments read-only
    if ( this->hasConstantSegmentsToProtect() )
        this->makeSegmentsReadOnly(state);

    // update state
    ldrState = PrebuiltLoader::State::fixedUp;
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

Loader* PrebuiltLoader::dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* depAttrs) const
{
    assert(depIndex < this->depCount);
    if ( depAttrs != nullptr ) {
        if ( this->dependentKindArrayOffset != 0 ) {
            const LinkedDylibAttributes* attrsArray = (LinkedDylibAttributes*)((uint8_t*)this + this->dependentKindArrayOffset);
            *depAttrs                                  = attrsArray[depIndex];
        }
        else {
            *depAttrs = LinkedDylibAttributes::regular;
        }
    }
    const PrebuiltLoader::LoaderRef* depRefsArray = (PrebuiltLoader::LoaderRef*)((uint8_t*)this + this->dependentLoaderRefsArrayOffset);
    PrebuiltLoader::LoaderRef        depLoaderRef = depRefsArray[depIndex];
    if ( depLoaderRef.isMissingWeakImage() )
        return nullptr;

    const PrebuiltLoader* depLoader = depLoaderRef.loader(state);
    // if we are in a catalyst app and this is a dylib in cache that links with something that does not support catalyst
    if ( this->dylibInDyldCache && !depLoader->supportsCatalyst && state.config.process.catalystRuntime ) {
        // switch to unzippered twin if there is one, if not, well, keep using macOS dylib...
        if ( depLoader->indexOfTwin != kNoUnzipperedTwin ) {
            PrebuiltLoader::LoaderRef twin(false, depLoader->indexOfTwin);
            depLoader = twin.loader(state);
        }
    }
    return (Loader*)depLoader;
}

bool PrebuiltLoader::getExportsTrie(uint64_t& runtimeOffset, uint32_t& size) const
{
    runtimeOffset = this->exportsTrieLoaderOffset;
    size          = this->exportsTrieLoaderSize;
    return (size != 0);
}

bool PrebuiltLoader::hiddenFromFlat(bool forceGlobal) const
{
    return false; // FIXME
}

bool PrebuiltLoader::representsCachedDylibIndex(uint16_t dylibIndex) const
{
    //dylibIndex = 0xFFFF;
    return false; // cannot make PrebuiltLoader for images that override the dyld cache
}

void PrebuiltLoader::recursiveMarkBeingValidated(const RuntimeState& state, bool sharedCacheLoadersAreAlwaysValid) const
{
    State pbLdrState = this->loaderState(state);
    if ( pbLdrState == State::unknown ) {
        // If this is a shared cache loader, and they are always valid, then just stop here.  We don't even set the state
        if ( sharedCacheLoadersAreAlwaysValid && this->dylibInDyldCache )
            return;

        this->loaderState(state) = State::beingValidated;
        bool haveInvalidDependent = false;
        for (int depIndex = 0; depIndex < this->depCount; ++depIndex) {
            if ( const Loader* dep = this->dependent(state, depIndex) ) {
                assert (dep->isPrebuilt);
                const PrebuiltLoader* pbDep = (PrebuiltLoader*)dep;
                pbDep->recursiveMarkBeingValidated(state, sharedCacheLoadersAreAlwaysValid);
                if ( pbDep->loaderState(state) == State::invalid )
                    haveInvalidDependent = true;
            }
        }
        if ( haveInvalidDependent )
            this->loaderState(state) = State::invalid;
    }
}


// Note: because of cycles, isValid() cannot just call isValid() on each of its dependents
// Instead we do this in three steps:
// 1) recursively mark all reachable Loaders as beingValidated
// 2) check each beingValidated Loader for an override (which invalidates the PrebuiltLoader)
// 3) propagate up invalidness
bool PrebuiltLoader::isValid(const RuntimeState& state) const
{
    static const bool verbose = false;

    bool sharedCacheLoadersAreAlwaysValid = state.config.dyldCache.sharedCacheLoadersAreAlwaysValid();

    // quick exit if already known to be valid or invalid
    switch ( this->loaderState(state) ) {
        case State::unknown:
            // mark everything it references as beingValidated
            this->recursiveMarkBeingValidated(state, sharedCacheLoadersAreAlwaysValid);
            break;
        case State::beingValidated:
            break;
        case State::notMapped:
        case State::mapped:
        case State::mappingDependents:
        case State::dependentsMapped:
        case State::fixedUp:
        case State::delayInitPending:
        case State::beingInitialized:
        case State::initialized:
            return true;
        case State::invalid:
            return false;
    }
    if (verbose) state.log("PrebuiltLoader::isValid(%s)\n", this->leafName(state));

    // make an array of all Loaders in beingValidated state
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const PrebuiltLoader*, loadersBeingValidated, 1024);
    if ( this->ref.app ) {
        // only examine processPrebuiltLoaderSet if Loader being validated is in processPrebuiltLoaderSet
        const PrebuiltLoaderSet* appDylibsSet = state.processPrebuiltLoaderSet();
        for ( uint32_t i = 0; i < appDylibsSet->loadersArrayCount; ++i ) {
            const PrebuiltLoader* ldr = appDylibsSet->atIndex(i);
            if ( ldr->loaderState(state) == State::beingValidated ) {
                loadersBeingValidated.push_back(ldr);
            }
        }
    }

    if ( !sharedCacheLoadersAreAlwaysValid ) {
        const PrebuiltLoaderSet* cachedDylibsSet = state.cachedDylibsPrebuiltLoaderSet();
        for ( uint32_t i = 0; i < cachedDylibsSet->loadersArrayCount; ++i ) {
            const PrebuiltLoader* ldr = cachedDylibsSet->atIndex(i);
            if ( ldr->loaderState(state) == State::beingValidated ) {
                loadersBeingValidated.push_back(ldr);
            }
        }
    }

    if (verbose) state.log("   have %llu beingValidated Loaders\n", loadersBeingValidated.count());

    // look at each individual dylib in beingValidated state to see if it has an override file
    for (const PrebuiltLoader* ldr : loadersBeingValidated) {
        ldr->invalidateInIsolation(state);
    }

    // now keep propagating invalidness until nothing changes
    bool more = true;
    while (more) {
        more = false;
        if (verbose) state.log("checking shallow for %llu loaders\n", loadersBeingValidated.count());
        for (const PrebuiltLoader* ldr : loadersBeingValidated) {
            State&      ldrState    = ldr->loaderState(state);
            const State ldrOrgState = ldrState;
            if ( ldrOrgState == State::beingValidated ) {
                if (verbose) state.log("   invalidateShallow(%s)\n", ldr->leafName(state));
                ldr->invalidateShallow(state);
                if ( ldrState != ldrOrgState ) {
                   if (verbose) state.log("     %s state changed\n", ldr->leafName(state));
                   more = true;
                }
            }
        }
    }

    // mark everything left in beingValidate as valid (notMapped)
    for (const PrebuiltLoader* ldr : loadersBeingValidated) {
        if ( ldr->loaderState(state) == State::beingValidated )
            ldr->loaderState(state) = State::notMapped;
    }

    return (this->loaderState(state) != State::invalid);
}


// look to see if anything this loader directly depends on is invalid
void PrebuiltLoader::invalidateShallow(const RuntimeState& state) const
{
   for (int depIndex = 0; depIndex < this->depCount; ++depIndex) {
        if ( const Loader* dep = this->dependent(state, depIndex) ) {
            if ( dep->isPrebuilt ) {
                const PrebuiltLoader* pbDep = (PrebuiltLoader*)dep;
                State& depState = pbDep->loaderState(state);
                if ( depState == State::invalid ) {
                    this->loaderState(state) = State::invalid;
                }
            }
        }
    }
}

// just look to see if this one file is overridden
void PrebuiltLoader::invalidateInIsolation(const RuntimeState& state) const
{
    State& ldrState = this->loaderState(state);
    if ( ldrState == State::invalid )
        return;
    if ( ldrState >= State::notMapped )
        return;

    // validate the source file has not changed
    if ( this->dylibInDyldCache ) {
        if ( state.config.dyldCache.addr == nullptr ) {
            ldrState = State::invalid;
            return;
        }
#if BUILDING_DYLD
        // check for roots that override this dylib in the dyld cache
        bool checkForRoots = false;
        if ( this->isOverridable ) {
            // isOverridable is always true when building Universal caches
            // check below to make sure we are not looking for roots of a dylib
            // in a customer configuration apart from libdispatch
            checkForRoots = true;
            if ( !state.config.dyldCache.development && !ProcessConfig::DyldCache::isAlwaysOverridablePath(this->path(state)) )
                checkForRoots = false;
        }
        if ( checkForRoots ) {
            __block bool hasOnDiskOverride = false;
            bool stop = false;
            state.config.pathOverrides.forEachPathVariant(this->path(state), state.config.process.platform, false, true, stop,
                                                      ^(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& innerStop) {
                                                          // look only at variants that might override the original path
                                                          if ( type > ProcessConfig::PathOverrides::Type::rawPath ) {
                                                              innerStop = true;
                                                              return;
                                                          }
                                                          FileID foundFileID = FileID::none();
                                                          if ( state.config.fileExists(possiblePath, &foundFileID) ) {
                                                              FileID recordedFileID = this->fileID(state);
                                                              // Note: sim caches will have valid() fileIDs, others won't
                                                              if ( recordedFileID.valid() ) {
                                                                  if ( foundFileID != recordedFileID ) {
                                                                      if ( state.config.log.loaders )
                                                                          console("found '%s' with different inode/mtime than PrebuiltLoader for '%s'\n", possiblePath, this->path(state));
                                                                      hasOnDiskOverride = true;
                                                                      innerStop         = true;
                                                                  }
                                                              }
                                                              else {
                                                                  // this Loader had no recorded FileID, so it was not expected on disk, but now a file showed up
                                                                  if ( state.config.log.loaders )
                                                                      console("found '%s' which invalidates PrebuiltLoader for '%s'\n", possiblePath, this->path(state));
                                                                  hasOnDiskOverride = true;
                                                                  innerStop         = true;
                                                              }
                                                          }
                                                      });
            if ( hasOnDiskOverride ) {
                if ( state.config.log.loaders )
                    console("PrebuiltLoader %p '%s' not used because a file was found that overrides it\n", this, this->leafName(state));
                // PrebuiltLoader is for dylib in cache, but have one on disk that overrides cache
                ldrState = State::invalid;
                return;
            }
        }
#endif
    }
    else {
#if BUILDING_DYLD
        // not in dyld cache
        FileID recordedFileID = this->fileID(state);
        if ( recordedFileID.valid() ) {
            // have recorded file inode (such as for embedded framework in 3rd party app)
            FileID foundFileID = FileID::none();
            if ( state.config.syscall.fileExists(this->path(state), &foundFileID) ) {
                if ( foundFileID != recordedFileID ) {
                    ldrState = State::invalid;
                    if ( state.config.log.loaders )
                        console("PrebuiltLoader %p not used because file inode/mtime does not match\n", this);
                }
            }
            else {
                ldrState = State::invalid;
                if ( state.config.log.loaders )
                    console("PrebuiltLoader %p not used because file missing\n", this);
            }
        }
        else {
            // PrebuildLoaderSet did not record inode, check cdhash
            const char* path = this->path(state);
            // skip over main exectuable.  It's cdHash is checked as part of initializeClosureMode()
            if ( strcmp(path, state.config.process.mainExecutablePath) != 0 ) {
                int fd = state.config.syscall.open(path, O_RDONLY, 0);
                if ( fd != -1 ) {
                    Diagnostics cdHashDiag;
                    if ( Loader::validateFile(cdHashDiag, state, fd, path, this->codeSignature, *this->fileValidationInfo()) == (uint64_t)(-1) ) {
                        ldrState = State::invalid;
                        if ( state.config.log.loaders )
                            console("PrebuiltLoader %p not used because file '%s' cdHash changed\n", this, path);
                    }
                    state.config.syscall.close(fd);
                }
                else {
                    ldrState = State::invalid;
                    if ( state.config.log.loaders )
                        console("PrebuiltLoader %p not used because file '%s' cannot be opened\n", this, path);
                }
            }
        }
#endif // BUILDING_DYLD
    }
}

bool PrebuiltLoader::dyldDoesObjCFixups() const
{
    // check if we stored objc info for this image
    if ( const ObjCBinaryInfo* fixupInfo = objCBinaryInfo() )
        return (fixupInfo->imageInfoRuntimeOffset != 0);

    // dylibs in dyld cache (had objc fixed up at cache build time)
    return this->dylibInDyldCache;
}

const SectionLocations* PrebuiltLoader::getSectionLocations() const
{
    return &this->sectionLocations;
}

Array<Loader::Region> PrebuiltLoader::segments() const
{
    return Array<Region>((Region*)((uint8_t*)this + regionsOffset), regionsCount, regionsCount);
}

const Array<PrebuiltLoader::BindTargetRef> PrebuiltLoader::bindTargets() const
{
    return Array<BindTargetRef>((BindTargetRef*)((uint8_t*)this + bindTargetRefsOffset), bindTargetRefsCount, bindTargetRefsCount);
}

const Array<PrebuiltLoader::BindTargetRef> PrebuiltLoader::overrideBindTargets() const
{
    return Array<BindTargetRef>((BindTargetRef*)((uint8_t*)this + overrideBindTargetRefsOffset), overrideBindTargetRefsCount, overrideBindTargetRefsCount);
}

bool PrebuiltLoader::hasBeenFixedUp(RuntimeState& state) const
{
    State& ldrState = this->loaderState(state);
    return ldrState >= State::fixedUp;
}

bool PrebuiltLoader::beginInitializers(RuntimeState& state)
{
    // do nothing if already initializers already run
    State& ldrState = this->loaderState(state);
    if ( ldrState == State::initialized )
        return true;
    if ( ldrState == State::beingInitialized )
        return true;

    assert(ldrState == State::fixedUp);

    // switch to being-inited state
    ldrState = State::beingInitialized;
    return false;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void PrebuiltLoader::runInitializers(RuntimeState& state) const
{
    // most images do not have initializers, so we make that case fast
    if ( this->hasInitializers ) {
        this->findAndRunAllInitializers(state);
    }
    this->loaderState(state) = State::initialized;
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

bool PrebuiltLoader::isDelayInit(RuntimeState& state) const
{
    return this->loaderState(state) == State::delayInitPending;
}

void PrebuiltLoader::setDelayInit(RuntimeState& state, bool value) const
{
    // This is used in the mark-and-sweep to determine which dylibs should be delay-inited.
    // But, PrebuiltLoaders are r/o and don't have a place to store this bit.
    // So, instead we manipulate the "class State" byte used by this PrebuiltLoader.
    // For newly loaded dylibs, the state will be "fixedUp" when the mark-and-sweep is done.
    // Older loaders are in "initialized" state.  So, when value==true (mark) and the state is "fixedUp"
    // we move the state to "delayInitPending", and when value==false (sweep) and the stat is
    // "delayInitPending", we move it back to "fixedUp".
    PrebuiltLoader::State& ldrState = this->loaderState(state);
    if ( value ) {
        // in "mark" phase
        if ( ldrState == State::fixedUp )
            ldrState = State::delayInitPending;
    }
    else {
        // in "sweep" phase
        if ( ldrState == State::delayInitPending )
            ldrState = State::fixedUp;
    }
}

bool PrebuiltLoader::isInitialized(const RuntimeState& state) const
{
    return this->loaderState(state) == State::initialized;
}

void PrebuiltLoader::setFixedUp(const RuntimeState& state) const
{
    this->loaderState(state) = State::fixedUp;
}

#if SUPPORT_VM_LAYOUT
void PrebuiltLoader::setLoadAddress(RuntimeState& state, const MachOLoaded* ml) const
{
    assert(this->ref.app && "shared cache addresses are fixed");
    state.setAppLoadAddress(this->ref.index, ml);
}
#else
void PrebuiltLoader::setMF(RuntimeState& state, const dyld3::MachOFile* mf) const
{
    assert(this->ref.app && "shared cache addresses are fixed");
    state.setAppMF(this->ref.index, mf);
}
#endif // SUPPORT_VM_LAYOUT

////////////////////////  other functions /////////////////////////////////

PrebuiltLoader::PrebuiltLoader(const Loader& jitLoader)
    : Loader(InitialOptions(jitLoader), true, false, 0, false)
{
}

size_t PrebuiltLoader::size() const
{
    return this->regionsOffset + this->regionsCount * sizeof(Region);
}

const Loader::FileValidationInfo* PrebuiltLoader::fileValidationInfo() const
{
    if ( this->fileValidationOffset == 0 )
        return nullptr;
    return (FileValidationInfo*)((uint8_t*)this + this->fileValidationOffset);
}

PrebuiltLoader::State& PrebuiltLoader::loaderState(const RuntimeState& state) const
{
    assert(sizeof(State) == sizeof(uint8_t));
    const uint8_t* stateArray = state.prebuiltStateArray(this->ref.app);
    return *((PrebuiltLoader::State*)&stateArray[this->ref.index]);
 }

////////////////////////////// ObjCBinaryInfo ///////////////////////////////////////

const ObjCBinaryInfo* PrebuiltLoader::objCBinaryInfo() const
{
    if ( this->objcBinaryInfoOffset == 0 )
        return nullptr;
    return (ObjCBinaryInfo*)((uint8_t*)this + this->objcBinaryInfoOffset);
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void PrebuiltLoader::applyObjCFixups(RuntimeState& state) const
{
    const ObjCBinaryInfo* fixupInfo = objCBinaryInfo();
    if ( fixupInfo == nullptr )
        return;

    const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)this->loadAddress(state);
    const uint8_t* baseAddress = (const uint8_t*)ma;
    const uint32_t pointerSize = this->loadAddress(state)->pointerSize();

    // imageInfoRuntimeOffset.  This is always set it we have objc
    {
        uintptr_t* fixUpLoc = (uintptr_t*)(baseAddress + fixupInfo->imageInfoRuntimeOffset);
        MachOAnalyzer::ObjCImageInfo* imageInfo = (MachOAnalyzer::ObjCImageInfo *)fixUpLoc;
        ((MachOAnalyzer::ObjCImageInfo*)imageInfo)->flags |= MachOAnalyzer::ObjCImageInfo::dyldPreoptimized;
        if ( state.config.log.fixups )
            state.log("fixup: *0x%012lX = 0x%012lX <objc-info preoptimized>\n", (uintptr_t)fixUpLoc, *fixUpLoc);
    }

    const dyld3::MachOAnalyzer::VMAddrConverter& vmAddrConverter = ma->makeVMAddrConverter(true);
    const uint64_t loadAddress = ((const Header*)ma)->preferredLoadAddress();

    // Protocols.
    // If we have only a single definition of a protocol, then that definition should be fixed up.
    // If we have multiple definitions of a protocol, then we should fix up just the first one we see.
    // Only the first is considered the canonical definition.
    if ( fixupInfo->protocolFixupsOffset != 0 ) {
        // Get the pointer to the Protocol class.
        uint64_t classProtocolPtr = (uint64_t)state.config.dyldCache.addr + state.processPrebuiltLoaderSet()->objcProtocolClassCacheOffset;

        Array<uint8_t> protocolFixups = fixupInfo->protocolFixups();
        __block uint32_t protocolIndex = 0;
        auto visitProtocol = ^(uint64_t protocolVMAddr, const dyld3::MachOAnalyzer::ObjCProtocol& objCProtocol, bool& stop) {
            bool isCanonical = protocolFixups[protocolIndex++] == 1;
            if ( isCanonical ) {
                uint64_t runtimeOffset = protocolVMAddr - loadAddress;
                uintptr_t* fixUpLoc = (uintptr_t*)(baseAddress + runtimeOffset);
                uintptr_t value = (uintptr_t)classProtocolPtr;
    #if __has_feature(ptrauth_calls)
                // Sign the ISA on arm64e.
                // Unfortunately a hard coded value here is not ideal, but this is ABI so we aren't going to change it
                // This matches the value in libobjc __objc_opt_ptrs: .quad x@AUTH(da, 27361, addr)
                value = MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(value, fixUpLoc, true, 27361, 2);
    #endif
                if ( state.config.log.fixups )
                    state.log("fixup: *0x%012lX = 0x%012lX <objc-protocol>\n", (uintptr_t)fixUpLoc, (uintptr_t)value);
                *fixUpLoc = value;
            }
        };
        ma->forEachObjCProtocol(fixupInfo->protocolListRuntimeOffset, fixupInfo->protocolListCount, vmAddrConverter, visitProtocol);
    }

    // Selectors
    if ( fixupInfo->selectorReferencesFixupsCount != 0 ) {
        const void* dyldCacheHashTable = state.config.dyldCache.objcSelectorHashTable;

        Array<BindTargetRef> selectorReferenceFixups = fixupInfo->selectorReferenceFixups();
        __block uint32_t fixupIndex = 0;
        PrebuiltObjC::forEachSelectorReferenceToUnique(state, this, loadAddress, *fixupInfo,
                                                       ^(uint64_t selectorReferenceRuntimeOffset,
                                                         uint64_t selectorStringRuntimeOffset,
                                                         const char* originalSelectorString) {
            const BindTargetRef& bindTargetRef = selectorReferenceFixups[fixupIndex++];

            const char* selectorString = nullptr;
            if ( bindTargetRef.isAbsolute() ) {
                // HACK!: We use absolute bind targets as offsets from the shared cache selector table base, not actual absolute fixups
                // Note: In older shared caches these were indices in to the shared cache selector table
                selectorString = (const char*)dyldCacheHashTable + bindTargetRef.absValue();
            } else {
                // For the app case, we just point directly to the image containing the selector
                selectorString = (const char*)bindTargetRef.value(state);
            }
            uintptr_t* fixUpLoc = (uintptr_t*)(baseAddress + selectorReferenceRuntimeOffset);
            uintptr_t value = (uintptr_t)selectorString;
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <objc-selector '%s'>\n", (uintptr_t)fixUpLoc, (uintptr_t)value, (const char*)value);
            *fixUpLoc = value;
        });
    }

    // protocol references
    if ( fixupInfo->protocolReferencesFixupsCount != 0 ) {
        Array<BindTargetRef> protocolReferenceFixups = fixupInfo->protocolReferenceFixups();

        __block uint32_t fixupIndex = 0;
        objc_visitor::Visitor objcVisitor(ma);
        objcVisitor.forEachProtocolReference(^(metadata_visitor::ResolvedValue& protoRefValue) {
            const BindTargetRef& bindTargetRef = protocolReferenceFixups[fixupIndex++];

            uint64_t targetProtocol = 0;
            if ( bindTargetRef.isAbsolute() ) {
                // HACK!: We use absolute bind targets as offsets in to the shared cache
                targetProtocol = (uint64_t)state.config.dyldCache.addr + bindTargetRef.absValue();
            } else {
                // For the app case, we just point directly to the image containing the protocol
                targetProtocol = bindTargetRef.value(state);
            }
            uintptr_t* fixUpLoc = (uintptr_t*)protoRefValue.value();
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <objc-protocol>\n", (uintptr_t)fixUpLoc, (uintptr_t)targetProtocol);
            *fixUpLoc = (uintptr_t)targetProtocol;
        });
    }

    // Stable Swift Classes
    if ( fixupInfo->hasClassStableSwiftFixups ) {
        dyld3::MachOAnalyzer::ClassCallback visitClass = ^(uint64_t classVMAddr,
                                                           uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr,
                                                           const MachOAnalyzer::ObjCClassInfo &objcClass, bool isMetaClass,
                                                           bool& stop) {
            if ( isMetaClass )
                return;

            // Does this class need to be fixed up for stable Swift ABI.
            if ( objcClass.isUnfixedBackwardDeployingStableSwift() ) {
                // Class really is stable Swift, pretending to be pre-stable.
                // Fix its lie.  This involves fixing the FAST bits on the class data value
                uint64_t runtimeOffset = classDataVMAddr - loadAddress;
                uintptr_t* fixUpLoc = (uintptr_t*)(baseAddress + runtimeOffset);
                uintptr_t value = ((*fixUpLoc) | MachOAnalyzer::ObjCClassInfo::FAST_IS_SWIFT_STABLE) & ~MachOAnalyzer::ObjCClassInfo::FAST_IS_SWIFT_LEGACY;
                if ( state.config.log.fixups )
                    state.log("fixup: *0x%012lX = 0x%012lX <mark swift stable>\n", (uintptr_t)fixUpLoc, (uintptr_t)value);
                *fixUpLoc = value;
            }
        };
        ma->forEachObjCClass(fixupInfo->classListRuntimeOffset, fixupInfo->classListCount, vmAddrConverter, visitClass);
    }

    // Method lists to set as uniqued.

    // This is done for all pointer-based method lists.  Relative method lists should already be uniqued as they point to __objc_selrefs
    auto trySetMethodListAsUniqued = [&](uint64_t methodListVMAddr) {
        if ( methodListVMAddr == 0 )
            return;

        uint64_t methodListRuntimeOffset = methodListVMAddr - loadAddress;
        if ( ma->objcMethodListIsRelative(methodListRuntimeOffset) )
            return;

        // Set the method list to have the uniqued bit set
        uint32_t* fixUpLoc = (uint32_t*)(baseAddress + methodListRuntimeOffset);
        uint32_t value = (*fixUpLoc) | MachOAnalyzer::ObjCMethodList::methodListIsUniqued;
        if ( state.config.log.fixups )
            state.log("fixup: *0x%012lX = 0x%012lX <mark method list uniqued>\n", (uintptr_t)fixUpLoc, (uintptr_t)value);
        *fixUpLoc = value;
    };

    // Class method lists
    if ( fixupInfo->hasClassMethodListsToSetUniqued ) {
        dyld3::MachOAnalyzer::ClassCallback visitClass = ^(uint64_t classVMAddr,
                                                           uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr,
                                                           const MachOAnalyzer::ObjCClassInfo &objcClass, bool isMetaClass,
                                                           bool& stop) {
            trySetMethodListAsUniqued(objcClass.baseMethodsVMAddr(pointerSize));
        };
        ma->forEachObjCClass(fixupInfo->classListRuntimeOffset, fixupInfo->classListCount, vmAddrConverter, visitClass);
    }

    // Category method lists
    if ( fixupInfo->hasCategoryMethodListsToSetUniqued ) {
        auto visitCategory = ^(uint64_t categoryVMAddr, const dyld3::MachOAnalyzer::ObjCCategory& objcCategory, bool& stop) {
            trySetMethodListAsUniqued(objcCategory.instanceMethodsVMAddr);
            trySetMethodListAsUniqued(objcCategory.classMethodsVMAddr);
        };
        ma->forEachObjCCategory(fixupInfo->categoryListRuntimeOffset, fixupInfo->categoryCount, vmAddrConverter, visitCategory);
    }

    // Protocol method lists
    if ( fixupInfo->hasProtocolMethodListsToSetUniqued ) {
        auto visitProtocol = ^(uint64_t protocolVMAddr, const dyld3::MachOAnalyzer::ObjCProtocol& objCProtocol, bool& stop) {
            trySetMethodListAsUniqued(objCProtocol.instanceMethodsVMAddr);
            trySetMethodListAsUniqued(objCProtocol.classMethodsVMAddr);
            trySetMethodListAsUniqued(objCProtocol.optionalInstanceMethodsVMAddr);
            trySetMethodListAsUniqued(objCProtocol.optionalClassMethodsVMAddr);
        };
        ma->forEachObjCProtocol(fixupInfo->protocolListRuntimeOffset, fixupInfo->protocolListCount, vmAddrConverter, visitProtocol);
    }
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

#if BUILDING_CLOSURE_UTIL
void PrebuiltLoader::printObjCFixups(RuntimeState& state, FILE* out) const
{
    const ObjCBinaryInfo* fixupInfo = objCBinaryInfo();
    if ( fixupInfo == nullptr )
        return;

    // imageInfoRuntimeOffset.  This is always set it we have objc
    {
        fprintf(out, ",\n");
        fprintf(out, "      \"objc-image-info-offset\":    \"0x%llX\"", fixupInfo->imageInfoRuntimeOffset);
    }


    // Protocols
    if ( fixupInfo->protocolFixupsOffset != 0 ) {
        fprintf(out, ",\n      \"objc-canonical-protocols\": [");
        Array<uint8_t> protocolFixups = fixupInfo->protocolFixups();
        bool needComma = false;
        for ( uint8_t isCanonical : protocolFixups ) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n          \"%s\"", (isCanonical == 1) ? "true" : "false");
            needComma = true;
        }
        fprintf(out, "\n      ]");
    }

    // Selectors
    if ( fixupInfo->selectorReferencesFixupsCount != 0 ) {
        fprintf(out, ",\n      \"objc-selectors\": [");
        bool needComma = false;
        for ( const BindTargetRef& target : fixupInfo->selectorReferenceFixups() ) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n          {\n");
            if ( target.isAbsolute() ) {
                // HACK!: We use absolute bind targets as offsets from the shared cache selector table base, not actual absolute fixups
                // Note: In older shared caches these were indices in to the shared cache selector table
                fprintf(out, "              \"shared-cache-table-offset\":    \"0x%llX\"\n", target.value(state));
            }
            else {
                fprintf(out, "              \"loader\":   \"%c.%d\",\n", target.loaderRef().app ? 'a' : 'c', target.loaderRef().index);
                fprintf(out, "              \"offset\":   \"0x%08llX\"\n", target.offset());
            }
            fprintf(out, "          }");
            needComma = true;
        }
        fprintf(out, "\n      ]");
    }

    // Protocol references
    if ( fixupInfo->protocolReferencesFixupsCount != 0 ) {
        fprintf(out, ",\n      \"objc-protorefs\": [");
        bool needComma = false;
        for ( const BindTargetRef& target : fixupInfo->protocolReferenceFixups() ) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n          {\n");
            if ( target.isAbsolute() ) {
                // HACK!: We use absolute bind targets as offsets in to the shared cache
                fprintf(out, "              \"shared-cache-offset\":    \"0x%llX\"\n", target.value(state));
            }
            else {
                fprintf(out, "              \"loader\":   \"%c.%d\",\n", target.loaderRef().app ? 'a' : 'c', target.loaderRef().index);
                fprintf(out, "              \"offset\":   \"0x%08llX\"\n", target.offset());
            }
            fprintf(out, "          }");
            needComma = true;
        }
        fprintf(out, "\n      ]");
    }
}
#endif // BUILDING_CLOSURE_UTIL

void PrebuiltLoader::serialize(Diagnostics& diag, RuntimeState& state, const JustInTimeLoader& jitLoader, LoaderRef buildRef,
                               CacheWeakDefOverride cacheWeakDefFixup, PrebuiltObjC& prebuiltObjC,
                               const PrebuiltSwift& prebuiltSwift, BumpAllocator& allocator)
{
    // use allocator and placement new to instantiate PrebuiltLoader object
    uint64_t serializationStart = allocator.size();
    allocator.zeroFill(sizeof(PrebuiltLoader));
    BumpAllocatorPtr<PrebuiltLoader> p(allocator, serializationStart);
    new (p.get()) PrebuiltLoader(jitLoader);
    p->ref                  = buildRef;

    // record offset of load command that specifies fixups (LC_DYLD_INFO or LC_DYLD_CHAINED_FIXUPS)
    const MachOFile* mf = jitLoader.mf(state);
    p->fixupsLoadCommandOffset = mf->getFixupsLoadCommandFileOffset();

    // append path to serialization
    p->pathOffset    = allocator.size() - serializationStart;
    const char* path = jitLoader.path(state);
    allocator.append(path, strlen(path) + 1);
    p->altPathOffset            = 0;
    const char* installNamePath = ((Header*)mf)->installName();
    if ( mf->isDylib() && (strcmp(installNamePath, path) != 0) ) {
        p->altPathOffset = allocator.size() - serializationStart;
        allocator.append(installNamePath, strlen(installNamePath) + 1);
    }

    // on customer installs, most dylibs in cache are not overridable
    p->isOverridable = jitLoader.dylibInDyldCache && state.config.dyldCache.isOverridablePath(path);

    // append dependents to serialization
    uint32_t depCount = jitLoader.dependentCount();
    p->depCount = depCount;
    allocator.align(sizeof(LoaderRef));
    uint16_t depLoaderRefsArrayOffset = allocator.size() - serializationStart;
    p->dependentLoaderRefsArrayOffset = depLoaderRefsArrayOffset;
    allocator.zeroFill(depCount * sizeof(LoaderRef));
    BumpAllocatorPtr<LoaderRef>      depArray(allocator, serializationStart + depLoaderRefsArrayOffset);
    Loader::LinkedDylibAttributes depAttrs[depCount+1];
    bool                  hasNonRegularLink = false;
    for ( uint32_t depIndex = 0; depIndex < depCount; ++depIndex ) {
        Loader* depLoader = jitLoader.dependent(state, depIndex, &depAttrs[depIndex]);
        if ( depAttrs[depIndex] != Loader::LinkedDylibAttributes::regular )
            hasNonRegularLink = true;
        if ( depLoader == nullptr ) {
            assert(depAttrs[depIndex].weakLink);
            depArray.get()[depIndex] = LoaderRef::missingWeakImage();
        }
        else {
            depArray.get()[depIndex] = depLoader->ref;
        }
    }

    // if any non-regular linking of dependents, append array for that
    p->dependentKindArrayOffset = 0;
    if ( hasNonRegularLink ) {
        static_assert(sizeof(Loader::LinkedDylibAttributes) == 1, "LinkedDylibAttributes expect to be one byte");
        uint16_t dependentKindArrayOff = allocator.size() - serializationStart;
        p->dependentKindArrayOffset    = dependentKindArrayOff;
        allocator.zeroFill(depCount * sizeof(Loader::LinkedDylibAttributes));
        BumpAllocatorPtr<Loader::LinkedDylibAttributes> kindArray(allocator, serializationStart + dependentKindArrayOff);
        memcpy(kindArray.get(), depAttrs, depCount * sizeof(Loader::LinkedDylibAttributes));
    }

    // record exports-trie location
    jitLoader.getExportsTrie(p->exportsTrieLoaderOffset, p->exportsTrieLoaderSize);

    // just record if image has any initializers (but not what they are)
    p->hasInitializers = mf->hasInitializer(diag);
    if ( diag.hasError() )
        return;

    // record code signature location
    p->codeSignature.fileOffset = 0;
    p->codeSignature.size       = 0;
    if ( !jitLoader.dylibInDyldCache ) {
        uint32_t sigFileOffset;
        uint32_t sigSize;
        if ( ((const Header*)mf)->hasCodeSignature(sigFileOffset, sigSize) ) {
            p->codeSignature.fileOffset = sigFileOffset;
            p->codeSignature.size       = sigSize;
        }
    }

    // append FileValidationInfo
    if ( !jitLoader.dylibInDyldCache || state.config.dyldCache.dylibsExpectedOnDisk ) {
        allocator.align(__alignof__(FileValidationInfo));
        FileValidationInfo info = jitLoader.getFileValidationInfo(state);
        uint64_t          off  = allocator.size() - serializationStart;
        p->fileValidationOffset = off;
        assert(p->fileValidationOffset == off && "uint16_t fileValidationOffset overflow");
        allocator.append(&info, sizeof(FileValidationInfo));
    }

    // append segments to serialization
    p->vmSpace = (uint32_t)mf->mappedSize();
    jitLoader.withRegions(mf, ^(const Array<Region>& regions) {
        allocator.align(__alignof__(Region));
        uint64_t off    = allocator.size() - serializationStart;
        p->regionsOffset = off;
        assert(p->regionsOffset == off && "uint16_t regionsOffset overflow");
        p->regionsCount = regions.count();
        allocator.append(&regions[0], sizeof(Region) * regions.count());
    });

    // append section locations
    p->sectionLocations = *jitLoader.getSectionLocations();

    // add catalyst support info
    bool isMacOSOrCataylyst = (state.config.process.basePlatform == Platform::macOS) || (state.config.process.basePlatform == Platform::macCatalyst);
    bool buildingMacOSCache = jitLoader.dylibInDyldCache && isMacOSOrCataylyst;
    p->supportsCatalyst     = buildingMacOSCache && ((mach_o::Header*)mf)->builtForPlatform(Platform::macCatalyst);
    p->isCatalystOverride   = false;
    p->indexOfTwin          = kNoUnzipperedTwin;
    p->reserved1            = 0;
    if ( buildingMacOSCache ) {
        // check if this is part of an unzippered twin
        if ( !p->supportsCatalyst ) {
            char catalystTwinPath[PATH_MAX];
            strlcpy(catalystTwinPath, "/System/iOSSupport", PATH_MAX);
            strlcat(catalystTwinPath, path, PATH_MAX);
            for (const Loader* ldr : state.loaded) {
                if ( ldr->matchesPath(state, catalystTwinPath) ) {
                    // record index of catalyst side in mac side
                    p->indexOfTwin = ldr->ref.index;
                    break;
                }
            }
        }
        else if ( strncmp(path, "/System/iOSSupport/", 19) == 0 ) {
            const char* macTwinPath = &path[18];
            for (const Loader* ldr : state.loaded) {
                if ( ldr->matchesPath(state, macTwinPath) ) {
                    // record index of mac side in catalyst side
                    p->indexOfTwin    = ldr->ref.index;
                    p->isCatalystOverride = true;  // catalyst side of twin (if used) is an override of the mac side
                    break;
                }
            }
        }
    }

    // append fixup target info to serialization
    // note: this can be very large, so it is last in the small layout so that uint16_t to other things don't overflow
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(BindTargetRef, overrideBindTargets, 16);
    if ( !jitLoader.dylibInDyldCache ) {
        allocator.align(__alignof__(BindTargetRef));
        uint64_t off           = allocator.size() - serializationStart;
        p->bindTargetRefsOffset = off;
        assert(p->bindTargetRefsOffset == off && "uint16_t bindTargetRefsOffset overflow");
        p->bindTargetRefsCount = 0;
        jitLoader.forEachBindTarget(diag, state, cacheWeakDefFixup, true, ^(const ResolvedSymbol& resolvedTarget, bool& stop) {
            // Regular and lazy binds
            BindTargetRef bindRef(diag, state, resolvedTarget);
            if ( diag.hasError() ) {
                stop = true;
                return;
            }
            allocator.append(&bindRef, sizeof(BindTargetRef));
            p->bindTargetRefsCount += 1;
            assert(p->bindTargetRefsCount != 0 && "bindTargetRefsCount overflow");
        }, ^(const ResolvedSymbol& resolvedTarget, bool& stop) {
            // Opcode based weak binds
            BindTargetRef bindRef(diag, state, resolvedTarget);
            if ( diag.hasError() ) {
                stop = true;
                return;
            }
            overrideBindTargets.push_back(bindRef);
        });
        if ( diag.hasError() )
            return;
    }

    // Everything from this point onwards needs 32-bit offsets
    if ( !overrideBindTargets.empty() ) {
        allocator.align(__alignof__(BindTargetRef));
        uint64_t off           = allocator.size() - serializationStart;
        p->overrideBindTargetRefsOffset = (uint32_t)off;
        p->overrideBindTargetRefsCount = (uint32_t)overrideBindTargets.count();
        allocator.append(&overrideBindTargets[0], sizeof(BindTargetRef) * overrideBindTargets.count());
    }

    // append ObjCFixups
    uint32_t objcFixupsOffset = prebuiltObjC.serializeFixups(jitLoader, allocator);
    p->objcBinaryInfoOffset = (objcFixupsOffset == 0) ? 0 : (objcFixupsOffset - (uint32_t)serializationStart);

    // uuid
    // Note this will still set the UUID to 0's if there wasn't a UUID
    memcpy(p->uuid, jitLoader.uuid, sizeof(uuid_t));

    p->cpusubtype   = mf->cpusubtype;

    // append patch table
    p->patchTableOffset = 0;
    const DylibPatch*   patchTable;
    uint16_t            cacheDylibOverriddenIndex;
    if ( jitLoader.overridesDylibInCache(patchTable, cacheDylibOverriddenIndex) ) {
        if ( patchTable != nullptr ) {
            p->patchTableOffset = (uint32_t)(allocator.size() - serializationStart);
            uint32_t patchTableSize = sizeof(DylibPatch);
            for ( const DylibPatch* patch = patchTable; patch->overrideOffsetOfImpl != DylibPatch::endOfPatchTable; ++patch )
                patchTableSize += sizeof(DylibPatch);
            allocator.append(patchTable, patchTableSize);
        }
    }
}

bool PrebuiltLoader::overridesDylibInCache(const DylibPatch*& patchTable, uint16_t& cacheDylibOverriddenIndex) const
{
    if ( !this->isCatalystOverride )
        return false;

    patchTable                = (this->patchTableOffset == 0) ? nullptr : (DylibPatch*)(((uint8_t*)this) + this->patchTableOffset);
    cacheDylibOverriddenIndex = this->indexOfTwin;
    return true;
}

void PrebuiltLoader::withLayout(Diagnostics &diag, const RuntimeState& state,
                                void (^callback)(const mach_o::Layout &layout)) const
{
#if SUPPORT_VM_LAYOUT
    // TODO: We might be able to do better here, eg, using the segments on the Loader instead
    // of parsing the MachO.
    this->analyzer(state)->withVMLayout(diag, callback);
#else
    // In the cache builder, we must have set a layout
    assert(!this->ref.app);
    const mach_o::Layout* layout = state.cachedDylibLayout(this->ref.index);
    assert(layout != nullptr);
    callback(*layout);
#endif
}

#if BUILDING_CLOSURE_UTIL

// Prints a string with any special characters delimited
static void printJSONString(FILE* out, const char* str)
{
    while ( *str != '\0' ) {
        char c = *str;
        if (c == '"')
            fputc('\\', out);
        fputc(c, out);
        ++str;
    }
}

void PrebuiltLoader::print(RuntimeState& state, FILE* out, bool printComments) const
{
    fprintf(out, "    {\n");
    fprintf(out, "      \"path\":    \"");
    printJSONString(out, path(state));
    fprintf(out, "\",\n");
    if ( altPathOffset != 0 ) {
        fprintf(out, "      \"path-alt\":    \"");
        printJSONString(out, (char*)this + this->altPathOffset);
        fprintf(out, "\",\n");
    }
    fprintf(out, "      \"loader\":  \"%c.%d\",\n", ref.app ? 'a' : 'c', ref.index);
    fprintf(out, "      \"vm-size\": \"0x%X\",\n", this->vmSpace);
    if ( this->dylibInDyldCache ) {
        fprintf(out, "      \"overridable\": \"%s\",\n", this->isOverridable ? "true" : "false");
        fprintf(out, "      \"supports-catalyst\": \"%s\",\n", this->supportsCatalyst ? "true" : "false");
        fprintf(out, "      \"catalyst-override\": \"%s\",\n", this->isCatalystOverride ? "true" : "false");
        if ( this->indexOfTwin != kNoUnzipperedTwin ) {
            if ( this->supportsCatalyst )
                fprintf(out, "      \"mac-twin\": \"c.%d\",", this->indexOfTwin);
            else
                fprintf(out, "      \"catalyst-twin\": \"c.%d\",", this->indexOfTwin);
           if ( printComments ) {
                PrebuiltLoader::LoaderRef twinRef(false, this->indexOfTwin);
                const char* twinPath =  twinRef.loader(state)->path(state);
                fprintf(out, "     # %s", twinPath);
            }
            fprintf(out, "\n");
            if ( this->patchTableOffset != 0 ) {
                uint32_t patchTableSizeCount = 0;
                for ( const DylibPatch* patch = (DylibPatch*)(((uint8_t*)this) + this->patchTableOffset); patch->overrideOffsetOfImpl != DylibPatch::endOfPatchTable; ++patch )
                    patchTableSizeCount++;
                fprintf(out, "      \"patch-table-entries\": \"%d\",\n", patchTableSizeCount);
            }
        }
    }
    fprintf(out, "      \"has-initializers\": \"%s\",\n", this->hasInitializers ? "true" : "false");
    bool needComma = false;
    fprintf(out, "      \"segments\": [");
    for ( const Region& seg : this->segments() ) {
        if ( needComma )
            fprintf(out, ",");
        fprintf(out, "\n        {\n");
        fprintf(out, "          \"vm-offset\":       \"0x%llX\",\n", seg.vmOffset);
        fprintf(out, "          \"file-size\":       \"0x%X\",\n", seg.fileSize);
        fprintf(out, "          \"file-offset\":     \"0x%X\",\n", seg.fileOffset);
        char writeChar = (seg.perms & 2) ? 'w' : '-';
        if ( seg.readOnlyData )
            writeChar = 'W';
        fprintf(out, "          \"permissions\":     \"%c%c%c\"\n", ((seg.perms & 1) ? 'r' : '-'), writeChar, ((seg.perms & 4) ? 'x' : '-'));
        fprintf(out, "         }");
        needComma = true;
    }
    fprintf(out, "\n      ],\n");

    if ( this->fileValidationOffset != 0 ) {
        const FileValidationInfo* fileInfo = this->fileValidationInfo();
        fprintf(out, "      \"file-info\":  {\n");
        if ( fileInfo->checkInodeMtime ) {
            fprintf(out, "          \"slice-offset\":    \"0x%llX\",\n", fileInfo->sliceOffset);
            fprintf(out, "          \"deviceID\":        \"0x%llX\",\n", fileInfo->deviceID);
            fprintf(out, "          \"inode\":           \"0x%llX\",\n", fileInfo->inode);
            fprintf(out, "          \"mod-time\":        \"0x%llX\",\n", fileInfo->mtime);
        }
        fprintf(out, "          \"code-sig-offset\": \"0x%X\",\n", this->codeSignature.fileOffset);
        fprintf(out, "          \"code-sig-size\":   \"0x%X\",\n", this->codeSignature.size);
        if ( fileInfo->checkCdHash ) {
            fprintf(out, "          \"cd-hash\":         \"%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\"\n",
                fileInfo->cdHash[0], fileInfo->cdHash[1], fileInfo->cdHash[2], fileInfo->cdHash[3],
                fileInfo->cdHash[4], fileInfo->cdHash[5], fileInfo->cdHash[6], fileInfo->cdHash[7],
                fileInfo->cdHash[8], fileInfo->cdHash[9], fileInfo->cdHash[10], fileInfo->cdHash[11],
                fileInfo->cdHash[12], fileInfo->cdHash[13], fileInfo->cdHash[14], fileInfo->cdHash[15],
                fileInfo->cdHash[16], fileInfo->cdHash[17], fileInfo->cdHash[18], fileInfo->cdHash[19]);
        }
        fprintf(out, "       },\n");
    }

    if ( exportsTrieLoaderOffset != 0 ) {
        fprintf(out, "      \"exports-trie\":  {\n");
        fprintf(out, "          \"vm-offset\":      \"0x%llX\",\n", exportsTrieLoaderOffset);
        fprintf(out, "          \"size\":           \"0x%X\"\n", exportsTrieLoaderSize);
        fprintf(out, "      },\n");
    }

    fprintf(out, "      \"dependents\": [");
    const PrebuiltLoader::LoaderRef* depsArray = (PrebuiltLoader::LoaderRef*)((uint8_t*)this + this->dependentLoaderRefsArrayOffset);
    needComma                                  = false;
    for ( uint32_t depIndex = 0; depIndex < this->depCount; ++depIndex ) {
        if ( needComma )
            fprintf(out, ",");
        PrebuiltLoader::LoaderRef dep     = depsArray[depIndex];
        char                      depAttrsStr[128];
        LinkedDylibAttributes  depAttrs  = LinkedDylibAttributes::regular;
        if ( this->dependentKindArrayOffset != 0 ) {
            const LinkedDylibAttributes* kindsArray = (LinkedDylibAttributes*)((uint8_t*)this + this->dependentKindArrayOffset);
            depAttrs = kindsArray[depIndex];
        }
        else {
            if ( depAttrs == LinkedDylibAttributes::regular ) {
                strlcpy(depAttrsStr, "regular", sizeof(depAttrsStr));
            }
            else {
                depAttrsStr[0] = '\0';
                if ( depAttrs.weakLink )
                    strlcat(depAttrsStr, "weak ", sizeof(depAttrsStr));
                if ( depAttrs.upward )
                    strlcat(depAttrsStr, "upward ", sizeof(depAttrsStr));
                if ( depAttrs.reExport )
                    strlcat(depAttrsStr, "re-export ", sizeof(depAttrsStr));
                if ( depAttrs.delayInit )
                    strlcat(depAttrsStr, "delay ", sizeof(depAttrsStr));
            }
        }
        const char* depPath = dep.isMissingWeakImage() ? "missing weak link" : dep.loader(state)->path(state);
        fprintf(out, "\n          {\n");
        fprintf(out, "              \"kind\":           \"%s\",\n", depAttrsStr);
        fprintf(out, "              \"loader\":         \"%c.%d\"", dep.app ? 'a' : 'c', dep.index);
        if ( printComments )
            fprintf(out, "     # %s\n", depPath);
        else
            fprintf(out, "\n");
        fprintf(out, "          }");
        needComma = true;
    }
    fprintf(out, "\n      ]");
    if ( bindTargetRefsOffset != 0 ) {
        fprintf(out, ",\n      \"targets\": [");
        needComma = false;
        for ( const BindTargetRef& target : this->bindTargets() ) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n          {\n");
            if ( target.isAbsolute() ) {
                fprintf(out, "              \"absolute-value\":      \"0x%llX\"\n", target.value(state));
            }
            else {
                fprintf(out, "              \"loader\":     \"%c.%d\",", target.loaderRef().app ? 'a' : 'c', target.loaderRef().index);
                if ( printComments )
                    fprintf(out, "        # %s\n", target.loaderRef().loader(state)->path(state));
                else
                    fprintf(out, "\n");
                uint64_t fvTableOffset;
                uint16_t variantIndex;
                if ( target.isFunctionVariant(fvTableOffset, variantIndex) ) {
                    fprintf(out, "              \"fvt-offset\": \"0x%08llX\",\n", fvTableOffset);
                    fprintf(out, "              \"fvt-index\":  \"%u\"\n", variantIndex);
                }
                else {
                    fprintf(out, "              \"offset\":     \"0x%08llX\"\n", target.offset());
                }
            }
            fprintf(out, "          }");
            needComma = true;
        }
        fprintf(out, "\n      ]");
    }

    if ( overrideBindTargetRefsOffset != 0 ) {
        fprintf(out, ",\n      \"override-targets\": [");
        needComma = false;
        for ( const BindTargetRef& target : this->overrideBindTargets() ) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n          {\n");
            if ( target.isAbsolute() ) {
                fprintf(out, "              \"absolute-value\":    \"0x%llX\"\n", target.value(state));
            }
            else {
                fprintf(out, "              \"loader\":   \"%c.%d\",", target.loaderRef().app ? 'a' : 'c', target.loaderRef().index);
                if ( printComments )
                    fprintf(out, "        # %s\n", target.loaderRef().loader(state)->path(state));
                else
                    fprintf(out, "\n");
                fprintf(out, "              \"offset\":   \"0x%08llX\"\n", target.offset());
            }
            fprintf(out, "          }");
            needComma = true;
        }
        fprintf(out, "\n      ]");
    }

    if ( objcBinaryInfoOffset != 0 )
        printObjCFixups(state, out);

    fprintf(out, "\n ");

    fprintf(out, "    }\n");
}

#endif // BUILDING_CLOSURE_UTIL


//
// MARK: --- PrebuiltLoaderSet methods ---
//

bool PrebuiltLoaderSet::hasValidMagic() const
{
    return (this->magic == kMagic);
}

bool PrebuiltLoaderSet::contains(const void* p, size_t pLen) const
{
    if ( (uint8_t*)p < (uint8_t*)this )
        return false;
    if ( ((uint8_t*)p + pLen) > ((uint8_t*)this + length) )
        return false;
    return true;
}


bool PrebuiltLoaderSet::validHeader(RuntimeState& state) const
{
    // verify this is the current PrebuiltLoaderSet format
    if ( !this->hasValidMagic() ) {
        if ( state.config.log.loaders )
            console("not using PrebuiltLoaderSet %p because magic at start does not match\n", this);
        return false;
    }
    if ( this->versionHash != PREBUILTLOADER_VERSION ) {
        if ( state.config.log.loaders ) {
            console("not using PrebuiltLoaderSet %p because versionHash (0x%08X) does not match dyld (0x%08X)\n",
                        this, this->versionHash, PREBUILTLOADER_VERSION);
        }
        return false;
    }
    return true;
}

#if SUPPORT_VM_LAYOUT
bool PrebuiltLoaderSet::isValid(RuntimeState& state) const
{
    // verify this is the current PrebuiltLoaderSet format
    if ( !this->validHeader(state) )
        return false;

    // verify current dyld cache is same as when PrebuiltLoaderSet was built
    uuid_t expectedCacheUUID;
    if ( this->hasCacheUUID(expectedCacheUUID) ) {
        if ( const DyldSharedCache* cache = state.config.dyldCache.addr ) {
            uuid_t actualCacheUUID;
            cache->getUUID(actualCacheUUID);
            if ( ::memcmp(expectedCacheUUID, actualCacheUUID, sizeof(uuid_t)) != 0 ) {
                if ( state.config.log.loaders )
                    console("not using PrebuiltLoaderSet %p because cache UUID does not match\n", this);
                return false;
            }
        }
        else {
            // PrebuiltLoaderSet was built with a dyld cache, but this process does not have a cache
            if ( state.config.log.loaders )
                console("not using PrebuiltLoaderSet %p because process does not have a dyld cache\n", this);
            return false;
        }
    }

    // verify must-be-missing files are still missing
    __block bool missingFileShowedUp = false;
    this->forEachMustBeMissingPath(^(const char* path, bool& stop) {
        if ( state.config.syscall.fileExists(path) ) {
            if ( state.config.log.loaders )
                console("not using PrebuiltLoaderSet %p because existence of file '%s' invalids the PrebuiltLoaderSet\n", this, path);
            missingFileShowedUp = true;
            stop = true;
        }
    });
    if ( missingFileShowedUp )
        return false;

    // verify all PrebuiltLoaders in the set are valid
    bool somethingInvalid = false;
    for ( uint32_t i = 0; i < loadersArrayCount; ++i ) {
        const PrebuiltLoader* ldr = this->atIndex(i);
        if ( !ldr->isValid(state) )
            somethingInvalid = true;
    }

    return !somethingInvalid;
}
#endif // SUPPORT_VM_LAYOUT

const PrebuiltLoader* PrebuiltLoaderSet::findLoader(const RuntimeState& state, const char* path) const
{
    uint16_t imageIndex;
    if ( this->findIndex(state, path, imageIndex) )
        return this->atIndex(imageIndex);
    return nullptr;
}

void PrebuiltLoaderSet::forEachMustBeMissingPath(void (^callback)(const char* path, bool& stop)) const
{
    bool stop = false;
    const char* path = (char*)this + this->mustBeMissingPathsOffset;
    for ( uint32_t i = 0; !stop && (i < this->mustBeMissingPathsCount); ++i ) {
        callback(path, stop);
        path += strlen(path)+1;
    }
}

bool PrebuiltLoaderSet::findIndex(const RuntimeState& state, const char* path, uint16_t& index) const
{
    for ( uint32_t i = 0; i < loadersArrayCount; ++i ) {
        const PrebuiltLoader* loader = this->atIndex(i);
        if ( strcmp(loader->path(state), path) == 0 ) {
            index = i;
            return true;
        }
    }
    return false;
}

bool PrebuiltLoaderSet::hasCacheUUID(uuid_t uuid) const
{
    if ( this->dyldCacheUUIDOffset == 0 )
        return false;
    ::memcpy(uuid, (uint8_t*)this + this->dyldCacheUUIDOffset, sizeof(uuid_t));
    return true;
}

const void* PrebuiltLoaderSet::objcSelectorMap() const {
    if ( this->objcSelectorHashTableOffset == 0 )
        return nullptr;
    return (const void*)((uint8_t*)this + this->objcSelectorHashTableOffset);
}

const void* PrebuiltLoaderSet::objcClassMap() const {
    if ( this->objcClassHashTableOffset == 0 )
        return nullptr;
    return (const void*)((uint8_t*)this + this->objcClassHashTableOffset);
}

const void* PrebuiltLoaderSet::objcProtocolMap() const {
    if ( this->objcProtocolHashTableOffset == 0 )
        return nullptr;
    return (const void*)((uint8_t*)this + this->objcProtocolHashTableOffset);
}

const uint64_t* PrebuiltLoaderSet::swiftTypeProtocolTable() const {
    if ( this->swiftTypeConformanceTableOffset == 0 )
        return nullptr;
    return (const uint64_t*)((uint8_t*)this + this->swiftTypeConformanceTableOffset);
}

const uint64_t* PrebuiltLoaderSet::swiftMetadataProtocolTable() const {
    if ( this->swiftMetadataConformanceTableOffset == 0 )
        return nullptr;
    return (const uint64_t*)((uint8_t*)this + this->swiftMetadataConformanceTableOffset);
}

const uint64_t* PrebuiltLoaderSet::swiftForeignTypeProtocolTable() const {
    if ( this->swiftForeignTypeConformanceTableOffset == 0 )
        return nullptr;
    return (const uint64_t*)((uint8_t*)this + this->swiftForeignTypeConformanceTableOffset);
}

bool PrebuiltLoaderSet::hasOptimizedSwift() const {
    return this->swiftTypeConformanceTableOffset != 0
    || this->swiftMetadataConformanceTableOffset != 0
    || this->swiftForeignTypeConformanceTableOffset != 0;
}

void PrebuiltLoaderSet::logDuplicateObjCClasses(RuntimeState& state) const
{
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    if ( const void* classesHashTable = objcClassMap() ) {
        if ( !(this->objcFlags & ObjCFlags::hasDuplicateClasses) || !state.config.log.initializers )
            return;

        // The main executable can contain a list of duplicates to ignore.
        const dyld3::MachOAnalyzer* mainMA = (const dyld3::MachOAnalyzer*)state.mainExecutableLoader->loadAddress(state);
        __block dyld3::CStringMapTo<bool> duplicateClassesToIgnore;
        mainMA->forEachObjCDuplicateClassToIgnore(^(const char *className) {
            duplicateClassesToIgnore[className] = true;
        });

        prebuilt_objc::forEachClass(classesHashTable, ^(const PrebuiltLoader::BindTargetRef& nameTarget,
                                                        const dyld3::Array<const PrebuiltLoader::BindTargetRef*>& implTargets) {
            // Skip entries without duplicates
            if ( implTargets.count() == 1 )
                return;

            // The first target is the one we warn everyone else is a duplicate against
            const char* className = (const char*)nameTarget.value(state);
            if ( duplicateClassesToIgnore.find(className) != duplicateClassesToIgnore.end() )
                return;

            const char* oldPath = implTargets[0]->loaderRef().loader(state)->path(state);
            const void* oldCls = (const void*)implTargets[0]->value(state);
            for ( const PrebuiltLoader::BindTargetRef* implTarget : implTargets.subArray(1, implTargets.count() - 1) ) {
                const char* newPath = implTarget->loaderRef().loader(state)->path(state);
                const void* newCls = (const void*)implTarget->value(state);
                state.log("Class %s is implemented in both %s (%p) and %s (%p). "
                          "One of the two will be used. Which one is undefined.\n",
                          className, oldPath, oldCls, newPath, newCls);
            }
        });
    }
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS
}

#if BUILDING_CLOSURE_UTIL
void PrebuiltLoaderSet::print(RuntimeState& state, FILE* out, bool printComments) const
{
    fprintf(out, "{\n");
    fprintf(out, "  \"loaders\": [\n");
    __block bool needComma = false;
    for ( uint32_t i = 0; i < loadersArrayCount; ++i ) {
        if ( needComma )
            fprintf(out, ",\n");
        atIndex(i)->print(state, out, printComments);
        needComma = true;
    }
    fprintf(out, "  ]");

    if ( this->mustBeMissingPathsCount > 0 ) {
        fprintf(out, ",\n  \"must-be-missing\": [\n");
        needComma = false;
        this->forEachMustBeMissingPath(^(const char* path, bool& stop) {
            if ( needComma )
                fprintf(out, ",\n");
            fprintf(out, "        \"%s\"", path);
            needComma = true;
        });
        fprintf(out, "\n    ]");
    }

    if ( this->cachePatchCount > 0 ) {
        fprintf(out, ",\n  \"cache-overrides\": [\n");
        needComma = false;
        this->forEachCachePatch(^(const CachePatch& patch) {
            if ( needComma )
                fprintf(out, ",\n");
            fprintf(out, "     {\n");
            fprintf(out, "        \"cache-dylib\":     \"%d\",\n", patch.cacheDylibIndex);
            fprintf(out, "        \"dylib-offset\":    \"0x%08X\",\n", patch.cacheDylibVMOffset);
            fprintf(out, "        \"replace-loader\":  \"%c.%d\",\n", patch.patchTo.loaderRef().app ? 'a' : 'c', patch.patchTo.loaderRef().index);
            fprintf(out, "        \"replace-offset\":  \"0x%08llX\"\n", patch.patchTo.offset());
            fprintf(out, "     }");
            needComma = true;
        });
        fprintf(out, "\n  ]");
    }

    // app specific ObjC selectors
    if ( const void* selOpt = this->objcSelectorMap() ) {
        fprintf(out, ",\n  \"selector-table\": [");
        needComma = false;

        prebuilt_objc::forEachSelectorStringEntry(selOpt, ^(const PrebuiltLoader::BindTargetRef& target) {
            const Loader::LoaderRef& ref = target.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");
            fprintf(out, "          \"loader\":   \"%c.%d\",\n",
                    ref.app ? 'a' : 'c', ref.index);
            fprintf(out, "          \"offset\":   \"0x%08llX\"\n", target.offset());
            fprintf(out, "      }");
            needComma = true;
        });

        fprintf(out, "\n  ]");
    }

    // Objc classes
    if ( const void* clsOpt = this->objcClassMap() ) {
        fprintf(out, ",\n  \"objc-class-table\": [");
        needComma = false;

        prebuilt_objc::forEachClass(clsOpt, ^(const PrebuiltLoader::BindTargetRef& nameTarget, const Array<const PrebuiltLoader::BindTargetRef*>& values) {
            const Loader::LoaderRef& nameRef = nameTarget.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");
            fprintf(out, "          \"name-loader\":   \"%c.%d\",\n",
                    nameRef.app ? 'a' : 'c', nameRef.index);
            fprintf(out, "          \"name-offset\":   \"0x%08llX\",\n", nameTarget.offset());

            if ( values.count() == 1 ) {
                const PrebuiltLoader::BindTargetRef& implTarget = *values[0];
                const Loader::LoaderRef& implRef = implTarget.loaderRef();
                fprintf(out, "          \"impl-loader\":   \"%c.%d\",\n",
                        implRef.app ? 'a' : 'c', implRef.index);
                fprintf(out, "          \"impl-offset\":   \"0x%08llX\"\n", implTarget.offset());
            } else {
                bool needImplComma = false;
                for ( const PrebuiltLoader::BindTargetRef* value : values ) {
                    if ( needImplComma )
                        fprintf(out, ",\n");

                    const PrebuiltLoader::BindTargetRef& implTarget = *value;
                    const Loader::LoaderRef& ref = implTarget.loaderRef();
                    fprintf(out, "          \"impl-loader\":   \"%c.%d\",\n",
                            ref.app ? 'a' : 'c', ref.index);
                    fprintf(out, "          \"impl-offset\":   \"0x%08llX\"", implTarget.offset());

                    needImplComma = true;
                }
            }
            fprintf(out, "\n");
            fprintf(out, "      }");
            needComma = true;
        });
        fprintf(out, "\n  ]");
    }

    // Objc protocols
    if ( const void* protocolOpt = this->objcProtocolMap() ) {
        fprintf(out, ",\n  \"objc-protocol-table\": [");
        needComma = false;

        prebuilt_objc::forEachProtocol(protocolOpt, ^(const PrebuiltLoader::BindTargetRef& nameTarget, const Array<const PrebuiltLoader::BindTargetRef*>& values) {
            const Loader::LoaderRef& nameRef = nameTarget.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");
            fprintf(out, "          \"name-loader\":   \"%c.%d\",\n",
                    nameRef.app ? 'a' : 'c', nameRef.index);
            fprintf(out, "          \"name-offset\":   \"0x%08llX\",\n", nameTarget.offset());

            if ( values.count() == 1 ) {
                const PrebuiltLoader::BindTargetRef& implTarget = *values[0];
                const Loader::LoaderRef& implRef = implTarget.loaderRef();
                fprintf(out, "          \"impl-loader\":   \"%c.%d\",\n",
                        implRef.app ? 'a' : 'c', implRef.index);
                fprintf(out, "          \"impl-offset\":   \"0x%08llX\"\n", implTarget.offset());
            } else {
                bool needImplComma = false;
                for ( const PrebuiltLoader::BindTargetRef* value : values ) {
                    if ( needImplComma )
                        fprintf(out, ",\n");

                    const PrebuiltLoader::BindTargetRef& implTarget = *value;
                    const Loader::LoaderRef& ref = implTarget.loaderRef();
                    fprintf(out, "          \"impl-loader\":   \"%c.%d\",\n",
                            ref.app ? 'a' : 'c', ref.index);
                    fprintf(out, "          \"impl-offset\":   \"0x%08llX\"", implTarget.offset());

                    needImplComma = true;
                }
            }
            fprintf(out, "\n");
            fprintf(out, "      }");
            needComma = true;
        });
        fprintf(out, "\n  ]");
    }

    if ( this->hasOptimizedSwift() ) {
        fprintf(out, ",\n  \"swift-conformance-tables\": {\n");
        fprintf(out, "      \"type-offset\":   \"0x%08X\"\n", this->swiftTypeConformanceTableOffset);
        fprintf(out, "      \"metadata-offset\":   \"0x%08X\"\n", this->swiftMetadataConformanceTableOffset);
        fprintf(out, "      \"foreign-type-offset\":   \"0x%08X\"\n", this->swiftForeignTypeConformanceTableOffset);
        fprintf(out, "\n  }");
    }

    // Swift type metadata table
    if ( const uint64_t* typeTableBuffer = this->swiftTypeProtocolTable() ) {
        TypeProtocolMap* typeProtocolMap = new (state.persistentAllocator.malloc(sizeof(TypeProtocolMap))) TypeProtocolMap(&state, typeTableBuffer);

        fprintf(out, ",\n  \"type-protocol-table\": [");
        needComma = false;

        typeProtocolMap->forEachEntry(^(const SwiftTypeProtocolConformanceDiskLocationKey &key, const Array<const SwiftTypeProtocolConformanceDiskLocation*>& values) {
            const Loader::LoaderRef& typeDescRef = key.typeDescriptor.loaderRef();
            const Loader::LoaderRef& protocolRef = key.protocol.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");

            // Type desc
            fprintf(out, "          \"type-desc\":   \"%c.%d\",\n",
                    typeDescRef.app ? 'a' : 'c', typeDescRef.index);
            fprintf(out, "          \"type-desc-offset\":   \"0x%08llX\",\n", key.typeDescriptor.offset());

            // Protocol
            fprintf(out, "          \"protocol\":   \"%c.%d\",\n",
                    protocolRef.app ? 'a' : 'c', protocolRef.index);
            fprintf(out, "          \"protocol-offset\":   \"0x%08llX\",\n", key.protocol.offset());

            // Values
            if ( values.count() == 1 ) {
                const PrebuiltLoader::BindTargetRef& implTarget = values[0]->protocolConformance;
                const Loader::LoaderRef& implRef = implTarget.loaderRef();
                fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                        implRef.app ? 'a' : 'c', implRef.index);
                fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"\n", implTarget.offset());
            } else {
                bool needImplComma = false;
                for ( const SwiftTypeProtocolConformanceDiskLocation* value : values ) {
                    const PrebuiltLoader::BindTargetRef& implTarget = value->protocolConformance;
                    if ( needImplComma )
                        fprintf(out, ",\n");

                    const Loader::LoaderRef& ref = implTarget.loaderRef();
                    fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                            ref.app ? 'a' : 'c', ref.index);
                    fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"", implTarget.offset());

                    needImplComma = true;
                }
                fprintf(out, "\n");
            }
            fprintf(out, "      }");
            needComma = true;
        });

        fprintf(out, "\n  ]");
    }

    // Swift metadata table
    if ( const uint64_t* metadataTableBuffer = this->swiftMetadataProtocolTable() ) {
        MetadataProtocolMap* metadataProtocolTable = new (state.persistentAllocator.malloc(sizeof(MetadataProtocolMap))) MetadataProtocolMap(&state, metadataTableBuffer);

        fprintf(out, ",\n  \"metadata-protocol-table\": [");
        needComma = false;

        metadataProtocolTable->forEachEntry(^(const SwiftMetadataProtocolConformanceDiskLocationKey &key, const Array<const SwiftMetadataProtocolConformanceDiskLocation*>& values) {
            const Loader::LoaderRef& metadataDescRef = key.metadataDescriptor.loaderRef();
            const Loader::LoaderRef& protocolRef = key.protocol.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");

            // Type desc
            fprintf(out, "          \"metadata-desc\":   \"%c.%d\",\n",
                    metadataDescRef.app ? 'a' : 'c', metadataDescRef.index);
            fprintf(out, "          \"metadata-desc-offset\":   \"0x%08llX\",\n", key.metadataDescriptor.offset());

            // Protocol
            fprintf(out, "          \"protocol\":   \"%c.%d\",\n",
                    protocolRef.app ? 'a' : 'c', protocolRef.index);
            fprintf(out, "          \"protocol-offset\":   \"0x%08llX\",\n", key.protocol.offset());

            // Values
            if ( values.count() == 1 ) {
                const PrebuiltLoader::BindTargetRef& implTarget = values[0]->protocolConformance;
                const Loader::LoaderRef& implRef = implTarget.loaderRef();
                fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                        implRef.app ? 'a' : 'c', implRef.index);
                fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"\n", implTarget.offset());
            } else {
                bool needImplComma = false;
                for ( const SwiftMetadataProtocolConformanceDiskLocation* value : values ) {
                    const PrebuiltLoader::BindTargetRef& implTarget = value->protocolConformance;
                    if ( needImplComma )
                        fprintf(out, ",\n");

                    const Loader::LoaderRef& ref = implTarget.loaderRef();
                    fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                            ref.app ? 'a' : 'c', ref.index);
                    fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"", implTarget.offset());

                    needImplComma = true;
                }
                fprintf(out, "\n");
            }
            fprintf(out, "      }");
            needComma = true;
        });

        fprintf(out, "\n  ]");
    }

    // Swift foreign type table
    if ( const uint64_t* foreignTableBuffer = this->swiftForeignTypeProtocolTable() ) {
        ForeignProtocolMap* foreignProtocolMap = new (state.persistentAllocator.malloc(sizeof(ForeignProtocolMap))) ForeignProtocolMap(&state, foreignTableBuffer);

        fprintf(out, ",\n  \"foreign-protocol-table\": [");
        needComma = false;

        foreignProtocolMap->forEachEntry(^(const SwiftForeignTypeProtocolConformanceDiskLocationKey &key, const Array<const SwiftForeignTypeProtocolConformanceDiskLocation*>& values) {
            const Loader::LoaderRef& foreignDescRef = key.foreignDescriptor.loaderRef();
            const Loader::LoaderRef& protocolRef = key.protocol.loaderRef();
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n      {\n");

            // Type desc
            fprintf(out, "          \"foreign-desc\":   \"%c.%d\",\n",
                    foreignDescRef.app ? 'a' : 'c', foreignDescRef.index);
            fprintf(out, "          \"foreign-desc-offset\":   \"0x%08llX\",\n", key.foreignDescriptor.offset());

            // Protocol
            fprintf(out, "          \"protocol\":   \"%c.%d\",\n",
                    protocolRef.app ? 'a' : 'c', protocolRef.index);
            fprintf(out, "          \"protocol-offset\":   \"0x%08llX\",\n", key.protocol.offset());

            // Values
            if ( values.count() == 1 ) {
                const PrebuiltLoader::BindTargetRef& implTarget = values[0]->protocolConformance;
                const Loader::LoaderRef& implRef = implTarget.loaderRef();
                fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                        implRef.app ? 'a' : 'c', implRef.index);
                fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"\n", implTarget.offset());
            } else {
                bool needImplComma = false;
                for ( const SwiftForeignTypeProtocolConformanceDiskLocation* value : values ) {
                    const PrebuiltLoader::BindTargetRef& implTarget = value->protocolConformance;
                    if ( needImplComma )
                        fprintf(out, ",\n");

                    const Loader::LoaderRef& ref = implTarget.loaderRef();
                    fprintf(out, "          \"conformance-loader\":   \"%c.%d\",\n",
                            ref.app ? 'a' : 'c', ref.index);
                    fprintf(out, "          \"conformance-offset\":   \"0x%08llX\"", implTarget.offset());

                    needImplComma = true;
                }
                fprintf(out, "\n");
            }
            fprintf(out, "      }");
            needComma = true;
        });

        fprintf(out, "\n  ]");
    }

    fprintf(out, "\n}\n");
}
#endif // BUILDING_CLOSURE_UTIL

const PrebuiltLoaderSet* PrebuiltLoaderSet::makeLaunchSet(Diagnostics& diag, RuntimeState& state, const MissingPaths& mustBeMissingPaths)
{
#if BUILDING_DYLD
    if ( !state.interposingTuplesAll.empty() || !state.patchedObjCClasses.empty() || !state.patchedSingletons.empty() ) {
        diag.error("cannot make PrebuiltLoaderSet for program that uses interposing");
        return nullptr;
    }
#elif BUILDING_CACHE_BUILDER
    // only dyld tries to populate state.interposingTuples, so in cache builder need to check for interposing in non-cached dylibs
    for ( const Loader* ldr : state.loaded ) {
        if ( ldr->dylibInDyldCache )
            break;
        const Header* hdr = (const Header*)ldr->mf(state);
        if ( hdr->isDylib() && hdr->hasInterposingTuples() ) {
            diag.error("cannot make PrebuiltLoaderSet for program that using interposing");
            return nullptr;
        }
    }
#endif
    if ( state.config.pathOverrides.dontUsePrebuiltForApp() ) {
        diag.error("cannot make PrebuiltLoaderSet for program that uses DYLD_* env vars");
        return nullptr;
    }
    if ( state.hasMissingFlatLazySymbols() ) {
        diag.error("cannot make PrebuiltLoaderSet for program that has missing flat lazy symbols");
        return nullptr;
    }

    // A launch may have JustInTimeLoaders at the top of the graph and PrebuiltLoaders at the bottom
    // The PrebuiltLoaders (from the dyld cache) may be re-used, so just make list of JIT ones
    STACK_ALLOC_ARRAY(JustInTimeLoader*, jitLoaders, state.loaded.size());
    uint16_t indexAsPrebuilt = 0;
    for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
        for ( const Loader* ldr : *list ) {
            if ( JustInTimeLoader* jl = (JustInTimeLoader*)(ldr->isJustInTimeLoader()) ) {
                if ( jl->dylibInDyldCache ) {
                    diag.error("cannot make PrebuiltLoader for dylib that is in dyld cache (%s)", jl->path(state));
                    return nullptr;
                }
                if ( jl->isOverrideOfCachedDylib() ) {
                    diag.error("cannot make PrebuiltLoader for dylib that overrides dylib in dyld cache (%s)", jl->path(state));
                    return nullptr;
                }
                jitLoaders.push_back(jl);
                jl->ref.app   = true;
                jl->ref.index = indexAsPrebuilt++;
            }
        }
    }

    // build objc and swift since we are going to save this for next time
    PrebuiltObjC prebuiltObjC;
    PrebuiltSwift prebuiltSwift;
    {
        Diagnostics objcDiag;
        prebuiltObjC.make(objcDiag, state);

        if ( !objcDiag.hasError() ) {
            Diagnostics swiftDiag;
            prebuiltSwift.make(swiftDiag, prebuiltObjC, state);
        }
        // We deliberately disregard the diagnostic object as we can run without objc or swift
        //TODO: Tell the user why their objc prevents faster launches
    }

    // initialize header of PrebuiltLoaderSet
    const uint64_t count = jitLoaders.count();
    __block BumpAllocator   allocator;
    allocator.zeroFill(sizeof(PrebuiltLoaderSet));
    BumpAllocatorPtr<PrebuiltLoaderSet> set(allocator, 0);
    set->magic               = kMagic;
    set->versionHash         = PREBUILTLOADER_VERSION;
    set->loadersArrayCount   = (uint32_t)count;
    set->loadersArrayOffset  = sizeof(PrebuiltLoaderSet);
    set->cachePatchCount     = 0;
    set->cachePatchOffset    = 0;
    set->dyldCacheUUIDOffset = 0;
    set->objcSelectorHashTableOffset    = 0;
    set->objcClassHashTableOffset       = 0;
    set->objcProtocolHashTableOffset    = 0;
    set->objcFlags                      = 0;
    set->objcProtocolClassCacheOffset   = 0;
    set->swiftTypeConformanceTableOffset          = 0;
    set->swiftMetadataConformanceTableOffset      = 0;
    set->swiftForeignTypeConformanceTableOffset   = 0;

    // initialize array of Loader offsets to zero
    allocator.zeroFill(count * sizeof(uint32_t));

#if BUILDING_DYLD
    // save UUID of dyld cache these PrebuiltLoaders were made against
    if ( const DyldSharedCache* cache = state.config.dyldCache.addr ) {
        set->dyldCacheUUIDOffset = (uint32_t)allocator.size();
        uuid_t uuid;
        cache->getUUID(uuid);
        allocator.append(uuid, sizeof(uuid_t));
    }
#endif

    // use block to save up all cache patches found while binding rest of PrebuiltClosureSet
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(CachePatch, cachePatches, 16);
    Loader::CacheWeakDefOverride cacheWeakDefFixup = ^(uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const Loader::ResolvedSymbol& target) {
        //state.log("patch index=%d, cacheOffset=0x%08x, symbol=%s, targetLoader=%s\n", cachedDylibIndex, exportCacheOffset, target.targetSymbolName, target.targetLoader->leafName());
        CachePatch patch = { cachedDylibIndex, cachedDylibVMOffset, PrebuiltLoader::BindTargetRef(diag, state, target) };
        cachePatches.push_back(patch);
    };

    // serialize and append each image to PrebuiltLoaderSet
    for ( uintptr_t i = 0; i < count; ++i ) {
        uint32_t* loadersOffsetsAray = (uint32_t*)((uint8_t*)set.get() + set->loadersArrayOffset);
        loadersOffsetsAray[i]        = (uint32_t)allocator.size();
        Loader::LoaderRef buildingRef(true, i);
        PrebuiltLoader::serialize(diag, state, *jitLoaders[i], buildingRef, cacheWeakDefFixup,
                                  prebuiltObjC, prebuiltSwift, allocator);
        if ( diag.hasError() )
            return nullptr;
    }

    // Add objc if we have it
    if ( prebuiltObjC.builtObjC ) {
        // Selector hash table
        if ( !prebuiltObjC.selectorMap.empty() ) {
            set->objcSelectorHashTableOffset = prebuiltObjC.serializeSelectorMap(allocator);
            allocator.align(8);
        }
        // Classes hash table
        if ( !prebuiltObjC.classMap.empty() ) {
            set->objcClassHashTableOffset = prebuiltObjC.serializeClassMap(allocator);
            allocator.align(8);
        }
        // Protocols hash table
        if ( !prebuiltObjC.protocolMap.empty() ) {
            set->objcProtocolHashTableOffset = prebuiltObjC.serializeProtocolMap(allocator);
            allocator.align(8);
        }
        set->objcProtocolClassCacheOffset = prebuiltObjC.objcProtocolClassCacheOffset.rawValue();

        // Set the flags
        if ( prebuiltObjC.hasClassDuplicates )
            set->objcFlags |= ObjCFlags::hasDuplicateClasses;
    }

    // Add swift if we have it
    if ( prebuiltSwift.builtSwift ) {
        // type conformances hash table
        if ( !prebuiltSwift.typeProtocolConformances.array().empty() ) {
            set->swiftTypeConformanceTableOffset = (uint32_t)allocator.size();
            prebuiltSwift.typeProtocolConformances.serialize(allocator);
            allocator.align(8);
        }
        // metadata conformances hash table
        if ( !prebuiltSwift.metadataProtocolConformances.array().empty() ) {
            set->swiftMetadataConformanceTableOffset = (uint32_t)allocator.size();
            prebuiltSwift.metadataProtocolConformances.serialize(allocator);
            allocator.align(8);
        }
        // foreign type conformances hash table
        if ( !prebuiltSwift.foreignProtocolConformances.array().empty() ) {
            // HACK: Before we serialize the table, null out the "originalPointer".  We need to remove it
            prebuiltSwift.foreignProtocolConformances.forEachKey(^(SwiftForeignTypeProtocolConformanceDiskLocationKey& key) {
                key.originalPointer = 0;
            });
            set->swiftForeignTypeConformanceTableOffset = (uint32_t)allocator.size();
            prebuiltSwift.foreignProtocolConformances.serialize(allocator);
            allocator.align(8);
        }
    }

    // add cache patches to end
    if ( !cachePatches.empty() ) {
        set->cachePatchOffset = (uint32_t)allocator.size();
        for ( const CachePatch& patch : cachePatches ) {
            allocator.append(&patch, sizeof(patch));
            set->cachePatchCount += 1;
        }
    }

    // add must-be-missing paths to end
    if ( mustBeMissingPaths.size() != 0 ) {
        set->mustBeMissingPathsOffset = (uint32_t)allocator.size();
        mustBeMissingPaths.forEachPath(^(const char* path) {
            allocator.append(path, strlen(path)+1);
            set->mustBeMissingPathsCount += 1;
        });
    }

    // record final length
    set->length = (uint32_t)allocator.size();

    PrebuiltLoaderSet* result = (PrebuiltLoaderSet*)allocator.finalize();
    // result->print(state, stderr, false);
    return result;
}

void PrebuiltLoaderSet::forEachCachePatch(void (^handler)(const CachePatch&)) const
{
    const CachePatch* patchArray = (CachePatch*)((uint8_t*)this + this->cachePatchOffset);
    for ( uint32_t i = 0; i < this->cachePatchCount; ++i ) {
        handler(patchArray[i]);
    }
}

void PrebuiltLoaderSet::deallocate() const
{
    uintptr_t used = round_page(this->size());
    ::vm_deallocate(mach_task_self(), (long)this, used);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
const PrebuiltLoaderSet* PrebuiltLoaderSet::makeDyldCachePrebuiltLoaders(Diagnostics& diag, RuntimeState& state, const Array<const Loader*>& jitLoaders)
{
    // scan JITLoaders and assign them prebuilt slots
    uint16_t indexAsPrebuilt = 0;
    for ( const Loader* ldr : jitLoaders ) {
        if ( ldr->isPrebuilt ) {
            diag.error("unexpected prebuilt loader in cached dylibs (%s)", ldr->path(state));
            return nullptr;
        }
        JustInTimeLoader* jldr = (JustInTimeLoader*)ldr;
        jldr->ref.app          = false;
        jldr->ref.index        = indexAsPrebuilt++;
    }

    // initialize header of PrebuiltLoaderSet
    const uintptr_t count = jitLoaders.count();
    BumpAllocator   allocator;
    allocator.zeroFill(sizeof(PrebuiltLoaderSet));
    BumpAllocatorPtr<PrebuiltLoaderSet> set(allocator, 0);
    set->magic               = kMagic;
    set->versionHash         = PREBUILTLOADER_VERSION;
    set->loadersArrayCount   = (uint32_t)count;
    set->loadersArrayOffset  = sizeof(PrebuiltLoaderSet);
    set->cachePatchCount     = 0;
    set->cachePatchOffset    = 0;
    set->dyldCacheUUIDOffset = 0;
    // initialize array of Loader offsets to zero
    allocator.zeroFill(count * sizeof(uint32_t));

    // serialize and append each image to PrebuiltLoaderSet
    for ( uintptr_t i = 0; i < count; ++i ) {
        BumpAllocatorPtr<uint32_t> loadersOffsetsArray(allocator, set->loadersArrayOffset);
        loadersOffsetsArray.get()[i] = (uint32_t)allocator.size();
        Loader::LoaderRef buildingRef(false, i);
        PrebuiltObjC prebuiltObjC;
        PrebuiltSwift prebuiltSwift;
        PrebuiltLoader::serialize(diag, state, *((JustInTimeLoader*)jitLoaders[i]), buildingRef,
                                  nullptr, prebuiltObjC, prebuiltSwift, allocator);
        if ( diag.hasError() )
            return nullptr;
    }

    set->length = (uint32_t)allocator.size();

    PrebuiltLoaderSet* result = (PrebuiltLoaderSet*)allocator.finalize();
    //    result->print();
    return result;
}
#endif


//
// MARK: --- BumpAllocator methods ---
//

void BumpAllocator::append(const void* payload, uint64_t payloadSize)
{
    uint64_t startSize = size();
    zeroFill(payloadSize);
    uint8_t* p = _vmAllocationStart + startSize;
    memcpy(p, payload, (size_t)payloadSize);
}

void BumpAllocator::zeroFill(uint64_t reqSize)
{
    const size_t allocationChunk = 1024*1024;
    uint64_t remaining = _vmAllocationSize - this->size();
    if ( reqSize > remaining ) {
        // if current buffer too small, grow it
        uint64_t growth = _vmAllocationSize;
        if ( growth < allocationChunk )
            growth = allocationChunk;
        if ( growth < reqSize )
            growth = allocationChunk * ((reqSize / allocationChunk) + 1);
        vm_address_t newAllocationAddr;
        uint64_t     newAllocationSize = _vmAllocationSize + growth;
        ::vm_allocate(mach_task_self(), &newAllocationAddr, (vm_size_t)newAllocationSize, VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_DYLD));
        assert(newAllocationAddr != 0);
        uint64_t currentInUse = this->size();
        if ( _vmAllocationStart != nullptr ) {
            ::memcpy((void*)newAllocationAddr, _vmAllocationStart, (size_t)currentInUse);
            ::vm_deallocate(mach_task_self(), (vm_address_t)_vmAllocationStart, (vm_size_t)_vmAllocationSize);
        }
        _usageEnd          = (uint8_t*)(newAllocationAddr + currentInUse);
        _vmAllocationStart = (uint8_t*)newAllocationAddr;
        _vmAllocationSize  = newAllocationSize;
    }
    assert((uint8_t*)_usageEnd + reqSize <= (uint8_t*)_vmAllocationStart + _vmAllocationSize);
    _usageEnd += reqSize;
}

void BumpAllocator::align(unsigned multipleOf)
{
    size_t extra = size() % multipleOf;
    if ( extra == 0 )
        return;
    zeroFill(multipleOf - extra);
}

// truncates buffer to size used, makes it read-only, then returns pointer and clears BumpAllocator fields
const void* BumpAllocator::finalize()
{
    // trim vm allocation down to just what is needed
    uintptr_t bufferStart = (uintptr_t)_vmAllocationStart;
    uintptr_t used        = round_page(this->size());
    if ( used < _vmAllocationSize ) {
        uintptr_t deallocStart = bufferStart + used;
        ::vm_deallocate(mach_task_self(), deallocStart, (vm_size_t)(_vmAllocationSize - used));
        _usageEnd         = nullptr;
        _vmAllocationSize = used;
    }
    // mark vm region read-only
    ::vm_protect(mach_task_self(), bufferStart, used, false, VM_PROT_READ);
    _vmAllocationStart = nullptr;
    return (void*)bufferStart;
}

BumpAllocator::~BumpAllocator()
{
    if ( _vmAllocationStart != nullptr ) {
        ::vm_deallocate(mach_task_self(), (vm_address_t)_vmAllocationStart, (vm_size_t)_vmAllocationSize);
        _vmAllocationStart  = nullptr;
        _vmAllocationSize   = 0;
        _usageEnd           = nullptr;
    }
}


//
// MARK: --- MissingPaths methods ---
//

void MissingPaths::addPath(const char* path)
{
    this->append(path, strlen(path)+1);
}

void MissingPaths::forEachPath(void (^callback)(const char* path)) const
{
    for (const uint8_t* s = _vmAllocationStart; s < _usageEnd; ++s) {
        const char* str = (char*)s;
        callback(str);
        s += strlen(str);
    }
}


} // namespace dyld4

#endif // SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

#endif // !TARGET_OS_EXCLAVEKIT
