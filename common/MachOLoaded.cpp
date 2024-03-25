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


#include <TargetConditionals.h>
#include "Defines.h"
#if !TARGET_OS_EXCLAVEKIT
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <mach-o/nlist.h>

#if !TARGET_OS_EXCLAVEKIT
extern "C" {
  #include <corecrypto/ccdigest.h>
  #include <corecrypto/ccsha1.h>
  #include <corecrypto/ccsha2.h>
}
#endif

#include "MachOFile.h"
#include "MachOLoaded.h"
#include "CodeSigningTypes.h"



namespace dyld3 {


void MachOLoaded::getLinkEditLoadCommands(Diagnostics& diag, LinkEditInfo& result) const
{
    result.dyldInfo       = nullptr;
    result.exportsTrie    = nullptr;
    result.chainedFixups  = nullptr;
    result.symTab         = nullptr;
    result.dynSymTab      = nullptr;
    result.splitSegInfo   = nullptr;
    result.functionStarts = nullptr;
    result.dataInCode     = nullptr;
    result.codeSig        = nullptr;
    __block bool hasUUID    = false;
    __block bool hasMinVersion = false;
    __block bool hasEncrypt = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                if ( cmd->cmdsize != sizeof(dyld_info_command) )
                    diag.error("LC_DYLD_INFO load command size wrong");
                else if ( result.dyldInfo != nullptr )
                    diag.error("multiple LC_DYLD_INFO load commands");
                result.dyldInfo = (dyld_info_command*)cmd;
                break;
            case LC_DYLD_EXPORTS_TRIE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_DYLD_EXPORTS_TRIE load command size wrong");
                else if ( result.exportsTrie != nullptr )
                    diag.error("multiple LC_DYLD_EXPORTS_TRIE load commands");
                result.exportsTrie = (linkedit_data_command*)cmd;
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_DYLD_CHAINED_FIXUPS load command size wrong");
                else if ( result.chainedFixups != nullptr )
                    diag.error("multiple LC_DYLD_CHAINED_FIXUPS load commands");
                result.chainedFixups = (linkedit_data_command*)cmd;
                break;
            case LC_SYMTAB:
                if ( cmd->cmdsize != sizeof(symtab_command) )
                    diag.error("LC_SYMTAB load command size wrong");
                else if ( result.symTab != nullptr )
                    diag.error("multiple LC_SYMTAB load commands");
                result.symTab = (symtab_command*)cmd;
                break;
            case LC_DYSYMTAB:
                if ( cmd->cmdsize != sizeof(dysymtab_command) )
                    diag.error("LC_DYSYMTAB load command size wrong");
                else if ( result.dynSymTab != nullptr )
                    diag.error("multiple LC_DYSYMTAB load commands");
                result.dynSymTab = (dysymtab_command*)cmd;
                break;
            case LC_SEGMENT_SPLIT_INFO:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_SEGMENT_SPLIT_INFO load command size wrong");
                else if ( result.splitSegInfo != nullptr )
                    diag.error("multiple LC_SEGMENT_SPLIT_INFO load commands");
                result.splitSegInfo = (linkedit_data_command*)cmd;
                break;
            case LC_FUNCTION_STARTS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_FUNCTION_STARTS load command size wrong");
                else if ( result.functionStarts != nullptr )
                    diag.error("multiple LC_FUNCTION_STARTS load commands");
                result.functionStarts = (linkedit_data_command*)cmd;
                break;
            case LC_DATA_IN_CODE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_DATA_IN_CODE load command size wrong");
                else if ( result.dataInCode != nullptr )
                    diag.error("multiple LC_DATA_IN_CODE load commands");
                result.dataInCode = (linkedit_data_command*)cmd;
                break;
            case LC_CODE_SIGNATURE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_CODE_SIGNATURE load command size wrong");
                else if ( result.codeSig != nullptr )
                     diag.error("multiple LC_CODE_SIGNATURE load commands");
                result.codeSig = (linkedit_data_command*)cmd;
                break;
            case LC_UUID:
                if ( cmd->cmdsize != sizeof(uuid_command) )
                    diag.error("LC_UUID load command size wrong");
                else if ( hasUUID )
                     diag.error("multiple LC_UUID load commands");
                hasUUID = true;
                break;
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_TVOS:
            case LC_VERSION_MIN_WATCHOS:
                if ( cmd->cmdsize != sizeof(version_min_command) )
                    diag.error("LC_VERSION_* load command size wrong");
                 else if ( hasMinVersion )
                     diag.error("multiple LC_VERSION_MIN_* load commands");
                hasMinVersion = true;
                break;
            case LC_BUILD_VERSION:
                if ( cmd->cmdsize != (sizeof(build_version_command) + ((build_version_command*)cmd)->ntools * sizeof(build_tool_version)) )
                    diag.error("LC_BUILD_VERSION load command size wrong");
                break;
            case LC_ENCRYPTION_INFO:
                if ( cmd->cmdsize != sizeof(encryption_info_command) )
                    diag.error("LC_ENCRYPTION_INFO load command size wrong");
                else if ( hasEncrypt )
                    diag.error("multiple LC_ENCRYPTION_INFO load commands");
                else if ( is64() )
                    diag.error("LC_ENCRYPTION_INFO found in 64-bit mach-o");
                hasEncrypt = true;
                break;
            case LC_ENCRYPTION_INFO_64:
                if ( cmd->cmdsize != sizeof(encryption_info_command_64) )
                    diag.error("LC_ENCRYPTION_INFO_64 load command size wrong");
                else if ( hasEncrypt )
                     diag.error("multiple LC_ENCRYPTION_INFO_64 load commands");
                else if ( !is64() )
                      diag.error("LC_ENCRYPTION_INFO_64 found in 32-bit mach-o");
                hasEncrypt = true;
                break;
        }
    });
    if ( diag.noError() && (result.dynSymTab != nullptr) && (result.symTab == nullptr) )
        diag.error("LC_DYSYMTAB but no LC_SYMTAB load command");
}

