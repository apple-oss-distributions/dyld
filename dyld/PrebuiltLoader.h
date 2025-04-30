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

#ifndef PreBuiltLoader_h
#define PreBuiltLoader_h

#include "Loader.h"
#include "JustInTimeLoader.h"
#include "CachePatching.h"
#include "BumpAllocator.h"

class DyldSharedCache;

//
// PrebuiltLoaders:
//
// dylibs in cache
//    load address is stored in object as offset into dyld cache
//    dependents Loader* are accessed through DepRef
//    state is in r/w byte array statically allocated in cache
//
// binaries in OS (not a dylib in cache)
//    load address is stored in array statically allocated in cache
//    dependents Loader* are accessed through DepRef
//    state is in r/w byte array statically allocated in cache
//
// binaries not in OS (3rd party apps)
//    load address is stored in array allocated by dyld
//    dependents Loader* are accessed through DepRef
//    state is in r/w byte array allocated by dyld
//

// Where do PrebuiltLoaders live?
//   1) in dyld cache
//   2) in third party launch closure

namespace dyld4 {

struct ObjCBinaryInfo;
struct PrebuiltLoaderSet;
class ProcessConfig;
struct PrebuiltObjC;
struct PrebuiltSwift;


// Used to build must-be-missing paths during launch
// By using a vm_allocated buffer, they temp dirty memory can be released
class MissingPaths : public BumpAllocator
{
public:
    void            addPath(const char*);
    void            forEachPath(void (^)(const char* path)) const;
};



class PrebuiltLoader : public Loader
{
public:
    union BindTargetRef {
    public:
                    BindTargetRef(Diagnostics& diag, const RuntimeState& state, const ResolvedSymbol&);
                    BindTargetRef(const BindTarget&);
#if SUPPORT_VM_LAYOUT
        uint64_t    value(RuntimeState&) const;
#endif
        bool        isAbsolute() const   { return (_abs.kind == absolute); }
        bool        isFunctionVariant(uint64_t& fvTableOffset, uint16_t& variantIndex) const;
        LoaderRef   loaderRef() const;
        uint64_t    offset() const;
        const char* loaderLeafName(RuntimeState& state) const;

        // The cache builder can't use value() as that needs a VM layout, but we need to be
        // able to get absolute values to test the ObjC hash tables, which use absolute values for
        // counts and offsets
        uint64_t    absValue() const;

        // This is a hack for the Swift hash tables.  As with absValue(), the cache builder can't
        // use value(), and yet we need to be able to hash BindTargetRef to put them in the tables
        uint64_t    absValueOrOffset() const;

        // To support ObjC, which wants to create pointers to values without symbols,
        // we need to allow creating references to arbitrary locations in the binaries
        static BindTargetRef makeAbsolute(uint64_t value);
    private:
        // To support the make* functions, we allow private constructors for values
        BindTargetRef(uint64_t absoluteValue);
        uint64_t unpackAbsoluteValue() const;

        enum Kind { imageOffset, absolute, imageFunctionVariant };
        struct LoaderAndOffset {
            uint64_t    loaderRef   : 16,
                        high8       :  8,
                        low38       : 38,   // unsigned
                        kind        :  2;
        };
        struct Absolute {
            uint64_t    high8       :  8,
                        low54       : 54,   // signed
                        kind        :  2;
        };
        struct LoaderAndFunctionVariant {
            uint64_t    loaderRef     : 16,
                        variantIndex  : 10,    // which function variant in table
                        fvTableOffset : 36,    // vm offset of FunctionVariantTable from mach_header
                        kind          :  2;
        };
        LoaderAndOffset          _regular;
        LoaderAndFunctionVariant _funcVariant;
        Absolute                 _abs;
        uint64_t                 _raw;
    };
    static_assert(sizeof(BindTargetRef) == 8, "Invalid size");

    uint16_t            pathOffset;
    uint16_t            dependentLoaderRefsArrayOffset; // offset to array of LoaderRef
    uint16_t            dependentKindArrayOffset;       // zero if all deps normal
    uint16_t            fixupsLoadCommandOffset;

    uint16_t            altPathOffset;                  // if install_name does not match real path
    uint16_t            fileValidationOffset;           // zero or offset to FileValidationInfo

