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

#ifndef Loader_h
#define Loader_h

#include <TargetConditionals.h>

#include "Defines.h"
#include "Array.h"
#include "DyldDelegates.h"
#include "DyldProcessConfig.h"

// lsl
#include "AuthenticatedValue.h"
#include "Vector.h"

// mach_o
#include "Header.h"

// For _dyld_section_location_kind.  Note we want our copy, not the copy in the SDK
#include "dyld_priv.h"

#if SUPPORT_VM_LAYOUT
#include "MachOLoaded.h"
#else
#include "MachOFile.h"
#endif

#if BUILDING_CACHE_BUILDER
  #include <string>
  #include <unordered_map>
#endif

class DyldSharedCache;



namespace dyld4 {

using dyld3::MachOLoaded;
using dyld3::MachOAnalyzer;
using dyld3::Array;

typedef dyld3::MachOLoaded::PointerMetaData    PointerMetaData;

class PrebuiltLoader;
class PseudoDylib;
class JustInTimeLoader;
class PremappedLoader;
class RuntimeState;
class DyldCacheDataConstLazyScopedWriter;

struct SectionLocations
{
    uint32_t version = 1;
    uint32_t flags   = 0;

    static constexpr uint32_t count = _dyld_section_location_count;

    uint64_t offsets[count];
    uint64_t sizes[count];
};

//
//  At runtime there is one Loader object for each mach-o image loaded.
//  Loader is an abstract base class.  The three concrete classes
//  instantiated at runtime are PrebuiltLoader, JustInTimeLoader and Premapped.
//  PrebuiltLoader objects mmap()ed in read-only from disk.
//  JustInTimeLoader objects are malloc()ed.
//  PremappedLoader objects are mapped by the exclave core
//
class Loader
{
public:
    struct LoaderRef {
                                LoaderRef(bool appPrebuilt, uint16_t indexInSet) : index(indexInSet), app(appPrebuilt) {}
        const PrebuiltLoader*   loader(const RuntimeState& state) const;

        uint16_t    index       : 15,   // index into PrebuiltLoaderSet
                    app         :  1;   // app vs dyld cache PrebuiltLoaderSet

        bool                    isMissingWeakImage() const { return ((index == 0x7fff) && (app == 0)); }
        static const LoaderRef  missingWeakImage() { return LoaderRef(0, 0x7fff); }
    };

    const uint32_t      magic;                    // kMagic
    const uint16_t      isPrebuilt         :  1,  // PrebuiltLoader vs JustInTimeLoader
                        dylibInDyldCache   :  1,
                        hasObjC            :  1,
                        mayHavePlusLoad    :  1,
                        hasReadOnlyData    :  1,  // __DATA_CONST.  Don't use directly.  Use hasConstantSegmentsToProtect()
                        neverUnload        :  1,  // part of launch or has non-unloadable data (e.g. objc, tlv)
                        leaveMapped        :  1,  // RTLD_NODELETE
                        hasReadOnlyObjC    :  1,  // Has __DATA_CONST,__objc_selrefs section
                        pre2022Binary      :  1,
                        isPremapped        :  1,  // mapped by exclave core
                        hasUUIDLoadCommand :  1,
                        hasWeakDefs        :  1,
                        hasTLVs            :  1,
                        belowLibSystem     :  1,
                        hasFuncVarFixups   :  1,
                        padding            :  1;
    LoaderRef           ref;
    uuid_t              uuid;
    uint32_t            cpusubtype;
    uint32_t            unused;

    enum ExportedSymbolMode { staticLink, shallow, dlsymNext, dlsymSelf };
    enum ResolverMode { runResolver, skipResolver };

    struct LoadChain
    {
        const LoadChain*   previous;
        const Loader*      image;
    };

    struct LoadOptions
    {
        typedef const Loader* (^Finder)(Diagnostics& diag, mach_o::Platform, const char* loadPath, const LoadOptions& options);
        typedef void          (^Missing)(const char* pathNotFound);

