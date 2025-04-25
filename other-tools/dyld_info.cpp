/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2018-2023 Apple Inc. All rights reserved.
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

// OS
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_priv.h>
#include <SoftLinking/WeakLinking.h>

// STL
#include <vector>
#include <tuple>
#include <set>
#include <unordered_set>
#include <string>

// mach_o
#include "DyldSharedCache.h"
#include "Header.h"
#include "Version32.h"
#include "Universal.h"
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "SplitSeg.h"
#include "ChainedFixups.h"
#include "FunctionStarts.h"
#include "Misc.h"
#include "Instructions.h"
#include "FunctionVariants.h"

// common
#include "Defines.h"
#include "FileUtils.h"
#include "SymbolicatedImage.h"

// other_tools
#include "MiscFileUtils.h"

using mach_o::Header;
using mach_o::LinkedDylibAttributes;
using mach_o::Version32;
using mach_o::Image;
using mach_o::MappedSegment;
using mach_o::Fixup;
using mach_o::Symbol;
using mach_o::PlatformAndVersions;
using mach_o::ChainedFixups;
using mach_o::CompactUnwind;
using mach_o::Architecture;
using mach_o::SplitSegInfo;
using mach_o::Error;
using mach_o::Instructions;
using mach_o::FunctionVariantsRuntimeTable;
using mach_o::FunctionVariants;
using mach_o::FunctionVariantFixups;
using other_tools::SymbolicatedImage;

typedef mach_o::ChainedFixups::PointerFormat    PointerFormat;

static void printPlatforms(const Header* header)
{
    if ( header->isPreload() )
        return;
    PlatformAndVersions pvs = header->platformAndVersions();
    char osVers[32];
    char sdkVers[32];
    pvs.minOS.toString(osVers);
    pvs.sdk.toString(sdkVers);
    printf("    -platform:\n");
    printf("        platform     minOS      sdk\n");
    printf(" %15s     %-7s   %-7s\n", pvs.platform.name().c_str(), osVers, sdkVers);
}

static void printUUID(const Header* header)
{
    printf("    -uuid:\n");
    uuid_t uuid;
    if ( header->getUuid(uuid) ) {
        uuid_string_t uuidString;
        uuid_unparse_upper(uuid, uuidString);
        printf("        %s\n", uuidString);
    }
}

static void permString(uint32_t permFlags, char str[4])
{
    str[0] = (permFlags & VM_PROT_READ)    ? 'r' : '.';
    str[1] = (permFlags & VM_PROT_WRITE)   ? 'w' : '.';
    str[2] = (permFlags & VM_PROT_EXECUTE) ? 'x' : '.';
    str[3] = '\0';
}

static void printSegments(const Header* header)
{
    if ( header->isPreload() ) {
        printf("    -segments:\n");
        printf("       file-offset vm-addr       segment     section         sect-size  seg-size  init/max-prot\n");
        __block std::string_view lastSegName;
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( sectInfo.segmentName != lastSegName ) {
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char maxProtChars[8];
                permString(sectInfo.segMaxProt, maxProtChars);
                char initProtChars[8];
                permString(sectInfo.segInitProt, initProtChars);
                printf("        0x%06X   0x%09llX    %-16.*s                    %6lluKB     %s/%s\n",
                       sectInfo.fileOffset, sectInfo.address,
                       (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                       segVmSize/1024, initProtChars, maxProtChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%06X   0x%09llX             %-16.*s %7llu\n",
                       sectInfo.fileOffset, sectInfo.address,
                       (int)sectInfo.sectionName.size(), sectInfo.sectionName.data(),
                       sectInfo.size);
        });
    }
    else if ( header->inDyldCache() ) {
        printf("    -segments:\n");
        printf("        unslid-addr    segment   section        sect-size  seg-size   init/max-prot\n");
        __block std::string_view lastSegName;
        __block uint64_t         segVmAddr    = 0;
        __block uint64_t         startVmAddr  = header->segmentVmAddr(0);
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( sectInfo.segmentName != lastSegName ) {
                segVmAddr = header->segmentVmAddr(sectInfo.segIndex);
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char maxProtChars[8];
                permString(sectInfo.segMaxProt, maxProtChars);
                char initProtChars[8];
                permString(sectInfo.segInitProt, initProtChars);
                printf("        0x%09llX    %-16.*s                %6lluKB     %s/%s\n",
                       segVmAddr,
                       (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                       segVmSize/1024, initProtChars, maxProtChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%09llX           %-16.*s %7llu\n",
                       startVmAddr+sectInfo.address,
                       (int)sectInfo.sectionName.size(), sectInfo.sectionName.data(),
                       sectInfo.size);
        });
    }
    else {
        printf("    -segments:\n");
        printf("        load-offset   segment  section       sect-size  seg-size   init/max-prot\n");
        __block std::string_view lastSegName;
        __block uint64_t         textSegVmAddr = 0;
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( lastSegName.empty() ) {
                textSegVmAddr  = header->segmentVmAddr(sectInfo.segIndex);
            }
            if ( sectInfo.segmentName != lastSegName ) {
                uint64_t segVmAddr = header->segmentVmAddr(sectInfo.segIndex);
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char maxProtChars[8];
                permString(sectInfo.segMaxProt, maxProtChars);
                char initProtChars[8];
                permString(sectInfo.segInitProt, initProtChars);
                printf("        0x%08llX    %-16.*s                  %6lluKB    %s/%s\n",
                       segVmAddr-textSegVmAddr,
                       (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                       segVmSize/1024, initProtChars,maxProtChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%08llX             %-16.*s %6llu\n",
                       sectInfo.address,
                       (int)sectInfo.sectionName.size(), sectInfo.sectionName.data(),
                       sectInfo.size);
        });
    }
}

static void printLinkedDylibs(const Header* mh)
{
    if ( mh->isPreload() )
        return;
    printf("    -linked_dylibs:\n");
    printf("        attributes     load path\n");
    mh->forEachLinkedDylib(^(const char* loadPath, LinkedDylibAttributes depAttrs, Version32 compatVersion, Version32 curVersion, 
                             bool synthesizedLink, bool& stop) {
        if ( synthesizedLink )
            return;
        std::string attributes;
        if ( depAttrs.upward )
            attributes += "upward ";
        if ( depAttrs.delayInit )
            attributes += "delay-init ";
        if ( depAttrs.weakLink )
            attributes += "weak-link ";
        if ( depAttrs.reExport )
            attributes += "re-export ";
        printf("        %-12s   %s\n", attributes.c_str(), loadPath);
    });
}

