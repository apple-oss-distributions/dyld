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
#include "UUID.h"
#include "Diagnostics.h"
#include "MachOLayout.h"
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

namespace dyld3 {

using lsl::UUID;

// replacements for posix that handle EINTR
int	stat(const char* path, struct stat* buf) VIS_HIDDEN;
int	open(const char* path, int flag, int other) VIS_HIDDEN;
int fstatat(int fd, const char *path, struct stat *buf, int flag) VIS_HIDDEN;


/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
inline bool greaterThanAddOrOverflow(uint32_t addLHS, uint32_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}

/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
inline bool greaterThanAddOrOverflow(uint64_t addLHS, uint64_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}


// Note, this should match PLATFORM_* values in <mach-o/loader.h>
enum class Platform {
    unknown             = 0,
    macOS               = 1,    // PLATFORM_MACOS
    iOS                 = 2,    // PLATFORM_IOS
    tvOS                = 3,    // PLATFORM_TVOS
    watchOS             = 4,    // PLATFORM_WATCHOS
    bridgeOS            = 5,    // PLATFORM_BRIDGEOS
    iOSMac              = 6,    // PLATFORM_MACCATALYST
    iOS_simulator       = 7,    // PLATFORM_IOSSIMULATOR
    tvOS_simulator      = 8,    // PLATFORM_TVOSSIMULATOR
    watchOS_simulator   = 9,    // PLATFORM_WATCHOSSIMULATOR
    driverKit           = 10,   // PLATFORM_DRIVERKIT
};

// A prioritized list of architectures
class VIS_HIDDEN GradedArchs {
public:
    struct CpuGrade { uint32_t type = 0; uint32_t subtype = 0; bool osBinary = false; uint16_t grade = 0; };
    // never construct new ones - just use existing static instances
    GradedArchs()                   = delete;
    GradedArchs(const GradedArchs&) = delete;
    constexpr GradedArchs(const CpuGrade& cg0, const CpuGrade& cg1 = {0,0,false,0} , const CpuGrade& cg2 = {0,0,false,0}) : _orderedCpuTypes({cg0, cg1, cg2, CpuGrade()}) {}

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
    static const GradedArchs&  launchCurrentOS(const char* simArches); // for emulating how the kernel chooses which slice to exec()
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

    static const char*      archName(uint32_t cputype, uint32_t cpusubtype);
    static const char*      platformName(Platform platform);
    static bool             cpuTypeFromArchName(const char* archName, cpu_type_t* cputype, cpu_subtype_t* cpusubtype);
    static void             packedVersionToString(uint32_t packedVersion, char versionString[32]);
    static const char*      currentArchName();
    static Platform         currentPlatform();
    static Platform         basePlatform(dyld3::Platform reqPlatform);
    static uint64_t         read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static int64_t          read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static bool             isSimulatorPlatform(Platform platform, Platform* basePlatform=nullptr);
    static bool             isSharedCacheEligiblePath(const char* path);
    static const MachOFile* compatibleSlice(Diagnostics& diag, const void* content, size_t size, const char* path, Platform, bool isOSBinary, const GradedArchs&);
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
    bool            builtForPlatform(Platform, bool onlyOnePlatform=false) const;
    bool            loadableIntoProcess(Platform processPlatform, const char* path) const;
    bool            isZippered() const;
    bool            inDyldCache() const;
    bool            getUuid(uuid_t uuid) const;
    UUID            uuid() const;
    bool            hasWeakDefs() const;
    bool            usesWeakDefs() const;
    bool            isBuiltForSimulator() const;
    bool            hasThreadLocalVariables() const;
    bool            getDylibInstallName(const char** installName, uint32_t* compatVersion, uint32_t* currentVersion) const;
    void            forEachSupportedPlatform(void (^callback)(Platform platform, uint32_t minOS, uint32_t sdk)) const;
    void            forEachSupportedBuildTool(void (^callback)(Platform platform, uint32_t tool, uint32_t version)) const;
    const char*     installName() const;  // returns nullptr is no install name
    void            forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const;
    void            forEachInterposingSection(Diagnostics& diag, void (^handler)(uint64_t vmOffset, uint64_t vmSize, bool& stop)) const;
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    bool            canBePlacedInDyldCache(const char* path, void (^failureReason)(const char*)) const;
    bool            canHavePrebuiltExecutableLoader(dyld3::Platform platform, const std::string_view& path,
                                                    void (^failureReason)(const char*)) const;
#endif
#if BUILDING_APP_CACHE_UTIL
    bool            canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const;
    bool            usesClassicRelocationsInKernelCollection() const;
#endif
    bool            canHavePrecomputedDlopenClosure(const char* path, void (^failureReason)(const char*)) const;
    bool            canBeFairPlayEncrypted() const;
    bool            isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const;
    bool            allowsAlternatePlatform() const;
    bool            hasChainedFixups() const;
    bool            hasChainedFixupsLoadCommand() const;
    bool            hasOpcodeFixups() const;
    void            forDyldEnv(void (^callback)(const char* envVar, bool& stop)) const;
    bool            enforceCompatVersion() const;
    bool            hasInterposingTuples() const;
    bool            isRestricted() const;
    void            removeLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& remove, bool& stop));
    bool            hasSection(const char* segName, const char* sectName) const;
    void            forEachRPath(void (^callback)(const char* rPath, bool& stop)) const;
    bool            inCodeSection(uint32_t runtimeOffset) const;
    uint32_t        dependentDylibCount(bool* allDepsAreNormal = nullptr) const;
    bool            hasPlusLoadMethod(Diagnostics& diag) const;
    uint32_t        getFixupsLoadCommandFileOffset() const;
    bool            hasInitializer(Diagnostics& diag) const;
    void            forEachInitializerPointerSection(Diagnostics& diag, void (^callback)(uint32_t sectionOffset, uint32_t sectionSize, bool& stop)) const;
    bool            hasCodeSignature() const;
    bool            hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool            hasObjC() const;
    uint64_t        mappedSize() const;
    uint32_t        segmentCount() const;
    void            forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const;
    bool            hasObjCMessageReferences() const;
    uint32_t        loadCommandsFreeSpace() const;

    // Looks for the given section in the segments in order: __DATA, __DATA_CONST, __DATA_DIRTY.
    // Returns true if it finds a section, and sets the out parameters to the first section it found.
    // Returns false if the given section doesn't exist in any of the given segments.
    bool            findObjCDataSection(const char* sectionName, uint64_t& sectionRuntimeOffset, uint64_t& sectionSize) const;

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

    const thread_command*           unixThreadLoadCommand() const;
    const linkedit_data_command*    chainedFixupsCmd() const;
    uint32_t                        entryAddrRegisterIndexForThreadCmd() const;
    bool                            use64BitEntryRegs() const;
    uint64_t                        entryAddrFromThreadCmd(const thread_command* cmd) const;
    uint64_t                        preferredLoadAddress() const;
    bool                            getEntry(uint64_t& offset, bool& usesCRT) const;

    // used by DyldSharedCache to find closure
    static const uint8_t*   trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol);