    uint16_t            hasInitializers      :  1,
                        isOverridable        :  1,      // if in dyld cache, can roots override it
                        supportsCatalyst     :  1,      // if false, this cannot be used in catalyst process
                        isCatalystOverride   :  1,      // catalyst side of unzippered twin
                        regionsCount         : 12;
    uint16_t            regionsOffset;                  // offset to Region array

    uint16_t            depCount;
    uint16_t            bindTargetRefsOffset;
    uint32_t            bindTargetRefsCount;            // bind targets can be large, so it is last
    // After this point, all offsets in to the PrebuiltLoader need to be 32-bits as the bind targets can be large

    uint32_t            objcBinaryInfoOffset;           // zero or offset to ObjCBinaryInfo
    uint16_t            indexOfTwin;                    // if in dyld cache and part of unzippered twin, then index of the other twin
    uint16_t            reserved1;

    uint64_t            exportsTrieLoaderOffset;
    uint32_t            exportsTrieLoaderSize;
    uint32_t            vmSpace;

    CodeSignatureInFile codeSignature;

    uint32_t            patchTableOffset;

    uint32_t            overrideBindTargetRefsOffset;
    uint32_t            overrideBindTargetRefsCount;

    SectionLocations    sectionLocations;

    // followed by:
    //  path chars
    //  dep kind array
    //  file validation info
    //  segments
    //  bind targets
    //

    // these are the "virtual" methods that override Loader
    const dyld3::MachOFile*     mf(const RuntimeState& state) const;
#if SUPPORT_VM_LAYOUT
    const MachOLoaded*          loadAddress(const RuntimeState& state) const;
#endif
    const char*                 path(const RuntimeState& state) const;
    const char*                 installName(const RuntimeState& state) const;
    uint32_t                    flags() const;
    bool                        contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const;
    bool                        matchesPath(const RuntimeState& state, const char* path) const;
    FileID                      fileID(const RuntimeState& state) const;
    uint32_t                    dependentCount() const;
    Loader*                     dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* kind=nullptr) const;
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


    size_t                      size() const;
    void                        print(RuntimeState& , FILE*, bool printComments) const;
    bool                        recordedCdHashIs(const uint8_t cdHash[20]) const;
    bool                        isValid(const RuntimeState& state) const;
    bool                        isInitialized(const RuntimeState& state) const;
    void                        setFixedUp(const RuntimeState& state) const;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    void                        withCDHash(void (^callback)(const uint8_t cdHash[20])) const;
#endif

#if SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // creating PrebuiltLoaders
    static void                 serialize(Diagnostics& diag, RuntimeState& state, const JustInTimeLoader& jitLoader,
                                          LoaderRef buildRef, CacheWeakDefOverride patcher, PrebuiltObjC& prebuiltObjC,
                                          const PrebuiltSwift& prebuiltSwift, BumpAllocator& allocator);
#endif // SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

    static PrebuiltLoader*      makeCachedDylib(PrebuiltLoaderSet* dyldCacheLoaders, const MachOLoaded* ml, const char* path, size_t& size);

    const ObjCBinaryInfo*       objCBinaryInfo() const;

private:

    friend struct PrebuiltLoaderSet;

                                PrebuiltLoader(const Loader& jitLoader);

    enum class State : uint8_t { unknown=0, beingValidated=1, notMapped=2, mapped=3, mappingDependents=4, dependentsMapped=5, fixedUp=6, delayInitPending=7, beingInitialized=8, initialized=9, invalid=255 };

    // helper functions
    State&                      loaderState(const RuntimeState& state) const;
    void                        map(Diagnostics& diag, RuntimeState& state, const LoadOptions& options, bool parentIsPrebuilt) const;
#if SUPPORT_VM_LAYOUT
    void                        setLoadAddress(RuntimeState& state, const MachOLoaded* ml) const;
#else
    void                        setMF(RuntimeState& state, const dyld3::MachOFile* mf) const;
#endif
    Array<Region>               segments() const;
    const Array<BindTargetRef>  bindTargets() const;
    const Array<BindTargetRef>  overrideBindTargets() const;
    const FileValidationInfo*   fileValidationInfo() const;
    void                        applyObjCFixups(RuntimeState& state) const;
    void                        printObjCFixups(RuntimeState& state, FILE* out) const;
    void                        invalidateInIsolation(const RuntimeState& state) const;
    void                        invalidateShallow(const RuntimeState& state) const;
    void                        recursiveMarkBeingValidated(const RuntimeState& state, bool sharedCacheLoadersAreAlwaysValid) const;
};


