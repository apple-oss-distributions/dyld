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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <uuid/uuid.h>

#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <mach-o/reloc.h>
  #include <libc_private.h>
#endif // !TARGET_OS_EXCLAVEKIT

#include "Image.h"
#include "Misc.h"
#include "CompactUnwind.h"

namespace mach_o {

Image::Image(const void* buffer, size_t bufferSize, MappingKind kind)
    : _buffer((Header*)buffer), _bufferSize(bufferSize), _mappingKind(kind), _hasZerofillExpansion(false)
{
    // figure out location of LINKEDIT
    switch ( kind ) {
        case MappingKind::wholeSliceMapped:
            _hasZerofillExpansion = false;
            break;
        case MappingKind::dyldLoadedPreFixups:
        case MappingKind::dyldLoadedPostFixups:
            _hasZerofillExpansion = _buffer->hasZerofillExpansion();
            break;
        case MappingKind::unknown:
            _hasZerofillExpansion = inferIfZerofillExpanded();
            break;
    }
    _linkeditBias = _buffer->computeLinkEditBias(_hasZerofillExpansion);

    // minimal check of load commands
    if ( Error err = _buffer->validStructureLoadCommands(bufferSize) )
        return;

    // build parts
    makeExportsTrie();
    makeSymbolTable();
    makeRebaseOpcodes();
    makeBindOpcodes();
    makeLazyBindOpcodes();
    makeWeakBindOpcodes();
    makeChainedFixups();
    makeFunctionStarts();
    makeCompactUnwind();
    makeSplitSegInfo();
}

// for dyld loaded images only
Image::Image(const mach_header* mh)
: _buffer((Header*)mh), _bufferSize(0), _mappingKind(MappingKind::dyldLoadedPostFixups)
{
    _hasZerofillExpansion = ((Header*)mh)->hasZerofillExpansion();
    _linkeditBias         = _buffer->computeLinkEditBias(_hasZerofillExpansion);

    // build parts
    makeExportsTrie();
    makeSymbolTable();
    makeRebaseOpcodes();
    makeBindOpcodes();
    makeLazyBindOpcodes();
    makeWeakBindOpcodes();
    makeChainedFixups();
    makeFunctionStarts();
    makeCompactUnwind();
    makeSplitSegInfo();
}


// need move constructor because object has pointers to within itself (e.g. _exportsTrie points to _exportsTrieSpace)
Image::Image(const Image&& other)
    : _buffer(other._buffer), _bufferSize(other._bufferSize), _linkeditBias(other._linkeditBias),
      _mappingKind(other._mappingKind), _hasZerofillExpansion(other._hasZerofillExpansion)
{
    // build parts
    makeExportsTrie();
    makeSymbolTable();
    makeRebaseOpcodes();
    makeBindOpcodes();
    makeLazyBindOpcodes();
    makeWeakBindOpcodes();
    makeChainedFixups();
    makeFunctionStarts();
    makeCompactUnwind();
    makeSplitSegInfo();
}


// used to figure out of mach-o was mapped with zero fill or not
bool Image::inferIfZerofillExpanded() const
{
    // MH_PRELOAD files can only be wholeSliceMapped because load commands and linkedit are not in segments
    if ( _buffer->isPreload() || _buffer->isFileSet() )
        return false;

    // if file has no zero-fill, then both ways to load are the same
    if ( !_buffer->hasZerofillExpansion() )
        return false;

    // if file is code-signed, check for code-sig-magic in both possible locations
    {
        uint32_t sigFileOffset;
        uint32_t sigSize;
        if ( _buffer->hasCodeSignature(sigFileOffset, sigSize) ) {
            if ( sigFileOffset < _bufferSize ) {
                const uint32_t* unexpandedLoc = (uint32_t*)((uint8_t*)_buffer + sigFileOffset);
                if ( *unexpandedLoc == 0xc00cdefa ) // CSMAGIC_EMBEDDED_SIGNATURE
                    return false;
                const uint32_t* expandedLoc = (uint32_t*)((uint8_t*)_buffer + _buffer->zerofillExpansionAmount() + sigFileOffset);
                if ( *expandedLoc == 0xc00cdefa )
                    return true;
            }
        }
    }

    // FIXME:
    assert(false && "handle unsigned");

    return false;
}

Error Image::validate() const
{
    // validate mach_header and load commands
    if ( Error err = _buffer->valid(_bufferSize) )
        return err;

    // create Policy object for this binary
    Policy policy(_buffer->arch(), _buffer->platformAndVersions(), _buffer->mh.filetype, false);

    // validate initializers
    if ( Error err = this->validInitializers(policy) )
        return err;

    // validate LINKEDIT
    if ( Error err = this->validLinkedit(policy) )
        return err;

    return Error::none();
}

Error Image::validLinkedit(const Policy& policy) const
{
    // validate structure of linkedit
    if ( Error err = validStructureLinkedit(policy) )
        return err;

    // if image has an exports trie, validate that
    if ( this->hasExportsTrie() ) {
        uint64_t max = 0x200000000; // FIXME
        if ( Error err = this->exportsTrie().valid(max) )
            return err;
    }

    // if image has a symbol table, validate that
    if ( this->hasSymbolTable() ) {
        uint64_t max = 0x200000000; // FIXME
        if ( Error err = this->symbolTable().valid(max) )
            return err;
    }

    uint32_t segCount = this->segmentCount();
    MappedSegment segs[segCount];
    for (uint32_t i=0; i < segCount; ++i)
        segs[i] = this->segment(i);
    std::span<const MappedSegment> segSpan{segs, segCount};

    // if image has rebase opcodes
    if ( this->hasRebaseOpcodes() ) {
        if ( Error err = this->rebaseOpcodes().valid(segSpan, _buffer->mayHaveTextFixups(), policy.enforceFixupsInWritableSegments()) )
            return err;
    }

    // if image has bind opcodes
    if ( this->hasBindOpcodes() ) {
        if ( Error err = this->bindOpcodes().valid(segSpan, _buffer->linkedDylibCount(), _buffer->mayHaveTextFixups(), policy.enforceFixupsInWritableSegments()) )
            return err;
    }

    // if image has lazy bind opcodes
    if ( this->hasLazyBindOpcodes() ) {
        if ( Error err = this->lazyBindOpcodes().valid(segSpan, _buffer->linkedDylibCount(), _buffer->mayHaveTextFixups(), policy.enforceFixupsInWritableSegments()) )
            return err;
    }

    // if image has chained fixups
    if ( this->hasChainedFixups() ) {
        if ( Error err = this->chainedFixups().valid(_buffer->preferredLoadAddress(), segSpan) )
            return err;
    }

    return Error::none();
}

namespace {
    struct LinkEditContentChunk
    {
        const char* name;
        uint32_t    alignment;
        uint32_t    fileOffset;
        size_t      size;

