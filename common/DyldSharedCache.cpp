/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/fsgetpath.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/dyld_priv.h>
#include <assert.h>
#include <unistd.h>
#include <dlfcn.h>
#endif // !TARGET_OS_EXCLAVEKIT

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "FileUtils.h"
#endif

#define NO_ULEB
#include "MachOLoaded.h"
#include "DyldSharedCache.h"
#include "Header.h"
#include "Trie.hpp"
#include "StringUtils.h"
#if !TARGET_OS_EXCLAVEKIT
#include "PrebuiltLoader.h"
#include "OptimizerSwift.h"
#include "ClosureFileSystemPhysical.h"
#endif

#include "objc-shared-cache.h"

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
#include "JSONWriter.h"
#include <sstream>
#endif

using dyld3::MachOFile;
using dyld3::MachOLoaded;
using dyld3::MachOAnalyzer;
using dyld4::PrebuiltLoader;
using dyld4::PrebuiltLoaderSet;

using mach_o::Header;
using mach_o::Platform;


void DyldSharedCache::getUUID(uuid_t uuid) const
{
    memcpy(uuid, header.uuid, sizeof(uuid_t));
}

uint32_t DyldSharedCache::numSubCaches() const {
    // We may or may not be followed by sub caches.
    if ( header.mappingOffset <= offsetof(dyld_cache_header, subCacheArrayCount) )
        return 0;

    return header.subCacheArrayCount;
}

intptr_t DyldSharedCache::slide() const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    return (intptr_t)this - (intptr_t)(mappings[0].address);
}

void DyldSharedCache::forEachPrewarmingEntry(void (^handler)(const void* content, uint64_t unslidVMAddr, uint64_t vmSize)) const
{
    if ( header.mappingOffset <= offsetof(dyld_cache_header, prewarmingDataSize) )
        return;


    const dyld_prewarming_header* prewarmingHeader = (const dyld_prewarming_header*)((char*)this + header.prewarmingDataOffset);
    if ( prewarmingHeader->version != 1 )
        return;

    const dyld_prewarming_entry* firstEntry = &prewarmingHeader->entries[0];

    const uint64_t baseAddress = this->unslidLoadAddress();
    for ( const dyld_prewarming_entry& entry : std::span(firstEntry, prewarmingHeader->count) ) {
        handler((const uint8_t*)this + entry.cacheVMOffset, baseAddress + entry.cacheVMOffset,
                entry.numPages * DYLD_CACHE_PREWARMING_DATA_PAGE_SIZE);
    }
}

const char* DyldSharedCache::mappingName(uint32_t maxProt, uint64_t flags)
{
    if ( maxProt & VM_PROT_EXECUTE ) {
        if ( flags & DYLD_CACHE_MAPPING_TEXT_STUBS ) {
            return "__TEXT_STUBS";
        } else {
            return "__TEXT";
        }
    } else if ( maxProt & VM_PROT_WRITE ) {
        if ( flags & DYLD_CACHE_MAPPING_AUTH_DATA ) {
            if ( flags & DYLD_CACHE_MAPPING_DIRTY_DATA )
                return "__AUTH_DIRTY";
            else if ( flags & DYLD_CACHE_MAPPING_CONST_TPRO_DATA )
                return "__AUTH_TPRO_CONST";
            else if ( flags & DYLD_CACHE_MAPPING_CONST_DATA )
                return "__AUTH_CONST";
            else
                return "__AUTH";
        } else {
            if ( flags & DYLD_CACHE_MAPPING_DIRTY_DATA )
                return "__DATA_DIRTY";
            else if ( flags & DYLD_CACHE_MAPPING_CONST_TPRO_DATA )
                return "__TPRO_CONST";
            else if ( flags & DYLD_CACHE_MAPPING_CONST_DATA )
                return "__DATA_CONST";
            else
                return "__DATA";
        }
    }
    else if ( maxProt & VM_PROT_READ ) {
        if ( flags & DYLD_CACHE_READ_ONLY_DATA )
            return "__READ_ONLY";
        else
            return "__LINKEDIT";
    } else {
        return "*unknown*";
    }
    return "";
}

uint64_t DyldSharedCache::getSubCacheVmOffset(uint8_t index) const {
    if (header.mappingOffset <= offsetof(dyld_cache_header, cacheSubType) ) {
        const dyld_subcache_entry_v1* subCacheEntries = (dyld_subcache_entry_v1*)((uintptr_t)this + header.subCacheArrayOffset);
        return subCacheEntries[index].cacheVMOffset;
    } else {
        const dyld_subcache_entry* subCacheEntries = (dyld_subcache_entry*)((uintptr_t)this + header.subCacheArrayOffset);
        return subCacheEntries[index].cacheVMOffset;
    }
}

void DyldSharedCache::forEachRegion(void (^handler)(const void* content, uint64_t vmAddr, uint64_t size,
                                                    uint32_t initProt, uint32_t maxProt, uint64_t flags,
                                                    bool& stopRegion)) const
{
    // <rdar://problem/49875993> sanity check cache header
    if ( strncmp(header.magic, "dyld_v1", 7) != 0 )
        return;
    if ( header.mappingOffset > 1024 )
        return;
    if ( header.mappingCount > 20 )
        return;
    if ( header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset) ) {
        const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
        const dyld_cache_mapping_info* mappingsEnd = &mappings[header.mappingCount];
        for (const dyld_cache_mapping_info* m=mappings; m < mappingsEnd; ++m) {
            bool stop = false;
            handler((char*)this + m->fileOffset, m->address, m->size, m->initProt, m->maxProt, 0, stop);
            if ( stop )
                return;
        }
    } else {
        const dyld_cache_mapping_and_slide_info* mappings = (const dyld_cache_mapping_and_slide_info*)((char*)this + header.mappingWithSlideOffset);
        const dyld_cache_mapping_and_slide_info* mappingsEnd = &mappings[header.mappingCount];
        uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);
        for (const dyld_cache_mapping_and_slide_info* m=mappings; m < mappingsEnd; ++m) {
            bool stop = false;
            // this is only called with a mapped dyld cache.  That means to get content,
            // we cannot use fileoffset, but intead us vmAddr + slide
            const void* content = (void*)(m->address + slide);
            handler(content, m->address, m->size, m->initProt, m->maxProt, m->flags, stop);
            if ( stop )
                return;
        }
    }
}

void DyldSharedCache::forEachCache(void (^handler)(const DyldSharedCache *cache, bool& stopCache)) const
{
    // Always start with the current file
    bool stop = false;
    handler(this, stop);
    if ( stop )
        return;

    // We may or may not be followed by sub caches.
    if ( header.mappingOffset <= offsetof(dyld_cache_header, subCacheArrayCount) )
        return;

    for (uint32_t i = 0; i != header.subCacheArrayCount; ++i) {
        const DyldSharedCache* cache = (const DyldSharedCache*)((uintptr_t)this + this->getSubCacheVmOffset(i));
        handler(cache, stop);
        if ( stop )
            return;
    }
}

void DyldSharedCache::forEachRange(void (^handler)(const char* mappingName,
                                                   uint64_t unslidVMAddr, uint64_t vmSize,
                                                   uint32_t cacheFileIndex, uint64_t fileOffset,
                                                   uint32_t initProt, uint32_t maxProt,
                                                   bool& stopRange),
                                   void (^subCacheHandler)(const DyldSharedCache* subCache, uint32_t cacheFileIndex)) const
{
    __block uint32_t cacheFileIndex = 0;
    forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
        cache->forEachRegion(^(const void *content, uint64_t unslidVMAddr, uint64_t size,
                        uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
            const char* mappingName = DyldSharedCache::mappingName(maxProt, flags);
            uint64_t fileOffset = (uint8_t*)content - (uint8_t*)cache;
            bool stop = false;
            handler(mappingName, unslidVMAddr, size, cacheFileIndex, fileOffset, initProt, maxProt, stop);
            if ( stop ) {
                stopRegion = true;
                stopCache = true;
                return;
            }
        });

        if ( stopCache )
            return;

        if ( subCacheHandler != nullptr )
            subCacheHandler(cache, cacheFileIndex);

        ++cacheFileIndex;
    });
}

void DyldSharedCache::forEachDylib(void (^handler)(const Header* mh, const char* installName, uint32_t imageIndex, uint64_t inode, uint64_t mtime, bool& stop)) const
{
    const dyld_cache_image_info*   dylibs   = (dyld_cache_image_info*)((char*)this + header.imagesOffset);
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return;
    uint64_t firstImageOffset = 0;
    uint64_t firstRegionAddress = mappings[0].address;
    for (uint32_t i=0; i < header.imagesCount; ++i) {
        uint64_t offset = dylibs[i].address - firstRegionAddress;
        if ( firstImageOffset == 0 )
            firstImageOffset = offset;
        const char* dylibPath  = (char*)this + dylibs[i].pathFileOffset;
        const mach_header* mh = (mach_header*)((char*)this + offset);
        bool stop = false;
        handler((const Header*)mh, dylibPath, i, dylibs[i].inode, dylibs[i].modTime, stop);
        if ( stop )
            break;
    }
}

std::span<const dyld_cache_image_text_info> DyldSharedCache::textImageSegments() const
{
    // check for old cache without imagesText array
    if ( (header.mappingOffset <= offsetof(dyld_cache_header, imagesTextOffset)) || (header.imagesTextCount == 0) )
        return { };

    const dyld_cache_image_text_info* imagesText = (dyld_cache_image_text_info*)((char*)this + header.imagesTextOffset);
    const dyld_cache_image_text_info* imagesTextEnd = &imagesText[header.imagesTextCount];
    return { imagesText, imagesTextEnd };
}

void DyldSharedCache::forEachImageTextSegment(void (^handler)(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName, bool& stop)) const
{
    for (const dyld_cache_image_text_info& p : this->textImageSegments() ) {
        bool stop = false;
        handler(p.loadAddress, p.textSegmentSize, p.uuid, (char*)this + p.pathOffset, stop);
        if ( stop )
            break;
    }
}

std::string_view DyldSharedCache::imagePath(const dyld_cache_image_text_info& info) const
{
    return (char*)this + info.pathOffset;
}

uint64_t DyldSharedCache::unslidLoadAddress() const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    return mappings[0].address;
}

uint32_t DyldSharedCache::imagesCount() const {
    if ( header.mappingOffset >= offsetof(dyld_cache_header, imagesCount) ) {
        return header.imagesCount;
    }
    return header.imagesCountOld;
}

const dyld_cache_image_info* DyldSharedCache::images() const {
    if ( header.mappingOffset >= offsetof(dyld_cache_header, imagesCount) ) {
        return (dyld_cache_image_info*)((char*)this + header.imagesOffset);
    }
    return (dyld_cache_image_info*)((char*)this + header.imagesOffsetOld);
}

bool DyldSharedCache::hasImagePath(const char* dylibPath, uint32_t& imageIndex) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return false;
    if ( header.mappingOffset >= 0x118 ) {
        uintptr_t      slide           = (uintptr_t)this - (uintptr_t)(mappings[0].address);
        const uint8_t* dylibTrieStart  = (uint8_t*)(this->header.dylibsTrieAddr + slide);
        const uint8_t* dylibTrieEnd    = dylibTrieStart + this->header.dylibsTrieSize;

        Diagnostics diag;
        const uint8_t* imageNode = dyld3::MachOLoaded::trieWalk(diag, dylibTrieStart, dylibTrieEnd, dylibPath);
        if ( imageNode != NULL ) {
            imageIndex = (uint32_t)dyld3::MachOFile::read_uleb128(diag, imageNode, dylibTrieEnd);
            return true;
        }
    }
    else {
        const dyld_cache_image_info* dylibs = images();
        uint64_t firstImageOffset = 0;
        uint64_t firstRegionAddress = mappings[0].address;
        for (uint32_t i=0; i < imagesCount(); ++i) {
            const char* aPath  = (char*)this + dylibs[i].pathFileOffset;
            if ( strcmp(aPath, dylibPath) == 0 ) {
                imageIndex = i;
                return true;
            }
            uint64_t offset = dylibs[i].address - firstRegionAddress;
            if ( firstImageOffset == 0 )
                firstImageOffset = offset;
            // skip over aliases.  This is no longer valid in newer caches.  They store aliases only in the trie
#if 0
            if ( dylibs[i].pathFileOffset < firstImageOffset)
                continue;
#endif
        }
    }

    return false;
}

const mach_header* DyldSharedCache::getIndexedImageEntry(uint32_t index, uint64_t& mTime, uint64_t& inode) const
{
    const dyld_cache_image_info*   dylibs   = images();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((uintptr_t)this + header.mappingOffset);
    mTime = dylibs[index].modTime;
    inode = dylibs[index].inode;
    return (mach_header*)((uintptr_t)this + dylibs[index].address - mappings[0].address);
}

const mach_header* DyldSharedCache::getIndexedImageEntry(uint32_t index) const
{
    uint64_t mTime = 0;
    uint64_t inode = 0;
    return this->getIndexedImageEntry(index, mTime, inode);
}

