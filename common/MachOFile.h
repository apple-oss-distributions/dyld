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


#ifndef MachOFile_h
#define MachOFile_h

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <uuid/uuid.h>

#include <array>
#include <optional>
#include <string_view>

#include "Defines.h"
#include "Header.h"
#include "UUID.h"
#include "Diagnostics.h"
#include "MachOLayout.h"
#include "Platform.h"
#include "SupportedArchs.h"
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>

// needed until dyld builds with a newer SDK
#ifndef CPU_SUBTYPE_ARM64E
  #define CPU_SUBTYPE_ARM64E 2
#endif
#ifndef CPU_TYPE_ARM64_32
  #define CPU_TYPE_ARM64_32 0x0200000C
#endif
#ifndef CPU_SUBTYPE_ARM64_32_V8
  #define CPU_SUBTYPE_ARM64_32_V8 1
#endif
#ifndef BIND_OPCODE_THREADED
    #define BIND_OPCODE_THREADED    0xD0
#endif
#ifndef BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB
    #define BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB    0x00
#endif
#ifndef BIND_SUBOPCODE_THREADED_APPLY
    #define BIND_SUBOPCODE_THREADED_APPLY                               0x01
#endif
#ifndef BIND_SPECIAL_DYLIB_WEAK_LOOKUP
  #define BIND_SPECIAL_DYLIB_WEAK_LOOKUP                (-3)
#endif
#ifndef EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
  #define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE             0x02
#endif
#ifndef SG_READ_ONLY
  #define SG_READ_ONLY            0x10
#endif

#ifndef LC_DYLD_EXPORTS_TRIE
  #define LC_DYLD_EXPORTS_TRIE    0x80000033
#endif
#ifndef LC_DYLD_CHAINED_FIXUPS
  #define LC_DYLD_CHAINED_FIXUPS  0x80000034
#endif

#ifndef S_INIT_FUNC_OFFSETS
  #define S_INIT_FUNC_OFFSETS       0x16
#endif

#ifndef MH_FILESET
    #define MH_FILESET              0xc     /* set of mach-o's */
#endif

namespace objc_visitor {
struct Visitor;
}

namespace dyld3 {

using lsl::UUID;

// replacements for posix that handle EINTR
int	stat(const char* path, struct stat* buf) VIS_HIDDEN;
int	open(const char* path, int flag, int other) VIS_HIDDEN;
int fstatat(int fd, const char *path, struct stat *buf, int flag) VIS_HIDDEN;


/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
VIS_HIDDEN inline bool greaterThanAddOrOverflow(uint32_t addLHS, uint32_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}

/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
VIS_HIDDEN inline bool greaterThanAddOrOverflow(uint64_t addLHS, uint64_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}

// A prioritized list of architectures
class VIS_HIDDEN GradedArchs {
public:
    struct CpuGrade { uint32_t type = 0; uint32_t subtype = 0; bool osBinary = false; uint16_t grade = 0; };
    // never construct new ones - just use existing static instances
    GradedArchs()                   = delete;
    GradedArchs(const GradedArchs&) = delete;
    constexpr GradedArchs(const CpuGrade& cg0, const CpuGrade& cg1 = {0,0,false,0} , const CpuGrade& cg2 = {0,0,false,0}) : _orderedCpuTypes({cg0, cg1, cg2, CpuGrade()}) {}

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
    static const GradedArchs&  launchCurrentOS(const char* simArches=""); // for emulating how the kernel chooses which slice to exec()
#endif
    static const GradedArchs&  forCurrentOS(bool keysOff, bool platformBinariesOnly);
    static const GradedArchs&  forName(const char* archName, bool keysOff = false);


    int                     grade(uint32_t cputype, uint32_t cpusubtype, bool platformBinariesOnly) const;
    const char*             name() const;
    bool                    checksOSBinary() const;
    bool                    supports64() const;
    void                    forEachArch(bool platformBinariesOnly, void (^handler)(const char* name)) const;

