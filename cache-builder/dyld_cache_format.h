/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2006-2015 Apple Inc. All rights reserved.
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
#ifndef __DYLD_CACHE_FORMAT__
#define __DYLD_CACHE_FORMAT__

#include <stdint.h>

#include <mach-o/fixup-chains.h>


struct dyld_cache_header
{
    char        magic[16];              // e.g. "dyld_v0    i386"
    uint32_t    mappingOffset;          // file offset to first dyld_cache_mapping_info
    uint32_t    mappingCount;           // number of dyld_cache_mapping_info entries
    uint32_t    imagesOffsetOld;        // UNUSED: moved to imagesOffset to prevent older dsc_extarctors from crashing
    uint32_t    imagesCountOld;         // UNUSED: moved to imagesCount to prevent older dsc_extarctors from crashing
    uint64_t    dyldBaseAddress;        // base address of dyld when cache was built
    uint64_t    codeSignatureOffset;    // file offset of code signature blob
    uint64_t    codeSignatureSize;      // size of code signature blob (zero means to end of file)
    uint64_t    slideInfoOffsetUnused;  // unused.  Used to be file offset of kernel slid info
    uint64_t    slideInfoSizeUnused;    // unused.  Used to be size of kernel slid info
    uint64_t    localSymbolsOffset;     // file offset of where local symbols are stored
    uint64_t    localSymbolsSize;       // size of local symbols information
    uint8_t     uuid[16];               // unique value for each shared cache file
    uint64_t    cacheType;              // 0 for development, 1 for production, 2 for multi-cache
    uint32_t    branchPoolsOffset;      // file offset to table of uint64_t pool addresses
    uint32_t    branchPoolsCount;       // number of uint64_t entries
    uint64_t    dyldInCacheMH;          // (unslid) address of mach_header of dyld in cache
    uint64_t    dyldInCacheEntry;       // (unslid) address of entry point (_dyld_start) of dyld in cache
    uint64_t    imagesTextOffset;       // file offset to first dyld_cache_image_text_info
    uint64_t    imagesTextCount;        // number of dyld_cache_image_text_info entries
    uint64_t    patchInfoAddr;          // (unslid) address of dyld_cache_patch_info
    uint64_t    patchInfoSize;          // Size of all of the patch information pointed to via the dyld_cache_patch_info
    uint64_t    otherImageGroupAddrUnused;    // unused
    uint64_t    otherImageGroupSizeUnused;    // unused
    uint64_t    progClosuresAddr;       // (unslid) address of list of program launch closures
    uint64_t    progClosuresSize;       // size of list of program launch closures
    uint64_t    progClosuresTrieAddr;   // (unslid) address of trie of indexes into program launch closures
    uint64_t    progClosuresTrieSize;   // size of trie of indexes into program launch closures
    uint32_t    platform;               // platform number (macOS=1, etc)
    uint32_t    formatVersion          : 8,  // dyld3::closure::kFormatVersion
                dylibsExpectedOnDisk   : 1,  // dyld should expect the dylib exists on disk and to compare inode/mtime to see if cache is valid
                simulator              : 1,  // for simulator of specified platform
                locallyBuiltCache      : 1,  // 0 for B&I built cache, 1 for locally built cache
                builtFromChainedFixups : 1,  // some dylib in cache was built using chained fixups, so patch tables must be used for overrides
                padding                : 20; // TBD
    uint64_t    sharedRegionStart;      // base load address of cache if not slid
    uint64_t    sharedRegionSize;       // overall size required to map the cache and all subCaches, if any
    uint64_t    maxSlide;               // runtime slide of cache can be between zero and this value
    uint64_t    dylibsImageArrayAddr;   // (unslid) address of ImageArray for dylibs in this cache
    uint64_t    dylibsImageArraySize;   // size of ImageArray for dylibs in this cache
    uint64_t    dylibsTrieAddr;         // (unslid) address of trie of indexes of all cached dylibs
    uint64_t    dylibsTrieSize;         // size of trie of cached dylib paths
    uint64_t    otherImageArrayAddr;    // (unslid) address of ImageArray for dylibs and bundles with dlopen closures
    uint64_t    otherImageArraySize;    // size of ImageArray for dylibs and bundles with dlopen closures
    uint64_t    otherTrieAddr;          // (unslid) address of trie of indexes of all dylibs and bundles with dlopen closures
    uint64_t    otherTrieSize;          // size of trie of dylibs and bundles with dlopen closures
    uint32_t    mappingWithSlideOffset; // file offset to first dyld_cache_mapping_and_slide_info
    uint32_t    mappingWithSlideCount;  // number of dyld_cache_mapping_and_slide_info entries
    uint64_t    dylibsPBLStateArrayAddrUnused;    // unused
    uint64_t    dylibsPBLSetAddr;           // (unslid) address of PrebuiltLoaderSet of all cached dylibs
    uint64_t    programsPBLSetPoolAddr;     // (unslid) address of pool of PrebuiltLoaderSet for each program 
    uint64_t    programsPBLSetPoolSize;     // size of pool of PrebuiltLoaderSet for each program
    uint64_t    programTrieAddr;            // (unslid) address of trie mapping program path to PrebuiltLoaderSet
    uint32_t    programTrieSize;
    uint32_t    osVersion;                  // OS Version of dylibs in this cache for the main platform
    uint32_t    altPlatform;                // e.g. iOSMac on macOS
    uint32_t    altOsVersion;               // e.g. 14.0 for iOSMac
    uint64_t    swiftOptsOffset;        // VM offset from cache_header* to Swift optimizations header
    uint64_t    swiftOptsSize;          // size of Swift optimizations header
    uint32_t    subCacheArrayOffset;    // file offset to first dyld_subcache_entry
    uint32_t    subCacheArrayCount;     // number of subCache entries
    uint8_t     symbolFileUUID[16];     // unique value for the shared cache file containing unmapped local symbols
    uint64_t    rosettaReadOnlyAddr;    // (unslid) address of the start of where Rosetta can add read-only/executable data
    uint64_t    rosettaReadOnlySize;    // maximum size of the Rosetta read-only/executable region
    uint64_t    rosettaReadWriteAddr;   // (unslid) address of the start of where Rosetta can add read-write data
    uint64_t    rosettaReadWriteSize;   // maximum size of the Rosetta read-write region
    uint32_t    imagesOffset;           // file offset to first dyld_cache_image_info
    uint32_t    imagesCount;            // number of dyld_cache_image_info entries
    uint32_t    cacheSubType;           // 0 for development, 1 for production, when cacheType is multi-cache(2)
    uint64_t    objcOptsOffset;         // VM offset from cache_header* to ObjC optimizations header
    uint64_t    objcOptsSize;           // size of ObjC optimizations header
    uint64_t    cacheAtlasOffset;       // VM offset from cache_header* to embedded cache atlas for process introspection
    uint64_t    cacheAtlasSize;         // size of embedded cache atlas
    uint64_t    dynamicDataOffset;      // VM offset from cache_header* to the location of dyld_cache_dynamic_data_header
    uint64_t    dynamicDataMaxSize;     // maximum size of space reserved from dynamic data
    uint32_t    tproMappingsOffset;     // file offset to first dyld_cache_tpro_mapping_info
    uint32_t    tproMappingsCount;      // number of dyld_cache_tpro_mapping_info entries
};