static void printInitializers(const Image& image)
{
    printf("    -inits:\n");
    SymbolicatedImage symImage(image);

    // print static initializers
    bool contentRebased = false;
    image.forEachInitializer(contentRebased, ^(uint32_t initOffset) {
        uint64_t    unslidInitAddr = image.header()->preferredLoadAddress() + initOffset;
        Symbol      symbol;
        uint64_t    addend         = 0;
        const char* initName       = symImage.symbolNameAt(initOffset);
        if ( initName == nullptr ) {
            if ( image.symbolTable().findClosestDefinedSymbol(unslidInitAddr, symbol) ) {
                initName = symbol.name().c_str();
                uint64_t symbolAddr = image.header()->preferredLoadAddress() + symbol.implOffset();
                addend = unslidInitAddr - symbolAddr;
            }
        }
        if ( initName == nullptr )
            initName = "";
        if ( addend == 0 )
            printf("        0x%08X  %s\n", initOffset, initName);
        else
            printf("        0x%08X  %s + %llu\n", initOffset, initName, addend);
    });

    // print static terminators
    if ( !image.header()->isArch("arm64e") ) {
        image.forEachClassicTerminator(contentRebased, ^(uint32_t termOffset) {
            uint64_t    unslidInitAddr = image.header()->preferredLoadAddress() + termOffset;
            Symbol      symbol;
            uint64_t    addend         = 0;
            const char* termName       = symImage.symbolNameAt(termOffset);
            if ( termName == nullptr ) {
                if ( image.symbolTable().findClosestDefinedSymbol(unslidInitAddr, symbol) ) {
                    termName = symbol.name().c_str();
                    uint64_t symbolAddr = image.header()->preferredLoadAddress() + symbol.implOffset();
                    addend = unslidInitAddr - symbolAddr;
                }
            }
            if ( termName == nullptr )
                termName = "";
            if ( addend == 0 )
                printf("        0x%08X  %s [terminator]\n", termOffset, termName);
            else
                printf("        0x%08X  %s + %llu [terminator]\n", termOffset, termName, addend);
        });
    }

    // print +load methods
    // TODO: rdar://122190141 (Enable +load initializers in dyld_info)
    //if ( image.header()->hasObjC() ) {
    //    const SymbolicatedImage* symImagePtr = &symImage; // for no copy in block...
    //    symImage.forEachDefinedObjCClass(^(uint64_t classVmAddr) {
    //        const char*  classname       = symImagePtr->className(classVmAddr);
    //        uint64_t     metaClassVmaddr = symImagePtr->metaClassVmAddr(classVmAddr);
    //        symImagePtr->forEachMethodInClass(metaClassVmaddr, ^(const char* methodName, uint64_t implAddr) {
    //            if ( strcmp(methodName, "load") == 0 )
    //                printf("        0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
    //        });
    //    });
    //    symImage.forEachObjCCategory(^(uint64_t categoryVmAddr) {
    //        const char* catname   = symImagePtr->categoryName(categoryVmAddr);
    //        const char* classname = symImagePtr->categoryClassName(categoryVmAddr);
    //        symImagePtr->forEachMethodInCategory(categoryVmAddr,
    //                                             ^(const char* instanceMethodName, uint64_t implAddr) {},
    //                                             ^(const char* classMethodName,    uint64_t implAddr) {
    //            if ( strcmp(classMethodName, "load") == 0 )
    //                printf("        0x%08llX  +[%s(%s) %s]\n", implAddr, classname, catname, classMethodName);
    //        });
    //    });
    //}
}

static void printChainInfo(const Image& image)
{
    printf("    -fixup_chains:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    if ( image.hasChainedFixups() ) {
        const ChainedFixups& chainedFixups = image.chainedFixups();
        if ( const dyld_chained_fixups_header* chainHeader = (dyld_chained_fixups_header*)chainedFixups.linkeditHeader() ) {
            printf("      fixups_version:   0x%08X\n",  chainHeader->fixups_version);
            printf("      starts_offset:    0x%08X\n",  chainHeader->starts_offset);
            printf("      imports_offset:   0x%08X\n",  chainHeader->imports_offset);
            printf("      symbols_offset:   0x%08X\n",  chainHeader->symbols_offset);
            printf("      imports_count:    %d\n",      chainHeader->imports_count);
            printf("      imports_format:   %d (%s)\n", chainHeader->imports_format, ChainedFixups::importsFormatName(chainHeader->imports_format));
            printf("      symbols_format:   %d\n",      chainHeader->symbols_format);
            const dyld_chained_starts_in_image* starts = (dyld_chained_starts_in_image*)((uint8_t*)chainHeader + chainHeader->starts_offset);
            for (int i=0; i < starts->seg_count; ++i) {
                if ( starts->seg_info_offset[i] == 0 )
                    continue;
                const dyld_chained_starts_in_segment* seg = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[i]);
                if ( seg->page_count == 0 )
                    continue;
                const uint8_t* segEnd = ((uint8_t*)seg + seg->size);
                const PointerFormat& pf = PointerFormat::make(seg->pointer_format);
                printf("        seg[%d]:\n", i);
                printf("          page_size:       0x%04X\n",      seg->page_size);
                printf("          pointer_format:  %d (%s)(%s)\n", seg->pointer_format, pf.name(), pf.description());
                printf("          segment_offset:  0x%08llX\n",    seg->segment_offset);
                printf("          max_pointer:     0x%08X\n",      seg->max_valid_pointer);
                printf("          pages:         %d\n",            seg->page_count);
                for (int pageIndex=0; pageIndex < seg->page_count; ++pageIndex) {
                    if ( (uint8_t*)(&seg->page_start[pageIndex]) >= segEnd ) {
                        printf("         start[% 2d]:  <<<off end of dyld_chained_starts_in_segment>>>\n", pageIndex);
                        continue;
                    }
                    uint16_t offsetInPage = seg->page_start[pageIndex];
                    if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
                        continue;
                    if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
                        // 32-bit chains which may need multiple starts per page
                        uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
                        bool chainEnd = false;
                        while (!chainEnd) {
                            chainEnd = (seg->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                            offsetInPage = (seg->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                            printf("         start[% 2d]:  0x%04X\n",   pageIndex, offsetInPage);
                            ++overflowIndex;
                        }
                    }
                    else {
                        // one chain per page
                        printf("             start[% 2d]:  0x%04X\n",   pageIndex, offsetInPage);
                    }
                }
            }
        }
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(fwPointerFormat);
        printf("  pointer_format:  %d (%s)\n", fwPointerFormat, pf.description());

        for (uint32_t i=0; i < fwStartsCount; ++i) {
            const uint32_t startVmOffset = fwStarts[i];
            printf("    start[% 2d]: vm offset: 0x%04X\n", i, startVmOffset);
        }
    }
}

