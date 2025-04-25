/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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


#ifndef DyldSharedCache_h
#define DyldSharedCache_h

#include <TargetConditionals.h>
#include <uuid/uuid.h>

#if (BUILDING_LIBDYLD || BUILDING_DYLD)
#include <sys/types.h>
#endif

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#endif

#include "dyld_cache_format.h"
#include "CachePatching.h"
#include "Diagnostics.h"
#include "MachOAnalyzer.h"
#include "Header.h"

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
#include "JSON.h"
#endif

typedef dyld3::MachOFile::PointerMetaData  PointerMetaData;

namespace dyld4 {
    class PrebuiltLoader;
    struct PrebuiltLoaderSet;
}

namespace mach_o {
    struct FunctionVariants;
}

namespace objc_opt {
struct objc_opt_t;
}

namespace objc
{
struct HeaderInfoRO;
struct HeaderInfoRW;
class SelectorHashTable;
class ClassHashTable;
class ProtocolHashTable;
}

struct SwiftOptimizationHeader;

struct VIS_HIDDEN ObjCOptimizationHeader
{
    uint32_t version;
    uint32_t flags;
    uint64_t headerInfoROCacheOffset;
    uint64_t headerInfoRWCacheOffset;
    uint64_t selectorHashTableCacheOffset;
    uint64_t classHashTableCacheOffset;
    uint64_t protocolHashTableCacheOffset;
    uint64_t relativeMethodSelectorBaseAddressOffset;
};

// convenience tuple for tracking file by fs/inode
struct VIS_HIDDEN FileIdTuple
{
                FileIdTuple() { ::bzero(this, sizeof(FileIdTuple)); }
#if !TARGET_OS_EXCLAVEKIT
                FileIdTuple(const struct stat&);
                FileIdTuple(const char* path);
                FileIdTuple(uint64_t fsidScalar, uint64_t fsobjidScalar);
    explicit    operator bool() const;
    bool        operator==(const FileIdTuple& other) const;
    bool        getPath(char pathBuff[PATH_MAX]) const;
    uint64_t    inode() const;
    uint64_t    fsID() const;
#endif

private:
    void        init(const struct stat&);

#if !TARGET_OS_EXCLAVEKIT
    fsid_t      fsid;       // file system
    fsobj_id_t  fsobjid;    // inode within filesystem
#endif // !TARGET_OS_EXCLAVEKIT
};

class VIS_HIDDEN DyldSharedCache
{
public:

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // FIXME: Delete this as its no longer used
    struct FileAlias
    {
        std::string             realPath;
        std::string             aliasPath;
    };

    enum CodeSigningDigestMode
    {
        SHA256only = 0,
        SHA1only   = 1,
        Agile      = 2
    };

    enum class LocalSymbolsMode {
        keep,
        unmap,
        strip
    };

    struct CreateOptions
    {
        std::string                                 outputFilePath;
        std::string                                 outputMapFilePath;
        const dyld3::GradedArchs*                   archs;
        mach_o::Platform                            platform;
        LocalSymbolsMode                            localSymbolMode;
        uint64_t                                    cacheConfiguration;
        bool                                        optimizeDyldDlopens;
        bool                                        optimizeDyldLaunches;
        CodeSigningDigestMode                       codeSigningDigestMode;
        bool                                        dylibsRemovedDuringMastering;
        bool                                        inodesAreSameAsRuntime;
        bool                                        cacheSupportsASLR;
        bool                                        forSimulator;
        bool                                        isLocallyBuiltCache;
        bool                                        verbose;
        bool                                        evictLeafDylibsOnOverflow;
        std::unordered_map<std::string, unsigned>   dylibOrdering;
        std::unordered_map<std::string, unsigned>   dirtyDataSegmentOrdering;
        json::Node                                  objcOptimizations;
        std::string                                 loggingPrefix;
        // Customer and dev caches share a local symbols file.  Only one will get this set to emit the file
        std::string                                 localSymbolsPath;
    };

    struct MappedMachO
    {
                                    MappedMachO()
                                            : mh(nullptr), length(0), isSetUID(false), protectedBySIP(false), sliceFileOffset(0), modTime(0), inode(0) { }
                                    MappedMachO(const std::string& path, const dyld3::MachOAnalyzer* p, size_t l, bool isu, bool sip, uint64_t o, uint64_t m, uint64_t i)
                                            : runtimePath(path), mh(p), length(l), isSetUID(isu), protectedBySIP(sip), sliceFileOffset(o), modTime(m), inode(i) { }