// Uncomment this and check the build errors for the current mapping offset to check against when adding new fields.
// template<size_t size> class A { int x[-size]; }; A<sizeof(dyld_cache_header)> a;


struct dyld_cache_mapping_info {
    uint64_t    address;
    uint64_t    size;
    uint64_t    fileOffset;
    uint32_t    maxProt;
    uint32_t    initProt;
};

// Contains the flags for the dyld_cache_mapping_and_slide_info flgs field
enum {
    DYLD_CACHE_MAPPING_AUTH_DATA            = 1 << 0U,
    DYLD_CACHE_MAPPING_DIRTY_DATA           = 1 << 1U,
    DYLD_CACHE_MAPPING_CONST_DATA           = 1 << 2U,
    DYLD_CACHE_MAPPING_TEXT_STUBS           = 1 << 3U,
    DYLD_CACHE_DYNAMIC_CONFIG_DATA          = 1 << 4U,
    DYLD_CACHE_READ_ONLY_DATA               = 1 << 5U,
    DYLD_CACHE_MAPPING_CONST_TPRO_DATA      = 1 << 6U,
};

struct dyld_cache_mapping_and_slide_info {
    uint64_t    address;
    uint64_t    size;
    uint64_t    fileOffset;
    uint64_t    slideInfoFileOffset;
    uint64_t    slideInfoFileSize;
    uint64_t    flags;
    uint32_t    maxProt;
    uint32_t    initProt;
};

