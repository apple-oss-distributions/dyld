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

#include "Array.h"
#include "MachOLayout.h"
#include "MachOFile.h"

#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <mach-o/x86_64/reloc.h>

// FIXME: We should get this from cctools
#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

namespace mach_o
{

// MARK: --- Layout methods ---

Layout::Layout(MachOFileRef mf, std::span<SegmentLayout> segments, const LinkeditLayout& linkedit)
    : mf(std::move(mf)), segments(segments), linkedit(linkedit)
{
}

uint64_t Layout::textUnslidVMAddr() const
{
    for ( const SegmentLayout& segment : this->segments ) {
        if ( segment.kind == SegmentLayout::Kind::text )
            return segment.vmAddr;
    }

    // MachOFile::preferredLoadAddress seems to return 0 if we didn't find __TEXT, so match it
    return 0;
}

bool Layout::isSwiftLibrary() const
{
    struct objc_image_info {
        int32_t version;
        uint32_t flags;
    };

    __block bool result = false;
    this->mf->forEachSection(^(const dyld3::MachOFile::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( (strncmp(sectInfo.sectName, "__objc_imageinfo", 16) == 0) && (strncmp(sectInfo.segInfo.segName, "__DATA", 6) == 0) ) {
            uint64_t segmentOffset = sectInfo.sectFileOffset - sectInfo.segInfo.fileOffset;
            objc_image_info* info =  (objc_image_info*)(this->segments[sectInfo.segInfo.segIndex].buffer + segmentOffset);
            uint32_t swiftVersion = ((info->flags >> 8) & 0xFF);
            if ( swiftVersion )
                result = true;
            stop = true;
        }
    });
    return result;
}

bool Layout::hasSection(std::string_view segmentName, std::string_view sectionName) const
{
    __block bool result = false;
    this->mf->forEachSection(^(const dyld3::MachOFile::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( (sectInfo.segInfo.segName == segmentName) && (sectInfo.sectName == sectionName) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

namespace {
    struct LinkEditContentChunk
    {
        const char* name;
        uint32_t    alignment;
        uint32_t    fileOffsetStart;
        uint32_t    size;

        // only have a few chunks, so bubble sort is ok.  Don't use libc's qsort because it may call malloc
        static void sort(LinkEditContentChunk array[], unsigned long count)
        {
            for (unsigned i=0; i < count-1; ++i) {
                bool done = true;
                for (unsigned j=0; j < count-i-1; ++j) {
                    if ( array[j].fileOffsetStart > array[j+1].fileOffsetStart ) {
                        LinkEditContentChunk temp = array[j];
                        array[j]   = array[j+1];
                        array[j+1] = temp;
                        done = false;
                    }
                }
                if ( done )
                    break;
            }
        }
    };
} // anonymous namespace

bool Layout::isValidLinkeditLayout(Diagnostics &diag, const char *path) const
{
    typedef dyld3::MachOFile::Malformed Malformed;

    const uint32_t ptrSize = this->mf->pointerSize();

    // build vector of all blobs in LINKEDIT
    LinkEditContentChunk blobs[32];
    LinkEditContentChunk* bp = blobs;
    if ( const Linkedit& blob = this->linkedit.rebaseOpcodes; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"rebase opcodes",          ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.regularBindOpcodes; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"bind opcodes",            ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.weakBindOpcodes; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"weak bind opcodes",       ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.lazyBindOpcodes; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"lazy bind opcodes",       ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.exportsTrie; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"exports trie",            ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.chainedFixups; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"chained fixups",          ptrSize, blob.fileOffset,   blob.bufferSize };
    }

    if ( const Linkedit& blob = this->linkedit.localRelocs; blob.hasValue() ) {
        if ( blob.entryCount != 0 ) {
            uint32_t bufferSize = (uint32_t)(blob.entryCount * sizeof(relocation_info));
            *bp++ = {"local relocations",       ptrSize, blob.fileOffset,   bufferSize };
        }
    }
    if ( const Linkedit& blob = this->linkedit.externRelocs; blob.hasValue() ) {
        if ( blob.entryCount != 0 ) {
            uint32_t bufferSize = (uint32_t)(blob.entryCount * sizeof(relocation_info));
            *bp++ = {"external relocations",    ptrSize, blob.fileOffset,   bufferSize };
        }
    }
    if ( const Linkedit& blob = this->linkedit.indirectSymbolTable; blob.hasValue() ) {
        if ( blob.entryCount != 0 ) {
            uint32_t bufferSize = (uint32_t)(blob.entryCount * sizeof(uint32_t));
            *bp++ = {"indirect symbol table",   4,       blob.fileOffset,   bufferSize };
        }
    }

    if ( const Linkedit& blob = this->linkedit.splitSegInfo; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"shared cache info",       ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.functionStarts; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"function starts",         ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.dataInCode; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"data in code",            ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.symbolTable; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"symbol table",            ptrSize, blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.symbolStrings; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"symbol table strings",    1,       blob.fileOffset,   blob.bufferSize };
    }
    if ( const Linkedit& blob = this->linkedit.codeSignature; blob.hasValue() ) {
        if ( blob.bufferSize != 0 )
            *bp++ = {"code signature",          ptrSize, blob.fileOffset,   blob.bufferSize };
    }

    // check for bad combinations
    if ( (this->linkedit.dyldInfoCmd == LC_DYLD_INFO_ONLY) ) {
        if ( (this->linkedit.localRelocs.entryCount != 0) && this->mf->enforceFormat(Malformed::dyldInfoAndlocalRelocs) ) {
            diag.error("in '%s' malformed mach-o contains LC_DYLD_INFO_ONLY and local relocations", path);
            return false;
        }
        if ( this->linkedit.externRelocs.entryCount != 0 ) {
            diag.error("in '%s' malformed mach-o contains LC_DYLD_INFO_ONLY and external relocations", path);
            return false;
        }
    }

    bool checkMissingDyldInfo = true;
#if BUILDING_DYLDINFO || BUILDING_APP_CACHE_UTIL
    checkMissingDyldInfo = this->mf->isDyldManaged() && !this->mf->isStaticExecutable();
#endif
    if ( (this->linkedit.dyldInfoCmd == 0 ) && !this->linkedit.hasDynSymTab && checkMissingDyldInfo ) {
        diag.error("in '%s' malformed mach-o misssing LC_DYLD_INFO and LC_DYSYMTAB", path);
        return false;
    }

    // FIXME: Remove this hack
#if BUILDING_APP_CACHE_UTIL
    if ( this->mf->isFileSet() )
        return true;
#endif

    const unsigned long blobCount = bp - blobs;
    if ( blobCount == 0 ) {
        diag.error("in '%s' malformed mach-o missing LINKEDIT", path);
        return false;
    }

    // Find the linkedit
    uint32_t linkeditFileOffset = ~0U;
    uint32_t linkeditFileSize   = ~0U;
    for ( const SegmentLayout& segment : this->segments ) {
        if ( segment.kind == SegmentLayout::Kind::linkedit ) {
            linkeditFileOffset = (uint32_t)segment.fileOffset;
            linkeditFileSize   = (uint32_t)segment.fileSize;
            break;
        }
    }

    uint32_t linkeditFileEnd = linkeditFileOffset + linkeditFileSize;


    // sort blobs by file-offset and error on overlaps
    LinkEditContentChunk::sort(blobs, blobCount);
    uint32_t     prevEnd = linkeditFileOffset;
    const char*  prevName = "start of LINKEDIT";
    for (unsigned long i=0; i < blobCount; ++i) {
        const LinkEditContentChunk& blob = blobs[i];
        if ( blob.fileOffsetStart < prevEnd ) {
            diag.error("in '%s' LINKEDIT overlap of %s and %s", path, prevName, blob.name);
            return false;
        }
        if (dyld3::greaterThanAddOrOverflow(blob.fileOffsetStart, blob.size, linkeditFileEnd)) {
            diag.error("in '%s' LINKEDIT content '%s' extends beyond end of segment", path, blob.name);
            return false;
        }
        if ( (blob.fileOffsetStart & (blob.alignment-1)) != 0 ) {
            // <rdar://problem/51115705> relax code sig alignment for pre iOS13
            Malformed kind = (strcmp(blob.name, "code signature") == 0) ? Malformed::codeSigAlignment : Malformed::linkeditAlignment;
            if ( this->mf->enforceFormat(kind) )
                diag.error("in '%s' mis-aligned LINKEDIT content '%s'", path, blob.name);
        }
        prevEnd  = blob.fileOffsetStart + blob.size;
        prevName = blob.name;
    }

    // Check for invalid symbol table sizes
    if ( this->linkedit.hasSymTab ) {
        const Linkedit& symbolTable = this->linkedit.symbolTable;
        if ( symbolTable.entryCount > 0x10000000 ) {
            diag.error("in '%s' malformed mach-o image: symbol table too large", path);
            return false;
        }
        if ( this->linkedit.hasDynSymTab ) {
            // validate indirect symbol table
            const Linkedit& localSymbolTable = this->linkedit.localSymbolTable;
            const Linkedit& globalSymbolTable = this->linkedit.globalSymbolTable;
            const Linkedit& undefSymbolTable = this->linkedit.undefSymbolTable;
            const Linkedit& indirectSymbolTable = this->linkedit.indirectSymbolTable;
            if ( indirectSymbolTable.entryCount != 0 ) {
                if ( indirectSymbolTable.entryCount > 0x10000000 ) {
                    diag.error("in '%s' malformed mach-o image: indirect symbol table too large", path);
                    return false;
                }
            }
            if ( (localSymbolTable.entryCount > symbolTable.entryCount) || (localSymbolTable.entryIndex > symbolTable.entryCount) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table local symbol count exceeds total symbols", path);
                return false;
            }
            if ( (localSymbolTable.entryIndex + localSymbolTable.entryCount) < localSymbolTable.entryIndex  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table local symbol count wraps", path);
                return false;
            }
            if ( (globalSymbolTable.entryCount > symbolTable.entryCount)
                || (globalSymbolTable.entryIndex > symbolTable.entryCount) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table extern symbol count exceeds total symbols", path);
                return false;
            }
            if ( (globalSymbolTable.entryIndex + globalSymbolTable.entryCount) < globalSymbolTable.entryIndex  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table extern symbol count wraps", path);
                return false;
            }
            if ( (undefSymbolTable.entryCount > symbolTable.entryCount) || (undefSymbolTable.entryIndex > symbolTable.entryCount) ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table undefined symbol count exceeds total symbols", path);
                return false;
            }
            if ( (undefSymbolTable.entryIndex + undefSymbolTable.entryCount) < undefSymbolTable.entryIndex  ) {
                diag.error("in '%s' malformed mach-o image: indirect symbol table undefined symbol count wraps", path);
                return false;
            }
        }
    }

    return true;
}

bool Layout::findExportedSymbol(Diagnostics& diag, const char* symbolName, bool weakImport,
                                FoundSymbol& foundInfo) const
{
    if ( this->linkedit.exportsTrie.hasValue() ) {
        // FIXME: Move all this to the ExportTrie class
        const uint8_t* trieStart = this->linkedit.exportsTrie.buffer;
        const uint8_t* trieEnd   = trieStart + this->linkedit.exportsTrie.bufferSize;
        const uint8_t* node      = dyld3::MachOFile::trieWalk(diag, trieStart, trieEnd, symbolName);
        if ( node == nullptr ) {
            // symbol not exported from this image. Seach any re-exported dylibs
            // FIXME: Implement this
#if 0
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
#endif
            return false;
        }
        const uint8_t* p = node;
        const uint64_t flags = dyld3::MachOFile::read_uleb128(diag, p, trieEnd);
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            // FIXME: Implement this
#if 0
            if ( !findDependent )
                return false;
            // re-export from another dylib, lookup there
            const uint64_t ordinal = dyld3::MachOFile::read_uleb128(diag, p, trieEnd);
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
#endif
            return false;
        }
        foundInfo.kind               = FoundSymbol::Kind::headerOffset;
        foundInfo.isThreadLocal      = false;
        foundInfo.isWeakDef          = false;
        foundInfo.foundInDylib       = this->mf;
        foundInfo.value              = dyld3::MachOFile::read_uleb128(diag, p, trieEnd);
        foundInfo.resolverFuncOffset = 0;
        foundInfo.foundSymbolName    = symbolName;
        if ( diag.hasError() )
            return false;
        switch ( flags & EXPORT_SYMBOL_FLAGS_KIND_MASK ) {
            case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                    foundInfo.resolverFuncOffset = (uint32_t)dyld3::MachOFile::read_uleb128(diag, p, trieEnd);
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
        foundInfo.foundInDylib.reset();

        SymbolTable symbolTable(*this);
        symbolTable.forEachGlobalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type,
                                                uint8_t n_sect, uint16_t n_desc, bool& stop) {
            if ( strcmp(aSymbolName, symbolName) == 0 ) {
                foundInfo.kind               = FoundSymbol::Kind::headerOffset;
                foundInfo.isThreadLocal      = false;
                foundInfo.foundInDylib       = this->mf;
                foundInfo.value              = n_value - this->textUnslidVMAddr();
                foundInfo.resolverFuncOffset = 0;
                foundInfo.foundSymbolName    = symbolName;
                stop = true;
            }
        });

        // FIXME: Implement this
#if 0
        if ( !foundInfo.foundInDylib.has_value() ) {
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
#endif
        return foundInfo.foundInDylib.has_value();
    }
}

// MARK: --- Fixups methods ---

Fixups::Fixups(const Layout& layout)
    : layout(layout)
{
}

void Fixups::forEachBindTarget(Diagnostics& diag, bool allowLazyBinds, intptr_t slide,
                               void (^handler)(const BindTargetInfo& info, bool& stop),
                               void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const
{
    if ( this->layout.mf->isPreload() )
        return;
    if ( this->layout.mf->hasChainedFixups() )
        this->forEachBindTarget_ChainedFixups(diag, handler);
    else if ( this->layout.mf->hasOpcodeFixups() )
        this->forEachBindTarget_Opcodes(diag, allowLazyBinds, handler, overrideHandler);
    else
        this->forEachBindTarget_Relocations(diag, slide, handler);
}

void Fixups::forEachBindTarget_ChainedFixups(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const
{
    __block unsigned targetIndex = 0;
    this->forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)  {
        BindTargetInfo info;
        info.targetIndex = targetIndex;
        info.libOrdinal  = libOrdinal;
        info.symbolName  = symbolName;
        info.addend      = addend;
        info.weakImport  = weakImport;
        info.lazyBind    = false;
        handler(info, stop);
       ++targetIndex;
    });

    // The C++ spec says main executables can define non-weak functions which override weak-defs in dylibs
    // This happens automatically for anything bound at launch, but the dyld cache is pre-bound so we need
    // to patch any binds that are overridden by this non-weak in the main executable.
    if ( diag.noError() && this->layout.mf->isMainExecutable() && this->layout.mf->hasWeakDefs() ) {
        dyld3::MachOFile::forEachTreatAsWeakDef(^(const char* symbolName) {
            BindTargetInfo info;
            info.targetIndex = targetIndex;
            info.libOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
            info.symbolName  = symbolName;
            info.addend      = 0;
            info.weakImport  = false;
            info.lazyBind    = false;
            bool stop = false;
            handler(info, stop);
           ++targetIndex;
        });
    }
}

void Fixups::parseOrgArm64eChainedFixups(Diagnostics& diag,
                                         void (^targetCount)(uint32_t totalTargets, bool& stop),
                                         void (^addTarget)(bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                         void (^addChainStart)(uint32_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop)) const
{
    bool            stop    = false;

    const uint32_t dylibCount = this->layout.mf->dependentDylibCount();

    if ( this->layout.linkedit.regularBindOpcodes.hasValue() ) {
        // process bind opcodes
        const uint8_t*  p    = this->layout.linkedit.regularBindOpcodes.buffer;
        const uint8_t*  end  = p + this->layout.linkedit.regularBindOpcodes.bufferSize;
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int             libraryOrdinal = 0;
        bool            segIndexSet = false;
        bool            libraryOrdinalSet = false;
        uint64_t        targetTableCount;
        uint64_t        addend = 0;
        bool            weakImport = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    stop = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    libraryOrdinal = immediate;
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    libraryOrdinal = (int)dyld3::MachOFile::read_uleb128(diag, p, end);
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    // the special ordinals are negative numbers
                    if ( immediate == 0 )
                        libraryOrdinal = 0;
                    else {
                        int8_t signExtended = BIND_OPCODE_MASK | immediate;
                        libraryOrdinal = signExtended;
                    }
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                    symbolName = (char*)p;
                    while (*p != '\0')
                        ++p;
                    ++p;
                    break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = dyld3::MachOFile::read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    if ( addTarget )
                        addTarget(libraryOrdinalSet, dylibCount, libraryOrdinal, type, symbolName, addend, weakImport, stop);
                    break;
                case BIND_OPCODE_THREADED:
                    switch (immediate) {
                        case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
                            targetTableCount = dyld3::MachOFile::read_uleb128(diag, p, end);
                            if ( targetTableCount > 65535 ) {
                                diag.error("BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB size too large");
                                stop = true;
                            }
                            else {
                                if ( targetCount )
                                    targetCount((uint32_t)targetTableCount, stop);
                            }
                            break;
                        case BIND_SUBOPCODE_THREADED_APPLY:
                            if ( addChainStart )
                                addChainStart(segmentIndex, segIndexSet, segmentOffset, DYLD_CHAINED_PTR_ARM64E, stop);
                            break;
                        default:
                            diag.error("bad BIND_OPCODE_THREADED sub-opcode 0x%02X", immediate);
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", immediate);
            }
        }
        if ( diag.hasError() )
            return;
    }
}

void Fixups::forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const
{
    if ( this->layout.linkedit.regularBindOpcodes.hasValue() ) {
        parseOrgArm64eChainedFixups(diag, nullptr, ^(bool libraryOrdinalSet, uint32_t dylibCount,
                                                    int libOrdinal, uint8_t type, const char* symbolName, uint64_t fixAddend, bool weakImport, bool& stopChain) {
            callback(libOrdinal, symbolName, fixAddend, weakImport, stopChain);
        }, nullptr);
    }
    else if ( this->layout.linkedit.chainedFixups.hasValue() ) {
        const dyld_chained_fixups_header* header = (dyld_chained_fixups_header*)this->layout.linkedit.chainedFixups.buffer;
        dyld3::MachOFile::forEachChainedFixupTarget(diag, header, this->layout.linkedit.chainedFixups.cmd, callback);
    }
}

#if (BUILDING_DYLD || BUILDING_LIBDYLD) && !__arm64e__
  #define SUPPORT_OLD_ARM64E_FORMAT 0
#else
  #define SUPPORT_OLD_ARM64E_FORMAT 1
#endif

// find dyld_chained_starts_in_image* in image
// if old arm64e binary, synthesize dyld_chained_starts_in_image*
void Fixups::withThreadedRebaseAsChainStarts(Diagnostics& diag, void (^callback)(const dyld_chained_fixups_header* header, uint64_t fixupsSize)) const
{
#if SUPPORT_OLD_ARM64E_FORMAT
    // don't want this code in non-arm64e dyld because it causes a stack protector which dereferences a GOT pointer before GOT is set up
    // old arm64e binary, create a dyld_chained_starts_in_image for caller
    uint64_t baseAddress = this->layout.mf->preferredLoadAddress();
    uint64_t imagePageCount = this->layout.mf->mappedSize()/0x4000;
    size_t bufferSize = this->layout.linkedit.regularBindOpcodes.bufferSize + (size_t)imagePageCount*sizeof(uint16_t) + 512;
    BLOCK_ACCCESSIBLE_ARRAY(uint8_t, buffer, bufferSize);
    uint8_t* bufferEnd = &buffer[bufferSize];
    dyld_chained_fixups_header* header = (dyld_chained_fixups_header*)buffer;
    header->fixups_version = 0;
    header->starts_offset  = sizeof(dyld_chained_fixups_header);
    header->imports_offset = 0;
    header->symbols_offset = 0;
    header->imports_count  = 0;
    header->imports_format = 0;
    header->symbols_format = 0;
    dyld_chained_starts_in_image* starts = (dyld_chained_starts_in_image*)(dyld_chained_starts_in_image*)((uint8_t*)header + header->starts_offset);
    starts->seg_count = (uint32_t)this->layout.segments.size();
    for (uint32_t i=0; i < starts->seg_count; ++i)
        starts->seg_info_offset[i] = 0;
    __block uint8_t curSegIndex = 0;
    __block dyld_chained_starts_in_segment* curSeg = (dyld_chained_starts_in_segment*)(&(starts->seg_info_offset[starts->seg_count]));
    parseOrgArm64eChainedFixups(diag, nullptr, nullptr, ^(uint32_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop) {
        uint32_t pageIndex = (uint32_t)(segmentOffset/0x1000);
        if ( segmentIndex != curSegIndex ) {
            if ( curSegIndex == 0 ) {
                starts->seg_info_offset[segmentIndex] = (uint32_t)((uint8_t*)curSeg - (uint8_t*)starts);
            }
            else {
                starts->seg_info_offset[segmentIndex] = (uint32_t)((uint8_t*)(&curSeg->page_start[curSeg->page_count]) - (uint8_t*)starts);
                curSeg = (dyld_chained_starts_in_segment*)((uint8_t*)starts+starts->seg_info_offset[segmentIndex]);
                assert((uint8_t*)curSeg < bufferEnd);
           }
           curSeg->page_count = 0;
           curSegIndex = segmentIndex;
        }
        while ( curSeg->page_count != pageIndex ) {
            assert((uint8_t*)(&curSeg->page_start[curSeg->page_count]) < bufferEnd);
            curSeg->page_start[curSeg->page_count] = 0xFFFF;
            curSeg->page_count++;
        }
        curSeg->size                  = (uint32_t)((uint8_t*)(&curSeg->page_start[pageIndex]) - (uint8_t*)curSeg);
        curSeg->page_size             = 0x1000; // old arm64e encoding used 4KB pages
        curSeg->pointer_format        = DYLD_CHAINED_PTR_ARM64E;
        curSeg->segment_offset        = this->layout.segments[segmentIndex].vmAddr - baseAddress;
        curSeg->max_valid_pointer     = 0;
        curSeg->page_count            = pageIndex+1;
        assert((uint8_t*)(&curSeg->page_start[pageIndex]) < bufferEnd);
        curSeg->page_start[pageIndex] = segmentOffset & 0xFFF;
        //fprintf(stderr, "segment_offset=0x%llX, vmAddr=0x%llX\n", curSeg->segment_offset, segments[segmentIndex].vmAddr );
        //printf("segIndex=%d, segOffset=0x%08llX, page_start[%d]=0x%04X, page_start[%d]=0x%04X\n",
        //        segmentIndex, segmentOffset, pageIndex, curSeg->page_start[pageIndex], pageIndex-1, pageIndex ? curSeg->page_start[pageIndex-1] : 0);
    });
    callback(header, (uint64_t)bufferSize);
#endif
}

const dyld_chained_fixups_header* Fixups::chainedFixupsHeader() const {
    if ( this->layout.linkedit.chainedFixups.hasValue() ) {
        // find dyld_chained_starts_in_image from dyld_chained_fixups_header
        return (dyld_chained_fixups_header*)this->layout.linkedit.chainedFixups.buffer;
    }
    return nullptr;
}

// find dyld_chained_starts_in_image* in image
// if old arm64e binary, synthesize dyld_chained_starts_in_image*
void Fixups::withChainStarts(Diagnostics& diag, void (^callback)(const dyld_chained_starts_in_image*)) const
{
    if ( const dyld_chained_fixups_header* chainHeader = this->chainedFixupsHeader() ) {
        // find dyld_chained_starts_in_image from dyld_chained_fixups_header
        callback((dyld_chained_starts_in_image*)((uint8_t*)chainHeader + chainHeader->starts_offset));
    }
#if SUPPORT_OLD_ARM64E_FORMAT
    // don't want this code in non-arm64e dyld because it causes a stack protector which dereferences a GOT pointer before GOT is set up
    else if ( this->layout.linkedit.regularBindOpcodes.hasValue() && (this->layout.mf->cputype == CPU_TYPE_ARM64) && (this->layout.mf->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) ) {
        // old arm64e binary, create a dyld_chained_starts_in_image for caller
        this->withThreadedRebaseAsChainStarts(diag, ^(const dyld_chained_fixups_header* header, uint64_t fixupsSize) {
            callback((dyld_chained_starts_in_image*)((uint8_t*)header + header->starts_offset));
        });
    }
#endif
    else {
        diag.error("image does not use chained fixups");
    }
}

void Fixups::forEachFixupInAllChains(Diagnostics& diag, const dyld_chained_starts_in_image* starts, bool notifyNonPointers,
                                     void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, uint64_t fixupSegmentOffset,
                                                     const dyld_chained_starts_in_segment* segInfo, bool& stop)) const
{

    bool stopped = false;
    for (uint32_t segIndex=0; segIndex < starts->seg_count && !stopped; ++segIndex) {
        if ( starts->seg_info_offset[segIndex] == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
        auto adaptor = ^(ChainedFixupPointerOnDisk* fixupLocation, uint64_t fixupSegmentOffset, bool& stop) {
            handler(fixupLocation, fixupSegmentOffset, segInfo, stop);
        };
        forEachFixupInSegmentChains(diag, segInfo, segIndex, notifyNonPointers, adaptor);
    }
}

void Fixups::forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool notifyNonPointers,
                                         void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, uint64_t fixupSegmentOffset, bool& stop)) const
{
    const uint8_t* segmentBuffer = this->layout.segments[segIndex].buffer;
    auto adaptor = ^(ChainedFixupPointerOnDisk* fixupLocation, bool& stop) {
        uint64_t fixupSegmentOffset = (uint64_t)fixupLocation - (uint64_t)segmentBuffer;
         handler(fixupLocation, fixupSegmentOffset, stop);
    };
    bool stopped = false;
    for (uint32_t pageIndex=0; pageIndex < segInfo->page_count && !stopped; ++pageIndex) {
        uint16_t offsetInPage = segInfo->page_start[pageIndex];
        if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
            continue;
        const uint8_t* pageContentStart = segmentBuffer + (pageIndex * segInfo->page_size);
        if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
            // 32-bit chains which may need multiple starts per page
            uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
            bool chainEnd = false;
            while (!stopped && !chainEnd) {
                chainEnd = (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);

                stopped = dyld3::MachOFile::walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, adaptor);
                ++overflowIndex;
            }
        }
        else {
            // one chain per page
            ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);
            stopped = dyld3::MachOFile::walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, adaptor);
        }
    }
}

