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

#ifndef MachOAnalyzer_h
#define MachOAnalyzer_h

#include <TargetConditionals.h>
#include "Defines.h"
#if SUPPORT_CLASSIC_RELOCS
  #include <mach-o/reloc.h>
#endif

#include "MachOLoaded.h"
#include "Array.h"
#include "Header.h"
#include "Platform.h"

#if !TARGET_OS_EXCLAVEKIT
  #include "ClosureFileSystem.h"
#endif

#if __has_feature(ptrauth_calls)
#define __ptrauth_dyld_pointer __ptrauth(0, 0, 0)
#else
#define __ptrauth_dyld_pointer
#endif

namespace dyld3 {

typedef int (*DyldLookFunc)(const char*, void**);

// Extra functionality on loaded mach-o files only used during closure building
struct VIS_HIDDEN MachOAnalyzer : public MachOLoaded
{
    // protected members of subclass promoted to public here
    using MachOLoaded::FoundSymbol;
    using MachOLoaded::findExportedSymbol;
    using MachOLoaded::forEachGlobalSymbol;
    using MachOLoaded::forEachImportedSymbol;
    using MachOLoaded::forEachLocalSymbol;
    using MachOFile::forEachLoadCommand;

    enum class Rebase {
        unknown,
        pointer32,
        pointer64,
        textPCrel32,
        textAbsolute32,
    };

#if !TARGET_OS_EXCLAVEKIT
    static bool loadFromBuffer(Diagnostics& diag, const closure::FileSystem& fileSystem,
                               const char* path, const GradedArchs& archs, mach_o::Platform platform,
                               closure::LoadedFileInfo& info);
    static closure::LoadedFileInfo load(Diagnostics& diag, const closure::FileSystem& fileSystem,
                                        const char* logicalPath, const GradedArchs& archs, mach_o::Platform platform, char realerPath[PATH_MAX]);
#endif
    bool  isValidMainExecutable(Diagnostics& diag, const char* path, uint64_t sliceLength,
                                                     const GradedArchs& archs, mach_o::Platform platform) const;

    typedef void (^ExportsCallback)(const char* symbolName, uint64_t imageOffset, uint64_t flags,
                                    uint64_t other, const char* importName, bool& stop);
    bool                validMachOForArchAndPlatform(Diagnostics& diag, size_t mappedSize, const char* path, const GradedArchs& archs, mach_o::Platform platform, bool isOSBinary, bool internalInstall=false) const;

    // Caches data useful for converting from raw data to VM addresses
    struct VMAddrConverter {
        uint64_t preferredLoadAddress                       = 0;
        intptr_t slide                                      = 0;
        uint16_t chainedPointerFormat                       = 0;
        bool contentRebased                                 = false;
#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
        enum class SharedCacheFormat : uint8_t {
            none            = 0,
            v1             = 1,
            v2_x86_64_tbi   = 2,
            v3              = 3,
            v4              = 4,
            v5              = 5,
        };
        SharedCacheFormat sharedCacheChainedPointerFormat   = SharedCacheFormat::none;
#endif

        uint64_t convertToVMAddr(uint64_t v) const;
        uint64_t convertToVMAddr(uint64_t v, const Array<uint64_t>& bindTargets) const;
    };

