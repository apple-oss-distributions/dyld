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


#ifndef MachOParser_h
#define MachOParser_h

#include <stdint.h>
#include <uuid/uuid.h>
#include <mach-o/loader.h>

#include <array>
#include <string>
#include <vector>

#include "Diagnostics.h"


#define BIND_TYPE_IMPORT_JMP_REL32 4

namespace dyld3 {

// Note, this should make PLATFORM_* values in <mach-o/loader.h>
enum class Platform {
    unknown     = 0,
    macOS       = 1,
    iOS         = 2,
    tvOS        = 3,
    watchOS     = 4,
    bridgeOS    = 5
};

struct VIS_HIDDEN UUID {
    UUID() {}
    UUID(const UUID& other) { uuid_copy(&_bytes[0], &other._bytes[0]); }
    UUID(const uuid_t other_uuid) { uuid_copy(&_bytes[0], other_uuid); }
    bool operator<(const UUID& other) const { return uuid_compare(&_bytes[0], &other._bytes[0]) < 0; }
    bool operator==(const UUID& other) const { return uuid_compare(&_bytes[0], &other._bytes[0]) == 0; }
    bool operator!=(const UUID& other) const { return !(*this == other); }

    size_t hash() const
    {
        size_t retval = 0;
        for (auto i = 0; i < 16 / sizeof(size_t); ++i) {
            retval ^= ((size_t*)(&_bytes[0]))[i];
        }
        return retval;
    }
    const unsigned char* get() const { return &_bytes[0]; };
private:
    std::array<unsigned char, 16> _bytes;
};

class VIS_HIDDEN MachOParser
{
public:
#if !DYLD_IN_PROCESS
    static bool         isValidMachO(Diagnostics& diag, const std::string& archName, Platform platform, const void* fileContent, size_t fileLength, const std::string& pathOpened, bool ignoreMainExecutables);
    static bool         isArch(const mach_header* mh, const std::string& archName);
    static std::string  archName(uint32_t cputype, uint32_t cpusubtype);
    static std::string  platformName(Platform platform);
    static std::string  versionString(uint32_t packedVersion);
    static uint32_t     cpuTypeFromArchName(const std::string& archName);
    static uint32_t     cpuSubtypeFromArchName(const std::string& archName);
#else
    static bool         isMachO(Diagnostics& diag, const void* fileContent, size_t fileLength);
    static bool         wellFormedMachHeaderAndLoadCommands(const mach_header* mh);
#endif
                        MachOParser(const mach_header* mh, bool dyldCacheIsRaw=false);
    bool                valid(Diagnostics& diag);

    const mach_header*  header() const;
    uint32_t            fileType() const;
    std::string         archName() const;
    bool                is64() const;
    bool                inDyldCache() const;
    bool                hasThreadLocalVariables() const;
    Platform            platform() const;
    uint64_t            preferredLoadAddress() const;
    UUID                uuid() const;
    bool                getUuid(uuid_t uuid) const;
    bool                getPlatformAndVersion(Platform* platform, uint32_t* minOS, uint32_t* sdk) const;
    bool                isSimulatorBinary() const;
    bool                getDylibInstallName(const char** installName, uint32_t* compatVersion, uint32_t* currentVersion) const;
    const char*         installName() const;
    uint32_t            dependentDylibCount() const;
    const char*         dependentDylibLoadPath(uint32_t depIndex) const;
    void                forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const;
    void                forEachSection(void (^callback)(const char* segName, const char* sectionName, uint32_t flags, const void* content, size_t size, bool illegalSectionSize, bool& stop)) const;
    void                forEachSegment(void (^callback)(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop)) const;
    void                forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void                forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void                forEachRPath(void (^callback)(const char* rPath, bool& stop)) const;
    void                forEachSection(void (^callback)(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, 
                                                        uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop)) const;

    struct FoundSymbol {
        enum class Kind { headerOffset, absolute, resolverOffset };
        Kind                kind;
        bool                isThreadLocal;
        const mach_header*  foundInDylib;
        void*               foundExtra;
        uint64_t            value;
        uint32_t            resolverFuncOffset;
        const char*         foundSymbolName;
    };

    typedef bool (^DependentFinder)(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra);

    bool                findExportedSymbol(Diagnostics& diag, const char* symbolName, void* extra, FoundSymbol& foundInfo, DependentFinder finder) const;
    bool                findClosestSymbol(uint64_t unSlidAddr, const char** symbolName, uint64_t* symbolUnslidAddr) const;
    bool                isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size);

#if DYLD_IN_PROCESS
    intptr_t            getSlide() const;
    bool                hasExportedSymbol(const char* symbolName, DependentFinder finder, void** result) const;
    bool                findClosestSymbol(const void* addr, const char** symbolName, const void** symbolAddress) const;
    const char*         segmentName(uint32_t segIndex) const;
#else