const char* DyldSharedCache::getIndexedImagePath(uint32_t index) const
{
   auto dylibs = images();
   return (char*)this + dylibs[index].pathFileOffset;
}


const mach_o::Header* DyldSharedCache::getImageFromPath(const char* dylibPath) const
{
    const dyld_cache_image_info*   dylibs   = images();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uint32_t dyldCacheImageIndex;
    if ( hasImagePath(dylibPath, dyldCacheImageIndex) )
        return (mach_o::Header*)((uintptr_t)this + dylibs[dyldCacheImageIndex].address - mappings[0].address);
    return nullptr;
}

uint64_t DyldSharedCache::mappedSize() const
{
    // If we have sub caches, then the cache header itself tells us how much space we need to cover all caches
    if ( header.mappingOffset >= offsetof(dyld_cache_header, subCacheArrayCount) ) {
        return header.sharedRegionSize;
    } else {
        __block uint64_t startAddr = 0;
        __block uint64_t endAddr = 0;
        forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size,
                        uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
            if ( startAddr == 0 )
                startAddr = vmAddr;
            uint64_t end = vmAddr+size;
            if ( end > endAddr )
                endAddr = end;
        });
        return (endAddr - startAddr);
    }
}

bool DyldSharedCache::inDyldCache(const DyldSharedCache* cache, const dyld3::MachOFile* mf) {
    return inDyldCache(cache, (const mach_o::Header*)mf);
}

bool DyldSharedCache::inDyldCache(const DyldSharedCache* cache, const mach_o::Header* header) {
    return header->inDyldCache() && (cache != nullptr) && ((uintptr_t)header >= (uintptr_t)cache) && ((uintptr_t)header < ((uintptr_t)cache + cache->mappedSize()));
}

#if BUILDING_CACHE_BUILDER
const objc_opt::objc_opt_t* DyldSharedCache::oldObjcOpt() const
{
    return nullptr;
}
#else
const objc_opt::objc_opt_t* DyldSharedCache::oldObjcOpt() const
{
    // Find the objc image
    __block const Header* objcHdr = nullptr;
    uint32_t imageIndex;
    if ( this->hasImagePath("/usr/lib/libobjc.A.dylib", imageIndex) ) {
        uint64_t mTime;
        uint64_t inode;
        objcHdr = (const Header*)(this->getIndexedImageEntry(imageIndex, mTime, inode));
    }

    if ( objcHdr == nullptr )
        return nullptr;

    // If we found the objc image, then try to find the read-only data inside.
    __block const objc_opt::objc_opt_t* objcROContent = nullptr;
    int64_t slide = objcHdr->getSlide();
    objcHdr->forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( info.segmentName != "__TEXT" )
            return;
        if ( info.sectionName != "__objc_opt_ro" )
            return;
        objcROContent = (objc_opt::objc_opt_t*)(info.address + slide);
    });
    if ( objcROContent == nullptr )
        return nullptr;

    // FIXME: We should fix this once objc and dyld are both in-sync with Large Caches changes
    if ( objcROContent->version == objc_opt::VERSION || (objcROContent->version == 15) )
        return objcROContent;

    return nullptr;
}
#endif

const ObjCOptimizationHeader* DyldSharedCache::objcOpts() const
{
    if ( header.mappingOffset <= offsetof(dyld_cache_header, objcOptsSize) )
        return nullptr;

    return (const ObjCOptimizationHeader*)((char*)this + header.objcOptsOffset);
}

const objc::HeaderInfoRO* DyldSharedCache::objcHeaderInfoRO() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->headerInfoROCacheOffset != 0 )
            return (const objc::HeaderInfoRO*)((char*)this + opts->headerInfoROCacheOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return (const objc::HeaderInfoRO*)opts->headeropt_ro();
    return nullptr;
}

const objc::HeaderInfoRW* DyldSharedCache::objcHeaderInfoRW() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->headerInfoRWCacheOffset != 0 )
            return (const objc::HeaderInfoRW*)((char*)this + opts->headerInfoRWCacheOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return (const objc::HeaderInfoRW*)opts->headeropt_rw();
    return nullptr;
}

const objc::ClassHashTable* DyldSharedCache::objcClassHashTable() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->classHashTableCacheOffset != 0 )
            return (const objc::ClassHashTable*)((char*)this + opts->classHashTableCacheOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->classOpt();
    return nullptr;
}

const objc::SelectorHashTable* DyldSharedCache::objcSelectorHashTable() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->selectorHashTableCacheOffset != 0 )
            return (const objc::SelectorHashTable*)((char*)this + opts->selectorHashTableCacheOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->selectorOpt();
    return nullptr;
}

const objc::ProtocolHashTable* DyldSharedCache::objcProtocolHashTable() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->protocolHashTableCacheOffset != 0 )
            return (const objc::ProtocolHashTable*)((char*)this + opts->protocolHashTableCacheOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->protocolOpt();
    return nullptr;
}


const SwiftOptimizationHeader* DyldSharedCache::swiftOpt() const {
    // check for old cache without imagesArray
    if ( header.mappingOffset <= offsetof(dyld_cache_header, swiftOptsSize) )
        return nullptr;

    if ( header.swiftOptsOffset == 0 )
        return nullptr;

    SwiftOptimizationHeader* optHeader = (SwiftOptimizationHeader*)((char*)this + header.swiftOptsOffset);
    return optHeader;
}

template<typename T>
const T DyldSharedCache::getAddrField(uint64_t addr) const {
    uint64_t slide = (uint64_t)this - unslidLoadAddress();
    return (const T)(addr + slide);
}

const void* DyldSharedCache::patchTable() const
{
    return getAddrField<const void*>(header.patchInfoAddr);
}

uint32_t DyldSharedCache::patchInfoVersion() const {
    if ( header.mappingOffset <= offsetof(dyld_cache_header, swiftOptsSize) ) {
        return 1;
    }

    const dyld_cache_patch_info_v2* patchInfo = getAddrField<dyld_cache_patch_info_v2*>(header.patchInfoAddr);
    return patchInfo->patchTableVersion;
}

void DyldSharedCache::forEachPatchableGOTUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                     void (^handler)(uint64_t cacheVMOffset,
                                                                     MachOFile::PointerMetaData pmd,
                                                                     uint64_t addend,
                                                                     bool isWeakImport)) const {
    if ( header.patchInfoAddr == 0 )
        return;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  Only V3 has GOT patching
        return;
    }

    // V3 and newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    patchTable.forEachPatchableGOTUseOfExport(imageIndex, dylibVMOffsetOfImpl, handler);
}

void DyldSharedCache::forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  void (^handler)(uint64_t cacheVMOffset,
                                                                  MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                  bool isWeakImport)) const {
    if ( header.patchInfoAddr == 0 )
        return;

    uint32_t patchVersion = patchInfoVersion();

    // Get GOT patches if we have them
    this->forEachPatchableGOTUseOfExport(imageIndex, dylibVMOffsetOfImpl, handler);

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs

        // This patch table uses cache offsets, so convert from "image + offset" to cache offset
        uint64_t mTime;
        uint64_t inode;
        const dyld3::MachOAnalyzer* imageMA = (dyld3::MachOAnalyzer*)(this->getIndexedImageEntry(imageIndex, mTime, inode));
        if ( imageMA == nullptr )
            return;

        uint64_t cacheUnslidAddress = unslidLoadAddress();
        uint32_t cacheOffsetOfImpl = (uint32_t)((((const Header*)imageMA)->preferredLoadAddress() - cacheUnslidAddress) + dylibVMOffsetOfImpl);

        // Loading a new cache so get the data from the cache header
        const dyld_cache_patch_info_v1* patchInfo = getAddrField<dyld_cache_patch_info_v1*>(header.patchInfoAddr);
        const dyld_cache_image_patches_v1* patchArray = getAddrField<dyld_cache_image_patches_v1*>(patchInfo->patchTableArrayAddr);
        if (imageIndex > patchInfo->patchTableArrayCount)
            return;
        const dyld_cache_image_patches_v1& patch = patchArray[imageIndex];
        if ( (patch.patchExportsStartIndex + patch.patchExportsCount) > patchInfo->patchExportArrayCount )
            return;
        const dyld_cache_patchable_export_v1* patchExports = getAddrField<dyld_cache_patchable_export_v1*>(patchInfo->patchExportArrayAddr);
        const dyld_cache_patchable_location_v1* patchLocations = getAddrField<dyld_cache_patchable_location_v1*>(patchInfo->patchLocationArrayAddr);
        for (uint64_t exportIndex = 0; exportIndex != patch.patchExportsCount; ++exportIndex) {
            const dyld_cache_patchable_export_v1& patchExport = patchExports[patch.patchExportsStartIndex + exportIndex];
            if ( patchExport.cacheOffsetOfImpl != cacheOffsetOfImpl )
                continue;
            if ( (patchExport.patchLocationsStartIndex + patchExport.patchLocationsCount) > patchInfo->patchLocationArrayCount )
                return;
            for (uint64_t locationIndex = 0; locationIndex != patchExport.patchLocationsCount; ++locationIndex) {
                const dyld_cache_patchable_location_v1& patchLocation = patchLocations[patchExport.patchLocationsStartIndex + locationIndex];

                dyld3::MachOLoaded::PointerMetaData pmd;
                pmd.diversity         = patchLocation.discriminator;
                pmd.high8             = patchLocation.high7 << 1;
                pmd.authenticated     = patchLocation.authenticated;
                pmd.key               = patchLocation.key;
                pmd.usesAddrDiversity = patchLocation.usesAddressDiversity;

                handler(patchLocation.cacheOffset, pmd, patchLocation.getAddend(), false);
            }
        }
        return;
    }

    // V2/V3 and newer structs
    auto getDylibAddress = ^(uint32_t dylibImageIndex) {
        auto* clientHdr = (const Header*)(this->getIndexedImageEntry(dylibImageIndex));
        if ( clientHdr == nullptr )
            return 0ULL;
        return clientHdr->preferredLoadAddress();
    };
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    patchTable.forEachPatchableCacheUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                this->unslidLoadAddress(),
                                                getDylibAddress, handler);
}

void DyldSharedCache::forEachPatchableExport(uint32_t imageIndex, void (^handler)(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                                                  PatchKind patchKind)) const {
    if ( header.patchInfoAddr == 0 )
        return;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs

        // This patch table uses cache offsets, so convert from cache offset to "image + offset"
        uint64_t mTime;
        uint64_t inode;
        const dyld3::MachOAnalyzer* imageMA = (dyld3::MachOAnalyzer*)(this->getIndexedImageEntry(imageIndex, mTime, inode));
        if ( imageMA == nullptr )
            return;

        uint64_t imageLoadAddress = ((const Header*)imageMA)->preferredLoadAddress();
        uint64_t cacheUnslidAddress = unslidLoadAddress();

        const dyld_cache_patch_info_v1* patchInfo = getAddrField<dyld_cache_patch_info_v1*>(header.patchInfoAddr);
        const dyld_cache_image_patches_v1* patchArray = getAddrField<dyld_cache_image_patches_v1*>(patchInfo->patchTableArrayAddr);
        if (imageIndex > patchInfo->patchTableArrayCount)
            return;
        const dyld_cache_image_patches_v1& patch = patchArray[imageIndex];
        if ( (patch.patchExportsStartIndex + patch.patchExportsCount) > patchInfo->patchExportArrayCount )
            return;
        const dyld_cache_patchable_export_v1* patchExports = getAddrField<dyld_cache_patchable_export_v1*>(patchInfo->patchExportArrayAddr);
        const char* exportNames = getAddrField<char*>(patchInfo->patchExportNamesAddr);
        for (uint64_t exportIndex = 0; exportIndex != patch.patchExportsCount; ++exportIndex) {
            const dyld_cache_patchable_export_v1& patchExport = patchExports[patch.patchExportsStartIndex + exportIndex];
            const char* exportName = ( patchExport.exportNameOffset < patchInfo->patchExportNamesSize ) ? &exportNames[patchExport.exportNameOffset] : "";

            // Convert from a cache offset to an offset from the input image
            uint32_t imageOffset = (uint32_t)((cacheUnslidAddress + patchExport.cacheOffsetOfImpl) - imageLoadAddress);
            handler(imageOffset, exportName, PatchKind::regular);
        }

        return;
    }

    // V2 newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    patchTable.forEachPatchableExport(imageIndex, handler);
}

bool DyldSharedCache::shouldPatchClientOfImage(uint32_t imageIndex, uint32_t userImageIndex) const {
    if ( header.patchInfoAddr == 0 )
        return false;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs
        // Only dyld uses this method and is on at least v2, so we don't implement this
        return false;
    }

    // V2/V3 and newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    return patchTable.imageHasClient(imageIndex, userImageIndex);
}