#if !SUPPORT_VM_LAYOUT
    // Get the layout information for this MachOFile.  Requires that the file is in file layout, not VM layout.
    // If you have a MachOLoaded/MachOAnalyzer, do not use this method
    bool                    getLinkeditLayout(Diagnostics& diag, mach_o::LinkeditLayout& layout) const;
    void                    withFileLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout& layout)) const;
#endif

    // Note all kinds have the low bit set, and all offsets to values don't
    enum class SingletonPatchKind : uint32_t
    {
        unknown = 0,

        // An ISA, followed by a uintptr_t of constant data
        cfObj2 = 1
    };

    void forEachSingletonPatch(Diagnostics& diag, void (^handler)(SingletonPatchKind kind,
                                                                  uint64_t runtimeOffset)) const;

    struct SegmentInfo
    {
        uint64_t    fileOffset;
        uint64_t    fileSize;
        uint64_t    vmAddr;
        uint64_t    vmSize;
        uint64_t    sizeOfSections;
        const char* segName;
        uint32_t    loadCommandOffset;
        uint32_t    protections;
        uint32_t    textRelocs    :  1,  // segment has text relocs (i386 only)
                    readOnlyData  :  1,
                    isProtected   :  1,  // segment is protected
                    hasZeroFill   :  1,  // fileSize < vmSize
                    segIndex      : 12,
                    p2align       : 16;
        bool        readable() const   { return protections & VM_PROT_READ; }
        bool        writable() const   { return protections & VM_PROT_WRITE; }
        bool        executable() const { return protections & VM_PROT_EXECUTE; }
     };

    struct SectionInfo
    {
        SegmentInfo segInfo;
        uint64_t    sectAddr;
        uint64_t    sectSize;
        const char* sectName;
        uint32_t    sectFileOffset;
        uint32_t    sectFlags;
        uint32_t    sectAlignP2;
        uint32_t    reserved1;
        uint32_t    reserved2;
    };

    void            forEachSegment(void (^callback)(const SegmentInfo& info, bool& stop)) const;
    void            forEachSection(void (^callback)(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop)) const;
    void            forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const;

    struct PointerMetaData
    {
                    PointerMetaData();
                    PointerMetaData(const ChainedFixupPointerOnDisk* fixupLoc, uint16_t pointer_format);

        uint32_t    diversity         : 16,
                    high8             :  8,
                    authenticated     :  1,
                    key               :  2,
                    usesAddrDiversity :  1;
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

    const encryption_info_command* findFairPlayEncryptionLoadCommand() const;

    struct ArchInfo
    {
        const char* name;
        uint32_t    cputype;
        uint32_t    cpusubtype;
    };
    static const ArchInfo       _s_archInfos[];

    struct PlatformInfo
    {
        const char* name;
        Platform    platform;
        uint32_t    loadCommand;
    };
    static const PlatformInfo   _s_platformInfos[];

    void    analyzeSegmentsLayout(uint64_t& vmSpace, bool& hasZeroFill) const;

    // This calls the callback for all code directories required for a given platform/binary combination.
    // On watchOS main executables this is all cd hashes.
    // On watchOS dylibs this is only the single cd hash we need (by rank defined by dyld, not the kernel).
    // On all other platforms this always returns a single best cd hash (ranked to match the kernel).
    // Note the callback parameter is really a CS_CodeDirectory.
    void    forEachCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen, void (^callback)(const void* cd)) const;
};


} // namespace dyld3

#endif /* MachOFile_h */