        std::string                 runtimePath;
        const dyld3::MachOAnalyzer* mh;
        size_t                      length;
        uint64_t                    isSetUID        :  1,
                                    protectedBySIP  :  1,
                                    sliceFileOffset : 62;
        uint64_t                    modTime;                // only recorded if inodesAreSameAsRuntime
        uint64_t                    inode;                  // only recorded if inodesAreSameAsRuntime
    };

    //
    // Returns a text "map" file as a big string
    //
    std::string         mapFile() const;

#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

#if !TARGET_OS_EXCLAVEKIT
    //
    // When the dyld cache is mapped from files, there is one region that is dynamically constructed
    //
    class DynamicRegion
    {
    public:
        static DynamicRegion*   make(uintptr_t prefAddress=0);
        void                    free();

        void                    setDyldCacheFileID(FileIdTuple);
        void                    setOSCryptexPath(const char*);
        void                    setCachePath(const char*);
        void                    setReadOnly();
        void                    setSystemWideFlags(__uint128_t);
        void                    setProcessorFlags(__uint128_t);

        bool                    validMagic() const;
        uint32_t                version() const;

        // available in version 0
        bool                    getDyldCacheFileID(FileIdTuple& ids) const;

        // available in version 1
        const char*             osCryptexPath() const;

        // available in version 2
        const char*             cachePath() const;

        // available in version 3
        __uint128_t             getSystemWideFunctionVariantFlags() const;
        __uint128_t             getProcessorFunctionVariantFlags() const;

        static size_t           size();

    private:
                        DynamicRegion();

        static constexpr const char* sMagic = "dyld_data    v3";

        // fields in v0
        char            _magic[16];              // e.g. "dyld_data    v0"
        FileIdTuple     _dyldCache;              // the inode of the main file for this dyld cache

        // fields added in v1
        uint32_t        _osCryptexPathOffset;

        // fields added in v2
        uint32_t        _cachePathOffset;

        // fields added in v3
        uint64_t        _paddingToAlign;
        __uint128_t     _systemWideFunctionVariantFlags;  // system wide function-variant flags set in launchd
        __uint128_t     _processorFunctionVariantFlags;   // arm64 or x86_64 specific function-variant flags
    };


    //
    // Returns the DynamicRegion of the dyld cache
    //
    const DynamicRegion*   dynamicRegion() const;
#endif // !TARGET_OS_EXCLAVEKIT


    //
    // Returns the architecture name of the shared cache, e.g. "arm64"
    //
    const char*         archName() const;


    //
    // Returns the platform the cache is for
    //
    mach_o::Platform    platform() const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImage(void (^handler)(const mach_o::Header* hdr, const char* installName)) const;
    void                forEachDylib(void (^handler)(const mach_o::Header* hdr, const char* installName, uint32_t imageIndex, uint64_t inode, uint64_t mtime, bool& stop)) const;


    //
    // Searches cache for dylib with specified path
    //
    bool                hasImagePath(const char* dylibPath, uint32_t& imageIndex) const;


    //
    // Path is to a dylib in the cache and this is an optimized cache so that path cannot be overridden
    //
    bool                hasNonOverridablePath(const char* dylibPath) const;

    //
    // Check if this shared cache file contains local symbols info
    // Note this might be the .symbols file, in which case this returns true
    // The main cache file in a split cache will return false here.
    // Use hasLocalSymbolsInfoFile() instead to see if a main cache has a .symbols file
    //
    const bool          hasLocalSymbolsInfo() const;


    //
    // Check if this cache file has a reference to a local symbols file
    //
    const bool          hasLocalSymbolsInfoFile() const;

    //
    // Get string name for a given cache type
    //
    static const char*   getCacheTypeName(uint64_t cacheType);

    //
    // Searches cache for dylib with specified mach_header
    //
    bool                findMachHeaderImageIndex(const mach_header* mh, uint32_t& imageIndex) const;

   //
    // Iterates over each dylib in the cache
    //
    void                forEachImageEntry(void (^handler)(const char* path, uint64_t mTime, uint64_t inode)) const;


    //
    // Get image entry from index
    //
    const mach_header*  getIndexedImageEntry(uint32_t index, uint64_t& mTime, uint64_t& node) const;
    const mach_header*  getIndexedImageEntry(uint32_t index) const;


    // iterates over all dylibs and aliases
    void                forEachDylibPath(void (^handler)(const char* dylibPath, uint32_t index)) const;

    //
    // If path is a dylib in the cache, return is mach_header
    //
    const mach_o::Header* getImageFromPath(const char* dylibPath) const;