void DyldSharedCache::forEachPatchableUseOfExportInImage(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl, uint32_t userImageIndex,
                                                         void (^handler)(uint32_t userVMOffset, MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                         bool isWeakImport)) const {
    if ( header.patchInfoAddr == 0 )
        return;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs

        // This patch table uses cache offsets, so convert from "image + offset" to cache offset
        uint64_t mTime;
        uint64_t inode;
        const dyld3::MachOAnalyzer* imageMA = (dyld3::MachOAnalyzer*)(this->getIndexedImageEntry(imageIndex, mTime, inode));
        if ( imageMA == nullptr )
            return;

        uint64_t cacheUnslidAddress = unslidLoadAddress();
        uint32_t cacheOffsetOfImpl = (uint32_t)((((const Header*)imageMA)->preferredLoadAddress() - cacheUnslidAddress) + dylibVMOffsetOfImpl);

        // Loading a new cache so get the data from the cache header
        const dyld_cache_patch_info_v1* patchInfo = getAddrField<dyld_cache_patch_info_v1*>(header.patchInfoAddr);
        const dyld_cache_image_patches_v1* patchArray = getAddrField<dyld_cache_image_patches_v1*>(patchInfo->patchTableArrayAddr);
        if (imageIndex > patchInfo->patchTableArrayCount)
            return;
        const dyld_cache_image_patches_v1& patch = patchArray[imageIndex];
        if ( (patch.patchExportsStartIndex + patch.patchExportsCount) > patchInfo->patchExportArrayCount )
            return;

        // V1 doesn't know which patch location corresponds to which dylib.  This is expensive, but temporary, so find the dylib for
        // each patch
        struct DataRange
        {
            uint64_t cacheOffsetStart;
            uint64_t cacheOffsetEnd;
        };
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(DataRange, dataRanges, 8);
        __block const Header* userDylib = nullptr;
        __block uint32_t userDylibImageIndex = ~0U;

        const dyld_cache_patchable_export_v1* patchExports = getAddrField<dyld_cache_patchable_export_v1*>(patchInfo->patchExportArrayAddr);
        const dyld_cache_patchable_location_v1* patchLocations = getAddrField<dyld_cache_patchable_location_v1*>(patchInfo->patchLocationArrayAddr);
        for (uint64_t exportIndex = 0; exportIndex != patch.patchExportsCount; ++exportIndex) {
            const dyld_cache_patchable_export_v1& patchExport = patchExports[patch.patchExportsStartIndex + exportIndex];
            if ( patchExport.cacheOffsetOfImpl != cacheOffsetOfImpl )
                continue;
            if ( (patchExport.patchLocationsStartIndex + patchExport.patchLocationsCount) > patchInfo->patchLocationArrayCount )
                return;
            for (uint64_t locationIndex = 0; locationIndex != patchExport.patchLocationsCount; ++locationIndex) {
                const dyld_cache_patchable_location_v1& patchLocation = patchLocations[patchExport.patchLocationsStartIndex + locationIndex];

                bool computeNewRanges = false;
                if ( userDylib == nullptr ) {
                    computeNewRanges = true;
                } else {
                    bool inRange = false;
                    for ( const DataRange& range : dataRanges ) {
                        if ( (patchLocation.cacheOffset >= range.cacheOffsetStart) && (patchLocation.cacheOffset < range.cacheOffsetEnd) ) {
                            inRange = true;
                            break;
                        }
                    }
                    if ( !inRange )
                        computeNewRanges = true;
                }

                if ( computeNewRanges ) {
                    userDylib = nullptr;
                    userDylibImageIndex = ~0U;
                    dataRanges.clear();
                    forEachDylib(^(const Header* hdr, const char* dylibPath, uint32_t cacheImageIndex, uint64_t, uint64_t, bool& stopImage) {
                        hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stopSegment) {
                            if ( info.writable() )
                                dataRanges.push_back({ info.vmaddr - cacheUnslidAddress, info.vmaddr + info.vmsize - cacheUnslidAddress });
                        });

                        bool inRange = false;
                        for ( const DataRange& range : dataRanges ) {
                            if ( (patchLocation.cacheOffset >= range.cacheOffsetStart) && (patchLocation.cacheOffset < range.cacheOffsetEnd) ) {
                                inRange = true;
                                break;
                            }
                        }
                        if ( inRange ) {
                            // This is dylib we want.  So we can keep these ranges, and record this mach-header
                            userDylib = hdr;
                            userDylibImageIndex = cacheImageIndex;
                            stopImage = true;
                        } else {
                            // These ranges don't work.  Clear them and move on to the next dylib
                            dataRanges.clear();
                        }
                    });
                }

                assert(userDylib != nullptr);
                assert(userDylibImageIndex != ~0U);
                assert(!dataRanges.empty());

                // We only want fixups in a specific image.  Skip any others
                if ( userDylibImageIndex == userImageIndex ) {
                    uint32_t userVMOffset = (uint32_t)((cacheUnslidAddress + patchLocation.cacheOffset) - userDylib->preferredLoadAddress());
                    dyld3::MachOLoaded::PointerMetaData pmd;
                    pmd.diversity         = patchLocation.discriminator;
                    pmd.high8             = patchLocation.high7 << 1;
                    pmd.authenticated     = patchLocation.authenticated;
                    pmd.key               = patchLocation.key;
                    pmd.usesAddrDiversity = patchLocation.usesAddressDiversity;

                    handler(userVMOffset, pmd, patchLocation.getAddend(), false);
                }
            }
        }
        return;
    }

    // V2/V3 and newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    patchTable.forEachPatchableUseOfExportInImage(imageIndex, dylibVMOffsetOfImpl,
                                                  userImageIndex, handler);
}


#if !TARGET_OS_EXCLAVEKIT
#if (BUILDING_LIBDYLD || BUILDING_DYLD)
VIS_HIDDEN bool gEnableSharedCacheDataConst = false;
#endif


const char* DyldSharedCache::getCacheTypeName(uint64_t cacheType) {
    switch ( cacheType ) {
        case kDyldSharedCacheTypeDevelopment:
            return "development";
        case kDyldSharedCacheTypeProduction:
            return "production";
        case kDyldSharedCacheTypeUniversal:
            return "universal";
        default:
            return "unknown";
    }
}

void DyldSharedCache::forEachTPRORegion(void (^handler)(const void* content, uint64_t unslidVMAddr, uint64_t vmSize,
                                                        bool& stopRegion)) const
{
    if ( header.mappingOffset <= offsetof(dyld_cache_header, tproMappingsCount) )
        return;

    uint64_t baseAddress = this->unslidLoadAddress();

    const dyld_cache_tpro_mapping_info* mappings = (const dyld_cache_tpro_mapping_info*)((char*)this + header.tproMappingsOffset);
    const dyld_cache_tpro_mapping_info* mappingsEnd = &mappings[header.tproMappingsCount];
    for (const dyld_cache_tpro_mapping_info* m = mappings; m < mappingsEnd; ++m) {
        bool stop = false;
        uint64_t offsetInCache = m->unslidAddress - baseAddress;
        handler((char*)this + (long)offsetInCache, m->unslidAddress, m->size, stop);
        if ( stop )
            return;
    }
}

int32_t DyldSharedCache::getSubCacheIndex(const void* addr) const
{
    __block int32_t index = 0;
    __block bool found = false;
    this->forEachCache(^(const DyldSharedCache *cache, bool &stopCache) {
        bool readOnly = false;
        if ( cache->inCache(addr, sizeof(uint64_t), readOnly) ) {
            stopCache = true;
            found = true;
            return;
        }
        index++;
    });
    int32_t result = found ? index : -1;
    return result;
}

void DyldSharedCache::getSubCacheUuid(uint8_t index, uint8_t uuid[]) const {
    if (header.mappingOffset <= offsetof(dyld_cache_header, cacheSubType) ) {
        const dyld_subcache_entry_v1* subCacheEntries = (dyld_subcache_entry_v1*)((uintptr_t)this + header.subCacheArrayOffset);
        memcpy(uuid, subCacheEntries[index].uuid, 16);
    } else {
        const dyld_subcache_entry* subCacheEntries = (dyld_subcache_entry*)((uintptr_t)this + header.subCacheArrayOffset);
        memcpy(uuid, subCacheEntries[index].uuid, 16);
    }
}

bool DyldSharedCache::inCache(const void* addr, size_t length, bool& immutable) const
{
    // quick out if before start of cache
    if ( addr < this )
        return false;

    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    uintptr_t unslidStart = (uintptr_t)addr - slide;

    // walk cache ranges
    __block bool found = false;
    auto inRange = ^(const char* mappingName, uint64_t unslidVMAddr, uint64_t vmSize, uint32_t cacheFileIndex,
                     uint64_t fileOffset, uint32_t initProt, uint32_t maxProt, bool& stopRange) {
        if ( (unslidVMAddr <= unslidStart) && ((unslidStart+length) < (unslidVMAddr+vmSize)) ) {
            found     = true;
            immutable = ((maxProt & VM_PROT_WRITE) == 0);
            stopRange = true;
        }
    };
    this->forEachRange(inRange, nullptr);

    return found;
}

bool DyldSharedCache::isAlias(const char* path) const {
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    // paths for aliases are store between cache header and first segment
    return path < ((char*)mappings[0].address + slide);
}

void DyldSharedCache::forEachImage(void (^handler)(const Header* hdr, const char* installName)) const
{
    const dyld_cache_image_info*   dylibs   = images();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return;
    uint64_t firstImageOffset = 0;
    uint64_t firstRegionAddress = mappings[0].address;
    for (uint32_t i=0; i < imagesCount(); ++i) {
        const char* dylibPath  = (char*)this + dylibs[i].pathFileOffset;
        uint64_t offset = dylibs[i].address - firstRegionAddress;
        if ( firstImageOffset == 0 )
            firstImageOffset = offset;
        // skip over aliases.  This is no longer valid in newer caches.  They store aliases only in the trie
#if 0
        if ( dylibs[i].pathFileOffset < firstImageOffset)
            continue;
#endif
        const Header* hdr = (const Header*)((char*)this + offset);
        handler(hdr, dylibPath);
    }
}


void DyldSharedCache::forEachImageEntry(void (^handler)(const char* path, uint64_t mTime, uint64_t inode)) const
{
    const dyld_cache_image_info*   dylibs   = images();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return;
    uint64_t firstImageOffset = 0;
    uint64_t firstRegionAddress = mappings[0].address;
    for (uint32_t i=0; i < imagesCount(); ++i) {
        const char* dylibPath  = (char*)this + dylibs[i].pathFileOffset;
        uint64_t offset = dylibs[i].address - firstRegionAddress;
        if ( firstImageOffset == 0 )
            firstImageOffset = offset;
        // skip over aliases.  This is no longer valid in newer caches.  They store aliases only in the trie
#if 0
        if ( dylibs[i].pathFileOffset < firstImageOffset)
            continue;
#endif
        handler(dylibPath, dylibs[i].modTime, dylibs[i].inode);
    }
}

const bool DyldSharedCache::hasLocalSymbolsInfo() const
{
    return (header.localSymbolsOffset != 0 && header.mappingOffset > offsetof(dyld_cache_header,localSymbolsSize));
}

const bool DyldSharedCache::hasLocalSymbolsInfoFile() const
{
    if ( header.mappingOffset > offsetof(dyld_cache_header, symbolFileUUID) )
        return !uuid_is_null(header.symbolFileUUID);

    // Old cache file
    return false;
}

const void* DyldSharedCache::getLocalNlistEntries(const dyld_cache_local_symbols_info* localInfo) {
    return (uint8_t*)localInfo + localInfo->nlistOffset;
}

const void* DyldSharedCache::getLocalNlistEntries() const
{
    // check for cache without local symbols info
    if (!this->hasLocalSymbolsInfo())
        return nullptr;
    const auto localInfo = (dyld_cache_local_symbols_info*)((uintptr_t)this + header.localSymbolsOffset);
    return getLocalNlistEntries(localInfo);
}

const uint32_t DyldSharedCache::getLocalNlistCount() const
{
    // check for cache without local symbols info
     if (!this->hasLocalSymbolsInfo())
        return 0;
    const auto localInfo = (dyld_cache_local_symbols_info*)((uintptr_t)this + header.localSymbolsOffset);
    return localInfo->nlistCount;
}

const char* DyldSharedCache::getLocalStrings(const dyld_cache_local_symbols_info* localInfo)
{
    return (char*)localInfo + localInfo->stringsOffset;
}

const char* DyldSharedCache::getLocalStrings() const
{
    // check for cache without local symbols info
     if (!this->hasLocalSymbolsInfo())
        return nullptr;
    const auto localInfo = (dyld_cache_local_symbols_info*)((uintptr_t)this + header.localSymbolsOffset);
    return getLocalStrings(localInfo);
}

const uint32_t DyldSharedCache::getLocalStringsSize() const
{
    // check for cache without local symbols info
     if (!this->hasLocalSymbolsInfo())
        return 0;
    const auto localInfo = (dyld_cache_local_symbols_info*)((uintptr_t)this + header.localSymbolsOffset);
    return localInfo->stringsSize;
}