void Fixups::forEachFixupChainSegment(Diagnostics& diag, const dyld_chained_starts_in_image* starts,
                                      void (^handler)(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stop))
{
    bool stopped = false;
    for (uint32_t segIndex=0; segIndex < starts->seg_count && !stopped; ++segIndex) {
        if ( starts->seg_info_offset[segIndex] == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
        handler(segInfo, segIndex, stopped);
    }
}

uint16_t Fixups::chainedPointerFormat() const
{
    if ( const dyld_chained_fixups_header* chainHeader = this->chainedFixupsHeader() ) {
        // get pointer format from chain info struct in LINKEDIT
        return dyld3::MachOFile::chainedPointerFormat(chainHeader);
    }
    assert(this->layout.mf->cputype == CPU_TYPE_ARM64
           && (this->layout.mf->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E)
           && "chainedPointerFormat() called on non-chained binary");
    return DYLD_CHAINED_PTR_ARM64E;
}

// walk through all binds, unifying weak, lazy, and regular binds
void Fixups::forEachBindUnified_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                        void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop),
                                        void (^overrideHandler)(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop)) const
{
    {
        __block unsigned         targetIndex = 0;
        __block BindTargetInfo   targetInfo;
        BindDetailedHandler binder =  ^(const char* opcodeName,
                                        bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                        uint32_t pointerSize, uint32_t segmentIndex, uint64_t segmentOffset,
                                        uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                        uint64_t addend, bool targetOrAddendChanged, bool& stop) {
            uint64_t bindVmOffset  = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
            uint64_t runtimeOffset = bindVmOffset - this->layout.textUnslidVMAddr();
            if ( targetOrAddendChanged ) {
                targetInfo.targetIndex = targetIndex++;
                targetInfo.libOrdinal  = libOrdinal;
                targetInfo.symbolName  = symbolName;
                targetInfo.addend      = addend;
                targetInfo.weakImport  = weakImport;
                targetInfo.lazyBind    = lazyBind && allowLazyBinds;
            }
            handler(runtimeOffset, segmentIndex, targetInfo, stop);
        };
        bool stopped = this->forEachBind_OpcodesRegular(diag, binder);
        if ( stopped )
            return;
        stopped = this->forEachBind_OpcodesLazy(diag, binder);
        if ( stopped )
            return;
    }

    // Opcode based weak-binds effectively override other binds/rebases.  Process them last
    // To match dyld2, they are allowed to fail to find a target, in which case the normal rebase/bind will
    // not be overridden.
    {
        __block unsigned         weakTargetIndex = 0;
        __block BindTargetInfo   weakTargetInfo;
        BindDetailedHandler weakBinder =  ^(const char* opcodeName,
                                            bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                            uint32_t pointerSize, uint32_t segmentIndex, uint64_t segmentOffset,
                                            uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                            uint64_t addend, bool targetOrAddendChanged, bool& stop) {

            uint64_t bindVmOffset  = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
            uint64_t runtimeOffset = bindVmOffset - this->layout.textUnslidVMAddr();
            if ( (symbolName != weakTargetInfo.symbolName) || (strcmp(symbolName, weakTargetInfo.symbolName) != 0) || (weakTargetInfo.addend != addend) ) {
                weakTargetInfo.targetIndex = weakTargetIndex++;
                weakTargetInfo.libOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
                weakTargetInfo.symbolName  = symbolName;
                weakTargetInfo.addend      = addend;
                weakTargetInfo.weakImport  = false;
                weakTargetInfo.lazyBind    = false;
            }
            overrideHandler(runtimeOffset, segmentIndex, weakTargetInfo, stop);
        };
        auto strongHandler = ^(const char* strongName) { };
        this->forEachBind_OpcodesWeak(diag, weakBinder, strongHandler);
    }
}

