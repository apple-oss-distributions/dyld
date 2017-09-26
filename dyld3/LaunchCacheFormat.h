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



#ifndef LaunchCacheFormat_h
#define LaunchCacheFormat_h


#include <stdint.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>

#include "LaunchCache.h"


namespace dyld3 {
namespace launch_cache {
namespace binary_format {


// bump this number each time binary format changes
enum  { kFormatVersion = 8 };

union VIS_HIDDEN ImageRef {
    ImageRef() : val(0xFFFFFFFF) { }
                ImageRef(uint8_t kind, uint32_t groupNum, uint32_t indexInGroup) : _linkKind(kind), _groupNum(groupNum), _indexInGroup(indexInGroup) {
                    assert(groupNum < (1 << 18));
                    assert(indexInGroup < (1 << 12));
                }
    uint8_t     kind()  const { return _linkKind; }
    uint32_t    groupNum() const { return _groupNum; }
    uint32_t    indexInGroup() const { return _indexInGroup; }
    uint16_t    value() const { return val; }
    void        clearKind()  { _linkKind = 0; }
    
    bool operator==(const ImageRef& rhs) const {
        return (val == rhs.val);
    }
    bool operator!=(const ImageRef& rhs) const {
        return (val != rhs.val);
    }
    static ImageRef weakImportMissing();
    static ImageRef makeEmptyImageRef() { return ImageRef(); }

private:
    ImageRef(uint32_t v) : val(v) { }