void DyldSharedCache::forEachLocalSymbolEntry(void (^handler)(uint64_t dylibOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop)) const
{
    // check for cache without local symbols info
    if (!this->hasLocalSymbolsInfo())
        return;
    const auto localInfo = (dyld_cache_local_symbols_info*)((uintptr_t)this + header.localSymbolsOffset);

    if ( header.mappingOffset >= offsetof(dyld_cache_header, symbolFileUUID) ) {
        // On new caches, the dylibOffset is 64-bits, and is a VM offset
        const auto localEntries = (dyld_cache_local_symbols_entry_64*)((uint8_t*)localInfo + localInfo->entriesOffset);
        bool stop = false;
        for (uint32_t i = 0; i < localInfo->entriesCount; i++) {
            const dyld_cache_local_symbols_entry_64& localEntry = localEntries[i];
            handler(localEntry.dylibOffset, localEntry.nlistStartIndex, localEntry.nlistCount, stop);
        }
    } else {
        // On old caches, the dylibOffset is 64-bits, and is a file offset
        // Note, as we are only looking for mach_header's, a file offset is a VM offset in this case
        const auto localEntries = (dyld_cache_local_symbols_entry*)((uint8_t*)localInfo + localInfo->entriesOffset);
        bool stop = false;
        for (uint32_t i = 0; i < localInfo->entriesCount; i++) {
            const dyld_cache_local_symbols_entry& localEntry = localEntries[i];
            handler(localEntry.dylibOffset, localEntry.nlistStartIndex, localEntry.nlistCount, stop);
        }
    }
}

bool DyldSharedCache::addressInText(uint64_t cacheOffset, uint32_t* imageIndex) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uint64_t targetAddr = mappings[0].address + cacheOffset;
    // walk imageText table and call callback for each entry
    const dyld_cache_image_text_info* imagesText = (dyld_cache_image_text_info*)((char*)this + header.imagesTextOffset);
    const dyld_cache_image_text_info* imagesTextEnd = &imagesText[header.imagesTextCount];
    for (const dyld_cache_image_text_info* p=imagesText; p < imagesTextEnd; ++p) {
        if ( (p->loadAddress <= targetAddr) && (targetAddr < p->loadAddress+p->textSegmentSize) ) {
            *imageIndex = (uint32_t)(p-imagesText);
            return true;
        }
    }
    return false;
}

const char* DyldSharedCache::archName() const
{
    const char* archSubString = ((char*)this) + 7;
    while (*archSubString == ' ')
        ++archSubString;
    return archSubString;
}

const DyldSharedCache::DynamicRegion* DyldSharedCache::dynamicRegion() const
{
    const DyldSharedCache::DynamicRegion* dr = (const DynamicRegion*)((uint8_t*)this + header.dynamicDataOffset);
    if ( dr->validMagic() )
        return dr;
    return nullptr;
}

Platform DyldSharedCache::platform() const
{
    return Platform(header.platform);
}

#if BUILDING_CACHE_BUILDER
std::string DyldSharedCache::mapFile() const
{
    __block std::string             result;
    __block std::vector<uint64_t>   regionStartAddresses;
    __block std::vector<uint64_t>   regionSizes;
    __block std::vector<uint64_t>   regionFileOffsets;

    result.reserve(256*1024);
    forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size,
                    uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
        regionStartAddresses.push_back(vmAddr);
        regionSizes.push_back(size);
        regionFileOffsets.push_back((uint8_t*)content - (uint8_t*)this);
        char lineBuffer[256];
        const char* prot = "RW";
        if ( maxProt == (VM_PROT_EXECUTE|VM_PROT_READ) )
            prot = "EX";
        else if ( maxProt == VM_PROT_READ )
            prot = "RO";
        if ( size > 1024*1024 )
            snprintf(lineBuffer, sizeof(lineBuffer), "mapping  %s %4lluMB 0x%0llX -> 0x%0llX\n", prot, size/(1024*1024), vmAddr, vmAddr+size);
        else
            snprintf(lineBuffer, sizeof(lineBuffer), "mapping  %s %4lluKB 0x%0llX -> 0x%0llX\n", prot, size/1024,        vmAddr, vmAddr+size);
        result += lineBuffer;
    });

    // TODO:  add linkedit breakdown
    result += "\n\n";

    forEachImage(^(const Header* hdr, const char* installName) {
        result += std::string(installName) + "\n";
        hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
            char lineBuffer[256];
            snprintf(lineBuffer, sizeof(lineBuffer), "\t%16.*s 0x%08llX -> 0x%08llX\n",
                     (int)info.segmentName.size(), info.segmentName.data(), info.vmaddr, info.vmaddr+info.vmsize);
            result += lineBuffer;
        });
        result += "\n";
    });

    return result;
}
#endif

bool DyldSharedCache::findMachHeaderImageIndex(const mach_header* mh, uint32_t& imageIndex) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    uint64_t unslidMh = (uintptr_t)mh - slide;
    const dyld_cache_image_info* dylibs = images();
    for (uint32_t i=0; i < imagesCount(); ++i) {
        if ( dylibs[i].address == unslidMh ) {
            imageIndex = i;
            return true;
        }
    }
    return false;
}

const PrebuiltLoaderSet* DyldSharedCache::dylibsLoaderSet() const
{
    if ( header.mappingOffset < offsetof(dyld_cache_header, programTrieSize) )
        return nullptr;
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return nullptr;
    if ( header.mappingOffset < offsetof(dyld_cache_header, dylibsPBLSetAddr) )
        return nullptr;
    if ( header.dylibsPBLSetAddr == 0 )
        return nullptr;
    uintptr_t                slide        = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const PrebuiltLoaderSet* pbLoaderSet  = (PrebuiltLoaderSet*)(this->header.dylibsPBLSetAddr + slide);
    return pbLoaderSet;
}

const PrebuiltLoader* DyldSharedCache::findPrebuiltLoader(const char* path) const
{
    if ( header.mappingOffset < offsetof(dyld_cache_header, programTrieSize) )
        return nullptr;
    uint32_t imageIndex;
    if ( !this->hasImagePath(path, imageIndex) )
        return nullptr;
    if ( const PrebuiltLoaderSet* pbLoaderSet = this->dylibsLoaderSet() )
        return pbLoaderSet->atIndex(imageIndex);

    return nullptr;
}

void DyldSharedCache::forEachLaunchLoaderSet(void (^handler)(const char* executableRuntimePath, const PrebuiltLoaderSet* pbls)) const
{
    if ( header.mappingOffset < offsetof(dyld_cache_header, programTrieSize) )
        return;
    if ( this->header.programTrieAddr == 0 )
        return;
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide                = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* executableTrieStart  = (uint8_t*)(this->header.programTrieAddr + slide);
    const uint8_t* executableTrieEnd    = executableTrieStart + this->header.programTrieSize;
    const uint8_t* poolStart            = (uint8_t*)(this->header.programsPBLSetPoolAddr + slide);

    std::vector<DylibIndexTrie::Entry> loaderSetEntries;
    if ( Trie<DylibIndex>::parseTrie(executableTrieStart, executableTrieEnd, loaderSetEntries) ) {
        for (const DylibIndexTrie::Entry& entry : loaderSetEntries ) {
            uint32_t offset = entry.info.index;
            if ( offset < this->header.programsPBLSetPoolSize )
                handler(entry.name.c_str(), (const PrebuiltLoaderSet*)(poolStart+offset));
        }
    }
}

const PrebuiltLoaderSet* DyldSharedCache::findLaunchLoaderSet(const char* executablePath) const
{
    if ( header.mappingOffset < offsetof(dyld_cache_header, programTrieSize) )
        return nullptr;
    if ( this->header.programTrieAddr == 0 )
        return nullptr;
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide                = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* executableTrieStart  = (uint8_t*)(this->header.programTrieAddr + slide);
    const uint8_t* executableTrieEnd    = executableTrieStart + this->header.programTrieSize;
    const uint8_t* poolStart            = (uint8_t*)(this->header.programsPBLSetPoolAddr + slide);

    Diagnostics diag;
    if ( const uint8_t* imageNode = dyld3::MachOLoaded::trieWalk(diag, executableTrieStart, executableTrieEnd, executablePath) ) {
        uint32_t poolOffset = (uint32_t)dyld3::MachOFile::read_uleb128(diag, imageNode, executableTrieEnd);
        if ( poolOffset < this->header.programsPBLSetPoolSize ) {
            return (PrebuiltLoaderSet*)((uint8_t*)poolStart + poolOffset);
        }
    }

    return nullptr;
}

bool DyldSharedCache::hasLaunchLoaderSetWithCDHash(const char* cdHashString) const
{
    return (findLaunchLoaderSetWithCDHash(cdHashString) != nullptr);
}

const dyld4::PrebuiltLoaderSet* DyldSharedCache::findLaunchLoaderSetWithCDHash(const char* cdHashString) const
{
    if ( cdHashString == nullptr )
        return nullptr;

    // Check source doesn't overflow buffer.  strncat unfortunately isn't available
    if ( strlen(cdHashString) >= 128 )
        return nullptr;

    char cdPath[140];
    strlcpy(cdPath, "/cdhash/", sizeof(cdPath));
    strlcat(cdPath, cdHashString, sizeof(cdPath));
    return findLaunchLoaderSet(cdPath);
}


#if 0 //!BUILDING_LIBDSC
const dyld3::closure::Image* DyldSharedCache::findDlopenOtherImage(const char* path) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return nullptr;
    if ( header.mappingOffset < offsetof(dyld_cache_header, otherImageArrayAddr) )
        return nullptr;
    if ( header.otherImageArrayAddr == 0 )
        return nullptr;
    uintptr_t      slide           = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* dylibTrieStart  = (uint8_t*)(this->header.otherTrieAddr + slide);
    const uint8_t* dylibTrieEnd    = dylibTrieStart + this->header.otherTrieSize;

    Diagnostics diag;
    const uint8_t* imageNode = dyld3::MachOLoaded::trieWalk(diag, dylibTrieStart, dylibTrieEnd, path);
    if ( imageNode != NULL ) {
        dyld3::closure::ImageNum imageNum = (uint32_t)dyld3::MachOFile::read_uleb128(diag, imageNode, dylibTrieEnd);
        uint64_t arrayAddrOffset = header.otherImageArrayAddr - mappings[0].address;
        const dyld3::closure::ImageArray* otherImageArray = (dyld3::closure::ImageArray*)((char*)this + arrayAddrOffset);
        return otherImageArray->imageForNum(imageNum);
    }

    return nullptr;
}

const dyld3::closure::LaunchClosure* DyldSharedCache::findClosure(const char* executablePath) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide                = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* executableTrieStart  = (uint8_t*)(this->header.progClosuresTrieAddr + slide);
    const uint8_t* executableTrieEnd    = executableTrieStart + this->header.progClosuresTrieSize;
    const uint8_t* closuresStart        = (uint8_t*)(this->header.progClosuresAddr + slide);

    Diagnostics diag;
    const uint8_t* imageNode = dyld3::MachOLoaded::trieWalk(diag, executableTrieStart, executableTrieEnd, executablePath);
    if ( (imageNode == NULL) && (strncmp(executablePath, "/System/", 8) == 0) ) {
        // anything in /System/ should have a closure.  Perhaps it was launched via symlink path
        char realPath[PATH_MAX];
        if ( realpath(executablePath, realPath) != NULL )
            imageNode = dyld3::MachOLoaded::trieWalk(diag, executableTrieStart, executableTrieEnd, realPath);
    }
    if ( imageNode != NULL ) {
        uint32_t closureOffset = (uint32_t)dyld3::MachOFile::read_uleb128(diag, imageNode, executableTrieEnd);
        if ( closureOffset < this->header.progClosuresSize )
            return (dyld3::closure::LaunchClosure*)((uint8_t*)closuresStart + closureOffset);
    }

    return nullptr;
}

#if !BUILDING_LIBDYLD && !BUILDING_DYLD
void DyldSharedCache::forEachLaunchClosure(void (^handler)(const char* executableRuntimePath, const dyld3::closure::LaunchClosure* closure)) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide                = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* executableTrieStart  = (uint8_t*)(this->header.progClosuresTrieAddr + slide);
    const uint8_t* executableTrieEnd    = executableTrieStart + this->header.progClosuresTrieSize;
    const uint8_t* closuresStart        = (uint8_t*)(this->header.progClosuresAddr + slide);

    std::vector<DylibIndexTrie::Entry> closureEntries;
    if ( Trie<DylibIndex>::parseTrie(executableTrieStart, executableTrieEnd, closureEntries) ) {
        for (DylibIndexTrie::Entry& entry : closureEntries ) {
            uint32_t offset = entry.info.index;
            if ( offset < this->header.progClosuresSize )
                handler(entry.name.c_str(), (const dyld3::closure::LaunchClosure*)(closuresStart+offset));
        }
    }
}

