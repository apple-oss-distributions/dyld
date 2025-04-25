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

#include <stdio.h>
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

#ifndef LC_ATOM_INFO
  #define LC_ATOM_INFO      0x36
#endif

#ifndef LC_FUNCTION_VARIANTS
  #define LC_FUNCTION_VARIANTS      0x37
#endif

#ifndef LC_FUNCTION_VARIANT_FIXUPS
  #define LC_FUNCTION_VARIANT_FIXUPS      0x38
#endif

#ifndef EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT
  #define EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT  0x20
#endif


#ifndef VM_PROT_READ
    #define VM_PROT_READ    0x01
#endif

#ifndef VM_PROT_WRITE
    #define VM_PROT_WRITE   0x02
#endif

#ifndef VM_PROT_EXECUTE
    #define VM_PROT_EXECUTE 0x04
#endif

#ifndef LC_TARGET_TRIPLE
    #define LC_TARGET_TRIPLE 0x39
    struct target_triple_command {
        uint32_t     cmd;        /* LC_TARGET_TRIPLE */
        uint32_t     cmdsize;    /* including string */
        union lc_str triple;     /* target triple string */
    };
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
 * @struct LinkedDylibAttributes
 *
 * @abstract
 *      Attributes of how a dylib can be linked
 */
struct VIS_HIDDEN LinkedDylibAttributes
{
    constexpr   LinkedDylibAttributes() : raw(0) {}
    union {
        struct {
            bool    weakLink  : 1 = false;
            bool    reExport  : 1 = false;
            bool    upward    : 1 = false;
            bool    delayInit : 1 = false;
            uint8_t padding   : 4 = 0;
        };
        uint8_t     raw;
    };

    void    toString(char buf[64]) const;
    static const LinkedDylibAttributes regular;
    static const LinkedDylibAttributes justWeakLink;
    static const LinkedDylibAttributes justUpward;
    static const LinkedDylibAttributes justReExport;
    static const LinkedDylibAttributes justDelayInit;
private:
    constexpr   LinkedDylibAttributes(uint8_t v) : raw(v) {}
};
static_assert(sizeof(LinkedDylibAttributes) == 1);