static void printImports(const Image& image)
{
    printf("    -imports:\n");
    __block uint32_t bindOrdinal = 0;
    if ( image.hasChainedFixups() ) {
        image.chainedFixups().forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
            char buffer[128];
            const char* weakStr = (target.weakImport ? "[weak-import]" : "");
            if ( target.addend == 0 )
                printf("      0x%04X  %s %s (from %s)\n", bindOrdinal, target.symbolName.c_str(), weakStr, SymbolicatedImage::libOrdinalName(image.header(), target.libOrdinal, buffer));
            else
                printf("      0x%04X  %s+0x%llX %s (from %s)\n", bindOrdinal, target.symbolName.c_str(), target.addend, weakStr, SymbolicatedImage::libOrdinalName(image.header(), target.libOrdinal, buffer));
            ++bindOrdinal;
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachUndefinedSymbol(^(const Symbol& symbol, uint32_t symIndex, bool& stop) {
            int  libOrdinal;
            bool weakImport;
            if ( symbol.isUndefined(libOrdinal, weakImport) ) {
                char buffer[128];
                const char* weakStr = (weakImport ? "[weak-import]" : "");
                printf("      %s %s (from %s)\n", symbol.name().c_str(), weakStr, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
            }
        });
    }
}


static void printChainDetails(const Image& image)
{
    printf("    -fixup_chain_details:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    uint64_t           prefLoadAddr = image.header()->preferredLoadAddress();
    if ( image.hasChainedFixups() ) {
        image.withSegments(^(std::span<const MappedSegment> segments) {
            image.chainedFixups().forEachFixupChainStartLocation(segments, ^(const void* chainStart, uint32_t segIndex, uint32_t pageIndex, uint32_t pageSize, const ChainedFixups::PointerFormat& pf, bool& stop) {
                pf.forEachFixupLocationInChain(chainStart, prefLoadAddr, &segments[segIndex], {}, pageIndex, pageSize,
                                               ^(const Fixup& info, bool& stop2) {
                    uint64_t vmOffset = (uint8_t*)info.location - (uint8_t*)image.header();
                    const void* nextLoc = pf.nextLocation(info.location);
                    uint32_t next = 0;
                    if ( nextLoc != nullptr )
                        next = (uint32_t)((uint8_t*)nextLoc - (uint8_t*)info.location)/pf.minNext();
                    if ( info.isBind ) {
                        if ( image.header()->is64() ) {
                            const char* authPrefix = "     ";
                            char authInfoStr[128] = "";
                            if ( info.authenticated ) {
                                authPrefix = "auth-";
                                snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                                         info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                            }
                            char addendInfo[32] = "";
                            if ( info.bind.embeddedAddend != 0 )
                                snprintf(addendInfo, sizeof(addendInfo), ", addend: %d", info.bind.embeddedAddend);
                            printf("  0x%08llX:  raw: 0x%016llX    %sbind: (next: %03d, %sbindOrdinal: 0x%06X%s)\n",
                                   vmOffset, *((uint64_t*)info.location), authPrefix, next, authInfoStr, info.bind.bindOrdinal, addendInfo);
                        }
                        else {
                            printf("  0x%08llX:  raw: 0x%08X     bind: (next: %02d bindOrdinal: 0x%07X)\n",
                                   vmOffset, *((uint32_t*)info.location), next, info.bind.bindOrdinal);

                        }
                    }
                    else {
                        uint8_t high8 = 0; // FIXME:
                        if ( image.header()->is64() ) {
                            const char* authPrefix = "     ";
                            char authInfoStr[128] = "";
                            if ( info.authenticated ) {
                                authPrefix = "auth-";
                                snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                                         info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                            }
                            char high8Info[32] = "";
                            if ( high8 != 0 )
                                snprintf(high8Info, sizeof(high8Info), ", high8: 0x%02X", high8);
                            printf("  0x%08llX:  raw: 0x%016llX  %srebase: (next: %03d, %starget: 0x%011llX%s)\n",
                                   vmOffset, *((uint64_t*)info.location), authPrefix, next, authInfoStr, info.rebase.targetVmOffset, high8Info);
                        }
                        else {
                            printf("  0x%08llX:  raw: 0x%08X  rebase: (next: %02d target: 0x%07llX)\n",
                                   vmOffset, *((uint32_t*)info.location), next, info.rebase.targetVmOffset);

                        }
                    }
                });
            });
        });
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        image.forEachFixup(^(const Fixup& info, bool& stop) {
            uint64_t segOffset = (uint8_t*)info.location - (uint8_t*)info.segment->content;
            uint64_t vmAddr    = prefLoadAddr + info.segment->runtimeOffset + segOffset;
            uint8_t  high8    = 0; // FIXME:
            if ( image.header()->is64() ) {
                const char* authPrefix = "     ";
                char authInfoStr[128] = "";
                if ( info.authenticated ) {
                    authPrefix = "auth-";
                    snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                             info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                }
                char high8Info[32] = "";
                if ( high8 != 0 )
                    snprintf(high8Info, sizeof(high8Info), ", high8: 0x%02X", high8);
                printf("  0x%08llX:  raw: 0x%016llX  %srebase: (%starget: 0x%011llX%s)\n",
                        vmAddr, *((uint64_t*)info.location), authPrefix, authInfoStr, info.rebase.targetVmOffset, high8Info);
            }
            else {
                printf("  0x%08llX:  raw: 0x%08X  rebase: (target: 0x%07llX)\n",
                        vmAddr, *((uint32_t*)info.location), info.rebase.targetVmOffset);
            }
        });
    }
}

static void printChainHeader(const Image& image)
{
    printf("    -fixup_chain_header:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    if ( image.hasChainedFixups() ) {
        const ChainedFixups& chainedFixups = image.chainedFixups();
        if ( const dyld_chained_fixups_header* chainsHeader = chainedFixups.linkeditHeader() ) {
            printf("        dyld_chained_fixups_header:\n");
            printf("            fixups_version  0x%08X\n", chainsHeader->fixups_version);
            printf("            starts_offset   0x%08X\n", chainsHeader->starts_offset);
            printf("            imports_offset  0x%08X\n", chainsHeader->imports_offset);
            printf("            symbols_offset  0x%08X\n", chainsHeader->symbols_offset);
            printf("            imports_count   0x%08X\n", chainsHeader->imports_count);
            printf("            imports_format  0x%08X\n", chainsHeader->imports_format);
            printf("            symbols_format  0x%08X\n", chainsHeader->symbols_format);
            const dyld_chained_starts_in_image* starts = (dyld_chained_starts_in_image*)((uint8_t*)chainsHeader + chainsHeader->starts_offset);
            printf("        dyld_chained_starts_in_image:\n");
            printf("            seg_count              0x%08X\n", starts->seg_count);
            for ( uint32_t i = 0; i != starts->seg_count; ++i )
                printf("            seg_info_offset[%d]     0x%08X\n", i, starts->seg_info_offset[i]);
            for (uint32_t segIndex = 0; segIndex < starts->seg_count; ++segIndex) {
                if ( starts->seg_info_offset[segIndex] == 0 )
                    continue;
                printf("        dyld_chained_starts_in_segment:\n");
                const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
                printf("            size                0x%08X\n", segInfo->size);
                printf("            page_size           0x%08X\n", segInfo->page_size);
                printf("            pointer_format      0x%08X\n", segInfo->pointer_format);
                printf("            segment_offset      0x%08llX\n", segInfo->segment_offset);
                printf("            max_valid_pointer   0x%08X\n", segInfo->max_valid_pointer);
                printf("            page_count          0x%08X\n", segInfo->page_count);
            }
            printf("        targets:\n");
            chainedFixups.forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
                printf("            symbol          %s\n", target.symbolName.c_str());
            });
        }
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(fwPointerFormat);
        printf("        firmware chains:\n");
        printf("          pointer_format:  %d (%s)\n", fwPointerFormat, pf.description());
    }
}

static void printSymbolicFixups(const Image& image)
{
    printf("    -symbolic_fixups:\n");

    SymbolicatedImage symImage(image);
    uint64_t lastSymbolBaseAddr = 0;
    for (size_t i=0; i < symImage.fixupCount(); ++i) {
        CString  inSymbolName     = symImage.fixupInSymbol(i);
        uint64_t inSymbolAddress  = symImage.fixupAddress(i);
        uint32_t inSymbolOffset   = symImage.fixupInSymbolOffset(i);
        uint64_t inSymbolBaseAddr = inSymbolAddress - inSymbolOffset;
        if ( inSymbolBaseAddr != lastSymbolBaseAddr )
            printf("%s:\n", inSymbolName.c_str());
        char targetStr[4096];
        printf("           +0x%04X %11s  %s\n",           inSymbolOffset, symImage.fixupTypeString(i), symImage.fixupTargetString(i, true, targetStr));
        lastSymbolBaseAddr = inSymbolBaseAddr;
    }
}