void Fixups::forEachBindTarget_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                       void (^handler)(const BindTargetInfo& info, bool& stop),
                                       void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const
{
    __block unsigned lastTargetIndex = -1;
    __block unsigned lastWeakBindTargetIndex = -1;
    this->forEachBindUnified_Opcodes(diag, allowLazyBinds,
                                     ^(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop) {
        // Regular/lazy binds
        if ( lastTargetIndex != targetInfo.targetIndex) {
            handler(targetInfo, stop);
            lastTargetIndex = targetInfo.targetIndex;
        }
    }, ^(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop) {
        // Weak binds
        if ( lastWeakBindTargetIndex != targetInfo.targetIndex) {
            overrideHandler(targetInfo, stop);
            lastWeakBindTargetIndex = targetInfo.targetIndex;
        }
    });
}

bool Fixups::forEachBind_OpcodesLazy(Diagnostics& diag, BindDetailedHandler handler) const
{
    if ( !this->layout.linkedit.lazyBindOpcodes.hasValue() )
        return false;

    uint32_t        lazyDoneCount   = 0;
    uint32_t        lazyBindCount   = 0;
    const uint32_t  ptrSize         = this->layout.mf->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = this->layout.mf->dependentDylibCount();
    const uint8_t*  p               = this->layout.linkedit.lazyBindOpcodes.buffer;
    const uint8_t*  end             = p + this->layout.linkedit.lazyBindOpcodes.bufferSize;
    uint8_t         type            = BIND_TYPE_POINTER;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = 0;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = false;
    int64_t         addend          = 0;
    bool            weakImport = false;
    while (  !stop && diag.noError() && (p < end) ) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                // this opcode marks the end of each lazy pointer binding
                ++lazyDoneCount;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                libraryOrdinal = immediate;
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                libraryOrdinal = (int)dyld3::MachOFile::read_uleb128(diag, p, end);
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // the special ordinals are negative numbers
                if ( immediate == 0 )
                    libraryOrdinal = 0;
                else {
                    int8_t signExtended = BIND_OPCODE_MASK | immediate;
                    libraryOrdinal = signExtended;
                }
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = dyld3::MachOFile::read_sleb128(diag, p, end);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segmentIndex = immediate;
                segmentOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
                segIndexSet = true;
                break;
            case BIND_OPCODE_DO_BIND:
                handler("BIND_OPCODE_DO_BIND", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, true, addend, true, stop);
                segmentOffset += ptrSize;
                ++lazyBindCount;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
            case BIND_OPCODE_ADD_ADDR_ULEB:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            default:
                diag.error("bad lazy bind opcode 0x%02X", opcode);
                break;
        }
    }
    if ( lazyDoneCount > lazyBindCount+7 ) {
        // diag.error("lazy bind opcodes missing binds");
    }
    return stop;
}