void DyldSharedCache::forEachDlopenImage(void (^handler)(const char* runtimePath, const dyld3::closure::Image* image)) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide           = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* otherTrieStart  = (uint8_t*)(this->header.otherTrieAddr + slide);
    const uint8_t* otherTrieEnd    = otherTrieStart + this->header.otherTrieSize;

    std::vector<DylibIndexTrie::Entry> otherEntries;
    if ( Trie<DylibIndex>::parseTrie(otherTrieStart, otherTrieEnd, otherEntries) ) {
        for (const DylibIndexTrie::Entry& entry : otherEntries ) {
            dyld3::closure::ImageNum imageNum = entry.info.index;
            uint64_t arrayAddrOffset = header.otherImageArrayAddr - mappings[0].address;
            const dyld3::closure::ImageArray* otherImageArray = (dyld3::closure::ImageArray*)((char*)this + arrayAddrOffset);
            handler(entry.name.c_str(), otherImageArray->imageForNum(imageNum));
        }
    }
}
#endif // !BUILDING_LIBDYLD && !BUILDING_DYLD


const dyld3::closure::ImageArray* DyldSharedCache::cachedDylibsImageArray() const
{
    // check for old cache without imagesArray
    if ( header.mappingOffset < 0x100 )
        return nullptr;

    if ( header.dylibsImageArrayAddr == 0 )
        return nullptr;
        
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uint64_t arrayAddrOffset = header.dylibsImageArrayAddr - mappings[0].address;
    return (dyld3::closure::ImageArray*)((char*)this + arrayAddrOffset);
}

const dyld3::closure::ImageArray* DyldSharedCache::otherOSImageArray() const
{
    // check for old cache without imagesArray
    if ( header.mappingOffset < offsetof(dyld_cache_header, otherImageArrayAddr) )
        return nullptr;

    if ( header.otherImageArrayAddr == 0 )
        return nullptr;

    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uint64_t arrayAddrOffset = header.otherImageArrayAddr - mappings[0].address;
    return (dyld3::closure::ImageArray*)((char*)this + arrayAddrOffset);
}
#endif // !BUILDING_LIBDSC

void DyldSharedCache::forEachDylibPath(void (^handler)(const char* dylibPath, uint32_t index)) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t      slide                = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const uint8_t* dylibTrieStart       = (uint8_t*)(this->header.dylibsTrieAddr + slide);
    const uint8_t* dylibTrieEnd         = dylibTrieStart + this->header.dylibsTrieSize;

   std::vector<DylibIndexTrie::Entry> dylibEntries;
    if ( Trie<DylibIndex>::parseTrie(dylibTrieStart, dylibTrieEnd, dylibEntries) ) {
        for (DylibIndexTrie::Entry& entry : dylibEntries ) {
            handler(entry.name.c_str(), entry.info.index);
        }
    }
}

void DyldSharedCache::forEachFunctionVariantPatchLocation(void (^handler)(const void* loc, PointerMetaData pmd, const mach_o::FunctionVariants& fvs, const mach_o::Header* dylibHdr, int variantIndex, bool& stop)) const
{
    // check for old cache
    if ( header.mappingOffset <= __offsetof(dyld_cache_header, functionVariantInfoSize) )
        return;

    const dyld_cache_mapping_info*          mappings  = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t                               slide     = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    const dyld_cache_function_variant_info* table     = (dyld_cache_function_variant_info*)((char*)this->header.functionVariantInfoAddr + slide);

    size_t sizeFromTable       = offsetof(dyld_cache_function_variant_info, entries[table->count]);
    size_t sizeFromCacheHeader = (size_t)this->header.functionVariantInfoSize;
    if ( sizeFromTable > sizeFromCacheHeader )
        return; // something is wrong

    bool stop = false;
    for (uint32_t i=0; i < table->count; ++i) {
        const dyld_cache_function_variant_entry& entry = table->entries[i];
        std::span<const uint8_t> fvSpan{(uint8_t*)(entry.functionVariantTableVmAddr + slide), (size_t)(entry.functionVariantTableSizeDiv4*4)};
        const mach_o::FunctionVariants  fvs(fvSpan);
        PointerMetaData                 pmd;
        pmd.authenticated     = entry.pacAuth;
        pmd.key               = entry.pacKey;
        pmd.usesAddrDiversity = entry.pacAddress;
        pmd.diversity         = entry.pacDiversity;
        pmd.high8             = 0;
        handler((void*)(entry.fixupLocVmAddr + slide), pmd, fvs, (mach_o::Header*)(entry.dylibHeaderVmAddr+slide), entry.variantIndex, stop);
        if ( stop )
            break;
    }
}
uint32_t DyldSharedCache::patchableExportCount(uint32_t imageIndex) const {
    if ( header.patchInfoAddr == 0 )
        return 0;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs

        const dyld_cache_patch_info_v1* patchInfo = getAddrField<dyld_cache_patch_info_v1*>(header.patchInfoAddr);
        const dyld_cache_image_patches_v1* patchArray = getAddrField<dyld_cache_image_patches_v1*>(patchInfo->patchTableArrayAddr);
        if (imageIndex > patchInfo->patchTableArrayCount)
            return 0;
        return patchArray[imageIndex].patchExportsCount;
    }

    // V2/V3 and newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    return patchTable.patchableExportCount(imageIndex);
}

#if BUILDING_SHARED_CACHE_UTIL
void DyldSharedCache::forEachPatchableUseOfExport(uint32_t imageIndex, uint32_t dylibVMOffsetOfImpl,
                                                  void (^handler)(uint32_t userImageIndex, uint32_t userVMOffset,
                                                                  MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                  bool isWeakImport)) const {
    if ( header.patchInfoAddr == 0 )
        return;

    uint32_t patchVersion = patchInfoVersion();

    if ( patchVersion == 1 ) {
        // Old cache.  The patch table uses the V1 structs

        // This patch table uses cache offsets, so convert from "image + offset" to cache offset
        uint64_t mTime;
        uint64_t inode;
        const dyld3::MachOAnalyzer* imageMA = (dyld3::MachOAnalyzer*)(this->getIndexedImageEntry(imageIndex, mTime, inode));
        if ( imageMA == nullptr )
            return;

        uint64_t cacheUnslidAddress = unslidLoadAddress();
        uint32_t cacheOffsetOfImpl = (uint32_t)((((const Header*)imageMA)->preferredLoadAddress() - cacheUnslidAddress) + dylibVMOffsetOfImpl);

        // Loading a new cache so get the data from the cache header
        const dyld_cache_patch_info_v1* patchInfo = getAddrField<dyld_cache_patch_info_v1*>(header.patchInfoAddr);
        const dyld_cache_image_patches_v1* patchArray = getAddrField<dyld_cache_image_patches_v1*>(patchInfo->patchTableArrayAddr);
        if (imageIndex > patchInfo->patchTableArrayCount)
            return;
        const dyld_cache_image_patches_v1& patch = patchArray[imageIndex];
        if ( (patch.patchExportsStartIndex + patch.patchExportsCount) > patchInfo->patchExportArrayCount )
            return;

        // V1 doesn't know which patch location corresponds to which dylib.  This is expensive, but temporary, so find the dylib for
        // each patch
        struct DataRange
        {
            uint64_t cacheOffsetStart;
            uint64_t cacheOffsetEnd;
        };
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(DataRange, dataRanges, 8);
        __block const Header* userDylib = nullptr;
        __block uint32_t userImageIndex = ~0U;

        const dyld_cache_patchable_export_v1* patchExports = getAddrField<dyld_cache_patchable_export_v1*>(patchInfo->patchExportArrayAddr);
        const dyld_cache_patchable_location_v1* patchLocations = getAddrField<dyld_cache_patchable_location_v1*>(patchInfo->patchLocationArrayAddr);
        for (uint64_t exportIndex = 0; exportIndex != patch.patchExportsCount; ++exportIndex) {
            const dyld_cache_patchable_export_v1& patchExport = patchExports[patch.patchExportsStartIndex + exportIndex];
            if ( patchExport.cacheOffsetOfImpl != cacheOffsetOfImpl )
                continue;
            if ( (patchExport.patchLocationsStartIndex + patchExport.patchLocationsCount) > patchInfo->patchLocationArrayCount )
                return;
            for (uint64_t locationIndex = 0; locationIndex != patchExport.patchLocationsCount; ++locationIndex) {
                const dyld_cache_patchable_location_v1& patchLocation = patchLocations[patchExport.patchLocationsStartIndex + locationIndex];

                bool computeNewRanges = false;
                if ( userDylib == nullptr ) {
                    computeNewRanges = true;
                } else {
                    bool inRange = false;
                    for ( const DataRange& range : dataRanges ) {
                        if ( (patchLocation.cacheOffset >= range.cacheOffsetStart) && (patchLocation.cacheOffset < range.cacheOffsetEnd) ) {
                            inRange = true;
                            break;
                        }
                    }
                    if ( !inRange )
                        computeNewRanges = true;
                }

                if ( computeNewRanges ) {
                    userDylib = nullptr;
                    userImageIndex = ~0U;
                    dataRanges.clear();
                    forEachDylib(^(const Header* hdr, const char* dylibPath, uint32_t cacheImageIndex, uint64_t, uint64_t, bool& stopImage) {
                        hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stopSegment) {
                            if ( info.writable() )
                                dataRanges.push_back({ info.vmaddr - cacheUnslidAddress, info.vmaddr + info.vmsize - cacheUnslidAddress });
                        });

                        bool inRange = false;
                        for ( const DataRange& range : dataRanges ) {
                            if ( (patchLocation.cacheOffset >= range.cacheOffsetStart) && (patchLocation.cacheOffset < range.cacheOffsetEnd) ) {
                                inRange = true;
                                break;
                            }
                        }
                        if ( inRange ) {
                            // This is dylib we want.  So we can keep these ranges, and record this mach-header
                            userDylib = hdr;
                            userImageIndex = cacheImageIndex;
                            stopImage = true;
                        } else {
                            // These ranges don't work.  Clear them and move on to the next dylib
                            dataRanges.clear();
                        }
                    });
                }

                assert(userDylib != nullptr);
                assert(userImageIndex != ~0U);
                assert(!dataRanges.empty());

                uint32_t userVMOffset = (uint32_t)((cacheUnslidAddress + patchLocation.cacheOffset) - userDylib->preferredLoadAddress());
                dyld3::MachOLoaded::PointerMetaData pmd;
                pmd.diversity         = patchLocation.discriminator;
                pmd.high8             = patchLocation.high7 << 1;
                pmd.authenticated     = patchLocation.authenticated;
                pmd.key               = patchLocation.key;
                pmd.usesAddrDiversity = patchLocation.usesAddressDiversity;

                handler(userImageIndex, userVMOffset, pmd, patchLocation.getAddend(), false);
            }
        }
        return;
    }

    // V2/V3 and newer structs
    PatchTable patchTable(this->patchTable(), header.patchInfoAddr);
    patchTable.forEachPatchableUseOfExport(imageIndex, dylibVMOffsetOfImpl, handler);
}
#endif


#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
// MRM map file generator
std::string DyldSharedCache::generateJSONMap(const char* disposition, uuid_t cache_uuid, bool verbose) const {
    json::Node cacheNode;

    cacheNode.map["version"].value = "1";
    cacheNode.map["disposition"].value = disposition;
    cacheNode.map["base-address"].value = json::hex(unslidLoadAddress());
    uuid_string_t cache_uuidStr;
    uuid_unparse(cache_uuid, cache_uuidStr);
    cacheNode.map["uuid"].value = cache_uuidStr;

    __block json::Node imagesNode;
    forEachImage(^(const Header *hdr, const char *installName) {
        json::Node imageNode;
        imageNode.map["path"].value = installName;
        uuid_t uuid;
        if (hdr->getUuid(uuid)) {
            uuid_string_t uuidStr;
            uuid_unparse(uuid, uuidStr);
            imageNode.map["uuid"].value = uuidStr;
        }

        __block json::Node segmentsNode;
        hdr->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
            json::Node segmentNode;
            segmentNode.map["name"].value = info.segmentName;
            segmentNode.map["start-vmaddr"].value = json::hex(info.vmaddr);
            segmentNode.map["end-vmaddr"].value = json::hex(info.vmaddr + info.vmsize);

            // Add sections in verbose mode
            if ( verbose ) {
                __block json::Node sectionsNode;
                hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stopSection) {
                    if ( sectInfo.segmentName == info.segmentName ) {
                        json::Node sectionNode;
                        sectionNode.map["name"].value = sectInfo.sectionName;
                        sectionNode.map["size"] = json::Node(sectInfo.size);
                        sectionsNode.array.push_back(sectionNode);
                    }
                });
                if ( !sectionsNode.array.empty() )
                    segmentNode.map["sections"] = std::move(sectionsNode);
            }
            segmentsNode.array.push_back(segmentNode);
        });
        imageNode.map["segments"] = segmentsNode;
        imagesNode.array.push_back(imageNode);
    });

    cacheNode.map["images"] = imagesNode;

    std::stringstream stream;
    printJSON(cacheNode, 0, stream);

    return stream.str();
}

