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

#ifndef MachOLayout_hpp
#define MachOLayout_hpp

#include "Defines.h"
#include "Diagnostics.h"

#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <optional>
#include <span>
#include <string_view>

namespace dyld3 {
struct MachOFile;
}

namespace mach_o
{

// Wrap the mach-o in a struct to prevent accidentally doing math on it.
// Specifically, we are no longer mapping everything in the VM layout, with zero fill,
// so VM offsets from the mh* don't work.
struct VIS_HIDDEN MachOFileRef
{
    MachOFileRef(const dyld3::MachOFile* mf)
        : mf(mf)
    {
    }
    MachOFileRef() = delete;
    ~MachOFileRef() = default;
    MachOFileRef(const MachOFileRef&) = default;
    MachOFileRef& operator=(const MachOFileRef&) = default;
    MachOFileRef(MachOFileRef&&) = default;
    MachOFileRef& operator=(MachOFileRef&&) = default;

    __attribute__((nodebug))
    const dyld3::MachOFile* operator->() const
    {
        return this->mf;
    }

    __attribute__((nodebug))
    dyld3::MachOFile* operator->()
    {
        return (dyld3::MachOFile*)this->mf;
    }

    const uint8_t* getOffsetInToFile(uint64_t offset) const
    {
        return (uint8_t*)this->mf + offset;
    }

    bool operator !=(const dyld3::MachOFile* other) const {
        return this->mf != other;
    }

    bool operator !=(const MachOFileRef& other) const {
        return this->mf != other.mf;
    }

private:
    const dyld3::MachOFile* mf;
};

struct VIS_HIDDEN SegmentLayout
{
    enum class Kind
    {
        // TODO: Fill in other entries if we need them
        unknown,
        text,
        linkedit
    };

    uint64_t        vmAddr          = 0;
    uint64_t        vmSize          = 0;
    uint64_t        fileOffset      = 0;
    uint64_t        fileSize        = 0;
    const uint8_t*  buffer          = nullptr;
    uint32_t        protections     = 0;
    Kind            kind            = Kind::unknown;

    bool readable() const   { return protections & VM_PROT_READ; }
    bool writable() const   { return protections & VM_PROT_WRITE; }
    bool executable() const { return protections & VM_PROT_EXECUTE; }
};

// Contains pointers to all the pieces of LINKEDIT
struct VIS_HIDDEN Linkedit
{
    uint32_t        fileOffset  = 0;

    const uint8_t*  buffer      = nullptr;
    uint32_t        bufferSize  = 0;

    // Some LINKEDIT, eg, LC_DYSYMTAB::ilocalsym is an index in to another buffer
    uint32_t        entryIndex  = 0;

    // Some LINKEDIT, eg, symbol table, wants to know the count of the number of strings
    uint32_t        entryCount  = 0;

    bool            hasLinkedit = false;

    __attribute__((nodebug))
    bool hasValue() const
    {
        return hasLinkedit;
    }
};

struct VIS_HIDDEN ChainedFixupsLinkedit : Linkedit
{
    const linkedit_data_command* cmd = nullptr;
};

struct VIS_HIDDEN LinkeditLayout
{
    // LC_DYSYMTAB::locreloff
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nlocrel
    Linkedit                localRelocs;

    // LC_DYSYMTAB::extreloff
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nextrel
    Linkedit                externRelocs;

    // LC_DYSYMTAB::indirectsymoff
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nindirectsyms
    Linkedit                indirectSymbolTable;

    // LC_DYSYMTAB::ilocalsym
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nlocalsym
    Linkedit                localSymbolTable;

    // LC_DYSYMTAB::iextdefsym
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nlocalsym
    Linkedit                globalSymbolTable;

    // LC_DYSYMTAB::iundefsym
    // Note Linkedit::entryCount is set here, and is LC_DYSYMTAB::nlocalsym
    Linkedit                undefSymbolTable;

    // LC_SYMTAB::symoff
    // Note Linkedit::entryCount is set here, and is LC_SYMTAB::nsyms
    Linkedit                symbolTable;

    // LC_SYMTAB::stroff
    Linkedit                symbolStrings;

    // LC_DYLD_INFO::rebase_off
    Linkedit                rebaseOpcodes;

    // LC_DYLD_INFO::bind_off
    Linkedit                regularBindOpcodes;

    // LC_DYLD_INFO::weak_bind_off
    Linkedit                weakBindOpcodes;

    // LC_DYLD_INFO::lazy_bind_off
    Linkedit                lazyBindOpcodes;

    // LC_DYLD_CHAINED_FIXUPS
    ChainedFixupsLinkedit   chainedFixups;

    // LC_DYLD_EXPORTS_TRIE or LC_DYLD_INFO::export_offs
    Linkedit                exportsTrie;

    // LC_SEGMENT_SPLIT_INFO
    Linkedit                splitSegInfo;

    // LC_FUNCTION_STARTS
    Linkedit                functionStarts;