void MachOLoaded::getLinkEditPointers(Diagnostics& diag, LinkEditInfo& result) const
{
    getLinkEditLoadCommands(diag, result);
    if ( diag.noError() )
        getLayoutInfo(result.layout);
}

const uint8_t* MachOLoaded::getExportsTrie(const LinkEditInfo& leInfo, uint64_t& trieSize) const
{
    if ( leInfo.exportsTrie != nullptr) {
        trieSize = leInfo.exportsTrie->datasize;
        uint64_t offsetInLinkEdit = leInfo.exportsTrie->dataoff - leInfo.layout.linkeditFileOffset;
        return (uint8_t*)this + (leInfo.layout.linkeditUnslidVMAddr - leInfo.layout.textUnslidVMAddr) + offsetInLinkEdit;
    }
    else if ( leInfo.dyldInfo != nullptr ) {
        trieSize = leInfo.dyldInfo->export_size;
        uint64_t offsetInLinkEdit = leInfo.dyldInfo->export_off - leInfo.layout.linkeditFileOffset;
        return (uint8_t*)this + (leInfo.layout.linkeditUnslidVMAddr - leInfo.layout.textUnslidVMAddr) + offsetInLinkEdit;
    }
    trieSize = 0;
    return nullptr;
}


void MachOLoaded::getLayoutInfo(LayoutInfo& result) const
{
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            result.textUnslidVMAddr = (uintptr_t)info.vmAddr;
            result.slide = (uintptr_t)(((uint64_t)this) - info.vmAddr);
        }
        else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            result.linkeditUnslidVMAddr = (uintptr_t)info.vmAddr;
            result.linkeditFileOffset   = (uint32_t)info.fileOffset;
            result.linkeditFileSize     = (uint32_t)info.fileSize;
            result.linkeditSegIndex     = info.segIndex;
        }
        result.lastSegIndex = info.segIndex;
    });
}


