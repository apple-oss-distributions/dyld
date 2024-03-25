/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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
#include "Defines.h"
#include "DyldRuntimeState.h"
#include "JustInTimeLoader.h"

// mach_o
#include "Header.h"
#include "Version32.h"

#include "PremappedLoader.h"

using mach_o::Header;
using mach_o::Version32;

namespace dyld4 {

#if SUPPORT_CREATING_PREMAPPEDLOADERS
PremappedLoader::PremappedLoader(const MachOFile* mh, const Loader::InitialOptions& options, const mach_o::Layout* layout)
  : JustInTimeLoader(mh, options, FileID::none(), nullptr, true)
{
}

//////////////////////// "virtual" functions /////////////////////////////////

void PremappedLoader::loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options)
{
    if ( dependentsSet )
        return;

    // add first level of dependents
    __block int                 depIndex = 0;
    const mach_o::MachOFileRef& mf = this->mappedAddress;
    const Header*               mh = (Header*)(&mf->magic); // Better way?
    mh->forEachDependentDylib(^(const char* loadPath, DependentDylibAttributes depAttr, Version32 compatVersion, Version32 curVersion, bool& stop) {
        if ( !this->allDepsAreNormal )
            dependentAttrs(depIndex) = depAttr;
        const Loader* depLoader = nullptr;
        // for absolute paths, do a quick check if this is already loaded with exact match
        if ( loadPath[0] == '/' ) {
            for ( const Loader* ldr : state.loaded ) {
                if ( ldr->matchesPath(loadPath) ) {
                    depLoader = ldr;
                    break;
                }
            }
        }
        if ( depLoader == nullptr ) {
            // first load, so do full search
            LoadChain   nextChain { options.rpathStack, this };
            LoadOptions depOptions             = options;
            depOptions.requestorNeedsFallbacks = false;
            depOptions.rpathStack              = &nextChain;
            depOptions.canBeMissing            = depAttr.weakLink;
            Diagnostics depDiag;
            depLoader               = getLoader(depDiag, state, loadPath, depOptions);
            if ( depDiag.hasError() ) {
                char  fromUuidStr[64];
                this->getUuidStr(state, fromUuidStr);
                diag.error("Library not loaded: %s\n  Referenced from:  <%s> %s\n  Reason: %s\n", loadPath, fromUuidStr, this->path(), depDiag.errorMessage());
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
}

void PremappedLoader::applyFixups(Diagnostics& diag, RuntimeState& state, DyldCacheDataConstLazyScopedWriter&, bool allowLazyBinds) const
{
    // build targets table
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, bindTargets, 512);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(const void*, overrideTargetAddrs, 32);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(MissingFlatLazySymbol, missingFlatLazySymbols, 4);
    this->forEachBindTarget(diag, state, nullptr, allowLazyBinds, ^(const ResolvedSymbol& target, bool& stop) {
        const void* targetAddr = (const void*)Loader::interpose(state, Loader::resolvedAddress(state, target), this);
        if ( state.config.log.fixups ) {
            const char* targetLoaderName = target.targetLoader ? target.targetLoader->leafName() : "<none>";
            state.log("<%s/bind#%llu> -> %p (%s/%s)\n", this->leafName(), bindTargets.count(), targetAddr, targetLoaderName, target.targetSymbolName);
        }

        // Record missing flat-namespace lazy symbols
        if ( target.isMissingFlatLazy )
            missingFlatLazySymbols.push_back({ target.targetSymbolName, (uint32_t)bindTargets.count() });
        bindTargets.push_back(targetAddr);
    }, ^(const ResolvedSymbol& target, bool& stop) {
        // Missing weak binds need placeholders to make the target indices line up, but we should otherwise ignore them
        if ( (target.kind == Loader::ResolvedSymbol::Kind::bindToImage) && (target.targetLoader == nullptr) ) {
            if ( state.config.log.fixups )
                state.log("<%s/bind#%llu> -> missing-weak-bind (%s)\n", this->leafName(), overrideTargetAddrs.count(), target.targetSymbolName);

            overrideTargetAddrs.push_back((const void*)UINTPTR_MAX);
        } else {
            const void* targetAddr = (const void*)Loader::interpose(state, Loader::resolvedAddress(state, target), this);
            if ( state.config.log.fixups ) {
                const char* targetLoaderName = target.targetLoader ? target.targetLoader->leafName() : "<none>";
                state.log("<%s/bind#%llu> -> %p (%s/%s)\n", this->leafName(), overrideTargetAddrs.count(), targetAddr, targetLoaderName, target.targetSymbolName);
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
    this->applyFixupsGeneric(diag, state, ~0ULL, bindTargets, overrideTargetAddrs, true, missingFlatLazySymbols);


    // mark any __DATA_CONST segments read-only
    if ( this->hasConstantSegmentsToProtect() )
        this->makeSegmentsReadOnly(state);

    this->fixUpsApplied = true;
}

bool PremappedLoader::dyldDoesObjCFixups() const
{
    return false; //TODO: double-check exclaves behaviour
}

void PremappedLoader::withLayout(Diagnostics &diag, RuntimeState& state,
                        void (^callback)(const mach_o::Layout &layout)) const
{
    this->analyzer()->withVMLayout(diag, callback);
}

bool PremappedLoader::hasBeenFixedUp(RuntimeState&) const
{
    return this->fixUpsApplied;
}

bool PremappedLoader::beginInitializers(RuntimeState&)
{
    // do nothing if already initializers already run
    if ( this->inited )
        return true;

    // switch to being-inited state
    this->inited = true;
    return false;
}

//////////////////////// private functions  /////////////////////////////////

static bool hasPlusLoad(const MachOFile* mh)
{
    Diagnostics diag;
    return mh->hasPlusLoadMethod(diag);
}

static bool hasDataConst(const MachOFile* mh)
{
    __block bool result = false;
    mh->forEachSegment(^(const MachOAnalyzer::SegmentInfo& info, bool& stop) {
        if ( info.readOnlyData )
            result = true;
    });
    return result;
}

PremappedLoader* PremappedLoader::make(RuntimeState& state, const MachOFile* mh, const char* path, bool willNeverUnload, const mach_o::Layout* layout)
{
    bool allDepsAreNormal  = true;
    uint32_t depCount      = mh->dependentDylibCount(&allDepsAreNormal);
    uint32_t minDepCount   = (depCount ? depCount - 1 : 1);
    size_t loaderSize      = sizeof(PremappedLoader) + (minDepCount * sizeof(AuthLoader)) + (allDepsAreNormal ? 0 : depCount);
    size_t sizeNeeded      = loaderSize + strlen(path) + 1;
    void* storage          = state.persistentAllocator.malloc(sizeNeeded);

    Loader::InitialOptions options;
    options.inDyldCache     = false;
    options.hasObjc         = mh->hasObjC();
    options.mayHavePlusLoad = hasPlusLoad(mh);
    options.roData          = hasDataConst(mh);
    options.neverUnloaded   = willNeverUnload;
    options.leaveMapped     = true;
    options.roObjC          = options.hasObjc && mh->hasSection("__DATA_CONST", "__objc_selrefs");
    options.pre2022Binary   = true;

    PremappedLoader* p = new (storage) PremappedLoader(mh, options, layout);

    // fill in extra data
    p->pathOffset       = loaderSize;
    p->depCount         = depCount;
    p->dependentsSet    = false;
    p->fixUpsApplied    = false;
    p->inited           = false;
    p->hidden           = false;
    p->altInstallName   = mh->isDylib() && (strcmp(mh->installName(), path) != 0);;
    p->allDepsAreNormal = allDepsAreNormal;
    p->padding          = 0;

    parseSectionLocations(mh, p->sectionLocations);

    if ( !mh->hasExportTrie(p->exportsTrieRuntimeOffset, p->exportsTrieSize) ) {
        p->exportsTrieRuntimeOffset = 0;
        p->exportsTrieSize          = 0;
    }

    for ( unsigned i = 0; i < depCount; ++i ) {
        new (&p->dependents[i]) (AuthLoader) { nullptr };
        if ( !allDepsAreNormal ) {
            p->dependentAttrs(i) = DependentDylibAttributes::regular;
        }
    }
    strlcpy(((char*)p) + p->pathOffset, path, PATH_MAX);
    state.add(p);

    if ( state.config.log.loaders )
        state.log("using PremappedLoader %p for %s\n", p, path);

    return p;
}

Loader* PremappedLoader::makePremappedLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options, const mach_o::Layout* layout)
{
    Loader* result = nullptr;
    for (auto& mappedFile : state.config.process.preMappedFiles) {
        if ( strcmp(path, mappedFile.path) != 0 )
            continue;
        xrt_platform_premapped_macho_change_state((mach_header_64*)mappedFile.loadAddress, XRT__PLATFORM_PREMAPPED_MACHO_READWRITE);
        result = PremappedLoader::make(state, ( const MachOAnalyzer*)mappedFile.loadAddress, mappedFile.path, true, layout);
        break;
    }
    if ( result == nullptr && !options.canBeMissing ) {
        diag.error("'%s' could not be found in Pre-Mapped files\n", path);
    }

    return result;
}

Loader* PremappedLoader::makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOAnalyzer* mainExec, const char* mainExecPath, const mach_o::Layout* layout)
{
    xrt_platform_premapped_macho_change_state((mach_header_64 *)mainExec, XRT__PLATFORM_PREMAPPED_MACHO_READWRITE);
    Loader* result = PremappedLoader::make(state, mainExec, mainExecPath, true, layout);
    return result;
}

#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
} // namespace dyld4
