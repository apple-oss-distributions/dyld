/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/errno.h>
  #include <sys/mman.h>
#endif

#include <assert.h>
#include <mach-o/nlist.h>

// mach_o
#include "FunctionVariants.h"
#include "Image.h"
#include "Header.h"
#include "Platform.h"
#include "ChainedFixups.h"

// common
#include "StringUtils.h"
#include "MachOAnalyzer.h"
#include "Defines.h"
#include "Utilities.h"

// dyld
#include "Loader.h"
#include "JustInTimeLoader.h"
#include "PrebuiltLoader.h"
#include "PremappedLoader.h"
#include "DyldRuntimeState.h"
#include "DyldProcessConfig.h"
#include "Tracing.h"
#if BUILDING_DYLD && SUPPORT_ROSETTA
  #include "RosettaSupport.h"
#endif

#if TARGET_OS_EXCLAVEKIT
  #include <liblibc/plat/dyld/exclaves_dyld.h>
#endif

#ifndef VM_PROT_TPRO
#define VM_PROT_TPRO 0x200
#endif

#if !TARGET_OS_EXCLAVEKIT
#if __has_include(<System/mach/dyld_pager.h>)
    #include <System/mach/dyld_pager.h>
    // this #define can be removed when rdar://92861504 is fixed
    #ifndef MWL_MAX_REGION_COUNT
      #define MWL_MAX_REGION_COUNT 5
    #endif
#else
    struct mwl_region {
        int                  mwlr_fd;      /* fd of file file to over map */
        vm_prot_t            mwlr_protections;/* protections for new overmapping */
        off_t                mwlr_file_offset;/* offset in file of start of mapping */
        mach_vm_address_t    mwlr_address; /* start address of existing region */
        mach_vm_size_t       mwlr_size;    /* size of existing region */
    };

    #define MWL_INFO_VERS 7
    struct mwl_info_hdr {
        uint32_t        mwli_version;            /* version of info blob, currently 7 */
        uint16_t        mwli_page_size;          /* 0x1000 or 0x4000 (for sanity checking) */
        uint16_t        mwli_pointer_format;     /* DYLD_CHAINED_PTR_* value */
        uint32_t        mwli_binds_offset;       /* offset within this blob of bind pointers table */
        uint32_t        mwli_binds_count;        /* number of pointers in bind pointers table (for range checks) */
        uint32_t        mwli_chains_offset;      /* offset within this blob of dyld_chained_starts_in_image */
        uint32_t        mwli_chains_size;        /* size of dyld_chained_starts_in_image */
        uint64_t        mwli_slide;              /* slide to add to rebased pointers */
        uint64_t        mwli_image_address;      /* add this to rebase offsets includes any slide */
        /* followed by the binds pointers and dyld_chained_starts_in_image */
    };
    #define MWL_MAX_REGION_COUNT 5
    extern int __map_with_linking_np(const struct mwl_region regions[], uint32_t regionCount, const struct mwl_info_hdr* blob, uint32_t blobSize);
#endif
#endif // !TARGET_OS_EXCLAVEKIT


extern struct mach_header __dso_handle;


using dyld3::MachOAnalyzer;
using dyld3::MachOFile;
using mach_o::FunctionVariants;
using mach_o::FunctionVariantsRuntimeTable;
using mach_o::Image;
using mach_o::Header;
using mach_o::Platform;
using mach_o::ChainedFixups;