bool Fixups::forEachBind_OpcodesWeak(Diagnostics& diag, BindDetailedHandler handler,  void (^strongHandler)(const char* symbolName)) const
{
    if ( !this->layout.linkedit.weakBindOpcodes.hasValue() )
        return false;

    const uint32_t  ptrSize         = this->layout.mf->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = this->layout.mf->dependentDylibCount();
    const uint8_t*  p               = this->layout.linkedit.weakBindOpcodes.buffer;
    const uint8_t*  end             = p + this->layout.linkedit.weakBindOpcodes.bufferSize;
    uint8_t         type            = BIND_TYPE_POINTER;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = true;
    int64_t         addend          = 0;
    bool            weakImport      = false;
    bool            targetOrAddendChanged   = true;
    bool            done            = false;
    uint64_t        count;
    uint64_t        skip;
    while ( !stop && diag.noError() && (p < end) && !done ) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                done = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                diag.error("unexpected dylib ordinal in weak_bind");
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                if ( immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION ) {
                    strongHandler(symbolName);
                }
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = dyld3::MachOFile::read_sleb128(diag, p, end);
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segmentIndex = immediate;
                segmentOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
                segIndexSet = true;
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                segmentOffset += dyld3::MachOFile::read_uleb128(diag, p, end);
                break;
            case BIND_OPCODE_DO_BIND:
                handler("BIND_OPCODE_DO_BIND", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += dyld3::MachOFile::read_uleb128(diag, p, end) + ptrSize;
                 targetOrAddendChanged = false;
               break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += immediate*ptrSize + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = dyld3::MachOFile::read_uleb128(diag, p, end);
                skip = dyld3::MachOFile::read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    segmentOffset += skip + ptrSize;
                    targetOrAddendChanged = false;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("bad bind opcode 0x%02X", *p);
        }
    }
    return stop;
}

bool Fixups::forEachBind_OpcodesRegular(Diagnostics& diag, BindDetailedHandler handler) const
{
    if ( !this->layout.linkedit.regularBindOpcodes.hasValue() )
        return false;

    const uint32_t  ptrSize         = this->layout.mf->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = this->layout.mf->dependentDylibCount();
    const uint8_t*  p               = this->layout.linkedit.regularBindOpcodes.buffer;
    const uint8_t*  end             = p + this->layout.linkedit.regularBindOpcodes.bufferSize;
    uint8_t         type            = 0;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = 0;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = false;
    bool            targetOrAddendChanged   = false;
    bool            done            = false;
    int64_t         addend          = 0;
    uint64_t        count;
    uint64_t        skip;
    bool            weakImport = false;
    while ( !stop && diag.noError() && (p < end) && !done ) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                done = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                libraryOrdinal = immediate;
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                libraryOrdinal = (int)dyld3::MachOFile::read_uleb128(diag, p, end);
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // the special ordinals are negative numbers
                if ( immediate == 0 )
                    libraryOrdinal = 0;
                else {
                    int8_t signExtended = BIND_OPCODE_MASK | immediate;
                    libraryOrdinal = signExtended;
                }
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = dyld3::MachOFile::read_sleb128(diag, p, end);
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segmentIndex = immediate;
                segmentOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
                segIndexSet = true;
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                segmentOffset += dyld3::MachOFile::read_uleb128(diag, p, end);
                break;
            case BIND_OPCODE_DO_BIND:
                handler("BIND_OPCODE_DO_BIND", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += dyld3::MachOFile::read_uleb128(diag, p, end) + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += immediate*ptrSize + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = dyld3::MachOFile::read_uleb128(diag, p, end);
                skip = dyld3::MachOFile::read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    segmentOffset += skip + ptrSize;
                    targetOrAddendChanged = false;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("bad bind opcode 0x%02X", *p);
        }
    }
    return stop;
}

void Fixups::forEachBindLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned targetIndex, bool& stop),
                                         void (^overrideHandler)(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned overrideBindTargetIndex, bool& stop)) const
{
    this->forEachBindUnified_Opcodes(diag, false,
                                     ^(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& targetInfo, bool& stop) {
        handler(runtimeOffset, segmentIndex, targetInfo.targetIndex, stop);
    }, ^(uint64_t runtimeOffset, uint32_t segmentIndex, const BindTargetInfo& weakTargetInfo, bool& stop) {
        overrideHandler(runtimeOffset, segmentIndex, weakTargetInfo.targetIndex, stop);
    });
}

void Fixups::forEachBindLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex,
                                                                                bool& stop)) const
{
    // As we don't need the private externs workaround, we also don't need a slide here
    bool supportPrivateExternsWorkaround = false;
    intptr_t unusedSlide = 0;

    __block int targetIndex = -1;
    this->forEachBind_Relocations(diag, supportPrivateExternsWorkaround, unusedSlide,
                                  ^(const char* opcodeName,
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                    uint32_t pointerSize, uint32_t segmentIndex, uint64_t segmentOffset,
                                    uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                    uint64_t addend, bool targetOrAddendChanged, bool& stop) {
        if ( targetOrAddendChanged )
            ++targetIndex;
        uint64_t bindVMAddr  = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset = bindVMAddr - this->layout.textUnslidVMAddr();
        handler(runtimeOffset, targetIndex, stop);
    });
}

// old binary, walk external relocations and indirect symbol table
void Fixups::forEachBindTarget_Relocations(Diagnostics& diag, intptr_t slide,
                                           void (^handler)(const BindTargetInfo& info, bool& stop)) const
{
    __block unsigned targetIndex = 0;
    this->forEachBind_Relocations(diag, true, slide,
                                  ^(const char* opcodeName,
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                    uint32_t pointerSize, uint32_t segmentIndex, uint64_t segmentOffset,
                                    uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                    uint64_t addend, bool targetOrAddendChanged, bool& stop) {
        if ( targetOrAddendChanged ) {
            BindTargetInfo info;
            info.targetIndex = targetIndex;
            info.libOrdinal  = libOrdinal;
            info.symbolName  = symbolName;
            info.addend      = addend;
            info.weakImport  = weakImport;
            info.lazyBind    = lazyBind;
            handler(info, stop);
           ++targetIndex;
        }
    });
}

bool Fixups::forEachRebaseLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex, bool& stop)) const
{
    return this->forEachRebase_Opcodes(diag,
                                       ^(const char* opcodeName, bool segIndexSet, uint32_t pointerSize,
                                         uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind,
                                         bool& stop) {
        uint64_t rebaseVMAddr = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset  = rebaseVMAddr - this->layout.textUnslidVMAddr();
        handler(runtimeOffset, segmentIndex, stop);
    });
}