static void printExports(const Image& image)
{
    printf("    -exports:\n");
    printf("        offset      symbol\n");
    if ( image.hasExportsTrie() ) {
        image.exportsTrie().forEachExportedSymbol(^(const Symbol& symbol, bool& stop) {
            uint64_t        resolverFuncOffset;
            uint64_t        absAddress;
            int             libOrdinal;
            uint32_t        fvtIndex;
            const char*     importName;
            const char*     symbolName = symbol.name().c_str();
            if ( symbol.isReExport(libOrdinal, importName) ) {
                char buffer[128];
                if ( strcmp(importName, symbolName) == 0 )
                    printf("        [re-export] %s (from %s)\n", symbolName, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
                else
                    printf("        [re-export] %s (%s from %s)\n", symbolName, importName, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
            }
            else if ( symbol.isAbsolute(absAddress) ) {
                printf("        0x%08llX  %s [absolute]\n", absAddress, symbolName);
            }
            else if ( symbol.isThreadLocal() ) {
                printf("        0x%08llX  %s [per-thread]\n", symbol.implOffset(), symbolName);
            }
            else if ( symbol.isFunctionVariant(fvtIndex) ) {
                printf("        0x%08llX  %s [function-variants-table#%d]\n", symbol.implOffset(), symbolName, fvtIndex);
            }
            else if ( symbol.isDynamicResolver(resolverFuncOffset) ) {
                printf("        0x%08llX  %s [dynamic-resolver=0x%08llX]\n", symbol.implOffset(), symbolName, resolverFuncOffset);
            }
            else if ( symbol.isWeakDef() ) {
                printf("        0x%08llX  %s [weak-def]\n", symbol.implOffset(), symbolName);
            }
            else {
                printf("        0x%08llX  %s\n", symbol.implOffset(), symbolName);
            }
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachExportedSymbol(^(const Symbol& symbol, uint32_t symIndex, bool& stop) {
            const char*     symbolName = symbol.name().c_str();
            uint64_t        absAddress;
            if ( symbol.isAbsolute(absAddress) ) {
                printf("        0x%08llX  %s [absolute]\n", absAddress, symbolName);
            }
            else if ( symbol.isWeakDef() ) {
                printf("        0x%08llX  %s [weak-def]\n", symbol.implOffset(), symbolName);
            }
            else {
                printf("        0x%08llX  %s\n", symbol.implOffset(), symbolName);
            }
        });
    }
    else {
        printf("no exported symbol information\n");
    }
}

static void printFixups(const Image& image)
{
    printf("    -fixups:\n");
    SymbolicatedImage symImage(image);
    printf("        segment         section          address             type   target\n");
    for (size_t i=0; i < symImage.fixupCount(); ++i) {
        char             targetStr[4096];
        uint8_t          sectNum  = symImage.fixupSectNum(i);
        std::string_view segName  = symImage.fixupSegment(sectNum);
        std::string_view sectName = symImage.fixupSection(sectNum);
        printf("        %-12.*s    %-16.*s 0x%08llX   %12s  %s\n",
               (int)segName.size(), segName.data(), 
               (int)sectName.size(), sectName.data(),
               symImage.fixupAddress(i), symImage.fixupTypeString(i), symImage.fixupTargetString(i, false, targetStr));
    }
    if ( image.hasFunctionVariantFixups() ) {
        image.functionVariantFixups().forEachFixup(^(FunctionVariantFixups::InternalFixup fixupInfo) {
            uint64_t address = image.segment(fixupInfo.segIndex).runtimeOffset + fixupInfo.segOffset + image.header()->preferredLoadAddress();
            __block uint32_t sectNum = 1;
            image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                if ( (sectInfo.address <= address) && (address < sectInfo.address+sectInfo.size) ) {
                    stop = true;
                    return;
                }
                ++sectNum;
            });
            const char* kindStr = "variant";
            const char* extras  = "";
            char        authInfoStr[128];
            if ( fixupInfo.pacAuth ) {
                kindStr = "auth-variant";
                snprintf(authInfoStr, sizeof(authInfoStr), "  (div=0x%04X ad=%d key=%s)", fixupInfo.pacDiversity, fixupInfo.pacAddress, mach_o::Fixup::keyName(fixupInfo.pacKey));
                extras = authInfoStr;
            }
            printf("        %-12s    %-16s 0x%08llX   %12s  table #%d %s\n",
                   image.segment(fixupInfo.segIndex).segName.data(), symImage.fixupSection(sectNum).data(),
                   address, kindStr, fixupInfo.variantIndex, extras);

        });
    }
}

static void printLoadCommands(const Image& image)
{
    printf("    -load_commands:\n");
    image.header()->printLoadCommands(stdout);
}


static void printObjC(const Image& image)
{
    printf("    -objc:\n");
    // build list of all fixups
    SymbolicatedImage symInfo(image);

    if ( symInfo.fairplayEncryptsSomeObjcStrings() )
        printf("        warning: FairPlay encryption of __TEXT will make printing ObjC info unreliable\n");

    symInfo.forEachDefinedObjCClass(^(uint64_t classVmAddr) {
        char protocols[1024];
        const char* classname = symInfo.className(classVmAddr);
        const char* supername = symInfo.superClassName(classVmAddr);
        symInfo.getClassProtocolNames(classVmAddr, protocols);
        printf("        @interface %s : %s %s\n", classname, supername, protocols);
        // walk instance methods
        symInfo.forEachMethodInClass(classVmAddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  -[%s %s]\n", implAddr, classname, methodName);
        });
        // walk class methods
        uint64_t metaClassVmaddr = symInfo.metaClassVmAddr(classVmAddr);
        symInfo.forEachMethodInClass(metaClassVmaddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
        });
        printf("        @end\n");
    });

    symInfo.forEachObjCCategory(^(uint64_t categoryVmAddr) {
        const char* catname   = symInfo.categoryName(categoryVmAddr);
        const char* classname = symInfo.categoryClassName(categoryVmAddr);
        printf("        @interface %s(%s)\n", classname, catname);
        symInfo.forEachMethodInCategory(categoryVmAddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  -[%s %s]\n", implAddr, classname, methodName);
        },
                                        ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
        });
        printf("        @end\n");
    });

    symInfo.forEachObjCProtocol(^(uint64_t protocolVmAddr) {
        char protocols[1024];
        const char* protocolname = symInfo.protocolName(protocolVmAddr);
        symInfo.getProtocolProtocolNames(protocolVmAddr, protocols);
        printf("        @protocol %s : %s\n", protocolname, protocols);
        symInfo.forEachMethodInProtocol(protocolVmAddr, ^(const char* methodName) {
            printf("          -[%s %s]\n", protocolname, methodName);
        },
                                        ^(const char* methodName) {
            printf("          +[%s %s]\n", protocolname, methodName);
        },
                                        ^(const char* methodName) {
            printf("          -[%s %s]\n", protocolname, methodName);
        },
                                        ^(const char* methodName) {
            printf("          +[%s %s]\n", protocolname, methodName);
        });
        printf("        @end\n");
    });
}



#if 0
static void printSwiftProtocolConformances(const dyld3::MachOAnalyzer* ma,
                                           const DyldSharedCache* dyldCache, size_t cacheLen)
{
    Diagnostics diag;

    __block std::vector<std::string> chainedFixupTargets;
    ma->forEachChainedFixupTarget(diag, ^(int libOrdinal, const char *symbolName, uint64_t addend, bool weakImport, bool &stop) {
        chainedFixupTargets.push_back(symbolName);
    });

    printf("    -swift-proto:\n");
    printf("        address             protocol-target     type-descriptor-target\n");

    uint64_t loadAddress = ma->preferredLoadAddress();

    uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
    __block metadata_visitor::SwiftVisitor swiftVisitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
    swiftVisitor.forEachProtocolConformance(^(const metadata_visitor::SwiftConformance &swiftConformance, bool &stopConformance) {
        VMAddress protocolConformanceVMAddr = swiftConformance.getVMAddress();
        metadata_visitor::SwiftPointer protocolPtr = swiftConformance.getProtocolPointer(swiftVisitor);
        metadata_visitor::SwiftConformance::SwiftTypeRefPointer typeRef = swiftConformance.getTypeRef(swiftVisitor);
        metadata_visitor::SwiftPointer typePtr = typeRef.getTargetPointer(swiftVisitor);
        const char* protocolConformanceFixup = "";
        const char* protocolFixup = "";
        const char* typeDescriptorFixup = "";

        uint64_t protocolRuntimeOffset = protocolPtr.targetValue.vmAddress().rawValue() - loadAddress;
        uint64_t typeRefRuntimeOffset = typePtr.targetValue.vmAddress().rawValue() - loadAddress;

        // If we have indirect fixups, see if we can find the names
        if ( !protocolPtr.isDirect ) {
            uint8_t* fixup = (uint8_t*)ma + protocolRuntimeOffset;
            const auto* fixupLoc = (const ChainedFixupPointerOnDisk*)fixup;
            uint32_t bindOrdinal = 0;
            int64_t addend = 0;
            if ( fixupLoc->isBind(DYLD_CHAINED_PTR_ARM64E_USERLAND, bindOrdinal, addend) ) {
                protocolFixup = chainedFixupTargets[bindOrdinal].c_str();
            }
        }
        if ( !typePtr.isDirect ) {
            uint8_t* fixup = (uint8_t*)ma + typeRefRuntimeOffset;
            const auto* fixupLoc = (const ChainedFixupPointerOnDisk*)fixup;
            uint32_t bindOrdinal = 0;
            int64_t addend = 0;
            if ( fixupLoc->isBind(DYLD_CHAINED_PTR_ARM64E_USERLAND, bindOrdinal, addend) ) {
                protocolFixup = chainedFixupTargets[bindOrdinal].c_str();
            }
        }
        printf("        0x%016llX(%s)  %s0x%016llX(%s)  %s0x%016llX(%s)\n",
               protocolConformanceVMAddr.rawValue(), protocolConformanceFixup,
               protocolPtr.isDirect ? "" : "*", protocolRuntimeOffset, protocolFixup,
               typePtr.isDirect ? "" : "*", typeRefRuntimeOffset, typeDescriptorFixup);
    });
}
#endif