std::string DyldSharedCache::generateJSONDependents() const {
    std::unordered_map<std::string, std::set<std::string>> dependents;
    computeTransitiveDependents(dependents);

    std::stringstream stream;

    stream << "{";
    bool first = true;
    for (auto p : dependents) {
        if (!first) stream << "," << std::endl;
        first = false;

        stream << "\"" << p.first << "\" : [" << std::endl;
        bool firstDependent = true;
        for (const std::string & dependent : p.second) {
            if (!firstDependent) stream << "," << std::endl;
            firstDependent = false;
            stream << "  \"" << dependent << "\"";
        }
        stream << "]" <<  std::endl;
    }
    stream << "}" << std::endl;
    return stream.str();
}

#endif

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
dyld3::MachOAnalyzer::VMAddrConverter DyldSharedCache::makeVMAddrConverter(bool contentRebased) const {
    typedef dyld3::MachOAnalyzer::VMAddrConverter VMAddrConverter;

    __block VMAddrConverter::SharedCacheFormat pointerFormat = VMAddrConverter::SharedCacheFormat::none;
    __block uint64_t pointerValueAdd = 0;
    // With subCaches, the first cache file might not have any slide info.  In that case, walk all the files
    // until we find one with slide info
    forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
        cache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *mappingPagesStart, uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfoHeader) {
            if ( slideInfoHeader->version == 1 ) {
                pointerFormat   = VMAddrConverter::SharedCacheFormat::v1;
                pointerValueAdd = 0;
            } else if ( slideInfoHeader->version == 2 ) {
                const dyld_cache_slide_info2* slideInfo = (dyld_cache_slide_info2*)(slideInfoHeader);
                assert(slideInfo->delta_mask == 0x00FFFF0000000000);
                pointerFormat   = VMAddrConverter::SharedCacheFormat::v2_x86_64_tbi;
                pointerValueAdd = slideInfo->value_add;
            } else if ( slideInfoHeader->version == 3 ) {
                pointerFormat   = VMAddrConverter::SharedCacheFormat::v3;
                pointerValueAdd = unslidLoadAddress();
            } else if ( slideInfoHeader->version == 4 ) {
                const dyld_cache_slide_info4* slideInfo = (dyld_cache_slide_info4*)(slideInfoHeader);
                assert(slideInfo->delta_mask == 0x00000000C0000000);
                pointerFormat   = VMAddrConverter::SharedCacheFormat::v4;
                pointerValueAdd = slideInfo->value_add;
            } else if ( slideInfoHeader->version == 5 ) {
                pointerFormat   = VMAddrConverter::SharedCacheFormat::v5;
                pointerValueAdd = unslidLoadAddress();
            } else {
                assert(false);
            }
        });
    });

    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);

    VMAddrConverter vmAddrConverter;
    vmAddrConverter.preferredLoadAddress            = pointerValueAdd;
    vmAddrConverter.slide                           = slide;
    vmAddrConverter.chainedPointerFormat            = 0;
    vmAddrConverter.sharedCacheChainedPointerFormat = pointerFormat;
    vmAddrConverter.contentRebased                  = contentRebased;

    return vmAddrConverter;
}
#endif


bool DyldSharedCache::isSubCachePath(const char* leafName)
{
    const char* firstDot = strchr(leafName, '.');
    // check for files with a suffix, to know wether or not they are sub-caches
    if ( firstDot != NULL ) {
        // skip files that are not of the format "<baseName>.development", as they are sub-caches
        if ( strcmp(firstDot, ".development") != 0 )
            return true;
    }
    return false;
}

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
// mmap() an shared cache file read/only but laid out like it would be at runtime
const DyldSharedCache* DyldSharedCache::mapCacheFile(const char* path,
                                           uint64_t baseCacheUnslidAddress,
                                           uint8_t* buffer)
{
    // We don't need to map R-X as we aren't running the code here, so only allow mapping up to RW
    const uint32_t maxPermissions = VM_PROT_READ | VM_PROT_WRITE;
    struct stat statbuf;
    if ( ::stat(path, &statbuf) ) {
        fprintf(stderr, "Error: stat failed for dyld shared cache at %s\n", path);
        return nullptr;
    }

    int cache_fd = ::open(path, O_RDONLY);
    if (cache_fd < 0) {
        fprintf(stderr, "Error: failed to open shared cache file at %s\n", path);
        return nullptr;
    }

    uint8_t  firstPage[4096];
    if ( ::pread(cache_fd, firstPage, 4096, 0) != 4096 ) {
        fprintf(stderr, "Error: failed to read shared cache file at %s\n", path);
        return nullptr;
    }
    const dyld_cache_header*       header   = (dyld_cache_header*)firstPage;
    if ( strncmp(header->magic, "dyld_v1", 7) != 0 ) {
        fprintf(stderr, "Error: Expected cache file magic to be 'dyld_v1...' in %s\n", path);
        return nullptr;
    }

   if ( header->mappingCount == 0 ) {
        fprintf(stderr, "Error: No mapping in shared cache file at %s\n", path);
        return nullptr;
    }
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)(firstPage + header->mappingOffset);
    const dyld_cache_mapping_info* lastMapping = &mappings[header->mappingCount - 1];

    // Allocate enough space for the cache and all subCaches
    uint64_t subCacheBufferOffset = 0;
    if ( baseCacheUnslidAddress == 0 ) {
        size_t vmSize = (size_t)header->sharedRegionSize;
        // If the size is 0, then we might be looking directly at a sub cache.  In that case just allocate a buffer large
        // enough for its mappings.
        if ( vmSize == 0 ) {
            vmSize = (size_t)(lastMapping->address + lastMapping->size - mappings[0].address);
        }
        vm_address_t result;
        kern_return_t r = ::vm_allocate(mach_task_self(), &result, vmSize, VM_FLAGS_ANYWHERE);
        if ( r != KERN_SUCCESS ) {
            fprintf(stderr, "Error: failed to allocate space to load shared cache file at %s\n", path);
            return nullptr;
        }
        buffer = (uint8_t*)result;
    } else {
        subCacheBufferOffset = mappings[0].address - baseCacheUnslidAddress;
    }

    for (uint32_t i=0; i < header->mappingCount; ++i) {
        uint64_t mappingAddressOffset = mappings[i].address - mappings[0].address;
        void* mapped_cache = ::mmap((void*)(buffer + mappingAddressOffset + subCacheBufferOffset), (size_t)mappings[i].size,
                                    mappings[i].maxProt & maxPermissions, MAP_FIXED | MAP_PRIVATE, cache_fd, mappings[i].fileOffset);
        if (mapped_cache == MAP_FAILED) {
            fprintf(stderr, "Error: mmap() for shared cache at %s failed, errno=%d\n", path, errno);
            return nullptr;
        }
    }
    ::close(cache_fd);

    return (DyldSharedCache*)(buffer + subCacheBufferOffset);
}

std::vector<const DyldSharedCache*> DyldSharedCache::mapCacheFiles(const char* path)
{
    const DyldSharedCache* cache = DyldSharedCache::mapCacheFile(path, 0, nullptr);
    if ( cache == nullptr )
        return {};

    std::vector<const DyldSharedCache*> caches;
    caches.push_back(cache);

    std::string basePath = std::string(path);
    if ( cache->header.cacheType == kDyldSharedCacheTypeUniversal )
    {
        std::size_t pos = basePath.find(DYLD_SHARED_CACHE_DEVELOPMENT_EXT);
        if (pos != std::string::npos)
            basePath = basePath.substr(0, basePath.size() - 12);
    }
    // Load all subcaches, if we have them
    if ( cache->header.mappingOffset >= offsetof(dyld_cache_header, subCacheArrayCount) ) {
        if ( cache->header.subCacheArrayCount != 0 ) {
            const dyld_subcache_entry* subCacheEntries = (dyld_subcache_entry*)((uint8_t*)cache + cache->header.subCacheArrayOffset);
            bool hasCacheSuffix = cache->header.mappingOffset > offsetof(dyld_cache_header, cacheSubType);

            for (uint32_t i = 0; i != cache->header.subCacheArrayCount; ++i) {
                std::string subCachePath = std::string(path) + "." + json::unpaddedDecimal(i + 1);
                if ( hasCacheSuffix ) {
                    subCachePath = basePath + subCacheEntries[i].fileSuffix;
                }
                const DyldSharedCache* subCache = DyldSharedCache::mapCacheFile(subCachePath.c_str(), cache->unslidLoadAddress(), (uint8_t*)cache);
                if ( subCache == nullptr )
                    return {};

                uint8_t uuid[16];
                cache->getSubCacheUuid(i, uuid);
                if ( memcmp(subCache->header.uuid, uuid, 16) != 0 ) {
                    uuid_string_t expectedUUIDString;
                    uuid_unparse_upper(uuid, expectedUUIDString);
                    uuid_string_t foundUUIDString;
                    uuid_unparse_upper(subCache->header.uuid, foundUUIDString);
                    fprintf(stderr, "Error: SubCache[%i] UUID mismatch.  Expected %s, got %s\n", i, expectedUUIDString, foundUUIDString);
                    return {};
                }

                caches.push_back(subCache);
            }
        }
    }

    return caches;
}

void DyldSharedCache::applyCacheRebases() const {
    auto rebaseChainV4 = ^(uint8_t* pageContent, uint16_t startOffset, const dyld_cache_slide_info4* slideInfo)
    {
        const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
        const uintptr_t   valueMask    = ~deltaMask;
        //const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
        const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

        uint32_t pageOffset = startOffset;
        uint32_t delta = 1;
        while ( delta != 0 ) {
            uint8_t* loc = pageContent + pageOffset;
            uintptr_t rawValue = *((uintptr_t*)loc);
            delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
            pageOffset += delta;
            uintptr_t value = (rawValue & valueMask);
            if ( (value & 0xFFFF8000) == 0 ) {
               // small positive non-pointer, use as-is
            }
            else if ( (value & 0x3FFF8000) == 0x3FFF8000 ) {
               // small negative non-pointer
               value |= 0xC0000000;
            }
            else {
                // We don't want to fix up pointers, just the stolen integer slots above
                // value += valueAdd;
                // value += slideAmount;
                continue;
            }
            *((uintptr_t*)loc) = value;
            //dyld::log("         pageOffset=0x%03X, loc=%p, org value=0x%08llX, new value=0x%08llX, delta=0x%X\n", pageOffset, loc, (uint64_t)rawValue, (uint64_t)value, delta);
        }
    };

    // On watchOS, the slide info v4 format steals high bits of integers.  We need to undo these
    this->forEachCache(^(const DyldSharedCache *subCache, bool& stopCache) {
        subCache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *dataPagesStart,
                                      uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfo) {
            if ( slideInfo->version == 4) {
                const dyld_cache_slide_info4* slideHeader = (dyld_cache_slide_info4*)slideInfo;
                const uint32_t  page_size = slideHeader->page_size;
                const uint16_t* page_starts = (uint16_t*)((long)(slideInfo) + slideHeader->page_starts_offset);
                const uint16_t* page_extras = (uint16_t*)((long)(slideInfo) + slideHeader->page_extras_offset);
                for (int i=0; i < slideHeader->page_starts_count; ++i) {
                    uint8_t* page = (uint8_t*)(long)(dataPagesStart + (page_size*i));
                    uint16_t pageEntry = page_starts[i];
                    //dyld::log("page[%d]: page_starts[i]=0x%04X\n", i, pageEntry);
                    if ( pageEntry == DYLD_CACHE_SLIDE4_PAGE_NO_REBASE )
                        continue;
                    if ( pageEntry & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA ) {
                        uint16_t chainIndex = (pageEntry & DYLD_CACHE_SLIDE4_PAGE_INDEX);
                        bool done = false;
                        while ( !done ) {
                            uint16_t pInfo = page_extras[chainIndex];
                            uint16_t pageStartOffset = (pInfo & DYLD_CACHE_SLIDE4_PAGE_INDEX)*4;
                            //dyld::log("     chain[%d] pageOffset=0x%03X\n", chainIndex, pageStartOffset);
                            rebaseChainV4(page, pageStartOffset, slideHeader);
                            done = (pInfo & DYLD_CACHE_SLIDE4_PAGE_EXTRA_END);
                            ++chainIndex;
                        }
                    }
                    else {
                        uint32_t pageOffset = pageEntry * 4;
                        //dyld::log("     start pageOffset=0x%03X\n", pageOffset);
                        rebaseChainV4(page, pageOffset, slideHeader);
                    }
                }
            }
        });
    });
}

#endif
const dyld_cache_slide_info* DyldSharedCache::legacyCacheSlideInfo() const
{
    assert(header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset));
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);

    uint64_t offsetInLinkEditRegion = (header.slideInfoOffsetUnused - mappings[2].fileOffset);
    return (dyld_cache_slide_info*)((uint8_t*)(mappings[2].address) + slide + offsetInLinkEditRegion);
}