void Fixups::forEachRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, uint64_t rebasedValue, bool& stop)) const
{
    if ( !this->layout.linkedit.rebaseOpcodes.hasValue() )
        return;

    const bool is64 = this->layout.mf->is64();
    this->forEachRebase_Opcodes(diag, ^(const char* opcodeName, bool segIndexSet, uint32_t pointerSize,
                                        uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind,
                                        bool& stop) {
        uint64_t rebaseVMAddr = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset  = rebaseVMAddr - this->layout.textUnslidVMAddr();
        const uint8_t* fixupLoc = this->layout.segments[segmentIndex].buffer + segmentOffset;
        uint64_t targetVMAddr = 0;
        if ( is64 ) {
            targetVMAddr = *(uint64_t*)fixupLoc;
        } else {
            targetVMAddr = *(uint32_t*)fixupLoc;
        }
        callback(runtimeOffset, targetVMAddr, stop);
    });
}

bool Fixups::forEachRebase_Opcodes(Diagnostics& diag, RebaseDetailHandler handler) const
{
    const bool is64 = this->layout.mf->is64();
    const Rebase pointerRebaseKind = is64 ? Rebase::pointer64 : Rebase::pointer32;
    assert(this->layout.linkedit.rebaseOpcodes.hasValue());

    const uint8_t* const start = this->layout.linkedit.rebaseOpcodes.buffer;
    const uint8_t* const end   = start + this->layout.linkedit.rebaseOpcodes.bufferSize;
    const uint8_t* p           = start;
    const uint32_t ptrSize     = this->layout.mf->pointerSize();
    Rebase   kind = Rebase::unknown;
    int      segIndex = 0;
    uint64_t segOffset = 0;
    uint64_t count;
    uint64_t skip;
    bool     segIndexSet = false;
    bool     stop = false;
    while ( !stop && diag.noError() && (p < end) ) {
        uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
        uint8_t opcode = *p & REBASE_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case REBASE_OPCODE_DONE:
                // Allow some padding, in case rebases were somehow aligned to 16-bytes in size
                if ( (end - p) > 15 )
                    diag.error("rebase opcodes terminated early at offset %d of %d", (int)(p-start), (int)(end-start));
                stop = true;
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                switch ( immediate ) {
                    case REBASE_TYPE_POINTER:
                        kind = pointerRebaseKind;
                        break;
                    case REBASE_TYPE_TEXT_ABSOLUTE32:
                        kind = Rebase::textAbsolute32;
                        break;
                    case REBASE_TYPE_TEXT_PCREL32:
                        kind = Rebase::textPCrel32;
                        break;
                    default:
                        kind = Rebase::unknown;
                        break;
                }
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segIndex = immediate;
                segOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
                segIndexSet = true;
                break;
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                segOffset += dyld3::MachOFile::read_uleb128(diag, p, end);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                segOffset += immediate*ptrSize;
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for (int i=0; i < immediate; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_IMM_TIMES", segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += ptrSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                count = dyld3::MachOFile::read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += ptrSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                segOffset += dyld3::MachOFile::read_uleb128(diag, p, end) + ptrSize;
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = dyld3::MachOFile::read_uleb128(diag, p, end);
                if ( diag.hasError() )
                    break;
                skip = dyld3::MachOFile::read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += skip + ptrSize;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("unknown rebase opcode 0x%02X", opcode);
        }
    }
    return stop;
}

bool Fixups::forEachRebaseLocation_Relocations(Diagnostics& diag,
                                               void (^handler)(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                               bool& stop)) const
{
    return this->forEachRebase_Relocations(diag, ^(const char* opcodeName, bool segIndexSet,
                                                   uint32_t pointerSize, uint8_t segmentIndex,
                                                   uint64_t segmentOffset, Rebase kind, bool& stop) {
        uint64_t rebaseVmOffset = this->layout.segments[segmentIndex].vmAddr + segmentOffset;
        uint64_t runtimeOffset  = rebaseVmOffset - this->layout.textUnslidVMAddr();
        handler(runtimeOffset, segmentIndex, stop);
    });
}

// relocs are normally sorted, we don't want to use qsort because it may switch to mergesort which uses malloc
static void sortRelocations(dyld3::Array<relocation_info>& relocs)
{
    // The kernel linker has malloc, and old-style relocations are extremely common.  So use qsort
#if BUILDING_APP_CACHE_UTIL
    ::qsort(&relocs[0], relocs.count(), sizeof(relocation_info),
            [](const void* l, const void* r) -> int {
                if ( ((relocation_info*)l)->r_address < ((relocation_info*)r)->r_address )
                    return -1;
                else
                    return 1;
    });
#else
    uintptr_t count = relocs.count();
    for (uintptr_t i=0; i < count-1; ++i) {
        bool done = true;
        for (uintptr_t j=0; j < count-i-1; ++j) {
            if ( relocs[j].r_address > relocs[j+1].r_address ) {
                relocation_info temp = relocs[j];
                relocs[j]   = relocs[j+1];
                relocs[j+1] = temp;
                done = false;
            }
        }
        if ( done )
            break;
    }
#endif
}

bool Fixups::forEachRebase_Relocations(Diagnostics& diag, RebaseDetailHandler handler) const
{
    // old binary, walk relocations
    bool                            is64Bit     = this->layout.mf->is64();
    const uint8_t                   ptrSize     = this->layout.mf->pointerSize();
    const uint64_t                  relocsStartAddress = localRelocBaseAddress();
    const relocation_info* const    relocsStart = (const relocation_info*)this->layout.linkedit.localRelocs.buffer;
    const relocation_info* const    relocsEnd   = &relocsStart[this->layout.linkedit.localRelocs.entryCount];
    const uint8_t                   relocSize   = (is64Bit ? 3 : 2);
    bool                            stop        = false;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(relocation_info, relocs, 2048);
    for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
        if ( reloc->r_length != relocSize ) {
            bool shouldEmitError = true;
#if BUILDING_APP_CACHE_UTIL
            if ( this->layout.mf->usesClassicRelocationsInKernelCollection() && (reloc->r_length == 2) && (relocSize == 3) )
                shouldEmitError = false;
#endif
            if ( shouldEmitError ) {
                diag.error("local relocation has wrong r_length");
                break;
            }
        }
        if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
            diag.error("local relocation has wrong r_type");
            break;
        }
        relocs.push_back(*reloc);
    }
    if ( !relocs.empty() ) {
        sortRelocations(relocs);
        for (relocation_info reloc : relocs) {
            uint32_t addrOff = reloc.r_address;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            uint64_t addr = 0;
#if BUILDING_APP_CACHE_UTIL
            // xnu for x86_64 has __HIB mapped before __DATA, so offsets appear to be
            // negative
            if ( this->layout.mf->isStaticExecutable() || this->layout.mf->isFileSet() ) {
                addr = relocsStartAddress + (int32_t)addrOff;
            } else {
                addr = relocsStartAddress + addrOff;
            }
#else
            addr = relocsStartAddress + addrOff;
#endif
            if ( segIndexAndOffsetForAddress(addr, segIndex, segOffset) ) {
                Rebase kind = (reloc.r_length == 2) ? Rebase::pointer32 : Rebase::pointer64;
                if ( this->layout.mf->cputype == CPU_TYPE_I386 ) {
                    if ( this->layout.segments[segIndex].executable() )
                        kind = Rebase::textAbsolute32;
                }
                handler("local relocation", true, ptrSize, segIndex, segOffset, kind, stop);
            }
            else {
                diag.error("local relocation has out of range r_address");
                break;
            }
        }
    }
    // then process indirect symbols
    const Rebase pointerRebaseKind = is64Bit ? Rebase::pointer64 : Rebase::pointer32;
    intptr_t unusedSlide = 0;
    forEachIndirectPointer(diag, false, unusedSlide,
                           ^(uint64_t address, bool bind, int bindLibOrdinal,
                             const char* bindSymbolName, bool bindWeakImport, bool bindLazy,
                             bool selfModifyingStub, bool& indStop) {
        if ( bind )
           return;
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(address, segIndex, segOffset) ) {
            handler("local relocation", true, ptrSize, segIndex, segOffset, pointerRebaseKind, indStop);
        }
        else {
            diag.error("local relocation has out of range r_address");
            indStop = true;
        }
    });

    return stop;
}


