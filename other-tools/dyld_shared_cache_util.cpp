/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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

#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syslimits.h>
#include <mach-o/arch.h>
#include <mach-o/nlist.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld_priv.h>
#include <bootstrap.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <uuid/uuid.h>

#include <TargetConditionals.h>

#include <map>
#include <vector>
#include <iostream>
#include <optional>

#include "ClosureFileSystemPhysical.h"
#include "DyldSharedCache.h"
#include "JSONWriter.h"
#include "Trie.hpp"
#include "dsc_extractor.h"
#include "dyld_introspection.h"
#include "OptimizerObjC.h"
#include "OptimizerSwift.h"

#include "PrebuiltLoader.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "Utilities.h"

#include "objc-shared-cache.h"
#include "OptimizerObjC.h"

#include "ObjCVisitor.h"
#include "SymbolicatedImage.h"

using namespace dyld4;

using other_tools::SymbolicatedImage;
using mach_o::Header;
using mach_o::Platform;
using mach_o::Version32;

#if TARGET_OS_OSX
#define DSC_BUNDLE_REL_PATH "../../lib/dsc_extractor.bundle"
#else
#define DSC_BUNDLE_REL_PATH "../lib/dsc_extractor.bundle"
#endif


enum Mode {
    modeNone,
    modeList,
    modeMap,
    modeDependencies,
    modeSlideInfo,
    modeVerboseSlideInfo,
    modeFixupsInDylib,
    modeTextInfo,
    modeLinkEdit,
    modeLocalSymbols,
    modeJSONMap,
    modeVerboseJSONMap,
    modeJSONDependents,
    modeSectionSizes,
    modeStrings,
    modeInfo,
    modeTPROInfo,
    modeStats,
    modeSize,
    modeObjCInfo,
    modeObjCProtocols,
    modeObjCImpCaches,
    modeObjCClasses,
    modeObjCClassLayout,
    modeObjCClassMethodLists,
    modeObjCClassHashTable,
    modeObjCSelectors,
    modeSwiftProtocolConformances,
    modeSwiftPtrTables,
    modeLookupVA,
    modeExtract,
    modePatchTable,
    modeRootsCost,
    modeListDylibsWithSection,
    modeDuplicates,
    modeDuplicatesSummary,
    modeMachHeaders,
    modeCacheHeader,
    modeDylibSymbols,
    modeFunctionStarts,
    modeFunctionVariants,
    modePrewarmingData,
};

struct Options {
    Mode            mode;
    const char*     dependentsOfPath;
    const char*     extractionDir;
    const char*     segmentName;
    const char*     sectionName;
    const char*     rootPath            = nullptr;
    const char*     fixupsInDylib;
    const char*     rootsCostOfDylib    = nullptr;
    const char*     lookupVA            = nullptr;
    bool            printUUIDs;
    bool            printVMAddrs;
    bool            printDylibVersions;
    bool            printInodes;
};


static void usage() {
    fprintf(stderr, "Usage: dyld_shared_cache_util <command> [-fs-root] [-inode] [-versions] [-vmaddr] [shared-cache-file]\n"
        "    Commands:\n"
        "        -list [-uuid] [-vmaddr]                  list images\n"
        "        -dependents <dylb-path>                  list dependents of dylib\n"
        "        -linkedit                                print linkedit contents\n"
        "        -info                                    print shared cache info\n"
        "        -stats                                   print size stats\n"
        "        -slide_info                              print slide info\n"
        "        -verbose_slide_info                      print verbose slide info\n"
        "        -fixups_in_dylib <dylib-path>            print fixups in dylib\n"
        "        -text_info                               print locations of TEXT segments\n"
        "        -local_symbols                           print local symbols and locations\n"
        "        -strings                                 print C strings in images\n"
        "        -sections                                print summary of section sizes\n"
        "        -exports                                 list exported symbols in images\n"
        "        -duplicate_exports                       list symbols exported by multiple images\n"
        "        -duplicate_exports_summary               print number of duplicated symbols per image\n"
        "        -map                                     print map of segment locations\n"
        "        -json-map                                print map of segment locations in JSON format\n"
        "        -verbose-json-map                        print map of segment and section locations in JSON format\n"
        "        -json-dependents                         print dependents in JSON format\n"
        "        -size                                    print the size of each image\n"
        "        -objc-info                               print summary of ObjC content\n"
        "        -objc-protocols                          list ObjC protocols\n"
        "        -objc-imp-caches                         print contents of ObjC method caches\n"
        "        -objc-classes                            print ObjC class names and methods in JSON format\n"
        "        -objc-class-layout                       print size, start offset, and ivars of ObjC classes\n"
        "        -objc-class-method-lists                 print methods and properties of ObjC classes\n"
        "        -objc-class-hash-table                   print the contents of the ObjC class table\n"
        "        -objc-selectors                          print all ObjC selector names and locations in JSON format\n"
        "        -swift-proto                             print Swift protocol conformance table\n"
        "        -swift-ptrtables                         print Swift pointer tables\n"
        "        -lookup-va                               lookup range and symbols at the given virtual address\n"
        "        -extract <directory>                     extract images into the given directory\n"
        "        -patch_table                             print symbol patch table\n"
        "        -list_dylibs_with_section <seg> <sect>   list images that contain the given section\n"
        "        -mach_headers                            summarize mach header of each image\n"
        "        -load_commands                           summarize load commands of each image\n"
        "        -cache_header                            print header of each shared cache file\n"
        "        -dylib_symbols                           print all symbol names and locations\n"
        "        -function_starts                         print address of beginning of each function\n");
}

static void checkMode(Mode mode) {
    if ( mode != modeNone ) {
        fprintf(stderr, "Error: select one of: -list, -dependents, -info, -slide_info, -verbose_slide_info, -linkedit, -map, -extract, or -size\n");
        usage();
        exit(1);
    }
}

struct SymbolicatedCache
{
    struct Range
    {
        uint64_t                    startAddr;
        uint64_t                    endAddr;
        std::optional<size_t>       imageIndex;
        std::string_view            segmentName;
        std::string_view            sectName;

        bool operator<(const Range& other) const
        {
            return startAddr < other.startAddr;
        }
    };

    SymbolicatedCache(const DyldSharedCache* cache, bool isCacheOnDisk);

    std::optional<size_t>   findClosestRange(uint64_t addr) const;
    void                    findClosestSymbol(uint64_t addr, const SymbolicatedImage*& image, const char*& inSymbolName, uint32_t& inSymbolOffset) const;

    std::string symbolNameAt(uint64_t addr) const;

    std::vector<Range>             ranges;
    std::vector<Image>             machoImages;
    std::vector<SymbolicatedImage> images;
    uint64_t                       cacheBaseAddr;
};

SymbolicatedCache::SymbolicatedCache(const DyldSharedCache* cache, bool isCacheOnDisk)
{
    cacheBaseAddr = cache->unslidLoadAddress();

    machoImages.reserve(cache->imagesCount());
    images.reserve(cache->imagesCount());
    cache->forEachImage(^(const Header* hdr, const char* installName) {
        machoImages.emplace_back((void*)hdr, (size_t)-1, isCacheOnDisk ? Image::MappingKind::dyldLoadedPreFixups : Image::MappingKind::dyldLoadedPostFixups);
    });

    for ( const Image& image : machoImages )
        images.emplace_back(image);

    for ( size_t i = 0; i < images.size(); ++i ) {
        const SymbolicatedImage& im = images[i];
        im.image().header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool &stop) {
            if ( sectInfo.size == 0 )
                return;
            ranges.push_back({ .imageIndex = i, .startAddr = sectInfo.address, .endAddr = sectInfo.address + sectInfo.size, .segmentName = sectInfo.segmentName, .sectName = sectInfo.sectionName });
        });
    }

    std::sort(ranges.begin(), ranges.end());
    for ( size_t i = 1; i < ranges.size(); ++i ) {
        if ( ranges[i-1].endAddr > ranges[i].startAddr ) {
            assert(false && "overlapping image ranges");
        }
    }
}

std::string SymbolicatedCache::symbolNameAt(uint64_t addr) const
{
    const char* name = nullptr;
    uint32_t offset = 0;
    const SymbolicatedImage* image = nullptr;
    findClosestSymbol(addr, image, name, offset);
    if ( name == nullptr ) {
        if ( image ) {
            return std::string(image->image().header()->installName()) + "+" + json::hex(offset);
        }
        return json::hex( addr );
    }

    std::string nameWithImage = std::string(image->image().header()->installName()) + "`" + std::string(name);
    if ( offset != 0 )
        return nameWithImage + "+" + json::hex(offset);
    return nameWithImage;
}

std::optional<size_t> SymbolicatedCache::findClosestRange(uint64_t addr) const
{
    auto it = std::lower_bound(ranges.begin(), ranges.end(), addr, [](const Range& range, uint64_t cmpAddr) -> bool {
        return range.startAddr <= cmpAddr;
    });
    // lower_bound returns the range after the one we need
    if ( (it != ranges.end()) && (it != ranges.begin()) ) {
        --it;
    } else {
        it = ranges.begin();
    }

    if ( addr < it->startAddr || addr >= it->endAddr )
        return std::nullopt;

    return std::distance(ranges.begin(), it);
}

void SymbolicatedCache::findClosestSymbol(uint64_t addr, const SymbolicatedImage*& image, const char*& inSymbolName, uint32_t& inSymbolOffset) const
{
    inSymbolName = nullptr;
    inSymbolOffset = 0;
    image = nullptr;
    if ( ranges.empty() )
        return;

    std::optional<size_t> rangeIndex = findClosestRange(addr);
    if ( !rangeIndex )
        return;

    const Range& range = ranges[*rangeIndex];
    if ( range.imageIndex == std::nullopt )
        return;

    size_t imageIndex = *range.imageIndex;
    assert(imageIndex < images.size());
    //fprintf(stderr, "debug symbol lookup at offset: %llu, abs: 0x%llX, image: %s\n", runtimeOffset, addr, images[imageIndex].image().header()->installName());
    image = &images[imageIndex];
    images[imageIndex].findClosestSymbol(addr, inSymbolName, inSymbolOffset);

    if ( inSymbolName == nullptr ) {
        inSymbolOffset = (uint32_t)(addr - image->prefLoadAddress());
    }
}


struct SegmentInfo
{
    uint64_t            vmAddr;
    uint64_t            vmSize;
    const char*         installName;
    std::string_view    segName;
};

static void sortSegmentInfo(std::vector<SegmentInfo>& segInfos)
{
    std::sort(segInfos.begin(), segInfos.end(), [](const SegmentInfo& l, const SegmentInfo& r) -> bool {
        return l.vmAddr < r.vmAddr;
    });
}

static void buildSegmentInfo(const Header* hdr, std::vector<SegmentInfo>& segInfos)
{
    const char* installName = hdr->installName();
    hdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        // Note, we subtract 1 from the vmSize so that lower_bound doesn't include the end of the segment
        // as being a match for a given address.
        segInfos.push_back({info.vmaddr, info.vmsize - 1, installName, info.segmentName});
    });
}

static void buildSegmentInfo(const DyldSharedCache* dyldCache, std::vector<SegmentInfo>& segInfos)
{
    dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
        buildSegmentInfo(hdr, segInfos);
    });
    sortSegmentInfo(segInfos);
}

static void printSlideInfoForDataRegion(const DyldSharedCache* dyldCache, uint64_t dataStartAddress, uint64_t dataSize,
                                        const uint8_t* dataPagesStart,
                                        const dyld_cache_slide_info* slideInfoHeader, bool verboseSlideInfo) {

    printf("slide info version=%d\n", slideInfoHeader->version);
    if ( slideInfoHeader->version == 1 ) {
        printf("toc_count=%d, data page count=%lld\n", slideInfoHeader->toc_count, dataSize/4096);
        const dyld_cache_slide_info_entry* entries = (dyld_cache_slide_info_entry*)((char*)slideInfoHeader + slideInfoHeader->entries_offset);
        const uint16_t* tocs = (uint16_t*)((char*)slideInfoHeader + slideInfoHeader->toc_offset);
        for(int i=0; i < slideInfoHeader->toc_count; ++i) {
            printf("0x%08llX: [% 5d,% 5d] ", dataStartAddress + i*4096, i, tocs[i]);
            const dyld_cache_slide_info_entry* entry = &entries[tocs[i]];
            for(int j=0; j < slideInfoHeader->entries_size; ++j)
                printf("%02X", entry->bits[j]);
            printf("\n");
            if ( verboseSlideInfo ) {
                uint8_t* pageContent = (uint8_t*)(long)(dataPagesStart + (4096 * i));
                for(int j=0; j < slideInfoHeader->entries_size; ++j) {
                    uint8_t bitmask = entry->bits[j];
                    for (unsigned k = 0; k != 8; ++k) {
                        if ( bitmask & (1 << k) ) {
                            uint32_t pageOffset = ((j * 8) + k) * 4;
                            uint8_t* loc = pageContent + pageOffset;
                            uint32_t rawValue = *((uint32_t*)loc);
                            printf("         [% 5d + 0x%04llX]: 0x%016llX\n", i, (uint64_t)(pageOffset), (uint64_t)rawValue);
                        }
                    }
                }
            }
        }
    }
    else if ( slideInfoHeader->version == 2 ) {
        const dyld_cache_slide_info2* slideInfo = (dyld_cache_slide_info2*)(slideInfoHeader);
        printf("page_size=%d\n", slideInfo->page_size);
        printf("delta_mask=0x%016llX\n", slideInfo->delta_mask);
        printf("value_add=0x%016llX\n", slideInfo->value_add);
        printf("page_starts_count=%d, page_extras_count=%d\n", slideInfo->page_starts_count, slideInfo->page_extras_count);
        const uint16_t* starts = (uint16_t* )((char*)slideInfo + slideInfo->page_starts_offset);
        const uint16_t* extras = (uint16_t* )((char*)slideInfo + slideInfo->page_extras_offset);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            const uint16_t start = starts[i];
            auto rebaseChain = [&](uint8_t* pageContent, uint16_t startOffset)
            {
                uintptr_t slideAmount = 0;
                const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
                const uintptr_t   valueMask    = ~deltaMask;
                const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
                const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

                uint32_t pageOffset = startOffset;
                uint32_t delta = 1;
                while ( delta != 0 ) {
                    uint8_t* loc = pageContent + pageOffset;
                    uintptr_t rawValue = *((uintptr_t*)loc);
                    delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
                    uintptr_t value = (rawValue & valueMask);
                    if ( value != 0 ) {
                        value += valueAdd;
                        value += slideAmount;
                    }
                    printf("    [% 5d + 0x%04llX]: 0x%016llX = 0x%016llX\n", i, (uint64_t)(pageOffset), (uint64_t)rawValue, (uint64_t)value);
                    pageOffset += delta;
                }
            };
            if ( start == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE ) {
                printf("page[% 5d]: no rebasing\n", i);
            }
            else if ( start & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA ) {
                printf("page[% 5d]: ", i);
                int j=(start & 0x3FFF);
                bool done = false;
                do {
                    uint16_t aStart = extras[j];
                    printf("start=0x%04X ", aStart & 0x3FFF);
                    if ( verboseSlideInfo ) {
                        uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                        uint16_t pageStartOffset = (aStart & 0x3FFF)*4;
                        rebaseChain(page, pageStartOffset);
                    }
                    done = (extras[j] & DYLD_CACHE_SLIDE_PAGE_ATTR_END);
                    ++j;
                } while ( !done );
                printf("\n");
            }
            else {
                printf("page[% 5d]: start=0x%04X\n", i, starts[i]);
                if ( verboseSlideInfo ) {
                    uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                    uint16_t pageStartOffset = start*4;
                    rebaseChain(page, pageStartOffset);
                }
            }
        }
    }
    else if ( slideInfoHeader->version == 3 ) {
        const dyld_cache_slide_info3* slideInfo = (dyld_cache_slide_info3*)(slideInfoHeader);
        printf("page_size=%d\n", slideInfo->page_size);
        printf("page_starts_count=%d\n", slideInfo->page_starts_count);
        printf("auth_value_add=0x%016llX\n", slideInfo->auth_value_add);
        const uintptr_t authValueAdd = (uintptr_t)(slideInfo->auth_value_add);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            uint16_t delta = slideInfo->page_starts[i];
            if ( delta == DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE ) {
                printf("page[% 5d]: no rebasing\n", i);
                continue;
            }

            printf("page[% 5d]: start=0x%04X\n", i, delta);
            if ( !verboseSlideInfo )
                continue;

            delta = delta/sizeof(uint64_t); // initial offset is byte based
            const uint8_t* pageStart = dataPagesStart + (i * slideInfo->page_size);
            const dyld_cache_slide_pointer3* loc = (dyld_cache_slide_pointer3*)pageStart;
            do {
                loc += delta;
                delta = loc->plain.offsetToNextPointer;
                dyld3::MachOLoaded::ChainedFixupPointerOnDisk ptr;
                ptr.raw64 = *((uint64_t*)loc);
                if ( loc->auth.authenticated ) {
                    uint64_t target = authValueAdd + loc->auth.offsetFromSharedCacheBase;
                    uint64_t targetValue = target;
                    printf("    [% 5d + 0x%04llX]: 0x%016llX (JOP: diversity %d, address %s, %s)\n",
                           i, (uint64_t)((const uint8_t*)loc - pageStart), targetValue,
                           ptr.arm64e.authBind.diversity, ptr.arm64e.authBind.addrDiv ? "true" : "false",
                           ptr.arm64e.keyName());
                }
                else {
                    uint64_t targetValue = ptr.arm64e.unpackTarget();
                    printf("    [% 5d + 0x%04llX]: 0x%016llX\n", i, (uint64_t)((const uint8_t*)loc - pageStart), targetValue);
                }
            } while (delta != 0);
        }
    }
    else if ( slideInfoHeader->version == 4 ) {
        const dyld_cache_slide_info4* slideInfo = (dyld_cache_slide_info4*)(slideInfoHeader);
        printf("page_size=%d\n", slideInfo->page_size);
        printf("delta_mask=0x%016llX\n", slideInfo->delta_mask);
        printf("value_add=0x%016llX\n", slideInfo->value_add);
        printf("page_starts_count=%d, page_extras_count=%d\n", slideInfo->page_starts_count, slideInfo->page_extras_count);
        const uint16_t* starts = (uint16_t* )((char*)slideInfo + slideInfo->page_starts_offset);
        const uint16_t* extras = (uint16_t* )((char*)slideInfo + slideInfo->page_extras_offset);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            const uint16_t start = starts[i];
            auto rebaseChainV4 = [&](uint8_t* pageContent, uint16_t startOffset)
            {
                uintptr_t slideAmount = 0;
                const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
                const uintptr_t   valueMask    = ~deltaMask;
                const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
                const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

                uint32_t pageOffset = startOffset;
                uint32_t delta = 1;
                while ( delta != 0 ) {
                    uint8_t* loc = pageContent + pageOffset;
                    uint32_t rawValue = *((uint32_t*)loc);
                    delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
                    uintptr_t value = (rawValue & valueMask);
                    if ( (value & 0xFFFF8000) == 0 ) {
                        // small positive non-pointer, use as-is
                    }
                    else if ( (value & 0x3FFF8000) == 0x3FFF8000 ) {
                        // small negative non-pointer
                        value |= 0xC0000000;
                    }
                    else  {
                        value += valueAdd;
                        value += slideAmount;
                    }
                    printf("    [% 5d + 0x%04X]: 0x%08X\n", i, pageOffset, rawValue);
                    pageOffset += delta;
                }
            };
            if ( start == DYLD_CACHE_SLIDE4_PAGE_NO_REBASE ) {
                printf("page[% 5d]: no rebasing\n", i);
            }
            else if ( start & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA ) {
                printf("page[% 5d]: ", i);
                int j=(start & DYLD_CACHE_SLIDE4_PAGE_INDEX);
                bool done = false;
                do {
                    uint16_t aStart = extras[j];
                    printf("start=0x%04X ", aStart & DYLD_CACHE_SLIDE4_PAGE_INDEX);
                    if ( verboseSlideInfo ) {
                        uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                        uint16_t pageStartOffset = (aStart & DYLD_CACHE_SLIDE4_PAGE_INDEX)*4;
                        rebaseChainV4(page, pageStartOffset);
                    }
                    done = (extras[j] & DYLD_CACHE_SLIDE4_PAGE_EXTRA_END);
                    ++j;
                } while ( !done );
                printf("\n");
            }
            else {
                printf("page[% 5d]: start=0x%04X\n", i, starts[i]);
                if ( verboseSlideInfo ) {
                    uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                    uint16_t pageStartOffset = start*4;
                    rebaseChainV4(page, pageStartOffset);
                }
            }
        }
    }
    else if ( slideInfoHeader->version == 5 ) {
        const dyld_cache_slide_info5* slideInfo = (dyld_cache_slide_info5*)(slideInfoHeader);
        printf("page_size=%d\n", slideInfo->page_size);
        printf("page_starts_count=%d\n", slideInfo->page_starts_count);
        printf("auth_value_add=0x%016llX\n", slideInfo->value_add);
        const uintptr_t valueAdd = (uintptr_t)(slideInfo->value_add);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            uint16_t delta = slideInfo->page_starts[i];
            if ( delta == DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE ) {
                printf("page[% 5d]: no rebasing\n", i);
                continue;
            }

            printf("page[% 5d]: start=0x%04X\n", i, delta);
            if ( !verboseSlideInfo )
                continue;

            delta = delta/sizeof(uint64_t); // initial offset is byte based
            const uint8_t* pageStart = dataPagesStart + (i * slideInfo->page_size);
            const dyld_cache_slide_pointer5* loc = (dyld_cache_slide_pointer5*)pageStart;

            do {
                loc += delta;
                delta = loc->regular.next;

                dyld3::MachOLoaded::ChainedFixupPointerOnDisk ptr;
                ptr.raw64 = *((uint64_t*)loc);
                PointerMetaData pmd(&ptr, DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE);

                uint64_t targetValue = valueAdd + loc->regular.runtimeOffset;
                if ( pmd.authenticated ) {
                    printf("    [% 5d + 0x%04llX]: 0x%016llX (JOP: diversity %d, address %s, %s)\n",
                           i, (uint64_t)((const uint8_t*)loc - pageStart), targetValue,
                           ptr.cache64e.auth.diversity, ptr.cache64e.auth.addrDiv ? "true" : "false",
                           ptr.cache64e.keyName());
                } else {
                    targetValue = targetValue | ptr.cache64e.high8();
                    printf("    [% 5d + 0x%04llX]: 0x%016llX\n", i, (uint64_t)((const uint8_t*)loc - pageStart), targetValue);
                }
            } while (delta != 0);
        }
    }
}