    // LC_FUNCTION_STARTS
    Linkedit                dataInCode;

    // LC_CODE_SIGNATURE
    Linkedit                codeSignature;

    // For isValidLinkeditLayout, record some details of what load commands we had
    uint32_t                dyldInfoCmd     = 0;
    bool                    hasSymTab       = false;
    bool                    hasDynSymTab    = false;
};

struct VIS_HIDDEN Layout
{
    Layout(const MachOFileRef mf, std::span<SegmentLayout> segments, const LinkeditLayout& linkedit);

    uint64_t textUnslidVMAddr() const;

    // FIXME: Should we have a SectionContent or similar class?
    bool isSwiftLibrary() const;
    bool hasSection(std::string_view segmentName, std::string_view sectionName) const;

    // This used to live in MachOAnalyzer, but we can validate everything based on the above struct
    bool isValidLinkeditLayout(Diagnostics& diag, const char* path) const;

    struct FoundSymbol {
        enum class Kind { headerOffset, absolute, resolverOffset };
        Kind                        kind;
        bool                        isThreadLocal;
        bool                        isWeakDef;
        std::optional<MachOFileRef> foundInDylib;
        uint64_t                    value;
        uint32_t                    resolverFuncOffset;
        const char*                 foundSymbolName;
    };

    // This is a terrible place for this, but we want to use the Layout to build other values like
    // the trie or symbol table
    bool findExportedSymbol(Diagnostics& diag, const char* symbolName, bool weakImport,
                            FoundSymbol& foundInfo) const;

    const MachOFileRef          mf;
    std::span<SegmentLayout>    segments;
    const LinkeditLayout&       linkedit;
};

// For use with new rebase/bind scheme were each fixup location on disk contains info on what
// fix up it needs plus the offset to the next fixup.
union VIS_HIDDEN ChainedFixupPointerOnDisk
{
    union Arm64e {
        dyld_chained_ptr_arm64e_auth_rebase authRebase;
        dyld_chained_ptr_arm64e_auth_bind   authBind;
        dyld_chained_ptr_arm64e_rebase      rebase;
        dyld_chained_ptr_arm64e_bind        bind;
        dyld_chained_ptr_arm64e_bind24      bind24;
        dyld_chained_ptr_arm64e_auth_bind24 authBind24;

        uint64_t            signExtendedAddend() const;
        uint64_t            unpackTarget() const;
        const char*         keyName() const;
        uint64_t            signPointer(void* loc, uint64_t target) const;
        static uint64_t     signPointer(uint64_t unsignedPtr, void* loc, bool addrDiv, uint16_t diversity, uint8_t key);
        static const char*  keyName(uint8_t keyBits);
    };

    union Generic64 {
        dyld_chained_ptr_64_rebase rebase;
        dyld_chained_ptr_64_bind   bind;

        uint64_t        signExtendedAddend() const;
        uint64_t        unpackedTarget() const;
    };

    union Generic32 {
        dyld_chained_ptr_32_rebase rebase;
        dyld_chained_ptr_32_bind   bind;

        uint64_t        signExtendedAddend() const;
    };

    struct Kernel64 : dyld_chained_ptr_64_kernel_cache_rebase {
        const char* keyName() const;
    };

    struct Firm32 : dyld_chained_ptr_32_firmware_rebase { };

    typedef dyld_chained_ptr_32_cache_rebase Cache32;

    uint64_t            raw64;
    Arm64e              arm64e;
    Generic64           generic64;
    Kernel64            kernel64;

    uint32_t            raw32;
    Generic32           generic32;
    Cache32             cache32;
    Firm32              firmware32;

    bool                isRebase(uint16_t pointerFormat, uint64_t preferedLoadAddress, uint64_t& targetRuntimeOffset) const;
    bool                isBind(uint16_t pointerFormat, uint32_t& bindOrdinal, int64_t& addend) const;
    static unsigned     strideSize(uint16_t pointerFormat);
};

struct VIS_HIDDEN Fixups
{
    Fixups(const Layout& layout);

    struct BindTargetInfo {
        unsigned    targetIndex;
        int         libOrdinal;
        const char* symbolName;
        uint64_t    addend;
        bool        weakImport;
        bool        lazyBind;
    };

    enum class Rebase {
        unknown,
        pointer32,
        pointer64,
        textPCrel32,
        textAbsolute32,
    };