        // only have a few chunks, so bubble sort is ok.  Don't use libc's qsort because it may call malloc
        static void sort(LinkEditContentChunk array[], unsigned long count)
        {
            for ( unsigned i = 0; i < count - 1; ++i ) {
                bool done = true;
                for ( unsigned j = 0; j < count - i - 1; ++j ) {
                    if ( array[j].fileOffset > array[j + 1].fileOffset ) {
                        LinkEditContentChunk temp = array[j];
                        array[j]                  = array[j + 1];
                        array[j + 1]              = temp;
                        done                      = false;
                    }
                }
                if ( done )
                    break;
            }
        }
    };
} // anonymous namespace

#if !TARGET_OS_EXCLAVEKIT
Error Image::validStructureLinkedit(const Policy& policy) const
{
    // build vector of all blobs in LINKEDIT
    const uint32_t       ptrSize = _buffer->pointerSize();
    LinkEditContentChunk blobs[32];
    __block LinkEditContentChunk* bp                = blobs;
    __block uint32_t              symCount          = 0;
    __block uint32_t              indSymCount       = 0;
    __block bool                  hasIndSymTab      = false;
    __block bool                  hasLocalRelocs    = false;
    __block bool                  hasExternalRelocs = false;
    __block bool                  hasDyldInfo       = false;
    __block bool                  hasChainedFixups  = false;
    __block Error                 lcError;
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_SYMTAB: {
                const symtab_command* symbTabCmd = (symtab_command*)cmd;
                symCount                         = symbTabCmd->nsyms;
                if ( symCount != 0 ) {
                    if ( symCount > 0x10000000 ) {
                        lcError = Error("malformed mach-o image: symbol table too large");
                        stop    = true;
                    }
                    size_t size = symCount * (ptrSize == 8 ? sizeof(nlist_64) : sizeof(struct nlist));
                    *bp++       = { "symbol table", ptrSize, symbTabCmd->symoff, (uint32_t)size };
                }
                if ( symbTabCmd->strsize != 0 )
                    *bp++ = { "symbol table strings", 1, symbTabCmd->stroff, symbTabCmd->strsize };
            } break;
            case LC_DYSYMTAB: {
                hasIndSymTab = true;
                const dysymtab_command* dySymTabCmd = (dysymtab_command*)cmd;
                if ( dySymTabCmd->nindirectsyms > 0x10000000 ) {
                    lcError = Error("malformed mach-o image: indirect symbol table too large");
                    stop    = true;
                }
                else if ( dySymTabCmd->ilocalsym != 0 ) {
                    lcError = Error("malformed mach-o image: indirect symbol table ilocalsym != 0");
                    stop    = true;
                }
                else if ( dySymTabCmd->iextdefsym != dySymTabCmd->nlocalsym ) {
                    lcError = Error("malformed mach-o image: indirect symbol table iextdefsym != nlocalsym");
                    stop    = true;
                }
                else if ( dySymTabCmd->iundefsym != (dySymTabCmd->iextdefsym + dySymTabCmd->nextdefsym) ) {
                    lcError = Error("malformed mach-o image: indirect symbol table iundefsym != iextdefsym+nextdefsym");
                    stop    = true;
                }
                indSymCount = dySymTabCmd->iundefsym + dySymTabCmd->nundefsym;
                if ( dySymTabCmd->nlocrel != 0 ) {
                    hasLocalRelocs = true;
                    *bp++          = { "local relocations", ptrSize, dySymTabCmd->locreloff, dySymTabCmd->nlocrel * sizeof(relocation_info) };
                }
                if ( dySymTabCmd->nextrel != 0 ) {
                    hasExternalRelocs = true;
                    *bp++             = { "external relocations", ptrSize, dySymTabCmd->extreloff, dySymTabCmd->nextrel * sizeof(relocation_info) };
                }
                if ( dySymTabCmd->nindirectsyms != 0 )
                    *bp++ = { "indirect symbol table", 4, dySymTabCmd->indirectsymoff, dySymTabCmd->nindirectsyms * 4 };
            } break;
            case LC_DYLD_INFO_ONLY:
                hasDyldInfo = true;
                [[clang::fallthrough]];
            case LC_DYLD_INFO: {
                const dyld_info_command* dyldInfoCmd = (dyld_info_command*)cmd;
                if ( dyldInfoCmd->rebase_size != 0 )
                    *bp++ = { "rebase opcodes", ptrSize, dyldInfoCmd->rebase_off, dyldInfoCmd->rebase_size };
                if ( dyldInfoCmd->bind_size != 0 )
                    *bp++ = { "bind opcodes", ptrSize, dyldInfoCmd->bind_off, dyldInfoCmd->bind_size };
                if ( dyldInfoCmd->weak_bind_size != 0 )
                    *bp++ = { "weak bind opcodes", ptrSize, dyldInfoCmd->weak_bind_off, dyldInfoCmd->weak_bind_size };
                if ( dyldInfoCmd->lazy_bind_size != 0 )
                    *bp++ = { "lazy bind opcodes", ptrSize, dyldInfoCmd->lazy_bind_off, dyldInfoCmd->lazy_bind_size };
                if ( dyldInfoCmd->export_size != 0 )
                    *bp++ = { "exports trie", ptrSize, dyldInfoCmd->export_off, dyldInfoCmd->export_size };
            } break;
            case LC_SEGMENT_SPLIT_INFO: {
                const linkedit_data_command* splitSegCmd = (linkedit_data_command*)cmd;
                if ( splitSegCmd->datasize != 0 )
                    *bp++ = { "shared cache info", ptrSize, splitSegCmd->dataoff, splitSegCmd->datasize };
            } break;
            case LC_ATOM_INFO: {
                const linkedit_data_command* relinkCmd = (linkedit_data_command*)cmd;
                if ( relinkCmd->datasize != 0 )
                    *bp++ = { "atom info", ptrSize, relinkCmd->dataoff, relinkCmd->datasize };
            } break;
            case LC_FUNCTION_STARTS: {
                const linkedit_data_command* funStartsCmd = (linkedit_data_command*)cmd;
                if ( funStartsCmd->datasize != 0 )
                    *bp++ = { "function starts", ptrSize, funStartsCmd->dataoff, funStartsCmd->datasize };
            } break;
            case LC_DATA_IN_CODE: {
                const linkedit_data_command* dataInCodeCmd = (linkedit_data_command*)cmd;
                if ( dataInCodeCmd->datasize != 0 )
                    *bp++ = { "data in code", ptrSize, dataInCodeCmd->dataoff, dataInCodeCmd->datasize };
            } break;
            case LC_CODE_SIGNATURE: {
                const linkedit_data_command* codeSigCmd = (linkedit_data_command*)cmd;
                if ( codeSigCmd->datasize != 0 )
                    *bp++ = { "code signature", ptrSize, codeSigCmd->dataoff, codeSigCmd->datasize };
            } break;
            case LC_DYLD_EXPORTS_TRIE: {
                const linkedit_data_command* exportsTrieCmd = (linkedit_data_command*)cmd;
                if ( exportsTrieCmd->datasize != 0 )
                    *bp++ = { "exports trie", ptrSize, exportsTrieCmd->dataoff, exportsTrieCmd->datasize };
            } break;
            case LC_DYLD_CHAINED_FIXUPS: {
                const linkedit_data_command* chainedFixupsCmd = (linkedit_data_command*)cmd;
                hasChainedFixups                              = true;
                if ( chainedFixupsCmd->datasize != 0 )
                    *bp++ = { "chained fixups", ptrSize, chainedFixupsCmd->dataoff, chainedFixupsCmd->datasize };
            } break;
        }
    });
    if ( lcError )
        return std::move(lcError);
    if ( hasIndSymTab && (symCount != indSymCount))
        return Error("symbol count from symbol table and dynamic symbol table differ");

    // check for bad combinations
    if ( hasDyldInfo && policy.enforceOneFixupEncoding() ) {
        if ( hasLocalRelocs )
            return Error("malformed mach-o contains LC_DYLD_INFO_ONLY and local relocations");
        if ( hasExternalRelocs )
            return Error("malformed mach-o contains LC_DYLD_INFO_ONLY and external relocations");
    }
    if ( hasChainedFixups ) {
        if ( hasLocalRelocs )
            return Error("malformed mach-o contains LC_DYLD_CHAINED_FIXUPS and local relocations");
        if ( hasExternalRelocs )
            return Error("malformed mach-o contains LC_DYLD_CHAINED_FIXUPS and external relocations");
    }
    if ( hasDyldInfo && hasChainedFixups )
        return Error("malformed mach-o contains LC_DYLD_INFO and LC_DYLD_CHAINED_FIXUPS");

    // find range of LINKEDIT
    __block uint64_t linkeditFileOffsetStart = 0;
    __block uint64_t linkeditFileOffsetEnd   = 0;
    if ( _buffer->isObjectFile() || _buffer->isPreload() ) {
        // .o  and -preload files don't have LINKEDIT, but the LINKEDIT content is still at the end of the file after the last section content
        _buffer->forEachSection(^(const Header::SectionInfo& info, bool& stop) {
            uint8_t sectType = (info.flags & SECTION_TYPE);
            bool isZeroFill = ((sectType == S_ZEROFILL) || (sectType == S_THREAD_LOCAL_ZEROFILL));
            if ( isZeroFill )
                return;
            uint64_t sectionEnd = info.fileOffset + info.size;
            if ( sectionEnd > linkeditFileOffsetStart )
                linkeditFileOffsetStart = sectionEnd;
        });
        linkeditFileOffsetEnd   = _bufferSize;
        if ( linkeditFileOffsetStart == 0 ) {
            // if all sections are zerofill sections, look for symbol table as start of linkedit
            _buffer->forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
                if ( cmd->cmd == LC_SYMTAB ) {
                    const symtab_command* symTab = (symtab_command*)cmd;
                    linkeditFileOffsetStart = symTab->symoff;
                }
            });
        }
    }
    else {
        (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
            if ( cmd->cmd == LC_SEGMENT_64 ) {
                const segment_command_64* segCmd = (segment_command_64*)cmd;
                if ( strcmp(segCmd->segname, "__LINKEDIT") == 0 ) {
                    linkeditFileOffsetStart = segCmd->fileoff;
                    linkeditFileOffsetEnd   = segCmd->fileoff + segCmd->filesize;
                    stop                    = true;
                }
            }
            else if ( cmd->cmd == LC_SEGMENT ) {
                const segment_command* segCmd = (segment_command*)cmd;
                if ( strcmp(segCmd->segname, "__LINKEDIT") == 0 ) {
                    linkeditFileOffsetStart = segCmd->fileoff;
                    linkeditFileOffsetEnd   = segCmd->fileoff + segCmd->filesize;
                    stop                    = true;
                }
            }
        });
        if ( (linkeditFileOffsetStart == 0) || (linkeditFileOffsetEnd == 0) )
            return Error("bad or unknown fileoffset/size for LINKEDIT");
    }

    // sort blobs by file-offset and check for overlaps
    const unsigned long blobCount = bp - blobs;
    if ( blobCount == 0 ) {
        // ok for .o files to have no content and no symbols
        if ( _buffer->isObjectFile() )
            return Error::none();
        return Error("malformed mach-o has no LINKEDIT information");
    }
    LinkEditContentChunk::sort(blobs, blobCount);
    uint64_t    prevEnd  = linkeditFileOffsetStart;
    const char* prevName = "start of LINKEDIT";
    for ( unsigned long i = 0; i < blobCount; ++i ) {
        const LinkEditContentChunk& blob = blobs[i];
        if ( blob.fileOffset < prevEnd ) {
            return Error("LINKEDIT overlap of %s and %s", prevName, blob.name);
        }
        if ( greaterThanAddOrOverflow((uint64_t)blob.fileOffset, blob.size, linkeditFileOffsetEnd) ) {
            return Error("LINKEDIT content '%s' extends beyond end of segment", blob.name);
        }
        if ( (blob.fileOffset & (blob.alignment - 1)) != 0 ) {
            // <rdar://problem/51115705> relax code sig alignment for pre iOS 13
            if ( strcmp(blob.name, "code signature") == 0 ) {
                if ( policy.enforceCodeSignatureAligned() )
                    return Error("mis-aligned code signature");
            }
            else {
                if ( policy.enforceLinkeditContentAlignment() )
                    return Error("mis-aligned LINKEDIT content '%s'", blob.name);
            }
        }
        prevEnd  = blob.fileOffset + blob.size;
        prevName = blob.name;
    }

    return Error::none();
}