namespace dyld4 {

Loader::InitialOptions::InitialOptions()
    : inDyldCache(false)
    , hasObjc(false)
    , mayHavePlusLoad(false)
    , roData(false)
    , neverUnloaded(false)
    , leaveMapped(false)
    , roObjC(false)
    , pre2022Binary(false)
    , hasUUID(false)
    , hasWeakDefs(false)
    , belowLibSystem(false)
{
}

Loader::InitialOptions::InitialOptions(const Loader& other)
    : inDyldCache(other.dylibInDyldCache)
    , hasObjc(other.hasObjC)
    , mayHavePlusLoad(other.mayHavePlusLoad)
    , roData(other.hasReadOnlyData)
    , neverUnloaded(other.neverUnload)
    , leaveMapped(other.leaveMapped)
    , roObjC(other.hasReadOnlyObjC)
    , pre2022Binary(other.pre2022Binary)
    , hasUUID(other.hasUUIDLoadCommand)
    , hasWeakDefs(other.hasWeakDefs)
    , hasTLVs(other.hasTLVs)
    , belowLibSystem(other.belowLibSystem)
    , hasFuncVarFixups(other.hasFuncVarFixups)
{
}

const char* Loader::path(const RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->path(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->path(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->path(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

const char* Loader::installName(const RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->installName(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->installName(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->installName(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

const MachOFile* Loader::mf(const RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->mf(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->mf(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->mf(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

#if SUPPORT_VM_LAYOUT
const MachOLoaded* Loader::loadAddress(const RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->loadAddress(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->loadAddress(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->loadAddress(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}
#endif

#if SUPPORT_VM_LAYOUT
bool Loader::contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->contains(state, addr, segAddr, segSize, segPerms);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->contains(state, addr, segAddr, segSize, segPerms);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->contains(state, addr, segAddr, segSize, segPerms);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}
#endif

bool Loader::matchesPath(const RuntimeState& state, const char* path) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->matchesPath(state, path);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->matchesPath(state, path);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->matchesPath(state, path);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

#if !SUPPORT_CREATING_PREMAPPEDLOADERS
FileID Loader::fileID(const RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->fileID(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->fileID(state);
}
#endif // !SUPPORT_CREATING_PREMAPPEDLOADERS

uint32_t Loader::dependentCount() const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->dependentCount();
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->dependentCount();
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->dependentCount();
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

Loader* Loader::dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* depAttr) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->dependent(state, depIndex, depAttr);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->dependent(state, depIndex, depAttr);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->dependent(state, depIndex, depAttr);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

void Loader::loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options)
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->loadDependents(diag, state, options);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->loadDependents(diag, state, options);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->loadDependents(diag, state, options);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

bool Loader::getExportsTrie(uint64_t& runtimeOffset, uint32_t& size) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->getExportsTrie(runtimeOffset, size);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->getExportsTrie(runtimeOffset, size);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->getExportsTrie(runtimeOffset, size);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

bool Loader::hiddenFromFlat(bool forceGlobal) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->hiddenFromFlat(forceGlobal);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->hiddenFromFlat(forceGlobal);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->hiddenFromFlat(forceGlobal);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

bool Loader::representsCachedDylibIndex(uint16_t dylibIndex) const
{
    assert(this->magic == kMagic);
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->representsCachedDylibIndex(dylibIndex);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->representsCachedDylibIndex(dylibIndex);
}

bool Loader::overridesDylibInCache(const DylibPatch*& patchTable, uint16_t& cacheDylibOverriddenIndex) const
{
    assert(this->magic == kMagic);
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->overridesDylibInCache(patchTable, cacheDylibOverriddenIndex);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->overridesDylibInCache(patchTable, cacheDylibOverriddenIndex);
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void Loader::applyFixups(Diagnostics& diag, RuntimeState& state, DyldCacheDataConstLazyScopedWriter& dataConst, bool allowLazyBinds,
                         lsl::Vector<PseudoDylibSymbolToMaterialize>* materializingSymbols) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    ((PremappedLoader*)this)->applyFixups(diag, state, dataConst, allowLazyBinds, materializingSymbols);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        ((PrebuiltLoader*)this)->applyFixups(diag, state, dataConst, allowLazyBinds, materializingSymbols);
    else
#endif // SUPPORT_PREBUILTLOADERS
        ((JustInTimeLoader*)this)->applyFixups(diag, state, dataConst, allowLazyBinds, materializingSymbols);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

void Loader::withLayout(Diagnostics &diag, const RuntimeState& state, void (^callback)(const mach_o::Layout &layout)) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    ((PremappedLoader*)this)->withLayout(diag, state, callback);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->withLayout(diag, state, callback);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->withLayout(diag, state, callback);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

bool Loader::dyldDoesObjCFixups() const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->dyldDoesObjCFixups();
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->dyldDoesObjCFixups();
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->dyldDoesObjCFixups();
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

const SectionLocations* Loader::getSectionLocations() const
{
    assert(this->magic == kMagic);
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->getSectionLocations();
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->getSectionLocations();
}

#if SUPPORT_IMAGE_UNLOADING
void Loader::unmap(RuntimeState& state, bool force) const
{
    assert(this->magic == kMagic);
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->unmap(state, force);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->unmap(state, force);
}
#endif


bool Loader::hasBeenFixedUp(RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->hasBeenFixedUp(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->hasBeenFixedUp(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->hasBeenFixedUp(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}


bool Loader::beginInitializers(RuntimeState& state)
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return ((PremappedLoader*)this)->beginInitializers(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->beginInitializers(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->beginInitializers(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void Loader::runInitializers(RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    ((PremappedLoader*)this)->runInitializers(state);
#else
#if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        ((PrebuiltLoader*)this)->runInitializers(state);
    else
#endif // SUPPORT_PREBUILTLOADERS
        ((JustInTimeLoader*)this)->runInitializers(state);
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS
}
#endif

bool Loader::isDelayInit(RuntimeState& state) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    return false;
#else
  #if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        return ((PrebuiltLoader*)this)->isDelayInit(state);
    else
  #endif // SUPPORT_PREBUILTLOADERS
        return ((JustInTimeLoader*)this)->isDelayInit(state);
#endif
}

void Loader::setDelayInit(RuntimeState& state, bool value) const
{
    assert(this->magic == kMagic);
#if SUPPORT_CREATING_PREMAPPEDLOADERS
    assert(this->isPremapped);
    // do nothing, delay-init not supported in exclaveKit
#else
  #if SUPPORT_PREBUILTLOADERS
    if ( this->isPrebuilt )
        ((PrebuiltLoader*)this)->setDelayInit(state, value);
    else
  #endif // SUPPORT_PREBUILTLOADERS
        ((JustInTimeLoader*)this)->setDelayInit(state, value);
#endif
}




const PrebuiltLoader* Loader::LoaderRef::loader(const RuntimeState& state) const
{
    if ( this->app )
        return state.processPrebuiltLoaderSet()->atIndex(this->index);
    else
        return state.cachedDylibsPrebuiltLoaderSet()->atIndex(this->index);
}

const char* Loader::leafName(const char* path)
{
    if ( const char* lastSlash = strrchr(path, '/') )
        return lastSlash + 1;
    else
        return path;
}

const char* Loader::leafName(const RuntimeState& state) const
{
    return leafName(path(state));
}

#if SUPPORT_VM_LAYOUT
const MachOAnalyzer* Loader::analyzer(const RuntimeState& state) const
{
    return (MachOAnalyzer*)loadAddress(state);
}

const mach_o::Header* Loader::header(const RuntimeState& state) const
{
    return (mach_o::Header*)loadAddress(state);
}
#endif

bool Loader::hasMagic() const
{
    return (this->magic == kMagic);
}

void Loader::appendHexNibble(uint8_t value, char*& p)
{
    if ( value < 10 )
        *p++ = '0' + value;
    else
        *p++ = 'A' + value - 10;
}

void Loader::appendHexByte(uint8_t value, char*& p)
{
    value &= 0xFF;
    appendHexNibble(value >> 4, p);
    appendHexNibble(value & 0x0F, p);
}

void Loader::uuidToStr(const uuid_t uuid, char  uuidStr[64])
{
    char* p = uuidStr;
    appendHexByte(uuid[0], p);
    appendHexByte(uuid[1], p);
    appendHexByte(uuid[2], p);
    appendHexByte(uuid[3], p);
    *p++ = '-';
    appendHexByte(uuid[4], p);
    appendHexByte(uuid[5], p);
    *p++ = '-';
    appendHexByte(uuid[6], p);
    appendHexByte(uuid[7], p);
    *p++ = '-';
    appendHexByte(uuid[8], p);
    appendHexByte(uuid[9], p);
    *p++ = '-';
    appendHexByte(uuid[10], p);
    appendHexByte(uuid[11], p);
    appendHexByte(uuid[12], p);
    appendHexByte(uuid[13], p);
    appendHexByte(uuid[14], p);
    appendHexByte(uuid[15], p);
    *p = '\0';
}

void Loader::getUuidStr(char uuidStr[64]) const
{
    if ( this->hasUUIDLoadCommand) {
        uuidToStr(uuid, uuidStr);
    }
    else {
        strlcpy(uuidStr, "no uuid", 64);
    }
}

void Loader::logLoad(RuntimeState& state, const char* path) const
{
    char  uuidStr[64];
    this->getUuidStr(uuidStr);
    state.log("<%s> %s\n", uuidStr, path);
}

#if TARGET_OS_EXCLAVEKIT
const Loader* Loader::makePremappedLoader(Diagnostics& diag, RuntimeState& state, const char* path, bool isInDyldCache, uint32_t dylibIndex, const LoadOptions& options, const mach_o::Layout* layout)
{
    return PremappedLoader::makePremappedLoader(diag, state, path, isInDyldCache, dylibIndex, options, layout);
}
#endif // !TARGET_OS_EXCLAVEKIT

#if !TARGET_OS_EXCLAVEKIT
const Loader* Loader::makeDiskLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options,
                                     bool overridesDyldCache, uint32_t dylibIndex,
                                     const mach_o::Layout* layout)
{
    // never create a new loader in RTLD_NOLOAD mode
    if ( options.rtldNoLoad )
        return nullptr;

    // don't use PrebuiltLoaders for simulator because the paths will be wrong (missing SIMROOT prefix)
#if SUPPORT_PREBUILTLOADERS
    // first check for a PrebuiltLoader
    const Loader* result = (Loader*)state.findPrebuiltLoader(path);
    if ( result != nullptr )
        return result;
#endif // SUPPORT_PREBUILTLOADERS

    // The dylibIndex for a catalyst root might be wrong. This can happen if the dylib is found via its macOS path (ie from a zippered dylib)
    // but getLoader() found the root in the /System/iOSSupport path
    // In this case, we want to rewrite the dylib index to be to the catalyst unzippered twin, not the macOS one
    if ( overridesDyldCache && state.config.process.catalystRuntime ) {
        uint32_t dylibInCacheIndex;
        if ( state.config.dyldCache.indexOfPath(path, dylibInCacheIndex) )
            dylibIndex = dylibInCacheIndex;
    }

    // try building a JustInTime Loader
    return JustInTimeLoader::makeJustInTimeLoaderDisk(diag, state, path, options, overridesDyldCache, dylibIndex, layout);
}

const Loader* Loader::makeDyldCacheLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options, uint32_t dylibIndex,
                                          const mach_o::Layout* layout)
{
    // never create a new loader in RTLD_NOLOAD mode
    if ( options.rtldNoLoad )
        return nullptr;

#if SUPPORT_PREBUILTLOADERS
    // first check for a PrebuiltLoader with compatible platform
    // rdar://76406035 (simulator cache paths need prefix)
    const PrebuiltLoader* result = state.findPrebuiltLoader(path);
    if ( result != nullptr ) {
        if ( ((const Header *)result->mf(state))->loadableIntoProcess(state.config.process.platform, path, state.config.security.isInternalOS) ) {
            if ( state.config.log.searching )
                state.log("  found: prebuilt-dylib-from-cache: (0x%04X) \"%s\"\n", dylibIndex, path);
            return result;
        }
    }
#endif // SUPPORT_PREBUILTLOADERS

    // try building a JustInTime Loader
    if ( state.config.log.searching )
        state.log("  found: dylib-from-cache: (0x%04X) \"%s\"\n", dylibIndex, path);
    return JustInTimeLoader::makeJustInTimeLoaderDyldCache(diag, state, path, options, dylibIndex, layout);
}

const Loader* Loader::makePseudoDylibLoader(Diagnostics& diag, RuntimeState &state, const char* path, const LoadOptions& options, const PseudoDylib* pd) {
    return JustInTimeLoader::makePseudoDylibLoader(diag, state, path, options, pd);
}

static bool isFileRelativePath(const char* path)
{
    if ( path[0] == '/' )
        return false;
    if ( (path[0] == '.') && (path[1] == '/') )
        return true;
    if ( (path[0] == '.') && (path[1] == '.') && (path[2] == '/') )
        return true;
    return (path[0] != '@');
}

static bool mightBeInSharedCache(const char* dylibName) {
    return (   (strncmp(dylibName, "/usr/lib/", 9) == 0)
            || (strncmp(dylibName, "/System/Library/", 16) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/usr/lib/", 27) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/System/Library/", 34) == 0)
            || (strncmp(dylibName, "/System/DriverKit/", 18) == 0) );
}


// This composes DyldProcessConfig::forEachPathVariant() with Loader::forEachResolvedAtPathVar()
// They are separate layers because DyldProcessConfig handles DYLD_ env vars and Loader handle @ paths
void Loader::forEachPath(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options,
                         void (^handler)(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool&))
{
    __block bool stop = false;
    const ProcessConfig::PathOverrides& po = state.config.pathOverrides;
    // <rdar://5951327> (DYLD_FALLBACK_LIBRARY_PATH should only apply to dlopen() of leaf names)
    bool skipFallbacks = !options.staticLinkage && (strchr(loadPath, '/') != nullptr) && (state.config.pathOverrides.getFrameworkPartialPath(loadPath) == nullptr);
    po.forEachPathVariant(loadPath, state.config.process.platform, options.requestorNeedsFallbacks, skipFallbacks, stop,
                          ^(const char* possibleVariantPath, ProcessConfig::PathOverrides::Type type, bool&) {
                            #if !TARGET_OS_EXCLAVEKIT
                                // relative name to dlopen() has special behavior
                                if ( !options.staticLinkage && (type == ProcessConfig::PathOverrides::Type::rawPath) && (loadPath[0] != '/') ) {
                                    // if relative path, turn into implicit @rpath
                                    if ( (loadPath[0] != '@') ) {
                                        char implicitRPath[PATH_MAX];
                                        strlcpy(implicitRPath, "@rpath/", sizeof(implicitRPath));
                                        strlcat(implicitRPath, possibleVariantPath, sizeof(implicitRPath));
                                        Loader::forEachResolvedAtPathVar(state, implicitRPath, options, ProcessConfig::PathOverrides::Type::implictRpathExpansion, stop, handler);
                                        if ( stop )
                                            return;
                                        // <rdar://47682983> always look in /usr/lib for leaf names
                                        char implicitPath[PATH_MAX];
                                        strlcpy(implicitPath, "/usr/lib/", sizeof(implicitRPath));
                                        strlcat(implicitPath, loadPath, sizeof(implicitPath));
                                        handler(implicitPath, ProcessConfig::PathOverrides::Type::standardFallback, stop);
                                        if ( stop )
                                            return;
                                        // only try cwd relative if afmi allows
                                        if ( state.config.security.allowAtPaths ) {
                                            handler(loadPath, type, stop);
                                        }
                                        // don't try anything else for dlopen of non-absolute paths
                                        return;
                                    }
                                }
                                // expand @ paths
                                Loader::forEachResolvedAtPathVar(state, possibleVariantPath, options, type, stop, handler);
                            #else
                                handler(possibleVariantPath, type, stop);
                            #endif // !TARGET_OS_EXCLAVEKIT
                          });
}
#endif // !TARGET_OS_EXCLAVEKIT

//
// Use PathOverrides class to walk possible paths, for each, look on disk, then in cache.
// Special case customer caches to look in cache first, to avoid stat() when result will be disgarded.
// For dylibs loaded from disk, we need to know if they override something in the cache in order to patch it in.
// It is considered an override if the initial path or path found is in the dyld cache
//
const Loader* Loader::getLoader(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options)
{
#if TARGET_OS_EXCLAVEKIT
    __block const Loader*  result  = nullptr;
    // check if this path already in use by a Loader
    for ( const Loader* ldr : state.loaded ) {
        if ( !ldr->matchesPath(state, loadPath) )
            continue;
        result = ldr;
        if ( state.config.log.searching )
            state.log("  found: already-loaded-by-path: \"%s\"\n", loadPath);
    }

    if ( result == nullptr ) {
        uint32_t dylibInCacheIndex = 0;
        bool pathIsInCache = state.config.dyldCache.indexOfPath(loadPath, dylibInCacheIndex);
        result = makePremappedLoader(diag, state, loadPath, pathIsInCache, dylibInCacheIndex, options, nullptr);
    }

    if ( (result == nullptr) && options.canBeMissing ) {
        diag.clearError();
    }

    return result;
#else
    __block const Loader*  result        = nullptr;
    const DyldSharedCache* cache         = state.config.dyldCache.addr;
    const bool             customerCache = (cache != nullptr) && !state.config.dyldCache.development;
    if ( state.config.log.searching )
        state.log("find path \"%s\"\n", loadPath);

    const bool loadPathIsRPath            = (::strncmp(loadPath, "@rpath/", 7) == 0);
    const bool loadPathIsFileRelativePath = isFileRelativePath(loadPath);

    // for @rpath paths, first check if already loaded as rpath
    if ( loadPathIsRPath ) {
        for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
            for ( const Loader* ldr : *list ) {
                if ( ldr->matchesPath(state, loadPath) ) {
                    if ( state.config.log.searching )
                        state.log("  found: already-loaded-by-rpath: %s\n", ldr->path(state));
                    return ldr;
                }
            }
        }
    }
    else if ( !options.staticLinkage && (loadPath[0] != '@') && (loadPath[0] != '/') && (strchr(loadPath, '/') == nullptr) ) {
        // handle dlopen("xxx") to mean "@rpath/xxx" when it is already loaded
        char implicitRPath[strlen(loadPath)+8];
        strlcpy(implicitRPath, "@rpath/", sizeof(implicitRPath));
        strlcat(implicitRPath, loadPath, sizeof(implicitRPath));
        for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
            for ( const Loader* ldr : *list ) {
                if ( ldr->matchesPath(state, implicitRPath) ) {
                    if ( state.config.log.searching )
                        state.log("  found: already-loaded-by-rpath: %s\n", ldr->path(state));
                    return ldr;
                }
            }
        }
    }

    // canonicalize shared cache paths
    if ( const char* canonicalPathInCache = state.config.canonicalDylibPathInCache(loadPath) ) {
        if ( strcmp(canonicalPathInCache, loadPath) != 0 ) {
            loadPath = canonicalPathInCache;
            if ( state.config.log.searching )
                state.log("  switch to canonical cache path: %s\n", loadPath);
        }
    }

    // get info about original path
    __block uint32_t dylibInCacheIndex;
    const bool       originalPathIsInDyldCache            = state.config.dyldCache.indexOfPath(loadPath, dylibInCacheIndex);

#if BUILDING_DYLD && TARGET_OS_OSX
    // On macOS, we need to support unzippered twins, which look like roots.  So if the original path is in the cache, it may
    // still be overridable by an unzippered twin which is also in the cache
    const bool       originalPathIsOverridableInDyldCache = originalPathIsInDyldCache;
#else
    const bool       originalPathIsOverridableInDyldCache = originalPathIsInDyldCache && state.config.dyldCache.isOverridablePath(loadPath);
#endif

    // search all locations
    Loader::forEachPath(diag, state, loadPath, options,
                        ^(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& stop) {
                            // On customer dyld caches, if loaded a path in cache, don't look for overrides
                            if ( customerCache && originalPathIsInDyldCache && !originalPathIsOverridableInDyldCache && (possiblePath != loadPath) )
                                return;
                            if ( state.config.log.searching )
                                state.log("  possible path(%s): \"%s\"\n", ProcessConfig::PathOverrides::typeName(type), possiblePath);

                            // check if this path already in use by a Loader
                            for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                                if ( options.rtldNoLoad && (list == &state.delayLoaded) )
                                    continue;
                                for ( const Loader* ldr : *list ) {
                                    if ( ldr->matchesPath(state, possiblePath) ) {
                                        result = ldr;
                                        stop   = true;
                                        diag.clearError(); // found dylib, so clear any errors from previous paths tried
                                        if ( state.config.log.searching )
                                            state.log("  found: already-loaded-by-path: \"%s\"\n", possiblePath);
                                        return;
                                    }
                                }
                            }

                            // <rdar://problem/47682983> don't allow file system relative paths in hardened programs
                            // (type == ProcessConfig::PathOverrides::Type::implictRpathExpansion)
                            if ( !state.config.security.allowEnvVarsPath && isFileRelativePath(possiblePath) ) {
                                if ( diag.noError() )
                                    diag.error("tried: '%s' (relative path not allowed in hardened program)", possiblePath);
                                else
                                    diag.appendError(", '%s' (relative path not allowed in hardened program)", possiblePath);
                                return;
                            }

                            // check dyld cache trie to see if this is an alias to a cached dylib
                            uint32_t possibleCacheIndex;
                            if ( state.config.dyldCache.indexOfPath(possiblePath, possibleCacheIndex) ) {
                                for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                                    if ( options.rtldNoLoad && (list == &state.delayLoaded) )
                                        continue;
                                    for ( const Loader* ldr : *list ) {
                                        if ( ldr->representsCachedDylibIndex(possibleCacheIndex) ) {
                                            result = ldr;
                                            stop   = true;
                                            diag.clearError(); // found dylib, so clear any errors from previous paths tried
                                            if ( state.config.log.searching )
                                                state.log("  found: already-loaded-by-dylib-index: \"%s\" -> %s\n", possiblePath, ldr->path(state));
                                            return;
                                        }
                                    }
                                }
                            }

                            // RTLD_NOLOAD used and this possible path not already in use, so skip to next
                            if ( options.rtldNoLoad ) {
                                return;
                            }

                            // Check for PseduoDylibs
                            if (!state.pseudoDylibs.empty()) {
                                // FIXME: Should all of this be in its own function?
                                if ( state.config.log.searching )
                                    state.log("searching %llu pseudo-dylibs:\n", state.pseudoDylibs.size());
                                for (auto &pd : state.pseudoDylibs) {
                                    if (auto *canonicalPath = pd->loadableAtPath(possiblePath)) {
                                        if ( state.config.log.searching )
                                            state.log("  found: pseduo-dylib: \"%s\"\n", possiblePath);
                                        Diagnostics possiblePathDiag;
                                        result = makePseudoDylibLoader(possiblePathDiag, state, canonicalPath, options, &*pd);
                                        // Dispose of canonicalPath if it is different from possiblePath
                                        // (loadableAtPath is allowed to return its argument, which should not be freed).
                                        if (canonicalPath != possiblePath)
                                          pd->disposeString(canonicalPath);
                                        if ( possiblePathDiag.hasError() ) {
                                          // Report error if pseudo-dylib failed to load.
                                          if ( diag.noError() )
                                            diag.error("tried: '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());
                                          else
                                            diag.appendError(", '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());

                                          if ( state.config.log.searching )
                                            state.log("  found: pseudo-dylib-error: \"%s\" => \"%s\"\n", possiblePath, possiblePathDiag.errorMessageCStr());
                                        }
                                        if (result) {
                                            diag.clearError();
                                            stop = true;
                                            return;
                                        }
                                    }
                                }
                                if ( state.config.log.searching && !result)
                                    state.log("   no pseudo-dylibs matched\n");
                            }

                            // see if this path is on disk or in dyld cache
                            int    possiblePathOnDiskErrNo    = 0;
                            bool   possiblePathHasFileOnDisk  = false;
                            bool   possiblePathIsInDyldCache  = false;
                            bool   possiblePathOverridesCache = false;
                            FileID possiblePathFileID = FileID::none();
                            if ( customerCache ) {
                                // for customer cache, check cache first and only stat() if overridable
                                if ( !ProcessConfig::PathOverrides::isOnDiskOnlyType(type) )
                                    possiblePathIsInDyldCache = state.config.dyldCache.indexOfPath(possiblePath, dylibInCacheIndex);
                                if ( possiblePathIsInDyldCache ) {
                                    if ( state.config.dyldCache.isOverridablePath(possiblePath) ) {
                                        // see if there is a root installed that overrides one of few overridable dylibs in the cache
                                        possiblePathHasFileOnDisk  = state.config.fileExists(possiblePath, &possiblePathFileID, &possiblePathOnDiskErrNo);
                                        possiblePathOverridesCache = possiblePathHasFileOnDisk;
                                    }
                                }
                                else {
                                    possiblePathHasFileOnDisk  = state.config.fileExists(possiblePath, &possiblePathFileID, &possiblePathOnDiskErrNo);
                                    possiblePathOverridesCache = possiblePathHasFileOnDisk && originalPathIsOverridableInDyldCache;
                                }
                            }
                            else {
                                // for dev caches, always stat() and check cache
                                possiblePathHasFileOnDisk = state.config.fileExists(possiblePath, &possiblePathFileID, &possiblePathOnDiskErrNo);
                                if ( possiblePathHasFileOnDisk || !ProcessConfig::PathOverrides::isOnDiskOnlyType(type) )
                                    possiblePathIsInDyldCache = state.config.dyldCache.indexOfPath(possiblePath, dylibInCacheIndex);
                                possiblePathOverridesCache = possiblePathHasFileOnDisk && (originalPathIsInDyldCache || possiblePathIsInDyldCache);
                                // rdar://121937452 (hardened apps cannot have libSystem overridden)
                                if ( possiblePathOverridesCache && !state.config.security.allowLibSystemOverrides && ProcessConfig::DyldCache::isProtectedLibSystemPath(loadPath) ) {
                                    possiblePathOverridesCache = false;
                                    possiblePathHasFileOnDisk = false;
                                    possiblePathOnDiskErrNo = ENOSYS;  // dummy value to get DYLD_PRINT_SEARCHING to print something informative
                                }
                            }

                            // see if this possible path was already loaded via a symlink or hardlink by checking inode
                            if ( possiblePathHasFileOnDisk && possiblePathFileID.valid() ) {
                                for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                                    if ( options.rtldNoLoad && (list == &state.delayLoaded) )
                                        continue;
                                    for ( const Loader* ldr : *list ) {
                                        FileID ldrFileID = ldr->fileID(state);
                                        if ( ldrFileID.valid() && (possiblePathFileID == ldrFileID) ) {
                                            result = ldr;
                                            stop   = true;
                                            diag.clearError(); // found dylib, so clear any errors from previous paths tried
                                            if ( state.config.log.searching )
                                                state.log("  found: already-loaded-by-inode-mtime: \"%s\"\n", ldr->path(state));
                                            return;
                                        }
                                    }
                                }
                            }

#if TARGET_OS_SIMULATOR
                            // rdar://76406035 (load simulator dylibs from cache)
                            if ( (state.config.dyldCache.addr != nullptr) && state.config.dyldCache.addr->header.dylibsExpectedOnDisk ) {
                                if ( const char* simRoot = state.config.pathOverrides.simRootPath() ) {
                                    size_t simRootLen = strlen(simRoot);
                                    // compare inode/mtime of dylib now vs when cache was built
                                    const char* possiblePathInSimDyldCache = nullptr;
                                    if ( strncmp(possiblePath, simRoot, simRootLen) == 0 ) {
                                        // looks like a dylib in the sim Runtime root, see if partial path is in the dyld cache
                                        possiblePathInSimDyldCache = &possiblePath[simRootLen];
                                    }
                                    else if ( strncmp(possiblePath, "/usr/lib/system/", 16) == 0 ) {
                                        // could be one of the magic host dylibs that got incorporated into the dyld cache
                                        possiblePathInSimDyldCache = possiblePath;
                                    }
                                    if ( possiblePathInSimDyldCache != nullptr ) {
                                        if ( state.config.dyldCache.indexOfPath(possiblePathInSimDyldCache, dylibInCacheIndex) ) {
                                            uint64_t expectedMTime;
                                            uint64_t expectedInode;
                                            state.config.dyldCache.addr->getIndexedImageEntry(dylibInCacheIndex, expectedMTime, expectedInode);
                                            FileID expectedID(expectedInode, state.config.process.dyldSimFSID, expectedMTime, true);
                                            if ( possiblePathFileID == expectedID ) {
                                                // inode/mtime matches when sim dyld cache was built, so use dylib from dyld cache and ignore file on disk
                                                possiblePathHasFileOnDisk = false;
                                                possiblePathIsInDyldCache = true;
                                            }
                                        }
                                    }
                                }
                            }
#endif
                            // if possiblePath not a file and not in dyld cache, skip to next possible path
                            if ( !possiblePathHasFileOnDisk && !possiblePathIsInDyldCache ) {
                                if ( options.pathNotFoundHandler && !ProcessConfig::PathOverrides::isOnDiskOnlyType(type) )
                                    options.pathNotFoundHandler(possiblePath);
                                // append each path tried to diag
                                if ( diag.noError() )
                                    diag.error("tried: ");
                                else
                                    diag.appendError(", ");
                                const char* sharedCacheMsg = "";
                                if ( !ProcessConfig::PathOverrides::isOnDiskOnlyType(type) && mightBeInSharedCache(possiblePath) )
                                    sharedCacheMsg = (state.config.dyldCache.addr != nullptr) ? ", not in dyld cache" : ", no dyld cache";
                                if ( possiblePathOnDiskErrNo == ENOENT ) {
                                    diag.appendError("'%s' (no such file%s)", possiblePath, sharedCacheMsg);
                                }
                                else if ( (possiblePathOnDiskErrNo == EPERM) && state.config.syscall.sandboxBlockedStat(possiblePath) ) {
                                    diag.appendError("'%s' (blocked by sandbox)%s", possiblePath, sharedCacheMsg);
                                    if ( state.config.log.searching )
                                      state.log("    stat(\"%s\") blocked by file system sandbox\n", possiblePath);
                                }
                                else if ( possiblePathOnDiskErrNo == ENOTAFILE_NP ) {
                                    diag.appendError("'%s' (not a file%s)", possiblePath, sharedCacheMsg);
                                    if ( state.config.log.searching )
                                      state.log("    stat(\"%s\") returned path is not a regular file\n", possiblePath);
                                }
                                else if ( possiblePathOnDiskErrNo == ENOSYS ) {
                                    diag.appendError("'%s' (cannot override a protected system dylib)%s", possiblePath, sharedCacheMsg);
                                    if ( state.config.log.searching )
                                      state.log("    \"%s\" cannot override a protected system dylib\n", possiblePath);
                                }
                                else {
                                    diag.appendError("'%s' (errno=%d%s)", possiblePath, possiblePathOnDiskErrNo, sharedCacheMsg);
                                    if ( state.config.log.searching )
                                      state.log("    stat(\"%s\") returned: errno=%d\n", possiblePath, possiblePathOnDiskErrNo);
                                }
                                return;
                            }

                            // try to build Loader from possiblePath
                            Diagnostics possiblePathDiag;
                            if ( possiblePathHasFileOnDisk ) {
                                if ( possiblePathOverridesCache ) {
                                    // use dylib on disk to override dyld cache
                                    if ( state.config.log.searching )
                                        state.log("  found: dylib-from-disk-to-override-cache: \"%s\"\n", possiblePath);
                                    result = makeDiskLoader(possiblePathDiag, state, possiblePath, options, true, dylibInCacheIndex, nullptr);
                                    if ( state.config.log.searching && possiblePathDiag.hasError() )
                                        state.log("  found: dylib-from-disk-to-override-cache-error: \"%s\" => \"%s\"\n", possiblePath, possiblePathDiag.errorMessageCStr());
                                }
                                else {
                                    // load from disk, nothing to do with dyld cache
                                    if ( state.config.log.searching )
                                        state.log("  found: dylib-from-disk: \"%s\"\n", possiblePath);
                                    result = makeDiskLoader(possiblePathDiag, state, possiblePath, options, false, 0, nullptr);
                                    if ( state.config.log.searching && possiblePathDiag.hasError() )
                                        state.log("  found: dylib-from-disk-error: \"%s\" => \"%s\"\n", possiblePath, possiblePathDiag.errorMessageCStr());
                                }
                            }
                            else if ( possiblePathIsInDyldCache ) {
                                // can use dylib in dyld cache
                                result = makeDyldCacheLoader(possiblePathDiag, state, possiblePath, options, dylibInCacheIndex, nullptr);
                                if ( state.config.log.searching && possiblePathDiag.hasError() )
                                    state.log("  found: dylib-from-cache-error: \"%s\" => \"%s\"\n", possiblePath, possiblePathDiag.errorMessageCStr());
                            }
                            if ( result != nullptr ) {
                                stop = true;
                                diag.clearError(); // found dylib, so clear any errors from previous paths tried
                            }
                            else {
                                // set diag to be contain all errors from all paths tried
                                if ( diag.noError() )
                                    diag.error("tried: '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());
                                else
                                    diag.appendError(", '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());
                            }
                        });

    // The last possibility is that the path provided has ../ or // in it,
    // or is a symlink to a dylib which is in the cache and no longer on disk.
    // Use realpath() and try getLoader() again.
    // Do this last and only if it would fail anyways so as to not slow down correct paths
    if ( result == nullptr ) {
        if ( !state.config.security.allowEnvVarsPath && loadPathIsFileRelativePath ) {
            // don't realpath() relative paths in hardened programs
            // but do check if path matches install name of something already loaded
            for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                for ( const Loader* ldr : *list ) {
                    if ( ldr->matchesPath(state, loadPath) ) {
                        if ( state.config.log.searching )
                            state.log("  found existing image by install name: \"%s\"\n", ldr->path(state));
                        result = ldr;
                        diag.clearError();
                        break;
                    }
                }
            }
        }
        else if ( !options.staticLinkage && (strchr(loadPath, '/') == nullptr) ) {
            // don't realpath() leaf names to dlopen(), they have already been handled
        }
        else {
            char canonicalPath[PATH_MAX];
            if ( (loadPath[0] != '@') && state.config.syscall.realpath(loadPath, canonicalPath) ) {
                // only call getLoader() again if the realpath is different to prevent recursion
                // don't call getLoader() again if the realpath is a just the loadPath cut back, because that means some dir was not found
                if ( ::strncmp(loadPath, canonicalPath, strlen(canonicalPath)) != 0 ) {
                    if ( state.config.log.searching )
                        state.log("  switch to realpath: \"%s\"\n", canonicalPath);
                    result = getLoader(diag, state, canonicalPath, options);
                }
            }
        }
    }

    if ( state.config.log.searching && (result == nullptr) )
        state.log("  not found: \"%s\"\n", loadPath);

    // if the load failed due to security policy, leave a hint in dlerror() or crash log messages 
    if ( (result == nullptr) && (loadPath[0] == '@') && !state.config.security.allowAtPaths ) {
        diag.appendError(", (security policy does not allow @ path expansion)");
    }

    // if dylib could not be found, but is not required, clear error message
    if ( result == nullptr ) {
        if ( (options.canBeMissing || options.rtldNoLoad) )
            diag.clearError();
        else if ( diag.noError() ) {
            bool isRPath = (strncmp(loadPath, "@rpath/", 7) == 0);
            if ( isRPath ) {
                __block bool hasRPath = false;
                for ( const LoadChain* link = options.rpathStack; (link != nullptr) && !hasRPath; link = link->previous ) {
                    const Header* hdr = (const Header*)link->image->mf(state);
                    hdr->forEachRPath(^(const char* rPath, bool& innerStop) {
                        hasRPath = true;
                        innerStop = true;
                    });
                }
                if ( !hasRPath ) {
                    diag.error("no LC_RPATH's found");
                } else  {
                    // To get here its very likely we failed to use any LC_RPATH's, even though we have them.
                    // For example if the LC_RPATH doesn't start with @loader_path or @executable_path then we might
                    // silently skip it and never get in to the main getLoader block where we'd normally issue a diagnostic
                    // if a path can't be found
                    diag.error("no LC_RPATH's used.  They may be invalid, eg, not start with '@loader_path' or '@executable_path'");
                }
            } else {
                // FIXME: Is there an error we can give if we can even get here?
            }
        }
    }
    return result;
#endif // !TARGET_OS_EXCLAVEKIT
}

#if !TARGET_OS_EXCLAVEKIT
bool Loader::expandAtLoaderPath(RuntimeState& state, const char* loadPath, const LoadOptions& options, const Loader* ldr, bool fromLCRPATH, char fixedPath[])
{
    // only do something if path starts with @loader_path
    if ( strncmp(loadPath, "@loader_path", 12) != 0 )
        return false;
    if ( (loadPath[12] != '/') && (loadPath[12] != '\0') )
        return false;

    // don't support @loader_path in DYLD_INSERT_LIBRARIES
    if ( options.insertedDylib ) {
        if ( state.config.log.searching )
            state.log("    @loader_path not allowed in DYLD_INSERT_LIBRARIES\n");
        return false;
    }

    // don't expand if security does not allow
    if ( !state.config.security.allowAtPaths && fromLCRPATH && (ldr == state.mainExecutableLoader) ) {
        // <rdar://42360708> but allow @loader_path in LC_LOAD_DYLIB during dlopen()
        if ( state.config.log.searching )
            state.log("    @loader_path in LC_RPATH from main executable not expanded due to security policy\n");
        return false;
    }

    size_t size = PATH_MAX;
    strlcpy(fixedPath, ldr->path(state), size);
    char* lastSlash = strrchr(fixedPath, '/');
    if ( lastSlash != nullptr ) {
        size -= (lastSlash - fixedPath);
        strlcpy(lastSlash, &loadPath[12], size);
        return true;
    }
    return false;
}

bool Loader::expandAndNormalizeAtExecutablePath(const char* mainPath, const char* pathWithAtExecutable, char fixedPath[PATH_MAX])
{
    // only do something if path starts with "@executable_path/" or is ""@executable_path"
    if ( strncmp(pathWithAtExecutable, "@executable_path", 16) != 0 )
        return false;
    if ( (pathWithAtExecutable[16] != '/') && (pathWithAtExecutable[16] != '\0') )
        return false;

    size_t size = PATH_MAX;
    strlcpy(fixedPath, mainPath, size);
    char* mainPathDirStart = strrchr(fixedPath, '/');
    if ( mainPathDirStart == nullptr )
        return false; // no slash in mainPath ??

    const char* trailingLoadPath = &pathWithAtExecutable[16];
    if ( *trailingLoadPath == '/' ) {
        // main executable path is already a real path, so we can remove ../ by chopping back path
        // Ex:  @executable_path/../Foo (when mainPath=/Applications/XZY.app/XZY)
        //    optimize /Applications/XZY.app/../Foo to /Applications/Foo
        while ( strncmp(trailingLoadPath, "/..", 3) == 0 ) {
            char* newLastSlash = mainPathDirStart-1;
            while ( (newLastSlash > fixedPath) && (*newLastSlash != '/') )
                --newLastSlash;
            if ( newLastSlash != fixedPath ) {
                trailingLoadPath += 3;
                mainPathDirStart = newLastSlash;
            } else {
                break;
            }
        }
    }
    else {
        ++mainPathDirStart;
    }
    size -= (mainPathDirStart - fixedPath);
    strlcpy(mainPathDirStart, trailingLoadPath, size);
    return true;
}

bool Loader::expandAtExecutablePath(RuntimeState& state, const char* loadPath, const LoadOptions& options, bool fromLCRPATH, char fixedPath[])
{
    // only do something if path starts with @executable_path
    if ( strncmp(loadPath, "@executable_path", 16) != 0 )
        return false;
    if ( (loadPath[16] != '/') && (loadPath[16] != '\0') )
        return false;

    // don't expand if security does not allow
    if ( !state.config.security.allowAtPaths ) {
        if ( state.config.log.searching )
            state.log("    @executable_path not expanded due to security policy\n");
        return false;
    }

    return expandAndNormalizeAtExecutablePath(state.config.process.mainExecutablePath, loadPath, fixedPath);
}

void Loader::forEachResolvedAtPathVar(RuntimeState& state, const char* loadPath, const LoadOptions& options, ProcessConfig::PathOverrides::Type type, bool& stop,
                                      void (^handler)(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& stop))
{
    // don't expand @rpath in DYLD_INSERT_LIBRARIES
    bool isRPath = (strncmp(loadPath, "@rpath/", 7) == 0);
    if ( isRPath && options.insertedDylib ) {
        handler(loadPath, type, stop);
        return;
    }

    // expand @loader_path
    BLOCK_ACCCESSIBLE_ARRAY(char, tempPath, PATH_MAX);
    if ( expandAtLoaderPath(state, loadPath, options, options.rpathStack->image, false, tempPath) ) {
        handler(tempPath, ProcessConfig::PathOverrides::Type::loaderPathExpansion, stop);
#if BUILDING_DYLD && TARGET_OS_OSX
        if ( !stop ) {
            // using @loader_path, but what it expanded to did not work ('stop' not set)
            // maybe this is an old binary with an install name missing the /Versions/A/ part
            const Loader*         orgLoader   = options.rpathStack->image;
            const MachOAnalyzer*  orgMA       = orgLoader->analyzer(state);
            if ( orgMA->isDylib() && !orgMA->enforceFormat(MachOAnalyzer::Malformed::loaderPathsAreReal) ) {
                const char*           fullPath    = orgLoader->path(state);
                const char*           installPath = ((const Header*)orgMA)->installName();
                if ( const char* installLeaf = strrchr(installPath, '/') ) {
                    size_t leafLen = strlen(installLeaf);
                    size_t fullLen = strlen(fullPath);
                    if ( fullLen > (leafLen+11) ) {
                        const char* fullWhereVersionMayBe = &fullPath[fullLen-leafLen-11];
                        if ( strncmp(fullWhereVersionMayBe, "/Versions/", 10) == 0 ) {
                            // try expanding @loader_path to this framework's path that is missing /Versions/A part
                            strlcpy(tempPath, fullPath, PATH_MAX);
                            tempPath[fullLen-leafLen-11] = '\0';
                            strlcat(tempPath, &loadPath[12], PATH_MAX);
                            handler(tempPath, ProcessConfig::PathOverrides::Type::loaderPathExpansion, stop);
                        }
                    }
                }
            }
        }
#endif
        return;
    }

    // expand @executable_path
    if ( expandAtExecutablePath(state, loadPath, options, false, tempPath) ) {
        handler(tempPath, ProcessConfig::PathOverrides::Type::executablePathExpansion, stop);
        return;
    }

    // expand @rpath
    if ( isRPath ) {
        // note: rpathTail starts with '/'
        const char* rpathTail = &loadPath[6];
        // keep track if this is an explict @rpath or implicit
        ProcessConfig::PathOverrides::Type expandType = ProcessConfig::PathOverrides::Type::rpathExpansion;
        if ( type == ProcessConfig::PathOverrides::Type::implictRpathExpansion )
            expandType = type;
        // rpath is expansion is a stack of rpath dirs built starting with main executable and pushing
        // LC_RPATHS from each dylib as they are recursively loaded.  options.rpathStack is a linnked list of that stack.
        for ( const LoadChain* link = options.rpathStack; (link != nullptr) && !stop; link = link->previous ) {
            const Header* hdr = (const Header*)link->image->mf(state);
            hdr->forEachRPath(^(const char* rPath, bool& innerStop) {
                if ( state.config.log.searching )
                    state.log("  LC_RPATH '%s' from '%s'\n", rPath, link->image->path(state));
                if ( expandAtLoaderPath(state, rPath, options, link->image, true, tempPath) || expandAtExecutablePath(state, rPath, options, true, tempPath) ) {
                    Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                    handler(tempPath, expandType, innerStop);
                }
                else if ( rPath[0] == '/' ) {
#if BUILDING_DYLD && TARGET_OS_OSX && __arm64__ // FIXME: this should be a runtime check to enable unit testing
                    // if LC_RPATH is to absolute path like /usr/lib/swift, but this iOS app running on macOS, we really need /System/iOSSupport/usr/lib/swift
                    if ( state.config.process.platform == Platform::iOS ) {
                        strlcpy(tempPath, "/System/iOSSupport", PATH_MAX);
                        strlcat(tempPath, rPath, PATH_MAX);
                        Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                        handler(tempPath, expandType, innerStop);
                        if ( innerStop ) {
                            stop = true;
                            return;
                        }
                    }
                    // fall through
#endif
#if TARGET_OS_SIMULATOR
                    // <rdar://problem/5869973> DYLD_ROOT_PATH should apply to LC_RPATH rpaths
                    if ( const char* simRoot = state.config.pathOverrides.simRootPath() ) {
                        strlcpy(tempPath, simRoot, PATH_MAX);
                        strlcat(tempPath, rPath, PATH_MAX);
                        Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                        handler(tempPath, expandType, innerStop);
                        if ( innerStop ) {
                            stop = true;
                            return;
                        }
                    }
					// <rdar://problem/49576123> Even if DYLD_ROOT_PATH exists, LC_RPATH should add raw path to rpaths
                    // so fall through
#endif

                    // LC_RPATH is an absolute path, not blocked by AtPath::none
                    strlcpy(tempPath, rPath, PATH_MAX);
                    Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                    handler(tempPath, expandType, innerStop);
                    if ( innerStop ) {
                        stop = true;
                        return;
                    }

                    // Note this is after the above call due to:
                    // rdar://91027811 (dyld should search for dylib overrides in / before /System/Cryptexes/OS)
                    // <rdar://problem/5869973> DYLD_ROOT_PATH should apply to LC_RPATH rpaths
                    if ( const char* cryptexRoot = state.config.pathOverrides.cryptexRootPath() ) {
                        strlcpy(tempPath, cryptexRoot, PATH_MAX);
                        strlcat(tempPath, rPath, PATH_MAX);
                        Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                        handler(tempPath, expandType, innerStop);
                        if ( innerStop ) {
                            stop = true;
                            return;
                        }
                    }
                } else {
#if BUILDING_DYLD && TARGET_OS_OSX // FIXME: this should be a runtime check to enable unit testing
                    // <rdar://81909581>
                    // Relative paths.  Only allow these if security supports them
                    if ( state.config.security.allowAtPaths ) {
                        strlcpy(tempPath, rPath, PATH_MAX);
                        Utils::concatenatePaths(tempPath, rpathTail, PATH_MAX);
                        handler(tempPath, expandType, innerStop);
                    }
#endif
                }
                if ( innerStop )
                    stop = true;
            });
        }
        if ( stop )
            return;
    }

    // only call with origin path if it did not start with @
    if ( loadPath[0] != '@' ) {
        handler(loadPath, type, stop);
    }
}
#endif // !TARGET_OS_EXCLAVEKIT


#if (BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_UNIT_TESTS) && !TARGET_OS_EXCLAVEKIT
uint64_t Loader::validateFile(Diagnostics& diag, const RuntimeState& state, int fd, const char* path,
                              const CodeSignatureInFile& codeSignature, const Loader::FileValidationInfo& fileValidation)
{
    // get file info
    struct stat statBuf;
    if ( state.config.syscall.fstat(fd, &statBuf) != 0 ) {
        int statErr = errno;
        if ( (statErr == EPERM) && state.config.syscall.sandboxBlockedStat(path) )
            diag.error("file system sandbox blocked stat(\"%s\")", path);
        else if ( statErr == ENOENT )
            diag.error("no such file");
        else
            diag.error("stat(\"%s\") failed with errno=%d", path, statErr);
        return -1;
    }

#if !__LP64__
    statBuf.st_ino = (statBuf.st_ino & 0xFFFFFFFF);
#endif

    // if inode/mtime was recorded, check that
    if ( fileValidation.checkInodeMtime ) {
        if ( statBuf.st_ino != fileValidation.inode ) {
            diag.error("file inode changed from 0x%llX to 0x%llX since Loader was built for '%s'", fileValidation.inode, statBuf.st_ino, path);
            return -1;
        }
        if ( (uint64_t)statBuf.st_mtime != fileValidation.mtime ) {
            diag.error("file mtime changed from 0x%llX to 0x%lX since Loader was built for '%s'", fileValidation.mtime, statBuf.st_mtime, path);
            return -1;
        }
        // sanity check slice offset
        if ( (uint64_t)statBuf.st_size < fileValidation.sliceOffset ) {
            diag.error("file too small for slice offset '%s'", path);
            return -1;
        }
        return fileValidation.sliceOffset;
    }
    else if ( codeSignature.size != 0 ) {
#if !TARGET_OS_SIMULATOR // some hashing functions not available in .a files
        // otherwise compare cdHash
        void* mappedFile = state.config.syscall.mmap(nullptr, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if ( mappedFile == MAP_FAILED ) {
            diag.error("could not mmap() '%s'", path);
            return -1;
        }
        uint64_t sliceOffset = 0;
        uint64_t sliceSize   = 0;
        bool     isOSBinary  = false; // FIXME
        if ( const MachOFile* mf = MachOFile::compatibleSlice(diag, sliceOffset, sliceSize, mappedFile, (size_t)statBuf.st_size, path, state.config.process.platform, isOSBinary, *state.config.process.archs, state.config.security.internalInstall) ) {
            const MachOLoaded* ml            = (MachOLoaded*)mf;
            __block bool       cdHashMatches = false;
            // Note, file is not mapped with zero fill so cannot use forEachCdHash()
            // need to use lower level forEachCDHashOfCodeSignature() which takes pointer to code blob
            ml->forEachCDHashOfCodeSignature((uint8_t*)mf + codeSignature.fileOffset, codeSignature.size, ^(const uint8_t cdHash[20]) {
                if ( ::memcmp((void*)cdHash, (void*)fileValidation.cdHash, 20) == 0 )
                    cdHashMatches = true;
            });
            if ( cdHashMatches ) {
                sliceOffset = (uint8_t*)mf - (uint8_t*)mappedFile;
            } else {
                sliceOffset = -1;
                diag.error("file cdHash not as expected '%s'", path);
            }
        } else {
            sliceOffset = -1;
        }
        state.config.syscall.munmap(mappedFile, (size_t)fileValidation.sliceOffset);
        return sliceOffset;
#endif
    }
    return -1;
}

#if BUILDING_DYLD
static bool getUuidFromFd(RuntimeState& state, int fd, uint64_t sliceOffset, char uuidStr[64])
{
    strlcpy(uuidStr, "no uuid", 64);
    mach_header mh;
    if ( state.config.syscall.pread(fd, &mh, sizeof(mh), (size_t)sliceOffset) == sizeof(mh) ) {
        if ( ((Header*)&mh)->hasMachOMagic() ) {
            size_t headerAndLoadCommandsSize = mh.sizeofcmds+sizeof(mach_header_64);
            uint8_t buffer[headerAndLoadCommandsSize];
            if ( state.config.syscall.pread(fd, buffer, sizeof(buffer), (size_t)sliceOffset) == headerAndLoadCommandsSize ) {
                uuid_t uuid;
                if ( ((Header*)buffer)->getUuid(uuid) ) {
                    Loader::uuidToStr(uuid, uuidStr);
                    return true;
                }
            }
        }
    }
    return false;
}
#endif

const MachOAnalyzer* Loader::mapSegments(Diagnostics& diag, RuntimeState& state, const char* path, int fd, uint64_t vmSpace,
                                         const CodeSignatureInFile& codeSignature, bool hasCodeSignature,
                                         const Array<Region>& regions, bool neverUnloads, bool prebuilt, const FileValidationInfo& fileValidation)
{
#if BUILDING_DYLD
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_MAP_IMAGE, path, 0, 0);
#endif

    // Validate this file has not changed since FileValidationInfo was retrieved
    uint64_t sliceOffset = validateFile(diag, state, fd, path, codeSignature, fileValidation);
    if ( diag.hasError() ) {
        return nullptr;
    }

#if BUILDING_DYLD
    // register code signature
    if ( hasCodeSignature && codeSignature.size != 0 ) {
        char uuidStr[64];
        getUuidFromFd(state, fd, sliceOffset, uuidStr);
        if ( !state.config.syscall.registerSignature(diag, path, uuidStr, fd, sliceOffset, codeSignature.fileOffset, codeSignature.size) )
            return nullptr;
    }

    // <rdar://problem/41015217> dyld should use F_CHECK_LV even on unsigned binaries
    // <rdar://problem/32684903> always call F_CHECK_LV to preflight
    {
        fchecklv checkInfo;
        char     messageBuffer[512];
        messageBuffer[0]                = '\0';
        checkInfo.lv_file_start         = sliceOffset;
        checkInfo.lv_error_message_size = sizeof(messageBuffer);
        checkInfo.lv_error_message      = messageBuffer;
        int res                         = state.config.syscall.fcntl(fd, F_CHECK_LV, &checkInfo);
        if ( res == -1 ) {
            // rdar://79796526 (include uuid of mis-signed binary to help debug)
            char uuidStr[64];
            getUuidFromFd(state, fd, sliceOffset, uuidStr);
            diag.error("code signature in <%s> '%s' not valid for use in process: %s", uuidStr, path, messageBuffer);
            return nullptr;
        }
    }
#endif

#if BUILDING_DYLD && SUPPORT_ROSETTA
    // if translated, need to add in translated code segment
    char     aotPath[PATH_MAX];
    uint64_t extraAllocSize = 0;
    if ( state.config.process.isTranslated ) {
        int ret = aot_get_extra_mapping_info(fd, path, extraAllocSize, aotPath, sizeof(aotPath));
        if ( ret == 0 ) {
            vmSpace += extraAllocSize;
        }
        else {
            extraAllocSize = 0;
            aotPath[0]     = '\0';
        }
    }
#endif

    // reserve address range
    vm_address_t  loadAddress = 0;
    kern_return_t r           = ::vm_allocate(mach_task_self(), &loadAddress, (vm_size_t)vmSpace, VM_FLAGS_ANYWHERE);
    if ( r != KERN_SUCCESS ) {
        diag.error("vm_allocate(size=0x%0llX) failed with result=%d", vmSpace, r);
        return nullptr;
    }

#if BUILDING_DYLD
    if ( state.config.log.segments ) {
        if ( sliceOffset != 0 )
            state.log("Mapping %s (slice offset=0x%llX)\n", path, sliceOffset);
        else
            state.log("Mapping %s\n", path);
    }
#endif

    // map each segment
    bool             mmapFailure               = false;
    const  bool      enableTpro                = state.config.process.enableTproDataConst;
    __block uint32_t segIndex                  = 0;
    for ( const Region& region : regions ) {
        // <rdar://problem/32363581> Mapping zero filled regions fails with mmap of size 0
        if ( region.isZeroFill || (region.fileSize == 0) )
            continue;
        if ( (region.vmOffset == 0) && (segIndex > 0) )
            continue;
        int perms = VM_PROT_READ;
        int flags = MAP_FIXED | MAP_PRIVATE;

#if BUILDING_DYLD
        perms = region.perms;
#endif
        if (enableTpro && region.readOnlyData) {
            flags |= MAP_TPRO;
        }
        void* segAddress = state.config.syscall.mmap((void*)(loadAddress + region.vmOffset), (size_t)region.fileSize, perms,
                                                     flags, fd, (size_t)(sliceOffset + region.fileOffset));
        int   mmapErr    = errno;
        if ( segAddress == MAP_FAILED ) {
            if ( mmapErr == EPERM ) {
                if ( state.config.syscall.sandboxBlockedMmap(path) )
                    diag.error("file system sandbox blocked mmap() of '%s'", path);
                else
                    diag.error("code signing blocked mmap() of '%s'", path);
            }
            else {
                diag.error("mmap(addr=0x%0llX, size=0x%08X) failed with errno=%d for %s", loadAddress + region.vmOffset,
                           region.fileSize, mmapErr, path);
            }
            mmapFailure = true;
            break;
        }

        // sanity check first segment is mach-o header
        if ( !mmapFailure && (segIndex == 0) ) {
            const MachOAnalyzer* ma = (MachOAnalyzer*)segAddress;
            if ( !ma->isMachO(diag, region.fileSize) ) {
                mmapFailure = true;
                break;
            }
        }
        if ( !mmapFailure ) {
#if BUILDING_DYLD
            uintptr_t mappedSize  = round_page((uintptr_t)region.fileSize);
            uintptr_t mappedStart = (uintptr_t)segAddress;
            uintptr_t mappedEnd   = mappedStart + mappedSize;
            if ( state.config.log.segments ) {
                const MachOLoaded* lmo = (MachOLoaded*)loadAddress;
                state.log("%14.*s (%c%c%c) 0x%012lX->0x%012lX\n",
                          (int)lmo->segmentName(segIndex).size(), lmo->segmentName(segIndex).data(),
                          (region.perms & PROT_READ) ? 'r' : '.', (region.perms & PROT_WRITE) ? 'w' : '.', (region.perms & PROT_EXEC) ? 'x' : '.',
                          mappedStart, mappedEnd);
            }
#endif
        }
        ++segIndex;
    }

#if BUILDING_DYLD && !TARGET_OS_SIMULATOR && __arm64__
    if ( !mmapFailure ) {
        // tell kernel about fairplay encrypted regions
        uint32_t fpTextOffset;
        uint32_t fpSize;
        const Header* hdr = (const Header*)loadAddress;
        // FIXME: record if FP info in PrebuiltLoader
        if ( hdr->isFairPlayEncrypted(fpTextOffset, fpSize) ) {
            int result = state.config.syscall.mremap_encrypted((void*)(loadAddress + fpTextOffset), fpSize, 1, hdr->arch().cpuType(), hdr->arch().cpuSubtype());
            if ( result != 0 ) {
                diag.error("could not register fairplay decryption, mremap_encrypted() => %d", result);
                mmapFailure = true;
            }
        }
    }
#endif

    if ( mmapFailure ) {
        ::vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)vmSpace);
        return nullptr;
    }

#if BUILDING_DYLD && SUPPORT_ROSETTA
    if ( state.config.process.isTranslated && (extraAllocSize != 0) ) {
        // map in translated code at end of mapped segments
        dyld_aot_image_info aotInfo;
        uint64_t            extraSpaceAddr = (long)loadAddress + vmSpace - extraAllocSize;
        int                 ret            = aot_map_extra(path, (mach_header*)loadAddress, (void*)extraSpaceAddr, aotInfo.aotLoadAddress, aotInfo.aotImageSize, aotInfo.aotImageKey);
        if ( ret == 0 ) {
            // fill in the load address, at this point the Rosetta trap has filled in the other fields
            aotInfo.x86LoadAddress = (mach_header*)loadAddress;
    #if HAS_EXTERNAL_STATE
            std::span<const dyld_aot_image_info> aots(&aotInfo, 1);
            // dyld automatically adds an entry to the image list when loading the dylib.
            // Add an entry for the aot info but pass an empty std::span for the dyld image info
            std::span<const dyld_image_info> infos;
            state.externallyViewable->addRosettaImages(aots, infos);
    #endif
            if ( state.config.log.segments ) {
                state.log("%14s (r.x) 0x%012llX->0x%012llX\n", "ROSETTA", extraSpaceAddr, extraSpaceAddr + extraAllocSize);
            }
        }
    }
#endif
    return (MachOAnalyzer*)loadAddress;
}
#endif // BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_UNIT_TESTS

#if BUILDING_DYLD || BUILDING_UNIT_TESTS

// can't do page-in linking with simulator until will know host OS will support it
#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT

static void fixupPage64(void* pageContent, const mwl_info_hdr* blob, const dyld_chained_starts_in_segment* segInfo, uint32_t pageIndex, bool offsetBased)
{
    const uint64_t* bindsArray  = (uint64_t*)((uint8_t*)blob + blob->mwli_binds_offset);
    uint16_t firstStartOffset = segInfo->page_start[pageIndex];
    // check marker for no fixups on the page
    if ( firstStartOffset == DYLD_CHAINED_PTR_START_NONE )
        return;
    uint64_t* chain   = (uint64_t*)((uint8_t*)pageContent + firstStartOffset);
    uint64_t* pageEnd = (uint64_t*)((uint8_t*)pageContent + blob->mwli_page_size);
    // walk chain
    const uint64_t targetAdjust = (offsetBased ? blob->mwli_image_address : blob->mwli_slide);
    uint64_t delta = 0;
    do {
        if ( chain >= pageEnd) {
            fprintf(stderr, "fixup chain entry (%p) off end of page starting at %p\n", chain, pageContent);
            break;
        }
        uint64_t value  = *chain;
        bool     isBind = (value & 0x8000000000000000ULL);
        delta = (value >> 51) & 0xFFF;
        //fprintf(stderr, "   offset=0x%08lX, chain=%p, value=0x%016llX, delta=%lld\n", (long)chain - (long)header, chain, value, delta);
        if ( isBind ) {
            // is bind
            uint32_t bindOrdinal = value & 0x00FFFFFF;
            if ( bindOrdinal >= blob->mwli_binds_count ) {
                fprintf(stderr, "out of range bind ordinal %u (max %u)", bindOrdinal, blob->mwli_binds_count);
                break;
            }
            else {
                uint32_t addend = (value >> 24) & 0xFF;
                *chain = bindsArray[bindOrdinal] + addend;
            }
        }
        else {
            // is rebase
            uint64_t target = value & 0xFFFFFFFFFULL;
            uint64_t high8  = (value >> 36) & 0xFF;
            *chain = target + targetAdjust + (high8 << 56);
        }
        chain = (uint64_t*)((uint8_t*)chain + (delta*4)); // 4-byte stride
    } while ( delta != 0 );
}


static void fixupChain32(uint32_t* chain, const mwl_info_hdr* blob, const dyld_chained_starts_in_segment* segInfo, const uint32_t bindsArray[], uint32_t* pageEnd)
{
    //fprintf(stderr, "fixupChain32(%p)\n", chain);
    uint32_t delta = 0;
    do {
        if ( chain >= pageEnd) {
            fprintf(stderr, "fixup chain entry (%p) off end of page ending at %p\n", chain, pageEnd);
            break;
        }
        uint32_t value = *chain;
        delta = (value >> 26) & 0x1F;
        //fprintf(stderr, "   chain=%p, value=0x%08X, delta=%u\n", chain, value, delta);
        if ( value & 0x80000000 ) {
            // is bind
            uint32_t bindOrdinal = value & 0x000FFFFF;
            if ( bindOrdinal >= blob->mwli_binds_count ) {
                fprintf(stderr, "out of range bind ordinal %u (max %u)", bindOrdinal, blob->mwli_binds_count);
                break;
            }
            else {
                uint32_t addend = (value >> 20) & 0x3F;
                *chain = bindsArray[bindOrdinal] + addend;
            }
        }
        else {
            // is rebase
            uint32_t target = value & 0x03FFFFFF;
            if ( target > segInfo->max_valid_pointer ) {
                // handle non-pointers in chain
                uint32_t bias = (0x04000000 + segInfo->max_valid_pointer)/2;
                *chain = target - bias;
            }
            else {
                *chain = target + (uint32_t)blob->mwli_slide;
            }
        }
        chain += delta;
    } while ( delta != 0 );
}


static void fixupPage32(void* pageContent, const mwl_info_hdr* blob, const dyld_chained_starts_in_segment* segInfo, uint32_t pageIndex)
{
    const uint32_t* bindsArray  = (uint32_t*)((uint8_t*)blob + blob->mwli_binds_offset);
    uint16_t        startOffset = segInfo->page_start[pageIndex];
    uint32_t*       pageEnd     = (uint32_t*)((uint8_t*)pageContent + blob->mwli_page_size);
    if ( startOffset == DYLD_CHAINED_PTR_START_NONE )
        return;
    if ( startOffset & DYLD_CHAINED_PTR_START_MULTI ) {
        // some fixups in the page are too far apart, so page has multiple starts
        uint32_t overflowIndex = startOffset & ~DYLD_CHAINED_PTR_START_MULTI;
        bool chainEnd = false;
        while ( !chainEnd ) {
            chainEnd    = (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
            startOffset = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
            uint32_t* chain = (uint32_t*)((uint8_t*)pageContent + startOffset);
            fixupChain32(chain, blob, segInfo, bindsArray, pageEnd);
            ++overflowIndex;
        }
    }
    else {
        uint32_t* chain = (uint32_t*)((uint8_t*)pageContent + startOffset);
        fixupChain32(chain, blob, segInfo, bindsArray, pageEnd);
    }
 }


#if __has_feature(ptrauth_calls)
static void fixupPageAuth64(void* pageContent, const mwl_info_hdr* blob, const dyld_chained_starts_in_segment* segInfo, uint32_t pageIndex, bool offsetBased)
{
    //fprintf(stderr, "fixupPageAuth64(): pageContent=%p, blob=%p, segInfo=%p, pageIndex=%u\n", pageContent, blob, segInfo, pageIndex);
    const uint64_t* bindsArray  = (uint64_t*)((uint8_t*)blob + blob->mwli_binds_offset);
    uint16_t firstStartOffset = segInfo->page_start[pageIndex];
    // check marker for no fixups on the page
    if ( firstStartOffset == DYLD_CHAINED_PTR_START_NONE )
        return;
    uint64_t* chain   = (uint64_t*)((uint8_t*)pageContent + firstStartOffset);
    uint64_t* pageEnd = (uint64_t*)((uint8_t*)pageContent + blob->mwli_page_size);
    // walk chain
    const uint64_t targetAdjust = (offsetBased ? blob->mwli_image_address : blob->mwli_slide);
    uint64_t delta = 0;
    do {
        if ( chain >= pageEnd) {
            fprintf(stderr, "fixup chain entry (%p) off end of page starting at %p\n", chain, pageContent);
            break;
        }
        uint64_t value = *chain;
        delta = (value >> 51) & 0x7FF;
        //fprintf(stderr, "   chain=%p, value=0x%08llX, delta=%llu\n", chain, value, delta);
        bool isAuth = (value & 0x8000000000000000ULL);
        bool isBind = (value & 0x4000000000000000ULL);
        if ( isAuth ) {
            ptrauth_key  key   = (ptrauth_key)((value >> 49) & 0x3);
            bool     addrDiv   = ((value & (1ULL << 48)) != 0);
            uint16_t diversity = (uint16_t)((value >> 32) & 0xFFFF);
            if ( isBind ) {
                uint32_t bindOrdinal = value & 0x00FFFFFF;
                if ( bindOrdinal >= blob->mwli_binds_count ) {
                    fprintf(stderr, "out of range bind ordinal %u (max %u)", bindOrdinal, blob->mwli_binds_count);
                    break;
                }
                else {
                    *chain = signPointer(bindsArray[bindOrdinal], chain, addrDiv, diversity, key);
                }
            }
            else {
                /* note: in auth rebases only have 32-bits, so target is always offset - never vmaddr */
                uint64_t target = (value & 0xFFFFFFFF) + blob->mwli_image_address;
                *chain = signPointer(target, chain, addrDiv, diversity, key);
            }
        }
        else {
            if ( isBind ) {
                uint32_t bindOrdinal = value & 0x00FFFFFF;
                if ( bindOrdinal >= blob->mwli_binds_count ) {
                    fprintf(stderr, "out of range bind ordinal %u (max %u)", bindOrdinal, blob->mwli_binds_count);
                    break;
                }
                else {
                    uint64_t addend19 = (value >> 32) & 0x0007FFFF;
                    if ( addend19 & 0x40000 )
                        addend19 |=  0xFFFFFFFFFFFC0000ULL;
                    *chain = bindsArray[bindOrdinal] + addend19;
                }
            }
            else {
                uint64_t target = (value & 0x7FFFFFFFFFFULL);
                uint64_t high8  = (value << 13) & 0xFF00000000000000ULL;
                *chain = target + targetAdjust + high8;
            }
        }
        chain += delta;
    } while ( delta != 0 );
}
#endif // __has_feature(ptrauth_calls)


static void fixupPage(void* pageContent, uint64_t userlandAddress, const mwl_info_hdr* blob)
{
    // find seg info and page within segment
    const dyld_chained_starts_in_segment* segInfo   = nullptr;
    uint32_t                              pageIndex = 0;
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)blob + blob->mwli_chains_offset);
    for (uint32_t segIndex=0; segIndex < startsInfo->seg_count; ++segIndex) {
        const dyld_chained_starts_in_segment* seg = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + startsInfo->seg_info_offset[segIndex]);
        uint64_t segStartAddress = (blob->mwli_image_address + seg->segment_offset);
        uint64_t segEndAddress   = segStartAddress + seg->page_count * seg->page_size;
        if ( (segStartAddress <= userlandAddress) && (userlandAddress < segEndAddress) ) {
            segInfo = seg;
            pageIndex = (uint32_t)((userlandAddress-segStartAddress)/(seg->page_size));
            break;
        }
    }
    //fprintf(stderr, "fixupPage(%p), blob=%p, pageIndex=%d, segInfo=%p\n", pageContent, blob, pageIndex, segInfo);
    assert(segInfo != nullptr);

    switch (blob->mwli_pointer_format) {
#if  __has_feature(ptrauth_calls)
       case DYLD_CHAINED_PTR_ARM64E:
            fixupPageAuth64(pageContent, blob, segInfo, pageIndex, false);
            break;
       case DYLD_CHAINED_PTR_ARM64E_USERLAND:
       case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            fixupPageAuth64(pageContent, blob, segInfo, pageIndex, true);
            break;
#endif
        case DYLD_CHAINED_PTR_64:
            fixupPage64(pageContent, blob, segInfo, pageIndex, false);
            break;
        case DYLD_CHAINED_PTR_64_OFFSET:
            fixupPage64(pageContent, blob, segInfo, pageIndex, true);
            break;
        case DYLD_CHAINED_PTR_32:
            fixupPage32(pageContent, blob, segInfo, pageIndex);
            break;
    }
}

// implement __map_with_linking_np() in userland
static int dyld_map_with_linking_np(const mwl_region regions[], uint32_t regionCount, const mwl_info_hdr* blob, uint32_t blobSize)
{
    // sanity check
    if ( blob->mwli_version != 7 )
        return -1;
    uint32_t pointerSize = (blob->mwli_pointer_format == DYLD_CHAINED_PTR_32) ? 4 : 8;
    if ( (blob->mwli_binds_offset + pointerSize*blob->mwli_binds_count) > blobSize ) {
        fprintf(stderr, "bind table extends past blob, blobSize=%d, offset=%d, count=%d\n", blobSize, blob->mwli_binds_offset, blob->mwli_binds_count);
        return -1;
    }
    if ( (blob->mwli_chains_offset + blob->mwli_chains_size) > blobSize )
        return -1;

    // apply fixups to each page in each page
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)blob + blob->mwli_chains_offset);
    //fprintf(stderr, "dyld_map_with_linking_np(), startsInfo=%p, seg_count=%d\n", startsInfo, startsInfo->seg_count);
    for (uint32_t s=0; s < startsInfo->seg_count; ++s) {
        if ( uint32_t segOffset = startsInfo->seg_info_offset[s] ) {
            const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + segOffset);
            uint8_t* segStartAddress = (uint8_t*)(blob->mwli_image_address + segInfo->segment_offset);
            //fprintf(stderr, "dyld_map_with_linking_np(), segStartAddress=%p, page_count=%d\n", segStartAddress, segInfo->page_count);
            for (uint32_t i=0; i < segInfo->page_count; ++i) {
                void* content = (void*)(uintptr_t)(segStartAddress + i*blob->mwli_page_size);
                fixupPage(content, (uintptr_t)content, blob);
            }
        }
    }
    return 0;
}

struct PageInLinkingRange { mwl_region region; std::string_view segName; const dyld_chained_starts_in_segment* chainInfo; };

// Note: disable tail call optimization, otherwise tailcall may remove stack allocated blob
[[clang::disable_tail_calls]] static
int setUpPageInLinkingRegions(RuntimeState& state, const Loader* ldr, uintptr_t slide, uint16_t pointer_format, uint16_t pageSize,
                                     bool forceDyldBinding, const Array<PageInLinkingRange>& ranges, const Array<const void*>& bindTargets)
{
    // create blob on the stack
    uint32_t chainInfoSize = (uint32_t)offsetof(dyld_chained_starts_in_image, seg_info_offset[ranges.count()]);
    for (const PageInLinkingRange& range : ranges) {
        chainInfoSize += range.chainInfo->size;
        chainInfoSize = (chainInfoSize + 3) & (-4); // size should always be 4-byte aligned
    }
    uint32_t pointerSize        = (pointer_format == DYLD_CHAINED_PTR_32) ? 4 : 8;
    uint32_t bindsOffset        = (sizeof(mwl_info_hdr) + chainInfoSize + 7) & (-8); // 8-byte align
    size_t   blobAllocationSize = (size_t)(bindsOffset + pointerSize*bindTargets.count());
    uint8_t buffer[blobAllocationSize];
    bzero(buffer,blobAllocationSize);
    mwl_info_hdr* blob = (mwl_info_hdr*)buffer;
    blob->mwli_version          = 7;
    blob->mwli_page_size        = pageSize;
    blob->mwli_pointer_format   = pointer_format;
    blob->mwli_binds_offset     = bindsOffset;
    blob->mwli_binds_count      = (uint32_t)bindTargets.count();
    blob->mwli_chains_offset    = sizeof(mwl_info_hdr);
    blob->mwli_chains_size      = chainInfoSize;
    blob->mwli_slide            = slide;
    blob->mwli_image_address    = (uintptr_t)ldr->loadAddress(state);
    ::memcpy(&buffer[blob->mwli_binds_offset], bindTargets.data(), pointerSize * blob->mwli_binds_count);
    uint32_t                        offsetInChainInfo = (uint32_t)offsetof(dyld_chained_starts_in_image, seg_info_offset[ranges.count()]);
    uint32_t                        rangeIndex        = 0;
    dyld_chained_starts_in_image*   starts            = (dyld_chained_starts_in_image*)((uint8_t*)blob + blob->mwli_chains_offset);
    starts->seg_count = (uint32_t)ranges.count();
    for (const PageInLinkingRange& range : ranges) {
        starts->seg_info_offset[rangeIndex] = offsetInChainInfo;
        ::memcpy(&buffer[blob->mwli_chains_offset + offsetInChainInfo], range.chainInfo, range.chainInfo->size);
        ++rangeIndex;
        offsetInChainInfo += range.chainInfo->size;
    }
    STACK_ALLOC_ARRAY(mwl_region, regions, ranges.count());
    for (const PageInLinkingRange& range : ranges) {
        regions.push_back(range.region);
    }

    int result = 0;
    if ( forceDyldBinding ) {
        result = dyld_map_with_linking_np(regions.data(), (uint32_t)regions.count(), blob, (uint32_t)blobAllocationSize);
    }
    else {
        if ( state.config.log.fixups || state.config.log.segments ) {
            state.log("Setting up kernel page-in linking for %s\n", ldr->path(state));
            for (const PageInLinkingRange& range : ranges) {
                state.log("%14.*s (%c%c%c) 0x%012llX->0x%012llX (fileOffset=0x%0llX, size=%lluKB)\n",
                          (int)range.segName.size(), range.segName.data(),
                          ((range.region.mwlr_protections & 1) ? 'r' : '.'), ((range.region.mwlr_protections & 2) ? 'w' : '.'), ((range.region.mwlr_protections & 4) ? 'x' : '.'),
                          range.region.mwlr_address, range.region.mwlr_address + range.region.mwlr_size, range.region.mwlr_file_offset, range.region.mwlr_size/1024);
            }
        }
#if BUILDING_DYLD
        result = __map_with_linking_np(regions.data(), (uint32_t)regions.count(), blob, (uint32_t)blobAllocationSize);
        if ( result != 0 ) {
            // kernel backed page-in linking failed, manually do fixups in-process
            if ( state.config.log.fixups || state.config.log.segments )
                state.log("__map_with_linking_np(%s) failed, falling back to linking in-process\n", ldr->path(state));
            result = dyld_map_with_linking_np(regions.data(), (uint32_t)regions.count(), blob, (uint32_t)blobAllocationSize);
        }
#endif
    }
    return result;
}


void Loader::setUpPageInLinking(Diagnostics& diag, RuntimeState& state, uintptr_t slide, uint64_t sliceOffset, const Array<const void*>& bindTargets) const
{
    int fd = state.config.syscall.open(this->path(state), O_RDONLY, 0);
    if ( fd == -1 ) {
        diag.error("open(\"%s\", O_RDONLY) failed with errno=%d", this->path(state), errno);
        return;
    }
    // don't use page-in linking after libSystem is initialized
    // don't use page-in linking if process has a sandbox that disables syscall
    bool canUsePageInLinkingSyscall = (state.config.process.pageInLinkingMode >= 2) && !state.libSystemInitialized() && !state.config.syscall.sandboxBlockedPageInLinking();
    const MachOAnalyzer* ma       = (MachOAnalyzer*)this->loadAddress(state);
    const bool enableTpro         = state.config.process.enableTproDataConst;
    __block uint16_t     format   = 0;
    __block uint16_t     pageSize = 0;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(PageInLinkingRange, kernelPageInRegionInfo, 8);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(PageInLinkingRange, dyldPageInRegionInfo, 8);
    ma->withChainStarts(diag, ma->chainStartsOffset(), ^(const dyld_chained_starts_in_image* startsInfo) {
        // build mwl_region array and compute page starts size
        const Header* mh = (const Header*)ma;
        __block const dyld_chained_starts_in_segment* lastSegChainInfo = nullptr;
        mh->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
            if ( segInfo.segmentIndex < startsInfo->seg_count ) {
                if ( startsInfo->seg_info_offset[segInfo.segmentIndex] == 0 ) {
                    return;
                }
                const dyld_chained_starts_in_segment* segChainInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + startsInfo->seg_info_offset[segInfo.segmentIndex]);
                if ( format == 0 ) {
                    format = segChainInfo->pointer_format;
                }
                else if ( format != segChainInfo->pointer_format ) {
                    diag.error("pointer_format is different in different segments");
                    stop = true;
                }
                if ( pageSize == 0 ) {
                    pageSize = segChainInfo->page_size;
                }
                else if ( pageSize != segChainInfo->page_size ) {
                    diag.error("page_size is different in different segments");
                    stop = true;
                }
                PageInLinkingRange rangeInfo;
                rangeInfo.region.mwlr_fd          = fd;
                rangeInfo.region.mwlr_protections = segInfo.initProt; // Note: DATA_CONST is r/w at this point, so objc can do its fixups
                rangeInfo.region.mwlr_file_offset = segInfo.fileOffset + sliceOffset;
                rangeInfo.region.mwlr_address     = segInfo.vmaddr + slide;
                rangeInfo.region.mwlr_size        = pageSize * segChainInfo->page_count; // in case some pages don't have fixups, don't use segment size
                rangeInfo.segName                 = segInfo.segmentName;
                rangeInfo.chainInfo               = segChainInfo;
                if ( canUsePageInLinkingSyscall ) {
                    // this is where we tune which fixups are done by the kernel
                    // currently only single page DATA segments are done by dyld
                    // the kernel only supports 5 regions per syscall, so any segments past that are fixed up by dyld
                    if ( (segInfo.readOnlyData() || (segChainInfo->page_count > 1)) && (kernelPageInRegionInfo.count() < MWL_MAX_REGION_COUNT) ) {
                        if (enableTpro && segInfo.readOnlyData()) {
                            rangeInfo.region.mwlr_protections |= VM_PROT_TPRO;
                        }
                        kernelPageInRegionInfo.push_back(rangeInfo);
                    }
                    else
                        dyldPageInRegionInfo.push_back(rangeInfo);
                }
                else {
                    dyldPageInRegionInfo.push_back(rangeInfo);
                }
                lastSegChainInfo = segChainInfo;
            }
        });
        // image has not DATA pages to page-in link, so do nothing
        if ( lastSegChainInfo == nullptr )
            return;

        if ( !kernelPageInRegionInfo.empty() ) {
            int kernResult = setUpPageInLinkingRegions(state, this, slide, format, pageSize, (state.config.process.pageInLinkingMode == 1), kernelPageInRegionInfo, bindTargets);
            // if kernel can't do page in linking, then have dyld do the fixups
            if ( kernResult != 0 )
                setUpPageInLinkingRegions(state, this, slide, format, pageSize, true, kernelPageInRegionInfo, bindTargets);
        }
        if ( !dyldPageInRegionInfo.empty() )
            setUpPageInLinkingRegions(state, this, slide, format, pageSize, true, dyldPageInRegionInfo, bindTargets);
    });

    state.config.syscall.close(fd);
}
#endif // !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT


static uintptr_t targetValue(Diagnostics& diag, const Header* hdr, const mach_o::Fixup& fixupInfo, const Array<const void*>& bindTargets)
{
    uintptr_t newValue = 0;
    if ( fixupInfo.isBind ) {
        if ( fixupInfo.bind.bindOrdinal >= bindTargets.count() ) {
            diag.error("out of range bind ordinal %d (max %llu)", fixupInfo.bind.bindOrdinal, bindTargets.count());
            return 0;
        }
        else {
            newValue = (uintptr_t)bindTargets[fixupInfo.bind.bindOrdinal] + fixupInfo.bind.embeddedAddend;
        }
    }
    else {
        newValue = (uintptr_t)hdr + (uintptr_t)fixupInfo.rebase.targetVmOffset;
    }
    if ( fixupInfo.authenticated ) {
#if __has_feature(ptrauth_calls)
        newValue = dyld3::MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, (void*)fixupInfo.location, fixupInfo.auth.usesAddrDiversity, fixupInfo.auth.diversity, fixupInfo.auth.key);
#else
        diag.error("architecture does not support PAC");
#endif
    }
    return newValue;
}

#if TARGET_OS_EXCLAVEKIT
// We allocate this blob in the dyld heap for each segment that needs page-in linking.
// We previously registered Loader::exclaveKitPageInFixups() to be called when a page needs fixups applied.
// The blob for each segment is passed to exclaveKitPageInFixups() when the page handler calls it.
struct EKCallbackInfo
{
    bool                                            sharedCachePages;   // which union variant
    union {
        struct {
            std::span<const void*>                  bindTargets;            // found address of all bindOrdinals
            const dyld_chained_starts_in_segment*   segInfo;                // chain fixup info from LINKEDIT of image
            uintptr_t                               prefLoadAddr;           // preferred address of image (usually zero), needed for vmAddr based chains
            uintptr_t                               loadAddress;            // mach_header of image with DATA pages being page-in linked
            intptr_t                                slide;                  // slid of image with DATA pages being page-in linked
        } standAloneImage;
        struct {
            const dyld_cache_slide_info5*           slideInfoHeader;        // slide info for region in read-only dyld cache
            uintptr_t                               rebaseBaseAddress;      // address each rebase is relative (value_add + cache_slide)
            uintptr_t                               regionStartAddress;     // start of DATA region for this slide info (for page index computation)
        } sharedCacheImage;
    };
};

void Loader::setUpExclaveKitPageInLinking(Diagnostics& diag, RuntimeState& state, uintptr_t slide, uint64_t sliceOffset, const Array<const void*>& bindTargets) const
{
    Image  image(this->loadAddress(state));
    if ( !image.hasChainedFixups() ) {
        diag.error("no chained fixups");
        return;
    }

    if ( state.config.log.fixups || state.config.log.segments )
        state.log("Setting up exclave page-in linking for %s\n", this->path(state));
    std::span<const void*> bindTargetsPermanent;
    const uint32_t         segCount = image.header()->segmentCount();
    for (uint32_t segIndex=0; segIndex < segCount; ++segIndex) {
        if ( const dyld_chained_starts_in_segment* segInfo = image.chainedFixups().startsForSegment(segIndex) ) {
            // lazily build copy bindTargets as separate allocation so it can be shared by multiple segments
            if ( !bindTargets.empty() && bindTargetsPermanent.empty() ) {
                size_t allocationSize = bindTargets.count()*sizeof(void*);
                void* allocation = state.persistentAllocator.malloc(allocationSize);
                memcpy(allocation, &bindTargets[0], allocationSize);
                bindTargetsPermanent = std::span<const void*>((const void**)allocation, bindTargets.count());
            }
            EKCallbackInfo* cbInfo = (EKCallbackInfo*)state.persistentAllocator.malloc(sizeof(EKCallbackInfo));
            cbInfo->sharedCachePages             = false;
            cbInfo->standAloneImage.bindTargets  = bindTargetsPermanent;
            cbInfo->standAloneImage.segInfo      = segInfo;
            cbInfo->standAloneImage.loadAddress  = (uintptr_t)image.header();
            cbInfo->standAloneImage.prefLoadAddr = cbInfo->standAloneImage.loadAddress - slide;
            cbInfo->standAloneImage.slide        = slide;

            uintptr_t   segmentAddress    = (uintptr_t)image.header() + segInfo->segment_offset;
            size_t      sizeNeedingFixups = segInfo->page_count * segInfo->page_size;
            size_t      segmentSize       = image.header()->segmentVmSize(segIndex);
            if ( state.config.log.segments ) {
                std::string_view segName = image.header()->segmentName(segIndex);
                state.log("page-in link setup: %14.*s 0x%012lX->0x%012lX\n",
                          (int)segName.size(), segName.data(),
                          segmentAddress, segmentAddress+sizeNeedingFixups);
            }
            xrt_dyld_register_dynamic_fixups(segmentAddress, sizeNeedingFixups, segmentSize, cbInfo);
        }
    }
}

static uintptr_t targetValue(const mach_o::Fixup& fixupInfo, const EKCallbackInfo* cbInfo, uintptr_t offsetToFinalAddress)
{
    uintptr_t newValue = 0;
    if ( fixupInfo.isBind ) {
        assert(!cbInfo->sharedCachePages);
        assert(fixupInfo.bind.bindOrdinal < cbInfo->standAloneImage.bindTargets.size());
        newValue = (uintptr_t)cbInfo->standAloneImage.bindTargets[fixupInfo.bind.bindOrdinal] + fixupInfo.bind.embeddedAddend;
    }
    else {
        if ( cbInfo->sharedCachePages )
            newValue = cbInfo->sharedCacheImage.rebaseBaseAddress + (uintptr_t)fixupInfo.rebase.targetVmOffset;
        else
            newValue = cbInfo->standAloneImage.loadAddress + (uintptr_t)fixupInfo.rebase.targetVmOffset;
    }
    if ( fixupInfo.authenticated ) {
        newValue = dyld3::MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, (void*)((uintptr_t)fixupInfo.location + offsetToFinalAddress), fixupInfo.auth.usesAddrDiversity, fixupInfo.auth.diversity, fixupInfo.auth.key);
    }
    return newValue;
}

// called during a page fault to apply fixups to a single page
void Loader::exclaveKitPageInFixups(void* cbarg, uintptr_t pageAddress, void* __sized_by(size) buffer, size_t size)
{
    assert(size == 0x4000);
    const EKCallbackInfo* cbInfo = (EKCallbackInfo*)cbarg;
    uint16_t  pointerFormat = 0;
    uint32_t  chainStartOffset = 0;
    uint64_t  baseAddressForRebases = 0;
    if ( cbInfo->sharedCachePages ) {
        uintptr_t pageOffsetInRegion = pageAddress - cbInfo->sharedCacheImage.regionStartAddress;
        uint32_t  pageIndex          = (uint32_t)(pageOffsetInRegion / 0x4000);
        assert(pageAddress >= cbInfo->sharedCacheImage.regionStartAddress);
        assert(pageIndex < cbInfo->sharedCacheImage.slideInfoHeader->page_starts_count);
        chainStartOffset      = cbInfo->sharedCacheImage.slideInfoHeader->page_starts[pageIndex];
        pointerFormat         = DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE;   // would be nice if this was in dyld_cache_slide_info5
        baseAddressForRebases = cbInfo->sharedCacheImage.rebaseBaseAddress;
    }
    else {
        const dyld_chained_starts_in_segment* segInfo = cbInfo->standAloneImage.segInfo;
        uintptr_t segmentStartAddress = cbInfo->standAloneImage.loadAddress + segInfo->segment_offset;
        uintptr_t pageOffsetInSegment = pageAddress - segmentStartAddress;
        uint32_t  pageIndex           = (uint32_t)(pageOffsetInSegment / 0x4000);
        assert(pageAddress >= segmentStartAddress);
        assert(pageIndex < segInfo->page_count);
        chainStartOffset      = segInfo->page_start[pageIndex];
        pointerFormat         = segInfo->pointer_format;
        baseAddressForRebases = cbInfo->standAloneImage.prefLoadAddr;
    }
    if ( chainStartOffset == DYLD_CHAINED_PTR_START_NONE )
        return; // page has no fixups
    const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(pointerFormat);
    const void* chainStartLoc = (void*)((uint8_t*)buffer + chainStartOffset);
    const void* nextLoc       = nullptr;
    const void* endPage       = (void*)((uint8_t*)buffer + 0x4000);
    const uintptr_t offsetToFinalAddress = pageAddress - (uintptr_t)buffer;
    for ( const void* fixupLoc = chainStartLoc; (fixupLoc != nullptr); fixupLoc = nextLoc) {
        // get next before applying fixups, since that will overwrite fields
        nextLoc = pf.nextLocation(fixupLoc);
        // parse fixup and evaluate to pointer value
        mach_o::Fixup fixup    = pf.parseChainEntry(fixupLoc, nullptr, baseAddressForRebases);
        uintptr_t     newValue = targetValue(fixup, cbInfo, offsetToFinalAddress);
        // set pointer
        *((uintptr_t*)fixupLoc) = (uintptr_t)newValue;
        
        if ( (nextLoc != nullptr) && (endPage != nullptr) ) {
            assert(nextLoc < endPage && "chain went off of end of page");
        }
    }
}

// now that we can allocate memory and the dyld cache is mapped, we can register
// page-fault handlers to do page-in linking for shared cache pages
void Loader::setUpExclaveKitSharedCachePageInLinking(RuntimeState& state)
{
    assert(state.config.process.sharedCachePageInLinking);
    assert(state.config.dyldCache.addr != nullptr);
    const DyldSharedCache* dyldCache = state.config.dyldCache.addr;
    dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
        cache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t* mappingPagesStart,
                                  uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info* slideInfoHeader) {
            assert(slideInfoHeader->version == 5);
            const dyld_cache_slide_info5* slinfo = (dyld_cache_slide_info5*)slideInfoHeader;
            EKCallbackInfo*               cbInfo = (EKCallbackInfo*)state.persistentAllocator.malloc(sizeof(EKCallbackInfo));
            cbInfo->sharedCachePages                    = true;
            cbInfo->sharedCacheImage.slideInfoHeader    = slinfo;
            cbInfo->sharedCacheImage.rebaseBaseAddress  = slinfo->value_add + state.config.dyldCache.slide;
            cbInfo->sharedCacheImage.regionStartAddress = (uintptr_t)mappingPagesStart;
            if ( state.config.log.segments ) {
                state.log("page-in link setup dyld cache: 0x%012lX->0x%012llX\n", (long)mappingPagesStart, (long)mappingPagesStart+mappingSize);
            }
            size_t  sizeNeedingFixups = mappingSize; // FIXME: in theory there might be pages at end that don't need page-in linking
            xrt_dyld_register_dynamic_fixups((uint64_t)mappingPagesStart, sizeNeedingFixups, mappingSize, (void*)cbInfo);
        });
    });
}
#endif