static void forEachSlidValue(const DyldSharedCache* dyldCache, uint64_t dataStartAddress, uint64_t dataSize,
                             const uint8_t* dataPagesStart,
                             const dyld_cache_slide_info* slideInfoHeader,
                             void (^callback)(uint64_t fixupVMAddr, uint64_t targetVMAddr,
                                              PointerMetaData PMD))
{
    if ( slideInfoHeader->version == 1 ) {
        const dyld_cache_slide_info_entry* entries = (dyld_cache_slide_info_entry*)((char*)slideInfoHeader + slideInfoHeader->entries_offset);
        const uint16_t* tocs = (uint16_t*)((char*)slideInfoHeader + slideInfoHeader->toc_offset);
        for(int i=0; i < slideInfoHeader->toc_count; ++i) {
            const dyld_cache_slide_info_entry* entry = &entries[tocs[i]];
            uint8_t* pageContent = (uint8_t*)(long)(dataPagesStart + (4096 * i));
            for(int j=0; j < slideInfoHeader->entries_size; ++j) {
                uint8_t bitmask = entry->bits[j];
                for (unsigned k = 0; k != 8; ++k) {
                    if ( bitmask & (1 << k) ) {
                        uint32_t pageOffset = ((j * 8) + k) * 4;
                        uint8_t* loc = pageContent + pageOffset;
                        uint32_t rawValue = *((uint32_t*)loc);

                        uint64_t offsetInDataRegion = loc - dataPagesStart;
                        uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                        uint64_t targetVMAddr = rawValue;
                        callback(fixupVMAddr, targetVMAddr, PointerMetaData());
                    }
                }
            }
        }
    }
    else if ( slideInfoHeader->version == 2 ) {
        const dyld_cache_slide_info2* slideInfo = (dyld_cache_slide_info2*)(slideInfoHeader);
        const uint16_t* starts = (uint16_t* )((char*)slideInfo + slideInfo->page_starts_offset);
        const uint16_t* extras = (uint16_t* )((char*)slideInfo + slideInfo->page_extras_offset);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            const uint16_t start = starts[i];
            auto rebaseChain = [&](uint8_t* pageContent, uint16_t startOffset)
            {
                uintptr_t slideAmount = 0;
                const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
                const uintptr_t   valueMask    = ~deltaMask;
                const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
                const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

                uint32_t pageOffset = startOffset;
                uint32_t delta = 1;
                while ( delta != 0 ) {
                    uint8_t* loc = pageContent + pageOffset;
                    uintptr_t rawValue = *((uintptr_t*)loc);
                    delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
                    uintptr_t value = (rawValue & valueMask);
                    if ( value != 0 ) {
                        value += valueAdd;
                        value += slideAmount;
                    }
                    pageOffset += delta;

                    uint64_t offsetInDataRegion = loc - dataPagesStart;
                    uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                    uint64_t targetVMAddr = value;
                    callback(fixupVMAddr, targetVMAddr, PointerMetaData());
                }
            };
            if ( start == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE ) {
                // Nothing to do here
            }
            else if ( start & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA ) {
                int j=(start & 0x3FFF);
                bool done = false;
                do {
                    uint16_t aStart = extras[j];
                    uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                    uint16_t pageStartOffset = (aStart & 0x3FFF)*4;
                    rebaseChain(page, pageStartOffset);
                    done = (extras[j] & DYLD_CACHE_SLIDE_PAGE_ATTR_END);
                    ++j;
                } while ( !done );
            }
            else {
                uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                uint16_t pageStartOffset = start*4;
                rebaseChain(page, pageStartOffset);
            }
        }
    }
    else if ( slideInfoHeader->version == 3 ) {
        const dyld_cache_slide_info3* slideInfo = (dyld_cache_slide_info3*)(slideInfoHeader);
        const uintptr_t authValueAdd = (uintptr_t)(slideInfo->auth_value_add);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            uint16_t delta = slideInfo->page_starts[i];
            if ( delta == DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE ) {
                // Nothing to do here
                continue;
            }

            delta = delta/sizeof(uint64_t); // initial offset is byte based
            const uint8_t* pageStart = dataPagesStart + (i * slideInfo->page_size);
            const dyld_cache_slide_pointer3* loc = (dyld_cache_slide_pointer3*)pageStart;
            do {
                loc += delta;
                delta = loc->plain.offsetToNextPointer;
                dyld3::MachOLoaded::ChainedFixupPointerOnDisk ptr;
                ptr.raw64 = *((uint64_t*)loc);
                if ( loc->auth.authenticated ) {
                    uint64_t targetVMAddr = authValueAdd + loc->auth.offsetFromSharedCacheBase;

                    PointerMetaData pmd(&ptr, DYLD_CHAINED_PTR_ARM64E);
                    uint64_t offsetInDataRegion = (const uint8_t*)loc - dataPagesStart;
                    uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                    callback(fixupVMAddr, targetVMAddr, pmd);
                }
                else {
                    uint64_t targetVMAddr = ptr.arm64e.unpackTarget();

                    uint64_t offsetInDataRegion = (const uint8_t*)loc - dataPagesStart;
                    uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                    callback(fixupVMAddr, targetVMAddr, PointerMetaData());
                }
            } while (delta != 0);
        }
    }
    else if ( slideInfoHeader->version == 4 ) {
        const dyld_cache_slide_info4* slideInfo = (dyld_cache_slide_info4*)(slideInfoHeader);
        const uint16_t* starts = (uint16_t* )((char*)slideInfo + slideInfo->page_starts_offset);
        const uint16_t* extras = (uint16_t* )((char*)slideInfo + slideInfo->page_extras_offset);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            const uint16_t start = starts[i];
            auto rebaseChainV4 = [&](uint8_t* pageContent, uint16_t startOffset)
            {
                uintptr_t slideAmount = 0;
                const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
                const uintptr_t   valueMask    = ~deltaMask;
                const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
                const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

                uint32_t pageOffset = startOffset;
                uint32_t delta = 1;
                while ( delta != 0 ) {
                    uint8_t* loc = pageContent + pageOffset;
                    uint32_t rawValue = *((uint32_t*)loc);
                    delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
                    uintptr_t value = (rawValue & valueMask);
                    if ( (value & 0xFFFF8000) == 0 ) {
                        // small positive non-pointer, use as-is
                    }
                    else if ( (value & 0x3FFF8000) == 0x3FFF8000 ) {
                        // small negative non-pointer
                        value |= 0xC0000000;
                    }
                    else  {
                        value += valueAdd;
                        value += slideAmount;

                        uint64_t offsetInDataRegion = (const uint8_t*)loc - dataPagesStart;
                        uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                        uint64_t targetVMAddr = value;
                        callback(fixupVMAddr, targetVMAddr, PointerMetaData());
                    }
                    pageOffset += delta;
                }
            };
            if ( start == DYLD_CACHE_SLIDE4_PAGE_NO_REBASE ) {
                // Nothing to do here
            }
            else if ( start & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA ) {
                int j=(start & DYLD_CACHE_SLIDE4_PAGE_INDEX);
                bool done = false;
                do {
                    uint16_t aStart = extras[j];
                    uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                    uint16_t pageStartOffset = (aStart & DYLD_CACHE_SLIDE4_PAGE_INDEX)*4;
                    rebaseChainV4(page, pageStartOffset);
                    done = (extras[j] & DYLD_CACHE_SLIDE4_PAGE_EXTRA_END);
                    ++j;
                } while ( !done );
            }
            else {
                uint8_t* page = (uint8_t*)(long)(dataPagesStart + (slideInfo->page_size*i));
                uint16_t pageStartOffset = start*4;
                rebaseChainV4(page, pageStartOffset);
            }
        }
    }
    else if ( slideInfoHeader->version == 5 ) {
        const dyld_cache_slide_info5* slideInfo = (dyld_cache_slide_info5*)(slideInfoHeader);
        const uintptr_t valueAdd = (uintptr_t)(slideInfo->value_add);
        for (int i=0; i < slideInfo->page_starts_count; ++i) {
            uint16_t delta = slideInfo->page_starts[i];
            if ( delta == DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE ) {
                // Nothing to do here
                continue;
            }

            delta = delta/sizeof(uint64_t); // initial offset is byte based
            const uint8_t* pageStart = dataPagesStart + (i * slideInfo->page_size);
            const dyld_cache_slide_pointer5* loc = (dyld_cache_slide_pointer5*)pageStart;
            do {
                loc += delta;
                delta = loc->regular.next;

                dyld3::MachOLoaded::ChainedFixupPointerOnDisk ptr;
                ptr.raw64 = *((uint64_t*)loc);
                PointerMetaData pmd(&ptr, DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE);

                uint64_t offsetInDataRegion = (const uint8_t*)loc - dataPagesStart;
                uint64_t fixupVMAddr = dataStartAddress + offsetInDataRegion;
                uint64_t targetVMAddr = valueAdd + loc->auth.runtimeOffset + ((uint64_t)pmd.high8 << 56);
                callback(fixupVMAddr, targetVMAddr, pmd);
            } while (delta != 0);
        }
    }
}


static bool findImageAndSegment(const DyldSharedCache* dyldCache, const std::vector<SegmentInfo>& segInfos, uint64_t cacheOffset, SegmentInfo* found)
{
    const uint64_t locVmAddr = dyldCache->unslidLoadAddress() + cacheOffset;
    const SegmentInfo target = { locVmAddr, 0, NULL, NULL };
    const auto lowIt = std::lower_bound(segInfos.begin(), segInfos.end(), target,
                                                                        [](const SegmentInfo& l, const SegmentInfo& r) -> bool {
                                                                            return l.vmAddr+l.vmSize < r.vmAddr+r.vmSize;
                                                                    });

    if ( lowIt == segInfos.end() )
        return false;

    if ( locVmAddr < lowIt->vmAddr )
        return false;
    if ( locVmAddr >= (lowIt->vmAddr  + lowIt->vmSize) )
        return false;

    *found = *lowIt;
    return true;
}

static void dumpObjCClassLayout(const DyldSharedCache* dyldCache)
{
    dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
        const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
        Diagnostics diag;

        uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
        __block objc_visitor::Visitor visitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
        visitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            const char* className = objcClass.getName(visitor);
            bool isMetaClass = objcClass.isMetaClass;
            uint32_t instanceStart = objcClass.getInstanceStart(visitor);
            objc_visitor::IVarList ivars = objcClass.getIVars(visitor);

            printf("%s (%s): start 0x%x\n", className, isMetaClass ? "metaclass" : "class", instanceStart);
            std::optional<metadata_visitor::ResolvedValue> superClassValue = objcClass.getSuperclass(visitor);
            if ( superClassValue.has_value() ) {
                bool unusedIsPatchable = false;
                objc_visitor::Class superClass(superClassValue.value(), isMetaClass, unusedIsPatchable);
                const char* superClassName = superClass.getName(visitor);
                uint32_t superStart = superClass.getInstanceStart(visitor);
                uint32_t superSize = superClass.getInstanceSize(visitor);
                printf("  super %s (%s): start 0x%x, size 0x%x\n", superClassName,
                       isMetaClass ? "metaclass" : "class", superStart, superSize);
            }

            uint32_t numIVars = ivars.numIVars();
            for ( uint32_t i = 0; i != numIVars; ++i ) {
                objc_visitor::IVar ivar = ivars.getIVar(visitor, i);
                std::optional<uint32_t> ivarStart = ivar.getOffset(visitor);
                const char* name = ivar.getName(visitor);
                printf("  ivar %s: 0x%x (start + 0x%d), alignment %d\n",
                       name,
                       ivarStart.has_value() ? ivarStart.value() : -1,
                       ivarStart.has_value() ? (ivarStart.value() - instanceStart) : -1,
                       ivar.getAlignment(visitor));
            }
        });
    });
}

template<typename ListTy>
static ListTy skipListsOfLists(ListTy&& list, const objc_visitor::Visitor& visitor)
{
    // we only want the class list. Ignore all ther other lists of lists
    if ( list.isListOfLists() ) {
        const ListOfListsEntry* listHeader = (ListOfListsEntry*)((uint8_t*) ((uint64_t)list.getLocation() & ~1));
        VMAddress methodListVMAddr = list.getVMAddress().value() - VMOffset(1ULL);

        if ( listHeader->count != 0 ) {
            uint32_t classListIndex = listHeader->count - 1;

            const ListOfListsEntry& listEntry = (listHeader + 1)[classListIndex];

            // The list entry is a relative offset to the target
            // Work out the VMAddress of that target
            VMOffset listEntryVMOffset{(uint64_t)&listEntry - (uint64_t)listHeader};
            VMAddress listEntryVMAddr = methodListVMAddr + listEntryVMOffset;
            VMAddress targetVMAddr = listEntryVMAddr + VMOffset((uint64_t)listEntry.offset);

            metadata_visitor::ResolvedValue classMethodListValue = visitor.getValueFor(targetVMAddr);
            ListTy classMethodList(classMethodListValue);

            return classMethodList;
        } else {
            return { std::nullopt };
        }
    }

    return list;
}

static void dumpObjCClassMethodLists(const DyldSharedCache* dyldCache)
{
    // Map from vmAddr to the category name for that address

    __block std::unordered_map<VMAddress, std::string, VMAddressHash, VMAddressEqual> categoryMap;
    dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
        const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
        Diagnostics diag;

        const char* leafName = strrchr(installName, '/');
        if ( leafName == NULL )
            leafName = installName;
        else
            leafName++;

        uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
        __block objc_visitor::Visitor visitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
        visitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
            const char* categoryName = objcCategory.getName(visitor);
            {
                objc_visitor::MethodList methodList = objcCategory.getClassMethods(visitor);
                std::optional<VMAddress> vmAddr = methodList.getVMAddress();
                if ( vmAddr.has_value() ) {
                    categoryMap[vmAddr.value()] = std::string(categoryName) + " - " + leafName;
                }
            }
            {
                objc_visitor::MethodList methodList = objcCategory.getInstanceMethods(visitor);
                std::optional<VMAddress> vmAddr = methodList.getVMAddress();
                if ( vmAddr.has_value() ) {
                    categoryMap[vmAddr.value()] = std::string(categoryName) + " - " + leafName;
                }
            }
            {
                objc_visitor::ProtocolList protocolList = objcCategory.getProtocols(visitor);
                std::optional<VMAddress> vmAddr = protocolList.getVMAddress();
                if ( vmAddr.has_value() ) {
                    categoryMap[vmAddr.value()] = std::string(categoryName) + " - " + leafName;
                }
            }
            {
                objc_visitor::PropertyList propertyList = objcCategory.getClassProperties(visitor);
                std::optional<VMAddress> vmAddr = propertyList.getVMAddress();
                if ( vmAddr.has_value() ) {
                    categoryMap[vmAddr.value()] = std::string(categoryName) + " - " + leafName;
                }
            }
            {
                objc_visitor::PropertyList propertyList = objcCategory.getInstanceProperties(visitor);
                std::optional<VMAddress> vmAddr = propertyList.getVMAddress();
                if ( vmAddr.has_value() ) {
                    categoryMap[vmAddr.value()] = std::string(categoryName) + " - " + leafName;
                }
            }
        });
    });

    __block std::map<uint64_t, const char*> dylibVMAddrMap;
    dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
        if ( hdr->hasObjC() )
            dylibVMAddrMap[hdr->preferredLoadAddress()] = installName;
    });

#if 0
        // Get a map of all dylibs in the cache from their "objc index" to install name
        __block std::map<uint16_t, const char*> dylibMap;

        const objc::HeaderInfoRO* headerInfoRO = dyldCache->objcHeaderInfoRO();
        const bool is64 = (strstr(dyldCache->archName(), "64") != nullptr) && (strstr(dyldCache->archName(), "64_32") == nullptr);
        if ( is64 ) {
            const auto* headerInfo64 = (objc::objc_headeropt_ro_t<uint64_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo64 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint64_t>* element = headerInfo64->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    dylibMap[headerInfo64->index(element)] = vmAddrAndName.second;
                }
            }
        } else {
            const auto* headerInfo32 = (objc::objc_headeropt_ro_t<uint32_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo32 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint32_t>* element = headerInfo32->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    dylibMap[headerInfo32->index(element)] = vmAddrAndName.second;
                }
            }
        }
    }
#endif

    // Print all method lists in the shared cache

    struct ListOfListsEntry {
        union {
            struct {
                uint64_t imageIndex: 16;
                int64_t  offset: 48;
            };
            struct {
                uint32_t entsize;
                uint32_t count;
            };
        };
    };

    __block std::unordered_set<VMAddress, VMAddressHash, VMAddressEqual> seenCategories;
    dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
        const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
        Diagnostics diag;

        printf("--- %s ---\n", installName);

        uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
        __block objc_visitor::Visitor visitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
        visitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            const char* className = objcClass.getName(visitor);
            bool isMetaClass = objcClass.isMetaClass;

            printf("%s (%s):\n", className, isMetaClass ? "metaclass" : "class");
            // method lists
            {
                objc_visitor::MethodList methodList = objcClass.getBaseMethods(visitor);
                if ( methodList.isListOfLists() ) {
                    const ListOfListsEntry* listHeader = (ListOfListsEntry*)((uint8_t*) ((uint64_t)methodList.getLocation() & ~1));
                    VMAddress methodListVMAddr = methodList.getVMAddress().value() - VMOffset(1ULL);

                    printf("(list of %d lists) {\n", listHeader->count);
                    for ( uint32_t i = 0; i != listHeader->count; ++i ) {
                        const ListOfListsEntry& listEntry = (listHeader + 1)[i];

                        // The list entry is a relative offset to the target
                        // Work out the VMAddress of that target
                        VMOffset listEntryVMOffset{(uint64_t)&listEntry - (uint64_t)listHeader};
                        VMAddress listEntryVMAddr = methodListVMAddr + listEntryVMOffset;
                        VMAddress targetVMAddr = listEntryVMAddr + VMOffset((uint64_t)listEntry.offset);

                        auto categoryIt = categoryMap.find(targetVMAddr);
                        if ( categoryIt != categoryMap.end() ) {
                            seenCategories.insert(targetVMAddr);
                            printf("  (category methods: image (%d) %s) {\n", listEntry.imageIndex, categoryIt->second.c_str());

                            metadata_visitor::ResolvedValue catMethodListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::MethodList catMethodList(catMethodListValue);
                            uint32_t numMethods = catMethodList.numMethods();
                            for ( uint32_t methodIndex = 0; methodIndex != numMethods; ++methodIndex ) {
                                objc_visitor::Method method = catMethodList.getMethod(visitor, methodIndex);
                                const char* name = method.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        } else {
                            // If we didn't find a category then we must be processing the class
                            // methods. These have to be last
                            if ( (i + 1) != listHeader->count ) {
                                fprintf(stderr, "Invalid method list on %s in %s\n", className, installName);
                                exit(1);
                            }
                            printf("  (class methods: image (%d)) {\n", listEntry.imageIndex);

                            metadata_visitor::ResolvedValue classMethodListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::MethodList classMethodList(classMethodListValue);
                            uint32_t numMethods = classMethodList.numMethods();
                            for ( uint32_t methodIndex = 0; methodIndex != numMethods; ++methodIndex ) {
                                objc_visitor::Method method = classMethodList.getMethod(visitor, methodIndex);
                                const char* name = method.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        }
                    }
                    printf("}\n");
                } else {
                    printf("(class methods) {\n");
                    uint32_t numMethods = methodList.numMethods();
                    for ( uint32_t methodIndex = 0; methodIndex != numMethods; ++methodIndex ) {
                        objc_visitor::Method method = methodList.getMethod(visitor, methodIndex);
                        const char* name = method.getName(visitor);
                        printf("  %s\n", name);
                    }
                    printf("}\n");
                }
            }

            // protocol lists
            if ( !isMetaClass) {
                objc_visitor::ProtocolList protocolList = objcClass.getBaseProtocols(visitor);
                if ( protocolList.isListOfLists() ) {

                    const ListOfListsEntry* listHeader = (ListOfListsEntry*)((uint8_t*) ((uint64_t)protocolList.getLocation() & ~1));
                    VMAddress protocolListVMAddr = protocolList.getVMAddress().value() - VMOffset(1ULL);

                    printf("(list of %d lists) {\n", listHeader->count);
                    for ( uint32_t i = 0; i != listHeader->count; ++i ) {
                        const ListOfListsEntry& listEntry = (listHeader + 1)[i];

                        // The list entry is a relative offset to the target
                        // Work out the VMAddress of that target
                        VMOffset listEntryVMOffset{(uint64_t)&listEntry - (uint64_t)listHeader};
                        VMAddress listEntryVMAddr = protocolListVMAddr + listEntryVMOffset;
                        VMAddress targetVMAddr = listEntryVMAddr + VMOffset((uint64_t)listEntry.offset);

                        auto categoryIt = categoryMap.find(targetVMAddr);
                        if ( categoryIt != categoryMap.end() ) {
                            seenCategories.insert(targetVMAddr);
                            printf("  (category protocols: image (%d) %s) {\n", listEntry.imageIndex, categoryIt->second.c_str());

                            metadata_visitor::ResolvedValue catProtocolListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::ProtocolList catProtocolList(catProtocolListValue);
                            uint64_t numProtocols = catProtocolList.numProtocols(visitor);
                            for ( uint64_t protocolIndex = 0; protocolIndex != numProtocols; ++protocolIndex ) {
                                objc_visitor::Protocol protocol = catProtocolList.getProtocol(visitor, protocolIndex);
                                const char* name = protocol.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        } else {
                            // If we didn't find a category then we must be processing the class
                            // protocols. These have to be last
                            if ( (i + 1) != listHeader->count ) {
                                fprintf(stderr, "Invalid protocol list on %s in %s\n", className, installName);
                                exit(1);
                            }
                            printf("  (class protocols: image (%d)) {\n", listEntry.imageIndex);

                            metadata_visitor::ResolvedValue classProtocolListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::ProtocolList classProtocolList(classProtocolListValue);
                            uint64_t numProtocols = classProtocolList.numProtocols(visitor);
                            for ( uint64_t protocolIndex = 0; protocolIndex != numProtocols; ++protocolIndex ) {
                                objc_visitor::Protocol protocol = classProtocolList.getProtocol(visitor, protocolIndex);
                                const char* name = protocol.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        }
                    }
                    printf("}\n");
                } else {
                    printf("(class protocols) {\n");
                    uint64_t numProtocols = protocolList.numProtocols(visitor);
                    for ( uint64_t protocolIndex = 0; protocolIndex != numProtocols; ++protocolIndex ) {
                        objc_visitor::Protocol protocol = protocolList.getProtocol(visitor, protocolIndex);
                        const char* name = protocol.getName(visitor);
                        printf("  %s\n", name);
                    }
                    printf("}\n");
                }
            }
            // property lists
            {
                objc_visitor::PropertyList propertyList = objcClass.getBaseProperties(visitor);
                if ( propertyList.isListOfLists() ) {
                    const ListOfListsEntry* listHeader = (ListOfListsEntry*)((uint8_t*) ((uint64_t)propertyList.getLocation() & ~1));
                    VMAddress propertyListVMAddr = propertyList.getVMAddress().value() - VMOffset(1ULL);

                    printf("(list of %d lists) {\n", listHeader->count);
                    for ( uint32_t i = 0; i != listHeader->count; ++i ) {
                        const ListOfListsEntry& listEntry = (listHeader + 1)[i];

                        // The list entry is a relative offset to the target
                        // Work out the VMAddress of that target
                        VMOffset listEntryVMOffset{(uint64_t)&listEntry - (uint64_t)listHeader};
                        VMAddress listEntryVMAddr = propertyListVMAddr + listEntryVMOffset;
                        VMAddress targetVMAddr = listEntryVMAddr + VMOffset((uint64_t)listEntry.offset);

                        auto categoryIt = categoryMap.find(targetVMAddr);
                        if ( categoryIt != categoryMap.end() ) {
                            seenCategories.insert(targetVMAddr);
                            printf("  (category properties: image (%d) %s) {\n", listEntry.imageIndex, categoryIt->second.c_str());

                            metadata_visitor::ResolvedValue catPropertyListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::PropertyList catPropertyList(catPropertyListValue);
                            uint32_t numProperties = catPropertyList.numProperties();
                            for ( uint32_t propertyIndex = 0; propertyIndex != numProperties; ++propertyIndex ) {
                                objc_visitor::Property property = catPropertyList.getProperty(visitor, propertyIndex);
                                const char* name = property.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        } else {
                            // If we didn't find a category then we must be processing the class
                            // properties. These have to be last
                            if ( (i + 1) != listHeader->count ) {
                                fprintf(stderr, "Invalid property list on %s in %s\n", className, installName);
                                exit(1);
                            }
                            printf("  (class properties: image (%d)) {\n", listEntry.imageIndex);

                            metadata_visitor::ResolvedValue classPropertyListValue = visitor.getValueFor(targetVMAddr);
                            objc_visitor::PropertyList classPropertyList(classPropertyListValue);
                            uint32_t numProperties = classPropertyList.numProperties();
                            for ( uint32_t propertyIndex = 0; propertyIndex != numProperties; ++propertyIndex ) {
                                objc_visitor::Property property = classPropertyList.getProperty(visitor, propertyIndex);
                                const char* name = property.getName(visitor);
                                printf("    %s\n", name);
                            }

                            printf("  }\n");
                        }
                    }
                    printf("}\n");
                } else {
                    printf("(class properties) {\n");
                    uint32_t numProperties = propertyList.numProperties();
                    for ( uint32_t propertyIndex = 0; propertyIndex != numProperties; ++propertyIndex ) {
                        objc_visitor::Property property = propertyList.getProperty(visitor, propertyIndex);
                        const char* name = property.getName(visitor);
                        printf("  %s\n", name);
                    }
                    printf("}\n");
                }
            }
        });
    });

    // Check if any categories weren't attached
    bool badCategory = false;
    for ( auto& [vmAddr, name] : categoryMap ) {
         if ( seenCategories.count(vmAddr) )
             continue;

        badCategory = true;
        fprintf(stderr, "Failed to find class with category: %s\n", name.c_str());
    }

    if ( badCategory )
        exit(1);
}