//
// A PrebuiltLoaderSet is an mmap()ed read-only data structure which holds a set of PrebuiltLoader objects;
// The contained PrebuiltLoader objects can be found be index O(1) or path O(n).
//
struct PrebuiltLoaderSet
{
    bool                    isValid(RuntimeState& state) const;
    bool                    hasValidMagic() const;
    bool                    validHeader(RuntimeState& state) const;
    size_t                  size() const { return length; }
    size_t                  loaderCount() const { return loadersArrayCount; }
    void                    print(RuntimeState& state, FILE*, bool printComments) const;
    const PrebuiltLoader*   findLoader(const RuntimeState& state, const char* path) const;
    const PrebuiltLoader*   atIndex(uint16_t) const;
    bool                    findIndex(const RuntimeState& state, const char* path, uint16_t& index) const;
    bool                    hasCacheUUID(uint8_t uuid[20]) const;
    const void*             objcSelectorMap() const;
    const void*             objcClassMap() const;
    const void*             objcProtocolMap() const;
    const uint64_t*         swiftTypeProtocolTable() const;
    const uint64_t*         swiftMetadataProtocolTable() const;
    const uint64_t*         swiftForeignTypeProtocolTable() const;
    bool                    hasOptimizedSwift() const;
    void                    logDuplicateObjCClasses(RuntimeState& state) const;
    void                    save() const;
    void                    deallocate() const;
    bool                    contains(const void* p, size_t len) const;

    struct CachePatch
    {
        uint32_t                        cacheDylibIndex;
        uint32_t                        cacheDylibVMOffset;
        PrebuiltLoader::BindTargetRef   patchTo;
    };
    void                    forEachCachePatch(void (^handler)(const CachePatch&)) const;


    static const PrebuiltLoaderSet*   makeLaunchSet(Diagnostics&, RuntimeState&, const MissingPaths&);
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    static const PrebuiltLoaderSet*   makeDyldCachePrebuiltLoaders(Diagnostics& diag, RuntimeState& state, const Array<const Loader*>& jitLoaders);
#endif

private:
    uint32_t    magic;
    uint32_t    versionHash;   // PREBUILTLOADER_VERSION
    uint32_t    length;
    uint32_t    loadersArrayCount;
    uint32_t    loadersArrayOffset;
    uint32_t    cachePatchCount;
    uint32_t    cachePatchOffset;
    uint32_t    dyldCacheUUIDOffset;
    uint32_t    mustBeMissingPathsCount;
    uint32_t    mustBeMissingPathsOffset;
    // ObjC prebuilt data
    uint32_t    objcSelectorHashTableOffset;
    uint32_t    objcClassHashTableOffset;
    uint32_t    objcProtocolHashTableOffset;
    uint32_t    objcFlags;
    uint64_t    objcProtocolClassCacheOffset;
    // Swift prebuilt data
    uint32_t    swiftTypeConformanceTableOffset;
    uint32_t    swiftMetadataConformanceTableOffset;
    uint32_t    swiftForeignTypeConformanceTableOffset;
    uint32_t    padding1;

    // followed by PrebuiltLoader objects

    friend class Loader;
    friend class PrebuiltLoader;

    void                 forEachMustBeMissingPath(void (^)(const char* path, bool& stop)) const;

    static const uint32_t kMagic = 'sp4d';

    enum ObjCFlags : uint32_t {
        noObjCFlags         = 0,
        hasDuplicateClasses = 1 << 0,
    };
};

inline const PrebuiltLoader* PrebuiltLoaderSet::atIndex(uint16_t loaderIndex) const
{
    assert(loaderIndex < loadersArrayCount);
    const uint32_t* loadersOffsetsAray = (uint32_t*)((uint8_t*)this + loadersArrayOffset);
    uint32_t pblOffset = loadersOffsetsAray[loaderIndex];
    return (PrebuiltLoader*)((uint8_t*)this + pblOffset);
}

// Stores information about the layout of the objc sections in a binary, as well as other properties relating to
// the objc information in there.
struct ObjCBinaryInfo {
    // Offset to the __objc_imageinfo section
    uint64_t imageInfoRuntimeOffset                = 0;