static void printSharedRegion(const Image& image)
{
    printf("    -shared_region:\n");

    if ( !image.hasSplitSegInfo() ) {
        printf("        no shared region info\n");
        return;
    }

    const SplitSegInfo& splitSeg = image.splitSegInfo();
    if ( splitSeg.isV1() ) {
        printf("        shared region v1\n");
        return;
    }

    if ( splitSeg.hasMarker() ) {
        printf("        no shared region info (marker present)\n");
        return;
    }

    __block std::vector<std::pair<std::string, std::string>> sectionNames;
    __block std::vector<uint64_t>                            sectionVMAddrs;
    sectionNames.emplace_back("","");
    sectionVMAddrs.push_back(0);
    image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        sectionNames.emplace_back(sectInfo.segmentName, sectInfo.sectionName);
        sectionVMAddrs.push_back(sectInfo.address);
    });
    printf("        from      to\n");
    Error err = splitSeg.forEachReferenceV2(^(const SplitSegInfo::Entry& entry, bool& stop) {
        std::string_view fromSegmentName    = sectionNames[(uint32_t)entry.fromSectionIndex].first;
        std::string_view fromSectionName    = sectionNames[(uint32_t)entry.fromSectionIndex].second;
        std::string_view toSegmentName      = sectionNames[(uint32_t)entry.toSectionIndex].first;
        std::string_view toSectionName      = sectionNames[(uint32_t)entry.toSectionIndex].second;
        uint64_t fromVMAddr                 = sectionVMAddrs[(uint32_t)entry.fromSectionIndex] + entry.fromSectionOffset;
        uint64_t toVMAddr                   = sectionVMAddrs[(uint32_t)entry.toSectionIndex]   + entry.toSectionOffset;
        printf("        %-16s %-16s 0x%08llx      %-16s %-16s 0x%08llx\n",
               fromSegmentName.data(), fromSectionName.data(), fromVMAddr,
               toSegmentName.data(), toSectionName.data(), toVMAddr);
    });
}

static void printFunctionStarts(const Image& image)
{
    printf("    -function_starts:\n");
    SymbolicatedImage symImage(image);
    if ( image.hasFunctionStarts() ) {
        uint64_t loadAddress = image.header()->preferredLoadAddress();
        image.functionStarts().forEachFunctionStart(loadAddress, ^(uint64_t addr) {
            const char* name = symImage.symbolNameAt(addr);
            if ( name == nullptr )
                name = "";
            printf("        0x%08llX  %s\n", addr, name);
        });
    }
    else {
        printf("        no function starts info\n");
    }
}

static void printOpcodes(const Image& image)
{
    printf("    -opcodes:\n");
    if ( image.hasRebaseOpcodes() ) {
        printf("        rebase opcodes:\n");
        image.rebaseOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no rebase opcodes\n");
    }
    if ( image.hasBindOpcodes() ) {
        printf("        bind opcodes:\n");
        image.bindOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no bind opcodes\n");
    }
    if ( image.hasLazyBindOpcodes() ) {
        printf("        lazy bind opcodes:\n");
        image.lazyBindOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no lazy bind opcodes\n");
    }
    // FIXME: add support for weak binds
}

static void printUnwindTable(const Image& image)
{
    printf("    -unwind:\n");
    if ( image.hasCompactUnwind() ) {
        printf("        address       encoding\n");
        uint64_t loadAddress = image.header()->preferredLoadAddress();
        const CompactUnwind& cu = image.compactUnwind();
        cu.forEachUnwindInfo(^(const CompactUnwind::UnwindInfo& info) {
            const void* funcBytes = (uint8_t*)image.header() + info.funcOffset;
            char encodingString[128];
            cu.encodingToString(info.encoding, funcBytes, encodingString);
            char lsdaString[32];
            lsdaString[0] = '\0';
            if ( info.lsdaOffset != 0 )
                snprintf(lsdaString, sizeof(lsdaString), " lsdaOffset=0x%08X", info.lsdaOffset);
            printf("        0x%08llX   0x%08X (%-56s)%s\n", info.funcOffset + loadAddress, info.encoding, encodingString, lsdaString);
        });
    }
    else {
        printf("        no compact unwind table\n");
    }
}

static void dumpHex(SymbolicatedImage& symImage, const Header::SectionInfo& sectInfo, size_t sectNum)
{
    const uint8_t*   sectionContent = symImage.content(sectInfo);
    const uint8_t*   bias           = sectionContent - (long)sectInfo.address;
    uint8_t          sectType       = (sectInfo.flags & SECTION_TYPE);
    bool             isZeroFill     = ((sectType == S_ZEROFILL) || (sectType == S_THREAD_LOCAL_ZEROFILL));
    symImage.forEachSymbolRangeInSection(sectNum, ^(const char* symbolName, uint64_t symbolAddr, uint64_t size) {
        if ( symbolName != nullptr ) {
            if ( (symbolAddr == sectInfo.address) && (strchr(symbolName, ',') != nullptr) ) {
                // don't print synthesized name for section start (e.g. "__DATA_CONST,__auth_ptr")
            }
            else {
                printf("%s:\n", symbolName);
            }
        }
        for (int i=0; i < size; ++i) {
            if ( (i & 0xF) == 0 )
                printf("0x%08llX: ", symbolAddr+i);
            uint8_t byte = (isZeroFill ? 0 : bias[symbolAddr + i]);
            printf("%02X ", byte);
            if ( (i & 0xF) == 0xF )
                printf("\n");
        }
        if ( (size & 0xF) != 0x0 )
            printf("\n");
    });
}