int main (int argc, const char* argv[]) {

    const char* sharedCachePath = nullptr;

    Options options;
    options.mode = modeNone;
    options.printUUIDs = false;
    options.printVMAddrs = false;
    options.printDylibVersions = false;
    options.printInodes = false;
    options.dependentsOfPath = NULL;
    options.extractionDir = NULL;

    bool printStrings = false;
    bool printExports = false;

    for (uint32_t i = 1; i < argc; i++) {
        const char* opt = argv[i];
        if (opt[0] == '-') {
            if (strcmp(opt, "-list") == 0) {
                checkMode(options.mode);
                options.mode = modeList;
            }
            else if (strcmp(opt, "-dependents") == 0) {
                checkMode(options.mode);
                options.mode = modeDependencies;
                options.dependentsOfPath = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -depdendents requires an argument\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-linkedit") == 0) {
                checkMode(options.mode);
                options.mode = modeLinkEdit;
            }
            else if (strcmp(opt, "-info") == 0) {
                checkMode(options.mode);
                options.mode = modeInfo;
            }
            else if (strcmp(opt, "-tpro") == 0) {
                checkMode(options.mode);
                options.mode = modeTPROInfo;
            }
            else if (strcmp(opt, "-stats") == 0) {
                checkMode(options.mode);
                options.mode = modeStats;
            }
            else if (strcmp(opt, "-slide_info") == 0) {
                checkMode(options.mode);
                options.mode = modeSlideInfo;
            }
            else if (strcmp(opt, "-verbose_slide_info") == 0) {
                checkMode(options.mode);
                options.mode = modeVerboseSlideInfo;
            }
            else if (strcmp(opt, "-fixups_in_dylib") == 0) {
                checkMode(options.mode);
                options.mode = modeFixupsInDylib;
                options.fixupsInDylib = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -fixups_in_dylib requires a path argument\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-text_info") == 0) {
                checkMode(options.mode);
                options.mode = modeTextInfo;
            }
            else if (strcmp(opt, "-local_symbols") == 0) {
                checkMode(options.mode);
                options.mode = modeLocalSymbols;
            }
            else if (strcmp(opt, "-strings") == 0) {
                if (options.mode != modeStrings)
                    checkMode(options.mode);
                options.mode = modeStrings;
                printStrings = true;
            }
            else if (strcmp(opt, "-sections") == 0) {
                checkMode(options.mode);
                options.mode = modeSectionSizes;
            }
            else if (strcmp(opt, "-exports") == 0) {
                if (options.mode != modeStrings)
                    checkMode(options.mode);
                options.mode = modeStrings;
                printExports = true;
            }
            else if (strcmp(opt, "-duplicate_exports") == 0) {
                options.mode = modeDuplicates;
            }
            else if (strcmp(opt, "-duplicate_exports_summary") == 0) {
                options.mode = modeDuplicatesSummary;
            }
            else if (strcmp(opt, "-map") == 0) {
                checkMode(options.mode);
                options.mode = modeMap;
            }
            else if (strcmp(opt, "-json-map") == 0) {
                checkMode(options.mode);
                options.mode = modeJSONMap;
            }
            else if (strcmp(opt, "-verbose-json-map") == 0) {
                checkMode(options.mode);
                options.mode = modeVerboseJSONMap;
            }
            else if (strcmp(opt, "-json-dependents") == 0) {
                checkMode(options.mode);
                options.mode = modeJSONDependents;
            }
            else if (strcmp(opt, "-size") == 0) {
                checkMode(options.mode);
                options.mode = modeSize;
            }
            else if (strcmp(opt, "-objc-info") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCInfo;
            }
            else if (strcmp(opt, "-objc-protocols") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCProtocols;
            }
            else if (strcmp(opt, "-objc-imp-caches") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCImpCaches;
            }
            else if (strcmp(opt, "-objc-classes") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCClasses;
            }
            else if (strcmp(opt, "-objc-class-layout") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCClassLayout;
            }
            else if (strcmp(opt, "-objc-class-method-lists") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCClassMethodLists;
            }
            else if (strcmp(opt, "-objc-class-hash-table") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCClassHashTable;
            }
            else if (strcmp(opt, "-objc-selectors") == 0) {
                checkMode(options.mode);
                options.mode = modeObjCSelectors;
            }
            else if (strcmp(opt, "-fs-root") == 0) {
                options.rootPath = argv[++i];
            }
            else if (strcmp(opt, "-swift-proto") == 0) {
                checkMode(options.mode);
                options.mode = modeSwiftProtocolConformances;
            }
            else if (strcmp(opt, "-swift-ptrtables") == 0) {
                checkMode(options.mode);
                options.mode = modeSwiftPtrTables;
            }
            else if (strcmp(opt, "-lookup-va") == 0) {
                checkMode(options.mode);
                options.mode = modeLookupVA;
                options.lookupVA = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -lookup-va requires an address argument\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-extract") == 0) {
                checkMode(options.mode);
                options.mode = modeExtract;
                options.extractionDir = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -extract requires a directory argument\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-uuid") == 0) {
                options.printUUIDs = true;
            }
            else if (strcmp(opt, "-inode") == 0) {
                options.printInodes = true;
            }
            else if (strcmp(opt, "-versions") == 0) {
                options.printDylibVersions = true;
            }
            else if (strcmp(opt, "-vmaddr") == 0) {
                options.printVMAddrs = true;
            }
            else if (strcmp(opt, "-patch_table") == 0) {
                options.mode = modePatchTable;
            }
            else if (strcmp(opt, "-function_variants") == 0) {
                options.mode = modeFunctionVariants;
            }
            else if (strcmp(opt, "-roots_cost") == 0) {
                checkMode(options.mode);
                options.mode = modeRootsCost;
                options.rootsCostOfDylib = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -roots_cost requires a path argument\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-list_dylibs_with_section") == 0) {
                options.mode = modeListDylibsWithSection;
                options.segmentName = argv[++i];
                options.sectionName = argv[++i];
                if ( i >= argc ) {
                    fprintf(stderr, "Error: option -list_dylibs_with_section requires a segment and section name\n");
                    usage();
                    exit(1);
                }
            }
            else if (strcmp(opt, "-mach_headers") == 0) {
                checkMode(options.mode);
                options.mode = modeMachHeaders;
            }
            else if (strcmp(opt, "-load_commands") == 0) {
                fprintf(stderr, "dyld_shared_cache_util -load_commands is deprecated.  Use dyld_info -load_commands instead\n");
            }
            else if (strcmp(opt, "-cache_header") == 0) {
                checkMode(options.mode);
                options.mode = modeCacheHeader;
            }
            else if (strcmp(opt, "-dylib_symbols") == 0) {
                checkMode(options.mode);
                options.mode = modeDylibSymbols;
            }
            else if (strcmp(opt, "-function_starts") == 0) {
                options.mode = modeFunctionStarts;
            }
            else if (strcmp(opt, "-prewarming_data") == 0) {
                options.mode = modePrewarmingData;
            }
            else {
                fprintf(stderr, "Error: unrecognized option %s\n", opt);
                usage();
                exit(1);
            }
        }
        else {
            sharedCachePath = opt;
        }
    }

    if ( options.mode == modeNone ) {
        fprintf(stderr, "Error: no command selected\n");
        usage();
        exit(1);
    }

    if ( options.mode != modeSlideInfo && options.mode != modeVerboseSlideInfo ) {
        if ( options.printUUIDs && (options.mode != modeList) )
            fprintf(stderr, "Warning: -uuid option ignored outside of -list mode\n");

        if ( options.printVMAddrs && (options.mode != modeList) )
            fprintf(stderr, "Warning: -vmaddr option ignored outside of -list mode\n");

        if ( options.printDylibVersions && (options.mode != modeDependencies) )
            fprintf(stderr, "Warning: -versions option ignored outside of -dependents mode\n");

        if ( (options.mode == modeDependencies) && (options.dependentsOfPath == NULL) ) {
            fprintf(stderr, "Error: -dependents given, but no dylib path specified\n");
            usage();
            exit(1);
        }
    }

    __block std::vector<const DyldSharedCache*> dyldCaches;

    const DyldSharedCache* dyldCache = nullptr;
    bool cacheOnDisk = false;
    if ( sharedCachePath != nullptr ) {
        dyldCaches = DyldSharedCache::mapCacheFiles(sharedCachePath);
        // mapCacheFile prints an error if something goes wrong, so just return in that case.
        if ( dyldCaches.empty() )
            return 1;
        dyldCache = dyldCaches.front();
        cacheOnDisk = true;
    }
    else {
        size_t cacheLength;
        dyldCache = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLength);
        if (dyldCache == nullptr) {
            fprintf(stderr, "Could not get in-memory shared cache\n");
            return 1;
        }
        if ( options.mode == modeObjCClasses ) {
            fprintf(stderr, "Cannot use -objc-classes with a live cache.  Please run with a path to an on-disk cache file\n");
            return 1;
        }
        if ( options.mode == modeObjCClassLayout ) {
            fprintf(stderr, "Cannot use -objc-class-layout with a live cache.  Please run with a path to an on-disk cache file\n");
            return 1;
        }
        if ( options.mode == modeObjCClassMethodLists ) {
            fprintf(stderr, "Cannot use -objc-class-method-lists with a live cache.  Please run with a path to an on-disk cache file\n");
            return 1;
        }
        if ( options.mode == modeVerboseSlideInfo ) {
            fprintf(stderr, "Cannot use -verbose_slide_info with a live cache.  Please run with a path to an on-disk cache file\n");
            return 1;
        }


        // The in-use cache might be the first cache file of many.  In that case, also add the sub caches
        dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
            dyldCaches.push_back(dyldCache);
        });
        cacheOnDisk = false;
    }

    if ( options.mode == modeSlideInfo || options.mode == modeVerboseSlideInfo ) {
        if ( dyldCache->numSubCaches() == 0 ) {
            if ( !dyldCache->hasSlideInfo() ) {
                fprintf(stderr, "Error: dyld shared cache does not contain slide info\n");
                exit(1);
            }
        }

        const bool verboseSlideInfo = (options.mode == modeVerboseSlideInfo);
        dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
            cache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *mappingPagesStart,
                                          uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfoHeader) {
                printSlideInfoForDataRegion(cache, mappingStartAddress, mappingSize, mappingPagesStart,
                                            slideInfoHeader, verboseSlideInfo);
            });
        });
        return 0;
    }
    else if ( options.mode == modeFixupsInDylib ) {
        if ( dyldCache->numSubCaches() == 0 ) {
            if ( !dyldCache->hasSlideInfo() ) {
                fprintf(stderr, "Error: dyld shared cache does not contain slide info\n");
                exit(1);
            }
        }

        uint32_t imageIndex = ~0U;
        if ( !dyldCache->hasImagePath(options.fixupsInDylib, imageIndex) ) {
            fprintf(stderr, "Error: dyld shared cache does not contain image: %s\n",
                    options.fixupsInDylib);
            exit(1);
        }

        const Header* hdr = (const Header*)dyldCache->getIndexedImageEntry(imageIndex);

        __block std::vector<SegmentInfo> dylibSegInfo;
        buildSegmentInfo(hdr, dylibSegInfo);
        sortSegmentInfo(dylibSegInfo);

        __block std::vector<SegmentInfo> cacheSegInfo;
        buildSegmentInfo(dyldCache, cacheSegInfo);

        uint64_t cacheBaseAddress = dyldCache->unslidLoadAddress();
        auto handler = ^(uint64_t fixupVMAddr, uint64_t targetVMAddr,
                         PointerMetaData pmd)
        {
            SegmentInfo fixupAt;
            if ( !findImageAndSegment(dyldCache, dylibSegInfo, fixupVMAddr - cacheBaseAddress, &fixupAt) ) {
                // Fixup is not in the given dylib
                return;
            }

            // Remove high8 if we have it
            uint64_t high8 = targetVMAddr >> 56;
            targetVMAddr = targetVMAddr & 0x00FFFFFFFFFFFFFF;

            SegmentInfo targetAt;
            if ( !findImageAndSegment(dyldCache, cacheSegInfo, targetVMAddr - cacheBaseAddress, &targetAt) ) {
                return;
            }

            if ( pmd.authenticated ) {
                static const char* keyNames[] = {
                    "IA", "IB", "DA", "DB"
                };
                printf("%.*s(0x%04llX) -> %.*s(0x%04llX):%s; (PAC: div=%d, addr=%s, key=%s)\n",
                       (int)fixupAt.segName.size(), fixupAt.segName.data(), fixupVMAddr - fixupAt.vmAddr,
                       (int)targetAt.segName.size(), targetAt.segName.data(), targetVMAddr - targetAt.vmAddr, targetAt.installName,
                       pmd.diversity, pmd.usesAddrDiversity ? "true" : "false", keyNames[pmd.key]);
            } else {
                if ( high8 != 0 ) {
                    printf("%.*s(0x%04llX) -> %.*s(0x%04llX):%s; (high8: 0x%02llX)\n",
                           (int)fixupAt.segName.size(), fixupAt.segName.data(), fixupVMAddr - fixupAt.vmAddr,
                           (int)targetAt.segName.size(), targetAt.segName.data(), targetVMAddr - targetAt.vmAddr, targetAt.installName,
                           high8);
                } else {
                    printf("%.*s(0x%04llX) -> %.*s(0x%04llX):%s\n",
                           (int)fixupAt.segName.size(), fixupAt.segName.data(), fixupVMAddr - fixupAt.vmAddr,
                           (int)targetAt.segName.size(), targetAt.segName.data(), targetVMAddr - targetAt.vmAddr, targetAt.installName);
                }
            }
        };

        dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
            cache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *mappingPagesStart,
                                          uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfoHeader) {
                forEachSlidValue(cache, mappingStartAddress, mappingSize, mappingPagesStart,
                                 slideInfoHeader, handler);
            });
        });
        return 0;
    }
    else if ( options.mode == modeInfo ) {
        const dyld_cache_header* header = &dyldCache->header;
        uuid_string_t uuidString;
        uuid_unparse_upper(header->uuid, uuidString);
        printf("uuid: %s\n", uuidString);
        printf("platform: %s\n", dyldCache->platform().name().c_str());
        printf("built by: %s\n", header->locallyBuiltCache ? "local machine" : "B&I");
        printf("cache type: %s\n", DyldSharedCache::getCacheTypeName(header->cacheType));
        if ( header->dylibsExpectedOnDisk )
            printf("dylibs expected on disk: true\n");
        if ( header->cacheType == kDyldSharedCacheTypeUniversal )
            printf("cache sub-type: %s\n", DyldSharedCache::getCacheTypeName(header->cacheSubType));
        if ( header->mappingOffset >= offsetof(dyld_cache_header, imagesCount) ) {
            printf("image count: %u\n", header->imagesCount);
        } else {
            printf("image count: %u\n", header->imagesCountOld);
        }
        if ( (header->mappingOffset >= 0x78) && (header->branchPoolsOffset != 0) ) {
            printf("branch pool count:  %u\n", header->branchPoolsCount);
        }
        {
            uint32_t pageSize            = 0x4000; // fix me for intel
            uint32_t possibleSlideValues = (uint32_t)(header->maxSlide/pageSize);
            uint32_t entropyBits = 0;
            if ( possibleSlideValues > 1 )
                entropyBits = __builtin_clz(possibleSlideValues - 1);
            printf("ASLR entropy: %u-bits (%lldMB)\n", entropyBits, header->maxSlide >> 20);
        }
        printf("mappings:\n");
        dyldCache->forEachRange(^(const char *mappingName, uint64_t unslidVMAddr, uint64_t vmSize,
                                  uint32_t cacheFileIndex, uint64_t fileOffset, uint32_t initProt, uint32_t maxProt, bool& stopRange) {
            std::string initProtString;
            initProtString += (initProt & VM_PROT_READ) ? "r" : "-";
            initProtString += (initProt & VM_PROT_WRITE) ? "w" : "-";
            initProtString += (initProt & VM_PROT_EXECUTE) ? "x" : "-";

            std::string maxProtString;
            maxProtString += (maxProt & VM_PROT_READ) ? "r" : "-";
            maxProtString += (maxProt & VM_PROT_WRITE) ? "w" : "-";
            maxProtString += (maxProt & VM_PROT_EXECUTE) ? "x" : "-";

            printf("%20s %4lluMB,  file offset: #%u/0x%08llX -> 0x%08llX,  address: 0x%08llX -> 0x%08llX, %s -> %s\n",
                   mappingName, vmSize / (1024*1024), cacheFileIndex, fileOffset, fileOffset + vmSize,
                   unslidVMAddr, unslidVMAddr + vmSize, initProtString.c_str(), maxProtString.c_str());
            if (header->mappingOffset >=  offsetof(dyld_cache_header, dynamicDataOffset)) {
                if ( (unslidVMAddr + vmSize) == (header->sharedRegionStart + header->dynamicDataOffset) ) {
                    printf("  dynamic config %4lluKB,                                             address: 0x%08llX -> 0x%08llX\n",
                           header->dynamicDataMaxSize/1024, header->sharedRegionStart + header->dynamicDataOffset,
                           header->sharedRegionStart + header->dynamicDataOffset + header->dynamicDataMaxSize);
                }
            }
        }, ^(const DyldSharedCache* subCache, uint32_t cacheFileIndex) {
            const dyld_cache_header* subCacheHeader = &subCache->header;

            if ( subCacheHeader->codeSignatureSize != 0) {
                    printf("%20s %4lluMB,  file offset: #%u/0x%08llX -> 0x%08llX\n",
                           "code sign", subCacheHeader->codeSignatureSize/(1024*1024), cacheFileIndex,
                           subCacheHeader->codeSignatureOffset, subCacheHeader->codeSignatureOffset + subCacheHeader->codeSignatureSize);
            }

            if ( subCacheHeader->mappingOffset > offsetof(dyld_cache_header, rosettaReadOnlySize) ) {
                if ( subCacheHeader->rosettaReadOnlySize != 0 ) {
                    printf("Rosetta RO:      %4lluMB,                                          address: 0x%08llX -> 0x%08llX\n",
                           subCacheHeader->rosettaReadOnlySize/(1024*1024), subCacheHeader->rosettaReadOnlyAddr,
                           subCacheHeader->rosettaReadOnlyAddr + subCacheHeader->rosettaReadOnlySize);
                }
                if ( subCacheHeader->rosettaReadWriteSize != 0 ) {
                    printf("Rosetta RW:      %4lluMB,                                          address: 0x%08llX -> 0x%08llX\n",
                           subCacheHeader->rosettaReadWriteSize/(1024*1024), subCacheHeader->rosettaReadWriteAddr,
                           subCacheHeader->rosettaReadWriteAddr + subCacheHeader->rosettaReadWriteSize);
                }
            }

            subCache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *mappingPagesStart,
                                         uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfoHeader) {

                printf("slide info:      %4lluKB,  file offset: #%u/0x%08llX -> 0x%08llX\n",
                       slideInfoSize/1024, cacheFileIndex, slideInfoOffset, slideInfoOffset + slideInfoSize);
            });
            if ( subCacheHeader->localSymbolsOffset != 0 )
                printf("local symbols:    %3lluMB,  file offset: #%u/0x%08llX -> 0x%08llX\n",
                       subCacheHeader->localSymbolsSize/(1024*1024), cacheFileIndex,
                       subCacheHeader->localSymbolsOffset, subCacheHeader->localSymbolsOffset + subCacheHeader->localSymbolsSize);
        });
    }
    else if ( options.mode == modeTPROInfo ) {
        printf("TPRO mappings:\n");
        __block bool foundMapping = false;
        dyldCache->forEachTPRORegion(^(const void *content, uint64_t unslidVMAddr, uint64_t vmSize, bool &stopRegion) {
            printf("    %4lluKB, address: 0x%08llX -> 0x%08llX\n", vmSize / 1024, unslidVMAddr, unslidVMAddr + vmSize);
            foundMapping = true;
        });
        if ( !foundMapping )
            printf("    none found\n");
    }
    else if ( options.mode == modeStats ) {
        __block std::map<std::string_view, uint64_t> mappingSizes;
        __block uint64_t totalFileSize = 0;
        __block uint64_t minVMAddr = UINT64_MAX;
        __block uint64_t maxVMAddr = 0;

        dyldCache->forEachRange(^(const char *mappingName, uint64_t unslidVMAddr, uint64_t vmSize,
                                  uint32_t cacheFileIndex, uint64_t fileOffset, uint32_t initProt, uint32_t maxProt, bool& stopRange) {
            mappingSizes[mappingName] += vmSize;
            totalFileSize += vmSize;
            minVMAddr = std::min(minVMAddr, unslidVMAddr);
            maxVMAddr = std::max(maxVMAddr, unslidVMAddr + vmSize);
        }, nullptr);

        uint64_t totalVMSize = maxVMAddr - minVMAddr;

        printf("-stats:\n");
        printf("  total file size: %lldMB\n", totalFileSize >> 20);
        printf("  total VM size: %lldMB\n", totalVMSize >> 20);
        for ( const auto& mappingNameAndSize : mappingSizes )
            printf("  total VM size (%s): %lldMB\n", mappingNameAndSize.first.data(), mappingNameAndSize.second >> 20);
    }
    else if ( options.mode == modeTextInfo ) {
        const dyld_cache_header* header = &dyldCache->header;
        printf("dylib text infos (count=%llu):\n", header->imagesTextCount);
        dyldCache->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const unsigned char *dylibUUID, const char *installName, bool &stop) {
            uuid_string_t uuidString;
            uuid_unparse_upper(dylibUUID, uuidString);
            printf("   0x%09llX -> 0x%09llX  <%s>  %s\n", loadAddressUnslid, loadAddressUnslid + textSegmentSize, uuidString, installName);
        });
    }
    else if ( options.mode == modeLocalSymbols ) {
        if ( !dyldCache->hasLocalSymbolsInfo() && !dyldCache->hasLocalSymbolsInfoFile() ) {
            fprintf(stderr, "Error: dyld shared cache does not contain local symbols info\n");
            exit(1);
        }

        if ( sharedCachePath == nullptr ) {
            fprintf(stderr, "Cannot use -local_symbols with a live cache.  Please run with a path to an on-disk cache file\n");
            exit(1);
        }

        // The locals are in an unmapped part of the cache.  So use the introspection APIs to map them in
        // For now only support the case where the cache was passed in as a file, not the live cache
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
        const bool is64 = (strstr(dyldCache->archName(), "64") != nullptr) && (strstr(dyldCache->archName(), "64_32") == nullptr);
        bool mappedCacheFile = dyld_shared_cache_for_file(sharedCachePath, ^(dyld_shared_cache_t cache) {
            __block uint32_t entriesCount = 0;
            dyld_shared_cache_for_each_image(cache, ^(dyld_image_t image) {
                // FIXME: Use dyld_image_get_file_path(image) when its available
                const char* imageName = dyldCache->getIndexedImagePath(entriesCount);
                bool foundNList = dyld_image_local_nlist_content_4Symbolication(image,
                                                                                ^(const void* nlistStart, uint64_t nlistCount,
                                                                                  const char* stringTable) {
                    printf("Local symbols nlist for: %s\n", imageName);
                    if ( is64 ) {
                        const nlist_64* symTab = (nlist_64*)nlistStart;
                        for (int e = 0; e < nlistCount; ++e) {
                            const nlist_64* entry = &symTab[e];
                            printf("     nlist[%d].str=%d, %s\n", e, entry->n_un.n_strx, &stringTable[entry->n_un.n_strx]);
                            printf("     nlist[%d].value=0x%0llX\n", e, entry->n_value);
                        }
                    } else {
                        const struct nlist* symTab = (struct nlist*)nlistStart;
                        for (int e = 0; e < nlistCount; ++e) {
                            const struct nlist* entry = &symTab[e];
                            printf("     nlist[%d].str=%d, %s\n", e, entry->n_un.n_strx, &stringTable[entry->n_un.n_strx]);
                            printf("     nlist[%d].value=0x%0X\n", e, entry->n_value);
                        }
                    }
                });
                if ( !foundNList ) {
                    fprintf(stderr, "Error: Failed to find local symbols nlist for: %s\n", imageName);
                    exit(1);
                }
                entriesCount++;
            });
            printf("local symbols by dylib (count=%d):\n", entriesCount);
        });

        if ( !mappedCacheFile ) {
            fprintf(stderr, "Error: Failed to map local symbols for shared cache file\n");
            exit(1);
        }
#pragma clang diagnostic pop

#if 0
        const bool is64 = (strstr(dyldCache->archName(), "64") != NULL);
        const uint32_t nlistFileOffset = (uint32_t)((uint8_t*)dyldCache->getLocalNlistEntries() - (uint8_t*)dyldCache);
        const uint32_t nlistCount = dyldCache->getLocalNlistCount();
        const uint32_t nlistByteSize = is64 ? nlistCount*16 : nlistCount*12;
        const char* localStrings = dyldCache->getLocalStrings();
        const uint32_t stringsFileOffset = (uint32_t)((uint8_t*)localStrings - (uint8_t*)dyldCache);
        const uint32_t stringsSize = dyldCache->getLocalStringsSize();

        printf("local symbols nlist array:  %3uMB,  file offset: 0x%08X -> 0x%08X\n", nlistByteSize/(1024*1024), nlistFileOffset, nlistFileOffset+nlistByteSize);
        printf("local symbols string pool:  %3uMB,  file offset: 0x%08X -> 0x%08X\n", stringsSize/(1024*1024), stringsFileOffset, stringsFileOffset+stringsSize);

        __block uint32_t entriesCount = 0;
        dyldCache->forEachLocalSymbolEntry(^(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nCount, bool &stop) {
            const char* imageName = dyldCache->getIndexedImagePath(entriesCount);
            printf("   nlistStartIndex=%5d, nlistCount=%5d, image=%s\n", nlistStartIndex, nCount, imageName);
#if 0
            if ( is64 ) {
                const nlist_64* symTab = (nlist_64*)((char*)dyldCache + nlistFileOffset);
                for (int e = 0; e < nlistLocalCount; ++e) {
                    const nlist_64* entry = &symTab[nlistStartIndex + e];
                    printf("     nlist[%d].str=%d, %s\n", e, entry->n_un.n_strx, &localStrings[entry->n_un.n_strx]);
                    printf("     nlist[%d].value=0x%0llX\n", e, entry->n_value);
                }
            }
#endif
            entriesCount++;
        });
        printf("local symbols by dylib (count=%d):\n", entriesCount);
#endif
    }
    else if ( (options.mode == modeJSONMap) || (options.mode == modeVerboseJSONMap) ) {
        bool verbose = (options.mode == modeVerboseJSONMap);
        uuid_t uuid;
        dyldCache->getUUID(uuid);
        std::string buffer = dyldCache->generateJSONMap("unknown", uuid, verbose);
        printf("%s\n", buffer.c_str());
    }
    else if ( options.mode == modeJSONDependents ) {
        std::cout <<  dyldCache->generateJSONDependents();
    }
    else if ( options.mode == modeStrings ) {
        if (printStrings) {
            // The cache has not been slid if we loaded it from disk
            bool cacheRebased = (sharedCachePath == nullptr);
            dyld3::MachOAnalyzer::VMAddrConverter vmAddrConverter = dyldCache->makeVMAddrConverter(cacheRebased);
            if ( !cacheRebased )
                dyldCache->applyCacheRebases();

            uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();

            dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
                __block std::unordered_set<std::string_view> seenStrings;
                const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                int64_t slide = ma->getSlide();
                uint32_t pointerSize = ma->pointerSize();

                ((const Header*)ma)->forEachSection(^(const Header::SectionInfo& info, bool& stop) {
                    if ( ( (info.flags & SECTION_TYPE) == S_CSTRING_LITERALS ) ) {
                        const uint8_t* content = (uint8_t*)(info.address + slide);
                        const char* s   = (char*)content;
                        const char* end = s + info.size;
                        while ( s < end ) {
                            printf("%s: %s\n", installName, s);
                            seenStrings.insert(s);
                            while (*s != '\0' )
                                ++s;
                            ++s;
                        }
                    }
                });

                // objc string sections are coalesced in the builder, so might not be present above
                // Find referenced objc strings by walking the other objc metadata
                auto printString = ^(uint64_t stringVMAddr) {
                    const char* selString = (const char*)stringVMAddr + slide;
                    auto itAndInserted = seenStrings.insert(selString);
                    if ( itAndInserted.second )
                        printf("%s: %s\n", installName, selString);
                };

                auto visitMethod = ^(uint64_t methodVMAddr, const dyld3::MachOAnalyzer::ObjCMethod& method,
                                     bool& stopMethod) {
                    printString(method.nameVMAddr);
                    printString(method.typesVMAddr);
                };

                auto visitProperty = ^(uint64_t propertyVMAddr, const dyld3::MachOAnalyzer::ObjCProperty& property) {
                    printString(property.nameVMAddr);
                };

                Diagnostics diag;
                ma->forEachObjCSelectorReference(diag, vmAddrConverter,
                                                 ^(uint64_t selRefVMAddr, uint64_t selRefTargetVMAddr, bool &stop) {
                    printString(selRefTargetVMAddr);
                });

                // If the cache hasn't been rebased, then we can also print other objc metadata, such as classes
                // If we are doing this, then we need to patch the cache to undo the bit-stealing in the ASLR format
                if ( !cacheRebased ) {
                    auto visitClass = ^(uint64_t classVMAddr,
                                        uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr,
                                        const dyld3::MachOAnalyzer::ObjCClassInfo& objcClass, bool isMetaClass,
                                        bool& stop) {
                        printString(objcClass.nameVMAddr(pointerSize));
                        ma->forEachObjCMethod(objcClass.baseMethodsVMAddr(pointerSize), vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCProperty(objcClass.basePropertiesVMAddr(pointerSize), vmAddrConverter, visitProperty);
                    };

                    auto visitCategory = ^(uint64_t categoryVMAddr,
                                           const dyld3::MachOAnalyzer::ObjCCategory& objcCategory,
                                           bool& stopCategory) {
                        printString(objcCategory.nameVMAddr);
                        ma->forEachObjCMethod(objcCategory.instanceMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCMethod(objcCategory.classMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCMethod(objcCategory.instanceMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCProperty(objcCategory.instancePropertiesVMAddr, vmAddrConverter, visitProperty);
                    };

                    auto visitProtocol = ^(uint64_t protoVMAddr,
                                           const dyld3::MachOAnalyzer::ObjCProtocol& objcProto,
                                           bool& stopProtocol) {
                        printString(objcProto.nameVMAddr);
                        ma->forEachObjCMethod(objcProto.instanceMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCMethod(objcProto.classMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCMethod(objcProto.optionalInstanceMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                        ma->forEachObjCMethod(objcProto.optionalClassMethodsVMAddr, vmAddrConverter, sharedCacheRelativeSelectorBaseVMAddress, visitMethod);
                    };

                    ma->forEachObjCClass(diag, vmAddrConverter, visitClass);
                    ma->forEachObjCCategory(diag, vmAddrConverter, visitCategory);
                    ma->forEachObjCProtocol(diag, vmAddrConverter, visitProtocol);
                }
            });
        }

        if (printExports) {
            dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
                const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                uint32_t exportTrieRuntimeOffset;
                uint32_t exportTrieSize;
                if ( ma->hasExportTrie(exportTrieRuntimeOffset, exportTrieSize) ) {
                    const uint8_t* start = (uint8_t*)hdr + exportTrieRuntimeOffset;
                    const uint8_t* end = start + exportTrieSize;
                    std::vector<ExportInfoTrie::Entry> exports;
                    if ( !ExportInfoTrie::parseTrie(start, end, exports) ) {
                        return;
                    }

                    for (const ExportInfoTrie::Entry& entry: exports) {
                        const char* resolver = "";
                        if ( entry.info.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER )
                            resolver = " (resolver)";
                        printf("%s: %s%s\n", installName, entry.name.c_str(), resolver);
                    }
                }
            });
        }
    }
    else if ( options.mode == modeSectionSizes ) {
        __block std::map<std::string, uint64_t> sectionSizes;
        dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)hdr;
            ((const Header*)ma)->forEachSection(^(const Header::SectionInfo &sectInfo, bool &stop) {
                std::string section = std::string(sectInfo.segmentName) + " " + std::string(sectInfo.sectionName);
                sectionSizes[section] += sectInfo.size;
            });
        });
        for (const auto& keyAndValue : sectionSizes) {
            printf("%lld %s\n", keyAndValue.second, keyAndValue.first.c_str());
        }
    }
    else if ( options.mode == modeObjCInfo ) {
        if ( !dyldCache->hasOptimizedObjC() ) {
            fprintf(stderr, "Error: could not get optimized objc\n");
            return 1;
        }

        printf("version:                            %u\n", dyldCache->objcOptVersion());
        printf("flags:                              0x%08x\n", dyldCache->objcOptFlags());
        if ( const objc::SelectorHashTable* selectors = dyldCache->objcSelectorHashTable() ) {
            printf("num selectors:                      %u\n", selectors->occupancy());
        }
        if ( const objc::ClassHashTable* classes = dyldCache->objcClassHashTable() ) {
            printf("num classes:                        %u\n", classes->occupancy());
        }
        if ( const objc::ProtocolHashTable* protocols = dyldCache->objcProtocolHashTable() ) {
            printf("num protocols:                      %u\n", protocols->occupancy());
        }
        if ( const void* relativeMethodListSelectorBase = dyldCache->objcRelativeMethodListsBaseAddress() ) {
            printf("method list selector base address:  0x%llx\n", dyldCache->unslidLoadAddress() + ((uint64_t)relativeMethodListSelectorBase - (uint64_t)dyldCache));
            printf("method list selector base value:    \"%s\"\n", (const char*)relativeMethodListSelectorBase);
        }

        // Dump the objc indices

        __block std::map<uint64_t, const char*> dylibVMAddrMap;
        dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
            if ( hdr->hasObjC() )
                dylibVMAddrMap[hdr->preferredLoadAddress()] = installName;
        });

        std::vector<std::pair<std::string_view, const objc::objc_image_info*>> objcDylibs;

        const objc::HeaderInfoRO* headerInfoRO = dyldCache->objcHeaderInfoRO();
        const bool is64 = (strstr(dyldCache->archName(), "64") != nullptr) && (strstr(dyldCache->archName(), "64_32") == nullptr);
        if ( is64 ) {
            const auto* headerInfo64 = (objc::objc_headeropt_ro_t<uint64_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo64 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint64_t>* element = headerInfo64->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    objcDylibs.resize(headerInfo64->index(element) + 1);
                    objcDylibs[headerInfo64->index(element)] = { vmAddrAndName.second, (const objc::objc_image_info*)element->imageInfo() };
                }
            }
        } else {
            const auto* headerInfo32 = (objc::objc_headeropt_ro_t<uint32_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo32 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint32_t>* element = headerInfo32->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    objcDylibs.resize(headerInfo32->index(element) + 1);
                    objcDylibs[headerInfo32->index(element)] = { vmAddrAndName.second, (const objc::objc_image_info*)element->imageInfo() };
                }
            }
        }

        printf("num objc dylibs:                      %lu\n", objcDylibs.size());
        for ( uint32_t i = 0; i != objcDylibs.size(); ++i ) {
            const std::pair<std::string_view, const objc::objc_image_info*> objcDylib = objcDylibs[i];

            // Try work out which flags we have
            std::string flagsStr;
            uint32_t flags = objcDylib.second->flags;
            std::pair<uint32_t, const char*> flagComponents[] = {
                { 1 << 0,       "dyldCategories" },
                { 1 << 1,       "supportsGC" },
                { 1 << 2,       "requiresGC" },
                { 1 << 3,       "optimizedByDyld" },
                { 1 << 4,       "signedClassRO" },
                { 1 << 5,       "isSimulated" },
                { 1 << 6,       "hasCategoryClassProperties" },
                { 1 << 7,       "optimizedByDyldClosure" },
                { 0xFF << 8,    "swiftUnstableVersion" },
                { 0xFFFF << 16, "swiftVersion" },
            };
            bool needsSeparator = false;
            for ( auto [mask, name] : flagComponents ) {
                if ( (flags & mask) != 0 ) {
                    if ( needsSeparator )
                        flagsStr += " | ";
                    needsSeparator = true;

                    flagsStr += name;
                }
            }
            printf("dylib[%d]: { 0x%x, 0x%08x } (%s) %s\n",
                   i, objcDylib.second->version, objcDylib.second->flags,
                   flagsStr.c_str(), objcDylib.first.data());
        }
    }
    else if ( options.mode == modeObjCProtocols ) {
        if ( !dyldCache->hasOptimizedObjC() ) {
            fprintf(stderr, "Error: could not get optimized objc\n");
            return 1;
        }
        const objc::ProtocolHashTable* protocols = dyldCache->objcProtocolHashTable();
        if ( protocols == nullptr ) {
            fprintf(stderr, "Error: could not get optimized objc protocols\n");
            return 1;
        }

        __block std::map<uint64_t, const char*> dylibVMAddrMap;
        dyldCache->forEachImage(^(const Header *hdr, const char *installName) {
            if ( hdr->hasObjC() )
                dylibVMAddrMap[hdr->preferredLoadAddress()] = installName;
        });

        __block std::map<uint16_t, const char*> dylibMap;

        const objc::HeaderInfoRO* headerInfoRO = dyldCache->objcHeaderInfoRO();
        const bool is64 = (strstr(dyldCache->archName(), "64") != nullptr) && (strstr(dyldCache->archName(), "64_32") == nullptr);
        if ( is64 ) {
            const auto* headerInfo64 = (objc::objc_headeropt_ro_t<uint64_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo64 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint64_t>* element = headerInfo64->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    dylibMap[headerInfo64->index(element)] = vmAddrAndName.second;
                }
            }
        } else {
            const auto* headerInfo32 = (objc::objc_headeropt_ro_t<uint32_t>*)headerInfoRO;
            uint64_t headerInfoVMAddr = dyldCache->unslidLoadAddress();
            headerInfoVMAddr += (uint64_t)headerInfo32 - (uint64_t)dyldCache;
            for ( std::pair<uint64_t, const char*> vmAddrAndName : dylibVMAddrMap ) {
                const objc::objc_header_info_ro_t<uint32_t>* element = headerInfo32->get(headerInfoVMAddr, vmAddrAndName.first);
                if ( element != nullptr ) {
                    dylibMap[headerInfo32->index(element)] = vmAddrAndName.second;
                }
            }
        }

        typedef objc::ProtocolHashTable::ObjectAndDylibIndex ObjectAndDylibIndex;
        protocols->forEachProtocol(^(uint32_t bucketIndex, const char* protocolName,
                                     const dyld3::Array<ObjectAndDylibIndex>& implCacheInfos) {

            if ( implCacheInfos.empty() ) {
                // Empty bucket
                printf("[% 5d]\n", bucketIndex);
                return;
            }

            if ( implCacheInfos.count() == 1 ) {
                // No duplicates
                printf("[% 5d] -> (% 8lld, %4d) = %s (in %s)\n",
                       bucketIndex, implCacheInfos[0].first, implCacheInfos[0].second, protocolName,
                       dylibMap.at(implCacheInfos[0].second));
                return;
            }

            // class appears in more than one header
            fprintf(stderr, "[% 5d] -> %llu duplicates = %s\n", bucketIndex, implCacheInfos.count(), protocolName);
            for (const ObjectAndDylibIndex& objectInfo : implCacheInfos) {
                printf("  - [% 5d] -> (% 8lld, %4d) = %s in (%s)\n",
                       bucketIndex, objectInfo.first, objectInfo.second, protocolName,
                       dylibMap.at(objectInfo.second));
            }
        });
    }
    else if ( options.mode == modeObjCClassHashTable ) {
        if ( !dyldCache->hasOptimizedObjC() ) {
            fprintf(stderr, "Error: could not get optimized objc\n");
            return 1;
        }
        const objc::ClassHashTable* classes = dyldCache->objcClassHashTable();
        if ( classes == nullptr ) {
            fprintf(stderr, "Error: could not get optimized objc classes\n");
            return 1;
        }

        typedef objc::ClassHashTable::ObjectAndDylibIndex ObjectAndDylibIndex;
        classes->forEachClass(^(uint32_t bucketIndex, const char* className,
                                const dyld3::Array<ObjectAndDylibIndex>& implCacheInfos) {
            if ( implCacheInfos.empty() ) {
                // Empty bucket
                printf("[% 5d]\n", bucketIndex);
                return;
            }

            if ( implCacheInfos.count() == 1 ) {
                // No duplicates
                printf("[% 5d] -> (% 8lld, %4d) = %s\n",
                       bucketIndex, implCacheInfos[0].first, implCacheInfos[0].second, className);
                return;
            }

            // class appears in more than one header
            printf("[% 5d] -> %llu duplicates = %s\n", bucketIndex, implCacheInfos.count(), className);
            for (const ObjectAndDylibIndex& objectInfo : implCacheInfos) {
                printf("  - [% 5d] -> (% 8lld, %4d) = %s\n",
                       bucketIndex, objectInfo.first, objectInfo.second, className);
            }
        });
    }
    else if ( options.mode == modeObjCClasses ) {

        // If we are running on macOS against a cache for another device, then we need a root path to find on-disk dylibs/executables
        if ( Platform(dyld_get_active_platform()) != dyldCache->platform() ) {
            if ( options.rootPath == nullptr ) {
                fprintf(stderr, "Analyzing cache file requires a root path for on-disk binaries.  Rerun with -fs-root *path*\n");
                return 1;
            }
        }

        dyldCache->applyCacheRebases();

        auto getString = ^const char *(const dyld3::MachOAnalyzer* ma, VMAddress nameVMAddr){
            dyld3::MachOAnalyzer::PrintableStringResult result;
            const char* name = ma->getPrintableString(nameVMAddr.rawValue(), result);
            if (result == dyld3::MachOAnalyzer::PrintableStringResult::CanPrint)
                return name;
            return nullptr;
        };

        uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();

        using json::Node;
        using json::NodeValueType;

        std::string instancePrefix("-");
        std::string classPrefix("+");

        // Build a map of class vm addrs to their names so that categories know the
        // name of the class they are attaching to
        __block std::unordered_map<uint64_t, const char*> classVMAddrToName;
        __block std::unordered_map<uint64_t, const char*> metaclassVMAddrToName;
        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;

            __block objc_visitor::Visitor visitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));

            auto visitClass = ^(const objc_visitor::Class& objcClass, bool& stopClass) {
                VMAddress classVMAddr = objcClass.getVMAddress();
                VMAddress nameVMAddr = objcClass.getNameVMAddr(visitor);
                if ( auto className = getString(ma, nameVMAddr) ) {
                    if ( objcClass.isMetaClass )
                        metaclassVMAddrToName[classVMAddr.rawValue()] = className;
                    else
                        classVMAddrToName[classVMAddr.rawValue()] = className;
                }
            };

            visitor.forEachClassAndMetaClass(visitClass);
        });

        // These are used only for the on-disk binaries we analyze
        __block std::vector<const char*>        onDiskChainedFixupBindTargets;
        __block std::unordered_map<uint64_t, const char*> onDiskClassVMAddrToName;
        __block std::unordered_map<uint64_t, const char*> onDiskMetaclassVMAddrToName;

        auto getProperties = ^(const dyld3::MachOAnalyzer* ma, const objc_visitor::PropertyList& propertyList,
                               objc_visitor::Visitor& visitor) {
            Node propertiesNode;

            for ( uint32_t i = 0, numProperties = propertyList.numProperties(); i != numProperties; ++i ) {
                objc_visitor::Property property = propertyList.getProperty(visitor, i);

                // Get the name && attributes
                const char* propertyName = property.getName(visitor);
                const char* propertyAttributes = property.getAttributes(visitor);

                Node propertyNode;
                propertyNode.map["name"] = Node{propertyName};
                propertyNode.map["attributes"] = Node{propertyAttributes};
                propertiesNode.array.push_back(propertyNode);
            }

            return propertiesNode.array.empty() ? std::optional<Node>() : propertiesNode;
        };

        auto getClassProtocols = ^(const dyld3::MachOAnalyzer* ma, const objc_visitor::ProtocolList& protocolList,
                                   objc_visitor::Visitor& visitor) {
            Node protocolsNode;

            for ( uint64_t i = 0, numProtocols = protocolList.numProtocols(visitor); i != numProtocols; ++i ) {
                objc_visitor::Protocol protocol = protocolList.getProtocol(visitor, i);

                if ( const char *name = getString(ma, protocol.getNameVMAddr(visitor)) ) {
                    protocolsNode.array.push_back(Node{name});
                }
            }

            return protocolsNode.array.empty() ? std::optional<Node>() : protocolsNode;
        };

        auto getProtocols = ^(const dyld3::MachOAnalyzer* ma,
                              objc_visitor::Visitor& visitor) {
            auto getMethods = ^(const dyld3::MachOAnalyzer* mh, objc_visitor::MethodList methodList,
                                const std::string &prefix, Node &node){
                for ( uint32_t i = 0, numMethods = methodList.numMethods(); i != numMethods; ++i ) {
                    objc_visitor::Method objcMethod = methodList.getMethod(visitor, i);

                    if ( auto name = getString(mh, objcMethod.getNameVMAddr(visitor)) ) {
                        node.array.push_back(Node{prefix + name});
                    }
                }
            };

            __block Node protocolsNode;
            auto visitProtocol = ^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
                const char* protoName = getString(ma, objcProtocol.getNameVMAddr(visitor));
                if ( !protoName )
                    return;

                Node entry;
                entry.map["protocolName"] = Node{protoName};

                objc_visitor::ProtocolList protocolList = objcProtocol.getProtocols(visitor);
                if ( uint64_t numProtocols = protocolList.numProtocols(visitor); numProtocols != 0 ) {
                    Node visitedProtocols;

                    for ( uint32_t i = 0; i != numProtocols; ++i ) {
                        objc_visitor::Protocol innerProtocol = protocolList.getProtocol(visitor, i);

                        if ( const char* name = getString(ma, innerProtocol.getNameVMAddr(visitor)) )
                            visitedProtocols.array.push_back(Node{name});
                    }

                    if (!visitedProtocols.array.empty()) {
                        entry.map["protocols"] = visitedProtocols;
                    }
                }

                Node methods;
                getMethods(ma, objcProtocol.getInstanceMethods(visitor), instancePrefix, methods);
                getMethods(ma, objcProtocol.getClassMethods(visitor), classPrefix, methods);
                if (!methods.array.empty()) {
                    entry.map["methods"] = methods;
                }

                Node optMethods;
                getMethods(ma, objcProtocol.getOptionalInstanceMethods(visitor), instancePrefix, optMethods);
                getMethods(ma, objcProtocol.getOptionalClassMethods(visitor), classPrefix, optMethods);
                if (!optMethods.array.empty()) {
                    entry.map["optionalMethods"] = optMethods;
                }

                protocolsNode.array.push_back(entry);
            };

            visitor.forEachProtocol(visitProtocol);

            return protocolsNode.array.empty() ? std::optional<Node>() : protocolsNode;
        };

        auto getSelRefs = ^(const dyld3::MachOAnalyzer* ma,
                            objc_visitor::Visitor& visitor) {
            __block std::vector<const char *> selNames;

            visitor.forEachSelectorReference(^(VMAddress selRefVMAddr, VMAddress selRefTargetVMAddr, const char *selectorString) {
                if ( auto selValue = getString(ma, selRefTargetVMAddr) ) {
                    selNames.push_back(selValue);
                }
            });

            std::sort(selNames.begin(), selNames.end(),
                      [](const char* a, const char* b) {
                return strcasecmp(a, b) < 0;
            });

            Node selrefs;
            for (auto s: selNames) {
                selrefs.array.push_back(Node{s});
            }

            return selrefs.array.empty() ? std::optional<Node>() : selrefs;
        };

        auto getClasses = ^(const dyld3::MachOAnalyzer* ma,
                            objc_visitor::Visitor& visitor) {
            const uint32_t pointerSize = ma->pointerSize();
            const uint16_t chainedPointerFormat = ma->hasChainedFixups() ? ma->chainedPointerFormat() : 0;

            // Get the vmAddrs for all exported symbols as we want to know if classes
            // are exported
            std::set<uint64_t> exportedSymbolVMAddrs;
            {
                uint64_t loadAddress = ((const Header*)ma)->preferredLoadAddress();

                uint32_t exportTrieRuntimeOffset;
                uint32_t exportTrieSize;
                if ( ma->hasExportTrie(exportTrieRuntimeOffset, exportTrieSize) ) {
                    const uint8_t* start = (uint8_t*)ma + exportTrieRuntimeOffset;
                    const uint8_t* end = start + exportTrieSize;
                    std::vector<ExportInfoTrie::Entry> exports;
                    if ( ExportInfoTrie::parseTrie(start, end, exports) ) {
                        for (const ExportInfoTrie::Entry& entry: exports) {
                            exportedSymbolVMAddrs.insert(loadAddress + entry.info.address);
                        }
                    }
                }
            }

            __block Node classesNode;
            __block bool skippedPreviousClass = false;
            auto visitClass = ^(const objc_visitor::Class& objcClass, bool& stopClass) {
                if ( objcClass.isMetaClass ) {
                    if (skippedPreviousClass) {
                        // If the class was bad, then skip the meta class too
                        skippedPreviousClass = false;
                        return;
                    }
                } else {
                    skippedPreviousClass = true;
                }

                std::string classType = "-";
                if ( objcClass.isMetaClass )
                    classType = "+";

                VMAddress classVMAddr = objcClass.getVMAddress();
                VMAddress nameVMAddr = objcClass.getNameVMAddr(visitor);

                dyld3::MachOAnalyzer::PrintableStringResult classNameResult;
                const char* className = ma->getPrintableString(nameVMAddr.rawValue(), classNameResult);
                if ( classNameResult != dyld3::MachOAnalyzer::PrintableStringResult::CanPrint ) {
                    return;
                }

                __block const char* superClassName = nullptr;
                if ( DyldSharedCache::inDyldCache(dyldCache, ma) ) {
                    std::optional<VMAddress> superclassVMAddr = objcClass.getSuperclassVMAddr(visitor);
                    if ( superclassVMAddr.has_value() ) {
                        if ( objcClass.isMetaClass ) {
                            // If we are root class, then our superclass should actually point to our own class
                            if ( objcClass.isRootClass(visitor) ) {
                                auto it = classVMAddrToName.find(superclassVMAddr.value().rawValue());
                                assert(it != classVMAddrToName.end());
                                superClassName = it->second;
                            } else {
                                auto it = metaclassVMAddrToName.find(superclassVMAddr.value().rawValue());
                                assert(it != metaclassVMAddrToName.end());
                                superClassName = it->second;
                            }
                        } else {
                            auto it = classVMAddrToName.find(superclassVMAddr.value().rawValue());
                            assert(it != classVMAddrToName.end());
                            superClassName = it->second;
                        }
                    }
                } else {
                    // On-disk binary.  Lets crack the chain to work out what we are pointing at
                    objcClass.withSuperclass(visitor, ^(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t) {
                        if ( (pointerSize == 8) && (fixup->raw64 == 0) )
                            return;
                        else if ( (pointerSize == 4) && (fixup->raw32 == 0) )
                            return;

                        uint32_t  bindOrdinal;
                        int64_t   embeddedAddend;
                        if ( fixup->isBind(chainedPointerFormat, bindOrdinal, embeddedAddend) ) {
                            // Bind to another image.  Use the bind table to work out which name to bind to
                            const char* symbolName = onDiskChainedFixupBindTargets[(size_t)bindOrdinal];
                            if ( objcClass.isMetaClass ) {
                                if ( strstr(symbolName, "_OBJC_METACLASS_$_") == symbolName ) {
                                    superClassName = symbolName + strlen("_OBJC_METACLASS_$_");
                                } else {
                                    // Swift classes don't start with these prefixes so just skip them
                                    if ( objcClass.isSwiftLegacy(visitor) || objcClass.isSwiftStable(visitor) )
                                        return;
                                }
                            } else {
                                if ( strstr(symbolName, "_OBJC_CLASS_$_") == symbolName ) {
                                    superClassName = symbolName + strlen("_OBJC_CLASS_$_");
                                } else {
                                    // Swift classes don't start with these prefixes so just skip them
                                    if ( objcClass.isSwiftLegacy(visitor) || objcClass.isSwiftStable(visitor) )
                                        return;
                                }
                            }
                        } else {
                            // Rebase within this image.
                            std::optional<VMAddress> superclassVMAddr = objcClass.getSuperclassVMAddr(visitor);
                            if ( objcClass.isMetaClass ) {
                                auto it = onDiskMetaclassVMAddrToName.find(superclassVMAddr.value().rawValue());
                                assert(it != onDiskMetaclassVMAddrToName.end());
                                superClassName = it->second;
                            } else {
                                auto it = onDiskClassVMAddrToName.find(superclassVMAddr.value().rawValue());
                                assert(it != onDiskClassVMAddrToName.end());
                                superClassName = it->second;
                            }
                        }
                    });

                    if ( !superClassName ) {
                        // Probably a swift class we want to skip
                        return;
                    }
                }

                // Print the methods on this class
                Node methodsNode;

                objc_visitor::MethodList objcMethodList = skipListsOfLists(objcClass.getBaseMethods(visitor), visitor);

                for ( uint32_t i = 0, numMethods = objcMethodList.numMethods(); i != numMethods; ++i ) {
                    objc_visitor::Method objcMethod = objcMethodList.getMethod(visitor, i);

                    dyld3::MachOAnalyzer::PrintableStringResult methodNameResult;
                    const char* methodName = ma->getPrintableString(objcMethod.getNameVMAddr(visitor).rawValue(),
                                                                    methodNameResult);
                    if (methodNameResult != dyld3::MachOAnalyzer::PrintableStringResult::CanPrint)
                        continue;

                    methodsNode.array.push_back(Node{classType + methodName});
                }

                objc_visitor::PropertyList propertyList = skipListsOfLists(objcClass.getBaseProperties(visitor), visitor);
                std::optional<Node> properties = getProperties(ma, propertyList, visitor);

                if ( objcClass.isMetaClass ) {
                    assert(!classesNode.array.empty());
                    Node& currentClassNode = classesNode.array.back();
                    assert(currentClassNode.map["className"].value == className);
                    if (!methodsNode.array.empty()) {
                        Node& currentMethodsNode = currentClassNode.map["methods"];
                        currentMethodsNode.array.insert(currentMethodsNode.array.end(),
                                                        methodsNode.array.begin(),
                                                        methodsNode.array.end());
                    }
                    if (properties.has_value()) {
                        Node& currentPropertiesNode = currentClassNode.map["properties"];
                        currentPropertiesNode.array.insert(currentPropertiesNode.array.end(),
                                                           properties->array.begin(),
                                                           properties->array.end());
                    }
                    return;
                }

                objc_visitor::ProtocolList protocolList = skipListsOfLists(objcClass.getBaseProtocols(visitor), visitor);

                Node currentClassNode;
                currentClassNode.map["className"] = Node{className};
                if ( superClassName != nullptr )
                    currentClassNode.map["superClassName"] = Node{superClassName};
                if (!methodsNode.array.empty())
                    currentClassNode.map["methods"] = methodsNode;
                if (properties.has_value())
                    currentClassNode.map["properties"] = properties.value();
                if (std::optional<Node> protocols = getClassProtocols(ma, protocolList, visitor))
                    currentClassNode.map["protocols"] = protocols.value();

                currentClassNode.map["exported"] = Node{exportedSymbolVMAddrs.count(classVMAddr.rawValue()) != 0};

                // We didn't skip this class so mark it as such
                skippedPreviousClass = false;

                classesNode.array.push_back(currentClassNode);
            };

            visitor.forEachClassAndMetaClass(visitClass);

            return classesNode.array.empty() ? std::optional<Node>() : classesNode;
        };

        auto getCategories = ^(const dyld3::MachOAnalyzer* ma,
                               objc_visitor::Visitor& visitor) {
            const uint32_t pointerSize = ma->pointerSize();
            const uint16_t chainedPointerFormat = ma->hasChainedFixups() ? ma->chainedPointerFormat() : 0;

            __block Node categoriesNode;
            auto visitCategory = ^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
                VMAddress nameVMAddr = objcCategory.getNameVMAddr(visitor);

                dyld3::MachOAnalyzer::PrintableStringResult categoryNameResult;
                const char* categoryName = ma->getPrintableString(nameVMAddr.rawValue(), categoryNameResult);
                if (categoryNameResult != dyld3::MachOAnalyzer::PrintableStringResult::CanPrint)
                    return;

                __block const char* className = nullptr;
                if ( DyldSharedCache::inDyldCache(dyldCache, ma) ) {
                    // The class might be missing if the target is not in the shared cache.  So just skip these ones
                    std::optional<VMAddress> clsVMAddr = objcCategory.getClassVMAddr(visitor);
                    if ( !clsVMAddr.has_value() )
                        return;

                    if ( objcCategory.isForSwiftStubClass() ) {
                        // We don't have a class for stub classes, so just use a marker
                        className = "unknown swift stub class";
                    } else {
                        auto it = classVMAddrToName.find(clsVMAddr.value().rawValue());
                        if (it == classVMAddrToName.end()) {
                            // This is an odd binary with perhaps a Swift class.  Just skip this entry
                            // Specifically, categories can be attached to "stub classes" which are not in the
                            // objc class list.  Instead the ISA (really the ISA + 8" of the class the category is
                            // attached to, is listed in a section called __objc_stublist.  Those are Swift stub classes
                            return;
                        }
                        className = it->second;
                    }
                } else {
                    // On-disk binary.  Lets crack the chain to work out what we are pointing at
                    objcCategory.withClass(visitor,
                                           ^(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t) {
                        if ( (pointerSize == 8) && (fixup->raw64 == 0) )
                            return;
                        else if ( (pointerSize == 4) && (fixup->raw32 == 0) )
                            return;

                        uint32_t  bindOrdinal;
                        int64_t   embeddedAddend;
                        if ( fixup->isBind(chainedPointerFormat, bindOrdinal, embeddedAddend) ) {
                            // Bind to another image.  Use the bind table to work out which name to bind to
                            const char* symbolName = onDiskChainedFixupBindTargets[(size_t)bindOrdinal];
                            if ( strstr(symbolName, "_OBJC_CLASS_$_") == symbolName ) {
                                className = symbolName + strlen("_OBJC_CLASS_$_");
                            } else {
                                // Swift classes don't start with these prefixes so just skip them
                                // We don't know that this is a Swift class/category though, but skip it anyway
                                return;
                            }
                        } else {
                            std::optional<VMAddress> clsVMAddr = objcCategory.getClassVMAddr(visitor);
                            auto it = onDiskClassVMAddrToName.find(clsVMAddr.value().rawValue());
                            if (it == onDiskClassVMAddrToName.end()) {
                                // This is an odd binary with perhaps a Swift class.  Just skip this entry
                                return;
                            }
                            className = it->second;
                        }
                    });

                    if ( !className ) {
                        // Probably a swift class we want to skip
                        return;
                    }
                }

                // Print the instance methods on this category
                __block Node methodsNode;
                {
                    objc_visitor::MethodList objcMethodList = objcCategory.getInstanceMethods(visitor);
                    for ( uint32_t i = 0, numMethods = objcMethodList.numMethods(); i != numMethods; ++i ) {
                        objc_visitor::Method objcMethod = objcMethodList.getMethod(visitor, i);

                        if ( auto methodName = getString(ma, objcMethod.getNameVMAddr(visitor)) )
                            methodsNode.array.push_back(Node{instancePrefix + methodName});
                    }
                }

                // Print the class methods on this category
                {
                    objc_visitor::MethodList objcMethodList = objcCategory.getClassMethods(visitor);
                    for ( uint32_t i = 0, numMethods = objcMethodList.numMethods(); i != numMethods; ++i ) {
                        objc_visitor::Method objcMethod = objcMethodList.getMethod(visitor, i);

                        if ( auto methodName = getString(ma, objcMethod.getNameVMAddr(visitor)) )
                            methodsNode.array.push_back(Node{classPrefix + methodName});
                    }
                }

                Node currentCategoryNode;
                currentCategoryNode.map["categoryName"] = Node{categoryName};
                currentCategoryNode.map["className"] = Node{className};
                if (!methodsNode.array.empty())
                    currentCategoryNode.map["methods"] = methodsNode;
                if (std::optional<Node> properties = getProperties(ma, objcCategory.getInstanceProperties(visitor), visitor))
                    currentCategoryNode.map["properties"] = properties.value();
                if (std::optional<Node> protocols = getClassProtocols(ma, objcCategory.getProtocols(visitor), visitor))
                    currentCategoryNode.map["protocols"] = protocols.value();

                categoriesNode.array.push_back(currentCategoryNode);
            };

            visitor.forEachCategory(visitCategory);
            return categoriesNode.array.empty() ? std::optional<Node>() : categoriesNode;
        };

        __block bool needsComma = false;

        json::streamArrayBegin(needsComma);

        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;

            objc_visitor::Visitor visitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));

            Node imageRecord;
            imageRecord.map["imagePath"] = Node{installName};
            imageRecord.map["imageType"] = Node{"cache-dylib"};
            std::optional<Node> classes = getClasses(ma, visitor);
            std::optional<Node> categories = getCategories(ma, visitor);
            std::optional<Node> protocols = getProtocols(ma, visitor);
            std::optional<Node> selrefs = getSelRefs(ma, visitor);

            // Skip emitting images with no objc data
            if (!classes.has_value() && !categories.has_value() && !protocols.has_value() && !selrefs.has_value())
                return;
            if (classes.has_value())
                imageRecord.map["classes"] = classes.value();
            if (categories.has_value())
                imageRecord.map["categories"] = categories.value();
            if (protocols.has_value())
                imageRecord.map["protocols"] = protocols.value();
            if (selrefs.has_value())
                imageRecord.map["selrefs"] = selrefs.value();

            json::streamArrayNode(needsComma, imageRecord);
        });

        const dyld3::MachOAnalyzer* mainMA = nullptr;
        if ( dyldCache ) {
            // gracefully handling older dyld caches
            if ( dyldCache->header.mappingOffset < 0x170 ) {
                fprintf(stderr, "dyld_closure_util: can't operate against an old (pre-dyld4) dyld cache\n");
                exit(1);
            }

            // HACK: use libSystem.dylib from cache as main executable to bootstrap state
            uint32_t imageIndex;
            if ( dyldCache->hasImagePath("/usr/lib/libSystem.B.dylib", imageIndex) ) {
                uint64_t ignore1;
                uint64_t ignore2;
                mainMA = (MachOAnalyzer*)dyldCache->getIndexedImageEntry(imageIndex, ignore1, ignore2);
            }
        }

        KernelArgs            kernArgs(mainMA, {"test.exe"}, {}, {});
        Allocator&            alloc = MemoryManager::memoryManager().defaultAllocator();
        SyscallDelegate       osDelegate;
        osDelegate._dyldCache   = dyldCache;
        osDelegate._rootPath    = options.rootPath;
        __block ProcessConfig   config(&kernArgs, osDelegate, alloc);
        RuntimeLocks            locks;
        RuntimeState            stateObject(config, locks, alloc);
        RuntimeState&           state = stateObject;

        config.dyldCache.addr->forEachLaunchLoaderSet(^(const char* executableRuntimePath, const PrebuiltLoaderSet* pbls) {

            __block Diagnostics diag;
            bool                checkIfOSBinary = state.config.process.archs->checksOSBinary();
            state.config.syscall.withReadOnlyMappedFile(diag, executableRuntimePath, checkIfOSBinary, ^(const void* mapping, size_t mappedSize, bool isOSBinary, const FileID& fileID, const char* canonicalPath, const int fileDescriptor) {
                uint64_t sliceOffset;
                uint64_t sliceSize;
                if ( const dyld3::MachOFile* mf = dyld3::MachOFile::compatibleSlice(diag, sliceOffset, sliceSize, mapping, mappedSize, executableRuntimePath, state.config.process.platform, isOSBinary, *state.config.process.archs) ) {
                    dyld3::closure::FileSystemPhysical fileSystem;
                    dyld3::closure::LoadedFileInfo fileInfo = {
                        .fileContent        = (void*)mf,
                        .fileContentLen     = sliceSize,
                        .sliceOffset        = 0,
                        .sliceLen           = sliceSize,
                        .isOSBinary         = false,
                        .inode              = 0,
                        .mtime              = 0,
                        .unload             = nullptr,
                        .path = executableRuntimePath
                    };
                    const dyld3::MachOAnalyzer* ma = ((const dyld3::MachOAnalyzer*)mf)->remapIfZeroFill(diag, fileSystem, fileInfo);

                    // Populate the bind targets for classes from other images
                    onDiskChainedFixupBindTargets.clear();
                    ma->forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
                        onDiskChainedFixupBindTargets.push_back(symbolName);
                    });
                    if ( diag.hasError() )
                        return;

                    // Populate the rebase targets for class names
                    onDiskMetaclassVMAddrToName.clear();
                    onDiskClassVMAddrToName.clear();

                    __block objc_visitor::Visitor visitor(dyldCache, ma, std::nullopt);

                    auto visitClass = ^(const objc_visitor::Class& objcClass, bool& stopClass) {
                        VMAddress classVMAddr = objcClass.getVMAddress();
                        VMAddress nameVMAddr = objcClass.getNameVMAddr(visitor);
                        if ( auto className = getString(ma, nameVMAddr) ) {
                            if ( objcClass.isMetaClass )
                                onDiskMetaclassVMAddrToName[classVMAddr.rawValue()] = className;
                            else
                                onDiskClassVMAddrToName[classVMAddr.rawValue()] = className;
                        }
                    };

                    visitor.forEachClassAndMetaClass(visitClass);

                    Node imageRecord;
                    imageRecord.map["imagePath"] = Node{executableRuntimePath};
                    imageRecord.map["imageType"] = Node{"executable"};
                    std::optional<Node> classes = getClasses(ma, visitor);
                    std::optional<Node> categories = getCategories(ma, visitor);
                    // TODO: protocols
                    std::optional<Node> selrefs = getSelRefs(ma, visitor);

                    // Skip emitting images with no objc data
                    if (!classes.has_value() && !categories.has_value() && !selrefs.has_value())
                        return;
                    if (classes.has_value())
                        imageRecord.map["classes"] = classes.value();
                    if (categories.has_value())
                        imageRecord.map["categories"] = categories.value();
                    if (selrefs.has_value())
                        imageRecord.map["selrefs"] = selrefs.value();

                    json::streamArrayNode(needsComma, imageRecord);
                }
            });
        });

        json::streamArrayEnd(needsComma);
    }
    else if ( options.mode == modeObjCClassLayout ) {
        dumpObjCClassLayout(dyldCache);
    }
    else if ( options.mode == modeObjCClassMethodLists ) {
        dumpObjCClassMethodLists(dyldCache);
    }
    else if ( options.mode == modeObjCSelectors ) {
        if ( !dyldCache->hasOptimizedObjC() ) {
            fprintf(stderr, "Error: could not get optimized objc\n");
            return 1;
        }
        const objc::SelectorHashTable* selectors = dyldCache->objcSelectorHashTable();
        if ( selectors == nullptr ) {
            fprintf(stderr, "Error: could not get optimized objc selectors\n");
            return 1;
        }

        __block std::vector<const char*> selNames;
        selectors->forEachString(^(const char *str) {
            selNames.push_back(str);
        });

        std::sort(selNames.begin(), selNames.end(),
                  [](const char* a, const char* b) {
            // Sort by offset, not string value
            return a < b;
        });

        json::Node root;
        for (const char* selName : selNames) {
            json::Node selNode;
            selNode.map["selectorName"] = json::Node{selName};
            selNode.map["offset"] = json::Node{(int64_t)selName - (int64_t)dyldCache};

            root.array.push_back(selNode);
        }

        json::printJSON(root, 0, std::cout);
    }
    else if ( options.mode == modeSwiftProtocolConformances ) {
#if 0
        // This would dump the conformances in each binary, not the table in the shared cache
        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;

            Diagnostics diag;
            bool contentRebased = (sharedCachePath == nullptr);
            auto vmAddrConverter = dyldCache->makeVMAddrConverter(contentRebased);
            uint64_t binaryCacheOffset = (uint64_t)ma - (uint64_t)dyldCache;
            ma->forEachSwiftProtocolConformance(diag, vmAddrConverter,
                                                ^(uint64_t protocolConformanceRuntimeOffset, const dyld3::MachOAnalyzer::SwiftProtocolConformance &protocolConformance, bool &stopProtocolConformance) {
                printf("(0x%08llx, 0x%08llx) -> 0x%08llx  %s\n",
                       binaryCacheOffset + protocolConformance.typeConformanceRuntimeOffset, binaryCacheOffset + protocolConformance.protocolRuntimeOffset,
                       binaryCacheOffset + protocolConformanceRuntimeOffset, installName);
            });
        });