struct dyld_cache_tpro_mapping_info {
    uint64_t    unslidAddress;
    uint64_t    size;
};

struct dyld_cache_image_info
{
    uint64_t    address;
    uint64_t    modTime;
    uint64_t    inode;
    uint32_t    pathFileOffset;
    uint32_t    pad;
};

struct dyld_cache_image_info_extra
{
    uint64_t    exportsTrieAddr;        // address of trie in unslid cache
    uint64_t    weakBindingsAddr;
    uint32_t    exportsTrieSize;
    uint32_t    weakBindingsSize;
    uint32_t    dependentsStartArrayIndex;
    uint32_t    reExportsStartArrayIndex;
};


struct dyld_cache_accelerator_info
{
    uint32_t    version;                // currently 1
    uint32_t    imageExtrasCount;       // does not include aliases
    uint32_t    imagesExtrasOffset;     // offset into this chunk of first dyld_cache_image_info_extra
    uint32_t    bottomUpListOffset;     // offset into this chunk to start of 16-bit array of sorted image indexes
    uint32_t    dylibTrieOffset;        // offset into this chunk to start of trie containing all dylib paths
    uint32_t    dylibTrieSize;          // size of trie containing all dylib paths
    uint32_t    initializersOffset;     // offset into this chunk to start of initializers list
    uint32_t    initializersCount;      // size of initializers list
    uint32_t    dofSectionsOffset;      // offset into this chunk to start of DOF sections list
    uint32_t    dofSectionsCount;       // size of initializers list
    uint32_t    reExportListOffset;     // offset into this chunk to start of 16-bit array of re-exports
    uint32_t    reExportCount;          // size of re-exports
    uint32_t    depListOffset;          // offset into this chunk to start of 16-bit array of dependencies (0x8000 bit set if upward)
    uint32_t    depListCount;           // size of dependencies
    uint32_t    rangeTableOffset;       // offset into this chunk to start of ss
    uint32_t    rangeTableCount;        // size of dependencies
    uint64_t    dyldSectionAddr;        // address of libdyld's __dyld section in unslid cache
};

struct dyld_cache_accelerator_initializer
{
    uint32_t    functionOffset;         // address offset from start of cache mapping
    uint32_t    imageIndex;
};

struct dyld_cache_range_entry
{
    uint64_t    startAddress;           // unslid address of start of region
    uint32_t    size;
    uint32_t    imageIndex;
};

struct dyld_cache_accelerator_dof
{
    uint64_t    sectionAddress;         // unslid address of start of region
    uint32_t    sectionSize;
    uint32_t    imageIndex;
};

struct dyld_cache_image_text_info
{
    uint8_t     uuid[16];
    uint64_t    loadAddress;            // unslid address of start of __TEXT
    uint32_t    textSegmentSize;
    uint32_t    pathOffset;             // offset from start of cache file
};


// The rebasing info is to allow the kernel to lazily rebase DATA pages of the
// dyld shared cache.  Rebasing is adding the slide to interior pointers.
struct dyld_cache_slide_info
{
    uint32_t    version;        // currently 1
    uint32_t    toc_offset;
    uint32_t    toc_count;
    uint32_t    entries_offset;
    uint32_t    entries_count;
    uint32_t    entries_size;  // currently 128 
    // uint16_t toc[toc_count];
    // entrybitmap entries[entries_count];
};