static void disassembleSection(SymbolicatedImage& symImage, const Header::SectionInfo& sectInfo, size_t sectNum)
{
#if HAVE_LIBLTO
    symImage.loadDisassembler();
    if ( symImage.llvmRef() != nullptr ) {
        // disassemble content
        const uint8_t* sectionContent    = symImage.content(sectInfo);
        const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
        const uint8_t* curContent = sectionContent;
        uint64_t       curPC      = sectInfo.address;
        symImage.setSectionContentBias(sectionContent - sectInfo.address);
        while ( curContent < sectionContentEnd ) {
            // add label if there is one for this PC
            if ( const char* symName = symImage.symbolNameAt(curPC) )
                printf("%s:\n", symName);
            char line[256];
            size_t len = LLVMDisasmInstruction(symImage.llvmRef(), (uint8_t*)curContent, sectInfo.size, curPC, line, sizeof(line));
            // llvm disassembler uses tabs to align operands, but that can look wonky, so convert to aligned spaces
            char instruction[16];
            char operands[256];
            char comment[256];
            comment[0] = '\0';
            if ( len == 0 ) {
                uint32_t value32;
                strcpy(instruction, ".long");
                memcpy(&value32, curContent, 4);
                snprintf(operands, sizeof(operands), "0x%08X", value32);
                //printf("0x%08llX \t.long\t0x%08X\n", curPC, value32);
                len = 4;
            }
            else {
                // parse: "\tinstr\toperands"
                if ( char* secondTab = strchr(&line[1],'\t') ) {
                    size_t instrLen = secondTab - &line[1];
                    if ( instrLen < sizeof(instruction) ) {
                        memcpy(instruction, &line[1], instrLen);
                        instruction[instrLen] = '\0';
                    }
                    // llvm diassembler addens wonky comments like "literal pool symbol address", improve wording
                    strlcpy(operands, &line[instrLen+2], sizeof(operands));
                    if ( char* literalComment = strstr(operands, "; literal pool symbol address: ") ) {
                        strlcpy(comment, "; ", sizeof(comment));
                        strlcat(comment, literalComment+31,sizeof(comment));
                        literalComment[0] = '\0'; // truncate operands
                    }
                    else if ( char* literalComment2 = strstr(operands, "## literal pool symbol address: ") ) {
                        strlcpy(comment, "; ", sizeof(comment));
                        strlcat(comment, literalComment2+32,sizeof(comment));
                        literalComment2[0] = '\0'; // truncate operands
                    }
                    else if ( char* literalComment3 = strstr(operands, "## literal pool for: ") ) {
                        strlcpy(comment, "; string literal: ", sizeof(comment));
                        strlcat(comment, literalComment3+21,sizeof(comment));
                        literalComment3[0] = '\0'; // truncate operands
                    }
                    else if ( char* numberComment = strstr(operands, "; 0x") ) {
                        strlcpy(comment, numberComment, sizeof(comment));
                        numberComment[0] = '\0';  // truncate operands
                    }
                }
                else {
                    strlcpy(instruction, &line[1], sizeof(instruction));
                    operands[0] = '\0';
                }
                printf("0x%09llX   %-8s %-20s %s\n", curPC, instruction, operands, comment);
            }
            curContent += len;
            curPC      += len;
        }
        return;
    }
#endif
    // disassembler not available, dump code in hex
    dumpHex(symImage, sectInfo, sectNum);
}

static void printQuotedString(const char* str)
{
    if ( (strchr(str, '\n') != nullptr) || (strchr(str, '\t') != nullptr) ) {
        printf("\"");
        for (const char* s=str; *s != '\0'; ++s) {
            if ( *s == '\n' )
                printf("\\n");
            else if ( *s == '\t' )
                printf("\\t");
            else
                printf("%c", *s);
        }
        printf("\"");
    }
    else {
        printf("\"%s\"", str);
    }
}

static void dumpCStrings(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const char* sectionContent  = (char*)symInfo.content(sectInfo);
    const char* stringStart     = sectionContent;
    for (int i=0; i < sectInfo.size; ++i) {
        if ( sectionContent[i] == '\0' ) {
            if ( *stringStart != '\0' ) {
                printf("0x%08llX ", sectInfo.address + i);
                printQuotedString(stringStart);
                printf("\n");
            }
            stringStart = &sectionContent[i+1];
        }
    }
}

static void dumpCFStrings(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const size_t   cfStringSize      = symInfo.is64() ? 32 : 16;
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        printf("0x%08llX\n", curAddr);
        const Fixup::BindTarget* bindTarget;
        if ( symInfo.isBind(sectionContent, bindTarget) ) {
            printf("    class: %s\n", bindTarget->symbolName.c_str());
            printf("    flags: 0x%08X\n", *((uint32_t*)(&curContent[cfStringSize/4])));
            uint64_t stringVmAddr;
            if ( symInfo.isRebase(&curContent[cfStringSize/2], stringVmAddr) ) {
                if ( const char* str = symInfo.cStringAt(stringVmAddr) ) {
                    printf("   string: ");
                    printQuotedString(str);
                    printf("\n");
                }
            }
            printf("   length: %u\n", *((uint32_t*)(&curContent[3*cfStringSize/4])));
        }
        curContent += cfStringSize;
        curAddr    += cfStringSize;
    }
}

static void dumpGOT(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        const Fixup::BindTarget* bindTarget;
        uint64_t                 rebaseTargetVmAddr;
        printf("0x%08llX  ", curAddr);
        if ( symInfo.isBind(curContent, bindTarget) ) {
            printf("%s\n", bindTarget->symbolName.c_str());
        }
        else if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            const char* targetName = symInfo.symbolNameAt(rebaseTargetVmAddr);
            if ( targetName != nullptr )
                printf("%s\n", targetName);
            else
                printf("0x%08llX\n", rebaseTargetVmAddr);
        }
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}

static void dumpClassPointers(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        uint64_t   rebaseTargetVmAddr;
        if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            printf("0x%08llX:  0x%08llX ", curAddr, rebaseTargetVmAddr);
            if ( const char* targetName = symInfo.symbolNameAt(rebaseTargetVmAddr) )
                printf("%s", targetName);
            printf("\n");
        }
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}

static void dumpStringPointers(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        uint64_t   rebaseTargetVmAddr;
        printf("0x%08llX  ", curAddr);
        if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            if ( const char* selector = symInfo.cStringAt(rebaseTargetVmAddr) )
                printQuotedString(selector);
        }
        printf("\n");
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}


struct NameAndFlagBitNum { CString name; uint32_t flagBitNum; };

#define FUNCTION_VARIANT_SYSTEM_WIDE(_flagBitNum, _name, _flagBitsInitialization) \
    { _name, _flagBitNum },
static const NameAndFlagBitNum sSystemWideFVNamesAndFlagBitNums[] = {
    #include "FunctionVariantsSystemWide.inc"
};

#define FUNCTION_VARIANT_PER_PROCESS(_flagBitNum, _name, _flagBitsInitialization) \
    { _name, _flagBitNum },
static const NameAndFlagBitNum sPerProcessFVNamesAndFlagBitNums[] = {
    #include "FunctionVariantsPerProcess.inc"
};

#define FUNCTION_VARIANT_ARM64(_flagBitNum, _name, _flagBitsInitialization) \
    { _name, _flagBitNum },
static const NameAndFlagBitNum sArm64FVNamesAndFlagBitNums[] = {
    #include "FunctionVariantsArm64.inc"
};

#define FUNCTION_VARIANT_X86_64(_flagBitNum, _name, _flagBitsInitialization) \
    { _name, _flagBitNum },
static const NameAndFlagBitNum sIntelFVNamesAndFlagBitNums[] = {
    #include "FunctionVariantsX86_64.inc"
};


static CString findName(std::span<const NameAndFlagBitNum> nameAndKeys, uint8_t flagBitNum)
{
    for (const NameAndFlagBitNum& entry : nameAndKeys) {
        if ( entry.flagBitNum == flagBitNum )
            return entry.name;
    }
    return "???";
}