void Loader::applyFixupsGeneric(Diagnostics& diag, RuntimeState& state, uint64_t sliceOffset, const Array<const void*>& bindTargets,
                                const Array<const void*>& overrideBindTargets, bool laziesMustBind,
                                const Array<MissingFlatLazySymbol>& missingFlatLazySymbols) const
{
    const MachOAnalyzer* ma    = (MachOAnalyzer*)this->loadAddress(state);
    const uintptr_t      slide = ma->getSlide();
    if ( ma->hasChainedFixups() ) {
        bool applyFixupsNow = true;
#if TARGET_OS_EXCLAVEKIT
        if ( state.config.process.pageInLinkingMode == 2 ) {
            this->setUpExclaveKitPageInLinking(diag, state, slide, sliceOffset, bindTargets);
            // if we cannot do page-in-linking, then do fixups now
            applyFixupsNow = diag.hasError();
            diag.clearError();
        }
#else
  #if !TARGET_OS_SIMULATOR
        // only do page in linking, if binary has standard chained fixups, config allows, and not so many targets that is wastes wired memory
        if ( (state.config.process.pageInLinkingMode != 0) && ma->hasChainedFixupsLoadCommand() && (bindTargets.count() < 10000) && !this->hasFuncVarFixups ) {
            this->setUpPageInLinking(diag, state, slide, sliceOffset, bindTargets);
            // if we cannot do page-in-linking, then do fixups now
            applyFixupsNow = diag.hasError();
            diag.clearError();
        }
  #endif // !TARGET_OS_SIMULATOR
#endif // TARGET_OS_EXCLAVEKIT
        if ( applyFixupsNow ) {
            // walk all chains in all segments and apply fixups non-lazily
            if ( ma->hasChainedFixupsLoadCommand() ) {
                mach_o::Image image((mach_header*)ma);
                uint64_t      baseAddress = image.header()->preferredLoadAddress();
                image.withSegments(^(std::span<const mach_o::MappedSegment> segments) {
                    image.chainedFixups().forEachFixupChainStartLocation(segments, ^(const void* chainStart, uint32_t segIndex, uint32_t pageIndex, uint32_t pageSize, const mach_o::ChainedFixups::PointerFormat& pf, bool& stop) {
                        pf.forEachFixupLocationInChain(chainStart, baseAddress, &segments[segIndex], {}, pageIndex, pageSize, ^(const mach_o::Fixup& fixupInfo, bool& stop2) {
                            const void* loc      = fixupInfo.location;
                            uintptr_t   newValue = targetValue(diag, image.header(), fixupInfo, bindTargets);
                            if ( diag.hasError() ) {
                                stop  = true;
                                stop2 = true;
                                return;
                            }
                            if ( state.config.log.fixups )
                                state.log("fixup: *0x%012lX = 0x%012lX\n", (uintptr_t)loc, (uintptr_t)newValue);
                            *((uintptr_t*)loc) = (uintptr_t)newValue;
                        });
                    });
                });
            }
            else {
                // this is a very old arm64e binary that use opcodes to describe the chains
                ma->withChainStarts(diag, ma->chainStartsOffset(), ^(const dyld_chained_starts_in_image* startsInfo) {
                    ma->fixupAllChainedFixups(diag, startsInfo, slide, bindTargets, ^(void* loc, void* newValue) {
                        if ( state.config.log.fixups )
                            state.log("fixup: *0x%012lX = 0x%012lX\n", (uintptr_t)loc, (uintptr_t)newValue);
                        *((uintptr_t*)loc) = (uintptr_t)newValue;
                    });
                });
            }
        }
#if TARGET_OS_EXCLAVEKIT && XRT_DYLD_FIXUP_INTERFACE_VERSION >= 1
        // At this point, it is safe for the ExclaveKit runtime to touch the
        // runtime sections; let them know.
        xrt_platform_premapped_macho_finalize_memory_state((mach_header *)this->loadAddress(state));
#endif
    }
    else if ( ma->hasOpcodeFixups() ) {
        // process all rebase opcodes
        ma->forEachRebaseLocation_Opcodes(diag, ^(uint64_t runtimeOffset, bool& stop) {
            uintptr_t* loc      = (uintptr_t*)((uint8_t*)ma + runtimeOffset);
            uintptr_t  locValue = *loc;
            uintptr_t  newValue = locValue + slide;
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <rebase>\n", (uintptr_t)loc, (uintptr_t)newValue);
            *loc = newValue;
        });
        if ( diag.hasError() )
            return;

        // process all bind opcodes
        ma->forEachBindLocation_Opcodes(diag, ^(uint64_t runtimeOffset, unsigned targetIndex, bool& stop) {
            uintptr_t* loc      = (uintptr_t*)((uint8_t*)ma + runtimeOffset);
            uintptr_t  newValue = (uintptr_t)(bindTargets[targetIndex]);

#if !TARGET_OS_EXCLAVEKIT && BUILDING_DYLD
            // Record missing lazy symbols
            if ( newValue == (uintptr_t)&missing_symbol_abort ) {
                for (const MissingFlatLazySymbol& missingSymbol : missingFlatLazySymbols) {
                    if ( missingSymbol.bindTargetIndex == targetIndex ) {
                        state.addMissingFlatLazySymbol(this, missingSymbol.symbolName, loc);
                        break;
                    }
                }
                newValue = 0;
            }
#endif
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <%s/bind#%u>\n", (uintptr_t)loc, (uintptr_t)newValue, this->leafName(state), targetIndex);
            *loc = newValue;

        }, ^(uint64_t runtimeOffset, unsigned overrideBindTargetIndex, bool& stop) {
            uintptr_t* loc      = (uintptr_t*)((uint8_t*)ma + runtimeOffset);
            uintptr_t  newValue = (uintptr_t)(overrideBindTargets[overrideBindTargetIndex]);

            // Skip missing weak binds
            if ( newValue == UINTPTR_MAX ) {
                if ( state.config.log.fixups )
                    state.log("fixup: *0x%012lX (skipping missing weak bind) <%s/weak-bind#%u>\n", (uintptr_t)loc, this->leafName(state), overrideBindTargetIndex);
                return;
            }

            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <%s/weak-bind#%u>\n", (uintptr_t)loc, (uintptr_t)newValue, this->leafName(state), overrideBindTargetIndex);
            *loc = newValue;
        });
    }
#if SUPPORT_CLASSIC_RELOCS
    else {
        // process internal relocations
        ma->forEachRebaseLocation_Relocations(diag, ^(uint64_t runtimeOffset, bool& stop) {
            uintptr_t* loc      = (uintptr_t*)((uint8_t*)ma + runtimeOffset);
            uintptr_t  locValue = *loc;
            uintptr_t  newValue = locValue + slide;
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <rebase>\n", (uintptr_t)loc, (uintptr_t)newValue);
            *loc = newValue;
        });
        if ( diag.hasError() )
            return;

        // process external relocations
        ma->forEachBindLocation_Relocations(diag, ^(uint64_t runtimeOffset, unsigned targetIndex, bool& stop) {
            uintptr_t* loc      = (uintptr_t*)((uint8_t*)ma + runtimeOffset);
            uintptr_t  newValue = (uintptr_t)(bindTargets[targetIndex]);
            if ( state.config.log.fixups )
                state.log("fixup: *0x%012lX = 0x%012lX <%s/bind#%u>\n", (uintptr_t)loc, (uintptr_t)newValue, this->leafName(state), targetIndex);
            *loc = newValue;
        });
    }
#endif // SUPPORT_CLASSIC_RELOCS
}