struct VIS_HIDDEN SegmentRanges
{
    struct SegmentRange {
        uint64_t vmAddrStart;
        uint64_t vmAddrEnd;
        uint32_t fileSize;
    };

    bool contains(uint64_t vmAddr) const {
        for (const SegmentRange& range : segments) {
            if ( (range.vmAddrStart <= vmAddr) && (vmAddr < range.vmAddrEnd) )
                return true;
        }
        return false;
    }

private:
    SegmentRange localAlloc[8];

public:
    dyld3::Array<SegmentRange> segments { localAlloc, sizeof(localAlloc) / sizeof(localAlloc[0]) };
};


Error Image::validInitializers(const Policy& policy) const
{
    uint64_t        prefLoadAddress = header()->preferredLoadAddress();
    uint64_t        slide           = header()->getSlide();
    __block Error   anErr;

    __block SegmentRanges executableSegments;
    header()->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( (info.perms & VM_PROT_EXECUTE) != 0 ) {
            executableSegments.segments.push_back({ info.vmaddr, info.vmaddr + info.vmsize, (uint32_t)info.fileSize });
        }
    });
    if (executableSegments.segments.empty()) {
        return Error("no executable segments");
    }

    // validate LC_ROUTINES initializer
    header()->forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ROUTINES ) {
            const routines_command* routines = (routines_command*)cmd;
            uint64_t dashInitAddr = routines->init_address;
            if ( !executableSegments.contains(dashInitAddr) ) {
                anErr = Error("LC_ROUTINES initializer 0x%08llX is not an offset to an executable segment", dashInitAddr);
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_ROUTINES_64 ) {
            const routines_command_64* routines = (routines_command_64*)cmd;
            uint64_t dashInitAddr = routines->init_address;
            if ( !executableSegments.contains(dashInitAddr) ) {
                anErr = Error("LC_ROUTINES _64 initializer 0x%08llX is not an offset to an executable segment", dashInitAddr);
                stop = true;
            }
        }
    });

    // validate any function pointers in __DATA,__mod_init_func section
    header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        uint8_t sectType = (sectInfo.flags & SECTION_TYPE);
        if ( (sectType == S_MOD_INIT_FUNC_POINTERS) || (sectType == S_MOD_TERM_FUNC_POINTERS)  ) {
            if ( (sectInfo.size % header()->pointerSize()) != 0 ) {
                anErr = Error("section %s/%s size (%llu) is not a multiple of pointer-size",
                              sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str(), sectInfo.size);
                stop = true;
                return;
            }
            if ( (sectInfo.address % header()->pointerSize()) != 0 ) {
                anErr = Error("section %s/%s address (0x%llX) is not pointer aligned",
                              sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str(), sectInfo.address);
                stop = true;
                return;
            }
            const uint8_t* sectionContent = (uint8_t*)header() + sectInfo.fileOffset;
            if ( header()->inDyldCache() )
                sectionContent = (uint8_t*)(sectInfo.address + header()->getSlide());
#if BUILDING_DYLD
            // in dyld, when this is called, the image is already rebased, so we can use pointers in section
            const uintptr_t* initsStart = (uintptr_t*)sectionContent;
            const uintptr_t* initsEnd   = (uintptr_t*)((uint8_t*)sectionContent + sectInfo.size);
            for (const uintptr_t* p=initsStart; p < initsEnd; ++p) {
                if ( !executableSegments.contains(*p) ) {
                    anErr = Error("initializer 0x%08lX is not in an executable segment", *p);
                    break;
                }
            }
#else
            if ( header()->is64() ) {
                const uint64_t* initsStart = (uint64_t*)sectionContent;
                const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)sectionContent + sectInfo.size);
                for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                    uint64_t anInit = *p;
                    // FIXME: as a quick hack, the low 32-bits with either rebase opcodes or chained fixups is offset in image
                    uint32_t low32 = (uint32_t)anInit;
                    if ( !executableSegments.contains(prefLoadAddress+low32) ) {
                        anErr = Error("initializer %lu/%llu is not in an executable segment", p-initsStart, sectInfo.size/8);
                        break;
                    }
                }
            }
            else {
                const uint32_t* initsStart = (uint32_t*)sectionContent;
                const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)sectionContent + sectInfo.size);
                for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                    uint32_t anInit = *p;
                    // FIXME: as a quick hack, the low 26-bits with either rebase opcodes or chained fixups is offset in image
                    uint32_t low26 = anInit & 0x03FFFFFF;
                    if ( !executableSegments.contains(prefLoadAddress+low26) ) {
                        anErr = Error("initializer %lu/%llu is not in an executable segment", p-initsStart, sectInfo.size/84);
                        break;
                    }
                }
            }