struct dyld_cache_slide_info_entry {
    uint8_t  bits[4096/(8*4)]; // 128-byte bitmap
};


// The version 2 of the slide info uses a different compression scheme. Since
// only interior pointers (pointers that point within the cache) are rebased
// (slid), we know the possible range of the pointers and thus know there are
// unused bits in each pointer.  We use those bits to form a linked list of
// locations needing rebasing in each page.
//
// Definitions:
//
//  pageIndex = (pageAddress - startOfAllDataAddress)/info->page_size
//  pageStarts[] = info + info->page_starts_offset
//  pageExtras[] = info + info->page_extras_offset
//  valueMask = ~(info->delta_mask)
//  deltaShift = __builtin_ctzll(info->delta_mask) - 2
//
// There are three cases:
//
// 1) pageStarts[pageIndex] == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE
//    The page contains no values that need rebasing.
//
// 2) (pageStarts[pageIndex] & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA) == 0
//    All rebase locations are in one linked list. The offset of the first
//    rebase location in the page is pageStarts[pageIndex] * 4.
//
// 3) pageStarts[pageIndex] & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA
//      Multiple linked lists are needed for all rebase locations in a page.
//    The pagesExtras array contains 2 or more entries each of which is the
//    start of a new linked list in the page. The first is at:
//       extrasStartIndex = (pageStarts[pageIndex] & 0x3FFF)
//      The next is at extrasStartIndex+1.  The last is denoted by
//    having the high bit (DYLD_CACHE_SLIDE_PAGE_ATTR_END) of the pageExtras[]
//    set.
//
// For 64-bit architectures, there is always enough free bits to encode all
// possible deltas.  The info->delta_mask field shows where the delta is located
// in the pointer.  That value must be masked off (valueMask) before the slide
// is added to the pointer.
//
// For 32-bit architectures, there are only three bits free (the three most
// significant bits). To extract the delta, you must first subtract value_add
// from the pointer value, then AND with delta_mask, then shift by deltaShift.
// That still leaves a maximum delta to the next rebase location of 28 bytes.
// To reduce the number or chains needed, an optimization was added.  Turns
// out zero is common in the DATA region.  A zero can be turned into a
// non-rebasing entry in the linked list.  The can be done because nothing
// in the shared cache should point out of its dylib to the start of the shared
// cache.
//
// The code for processing a linked list (chain) is:
//   
//    uint32_t delta = 1;
//    while ( delta != 0 ) {
//        uint8_t* loc = pageStart + pageOffset;
//        uintptr_t rawValue = *((uintptr_t*)loc);
//        delta = ((rawValue & deltaMask) >> deltaShift);
//        uintptr_t newValue = (rawValue & valueMask);
//        if ( newValue != 0 ) {
//            newValue += valueAdd;
//            newValue += slideAmount;
//        }
//        *((uintptr_t*)loc) = newValue;
//        pageOffset += delta;
//    }
//
//
struct dyld_cache_slide_info2
{
    uint32_t    version;            // currently 2
    uint32_t    page_size;          // currently 4096 (may also be 16384)
    uint32_t    page_starts_offset;
    uint32_t    page_starts_count;
    uint32_t    page_extras_offset;
    uint32_t    page_extras_count;
    uint64_t    delta_mask;         // which (contiguous) set of bits contains the delta to the next rebase location
    uint64_t    value_add;
    //uint16_t    page_starts[page_starts_count];
    //uint16_t    page_extras[page_extras_count];
};
#define DYLD_CACHE_SLIDE_PAGE_ATTRS                0xC000  // high bits of uint16_t are flags
#define DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA           0x8000  // index is into extras array (not starts array)
#define DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE       0x4000  // page has no rebasing
#define DYLD_CACHE_SLIDE_PAGE_ATTR_END             0x8000  // last chain entry for page