//#if BUILDING_LIBDYLD
// this is only used by dlsym() at runtime.  All other binding is done when the closure is built.
bool MachOLoaded::hasExportedSymbol(const char* symbolName, DependentToMachOLoaded finder, void** result,
                                    bool* resultPointsToInstructions) const
{
    typedef void* (*ResolverFunc)(void);
    ResolverFunc resolver;
    Diagnostics diag;
    FoundSymbol foundInfo;
    if ( findExportedSymbol(diag, symbolName, false, foundInfo, finder) ) {
        switch ( foundInfo.kind ) {
            case FoundSymbol::Kind::headerOffset: {
                *result = (uint8_t*)foundInfo.foundInDylib + foundInfo.value;
                *resultPointsToInstructions = false;
                int64_t slide = foundInfo.foundInDylib->getSlide();
                foundInfo.foundInDylib->forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
                    uint64_t sectStartAddr = sectInfo.sectAddr + slide;
                    uint64_t sectEndAddr = sectStartAddr + sectInfo.sectSize;
                    if ( ((uint64_t)*result >= sectStartAddr) && ((uint64_t)*result < sectEndAddr) ) {
                        *resultPointsToInstructions = (sectInfo.sectFlags & S_ATTR_PURE_INSTRUCTIONS) || (sectInfo.sectFlags & S_ATTR_SOME_INSTRUCTIONS);
                        stop = true;
                    }
                });
                break;
            }
            case FoundSymbol::Kind::absolute:
                *result = (void*)(long)foundInfo.value;
                *resultPointsToInstructions = false;
                break;
            case FoundSymbol::Kind::resolverOffset:
                // foundInfo.value contains "stub".
                // in dlsym() we want to call resolver function to get final function address
                resolver = (ResolverFunc)((uint8_t*)foundInfo.foundInDylib + foundInfo.resolverFuncOffset);
                *result = (*resolver)();
                // FIXME: Set this properly
                *resultPointsToInstructions = true;
                break;
        }
        return true;
    }
    return false;
}
//#endif // BUILDING_LIBDYLD

bool MachOLoaded::findExportedSymbol(Diagnostics& diag, const char* symbolName, bool weakImport, FoundSymbol& foundInfo, DependentToMachOLoaded findDependent) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    uint64_t trieSize;
    if ( const uint8_t* trieStart = getExportsTrie(leInfo, trieSize) ) {
        const uint8_t* trieEnd   = trieStart + trieSize;
        const uint8_t* node      = trieWalk(diag, trieStart, trieEnd, symbolName);
        if ( node == nullptr ) {
            // symbol not exported from this image. Seach any re-exported dylibs
            __block unsigned        depIndex = 0;
            __block bool            foundInReExportedDylib = false;
            forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( isReExport && findDependent ) {
                    if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                       if ( depMH->findExportedSymbol(diag, symbolName, weakImport, foundInfo, findDependent) ) {
                            stop = true;
                            foundInReExportedDylib = true;
                        }
                    }
                }
                ++depIndex;
            });
            return foundInReExportedDylib;
        }
        const uint8_t* p = node;
        const uint64_t flags = read_uleb128(diag, p, trieEnd);
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            if ( !findDependent )
                return false;
            // re-export from another dylib, lookup there
            const uint64_t ordinal = read_uleb128(diag, p, trieEnd);
            const char* importedName = (char*)p;
            if ( importedName[0] == '\0' )
                importedName = symbolName;
            if ( (ordinal == 0) || (ordinal > dependentDylibCount()) ) {
                diag.error("re-export ordinal %lld out of range for %s", ordinal, symbolName);
                return false;
            }
            uint32_t depIndex = (uint32_t)(ordinal-1);
            if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                return depMH->findExportedSymbol(diag, importedName, weakImport, foundInfo, findDependent);
            }
            else if (weakImport) {
                return false;
            }
            else {
                diag.error("dependent dylib %lld not found for re-exported symbol %s", ordinal, symbolName);
                return false;
            }
        }
        foundInfo.kind               = FoundSymbol::Kind::headerOffset;
        foundInfo.isThreadLocal      = false;
        foundInfo.isWeakDef          = false;
        foundInfo.foundInDylib       = this;
        foundInfo.value              = read_uleb128(diag, p, trieEnd);
        foundInfo.resolverFuncOffset = 0;
        foundInfo.foundSymbolName    = symbolName;
        if ( diag.hasError() )
            return false;
        switch ( flags & EXPORT_SYMBOL_FLAGS_KIND_MASK ) {
            case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                    foundInfo.resolverFuncOffset = (uint32_t)read_uleb128(diag, p, trieEnd);
                }
                else {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                }
                if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
                    foundInfo.isWeakDef = true;
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
                foundInfo.isThreadLocal = true;
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE:
                foundInfo.kind = FoundSymbol::Kind::absolute;
                break;
            default:
                diag.error("unsupported exported symbol kind. flags=%llu at node offset=0x%0lX", flags, (long)(node-trieStart));
                return false;
        }
        return true;
    }
    else {
        // this is an old binary (before macOS 10.6), scan the symbol table
        foundInfo.foundInDylib = nullptr;
        forEachGlobalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
            if ( strcmp(aSymbolName, symbolName) == 0 ) {
                foundInfo.kind               = FoundSymbol::Kind::headerOffset;
                foundInfo.isThreadLocal      = false;
                foundInfo.foundInDylib       = this;
                foundInfo.value              = n_value - leInfo.layout.textUnslidVMAddr;
                foundInfo.resolverFuncOffset = 0;
                foundInfo.foundSymbolName    = symbolName;
                stop = true;
            }
        });
        if ( foundInfo.foundInDylib == nullptr ) {
            // symbol not exported from this image. Search any re-exported dylibs
            __block unsigned depIndex = 0;
            forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( isReExport && findDependent ) {
                    if ( const MachOLoaded* depMH = findDependent(this, depIndex) ) {
                        if ( depMH->findExportedSymbol(diag, symbolName, weakImport, foundInfo, findDependent) ) {
                            stop = true;
                        }
                    }
                }
                ++depIndex;
            });
        }
        return (foundInfo.foundInDylib != nullptr);
    }
}