    // Offsets to sections containing objc pointers
    uint64_t selRefsRuntimeOffset                  = 0;
    uint64_t classListRuntimeOffset                = 0;
    uint64_t categoryListRuntimeOffset             = 0;
    uint64_t protocolListRuntimeOffset             = 0;
    uint64_t protocolRefsRuntimeOffset             = 0;

    // Counts of the above sections.
    uint32_t selRefsCount                          = 0;
    uint32_t classListCount                        = 0;
    uint32_t categoryCount                         = 0;
    uint32_t protocolListCount                     = 0;
    uint32_t protocolRefsCount                     = 0;

    // Do we have stable Swift fixups to apply to at least one class?
    bool     hasClassStableSwiftFixups             = false;

    // Do we have any pointer-based method lists to set as uniqued?
    bool     hasClassMethodListsToSetUniqued       = false;
    bool     hasCategoryMethodListsToSetUniqued    = false;
    bool     hasProtocolMethodListsToSetUniqued    = false;

    // Do we have any method lists in which to set selector references.
    // Note we only support visiting selector refernces in pointer based method lists
    // Relative method lists should have been verified to always point to __objc_selrefs
    bool     hasClassMethodListsToUnique           = false;
    bool     hasCategoryMethodListsToUnique        = false;
    bool     hasProtocolMethodListsToUnique        = false;

    // Whwn serialized to the PrebuildLoader, these fields will encode other information about
    // the binary.

    // Offset to an array of uint8_t's.  One for each protocol.
    // Note this can be 0 (ie, have no fixups), even if we have protocols.  That would be the case
    // if this binary contains no canonical protocol definitions, ie, all canonical defs are in other binaries
    // or the shared cache.
    uint32_t protocolFixupsOffset                   = 0;
    // Offset to an array of BindTargetRef's.  One for each selector reference to fix up
    // Note we only fix up selector refs in the __objc_selrefs section, and in pointer-based method lists
    uint32_t selectorReferencesFixupsOffset         = 0;
    uint32_t selectorReferencesFixupsCount          = 0;
    // Offset to an array of BindTargetRef's.  One for each protocol reference to fix up
    // Note we only fix up selector refs in the __objc_protorefs section.
    uint32_t protocolReferencesFixupsOffset         = 0;
    uint32_t protocolReferencesFixupsCount          = 0;

    const Array<uint8_t> protocolFixups() const
    {
        return Array<uint8_t>((uint8_t*)((uint8_t*)this+protocolFixupsOffset), protocolListCount, protocolListCount);
    }
    const Array<PrebuiltLoader::BindTargetRef> selectorReferenceFixups() const
    {
        return Array<PrebuiltLoader::BindTargetRef>((PrebuiltLoader::BindTargetRef*)((uint8_t*)this+selectorReferencesFixupsOffset),
                                                    selectorReferencesFixupsCount, selectorReferencesFixupsCount);
    }
    const Array<PrebuiltLoader::BindTargetRef> protocolReferenceFixups() const
    {
        return Array<PrebuiltLoader::BindTargetRef>((PrebuiltLoader::BindTargetRef*)((uint8_t*)this+protocolReferencesFixupsOffset),
                                                    protocolReferencesFixupsCount, protocolReferencesFixupsCount);
    }
};


//
// Have one PrebuiltLoaderSet for all dylibs in the dyld cache
//      getting load address means O(1) indirection through cache header
//      State is kept in byte array in r/w cache
//
// Each OS program has its own PrebuiltLoaderSet
///     os programs are then just like third party apps, except PrebuiltLoaderSet is in cache
//      often means PrebuiltLoaderSet has just one PrebuiltLoader in it
//      dyld cache has a trie of program names that leads to its PrebuiltLoaderSet
//
// dlopen() of dylib not in cache will cause a JustInTimeLoader to be created (no PrebuiltLoader)
//
// For each app PrebuiltLoaderSet (in cache or not)
//      cache builder has pre-allocated r/w State and loadAddress array (but fixed to 32 entries)
//      if app PrebuiltLoaderSet has > 32 entries, dyld malloc()s a new State and loadAddress array

} // namespace dyld4


#endif // PreBuiltLoader_h