    bool                uses16KPages() const;
    bool                hasObjC() const;
    bool                hasWeakDefs() const;
    bool                isEncrypted() const;
    bool                hasPlusLoadMethod(Diagnostics& diag) const;
    bool                hasInitializer(Diagnostics& diag) const;
    bool                getCDHash(uint8_t cdHash[20]);
    bool                hasCodeSignature(uint32_t& fileOffset, uint32_t& size);
    bool                usesLibraryValidation() const;
    bool                isRestricted() const;
    bool                getEntry(uint32_t& offset, bool& usesCRT);
    bool                canBePlacedInDyldCache(const std::string& path) const;
    bool                canBePlacedInDyldCache(const std::string& path, std::set<std::string>& reasons) const;
    bool                isDynamicExecutable() const;
    bool                isSlideable() const;
    void                forEachInitializer(Diagnostics& diag, void (^callback)(uint32_t offset)) const;
    void                forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const;
    uint32_t            segmentCount() const;
    void                forEachExportedSymbol(Diagnostics diag, void (^callback)(const char* symbolName, uint64_t imageOffset, bool isReExport, bool& stop)) const;
    void                forEachSegment(void (^callback)(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop)) const;
    void                forEachRebase(Diagnostics& diag, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, bool& stop)) const;
    void                forEachBind(Diagnostics& diag, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal,
                                                    uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const;
    void                forEachWeakDef(Diagnostics& diag, void (^callback)(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset,
                                                    uint64_t addend, const char* symbolName, bool& stop)) const;
    void                forEachIndirectPointer(Diagnostics& diag, void (^handler)(uint32_t dataSegIndex, uint64_t dataSegOffset, bool bind, int bindLibOrdinal,
                                                                                  const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const;
    void                forEachInterposingTuple(Diagnostics& diag, void (^handler)(uint32_t segIndex, uint64_t replacementSegOffset, uint64_t replaceeSegOffset, uint64_t replacementContent, bool& stop)) const;
    const void*         content(uint64_t vmOffset);
#endif

    static const uint8_t*   trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol);
    static uint64_t         read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static int64_t          read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end);
    static bool             cdHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen, uint8_t cdHash[20]);
    static Platform         currentPlatform();

private:
   struct LayoutInfo {
#if DYLD_IN_PROCESS
        uintptr_t    slide;
        uintptr_t    textUnslidVMAddr;
        uintptr_t    linkeditUnslidVMAddr;
        uint32_t     linkeditFileOffset;
#else
        uint32_t     segmentCount;
        uint32_t     linkeditSegIndex;
        struct {
            uint64_t    mappingOffset;
            uint64_t    fileOffset;
            uint64_t    fileSize;
            uint64_t    segUnslidAddress;
            uint64_t    writable          :  1,
                        executable        :  1,
                        textRelocsAllowed :  1,  // segment supports text relocs (i386 only)
                        segSize           : 61;
           }            segments[128];
#endif
    };

    struct LinkEditInfo
    {
        const dyld_info_command*     dyldInfo;
        const symtab_command*        symTab;
        const dysymtab_command*      dynSymTab;
        const linkedit_data_command* splitSegInfo;
        const linkedit_data_command* functionStarts;
        const linkedit_data_command* dataInCode;
        const linkedit_data_command* codeSig;
        LayoutInfo                   layout;
    };

    void                    getLinkEditPointers(Diagnostics& diag, LinkEditInfo&) const;
    void                    getLinkEditLoadCommands(Diagnostics& diag, LinkEditInfo& result) const;
    void                    getLayoutInfo(LayoutInfo&) const;
    const uint8_t*          getLinkEditContent(const LayoutInfo& info, uint32_t fileOffset) const;

#if !DYLD_IN_PROCESS
    struct ArchInfo
    {
        const char* name;
        uint32_t    cputype;
        uint32_t    cpusubtype;
    };
    static const ArchInfo   _s_archInfos[];

    const uint8_t*          getContentForVMAddr(const LayoutInfo& info, uint64_t vmAddr) const;
    bool                    doLocalReloc(Diagnostics& diag, uint32_t r_address, bool& stop, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, bool& stop)) const;
    uint8_t                 relocPointerType() const;
    int                     libOrdinalFromDesc(uint16_t n_desc) const;
    bool                    doExternalReloc(Diagnostics& diag, uint32_t r_address, uint32_t r_symbolnum, LinkEditInfo& leInfo, bool& stop,
                                            void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal,
                                                             uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const;
    bool                    validLoadCommands(Diagnostics& diag, size_t fileLen);
    bool                    validEmbeddedPaths(Diagnostics& diag);
    bool                    validSegments(Diagnostics& diag, size_t fileLen);
    bool                    validLinkeditLayout(Diagnostics& diag);
    bool                    invalidBindState(Diagnostics& diag, const char* opcodeName, const MachOParser::LinkEditInfo& leInfo, bool segIndexSet, bool libraryOrdinalSet,
                                             uint32_t dylibCount, int libOrdinal, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const;
    bool                    invalidRebaseState(Diagnostics& diag, const char* opcodeName, const MachOParser::LinkEditInfo& leInfo, bool segIndexSet,
                                              uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type) const;
#endif
    static const void*      findCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen);
    void                    forEachSection(void (^callback)(const char* segName, uint32_t segIndex, uint64_t segVMAddr, const char* sectionName, uint32_t sectFlags,
                                                            uint64_t sectAddr, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop)) const;

    void                    forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const;
    bool                    isRaw() const;
    bool                    inRawCache() const;

    long                _data; // if low bit true, then this is raw file (not loaded image)
};



class VIS_HIDDEN FatUtil
{
public:
    static bool         isFatFile(const void* fileStart);
    static void         forEachSlice(Diagnostics& diag, const void* fileContent, size_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, size_t sliceSize, bool& stop));
#if !DYLD_IN_PROCESS
    static bool         isFatFileWithSlice(Diagnostics& diag, const void* fileContent, size_t fileLen, const std::string& archName, size_t& sliceOffset, size_t& sliceLen, bool& missingSlice);
#endif
};


} // namespace dyld3

namespace std {
template <>
struct hash<dyld3::UUID> {
    size_t operator()(const dyld3::UUID& x) const
    {
        return x.hash();
    }
};
}

#endif // MachOParser_h