intptr_t MachOLoaded::getSlide() const
{
    Diagnostics diag;
    __block intptr_t slide = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = (uintptr_t)(((uint64_t)this) - seg->vmaddr);
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = (uintptr_t)(((uint64_t)this) - seg->vmaddr);
                stop = true;
            }
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return slide;
}

const uint8_t* MachOLoaded::getLinkEditContent(const LayoutInfo& info, uint32_t fileOffset) const
{
    uint32_t offsetInLinkedit   = fileOffset - info.linkeditFileOffset;
    uintptr_t linkeditStartAddr = info.linkeditUnslidVMAddr + info.slide;
    return (uint8_t*)(linkeditStartAddr + offsetInLinkedit);
}


void MachOLoaded::forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t globalsStartIndex = 0;
        uint32_t globalsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            globalsStartIndex = leInfo.dynSymTab->iextdefsym;
            globalsCount      = leInfo.dynSymTab->nextdefsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)symbols;
        bool                   stop             = false;
        for (uint32_t i=0; (i < globalsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

void MachOLoaded::forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t localsStartIndex = 0;
        uint32_t localsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            localsStartIndex = leInfo.dynSymTab->ilocalsym;
            localsCount      = leInfo.dynSymTab->nlocalsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)(getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        bool                   stop             = false;
        for (uint32_t i=0; (i < localsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}


void MachOLoaded::forEachImportedSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t globalsStartIndex = 0;
        uint32_t globalsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            globalsStartIndex = leInfo.dynSymTab->iundefsym;
            globalsCount      = leInfo.dynSymTab->nundefsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)symbols;
        bool                   stop             = false;
        for (uint32_t i=0; (i < globalsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_TYPE) == N_UNDF )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_TYPE) == N_UNDF )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

const char* MachOLoaded::dependentDylibLoadPath(uint32_t depIndex) const
{
    __block const char* foundLoadPath = nullptr;
    __block uint32_t curDepIndex = 0;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( curDepIndex == depIndex ) {
            foundLoadPath = loadPath;
            stop = true;
        }
        ++curDepIndex;
    });
    return foundLoadPath;
}