        bool        launching           = false;
        bool        staticLinkage       = false;    // did this path come from an LC_LOAD_DYLIB (as opposed to top level dlopen)
        bool        canBeMissing        = false;
        bool        rtldLocal           = false;
        bool        rtldNoDelete        = false;
        bool        rtldNoLoad          = false;
        bool        insertedDylib       = false;
        bool        canBeDylib          = false;
        bool        canBeBundle         = false;
        bool        canBeExecutable     = false;
        bool        forceUnloadable     = false;
        bool        requestorNeedsFallbacks = false;
        LoadChain*  rpathStack          = nullptr;
        Finder      finder              = nullptr;
        Missing     pathNotFoundHandler = nullptr;
    };

    struct ResolvedSymbol {
        enum class Kind { rebase, bindToImage, bindAbsolute };
        const Loader*   targetLoader;
        const char*     targetSymbolName;
        uint64_t        targetRuntimeOffset;
        uintptr_t       targetAddressForDlsym;
        Kind            kind;
        bool            isCode              = false;
        bool            isWeakDef           = false;
        bool            isMissingFlatLazy   = false;
        bool            isMaterializing     = false;
        bool            isFunctionVariant   = false;
        uint16_t        variantIndex        = 0;
    };
    struct BindTarget { const Loader* loader; uint64_t runtimeOffset; };
    typedef mach_o::LinkedDylibAttributes   LinkedDylibAttributes;

    // stored in PrebuiltLoader when it references a file on disk
    struct FileValidationInfo
    {
        uint64_t    sliceOffset;
        uint64_t    deviceID;
        uint64_t    inode;
        uint64_t    mtime;
        uint8_t     cdHash[20];         // to validate file has not changed since PrebuiltLoader was built
        bool        checkInodeMtime;
        bool        checkCdHash;
    };

    // stored in PrebuiltLoaders and generated on the fly by JustInTimeLoaders, passed to mapSegments()
    struct Region
    {
        uint64_t    vmOffset     : 59,
                    perms        :  3,
                    isZeroFill   :  1,
                    readOnlyData :  1;
        uint32_t    fileOffset;
        uint32_t    fileSize;       // mach-o files are limited to 4GB, but zero fill data can be very large
    };

    // Records which binds are to flat-namespace, lazy symbols
    struct MissingFlatLazySymbol
    {
        const char* symbolName;
        uint32_t    bindTargetIndex;
    };

    struct DylibPatch {
        int64_t             overrideOffsetOfImpl;   // this is a signed so that it can reach re-expoted symbols in another dylib

        // We need a few special values for markers.  These shouldn't be valid offsets
        enum : int64_t {
            endOfPatchTable     = -1,
            missingSymbol       = 0,
            objcClass           = 1,
            singleton           = 2
        };
    };

    // these are the "virtual" methods that JustInTimeLoader, PrebuiltLoader and PremappedLoader implement
    const dyld3::MachOFile* mf(const RuntimeState& state) const;
#if SUPPORT_VM_LAYOUT
    const MachOLoaded*      loadAddress(const RuntimeState& state) const;
#endif
    const char*             path(const RuntimeState& state) const;
    const char*             installName(const RuntimeState& state) const;
    bool                    contains(RuntimeState& state, const void* addr, const void** segAddr, uint64_t* segSize, uint8_t* segPerms) const;
    bool                    matchesPath(const RuntimeState& state, const char* path) const;
#if !TARGET_OS_EXCLAVEKIT
    FileID                  fileID(const RuntimeState& state) const;
#endif
    uint32_t                dependentCount() const;
    Loader*                 dependent(const RuntimeState& state, uint32_t depIndex, LinkedDylibAttributes* depAttrs=nullptr) const;
    bool                    hiddenFromFlat(bool forceGlobal=false) const;
    bool                    representsCachedDylibIndex(uint16_t dylibIndex) const;
    bool                    getExportsTrie(uint64_t& runtimeOffset, uint32_t& size) const;
    void                    loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options);
#if SUPPORT_IMAGE_UNLOADING
    void                    unmap(RuntimeState& state, bool force=false) const;
#endif
    typedef std::pair<const Loader*, const char*> PseudoDylibSymbolToMaterialize;
    void                    applyFixups(Diagnostics&, RuntimeState&, DyldCacheDataConstLazyScopedWriter&, bool allowLazyBinds,
                                        lsl::Vector<PseudoDylibSymbolToMaterialize>* materializingSymbols) const;
    bool                    overridesDylibInCache(const DylibPatch*& patchTable, uint16_t& cacheDylibOverriddenIndex) const;
    bool                    dyldDoesObjCFixups() const;
    const SectionLocations* getSectionLocations() const;
    void                    withLayout(Diagnostics &diag, const RuntimeState& state, void (^callback)(const mach_o::Layout &layout)) const;