#endif
            if ( sectType == S_MOD_TERM_FUNC_POINTERS ) {
                if ( header()->isDyldManaged() && header()->isArch("arm6e") )
                    anErr = Error("terminators section %s/%s not supported for arm64e", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
            }
        }
    });
    if ( anErr.hasError() )
        return std::move(anErr);

    // validate offsets in __TEXT,__init_offsets
    header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.flags & SECTION_TYPE) == S_INIT_FUNC_OFFSETS ) {
            const uint8_t* content = (uint8_t*)(sectInfo.address + slide);
            if ( sectInfo.segPerms & VM_PROT_WRITE ) {
                anErr = Error("initializer offsets section %s/%s must be in read-only segment", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
                stop = true;
                return;
            }
            if ( (sectInfo.size % 4) != 0 ) {
                anErr = Error("initializer offsets section %s/%s has bad size", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
                stop = true;
                return;
            }
            if ( (sectInfo.address % 4) != 0 ) {
                anErr = Error("initializer offsets section %s/%s is not 4-byte aligned", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
                stop = true;
                return;
            }
            const uint32_t* initsStart = (uint32_t*)content;
            const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + sectInfo.size);
            for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                uint32_t anInitOffset = *p;
                if ( !executableSegments.contains(prefLoadAddress + anInitOffset) ) {
                    anErr = Error("initializer 0x%08X is not an offset to an executable segment", anInitOffset);
                    stop = true;
                    break;
                }
            }
        }
    });
    if ( anErr.hasError() )
        return std::move(anErr);

    return Error::none();
}