const char* MachOLoaded::segmentName(uint32_t targetSegIndex) const
{
    __block const char* result = nullptr;
	forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( targetSegIndex == info.segIndex ) {
            result = info.segName;
            stop = true;
        }
    });
    return result;
}

bool MachOLoaded::findClosestFunctionStart(uint64_t address, uint64_t* functionStartAddress) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( leInfo.functionStarts == nullptr )
        return false;

    const uint8_t* starts    = getLinkEditContent(leInfo.layout, leInfo.functionStarts->dataoff);
    const uint8_t* startsEnd = starts + leInfo.functionStarts->datasize;

    uint64_t lastAddr    = (uint64_t)(long)this;
    uint64_t runningAddr = lastAddr;
    while (diag.noError()) {
        uint64_t value = read_uleb128(diag, starts, startsEnd);
        if ( value == 0 )
            break;
        lastAddr = runningAddr;
        runningAddr += value;
        //fprintf(stderr, "  addr=0x%08llX\n", runningAddr);
        if ( runningAddr > address ) {
            *functionStartAddress = lastAddr;
            return true;
        }
    };

    return false;
}

void MachOLoaded::forEachFunctionStart(void (^callback)(uint64_t runtimeOffset)) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;
    if ( leInfo.functionStarts == nullptr )
        return;

    const uint8_t* starts    = getLinkEditContent(leInfo.layout, leInfo.functionStarts->dataoff);
    const uint8_t* startsEnd = starts + leInfo.functionStarts->datasize;

    uint64_t runtimeOffset = 0;
    while (diag.noError()) {
        uint64_t value = read_uleb128(diag, starts, startsEnd);
        if ( value == 0 )
            break;
        runtimeOffset += value;
        callback(runtimeOffset);
    };
}

bool MachOLoaded::findClosestSymbol(uint64_t address, const char** symbolName, uint64_t* symbolAddr) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( (leInfo.symTab == nullptr) || (leInfo.dynSymTab == nullptr) )
        return false;
    uint64_t targetUnslidAddress = address - leInfo.layout.slide;

    // find section index the address is in to validate n_sect
    __block uint32_t sectionIndexForTargetAddress = 0;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        ++sectionIndexForTargetAddress;
        if ( (sectInfo.sectAddr <= targetUnslidAddress) && (targetUnslidAddress < sectInfo.sectAddr+sectInfo.sectSize) ) {
            stop = true;
        }
    });

    uint32_t               maxStringOffset  = leInfo.symTab->strsize;
    const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
    if ( is64() ) {
        const struct nlist_64* symbols64  = (struct nlist_64*)symbols;
        const struct nlist_64* bestSymbol = nullptr;
        // first walk all global symbols
        const struct nlist_64* const globalsStart = &symbols64[leInfo.dynSymTab->iextdefsym];
        const struct nlist_64* const globalsEnd   = &globalsStart[leInfo.dynSymTab->nextdefsym];
        for (const struct nlist_64* s = globalsStart; s < globalsEnd; ++s) {
            if ( (s->n_type & N_TYPE) == N_SECT ) {
                if ( bestSymbol == nullptr ) {
                    if ( (s->n_value <= targetUnslidAddress) && (s->n_sect == sectionIndexForTargetAddress) )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) && (s->n_sect == sectionIndexForTargetAddress) ) {
                    bestSymbol = s;
                }
            }
        }
        // next walk all local symbols
        const struct nlist_64* const localsStart = &symbols64[leInfo.dynSymTab->ilocalsym];
        const struct nlist_64* const localsEnd   = &localsStart[leInfo.dynSymTab->nlocalsym];
        for (const struct nlist_64* s = localsStart; s < localsEnd; ++s) {
             if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
                if ( bestSymbol == nullptr ) {
                    if ( (s->n_value <= targetUnslidAddress) && (s->n_sect == sectionIndexForTargetAddress) )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) && (s->n_sect == sectionIndexForTargetAddress) ) {
                    bestSymbol = s;
                }
            }
        }
        if ( bestSymbol != NULL ) {
            *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
            if ( bestSymbol->n_un.n_strx < maxStringOffset )
                *symbolName = &stringPool[bestSymbol->n_un.n_strx];
            return true;
        }
    }
    else {
       const struct nlist* bestSymbol = nullptr;
        // first walk all global symbols
        const struct nlist* const globalsStart = &symbols[leInfo.dynSymTab->iextdefsym];
        const struct nlist* const globalsEnd   = &globalsStart[leInfo.dynSymTab->nextdefsym];
        for (const struct nlist* s = globalsStart; s < globalsEnd; ++s) {
            if ( (s->n_type & N_TYPE) == N_SECT ) {
                if ( bestSymbol == nullptr ) {
                    if ( (s->n_value <= targetUnslidAddress) && (s->n_sect == sectionIndexForTargetAddress) )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) && (s->n_sect == sectionIndexForTargetAddress) ) {
                    bestSymbol = s;
                }
            }
        }
        // next walk all local symbols
        const struct nlist* const localsStart = &symbols[leInfo.dynSymTab->ilocalsym];
        const struct nlist* const localsEnd   = &localsStart[leInfo.dynSymTab->nlocalsym];
        for (const struct nlist* s = localsStart; s < localsEnd; ++s) {
             if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
                if ( bestSymbol == nullptr ) {
                    if ( (s->n_value <= targetUnslidAddress) && (s->n_sect == sectionIndexForTargetAddress) )
                        bestSymbol = s;
                }
                else if ( (s->n_value <= targetUnslidAddress) && (bestSymbol->n_value < s->n_value) && (s->n_sect == sectionIndexForTargetAddress) ) {
                    bestSymbol = s;
                }
            }
        }
        if ( bestSymbol != nullptr ) {
#if __arm__
            if ( bestSymbol->n_desc & N_ARM_THUMB_DEF )
                *symbolAddr = (bestSymbol->n_value | 1) + leInfo.layout.slide;
            else
                *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
#else
            *symbolAddr = bestSymbol->n_value + leInfo.layout.slide;
#endif
            if ( bestSymbol->n_un.n_strx < maxStringOffset )
                *symbolName = &stringPool[bestSymbol->n_un.n_strx];
            return true;
        }
    }

    return false;
}