void Loader::applyFunctionVariantFixups(Diagnostics& diag, const RuntimeState& state) const
{
    // check if image use function-variants and needs some internal pointers adjusted
    // dylibs in cache have adjustments done in adjustFunctionVariantsInDyldCache()
    if ( this->dylibInDyldCache || !this->hasFuncVarFixups )
        return;
    const Header* hdr = this->header(state);
    Image         image((mach_header*)hdr);
    if ( !image.hasFunctionVariantFixups() )
        return;
    uint64_t slide = hdr->getSlide();
    image.functionVariantFixups().forEachFixup(^(mach_o::FunctionVariantFixups::InternalFixup fixupInfo) {
        uint64_t   bestImplOffset = this->selectFromFunctionVariants(diag, state, "<internal>", fixupInfo.variantIndex);
        uintptr_t  bestImplAddr   = (uintptr_t)hdr + (uintptr_t)bestImplOffset;
        uint64_t   address        = hdr->segmentVmAddr(fixupInfo.segIndex) + fixupInfo.segOffset + slide;
        uintptr_t* loc            = (uintptr_t*)address;
#if __has_feature(ptrauth_calls)
        if ( fixupInfo.pacAuth )
            bestImplAddr = signPointer(bestImplAddr, loc, fixupInfo.pacAddress, fixupInfo.pacDiversity, (ptrauth_key)fixupInfo.pacKey);
#endif
        if ( state.config.log.fixups )
            state.log("fixup: *0x%012lX = 0x%012lX <function-variant-table#%u>\n", (uintptr_t)loc, (uintptr_t)bestImplAddr, fixupInfo.variantIndex);
        *loc = bestImplAddr;
    });
}