const dyld_cache_mapping_info* DyldSharedCache::legacyCacheDataRegionMapping() const
{
    assert(header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset));
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    return &mappings[1];
}

const uint8_t* DyldSharedCache::legacyCacheDataRegionBuffer() const
{
    assert(header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset));
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    uintptr_t slide = (uintptr_t)this - (uintptr_t)(mappings[0].address);
    
    return (uint8_t*)(legacyCacheDataRegionMapping()->address) + slide;
}

const void* DyldSharedCache::objcOptPtrs() const
{
    // Find the objc image
    const Header* objcHdr = nullptr;
    uint32_t imageIndex;
    if ( this->hasImagePath("/usr/lib/libobjc.A.dylib", imageIndex) ) {
        uint64_t mTime;
        uint64_t inode;
        objcHdr = (const Header*)this->getIndexedImageEntry(imageIndex, mTime, inode);
    }
    else {
        return nullptr;
    }

    // If we found the objc image, then try to find the read-only data inside.
    __block const void* objcPointersContent = nullptr;
    int64_t slide = objcHdr->getSlide();
    uint32_t pointerSize = objcHdr->pointerSize();
    objcHdr->forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( !info.segmentName.starts_with("__DATA") && !info.segmentName.starts_with("__AUTH") )
            return;
        if ( info.sectionName != "__objc_opt_ptrs" )
            return;
        if ( info.size != pointerSize ) {
            stop = true;
            return;
        }
        objcPointersContent = (uint8_t*)(info.address + slide);
    });

    return objcPointersContent;
}

bool DyldSharedCache::hasOptimizedObjC() const
{
    return (this->objcOpts() != nullptr) || (this->oldObjcOpt() != nullptr);
}

uint32_t DyldSharedCache::objcOptVersion() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() )
        return opts->version;
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->version;
    return 0;
}

uint32_t DyldSharedCache::objcOptFlags() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() )
        return opts->flags;
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->flags;
    return 0;
}

const void* DyldSharedCache::objcRelativeMethodListsBaseAddress() const
{
    if ( const ObjCOptimizationHeader* opts = this->objcOpts() ) {
        if ( opts->relativeMethodSelectorBaseAddressOffset != 0 )
            return (const void*)((char*)this + opts->relativeMethodSelectorBaseAddressOffset);
        return nullptr;
    }
    if ( const objc_opt::objc_opt_t* opts = this->oldObjcOpt() )
        return opts->relativeMethodListsBaseAddress();
    return nullptr;
}

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
uint64_t DyldSharedCache::sharedCacheRelativeSelectorBaseVMAddress() const {
    const void* value = this->objcRelativeMethodListsBaseAddress();
    if ( !value )
        return 0;

    uint64_t vmOffset = (uint64_t)value - (uint64_t)this;
    return this->unslidLoadAddress() + vmOffset;
}
#endif

std::pair<const void*, uint64_t> DyldSharedCache::getObjCConstantRange() const
{
    uint32_t imageIndex;
    if ( this->hasImagePath("/usr/lib/system/libdyld.dylib", imageIndex) ) {
        uint64_t mTime;
        uint64_t inode;
        const dyld3::MachOAnalyzer* libDyldMA = (dyld3::MachOAnalyzer*)this->getIndexedImageEntry(imageIndex, mTime, inode);
        std::pair<const void*, uint64_t> ranges = { nullptr, 0 };
#if TARGET_OS_OSX
        ranges.first = libDyldMA->findSectionContent("__DATA", "__objc_ranges", ranges.second);
#else
        ranges.first = libDyldMA->findSectionContent("__DATA_CONST", "__objc_ranges", ranges.second);
#endif
        return ranges;
    }
    return { nullptr, 0 };
}

bool DyldSharedCache::hasSlideInfo() const {
    if ( header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset) ) {
        return header.slideInfoSizeUnused != 0;
    } else {
        const dyld_cache_mapping_and_slide_info* slidableMappings = (const dyld_cache_mapping_and_slide_info*)((char*)this + header.mappingWithSlideOffset);
        for (uint32_t i = 0; i != header.mappingWithSlideCount; ++i) {
            if ( slidableMappings[i].slideInfoFileSize != 0 ) {
                return true;
            }
        }
    }
    return false;
}
#endif // !TARGET_OS_EXCLAVEKIT

void DyldSharedCache::forEachSlideInfo(void (^handler)(uint64_t mappingStartAddress, uint64_t mappingSize,
                                                       const uint8_t* mappingPagesStart,
                                                       uint64_t slideInfoOffset, uint64_t slideInfoSize,
                                                       const dyld_cache_slide_info* slideInfoHeader)) const {
#if  !TARGET_OS_EXCLAVEKIT
    if ( header.mappingOffset <= offsetof(dyld_cache_header, mappingWithSlideOffset) ) {
        // Old caches should get the slide info from the cache header and assume a single data region.
        const dyld_cache_mapping_info* dataMapping = legacyCacheDataRegionMapping();
        uint64_t dataStartAddress = dataMapping->address;
        uint64_t dataSize = dataMapping->size;
        const uint8_t* dataPagesStart = legacyCacheDataRegionBuffer();
        const dyld_cache_slide_info* slideInfoHeader = legacyCacheSlideInfo();

        handler(dataStartAddress, dataSize, dataPagesStart,
                header.slideInfoOffsetUnused, header.slideInfoSizeUnused, slideInfoHeader);
    }
    else
#endif
    {
        const dyld_cache_mapping_and_slide_info* slidableMappings = (const dyld_cache_mapping_and_slide_info*)((char*)this + header.mappingWithSlideOffset);
        const dyld_cache_mapping_and_slide_info* linkeditMapping = &slidableMappings[header.mappingWithSlideCount - 1];
        uint64_t sharedCacheSlide = (uint64_t)this - unslidLoadAddress();

        for (uint32_t i = 0; i != header.mappingWithSlideCount; ++i) {
            if ( slidableMappings[i].slideInfoFileOffset != 0 ) {
                // Get the data pages
                uint64_t dataStartAddress = slidableMappings[i].address;
                uint64_t dataSize = slidableMappings[i].size;
                const uint8_t* dataPagesStart = (uint8_t*)dataStartAddress + sharedCacheSlide;

                // Get the slide info
                uint64_t offsetInLinkEditRegion = (slidableMappings[i].slideInfoFileOffset - linkeditMapping->fileOffset);
                const dyld_cache_slide_info* slideInfoHeader = (dyld_cache_slide_info*)((uint8_t*)(linkeditMapping->address) + sharedCacheSlide + offsetInLinkEditRegion);
                handler(dataStartAddress, dataSize, dataPagesStart,
                        slidableMappings[i].slideInfoFileOffset, slidableMappings[i].slideInfoFileSize, slideInfoHeader);
            }
        }
    }
}

#if !TARGET_OS_EXCLAVEKIT
const char* DyldSharedCache::getCanonicalPath(const char *path) const
{
    uint32_t dyldCacheImageIndex;
    if ( hasImagePath(path, dyldCacheImageIndex) )
        return getIndexedImagePath(dyldCacheImageIndex);
    return nullptr;
}

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
void DyldSharedCache::fillMachOAnalyzersMap(std::unordered_map<std::string,dyld3::MachOAnalyzer*> & dylibAnalyzers) const {
    forEachImage(^(const Header *hdr, const char *iteratedInstallName) {
        dylibAnalyzers[std::string(iteratedInstallName)] = (dyld3::MachOAnalyzer*)hdr;
    });
}

void DyldSharedCache::computeReverseDependencyMapForDylib(std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap, const std::unordered_map<std::string,dyld3::MachOAnalyzer*> & dylibAnalyzers, const std::string &loadPath) const {
    dyld3::MachOAnalyzer *ma = dylibAnalyzers.at(loadPath);
    if (reverseDependencyMap.find(loadPath) != reverseDependencyMap.end()) return;
    reverseDependencyMap[loadPath] = std::set<std::string>();

    ma->forEachDependentDylib(^(const char *dependencyLoadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool &stop) {
        if (isUpward) return;
        std::string dependencyLoadPathString = std::string(dependencyLoadPath);
        computeReverseDependencyMapForDylib(reverseDependencyMap, dylibAnalyzers, dependencyLoadPathString);
        reverseDependencyMap[dependencyLoadPathString].insert(loadPath);
    });
}

// Walks the shared cache and construct the reverse dependency graph (if dylib A depends on B,
// constructs the graph with B -> A edges)
void DyldSharedCache::computeReverseDependencyMap(std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap) const {
    std::unordered_map<std::string,dyld3::MachOAnalyzer*> dylibAnalyzers;

    fillMachOAnalyzersMap(dylibAnalyzers);
    forEachImage(^(const Header *hdr, const char *installName) {
        computeReverseDependencyMapForDylib(reverseDependencyMap, dylibAnalyzers, std::string(installName));
    });
}

// uses the reverse dependency graph constructed above to find the recursive set of dependents for each dylib
void DyldSharedCache::findDependentsRecursively(std::unordered_map<std::string, std::set<std::string>> &transitiveDependents, const std::unordered_map<std::string, std::set<std::string>> &reverseDependencyMap, std::set<std::string> & visited, const std::string &loadPath) const {

    if (transitiveDependents.find(loadPath) != transitiveDependents.end()) {
        return;
    }

    if (visited.find(loadPath) != visited.end()) {
        return;
    }

    visited.insert(loadPath);

    std::set<std::string> dependents;

    for (const std::string & dependent : reverseDependencyMap.at(loadPath)) {
        findDependentsRecursively(transitiveDependents, reverseDependencyMap, visited, dependent);
        if (transitiveDependents.find(dependent) != transitiveDependents.end()) {
            std::set<std::string> & theseTransitiveDependents = transitiveDependents.at(dependent);
            dependents.insert(theseTransitiveDependents.begin(), theseTransitiveDependents.end());
        }
        dependents.insert(dependent);
    }

    transitiveDependents[loadPath] = dependents;
}

// Fills a map from each install name N to the set of install names depending on N
void DyldSharedCache::computeTransitiveDependents(std::unordered_map<std::string, std::set<std::string>> & transitiveDependents) const {
    std::unordered_map<std::string, std::set<std::string>> reverseDependencyMap;
    computeReverseDependencyMap(reverseDependencyMap);
    forEachImage(^(const Header *hdr, const char *installName) {
        std::set<std::string> visited;
        findDependentsRecursively(transitiveDependents, reverseDependencyMap, visited, std::string(installName));
    });
}
#endif



DyldSharedCache::DynamicRegion* DyldSharedCache::DynamicRegion::make(uintptr_t prefAddress)
{
    // allocate page for DynamicRegion
    DynamicRegion* dynamicRegion = nullptr;
    if ( prefAddress == 0 ) {
        // for system wide cache (loaded in launchd) we allocate a page at a random address 
        // and __shared_region_map_and_slide_2_np() copies to a where the cache is mapped
        vm_address_t  dynamicConfigData = 0;
        kern_return_t kr = ::vm_allocate(mach_task_self(), &dynamicConfigData, size(), VM_FLAGS_ANYWHERE);
        if ( kr != KERN_SUCCESS )
            return nullptr;
        dynamicRegion = (DynamicRegion*)dynamicConfigData;
    }
    else {
        // for private caches it is at a specified address
        void* mapResult = ::mmap((void*)prefAddress, size(), VM_PROT_READ | VM_PROT_WRITE, MAP_ANON | MAP_FIXED | MAP_PRIVATE, -1, 0);
        if ( mapResult == MAP_FAILED)
            return nullptr;
        dynamicRegion = (DynamicRegion*)mapResult;
    }

    // initialize header of dynamic data
    strcpy(dynamicRegion->_magic, sMagic);

    return dynamicRegion;
}

uint32_t DyldSharedCache::DynamicRegion::version() const
{
    return _magic[14] - '0';
}

void DyldSharedCache::DynamicRegion::free()
{
    ::vm_deallocate(mach_task_self(), (vm_address_t)this, (vm_size_t)size());
}

bool DyldSharedCache::DynamicRegion::validMagic() const
{
    return (memcmp(_magic, sMagic, 14) == 0);   // don't compare last char (version num)
}

size_t DyldSharedCache::DynamicRegion::size()
{
    static_assert(sizeof(DynamicRegion) < 0x4000);
    return 0x4000;
}

void DyldSharedCache::DynamicRegion::setDyldCacheFileID(FileIdTuple ids)
{
    _dyldCache = ids;
}

void DyldSharedCache::DynamicRegion::setOSCryptexPath(const char* path)
{
    assert(_osCryptexPathOffset == 0); // Make sure we have not already set a cryptexPath
    assert(_cachePathOffset == 0); // setCachePath() uses _osCryptexPathOffset, so if it has already been set then this will corrupt it
    _osCryptexPathOffset = sizeof(DynamicRegion);
    strlcpy(((char*)this)+_osCryptexPathOffset, path, size()-_osCryptexPathOffset);
}