#endif

        auto getLibraryLeafName = [](const char* path)
        {
            const char* start = strrchr(path, '/');
            if ( start != NULL )
                return &start[1];
            else
                return path;
        };

        // Find all the symbols.  This maps from VM Addresses to symbol name
        __block std::unordered_map<uint64_t, std::string_view> symbols;
        __block std::unordered_map<uint64_t, std::string_view> dylibs;
        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;
            Diagnostics diag;
            ma->forEachGlobalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                symbols[n_value] = symbolName;
                dylibs[n_value] = getLibraryLeafName(installName);
            });
            ma->forEachLocalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                symbols[n_value] = symbolName;
                dylibs[n_value] = getLibraryLeafName(installName);
            });
        });

#if 0
        // FIXME: Move to the new code for unmapped locals
        if ( (sharedCachePath != nullptr) && dyldCache->hasLocalSymbolsInfo() ) {
            // When mapping the cache from disk, we can also get the unmapped locals
            struct stat statbuf;
            if ( ::stat(sharedCachePath, &statbuf) ) {
                fprintf(stderr, "Error: stat failed for dyld shared cache at %s\n", sharedCachePath);
                return 1;
            }

            int cache_fd = ::open(sharedCachePath, O_RDONLY);
            if (cache_fd < 0) {
                fprintf(stderr, "Error: failed to open shared cache file at %s\n", sharedCachePath);
                return 1;
            }

            const void* mappedData = ::mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
            if ( mappedData == MAP_FAILED ) {
                fprintf(stderr, "Error: Shared cache not mmap()able\b");
                ::close(cache_fd);
                return 1;
            }
            ::close(cache_fd);

            const DyldSharedCache* localsCache = (const DyldSharedCache*)mappedData;

            const bool is64 = (strstr(localsCache->archName(), "64") != NULL);
            const uint32_t nlistFileOffset = (uint32_t)((uint8_t*)localsCache->getLocalNlistEntries() - (uint8_t*)dyldCache);
            const char* localStrings = localsCache->getLocalStrings();

            __block uint32_t entriesCount = 0;
            localsCache->forEachLocalSymbolEntry(^(uint32_t dylibOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool &stop) {
                const char* imageName = dyldCache->getIndexedImagePath(entriesCount);
                //printf("   nlistStartIndex=%5d, nlistCount=%5d, image=%s\n", nlistStartIndex, nlistCount, imageName);
                if ( is64 ) {
                    const nlist_64* symTab = (nlist_64*)((char*)dyldCache + nlistFileOffset);
                    for (int e = 0; e < nlistCount; ++e) {
                        const nlist_64* entry = &symTab[nlistStartIndex + e];
                        //printf("     nlist[%d].str=%d, %s\n", e, entry->n_un.n_strx, &localStrings[entry->n_un.n_strx]);
                        //printf("     nlist[%d].value=0x%0llX\n", e, entry->n_value);
                        symbols[entry->n_value] = &localStrings[entry->n_un.n_strx];
                        dylibs[entry->n_value] = getLibraryLeafName(imageName);
                    }
                }
                entriesCount++;
            });
        }