// The version 3 of the slide info uses a different compression scheme. Since
// only interior pointers (pointers that point within the cache) are rebased
// (slid), we know the possible range of the pointers and thus know there are
// unused bits in each pointer.  We use those bits to form a linked list of
// locations needing rebasing in each page.
//
// Definitions:
//
//  pageIndex = (pageAddress - startOfAllDataAddress)/info->page_size
//  pageStarts[] = info + info->page_starts_offset
//
// There are two cases:
//
// 1) pageStarts[pageIndex] == DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE
//    The page contains no values that need rebasing.
//
// 2) otherwise...
//    All rebase locations are in one linked list. The offset of the first
//    rebase location in the page is pageStarts[pageIndex].
//
// A pointer is one of of the variants in dyld_cache_slide_pointer3
//
// The code for processing a linked list (chain) is:
//
//    uint32_t delta = pageStarts[pageIndex];
//    dyld_cache_slide_pointer3* loc = pageStart;
//    do {
//        loc += delta;
//        delta = loc->offsetToNextPointer;
//        if ( loc->auth.authenticated ) {
//            newValue = loc->offsetFromSharedCacheBase  + results->slide + auth_value_add;
//            newValue = sign_using_the_various_bits(newValue);
//        }
//        else {
//            uint64_t value51      = loc->pointerValue;
//            uint64_t top8Bits     = value51 & 0x0007F80000000000ULL;
//            uint64_t bottom43Bits = value51 & 0x000007FFFFFFFFFFULL;
//            uint64_t targetValue  = ( top8Bits << 13 ) | bottom43Bits;
//            newValue = targetValue + results->slide;
//        }
//        loc->raw = newValue;
//    } while (delta != 0);
//
//
struct dyld_cache_slide_info3
{
    uint32_t    version;            // currently 3
    uint32_t    page_size;          // currently 4096 (may also be 16384)
    uint32_t    page_starts_count;
    uint64_t    auth_value_add;
    uint16_t    page_starts[/* page_starts_count */];
};

#define DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE    0xFFFF    // page has no rebasing

union dyld_cache_slide_pointer3
{
    uint64_t  raw;
    struct {
        uint64_t    pointerValue        : 51,
                    offsetToNextPointer : 11,
                    unused              :  2;
    }         plain;

    struct {
        uint64_t    offsetFromSharedCacheBase : 32,
                    diversityData             : 16,
                    hasAddressDiversity       :  1,
                    key                       :  2,
                    offsetToNextPointer       : 11,
                    unused                    :  1,
                    authenticated             :  1; // = 1;
    }         auth;
};