    //
    // Get image path from index
    //
    const char*         getIndexedImagePath(uint32_t index) const;

    //
    // Get the canonical (dylib) path for a given path, which may be a symlink to something in the cache
    //
    const char*         getCanonicalPath(const char* path) const;

    //
    // Iterates over each text segment in the cache
    //
    void                forEachImageTextSegment(void (^handler)(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName, bool& stop)) const;

    //
    // Returns the dyld_cache_image_text_info[] from the cache header
    //
    std::span<const dyld_cache_image_text_info> textImageSegments() const;

    // Get the path from a dyld_cache_image_text_info
    std::string_view imagePath(const dyld_cache_image_text_info& info) const;

    //
    // Iterates over each of the three regions in the cache
    //
    void                forEachRegion(void (^handler)(const void* content, uint64_t vmAddr, uint64_t size,
                                                      uint32_t initProt, uint32_t maxProt, uint64_t flags,
                                                      bool& stopRegion)) const;


    //
    // Iterates over each of the TPRO regions in the cache
    //
    void                forEachTPRORegion(void (^handler)(const void* content, uint64_t unslidVMAddr, uint64_t vmSize,
                                                          bool& stopRegion)) const;

    //
    // Gets a name for the mapping.
    //
    static const char*  mappingName(uint32_t maxProt, uint64_t flags);

    //
    // Iterates over each of the mappings in the cache and all subCaches
    // After iterating over all mappings, calls the subCache handler if its not-null
    //
    void                forEachRange(void (^mappingHandler)(const char* mappingName,
                                                            uint64_t unslidVMAddr, uint64_t vmSize,
                                                            uint32_t cacheFileIndex, uint64_t fileOffset,
                                                            uint32_t initProt, uint32_t maxProt,
                                                            bool& stopRange),
                                     void (^subCacheHandler)(const DyldSharedCache* subCache, uint32_t cacheFileIndex) = nullptr) const;

    //
    // Iterates over each of the subCaches, including the current cache
    //
    void                forEachCache(void (^handler)(const DyldSharedCache* cache, bool& stopCache)) const;

    //
    // Returns the number of subCache files
    //
    uint32_t            numSubCaches() const;

    //
    // Returns index of subCache containing the address
    //
    int32_t            getSubCacheIndex(const void* addr) const;

    //
    // Gets uuid of the subCache
    //
    void            getSubCacheUuid(uint8_t index, uint8_t uuid[]) const;

    //
    // Returns the vmOffset of the subCache
    //
    uint64_t  getSubCacheVmOffset(uint8_t index) const;

    //
    // Returns the address of the first dyld_cache_image_info in the cache
    //
    const dyld_cache_image_info* images() const;
    
    //
    // Returns the number of images in the cache
    //
    uint32_t imagesCount() const;

    //
    // Get local symbols nlist entries
    //
    static const void*  getLocalNlistEntries(const dyld_cache_local_symbols_info* localInfo);
    const void*         getLocalNlistEntries() const;


    //
    // Get local symbols nlist count
    //
    const uint32_t      getLocalNlistCount() const;


    //
    // Get local symbols strings
    //
    static const char*  getLocalStrings(const dyld_cache_local_symbols_info* localInfo);
    const char*         getLocalStrings() const;


    //
    // Get local symbols strings size
    //
    const uint32_t       getLocalStringsSize() const;


     //
     // Iterates over each local symbol entry in the cache
     //
     void                forEachLocalSymbolEntry(void (^handler)(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop)) const;

    //
    // Returns if an address range is in this cache, and if so if in an immutable area
    //
    bool                inCache(const void* addr, size_t length, bool& immutable) const;

    //
    // Returns true if a path is an alternate path (symlink)
    //
    bool                isAlias(const char* path) const;

    //
    // returns address the cache would load at if unslid
    //
    uint64_t            unslidLoadAddress() const;


    //
    // returns UUID of cache
    //
    void                getUUID(uuid_t uuid) const;


    //
    // returns the vm size required to map cache
    //
    uint64_t            mappedSize() const;

    //
    // Returns the cache PBLS, if one exists
    //
    const dyld4::PrebuiltLoaderSet* dylibsLoaderSet() const;


    //
    // searches cache for PrebuiltLoader for image
    //
    const dyld4::PrebuiltLoader* findPrebuiltLoader(const char* path) const;


    //
    // calculate how much cache was slid when loaded
    //
    intptr_t    slide() const;

   //
    // iterates all pre-built closures for program
    //
    void forEachLaunchLoaderSet(void (^handler)(const char* executableRuntimePath, const dyld4::PrebuiltLoaderSet* pbls)) const;