static void dumpFunctionVariantTables(const SymbolicatedImage& symInfo, const FunctionVariants& allTables)
{
    printf("    -function_variants:\n");
    for (uint32_t i=0; i < allTables.count(); ++i) {
        printf("      table #%u\n", i);
        const FunctionVariantsRuntimeTable*        table = allTables.entry(i);
        std::span<const NameAndFlagBitNum> nameTable;
        switch ( table->kind ) {
            case FunctionVariantsRuntimeTable::Kind::perProcess:
                printf("        namespace: per-process\n");
                nameTable = std::span<const NameAndFlagBitNum>(sPerProcessFVNamesAndFlagBitNums, sizeof(sPerProcessFVNamesAndFlagBitNums)/sizeof(NameAndFlagBitNum));
                break;
            case FunctionVariantsRuntimeTable::Kind::systemWide:
                printf("        namespace: system-wide\n");
                nameTable = std::span<const NameAndFlagBitNum>(sSystemWideFVNamesAndFlagBitNums, sizeof(sSystemWideFVNamesAndFlagBitNums)/sizeof(NameAndFlagBitNum));
                break;
            case FunctionVariantsRuntimeTable::Kind::arm64:
                printf("        namespace: arm64\n");
                nameTable = std::span<const NameAndFlagBitNum>(sArm64FVNamesAndFlagBitNums, sizeof(sArm64FVNamesAndFlagBitNums)/sizeof(NameAndFlagBitNum));
                break;
            case FunctionVariantsRuntimeTable::Kind::x86_64:
                printf("        namespace: x86_64\n");
                nameTable = std::span<const NameAndFlagBitNum>(sIntelFVNamesAndFlagBitNums, sizeof(sIntelFVNamesAndFlagBitNums)/sizeof(NameAndFlagBitNum));
                break;
            default:
                printf("      namespace: unknown (%d)\n", table->kind);
                return;
        }
        __block size_t longestNameLength = 0;
        table->forEachVariant(^(FunctionVariantsRuntimeTable::Kind kind, uint32_t implOffset, bool implIsTable, std::span<const uint8_t> flagBitNums, bool& stop) {
            const char* name = symInfo.symbolNameAt(symInfo.prefLoadAddress() + implOffset);
            if ( name == NULL )
                name = "???";
            size_t len = strlen(name);
            if ( len > longestNameLength )
                longestNameLength = len;
        });
        table->forEachVariant(^(FunctionVariantsRuntimeTable::Kind kind, uint32_t implOffset, bool implIsTable, std::span<const uint8_t> flagBitNums, bool& stop) {
            if ( implIsTable ) {
                printf("            table: #%d", implOffset);
                printf("%*s", (int)(longestNameLength+14), "-->");
            }
            else {
                const char* name = symInfo.symbolNameAt(symInfo.prefLoadAddress() + implOffset);
                if ( name == NULL )
                    name = "???";
                printf("         function: 0x%08X %s ", implOffset, name);
                printf("%*s", (int)(longestNameLength-strlen(name)+4), "-->");
            }
            if ( flagBitNums.size() == 0 ) {
                printf("  0x00 (\"default\")\n");
            }
            else if ( flagBitNums.size() == 1 ) {
                printf("  0x%02X (\"%s\")\n", flagBitNums[0], findName(nameTable, flagBitNums[0]).c_str());
            }
            else {
                printf("  ");
                for (uint8_t flag : flagBitNums)
                    printf("0x%02X ", flag);
                printf("(");
                for (uint8_t flag : flagBitNums)
                    printf("\"%s\" ", findName(nameTable, flag).c_str());
                printf(")\n");
            }
        });
    }
}

static void printFunctionVariants(const Image& image)
{
    if ( image.hasFunctionVariants() ) {
        SymbolicatedImage symImage(image);
        dumpFunctionVariantTables(symImage, image.functionVariants());
    }
}

static void printDisassembly(const Image& image)
{
    __block SymbolicatedImage symImage(image);
    __block size_t sectNum = 1;
    image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.flags & (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS) ) {
            printf("(%.*s,%.*s) section:\n", 
                   (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                   (int)sectInfo.sectionName.size(), sectInfo.sectionName.data());
            disassembleSection(symImage, sectInfo, sectNum);
        }
        ++sectNum;
    });
}

static void usage()
{
    fprintf(stderr, "Usage: dyld_info [-arch <arch>]* <options>* <mach-o file>+ | -all_dir <dir> \n"
            "\t-platform                   print platform (default if no options specified)\n"
            "\t-segments                   print segments (default if no options specified)\n"
            "\t-linked_dylibs              print all dylibs this image links against (default if no options specified)\n"
            "\t-inits                      print initializers\n"
            "\t-fixups                     print locations dyld will rebase/bind\n"
            "\t-exports                    print all exported symbols\n"
            "\t-imports                    print all symbols needed from other dylibs\n"
            "\t-fixup_chains               print info about chain format and starts\n"
            "\t-fixup_chain_details        print detailed info about every fixup in chain\n"
            "\t-fixup_chain_header         print detailed info about the fixup chains header\n"
            "\t-symbolic_fixups            print ranges of each atom of DATA with symbol name and fixups\n"
          //"\t-swift_protocols            print swift protocols\n"
            "\t-objc                       print objc classes, categories, etc\n"
            "\t-shared_region              print shared cache (split seg) info\n"
            "\t-function_starts            print function starts information\n"
            "\t-opcodes                    print opcodes information\n"
            "\t-load_commands              print load commands\n"
            "\t-uuid                       print UUID of binary\n"
            "\t-function_variants          print info on function variants in binary\n"
            "\t-disassemble                print all code sections using disassembler\n"
            "\t-section <seg> <sect>       print content of section, formatted by section type\n"
            "\t-all_sections               print content of all sections, formatted by section type\n"
            "\t-section_bytes <seg> <sect> print content of section, as raw hex bytes\n"
            "\t-all_sections_bytes         print content of all sections, formatted as raw hex bytes\n"
            "\t-validate_only              only prints an malformedness about file(s)\n"
            "\t-no_validate                don't check for malformedness about file(s)\n"
        );
}

struct SegSect { std::string_view segmentName; std::string_view sectionName; };
typedef std::vector<SegSect> SegSectVector;

static bool hasSegSect(const SegSectVector& vec, const Header::SectionInfo& sectInfo) {
    for (const SegSect& ss : vec) {
        if ( (ss.segmentName == sectInfo.segmentName) && (ss.sectionName == sectInfo.sectionName) )
            return true;
    }
    return false;
}


struct PrintOptions
{
    bool            platform            = false;
    bool            segments            = false;
    bool            linkedDylibs        = false;
    bool            initializers        = false;
    bool            exports             = false;
    bool            imports             = false;
    bool            fixups              = false;
    bool            fixupChains         = false;
    bool            fixupChainDetails   = false;
    bool            fixupChainHeader    = false;
    bool            symbolicFixups      = false;
    bool            objc                = false;
    bool            swiftProtocols      = false;
    bool            sharedRegion        = false;
    bool            functionStarts      = false;
    bool            opcodes             = false;
    bool            unwind              = false;
    bool            uuid                = false;
    bool            loadCommands        = false;
    bool            functionVariants    = false;
    bool            disassemble         = false;
    bool            allSections         = false;
    bool            allSectionsHex      = false;
    bool            validateOnly        = false;
    bool            validate            = true;
    SegSectVector   sections;
    SegSectVector   sectionsHex;
};