// The version 4 of the slide info is optimized for 32-bit caches up to 1GB.
// Since only interior pointers (pointers that point within the cache) are rebased
// (slid), we know the possible range of the pointers takes 30 bits.  That
// gives us two bits to use to chain to the next rebase.
//
// Definitions:
//
//  pageIndex = (pageAddress - startOfAllDataAddress)/info->page_size
//  pageStarts[] = info + info->page_starts_offset
//  pageExtras[] = info + info->page_extras_offset
//  valueMask = ~(info->delta_mask)
//  deltaShift = __builtin_ctzll(info->delta_mask) - 2
//
// There are three cases:
//
// 1) pageStarts[pageIndex] == DYLD_CACHE_SLIDE4_PAGE_NO_REBASE
//    The page contains no values that need rebasing.
//
// 2) (pageStarts[pageIndex] & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA) == 0
//    All rebase locations are in one linked list. The offset of the first
//    rebase location in the page is pageStarts[pageIndex] * 4.
//
// 3) pageStarts[pageIndex] & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA
//    Multiple chains are needed for all rebase locations in a page.
//    The pagesExtras array contains 2 or more entries each of which is the
//    start of a new chain in the page. The first is at:
//       extrasStartIndex = (pageStarts[pageIndex] & DYLD_CACHE_SLIDE4_PAGE_INDEX)
//    The next is at extrasStartIndex+1.  The last is denoted by
//    having the high bit (DYLD_CACHE_SLIDE4_PAGE_EXTRA_END) of the pageExtras[].
//
// For 32-bit architectures, there are only two bits free (the two most
// significant bits). To extract the delta, you must first subtract value_add
// from the pointer value, then AND with delta_mask, then shift by deltaShift.
// That still leaves a maximum delta to the next rebase location of 12 bytes.
// To reduce the number or chains needed, an optimization was added.  Turns
// most of the non-rebased data are small values and can be co-opt'ed into
// being used in the chain. The can be done because nothing
// in the shared cache should point to the first 64KB which are in the shared
// cache header information. So if the resulting pointer points to the
// start of the cache +/-32KB, then it is actually a small number that should
// not be rebased, but just reconstituted.
//
// The code for processing a linked list (chain) is:
//
//    uint32_t delta = 1;
//    while ( delta != 0 ) {
//        uint8_t* loc = pageStart + pageOffset;
//        uint32_t rawValue = *((uint32_t*)loc);
//        delta = ((rawValue & deltaMask) >> deltaShift);
//        uintptr_t newValue = (rawValue & valueMask);
//        if ( (newValue & 0xFFFF8000) == 0 ) {
//           // small positive non-pointer, use as-is
//        }
//        else if ( (newValue & 0x3FFF8000) == 0x3FFF8000 ) {
//           // small negative non-pointer
//           newValue |= 0xC0000000;
//        }
//        else  {
//            // pointer that needs rebasing
//            newValue += valueAdd;
//            newValue += slideAmount;
//        }
//        *((uint32_t*)loc) = newValue;
//        pageOffset += delta;
//    }
//
//
struct dyld_cache_slide_info4
{
    uint32_t    version;            // currently 4
    uint32_t    page_size;          // currently 4096 (may also be 16384)
    uint32_t    page_starts_offset;
    uint32_t    page_starts_count;
    uint32_t    page_extras_offset;
    uint32_t    page_extras_count;
    uint64_t    delta_mask;         // which (contiguous) set of bits contains the delta to the next rebase location (0xC0000000)
    uint64_t    value_add;          // base address of cache
    //uint16_t    page_starts[page_starts_count];
    //uint16_t    page_extras[page_extras_count];
};
#define DYLD_CACHE_SLIDE4_PAGE_NO_REBASE           0xFFFF  // page has no rebasing
#define DYLD_CACHE_SLIDE4_PAGE_INDEX               0x7FFF  // mask of page_starts[] values
#define DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA           0x8000  // index is into extras array (not a chain start offset)
#define DYLD_CACHE_SLIDE4_PAGE_EXTRA_END           0x8000  // last chain entry for page


// The version 5 of the slide info uses a different compression scheme. Since
// only interior pointers (pointers that point within the cache) are rebased
// (slid), we know the possible range of the pointers and thus know there are
// unused bits in each pointer.  We use those bits to form a linked list of
// locations needing rebasing in each page.
//
// Definitions:
//
//  pageIndex = (pageAddress - startOfAllDataAddress)/info->page_size
//  pageStarts[] = info + info->page_starts_offset
//
// There are two cases:
//
// 1) pageStarts[pageIndex] == DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE
//    The page contains no values that need rebasing.
//
// 2) otherwise...
//    All rebase locations are in one linked list. The offset of the first
//    rebase location in the page is pageStarts[pageIndex].
//
// A pointer is one of of the variants in dyld_cache_slide_pointer5
//
// The code for processing a linked list (chain) is:
//
//    uint32_t delta = pageStarts[pageIndex];
//    dyld_cache_slide_pointer5* loc = pageStart;
//    do {
//        loc += delta;
//        delta = loc->offsetToNextPointer;
//        newValue = loc->regular.target + value_add + results->slide;
//        if ( loc->auth.authenticated ) {
//            newValue = sign_using_the_various_bits(newValue);
//        }
//        else {
//            newValue = newValue | (loc->regular.high8 < 56);
//        }
//        loc->raw = newValue;
//    } while (delta != 0);
//
//
struct dyld_cache_slide_info5
{
    uint32_t    version;            // currently 5
    uint32_t    page_size;          // currently 4096 (may also be 16384)
    uint32_t    page_starts_count;
    uint64_t    value_add;
    uint16_t    page_starts[/* page_starts_count */];
};