const void* MachOLoaded::findSectionContent(const char* segName, const char* sectName, uint64_t& size,
                                            bool matchSegNameAsPrefix) const
{
    __block const void* result = nullptr;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( strcmp(sectInfo.sectName, sectName) != 0 )
            return;

        // Segment name is either matched exactly or by prefix
        if ( matchSegNameAsPrefix ) {
            if ( strstr(sectInfo.segInfo.segName, segName) != sectInfo.segInfo.segName )
                return;
        } else {
            if ( strcmp(sectInfo.segInfo.segName, segName) != 0 )
                return;
        }

        size = sectInfo.sectSize;
        if ( this->isPreload() )
            result = (uint8_t*)this + sectInfo.sectFileOffset;
        else
            result = (void*)(sectInfo.sectAddr + getSlide());
        stop = true;
    });
    return result;
}


bool MachOLoaded::intersectsRange(uintptr_t start, uintptr_t length) const
{
    __block bool result = false;
    uintptr_t slide = getSlide();
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( (info.vmAddr+info.vmSize+slide >= start) && (info.vmAddr+slide < start+length) )
            result = true;
    });
    return result;
}

//#if BUILDING_DYLD || BUILDING_LIBDYLD
void MachOLoaded::fixupAllChainedFixups(Diagnostics& diag, const dyld_chained_starts_in_image* starts, uintptr_t slide,
                                        Array<const void*> bindTargets, void (^logFixup)(void* loc, void* newValue)) const
{
    forEachFixupInAllChains(diag, starts, true, ^(ChainedFixupPointerOnDisk* fixupLoc, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
        void* newValue;
        switch (segInfo->pointer_format) {
#if __LP64__
  #if  __has_feature(ptrauth_calls)
           case DYLD_CHAINED_PTR_ARM64E:
           case DYLD_CHAINED_PTR_ARM64E_KERNEL:
           case DYLD_CHAINED_PTR_ARM64E_USERLAND:
           case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
               if ( fixupLoc->arm64e.authRebase.auth ) {
                    if ( fixupLoc->arm64e.authBind.bind ) {
                        uint32_t bindOrdinal = (segInfo->pointer_format == DYLD_CHAINED_PTR_ARM64E_USERLAND24) ? fixupLoc->arm64e.authBind24.ordinal : fixupLoc->arm64e.authBind.ordinal;
                        if ( bindOrdinal >= bindTargets.count() ) {
                            diag.error("out of range bind ordinal %d (max %llu)", bindOrdinal, bindTargets.count());
                            stop = true;
                            break;
                        }
                        else {
                            // authenticated bind
                            newValue = (void*)(bindTargets[bindOrdinal]);
                            if (newValue != 0)  // Don't sign missing weak imports
                                newValue = (void*)fixupLoc->arm64e.signPointer(fixupLoc, (uintptr_t)newValue);
                        }
                    }
                    else {
                        // authenticated rebase
                        newValue = (void*)fixupLoc->arm64e.signPointer(fixupLoc, (uintptr_t)this + fixupLoc->arm64e.authRebase.target);
                    }
                }
                else {
                    if ( fixupLoc->arm64e.bind.bind ) {
                        uint32_t bindOrdinal = (segInfo->pointer_format == DYLD_CHAINED_PTR_ARM64E_USERLAND24) ? fixupLoc->arm64e.bind24.ordinal : fixupLoc->arm64e.bind.ordinal;
                        if ( bindOrdinal >= bindTargets.count() ) {
                            diag.error("out of range bind ordinal %d (max %llu)", bindOrdinal, bindTargets.count());
                            stop = true;
                            break;
                        }
                        else {
                            // plain bind
                            newValue = (void*)((long)bindTargets[bindOrdinal] + fixupLoc->arm64e.signExtendedAddend());
                        }
                    }
                    else {
                        // plain rebase (old format target is vmaddr, new format target is offset)
                        if ( segInfo->pointer_format == DYLD_CHAINED_PTR_ARM64E )
                            newValue = (void*)(fixupLoc->arm64e.unpackTarget()+slide);
                        else
                            newValue = (void*)((uintptr_t)this + fixupLoc->arm64e.unpackTarget());
                   }
                }
                if ( logFixup )
                    logFixup(fixupLoc, newValue);
                fixupLoc->raw64 = (uintptr_t)newValue;
                break;
  #endif
            case DYLD_CHAINED_PTR_64:
            case DYLD_CHAINED_PTR_64_OFFSET:
                if ( fixupLoc->generic64.bind.bind ) {
                    if ( fixupLoc->generic64.bind.ordinal >= bindTargets.count() ) {
                        diag.error("out of range bind ordinal %d (max %llu)", fixupLoc->generic64.bind.ordinal, bindTargets.count());
                        stop = true;
                        break;
                    }
                    else {
                        newValue = (void*)((long)bindTargets[fixupLoc->generic64.bind.ordinal] + fixupLoc->generic64.signExtendedAddend());
                    }
                }
                else {
                    // plain rebase (old format target is vmaddr, new format target is offset)
                    if ( segInfo->pointer_format == DYLD_CHAINED_PTR_64 )
                        newValue = (void*)(fixupLoc->generic64.unpackedTarget()+slide);
                    else
                        newValue = (void*)((uintptr_t)this + fixupLoc->generic64.unpackedTarget());
                }
                if ( logFixup )
                    logFixup(fixupLoc, newValue);
                fixupLoc->raw64 = (uintptr_t)newValue;
               break;
#else
            case DYLD_CHAINED_PTR_32:
                if ( fixupLoc->generic32.bind.bind ) {
                    if ( fixupLoc->generic32.bind.ordinal >= bindTargets.count() ) {
                        diag.error("out of range bind ordinal %d (max %llu)", fixupLoc->generic32.bind.ordinal, bindTargets.count());
                        stop = true;
                        break;
                    }
                    else {
                        newValue = (void*)((long)bindTargets[fixupLoc->generic32.bind.ordinal] + fixupLoc->generic32.bind.addend);
                    }
                }
                else {
                    if ( fixupLoc->generic32.rebase.target > segInfo->max_valid_pointer ) {
                        // handle non-pointers in chain
                        uint32_t bias = (0x04000000 + segInfo->max_valid_pointer)/2;
                        newValue = (void*)(fixupLoc->generic32.rebase.target - bias);
                    }
                    else {
                        newValue = (void*)(fixupLoc->generic32.rebase.target + slide);
                    }
                }
                if ( logFixup )
                    logFixup(fixupLoc, newValue);
                fixupLoc->raw32 = (uint32_t)(uintptr_t)newValue;
               break;
#endif // __LP64__
            default:
                diag.error("unsupported pointer chain format: 0x%04X", segInfo->pointer_format);
                stop = true;
                break;
        }
    });
}
//#endif