bool Fixups::forEachBind_Relocations(Diagnostics& diag, bool supportPrivateExternsWorkaround,
                                     intptr_t slide, BindDetailedHandler handler) const
{
    // Firmare binaries won't have a dynSymTab
    if ( !this->layout.linkedit.externRelocs.hasValue() )
        return false;

    const uint64_t                  relocsStartAddress = externalRelocBaseAddress();
    const relocation_info* const    relocsStart = (const relocation_info*)this->layout.linkedit.externRelocs.buffer;
    const relocation_info* const    relocsEnd   = &relocsStart[this->layout.linkedit.externRelocs.entryCount];
    bool                            is64Bit     = this->layout.mf->is64() ;
    const uint32_t                  ptrSize     = this->layout.mf->pointerSize();
    const uint32_t                  dylibCount  = this->layout.mf->dependentDylibCount();
    const uint8_t                   relocSize   = (is64Bit ? 3 : 2);
    const void*                     symbolTable = this->layout.linkedit.symbolTable.buffer;
    const struct nlist_64*          symbols64   = (nlist_64*)symbolTable;
    const struct nlist*             symbols32   = (struct nlist*)symbolTable;
    const char*                     stringPool  = (char*)this->layout.linkedit.symbolStrings.buffer;
    uint32_t                        symCount    = this->layout.linkedit.symbolTable.entryCount;
    uint32_t                        poolSize    = this->layout.linkedit.symbolStrings.bufferSize;
    uint32_t                        lastSymIndx = -1;
    uint64_t                        lastAddend  = 0;
    bool                            stop        = false;
    for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
        bool isBranch = false;
#if BUILDING_APP_CACHE_UTIL
        if ( this->layout.mf->isKextBundle() ) {
            // kext's may have other kinds of relocations, eg, branch relocs.  Skip them
            if ( this->layout.mf->isArch("x86_64") || this->layout.mf->isArch("x86_64h") ) {
                if ( reloc->r_type == X86_64_RELOC_BRANCH ) {
                    if ( reloc->r_length != 2 ) {
                        diag.error("external relocation has wrong r_length");
                        break;
                    }
                    if ( reloc->r_pcrel != true ) {
                        diag.error("external relocation should be pcrel");
                        break;
                    }
                    isBranch = true;
                }
            }
        }
#endif
        if ( !isBranch ) {
            if ( reloc->r_length != relocSize ) {
                diag.error("external relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA == ARM64_RELOC_UNSIGNED
                diag.error("external relocation has wrong r_type");
                break;
            }
        }
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(relocsStartAddress+reloc->r_address, segIndex, segOffset) ) {
            uint32_t symbolIndex = reloc->r_symbolnum;
            if ( symbolIndex > symCount ) {
                diag.error("external relocation has out of range r_symbolnum");
                break;
            }
            else {
                uint32_t strOffset  = is64Bit ? symbols64[symbolIndex].n_un.n_strx : symbols32[symbolIndex].n_un.n_strx;
                uint16_t n_desc     = is64Bit ? symbols64[symbolIndex].n_desc : symbols32[symbolIndex].n_desc;
                uint8_t  n_type     = is64Bit ? symbols64[symbolIndex].n_type : symbols32[symbolIndex].n_type;
                uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
                if ( strOffset >= poolSize ) {
                    diag.error("external relocation has r_symbolnum=%d which has out of range n_strx", symbolIndex);
                    break;
                }
                else {
                    const char*     symbolName = stringPool + strOffset;
                    bool            weakImport = (n_desc & N_WEAK_REF);
                    const uint8_t*  content    = this->layout.segments[segIndex].buffer + segOffset;
                    uint64_t        addend     = (reloc->r_length == 3) ? *((uint64_t*)content) : *((uint32_t*)content);
                    // Handle defined weak def symbols which need to get a special ordinal
                    if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) && ((n_desc & N_WEAK_DEF) != 0) )
                        libOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
                    uint8_t type = isBranch ? BIND_TYPE_TEXT_PCREL32 : BIND_TYPE_POINTER;
                    bool targetOrAddendChanged = (lastSymIndx != symbolIndex) || (lastAddend != addend);
                    handler("external relocation", true, true, dylibCount, libOrdinal,
                             ptrSize, segIndex, segOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    lastSymIndx = symbolIndex;
                    lastAddend  = addend;
                }
            }
        }
        else {
            diag.error("local relocation has out of range r_address");
            break;
        }
    }
    // then process indirect symbols
    forEachIndirectPointer(diag, supportPrivateExternsWorkaround, slide,
                           ^(uint64_t address, bool bind, int bindLibOrdinal,
                             const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
        if ( !bind )
           return;
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(address, segIndex, segOffset) ) {
            handler("indirect symbol", true, true, dylibCount, bindLibOrdinal,
                     ptrSize, segIndex, segOffset, BIND_TYPE_POINTER, bindSymbolName, bindWeakImport, bindLazy, 0, true, indStop);
        }
        else {
            diag.error("indirect symbol has out of range address");
            indStop = true;
        }
    });

    return false;
}

void Fixups::forEachIndirectPointer(Diagnostics& diag, bool supportPrivateExternsWorkaround, intptr_t slide,
                                    void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal, const char* bindSymbolName,
                                                    bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const
{
    // find lazy and non-lazy pointer sections
    const bool              is64Bit                  = this->layout.mf->is64();
    const uint32_t* const   indirectSymbolTable      = (uint32_t*)this->layout.linkedit.indirectSymbolTable.buffer;
    const uint32_t          indirectSymbolTableCount = this->layout.linkedit.indirectSymbolTable.entryCount;
    const uint32_t          ptrSize                  = this->layout.mf->pointerSize();
    const void*             symbolTable              = this->layout.linkedit.symbolTable.buffer;
    const struct nlist_64*  symbols64                = (nlist_64*)symbolTable;
    const struct nlist*     symbols32                = (struct nlist*)symbolTable;
    const char*             stringPool               = (char*)this->layout.linkedit.symbolStrings.buffer;
    uint32_t                symCount                 = this->layout.linkedit.symbolTable.entryCount;
    uint32_t                poolSize                 = this->layout.linkedit.symbolStrings.bufferSize;
    __block bool            stop                     = false;

    // Old kexts put S_LAZY_SYMBOL_POINTERS on the __got section, even if they didn't have indirect symbols to prcess.
    // In that case, skip the loop as there shouldn't be anything to process
    if ( (indirectSymbolTableCount == 0) && this->layout.mf->isKextBundle() )
        return;

    this->layout.mf->forEachSection(^(const dyld3::MachOFile::SectionInfo& sectInfo, bool malformedSectionRange, bool& sectionStop) {
        uint8_t  sectionType  = (sectInfo.sectFlags & SECTION_TYPE);
        bool selfModifyingStub = (sectionType == S_SYMBOL_STUBS) && (sectInfo.sectFlags & S_ATTR_SELF_MODIFYING_CODE) && (sectInfo.reserved2 == 5) && (this->layout.mf->cputype == CPU_TYPE_I386);
        if ( (sectionType != S_LAZY_SYMBOL_POINTERS) && (sectionType != S_NON_LAZY_SYMBOL_POINTERS) && !selfModifyingStub )
            return;
        if ( (sectInfo.sectFlags & S_ATTR_SELF_MODIFYING_CODE) && !selfModifyingStub ) {
            diag.error("S_ATTR_SELF_MODIFYING_CODE section type only valid in old i386 binaries");
            sectionStop = true;
            return;
        }
        uint32_t elementSize = selfModifyingStub ? sectInfo.reserved2 : ptrSize;
        uint32_t elementCount = (uint32_t)(sectInfo.sectSize/elementSize);
        if ( dyld3::greaterThanAddOrOverflow(sectInfo.reserved1, elementCount, indirectSymbolTableCount) ) {
            diag.error("section %s overflows indirect symbol table", sectInfo.sectName);
            sectionStop = true;
            return;
        }

        for (uint32_t i=0; (i < elementCount) && !stop; ++i) {
            uint32_t symNum = indirectSymbolTable[sectInfo.reserved1 + i];
            if ( symNum == INDIRECT_SYMBOL_ABS )
                continue;
            if ( symNum == INDIRECT_SYMBOL_LOCAL ) {
                handler(sectInfo.sectAddr+i*elementSize, false, 0, "", false, false, false, stop);
                continue;
            }
            if ( symNum > symCount ) {
                diag.error("indirect symbol[%d] = %d which is invalid symbol index", sectInfo.reserved1 + i, symNum);
                sectionStop = true;
                return;
            }
            uint16_t n_desc = is64Bit ? symbols64[symNum].n_desc : symbols32[symNum].n_desc;
            uint8_t  n_type     = is64Bit ? symbols64[symNum].n_type : symbols32[symNum].n_type;
            uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
            uint32_t strOffset = is64Bit ? symbols64[symNum].n_un.n_strx : symbols32[symNum].n_un.n_strx;
            if ( strOffset > poolSize ) {
               diag.error("symbol[%d] string offset out of range", sectInfo.reserved1 + i);
                sectionStop = true;
                return;
            }
            const char* symbolName  = stringPool + strOffset;
            bool        weakImport  = (n_desc & N_WEAK_REF);
            bool        lazy        = (sectionType == S_LAZY_SYMBOL_POINTERS);
#if SUPPORT_PRIVATE_EXTERNS_WORKAROUND
            if ( lazy && ((n_type & N_PEXT) != 0) ) {
                // don't know why the static linker did not eliminate the internal reference to a private extern definition
                // As this is private extern, we know the symbol lookup will fail.  We also know that this is a lazy-bind, and so
                // there is a corresponding rebase.  The rebase will be run later, and will slide whatever value is in here.
                // So lets change the value in this slot, and let the existing rebase slide it for us
                // Note we only want to change the value in memory once, before rebases are applied.  We don't want to accidentally
                // change it again later.
                if ( supportPrivateExternsWorkaround ) {
                    uintptr_t* ptr = (uintptr_t*)((uint8_t*)(sectInfo.sectAddr+i*elementSize) + slide);
                    uint64_t n_value = is64Bit ? symbols64[symNum].n_value : symbols32[symNum].n_value;
                    *ptr = (uintptr_t)n_value;
                }
                continue;
            }
#endif
            // Handle defined weak def symbols which need to get a special ordinal
            if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) && ((n_desc & N_WEAK_DEF) != 0) )
                libOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
            handler(sectInfo.sectAddr+i*elementSize, true, libOrdinal, symbolName, weakImport, lazy, selfModifyingStub, stop);
        }
        sectionStop = stop;
    });
}

uint64_t Fixups::localRelocBaseAddress() const
{
    if ( this->layout.mf->isArch("x86_64") || this->layout.mf->isArch("x86_64h") ) {
#if BUILDING_APP_CACHE_UTIL
        if ( this->layout.mf->isKextBundle() ) {
            // for kext bundles the reloc base address starts at __TEXT segment
            return this->layout.segments[0].vmAddr;
        }
#endif
        // for all other kinds, the x86_64 reloc base address starts at first writable segment (usually __DATA)
        for ( const SegmentLayout& segment : this->layout.segments ) {
            if ( segment.writable() )
                return segment.vmAddr;
        }
    }
    return this->layout.segments[0].vmAddr;
}

uint64_t Fixups::externalRelocBaseAddress() const
{
    // Dyld caches are too large for a raw r_address, so everything is an offset from the base address
    if ( this->layout.mf->inDyldCache() ) {
        return this->layout.mf->preferredLoadAddress();
    }

#if BUILDING_APP_CACHE_UTIL
    if ( this->layout.mf->isKextBundle() ) {
        // for kext bundles the reloc base address starts at __TEXT segment
        return this->layout.mf->preferredLoadAddress();
    }
#endif

    if ( this->layout.mf->isArch("x86_64") || this->layout.mf->isArch("x86_64h") ) {
        // for x86_64 reloc base address starts at first writable segment (usually __DATA)
        for ( const SegmentLayout& segment : this->layout.segments ) {
            if ( segment.writable() )
                return segment.vmAddr;
        }
    }
    // For everyone else we start at 0
    return 0;
}

