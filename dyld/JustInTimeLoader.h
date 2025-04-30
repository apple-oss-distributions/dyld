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

#ifndef JustInTimeLoader_h
#define JustInTimeLoader_h


#include <stdint.h>
#include <TargetConditionals.h>

#include "Defines.h"
#include "Loader.h"
#include "Array.h"
#if SUPPORT_VM_LAYOUT
#include "MachOFile.h"
#else
#include "MachOAnalyzer.h"
#endif

class DyldSharedCache;

namespace dyld4 {
using lsl::UUID;
using dyld3::MachOAnalyzer;

class PrebuiltLoader;
class DyldCacheDataConstLazyScopedWriter;


class JustInTimeLoader : public Loader
{
public:
    // these are the "virtual" methods that override Loader
    const dyld3::MachOFile*     mf(const RuntimeState& state) const;
#if SUPPORT_VM_LAYOUT
    const MachOLoaded*          loadAddress(const RuntimeState&) const;
#endif
    const char*                 path(const RuntimeState& state) const;
    const char*                 installName(const RuntimeState& state) const;
    bool                        contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const;
    bool                        matchesPath(const RuntimeState& state, const char* path) const;
    FileID                      fileID(const RuntimeState& state) const;
    uint32_t                    dependentCount() const;
    Loader*                     dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* depAttrs=nullptr) const;
    bool                        getExportsTrie(uint64_t& runtimeOffset, uint32_t& size) const;
    bool                        hiddenFromFlat(bool forceGlobal) const;
    bool                        representsCachedDylibIndex(uint16_t dylibIndex) const;
    void                        loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options);
#if SUPPORT_IMAGE_UNLOADING
    void                        unmap(RuntimeState& state, bool force=false) const;
#endif
    void                        applyFixups(Diagnostics&, RuntimeState& state, DyldCacheDataConstLazyScopedWriter&, bool allowLazyBinds,
                                            lsl::Vector<PseudoDylibSymbolToMaterialize>* materializingSymbols) const;
    bool                        overridesDylibInCache(const DylibPatch*& patchTable, uint16_t& cacheDylibOverriddenIndex) const;
    bool                        dyldDoesObjCFixups() const;
    const SectionLocations*     getSectionLocations() const;
    void                        withLayout(Diagnostics &diag, const RuntimeState& state, void (^callback)(const mach_o::Layout &layout)) const;
    // these are private "virtual" methods
    bool                        hasBeenFixedUp(RuntimeState&) const;
    bool                        beginInitializers(RuntimeState&);
    void                        runInitializers(RuntimeState&) const;
    bool                        isDelayInit(RuntimeState&) const;
    void                        setDelayInit(RuntimeState&, bool value) const;

    bool                shouldLeaveMapped() const { return this->leaveMapped || this->lateLeaveMapped; }
    void                setLateLeaveMapped() { this->lateLeaveMapped = true; }
    bool                isOverrideOfCachedDylib() const { return overridesCache; }
    const PseudoDylib*  pseudoDylib() const { return pd; }

    FileValidationInfo  getFileValidationInfo(RuntimeState& state) const;
    static void         withRegions(const MachOFile* mf, void (^callback)(const Array<Region>& regions));

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    static void         handleStrongWeakDefOverrides(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst);
#endif

    static void         parseSectionLocations(const mach_o::Header* hdr, SectionLocations& metadata);

    // Wehn patching an iOSMac dylib, we may need an additional patch table for the macOS twin. This returns that patch table
    const DylibPatch*   getCatalystMacTwinPatches() const;


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    static JustInTimeLoader*    makeJustInTimeLoaderDyldCache(RuntimeState& state, const MachOFile* mf, const char* loadPath, uint32_t dylibCacheIndex, const FileID& fileID, bool twin, uint32_t twinIndex,
                                                              const mach_o::Layout* layout);
    static Loader*              makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOFile* mainExe, const char* mainExePath, const mach_o::Layout* layout);
#endif
#if BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_UNIT_TESTS
    static JustInTimeLoader*    makeJustInTimeLoader(RuntimeState& state, const MachOFile* mf, const char* loadPath);
#endif
    static Loader*      makeJustInTimeLoaderDyldCache(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options, uint32_t dylibCacheIndex,
                                                      const mach_o::Layout* layout);
    static Loader*      makeJustInTimeLoaderDisk(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options, bool overridesCache, uint32_t overridesCacheIndex,
                                                 const mach_o::Layout* layout);
    static Loader*      makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOAnalyzer* mainExe, const char* mainExePath,
                                         const mach_o::Layout* layout);

    static const Loader* makePseudoDylibLoader(Diagnostics& diag, RuntimeState &state, const char* path, const LoadOptions& options, const PseudoDylib* pd);

private:


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // The layout of the Mach-O in the cache builder may not match the file layout seen in
    // a MachOFile, or the VM layout in a MachOLoaded.  This pointer, which is required to be set, will give
    // us the layout of the mach-o in the builder
    const mach_o::Layout* nonRuntimeLayout = nullptr;
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

    void                        logFixup(RuntimeState& state, uint64_t fixupLocRuntimeOffset, uintptr_t newValue, PointerMetaData pmd, const Loader::ResolvedSymbol& target) const;
    void                        fixupUsesOfOverriddenDylib(const JustInTimeLoader* overriddenDylib) const;
    static void                 cacheWeakDefFixup(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst,
                                                  uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const ResolvedSymbol& target);
    const DylibPatch*           makePatchTable(RuntimeState& state, uint32_t indexOfOverriddenCachedDylib) const;
    static JustInTimeLoader*    make(RuntimeState& state, const MachOFile* mh, const char* path, const FileID& fileID, uint64_t sliceOffset,
                                     bool willNeverUnload, bool leaveMapped, bool overridesCache, uint16_t dylibIndex, const mach_o::Layout* layout);



protected:
#if SUPPORT_VM_LAYOUT
    const MachOLoaded*          mappedAddress;
    const MachOAnalyzer*        analyzer() const { return (MachOAnalyzer*)mappedAddress; }
#else
    const mach_o::MachOFileRef  mappedAddress;
#endif //SUPPORT_VM_LAYOUT

    mutable uint64_t     pathOffset         : 16,
                         dependentsSet      :  1,
                         fixUpsApplied      :  1,
                         inited             :  1,
                         hidden             :  1,
                         altInstallName     :  1,
                         lateLeaveMapped    :  1,
                         overridesCache     :  1,
                         allDepsAreNormal   :  1,
                         overrideIndex      : 15,
                         depCount           : 16,
                         delayInit          :  1,
                         padding            :  8;
    uint64_t             sliceOffset                    = 0;
    FileID               fileIdent;
    const DylibPatch*    overridePatches                = nullptr;
    const DylibPatch*    overridePatchesCatalystMacTwin = nullptr;
    ConstAuthPseudoDylib pd                             = nullptr;
    uint32_t             exportsTrieRuntimeOffset       = 0;
    uint32_t             exportsTrieSize                = 0;
    SectionLocations     sectionLocations;
    AuthLoader           dependents[1];
    // DependentsKind[]: If allDepsAreNormal is false, then we have an array here too, with 1 entry per dependent


    LinkedDylibAttributes&      dependentAttrs(uint32_t depIndex);
                                JustInTimeLoader(const MachOFile* mh, const Loader::InitialOptions& options, const FileID& fileID, const mach_o::Layout* layout, bool isPremapped);
};

}  // namespace dyld4

#endif // JustInTimeLoader_h