    //
    // searches cache for PrebuiltLoader for program
    //
    const dyld4::PrebuiltLoaderSet* findLaunchLoaderSet(const char* executablePath) const;

    //
    // searches cache for PrebuiltLoader for program
    //
    const dyld4::PrebuiltLoaderSet* findLaunchLoaderSetWithCDHash(const char* cdHashString) const;


    //
    // Iterates over each of the prewarming data entries
    //
    void                forEachPrewarmingEntry(void (^handler)(const void* content, uint64_t unslidVMAddr, uint64_t vmSize)) const;

    //
    // Iterates over function variant pointers in the dyld cache
    //
    void forEachFunctionVariantPatchLocation(void (^handler)(const void* loc, PointerMetaData pmd, const mach_o::FunctionVariants& fvs, const mach_o::Header* dylibHdr, int variantIndex, bool& stop)) const;

    //
    // searches cache for PrebuiltLoader for program by cdHash
    //
    bool hasLaunchLoaderSetWithCDHash(const char* cdHashString) const;

    //
    // Returns the pointer to the slide info for this cache
    //
    const dyld_cache_slide_info* legacyCacheSlideInfo() const;

    //
    // Returns a pointer to the __DATA region mapping in the cache
    //
    const dyld_cache_mapping_info* legacyCacheDataRegionMapping() const;

    //
    // Returns a pointer to the start of the __DATA region in the cache
    //
    const uint8_t* legacyCacheDataRegionBuffer() const;

    //
    // Returns a pointer to the shared cache optimized Objective-C pointer structures
    //
    const void* objcOptPtrs() const;

    bool                            hasOptimizedObjC() const;
    uint32_t                        objcOptVersion() const;
    uint32_t                        objcOptFlags() const;
    const objc::HeaderInfoRO*       objcHeaderInfoRO() const;
    const objc::HeaderInfoRW*       objcHeaderInfoRW() const;
    const objc::SelectorHashTable*  objcSelectorHashTable() const;
    const objc::ClassHashTable*     objcClassHashTable() const;
    const objc::ProtocolHashTable*  objcProtocolHashTable() const;
    const void*                     objcRelativeMethodListsBaseAddress() const;

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    //
    // In Large Shared Caches, shared cache relative method lists are offsets from the magic
    // selector in libobjc.
    // Returns the VM address of that selector, if it exists
    //
    uint64_t sharedCacheRelativeSelectorBaseVMAddress() const;
#endif

    //
    // Returns a pointer to the shared cache optimized Swift data structures
    //
    const SwiftOptimizationHeader* swiftOpt() const;

    // Returns true if the cache has any slide info, either old style on a single data region
    // or on each individual data mapping
    bool                hasSlideInfo() const;

    void                forEachSlideInfo(void (^handler)(uint64_t mappingStartAddress, uint64_t mappingSize,
                                                         const uint8_t* mappingPagesStart,
                                                         uint64_t slideInfoOffset, uint64_t slideInfoSize,
                                                         const dyld_cache_slide_info* slideInfoHeader)) const;


    //
    // returns true if the offset is in the TEXT of some cached dylib and sets *index to the dylib index
    //
    bool              addressInText(uint64_t cacheOffset, uint32_t* index) const;

    const void*       patchTable() const;

    uint32_t          patchInfoVersion() const;
    uint32_t          patchableExportCount(uint32_t imageIndex) const;
    void              forEachPatchableExport(uint32_t imageIndex,
                                             void (^handler)(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                             PatchKind kind)) const;
#if BUILDING_SHARED_CACHE_UTIL
    void              forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  void (^handler)(uint32_t userImageIndex, uint32_t userVMOffset,
                                                                  dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                  bool isWeakImport)) const;
#endif
    // Use this when you have a root of at imageIndex, and are trying to patch a cached dylib at userImageIndex
    bool              shouldPatchClientOfImage(uint32_t imageIndex, uint32_t userImageIndex) const;
    void              forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl, uint32_t userImageIndex,
                                                         void (^handler)(uint32_t userVMOffset, dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                         bool isWeakImport)) const;
    // Note, use this for weak-defs when you just want all uses of an export, regardless of which dylib they are in.
    void              forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  void (^handler)(uint64_t cacheVMOffset,
                                                                  dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                  bool isWeakImport)) const;
    // Used to walk just the GOT uses of a given export.  The above method will walk both regular and GOT uses
    void              forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                     void (^handler)(uint64_t cacheVMOffset,
                                                                     dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                     bool isWeakImport)) const;

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    // MRM map file generator
    std::string generateJSONMap(const char* disposition, uuid_t cache_uuid, bool verbose) const;

    // This generates a JSON representation of deep reverse dependency information in the cache.
    // For each dylib, the output will contain the list of all the other dylibs transitively
    // dependening on that library. (For example, the entry for libsystem will contain almost
    // all of the dylibs in the cache ; a very high-level framework such as ARKit will have way
    // fewer dependents).
    // This is used by the shared cache ordering script to put "deep" dylibs used by everybody
    // closer to the center of the cache.
    std::string generateJSONDependents() const;