bool Fixups::segIndexAndOffsetForAddress(uint64_t addr, uint32_t& segIndex, uint64_t& segOffset) const
{
    for (uint32_t i=0; i < this->layout.segments.size(); ++i) {
        const SegmentLayout& segment = this->layout.segments[i];
        if ( (segment.vmAddr <= addr) && (addr < (segment.vmAddr + segment.vmSize)) ) {
            segIndex  = i;
            segOffset = addr - segment.vmAddr;
            return true;
        }
    }
    return false;
}

int Fixups::libOrdinalFromDesc(uint16_t n_desc) const
{
    // -flat_namespace is always flat lookup
    if ( (this->layout.mf->flags & MH_TWOLEVEL) == 0 )
        return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

    // extract byte from undefined symbol entry
    int libIndex = GET_LIBRARY_ORDINAL(n_desc);
    switch ( libIndex ) {
        case SELF_LIBRARY_ORDINAL:
            return BIND_SPECIAL_DYLIB_SELF;

        case DYNAMIC_LOOKUP_ORDINAL:
            return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

        case EXECUTABLE_ORDINAL:
            return BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE;
    }

    return libIndex;
}

// MARK: --- SplitSeg methods ---

SplitSeg::SplitSeg(const Layout& layout)
    : layout(layout)
{
}

bool SplitSeg::isV1() const
{
    if ( !this->layout.linkedit.splitSegInfo.hasValue() )
        return false;

    const void* splitSegStart = this->layout.linkedit.splitSegInfo.buffer;
    return (*(const uint8_t*)splitSegStart) != DYLD_CACHE_ADJ_V2_FORMAT;
}

bool SplitSeg::isV2() const
{
    if ( !this->layout.linkedit.splitSegInfo.hasValue() )
        return false;

    const void* splitSegStart = this->layout.linkedit.splitSegInfo.buffer;
    return (*(const uint8_t*)splitSegStart) == DYLD_CACHE_ADJ_V2_FORMAT;
}

bool SplitSeg::hasValue() const
{
    return this->layout.linkedit.splitSegInfo.hasValue();
}

void SplitSeg::forEachReferenceV2(Diagnostics& diag, ReferenceCallbackV2 callback) const
{
    const uint8_t* infoStart = layout.linkedit.splitSegInfo.buffer;
    const uint8_t* infoEnd = infoStart + layout.linkedit.splitSegInfo.bufferSize;

    if ( *infoStart++ != DYLD_CACHE_ADJ_V2_FORMAT ) {
        return;
    }

    // Whole         :== <count> FromToSection+
    // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
    // ToOffset         :== <to-sect-offset-delta> <count> FromOffset+
    // FromOffset     :== <kind> <count> <from-sect-offset-delta>
    const uint8_t* p = infoStart;
    uint64_t sectionCount = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
    for (uint64_t i=0; i < sectionCount; ++i) {
        uint64_t fromSectionIndex = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
        uint64_t toSectionIndex = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
        uint64_t toOffsetCount = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
        uint64_t toSectionOffset = 0;
        for (uint64_t j=0; j < toOffsetCount; ++j) {
            uint64_t toSectionDelta = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
            uint64_t fromOffsetCount = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
            toSectionOffset += toSectionDelta;
            for (uint64_t k=0; k < fromOffsetCount; ++k) {
                uint64_t kind = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
                if ( kind > 13 ) {
                    diag.error("bad kind (%llu) value in %s\n", kind, this->layout.mf->installName());
                }
                uint64_t fromSectDeltaCount = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
                uint64_t fromSectionOffset = 0;
                for (uint64_t l=0; l < fromSectDeltaCount; ++l) {
                    uint64_t delta = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
                    fromSectionOffset += delta;
                    bool stop = false;
                    callback(fromSectionIndex, fromSectionOffset, toSectionIndex, toSectionOffset, stop);
                    if ( stop )
                        return;
                }
            }
        }
    }
}


void SplitSeg::forEachSplitSegSection(void (^callback)(std::string_view segmentName,
                                                       std::string_view sectionName,
                                                       uint64_t sectionVMAddr)) const
{
    callback("mach header", "", 0);
    this->layout.mf->forEachSection(^(const dyld3::MachOFile::SectionInfo &sectInfo,
                                      bool malformedSectionRange, bool &stop) {
        std::string_view segmentName(sectInfo.segInfo.segName, strnlen(sectInfo.segInfo.segName, 16));
        std::string_view sectionName(sectInfo.sectName, strnlen(sectInfo.sectName, 16));
        callback(segmentName, sectionName, sectInfo.sectAddr);
    });
}

// MARK: --- ExportTrie methods ---

ExportTrie::ExportTrie(const Layout& layout)
    : layout(layout)
{
}

static void recurseTrie(Diagnostics& diag, const uint8_t* const start, const uint8_t* p, const uint8_t* const end,
                        dyld3::OverflowSafeArray<char>& cummulativeString, int curStrOffset, bool& stop,
                        ExportTrie::ExportsCallback callback)
{
    if ( p >= end ) {
        diag.error("malformed trie, node past end");
        return;
    }
    const uint64_t terminalSize = dyld3::MachOFile::read_uleb128(diag, p, end);
    const uint8_t* children = p + terminalSize;
    if ( terminalSize != 0 ) {
        uint64_t    imageOffset = 0;
        uint64_t    flags       = dyld3::MachOFile::read_uleb128(diag, p, end);
        uint64_t    other       = 0;
        const char* importName  = nullptr;
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            other = dyld3::MachOFile::read_uleb128(diag, p, end); // dylib ordinal
            importName = (char*)p;
        }
        else {
            imageOffset = dyld3::MachOFile::read_uleb128(diag, p, end);
            if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER )
                other = dyld3::MachOFile::read_uleb128(diag, p, end);
            else
                other = 0;
        }
        if ( diag.hasError() )
            return;
        callback(cummulativeString.begin(), imageOffset, flags, other, importName, stop);
        if ( stop )
            return;
    }
    if ( children > end ) {
        diag.error("malformed trie, terminalSize extends beyond trie data");
        return;
    }
    const uint8_t childrenCount = *children++;
    const uint8_t* s = children;
    for (uint8_t i=0; i < childrenCount; ++i) {
        int edgeStrLen = 0;
        while (*s != '\0') {
            cummulativeString.resize(curStrOffset+edgeStrLen + 1);
            cummulativeString[curStrOffset+edgeStrLen] = *s++;
            ++edgeStrLen;
            if ( s > end ) {
                diag.error("malformed trie node, child node extends past end of trie\n");
                return;
            }
       }
        cummulativeString.resize(curStrOffset+edgeStrLen + 1);
        cummulativeString[curStrOffset+edgeStrLen] = *s++;
        uint64_t childNodeOffset = dyld3::MachOFile::read_uleb128(diag, s, end);
        if (childNodeOffset == 0) {
            diag.error("malformed trie, childNodeOffset==0");
            return;
        }
        recurseTrie(diag, start, start+childNodeOffset, end, cummulativeString, curStrOffset+edgeStrLen, stop, callback);
        if ( diag.hasError() || stop )
            return;
    }
}

void ExportTrie::forEachExportedSymbol(Diagnostics& diag, ExportsCallback callback) const
{
    if ( layout.linkedit.exportsTrie.hasValue() ) {
        const uint8_t* trieStart   = layout.linkedit.exportsTrie.buffer;
        const uint8_t* trieEnd     = trieStart + layout.linkedit.exportsTrie.bufferSize;

        // We still emit empty export trie load commands just as a placeholder to show we have
        // no exports.  In that case, don't start recursing as we'll immediately think we ran
        // of the end of the buffer
        if ( trieStart == trieEnd )
            return;

        bool stop = false;
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
        recurseTrie(diag, trieStart, trieStart, trieEnd, cummulativeString, 0, stop, callback);
   }
}

// MARK: --- ChainedFixupPointerOnDisk methods ---

uint64_t ChainedFixupPointerOnDisk::Arm64e::unpackTarget() const
{
    assert(this->authBind.bind == 0);
    assert(this->authBind.auth == 0);
    return ((uint64_t)(this->rebase.high8) << 56) | (this->rebase.target);
}

uint64_t ChainedFixupPointerOnDisk::Arm64e::signExtendedAddend() const
{
    assert(this->authBind.bind == 1);
    assert(this->authBind.auth == 0);
    uint64_t addend19 = this->bind.addend;
    if ( addend19 & 0x40000 )
        return addend19 | 0xFFFFFFFFFFFC0000ULL;
    else
        return addend19;
}

const char* ChainedFixupPointerOnDisk::Arm64e::keyName(uint8_t keyBits)
{
    static const char* const names[] = {
        "IA", "IB", "DA", "DB"
    };
    assert(keyBits < 4);
    return names[keyBits];
}
const char* ChainedFixupPointerOnDisk::Arm64e::keyName() const
{
    assert(this->authBind.auth == 1);
    return keyName(this->authBind.key);
}

uint64_t ChainedFixupPointerOnDisk::Arm64e::signPointer(uint64_t unsignedAddr, void* loc, bool addrDiv, uint16_t diversity, uint8_t key)
{
    // don't sign NULL
    if ( unsignedAddr == 0 )
        return 0;

#if __has_feature(ptrauth_calls)
    uint64_t extendedDiscriminator = diversity;
    if ( addrDiv )
        extendedDiscriminator = __builtin_ptrauth_blend_discriminator(loc, extendedDiscriminator);
    switch ( key ) {
        case 0: // IA
            return (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 0, extendedDiscriminator);
        case 1: // IB
            return (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 1, extendedDiscriminator);
        case 2: // DA
            return (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 2, extendedDiscriminator);
        case 3: // DB
            return (uintptr_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 3, extendedDiscriminator);
    }
    assert(0 && "invalid signing key");
#else
    assert(0 && "arm64e signing only arm64e");
#endif
}


uint64_t ChainedFixupPointerOnDisk::Arm64e::signPointer(void* loc, uint64_t target) const
{
    assert(this->authBind.auth == 1);
    return signPointer(target, loc, authBind.addrDiv, authBind.diversity, authBind.key);
}

