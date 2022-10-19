/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2003-2010 Apple Inc. All rights reserved.
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

#ifndef _CACHE_PATCHING_H_
#define _CACHE_PATCHING_H_

#include "MachOFile.h"
#include "Types.h"

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "Error.h"

#include <assert.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

//
// MARK: --- V1 patching.  This is for old caches, before Large/Split caches ---
//

struct dyld_cache_patch_info_v1
{
    uint64_t    patchTableArrayAddr;    // (unslid) address of array for dyld_cache_image_patches for each image
    uint64_t    patchTableArrayCount;   // count of patch table entries
    uint64_t    patchExportArrayAddr;   // (unslid) address of array for patch exports for each image
    uint64_t    patchExportArrayCount;  // count of patch exports entries
    uint64_t    patchLocationArrayAddr; // (unslid) address of array for patch locations for each patch
    uint64_t    patchLocationArrayCount;// count of patch location entries
    uint64_t    patchExportNamesAddr;   // blob of strings of export names for patches
    uint64_t    patchExportNamesSize;   // size of string blob of export names for patches
};

struct dyld_cache_image_patches_v1
{
    uint32_t    patchExportsStartIndex;
    uint32_t    patchExportsCount;
};

struct dyld_cache_patchable_export_v1
{
    uint32_t            cacheOffsetOfImpl;
    uint32_t            patchLocationsStartIndex;
    uint32_t            patchLocationsCount;
    uint32_t            exportNameOffset;
};

struct dyld_cache_patchable_location_v1
{
    uint32_t            cacheOffset;
    uint64_t            high7                   : 7,
                        addend                  : 5,    // 0..31
                        authenticated           : 1,
                        usesAddressDiversity    : 1,
                        key                     : 2,
                        discriminator           : 16;

    uint64_t getAddend() const {
        uint64_t unsingedAddend = addend;
        int64_t signedAddend = (int64_t)unsingedAddend;
        signedAddend = (signedAddend << 52) >> 52;
        return (uint64_t)signedAddend;
    }
};

//
// MARK: --- V2 patching.  This is for Large/Split caches and newer ---
//

// Patches can be different kinds.  This lives in the high nibble of the exportNameOffset,
// so we restrict these to 4-bits
enum class PatchKind : uint32_t
{
    // Just a normal patch. Isn't one of ther other kinds
    regular     = 0x0,

    // One of { void* isa, uintptr_t }, from CF
    cfObj2      = 0x1,

    // objc patching was added before this enum exists, in just the high bit
    // of the 4-bit nubble.  This matches that bit layout
    objcClass   = 0x8
};

// This is the base for all v2 and newer info
struct dyld_cache_patch_info
{
    uint32_t    patchTableVersion;              // == 2 or 3 for now
};

struct dyld_cache_patch_info_v2 : dyld_cache_patch_info
{
    uint32_t    patchLocationVersion;           // == 0 for now
    uint64_t    patchTableArrayAddr;            // (unslid) address of array for dyld_cache_image_patches_v2 for each image
    uint64_t    patchTableArrayCount;           // count of patch table entries
    uint64_t    patchImageExportsArrayAddr;     // (unslid) address of array for dyld_cache_image_export_v2 for each image
    uint64_t    patchImageExportsArrayCount;    // count of patch table entries
    uint64_t    patchClientsArrayAddr;          // (unslid) address of array for dyld_cache_image_clients_v2 for each image
    uint64_t    patchClientsArrayCount;         // count of patch clients entries
    uint64_t    patchClientExportsArrayAddr;    // (unslid) address of array for patch exports for each client image
    uint64_t    patchClientExportsArrayCount;   // count of patch exports entries
    uint64_t    patchLocationArrayAddr;         // (unslid) address of array for patch locations for each patch
    uint64_t    patchLocationArrayCount;        // count of patch location entries
    uint64_t    patchExportNamesAddr;           // blob of strings of export names for patches
    uint64_t    patchExportNamesSize;           // size of string blob of export names for patches
};