    // pre-built lists for existing hardware
#ifdef i386
#undef i386
#endif
    static const GradedArchs i386;            // 32-bit Mac
    static const GradedArchs x86_64;          // older Mac
    static const GradedArchs x86_64h;         // haswell Mac
    static const GradedArchs arm64;           // A11 or earlier iPhone or iPad
#if SUPPORT_ARCH_arm64e
    static const GradedArchs arm64e;            // A12 or later iPhone or iPad
    static const GradedArchs arm64e_keysoff;    // A12 running with signing keys disabled
    static const GradedArchs arm64e_pb;         // macOS Apple Silicon running platform binary
    static const GradedArchs arm64e_keysoff_pb; // macOS Apple Silicon running with signing keys disabled
#endif
    static const GradedArchs armv7k;          // watch thru series 3
    static const GradedArchs armv7s;          // deprecated
    static const GradedArchs armv7;           // deprecated
    static const GradedArchs armv6m;          // firmware
    static const GradedArchs armv7m;          // firmware
    static const GradedArchs armv7em;         // firmware
    static const GradedArchs armv8m;          // firmware

#if SUPPORT_ARCH_arm64_32
    static const GradedArchs arm64_32;        // watch series 4 and later
#endif

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
    static const GradedArchs launch_AS;       // Apple Silicon macs
    static const GradedArchs launch_AS_Sim;   // iOS simulator for Apple Silicon macs
    static const GradedArchs launch_Intel_h;  // Intel macs with haswell cpu
    static const GradedArchs launch_Intel;    // Intel macs
    static const GradedArchs launch_Intel_Sim; // iOS simulator for Intel macs
#endif

// private:
// should be private, but compiler won't statically initialize static members above
    const std::array<CpuGrade, 4>     _orderedCpuTypes;  // zero terminated
};


// A file read/mapped into memory
struct VIS_HIDDEN FatFile : fat_header
{
    static const FatFile*  isFatFile(const void* fileContent);
    void                   forEachSlice(Diagnostics& diag, uint64_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const;
    bool                   isFatFileWithSlice(Diagnostics& diag, uint64_t fileLen, const GradedArchs& archs, bool osBinary, uint64_t& sliceOffset, uint64_t& sliceLen, bool& missingSlice) const;
    const char*            archNames(char strBuf[256], uint64_t fileLen) const;
private:
    bool                   isValidSlice(Diagnostics& diag, uint64_t fileLen, uint32_t sliceIndex,
                                        uint32_t sliceCpuType, uint32_t sliceCpuSubType, uint64_t sliceOffset, uint64_t sliceLen) const;
    void                   forEachSlice(Diagnostics& diag, uint64_t fileLen, bool validate,
                                        void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const;
};


// A mach-o file read/mapped into memory
// Only info from mach_header or load commands is accessible (no LINKEDIT info)
struct VIS_HIDDEN MachOFile : mach_header
{
    typedef mach_o::ChainedFixupPointerOnDisk ChainedFixupPointerOnDisk;

    static uint64_t         read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static int64_t          read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static const MachOFile* compatibleSlice(Diagnostics& diag, uint64_t& sliceOffsetOut, uint64_t& sliceLenOut, const void* content, size_t size, const char* path, mach_o::Platform platform, bool isOSBinary, const GradedArchs&, bool internalInstall=false);
    static const MachOFile* isMachO(const void* content);

    bool            hasMachOMagic() const;
    bool            isMachO(Diagnostics& diag, uint64_t fileSize) const;
    bool            isDyldManaged() const;
    bool            isDylib() const;
    bool            isBundle() const;
    bool            isMainExecutable() const;
    bool            isDynamicExecutable() const;
    bool            isStaticExecutable() const;
    bool            isKextBundle() const;
    bool            isFileSet() const;
    bool            isPreload() const;
    bool            isDyld() const;
    bool            isPIE() const;
    bool            isArch(const char* archName) const;
    const char*     archName() const;
    bool            is64() const;
    uint32_t        maskedCpuSubtype() const;
    size_t          machHeaderSize() const;
    uint32_t        pointerSize() const;
    bool            uses16KPages() const;
    bool            inDyldCache() const;
    bool            hasWeakDefs() const;
    bool            usesWeakDefs() const;
    void            forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const;
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_UNIT_TESTS || BUILDING_DYLD_SYMBOLS_CACHE
    bool            addendsExceedPatchTableLimit(Diagnostics& diag, mach_o::Fixups fixups) const;
    bool            canBePlacedInDyldCache(const char* path, bool checkObjC, void (^failureReason)(const char* format, ...)) const;
    bool            canHavePrebuiltExecutableLoader(mach_o::Platform platform, const std::string_view& path,
                                                    void (^failureReason)(const char*)) const;
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // Note this is only for binaries in file layout, so can only be used by the cache builder
    objc_visitor::Visitor makeObjCVisitor(Diagnostics& diag) const;
#endif

#if BUILDING_APP_CACHE_UTIL
    bool            canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const;
#endif
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
    bool            usesClassicRelocationsInKernelCollection() const;
#endif
    bool            hasChainedFixups() const;
    bool            hasChainedFixupsLoadCommand() const;
    bool            hasOpcodeFixups() const;
    void            removeLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& remove, bool& stop));
    bool            hasSection(const char* segName, const char* sectName) const;
    bool            inCodeSection(uint32_t runtimeOffset) const;
    uint32_t        dependentDylibCount(bool* allDepsAreNormal = nullptr) const;
    uint32_t        getFixupsLoadCommandFileOffset() const;
    bool            hasInitializer(Diagnostics& diag) const;
    void            forEachInitializerPointerSection(Diagnostics& diag, void (^callback)(uint32_t sectionOffset, uint32_t sectionSize, bool& stop)) const;
    bool            hasObjC() const;
    bool            hasConstObjCSection() const;
    uint64_t        mappedSize() const;
    void            forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const;
    enum class Malformed { linkeditOrder, linkeditAlignment, linkeditPermissions, dyldInfoAndlocalRelocs, segmentOrder,
                            textPermissions, executableData, writableData, codeSigAlignment, sectionsAddrRangeWithinSegment,
                            noLinkedDylibs, loaderPathsAreReal, mainExecInDyldCache, noUUID, zerofillSwiftMetadata, sdkOnOrAfter2021, sdkOnOrAfter2022 };
    bool            enforceFormat(Malformed) const;

