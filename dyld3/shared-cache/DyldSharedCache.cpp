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


#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/dyld_priv.h>
#include <assert.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonDigestSPI.h>

#if !DYLD_IN_PROCESS
#include <set>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#endif

#define NO_ULEB
#include "MachOParser.h"
#include "CacheBuilder.h"
#include "DyldSharedCache.h"
#include "LaunchCache.h"
#include "Trie.hpp"
#include "StringUtils.h"



#if !DYLD_IN_PROCESS
DyldSharedCache::CreateResults DyldSharedCache::create(const CreateOptions&             options,
                                                       const std::vector<MappedMachO>&  dylibsToCache,
                                                       const std::vector<MappedMachO>&  otherOsDylibs,
                                                       const std::vector<MappedMachO>&  osExecutables)
{
    CreateResults  results;
    CacheBuilder   cache(options);

    cache.build(dylibsToCache, otherOsDylibs, osExecutables);

    results.agileSignature = cache.agileSignature();
    results.cdHashFirst = cache.cdHashFirst();
    results.cdHashSecond = cache.cdHashSecond();
    results.warnings = cache.warnings();
    results.evictions = cache.evictions();

    if ( cache.errorMessage().empty() ) {
        results.cacheContent = cache.buffer();
        results.cacheLength  = cache.bufferSize();
    }
    else {
        cache.deleteBuffer();
        results.cacheContent = nullptr;
        results.cacheLength  = 0;
        results.errorMessage = cache.errorMessage();
    }
    return results;
}

bool DyldSharedCache::verifySelfContained(std::vector<MappedMachO>& dylibsToCache, MappedMachO (^loader)(const std::string& runtimePath), std::vector<std::pair<DyldSharedCache::MappedMachO, std::set<std::string>>>& rejected)
{

    // build map of dylibs
    __block std::map<std::string, std::set<std::string>> badDylibs;
    __block std::set<std::string> knownDylibs;
    for (const DyldSharedCache::MappedMachO& dylib : dylibsToCache) {
        std::set<std::string> reasons;
        dyld3::MachOParser parser(dylib.mh);
        if (parser.canBePlacedInDyldCache(dylib.runtimePath, reasons)) {
            knownDylibs.insert(dylib.runtimePath);
            knownDylibs.insert(parser.installName());
        } else {
            badDylibs[dylib.runtimePath] = reasons;
        }
    }

    // check all dependencies to assure every dylib in cache only depends on other dylibs in cache
    __block bool doAgain = true;
    while ( doAgain ) {
        __block std::vector<DyldSharedCache::MappedMachO> foundMappings;
        doAgain = false;
        // scan dylib list making sure all dependents are in dylib list
        for (const DyldSharedCache::MappedMachO& dylib : dylibsToCache) {
            if ( badDylibs.count(dylib.runtimePath) != 0 )
                continue;
            dyld3::MachOParser parser(dylib.mh);
            parser.forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( knownDylibs.count(loadPath) == 0 ) {
                    doAgain = true;
                    MappedMachO foundMapping;
                    if ( badDylibs.count(loadPath) == 0 )
                        foundMapping = loader(loadPath);
                    if ( foundMapping.length == 0 ) {
                        std::string reason = std::string("Could not find dependency '") + loadPath +"'";
                        auto i = badDylibs.find(dylib.runtimePath);
                        if (i == badDylibs.end()) {
                            std::set<std::string> reasons;
                            reasons.insert(reason);
                            badDylibs[dylib.runtimePath] = reasons;
                        } else {
                            i->second.insert(reason);
                        }
                        knownDylibs.erase(dylib.runtimePath);
                        dyld3::MachOParser parserBad(dylib.mh);
                        knownDylibs.erase(parserBad.installName());
                    }
                    else {
                        dyld3::MachOParser foundParser(foundMapping.mh);
                        std::set<std::string> reasons;
                        if (foundParser.canBePlacedInDyldCache(foundParser.installName(), reasons)) {
                            foundMappings.push_back(foundMapping);
                            knownDylibs.insert(foundMapping.runtimePath);
                            knownDylibs.insert(foundParser.installName());
                        } else {
                            auto i = badDylibs.find(dylib.runtimePath);
                            if (i == badDylibs.end()) {
                                badDylibs[dylib.runtimePath] = reasons;
                            } else {
                                i->second.insert(reasons.begin(), reasons.end());
                            }
                        }
                   }
                }
            });
        }
        dylibsToCache.insert(dylibsToCache.end(), foundMappings.begin(), foundMappings.end());
        // remove bad dylibs
        const auto badDylibsCopy = badDylibs;
        dylibsToCache.erase(std::remove_if(dylibsToCache.begin(), dylibsToCache.end(), [&](const DyldSharedCache::MappedMachO& dylib) {
            auto i = badDylibsCopy.find(dylib.runtimePath);
            if ( i !=  badDylibsCopy.end()) {
                rejected.push_back(std::make_pair(dylib, i->second));
                return true;
             }
             else {
                return false;
             }
        }), dylibsToCache.end());
    }

    return badDylibs.empty();
}
#endif