struct dyld_cache_image_patches_v2
{
    uint32_t    patchClientsStartIndex;
    uint32_t    patchClientsCount;
    uint32_t    patchExportsStartIndex;         // Points to dyld_cache_image_export_v2[]
    uint32_t    patchExportsCount;
};

struct dyld_cache_image_export_v2
{
    uint32_t    dylibOffsetOfImpl;              // Offset from the dylib we used to find a dyld_cache_image_patches_v2
    uint32_t    exportNameOffset : 28;
    uint32_t    patchKind        : 4;           // One of DyldSharedCache::patchKind
};

static_assert(sizeof(dyld_cache_image_export_v2) == 8, "Wrong size");

struct dyld_cache_image_clients_v2
{
    uint32_t    clientDylibIndex;
    uint32_t    patchExportsStartIndex;         // Points to dyld_cache_patchable_export_v2[]
    uint32_t    patchExportsCount;
};

struct dyld_cache_patchable_export_v2
{
    uint32_t    imageExportIndex;               // Points to dyld_cache_image_export_v2
    uint32_t    patchLocationsStartIndex;       // Points to dyld_cache_patchable_location_v2[]
    uint32_t    patchLocationsCount;
};

struct dyld_cache_patchable_location_v2
{
    uint32_t    dylibOffsetOfUse;               // Offset from the dylib we used to get a dyld_cache_image_clients_v2
    uint32_t    high7                   : 7,
                addend                  : 5,    // 0..31
                authenticated           : 1,
                usesAddressDiversity    : 1,
                key                     : 2,
                discriminator           : 16;

    uint64_t getAddend() const {
        uint64_t unsingedAddend = addend;
        int64_t signedAddend = (int64_t)unsingedAddend;
        signedAddend = (signedAddend << 52) >> 52;
        return (uint64_t)signedAddend;
    }
};

//
// MARK: --- V3 patching.  This is V2 plus support for GOT combining ---
//
struct dyld_cache_patch_info_v3 : dyld_cache_patch_info_v2
{
    // uint32_t    patchTableVersion;       // == 3
    // ... other fields from dyld_cache_patch_info_v2
    uint64_t    gotClientsArrayAddr;        // (unslid) address of array for dyld_cache_image_got_clients_v3 for each image
    uint64_t    gotClientsArrayCount;       // count of got clients entries.  Should always match the patchTableArrayCount
    uint64_t    gotClientExportsArrayAddr;  // (unslid) address of array for patch exports for each GOT image
    uint64_t    gotClientExportsArrayCount; // count of patch exports entries
    uint64_t    gotLocationArrayAddr;       // (unslid) address of array for patch locations for each GOT patch
    uint64_t    gotLocationArrayCount;      // count of patch location entries
};

struct dyld_cache_image_got_clients_v3
{
    uint32_t    patchExportsStartIndex;         // Points to dyld_cache_patchable_export_v3[]
    uint32_t    patchExportsCount;
};

struct dyld_cache_patchable_export_v3
{
    uint32_t    imageExportIndex;               // Points to dyld_cache_image_export_v2
    uint32_t    patchLocationsStartIndex;       // Points to dyld_cache_patchable_location_v3[]
    uint32_t    patchLocationsCount;
};

struct dyld_cache_patchable_location_v3
{
    uint64_t    cacheOffsetOfUse;               // Offset from the cache header
    uint32_t    high7                   : 7,
                addend                  : 5,    // 0..31
                authenticated           : 1,
                usesAddressDiversity    : 1,
                key                     : 2,
                discriminator           : 16;

    uint64_t getAddend() const {
        uint64_t unsingedAddend = addend;
        int64_t signedAddend = (int64_t)unsingedAddend;
        signedAddend = (signedAddend << 52) >> 52;
        return (uint64_t)signedAddend;
    }
};

// The base class for patch tables.  Forwards to one of the subclasses, depending on the version
// Note that the version 1 table doesn't use the struct below, as it had a different layout.
struct PatchTable
{
    PatchTable() = default;
    PatchTable(const void* table, uint64_t tableVMAddr)
        : table((const uint8_t*)table), tableVMAddr(tableVMAddr)
    {
    }
    ~PatchTable() = default;
    PatchTable(const PatchTable&) = delete;
    PatchTable& operator=(const PatchTable&) = delete;
    PatchTable(PatchTable&&) = default;
    PatchTable& operator=(PatchTable&&) = default;