void Image::makeExportsTrie()
{
    // if image has an exports trie, use placement new to build ExportTrie object in _exportsTrieSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_DYLD_EXPORTS_TRIE ) {
            const linkedit_data_command* exportsTrie = (linkedit_data_command*)cmd;
            if ( exportsTrie->dataoff != 0 )
                _exportsTrie = new (_exportsTrieSpace) ExportsTrie(_linkeditBias + exportsTrie->dataoff, exportsTrie->datasize);
        }
        else if ( (cmd->cmd == LC_DYLD_INFO) || (cmd->cmd == LC_DYLD_INFO_ONLY) ) {
            const dyld_info_command* dyldInfo = (dyld_info_command*)cmd;
            if ( dyldInfo->export_off != 0 )
                _exportsTrie = new (_exportsTrieSpace) ExportsTrie(_linkeditBias + dyldInfo->export_off, dyldInfo->export_size);
        }
    });
}

void Image::makeSymbolTable()
{
    // if image has an nlist symbol table, use placement new to build SymbolTable object in _symbolTableSpace
    __block const symtab_command* symTabCmd      = nullptr;
    __block const dysymtab_command* dynSymTabCmd = nullptr;
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SYMTAB ) {
            symTabCmd = (symtab_command*)cmd;
        }
        else if ( cmd->cmd == LC_DYSYMTAB ) {
            dynSymTabCmd = (dysymtab_command*)cmd;
        }
    });

    if ( symTabCmd == nullptr )
        return;

    uint32_t nlocalsym  = 0;
    uint32_t nextdefsym = 0;
    uint32_t nundefsym  = 0;
    if ( dynSymTabCmd != nullptr ) {
        // some .o files do not have LC_DYSYMTAB
        nlocalsym  = dynSymTabCmd->nlocalsym;
        nextdefsym = dynSymTabCmd->nextdefsym;
        nundefsym  = dynSymTabCmd->nundefsym;
    }
    if ( _buffer->is64() ) {
        uint64_t               preferredLoadAddress = _buffer->preferredLoadAddress();
        const struct nlist_64* nlistArray           = (struct nlist_64*)(_linkeditBias + symTabCmd->symoff);
        _symbolTable                                = new (_symbolTableSpace) NListSymbolTable(preferredLoadAddress, nlistArray, symTabCmd->nsyms, (char*)_linkeditBias + symTabCmd->stroff,
                                                           symTabCmd->strsize, nlocalsym, nextdefsym, nundefsym);
    }
    else {
        uint32_t            preferredLoadAddress = (uint32_t)_buffer->preferredLoadAddress();
        const struct nlist* nlistArray           = (struct nlist*)(_linkeditBias + symTabCmd->symoff);
        _symbolTable                             = new (_symbolTableSpace) NListSymbolTable(preferredLoadAddress, nlistArray, symTabCmd->nsyms, (char*)_linkeditBias + symTabCmd->stroff,
                                                           symTabCmd->strsize, nlocalsym, nextdefsym, nundefsym);
    }
}