void Loader::findAndRunAllInitializers(RuntimeState& state) const
{
    Diagnostics                           diag;
    const MachOAnalyzer*                  ma              = this->analyzer(state);
    dyld3::MachOAnalyzer::VMAddrConverter vmAddrConverter = ma->makeVMAddrConverter(true);
    ma->forEachInitializer(diag, vmAddrConverter, ^(uint32_t offset) {
        void *func = (void *)((uint8_t*)ma + offset);
        if ( state.config.log.initializers )
            state.log("running initializer %p in %s\n", func, this->path(state));
#if __has_feature(ptrauth_calls)
        func = __builtin_ptrauth_sign_unauthenticated(func, ptrauth_key_asia, 0);
#endif
        dyld3::ScopedTimer timer(DBG_DYLD_TIMING_STATIC_INITIALIZER, (uint64_t)ma, (uint64_t)func, 0);
        MemoryManager::withReadOnlyMemory([&]{
            ((Initializer)func)(state.config.process.argc, state.config.process.argv, state.config.process.envp, state.config.process.apple, &state.vars);
        });
    });

#if !TARGET_OS_EXCLAVEKIT
    // don't support static terminators in arm64e binaries
    if ( ma->isArch("arm64e") )
        return;
    // register static terminators in old binaries, if any
    typedef void (*Terminator)(void*);
    ma->forEachTerminator(diag, vmAddrConverter, ^(uint32_t offset) {
        Terminator func = (Terminator)((uint8_t*)ma + offset);
        state.libSystemHelpers.__cxa_atexit(func, nullptr, (void*)ma);
        if ( state.config.log.initializers )
            state.log("registering old style destructor %p for %s\n", func, this->path(state));
    });
#endif // !TARGET_OS_EXCLAVEKIT
}