#define DYLD_CACHE_SLIDE_V5_PAGE_ATTR_NO_REBASE    0xFFFF    // page has no rebasing

union dyld_cache_slide_pointer5
{
    uint64_t                                                raw;
    struct dyld_chained_ptr_arm64e_shared_cache_rebase      regular;
    struct dyld_chained_ptr_arm64e_shared_cache_auth_rebase auth;
};

struct dyld_cache_local_symbols_info
{
    uint32_t    nlistOffset;        // offset into this chunk of nlist entries
    uint32_t    nlistCount;         // count of nlist entries
    uint32_t    stringsOffset;      // offset into this chunk of string pool
    uint32_t    stringsSize;        // byte count of string pool
    uint32_t    entriesOffset;      // offset into this chunk of array of dyld_cache_local_symbols_entry
    uint32_t    entriesCount;       // number of elements in dyld_cache_local_symbols_entry array
};

struct dyld_cache_local_symbols_entry
{
    uint32_t    dylibOffset;        // offset in cache file of start of dylib
    uint32_t    nlistStartIndex;    // start index of locals for this dylib
    uint32_t    nlistCount;         // number of local symbols for this dylib
};

struct dyld_cache_local_symbols_entry_64
{
    uint64_t    dylibOffset;        // offset in cache buffer of start of dylib
    uint32_t    nlistStartIndex;    // start index of locals for this dylib
    uint32_t    nlistCount;         // number of local symbols for this dylib
};

struct dyld_subcache_entry_v1
{
    uint8_t     uuid[16];           // The UUID of the subCache file
    uint64_t    cacheVMOffset;      // The offset of this subcache from the main cache base address
};

struct dyld_subcache_entry
{
    uint8_t     uuid[16];           // The UUID of the subCache file
    uint64_t    cacheVMOffset;      // The offset of this subcache from the main cache base address
    char        fileSuffix[32];     // The file name suffix of the subCache file e.g. ".25.data", ".03.development"
};

// This struct is a small piece of dynamic data that can be included in the shared region, and contains configuration
// data about the shared cache in use by the process. It is located
struct dyld_cache_dynamic_data_header
{
    char        magic[16];              // e.g. "dyld_data    v0"
    uint64_t    fsId;                   // The fsid_t of the shared cache being used by a process
    uint64_t    fsObjId;                // The fs_obj_id_t of the shared cache being used by a process
};

// This is the  location of the macOS shared cache on macOS 11.0 and later
#define MACOSX_MRM_DYLD_SHARED_CACHE_DIR   "/System/Library/dyld/"

// This is old define for the old location of the dyld cache
#define MACOSX_DYLD_SHARED_CACHE_DIR       MACOSX_MRM_DYLD_SHARED_CACHE_DIR

#define IPHONE_DYLD_SHARED_CACHE_DIR       "/System/Library/Caches/com.apple.dyld/"

#define DRIVERKIT_DYLD_SHARED_CACHE_DIR    "/System/DriverKit/System/Library/dyld/"

#define EXCLAVEKIT_DYLD_SHARED_CACHE_DIR   "/System/ExclaveKit/System/Library/dyld/"

#if !TARGET_OS_SIMULATOR
  #define DYLD_SHARED_CACHE_BASE_NAME        "dyld_shared_cache_"
#else
  #define DYLD_SHARED_CACHE_BASE_NAME        "dyld_sim_shared_cache_"
#endif
#define DYLD_SHARED_CACHE_DEVELOPMENT_EXT  ".development"

#define DYLD_SHARED_CACHE_DYNAMIC_DATA_MAGIC    "dyld_data    v0"

static const char* cryptexPrefixes[] = {
    "/System/Volumes/Preboot/Cryptexes/OS/",
    "/private/preboot/Cryptexes/OS/",
    "/System/Cryptexes/OS"
};

static const uint64_t kDyldSharedCacheTypeDevelopment = 0;
static const uint64_t kDyldSharedCacheTypeProduction = 1;
static const uint64_t kDyldSharedCacheTypeUniversal = 2;





#endif // __DYLD_CACHE_FORMAT__


