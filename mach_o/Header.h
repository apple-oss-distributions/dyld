/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_Header_h
#define mach_o_Header_h

#include <array>
#include <span>
#include <string_view>

#include <limits.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <uuid/uuid.h>

#include "Platform.h"
#include "Architecture.h"
#include "Version32.h"
#include "Version64.h"
#include "Error.h"
#include "Policy.h"

#if BUILDING_MACHO_WRITER
#include <vector>
#endif

#ifndef LC_ATOM_INFO
#define LC_ATOM_INFO      0x36
#endif

#ifndef DYLIB_USE_WEAK_LINK
    struct dylib_use_command {
        uint32_t    cmd;                     /* LC_LOAD_DYLIB or LC_LOAD_WEAK_DYLIB */
        uint32_t    cmdsize;                 /* overall size, including path */
        uint32_t    nameoff;                 /* == 16, dylibs's path offset */
        uint32_t    marker;                  /* == 0x1a741800 */
        uint32_t    current_version;         /* dylib's current version number */
        uint32_t    compat_version;          /* == 0x00010000 */
        uint32_t    flags;                   /* DYLIB_USE_... flags */
    };
    #define DYLIB_USE_WEAK_LINK      0x01
    #define DYLIB_USE_REEXPORT       0x02
    #define DYLIB_USE_UPWARD         0x04
    #define DYLIB_USE_DELAYED_INIT   0x08
#endif

namespace mach_o {

/*!
 * @union DependentDylibAttributes
 *
 * @abstract
 *      Attributes of how a dylib can be linked
 */
union DependentDylibAttributes
{
    constexpr   DependentDylibAttributes() : raw(0) {}
    struct {
        bool    weakLink  : 1 = false;
        bool    reExport  : 1 = false;
        bool    upward    : 1 = false;
        bool    delayInit : 1 = false;
        uint8_t padding   : 4 = 0;
    };
    uint8_t     raw;

    static const DependentDylibAttributes regular;
    static const DependentDylibAttributes justWeakLink;
    static const DependentDylibAttributes justUpward;
    static const DependentDylibAttributes justReExport;
    static const DependentDylibAttributes justDelayInit;
private:
    constexpr   DependentDylibAttributes(uint8_t v) : raw(v) {}
};
static_assert(sizeof(DependentDylibAttributes) == 1);

inline bool operator==(const DependentDylibAttributes& a, DependentDylibAttributes b) { return (a.raw == b.raw); }

/*!
 * @class Header
 *
 * @abstract
 *      A mapped mach-o file can be cast to a Header*, then these methods used to parse/validate it.
 *      The Header constructor can be used to build a mach-o file dynamically for unit tests
 */
struct VIS_HIDDEN Header
{
    static bool             isSharedCacheEligiblePath(const char* path);
    static const Header*    isMachO(std::span<const uint8_t> content);

    Error           valid(uint64_t fileSize) const;

    // methods that look at mach_header
    bool            hasMachOMagic() const;
    const char*     archName() const;
    Architecture    arch() const;
    uint32_t        pointerSize() const;
    bool            uses16KPages() const;
    bool            is64() const;
    bool            isDyldManaged() const;
    bool            isDylib() const;
    bool            isBundle() const;
    bool            isMainExecutable() const;
    bool            isDynamicExecutable() const;
    bool            isStaticExecutable() const;
    bool            isKextBundle() const;
    bool            isObjectFile() const;
    bool            isFileSet() const;
    bool            isPreload() const;
    bool            isPIE() const;
    bool            isArch(const char* archName) const;
    bool            inDyldCache() const;
    bool            hasThreadLocalVariables() const;
    bool            hasWeakDefs() const;
    bool            usesWeakDefs() const;
    uint32_t        machHeaderSize() const;
    bool            mayHaveTextFixups() const;
    bool            hasSubsectionsViaSymbols() const;
    bool            noReexportedDylibs() const;
    bool            isAppExtensionSafe() const;
    bool            isSimSupport() const;