#endif

        auto getDylibForAddress = ^(uint64_t vmAddress) {
            __block std::string_view dylibName;
            dyldCache->forEachImage(^(const Header *mh, const char *installName) {
                if ( !dylibName.empty() )
                    return;
                ((const Header*)mh)->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
                    if ( (vmAddress >= info.vmaddr) && (vmAddress < (info.vmaddr + info.vmsize)) ) {
                        dylibName = installName;
                        stop = true;
                    }
                });
            });
            return dylibName;
        };

        uint64_t cacheBaseAddress = dyldCache->unslidLoadAddress();

        const SwiftOptimizationHeader* swiftOptHeader = dyldCache->swiftOpt();
        if ( swiftOptHeader == nullptr ) {
            printf("No Swift optimization information present\n");
            return 0;
        }
        printf("Swift optimization version: %d\n", swiftOptHeader->version);
        if ( swiftOptHeader->version == 1 || swiftOptHeader->version == 2 || swiftOptHeader->version == 3 ) {
            printf("Type hash table\n");
            const SwiftHashTable* typeHashTable = (const SwiftHashTable*)((uint8_t*)dyldCache + swiftOptHeader->typeConformanceHashTableCacheOffset);
            typeHashTable->forEachValue(^(uint32_t bucketIndex, const dyld3::Array<SwiftTypeProtocolConformanceLocation>& impls) {
                for (const SwiftTypeProtocolConformanceLocation& protoLoc : impls) {
                    std::string_view typeDesc = "n/a";
                    std::string_view typeDescDylib;
                    if ( auto it = symbols.find(protoLoc.typeDescriptorCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        typeDesc = it->second;
                        typeDescDylib = dylibs[protoLoc.typeDescriptorCacheOffset + cacheBaseAddress];
                    } else {
                        typeDescDylib = getDylibForAddress(protoLoc.typeDescriptorCacheOffset + cacheBaseAddress);
                        if ( typeDescDylib.empty() )
                            typeDescDylib = "n/a";
                        else
                            typeDescDylib = getLibraryLeafName(typeDescDylib.data());
                    }

                    std::string_view protocol = "n/a";
                    std::string_view protocolDylib;
                    if ( auto it = symbols.find(protoLoc.protocolCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        protocol = it->second;
                        protocolDylib = dylibs[protoLoc.protocolCacheOffset + cacheBaseAddress];
                    } else {
                        protocolDylib = getDylibForAddress(protoLoc.protocolCacheOffset + cacheBaseAddress);
                        if ( protocolDylib.empty() )
                            protocolDylib = "n/a";
                        else
                            protocolDylib = getLibraryLeafName(protocolDylib.data());
                    }

                    std::string_view conformance = "n/a";
                    std::string_view conformanceDylib;
                    if ( auto it = symbols.find(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        conformance = it->second;
                        conformanceDylib = dylibs[protoLoc.protocolConformanceCacheOffset + cacheBaseAddress];
                    } else {
                        conformanceDylib = getDylibForAddress(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress);
                        if ( conformanceDylib.empty() )
                            conformanceDylib = "n/a";
                        else
                            conformanceDylib = getLibraryLeafName(conformanceDylib.data());
                    }

                    printf("[%d]: (type: %s (cache offset 0x%llx) in %s, protocol %s (cache offset 0x%llx) in %s) -> (conformance: %s (cache offset 0x%llx) in %s)\n",
                           bucketIndex,
                           typeDesc.data(), protoLoc.typeDescriptorCacheOffset, typeDescDylib.data(),
                           protocol.data(), protoLoc.protocolCacheOffset, protocolDylib.data(),
                           conformance.data(), protoLoc.protocolConformanceCacheOffset, conformanceDylib.data());
                }
            });

            printf("Metadata hash table\n");
            const SwiftHashTable* metadataHashTable = (const SwiftHashTable*)((uint8_t*)dyldCache + swiftOptHeader->metadataConformanceHashTableCacheOffset);
            metadataHashTable->forEachValue(^(uint32_t bucketIndex, const dyld3::Array<SwiftMetadataProtocolConformanceLocation>& impls) {
                for (const SwiftMetadataProtocolConformanceLocation& protoLoc : impls) {
                    std::string_view metadataDesc = "n/a";
                    std::string_view metadataDescDylib;
                    if ( auto it = symbols.find(protoLoc.metadataCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        metadataDesc = it->second;
                        metadataDescDylib = dylibs[protoLoc.metadataCacheOffset + cacheBaseAddress];
                    } else {
                        metadataDescDylib = getDylibForAddress(protoLoc.metadataCacheOffset + cacheBaseAddress);
                        if ( metadataDescDylib.empty() )
                            metadataDescDylib = "n/a";
                        else
                            metadataDescDylib = getLibraryLeafName(metadataDescDylib.data());
                    }

                    std::string_view protocol = "n/a";
                    std::string_view protocolDylib;
                    if ( auto it = symbols.find(protoLoc.protocolCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        protocol = it->second;
                        protocolDylib = dylibs[protoLoc.protocolCacheOffset + cacheBaseAddress];
                    } else {
                        protocolDylib = getDylibForAddress(protoLoc.protocolCacheOffset + cacheBaseAddress);
                        if ( protocolDylib.empty() )
                            protocolDylib = "n/a";
                        else
                            protocolDylib = getLibraryLeafName(protocolDylib.data());
                    }

                    std::string_view conformance = "n/a";
                    std::string_view conformanceDylib;
                    if ( auto it = symbols.find(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        conformance = it->second;
                        conformanceDylib = dylibs[protoLoc.protocolConformanceCacheOffset + cacheBaseAddress];
                    } else {
                        conformanceDylib = getDylibForAddress(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress);
                        if ( conformanceDylib.empty() )
                            conformanceDylib = "n/a";
                        else
                            conformanceDylib = getLibraryLeafName(conformanceDylib.data());
                    }

                    printf("[%d]: (metadata: %s (cache offset 0x%llx) in %s, protocol %s (cache offset 0x%llx) in %s) -> (conformance: %s (cache offset 0x%llx) in %s)\n",
                           bucketIndex,
                           metadataDesc.data(), protoLoc.metadataCacheOffset, metadataDescDylib.data(),
                           protocol.data(), protoLoc.protocolCacheOffset, protocolDylib.data(),
                           conformance.data(), protoLoc.protocolConformanceCacheOffset, conformanceDylib.data());
                }
            });

            printf("Foreign type hash table\n");
            const SwiftHashTable* foreignTypeHashTable = (const SwiftHashTable*)((uint8_t*)dyldCache + swiftOptHeader->foreignTypeConformanceHashTableCacheOffset);
            foreignTypeHashTable->forEachValue(^(uint32_t bucketIndex, const dyld3::Array<SwiftForeignTypeProtocolConformanceLocation>& impls) {
                for (const SwiftForeignTypeProtocolConformanceLocation& protoLoc : impls) {
                    std::string_view typeNameView((const char*)dyldCache + protoLoc.foreignDescriptorNameCacheOffset, protoLoc.foreignDescriptorNameLength);
                    std::string typeName;
                    if ( typeNameView.size() != strlen(typeNameView.data()) ) {
                        typeName.reserve(typeNameView.size());
                        for (const char c : typeNameView) {
                            if ( c == '\0' )
                                typeName += "\\0";
                            else
                                typeName += c;
                        }
                        typeNameView = typeName;
                    }
;
                    std::string_view protocol = "n/a";
                    std::string_view protocolDylib;
                    if ( auto it = symbols.find(protoLoc.protocolCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        protocol = it->second;
                        protocolDylib = dylibs[protoLoc.protocolCacheOffset + cacheBaseAddress];
                    } else {
                        protocolDylib = getDylibForAddress(protoLoc.protocolCacheOffset + cacheBaseAddress);
                        if ( protocolDylib.empty() )
                            protocolDylib = "n/a";
                        else
                            protocolDylib = getLibraryLeafName(protocolDylib.data());
                    }

                    std::string_view conformance = "n/a";
                    std::string_view conformanceDylib;
                    if ( auto it = symbols.find(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress); it != symbols.end() ) {
                        conformance = it->second;
                        conformanceDylib = dylibs[protoLoc.protocolConformanceCacheOffset + cacheBaseAddress];
                    } else {
                        conformanceDylib = getDylibForAddress(protoLoc.protocolConformanceCacheOffset + cacheBaseAddress);
                        if ( conformanceDylib.empty() )
                            conformanceDylib = "n/a";
                        else
                            conformanceDylib = getLibraryLeafName(conformanceDylib.data());
                    }

                    printf("[%d]: (type name: %s (cache offset 0x%llx), protocol %s (cache offset 0x%llx) in %s) -> (conformance: %s (cache offset 0x%llx) in %s)\n",
                           bucketIndex,
                           typeName.data(), protoLoc.foreignDescriptorNameCacheOffset,
                           protocol.data(), protoLoc.protocolCacheOffset, protocolDylib.data(),
                           conformance.data(), protoLoc.protocolConformanceCacheOffset, conformanceDylib.data());
                }
            });

            if ( swiftOptHeader->version >= 2 )
                printf("Swift prespecialization data offset: 0x%llx\n", swiftOptHeader->prespecializationDataCacheOffset);
        } else {
            printf("Unhandled version\n");
        }
    }
    else if ( options.mode == modeSwiftPtrTables ) {
        uint64_t cacheBaseAddr = dyldCache->unslidLoadAddress();
        const SwiftOptimizationHeader* swiftOptHeader = dyldCache->swiftOpt();
        if ( swiftOptHeader == nullptr ) {
            printf("No Swift optimization information present\n");
            return 0;
        }
        printf("Swift optimization version: %d\n", swiftOptHeader->version);
        if ( swiftOptHeader->version == 3 ) {
            SymbolicatedCache symbolicatedCache(dyldCache, cacheOnDisk);
            for ( size_t i = 0; i < SwiftOptimizationHeader::MAX_PRESPECIALIZED_METADATA_TABLES; ++i ) {
                uint64_t ptrTableOffset = swiftOptHeader->prespecializedMetadataHashTableCacheOffsets[i];
                if ( ptrTableOffset == 0 )
                    continue;

                printf("Swift prespecialized metadata hash table #%lu\n", i);
                const SwiftHashTable* ptrTable = (const SwiftHashTable*)((uint8_t*)dyldCache + ptrTableOffset);

                ptrTable->forEachValue(^(uint32_t bucketIndex, const dyld3::Array<PointerHashTableValue>& values) {
                    for ( const PointerHashTableValue& value : values ) {
                        printf("  - k: [ ");
                        const uint64_t* keys = ptrTable->getCacheOffsets(value);
                        for ( size_t numKey = 0; numKey < value.numOffsets; ++numKey ) {
                            if ( numKey > 0 )
                                printf(", ");
                            printf("%s (0x%llx)", symbolicatedCache.symbolNameAt(cacheBaseAddr + keys[numKey]).c_str(), cacheBaseAddr + keys[numKey]);
                        }
                        printf(" ]\n    v: %s (0x%llx)\n", symbolicatedCache.symbolNameAt(cacheBaseAddr + value.cacheOffset).c_str(), cacheBaseAddr + value.cacheOffset);
                    }
                });
            }
        } else {
            printf("Unhandled version\n");
        }
    }
    else if ( options.mode == modeLookupVA ) {
        CString vaString = options.lookupVA;

        SymbolicatedCache symbolicatedCache(dyldCache, cacheOnDisk);

        while ( !vaString.empty() ) {
            char* endptr = nullptr;
            uint64_t addr = strtoull(vaString.c_str(), &endptr, 16);
            if ( addr == 0 )
                break;

            if ( endptr )
                ++endptr;
            vaString = endptr;

            printf("0x%llx\n", addr);
            std::optional<size_t> rangeIndexOpt = symbolicatedCache.findClosestRange(addr);
            if ( !rangeIndexOpt )
                return 0;

            size_t rangeIndex = *rangeIndexOpt;

            const SymbolicatedCache::Range& range = symbolicatedCache.ranges[rangeIndex];
            const SymbolicatedImage* image = nullptr;
            if ( range.imageIndex ) {
                image = &symbolicatedCache.images[*range.imageIndex];
                printf("  %15s %s\n", "in:", image->image().header()->installName());
                printf("  %15s 0x%llx\n", "image base:", image->image().header()->preferredLoadAddress());
            }
            if ( !range.segmentName.empty() ) {
                printf("  %15s %.*s,%.*s\n", "segment name:",
                       (int)range.segmentName.size(), range.segmentName.data(),
                       (int)range.sectName.size(), range.sectName.data());
            }
            printf("  %15s 0x%llx - 0x%llx\n", "range:", range.startAddr, range.endAddr);
            printf("  %15s %s\n", "symbol:", symbolicatedCache.symbolNameAt(addr).c_str());
        }
    }
    else if ( options.mode == modeExtract ) {
        return dyld_shared_cache_extract_dylibs(sharedCachePath, options.extractionDir);
    }
    else if ( options.mode == modeObjCImpCaches ) {
        if (sharedCachePath == nullptr) {
            fprintf(stderr, "Cannot emit imp caches with live cache.  Run again with the path to the cache file\n");
            return 1;
        }
        __block std::map<uint64_t, const char*> methodToClassMap;
        __block std::map<uint64_t, const char*> classVMAddrToNameMap;
        const bool contentRebased = false;
        const uint32_t pointerSize = 8;

        // Get the base pointers from the magic section in objc
        __block uint64_t objcCacheOffsetsSize = 0;
        __block const void* objcCacheOffsets = nullptr;
        __block int impCachesVersion = 1;
        __block Diagnostics diag;
        dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
            if ( !strcmp(installName, "/usr/lib/libobjc.A.dylib") ) {
                const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)hdr;
                objcCacheOffsets = ma->findSectionContent("__DATA_CONST", "__objc_scoffs", objcCacheOffsetsSize);
                dyld3::MachOAnalyzer::FoundSymbol foundInfo;
                if (ma->findExportedSymbol(diag, "_objc_opt_preopt_caches_version", false, foundInfo, nullptr)) {
                    impCachesVersion = *(int*)((uint8_t*)ma + foundInfo.value);
                }
            }
        });

        if ( objcCacheOffsets == nullptr ) {
            fprintf(stderr, "Unable to print imp-caches as cannot find __DATA_CONST __objc_scoffs inside /usr/lib/libobjc.A.dylib\n");
            return 1;
        }

        if ( objcCacheOffsetsSize < (4 * pointerSize) ) {
            fprintf(stderr, "Unable to print imp-caches as __DATA_CONST __objc_scoffs is too small (%lld vs required %u)\n", objcCacheOffsetsSize, (4 * pointerSize));
            return 1;
        }

        dyld3::MachOAnalyzer::VMAddrConverter vmAddrConverter = dyldCache->makeVMAddrConverter(contentRebased);

        uint8_t selectorStringIndex = 0;
        if (impCachesVersion > 1)
            selectorStringIndex = 1;
        uint64_t selectorStringVMAddrStart  = vmAddrConverter.convertToVMAddr(((uint64_t*)objcCacheOffsets)[selectorStringIndex]);
        uint64_t selectorStringVMAddrEnd    = vmAddrConverter.convertToVMAddr(((uint64_t*)objcCacheOffsets)[selectorStringIndex+1]);

        uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();

        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            if (diag.hasError())
                return;

            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;
            intptr_t slide = ma->getSlide();

            auto visitClass = ^(uint64_t classVMAddr,
                                uint64_t classSuperclassVMAddr,
                                uint64_t classDataVMAddr,
                                const dyld3::MachOAnalyzer::ObjCClassInfo& objcClass,
                                bool isMetaClass,
                                bool& stopClass) {
                const char* className = (const char*)objcClass.nameVMAddr(pointerSize) + slide;
                classVMAddrToNameMap[classVMAddr] = className;
                ma->forEachObjCMethod(objcClass.baseMethodsVMAddr(pointerSize), vmAddrConverter,
                                      sharedCacheRelativeSelectorBaseVMAddress,
                                      ^(uint64_t methodVMAddr, const dyld3::MachOAnalyzer::ObjCMethod& method, bool& stopMethod) {
                    // const char* methodName = (const char*)(method.nameVMAddr + slide);
                    methodToClassMap[method.impVMAddr] = className;
                });
            };
            ma->forEachObjCClass(diag, vmAddrConverter, visitClass);

            auto visitCategory = ^(uint64_t categoryVMAddr, const dyld3::MachOAnalyzer::ObjCCategory& objcCategory, bool& stopCategory) {
                ma->forEachObjCMethod(objcCategory.instanceMethodsVMAddr, vmAddrConverter,
                                      sharedCacheRelativeSelectorBaseVMAddress,
                                      ^(uint64_t methodVMAddr, const dyld3::MachOAnalyzer::ObjCMethod& method, bool& stopMethod) {
                    const char* catName = (const char*)objcCategory.nameVMAddr + slide;
                    // const char* methodName = (const char*)(method.nameVMAddr + slide);
                    methodToClassMap[method.impVMAddr] = catName;
                });

                ma->forEachObjCMethod(objcCategory.classMethodsVMAddr, vmAddrConverter,
                                      sharedCacheRelativeSelectorBaseVMAddress,
                                      ^(uint64_t methodVMAddr, const dyld3::MachOAnalyzer::ObjCMethod& method, bool& stopMethod) {
                    const char* catName = (const char*)objcCategory.nameVMAddr + slide;
                    // const char* methodName = (const char*)(method.nameVMAddr + slide);
                    methodToClassMap[method.impVMAddr] = catName;
                });
            };
            ma->forEachObjCCategory(diag, vmAddrConverter, visitCategory);
        });
        if (diag.hasError())
            return 1;

        dyldCache->forEachImage(^(const Header *mh, const char *installName) {
            if (diag.hasError())
                return;

            const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;
            intptr_t slide = ma->getSlide();

            auto visitClass = ^(uint64_t classVMAddr,
                                uint64_t classSuperclassVMAddr,
                                uint64_t classDataVMAddr,
                                const dyld3::MachOAnalyzer::ObjCClassInfo& objcClass,
                                bool isMetaClass,
                                bool& stopClass) {
                const char* type = "class";
                if (isMetaClass)
                    type = "meta-class";
                const char* className = (const char*)objcClass.nameVMAddr(pointerSize) + slide;

                if (objcClass.methodCacheVMAddr == 0) {
                    printf("%s (%s): empty\n", className, type);
                    return;
                }

                const uint8_t* impCacheBuffer = (const uint8_t*)(objcClass.methodCacheVMAddr + slide);
                uint32_t cacheMask = 0;
                uint32_t sizeOfHeader = 0;
                if (impCachesVersion < 3) {
                    const ImpCacheHeader_v1* impCache = (const ImpCacheHeader_v1*)impCacheBuffer;
                    printf("%s (%s): %d buckets\n", className, type, impCache->cache_mask + 1);
                    if ((classVMAddr + impCache->fallback_class_offset) != objcClass.superclassVMAddr)
                        printf("Flattening fallback: %s\n", classVMAddrToNameMap[classVMAddr + impCache->fallback_class_offset]);
                    cacheMask = impCache->cache_mask;
                    sizeOfHeader = sizeof(ImpCacheHeader_v1);
                } else {
                    const ImpCacheHeader_v2* impCache = (const ImpCacheHeader_v2*)impCacheBuffer;
                    printf("%s (%s): %d buckets\n", className, type, impCache->cache_mask + 1);
                    if ((classVMAddr + impCache->fallback_class_offset) != objcClass.superclassVMAddr)
                        printf("Flattening fallback: %s\n", classVMAddrToNameMap[classVMAddr + impCache->fallback_class_offset]);
                    cacheMask = impCache->cache_mask;
                    sizeOfHeader = sizeof(ImpCacheHeader_v2);
                }

                const uint8_t* buckets = impCacheBuffer + sizeOfHeader;
                // Buckets are a 32-bit offset from the impcache itself
                for (uint32_t i = 0; i <= cacheMask ; ++i) {
                    uint64_t sel = 0;
                    uint64_t imp = 0;
                    bool empty = false;
                    if (impCachesVersion == 1) {
                        const ImpCacheEntry_v1* bucket = (ImpCacheEntry_v1*)buckets + i;
                        sel = selectorStringVMAddrStart + bucket->selOffset;
                        imp = classVMAddr - bucket->impOffset;
                        if (bucket->selOffset == 0xFFFFFFF && bucket->impOffset == 0)
                            empty = true;
                    } else {
                        const ImpCacheEntry_v2* bucket = (ImpCacheEntry_v2*)buckets + i;
                        sel = selectorStringVMAddrStart + bucket->selOffset;
                        imp = classVMAddr - (bucket->impOffset << 2);
                        if (bucket->selOffset == 0x3FFFFFF && bucket->impOffset == 0)
                            empty = true;
                    }

                    if ( empty ) {
                        // Empty bucket
                        printf("  - 0x%016llx: %s\n", 0ULL, "");
                    } else {
                        assert(sel < selectorStringVMAddrEnd);
                        auto it = methodToClassMap.find(imp);
                        if (it == methodToClassMap.end()) {
                            fprintf(stderr, "Could not find IMP %llx (for %s)\n", imp, (const char*)(sel + slide));
                        }
                        assert(it != methodToClassMap.end());
                        printf("  - 0x%016llx: %s (from %s)\n", imp, (const char*)(sel + slide), it->second);
                    }
                }
           };
            ma->forEachObjCClass(diag, vmAddrConverter, visitClass);
        });
    } else {
        switch ( options.mode ) {
            case modeList: {
                if ( options.printInodes && !dyldCache->header.dylibsExpectedOnDisk ) {
                    fprintf(stderr, "Error: '-inode' option only valid on simulator shared caches\n");
                    return 1;
                }
                // list all dylibs, including their aliases (symlinks to them) with option vmaddr
                __block std::vector<std::unordered_set<std::string>> indexToPaths;
                __block std::vector<uint64_t> indexToAddr;
                __block std::unordered_map<uint64_t, uint64_t> indexToINode;
                __block std::unordered_map<uint64_t, uint64_t> indexToModTime;
                __block std::vector<std::string> indexToUUID;
                dyldCache->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const unsigned char* dylibUUID, const char* installName, bool& stop) {
                    std::unordered_set<std::string> empty;
                    if ( options.printVMAddrs )
                        indexToAddr.push_back(loadAddressUnslid);
                    if ( options.printUUIDs ) {
                        uuid_string_t uuidString;
                        uuid_unparse_upper(dylibUUID, uuidString);
                        indexToUUID.push_back(uuidString);
                    }
                    indexToPaths.push_back(empty);
                    indexToPaths.back().insert(installName);
                });
                dyldCache->forEachDylibPath(^(const char* dylibPath, uint32_t index) {
                    indexToPaths[index].insert(dylibPath);

                    uint64_t mTime = ~0ULL;
                    uint64_t inode = ~0ULL;
                    dyldCache->getIndexedImageEntry(index, mTime, inode);
                    indexToINode[index] = inode;
                    indexToModTime[index] = mTime;
                });
                int index = 0;
                for (const std::unordered_set<std::string>& paths : indexToPaths) {
                    for (const std::string& path: paths) {
                        if ( options.printVMAddrs )
                            printf("0x%08llX ", indexToAddr[index]);
                        if ( options.printUUIDs )
                             printf("<%s> ", indexToUUID[index].c_str());
                        if ( options.printInodes )
                            printf("0x%08llX 0x%08llX ", indexToINode[index], indexToModTime[index]);
                       printf("%s\n", path.c_str());
                    }
                    ++index;
                }
                break;
            }
            case modeListDylibsWithSection: {
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                        if ( (sectInfo.sectionName == options.sectionName) && (sectInfo.segmentName == options.segmentName) ) {
                            printf("%s\n", installName);
                            stop = true;
                        }
                    });
                });
                break;
            }
            case modeMap: {
                __block std::map<uint64_t, const char*> dataSegNames;
                __block std::map<uint64_t, uint64_t>    dataSegEnds;
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    hdr->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
                        printf("0x%08llX - 0x%08llX %.*s %s\n", info.vmaddr, info.vmaddr + info.vmsize,
                               (int)info.segmentName.size(), info.segmentName.data(),
                               installName);
                        if ( info.segmentName.starts_with("__DATA") ) {
                            dataSegNames[info.vmaddr] = installName;
                            dataSegEnds[info.vmaddr] = info.vmaddr + info.vmsize;
                        }
                    });
                });
                // <rdar://problem/51084507> Enhance dyld_shared_cache_util to show where section alignment added padding
                uint64_t lastEnd = 0;
                for (const auto& entry : dataSegEnds) {
                    uint64_t padding = entry.first - lastEnd;
                    if ( (padding > 32) && (lastEnd != 0) ) {
                        printf("0x%08llX - 0x%08llX PADDING %lluKB\n", lastEnd, entry.first, padding/1024);
                    }
                    lastEnd = entry.second;
                }
                break;
            }
            case modeDependencies: {
                __block bool dependentTargetFound = false;
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    if ( strcmp(options.dependentsOfPath, installName) != 0 )
                        return;
                    dependentTargetFound = true;

                    auto printDep = [&options](const char *loadPath, uint32_t compatVersion, uint32_t curVersion) {
                        if ( options.printDylibVersions ) {
                            uint32_t compat_vers = compatVersion;
                            uint32_t current_vers = curVersion;
                            printf("\t%s", loadPath);
                            if ( compat_vers != 0xFFFFFFFF ) {
                                printf("(compatibility version %u.%u.%u, current version %u.%u.%u)\n",
                                       (compat_vers >> 16),
                                       (compat_vers >> 8) & 0xff,
                                       (compat_vers) & 0xff,
                                       (current_vers >> 16),
                                       (current_vers >> 8) & 0xff,
                                       (current_vers) & 0xff);
                            }
                            else {
                                printf("\n");
                            }
                        }
                        else {
                            printf("\t%s\n", loadPath);
                        }
                    };

                    dyld3::MachOFile* mf = (dyld3::MachOFile*)hdr;

                    // First print out our dylib and version.
                    const char* dylibInstallName;
                    Version32 currentVersion;
                    Version32 compatVersion;
                    if ( hdr->getDylibInstallName(&dylibInstallName, &compatVersion, &currentVersion) ) {
                        printDep(dylibInstallName, compatVersion.value(), currentVersion.value());
                    }

                    // Then the dependent dylibs.
                    mf->forEachDependentDylib(^(const char* depPath, bool isWeak, bool isReExport, bool isUpward, uint32_t cpatVersion, uint32_t curVersion, bool& stop) {
                        printDep(depPath, cpatVersion, curVersion);
                    });
                });
                if (options.dependentsOfPath && !dependentTargetFound) {
                    fprintf(stderr, "Error: could not find '%s' in the shared cache at\n  %s\n", options.dependentsOfPath, sharedCachePath);
                    exit(1);
                }
                break;
            }
            case modeLinkEdit: {
                std::map<uint32_t, const char*> pageToContent;
                auto add_linkedit = [&pageToContent](uint32_t pageStart, uint32_t pageEnd, const char* message) {
                    for (uint32_t p = pageStart; p <= pageEnd; p += 4096) {
                        std::map<uint32_t, const char*>::iterator pos = pageToContent.find(p);
                        if ( pos == pageToContent.end() ) {
                            pageToContent[p] = strdup(message);
                        }
                        else {
                            const char* oldMessage = pos->second;
                            char* newMesssage;
                            asprintf(&newMesssage, "%s, %s", oldMessage, message);
                            pageToContent[p] = newMesssage;
                            ::free((void*)oldMessage);
                        }
                    }
                };

                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                    Diagnostics diag;
                    dyld3::MachOAnalyzer::LinkEditInfo leInfo;
                    ma->getLinkEditPointers(diag, leInfo);

                    if (diag.hasError())
                        return;

                    char message[1000];
                    const char* shortName = strrchr(installName, '/') + 1;

                    if ( leInfo.dyldInfo != nullptr ) {
                        // add export trie info
                        if ( leInfo.dyldInfo->export_size != 0 ) {
                            //printf("export_off=0x%X\n", leInfo.dyldInfo->export_off());
                            uint32_t exportPageOffsetStart = leInfo.dyldInfo->export_off & (-4096);
                            uint32_t exportPageOffsetEnd = (leInfo.dyldInfo->export_off + leInfo.dyldInfo->export_size) & (-4096);
                            snprintf(message, sizeof(message), "exports from %s", shortName);
                            add_linkedit(exportPageOffsetStart, exportPageOffsetEnd, message);
                        }
                        // add binding info
                        if ( leInfo.dyldInfo->bind_size != 0 ) {
                            uint32_t bindPageOffsetStart = leInfo.dyldInfo->bind_off & (-4096);
                            uint32_t bindPageOffsetEnd = (leInfo.dyldInfo->bind_off + leInfo.dyldInfo->bind_size) & (-4096);
                            snprintf(message, sizeof(message), "bindings from %s", shortName);
                            add_linkedit(bindPageOffsetStart, bindPageOffsetEnd, message);
                        }
                        // add lazy binding info
                        if ( leInfo.dyldInfo->lazy_bind_size != 0 ) {
                            uint32_t lazybindPageOffsetStart = leInfo.dyldInfo->lazy_bind_off & (-4096);
                            uint32_t lazybindPageOffsetEnd = (leInfo.dyldInfo->lazy_bind_off + leInfo.dyldInfo->lazy_bind_size) & (-4096);
                            snprintf(message, sizeof(message), "lazy bindings from %s", shortName);
                            add_linkedit(lazybindPageOffsetStart, lazybindPageOffsetEnd, message);
                        }
                        // add weak binding info
                        if ( leInfo.dyldInfo->weak_bind_size != 0 ) {
                            uint32_t weakbindPageOffsetStart = leInfo.dyldInfo->weak_bind_off & (-4096);
                            uint32_t weakbindPageOffsetEnd = (leInfo.dyldInfo->weak_bind_off + leInfo.dyldInfo->weak_bind_size) & (-4096);
                            snprintf(message, sizeof(message), "weak bindings from %s", shortName);
                            add_linkedit(weakbindPageOffsetStart, weakbindPageOffsetEnd, message);
                        }
                    } else {
                        // add export trie info
                        if ( (leInfo.exportsTrie != nullptr) && (leInfo.exportsTrie->datasize != 0) ) {
                            //printf("export_off=0x%X\n", leInfo.exportsTrie->export_off());
                            uint32_t exportPageOffsetStart = leInfo.exportsTrie->dataoff & (-4096);
                            uint32_t exportPageOffsetEnd = (leInfo.exportsTrie->dataoff + leInfo.exportsTrie->datasize) & (-4096);
                            snprintf(message, sizeof(message), "exports from %s", shortName);
                            add_linkedit(exportPageOffsetStart, exportPageOffsetEnd, message);
                        }
                        // Chained fixups are stripped from cache binaries, so no need to check for them here
                    }
                });

                for (std::map<uint32_t, const char*>::iterator it = pageToContent.begin(); it != pageToContent.end(); ++it) {
                    printf("0x%08X %s\n", it->first, it->second);
                }
                break;
            }
            case modeSize: {
                struct TextInfo {
                    uint64_t    textSize;
                    const char* path;
                };
                __block std::vector<TextInfo> textSegments;
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    hdr->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
                        if ( info.segmentName != "__TEXT" )
                            return;
                        textSegments.push_back({ info.fileSize, installName });
                    });
                });
                std::sort(textSegments.begin(), textSegments.end(), [](const TextInfo& left, const TextInfo& right) {
                    return (left.textSize > right.textSize);
                });
                for (std::vector<TextInfo>::iterator it = textSegments.begin(); it != textSegments.end(); ++it) {
                    printf(" 0x%08llX  %s\n", it->textSize, it->path);
                }
                break;
            }
            case modeFunctionVariants: {
                printf("Function Variant table size: %lld bytes\n", dyldCache->header.functionVariantInfoSize);
                uintptr_t cacheSlide = dyldCache->slide();
                dyldCache->forEachFunctionVariantPatchLocation(^(const void* loc, PointerMetaData pmd, const mach_o::FunctionVariants& fvs, const mach_o::Header* dylibHdr, int variantIndex, bool& stop) {
                    if ( pmd.authenticated ) {
                        printf("    fixup-loc=%p (key=%d, addr=%d, diversity=0x%04X), header-of-dylib-with-variant=%p, variant-index=%d\n",
                               (void*)((uintptr_t)loc - cacheSlide), pmd.key, pmd.usesAddrDiversity, pmd.diversity, (void*)((uintptr_t)dylibHdr - cacheSlide), variantIndex);
                    }
                    else {
                        printf("    fixup-loc=%p, header-of-dylib-with-variant=%p, variant-index=%d\n", (void*)((uintptr_t)loc - cacheSlide), (void*)((uintptr_t)dylibHdr - cacheSlide), variantIndex);
                    }
                });
                break;
            }
            case modePatchTable: {
                printf("Patch table size: %lld bytes\n", dyldCache->header.patchInfoSize);

                std::vector<SegmentInfo> segInfos;
                buildSegmentInfo(dyldCache, segInfos);
                __block uint32_t imageIndex = 0;
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    printf("%s:\n", installName);
                    uint64_t cacheBaseAddress = dyldCache->unslidLoadAddress();
                    uint64_t dylibBaseAddress = hdr->preferredLoadAddress();
                    dyldCache->forEachPatchableExport(imageIndex, ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                                    PatchKind patchKind) {
                        uint64_t cacheOffsetOfImpl = (dylibBaseAddress + dylibVMOffsetOfImpl) - cacheBaseAddress;
                        printf("    export: 0x%08llX%s  %s\n", cacheOffsetOfImpl,
                               PatchTable::patchKindName(patchKind), exportName);
                        dyldCache->forEachPatchableUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                               ^(uint32_t userImageIndex, uint32_t userVMOffset,
                                                                 dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                 bool isWeakImport) {
                            // Get the image so that we can convert from dylib offset to cache offset
                            uint64_t mTime;
                            uint64_t inode;
                            const Header* imageHdr = (const Header*)(dyldCache->getIndexedImageEntry(userImageIndex, mTime, inode));
                            if ( imageHdr == nullptr )
                                return;

                            SegmentInfo usageAt;
                            const uint64_t patchLocVmAddr = imageHdr->preferredLoadAddress() + userVMOffset;
                            const uint64_t patchLocCacheOffset = patchLocVmAddr - cacheBaseAddress;
                            findImageAndSegment(dyldCache, segInfos, patchLocCacheOffset, &usageAt);

                            // Verify that findImage and the callback image match
                            std::string_view userInstallName = imageHdr->installName();

                            // FIXME: We can't get this from MachoLoaded without having a fixup location to call
                            static const char* keyNames[] = {
                                "IA", "IB", "DA", "DB"
                            };

                            uint64_t sectionOffset = patchLocVmAddr-usageAt.vmAddr;

                            const char* weakImportString = isWeakImport ? " (weak-import)" : "";

                            if ( addend == 0 ) {
                                if ( pmd.authenticated ) {
                                    printf("        used by: %.*s(0x%04llX)%s (PAC: div=%d, addr=%s, key=%s) in %s\n",
                                           (int)usageAt.segName.size(), usageAt.segName.data(),
                                           sectionOffset, weakImportString,
                                           pmd.diversity, pmd.usesAddrDiversity ? "true" : "false", keyNames[pmd.key],
                                           userInstallName.data());
                                } else {
                                    printf("        used by: %.*s(0x%04llX)%s in %s\n",
                                           (int)usageAt.segName.size(), usageAt.segName.data(),
                                           sectionOffset, weakImportString, userInstallName.data());
                                }
                            } else {
                                if ( pmd.authenticated ) {
                                    printf("        used by: %.*s(0x%04llX)%s (addend=%lld) (PAC: div=%d, addr=%s, key=%s) in %s\n",
                                           (int)usageAt.segName.size(), usageAt.segName.data(),
                                           sectionOffset, weakImportString, addend,
                                           pmd.diversity, pmd.usesAddrDiversity ? "true" : "false", keyNames[pmd.key],
                                           userInstallName.data());
                                } else {
                                    printf("        used by: %.*s(0x%04llX)%s (addend=%lld) in %s\n",
                                           (int)usageAt.segName.size(), usageAt.segName.data(),
                                           sectionOffset, weakImportString,
                                           addend, userInstallName.data());
                                }
                            }
                        });

                        // Print GOT uses
                        dyldCache->forEachPatchableGOTUseOfExport(imageIndex, dylibVMOffsetOfImpl,
                                                                  ^(uint64_t cacheVMOffset,
                                                                    dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                    bool isWeakImport) {

                            // FIXME: We can't get this from MachoLoaded without having a fixup location to call
                            static const char* keyNames[] = {
                                "IA", "IB", "DA", "DB"
                            };

                            const char* weakImportString = isWeakImport ? " (weak-import)" : "";
                            if ( addend == 0 ) {
                                if ( pmd.authenticated ) {
                                    printf("        used by: GOT%s (PAC: div=%d, addr=%s, key=%s)\n",
                                           weakImportString, pmd.diversity, pmd.usesAddrDiversity ? "true" : "false", keyNames[pmd.key]);
                                } else {
                                    printf("        used by: GOT%s\n", weakImportString);
                                }
                            } else {
                                if ( pmd.authenticated ) {
                                    printf("        used by: GOT%s (addend=%lld) (PAC: div=%d, addr=%s, key=%s)\n",
                                           weakImportString, addend, pmd.diversity, pmd.usesAddrDiversity ? "true" : "false", keyNames[pmd.key]);
                                } else {
                                    printf("        used by: GOT%s (addend=%lld)\n", weakImportString, addend);
                                }
                            }
                        });
                    });
                    ++imageIndex;
                });
                break;
            }
            case modeRootsCost: {
                std::vector<SegmentInfo> segInfos;
                buildSegmentInfo(dyldCache, segInfos);

                __block std::optional<uint32_t> rootImageIndex;
                {
                    __block uint32_t imageIndex = 0;
                    dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                        if ( strcmp(installName, options.rootsCostOfDylib) == 0 )
                            rootImageIndex = imageIndex;
                        ++imageIndex;
                    });
                }

                if ( !rootImageIndex.has_value() ) {
                    fprintf(stderr, "Could not find image '%s' in shared cache\n", options.rootsCostOfDylib);
                    return 1;
                }

                // For each page of the cache, we record if it was dirtied by patching, and by which dylibs
                typedef std::pair<const char*, std::string_view> InstallNameAndSegment;
                __block std::map<uint64_t, std::set<InstallNameAndSegment>> pages;

                uint64_t cacheBaseAddress = dyldCache->unslidLoadAddress();
                dyldCache->forEachPatchableExport(rootImageIndex.value(),
                                                  ^(uint32_t dylibVMOffsetOfImpl, const char* exportName,
                                                    PatchKind patchKind) {
                    if ( (patchKind == PatchKind::cfObj2) || (patchKind == PatchKind::objcClass) )
                        return;
                    dyldCache->forEachPatchableUseOfExport(rootImageIndex.value(), dylibVMOffsetOfImpl,
                                                           ^(uint32_t userImageIndex, uint32_t userVMOffset,
                                                             dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                             bool isWeakImport) {
                        // Get the image so that we can convert from dylib offset to cache offset
                        uint64_t mTime;
                        uint64_t inode;
                        const dyld3::MachOAnalyzer* imageMA = (dyld3::MachOAnalyzer*)(dyldCache->getIndexedImageEntry(userImageIndex, mTime, inode));
                        if ( imageMA == nullptr )
                            return;

                        SegmentInfo usageAt;
                        const uint64_t patchLocVmAddr = ((const Header*)imageMA)->preferredLoadAddress() + userVMOffset;
                        const uint64_t patchLocCacheOffset = patchLocVmAddr - cacheBaseAddress;
                        findImageAndSegment(dyldCache, segInfos, patchLocCacheOffset, &usageAt);

                        // Round to the 16KB page we dirty
                        //clientUsedPages[userImageIndex].insert(usageAt.vmAddr & ~0x3FFF);
                        uint64_t pageAddr = usageAt.vmAddr & ~0x3FFF;
                        pages[pageAddr].insert({ usageAt.installName, usageAt.segName });
                    });

                    // Print GOT uses
                    dyldCache->forEachPatchableGOTUseOfExport(rootImageIndex.value(), dylibVMOffsetOfImpl,
                                                              ^(uint64_t cacheVMOffset,
                                                                dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                bool isWeakImport) {
                        // Round to the 16KB page we dirty
                        //gotUsedPages.insert((cacheBaseAddress + cacheVMOffset) & ~0x3FFF);
                        uint64_t pageAddr = (cacheBaseAddress + cacheVMOffset) & ~0x3FFF;
                        pages[pageAddr].insert({ "GOT", nullptr });
                    });
                });

                // Print the results
                printf("Cost of root of '%s' is %lld pages:\n", options.rootsCostOfDylib, (uint64_t)pages.size());

                for ( const auto& page : pages ) {
                    printf("0x%08llx ", page.first);

                    bool needsComma = false;
                    for ( const InstallNameAndSegment& installNameAndSegment : page.second ) {
                        if ( needsComma )
                            printf(", ");
                        needsComma = true;

                        const char* leafName = strrchr(installNameAndSegment.first, '/');
                        if ( leafName == NULL )
                            leafName = installNameAndSegment.first;
                        else
                            leafName++;

                        if ( !installNameAndSegment.second.empty() )
                            printf("%s(%.*s)", leafName, (int)installNameAndSegment.second.size(), installNameAndSegment.second.data());
                        else
                            printf("%s", leafName);
                    }
                    printf("\n");
                }

                break;
            }
            case modeMachHeaders: {
                auto printRow = [](const char* magic, const char* arch, const char* filetype,
                                   const char* ncmds, const char* sizeofcmds , const char* flags,
                                   const char* installname) {
                    printf("%12s %8s %8s %8s %12s %12s %8s\n",
                           magic, arch, filetype, ncmds, sizeofcmds , flags, installname);
                };

                printRow("magic", "arch", "filetype", "ncmds", "sizeofcmds", "flags", "installname");
                dyldCache->forEachDylib(^(const Header *mh, const char *installName, uint32_t imageIndex, uint64_t inode, uint64_t mtime, bool &stop) {
                    const dyld3::MachOFile* mf = (const dyld3::MachOFile*)mh;
                    const char* magic = nullptr;
                    if ( mf->magic == MH_MAGIC )
                        magic = "MH_MAGIC";
                    else if ( mf->magic == MH_MAGIC_64 )
                        magic = "MH_MAGIC_64";
                    else if ( mf->magic == MH_CIGAM )
                        magic = "MH_CIGAM";
                    else if ( mf->magic == MH_CIGAM_64 )
                        magic = "MH_CIGAM_64";

                    const char* arch = mf->archName();
                    const char* filetype = mf->isDylib() ? "DYLIB" : "UNKNOWN";
                    std::string ncmds = json::decimal(mf->ncmds);
                    std::string sizeofcmds = json::decimal(mf->sizeofcmds);
                    std::string flags = json::hex(mf->flags);

                    printRow(magic, arch, filetype, ncmds.c_str(), sizeofcmds.c_str(), flags.c_str(), installName);
                });

                break;
            }
            case modeCacheHeader: {
                __block uint32_t cacheIndex = 0;
                dyldCache->forEachCache(^(const DyldSharedCache *cache, bool &stopCache) {
                    printf("Cache #%d\n", cacheIndex);

                    uuid_string_t uuidString;
                    uuid_unparse_upper(dyldCache->header.uuid, uuidString);

                    uuid_string_t symbolFileUUIDString;
                    uuid_unparse_upper(dyldCache->header.symbolFileUUID, symbolFileUUIDString);

                    printf("  - magic: %s\n", dyldCache->header.magic);
                    printf("  - mappingOffset: 0x%llx\n", (uint64_t)dyldCache->header.mappingOffset);
                    printf("  - mappingCount: 0x%llx\n", (uint64_t)dyldCache->header.mappingCount);
                    printf("  - imagesOffsetOld: 0x%llx\n", (uint64_t)dyldCache->header.imagesOffsetOld);
                    printf("  - imagesCountOld: 0x%llx\n", (uint64_t)dyldCache->header.imagesCountOld);
                    printf("  - dyldBaseAddress: 0x%llx\n", (uint64_t)dyldCache->header.dyldBaseAddress);
                    printf("  - codeSignatureOffset: 0x%llx\n", (uint64_t)dyldCache->header.codeSignatureOffset);
                    printf("  - codeSignatureSize: 0x%llx\n", (uint64_t)dyldCache->header.codeSignatureSize);
                    printf("  - slideInfoOffsetUnused: 0x%llx\n", (uint64_t)dyldCache->header.slideInfoOffsetUnused);
                    printf("  - slideInfoSizeUnused: 0x%llx\n", (uint64_t)dyldCache->header.slideInfoSizeUnused);
                    printf("  - localSymbolsOffset: 0x%llx\n", (uint64_t)dyldCache->header.localSymbolsOffset);
                    printf("  - localSymbolsSize: 0x%llx\n", (uint64_t)dyldCache->header.localSymbolsSize);
                    printf("  - uuid: %s\n", uuidString);
                    printf("  - cacheType: 0x%llx\n", (uint64_t)dyldCache->header.cacheType);
                    printf("  - branchPoolsOffset: 0x%llx\n", (uint64_t)dyldCache->header.branchPoolsOffset);
                    printf("  - branchPoolsCount: 0x%llx\n", (uint64_t)dyldCache->header.branchPoolsCount);
                    printf("  - dyldInCacheMH: 0x%llx\n", (uint64_t)dyldCache->header.dyldInCacheMH);
                    printf("  - dyldInCacheEntry: 0x%llx\n", (uint64_t)dyldCache->header.dyldInCacheEntry);
                    printf("  - imagesTextOffset: 0x%llx\n", (uint64_t)dyldCache->header.imagesTextOffset);
                    printf("  - imagesTextCount: 0x%llx\n", (uint64_t)dyldCache->header.imagesTextCount);
                    printf("  - patchInfoAddr: 0x%llx\n", (uint64_t)dyldCache->header.patchInfoAddr);
                    printf("  - patchInfoSize: 0x%llx\n", (uint64_t)dyldCache->header.patchInfoSize);
                    printf("  - otherImageGroupAddrUnused: 0x%llx\n", (uint64_t)dyldCache->header.otherImageGroupAddrUnused);
                    printf("  - otherImageGroupSizeUnused: 0x%llx\n", (uint64_t)dyldCache->header.otherImageGroupSizeUnused);
                    printf("  - progClosuresAddr: 0x%llx\n", (uint64_t)dyldCache->header.progClosuresAddr);
                    printf("  - progClosuresSize: 0x%llx\n", (uint64_t)dyldCache->header.progClosuresSize);
                    printf("  - progClosuresTrieAddr: 0x%llx\n", (uint64_t)dyldCache->header.progClosuresTrieAddr);
                    printf("  - progClosuresTrieSize: 0x%llx\n", (uint64_t)dyldCache->header.progClosuresTrieSize);
                    printf("  - platform: 0x%llx\n", (uint64_t)dyldCache->header.platform);
                    printf("  - formatVersion: 0x%llx\n", (uint64_t)dyldCache->header.formatVersion);
                    printf("  - dylibsExpectedOnDisk: 0x%llx\n", (uint64_t)dyldCache->header.dylibsExpectedOnDisk);
                    printf("  - simulator: 0x%llx\n", (uint64_t)dyldCache->header.simulator);
                    printf("  - locallyBuiltCache: 0x%llx\n", (uint64_t)dyldCache->header.locallyBuiltCache);
                    printf("  - builtFromChainedFixups: 0x%llx\n", (uint64_t)dyldCache->header.builtFromChainedFixups);
                    printf("  - padding: 0x%llx\n", (uint64_t)dyldCache->header.padding);
                    printf("  - sharedRegionStart: 0x%llx\n", (uint64_t)dyldCache->header.sharedRegionStart);
                    printf("  - sharedRegionSize: 0x%llx\n", (uint64_t)dyldCache->header.sharedRegionSize);
                    printf("  - maxSlide: 0x%llx\n", (uint64_t)dyldCache->header.maxSlide);
                    printf("  - dylibsImageArrayAddr: 0x%llx\n", (uint64_t)dyldCache->header.dylibsImageArrayAddr);
                    printf("  - dylibsImageArraySize: 0x%llx\n", (uint64_t)dyldCache->header.dylibsImageArraySize);
                    printf("  - dylibsTrieAddr: 0x%llx\n", (uint64_t)dyldCache->header.dylibsTrieAddr);
                    printf("  - dylibsTrieSize: 0x%llx\n", (uint64_t)dyldCache->header.dylibsTrieSize);
                    printf("  - otherImageArrayAddr: 0x%llx\n", (uint64_t)dyldCache->header.otherImageArrayAddr);
                    printf("  - otherImageArraySize: 0x%llx\n", (uint64_t)dyldCache->header.otherImageArraySize);
                    printf("  - otherTrieAddr: 0x%llx\n", (uint64_t)dyldCache->header.otherTrieAddr);
                    printf("  - otherTrieSize: 0x%llx\n", (uint64_t)dyldCache->header.otherTrieSize);
                    printf("  - mappingWithSlideOffset: 0x%llx\n", (uint64_t)dyldCache->header.mappingWithSlideOffset);
                    printf("  - mappingWithSlideCount: 0x%llx\n", (uint64_t)dyldCache->header.mappingWithSlideCount);
                    printf("  - dylibsPBLStateArrayAddrUnused: 0x%llx\n", (uint64_t)dyldCache->header.dylibsPBLStateArrayAddrUnused);
                    printf("  - dylibsPBLSetAddr: 0x%llx\n", (uint64_t)dyldCache->header.dylibsPBLSetAddr);
                    printf("  - programsPBLSetPoolAddr: 0x%llx\n", (uint64_t)dyldCache->header.programsPBLSetPoolAddr);
                    printf("  - programsPBLSetPoolSize: 0x%llx\n", (uint64_t)dyldCache->header.programsPBLSetPoolSize);
                    printf("  - programTrieAddr: 0x%llx\n", (uint64_t)dyldCache->header.programTrieAddr);
                    printf("  - programTrieSize: 0x%llx\n", (uint64_t)dyldCache->header.programTrieSize);
                    printf("  - osVersion: 0x%llx\n", (uint64_t)dyldCache->header.osVersion);
                    printf("  - altPlatform: 0x%llx\n", (uint64_t)dyldCache->header.altPlatform);
                    printf("  - altOsVersion: 0x%llx\n", (uint64_t)dyldCache->header.altOsVersion);
                    printf("  - swiftOptsOffset: 0x%llx\n", (uint64_t)dyldCache->header.swiftOptsOffset);
                    printf("  - swiftOptsSize: 0x%llx\n", (uint64_t)dyldCache->header.swiftOptsSize);
                    printf("  - subCacheArrayOffset: 0x%llx\n", (uint64_t)dyldCache->header.subCacheArrayOffset);
                    printf("  - subCacheArrayCount: 0x%llx\n", (uint64_t)dyldCache->header.subCacheArrayCount);
                    printf("  - symbolFileUUID: %s\n", symbolFileUUIDString);
                    printf("  - rosettaReadOnlyAddr: 0x%llx\n", (uint64_t)dyldCache->header.rosettaReadOnlyAddr);
                    printf("  - rosettaReadOnlySize: 0x%llx\n", (uint64_t)dyldCache->header.rosettaReadOnlySize);
                    printf("  - rosettaReadWriteAddr: 0x%llx\n", (uint64_t)dyldCache->header.rosettaReadWriteAddr);
                    printf("  - rosettaReadWriteSize: 0x%llx\n", (uint64_t)dyldCache->header.rosettaReadWriteSize);
                    printf("  - imagesOffset: 0x%llx\n", (uint64_t)dyldCache->header.imagesOffset);
                    printf("  - imagesCount: 0x%llx\n", (uint64_t)dyldCache->header.imagesCount);
                    printf("  - cacheSubType: 0x%llx\n", (uint64_t)dyldCache->header.cacheSubType);
                    printf("  - objcOptsOffset: 0x%llx\n", (uint64_t)dyldCache->header.objcOptsOffset);
                    printf("  - cacheAtlasOffset: 0x%llx\n", (uint64_t)dyldCache->header.cacheAtlasOffset);
                    printf("  - cacheAtlasSize: 0x%llx\n", (uint64_t)dyldCache->header.cacheAtlasSize);
                    printf("  - dynamicDataOffset: 0x%llx\n", (uint64_t)dyldCache->header.dynamicDataOffset);
                    printf("  - dynamicDataMaxSize: 0x%llx\n", (uint64_t)dyldCache->header.dynamicDataMaxSize);
                    printf("  - tproMappingsOffset: 0x%llx\n", (uint64_t)dyldCache->header.tproMappingsOffset);
                    printf("  - tproMappingsCount: 0x%llx\n", (uint64_t)dyldCache->header.tproMappingsCount);
                    printf("  - functionVariantInfoAddr: 0x%llx\n", (uint64_t)dyldCache->header.functionVariantInfoAddr);
                    printf("  - functionVariantInfoSize: 0x%llx\n", (uint64_t)dyldCache->header.functionVariantInfoSize);
                    ++cacheIndex;
                });
                break;
            }
            case modeDylibSymbols:
            {
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                    Diagnostics diag;

                    printf("%s globals:\n", installName);
                    ma->forEachGlobalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                        printf("  0x%08llX: %s\n", n_value, symbolName);
                    });
                    printf("%s locals:\n", installName);
                    ma->forEachLocalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                        printf("  0x%08llX: %s\n", n_value, symbolName);
                    });
                    printf("%s undefs:\n", installName);
                    ma->forEachImportedSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                        printf("  undef: %s\n", symbolName);
                    });
                });
                break;
            }
            case modeFunctionStarts: {
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    printf("%s:\n", installName);
                    uint64_t loadAddress = hdr->preferredLoadAddress();
                    Diagnostics diag;
                    const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                    ma->forEachFunctionStart(^(uint64_t runtimeOffset) {
                        uint64_t targetVMAddr = loadAddress + runtimeOffset;
                        printf("        0x%08llX\n", targetVMAddr);
                    });
                });
                break;
            }
            case modePrewarmingData: {
                printf("prewarming_data:\n");
                dyldCache->forEachPrewarmingEntry(^(const void *content, uint64_t unslidVMAddr, uint64_t vmSize) {
                    printf("0x%08llx -> 0x%08llx\n", unslidVMAddr, unslidVMAddr + vmSize);
                });
                break;
            }
            case modeDuplicates:
            case modeDuplicatesSummary:
            {
                __block std::map<std::string, std::vector<const char*>> symbolsToInstallNames;
                __block std::set<std::string>                           weakDefSymbols;
                dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                    const dyld3::MachOAnalyzer* ma = (dyld3::MachOAnalyzer*)hdr;
                    uint32_t exportTrieRuntimeOffset;
                    uint32_t exportTrieSize;
                    if ( ma->hasExportTrie(exportTrieRuntimeOffset, exportTrieSize) ) {
                        const uint8_t* start = (uint8_t*)hdr + exportTrieRuntimeOffset;
                        const uint8_t* end = start + exportTrieSize;
                        std::vector<ExportInfoTrie::Entry> exports;
                        if ( ExportInfoTrie::parseTrie(start, end, exports) ) {
                            for (const ExportInfoTrie::Entry& entry: exports) {
                                if ( (entry.info.flags & EXPORT_SYMBOL_FLAGS_REEXPORT) == 0 ) {
                                    symbolsToInstallNames[entry.name].push_back(installName);
                                    if ( entry.info.flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION ) {
                                        weakDefSymbols.insert(entry.name);
                                    }
                                }
                            }
                        }
                    }
                });
                // filter out unzippered twins
                std::set<std::string> okTwinSymbols;
                for (const auto& pos : symbolsToInstallNames) {
                    const std::vector<const char*>& paths = pos.second;
                    if ( paths.size() == 2 ) {
                        // ignore unzippered twins
                        const char* one = paths[0];
                        const char* two = paths[1];
                        if ( (strncmp(one, "/System/iOSSupport/", 19) == 0) || (strncmp(two, "/System/iOSSupport/", 19) == 0) ) {
                            if ( const char* tailOne = Utils::strrstr(one, ".framework/") ) {
                                if ( const char* tailTwo = Utils::strrstr(two, ".framework/") ) {
                                    if ( strcmp(tailOne, tailTwo) == 0 )
                                        okTwinSymbols.insert(pos.first);
                                }
                            }
                        }
                    }
                }
                std::erase_if(symbolsToInstallNames, [&](auto const& pos) { return okTwinSymbols.count(pos.first); });

                if ( options.mode == modeDuplicatesSummary ) {
                    __block std::map<std::string, int> dylibDuplicatesCount;
                    for (const auto& pos : symbolsToInstallNames) {
                        const std::vector<const char*>& paths = pos.second;
                        if ( paths.size() <= 1 )
                            continue;
                        for (const char* path : paths) {
                            dylibDuplicatesCount[path] += 1;
                        }
                    }
                    struct DupCount { const char* path; int count; };
                    std::vector<DupCount> summary;
                    for (const auto& pos : dylibDuplicatesCount) {
                        summary.push_back({pos.first.c_str(), pos.second});
                    }
                    std::sort(summary.begin(), summary.end(), [](const DupCount& l, const DupCount& r) -> bool {
                        return (l.count > r.count);
                    });
                    for ( const DupCount& entry : summary)
                         printf("% 5d  %s\n", entry.count, entry.path);
                }
                else {
                    for (const auto& pos : symbolsToInstallNames) {
                        const std::vector<const char*>& paths = pos.second;
                        if ( paths.size() > 1 ) {
                            bool isWeakDef = (weakDefSymbols.count(pos.first) != 0);
                            printf("%s%s\n", pos.first.c_str(), (isWeakDef ? " [weak-def]" : ""));
                            for (const char* path : paths)
                                printf("   %s\n", path);
                        }
                    }
                }
            }
            break;

            case modeNone:
            case modeInfo:
            case modeTPROInfo:
            case modeStats:
            case modeSlideInfo:
            case modeVerboseSlideInfo:
            case modeFixupsInDylib:
            case modeTextInfo:
            case modeLocalSymbols:
            case modeJSONMap:
            case modeVerboseJSONMap:
            case modeJSONDependents:
            case modeSectionSizes:
            case modeStrings:
            case modeObjCInfo:
            case modeObjCProtocols:
            case modeObjCImpCaches:
            case modeObjCClasses:
            case modeObjCClassLayout:
            case modeObjCClassMethodLists:
            case modeObjCClassHashTable:
            case modeObjCSelectors:
            case modeSwiftProtocolConformances:
            case modeSwiftPtrTables:
            case modeExtract:
            case modeLookupVA:
                break;
        }
    }
    return 0;
}