    // Returns the version of the patch table.  Clients typically shouldn't need to use this
    // as we should abstract away everything in the forEeach* methods.
    uint32_t version() const;

    // Returns the number of images in the patch table.  There should be 1 patch table image for
    // each shared cache image
    uint64_t numImages() const;

    // For the given image, returns how many exports this image has which need patches
    uint32_t patchableExportCount(uint32_t imageIndex) const;

    // Returns true if userImageIndex uses at least 1 location in imageIndex, ie, needs to be patched
    // if we root imageIndex
    bool imageHasClient(uint32_t imageIndex, uint32_t userImageIndex) const;

    // Walk the exports for the given dylib
    typedef void (^ExportHandler)(uint32_t dylibVMOffsetOfImpl, const char* exportName, PatchKind patchKind);
    void forEachPatchableExport(uint32_t imageIndex, ExportHandler handler) const;

    // Walk all uses of a given export in a given dylib
    typedef void (^ExportUseHandler)(uint32_t userImageIndex, uint32_t userVMOffset,
                                     dyld3::MachOFile::PointerMetaData pmd, uint64_t addend);
    void forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                     ExportUseHandler handler) const;

    typedef void (^ExportUseInImageHandler)(uint32_t userVMOffset,
                                            dyld3::MachOFile::PointerMetaData pmd, uint64_t addend);
    void forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                            uint32_t userImageIndex,
                                            ExportUseInImageHandler handler) const;

    typedef void (^ExportCacheUseHandler)(uint64_t cacheVMOffset,
                                          dyld3::MachOFile::PointerMetaData pmd, uint64_t addend);
    typedef uint64_t (^GetDylibAddressHandler)(uint32_t dylibCacheIndex);
    void forEachPatchableCacheUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                          uint64_t cacheUnslidAddress,
                                          GetDylibAddressHandler getDylibHandler,
                                          ExportCacheUseHandler handler) const;

    typedef void (^GOTUseHandler)(uint64_t cacheVMOffset, dyld3::MachOFile::PointerMetaData pmd,
                                  uint64_t addend);
    void forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                        GOTUseHandler handler) const;

    bool hasValue() const {
        return this->table != nullptr;
    }

    static const char* patchKindName(PatchKind patchKind);

private:
    const dyld_cache_patch_info* info() const;

protected:
    const uint8_t* table    = nullptr;
    uint64_t tableVMAddr    = 0;
};

// Wraps a dyld_cache_patch_info_v2 with various helper methods.  Use PatchTable above to
// dispatch to this automatically
struct PatchTableV2 : PatchTable
{
    // "virtual" methods from PatchTable
    uint64_t numImages() const;
    uint32_t patchableExportCount(uint32_t imageIndex) const;
    bool imageHasClient(uint32_t imageIndex, uint32_t userImageIndex) const;
    void forEachPatchableExport(uint32_t imageIndex, ExportHandler handler) const;
    void forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                     ExportUseHandler handler) const;
    void forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                            uint32_t userImageIndex,
                                            ExportUseInImageHandler handler) const;
    void forEachPatchableCacheUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                          uint64_t cacheUnslidAddress,
                                          GetDylibAddressHandler getDylibHandler,
                                          ExportCacheUseHandler handler) const;
    void forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                        GOTUseHandler handler) const;

private:
    const dyld_cache_patch_info_v2*     info() const;

