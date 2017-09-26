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


#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>


#include "DyldCacheParser.h"
#include "Trie.hpp"


namespace dyld3 {

DyldCacheParser::DyldCacheParser(const DyldSharedCache* cacheHeader, bool rawFile)
{
    _data = (long)cacheHeader;
    if ( rawFile )
        _data |= 1;
}

const dyld_cache_header* DyldCacheParser::header() const
{
    return (dyld_cache_header*)(_data & -2);
}

const DyldSharedCache* DyldCacheParser::cacheHeader() const
{
    return (DyldSharedCache*)header();
}

bool DyldCacheParser::cacheIsMappedRaw() const
{
    return (_data & 1);
}


uint64_t DyldCacheParser::dataRegionRuntimeVmOffset() const
{
    const dyld_cache_header* cacheHeader = header();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);
    return (mappings[1].address - mappings[0].address);
}

const dyld3::launch_cache::binary_format::ImageGroup* DyldCacheParser::cachedDylibsGroup() const
{
    const dyld_cache_header* cacheHeader = header();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);

    if ( cacheIsMappedRaw() ) {
        // Whole file is mapped read-only. Use mapping file-offsets to find ImageGroup
        uint64_t offsetInLinkEditRegion = (cacheHeader->dylibsImageGroupAddr - mappings[2].address);
        return (dyld3::launch_cache::binary_format::ImageGroup*)((uint8_t*)cacheHeader + mappings[2].fileOffset + offsetInLinkEditRegion);
    }
    else {
        // Cache file is mapped in three non-contiguous ranges.  Use mapping addresses to find ImageGroup
        return (dyld3::launch_cache::binary_format::ImageGroup*)((uint8_t*)cacheHeader + (cacheHeader->dylibsImageGroupAddr - mappings[0].address));
    }
}


const dyld3::launch_cache::binary_format::ImageGroup* DyldCacheParser::otherDylibsGroup() const
{
    const dyld_cache_header* cacheHeader = header();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);

    if ( cacheIsMappedRaw() ) {
        // Whole file is mapped read-only. Use mapping file-offsets to find ImageGroup
        uint64_t offsetInLinkEditRegion = (cacheHeader->otherImageGroupAddr - mappings[2].address);
        return (dyld3::launch_cache::binary_format::ImageGroup*)((uint8_t*)cacheHeader + mappings[2].fileOffset + offsetInLinkEditRegion);
    }
    else {
        // Cache file is mapped in three non-contiguous ranges.  Use mapping addresses to find ImageGroup
        return (dyld3::launch_cache::binary_format::ImageGroup*)((uint8_t*)cacheHeader + (cacheHeader->otherImageGroupAddr - mappings[0].address));
    }
}

const dyld3::launch_cache::binary_format::Closure* DyldCacheParser::findClosure(const char* path) const
{
    const dyld_cache_header* cacheHeader = header();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);

    const uint8_t* executableTrieStart   = nullptr;
    const uint8_t* executableTrieEnd     = nullptr;
    const uint8_t* closuresStart         = nullptr;

    if ( cacheIsMappedRaw() ) {
        // Whole file is mapped read-only. Use mapping file-offsets to find trie and closures
        executableTrieStart   = (uint8_t*)cacheHeader + cacheHeader->progClosuresTrieAddr - mappings[2].address + mappings[2].fileOffset;
        executableTrieEnd     = executableTrieStart + cacheHeader->progClosuresTrieSize;
        closuresStart         = (uint8_t*)cacheHeader + cacheHeader->progClosuresAddr - mappings[2].address + mappings[2].fileOffset;
    }
    else {
        // Cache file is mapped in three non-contiguous ranges.  Use mapping addresses to find trie and closures
        uintptr_t slide       = (uintptr_t)cacheHeader - (uintptr_t)(mappings[0].address);
        executableTrieStart   = (uint8_t*)(cacheHeader->progClosuresTrieAddr + slide);
        executableTrieEnd     = executableTrieStart + cacheHeader->progClosuresTrieSize;
        closuresStart         = (uint8_t*)(cacheHeader->progClosuresAddr + slide);
    }
    Diagnostics diag;
    const uint8_t* imageNode = dyld3::MachOParser::trieWalk(diag, executableTrieStart, executableTrieEnd, path);
    if ( imageNode != NULL ) {
        uint32_t closureOffset = (uint32_t)dyld3::MachOParser::read_uleb128(diag, imageNode, executableTrieEnd);
        return (const dyld3::launch_cache::BinaryClosureData*)((uint8_t*)closuresStart + closureOffset);
    }
    return nullptr;
}


#if !DYLD_IN_PROCESS
void DyldCacheParser::forEachClosure(void (^handler)(const char* runtimePath, const dyld3::launch_cache::binary_format::Closure* cls)) const
{
    const dyld_cache_header* cacheHeader = header();
    const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);

    const uint8_t* executableTrieStart   = nullptr;
    const uint8_t* executableTrieEnd     = nullptr;
    const uint8_t* closuresStart         = nullptr;

    if ( cacheIsMappedRaw() ) {
        // Whole file is mapped read-only. Use mapping file-offsets to find trie and closures
        executableTrieStart   = (uint8_t*)cacheHeader + cacheHeader->progClosuresTrieAddr - mappings[2].address + mappings[2].fileOffset;
        executableTrieEnd     = executableTrieStart + cacheHeader->progClosuresTrieSize;
        closuresStart         = (uint8_t*)cacheHeader + cacheHeader->progClosuresAddr - mappings[2].address + mappings[2].fileOffset;
    }
    else {
        // Cache file is mapped in three non-contiguous ranges.  Use mapping addresses to find trie and closures
        uintptr_t slide       = (uintptr_t)cacheHeader - (uintptr_t)(mappings[0].address);
        executableTrieStart   = (uint8_t*)(cacheHeader->progClosuresTrieAddr + slide);
        executableTrieEnd     = executableTrieStart + cacheHeader->progClosuresTrieSize;
        closuresStart         = (uint8_t*)(cacheHeader->progClosuresAddr + slide);
    }

    std::vector<DylibIndexTrie::Entry> closureEntries;
    if ( Trie<DylibIndex>::parseTrie(executableTrieStart, executableTrieEnd, closureEntries) ) {
        for (DylibIndexTrie::Entry& entry : closureEntries ) {
            uint32_t offset = entry.info.index;
            if ( offset < cacheHeader->progClosuresSize )
                handler(entry.name.c_str(), (const dyld3::launch_cache::binary_format::Closure*)(closuresStart+offset));
        }
    }
}
#endif




} // namespace dyld3

