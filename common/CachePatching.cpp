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

#include "CachePatching.h"
#include "Error.h"
#include "Types.h"

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "CacheDylib.h"
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

#include <assert.h>

using error::Error;

//
// MARK: --- PatchTable methods ---
//

const dyld_cache_patch_info* PatchTable::info() const
{
    return (dyld_cache_patch_info*)this->table;
}

uint32_t PatchTable::version() const
{
    return info()->patchTableVersion;
}

uint64_t PatchTable::numImages() const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->numImages();
        case 3:
            return ((PatchTableV3*)this)->numImages();
        default:
            assert("Unknown patch table version");
            break;
    }
    return 0;
}

uint32_t PatchTable::patchableExportCount(uint32_t imageIndex) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->patchableExportCount(imageIndex);
        case 3:
            return ((PatchTableV3*)this)->patchableExportCount(imageIndex);
        default:
            assert("Unknown patch table version");
            break;
    }
    return 0;
}

bool PatchTable::imageHasClient(uint32_t imageIndex, uint32_t userImageIndex) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->imageHasClient(imageIndex, userImageIndex);
        case 3:
            return ((PatchTableV3*)this)->imageHasClient(imageIndex, userImageIndex);
        default:
            assert("Unknown patch table version");
            break;
    }
    return false;
}

void PatchTable::forEachPatchableExport(uint32_t imageIndex, ExportHandler handler) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->forEachPatchableExport(imageIndex, handler);
        case 3:
            return ((PatchTableV3*)this)->forEachPatchableExport(imageIndex, handler);
        default:
            assert("Unknown patch table version");
            break;
    }
}

#if BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL
void PatchTable::forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                             ExportUseHandler handler) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->forEachPatchableUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                      handler);
        case 3:
            return ((PatchTableV3*)this)->forEachPatchableUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                      handler);
        default:
            assert("Unknown patch table version");
            break;
    }
}
#endif

void PatchTable::forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                    uint32_t userImageIndex,
                                                    ExportUseInImageHandler handler) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->forEachPatchableUseOfExportInImage(imageIndex, dylibVMOffsetOfImpl,
                                                                             userImageIndex, handler);
        case 3:
            return ((PatchTableV3*)this)->forEachPatchableUseOfExportInImage(imageIndex, dylibVMOffsetOfImpl,
                                                                             userImageIndex, handler);
        default:
            assert("Unknown patch table version");
            break;
    }
}

void PatchTable::forEachPatchableCacheUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  uint64_t cacheUnslidAddress,
                                                  GetDylibAddressHandler getDylibHandler,
                                                  ExportCacheUseHandler handler) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->forEachPatchableCacheUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                           cacheUnslidAddress,
                                                                           getDylibHandler,
                                                                           handler);
        case 3:
            return ((PatchTableV3*)this)->forEachPatchableCacheUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                           cacheUnslidAddress,
                                                                           getDylibHandler,
                                                                           handler);
        default:
            assert("Unknown patch table version");
            break;
    }
}

void PatchTable::forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                GOTUseHandler handler) const
{
    switch ( this->version() ) {
        case 2:
            return ((PatchTableV2*)this)->forEachPatchableGOTUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                         handler);
        case 3:
            return ((PatchTableV3*)this)->forEachPatchableGOTUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                         handler);
        default:
            assert("Unknown patch table version");
            break;
    }
}

const char* PatchTable::patchKindName(PatchKind patchKind)
{
    const char* name = "(unknown patch kind)";
    switch ( patchKind ) {
        case PatchKind::regular:
            name = "";
            break;
        case PatchKind::cfObj2:
            name = "(CF obj2)";
            break;
        case PatchKind::objcClass:
            name = "(objc class)";
            break;
    }
    return name;
}

//
// MARK: --- PatchTableV2 methods ---
//

uint64_t PatchTableV2::numImages() const
{
    return info()->patchTableArrayCount;
}

const dyld_cache_patch_info_v2* PatchTableV2::info() const
{
    return (dyld_cache_patch_info_v2*)this->table;
}

std::span<dyld_cache_image_patches_v2> PatchTableV2::images() const
{
    uint64_t offset = info()->patchTableArrayAddr - this->tableVMAddr;
    return { (dyld_cache_image_patches_v2*)(this->table + offset), (size_t)info()->patchTableArrayCount };
}

std::span<dyld_cache_image_export_v2> PatchTableV2::imageExports() const
{
    uint64_t offset = info()->patchImageExportsArrayAddr - this->tableVMAddr;
    return { (dyld_cache_image_export_v2*)(this->table + offset), (size_t)info()->patchImageExportsArrayCount };
}

std::span<dyld_cache_image_clients_v2> PatchTableV2::imageClients() const
{
    uint64_t offset = info()->patchClientsArrayAddr - this->tableVMAddr;
    return { (dyld_cache_image_clients_v2*)(this->table + offset), (size_t)info()->patchClientsArrayCount };
}

std::span<dyld_cache_patchable_export_v2> PatchTableV2::clientExports() const
{
    uint64_t offset = info()->patchClientExportsArrayAddr - this->tableVMAddr;
    return { (dyld_cache_patchable_export_v2*)(this->table + offset), (size_t)info()->patchClientExportsArrayCount };
}

std::span<dyld_cache_patchable_location_v2> PatchTableV2::patchableLocations() const
{
    uint64_t offset = info()->patchLocationArrayAddr - this->tableVMAddr;
    return { (dyld_cache_patchable_location_v2*)(this->table + offset), (size_t)info()->patchLocationArrayCount };
}

std::string_view PatchTableV2::exportNames() const
{
    uint64_t offset = info()->patchExportNamesAddr - this->tableVMAddr;
    return { (const char*)(this->table + offset), (size_t)info()->patchExportNamesSize };
}