// This recusively walks the image graph.  There is the potential for cycles.  To break cycles, if the image is delayed, we
// use the visitedDelayed set to track if the image was already visited.  If the image is not delayed, we use
// beginInitializers() to mark the image visited.
// We have to recurse into delayed dylibs because they may need to be initialized because they have weak-defs or interposing tuples.
void Loader::runInitializersBottomUp(RuntimeState& state, Array<const Loader*>& danglingUpwards, Array<const Loader*>& visitedDelayed) const
{
    // don't run initializers in images that are in delayInit state
    // but continue down graph and run initializers in children if needed
    const bool delayed = this->isDelayInit(state) ;

    // do nothing if already visited
    if ( delayed ) {
        if ( visitedDelayed.contains(this) )
            return;
        // use 'visitedDelayed' to mark we have already handled his image
        visitedDelayed.push_back(this);
    }
    else {
        // marks visited
        if ( (const_cast<Loader*>(this))->beginInitializers(state) )
            return;
    }

    //state.log("runInitializersBottomUp(%s)\n", this->path());

    // make sure everything below this image is initialized before running my initializers
    const uint32_t depCount = this->dependentCount();
    for ( uint32_t i = 0; i < depCount; ++i ) {
        LinkedDylibAttributes childAttrs;
        if ( Loader* child = this->dependent(state, i, &childAttrs) ) {
            if ( childAttrs.upward ) {
                // add upwards to list to process later
                if ( !danglingUpwards.contains(child) )
                    danglingUpwards.push_back(child);
            }
            else {
                child->runInitializersBottomUp(state, danglingUpwards, visitedDelayed);
            }
        }
    }

    if ( !delayed ) {
        // tell objc to run any +load methods in this image (done before C++ initializers)
        state.notifyObjCInit(this);

        // run initializers for this image
        this->runInitializers(state);
    }
}

void Loader::runInitializersBottomUpPlusUpwardLinks(RuntimeState& state) const
{
    //state.log("runInitializersBottomUpPlusUpwardLinks() %s\n", this->path());
    MemoryManager::withWritableMemory([&]{
        // recursively run all initializers
        STACK_ALLOC_ARRAY(const Loader*, danglingUpwards, state.loaded.size()+state.delayLoaded.size());
        STACK_ALLOC_ARRAY(const Loader*, visitedDelayed, state.delayLoaded.size());
        this->runInitializersBottomUp(state, danglingUpwards, visitedDelayed);

        //state.log("runInitializersBottomUpPlusUpwardLinks(%s), found %d dangling upwards\n", this->path(), danglingUpwards.count());

        // go back over all images that were upward linked, and recheck they were initialized (might be danglers)
        STACK_ALLOC_ARRAY(const Loader*, extraDanglingUpwards, state.loaded.size()+state.delayLoaded.size());
        for ( const Loader* ldr : danglingUpwards ) {
            //state.log("running initializers for dangling upward link %s\n", ldr->path());
            ldr->runInitializersBottomUp(state, extraDanglingUpwards, visitedDelayed);
        }
        if ( !extraDanglingUpwards.empty() ) {
            // in case of double upward dangling images, check initializers again
            danglingUpwards.resize(0);
            for ( const Loader* ldr : extraDanglingUpwards ) {
                //state.log("running initializers for dangling upward link %s\n", ldr->path());
                ldr->runInitializersBottomUp(state, danglingUpwards, visitedDelayed);
            }
        }
    });
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

// Used to build prebound targets in PrebuiltLoader.
void Loader::forEachBindTarget(Diagnostics& diag, RuntimeState& state, CacheWeakDefOverride cacheWeakDefFixup, bool allowLazyBinds,
                                         void (^callback)(const ResolvedSymbol& target, bool& stop),
                                         void (^overrideBindCallback)(const ResolvedSymbol& target, bool& stop)) const
{
    this->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        mach_o::Fixups fixups(layout);

        __block unsigned     targetIndex = 0;
        __block unsigned     overrideBindTargetIndex = 0;
#if SUPPORT_PRIVATE_EXTERNS_WORKAROUND
        intptr_t             slide = this->analyzer(state)->getSlide();
#else
        intptr_t             slide = 0;
#endif
        fixups.forEachBindTarget(diag, allowLazyBinds, slide, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
            // Regular and lazy binds
            assert(targetIndex == info.targetIndex);
            ResolvedSymbol targetInfo = this->resolveSymbol(diag, state, info.libOrdinal, info.symbolName, info.weakImport, info.lazyBind, cacheWeakDefFixup);
            targetInfo.targetRuntimeOffset += info.addend;
            callback(targetInfo, stop);
            if ( diag.hasError() )
                stop = true;
            ++targetIndex;
        }, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
            // Opcode based weak binds
            assert(overrideBindTargetIndex == info.targetIndex);
            Diagnostics weakBindDiag; // failures aren't fatal here
            ResolvedSymbol targetInfo = this->resolveSymbol(weakBindDiag, state, info.libOrdinal, info.symbolName, info.weakImport, info.lazyBind, cacheWeakDefFixup);
            if ( weakBindDiag.hasError() ) {
                // In dyld2, it was also ok for a weak bind to be missing.  Then we would let the bind/rebase on this
                // address handle it
                targetInfo.targetLoader        = nullptr;
                targetInfo.targetRuntimeOffset = 0;
                targetInfo.kind                = ResolvedSymbol::Kind::bindToImage;
                targetInfo.isCode              = false;
                targetInfo.isWeakDef           = false;
                targetInfo.isMissingFlatLazy   = false;
                targetInfo.isMaterializing     = false;
            } else {
                targetInfo.targetRuntimeOffset += info.addend;
            }
            overrideBindCallback(targetInfo, stop);
            ++overrideBindTargetIndex;
        });
    });
}

uint64_t Loader::functionVariantTableVMOffset(const RuntimeState& state) const
{
    uint64_t      result = 0;
    const Header* hdr    = (Header*)(this->mf(state));
    if ( hdr->hasFunctionsVariantTable(result) )
        return result;

    return 0; // assert?
}

bool Loader::hasConstantSegmentsToProtect() const
{
    return this->hasReadOnlyData && !this->dylibInDyldCache;
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void Loader::makeSegmentsReadOnly(RuntimeState& state) const
{
    const MachOAnalyzer* ma    = this->analyzer(state);
    uintptr_t            slide = ma->getSlide();
    ((const Header *)ma)->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
        if ( segInfo.readOnlyData() ) {
    #if TARGET_OS_EXCLAVEKIT
            //TODO: EXCLAVES
            (void)slide;
    #else
            const uint8_t* start = (uint8_t*)(segInfo.vmaddr + slide);
            size_t         size  = (size_t)segInfo.vmsize;
            state.config.syscall.mprotect((void*)start, size, PROT_READ);
            if ( state.config.log.segments )
                state.log("mprotect 0x%012lX->0x%012lX to read-only (%s)\n", (long)start, (long)start + size, this->leafName(state));
    #endif
        }
    });
}

void Loader::makeSegmentsReadWrite(RuntimeState& state) const
{
    const MachOAnalyzer* ma    = this->analyzer(state);
    uintptr_t            slide = ma->getSlide();
    ((const Header *)ma)->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
        if ( segInfo.readOnlyData() ) {
    #if TARGET_OS_EXCLAVEKIT
            //TODO: EXCLAVES
            (void)slide;
    #else
            const uint8_t* start = (uint8_t*)(segInfo.vmaddr + slide);
            size_t         size  = (size_t)segInfo.vmsize;
            state.config.syscall.mprotect((void*)start, size, PROT_READ | PROT_WRITE);
            if ( state.config.log.segments )
                state.log("mprotect 0x%012lX->0x%012lX to read-write (%s)\n", (long)start, (long)start + size, this->leafName(state));
    #endif
        }
    });
}

void Loader::logSegmentsFromSharedCache(RuntimeState& state) const
{
    state.log("Using mapping in dyld cache for %s\n", this->path(state));
    uint64_t cacheSlide = state.config.dyldCache.slide;
    ((const Header *)this->loadAddress(state))->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        state.log("%14.*s (%c%c%c) 0x%012llX->0x%012llX \n",
                  (int)info.segmentName.size(), info.segmentName.data(),
                  (info.readable() ? 'r' : '.'), (info.writable() ? 'w' : '.'), (info.executable() ? 'x' : '.'),
                  info.vmaddr + cacheSlide, info.vmaddr + cacheSlide + info.vmsize);
    });
}

// FIXME:  This only handles weak-defs and does not look for non-weaks that override weak-defs
void Loader::addWeakDefsToMap(RuntimeState& state, const std::span<const Loader*>& newLoaders)
{
    for (const Loader* ldr : newLoaders) {
        const MachOAnalyzer* ma = ldr->analyzer(state);
        if ( (ma->flags & MH_WEAK_DEFINES) == 0 )
            continue;
        if ( ldr->hiddenFromFlat() )
            continue;

        // NOTE: using the nlist is faster to scan for weak-def exports, than iterating the exports trie
        Diagnostics diag;
        uint64_t    baseAddress = ((const Header*)ma)->preferredLoadAddress();
        ma->forEachGlobalSymbol(diag, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
            if ( (n_desc & N_WEAK_DEF) != 0 ) {
                // only add if not already in map
                const auto& pos = state.weakDefMap->find(symbolName);
                if ( pos == state.weakDefMap->end() ) {
                    WeakDefMapValue mapEntry;
                    mapEntry.targetLoader        = ldr;
                    mapEntry.targetRuntimeOffset = n_value - baseAddress;
                    mapEntry.isCode              = false;  // unused
                    mapEntry.isWeakDef           = true;
                    state.weakDefMap->operator[](symbolName) = mapEntry;
                }
            }
        });
    }
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

Loader::ResolvedSymbol Loader::resolveSymbol(Diagnostics& diag, RuntimeState& state, int libOrdinal, const char* symbolName,
                                             bool weakImport, bool lazyBind, CacheWeakDefOverride patcher, bool buildingCache) const
{
    __block ResolvedSymbol result = { nullptr, symbolName, 0, 0, ResolvedSymbol::Kind::bindAbsolute, false, false };
    if ( (libOrdinal > 0) && ((unsigned)libOrdinal <= this->dependentCount()) ) {
        result.targetLoader = dependent(state, libOrdinal - 1);
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
        result.targetLoader = this;
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
        result.targetLoader = state.mainExecutableLoader;
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
        __block bool found = false;
        state.locks.withLoadersReadLock(^{
            for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                for ( const Loader* ldr : *list ) {
                    // flat lookup can look in self, even if hidden
                    if ( ldr->hiddenFromFlat() && (ldr != this) )
                        continue;
                    if ( ldr->hasExportedSymbol(diag, state, symbolName, Loader::shallow, Loader::skipResolver, &result) ) {
                        found = true;
                        return;
                    }
                }
            }
        });
        if ( found ) {
            // record the dynamic dependency so the symbol we found does not get unloaded from under us
            if ( result.targetLoader != this ) {
                state.addDynamicReference(this, result.targetLoader);
            }
        }
        else {
            if ( weakImport ) {
                // ok to be missing, bind to NULL
                result.kind                = ResolvedSymbol::Kind::bindAbsolute;
                result.targetRuntimeOffset = 0;
            }
#if BUILDING_DYLD
            else if ( lazyBind ) {
                // lazy bound symbols can be bound to __dyld_missing_symbol_abort
                result.targetLoader        = nullptr;
                result.targetSymbolName    = symbolName;
                result.targetRuntimeOffset = (uintptr_t)&missing_symbol_abort;
                result.targetAddressForDlsym = 0;
                result.kind                = ResolvedSymbol::Kind::bindAbsolute;
                result.isCode              = false; // only used for arm64e which uses trie not nlist
                result.isWeakDef           = false;
                result.isMissingFlatLazy   = true;
                result.isMaterializing     = false;
            }
#endif
            else {
                // missing symbol, but not weak-import or lazy-bound, so error
                diag.error("symbol not found in flat namespace '%s'", symbolName);
            }
        }
        return result;
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP ) {
        const bool             verboseWeak        = state.config.log.fixups;
        __block bool           foundFirst         = false;
#if BUILDING_CACHE_BUILDER
        if ( buildingCache ) {
            // when dylibs in cache are build, we don't have real load order, so do weak binding differently
            if ( verboseWeak )
                state.log("looking for weak-def symbol %s\n", symbolName);

            // look first in /usr/lib/libc++, most will be here
            for ( const Loader* ldr : state.loaded ) {
                ResolvedSymbol libcppResult;
                if ( ldr->hasWeakDefs && (strncmp(ldr->path(state), "/usr/lib/libc++.", 16) == 0) ) {
                    if ( ldr->hasExportedSymbol(diag, state, symbolName, Loader::shallow, Loader::skipResolver, &libcppResult) ) {
                        if ( verboseWeak )
                            state.log("  using %s from libc++.dylib\n", symbolName);
                        return libcppResult;
                    }
                }
            }

            // if not found, try looking in the images itself, most custom weak-def symbols have a copy in the image itself
            ResolvedSymbol selfResult;
            if ( this->hasExportedSymbol(diag, state, symbolName, Loader::shallow, Loader::skipResolver, &selfResult) ) {
                if ( verboseWeak )
                    state.log("  using %s from self %s\n", symbolName, this->path(state));
                return selfResult;
            }

            // if this image directly links with something that also defines this weak-def, use that because we know it will be loaded
            const uint32_t depCount = this->dependentCount();
            for ( uint32_t i = 0; i < depCount; ++i ) {
                Loader::LinkedDylibAttributes depAttrs;
                if ( Loader* depLoader = this->dependent(state, i, &depAttrs) ) {
                    if ( !depAttrs.upward ) {
                        ResolvedSymbol depResult;
                        if ( depLoader->hasExportedSymbol(diag, state, symbolName, Loader::staticLink, Loader::skipResolver, &depResult) ) {
                            if ( verboseWeak )
                                state.log("  using %s from dependent %s\n", symbolName, depLoader->path(state));
                            return depResult;
                        }
                    }
                }
            }

            // no impl??
            diag.error("weak-def symbol (%s) not found in dyld cache", symbolName);
            return result;
        }
        else // fall into app launch case
#endif
        state.locks.withLoadersReadLock(^{
            if ( verboseWeak )
                state.log("looking for weak-def symbol '%s':\n", symbolName);
            state.weakDefResolveSymbolCount++;
            // 5000 is a guess that "this is a large C++ app" and could use a map to speed up coalescing
            if ( (state.weakDefResolveSymbolCount > 5000) && (state.weakDefMap == nullptr) ) {
                state.weakDefMap = new (state.persistentAllocator.malloc(sizeof(WeakDefMap))) WeakDefMap();
            }
            if ( state.weakDefMap != nullptr ) {
                const auto& pos = state.weakDefMap->find(symbolName);
                if ( (pos != state.weakDefMap->end()) && (pos->second.targetLoader != nullptr) ) {
                    //state.log("resolveSymbol(%s) found in map\n", symbolName);
                    result.targetLoader        = pos->second.targetLoader;
                    result.targetSymbolName    = symbolName;
                    result.targetRuntimeOffset = pos->second.targetRuntimeOffset;
                    result.targetAddressForDlsym = 0;
                    result.kind                = ResolvedSymbol::Kind::bindToImage;
                    result.isCode              = pos->second.isCode;
                    result.isWeakDef           = pos->second.isWeakDef;
                    result.isMissingFlatLazy   = false;
                    result.isMaterializing     = false;
                    if ( verboseWeak )
                        state.log("  found %s in map, using impl from %s\n", symbolName, result.targetLoader->path(state));
                    foundFirst = true;
                    return;
                }
            }

            // Keep track of results from the cache to be processed at the end, once
            // we've chosen a canonical definition
            struct CacheLookupResult {
                const Loader*   targetLoader        = nullptr;
                uint64_t        targetRuntimeOffset = 0;
            };
            STACK_ALLOC_ARRAY(CacheLookupResult, cacheResults, state.loaded.size());

            bool weakBindOpcodeClient = !this->dylibInDyldCache && this->mf(state)->hasOpcodeFixups();
            for (const Vector<ConstAuthLoader>* list : { &state.loaded, &state.delayLoaded } ) {
                for ( const Loader* ldr : *list ) {
                    if (  ldr->hasWeakDefs  ) {
                        ResolvedSymbol thisResult;
                        // weak coalescing ignores hidden images
                        if ( ldr->hiddenFromFlat() )
                            continue;
                        if ( ldr->hasExportedSymbol(diag, state, symbolName, Loader::shallow, Loader::skipResolver, &thisResult) ) {
                            if ( weakBindOpcodeClient && !thisResult.isWeakDef && ldr->dylibInDyldCache ) {
                                // rdar://75956202 ignore non-weak symbols in shared cache when opcode based binary is looking for symbols to coalesce
                                continue;
                            }
                            if ( thisResult.targetLoader->dylibInDyldCache && !ldr->hasBeenFixedUp(state) )
                                cacheResults.push_back({ thisResult.targetLoader, thisResult.targetRuntimeOffset });

                            // record first implementation found, but keep searching
                            if ( !foundFirst ) {
                                foundFirst        = true;
                                result            = thisResult;
                                if ( verboseWeak )
                                    state.log("  using '%s' in %s\n", symbolName, thisResult.targetLoader->path(state));
                            }
                            if ( !thisResult.isWeakDef && result.isWeakDef ) {
                                // non-weak wins over previous weak-def
                                // we don't stop search because we need to see if this overrides anything in the dyld cache
                                result = thisResult;
                                if ( verboseWeak )
                                    state.log("  using non-weak '%s' in %s\n", symbolName, thisResult.targetLoader->path(state));
                            }
                        }
                    }
                }
                // if not found anywhere else and this image is hidden, try looking in itself
                if ( !foundFirst && this->hiddenFromFlat() ) {
                    if ( verboseWeak )
                        state.log("  did not find unhidden '%s', trying self (%s)\n", symbolName, this->leafName(state));
                    ResolvedSymbol thisResult;
                    if ( this->hasExportedSymbol(diag, state, symbolName, Loader::shallow, Loader::skipResolver, &thisResult) ) {
                        foundFirst = true;
                        result     = thisResult;
                    }
                }

                // If we find an image in the regular image list, break out so we don't look in the delay init list
                if ( foundFirst )
                    break;
            }

            // Patch the cache if we chose a definition which overrides it
            if ( foundFirst && !cacheResults.empty() && !result.targetLoader->dylibInDyldCache && (patcher != nullptr) ) {
                uint64_t patchedCacheOffset = 0;
                for ( const CacheLookupResult& cacheResult : cacheResults ) {
                    // We have already found the impl which we want all clients to use.
                    // But, later in load order we see something in the dyld cache that also implements
                    // this symbol, so we need to change all caches uses of that to use the found one instead.
                    const Header* cacheHdr = (const Header*)cacheResult.targetLoader->mf(state);
                    uint32_t         cachedOverriddenDylibIndex;
                    if ( state.config.dyldCache.findMachHeaderImageIndex((const mach_header*)cacheHdr, cachedOverriddenDylibIndex) ) {
                        // Use VMAddr's as the cache may not exist if we are in the builder
                        uint64_t cacheOverriddenExportVMAddr = cacheHdr->preferredLoadAddress() + cacheResult.targetRuntimeOffset;
                        uint64_t cacheOverriddenExportOffset = cacheOverriddenExportVMAddr - state.config.dyldCache.unslidLoadAddress;
                        if ( cacheOverriddenExportOffset != patchedCacheOffset ) {
                            // because of re-exports, same cacheOffset shows up in multiple dylibs.  Only call patcher once per
                            if ( verboseWeak )
                                state.log("  found use of '%s' in cache, need to override: %s\n", symbolName, cacheResult.targetLoader->path(state));
                            patcher(cachedOverriddenDylibIndex, (uint32_t)cacheResult.targetRuntimeOffset, result);
                            patchedCacheOffset = cacheOverriddenExportOffset;
                        }
                    }
                }
            }
        });
        if ( foundFirst ) {
            // if a c++ dylib weak-def binds to another dylibs, record the dynamic dependency
            if ( result.targetLoader != this ) {
                state.addDynamicReference(this, result.targetLoader);
            }
            // if we are using a map to cache weak-def resolution, add to map
            if ( (state.weakDefMap != nullptr) && !result.targetLoader->hiddenFromFlat() ) {
                WeakDefMapValue mapEntry;
                mapEntry.targetLoader        = result.targetLoader;
                mapEntry.targetRuntimeOffset = result.targetRuntimeOffset;
                mapEntry.isCode              = result.isCode;
                mapEntry.isWeakDef           = result.isWeakDef;
                state.weakDefMap->operator[](symbolName) = mapEntry;
            }
        }
        else {
            if ( weakImport ) {
                // ok to be missing, bind to NULL
                result.kind                = ResolvedSymbol::Kind::bindAbsolute;
                result.targetRuntimeOffset = 0;
            }
            else {
                // symbol not found in any dylib! even the dylib requesting the weak-def symbol
                char  fromUuidStr[64];
                this->getUuidStr(fromUuidStr);
                diag.error("Symbol not found: %s\n  Referenced from: <%s> %s\n  Expected as weak-def export from some loaded dylib",
                            symbolName, fromUuidStr, this->path(state));
            }
        }
        return result;
    }
    else {
        diag.error("unknown library ordinal %d in %s when binding '%s'", libOrdinal, this->path(state), symbolName);
        return result;
    }
    if ( result.targetLoader != nullptr ) {
        STACK_ALLOC_ARRAY(const Loader*, alreadySearched, state.loaded.size());
        if ( result.targetLoader->hasExportedSymbol(diag, state, symbolName, Loader::staticLink, Loader::skipResolver, &result, &alreadySearched) ) {
            return result;
        }
    }