protected:
    // These are also used by PatchTableV3, so we need them to be protected, not private
    std::span<dyld_cache_image_patches_v2>      images() const;
    std::span<dyld_cache_image_export_v2>       imageExports() const;
    std::span<dyld_cache_image_clients_v2>      imageClients() const;
    std::span<dyld_cache_patchable_export_v2>   clientExports() const;
    std::span<dyld_cache_patchable_location_v2> patchableLocations() const;
    std::string_view                            exportNames() const;

    // An image uses a range of exports from the list of all exports.  This returns just the exports
    // for the given image
    std::span<dyld_cache_image_export_v2>   exportsForImage(uint32_t imageIndex) const;

    // An image uses a range of clients from the list of all clients.  This returns just the clients
    // for the given image
    std::span<dyld_cache_image_clients_v2> clientsForImage(uint32_t imageIndex) const;

    // An image has a list of clients, and clients have a list of exports they use.
    // This returns just exports used by the client in the given image
    std::span<dyld_cache_patchable_export_v2> clientsExportsForImageAndClient(uint32_t imageIndex,
                                                                          uint32_t userImageIndex) const;
};

// Wraps a dyld_cache_patch_info_v3 with various helper methods.  Use PatchTable above to
// dispatch to this automatically
struct PatchTableV3 : PatchTableV2
{
    // "virtual" methods from PatchTable
    using PatchTableV2::numImages;
    using PatchTableV2::patchableExportCount;
    using PatchTableV2::imageHasClient;
    using PatchTableV2::forEachPatchableExport;
    using PatchTableV2::forEachPatchableUseOfExport;
    using PatchTableV2::forEachPatchableUseOfExportInImage;
    using PatchTableV2::forEachPatchableCacheUseOfExport;

    // V2 doesn't have GOT uses, so we need to add our own handler
    void forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                        GOTUseHandler handler) const;

private:
    const dyld_cache_patch_info_v3*             info() const;
    std::span<dyld_cache_image_got_clients_v3>  gotClients() const;
    std::span<dyld_cache_patchable_export_v3>   gotClientExports() const;
    std::span<dyld_cache_patchable_location_v3> gotPatchableLocations() const;
    std::span<dyld_cache_patchable_export_v3>   gotClientExportsForImage(uint32_t imageIndex) const;
};

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
namespace cache_builder
{

struct CacheDylib;

struct dyld_cache_patchable_location
{
    dyld_cache_patchable_location(CacheVMAddress cacheVMAddr, dyld3::MachOFile::PointerMetaData pmd, uint64_t addend);
    ~dyld_cache_patchable_location() = default;
    dyld_cache_patchable_location(const dyld_cache_patchable_location&) = default;
    dyld_cache_patchable_location(dyld_cache_patchable_location&&) = default;
    dyld_cache_patchable_location& operator=(const dyld_cache_patchable_location&) = default;
    dyld_cache_patchable_location& operator=(dyld_cache_patchable_location&&) = default;

    bool operator==(const dyld_cache_patchable_location& other) const = default;

    CacheVMAddress      cacheVMAddr;
    uint64_t            high7                   : 7,
                        addend                  : 5,    // 0..31
                        authenticated           : 1,
                        usesAddressDiversity    : 1,
                        key                     : 2,
                        discriminator           : 16;
};

// There will be one of these PatchInfo structs for each dylib in the cache
struct PatchInfo
{
    struct GOTInfo
    {
        dyld_cache_patchable_location   patchInfo;
        VMOffset                        targetValue;
    };

    std::vector<std::vector<dyld_cache_patchable_location>> bindUses;
    std::vector<std::vector<GOTInfo>>                       bindGOTUses;
    std::vector<std::vector<GOTInfo>>                       bindAuthGOTUses;
    std::vector<std::string>                                bindTargetNames;
};

struct DylibClient
{
    DylibClient(const CacheDylib* clientCacheDylib)
        : clientCacheDylib(clientCacheDylib)
    {
    }
    ~DylibClient()                  = default;
    DylibClient(const DylibClient&) = delete;
    DylibClient(DylibClient&&)      = default;
    DylibClient& operator=(const DylibClient&) = delete;
    DylibClient& operator=(DylibClient&&) = default;

    typedef std::map<CacheVMAddress, std::vector<dyld_cache_patchable_location>, CacheVMAddressLessThan> UsesMap;