    // methods that look for load commands
    Error               validStructureLoadCommands(uint64_t fileSize) const;
    Error               forEachLoadCommand(void (^callback)(const load_command* cmd, bool& stop)) const;
    void                forEachLoadCommandSafe(void (^callback)(const load_command* cmd, bool& stop)) const; //asserts if error
    void                forDyldEnv(void (^callback)(const char* envVar, bool& stop)) const;
    void                forEachRPath(void (^callback)(const char* rPath, bool& stop)) const;
    void                forEachLinkerOption(void (^callback)(const char* opt, bool& stop)) const;
    void                forAllowableClient(void (^callback)(const char* clientName, bool& stop)) const;
    PlatformAndVersions platformAndVersions() const;
    bool                builtForPlatform(Platform, bool onlyOnePlatform=false) const;
    bool                isZippered() const;
    bool                getUuid(uuid_t uuid) const;
    bool                getDylibInstallName(const char** installName, Version32* compatVersion, Version32* currentVersion) const;
    const char*         installName() const;  // returns nullptr is no install name
    const char*         umbrellaName() const; // returns nullptr if dylib is not in an umbrella
    void                forEachDependentDylib(void (^callback)(const char* loadPath, DependentDylibAttributes kind, Version32 compatVersion, Version32 curVersion, bool& stop)) const;
    const char*         dependentDylibLoadPath(uint32_t depIndex) const;
    uint32_t            dependentDylibCount(bool* allDepsAreRegular = nullptr) const;
    bool                canBeFairPlayEncrypted() const;
    bool                hasEncryptionInfo(uint32_t& cryptId, uint32_t& textOffset, uint32_t& size) const;
    bool                isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const;
    bool                hasChainedFixups() const;
    bool                hasChainedFixupsLoadCommand() const;
    bool                hasOpcodeFixups() const;
    bool                hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool                getEntry(uint64_t& offset, bool& usesCRT) const;
    bool                hasIndirectSymbolTable(uint32_t& fileOffset, uint32_t& count) const;
    bool                hasSplitSegInfo() const;
    bool                hasAtomInfo(uint32_t& fileOffset, uint32_t& count) const;

    // methods that look for segments/sections
    uint32_t         segmentCount() const;
    uint32_t         loadCommandsFreeSpace() const;
    bool             allowsAlternatePlatform() const;
    bool             hasInterposingTuples() const;
    bool             isRestricted() const;
    uint64_t         preferredLoadAddress() const;
    int64_t          getSlide() const;
    bool             hasObjC() const;
    bool             hasDataConst() const;
    std::string_view segmentName(uint32_t segIndex) const;
    struct SegmentInfo { std::string_view segmentName; uint64_t vmaddr=0; uint64_t vmsize=0; uint32_t fileOffset=0; uint32_t fileSize=0; uint32_t flags=0; uint8_t perms=0; };
    void             forEachSegment(void (^callback)(const SegmentInfo& info, bool& stop)) const;
    struct SectionInfo { std::string_view segmentName; std::string_view sectionName; uint32_t segPerms = 0; uint32_t flags=0; uint32_t alignment=0;
                         uint64_t address=0; uint64_t size=0; uint32_t fileOffset=0;
                         uint32_t relocsOffset=0; uint32_t relocsCount=0; uint32_t reserved1=0; uint32_t reserved2=0; };
    void             forEachSection(void (^callback)(const SectionInfo&, bool& stop)) const;
    uint32_t         headerAndLoadCommandsSize() const;
    uint32_t         fileSize() const;
    const uint8_t*   computeLinkEditBias(bool zeroFillExpanded) const;
    bool             hasZerofillExpansion() const;
    uint64_t         zerofillExpansionAmount() const;
    bool             hasCustomStackSize(uint64_t& size) const;

#if BUILDING_MACHO_WRITER
    // for building
    static Header*  make(std::span<uint8_t> buffer, uint32_t filetype, uint32_t flags, Architecture, bool addImplicitTextSegment=true);
    