int main(int argc, const char* argv[])
{
    if ( argc == 1 ) {
        usage();
        return 0;
    }

    bool                             someOptionSpecified = false;
    const char*                      dyldCachePath = nullptr;
    bool                             allDyldCache = false;
    PrintOptions                     printOptions;
    __block std::vector<const char*> files;
            std::vector<const char*> cmdLineArchs;
    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-platform") == 0 ) {
            printOptions.platform = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-segments") == 0 ) {
            printOptions.segments = true;
            someOptionSpecified = true;
        }
        else if ( (strcmp(arg, "-linked_dylibs") == 0) || (strcmp(arg, "-dependents") == 0) ) {
            printOptions.linkedDylibs = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-inits") == 0 ) {
            printOptions.initializers = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixups") == 0 ) {
            printOptions.fixups = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chains") == 0 ) {
            printOptions.fixupChains = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chain_details") == 0 ) {
            printOptions.fixupChainDetails = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chain_header") == 0 ) {
            printOptions.fixupChainHeader = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-symbolic_fixups") == 0 ) {
            printOptions.symbolicFixups = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-exports") == 0 ) {
            printOptions.exports = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-imports") == 0 ) {
            printOptions.imports = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-objc") == 0 ) {
            printOptions.objc = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-swift_protocols") == 0 ) {
            printOptions.swiftProtocols = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-shared_region") == 0 ) {
            printOptions.sharedRegion = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-function_starts") == 0 ) {
            printOptions.functionStarts = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-opcodes") == 0 ) {
            printOptions.opcodes = true;
        }
        else if ( strcmp(arg, "-unwind") == 0 ) {
            printOptions.unwind = true;
        }
        else if ( strcmp(arg, "-uuid") == 0 ) {
            printOptions.uuid = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-load_commands") == 0 ) {
            printOptions.loadCommands = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-disassemble") == 0 ) {
            printOptions.disassemble = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-section") == 0 ) {
            const char* segName  = argv[++i];
            const char* sectName = argv[++i];
            if ( !segName || !sectName ) {
                fprintf(stderr, "-section requires segment-name and section-name");
                return 1;
            }
            printOptions.sections.push_back({segName, sectName});
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-all_sections") == 0 ) {
            printOptions.allSections = true;
        }
        else if ( strcmp(arg, "-section_bytes") == 0 ) {
            const char* segName  = argv[++i];
            const char* sectName = argv[++i];
            if ( !segName || !sectName ) {
                fprintf(stderr, "-section_bytes requires segment-name and section-name");
                return 1;
            }
            printOptions.sectionsHex.push_back({segName, sectName});
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-all_sections_bytes") == 0 ) {
            printOptions.allSectionsHex = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-function_variants") == 0 ) {
            printOptions.functionVariants = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-validate_only") == 0 ) {
            printOptions.validateOnly = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-no_validate") == 0 ) {
            printOptions.validate = false;
        }
        else if ( strcmp(arg, "-arch") == 0 ) {
            if ( ++i < argc ) {
                cmdLineArchs.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-arch missing architecture name");
                return 1;
            }
        }
        else if ( strcmp(arg, "-all_dir") == 0 ) {
            if ( ++i < argc ) {
                const char* searchDir = argv[i];
                iterateDirectoryTree("", searchDir, ^(const std::string& dirPath) { return false; },
                                     ^(const std::string& path, const struct stat& statBuf) { if ( statBuf.st_size > 4096 ) files.push_back(strdup(path.c_str())); },
                                     true /* process files */, true /* recurse */);

            }
            else {
                fprintf(stderr, "-all_dir directory");
                return 1;
            }
        }
        else if ( strcmp(arg, "-dyld_cache_path") == 0 ) {
            if ( ++i < argc ) {
                dyldCachePath = argv[i];
            }
            else {
                fprintf(stderr, "-dyld_cache_path path");
                return 1;
            }

        }
        else if ( strcmp(arg, "-all_dyld_cache") == 0 ) {
            allDyldCache = true;
        }
        else if ( arg[0] == '-' ) {
            fprintf(stderr, "dyld_info: unknown option: %s\n", arg);
            return 1;
        }
        else {
            files.push_back(arg);
        }
    }

    const DyldSharedCache* dyldCache = nullptr;
    if ( dyldCachePath ) {
        std::vector<const DyldSharedCache*> dyldCaches = DyldSharedCache::mapCacheFiles(dyldCachePath);
        if ( dyldCaches.empty() ) {
            fprintf(stderr, "dyld_info: can't map shared cache at %s\n", dyldCachePath);
            return 1;
        }
        dyldCache = dyldCaches.front();
    } else {
        size_t cacheLen;
        dyldCache = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLen);
    }

    if ( allDyldCache ) {
        if ( dyldCache ) {
            dyldCache->forEachImage(^(const Header* hdr, const char* installName) {
                files.push_back(installName);
            });
        } else {
            fprintf(stderr, "dyld_info: -all_dyld_cache specified but shared cache isn't loaded");
            return 1;
        }
    }


    // check some files specified
    if ( files.size() == 0 ) {
        usage();
        return 0;
    }

    // if no options specified, use default set
    if ( !someOptionSpecified ) {
        printOptions.platform     = true;
        printOptions.uuid         = true;
        printOptions.segments     = true;
        printOptions.linkedDylibs = true;
    }

    __block bool sliceFound = false;
    other_tools::forSelectedSliceInPaths(files, cmdLineArchs, dyldCache, ^(const char* path, const Header* header, size_t sliceLen) {
        if ( header == nullptr )
            return; // non-mach-o file found
        sliceFound = true;
        printf("%s [%s]:\n", path, header->archName());
        if ( header->isObjectFile() )
            return;
        Image image((void*)header, sliceLen, (header->inDyldCache() ? Image::MappingKind::dyldLoadedPostFixups : Image::MappingKind::wholeSliceMapped));
        if ( printOptions.validate ) {
            if ( Error err = image.validate() ) {
                printf("   %s\n", err.message());
                return;
            }
        }
        if ( !printOptions.validateOnly ) {
            if ( printOptions.platform )
                printPlatforms(image.header());

            if ( printOptions.uuid )
                printUUID(image.header());

            if ( printOptions.segments )
                 printSegments(image.header());

            if ( printOptions.linkedDylibs )
                printLinkedDylibs(image.header());

            if ( printOptions.initializers )
                printInitializers(image);

            if ( printOptions.exports )
                printExports(image);

            if ( printOptions.imports )
                printImports(image);

            if ( printOptions.fixups )
                printFixups(image);

            if ( printOptions.fixupChains )
                printChainInfo(image);

            if ( printOptions.fixupChainDetails )
                printChainDetails(image);

            if ( printOptions.fixupChainHeader )
                printChainHeader(image);

            if ( printOptions.symbolicFixups )
                printSymbolicFixups(image);

            if ( printOptions.opcodes )
                printOpcodes(image);

            if ( printOptions.functionStarts )
                printFunctionStarts(image);

            if ( printOptions.unwind )
                printUnwindTable(image);

            if ( printOptions.objc )
                printObjC(image);

            // FIXME: implement or remove
            //if ( printOptions.swiftProtocols )
            //    printSwiftProtocolConformances(ma, dyldCache, cacheLen);

            if ( printOptions.loadCommands )
                printLoadCommands(image);

            if ( printOptions.sharedRegion )
                printSharedRegion(image);

            if ( printOptions.functionVariants )
                printFunctionVariants(image);

            if ( printOptions.disassemble )
                printDisassembly(image);

            if ( printOptions.allSections || !printOptions.sections.empty() ) {
                __block SymbolicatedImage symImage(image);
                __block size_t sectNum = 1;
                image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                    if ( printOptions.allSections || hasSegSect(printOptions.sections, sectInfo) ) {
                        printf("(%.*s,%.*s) section:\n", 
                               (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                               (int)sectInfo.sectionName.size(), sectInfo.sectionName.data());
                        if ( sectInfo.flags & (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS) ) {
                            disassembleSection(symImage, sectInfo, sectNum);
                        }
                        else if ( (sectInfo.flags & SECTION_TYPE) == S_CSTRING_LITERALS ) {
                            dumpCStrings(symImage, sectInfo);
                        }
                        else if ( (sectInfo.flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS ) {
                            dumpGOT(image, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__cfstring") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpCFStrings(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_classrefs") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpGOT(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_classlist") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpClassPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_catlist") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpClassPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_selrefs") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpStringPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__info_plist") && sectInfo.segmentName.starts_with("__TEXT") ) {
                            dumpCStrings(image, sectInfo);
                        }
                        // FIXME: other section types
                        else {
                            dumpHex(symImage, sectInfo, sectNum);
                        }
                    }
                    ++sectNum;
                });
            }

            if ( printOptions.allSectionsHex || !printOptions.sectionsHex.empty() ) {
                __block SymbolicatedImage symImage(image);
                __block size_t sectNum = 1;
                image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                    if ( printOptions.allSectionsHex || hasSegSect(printOptions.sectionsHex, sectInfo) ) {
                        printf("(%.*s,%.*s) section:\n", 
                               (int)sectInfo.segmentName.size(), sectInfo.segmentName.data(),
                               (int)sectInfo.sectionName.size(), sectInfo.sectionName.data());
                        dumpHex(symImage, sectInfo, sectNum);
                    }
                    ++sectNum;
                });
            }
        }
    });

    if ( !sliceFound && (files.size() == 1) ) {
        if ( cmdLineArchs.empty() ) {
            fprintf(stderr, "dyld_info: '%s' file not found\n", files[0]);
            // FIXME: projects compatibility (rdar://121555064)
            if ( printOptions.linkedDylibs )
                return 0;
        }
        else
            fprintf(stderr, "dyld_info: '%s' does not contain specified arch(s)\n", files[0]);
        return 1;
    }

    return 0;
}