VIS_HIDDEN inline bool operator==(const LinkedDylibAttributes& a, LinkedDylibAttributes b) { return (a.raw == b.raw); }


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
    uint32_t        ncmds() const;
    bool            uses16KPages() const;
    bool            is64() const;
    bool            isDyldManaged() const;
    bool            isDylib() const;
    bool            isDylibStub() const;
    bool            isDylibOrStub() const;
    bool            isBundle() const;
    bool            isMainExecutable() const;
    bool            isDynamicExecutable() const;
    bool            isStaticExecutable() const;
    bool            isDylinker() const;
    bool            isKextBundle() const;
    bool            isObjectFile() const;
    bool            isFileSet() const;
    bool            isPreload() const;
    bool            isPIE() const;
    bool            usesTwoLevelNamespace() const;
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
    bool                builtForSimulator() const;
    void                forEachBuildTool(void (^handler)(Platform platform, uint32_t tool, uint32_t version)) const;
    bool                getUuid(uuid_t uuid) const;
    bool                sourceVersion(Version64& version) const;
    bool                getDylibInstallName(const char** installName, Version32* compatVersion, Version32* currentVersion) const;
    const char*         installName() const;  // returns nullptr is no install name
    const char*         umbrellaName() const; // returns nullptr if dylib is not in an umbrella
    void                forEachLinkedDylib(void (^callback)(const char* loadPath, LinkedDylibAttributes kind,
                                                            Version32 compatVersion, Version32 curVersion,
                                                            bool synthesizedLink, bool& stop)) const;
    const char*         linkedDylibLoadPath(uint32_t depIndex) const;
    uint32_t            linkedDylibCount(bool* allDepsAreRegular = nullptr) const;
    bool                canBeFairPlayEncrypted() const;
    bool                hasEncryptionInfo(uint32_t& cryptId, uint32_t& textOffset, uint32_t& size) const;
    bool                isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const;
    bool                hasChainedFixups() const;
    bool                hasChainedFixupsLoadCommand() const;
    bool                hasOpcodeFixups() const;
    bool                hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool                hasLinkerOptimizationHints(uint32_t& offset, uint32_t& size) const;
    bool                getEntry(uint64_t& offset, bool& usesCRT) const;
    bool                hasIndirectSymbolTable(uint32_t& fileOffset, uint32_t& count) const;
    bool                hasSplitSegInfo(bool& isMarker) const;
    bool                hasAtomInfo(uint32_t& fileOffset, uint32_t& count) const;
    CString             libOrdinalName(int libOrdinal) const;
    const load_command* findLoadCommand(uint32_t& index, bool (^predicate)(const load_command* lc)) const;
    void                findLoadCommandRange(uint32_t& startIndex, uint32_t& endIndex, bool (^predicate)(const load_command* lc)) const;
    void                printLoadCommands(FILE* out=stdout, unsigned indentLevel=0) const;
    bool                hasFunctionVariantFixups() const;
    bool                loadableIntoProcess(Platform processPlatform, CString path, bool internalInstall=false) const;
    bool                hasPlusLoadMethod() const;
    void                forEachSingletonPatch(void (^handler)(uint64_t runtimeOffset)) const;
    bool                hasObjCMessageReferences() const;
    bool                findObjCDataSection(CString sectionName, uint64_t& sectionRuntimeOffset, uint64_t& sectionSize) const;
    CString             targetTriple() const; // returns empty string if no triple specified
    bool                hasFunctionsVariantTable(uint64_t& runtimeOffset) const;

    // load command helpers
    static const dylib_command* isDylibLoadCommand(const load_command* lc);
    const thread_command*       unixThreadLoadCommand() const;
    uint32_t                    sizeForLinkedDylibCommand(const char* path, LinkedDylibAttributes depAttrs, uint32_t& traditionalCmd) const;

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
    uint64_t         segmentVmAddr(uint32_t segIndex) const;
    uint64_t         segmentVmSize(uint32_t segIndex) const;
    uint32_t         segmentFileOffset(uint32_t segIndex) const;
    
    struct SegmentInfo {
        std::string_view    segmentName;
        uint64_t            vmaddr              = 0;
        uint64_t            vmsize              = 0;
        uint32_t            fileOffset          = 0;
        uint32_t            fileSize            = 0;
        uint32_t            flags               = 0;
        uint16_t            segmentIndex        = 0;
        uint8_t             maxProt             = 0;
        uint8_t             initProt            = 0;
        
        bool        readOnlyData() const    { return flags & SG_READ_ONLY; }
        bool        isProtected() const     { return flags & SG_PROTECTED_VERSION_1; }
        bool        executable() const      { return initProt & VM_PROT_EXECUTE; }
        bool        writable() const        { return initProt & VM_PROT_WRITE; }
        bool        readable() const        { return initProt & VM_PROT_READ; }
        bool        hasZeroFill() const     { return (initProt == 3) && (fileSize < vmsize); }
    };
    void             forEachSegment(void (^callback)(const SegmentInfo& info, bool& stop)) const;
    void             forEachSegment(void (^callback)(const SegmentInfo& info, uint64_t sizeOfSections, uint32_t maxAlignOfSections, bool& stop)) const;
    struct SectionInfo {
        std::string_view    sectionName;
        std::string_view    segmentName;
        uint32_t            segIndex        = 0;
        uint32_t            segMaxProt      = 0;
        uint32_t            segInitProt     = 0;
        uint32_t            flags           = 0;
        uint32_t            alignment       = 0;
        uint64_t            address         = 0;
        uint64_t            size            = 0;
        uint32_t            fileOffset      = 0;
        uint32_t            relocsOffset    = 0;
        uint32_t            relocsCount     = 0;
        uint32_t            reserved1       = 0;
        uint32_t            reserved2       = 0;
    };
    void             forEachSection(void (^callback)(const SectionInfo&, bool& stop)) const;
    void             forEachSection(void (^callback)(const SegmentInfo&, const SectionInfo&, bool& stop)) const;
    void             forEachInterposingSection(void (^callback)(const SectionInfo&, bool& stop)) const;
    std::span<const uint8_t> findSectionContent(CString segName, CString sectName, bool useVmOffset) const;
    static uint32_t  threadLoadCommandsSize(const Architecture& arch);
    uint32_t         threadLoadCommandsSize() const;
    uint32_t         headerAndLoadCommandsSize() const;
    static uint32_t  pointerAligned(bool is64, uint32_t value);
    uint32_t         pointerAligned(uint32_t value) const;
    uint32_t         fileSize() const;
    const uint8_t*   computeLinkEditBias(bool zeroFillExpanded) const;
    bool             hasZerofillExpansion() const;
    uint64_t         zerofillExpansionAmount() const;
    bool             hasCustomStackSize(uint64_t& size) const;
    bool             hasFirmwareChainStarts(uint16_t* pointerFormat=nullptr, uint32_t* startsCount=nullptr, const uint32_t** starts=nullptr) const;
    bool             hasFirmwareRebaseRuns() const;
    bool             forEachFirmwareRebaseRuns(void (^callback)(uint32_t offset, bool& stop)) const;
    static const char* protectionString(uint32_t flags, char str[8]);

private:
    friend class Image;
    friend class RebaseOpcodes;
    friend class BindOpcodes;
    friend class LazyBindOpcodes;
    friend class ChainedFixups;

    bool            entryAddrFromThreadCmd(const thread_command* cmd, uint64_t& addr) const;

    bool            hasLoadCommand(uint32_t lc) const;
    void            forEachPlatformLoadCommand(void (^callback)(Platform platform, Version32 minOS, Version32 sdk)) const;
    Error           validSemanticsPlatform() const;
    Error           validSemanticsInstallName(const Policy& policy) const;
    Error           validSemanticsLinkedDylibs(const Policy& policy) const;
    Error           validSemanticsLinkerOptions(const Policy& policy) const;
    Error           validSemanticsUUID(const Policy& policy) const;
    Error           validSemanticsRPath(const Policy& policy) const;
    Error           validSemanticsSegments(const Policy& policy, uint64_t fileSize) const;
    Error           validSemanticsSegmentInIsolation(const Policy& policy, uint64_t wholeFileSize, const segment_command_64* segLC) const;
    Error           validSemanticsMain(const Policy& policy) const;

    template <typename SG, typename SC>
    Error           validSegment(const Policy& policy, uint64_t wholeFileSize, const SG* seg) const;

    static std::string_view        name16(const char name[16]);

    const encryption_info_command* findFairPlayEncryptionLoadCommand() const;

    static LinkedDylibAttributes   loadCommandToDylibKind(const dylib_command* dylibCmd);

protected:
    bool            hasMachOBigEndianMagic() const;

protected:
    mach_header  mh;
};



} // namespace mach_o

#endif /* mach_o_Header_h */