    const CacheDylib*   clientCacheDylib = nullptr;
    UsesMap             uses;
};

struct DylibClients
{
    DylibClients() : gotClient(nullptr)
    {
    }
    ~DylibClients()                  = default;
    DylibClients(const DylibClients&) = delete;
    DylibClients(DylibClients&&)      = default;
    DylibClients& operator=(const DylibClients&) = delete;
    DylibClients& operator=(DylibClients&&) = default;

    // Other dylibs which point to this dylib, not via uniqued GOTs
    std::vector<DylibClient>                         clients;

    // For and uniqued GOTs which use this dylib
    DylibClient                                      gotClient;

private:
    std::vector<CacheVMAddress>                      usedExports;

public:

    const std::vector<CacheVMAddress>& getUsedExports() const { return usedExports; }

    // This accepts the new exports by value, so that callers can pass
    // an rvalue reference to avoid an unnecessary copy.
    void setUsedExports(std::vector<CacheVMAddress> newUsedExports) {
        assert(usedExports.empty() && "Used exports should be set only once");

        usedExports = std::move(newUsedExports);

        std::sort(usedExports.begin(), usedExports.end(), CacheVMAddressLessThan());
        usedExports.erase(std::unique(usedExports.begin(), usedExports.end(),
            CacheVMAddressEqual()), usedExports.end());
    }

    decltype(usedExports)::const_iterator findExport(const CacheVMAddress& addr) const {
        auto exportIt = std::lower_bound(usedExports.cbegin(),
                                         usedExports.cend(), addr,
                                         CacheVMAddressLessThan());
        if (exportIt != usedExports.cend() && *exportIt == addr) {
            return exportIt;
        }

        return usedExports.cend();
    }
};

struct PatchTableBuilder
{
    typedef std::unordered_set<CacheVMAddress, CacheVMAddressHash, CacheVMAddressEqual> PatchableClassesSet;
    typedef std::unordered_set<CacheVMAddress, CacheVMAddressHash, CacheVMAddressEqual> PatchableSingletonsSet;

    error::Error    build(const std::span<CacheDylib>& cacheDylibs,
                          const std::span<PatchInfo>& patchInfos,
                          const PatchableClassesSet& patchableObjCClasses,
                          const PatchableSingletonsSet& patchableCFObj2,
                          CacheVMAddress cacheBaseAddress);
    uint64_t        getPatchTableSize() const;
    error::Error    write(uint8_t* buffer, uint64_t bufferSize, uint64_t patchInfoAddr) const;
    
private:
    // Takes the PatchInfo's for each dylib, and merges them in to the data structures needed
    // in the builder
    void mergePatchInfos(const std::span<CacheDylib>& cacheDylibs,
                         const std::span<PatchInfo>& patchInfos);
    
    void        calculateRequiredSpace(const std::span<CacheDylib>& cacheDylibs);
    void        calculatePatchTable(const std::span<CacheDylib>& cacheDylibs,
                                    const PatchableClassesSet& patchableObjCClasses,
                                    const PatchableSingletonsSet& patchableCFObj2,
                                    CacheVMAddress cacheBaseAddress);
    
    typedef std::unordered_map<CacheVMAddress, std::string_view,
                               CacheVMAddressHash, CacheVMAddressEqual> ExportToNameMap;
    
    std::vector<DylibClients>   dylibClients;
    ExportToNameMap             exportsToName;
    
    std::vector<dyld_cache_image_patches_v2>        patchImages;
    std::vector<dyld_cache_image_export_v2>         imageExports;
    std::vector<dyld_cache_image_clients_v2>        patchClients;
    std::vector<dyld_cache_patchable_export_v2>     clientExports;
    std::vector<dyld_cache_patchable_location_v2>   patchLocations;
    std::vector<char>                               patchExportNames;
    std::vector<dyld_cache_image_got_clients_v3>    gotClients;
    std::vector<dyld_cache_patchable_export_v3>     gotClientExports;
    std::vector<dyld_cache_patchable_location_v3>   gotPatchLocations;
    
    const bool                  log = false;
};

} // namespace cache_builder
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS


#endif /* _CACHE_PATCHING_H_ */