    // these are private
    bool                    hasBeenFixedUp(RuntimeState&) const;
    bool                    beginInitializers(RuntimeState&);
    void                    runInitializers(RuntimeState&) const;
    bool                    isDelayInit(RuntimeState&) const;
    void                    setDelayInit(RuntimeState&, bool value) const;

    typedef void (^FixUpHandler)(uint64_t fixupLocRuntimeOffset, uint64_t addend, PointerMetaData pmd, const ResolvedSymbol& target, bool& stop);
    typedef void (^CacheWeakDefOverride)(uint32_t cachedDylibIndex, uint32_t cachedDylibVMOffset, const ResolvedSymbol& target);

    // helper functions
    bool                    validMagic() const { return (this->magic == kMagic); }
    const char*             leafName(const RuntimeState&) const;
#if SUPPORT_VM_LAYOUT
    const MachOAnalyzer*    analyzer(const RuntimeState& state) const;
    const mach_o::Header*   header(const RuntimeState& state) const;
#endif
    bool                    hasExportedSymbol(Diagnostics& diag, RuntimeState&, const char* symbolName, ExportedSymbolMode mode,
                                              ResolverMode resolverMode, ResolvedSymbol* result,
                                              dyld3::Array<const Loader*>* searched=nullptr) const;
    uint64_t                selectFromFunctionVariants(Diagnostics& diag, const RuntimeState& state, const char* symbolName, uint32_t fvTableIndex) const;
    void                    logSegmentsFromSharedCache(RuntimeState& state) const;
    bool                    hasConstantSegmentsToProtect() const;
    void                    makeSegmentsReadOnly(RuntimeState& state) const;
    void                    makeSegmentsReadWrite(RuntimeState& state) const;
    ResolvedSymbol          resolveSymbol(Diagnostics& diag, RuntimeState&, int libOrdinal, const char* symbolName, bool weakImport,
                                          bool lazyBind, CacheWeakDefOverride patcher, bool buildingCache=false) const;
    void                    runInitializersBottomUp(RuntimeState&, Array<const Loader*>& danglingUpwards, Array<const Loader*>& visitedDelayed) const;
    void                    runInitializersBottomUpPlusUpwardLinks(RuntimeState&) const;
    void                    findAndRunAllInitializers(RuntimeState&) const;
    bool                    hasMagic() const;
    void                    applyFixupsCheckCachePatching(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const;
    void                    applyCachePatchesToOverride(RuntimeState& state, const Loader* dylibToPatch,
                                                        uint16_t overriddenDylibIndex, const DylibPatch* patches,
                                                        DyldCacheDataConstLazyScopedWriter& cacheDataConst) const;
    void                    applyCachePatchesTo(RuntimeState& state, const Loader* dylibToPatch, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const;
    void                    applyCachePatches(RuntimeState& state, DyldCacheDataConstLazyScopedWriter& cacheDataConst) const;
    void                    applyFixupsGeneric(Diagnostics&, RuntimeState& state, uint64_t sliceOffset, const Array<const void*>& bindTargets,
                                               const Array<const void*>& overrideBindTargets, bool laziesMustBind,
                                               const Array<MissingFlatLazySymbol>& missingFlatLazySymbols) const;
    void                    applyFunctionVariantFixups(Diagnostics& diag, const RuntimeState& state) const;
    void                    forEachBindTarget(Diagnostics& diag, RuntimeState& state,
                                              CacheWeakDefOverride patcher, bool allowLazyBinds,
                                          void (^callback)(const ResolvedSymbol& target, bool& stop),
                                          void (^overrideBindCallback)(const ResolvedSymbol& target, bool& stop)) const;
    const JustInTimeLoader* isJustInTimeLoader() const { return (this->isPrebuilt ? nullptr               : (JustInTimeLoader*)this); };
    const PrebuiltLoader*   isPrebuiltLoader() const   { return (this->isPrebuilt ? (PrebuiltLoader*)this : nullptr); };
    const PremappedLoader*  isPremappedLoader() const   { return (this->isPremapped ? (PremappedLoader*)this : nullptr); };
    void                    getUuidStr(char uuidStr[64]) const;
    void                    logLoad(RuntimeState&, const char* path) const;
    void                    tooNewErrorAddendum(Diagnostics& diag, RuntimeState&) const;
    void                    logChainToLinksWith(RuntimeState& state, const char* msgPrefix) const;
    uint64_t                functionVariantTableVMOffset(const RuntimeState &state) const;

    static uintptr_t        resolvedAddress(RuntimeState& state, const ResolvedSymbol& symbol);

    static void             appendHexNibble(uint8_t value, char*& p);
    static void             appendHexByte(uint8_t value, char*& p);
    static void             uuidToStr(const uuid_t uuid, char  uuidStr[64]);
    static void             applyInterposingToDyldCache(RuntimeState& state);
    static void             adjustFunctionVariantsInDyldCache(RuntimeState& state);

    static uintptr_t        interpose(RuntimeState& state, uintptr_t value, const Loader* forLoader=nullptr);
    static const Loader*    getLoader(Diagnostics& diag, RuntimeState& state, const char* loadPath, const LoadOptions& options);
    static const char*      leafName(const char* path);
    static void             forEachResolvedAtPathVar(RuntimeState& state, const char* loadPath, const LoadOptions& options, ProcessConfig::PathOverrides::Type type, bool& stop,
                                                     void (^handler)(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& stop));
    static void             forEachPath(Diagnostics& diag, RuntimeState& state, const char* requestedPath, const LoadOptions& options,
                                        void (^handler)(const char* possiblePath, ProcessConfig::PathOverrides::Type, bool& stop));

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    static void             addWeakDefsToMap(RuntimeState& state, const std::span<const Loader*>& newLoaders);
#endif
    static bool             expandAndNormalizeAtExecutablePath(const char* mainPath, const char* loadPath, char fixedPath[PATH_MAX]);

    struct LinksWithChain { LinksWithChain* next=nullptr; const Loader* ldr=nullptr; LinkedDylibAttributes attr=LinkedDylibAttributes::regular; };

#if TARGET_OS_EXCLAVEKIT
    static void             setUpExclaveKitSharedCachePageInLinking(RuntimeState& state);
    static void             exclaveKitPageInFixups(void* cbarg_for_segment, uintptr_t address_where_fixed_up_page_will_be_mapped,
                                                   void* __sized_by(size) buf_to_perform_fixups_in, size_t size);
#endif

protected:

    enum { kMagic = 'l4yd' };

    static const uint16_t kNoUnzipperedTwin = 0xFFFF;

    struct InitialOptions
    {
              InitialOptions();
              InitialOptions(const Loader& other);
        bool inDyldCache        = false;
        bool hasObjc            = false;
        bool mayHavePlusLoad    = false;
        bool roData             = false;
        bool neverUnloaded      = false;
        bool leaveMapped        = false;
        bool roObjC             = false;
        bool pre2022Binary      = false;
        bool hasUUID            = false;
        bool hasWeakDefs        = false;
        bool hasTLVs            = false;
        bool belowLibSystem     = false;
        bool hasFuncVarFixups   = false;
   };

    struct CodeSignatureInFile
    {
        uint32_t   fileOffset;
        uint32_t   size;
    };


                               Loader(const InitialOptions& options, bool prebuilt, bool prebuiltApp, bool prebuiltIndex, bool premapped)
                                       : magic(kMagic), isPrebuilt(prebuilt), dylibInDyldCache(options.inDyldCache),
                                         hasObjC(options.hasObjc), mayHavePlusLoad(options.mayHavePlusLoad), hasReadOnlyData(options.roData),
                                         neverUnload(options.neverUnloaded), leaveMapped(options.leaveMapped), hasReadOnlyObjC(options.roObjC),
                                         pre2022Binary(options.pre2022Binary), isPremapped(premapped), hasUUIDLoadCommand(options.hasUUID),
                                         hasWeakDefs(options.hasWeakDefs), hasTLVs(options.hasTLVs), belowLibSystem(options.belowLibSystem),
                                         hasFuncVarFixups(options.hasFuncVarFixups), padding(0),
                                         ref(prebuiltApp, prebuiltIndex),
                                         cpusubtype(0), unused(0) { }

#if TARGET_OS_EXCLAVEKIT
    void                        setUpExclaveKitPageInLinking(Diagnostics& diag, RuntimeState& state, uintptr_t slide, uint64_t sliceOffset, const Array<const void*>& bindTargets) const;
#else
    void                        setUpPageInLinking(Diagnostics& diag, RuntimeState& state, uintptr_t slide, uint64_t sliceOffset, const Array<const void*>& bindTargets) const;
#endif

    void                        recursivelyLogChainToLinksWith(RuntimeState& state, const char* msgPrefx, const Loader* targetLoader, LinksWithChain* start, LinksWithChain* prev, Array<const Loader*>& visited) const;

    static bool                 expandAtLoaderPath(RuntimeState& state, const char* loadPath, const LoadOptions& options, const Loader* ldr, bool fromLCRPATH, char fixedPath[]);
    static bool                 expandAtExecutablePath(RuntimeState& state, const char* loadPath, const LoadOptions& options, bool fromLCRPATH, char fixedPath[]);
    static const Loader*        makePremappedLoader(Diagnostics& diag, RuntimeState& state, const char* path, bool isInDyldCache, uint32_t dylibIndex, const LoadOptions& options, const mach_o::Layout* layout);
    static const Loader*        makeDiskLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options, bool overridesDyldCache, uint32_t dylibIndex,
                                               const mach_o::Layout* layout);
    static const Loader*        makeDyldCacheLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options, uint32_t dylibIndex,
                                                    const mach_o::Layout* layout);