#if !SUPPORT_CREATING_PREMAPPEDLOADERS
    const DylibPatch*  ignorePatchTable;
    uint16_t           ignoreCacheDylibOverriddenIndex;
#endif
    if ( weakImport ) {
        // ok to be missing, bind to NULL
        result.kind                = ResolvedSymbol::Kind::bindAbsolute;
        result.targetRuntimeOffset = 0;
    }
#if BUILDING_DYLD
    else if ( lazyBind ) {
        // missing lazy binds are bound to abort
        result.targetLoader        = nullptr;
        result.targetSymbolName    = symbolName;
        result.targetRuntimeOffset = (uintptr_t)&missing_symbol_abort;
        result.kind                = ResolvedSymbol::Kind::bindAbsolute;
        result.isCode              = false; // isCode only used for arm64e which does not have lazy symbols, so value is moot
        result.isWeakDef           = false;
        result.isMissingFlatLazy   = false;
        result.isMaterializing     = false;
    }
#endif
#if !SUPPORT_CREATING_PREMAPPEDLOADERS
    else if ( this->overridesDylibInCache(ignorePatchTable, ignoreCacheDylibOverriddenIndex) ) {
        // rdar://126412250 (When processing binds in a dylib that overrides the dyld cache, dyld should set missing symbol values to 0xbad4007 instead of erroring)
        result.kind                = ResolvedSymbol::Kind::bindAbsolute;
        result.targetRuntimeOffset = DYLD_BADROOT_MARKER;
    }
#endif
    else {
        // if libSystem.dylib has not been initialized yet, then the missing symbol is during launch and need to save that info
        const char* expectedInDylib = "unknown";
        if ( result.targetLoader != nullptr )
            expectedInDylib = result.targetLoader->path(state);
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if ( !state.libSystemInitialized() ) {
            state.setLaunchMissingSymbol(symbolName, expectedInDylib, this->path(state));
        }
#endif
        // rdar://79796526 add UUID to error message
        char  fromUuidStr[64];
        this->getUuidStr(fromUuidStr);
        char  expectedUuidStr[64];
        if ( result.targetLoader != nullptr )
            result.targetLoader->getUuidStr(expectedUuidStr);
        else
            strlcpy(expectedUuidStr, "no uuid", sizeof(expectedUuidStr));

        // rdar://15648948 (On fatal errors, check binary's min-OS version and note if from the future)
        Diagnostics tooNewBinaryDiag;
        this->tooNewErrorAddendum(tooNewBinaryDiag, state);

        if ( diag.hasError() ) {
            char symbolErrMsg[1024];
            strlcpy(symbolErrMsg, diag.errorMessageCStr(), sizeof(symbolErrMsg));
            diag.clearError();
            diag.error("%s\n  Referenced from: <%s> %s%s\n  Expected in:     <%s> %s",
                       symbolErrMsg, fromUuidStr, this->path(state), tooNewBinaryDiag.errorMessageCStr(), expectedUuidStr, expectedInDylib);
        }
        else {
            diag.error("Symbol not found: %s\n  Referenced from: <%s> %s%s\n  Expected in:     <%s> %s",
                       symbolName, fromUuidStr, this->path(state), tooNewBinaryDiag.errorMessageCStr(), expectedUuidStr, expectedInDylib);
        }
    }
    return result;
}

// if the binary for this Loader is newer than dyld, then we are trying to run a too new binary
void Loader::tooNewErrorAddendum(Diagnostics& diag, RuntimeState& state) const
{
    mach_o::PlatformAndVersions dyldpvs = ((mach_o::Header*)(&__dso_handle))->platformAndVersions();

    ((mach_o::Header*)this->mf(state))->platformAndVersions().unzip(^(mach_o::PlatformAndVersions pvs) {
        if ( (pvs.platform == dyldpvs.platform) && (pvs.minOS > dyldpvs.minOS) ) {
            char versionString[32];
            diag.error(" (built for %s %s which is newer than running OS)",
                       dyldpvs.platform.name().c_str(), pvs.minOS.toString(versionString));
        }
    });
}

bool Loader::hasExportedSymbol(Diagnostics& diag, RuntimeState& state, const char* symbolName, ExportedSymbolMode mode, ResolverMode resolverMode,
                               ResolvedSymbol* result, dyld3::Array<const Loader*>* alreadySearched) const
{
    // don't search twice
    if ( alreadySearched != nullptr ) {
        for ( const Loader* im : *alreadySearched ) {
            if ( im == this )
                return false;
        }
        alreadySearched->push_back(this);
    }
    bool               canSearchDependents;
    bool               searchNonReExports;
    bool               searchSelf;
    ExportedSymbolMode depsMode;
    switch ( mode ) {
        case staticLink:
            canSearchDependents = true;
            searchNonReExports  = false;
            searchSelf          = true;
            depsMode            = staticLink;
            break;
        case shallow:
            canSearchDependents = false;
            searchNonReExports  = false;
            searchSelf          = true;
            depsMode            = shallow;
            break;
        case dlsymNext:
            canSearchDependents = true;
            searchNonReExports  = true;
            searchSelf          = false;
            depsMode            = dlsymSelf;
            break;
        case dlsymSelf:
            canSearchDependents = true;
            searchNonReExports  = true;
            searchSelf          = true;
            depsMode            = dlsymSelf;
            break;
    }

    // The cache builder can't use runtimeOffset's to get the exports trie.  Instead use the layout from
    // the builder
    __block const uint8_t* trieStart = nullptr;
    __block const uint8_t* trieEnd   = nullptr;
    __block bool hasTrie = false;
#if SUPPORT_VM_LAYOUT
    const MachOLoaded* ml = this->loadAddress(state);
    //state.log("Loader::hasExportedSymbol(%s) this=%s\n", symbolName, this->path());
    uint64_t trieRuntimeOffset;
    uint32_t trieSize;
    if ( this->getExportsTrie(trieRuntimeOffset, trieSize) ) {
        trieStart = (uint8_t*)ml + trieRuntimeOffset;
        trieEnd   = trieStart + trieSize;
        hasTrie   = true;
    }
#else
    this->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        if ( layout.linkedit.exportsTrie.hasValue() ) {
            trieStart   = layout.linkedit.exportsTrie.buffer;
            trieEnd     = trieStart + layout.linkedit.exportsTrie.bufferSize;
            hasTrie     = true;
        }
    });
#endif

    if ( hasTrie ) {
        const uint8_t* node      = MachOLoaded::trieWalk(diag, trieStart, trieEnd, symbolName);
        //state.log("    trieStart=%p, trieSize=0x%08X, node=%p, error=%s\n", trieStart, trieSize, node, diag.errorMessage());
        if ( (node != nullptr) && searchSelf ) {
            const uint8_t* p     = node;
            const uint64_t flags = MachOLoaded::read_uleb128(diag, p, trieEnd);
            if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
                // re-export from another dylib, lookup there
                const uint64_t ordinal      = MachOLoaded::read_uleb128(diag, p, trieEnd);
                const char*    importedName = (char*)p;
                bool nameChanged = false;
                if ( importedName[0] == '\0' ) {
                    importedName = symbolName;
                } else if ( strcmp(importedName, symbolName) != 0 ) {
                    nameChanged = true;
                }
                if ( (ordinal == 0) || (ordinal > this->dependentCount()) ) {
                    diag.error("re-export ordinal %lld in %s out of range for %s", ordinal, this->path(state), symbolName);
                    return false;
                }
                uint32_t                 depIndex = (uint32_t)(ordinal - 1);
                LinkedDylibAttributes depAttrs;
                if ( Loader* depLoader = this->dependent(state, depIndex, &depAttrs) ) {
                    // <rdar://91326465> Explicitly promote to a ::staticLink
                    // resolution when looking for a reexported symbol in ::shallow mode.
                    // The symbol might be located in one of the reexported libraries
                    // of the dependent. If the caller checks all loaders with
                    // ::shallow mode it won't be able to find an aliased symbol,
                    // because it will only look for the original name.
                    if ( nameChanged && mode == Loader::shallow )
                        mode = Loader::staticLink;
                    if ( nameChanged && alreadySearched ) {
                        // As we are changing the symbol name we are looking for, use a new alreadySearched.  The existnig
                        // alreadySearched may include loaders we have searched before for the old name, but not the new one,
                        // and we want to check them again
                        STACK_ALLOC_ARRAY(const Loader*, nameChangedAlreadySearched, state.loaded.size());
                        return depLoader->hasExportedSymbol(diag, state, importedName, mode, resolverMode, result, &nameChangedAlreadySearched);
                    }
                    return depLoader->hasExportedSymbol(diag, state, importedName, mode, resolverMode, result, alreadySearched);
                }
                return false; // re-exported symbol from weak-linked dependent which is missing
            }
            else if ( flags & EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT ) {
                if ( diag.hasError() )
                    return false;
                // symbol has a variant table as second entry, skip first value (default impl addr)
                (void)MachOLoaded::read_uleb128(diag, p, trieEnd);
                uint32_t  fvTableIndex         = (uint32_t)MachOLoaded::read_uleb128(diag, p, trieEnd);
                uint64_t  betterFunctionOffset = this->selectFromFunctionVariants(diag, state, symbolName, fvTableIndex);
                if ( diag.hasError() )
                    return false;
                result->targetLoader        = this;
                result->targetSymbolName    = symbolName;
                result->targetRuntimeOffset = betterFunctionOffset;
                result->kind                = ResolvedSymbol::Kind::bindToImage;
                result->isCode              = true;
                result->isWeakDef           = false;
                result->isMissingFlatLazy   = false;
                result->isFunctionVariant   = true;
                result->variantIndex        = fvTableIndex;
                return true;
            }
            else {
                if ( diag.hasError() )
                    return false;
                bool isAbsoluteSymbol           = ((flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE);
                uintptr_t targetRuntimeOffset   = (uintptr_t)MachOLoaded::read_uleb128(diag, p, trieEnd);

#if BUILDING_DYLD
                bool isResolver = (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER);
                if ( isResolver && (resolverMode == runResolver) ) {
                    uintptr_t resolverFuncRuntimeOffset = (uintptr_t)MachOLoaded::read_uleb128(diag, p, trieEnd);
                    const uint8_t* dylibLoadAddress = (const uint8_t*)this->loadAddress(state);
                    typedef void* (*ResolverFunc)(void);
                    ResolverFunc resolver = (ResolverFunc)(dylibLoadAddress + resolverFuncRuntimeOffset);
#if __has_feature(ptrauth_calls)
                    resolver = __builtin_ptrauth_sign_unauthenticated(resolver, ptrauth_key_asia, 0);
#endif
                    const void* resolverResult = (*resolver)();
                    targetRuntimeOffset = (uintptr_t)resolverResult - (uintptr_t)dylibLoadAddress;
                }
#endif

                result->targetLoader            = this;
                result->targetSymbolName        = symbolName;
                result->targetRuntimeOffset     = targetRuntimeOffset;
                result->kind                    = isAbsoluteSymbol ? ResolvedSymbol::Kind::bindAbsolute : ResolvedSymbol::Kind::bindToImage;
                result->isCode                  = this->mf(state)->inCodeSection((uint32_t)(result->targetRuntimeOffset));
#if BUILDING_DYLD
                result->targetAddressForDlsym   = resolvedAddress(state, *result);
                result->targetAddressForDlsym   = interpose(state, result->targetAddressForDlsym);
#if __has_feature(ptrauth_calls)
                if ( result->isCode )
                    result->targetAddressForDlsym = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)result->targetAddressForDlsym, ptrauth_key_asia, 0);
#endif
#endif
                result->isWeakDef               = (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
                result->isMissingFlatLazy       = false;
                result->isMaterializing         = false;
                return true;
            }
        }
    }
    else {
        // try old slow way
        const mach_o::MachOFileRef fileRef = this->mf(state);
        __block bool         found = false;
        this->withLayout(diag, state, ^(const mach_o::Layout& layout) {
            mach_o::SymbolTable symbolTable(layout);

            symbolTable.forEachGlobalSymbol(diag, ^(const char* n_name, uint64_t n_value, uint8_t n_type,
                                                    uint8_t n_sect, uint16_t n_desc, bool& stop) {
                if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) ) {
                    if ( strcmp(n_name, symbolName) == 0 ) {
                        result->targetLoader            = this;
                        result->targetSymbolName        = symbolName;
                        result->targetRuntimeOffset     = (uintptr_t)(n_value - ((const Header*)fileRef)->preferredLoadAddress());
                        result->kind                    = ResolvedSymbol::Kind::bindToImage;
                        result->isCode                  = false; // only used for arm64e which uses trie not nlist
                        result->isWeakDef               = (n_desc & N_WEAK_DEF);
                        result->isMissingFlatLazy       = false;
                        result->isMaterializing         = false;
#if BUILDING_DYLD
                        result->targetAddressForDlsym   = resolvedAddress(state, *result);
                        result->targetAddressForDlsym   = interpose(state, result->targetAddressForDlsym);
                        // arm64e will always have exports trie and we are in nlist case, so does not need to be PAC signed
#else
                        result->targetAddressForDlsym   = 0;
#endif
                        stop                            = true;
                        found                           = true;
                    }
                }
            });
        });
        if ( found )
            return true;
    }

    if ( const JustInTimeLoader* jitThis = this->isJustInTimeLoader() ) {
        if ( const PseudoDylib *pd = jitThis->pseudoDylib() ) {
            const char *symbolNames[] = { symbolName };
            void *addrs[1] = { nullptr };
            _dyld_pseudodylib_symbol_flags flags[1] = { DYLD_PSEUDODYLIB_SYMBOL_FLAGS_NONE };
            if (char *errMsg = pd->lookupSymbols(symbolNames, addrs, flags)) {
                diag.error("pseudo-dylib lookup error: %s", errMsg);
                pd->disposeString(errMsg);
                return false;
            }
            if ( flags[0] & DYLD_PSEUDODYLIB_SYMBOL_FLAGS_FOUND ) {
                result->targetLoader = this;
                result->targetSymbolName        = symbolName;
                result->targetRuntimeOffset     = (uintptr_t)addrs[0] - (uintptr_t)this->mf(state);
                result->kind                    = ResolvedSymbol::Kind::bindToImage;
                result->isCode                  = flags[0] & DYLD_PSEUDODYLIB_SYMBOL_FLAGS_CALLABLE;
#if BUILDING_DYLD
                result->targetAddressForDlsym   = resolvedAddress(state, *result);
                result->targetAddressForDlsym   = interpose(state, result->targetAddressForDlsym);
#if __has_feature(ptrauth_calls)
                if ( result->isCode )
                    result->targetAddressForDlsym = (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)result->targetAddressForDlsym, ptrauth_key_asia, 0);
#endif
#endif
                result->isWeakDef               = flags[0] & DYLD_PSEUDODYLIB_SYMBOL_FLAGS_WEAK_DEF;
                result->isMissingFlatLazy       = false;
                result->isMaterializing         = flags[0] & DYLD_PSEUDODYLIB_SYMBOL_FLAGS_MATERIALIZING;
                return true;
            }
        }
    }

    if ( canSearchDependents ) {
        // Search re-exported dylibs
        const uint32_t depCount = this->dependentCount();
        for ( uint32_t i = 0; i < depCount; ++i ) {
            Loader::LinkedDylibAttributes depAttrs;
            if ( Loader* depLoader = this->dependent(state, i, &depAttrs) ) {
                //state.log("dep #%d of %p is %d %p (%s %s)\n", i, this, (int)depKind, depLoader, this->path(), depLoader->path());
                // when dlsym() continues to search (searchNonReExports), don't follow upward or delay linkages
                if ( depAttrs.reExport || (searchNonReExports && !depAttrs.upward && !depAttrs.delayInit)  ) {
                    if ( depLoader->hasExportedSymbol(diag, state, symbolName, depsMode, resolverMode, result, alreadySearched) )
                        return true;
                }
            }
        }
    }
    return false;
}

// FIXME: use of Image here is expensive, especially for PrebuiltLoaders.  Getting the function variants linkedit blob
// should be different for JustInTimeLoaders and PrebuiltLoaders
uint64_t Loader::selectFromFunctionVariants(Diagnostics& diag, const RuntimeState& state, const char* symbolName, uint32_t fvTableIndex) const
{
    Image image(this->mf(state));
    if ( !image.hasFunctionVariants() ) {
        diag.error("function variant table missing for '%s'", symbolName);
        return 0;
    }
#if SUPPORT_VM_LAYOUT
    const FunctionVariants& fvs = image.functionVariants();
#else
    __block std::span<const uint8_t> linkeditBytes;
    this->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        if ( layout.linkedit.functionVariants.hasValue() ) {
            linkeditBytes = std::span<const uint8_t>(layout.linkedit.functionVariants.buffer, layout.linkedit.functionVariants.bufferSize);
        }
    });
    const FunctionVariants fvs(linkeditBytes);