void Image::makeRebaseOpcodes()
{
    // if image has an rebase opcpdes, use placement new to build RebaseOpcodes object in _rebaseOpcodesSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_DYLD_INFO) || (cmd->cmd == LC_DYLD_INFO_ONLY) ) {
            const dyld_info_command* dyldInfoCmd = (dyld_info_command*)cmd;
            if ( dyldInfoCmd->rebase_size != 0 )
                _rebaseOpcodes = new (_rebaseOpcodesSpace) RebaseOpcodes(_linkeditBias + dyldInfoCmd->rebase_off, dyldInfoCmd->rebase_size, _buffer->is64());
        }
    });
}

void Image::makeBindOpcodes()
{
    // if image has an rebase opcpdes, use placement new to build BindOpcodes object in _bindOpcodesSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_DYLD_INFO) || (cmd->cmd == LC_DYLD_INFO_ONLY) ) {
            const dyld_info_command* dyldInfoCmd = (dyld_info_command*)cmd;
            if ( dyldInfoCmd->bind_size != 0 )
                _bindOpcodes = new (_bindOpcodesSpace) BindOpcodes(_linkeditBias + dyldInfoCmd->bind_off, dyldInfoCmd->bind_size, _buffer->is64());
        }
    });
}

void Image::makeLazyBindOpcodes()
{
    // if image has an rebase opcpdes, use placement new to build LazyBindOpcodes object in _lazyBindOpcodesSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_DYLD_INFO) || (cmd->cmd == LC_DYLD_INFO_ONLY) ) {
            const dyld_info_command* dyldInfoCmd = (dyld_info_command*)cmd;
            if ( dyldInfoCmd->lazy_bind_size != 0 )
                _lazyBindOpcodes = new (_lazyBindOpcodesSpace) LazyBindOpcodes(_linkeditBias + dyldInfoCmd->lazy_bind_off, dyldInfoCmd->lazy_bind_size, _buffer->is64());
        }
    });
}

void Image::makeWeakBindOpcodes()
{
    // if image has an rebase opcpdes, use placement new to build BindOpcodes object in _weakBindOpcodesSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_DYLD_INFO) || (cmd->cmd == LC_DYLD_INFO_ONLY) ) {
            const dyld_info_command* dyldInfoCmd = (dyld_info_command*)cmd;
            if ( dyldInfoCmd->weak_bind_size != 0 )
                _weakBindOpcodes = new (_weakBindOpcodesSpace) BindOpcodes(_linkeditBias + dyldInfoCmd->weak_bind_off, dyldInfoCmd->weak_bind_size, _buffer->is64());
        }
    });
}

void Image::makeChainedFixups()
{
    // if image has an fixup chains, use placement new to build ChainedFixups object in _chainedFixupsSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_DYLD_CHAINED_FIXUPS ) {
            const linkedit_data_command* chainedFixupsCmd = (linkedit_data_command*)cmd;
            if ( chainedFixupsCmd->datasize != 0 ) {
                const dyld_chained_fixups_header* fixupsHeader = (dyld_chained_fixups_header*)(_linkeditBias + chainedFixupsCmd->dataoff);
                _chainedFixups = new (_chainedFixupsSpace) ChainedFixups(fixupsHeader, chainedFixupsCmd->datasize);
            }
        }
    });
}

void Image::makeFunctionStarts()
{
    // if image has an function starts, use placement new to build FunctionStarts object in _functionStartsSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_FUNCTION_STARTS ) {
            const linkedit_data_command* funcStartsCmd = (linkedit_data_command*)cmd;
            if ( funcStartsCmd->datasize != 0 ) {
                const uint8_t* functionsStartBytes = (uint8_t*)(_linkeditBias + funcStartsCmd->dataoff);
                _functionStarts = new (_functionStartsSpace) FunctionStarts(functionsStartBytes, funcStartsCmd->datasize);
            }
        }
    });
}

void Image::makeCompactUnwind()
{
    // if image has an a compact unwind section, use placement new to build CompactUnwind object in _compactUnwindSpace
    _buffer->forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__unwind_info") && info.segmentName.starts_with("__TEXT") ) {
            const uint8_t* sectionContent = (uint8_t*)_buffer + info.fileOffset;
            _compactUnwind = new (_compactUnwindSpace) CompactUnwind(_buffer->arch(), sectionContent, (size_t)info.size);
            stop   = true;
        }
    });
}

void Image::makeSplitSegInfo()
{
    // if image has a split seg info load command, use placement new to build SplitSegInfo object in _splitSegInfoSpace
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO ) {
            const linkedit_data_command* splitSegCmd = (linkedit_data_command*)cmd;
            const uint8_t* startBytes = (uint8_t*)(_linkeditBias + splitSegCmd->dataoff);
            _splitSegInfo = new (_splitSegSpace) SplitSegInfo(startBytes, splitSegCmd->datasize);
            stop = true;
        }
    });
}

uint32_t Image::segmentCount() const
{
    __block uint32_t count = 0;
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_SEGMENT) || (cmd->cmd == LC_SEGMENT_64) )
            ++count;
    });
    return count;
}