void DyldSharedCache::DynamicRegion::setCachePath(const char* path) {
    assert(_cachePathOffset == 0);
    _cachePathOffset = sizeof(DynamicRegion);
    if (const char* cryptexPath = osCryptexPath()) {
        _cachePathOffset += (sizeof(cryptexPath) + 1);
    }
    strlcpy(((char*)this)+_cachePathOffset, path, size()-_cachePathOffset);
}


void DyldSharedCache::DynamicRegion::setReadOnly()
{
    ::mprotect(this, size(), VM_PROT_READ);
}

void DyldSharedCache::DynamicRegion::setSystemWideFlags(__uint128_t flags)
{
    _systemWideFunctionVariantFlags = flags;
}

void DyldSharedCache::DynamicRegion::setProcessorFlags(__uint128_t flags)
{
    _processorFunctionVariantFlags = flags;
}

bool DyldSharedCache::DynamicRegion::getDyldCacheFileID(FileIdTuple& ids) const
{
    if ( !_dyldCache )
        return false;

    ids = _dyldCache;
    return true;
}

__uint128_t DyldSharedCache::DynamicRegion::getSystemWideFunctionVariantFlags() const
{
    return _systemWideFunctionVariantFlags;
}

__uint128_t DyldSharedCache::DynamicRegion::getProcessorFunctionVariantFlags() const
{
    return _processorFunctionVariantFlags;
}


const char* DyldSharedCache::DynamicRegion::osCryptexPath() const
{
    if (!_osCryptexPathOffset)
        return nullptr;

    return ((char*)this)+_osCryptexPathOffset;
}
    

const char* DyldSharedCache::DynamicRegion::cachePath() const
{
    if (!_cachePathOffset)
        return nullptr;

    return ((char*)this)+_cachePathOffset;
}

FileIdTuple::FileIdTuple(const char* path)
{
    struct stat sb;
    if ( ::stat(path, &sb) == -1 )
        return;
    init(sb);
}

FileIdTuple::FileIdTuple(const struct stat& sb)
{
    init(sb);
}

void FileIdTuple::init(const struct stat& sb)
{
    memcpy(&fsobjid, &sb.st_ino, 8);
    fsid.val[0]             = sb.st_dev;
    fsid.val[1]             = 0;
}

FileIdTuple::FileIdTuple(uint64_t fsidScalar, uint64_t fsobjidScalar) {
    memcpy(&fsid, &fsidScalar, 8);
    memcpy(&fsobjid, &fsobjidScalar, 8);
}

uint64_t FileIdTuple::inode() const
{
    uint64_t result;
    memcpy(&result, &fsobjid, 8);
    return result;
}

uint64_t FileIdTuple::fsID() const
{
    return fsid.val[0];
}

FileIdTuple::operator bool() const
{
    return (fsid.val[0] != 0) && (fsobjid.fid_objno != 0);
}

bool FileIdTuple::operator==(const FileIdTuple& other) const
{
    return (fsid.val[0] == other.fsid.val[0]) && (fsid.val[1] == other.fsid.val[1])
        && (fsobjid.fid_objno == other.fsobjid.fid_objno) && (fsobjid.fid_generation == other.fsobjid.fid_generation);
}

bool FileIdTuple::getPath(char pathBuff[PATH_MAX]) const
{
    if ( ::fsgetpath(pathBuff, PATH_MAX, (fsid_t*)&fsid, inode()) != -1 )
        return true;
    return false;
}

#endif // !TARGET_OS_EXCLAVEKIT

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

// Helpers to handle the JSON map file
struct MapFile
{
    std::string archName;
    std::string platformName;
    std::vector<std::string> imagePaths;
};

static MapFile parseMapFile(Diagnostics& diags, json::Node& mapNode)
{
    MapFile mapFile;

    // Top level node should be a map of the version and files
    if ( mapNode.map.empty() ) {
        diags.error("Expected map for JSON cache map node\n");
        return { };
    }

    // Parse the nodes in the top level manifest node
    const json::Node& versionMapNode  = json::getRequiredValue(diags, mapNode, "version");
    uint64_t mapVersion               = json::parseRequiredInt(diags, versionMapNode);
    if ( diags.hasError() )
        return { };

    const uint64_t supportedMapVersion = 1;
    if ( mapVersion != supportedMapVersion ) {
        diags.error("JSON map version of %lld is unsupported.  Supported version is %lld\n",
                    mapVersion, supportedMapVersion);
        return { };
    }

    // Parse arch if we have it
    if ( const json::Node* archNode = json::getOptionalValue(diags, mapNode, "arch") )
        mapFile.archName = archNode->value;

    // Parse arch if we have it
    if ( const json::Node* platformNode = json::getOptionalValue(diags, mapNode, "platform") )
        mapFile.platformName = platformNode->value;

    // Parse the images
    const json::Node& imagesNode = json::getRequiredValue(diags, mapNode, "images");
    if ( diags.hasError() )
        return { };
    if ( imagesNode.array.empty() ) {
        diags.error("Images node is not an array\n");
        return { };
    }

    for (const json::Node& imageNode : imagesNode.array) {
        const json::Node& pathNode = json::getRequiredValue(diags, imageNode, "path");
        if (pathNode.value.empty()) {
            diags.error("Image path node is not a string\n");
            return { };
        }
        mapFile.imagePaths.push_back(pathNode.value);
    }

    return mapFile;
}

BaselineCachesChecker::BaselineCachesChecker(std::vector<const char*> archs, mach_o::Platform platform)
{
    this->_archs.insert(this->_archs.end(), archs.begin(), archs.end());
    this->_platform = platform;
}

mach_o::Error BaselineCachesChecker::addBaselineMap(std::string_view path)
{
    Diagnostics diags;
    json::Node mapNode = json::readJSON(diags, path.data(), false /* useJSON5 */);
    if ( diags.hasError() )
        return mach_o::Error("%s", diags.errorMessageCStr());

    MapFile mapFile = parseMapFile(diags, mapNode);
    if ( diags.hasError() )
        return mach_o::Error("%s", diags.errorMessageCStr());

    std::string archName = mapFile.archName;
    if ( mapFile.archName.empty() ) {
        // HACK: Add an arch to the JSON, but for now use the path.
        if ( path.find(".arm64e.") != std::string::npos )
            archName = "arm64e";
        else if ( path.find(".arm64.") != std::string::npos )
            archName = "arm64";
        else if ( path.find(".arm64_32.") != std::string::npos )
            archName = "arm64_32";
        else if ( path.find(".x86_64.") != std::string::npos )
            archName = "x86_64";
        else if ( path.find(".x86_64h.") != std::string::npos )
            archName = "x86_64h";
    }

    for ( const std::string& imagePath : mapFile.imagePaths ) {
        this->_unionBaselineDylibs.insert(imagePath);
        if ( !archName.empty() )
            this->_baselineDylibs[archName].push_back(imagePath);
    }

    return mach_o::Error::none();
}

mach_o::Error BaselineCachesChecker::addBaselineMaps(std::string_view dirPath)
{
    // Make sure the directory exists and is a directory
    {
        struct stat statbuf;
        if ( ::stat(dirPath.data(), &statbuf) )
            return mach_o::Error("stat failed for cache maps path at '%s', due to '%s'", dirPath.data(), strerror(errno));

        if ( !S_ISDIR(statbuf.st_mode) )
            return mach_o::Error("cache maps path was not a directory at '%s'", dirPath.data());
    }

    // Walk the directory and parse all the JSON files we find
    __block std::vector<std::string> filePaths;
    auto dirFilter = ^(const std::string& path) { return false; };
    auto fileHandler = ^(const std::string& path, const struct stat& statBuf) {
        filePaths.push_back(path);
    };
    iterateDirectoryTree("", dirPath.data(), dirFilter, fileHandler, true /* process files */, false /* recurse */);

    if ( filePaths.empty() )
        return mach_o::Error("no files found in cache map directory '%s'", dirPath.data());

    for ( std::string_view filePath : filePaths ) {
        if ( !filePath.ends_with(".json") ) {
            fprintf(stderr, "warning: skipping cache map without .json extension: '%s'\n", filePath.data());
            continue;
        }

        Diagnostics diags;
        json::Node mapNode = json::readJSON(diags, filePath.data(), false /* useJSON5 */);
        if ( diags.hasError() )
            return mach_o::Error("could not read cache map '%s': '%s'", filePath.data(), diags.errorMessageCStr());

        MapFile mapFile = parseMapFile(diags, mapNode);
        if ( diags.hasError() )
            return mach_o::Error("could not parse cache map '%s': '%s'", filePath.data(), diags.errorMessageCStr());

        if ( mapFile.archName.empty() )
            return mach_o::Error("cache map does contain an arch '%s'", filePath.data());

        if ( mapFile.platformName.empty() )
            return mach_o::Error("cache map does contain a platform '%s'", filePath.data());

        if ( mapFile.platformName != this->_platform.name() ) {
            fprintf(stderr, "warning: skipping cache map for different platform (%s vs %s): '%s'\n",
                    mapFile.platformName.c_str(), this->_platform.name().c_str(), filePath.data());
            continue;
        }

        if ( std::find(this->_archs.begin(), this->_archs.end(), mapFile.archName) == this->_archs.end() ) {
            fprintf(stderr, "warning: skipping cache map for different arch (%s): '%s'\n",
                    mapFile.archName.c_str(), filePath.data());
            continue;
        }

        printf("found cache map: %s\n", filePath.data());

        for ( const std::string& imagePath : mapFile.imagePaths ) {
            this->_unionBaselineDylibs.insert(imagePath);
            this->_baselineDylibs[mapFile.archName].push_back(imagePath);
        }
    }

    if ( this->_baselineDylibs.empty() )
        return mach_o::Error("no dylibs found in cache maps in '%s'", dirPath.data());

    if ( !allBaselineArchsPresent() )
        return mach_o::Error("missing baseline maps for some archs/platforms '%s'", dirPath.data());

    return mach_o::Error::none();
}

mach_o::Error BaselineCachesChecker::addNewMap(std::string_view mapString)
{
    Diagnostics diags;
    json::Node mapNode = json::readJSON(diags, mapString.data(), mapString.size(), false /* useJSON5 */);
    if ( diags.hasError() )
        return mach_o::Error("%s", diags.errorMessageCStr());

    MapFile mapFile = parseMapFile(diags, mapNode);
    if ( mapFile.archName.empty() )
        return mach_o::Error("expected arch name in cache file map");

    for ( const std::string& imagePath : mapFile.imagePaths ) {
        this->_newDylibs[mapFile.archName].insert(imagePath);
    }

    return mach_o::Error::none();
}

void BaselineCachesChecker::setFilesFromNewCaches(std::span<const char* const> files)
{
    for ( const char* file : files )
        this->_dylibsInNewCaches.insert(file);
}

bool BaselineCachesChecker::allBaselineArchsPresent() const
{
    for ( const std::string& arch : this->_archs ) {
        if ( this->_baselineDylibs.find(arch) == this->_baselineDylibs.end() )
            return false;
    }

    return true;
}

std::set<std::string> BaselineCachesChecker::dylibsMissingFromNewCaches() const
{
    std::set<std::string> result;

    // Check if we have map files for all archs we are building.
    // If we have all of them, then we can check them individually, but otherwise
    // we need to union them all to be conservative
    bool checkIndividualMaps = allBaselineArchsPresent();

    if ( checkIndividualMaps ) {
        // Walk all the dylibs in the baseline and new caches and compare if anything is missing an arch
        for ( const std::string& arch : this->_archs ) {
            auto baselineIt = this->_baselineDylibs.find(arch);
            auto newIt = this->_newDylibs.find(arch);
            if ( baselineIt == this->_baselineDylibs.end() )
                return { };
            if ( newIt == this->_newDylibs.end() )
                return { };

            // If a dylib is in the baseline, but not the corresponding new cache, then we need
            // to add it
            for ( const std::string& imagePath : baselineIt->second ) {
                if ( newIt->second.count(imagePath) == 0 )
                    result.insert(imagePath);
            }
        }
    } else {
        // TODO: Remove this old code once we always have an arch name
        std::set<std::string> simulatorSupportDylibs;
        if ( this->_platform == mach_o::Platform::macOS ) {
            //FIXME: We should be using MH_SIM_SUPPORT now that all the relevent binaries include it in their headers
            // macOS has to leave the simulator support binaries on disk
            // It won't put them in the result of getFilesToRemove() so we need to manually add them
            simulatorSupportDylibs.insert("/usr/lib/system/libsystem_kernel.dylib");
            simulatorSupportDylibs.insert("/usr/lib/system/libsystem_platform.dylib");
            simulatorSupportDylibs.insert("/usr/lib/system/libsystem_pthread.dylib");
        }

        for (const std::string& baselineDylib : this->_unionBaselineDylibs) {
            if ( !this->_dylibsInNewCaches.count(baselineDylib) && !simulatorSupportDylibs.count(baselineDylib))
                result.insert(baselineDylib);
        }
    }

    return result;
}

#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