    void forEachBindTarget(Diagnostics& diag, bool allowLazyBinds, intptr_t slide,
                           void (^handler)(const BindTargetInfo& info, bool& stop),
                           void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const;

    void forEachBindTarget_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                   void (^handler)(const BindTargetInfo& info, bool& stop),
                                   void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const;
    void forEachBindTarget_ChainedFixups(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const;
    void forEachBindTarget_Relocations(Diagnostics& diag, intptr_t slide, void (^handler)(const BindTargetInfo& info, bool& stop)) const;

    void forEachBindLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                                        unsigned targetIndex, bool& stop),
                                     void (^overrideHandler)(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                             unsigned overrideBindTargetIndex, bool& stop)) const;
    void forEachBindLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex,
                                                                            bool& stop)) const;

    bool forEachRebaseLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, bool& stop)) const;
    void forEachRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, uint64_t rebasedValue, bool& stop)) const;
    bool forEachRebaseLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, bool& stop)) const;

    void withThreadedRebaseAsChainStarts(Diagnostics& diag, void (^callback)(const dyld_chained_fixups_header* header, uint64_t fixupsSize)) const;
    const dyld_chained_fixups_header* chainedFixupsHeader() const;
    void withChainStarts(Diagnostics& diag, void (^callback)(const dyld_chained_starts_in_image*)) const;
    void forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const;
    void forEachFixupInAllChains(Diagnostics& diag, const dyld_chained_starts_in_image* starts, bool notifyNonPointers,
                                 void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop)) const;
    void forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool notifyNonPointers,
                                     void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, uint64_t fixupSegmentOffset, bool& stop)) const;

    static void forEachFixupChainSegment(Diagnostics& diag, const dyld_chained_starts_in_image* starts,
                                         void (^handler)(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stop));

    uint16_t chainedPointerFormat() const;

private:
    typedef void (^BindDetailedHandler)(const char* opcodeName,
                                        bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                        uint32_t pointerSize, uint32_t segmentIndex, uint64_t segmentOffset,
                                        uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                        uint64_t addend, bool targetOrAddendChanged, bool& stop);
    typedef void (^RebaseDetailHandler)(const char* opcodeName, bool segIndexSet, uint32_t pointerSize,
                                        uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind,
                                        bool& stop);

    void        parseOrgArm64eChainedFixups(Diagnostics& diag, void (^targetCount)(uint32_t totalTargets, bool& stop),
                                            void (^addTarget)(bool libraryOrdinalSet, uint32_t dylibCount,
                                                              int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                            void (^addChainStart)(uint32_t segmentIndex, bool segIndexSet,
                                                                  uint64_t segmentOffset, uint16_t format, bool& stop)) const;
    void        forEachBindUnified_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                           void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop),
                                           void (^overrideHandler)(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop)) const;

    bool        forEachBind_OpcodesLazy(Diagnostics& diag, BindDetailedHandler) const;
    bool        forEachBind_OpcodesWeak(Diagnostics& diag, BindDetailedHandler,  void (^strongHandler)(const char* symbolName)) const;
    bool        forEachBind_OpcodesRegular(Diagnostics& diag, BindDetailedHandler) const;

    bool        forEachBind_Relocations(Diagnostics& diag, bool supportPrivateExternsWorkaround,
                                        intptr_t slide, BindDetailedHandler) const;
    bool        forEachRebase_Relocations(Diagnostics& diag, RebaseDetailHandler) const;

    void        forEachIndirectPointer(Diagnostics& diag, bool supportPrivateExternsWorkaround, intptr_t slide,
                                       void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal,
                                                       const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const;

    bool        forEachRebase_Opcodes(Diagnostics& diag, RebaseDetailHandler) const;

    int         libOrdinalFromDesc(uint16_t n_desc) const;
    uint64_t    localRelocBaseAddress() const;
    uint64_t    externalRelocBaseAddress() const;
    bool        segIndexAndOffsetForAddress(uint64_t addr, uint32_t& segIndex, uint64_t& segOffset) const;

    const Layout& layout;
};

struct VIS_HIDDEN SymbolTable
{
    SymbolTable(const Layout& layout);

    void forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void forEachImportedSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const;
    void forEachIndirectSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint32_t symNum)) const;

private:
    const Layout& layout;
};

struct VIS_HIDDEN SplitSeg
{
    SplitSeg(const Layout& layout);

    bool isV1() const;
    bool isV2() const;
    bool hasValue() const;

    typedef void (^ReferenceCallbackV2)(uint64_t fromSectionIndex, uint64_t fromSectionOffset,
                                        uint64_t toSectionIndex, uint64_t toSectionOffset,
                                        bool& stop);
    void forEachReferenceV2(Diagnostics& diag, ReferenceCallbackV2 callback) const;

    void forEachSplitSegSection(void (^callback)(std::string_view segmentName,
                                                 std::string_view sectionName,
                                                 uint64_t sectionVMAddr)) const;

private:
    const Layout& layout;
};

struct VIS_HIDDEN ExportTrie
{
    ExportTrie(const Layout& layout);

    typedef void (^ExportsCallback)(const char* symbolName, uint64_t imageOffset, uint64_t flags,
                                    uint64_t other, const char* importName, bool& stop);
    void forEachExportedSymbol(Diagnostics& diag, ExportsCallback callback) const;

private:
    const Layout& layout;
};

} // namespace mach_o

#endif /* MachOLayout_hpp */