uint64_t ChainedFixupPointerOnDisk::Generic64::unpackedTarget() const
{
    return (((uint64_t)this->rebase.high8) << 56) | (uint64_t)(this->rebase.target);
}

uint64_t ChainedFixupPointerOnDisk::Generic64::signExtendedAddend() const
{
    uint64_t addend27     = this->bind.addend;
    uint64_t top8Bits     = addend27 & 0x00007F80000ULL;
    uint64_t bottom19Bits = addend27 & 0x0000007FFFFULL;
    uint64_t newValue     = (top8Bits << 13) | (((uint64_t)(bottom19Bits << 37) >> 37) & 0x00FFFFFFFFFFFFFF);
    return newValue;
}

const char* ChainedFixupPointerOnDisk::Kernel64::keyName() const
{
    static const char* names[] = {
        "IA", "IB", "DA", "DB"
    };
    assert(this->isAuth == 1);
    uint8_t keyBits = this->key;
    assert(keyBits < 4);
    return names[keyBits];
}

bool ChainedFixupPointerOnDisk::isRebase(uint16_t pointerFormat, uint64_t preferedLoadAddress, uint64_t& targetRuntimeOffset) const
{
    switch (pointerFormat) {
       case DYLD_CHAINED_PTR_ARM64E:
       case DYLD_CHAINED_PTR_ARM64E_USERLAND:
       case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
       case DYLD_CHAINED_PTR_ARM64E_KERNEL:
       case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
            if ( this->arm64e.bind.bind )
                return false;
            if ( this->arm64e.authRebase.auth ) {
                targetRuntimeOffset = this->arm64e.authRebase.target;
                return true;
            }
            else {
                targetRuntimeOffset = this->arm64e.unpackTarget();
                if ( (pointerFormat == DYLD_CHAINED_PTR_ARM64E) || (pointerFormat == DYLD_CHAINED_PTR_ARM64E_FIRMWARE) ) {
                    targetRuntimeOffset -= preferedLoadAddress;
                }
                return true;
            }
            break;
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET:
            if ( this->generic64.bind.bind )
                return false;
            targetRuntimeOffset = this->generic64.unpackedTarget();
            if ( pointerFormat == DYLD_CHAINED_PTR_64 )
                targetRuntimeOffset -= preferedLoadAddress;
            return true;
            break;
        case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
        case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
            targetRuntimeOffset = this->kernel64.target;
            return true;
            break;
        case DYLD_CHAINED_PTR_32:
            if ( this->generic32.bind.bind )
                return false;
            targetRuntimeOffset = this->generic32.rebase.target - preferedLoadAddress;
            return true;
            break;
        case DYLD_CHAINED_PTR_32_FIRMWARE:
            targetRuntimeOffset = this->firmware32.target - preferedLoadAddress;
            return true;
            break;
        default:
            break;
    }
    assert(0 && "unsupported pointer chain format");
}

bool ChainedFixupPointerOnDisk::isBind(uint16_t pointerFormat, uint32_t& bindOrdinal, int64_t& addend) const
{
    addend = 0;
    switch (pointerFormat) {
        case DYLD_CHAINED_PTR_ARM64E:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
            if ( !this->arm64e.authBind.bind )
                return false;
            if ( this->arm64e.authBind.auth ) {
                if ( pointerFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND24 )
                    bindOrdinal = this->arm64e.authBind24.ordinal;
                else
                    bindOrdinal = this->arm64e.authBind.ordinal;
                return true;
            }
            else {
                if ( pointerFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND24 )
                    bindOrdinal = this->arm64e.bind24.ordinal;
                else
                    bindOrdinal = this->arm64e.bind.ordinal;
                addend = this->arm64e.signExtendedAddend();
                return true;
            }
            break;
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET:
            if ( !this->generic64.bind.bind )
                return false;
            bindOrdinal = this->generic64.bind.ordinal;
            addend = this->generic64.bind.addend;
            return true;
            break;
        case DYLD_CHAINED_PTR_32:
            if ( !this->generic32.bind.bind )
                return false;
            bindOrdinal = this->generic32.bind.ordinal;
            addend = this->generic32.bind.addend;
            return true;
            break;
        case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
        case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
            return false;
        default:
            break;
    }
    assert(0 && "unsupported pointer chain format");
}

unsigned ChainedFixupPointerOnDisk::strideSize(uint16_t pointerFormat)
{
    switch (pointerFormat) {
        case DYLD_CHAINED_PTR_ARM64E:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            return 8;
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
        case DYLD_CHAINED_PTR_32_FIRMWARE:
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET:
        case DYLD_CHAINED_PTR_32:
        case DYLD_CHAINED_PTR_32_CACHE:
        case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
            return 4;
        case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
            return 1;
    }
    assert(0 && "unsupported pointer chain format");
}

// MARK: --- SymbolTable methods ---

SymbolTable::SymbolTable(const Layout& layout)
    : layout(layout)
{
}

void SymbolTable::forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    const bool is64Bit = this->layout.mf->is64();
    if ( this->layout.linkedit.symbolTable.hasValue() ) {
        uint32_t localsStartIndex = 0;
        uint32_t localsCount      = this->layout.linkedit.symbolTable.entryCount;
        if ( this->layout.linkedit.localSymbolTable.hasValue() ) {
            localsStartIndex = this->layout.linkedit.localSymbolTable.entryIndex;
            localsCount      = this->layout.linkedit.localSymbolTable.entryCount;
        }
        uint32_t               maxStringOffset  = this->layout.linkedit.symbolStrings.bufferSize;
        const char*            stringPool       = (char*)this->layout.linkedit.symbolStrings.buffer;
        const struct nlist*    symbols          = (struct nlist*)this->layout.linkedit.symbolTable.buffer;
        const struct nlist_64* symbols64        = (struct nlist_64*)this->layout.linkedit.symbolTable.buffer;
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


void SymbolTable::forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    const bool is64Bit = this->layout.mf->is64();
    if ( this->layout.linkedit.symbolTable.hasValue() ) {
        uint32_t globalsStartIndex = 0;
        uint32_t globalsCount      = this->layout.linkedit.symbolTable.entryCount;
        if ( this->layout.linkedit.globalSymbolTable.hasValue() ) {
            globalsStartIndex = this->layout.linkedit.globalSymbolTable.entryIndex;
            globalsCount      = this->layout.linkedit.globalSymbolTable.entryCount;
        }
        uint32_t               maxStringOffset  = this->layout.linkedit.symbolStrings.bufferSize;
        const char*            stringPool       = (char*)this->layout.linkedit.symbolStrings.buffer;
        const struct nlist*    symbols          = (struct nlist*)this->layout.linkedit.symbolTable.buffer;
        const struct nlist_64* symbols64        = (struct nlist_64*)this->layout.linkedit.symbolTable.buffer;
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


void SymbolTable::forEachImportedSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    const bool is64Bit = this->layout.mf->is64();
    if ( this->layout.linkedit.symbolTable.hasValue() ) {
        uint32_t undefsStartIndex = 0;
        uint32_t undefsCount      = this->layout.linkedit.symbolTable.entryCount;
        if ( this->layout.linkedit.undefSymbolTable.hasValue() ) {
            undefsStartIndex = this->layout.linkedit.undefSymbolTable.entryIndex;
            undefsCount      = this->layout.linkedit.undefSymbolTable.entryCount;
        }
        uint32_t               maxStringOffset  = this->layout.linkedit.symbolStrings.bufferSize;
        const char*            stringPool       = (char*)this->layout.linkedit.symbolStrings.buffer;
        const struct nlist*    symbols          = (struct nlist*)this->layout.linkedit.symbolTable.buffer;
        const struct nlist_64* symbols64        = (struct nlist_64*)this->layout.linkedit.symbolTable.buffer;
        bool                   stop             = false;
        for (uint32_t i=0; (i < undefsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[undefsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_TYPE) == N_UNDF )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[undefsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_TYPE) == N_UNDF )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

void SymbolTable::forEachIndirectSymbol(Diagnostics& diag,
                                        void (^callback)(const char* symbolName, uint32_t symNum)) const
{
    // find lazy and non-lazy pointer sections
    const bool              is64Bit                  = this->layout.mf->is64();
    const uint32_t* const   indirectSymbolTable      = (uint32_t*)this->layout.linkedit.indirectSymbolTable.buffer;
    const uint32_t          indirectSymbolTableCount = this->layout.linkedit.indirectSymbolTable.entryCount;
    const void*             symbolTable              = this->layout.linkedit.symbolTable.buffer;
    const struct nlist_64*  symbols64                = (nlist_64*)symbolTable;
    const struct nlist*     symbols32                = (struct nlist*)symbolTable;
    const char*             stringPool               = (char*)this->layout.linkedit.symbolStrings.buffer;
    uint32_t                symCount                 = this->layout.linkedit.symbolTable.entryCount;
    uint32_t                poolSize                 = this->layout.linkedit.symbolStrings.bufferSize;

    if ( indirectSymbolTableCount == 0 )
        return;

    for (uint32_t i = 0; i != indirectSymbolTableCount; ++i ) {
        uint32_t symNum = indirectSymbolTable[i];
        if ( symNum == INDIRECT_SYMBOL_ABS ) {
            // FIXME: The client wants to know about all the entries, so what should we pass here?
            callback(nullptr, symNum);
            continue;
        }
        if ( symNum == INDIRECT_SYMBOL_LOCAL ) {
            callback("", symNum);
            continue;
        }
        if ( symNum == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) ) {
            // FIXME: We are using the "local" callback. Should we use the "abs" one instead
            callback("", symNum);
            continue;
        }
        if ( symNum > symCount ) {
            diag.error("indirect symbol[%d] = %d which is invalid symbol index", i, symNum);
            return;
        }
        uint32_t strOffset = is64Bit ? symbols64[symNum].n_un.n_strx : symbols32[symNum].n_un.n_strx;
        if ( strOffset > poolSize ) {
            diag.error("symbol[%d] string offset out of range", i);
            return;
        }
        const char* symbolName  = stringPool + strOffset;
        callback(symbolName, symNum);
    }
}

} // namespace mach_o