#endif

    // Note these enum entries are only valid for 64-bit archs.
    enum class ConstantClasses {
        cfStringAtomSize = 32
    };

    // Returns the start and size of the range in the shared cache of the ObjC constants, such as
    // all of the CFString's which have been moved in to a contiguous range
    std::pair<const void*, uint64_t> getObjCConstantRange() const;

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    dyld3::MachOAnalyzer::VMAddrConverter makeVMAddrConverter(bool contentRebased) const;
#endif

    // Returns true if the given MachO is in the shared cache range.
    // Returns false if the cache is null.
    static bool inDyldCache(const DyldSharedCache* cache, const dyld3::MachOFile* mf);
    static bool inDyldCache(const DyldSharedCache* cache, const mach_o::Header* header);

    // Returns ture if the given path is a subCache filepath.
    static bool isSubCachePath(const char* leafName);

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    //
    // Apply rebases for manually mapped shared cache
    //
    void applyCacheRebases() const;

    // mmap() an shared cache file read/only but laid out like it would be at runtime
    static const DyldSharedCache* mapCacheFile(const char* path,
                                               uint64_t baseCacheUnslidAddress,
                                               uint8_t* buffer);

    static std::vector<const DyldSharedCache*> mapCacheFiles(const char* path);
#endif

    dyld_cache_header header;

    // The most mappings we could generate.
    // For now its __TEXT, __DATA_CONST, __DATA_DIRTY, __DATA, __LINKEDIT,
    // and optionally also __AUTH, __AUTH_CONST, __AUTH_DIRTY
    static const uint32_t MaxMappings = 8;

private:
    // Returns a variable of type "const T" which corresponds to the header field with the given unslid address
    template<typename T>
    const T getAddrField(uint64_t addr) const;

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    void fillMachOAnalyzersMap(std::unordered_map<std::string,dyld3::MachOAnalyzer*> & dylibAnalyzers) const;
    void computeReverseDependencyMapForDylib(std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap, const std::unordered_map<std::string,dyld3::MachOAnalyzer*> & dylibAnalyzers, const std::string &loadPath) const;
    void computeReverseDependencyMap(std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap) const;
    void findDependentsRecursively(std::unordered_map<std::string, std::set<std::string>> &transitiveDependents, const std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap, std::set<std::string> & visited, const std::string &loadPath) const;
    void computeTransitiveDependents(std::unordered_map<std::string, std::set<std::string>> & transitiveDependents) const;
#endif

    //
    // Returns a pointer to the old shared cache optimized Objective-C data structures
    //
    const objc_opt::objc_opt_t* oldObjcOpt() const;

    //
    // Returns a pointer to the new shared cache optimized Objective-C data structures
    //
    const ObjCOptimizationHeader* objcOpts() const;
};

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

// Manages checking newly built caches against baseline builds
struct BaselineCachesChecker
{
    BaselineCachesChecker(std::vector<const char*> archs, mach_o::Platform platform);

    // Add a baseline cache map to the checker
    mach_o::Error addBaselineMap(std::string_view path);
    mach_o::Error addBaselineMaps(std::string_view dirPath);
    mach_o::Error addNewMap(std::string_view mapString);
    void          setFilesFromNewCaches(std::span<const char* const> files);

    const std::set<std::string>& unionBaselineDylibs() { return _unionBaselineDylibs; }

    std::set<std::string> dylibsMissingFromNewCaches() const;

private:
    // returns if we have a baseline arch for every arch we are building for
    bool    allBaselineArchsPresent() const;

    std::vector<std::string>                                    _archs;
    mach_o::Platform                                            _platform;
    std::set<std::string>                                       _unionBaselineDylibs;
    std::set<std::string>                                       _dylibsInNewCaches;
    std::unordered_map<std::string, std::vector<std::string>>   _baselineDylibs;
    std::unordered_map<std::string, std::set<std::string>>      _newDylibs;
};

#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS


#endif /* DyldSharedCache_h */