std::span<dyld_cache_image_export_v2> PatchTableV2::exportsForImage(uint32_t imageIndex) const
{
    std::span<dyld_cache_image_patches_v2> cacheImages = this->images();
    if ( imageIndex >= cacheImages.size() )
        return { };

    auto& image = cacheImages[imageIndex];
    std::span<dyld_cache_image_export_v2> cacheImageExports = this->imageExports();

    // The image uses just a slice of the exports for the whole cache
    return cacheImageExports.subspan(image.patchExportsStartIndex, image.patchExportsCount);
}

std::span<dyld_cache_image_clients_v2> PatchTableV2::clientsForImage(uint32_t imageIndex) const
{
    std::span<dyld_cache_image_patches_v2> cacheImages = this->images();
    if ( imageIndex >= cacheImages.size() )
        return { };

    auto& image = cacheImages[imageIndex];
    std::span<dyld_cache_image_clients_v2> cacheImageClients = this->imageClients();

    // The image uses just a slice of the exports for the whole cache
    return cacheImageClients.subspan(image.patchClientsStartIndex, image.patchClientsCount);
}

std::span<dyld_cache_patchable_export_v2> PatchTableV2::clientsExportsForImageAndClient(uint32_t imageIndex,
                                                                                    uint32_t userImageIndex) const
{
    std::span<dyld_cache_image_clients_v2>      imageClients = this->clientsForImage(imageIndex);
    std::span<dyld_cache_patchable_export_v2>   cacheClientExports = this->clientExports();

    // Each image has a list of clients
    for ( const dyld_cache_image_clients_v2& imageClient : imageClients ) {
        // We only want results from a specific client
        if ( imageClient.clientDylibIndex != userImageIndex )
            continue;

        // Each client has a list of exports from the image
        return cacheClientExports.subspan(imageClient.patchExportsStartIndex,
                                          imageClient.patchExportsCount);
    }

    return { };
}

uint32_t PatchTableV2::patchableExportCount(uint32_t imageIndex) const
{
    std::span<dyld_cache_image_patches_v2> cacheImages = images();
    if ( imageIndex >= cacheImages.size() )
        return 0;

    return cacheImages[imageIndex].patchExportsCount;
}

bool PatchTableV2::imageHasClient(uint32_t imageIndex, uint32_t userImageIndex) const
{
    std::span<dyld_cache_image_clients_v2> imageClients = this->clientsForImage(imageIndex);

    // Each image has a list of clients
    for ( const dyld_cache_image_clients_v2& imageClient : imageClients ) {
        if ( imageClient.clientDylibIndex == userImageIndex )
            return true;
    }
    return false;
}

void PatchTableV2::forEachPatchableExport(uint32_t imageIndex, ExportHandler handler) const
{
    std::span<dyld_cache_image_export_v2> imageExports = this->exportsForImage(imageIndex);
    std::string_view                      cacheExportNames = this->exportNames();
    for ( const dyld_cache_image_export_v2& imageExport : imageExports ) {
        const char* exportName = cacheExportNames.substr(imageExport.exportNameOffset).data();
        handler(imageExport.dylibOffsetOfImpl, exportName, (PatchKind)imageExport.patchKind);
    }
}

// This is extremely inefficient, so only used by tests and cache util
#if BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL
void PatchTableV2::forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                               ExportUseHandler handler) const
{
    std::span<dyld_cache_image_clients_v2>      imageClients = this->clientsForImage(imageIndex);
    std::span<dyld_cache_image_export_v2>       cacheImageExports = this->imageExports();
    std::span<dyld_cache_patchable_export_v2>   cacheClientExports = this->clientExports();
    std::span<dyld_cache_patchable_location_v2> cachePatchableLocations = this->patchableLocations();

    // Each image has a list of clients
    for ( const dyld_cache_image_clients_v2& imageClient : imageClients ) {
        // Each client has a list of exports from the image
        auto exportsForClient = cacheClientExports.subspan(imageClient.patchExportsStartIndex,
                                                           imageClient.patchExportsCount);
        for ( const dyld_cache_patchable_export_v2& clientExport : exportsForClient ) {
            const dyld_cache_image_export_v2& imageExport = cacheImageExports[clientExport.imageExportIndex];

            // Skip exports which aren't the one we are looking for
            if ( imageExport.dylibOffsetOfImpl != dylibVMOffsetOfImpl )
                continue;

            // The client may have multiple locations to patch for the same symbol
            auto patchableLocationsForExport = cachePatchableLocations.subspan(clientExport.patchLocationsStartIndex,
                                                                               clientExport.patchLocationsCount);
            for ( const dyld_cache_patchable_location_v2& loc : patchableLocationsForExport ) {
                dyld3::MachOFile::PointerMetaData pmd;
                pmd.diversity         = loc.discriminator;
                pmd.high8             = loc.high7 << 1;
                pmd.authenticated     = loc.authenticated;
                pmd.key               = loc.key;
                pmd.usesAddrDiversity = loc.usesAddressDiversity;
                handler(imageClient.clientDylibIndex, loc.dylibOffsetOfUse, pmd, loc.getAddend());
            }

            // We found the export, so we're done with this client.  There might be uses in other
            // clients though
            break;
        }
    }
}
#endif // BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL

void PatchTableV2::forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                      uint32_t userImageIndex,
                                                      ExportUseInImageHandler handler) const
{
    std::span<dyld_cache_image_export_v2>       cacheImageExports = this->imageExports();
    std::span<dyld_cache_patchable_location_v2> cachePatchableLocations = this->patchableLocations();

    // Get the exports used by this client in the given image
    std::span<dyld_cache_patchable_export_v2> clientExports = this->clientsExportsForImageAndClient(imageIndex, userImageIndex);
    for ( const dyld_cache_patchable_export_v2& clientExport : clientExports ) {
        const dyld_cache_image_export_v2& imageExport = cacheImageExports[clientExport.imageExportIndex];

        // Skip exports which aren't the one we are looking for
        if ( imageExport.dylibOffsetOfImpl != dylibVMOffsetOfImpl )
            continue;

        // The client may have multiple locations to patch for the same symbol
        auto patchableLocationsForExport = cachePatchableLocations.subspan(clientExport.patchLocationsStartIndex,
                                                                           clientExport.patchLocationsCount);
        for ( const dyld_cache_patchable_location_v2& loc : patchableLocationsForExport ) {
            dyld3::MachOFile::PointerMetaData pmd;
            pmd.diversity         = loc.discriminator;
            pmd.high8             = loc.high7 << 1;
            pmd.authenticated     = loc.authenticated;
            pmd.key               = loc.key;
            pmd.usesAddrDiversity = loc.usesAddressDiversity;
            handler(loc.dylibOffsetOfUse, pmd, loc.getAddend());
        }

        // We found the export, so we're done
        break;
    }
}

void PatchTableV2::forEachPatchableCacheUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                    uint64_t cacheUnslidAddress,
                                                    GetDylibAddressHandler getDylibHandler,
                                                    ExportCacheUseHandler handler) const
{
    std::span<dyld_cache_image_clients_v2>      imageClients = this->clientsForImage(imageIndex);
    std::span<dyld_cache_image_export_v2>       cacheImageExports = this->imageExports();
    std::span<dyld_cache_patchable_export_v2>   cacheClientExports = this->clientExports();
    std::span<dyld_cache_patchable_location_v2> cachePatchableLocations = this->patchableLocations();

    // Each image has a list of clients
    for ( const dyld_cache_image_clients_v2& imageClient : imageClients ) {
        // We need the address of the client to compute cache offsets later
        uint64_t clientUnslidAddress = getDylibHandler(imageClient.clientDylibIndex);

        // Each client has a list of exports from the image
        auto exportsForClient = cacheClientExports.subspan(imageClient.patchExportsStartIndex,
                                                           imageClient.patchExportsCount);

        for ( const dyld_cache_patchable_export_v2& clientExport : exportsForClient ) {
            const dyld_cache_image_export_v2& imageExport = cacheImageExports[clientExport.imageExportIndex];

            // Skip exports which aren't the one we are looking for
            if ( imageExport.dylibOffsetOfImpl != dylibVMOffsetOfImpl )
                continue;

            // The client may have multiple locations to patch for the same symbol
            auto patchableLocationsForExport = cachePatchableLocations.subspan(clientExport.patchLocationsStartIndex,
                                                                               clientExport.patchLocationsCount);
            for ( const dyld_cache_patchable_location_v2& loc : patchableLocationsForExport ) {
                uint64_t cacheOffset = (clientUnslidAddress + loc.dylibOffsetOfUse) - cacheUnslidAddress;
                dyld3::MachOFile::PointerMetaData pmd;
                pmd.diversity         = loc.discriminator;
                pmd.high8             = loc.high7 << 1;
                pmd.authenticated     = loc.authenticated;
                pmd.key               = loc.key;
                pmd.usesAddrDiversity = loc.usesAddressDiversity;
                handler(cacheOffset, pmd, loc.getAddend());
            }
        }
    }
}

void PatchTableV2::forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  GOTUseHandler handler) const
{
    // V2 has no GOT fixups
}

//
// MARK: --- PatchTableV3 methods ---
//

const dyld_cache_patch_info_v3* PatchTableV3::info() const
{
    return (dyld_cache_patch_info_v3*)this->table;
}

std::span<dyld_cache_image_got_clients_v3> PatchTableV3::gotClients() const
{
    uint64_t offset = info()->gotClientsArrayAddr - this->tableVMAddr;
    return { (dyld_cache_image_got_clients_v3*)(this->table + offset), (size_t)info()->gotClientsArrayCount };
}

std::span<dyld_cache_patchable_export_v3> PatchTableV3::gotClientExports() const
{
    uint64_t offset = info()->gotClientExportsArrayAddr - this->tableVMAddr;
    return { (dyld_cache_patchable_export_v3*)(this->table + offset), (size_t)info()->gotClientExportsArrayCount };
}

std::span<dyld_cache_patchable_location_v3> PatchTableV3::gotPatchableLocations() const
{
    uint64_t offset = info()->gotLocationArrayAddr - this->tableVMAddr;
    return { (dyld_cache_patchable_location_v3*)(this->table + offset), (size_t)info()->gotLocationArrayCount };
}

std::span<dyld_cache_patchable_export_v3> PatchTableV3::gotClientExportsForImage(uint32_t imageIndex) const
{
    std::span<dyld_cache_image_got_clients_v3> cacheGOTClients = this->gotClients();
    if ( imageIndex >= cacheGOTClients.size() )
        return { };

    auto& gotClient = cacheGOTClients[imageIndex];
    std::span<dyld_cache_patchable_export_v3> cacheGOTClientExports = this->gotClientExports();

    // The image uses just a slice of the GOT exports for the whole cache
    return cacheGOTClientExports.subspan(gotClient.patchExportsStartIndex, gotClient.patchExportsCount);
}