MappedSegment Image::segment(uint32_t segIndex) const
{
    __block MappedSegment result;
    __block uint32_t      curSegIndex = 0;
    __block uint64_t      textVmAddr  = 0;
    (void)_buffer->forEachLoadCommand(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 )
                textVmAddr = segCmd->vmaddr;
            if ( curSegIndex == segIndex ) {
                result.runtimeOffset = segCmd->vmaddr - textVmAddr;
                if ( _hasZerofillExpansion )
                    result.content = (uint8_t*)_buffer + (segCmd->vmaddr - textVmAddr);
                else
                    result.content = (uint8_t*)_buffer + segCmd->fileoff;
                result.runtimeSize  = segCmd->vmsize;
                result.fileOffset   = segCmd->fileoff;
                result.segName      = segCmd->segname;
                result.readable     = ((segCmd->initprot & VM_PROT_READ)    != 0);
                result.writable     = ((segCmd->initprot & VM_PROT_WRITE)   != 0);
                result.executable   = ((segCmd->initprot & VM_PROT_EXECUTE) != 0);
                stop = true;
            }
            ++curSegIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) 
                textVmAddr = segCmd->vmaddr;
           if ( curSegIndex == segIndex ) {
                result.runtimeOffset = segCmd->vmaddr - textVmAddr;
                if ( _hasZerofillExpansion )
                    result.content = (uint8_t*)_buffer + (segCmd->vmaddr - textVmAddr);
                else
                    result.content = (uint8_t*)_buffer + segCmd->fileoff;
                result.runtimeSize  = segCmd->vmsize;
                result.fileOffset   = segCmd->fileoff;
                result.segName      = segCmd->segname;
                result.readable     = ((segCmd->initprot & VM_PROT_READ)    != 0);
                result.writable     = ((segCmd->initprot & VM_PROT_WRITE)   != 0);
                result.executable   = ((segCmd->initprot & VM_PROT_EXECUTE) != 0);
                stop = true;
            }
            ++curSegIndex;
        }
    });
    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT

void Image::withSegments(void (^callback)(std::span<const MappedSegment> segments)) const
{
    const uint32_t count = segmentCount();
    MappedSegment segments[count];
    for (uint32_t segIndex=0; segIndex < count; ++segIndex)
        segments[segIndex] = this->segment(segIndex);

    callback(std::span(segments, count));
}

// This is a high level abstraction for mach-o files.  No matter the format, it returns all bind targets
void Image::forEachBindTarget(void (^callback)(const Fixup::BindTarget& targetInfo, bool& stop)) const
{
    if ( this->hasChainedFixups() ) {
        this->chainedFixups().forEachBindTarget(callback);
    }
    else if ( this->hasBindOpcodes() ) {
        // FIXME: Do we want to pass up the strong binds?
        this->bindOpcodes().forEachBindTarget(callback, ^(const char* symbolName) { });

        if ( hasLazyBindOpcodes() )
            this->lazyBindOpcodes().forEachBindTarget(callback, ^(const char* symbolName) { });
    }
}


// This is a high level abstraction for mach-o files.  No matter the format, it iterates all fixups
void Image::forEachFixup(void (^callback)(const Fixup& fixup, bool& stop)) const
{
    const uint64_t prefLoadAddr = this->header()->preferredLoadAddress();
    this->withSegments(^(std::span<const MappedSegment> segments) {
        uint16_t           fwPointerFormat;
        uint32_t           fwStartsCount;
        const uint32_t*    fwStarts;
        if ( this->hasChainedFixups() ) {
            // userland binary with LC_DYLD_CHAINED_FIXUPS
            this->chainedFixups().forEachFixupChainStartLocation(segments, ^(const void* chainStart, uint32_t segIndex, const ChainedFixups::PointerFormat& pf, bool& stop) {
                pf.forEachFixupLocationInChain(chainStart, prefLoadAddr, &segments[segIndex], callback);
            });
        }
        else if ( this->header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
            // firmware binary with __TEXT,__chain_starts section
            // Note: for historical reasons firmware __chain_starts section use file-offsets from the start of __TEXT
            // but that can be changed with -fixup_chains_section_vm linker option. But which option is used is not
            // encoded in the binary, so we need a heuristic here.
            bool startOffsetsAreFileOffsets = true;
            if ( (fwStartsCount > 0) && (segments.back().fileOffset + segments.back().runtimeSize < fwStarts[fwStartsCount-1]) )
                startOffsetsAreFileOffsets = false;

            const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(fwPointerFormat);
            for (uint32_t i=0; i < fwStartsCount; ++i) {
                const void*    chainStart = nullptr;
                if ( startOffsetsAreFileOffsets ) {
                    chainStart = ((uint8_t*)this->header()) + segments[0].fileOffset + fwStarts[i];
                }
                else {
                    for (const MappedSegment& seg : segments) {
                        uint32_t startOffset = fwStarts[i];
                        if ( (seg.runtimeOffset <= startOffset) && (startOffset < seg.runtimeOffset+seg.runtimeSize) ) {
                            uint64_t vmOffsetInSegment = startOffset - seg.runtimeOffset;
                            chainStart = ((uint8_t*)this->header()) + seg.fileOffset + vmOffsetInSegment;
                        }
                    }
                }
                // Note: firmware chains can cross segments, so we cannot pre-compute 'seg'
                pf.forEachFixupLocationInChain(chainStart, prefLoadAddr, nullptr, ^(const Fixup& fixup, bool& stop) {
                    Fixup     fixupWithSeg = fixup;
                    uint64_t  chainOffset  = (uint8_t*)fixup.location - ((uint8_t*)this->header());
                    for (size_t s=0; s < segments.size(); ++s) {
                        if ( (segments[s].fileOffset <= chainOffset) && (chainOffset < segments[s].fileOffset+segments[s].runtimeSize) ) {
                            fixupWithSeg.segment = &segments[s];
                            break;
                        }
                    }
                    callback(fixupWithSeg, stop);
                });
            }
        }
        else if ( this->header()->hasFirmwareRebaseRuns() ) {
            // firmware binary with __TEXT,__rebase_info section
            bool is64 = this->header()->is64();
            this->header()->forEachFirmwareRebaseRuns(^(uint32_t address, bool& stop) {
                // Note: __rebase_info addresses are vmaddrs
                const MappedSegment* seg       = nullptr;
                uint64_t             segOffset = 0;
                for (size_t s=0; s < segments.size(); ++s) {
                    uint64_t segStartAddresss = prefLoadAddr+segments[s].runtimeOffset;
                    if ( (segStartAddresss <= address) && (address < segStartAddresss+segments[s].runtimeSize) ) {
                        seg       = &segments[s];
                        segOffset = address - segStartAddresss;
                        break;
                    }
                }
                if ( seg != nullptr ) {
                    const void* loc          = (uint8_t*)seg->content + segOffset;
                    uint64_t    targetVmAddr = is64 ? (*(uint64_t*)loc) : (*(uint32_t*)loc);
                    Fixup fixup(loc, seg, targetVmAddr-prefLoadAddr);
                    callback(fixup, stop);
                }
            });
        }
        else if ( this->header()->hasOpcodeFixups() ) {
            // userland binary with LC_DYLD_INFO
            uint32_t bindOrdinal = 0;
            if ( this->hasBindOpcodes() ) {
                bindOrdinal = this->bindOpcodes().forEachBindLocation(segments, bindOrdinal, callback);
            }
            if ( this->hasLazyBindOpcodes() ) {
                this->lazyBindOpcodes().forEachBindLocation(segments, bindOrdinal, callback);
            }
            if ( this->hasRebaseOpcodes() ) {
                this->rebaseOpcodes().forEachRebaseLocation(segments, prefLoadAddr, callback);
            }
        }
        else {
            // unsupported format
        }
    });
}