void DyldSharedCache::forEachRegion(void (^handler)(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions)) const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    const dyld_cache_mapping_info* mappingsEnd = &mappings[header.mappingCount];
    for (const dyld_cache_mapping_info* m=mappings; m < mappingsEnd; ++m) {
        handler((char*)this + m->fileOffset, m->address, m->size, m->initProt);
    }
}

void DyldSharedCache::forEachImage(void (^handler)(const mach_header* mh, const char* installName)) const
{
    const dyld_cache_image_info*   dylibs   = (dyld_cache_image_info*)((char*)this + header.imagesOffset);
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return;
    uint64_t firstImageOffset = 0;
    uint64_t firstRegionAddress = mappings[0].address;
    for (uint32_t i=0; i < header.imagesCount; ++i) {
        const char* dylibPath  = (char*)this + dylibs[i].pathFileOffset;
        uint64_t offset = dylibs[i].address - firstRegionAddress;
        if ( firstImageOffset == 0 )
            firstImageOffset = offset;
        // skip over aliases
        if ( dylibs[i].pathFileOffset < firstImageOffset)
            continue;
        const mach_header* mh = (mach_header*)((char*)this + offset);
        handler(mh, dylibPath);
    }
}

void DyldSharedCache::forEachImageEntry(void (^handler)(const char* path, uint64_t mTime, uint64_t inode)) const
{
    const dyld_cache_image_info*   dylibs   = (dyld_cache_image_info*)((char*)this + header.imagesOffset);
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    if ( mappings[0].fileOffset != 0 )
        return;
    uint64_t firstImageOffset = 0;
    uint64_t firstRegionAddress = mappings[0].address;
    for (uint32_t i=0; i < header.imagesCount; ++i) {
        const char* dylibPath  = (char*)this + dylibs[i].pathFileOffset;
        uint64_t offset = dylibs[i].address - firstRegionAddress;
        if ( firstImageOffset == 0 )
            firstImageOffset = offset;
        // skip over aliases
        if ( dylibs[i].pathFileOffset < firstImageOffset)
            continue;
        handler(dylibPath, dylibs[i].modTime, dylibs[i].inode);
    }
}

void DyldSharedCache::forEachImageTextSegment(void (^handler)(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName)) const
{
    // check for old cache without imagesText array
    if ( header.mappingOffset < 123 )
        return;

    // walk imageText table and call callback for each entry
    const dyld_cache_image_text_info* imagesText = (dyld_cache_image_text_info*)((char*)this + header.imagesTextOffset);
    const dyld_cache_image_text_info* imagesTextEnd = &imagesText[header.imagesTextCount];
    for (const dyld_cache_image_text_info* p=imagesText; p < imagesTextEnd; ++p) {
        handler(p->loadAddress, p->textSegmentSize, p->uuid, (char*)this + p->pathOffset);
    }
}


std::string DyldSharedCache::archName() const
{
    const char* archSubString = ((char*)this) + 8;
    while (*archSubString == ' ')
        ++archSubString;
    return archSubString;
}


uint32_t DyldSharedCache::platform() const
{
    return header.platform;
}

#if !DYLD_IN_PROCESS
std::string DyldSharedCache::mapFile() const
{
    __block std::string             result;
    __block std::vector<uint64_t>   regionStartAddresses;
    __block std::vector<uint64_t>   regionSizes;
    __block std::vector<uint64_t>   regionFileOffsets;

    result.reserve(256*1024);
    forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
        regionStartAddresses.push_back(vmAddr);
        regionSizes.push_back(size);
        regionFileOffsets.push_back((uint8_t*)content - (uint8_t*)this);
        char lineBuffer[256];
        const char* prot = "RW";
        if ( permissions == (VM_PROT_EXECUTE|VM_PROT_READ) )
            prot = "EX";
        else if ( permissions == VM_PROT_READ )
            prot = "RO";
        if ( size > 1024*1024 )
            sprintf(lineBuffer, "mapping  %s %4lluMB 0x%0llX -> 0x%0llX\n", prot, size/(1024*1024), vmAddr, vmAddr+size);
        else
            sprintf(lineBuffer, "mapping  %s %4lluKB 0x%0llX -> 0x%0llX\n", prot, size/1024,        vmAddr, vmAddr+size);
        result += lineBuffer;
    });

    // TODO:  add linkedit breakdown
    result += "\n\n";

    forEachImage(^(const mach_header* mh, const char* installName) {
        result += std::string(installName) + "\n";
        dyld3::MachOParser parser(mh);
        parser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
            char lineBuffer[256];
            sprintf(lineBuffer, "\t%16s 0x%08llX -> 0x%08llX\n", segName, vmAddr, vmAddr+vmSize);
            result += lineBuffer;
        });
        result += "\n";
    });

    return result;
}
#endif


uint64_t DyldSharedCache::unslidLoadAddress() const
{
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)this + header.mappingOffset);
    return mappings[0].address;
}

void DyldSharedCache::getUUID(uuid_t uuid) const
{
    memcpy(uuid, header.uuid, sizeof(uuid_t));
}

uint64_t DyldSharedCache::mappedSize() const
{
    __block uint64_t startAddr = 0;
    __block uint64_t endAddr = 0;
    forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
        if ( startAddr == 0 )
            startAddr = vmAddr;
        uint64_t end = vmAddr+size;
        if ( end > endAddr )
            endAddr = end;
    });
    return (endAddr - startAddr);
}