    VMAddrConverter     makeVMAddrConverter(bool contentRebased) const;
    bool                hasSwiftOrObjC(bool* hasSwift = nullptr) const;
    bool                hasSwift() const;
    bool                usesObjCGarbageCollection() const;
    void                forEachCDHash(void (^handler)(const uint8_t cdHash[20])) const;
    bool                usesLibraryValidation() const;
    bool                isSlideable() const;
    void                forEachInitializer(Diagnostics& diag, const VMAddrConverter& vmAddrConverter, void (^callback)(uint32_t offset), const void* dyldCache=nullptr) const;
    bool                hasTerminators(Diagnostics& diag, const VMAddrConverter& vmAddrConverter) const;
    void                forEachTerminator(Diagnostics& diag, const VMAddrConverter& vmAddrConverter, void (^callback)(uint32_t offset)) const;
    void                forEachExportedSymbol(Diagnostics& diag, ExportsCallback callback) const;
    void                forEachWeakDef(Diagnostics& diag, void (^callback)(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset,
                                                    uint64_t addend, const char* symbolName, bool& stop)) const;
    void                forEachIndirectPointer(Diagnostics& diag, bool supportPrivateExternsWorkaround,
                                               void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal,
                                                               const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const;
    const void*         content(uint64_t vmOffset);
    void                forEachLocalReloc(void (^handler)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachExternalReloc(void (^handler)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName, bool& stop)) const;

    const void*         getRebaseOpcodes(uint32_t& size) const;
    const void*         getBindOpcodes(uint32_t& size) const;
    const void*         getLazyBindOpcodes(uint32_t& size) const;
    const void*         getSplitSeg(uint32_t& size) const;
    bool                hasSplitSeg() const;
    bool                isSplitSegV1() const;
    bool                isSplitSegV2() const;
    uint64_t            segAndOffsetToRuntimeOffset(uint8_t segIndex, uint64_t segOffset) const;
    bool                hasLazyPointers(uint32_t& runtimeOffset, uint32_t& size) const;
    void                forEachRebase(Diagnostics& diag, bool ignoreLazyPointer, void (^callback)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool isLazyPointerRebase, bool& stop)) const;
    void                forEachTextRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool& stop)) const;
    void                forEachBind(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName,
                                                                        bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                    void (^strongHandler)(const char* symbolName)) const;
    void                forEachBind(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, int libOrdinal, uint8_t type, const char* symbolName,
                                                                        bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                    void (^strongHandler)(const char* symbolName)) const;
    void                forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const;
    void                forEachRebase(Diagnostics& diag, void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                                                             bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop)) const;
    void                forEachBind(Diagnostics& diag, void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                                                       bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                                                       uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                                                       uint8_t type, const char* symbolName, bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                                       void (^strongHandler)(const char* symbolName)) const;
#if BUILDING_APP_CACHE_UTIL
    bool                canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const;
#endif

#if DEBUG
    void                validateDyldCacheDylib(Diagnostics& diag, const char* path) const;
#endif
    void                withChainStarts(Diagnostics& diag, uint64_t startsStructOffsetHint, void (^callback)(const dyld_chained_starts_in_image*)) const;
    uint64_t            chainStartsOffset() const;
    uint16_t            chainedPointerFormat() const;
    bool                hasUnalignedPointerFixups() const;
    const dyld_chained_fixups_header* chainedFixupsHeader() const;
    bool                hasFirmwareChainStarts(uint16_t* pointerFormat, uint32_t* startsCount, const uint32_t** starts) const;
    bool                hasRebaseRuns(const void** runs, size_t* runsSize) const;
    void                forEachRebaseRunAddress(const void* runs, size_t runsSize, void (^handler)(uint32_t address)) const;
    bool                isOSBinary(int fd, uint64_t sliceOffset, uint64_t sliceSize) const;  // checks if binary is codesigned to be part of the OS
    static bool         sliceIsOSBinary(int fd, uint64_t sliceOffset, uint64_t sliceSize);

#if !TARGET_OS_EXCLAVEKIT
    const MachOAnalyzer*    remapIfZeroFill(Diagnostics& diag, const closure::FileSystem& fileSystem, closure::LoadedFileInfo& info) const;
#endif
    bool                    neverUnload() const;
#if SUPPORT_CLASSIC_RELOCS
    void                    sortRelocations(Array<relocation_info>& relocs) const;