    bool            validSegments(Diagnostics& diag, const char* path, size_t fileLen) const;

    // used at runtime to validate loaded image matches closure
    void            forEachCDHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen,
                                                 void (^callback)(const uint8_t cdHash[20])) const;

    static void     forEachTreatAsWeakDef(void (^handler)(const char* symbolName));

    // used by closure builder to find the offset and size of the trie.
    bool            hasExportTrie(uint32_t& runtimeOffset, uint32_t& size) const;

    uint32_t                        entryAddrRegisterIndexForThreadCmd() const;
    bool                            use64BitEntryRegs() const;
    uint64_t                        entryAddrFromThreadCmd(const thread_command* cmd) const;

    // used by DyldSharedCache to find closure
    static const uint8_t*   trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol);

#if !SUPPORT_VM_LAYOUT || BUILDING_UNIT_TESTS || BUILDING_DYLD_SYMBOLS_CACHE
    // Get the layout information for this MachOFile.  Requires that the file is in file layout, not VM layout.
    // If you have a MachOLoaded/MachOAnalyzer, do not use this method
    bool                    getLinkeditLayout(Diagnostics& diag, mach_o::LinkeditLayout& layout) const;
    void                    withFileLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout& layout)) const;
#endif

    void            forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const;

    struct PointerMetaData
    {
                    PointerMetaData();
                    PointerMetaData(const ChainedFixupPointerOnDisk* fixupLoc, uint16_t pointer_format);

        bool        operator==(const PointerMetaData& other) const;

        uint32_t    diversity         : 16,
                    high8             :  8,
                    authenticated     :  1,
                    key               :  2,
                    usesAddrDiversity :  1,
                    padding           :  4 = 0;
    };

    static uint16_t chainedPointerFormat(const dyld_chained_fixups_header* chainHeader);
    static void     withChainStarts(Diagnostics& diag, const dyld_chained_fixups_header* chainHeader, void (^callback)(const dyld_chained_starts_in_image*));

    static void     forEachFixupChainSegment(Diagnostics& diag, const dyld_chained_starts_in_image* starts,
                                             void (^handler)(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stop));
    static void     forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo,
                                                bool notifyNonPointers, uint8_t* segmentContent,
                                                void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop));
    static bool     walkChain(Diagnostics& diag, ChainedFixupPointerOnDisk* start, uint16_t pointer_format,
                              bool notifyNonPointers, uint32_t max_valid_pointer,
                              void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop));
    static void     forEachChainedFixupTarget(Diagnostics& diag, const dyld_chained_fixups_header* chainHeader, const linkedit_data_command* chainedFixups,
                                              void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop));

protected:
    bool            hasMachOBigEndianMagic() const;
    bool            hasLoadCommand(uint32_t) const;

    void    analyzeSegmentsLayout(uint64_t& vmSpace, bool& hasZeroFill) const;

    // This calls the callback for all code directories required for a given platform/binary combination.
    // On watchOS main executables this is all cd hashes.
    // On watchOS dylibs this is only the single cd hash we need (by rank defined by dyld, not the kernel).
    // On all other platforms this always returns a single best cd hash (ranked to match the kernel).
    // Note the callback parameter is really a CS_CodeDirectory.
    void    forEachCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen, void (^callback)(const void* cd)) const;
    void    forEachSection(void (^callback)(const mach_o::Header::SectionInfo&, bool& stop)) const;
    void    forEachSection(void (^callback)(const mach_o::Header::SegmentInfo&, const mach_o::Header::SectionInfo&, bool& stop)) const;
};


} // namespace dyld3

#endif /* MachOFile_h */