void MachOLoaded::forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo, bool notifyNonPointers,
                                              void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, const dyld_chained_starts_in_segment* segInfo, bool& stop)) const
{
    auto adaptor = ^(ChainedFixupPointerOnDisk* fixupLocation, bool& stop) {
         handler(fixupLocation, segInfo, stop);
    };
    bool stopped = false;
    for (uint32_t pageIndex=0; pageIndex < segInfo->page_count && !stopped; ++pageIndex) {
        uint16_t offsetInPage = segInfo->page_start[pageIndex];
        if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
            continue;
        if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
            // 32-bit chains which may need multiple starts per page
            uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
            bool chainEnd = false;
            while (!stopped && !chainEnd) {
                chainEnd = (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                uint8_t* pageContentStart = (uint8_t*)this + segInfo->segment_offset + (pageIndex * segInfo->page_size);
                ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);

                stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, adaptor);
                ++overflowIndex;
            }
        }
        else {
            // one chain per page
            uint8_t* pageContentStart = (uint8_t*)this + segInfo->segment_offset + (pageIndex * segInfo->page_size);
            ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);

            stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, adaptor);
        }
    }
}

void MachOLoaded::forEachFixupInAllChains(Diagnostics& diag, const dyld_chained_starts_in_image* starts, bool notifyNonPointers,
                                          void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, const dyld_chained_starts_in_segment* segInfo, bool& stop)) const
{
    bool stopped = false;
    for (uint32_t segIndex=0; segIndex < starts->seg_count && !stopped; ++segIndex) {
        if ( starts->seg_info_offset[segIndex] == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
        forEachFixupInSegmentChains(diag, segInfo, notifyNonPointers, handler);
    }
}

void MachOLoaded::forEachFixupInAllChains(Diagnostics& diag, uint16_t pointer_format, uint32_t starts_count, const uint32_t chain_starts[],
                                          void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop)) const
{
    auto adaptor = ^(ChainedFixupPointerOnDisk* fixupLocation, bool& stop) {
         handler(fixupLocation, stop);
    };
    for (uint32_t i=0; i < starts_count; ++i) {
        const uint32_t                      startVmOffset = chain_starts[i];
        __block ChainedFixupPointerOnDisk*  chain         = nullptr;
        if ( this->isPreload() ) {
            // starts are vm-offsets but image is not loaded with zerofill, so need to map vm-offsets to file-offsets
            __block uint64_t startVmAddr = ~0ULL;
            this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
                if ( startVmAddr == ~0ULL )
                    startVmAddr = info.vmAddr + startVmOffset;
                if ( (info.vmAddr <= startVmAddr) && (startVmAddr < (info.vmAddr + info.vmSize)) ) {
                    uint64_t startFileOffset = info.fileOffset + startVmAddr - info.vmAddr;
                    chain = (ChainedFixupPointerOnDisk*)((uint8_t*)this + startFileOffset);
                    stop = true;
                }
            });
        }
        else {
            chain = (ChainedFixupPointerOnDisk*)((uint8_t*)this + startVmOffset);
        }
        if ( walkChain(diag, chain, pointer_format, false, 0, adaptor) )
            break;
    }
}

uint64_t MachOLoaded::firstSegmentFileOffset() const
{
    __block uint64_t result = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        result = info.fileOffset;
        stop = true;
    });
    return result;
}




} // namespace dyld3