#endif

    struct ObjCInfo {
        uint32_t    selRefCount;
        uint32_t    classDefCount;
        uint32_t    protocolDefCount;
    };
    ObjCInfo            getObjCInfo() const;

    struct ObjCClassInfo {
        // These fields are all present on the objc_class_t struct
        uint64_t isaVMAddr                                  = 0;
        uint64_t superclassVMAddr                           = 0;
        //uint64_t methodCacheBuckets;
        uint64_t methodCacheVMAddr                          = 0;
        uint64_t dataVMAddr                                 = 0;

        // This field is only present if this is a Swift object, ie, has the Swift
        // fast bits set
        uint32_t swiftClassFlags                            = 0;

        // These are taken from the low bits of the dataVMAddr value
        bool     isSwiftLegacy                              = false;
        bool     isSwiftStable                              = false;

        // Cache the data to convert vmAddr's
        MachOAnalyzer::VMAddrConverter  vmAddrConverter;

        // These are from the class_ro_t which data points to
        enum class ReadOnlyDataField {
            name,
            baseProtocols,
            baseMethods,
            baseProperties,
            flags
        };

        uint64_t getReadOnlyDataField(ReadOnlyDataField field, uint32_t pointerSize) const;
        uint64_t nameVMAddr(uint32_t pointerSize) const {
            return getReadOnlyDataField(ReadOnlyDataField::name, pointerSize);
        }
        uint64_t baseProtocolsVMAddr(uint32_t pointerSize) const {
            return getReadOnlyDataField(ReadOnlyDataField::baseProtocols, pointerSize);
        }
        uint64_t baseMethodsVMAddr(uint32_t pointerSize) const {
            return getReadOnlyDataField(ReadOnlyDataField::baseMethods, pointerSize);
        }
        uint64_t basePropertiesVMAddr(uint32_t pointerSize) const {
            return getReadOnlyDataField(ReadOnlyDataField::baseProperties, pointerSize);
        }
        uint64_t flags(uint32_t pointerSize) const {
            return getReadOnlyDataField(ReadOnlyDataField::flags, pointerSize);
        }

        // These are embedded in the Mach-O itself by the compiler
        enum FastDataBits {
            FAST_IS_SWIFT_LEGACY    = 0x1,
            FAST_IS_SWIFT_STABLE    = 0x2
        };

        // These are embedded by the Swift compiler in the swiftClassFlags field
        enum SwiftClassFlags {
            isSwiftPreStableABI     = 0x1
        };

        // Note this is taken from the objc runtime
        bool isUnfixedBackwardDeployingStableSwift() const {
            // Only classes marked as Swift legacy need apply.
            if (!isSwiftLegacy) return false;

            // Check the true legacy vs stable distinguisher.
            // The low bit of Swift's ClassFlags is SET for true legacy
            // and UNSET for stable pretending to be legacy.
            bool isActuallySwiftLegacy = (swiftClassFlags & isSwiftPreStableABI) != 0;
            return !isActuallySwiftLegacy;
        }
    };

    struct ObjCMethodList {
        // This matches the bits in the objc runtime
        enum : uint32_t {
            methodListIsUniqued = 0x1,
            methodListIsSorted  = 0x2,

            // The size is bits 2 through 16 of the entsize field
            // The low 2 bits are uniqued/sorted as above.  The upper 16-bits
            // are reserved for other flags
            methodListSizeMask  = 0x0000FFFC
        };
    };

    struct ObjCImageInfo {
        uint32_t version;
        uint32_t flags;

        // FIXME: Put this somewhere objc can see it.
        enum : uint32_t {
            dyldPreoptimized = 1 << 7
        };
    };

    struct ObjCMethod {
        uint64_t nameVMAddr;   // & SEL
        uint64_t typesVMAddr;  // & const char *
        uint64_t impVMAddr;    // & IMP

        // We also need to know where the reference to the nameVMAddr was
        // This is so that we know how to rebind that location
        uint64_t nameLocationVMAddr;
    };

    struct ObjCProperty {
        uint64_t nameVMAddr;        // & const char *
        uint64_t attributesVMAddr;  // & const char *
    };

    struct ObjCCategory {
        uint64_t nameVMAddr;
        uint64_t clsVMAddr;
        uint64_t instanceMethodsVMAddr;
        uint64_t classMethodsVMAddr;
        uint64_t protocolsVMAddr;
        uint64_t instancePropertiesVMAddr;
    };

    struct ObjCProtocol {
        uint64_t isaVMAddr;
        uint64_t nameVMAddr;
        uint64_t protocolsVMAddr;
        uint64_t instanceMethodsVMAddr;
        uint64_t classMethodsVMAddr;
        uint64_t optionalInstanceMethodsVMAddr;
        uint64_t optionalClassMethodsVMAddr;
        //uint64_t instancePropertiesVMAddr;
        //uint32_t size;
        //uint32_t flags;
        // Fields below this point are not always present on disk.
        //uint64_t extendedMethodTypesVMAddr;
        //uint64_t demangledNameVMAddr;
        //uint64_t classPropertiesVMAddr;
    };

    enum class PrintableStringResult {
        CanPrint,
        FairPlayEncrypted,
        ProtectedSection,
        UnknownSection
    };

    const char* getPrintableString(uint64_t stringVMAddr, PrintableStringResult& result) const;

    void parseObjCClass(const VMAddrConverter& vmAddrConverter,
                        uint64_t classVMAddr, const Array<uint64_t>& bindTargets,
                        void (^handler)(uint64_t classSuperclassVMAddr,
                                        uint64_t classDataVMAddr,
                                        const ObjCClassInfo& objcClass)) const;

    bool isSwiftClass(const void* classPtr) const;

    typedef void (^ClassCallback)(uint64_t classVMAddr,
                                  uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr,
                                  const ObjCClassInfo& objcClass, bool isMetaClass, bool& stop);
    void forEachObjCClass(uint64_t classListRuntimeOffset, uint64_t classListCount,
                          const VMAddrConverter& vmAddrConverter, ClassCallback& callback) const;
    void forEachObjCClass(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                          ClassCallback& callback) const;

    typedef void (^CategoryCallback)(uint64_t categoryVMAddr, const dyld3::MachOAnalyzer::ObjCCategory& objcCategory,
                                     bool& stop);
    void forEachObjCCategory(uint64_t categoryListRuntimeOffset, uint64_t categoryListCount,
                             const VMAddrConverter& vmAddrConverter, CategoryCallback& callback) const;
    void forEachObjCCategory(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                             CategoryCallback& callback) const;

    // lists all Protocols defined in the image
    typedef void (^ProtocolCallback)(uint64_t protocolVMAddr, const dyld3::MachOAnalyzer::ObjCProtocol& objCProtocol,
                                     bool& stop);
    void forEachObjCProtocol(uint64_t protocolListRuntimeOffset, uint64_t protocolListCount,
                             const VMAddrConverter& vmAddrConverter, ProtocolCallback& callback) const;
    void forEachObjCProtocol(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                             ProtocolCallback& callback) const;

    // Walk a method list starting from its vmAddr.
    // Note, classes, categories, protocols, etc, all share the same method list struture so can all use this.
    void forEachObjCMethod(uint64_t methodListVMAddr, const VMAddrConverter& vmAddrConverter,
                           uint64_t sharedCacheRelativeSelectorBaseVMAddress,
                           void (^handler)(uint64_t methodVMAddr, const ObjCMethod& method, bool& stop)) const;

    // Returns true if the given method list is a relative method list
    bool objcMethodListIsRelative(uint64_t methodListRuntimeOffset) const;

    void forEachObjCProperty(uint64_t propertyListVMAddr, const VMAddrConverter& vmAddrConverter,
                             void (^handler)(uint64_t propertyVMAddr, const ObjCProperty& property)) const;

    // lists all Protocols on a protocol_list_t
    void forEachObjCProtocol(uint64_t protocolListVMAddr, const VMAddrConverter& vmAddrConverter,
                             void (^handler)(uint64_t protocolRefVMAddr, const ObjCProtocol& protocol)) const;

    void forEachObjCSelectorReference(uint64_t selRefsRuntimeOffset, uint64_t selRefsCount, const VMAddrConverter& vmAddrConverter,
                                      void (^handler)(uint64_t selRefVMAddr, uint64_t selRefTargetVMAddr, bool& stop)) const;

    void forEachObjCSelectorReference(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                                      void (^handler)(uint64_t selRefVMAddr, uint64_t selRefTargetVMAddr, bool& stop)) const;

    void forEachObjCMethodName(void (^handler)(const char* methodName)) const;

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    void forEachObjCDuplicateClassToIgnore(void (^handler)(const char* className)) const;
#endif

    const ObjCImageInfo* objcImageInfo() const;

    void forEachWeakDef(Diagnostics& diag, void (^handler)(const char* symbolName, uint64_t imageOffset, bool isFromExportTrie)) const;

    struct BindTargetInfo {
        unsigned    targetIndex;
        int         libOrdinal;
        const char* symbolName;
        uint64_t    addend;
        bool        weakImport;
        bool        lazyBind;
    };
    void                forEachBindTarget(Diagnostics& diag, bool allowLazyBinds,
                                          void (^handler)(const BindTargetInfo& info, bool& stop),
                                          void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const;
    void                forEachBindLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex, bool& stop),
                                                    void (^overrideHandler)(uint64_t runtimeOffset, unsigned overrideBindTargetIndex, bool& stop)) const;
    void                forEachBindLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex, bool& stop)) const;
    bool                forEachRebaseLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const;
    bool                forEachRebaseLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const;

    // Get the layout information for this MachOAnalyzer.  Requires that the file is in VM layout, not file layout.
    bool                    getLinkeditLayout(Diagnostics& diag, uint64_t linkeditFileOffset,
                                              const uint8_t* linkeditStartAddr, mach_o::LinkeditLayout& layout) const;
    void                    withVMLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout& layout)) const;
    void                    getAllSegmentsInfos(Diagnostics& diag, mach_o::Header::SegmentInfo segments[]) const;

    typedef void (^BindDetailedHandler)(const char* opcodeName, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                uint64_t addend, bool targetOrAddendChanged, bool& stop);
    typedef void (^RebaseDetailHandler)(const char* opcodeName, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                        bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop);
    bool                forEachBind_OpcodesLazy(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], BindDetailedHandler) const;
    bool                forEachBind_OpcodesWeak(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], BindDetailedHandler,  void (^strongHandler)(const char* symbolName)) const;
    bool                forEachBind_OpcodesRegular(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], BindDetailedHandler) const;
    bool                forEachRebase_Opcodes(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], RebaseDetailHandler) const;