#endif
    if ( fvTableIndex >= fvs.count() ) {
        diag.error("function variant index %u too large for '%s'", fvTableIndex, symbolName);
        return 0;
    }

    return state.config.process.selectFromFunctionVariants(fvs, fvTableIndex);
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
uintptr_t Loader::resolvedAddress(RuntimeState& state, const ResolvedSymbol& symbol)
{
    switch ( symbol.kind ) {
        case ResolvedSymbol::Kind::rebase:
        case ResolvedSymbol::Kind::bindToImage:
            return (uintptr_t)symbol.targetLoader->loadAddress(state) + (uintptr_t)symbol.targetRuntimeOffset;
        case ResolvedSymbol::Kind::bindAbsolute:
            return (uintptr_t)symbol.targetRuntimeOffset;
    }
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS



uintptr_t Loader::interpose(RuntimeState& state, uintptr_t value, const Loader* forLoader)
{
    // <rdar://problem/25686570> ignore interposing on a weak function that does not exist
    if ( value == 0 )
        return 0;

    // Always start with objc patching.  This is required every when AMFI may not permit other interposing
    for ( const InterposeTupleAll& tuple : state.patchedObjCClasses ) {
        if ( tuple.replacee == value ) {
            if ( state.config.log.interposing )
                state.log("  interpose replaced 0x%08lX with 0x%08lX in %s\n", value, tuple.replacement, forLoader ? forLoader->path(state) : "dlsym");
            return tuple.replacement;
        }
    }

    // Next singleton patching, which also may happen without other interposing
    for ( const InterposeTupleAll& tuple : state.patchedSingletons ) {
        if ( tuple.replacee == value ) {
            if ( state.config.log.interposing )
                state.log("  interpose replaced 0x%08lX with 0x%08lX in %s\n", value, tuple.replacement, forLoader ? forLoader->path(state) : "dlsym");
            return tuple.replacement;
        }
    }

#if !TARGET_OS_EXCLAVEKIT
    // AMFI can ban interposing
    // Note we check this here just in case someone tried to substitute a fake interposing tuples array in the state
    if ( !state.config.security.allowInterposing )
        return value;
#endif 

    // look for image specific interposing (needed for multiple interpositions on the same function)
    for ( const InterposeTupleSpecific& tuple : state.interposingTuplesSpecific ) {
        if ( (tuple.replacee == value) && (tuple.onlyImage == forLoader) ) {
            if ( state.config.log.interposing )
                state.log("  interpose replaced 0x%08lX with 0x%08lX in %s\n", value, tuple.replacement, forLoader ? forLoader->path(state) : "");
            return tuple.replacement;
        }
    }

    // no image specific interpose, so look for generic interpose
    for ( const InterposeTupleAll& tuple : state.interposingTuplesAll ) {
        if ( tuple.replacee == value ) {
            if ( state.config.log.interposing )
                state.log("  interpose replaced 0x%08lX with 0x%08lX in %s\n", value, tuple.replacement, forLoader ? forLoader->path(state) : "dlsym");
            return tuple.replacement;
        }
    }
    return value;
}

#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
void Loader::adjustFunctionVariantsInDyldCache(RuntimeState& state)
{
    const DyldSharedCache* dyldCache = state.config.dyldCache.addr;
    if ( dyldCache == nullptr )
        return; // no dyld cache

    // make the cache writable for this block
    DyldCacheDataConstLazyScopedWriter dataConstWriter(state);
    DyldCacheDataConstLazyScopedWriter* dataConstWriterPtr = &dataConstWriter; // work around to make accessible in cacheWeakDefFixup

    // If dylibs in dyld cache have any function-variants, select which variant to use.
    // Requires DATA_CONST to be writable, and function-variant flags to be setup.
    if ( dyldCache != nullptr ) {
        dyldCache->forEachFunctionVariantPatchLocation(^(const void* loc, PointerMetaData pmd, const mach_o::FunctionVariants& fvs, const mach_o::Header* dylibHdr, int variantIndex, bool& stop) {
            uintptr_t  bestImplOffset = (uintptr_t)state.config.process.selectFromFunctionVariants(fvs, variantIndex);
            uintptr_t  bestImplAddr   = (uintptr_t)dylibHdr + (uintptr_t)bestImplOffset;
            uintptr_t* fixupLoc       = (uintptr_t*)loc;
#if __has_feature(ptrauth_calls)
            if ( pmd.authenticated )
                bestImplAddr = signPointer(bestImplAddr, (void*)loc, pmd.usesAddrDiversity, pmd.diversity, (ptrauth_key)pmd.key);
#endif
            if ( *fixupLoc != bestImplAddr ) {
                dataConstWriterPtr->makeWriteable();
                if ( state.config.log.fixups )
                    state.log("dyld cache function variant patch: *%p = %p\n", fixupLoc, (void*)bestImplAddr);
                *fixupLoc = bestImplAddr;
            }
        });
    }
}
#endif


#if (BUILDING_DYLD || BUILDING_UNIT_TESTS)
#if !TARGET_OS_EXCLAVEKIT
void Loader::applyInterposingToDyldCache(RuntimeState& state)
{
    const DyldSharedCache* dyldCache = state.config.dyldCache.addr;
    if ( dyldCache == nullptr )
        return; // no dyld cache to interpose
    if ( state.interposingTuplesAll.empty() )
        return; // no interposing tuples

    // make the cache writable for this block
    DyldCacheDataConstScopedWriter patcher(state);

    state.setVMAccountingSuspending(true);
    for ( const InterposeTupleAll& tuple : state.interposingTuplesAll ) {
        uint32_t imageIndex;
        uintptr_t cacheOffsetOfReplacee = tuple.replacee - (uintptr_t)dyldCache;
        if ( !dyldCache->addressInText(cacheOffsetOfReplacee, &imageIndex) )
            continue;

        // Convert from a cache offset to an image offset
        uint64_t mTime;
        uint64_t inode;
        const Header* imageHdr = (const Header*)(dyldCache->getIndexedImageEntry(imageIndex, mTime, inode));
        if ( imageHdr == nullptr )
            continue;

        uint32_t dylibOffsetOfReplacee = (uint32_t)((dyldCache->unslidLoadAddress() + cacheOffsetOfReplacee) - imageHdr->preferredLoadAddress());

        dyldCache->forEachPatchableExport(imageIndex, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                        PatchKind patchKind) {
            // Skip patching anything other than this symbol
            if ( dylibVMOffsetOfImpl != dylibOffsetOfReplacee )
                return;
            uintptr_t newLoc = tuple.replacement;
            dyldCache->forEachPatchableUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                   ^(uint64_t cacheVMOffset, MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                     bool isWeakImport) {
                uintptr_t* loc      = (uintptr_t*)((uintptr_t)dyldCache + cacheVMOffset);
                uintptr_t  newValue = newLoc + (uintptr_t)addend;
    #if __has_feature(ptrauth_calls)
                if ( pmd.authenticated ) {
                    newValue = dyld3::MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, loc, pmd.usesAddrDiversity, pmd.diversity, pmd.key);
                    *loc     = newValue;
                    if ( state.config.log.interposing )
                        state.log("interpose: *%p = %p (JOP: diversity 0x%04X, addr-div=%d, key=%s)\n",
                                  loc, (void*)newValue, pmd.diversity, pmd.usesAddrDiversity, MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::keyName(pmd.key));
                    return;
                }
    #endif
                if ( state.config.log.interposing )
                    state.log("interpose: *%p = 0x%0llX (dyld cache patch) to %s\n", loc, newLoc + addend, exportName);
                *loc = newValue;
            });
        });
    }
    state.setVMAccountingSuspending(false);
}
#endif // !TARGET_OS_EXCLAVEKIT

void Loader::applyCachePatchesToOverride(RuntimeState& state, const Loader* dylibToPatch,
                                         uint16_t overriddenDylibIndex, const DylibPatch* patches,
                                         DyldCacheDataConstLazyScopedWriter& cacheDataConst) const
{
    const DyldSharedCache*  dyldCache           = state.config.dyldCache.addr;
    const MachOAnalyzer*    dylibToPatchMA      = dylibToPatch->analyzer(state);
    uint32_t                dylibToPatchIndex   = dylibToPatch->ref.index;

    // Early return if we have no exports used in the client dylib.  Then we don't need to walk every export
    if ( !dyldCache->shouldPatchClientOfImage(overriddenDylibIndex, dylibToPatchIndex) )
        return;

    uint32_t patchVersion = dyldCache->patchInfoVersion();
    assert((patchVersion == 2) || (patchVersion == 3) || (patchVersion == 4));
    __block bool suspended = false;
    __block const DylibPatch* cachePatch = patches;
    dyldCache->forEachPatchableExport(overriddenDylibIndex, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                              PatchKind patchKind) {
        const DylibPatch* patch = cachePatch;
        ++cachePatch;

        // Skip patching objc classes and singletons.  We'll handle those another way
        switch ( patchKind ) {
            case PatchKind::regular:
                break;
            case PatchKind::cfObj2:
                if ( patch->overrideOffsetOfImpl == DylibPatch::singleton )
                    return;
                break;
            case PatchKind::objcClass:
                if ( patch->overrideOffsetOfImpl == DylibPatch::objcClass )
                    return;
                break;
        }

        uintptr_t targetRuntimeAddress = DYLD_BADROOT_MARKER;   // magic value to cause a unique crash if missing symbol in root is used
        if ( patch->overrideOffsetOfImpl != DylibPatch::missingSymbol )
            targetRuntimeAddress = (uintptr_t)(this->loadAddress(state)) + ((intptr_t)patch->overrideOffsetOfImpl);
        
        dyldCache->forEachPatchableUseOfExportInImage(overriddenDylibIndex, dylibVMOffsetOfImpl, dylibToPatchIndex,
                                                      ^(uint32_t userVMOffset,
                                                        dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                        bool isWeakImport) {
            // ensure dyld cache __DATA_CONST is writeable
            cacheDataConst.makeWriteable();

            // overridden dylib may not effect this dylib, so only suspend when we find it does effect it
            if ( !suspended ) {
#if !TARGET_OS_EXCLAVEKIT
                state.setVMAccountingSuspending(true);
                suspended = true;
#endif // !TARGET_OS_EXCLAVEKIT
            }

            uintptr_t* loc                  = (uintptr_t*)((uint8_t*)dylibToPatchMA + userVMOffset);
            uintptr_t  newValue             = targetRuntimeAddress + (uintptr_t)addend;

            // if client in dyld cache is ok with symbol being missing, set its use to NULL instead of bad-missing-value
            if ( isWeakImport && (targetRuntimeAddress == DYLD_BADROOT_MARKER) )
                newValue = 0;

            // if overridden dylib is also interposed, use interposing
            for ( const InterposeTupleAll& tuple : state.interposingTuplesAll ) {
                if ( tuple.replacee == newValue ) {
                    newValue = tuple.replacement;
                }
            }
            // rdar://125168527 (help debug issues with roots missing symbols)
            if ( !isWeakImport && (targetRuntimeAddress == DYLD_BADROOT_MARKER) ) {
                const char* overriddenPath = dyldCache->getIndexedImagePath(overriddenDylibIndex);
                state.log("symbol '%s' missing from root that overrides %s. Use of that symbol in %s is being set to 0xBAD4007.\n", exportName, overriddenPath, dylibToPatch->path(state));
            }
#if __has_feature(ptrauth_calls)
            if ( pmd.authenticated && (newValue != 0) ) {
                newValue = dyld3::MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, loc, pmd.usesAddrDiversity, pmd.diversity, pmd.key);
                if ( *loc != newValue ) {
                    *loc = newValue;
                    if ( state.config.log.fixups ) {
                        state.log("cache fixup: *0x%012lX = 0x%012lX (*%s+0x%012lX = %s+0x%012lX) (JOP: diversity=0x%04X, addr-div=%d, key=%s)\n",
                                  (long)loc, newValue,
                                  dylibToPatch->leafName(state), (long)userVMOffset,
                                  this->leafName(state), (long)patch->overrideOffsetOfImpl,
                                  pmd.diversity, pmd.usesAddrDiversity, MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::keyName(pmd.key));
                    }
                }
                return;
            }
#endif
            if ( *loc != newValue ) {
                *loc = newValue;
                if ( state.config.log.fixups )
                    state.log("cache fixup: *0x%012lX = 0x%012lX (*%s+0x%012lX = %s+0x%012lX)\n",
                              (long)loc, (long)newValue,
                              dylibToPatch->leafName(state), (long)userVMOffset,
                              this->leafName(state), (long)patch->overrideOffsetOfImpl);
            }
        });
    });
    // Ensure the end marker is as expected
    assert(cachePatch->overrideOffsetOfImpl == DylibPatch::endOfPatchTable);


    if ( suspended ) {
#if !TARGET_OS_EXCLAVEKIT
        state.setVMAccountingSuspending(false);
#endif // !TARGET_OS_EXCLAVEKIT
    }
}

void Loader::applyFixupsCheckCachePatching(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const
{
    // if this is in the dyld cache, there is normally no fixups need
    if ( this->dylibInDyldCache ) {
        // But if some lower level cached dylib has a root, we
        // need to patch this image's uses of that rooted dylib.
        // We also need to patch if there are unzippered twins.
        // In that case, we need to make sure that if this binary is in the cache and linked to
        // the macOS side of the unzippered twins, then the iOSMac unzippered twin can now patch this dylib
        if ( state.hasOverriddenCachedDylib() || state.hasOverriddenUnzipperedTwin() ) {
            // have each other image apply to me any cache patching it has
            for ( const Loader* ldr : state.loaded ) {
                ldr->applyCachePatchesTo(state, this, cacheDataConst);
            }
        }
    }
}

void Loader::applyCachePatchesTo(RuntimeState& state, const Loader* dylibToPatch, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const
{
    // do nothing if this dylib does not override something in the dyld cache
    uint16_t            overriddenDylibIndex;
    const DylibPatch*   patches;
    if ( !this->overridesDylibInCache(patches, overriddenDylibIndex) )
        return;
    if ( patches != nullptr )
        this->applyCachePatchesToOverride(state, dylibToPatch, overriddenDylibIndex, patches, cacheDataConst);

    // The override here may be a root of an iOSMac dylib, in which case we should also try patch uses of the macOS unzippered twin
    if ( !this->isPrebuilt && state.config.process.catalystRuntime ) {
        if ( const JustInTimeLoader* jitThis = this->isJustInTimeLoader() ) {
            if ( const DylibPatch* patches2 = jitThis->getCatalystMacTwinPatches() ) {
                uint16_t macOSTwinIndex = Loader::indexOfUnzipperedTwin(state, overriddenDylibIndex);
                if ( macOSTwinIndex != kNoUnzipperedTwin )
                    this->applyCachePatchesToOverride(state, dylibToPatch, macOSTwinIndex, patches2, cacheDataConst);
            }
        }
    }
}

void Loader::applyCachePatches(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const
{
    // do nothing if this dylib does not override something in the dyld cache
    uint16_t            overriddenDylibIndex;
    const DylibPatch*   patches;
    if ( !this->overridesDylibInCache(patches, overriddenDylibIndex) )
        return;

    if ( patches == nullptr )
        return;

    const DyldSharedCache* dyldCache = state.config.dyldCache.addr;

    __block bool suspended = false;
    __block const DylibPatch* cachePatch = patches;
    dyldCache->forEachPatchableExport(overriddenDylibIndex, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                              PatchKind patchKind) {
        const DylibPatch* patch = cachePatch;
        ++cachePatch;

        // Skip patching objc classes and singletons.  We'll handle those another way
        switch ( patchKind ) {
            case PatchKind::regular:
                break;
            case PatchKind::cfObj2:
                if ( patch->overrideOffsetOfImpl == DylibPatch::singleton )
                    return;
                break;
            case PatchKind::objcClass:
                if ( patch->overrideOffsetOfImpl == DylibPatch::objcClass )
                    return;
                break;
        }

        uintptr_t targetRuntimeAddress = DYLD_BADROOT_MARKER;   // magic value to cause a unique crash if missing symbol in root is used
        if ( patch->overrideOffsetOfImpl != DylibPatch::missingSymbol )
            targetRuntimeAddress = (uintptr_t)(this->loadAddress(state)) + ((intptr_t)patch->overrideOffsetOfImpl);

        dyldCache->forEachPatchableGOTUseOfExport(overriddenDylibIndex, dylibVMOffsetOfImpl,
                                                  ^(uint64_t cacheVMOffset, dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                    bool isWeakImport) {
            // ensure dyld cache __DATA_CONST is writeable
            cacheDataConst.makeWriteable();


            // overridden dylib may not effect this dylib, so only suspend when we find it does effect it
            if ( !suspended ) {
#if !TARGET_OS_EXCLAVEKIT
                state.setVMAccountingSuspending(true);
                suspended = true;
#endif // !TARGET_OS_EXCLAVEKIT
            }

            uintptr_t* loc      = (uintptr_t*)((uint8_t*)dyldCache + cacheVMOffset);
            uintptr_t  newValue = targetRuntimeAddress + (uintptr_t)addend;

            // if client in dyld cache is ok with symbol being missing, set its use to NULL instead of bad-missing-value
            if ( isWeakImport && (targetRuntimeAddress == DYLD_BADROOT_MARKER) )
                newValue = 0;

            // rdar://125168527 (help debug issues with roots missing symbols)
            if ( !isWeakImport && (targetRuntimeAddress == DYLD_BADROOT_MARKER) ) {
                // FIXME: re-enable when we can check all the dylibs using this shared GOT slot have not been rooted
                //const char* overriddenPath = dyldCache->getIndexedImagePath(overriddenDylibIndex);
                //state.log("symbol '%s' missing from root that overrides %s. The shared GOT use of that symbol is being set to 0xBAD4007.\n", exportName, overriddenPath);
            }
#if __has_feature(ptrauth_calls)
            if ( pmd.authenticated && (newValue != 0) ) {
                newValue = dyld3::MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, loc, pmd.usesAddrDiversity, pmd.diversity, pmd.key);
                if ( *loc != newValue ) {
                    *loc = newValue;
                    if ( state.config.log.fixups ) {
                        state.log("cache GOT fixup: *0x%012lX = 0x%012lX (*cache+0x%012lX = %s+0x%012lX) (JOP: diversity=0x%04X, addr-div=%d, key=%s)\n",
                                  (long)loc, newValue, (long)cacheVMOffset,
                                  this->leafName(state), (long)patch->overrideOffsetOfImpl,
                                  pmd.diversity, pmd.usesAddrDiversity,
                                  MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::keyName(pmd.key));
                    }
                }
                return;
            }
#endif
            if ( *loc != newValue ) {
                *loc = newValue;
                if ( state.config.log.fixups )
                    state.log("cache GOT fixup: *0x%012lX = 0x%012lX (*cache+0x%012lX = %s+0x%012lX)\n",
                              (long)loc, (long)newValue,
                              (long)cacheVMOffset,
                              this->leafName(state), (long)patch->overrideOffsetOfImpl);
            }
        });
    });
    // Ensure the end marker is as expected
    assert(cachePatch->overrideOffsetOfImpl == DylibPatch::endOfPatchTable);


    if ( suspended ) {
#if !TARGET_OS_EXCLAVEKIT
        state.setVMAccountingSuspending(false);
#endif // !TARGET_OS_EXCLAVEKIT
    }
}

#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

uint16_t Loader::indexOfUnzipperedTwin(const RuntimeState& state, uint16_t overrideIndex)
{
    if ( state.config.process.catalystRuntime ) {
        // Find the macOS twin overridden index
        if ( const PrebuiltLoaderSet* cachePBLS = state.cachedDylibsPrebuiltLoaderSet() ) {
            const Loader* overridenDylibLdr = cachePBLS->atIndex(overrideIndex);
            if ( const PrebuiltLoader* overridenDylibPBLdr = overridenDylibLdr->isPrebuiltLoader() ) {
                if ( overridenDylibPBLdr->supportsCatalyst )
                    return overridenDylibPBLdr->indexOfTwin;
            }
        } else {
            // We might be running with an invalid version, so can't use Prebuilt loaders
            const char* catalystInstallName = state.config.dyldCache.getIndexedImagePath(overrideIndex);
            if ( strncmp(catalystInstallName, "/System/iOSSupport/", 19) == 0 ) {
                const char* macTwinPath = &catalystInstallName[18];
                uint32_t macDylibCacheIndex;
                if ( state.config.dyldCache.indexOfPath(macTwinPath, macDylibCacheIndex) )
                    return macDylibCacheIndex;
            }
        }
    }

    return kNoUnzipperedTwin;
}

#if !TARGET_OS_EXCLAVEKIT
uint64_t Loader::getOnDiskBinarySliceOffset(RuntimeState& state, const MachOAnalyzer* ma, const char* path)
{
#if BUILDING_DYLD
#if TARGET_OS_OSX && __arm64__
    // these are always thin and sanboxing blocks open()ing them
    if ( strncmp(path, "/usr/libexec/rosetta/", 21) == 0 )
        return 0;
#endif
    __block Diagnostics diag;
    __block uint64_t    sliceOffset = 0;
    state.config.syscall.withReadOnlyMappedFile(diag, path, false, ^(const void* mapping, size_t mappedSize, bool isOSBinary, const FileID& fileID, const char* realPath, int fileDescriptor) {
        if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(mapping) ) {
            ff->forEachSlice(diag, mappedSize, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
                if ( memcmp(ma, sliceStart, 64) == 0 ) {
                    sliceOffset = (uint8_t*)sliceStart - (uint8_t*)mapping;
                    stop = true;
                }
            });
        }
    });
    return sliceOffset;
#else
    // don't record a sliceOffset when the dyld cache builder is run in Mastering because the file may be thinned later
    return 0;
#endif
}
#endif // !TARGET_OS_EXCLAVEKIT

void Loader::recursivelyLogChainToLinksWith(RuntimeState& state, const char* msgPrefix, const Loader* targetLoader, LinksWithChain* start, LinksWithChain* prev, Array<const Loader*>& visited) const
{
    // print if chain starts from main executable or from a dlopen with linkage chain all the way to target of DYLD_PRINT_LINKS_WITH
    //   main: prog.exe -> libfoo -> libbar
    // dlopen: libbaz -> libbar
    if ( this == targetLoader ) {
        state.printLinkageChain(start, msgPrefix);
        return;
    }

    // break cycles
    if ( visited.contains(this) )
        return;
    visited.push_back(this);

    // recursive on each child Loader
    const uint32_t depCount = this->dependentCount();
    for ( uint32_t i = 0; i < depCount; ++i ) {
        LinkedDylibAttributes childAttrs;
        if ( Loader* child = this->dependent(state, i, &childAttrs) ) {
            LinksWithChain chain{nullptr, child, childAttrs};
            prev->next = &chain;
            child->recursivelyLogChainToLinksWith(state, msgPrefix, targetLoader, start, &chain, visited);
            prev->next = nullptr;
        }
    }

}

// rdar://124157900 (add DYLD_PRINT_LINKS_WITH env var to help debug why a particular dylib is being loaded)
void Loader::logChainToLinksWith(RuntimeState& state, const char* msgPrefix) const
{
    const Loader* targetLoader = nullptr;
    for (const Loader* ldr : state.loaded) {
        if ( state.config.log.linksWith == ldr->leafName(state) ) {
            targetLoader = ldr;
            break;
        }
    }
    if ( targetLoader == nullptr ) {
        for (const Loader* ldr : state.delayLoaded) {
            if ( state.config.log.linksWith == ldr->leafName(state) ) {
                targetLoader = ldr;
                break;
            }
        }
    }
    if ( targetLoader != nullptr ) {
        STACK_ALLOC_ARRAY(const Loader*, visited, state.loaded.size()+state.delayLoaded.size());
        LinksWithChain start{nullptr, this};
        this->recursivelyLogChainToLinksWith(state, msgPrefix, targetLoader, &start, &start, visited);
    }
}

} // namespace
