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
bool PremappedLoader::dyldDoesObjCFixups() const
{
    return false; //TODO: double-check exclaves behaviour
}

void PremappedLoader::withLayout(Diagnostics &diag, const RuntimeState& state,
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

static bool hasDataConst(const Header* mh)
{
    __block bool result = false;
    mh->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.readOnlyData() )
            result = true;
    });
    return result;
}

PremappedLoader* PremappedLoader::make(RuntimeState& state, const MachOFile* mh, const char* path, bool willNeverUnload, bool overridesCache, uint16_t overridesDylibIndex, const mach_o::Layout* layout)
{
    bool allDepsAreNormal  = true;
    uint32_t depCount      = mh->dependentDylibCount(&allDepsAreNormal);
    uint32_t minDepCount   = (depCount ? depCount - 1 : 1);
    size_t loaderSize      = sizeof(PremappedLoader) + (minDepCount * sizeof(AuthLoader)) + (allDepsAreNormal ? 0 : depCount);
    size_t sizeNeeded      = loaderSize + strlen(path) + 1;
    void* storage          = state.persistentAllocator.malloc(sizeNeeded);

    uuid_t uuid;

    Loader::InitialOptions options;
    options.inDyldCache     = DyldSharedCache::inDyldCache(state.config.dyldCache.addr, mh);
    options.hasObjc         = mh->hasObjC();
    options.mayHavePlusLoad = ((const Header*)mh)->hasPlusLoadMethod();
    options.roData          = hasDataConst((const Header*)mh);
    options.neverUnloaded   = willNeverUnload;
    options.leaveMapped     = true;
    options.roObjC          = options.hasObjc && mh->hasSection("__DATA_CONST", "__objc_selrefs");
    options.pre2022Binary   = true;
    options.hasUUID         = ((const Header*)mh)->getUuid(uuid);
    options.hasWeakDefs     = mh->hasWeakDefs();
    options.hasTLVs         = ((Header*)mh)->hasThreadLocalVariables();
    options.belowLibSystem  = mh->isDylib() && (strncmp(((const Header*)mh)->installName(), "/usr/lib/system/lib", 19) == 0);

    PremappedLoader* p = new (storage) PremappedLoader(mh, options, layout);

    // fill in extra data
    p->pathOffset       = loaderSize;
    p->depCount         = depCount;
    p->dependentsSet    = false;
    p->fixUpsApplied    = false;
    p->inited           = false;
    p->hidden           = false;
    p->altInstallName   = mh->isDylib() && (strcmp(((const Header*)mh)->installName(), path) != 0);;
    p->allDepsAreNormal = allDepsAreNormal;
    p->padding          = 0;

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
    p->overridesCache  = overridesCache;
    p->overrideIndex   = overridesDylibIndex;

    for ( unsigned i = 0; i < depCount; ++i ) {
        new (&p->dependents[i]) (AuthLoader) { nullptr };
        if ( !allDepsAreNormal ) {
            p->dependentAttrs(i) = LinkedDylibAttributes::regular;
        }
    }
    strlcpy(((char*)p) + p->pathOffset, path, PATH_MAX);
    state.add(p);

    if ( overridesCache ) {
        state.setHasOverriddenCachedDylib();
    }

    if ( state.config.log.loaders )
        state.log("using PremappedLoader %p for %s\n", p, path);

    return p;
}

Loader* PremappedLoader::makePremappedLoader(Diagnostics& diag, RuntimeState& state, const char* path, bool isInDyldCache, uint32_t dylibCacheIndex, const LoadOptions& options, const mach_o::Layout* layout)
{
    mach_header_64* mh = nullptr;
    bool wasPremapped = false;
    for (auto& mappedFile : state.config.process.preMappedFiles) {
        if ( strcmp(path, mappedFile.path) != 0 )
            continue;
        mh  = (mach_header_64*)mappedFile.loadAddress;
        wasPremapped = true;
        xrt_platform_premapped_macho_change_state(mh, XRT__PLATFORM_PREMAPPED_MACHO_READWRITE);
        break;
    }

    if ( mh == nullptr ) {
        // Image is not in the list of pre-mapped files, look in the shared cache
        if ( isInDyldCache ) {
            uint64_t mtime = 0;
            uint64_t inode = 0;
            mh = (mach_header_64*)state.config.dyldCache.getIndexedImageEntry(dylibCacheIndex, mtime, inode);
        }

        if ( mh == nullptr && !options.canBeMissing )
            diag.error("'%s' could not be found\n", path);
    }

    if ( mh == nullptr )
        return nullptr;

    bool overridesDyldCache = wasPremapped && isInDyldCache;

    Loader* result = PremappedLoader::make(state, (const MachOAnalyzer*)mh, path, true, overridesDyldCache, dylibCacheIndex, layout);
    result->ref.index = dylibCacheIndex;
    return result;
}

Loader* PremappedLoader::makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOAnalyzer* mainExec, const char* mainExecPath, const mach_o::Layout* layout)
{
    xrt_platform_premapped_macho_change_state((mach_header_64 *)mainExec, XRT__PLATFORM_PREMAPPED_MACHO_READWRITE);
    Loader* result = PremappedLoader::make(state, mainExec, mainExecPath, true /* willNeverUnload */, false /* overridesCache */, 0 /* overridesDylibIndex*/, layout);
    return result;
}

#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
} // namespace dyld4
