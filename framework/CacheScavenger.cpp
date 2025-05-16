/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2025 Apple Inc. All rights reserved.
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

#include <fcntl.h>
#include <stdio.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <uuid/uuid.h>

#include "AAREncoder.h"
#include "Allocator.h"
#include "PropertyList.h"
#include "SnapshotShared.h"
#include "Header.h"
#include "DyldSharedCache.h"
#include "Vector.h"

#include "ProcessScavenger.h"

#if TARGET_OS_OSX
namespace {
struct CacheMapping {
    void* address;
    size_t fileSize;
    size_t vmSize;
    uint64_t preferredLoadAddress;
};
void addSubCacheFileInfo(uint64_t cacheVMAddress, PropertyList::Array &files, dyld_cache_header* subcacheHeader, CacheMapping& cacheMapping, std::string fileName) {
    using Array         = PropertyList::Array;
    using Integer       = PropertyList::Integer;
    using String        = PropertyList::String;
    using Dictionary    = PropertyList::Dictionary;

    auto& subCacheFile = files.addObject<Dictionary>();

    subCacheFile.addObjectForKey<String>("name", fileName);
    subCacheFile.addObjectForKey<PropertyList::UUID>(kDyldAtlasSharedCacheUUIDKey,subcacheHeader->uuid);
    subCacheFile.addObjectForKey<Integer>("voff",subcacheHeader->sharedRegionStart-cacheVMAddress);
    subCacheFile.addObjectForKey<Integer>("fsze", cacheMapping.fileSize);
    subCacheFile.addObjectForKey<Integer>("padr", subcacheHeader->sharedRegionStart);
    auto& mappingsArray = subCacheFile.addObjectForKey<Array>(kDyldAtlasSharedCacheMappingArrayKey);
    auto* mappings                      = (dyld_cache_mapping_info*)((uint8_t*)subcacheHeader + subcacheHeader->mappingOffset);

    uint64_t lastAddress = 0;
    for ( auto i = 0; i < subcacheHeader->mappingCount; ++i) {
        auto& mapping = mappingsArray.addObject<Dictionary>();
        mapping.addObjectForKey<Integer>(kDyldAtlasSharedCacheMappingsSizeKey, mappings[i].size);
        mapping.addObjectForKey<Integer>(kDyldAtlasSharedCacheMappingsPreferredLoadAddressKey, mappings[i].address);
        mapping.addObjectForKey<Integer>(kDyldAtlasSharedCacheMappingsFileOffsetKey, mappings[i].fileOffset);
        mapping.addObjectForKey<Integer>(kDyldAtlasSharedCacheMappingsMaxProtKey, mappings[i].maxProt);
        if (mappings[i].address + mappings[i].size > lastAddress) {
            lastAddress = mappings[i].address + mappings[i].size;
        }
    }
    cacheMapping.vmSize = lastAddress-subcacheHeader->sharedRegionStart;
    subCacheFile.addObjectForKey<Integer>("size", cacheMapping.vmSize);
}

static
CacheMapping mapFile(std::string dir, std::string name) {
    std::string fullPath = dir + "/" + name;
    int fd = open(fullPath.c_str(), O_RDONLY);
    if (fd < 0) {
        return { nullptr, 0, 0 };
    }
    struct stat stat_buf;
    if (fstat(fd, &stat_buf) != 0) {
        close(fd);
        return { nullptr, 0, 0 };
    }
    void* mapping = mmap(nullptr, stat_buf.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) {
        return { nullptr, 0 };
    }
    dyld_cache_header* cacheHeader = (dyld_cache_header*)mapping;

    return { mapping, static_cast<size_t>(stat_buf.st_size), 0, cacheHeader->sharedRegionStart };
}

static
void unmapFile(CacheMapping& mapping) {
    if (mapping.address == nullptr) { return; }
    if (mapping.fileSize == 0)          { return; }
    munmap(mapping.address, mapping.fileSize);
}

static
std::span<const dyld_cache_image_info> cacheImageInfos(dyld_cache_header* header) {
    if ( header->mappingOffset >= offsetof(dyld_cache_header, imagesCount) ) {
        dyld_cache_image_info* start = (dyld_cache_image_info*)((char*)header + header->imagesOffset);
        dyld_cache_image_info* end = &start[header->imagesCount];
        return { start, end };
    }
    dyld_cache_image_info* start = (dyld_cache_image_info*)((char*)header + header->imagesOffsetOld);
    dyld_cache_image_info* end = &start[header->imagesCount];
    return { start, end };
}

static
std::span<const dyld_cache_image_text_info> cacheTextImageSegments(dyld_cache_header* header)
{
    // check for old cache without imagesText array
    if ( (header->mappingOffset <= offsetof(dyld_cache_header, imagesTextOffset)) || (header->imagesTextCount == 0) )
        return { };

    const dyld_cache_image_text_info* imagesText = (dyld_cache_image_text_info*)((char*)header + header->imagesTextOffset);
    const dyld_cache_image_text_info* imagesTextEnd = &imagesText[header->imagesTextCount];
    return { imagesText, imagesTextEnd };
}

static
void scavengeCache(const char* path, ByteStream& byteStream) {
    char buffer[MAXPATHLEN];
    const char* dir = strdup(dirname_r(path, buffer));
    const char* basename = strdup(basename_r(path, buffer));
    std::vector<CacheMapping> cacheMappings;

    STACK_ALLOCATOR(allocator, 0);
    using Array         = PropertyList::Array;
    using Dictionary    = PropertyList::Dictionary;
    using Integer       = PropertyList::Integer;
    using String        = PropertyList::String;
    auto propertyListEncoder            = PropertyList(allocator);
    auto& rootDictionary                = propertyListEncoder.rootDictionary();
    // The same plist contains both the customer and shared cache data, since they share layouts
    // We include dictionaries at the root so they can be lookup by leaf name or UUID
    auto& byUuidDictionary              = rootDictionary.addObjectForKey<Dictionary>("uuids");
    auto& byNameDictionary              = rootDictionary.addObjectForKey<Dictionary>("names");
    std::string cacheName               = basename;
    auto mainCacheMapping = mapFile(dir, basename);
    if (mainCacheMapping.address == nullptr) { return; }
    dyld_cache_header* cacheHeader = (dyld_cache_header*)mainCacheMapping.address;
    uuid_string_t cacheUUID             = {0};
    uuid_unparse_upper(cacheHeader->uuid, cacheUUID);
    Dictionary* cacheAtlas              = &byUuidDictionary.addObjectForKey<Dictionary>(cacheUUID);
    byNameDictionary.insertObjectForKey(cacheName, *cacheAtlas);
    cacheAtlas->addObjectForKey<PropertyList::UUID>(kDyldAtlasSharedCacheUUIDKey, cacheHeader->uuid);
    cacheAtlas->addObjectForKey<Integer>(kDyldAtlasSharedCachePreferredLoadAddressKey, cacheHeader->sharedRegionStart);
    cacheAtlas->addObjectForKey<Integer>(kDyldAtlasSharedCacheVMSizeKey, cacheHeader->sharedRegionSize);
    if (!uuid_is_null(cacheHeader->symbolFileUUID)) {
        cacheAtlas->addObjectForKey<String>(kDyldAtlasSharedCacheSymbolFileName, cacheName + ".symbols");
        cacheAtlas->addObjectForKey<PropertyList::UUID>(kDyldAtlasSharedCacheSymbolFileName, cacheHeader->symbolFileUUID);
    }
    // We only support scavenging on `macOS`, and all caches on macOS have 8 byte pointers
    cacheAtlas->addObjectForKey<Integer>("psze", 8);

    auto& files = cacheAtlas->addObjectForKey<Array>("dscs");
    addSubCacheFileInfo(cacheHeader->sharedRegionStart, files, cacheHeader, mainCacheMapping, cacheName);
    cacheMappings.push_back(mainCacheMapping);

    if (cacheHeader->mappingOffset <= offsetof(dyld_cache_header, cacheSubType) ) {
        for (auto i = 0; i < cacheHeader->subCacheArrayCount; ++i) {
            char* fileSuffix = nullptr;
            asprintf(&fileSuffix, "%u", i+1);
            auto subCacheMapping = mapFile(dir, basename + cacheName + fileSuffix);
            dyld_cache_header* subCacheHeader = (dyld_cache_header*)subCacheMapping.address;
            addSubCacheFileInfo(cacheHeader->sharedRegionStart, files, subCacheHeader, subCacheMapping, cacheName + fileSuffix);
            cacheMappings.push_back(subCacheMapping);
            free((void*)fileSuffix);
        }
    } else {
        const dyld_subcache_entry* subCacheEntries = (dyld_subcache_entry*)((uintptr_t)cacheHeader + cacheHeader->subCacheArrayOffset);
        for (auto i = 0; i < cacheHeader->subCacheArrayCount; ++i) {
            auto subCacheMapping = mapFile(dir, cacheName + subCacheEntries[i].fileSuffix);
            dyld_cache_header* subCacheHeader = (dyld_cache_header*)subCacheMapping.address;
            addSubCacheFileInfo(cacheHeader->sharedRegionStart, files, subCacheHeader, subCacheMapping, subCacheEntries[i].fileSuffix);
            cacheMappings.push_back(subCacheMapping);
        }
    }
    Array* images = &cacheAtlas->addObjectForKey<Array>(kDyldAtlasSharedCacheImageArrayKey);
    std::span<const dyld_cache_image_info> cacheImages = cacheImageInfos(cacheHeader);
    std::span<const dyld_cache_image_text_info> cacheTextSegments = cacheTextImageSegments(cacheHeader);

    for (auto i = 0; i < cacheImages.size(); ++i ) {
        auto& image = images->addObject<Dictionary>();
        auto& segments = image.addObjectForKey<Array>(kDyldAtlasImageSegmentArrayKey);
        uint64_t imageAddress = cacheImages[i].address;
        image.addObjectForKey<String>(kDyldAtlasImageInstallnameKey, (const char*)cacheHeader + cacheTextSegments[i].pathOffset);
        image.addObjectForKey<Integer>(kDyldAtlasImagePreferredLoadAddressKey, imageAddress);
        uuid_t uuid;
        const char* uuidBegin = (const char*)&cacheTextSegments[i].uuid[0];
        std::copy(uuidBegin, uuidBegin+16, &uuid[0]);
        image.addObjectForKey<PropertyList::UUID>(kDyldAtlasImageUUIDKey, uuid);
        auto mapping = std::find_if(cacheMappings.begin(), cacheMappings.end(), [&](CacheMapping& cacheMapping) {
            uint64_t startAddress   = (uint64_t)cacheMapping.preferredLoadAddress;
            uint64_t endAddress     = startAddress + cacheMapping.vmSize;
            if (imageAddress < startAddress) { return false; }
            if (imageAddress >= endAddress) { return false; }
            return true;
        });
        assert(mapping != cacheMappings.end());
        uint64_t subcacheImageOffset = imageAddress - mapping->preferredLoadAddress;
        uint8_t* machHeaderAddress = (uint8_t*)mapping->address + subcacheImageOffset;
        std::span<uint8_t> machHeaderSpan = std::span(machHeaderAddress,  (uint8_t*)mapping->address + mapping->fileSize);
        const mach_o::Header* mh = mach_o::Header::isMachO(machHeaderSpan);
        mh->forEachSegment(^(const mach_o::Header::SegmentInfo &info, bool &stop) {
            auto& segment = segments.addObject<Dictionary>();
            segment.addObjectForKey<String>(kDyldAtlasSegmentNameKey, info.segmentName);
            segment.addObjectForKey<Integer>(kDyldAtlasSegmentPreferredLoadAddressKey, info.vmaddr);
            segment.addObjectForKey<Integer>(kDyldAtlasSegmentSizeKey, info.vmsize);
            segment.addObjectForKey<Integer>(kDyldAtlasSegmentFileOffsetKey, info.fileOffset);
            segment.addObjectForKey<Integer>(kDyldAtlasSegmentFileSizeKey, info.fileSize);
            if ( info.segmentName == "__TEXT" ) {
                segment.addObjectForKey<Integer>(kDyldAtlasSegmentPermissionsKey, VM_PROT_READ | VM_PROT_EXECUTE);
            } else if ( info.segmentName == "__LINKEDIT" ) {
                segment.addObjectForKey<Integer>(kDyldAtlasSegmentPermissionsKey, VM_PROT_READ);
            } else {
                segment.addObjectForKey<Integer>(kDyldAtlasSegmentPermissionsKey, VM_PROT_READ | VM_PROT_WRITE);
            }
        });
    }

    for (auto cacheMapping: cacheMappings) {
        unmapFile(cacheMapping);
    }

    ByteStream fileStream(allocator);
    propertyListEncoder.encode(fileStream);
    AAREncoder aarEncoder(allocator);

    std::string plistPath = std::string("caches/uuids/") + cacheUUID + ".plist";
    std::string symlinkTarget = std::string("../uuids/") + cacheUUID + ".plist";
    std::string symlinkSource = std::string("caches/names/") + cacheName + ".plist";

    aarEncoder.addFile(plistPath, fileStream.span());
    aarEncoder.addSymLink(symlinkSource, symlinkTarget);

    ByteStream outputStream(allocator);
    aarEncoder.encode(outputStream);
    std::copy(outputStream.begin(), outputStream.end(), std::back_insert_iterator(byteStream));
}
};
#endif

void* scavengeCache(const char* path, uint64_t* bufferSize) {
#if TARGET_OS_OSX
    STACK_ALLOCATOR(allocator, 0);
    ByteStream outputStream(allocator);
    scavengeCache(path, outputStream);
    *bufferSize = outputStream.size();
    if (*bufferSize == 0) { return nullptr; }
    std::byte* buffer = (std::byte*)malloc((size_t)(*bufferSize));
    std::copy(outputStream.begin(), outputStream.end(), (std::byte*)buffer);
    return (void*)buffer;
#else
    *bufferSize = 0;
    return nullptr;
#endif
}