void PatchTableV3::forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  GOTUseHandler handler) const
{
    auto gotClientExports = this->gotClientExportsForImage(imageIndex);
    auto cacheImageExports = this->imageExports();
    auto cachePatchableLocations = this->gotPatchableLocations();

    if ( gotClientExports.empty() )
        return;

    // Binary search for the dylibOffset we want.  This works because they were sorted in the cache builder
    const dyld_cache_patchable_export_v3* foundClientExport = nullptr;
    int64_t start = 0;
    int64_t end = (int64_t)gotClientExports.size() - 1;
    while ( start <= end ) {
        int64_t i = (start + end) / 2;

        // Get element[i]
        const dyld_cache_patchable_export_v3& clientExport = gotClientExports[(uint32_t)i];
        const dyld_cache_image_export_v2& imageExport = cacheImageExports[clientExport.imageExportIndex];

        if ( imageExport.dylibOffsetOfImpl == dylibVMOffsetOfImpl ) {
            foundClientExport = &clientExport;
            break;
        }

        if ( dylibVMOffsetOfImpl < imageExport.dylibOffsetOfImpl ) {
            end = i-1;
        } else {
            start = i+1;
        }
    }

    if ( foundClientExport == nullptr )
        return;

    // The client may have multiple locations to patch for the same symbol
    auto patchableLocationsForExport = cachePatchableLocations.subspan(foundClientExport->patchLocationsStartIndex,
                                                                       foundClientExport->patchLocationsCount);
    for ( const dyld_cache_patchable_location_v3& loc : patchableLocationsForExport ) {
        dyld3::MachOFile::PointerMetaData pmd;
        pmd.diversity         = loc.discriminator;
        pmd.high8             = loc.high7 << 1;
        pmd.authenticated     = loc.authenticated;
        pmd.key               = loc.key;
        pmd.usesAddrDiversity = loc.usesAddressDiversity;
        handler(loc.cacheOffsetOfUse, pmd, loc.getAddend());
    }
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

namespace cache_builder
{

//
// MARK: --- PatchTableBuilder methods ---
//

void PatchTableBuilder::mergePatchInfos(const std::span<CacheDylib>& cacheDylibs,
                                        const std::span<PatchInfo>& patchInfos)
{
    for ( const CacheDylib& cacheDylib : cacheDylibs ) {
        const PatchInfo& dylibPatchInfo = patchInfos[cacheDylib.cacheIndex];
        assert(cacheDylib.bindTargets.size() == dylibPatchInfo.bindUses.size());
        assert(cacheDylib.bindTargets.size() == dylibPatchInfo.bindTargetNames.size());
        for ( uint32_t bindIndex = 0; bindIndex != cacheDylib.bindTargets.size(); ++bindIndex ) {
            const CacheDylib::BindTarget& bindTarget = cacheDylib.bindTargets[bindIndex];

            // Skip binds with no uses
            const std::vector<dyld_cache_patchable_location>& clientUses = dylibPatchInfo.bindUses[bindIndex];
            if ( clientUses.empty() )
                continue;

            // Skip absolute binds.  Perhaps we should track these, but we lost the information to patch them
            if ( bindTarget.kind == CacheDylib::BindTarget::Kind::absolute )
                continue;

            assert(bindTarget.kind == CacheDylib::BindTarget::Kind::cacheImage);
            const CacheDylib::BindTarget::CacheImage& cacheImageTarget = bindTarget.cacheImage;
            CacheVMAddress                            bindTargetVMAddr = cacheImageTarget.targetDylib->cacheLoadAddress + cacheImageTarget.targetRuntimeOffset;

            // Find the target dylib.  We need to add this dylib as a client of the target
            DylibClients& targetDylibClients = dylibClients[cacheImageTarget.targetDylib->cacheIndex];

            // Add this dylib as a client if its not already there
            if ( targetDylibClients.clients.empty() || (targetDylibClients.clients.back().clientCacheDylib != &cacheDylib) )
                targetDylibClients.clients.emplace_back(&cacheDylib);

            DylibClient&                                targetDylibClient = targetDylibClients.clients.back();
            std::vector<dyld_cache_patchable_location>& uses              = targetDylibClient.uses[bindTargetVMAddr];
            uses.insert(uses.end(), clientUses.begin(), clientUses.end());

            exportsToName.insert({ bindTargetVMAddr, dylibPatchInfo.bindTargetNames[bindIndex] });

            if ( log ) {
                printf("%d patch loc(s) in %s, of symbol %s in %s\n",
                       (uint32_t)clientUses.size(), cacheDylib.installName.data(),
                       dylibPatchInfo.bindTargetNames[bindIndex].c_str(),
                       cacheDylibs[cacheImageTarget.targetDylib->cacheIndex].installName.data());
            }
        }

        // GOTs
        for ( bool auth : { false, true } ) {
            const auto& bindGOTUses = auth ? dylibPatchInfo.bindAuthGOTUses : dylibPatchInfo.bindGOTUses;
            assert(cacheDylib.bindTargets.size() == bindGOTUses.size());
            for ( uint32_t bindIndex = 0; bindIndex != cacheDylib.bindTargets.size(); ++bindIndex ) {
                const CacheDylib::BindTarget& bindTarget = cacheDylib.bindTargets[bindIndex];

                // Skip binds with no uses
                const std::vector<PatchInfo::GOTInfo>& clientUses = bindGOTUses[bindIndex];
                if ( clientUses.empty() )
                    continue;

                // Skip absolute binds.  Perhaps we should track these, but we lost the information to patch them
                if ( bindTarget.kind == CacheDylib::BindTarget::Kind::absolute )
                    continue;

                assert(bindTarget.kind == CacheDylib::BindTarget::Kind::cacheImage);
                const CacheDylib::BindTarget::CacheImage& cacheImageTarget = bindTarget.cacheImage;
                CacheVMAddress                            bindTargetVMAddr = cacheImageTarget.targetDylib->cacheLoadAddress + cacheImageTarget.targetRuntimeOffset;

                // Find the target dylib.  We need to add this dylib as a client of the target
                DylibClients& targetDylibClients = dylibClients[cacheImageTarget.targetDylib->cacheIndex];

                DylibClient&                                targetDylibClient = targetDylibClients.gotClient;
                std::vector<dyld_cache_patchable_location>& uses              = targetDylibClient.uses[bindTargetVMAddr];
                for ( const PatchInfo::GOTInfo& gotInfo : clientUses )
                    uses.push_back(gotInfo.patchInfo);

                exportsToName.insert({ bindTargetVMAddr, dylibPatchInfo.bindTargetNames[bindIndex] });

                if ( log ) {
                    printf("%d patch loc(s) in %s, of symbol %s in %s\n",
                           (uint32_t)clientUses.size(), cacheDylib.installName.data(),
                           dylibPatchInfo.bindTargetNames[bindIndex].c_str(),
                           cacheDylibs[cacheImageTarget.targetDylib->cacheIndex].installName.data());
                }
            }
        }
    }
}

void PatchTableBuilder::calculateRequiredSpace(const std::span<CacheDylib>& cacheDylibs)
{
    // Calculate how much space we need
    uint64_t numPatchImages          = cacheDylibs.size();
    uint64_t numImageExports         = 0;
    uint64_t numPatchClients         = 0;
    uint64_t numClientExports        = 0;
    uint64_t numPatchLocations       = 0;
    uint64_t numPatchExportNameBytes = 0;
    uint64_t numGOTClients           = 0;
    uint64_t numGotClientExports     = 0;
    uint64_t numGotPatchLocations    = 0;

    typedef std::unordered_map<CacheVMAddress, uint32_t, CacheVMAddressHash, CacheVMAddressEqual> ExportNameOffsetMap;
    ExportNameOffsetMap exportNameOffsets;

    for ( uint32_t dylibIndex = 0; dylibIndex != cacheDylibs.size(); ++dylibIndex ) {
        DylibClients& dylibClientData = dylibClients[dylibIndex];
        std::vector<CacheVMAddress> usedExports;

        for ( const DylibClient& clientDylib : dylibClientData.clients ) {
            bool clientUsed = false;
            for ( auto& exportVMAddrAndUses : clientDylib.uses ) {
                CacheVMAddress                                    exportCacheVMAddr = exportVMAddrAndUses.first;
                const std::vector<dyld_cache_patchable_location>& uses              = exportVMAddrAndUses.second;
                if ( uses.empty() )
                    continue;

                // We have uses in this client->location->uses list.  Track them
                clientUsed = true;
                ++numClientExports;
                numPatchLocations += uses.size();

                // Track this location as one the target dylib needs to export
                usedExports.push_back(exportCacheVMAddr);

                // We need space for the name too
                auto itAndInserted = exportNameOffsets.insert({ exportCacheVMAddr, numPatchExportNameBytes });
                if ( itAndInserted.second ) {
                    // We inserted the name, so make space for it
                    // We should have an export already, from the previous scan to size the tables
                    auto exportNameIt = exportsToName.find(exportCacheVMAddr);
                    assert(exportNameIt != exportsToName.end());
                    const std::string_view& exportName = exportNameIt->second;
                    numPatchExportNameBytes += exportName.size() + 1;
                }
            }

            // Make space for this client, if it is used
            if ( clientUsed )
                ++numPatchClients;
        }

        // GOTs
        {
            for ( auto& exportVMAddrAndUses : dylibClientData.gotClient.uses ) {
                CacheVMAddress                              exportCacheVMAddr = exportVMAddrAndUses.first;
                std::vector<dyld_cache_patchable_location>& uses              = exportVMAddrAndUses.second;
                if ( uses.empty() )
                    continue;

                // Many dylibs will all add the same GOT use.  Remove duplicates
                uses.erase(std::unique(uses.begin(), uses.end()), uses.end());

                // We have uses in this client->location->uses list.  Track them
                ++numGotClientExports;
                numGotPatchLocations += uses.size();

                // Track this location as one the target dylib needs to export
                usedExports.push_back(exportCacheVMAddr);

                // We need space for the name too
                auto itAndInserted = exportNameOffsets.insert({ exportCacheVMAddr, numPatchExportNameBytes });
                if ( itAndInserted.second ) {
                    // We inserted the name, so make space for it
                    // We should have an export already, from the previous scan to size the tables
                    auto exportNameIt = exportsToName.find(exportCacheVMAddr);
                    assert(exportNameIt != exportsToName.end());
                    const std::string_view& exportName = exportNameIt->second;
                    numPatchExportNameBytes += exportName.size() + 1;
                }
            }

            // Make space for this GOT client.  We always do this, even if empty
            ++numGOTClients;
        }

        dylibClientData.setUsedExports(std::move(usedExports));

        // Track how many exports this image needs
        numImageExports += dylibClientData.getUsedExports().size();
    }

    // Now reserve the space

    patchImages.reserve(numPatchImages);
    imageExports.reserve(numImageExports);
    patchClients.reserve(numPatchClients);
    clientExports.reserve(numClientExports);
    patchLocations.reserve(numPatchLocations);
    patchExportNames.reserve(numPatchExportNameBytes);
    gotClients.reserve(numGOTClients);
    gotClientExports.reserve(numGotClientExports);
    gotPatchLocations.reserve(numGotPatchLocations);
}

void PatchTableBuilder::calculatePatchTable(const std::span<CacheDylib>& cacheDylibs,
                                            const PatchableClassesSet& patchableObjCClasses,
                                            const PatchableSingletonsSet& patchableCFObj2,
                                            CacheVMAddress cacheBaseAddress)
{
    typedef std::unordered_map<CacheVMAddress, uint32_t, CacheVMAddressHash, CacheVMAddressEqual> ExportNameOffsetMap;
    ExportNameOffsetMap exportNameOffsets;

    for ( uint32_t dylibIndex = 0; dylibIndex != cacheDylibs.size(); ++dylibIndex ) {
        DylibClients& dylibClientData = dylibClients[dylibIndex];

        // Add the patch image which points in to the clients
        // Note we always add 1 patch image for every dylib in the cache, even if
        // it has no other data
        dyld_cache_image_patches_v2 patchImage;
        patchImage.patchClientsStartIndex = (uint32_t)patchClients.size();
        patchImage.patchClientsCount      = 0;
        patchImage.patchExportsStartIndex = (uint32_t)imageExports.size();
        patchImage.patchExportsCount      = (uint32_t)dylibClientData.getUsedExports().size();

        // Add regular clients
        for ( const DylibClient& clientDylib : dylibClientData.clients ) {
            bool clientUsed = false;

            CacheVMAddress clientDylibVMAddr = clientDylib.clientCacheDylib->cacheLoadAddress;

            // We might add a client.  If we do, then set it up now so that we have the
            // right offset to the exports table
            dyld_cache_image_clients_v2 clientImage;
            clientImage.clientDylibIndex       = clientDylib.clientCacheDylib->cacheIndex;
            clientImage.patchExportsStartIndex = (uint32_t)clientExports.size();
            clientImage.patchExportsCount      = 0;

            for ( auto& exportVMAddrAndUses : clientDylib.uses ) {
                CacheVMAddress exportCacheVMAddr = exportVMAddrAndUses.first;

                const std::vector<dyld_cache_patchable_location>& uses = exportVMAddrAndUses.second;
                if ( uses.empty() )
                    continue;

                // We have uses in this client->location->uses list.  Track them
                clientUsed = true;

                // We should have an export already, from the previous scan to size the tables
                auto exportIt = dylibClientData.findExport(exportCacheVMAddr);
                assert(exportIt != dylibClientData.getUsedExports().end());

                uint32_t imageExportIndex = (uint32_t)std::distance(dylibClientData.getUsedExports().cbegin(), exportIt);

                // Add an export for this client dylib
                dyld_cache_patchable_export_v2 cacheExport;
                cacheExport.imageExportIndex         = patchImage.patchExportsStartIndex + imageExportIndex;
                cacheExport.patchLocationsStartIndex = (uint32_t)patchLocations.size();
                cacheExport.patchLocationsCount      = (uint32_t)uses.size();
                clientExports.push_back(cacheExport);
                ++clientImage.patchExportsCount;

                // Now add the list of locations.
                // At this point we need to translate from the locations the cache recorded to what we encode
                for ( const dyld_cache_patchable_location& use : uses ) {
                    dyld_cache_patchable_location_v2 loc;
                    loc.dylibOffsetOfUse     = (uint32_t)(use.cacheVMAddr - clientDylibVMAddr).rawValue();
                    loc.high7                = use.high7;
                    loc.addend               = use.addend;
                    loc.authenticated        = use.authenticated;
                    loc.usesAddressDiversity = use.usesAddressDiversity;
                    loc.key                  = use.key;
                    loc.discriminator        = use.discriminator;
                    patchLocations.push_back(loc);
                }
            }

            // Add the client to the table, if its used
            if ( clientUsed ) {
                ++patchImage.patchClientsCount;
                patchClients.push_back(clientImage);
            }
        }

        // Add GOT clients
        {
            dyld_cache_image_got_clients_v3 gotClient;
            gotClient.patchExportsStartIndex   = (uint32_t)gotClientExports.size();
            gotClient.patchExportsCount        = 0;

            for ( auto& exportVMAddrAndUses : dylibClientData.gotClient.uses ) {
                CacheVMAddress exportCacheVMAddr = exportVMAddrAndUses.first;

                const std::vector<dyld_cache_patchable_location>& uses = exportVMAddrAndUses.second;
                if ( uses.empty() )
                    continue;

                // We should have an export already, from the previous scan to size the tables
                auto exportIt = dylibClientData.findExport(exportCacheVMAddr);
                assert(exportIt != dylibClientData.getUsedExports().end());

                uint32_t imageExportIndex = (uint32_t)std::distance(dylibClientData.getUsedExports().cbegin(), exportIt);

                // Add an export for this GOT client
                dyld_cache_patchable_export_v3 cacheExport;
                cacheExport.imageExportIndex            = patchImage.patchExportsStartIndex + imageExportIndex;
                cacheExport.patchLocationsStartIndex    = (uint32_t)gotPatchLocations.size();
                cacheExport.patchLocationsCount         = (uint32_t)uses.size();
                gotClientExports.push_back(cacheExport);
                ++gotClient.patchExportsCount;

                // Now add the list of locations.
                // At this point we need to translate from the locations the cache recorded to what we encode
                for (const dyld_cache_patchable_location& use : uses) {
                    dyld_cache_patchable_location_v3 loc;
                    loc.cacheOffsetOfUse            = (use.cacheVMAddr - cacheBaseAddress).rawValue();
                    loc.high7                       = use.high7;
                    loc.addend                      = use.addend;
                    loc.authenticated               = use.authenticated;
                    loc.usesAddressDiversity        = use.usesAddressDiversity;
                    loc.key                         = use.key;
                    loc.discriminator               = use.discriminator;
                    gotPatchLocations.push_back(loc);
                }
            }

            // Add the GOT to the table, even if unused
            gotClients.push_back(gotClient);
        }

        const CacheDylib& cacheDylib       = cacheDylibs[dylibIndex];
        CacheVMAddress    imageBaseAddress = cacheDylib.cacheLoadAddress;

        // Add all the exports for this image
        for ( const CacheVMAddress exportCacheVMAddr : dylibClientData.getUsedExports() ) {
            // Add the name, if no-one else has
            uint32_t exportNameOffset  = 0;
            auto     nameItAndInserted = exportNameOffsets.insert({ exportCacheVMAddr, (uint32_t)patchExportNames.size() });
            if ( nameItAndInserted.second ) {
                // We inserted the name, so make space for it
                const std::string_view& exportName = exportsToName[exportCacheVMAddr];
                patchExportNames.insert(patchExportNames.end(), &exportName[0], &exportName[0] + exportName.size() + 1);
                exportNameOffset = nameItAndInserted.first->second;
            }
            else {
                // The name already existed.  Use the offset from earlier
                exportNameOffset = nameItAndInserted.first->second;
            }


            PatchKind patchKind = PatchKind::regular;
            if ( patchableObjCClasses.count(exportCacheVMAddr) ) {
                patchKind = PatchKind::objcClass;
            }
            else if ( patchableCFObj2.count(exportCacheVMAddr) ) {
                patchKind = PatchKind::cfObj2;
            }

            dyld_cache_image_export_v2 imageExport;
            imageExport.dylibOffsetOfImpl = (uint32_t)(exportCacheVMAddr - imageBaseAddress).rawValue();
            imageExport.exportNameOffset  = (uint32_t)exportNameOffset;
            imageExport.patchKind         = (uint32_t)patchKind;
            imageExports.push_back(imageExport);
        }

        patchImages.push_back(patchImage);
    }

    while ( (patchExportNames.size() % 4) != 0 )
        patchExportNames.push_back('\0');
}

uint64_t PatchTableBuilder::getPatchTableSize() const
{
    uint64_t patchInfoSize = sizeof(dyld_cache_patch_info_v3);
    patchInfoSize += sizeof(dyld_cache_image_patches_v2) * patchImages.size();
    patchInfoSize += sizeof(dyld_cache_image_export_v2) * imageExports.size();
    patchInfoSize += sizeof(dyld_cache_image_clients_v2) * patchClients.size();
    patchInfoSize += sizeof(dyld_cache_patchable_export_v2) * clientExports.size();
    patchInfoSize += sizeof(dyld_cache_patchable_location_v2) * patchLocations.size();
    patchInfoSize += sizeof(dyld_cache_image_got_clients_v3) * gotClients.size();
    patchInfoSize += sizeof(dyld_cache_patchable_export_v3) * gotClientExports.size();
    patchInfoSize += sizeof(dyld_cache_patchable_location_v3) * gotPatchLocations.size();
    patchInfoSize += patchExportNames.size();
    
#if 0
    fprintf(stderr, "sizeof(dyld_cache_patch_info_v3): %lu\n", sizeof(dyld_cache_patch_info_v3));
    fprintf(stderr, "sizeof(dyld_cache_image_patches_v2) * patchImages.size(): %lu with %lu uses\n", sizeof(dyld_cache_image_patches_v2) * patchImages.size(), patchImages.size());
    fprintf(stderr, "sizeof(dyld_cache_image_export_v2) * imageExports.size(): %lu with %lu uses\n", sizeof(dyld_cache_image_export_v2) * imageExports.size(), imageExports.size());
    fprintf(stderr, "sizeof(dyld_cache_image_clients_v2) * patchClients.size(): %lu with %lu uses\n", sizeof(dyld_cache_image_clients_v2) * patchClients.size(), patchClients.size());
    fprintf(stderr, "sizeof(dyld_cache_patchable_export_v2) * clientExports.size(): %lu with %lu uses\n", sizeof(dyld_cache_patchable_export_v2) * clientExports.size(), clientExports.size());
    fprintf(stderr, "sizeof(dyld_cache_patchable_location_v2) * patchLocations.size(): %lu with %lu uses\n", sizeof(dyld_cache_patchable_location_v2) * patchLocations.size(), patchLocations.size());
    fprintf(stderr, "sizeof(dyld_cache_image_got_clients_v3) * gotClients.size(): %lu with %lu uses\n", sizeof(dyld_cache_image_got_clients_v3) * gotClients.size(), gotClients.size());
    fprintf(stderr, "sizeof(dyld_cache_patchable_export_v3) * gotClientExports.size(): %lu with %lu uses\n", sizeof(dyld_cache_patchable_export_v3) * gotClientExports.size(), gotClientExports.size());
    fprintf(stderr, "sizeof(dyld_cache_patchable_location_v3) * gotPatchLocations.size(): %lu with %lu uses\n", sizeof(dyld_cache_patchable_location_v3) * gotPatchLocations.size(), gotPatchLocations.size());
    fprintf(stderr, "patchExportNames.size(): %lu\n", patchExportNames.size());
    fprintf(stderr, "patchInfoSize: %lld\n", patchInfoSize);
#endif
    
    return patchInfoSize;
}

Error PatchTableBuilder::write(uint8_t* buffer, uint64_t bufferSize,
                               uint64_t patchInfoAddr) const
{
    // check for fit
    uint64_t patchInfoSize = this->getPatchTableSize();
    if ( patchInfoSize > bufferSize ) {
        return Error("cache buffer too small to hold patch table (buffer size=%lldMB, patch size=%lluKB)",
                     bufferSize / 1024 / 1024, patchInfoSize / 1024);
    }
    
    dyld_cache_patch_info_v3 patchInfo;
    patchInfo.patchTableVersion             = 3;
    patchInfo.patchLocationVersion          = 0;
    patchInfo.patchTableArrayAddr           = patchInfoAddr + sizeof(dyld_cache_patch_info_v3);
    patchInfo.patchTableArrayCount          = patchImages.size();
    patchInfo.patchImageExportsArrayAddr    = patchInfo.patchTableArrayAddr + (patchInfo.patchTableArrayCount * sizeof(dyld_cache_image_patches_v2));
    patchInfo.patchImageExportsArrayCount   = imageExports.size();
    patchInfo.patchClientsArrayAddr         = patchInfo.patchImageExportsArrayAddr + (patchInfo.patchImageExportsArrayCount * sizeof(dyld_cache_image_export_v2));
    patchInfo.patchClientsArrayCount        = patchClients.size();
    patchInfo.patchClientExportsArrayAddr   = patchInfo.patchClientsArrayAddr + (patchInfo.patchClientsArrayCount * sizeof(dyld_cache_image_clients_v2));
    patchInfo.patchClientExportsArrayCount  = clientExports.size();
    patchInfo.patchLocationArrayAddr        = patchInfo.patchClientExportsArrayAddr + (patchInfo.patchClientExportsArrayCount * sizeof(dyld_cache_patchable_export_v2));
    patchInfo.patchLocationArrayCount       = patchLocations.size();
    patchInfo.gotClientsArrayAddr           = patchInfo.patchLocationArrayAddr + (patchInfo.patchLocationArrayCount * sizeof(dyld_cache_patchable_location_v2));
    patchInfo.gotClientsArrayCount          = gotClients.size();
    patchInfo.gotClientExportsArrayAddr     = patchInfo.gotClientsArrayAddr + (patchInfo.gotClientsArrayCount * sizeof(dyld_cache_image_got_clients_v3));
    patchInfo.gotClientExportsArrayCount    = gotClientExports.size();
    patchInfo.gotLocationArrayAddr          = patchInfo.gotClientExportsArrayAddr + (patchInfo.gotClientExportsArrayCount * sizeof(dyld_cache_patchable_export_v3));
    patchInfo.gotLocationArrayCount         = gotPatchLocations.size();
    patchInfo.patchExportNamesAddr          = patchInfo.gotLocationArrayAddr + (patchInfo.gotLocationArrayCount * sizeof(dyld_cache_patchable_location_v3));
    patchInfo.patchExportNamesSize          = patchExportNames.size();

    // (dylib, client) patch table
    ::memcpy(buffer + patchInfoAddr - patchInfoAddr, &patchInfo, sizeof(dyld_cache_patch_info_v3));
    ::memcpy(buffer + patchInfo.patchTableArrayAddr - patchInfoAddr, &patchImages[0], sizeof(patchImages[0]) * patchImages.size());
    ::memcpy(buffer + patchInfo.patchImageExportsArrayAddr - patchInfoAddr, &imageExports[0], sizeof(imageExports[0]) * imageExports.size());
    ::memcpy(buffer + patchInfo.patchClientsArrayAddr - patchInfoAddr, &patchClients[0], sizeof(patchClients[0]) * patchClients.size());
    ::memcpy(buffer + patchInfo.patchClientExportsArrayAddr - patchInfoAddr, &clientExports[0], sizeof(clientExports[0]) * clientExports.size());
    ::memcpy(buffer + patchInfo.patchLocationArrayAddr - patchInfoAddr, &patchLocations[0], sizeof(patchLocations[0]) * patchLocations.size());

    // GOT patch table
    ::memcpy(buffer + patchInfo.gotClientsArrayAddr - patchInfoAddr, &gotClients[0], sizeof(gotClients[0]) * gotClients.size());
    ::memcpy(buffer + patchInfo.gotClientExportsArrayAddr - patchInfoAddr, &gotClientExports[0], sizeof(gotClientExports[0]) * gotClientExports.size());
    ::memcpy(buffer + patchInfo.gotLocationArrayAddr - patchInfoAddr, &gotPatchLocations[0], sizeof(gotPatchLocations[0]) * gotPatchLocations.size());

    // Shared export names
    ::memcpy(buffer + patchInfo.patchExportNamesAddr - patchInfoAddr, &patchExportNames[0], patchExportNames.size());
    
    return Error();
}

Error PatchTableBuilder::build(const std::span<CacheDylib>& cacheDylibs,
                               const std::span<PatchInfo>& patchInfos,
                               const PatchableClassesSet& patchableObjCClasses,
                               const PatchableSingletonsSet& patchableCFObj2,
                               CacheVMAddress cacheBaseAddress)
{
    if ( cacheDylibs.size() != patchInfos.size() ) {
        return Error("Mismatch in patch table inputs: %lld vs %lld",
                     (uint64_t)cacheDylibs.size(), (uint64_t)patchInfos.size());
    }

    // Each dylib has a list of its uses of each bindTarget in its array.  We now need to combine those in
    // to the list of uses of each exported symbol from each dylib
    this->dylibClients.resize(cacheDylibs.size());
    this->mergePatchInfos(cacheDylibs, patchInfos);

    // We now have everything in the state we want, ie, each dylib has a list of who uses it.
    // That is the form the patch table uses on-disk
    this->calculateRequiredSpace(cacheDylibs);
    this->calculatePatchTable(cacheDylibs, patchableObjCClasses, patchableCFObj2, cacheBaseAddress);

    return Error();
}

//
// MARK: --- dyld_cache_patchable_location methods ---
//

dyld_cache_patchable_location::dyld_cache_patchable_location(CacheVMAddress cacheVMAddr,
                                                             dyld3::MachOFile::PointerMetaData pmd,
                                                             uint64_t addend)
{
    this->cacheVMAddr          = cacheVMAddr;
    this->high7                = pmd.high8 >> 1;
    this->addend               = addend;
    this->authenticated        = pmd.authenticated;
    this->usesAddressDiversity = pmd.usesAddrDiversity;
    this->key                  = pmd.key;
    this->discriminator        = pmd.diversity;
    // check for truncations
    assert(this->addend == addend);
    assert((this->high7 << 1) == pmd.high8);
}

} // namespace cache_builder

#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