    void            save(char savedPath[PATH_MAX]) const;
    load_command*   findLoadCommand(uint32_t cmd);
    uint32_t        pointerAligned(uint32_t value) const;
    void            setHasThreadLocalVariables();
    void            setHasWeakDefs();
    void            setUsesWeakDefs();
    void            setAppExtensionSafe();
    void            setSimSupport();
    void            setNoReExportedDylibs();
    void            addPlatformInfo(Platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools={});
    void            addUniqueUUID(uuid_t copyOfUUID=nullptr);
    void            addNullUUID();
    void            updateUUID(uuid_t);
    void            addInstallName(const char* path, Version32 compatVers, Version32 currentVersion);
    void            addDependentDylib(const char* path, DependentDylibAttributes kind=DependentDylibAttributes::regular, Version32 compatVers=Version32(1,0), Version32 currentVersion=Version32(1,0));
    void            addLibSystem();
    void            addDylibId(CString name, Version32 compatVers, Version32 currentVersion);
    void            addDyldID();
    void            addDynamicLinker();
    void            addRPath(const char* path);
    void            addSourceVersion(Version64 srcVers);
    void            addDyldEnvVar(const char* envVar);
    void            addAllowableClient(const char* clientName);
    void            addUmbrellaName(const char* umbrellaName);
    void            addFairPlayEncrypted(uint32_t offset, uint32_t size);
    void            addCodeSignature(uint32_t fileOffset, uint32_t fileSize);
    void            addSegment(std::string_view segName, uint64_t vmaddr, uint64_t vmsize, uint32_t perms, uint32_t sectionCount);
    void            setMain(uint32_t offset);
    void            setCustomStackSize(uint64_t stackSize);
    void            setUnixEntry(uint64_t addr);
    void            setSymbolTable(uint32_t nlistOffset, uint32_t nlistCount, uint32_t stringPoolOffset, uint32_t stringPoolSize,
                                   uint32_t localsCount, uint32_t globalsCount, uint32_t undefCount, uint32_t indOffset, uint32_t indCount);
    void            setBindOpcodesInfo(uint32_t rebaseOffset, uint32_t rebaseSize,
                                       uint32_t bindsOffset, uint32_t bindsSize,
                                       uint32_t weakBindsOffset, uint32_t weakBindsSize,
                                       uint32_t lazyBindsOffset, uint32_t lazyBindsSize,
                                       uint32_t exportTrieOffset, uint32_t exportTrieSize);
    void            setChainedFixupsInfo(uint32_t cfOffset, uint32_t cfSize);
    void            setExportTrieInfo(uint32_t offset, uint32_t size);
    void            setSplitSegInfo(uint32_t offset, uint32_t size);
    void            setDataInCode(uint32_t offset, uint32_t size);
    void            setFunctionStarts(uint32_t offset, uint32_t size);
    void            addLinkerOption(std::span<uint8_t> buffer, uint32_t count);
    void            setAtomInfo(uint32_t offset, uint32_t size);

    void            updateRelocatableSegmentSize(uint64_t vmSize, uint32_t fileSize);
    void            setRelocatableSectionCount(uint32_t sectionCount);
    void            setRelocatableSectionInfo(uint32_t sectionIndex, const char* segName, const char* sectName, uint32_t flags, uint64_t address,
                                              uint64_t size, uint32_t fileOffset, uint16_t alignment, uint32_t relocsOffset, uint32_t relocsCount);

    void            addSegment(const SegmentInfo&, std::span<const char* const> sectionNames=std::span<const char* const>{});
    void            updateSegment(const SegmentInfo& info);
    void            updateSection(const SectionInfo& info);

    struct LinkerOption
    {
        std::vector<uint8_t> buffer;
        uint32_t             count = 0;

        uint32_t lcSize() const { return ((uint32_t)buffer.size() + sizeof(linker_option_command) + 7) & (-8); }

        static LinkerOption make(std::span<CString>);
    };

    static uint32_t relocatableHeaderAndLoadCommandsSize(bool is64, uint32_t sectionCount, uint32_t platformsCount, std::span<const Header::LinkerOption> linkerOptions);
#endif

private:
    friend class Image;
    friend class RebaseOpcodes;
    friend class BindOpcodes;
    friend class LazyBindOpcodes;
    friend class ChainedFixups;

    bool            entryAddrFromThreadCmd(const thread_command* cmd, uint64_t& addr) const;

    bool            hasMachOBigEndianMagic() const;
    void            removeLoadCommand(void (^callback)(const load_command* cmd, bool& remove, bool& stop));
    bool            hasLoadCommand(uint32_t lc) const;
    void            forEachPlatformLoadCommand(void (^callback)(Platform platform, Version32 minOS, Version32 sdk)) const;
    Error           validSemanticsPlatform() const;
    Error           validSemanticsInstallName(const Policy& policy) const;
    Error           validSemanticsDependents(const Policy& policy) const;
    Error           validSemanticsLinkerOptions(const Policy& policy) const;
    Error           validSemanticsUUID(const Policy& policy) const;
    Error           validSemanticsRPath(const Policy& policy) const;
    Error           validSemanticsSegments(const Policy& policy, uint64_t fileSize) const;
    Error           validSemanticsSegmentInIsolation(const Policy& policy, uint64_t wholeFileSize, const segment_command_64* segLC) const;
    Error           validSemanticsMain(const Policy& policy) const;

    load_command*   firstLoadCommand();
    load_command*   appendLoadCommand(uint32_t cmd, uint32_t cmdSize);
    void            appendLoadCommand(const load_command* lc);
    void            addBuildVersion(Platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools);
    void            addMinVersion(Platform, Version32 minOS, Version32 sdk);

    template <typename SG, typename SC>
    Error           validSegment(const Policy& policy, uint64_t wholeFileSize, const SG* seg) const;

    const encryption_info_command* findFairPlayEncryptionLoadCommand() const;

    static DependentDylibAttributes   loadCommandToDylibKind(const dylib_command* dylibCmd);

    mach_header  mh;
};



} // namespace mach_o

#endif /* mach_o_Header_h */