private:
    void                forEachBindUnified_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                                   void (^handler)(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop),
                                                   void (^overrideHandler)(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop)) const;

    void                forEachBindTarget_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                                  void (^handler)(const BindTargetInfo& info, bool& stop),
                                                  void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const;
    void                forEachBindTarget_ChainedFixups(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const;
    void                forEachBindTarget_Relocations(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const;

    bool                forEachBind_Relocations(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                                bool supportPrivateExternsWorkaround, BindDetailedHandler) const;
    bool                forEachRebase_Relocations(Diagnostics& diag, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], RebaseDetailHandler) const;

    struct SegmentStuff
    {
        uint64_t    fileOffset;
        uint64_t    fileSize;
        uint64_t    writable          :  1,
                    executable        :  1,
                    textRelocsAllowed :  1,  // segment supports text relocs (i386 only)
                    segSize           : 61;
	};

    const uint8_t*          getContentForVMAddr(const LayoutInfo& info, uint64_t vmAddr) const;
    bool                    validLoadCommands(Diagnostics& diag, const char* path, size_t fileLen) const;
    bool                    validEmbeddedPaths(Diagnostics& diag, mach_o::Platform platform, const char* path, bool internalInstall=false) const;
    bool                    validLinkedit(Diagnostics& diag, const char* path) const;
    bool                    validLinkeditLayout(Diagnostics& diag, const char* path) const;
    bool                    validRebaseInfo(Diagnostics& diag, const char* path) const;
    bool                    validBindInfo(Diagnostics& diag, const char* path) const;
    bool                    validMain(Diagnostics& diag, const char* path) const;
    bool                    validChainedFixupsInfo(Diagnostics& diag, const char* path) const;
    bool                    validChainedFixupsInfoOldArm64e(Diagnostics& diag, const char* path) const;

    bool                    invalidRebaseState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                              bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind) const;
    bool                    invalidBindState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[],
                                              bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint32_t pointerSize,
                                              uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const;
    bool                    doLocalReloc(Diagnostics& diag, uint32_t r_address, bool& stop, void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, bool& stop)) const;
    uint8_t                 relocPointerType() const;
    int                     libOrdinalFromDesc(uint16_t n_desc) const;
    bool                    doExternalReloc(Diagnostics& diag, uint32_t r_address, uint32_t r_symbolnum, LinkEditInfo& leInfo, bool& stop,
                                            void (^callback)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal,
                                                             uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const;

    bool                    segmentHasTextRelocs(uint32_t segIndex) const;
    uint64_t                localRelocBaseAddress(const mach_o::Header::SegmentInfo segmentsInfos[], uint32_t segCount) const;
    uint64_t                externalRelocBaseAddress(const mach_o::Header::SegmentInfo segmentsInfos[], uint32_t segCount) const;
    bool                    segIndexAndOffsetForAddress(uint64_t addr, const mach_o::Header::SegmentInfo segmentsInfos[], uint32_t segCount, uint32_t& segIndex, uint64_t& segOffset) const;
    void                    parseOrgArm64eChainedFixups(Diagnostics& diag, void (^targetCount)(uint32_t totalTargets, bool& stop),
                                                                           void (^addTarget)(const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                                                           void (^addChainStart)(const LinkEditInfo& leInfo, const mach_o::Header::SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop)) const;
    bool                    contentIsRegularStub(const uint8_t* helperContent) const;
    void                    recurseTrie(Diagnostics& diag, const uint8_t* const start, const uint8_t* p, const uint8_t* const end,
                                        OverflowSafeArray<char>& cummulativeString, int curStrOffset, bool& stop, MachOAnalyzer::ExportsCallback callback) const;

};


} // namespace dyld3

#endif /* MachOAnalyzer_h */