    uint32_t     val;
    struct {
        uint32_t _linkKind       :   2,     // Image::LinkKind
                 _groupNum       :  18,     // 0 => cached dylib group, 1 => other dylib group, 2 => main closure, etc
                 _indexInGroup   :  12;     // max 64K images in group
    };
};




// In disk based images, all segments are multiples of page size
// This struct just tracks the size (disk and vm) of each segment.
// This is compact for most every image which have contiguous segments.
// If the image does not have contiguous segments (rare), an extra
// DiskSegment is inserted with the paddingNotSeg bit set.
struct DiskSegment
{
    uint64_t    filePageCount   : 30,
                vmPageCount     : 30,
                permissions     : 3,
                paddingNotSeg   : 1;
};


// In cache DATA_DIRTY is not page aligned or sized
// This struct allows segments with any alignment and up to 256MB in size
struct DyldCacheSegment
{
    uint64_t    cacheOffset : 32,
                size        : 28,
                permissions : 4;
};

// When an Image is built on the device, the mtime and inode are recorded.
// When built off device, the first 16 bytes of SHA1 of CodeDirectory is recorded.
union FileInfo
{
    struct {
        uint64_t mtime;
        uint64_t inode;
    } statInfo;
    struct {
        uint8_t  bytes[16];
    } cdHash16;
};

struct Image
{
    uint32_t            isDiskImage      : 1,       // images are DiskImage - not Image
                        isInvalid        : 1,       // an error occurred creating the info for this image
                        has16KBpages     : 1,
                        hasTextRelocs    : 1,
                        hasObjC          : 1,
                        mayHavePlusLoads : 1,
                        isEncrypted      : 1,       // image is DSMOS or FairPlay encrypted
                        hasWeakDefs      : 1,
                        neverUnload      : 1,
                        cwdSameAsThis    : 1,       // dylibs use file system relative paths, cwd must be main's dir
                        isPlatformBinary : 1,       // part of OS - can be loaded into LV process
                        isBundle         : 1,
                        overridableDylib : 1,       // only applicable to group 0
                        padding          : 7,
                        maxLoadCount     : 12;
    int32_t             groupOffset;                // back pointer to containing ImageGroup (from start of Image)
    uint32_t            pathPoolOffset;
    uint32_t            pathHash;
    FileInfo            fileInfo;
    uuid_t              uuid;
    uint16_t            dependentsArrayStartIndex;
    uint16_t            dependentsArrayCount;
    uint16_t            segmentsArrayStartIndex;
    uint16_t            segmentsArrayCount;
    uint16_t            initBeforeArrayStartIndex;
    uint16_t            initBeforeArrayCount;
    uint16_t            initOffsetsArrayStartIndex;
    uint16_t            initOffsetsArrayCount;
    uint16_t            dofOffsetsArrayStartIndex;
    uint16_t            dofOffsetsArrayCount;
};

// an image in the dyld shared cache
struct CachedImage : public Image
{
    uint32_t            patchStartIndex;
    uint32_t            patchCount;
};

// an image not in the dyld shared cache (loaded from disk at runtime)
struct DiskImage : public Image
{
    uint32_t            totalVmPages;
    uint32_t            sliceOffsetIn4K;
    uint32_t            codeSignFileOffset;
    uint32_t            codeSignFileSize;
    uint32_t            fixupsPoolOffset      : 28,    // offset in ImageGroup's pool for AllFixupsBySegment
                        fixupsPoolSegCount    : 4;     // count of segments in AllFixupsBySegment for this image
    uint32_t            fairPlayTextPageCount : 28,
                        fairPlayTextStartPage : 4;
    uint32_t            targetsArrayStartIndex;         // index in ImageGroup's pool of OrdinalEntry
    uint32_t            targetsArrayCount;
};


// if an Image has an alias (symlink to it), the Image does not record the alias, but the ImageGroup does
struct AliasEntry
{
    uint32_t    aliasHash;
    uint32_t    imageIndexInGroup;
    uint32_t    aliasOffsetInStringPool;
};

// each DiskImage points to an array of these, one per segment with fixups
struct AllFixupsBySegment
{
    uint32_t    segIndex    : 4,
                offset      : 28;    // from start of AllFixupsBySegment to this seg's SegmentFixupsByPage
};


// This struct is suitable for passing into kernel when kernel supports fixups on page-in.
struct SegmentFixupsByPage
{
    uint32_t    size;                // of this struct, including fixup opcodes
    uint32_t    pageSize;            // 0x1000 or 0x4000
    uint32_t    pageCount;
    uint32_t    pageInfoOffsets[1];  // array size is pageCount
    // each page info is a FixUpOpcode[]
};

enum class FixUpOpcode : uint8_t {
      done            = 0x00,
//    apply           = 0x10,
      rebase32        = 0x10,    // add32 slide at current pageOffset, increment pageOffset by 4
      rebase64        = 0x11,    // add64 slide at current pageOffset, increment pageOffset by 8
      bind32          = 0x12,    // set 32-bit ordinal value at current pageOffset, increment pageOffset by 4
      bind64          = 0x13,    // set 64-bit ordinal value at current pageOffset, increment pageOffset by 8
      rebaseText32    = 0x14,    // add32 slide at current text pageOffset, increment pageOffset by 4
      bindText32      = 0x15,    // set 32-bit ordinal value at current text pageOffset, increment pageOffset by 4
      bindTextRel32   = 0x16,    // set delta to 32-bit ordinal value at current text pageOffset, increment pageOffset by 4 (i386 CALL to dylib)
      bindImportJmp32 = 0x17,    // set delta to 32-bit ordinal value at current text pageOffset, increment pageOffset by 4 (i386 JMP to dylib)
//    fixupChain64    = 0x18,    // current page offset is start of a chain of locations to fix up
//    adjPageOffset   = 0x20,
      setPageOffset   = 0x20,    // low 4-bits is amount to increment (1 to 15).  If zero, then add next ULEB (note: can set offset for unaligned pointer)
      incPageOffset   = 0x30,    // low 4-bits *4 is amount to increment (4 to 60).  If zero, then add next ULEB * 4
//    adjOrdinal      = 0x40,
      setOrdinal      = 0x40,    // low 4-bits is ordinal (1-15).  If zero, then ordinal is next ULEB
      incOrdinal      = 0x50,    // low 4-bits is ordinal inc amount (1-15).  If zero, then ordinal is next ULEB
    repeat            = 0x60     // low 5-bits is how many next bytes to repeat.  next ULEB is repeat count
};

// If a closure uses DYLD_LIBRARY_PATH to override an OS dylib, there is an
// ImageRefOverride entry to redirect uses of the OS dylib.
struct ImageRefOverride
{
    ImageRef standardDylib;
    ImageRef overrideDylib;
};

// If a closure interposes on, or has a dylib that overrides, something in the dyld shared cache,
// then closure's ImageGroup contains an array of these
struct DyldCacheOverride
{
    uint64_t    patchTableIndex     : 24,       // index into PatchTable array of group 0
                imageIndex          : 8,        // index in this group (2) of what to replace with
                imageOffset         : 32;       // offset within image to override something in cache
};


// The ImageGroup for the dyld shared cache dylibs contains and array of these
// with one entry for each symbol in a cached dylib that is used by some other cached dylib.
struct PatchTable
{
    uint32_t    targetCacheOffset;      // delta from the base address of the cache to the address of the symbol to patch
    uint32_t    offsetsStartIndex;      // index into the PatchOffset array of first location to patch, last offset has low bit set
};

struct PatchOffset
{
    uint32_t    last             : 1,
                hasAddend        : 1,
                dataRegionOffset : 30;
};

struct ImageGroup
{
    uint32_t        imagesEntrySize         : 8,
                    dylibsExpectedOnDisk    : 1,
                    imageFileInfoIsCdHash   : 1,
                    padding                 : 14;
    uint32_t        groupNum;
    uint32_t        imagesPoolCount;
    uint32_t        imagesPoolOffset;           // offset to array of Image or DiskImage
    uint32_t        imageAliasCount;
    uint32_t        imageAliasOffset;           // offset to array of AliasEntry
    uint32_t        segmentsPoolCount;
    uint32_t        segmentsPoolOffset;         // offset to array of Segment or DyldCacheSegment
    uint32_t        dependentsPoolCount;
    uint32_t        dependentsPoolOffset;       // offset to array of ImageRef
    uint32_t        intializerOffsetPoolCount;
    uint32_t        intializerOffsetPoolOffset; // offset to array of uint32_t
    uint32_t        intializerListPoolCount;
    uint32_t        intializerListPoolOffset;   // offset to array of ImageRef
    uint32_t        targetsPoolCount;
    uint32_t        targetsOffset;              // offset to array of TargetSymbolValue
    uint32_t        fixupsPoolSize;
    uint32_t        fixupsOffset;               // offset to list of AllFixupsBySegment
    uint32_t        cachePatchTableCount;
    uint32_t        cachePatchTableOffset;      // offset to array of PatchTable (used only in group 0)
    uint32_t        cachePatchOffsetsCount;
    uint32_t        cachePatchOffsetsOffset;    // offset to array of PatchOffset cache offsets (used only in group 0)
    uint32_t        symbolOverrideTableCount;
    uint32_t        symbolOverrideTableOffset;  // offset to array of DyldCacheOverride (used only in group 2)
    uint32_t        imageOverrideTableCount;
    uint32_t        imageOverrideTableOffset;   // offset to array of ImageRefOverride (used only in group 2)
    uint32_t        dofOffsetPoolCount;
    uint32_t        dofOffsetPoolOffset;        // offset to array of uint32_t
    uint32_t        indirectGroupNumPoolCount;
    uint32_t        indirectGroupNumPoolOffset; // offset to array of uint32_t
    uint32_t        stringsPoolSize;
    uint32_t        stringsPoolOffset;
    // Image array
    // Alias array
    // Segment array
    // ImageRef array
    // Initializer offsets array
    // Initializer ImageRef array
    // TargetSymbolValue array
    // AllFixupsBySegment pool
    // PatchTable array
    // PatchOffset array
    // DyldCacheOverride array
    // ImageRefOverride array
    // string pool
    // DOF offsets array
};


struct Closure
{
    enum { magicV1 = 0x31646c6e };

    uint32_t        magic;
    uint32_t        usesCRT                  : 1,
                    isRestricted             : 1,
                    usesLibraryValidation    : 1,
                    padding                  : 29;
    uint32_t        missingFileComponentsOffset;    // offset to array of 16-bit string pool offset of path components
    uint32_t        dyldEnvVarsOffset;
    uint32_t        dyldEnvVarsCount;
    uint32_t        stringPoolOffset;
    uint32_t        stringPoolSize;
    ImageRef        libSystemRef;
    ImageRef        libDyldRef;
    uint32_t        libdyldVectorOffset;
    uint32_t        mainExecutableIndexInGroup;
    uint32_t        mainExecutableEntryOffset;
    uint32_t        initialImageCount;
    uuid_t          dyldCacheUUID;                // all zero if this closure is embedded in a dyld cache
    uint8_t         mainExecutableCdHash[20];     // or UUID if not code signed
    ImageGroup      group;
    // MissingFile array
    // env vars array
    // string pool
};



} // namespace binary_format

} // namespace launch_cache
} // namespace dyld


#endif // LaunchCacheFormat_h