std::span<const uint32_t> Image::indirectSymbolTable() const
{
    uint32_t fileOffset;
    uint32_t count;
    if ( header()->hasIndirectSymbolTable(fileOffset, count) ) {
        return std::span<const uint32_t>((uint32_t*)(_linkeditBias + fileOffset), count);
    }
    return std::span<const uint32_t>();
}

std::span<uint8_t> Image::atomInfo() const
{
    uint32_t fileOffset;
    uint32_t count;
    if ( header()->hasAtomInfo(fileOffset, count) ) {
        return std::span<uint8_t>((uint8_t*)(_linkeditBias + fileOffset), count);
    }
    return std::span<uint8_t>();
}

static void forEachPointerInSection(const Header* hdr, uint8_t sectionType, uint64_t prefLoadAddress, void (^callback)(uint32_t offset))
{
    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.flags & SECTION_TYPE) == sectionType ) {
            const uint8_t* sectionContent = (uint8_t*)hdr + sectInfo.fileOffset;
            if ( hdr->inDyldCache() )
                sectionContent = (uint8_t*)(sectInfo.address + hdr->getSlide());
#if BUILDING_DYLD
            // in dyld, when this is called, the image is already rebased, so we can use pointers in section
            const uintptr_t* initsStart = (uintptr_t*)sectionContent;
            const uintptr_t* initsEnd   = (uintptr_t*)((uint8_t*)sectionContent + sectInfo.size);
            for (const uintptr_t* p=initsStart; p < initsEnd; ++p) {
                uintptr_t anInit       = *p;
                uint32_t  anInitOffset = (uint32_t)(anInit - prefLoadAddress);
                callback(anInitOffset);
            }
#else
            if ( hdr->is64() ) {
                const uint64_t* initsStart = (uint64_t*)sectionContent;
                const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)sectionContent + sectInfo.size);
                for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                    uint64_t anInit = *p;
                    // FIXME: as a quick hack, the low 32-bits with either rebase opcodes or chained fixups is offset in image
                    callback((uint32_t)anInit);
                }
            }
            else {
                const uint32_t* initsStart = (uint32_t*)sectionContent;
                const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)sectionContent + sectInfo.size);
                for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                    uint32_t anInitOffset = *p;
                    // FIXME: as a quick hack, the low 26-bits with either rebase opcodes or chained fixups is offset in image
                    callback(anInitOffset & 0x03FFFFFF);
                }
            }
#endif
        }
    });
}

void Image::forEachInitializer(void (^callback)(uint32_t offset)) const
{
    const Header*   hdr             = header();
    uint64_t        prefLoadAddress = hdr->preferredLoadAddress();

    // if dylib linked with -init linker option, that initializer is first
    hdr->forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ROUTINES ) {
            const routines_command* routines = (routines_command*)cmd;
            uint64_t dashInit = routines->init_address;
            callback((uint32_t)(dashInit - prefLoadAddress));
        }
        else if ( cmd->cmd == LC_ROUTINES_64 ) {
            const routines_command_64* routines = (routines_command_64*)cmd;
            uint64_t dashInit = routines->init_address;
            callback((uint32_t)(dashInit - prefLoadAddress));
        }
    });

    // next any function pointers in __DATA,__mod_init_func section
    forEachPointerInSection(hdr, S_MOD_INIT_FUNC_POINTERS, prefLoadAddress, callback);

    // next any function pointers in __TEXT,__init_offsets
    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.flags & SECTION_TYPE) == S_INIT_FUNC_OFFSETS ) {
            const uint8_t* sectionContent = (uint8_t*)hdr + sectInfo.fileOffset;
            if ( hdr->inDyldCache() )
                sectionContent = (uint8_t*)(sectInfo.address + hdr->getSlide());
            const uint32_t* initsStart = (uint32_t*)sectionContent;
            const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)sectionContent + sectInfo.size);
            for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                uint32_t anInitOffset = *p;
                callback(anInitOffset);
            }
        }
    });
}

void Image::forEachClassicTerminator(void (^callback)(uint32_t offset)) const
{
    uint64_t prefLoadAddress = header()->preferredLoadAddress();

    // any function pointers in __DATA,__mod_term_func section
    forEachPointerInSection(header(), S_MOD_TERM_FUNC_POINTERS, prefLoadAddress, callback);
}



} // namespace mach_o