    static const Loader*        makePseudoDylibLoader(Diagnostics& diag, RuntimeState &state, const char* path, const LoadOptions& options, const PseudoDylib* pd);

#if SUPPORT_VM_LAYOUT
    static const MachOAnalyzer* mapSegments(Diagnostics&, RuntimeState&, const char* path, int fd, uint64_t vmSpace,
                                            const CodeSignatureInFile& codeSignature, bool hasCodeSignature,
                                            const Array<Region>& segments, bool neverUnloads, bool prebuilt, const FileValidationInfo&);
#endif

    static uint64_t             validateFile(Diagnostics& diag, const RuntimeState& state, int fd, const char* path,
                                             const Loader::CodeSignatureInFile& codeSignature, const Loader::FileValidationInfo& fileValidation);

    static uint16_t             indexOfUnzipperedTwin(const RuntimeState& state, uint16_t overrideIndex);

    static uint64_t             getOnDiskBinarySliceOffset(RuntimeState& state, const MachOAnalyzer* ma, const char* path);

};

static_assert(sizeof(Loader) == 32, "Invalid size");

#if __has_feature(ptrauth_calls)
    typedef AuthenticatedValue<Loader*> AuthLoader;
    typedef AuthenticatedValue<const Loader*> ConstAuthLoader;
#else
    typedef Loader* AuthLoader;
    typedef const Loader* ConstAuthLoader;
#endif

#if __has_feature(ptrauth_calls)
    typedef AuthenticatedValue<PseudoDylib*> AuthPseudoDylib;
    typedef AuthenticatedValue<const PseudoDylib*> ConstAuthPseudoDylib;
#else
    typedef PseudoDylib* AuthPseudoDylib;
    typedef const PseudoDylib* ConstAuthPseudoDylib;
#endif

}

#endif /* Loader_h */
