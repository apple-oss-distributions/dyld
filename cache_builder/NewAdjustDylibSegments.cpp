/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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

#include "NewAdjustDylibSegments.h"
#include "ASLRTracker.h"

#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <assert.h>

#include <fstream>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "Diagnostics.h"
#include "Trie.hpp"
#include "MachOFileAbstraction.hpp"
#include "MachOLoaded.h"
#include "mach-o/fixup-chains.h"

#ifndef EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
    #define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE 0x02
#endif

#define SUPPORT_KERNEL 0

using mach_o::MachOFileRef;

namespace {

typedef std::unordered_map<MovedLinkedit::Kind, MovedLinkedit> MovedLinkeditMap;

template <typename P>
class Adjustor
{
public:
    Adjustor(Diagnostics& diag,
             uint64_t cacheBaseAddress, dyld3::MachOFile* mh, const char* dylibID,
             const std::vector<MovedSegment>& mappingInfo,
             const MovedLinkeditMap&          linkeditInfo,
             const NListInfo&                 nlistInfo,
             const uint8_t* chainedFixupsStart, const uint8_t* chainedFixupsEnd,
             const uint8_t* splitSegInfoStart, const uint8_t* splitSegInfoEnd,
             const uint8_t* rebaseOpcodesStart, const uint8_t* rebaseOpcodesEnd);
    void adjustImageForNewSegmentLocations(const DylibSectionCoalescer* sectionCoalescer);

private:
    void     adjustReferencesUsingInfoV2(const DylibSectionCoalescer* sectionCoalescer);
    void     adjustReference(uint32_t kind, uint8_t* mappedAddr, uint64_t fromNewAddress, uint64_t toNewAddress,
                             int64_t adjust, int64_t targetSlide,
                             uint64_t imageStartAddress, uint64_t imageEndAddress,
                             cache_builder::ASLR_Tracker* aslrTracker,
                             uint32_t*& lastMappedAddr32, uint32_t& lastKind, uint64_t& lastToNewAddress);
    void     adjustDataPointers();
    void     adjustRebaseChains();
    void     slidePointer(int segIndex, uint64_t segOffset, uint8_t type);
    void     adjustSymbolTable();
    void     adjustChainedFixups();
    void     adjustExternalRelocations();
    void     adjustExportsTrie(std::vector<uint8_t>& newTrieBytes);
    void     rebuildLinkEdit();
    void     adjustCode();
    void     adjustInstruction(uint8_t kind, uint8_t* textLoc, uint64_t codeToDataDelta);
    void     rebuildLinkEditAndLoadCommands(const DylibSectionCoalescer* sectionCoalescer);
    uint64_t slideForOrigAddress(uint64_t addr);
    void     convertGeneric64RebaseToIntermediate(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* chainPtr,
                                                  cache_builder::ASLR_Tracker* aslrTracker,
                                                  uint64_t targetVMaddr);
    void     convertArm64eRebaseToIntermediate(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* chainPtr,
                                               cache_builder::ASLR_Tracker* aslrTracker,
                                               uint64_t targetSlide);

    void     adjustLinkeditLoadCommand(MovedLinkedit::Kind kind, uint32_t& dataoff, uint32_t& datasize);
    uint8_t* getLinkedDataBase(MovedLinkedit::Kind kind);

    typedef typename P::uint_t pint_t;
    typedef typename P::E      E;

    uint64_t                               _cacheBaseAddress = 0;
    MachOFileRef                           _mh;
    Diagnostics&                           _diagnostics;
    bool                                   _maskPointers        = false;
    bool                                   _splitSegInfoV2      = false;
    const char*                            _dylibID             = nullptr;
    symtab_command*                        _symTabCmd           = nullptr;
    dysymtab_command*                      _dynSymTabCmd        = nullptr;
    dyld_info_command*                     _dyldInfo            = nullptr;
    linkedit_data_command*                 _exportTrieCmd       = nullptr;
    uint16_t                               _chainedFixupsFormat = 0;
    const uint8_t*                         _chainedFixupsStart  = nullptr;
    const uint8_t*                         _chainedFixupsEnd    = nullptr;
    const uint8_t*                         _splitSegInfoStart   = nullptr;
    const uint8_t*                         _splitSegInfoEnd     = nullptr;
    const uint8_t*                         _rebaseOpcodesStart  = nullptr;
    const uint8_t*                         _rebaseOpcodesEnd    = nullptr;
    std::vector<uint64_t>                  _segOrigStartAddresses;
    std::vector<uint64_t>                  _segSizes;
    std::vector<uint64_t>                  _segSlides;
    std::vector<macho_segment_command<P>*> _segCmds;
    const std::vector<MovedSegment>&       _mappingInfo;
    const MovedLinkeditMap&                _linkeditInfo;
    const NListInfo&                       _nlistInfo;
};

template <typename P>
Adjustor<P>::Adjustor(Diagnostics& diag,
                      uint64_t cacheBaseAddress, dyld3::MachOFile* mh, const char* dylibID,
                      const std::vector<MovedSegment>& mappingInfo,
                      const MovedLinkeditMap&          linkeditInfo,
                      const NListInfo&                 nlistInfo,
                      const uint8_t* chainedFixupsStart, const uint8_t* chainedFixupsEnd,
                      const uint8_t* splitSegInfoStart, const uint8_t* splitSegInfoEnd,
                      const uint8_t* rebaseOpcodesStart, const uint8_t* rebaseOpcodesEnd)
    : _cacheBaseAddress(cacheBaseAddress)
    , _mh(mh)
    , _diagnostics(diag)
    , _dylibID(dylibID)
    , _chainedFixupsStart(chainedFixupsStart)
    , _chainedFixupsEnd(chainedFixupsEnd)
    , _splitSegInfoStart(splitSegInfoStart)
    , _splitSegInfoEnd(splitSegInfoEnd)
    , _rebaseOpcodesStart(rebaseOpcodesStart)
    , _rebaseOpcodesEnd(rebaseOpcodesEnd)
    , _mappingInfo(mappingInfo)
    , _linkeditInfo(linkeditInfo)
    , _nlistInfo(nlistInfo)
{
    assert((_mh->magic == MH_MAGIC) || (_mh->magic == MH_MAGIC_64));

    __block unsigned segIndex = 0;
    mh->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_SYMTAB: {
                _symTabCmd = (symtab_command*)cmd;

                // Adjust the offsets immediately to point to the new linkedInfo for this data
                uint32_t nlistByteSize = 0;
                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::symbolNList, _symTabCmd->symoff, nlistByteSize);
                assert((nlistByteSize % sizeof(macho_nlist<P>)) == 0);
                _symTabCmd->nsyms = nlistByteSize / sizeof(macho_nlist<P>);

                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::symbolStrings, _symTabCmd->stroff, _symTabCmd->strsize);
                break;
            }
            case LC_DYSYMTAB: {
                _dynSymTabCmd = (dysymtab_command*)cmd;

                // The nlist was optimized.  Reset the ranges to the new optimized locations
                _dynSymTabCmd->iextdefsym = this->_nlistInfo.globalsStartIndex;
                _dynSymTabCmd->nextdefsym = this->_nlistInfo.globalsCount;
                _dynSymTabCmd->ilocalsym = this->_nlistInfo.localsStartIndex;
                _dynSymTabCmd->nlocalsym = this->_nlistInfo.localsCount;
                _dynSymTabCmd->iundefsym = this->_nlistInfo.undefsStartIndex;
                _dynSymTabCmd->nundefsym = this->_nlistInfo.undefsCount;

                assert(_dynSymTabCmd->tocoff == 0);
                assert(_dynSymTabCmd->ntoc == 0);
                assert(_dynSymTabCmd->modtaboff == 0);
                assert(_dynSymTabCmd->nmodtab == 0);
                assert(_dynSymTabCmd->extrefsymoff == 0);
                assert(_dynSymTabCmd->nextrefsyms == 0);

                if ( _dynSymTabCmd->indirectsymoff != 0 ) {
                    assert(_dynSymTabCmd->nindirectsyms != 0);

                    uint32_t indirectSymsByteSize = 0;
                    this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::indirectSymbols, _dynSymTabCmd->indirectsymoff, indirectSymsByteSize);
                    assert((indirectSymsByteSize % sizeof(uint32_t)) == 0);
                    _dynSymTabCmd->nindirectsyms = indirectSymsByteSize / sizeof(uint32_t);
                }
                else {
                    assert(_dynSymTabCmd->nindirectsyms == 0);
                }

                assert(_dynSymTabCmd->extreloff == 0);
                assert(_dynSymTabCmd->nextrel == 0);
                assert(_dynSymTabCmd->locreloff == 0);
                assert(_dynSymTabCmd->nlocrel == 0);

                break;
            }
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                // Most things should be chained fixups, but some old dylibs exist for back deployment
                _dyldInfo = (dyld_info_command*)cmd;

                if ( _dyldInfo->rebase_size != 0 )
                    assert(_rebaseOpcodesStart != nullptr);

                // Zero out all the other fields.  We don't need them any more
                _dyldInfo->rebase_off     = 0;
                _dyldInfo->rebase_size    = 0;
                _dyldInfo->bind_off       = 0;
                _dyldInfo->bind_size      = 0;
                _dyldInfo->lazy_bind_off  = 0;
                _dyldInfo->lazy_bind_size = 0;
                _dyldInfo->weak_bind_off  = 0;
                _dyldInfo->weak_bind_size = 0;

                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::exportTrie, _dyldInfo->export_off, _dyldInfo->export_size);
                break;
            case LC_SEGMENT_SPLIT_INFO:
                // We drop split seg from the cache.  But we should have it available if it was in the original binary
                assert(_splitSegInfoStart != nullptr);
                break;
            case LC_FUNCTION_STARTS: {
                linkedit_data_command* functionStartsCmd = (linkedit_data_command*)cmd;
                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::functionStarts, functionStartsCmd->dataoff, functionStartsCmd->datasize);
                break;
            }
            case LC_DATA_IN_CODE: {
                linkedit_data_command* dataInCodeCmd = (linkedit_data_command*)cmd;
                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::dataInCode, dataInCodeCmd->dataoff, dataInCodeCmd->datasize);
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS:
                // We drop chained fixups from the cache
                assert(_chainedFixupsStart != nullptr);
                _chainedFixupsFormat = dyld3::MachOFile::chainedPointerFormat((dyld_chained_fixups_header*)_chainedFixupsStart);
                break;
            case LC_DYLD_EXPORTS_TRIE:
                _exportTrieCmd = (linkedit_data_command*)cmd;
                this->adjustLinkeditLoadCommand(MovedLinkedit::Kind::exportTrie, _exportTrieCmd->dataoff, _exportTrieCmd->datasize);
                break;
            case macho_segment_command<P>::CMD:
                macho_segment_command<P>* segCmd = (macho_segment_command<P>*)cmd;
                _segCmds.push_back(segCmd);
                _segOrigStartAddresses.push_back(segCmd->vmaddr());
                _segSizes.push_back(segCmd->vmsize());
                _segSlides.push_back(_mappingInfo[segIndex].cacheVMAddress.rawValue() - segCmd->vmaddr());
                ++segIndex;
                break;
        }
    });

    _maskPointers = (mh->cputype == CPU_TYPE_ARM64) || (mh->cputype == CPU_TYPE_ARM64_32);
    if ( _splitSegInfoStart != nullptr ) {
        _splitSegInfoV2 = (*_splitSegInfoStart == DYLD_CACHE_ADJ_V2_FORMAT);
    }
    else {
        bool canHaveMissingSplitSeg = false;
#if BUILDING_APP_CACHE_UTIL
        if ( mh->isKextBundle() ) {
            if ( mh->isArch("x86_64") || mh->isArch("x86_64h") )
                canHaveMissingSplitSeg = true;
        }
#endif
        if ( !canHaveMissingSplitSeg )
            _diagnostics.error("missing LC_SEGMENT_SPLIT_INFO in %s", _dylibID);
    }

    // Set the chained pointer format on old arm64e binaries using threaded rebase, and
    // which don't have LC_DYLD_CHAINED_FIXUPS
    if ( (_chainedFixupsFormat == 0) && mh->isArch("arm64e") ) {
        _chainedFixupsFormat = DYLD_CHAINED_PTR_ARM64E;
    }
}

template <typename P>
void Adjustor<P>::adjustImageForNewSegmentLocations(const DylibSectionCoalescer* sectionCoalescer)
{
    if ( _diagnostics.hasError() )
        return;
    if ( _splitSegInfoV2 ) {
        adjustReferencesUsingInfoV2(sectionCoalescer);
        adjustChainedFixups();
    }
    else if ( _chainedFixupsStart != nullptr ) {
        // need to adjust the chain fixup segment_offset fields in LINKEDIT before chains can be walked
        adjustChainedFixups();
        adjustRebaseChains();
        adjustCode();
    }
    else {
        adjustDataPointers();
        adjustCode();
    }
    if ( _diagnostics.hasError() )
        return;
    adjustSymbolTable();
    if ( _diagnostics.hasError() )
        return;

    adjustExternalRelocations();
    if ( _diagnostics.hasError() )
        return;
    rebuildLinkEditAndLoadCommands(sectionCoalescer);

#if DEBUG
    __block Diagnostics  diag;
    _mh->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        layout.isValidLinkeditLayout(diag, _dylibID);
        if ( diag.hasError() ) {
            fprintf(stderr, "%s\n", diag.errorMessage().c_str());
            return;
        }
    });

    _mh->validSegments(diag, _dylibID, 0xffffffff);
    if ( diag.hasError() ) {
        fprintf(stderr, "%s\n", diag.errorMessage().c_str());
        return;
    }
#endif
}

template <typename P>
uint64_t Adjustor<P>::slideForOrigAddress(uint64_t addr)
{
    for ( unsigned i = 0; i < _segOrigStartAddresses.size(); ++i ) {
        if ( (_segOrigStartAddresses[i] <= addr) && (addr < (_segOrigStartAddresses[i] + _segCmds[i]->vmsize())) )
            return _segSlides[i];
    }
    // On arm64, high nibble of pointers can have extra bits
    if ( _maskPointers && (addr & 0xF000000000000000) ) {
        return slideForOrigAddress(addr & 0x0FFFFFFFFFFFFFFF);
    }
    _diagnostics.error("slide not known for dylib address 0x%llX in %s", addr, _dylibID);
    return 0;
}

template <typename P>
void Adjustor<P>::rebuildLinkEditAndLoadCommands(const DylibSectionCoalescer* sectionCoalescer)
{
    // Exports trie is only data structure in LINKEDIT that might grow
    std::vector<uint8_t> newTrieBytes;
    adjustExportsTrie(newTrieBytes);

    // updates load commands and removed ones no longer needed

    __block unsigned segIndex = 0;
    _mh->forEachLoadCommand(_diagnostics, ^(const load_command* cmd, bool& stop) {
        macho_segment_command<P>*  segCmd;
        macho_routines_command<P>* routinesCmd;
        dylib_command*             dylibIDCmd;
        int32_t                    segFileOffsetDelta;
        switch ( cmd->cmd ) {
            case LC_ID_DYLIB:
                dylibIDCmd                  = (dylib_command*)cmd;
                dylibIDCmd->dylib.timestamp = 2; // match what static linker sets in LC_LOAD_DYLIB
                break;
            case LC_DYSYMTAB: {
                dysymtab_command* dynSymTabCmd = (dysymtab_command*)cmd;

                assert(dynSymTabCmd->tocoff == 0);
                assert(dynSymTabCmd->ntoc == 0);
                assert(dynSymTabCmd->modtaboff == 0);
                assert(dynSymTabCmd->nmodtab == 0);
                assert(dynSymTabCmd->extrefsymoff == 0);
                assert(dynSymTabCmd->nextrefsyms == 0);

                if ( dynSymTabCmd->indirectsymoff != 0 ) {
                    // indirectsymoff was adjusted earlier
                    assert(dynSymTabCmd->nindirectsyms != 0);
                }
                else {
                    assert(dynSymTabCmd->nindirectsyms == 0);
                }

                // The kernel linker needs external relocations to resolve binds.
                // We'll need to keep a copy of them, or perhaps just use the ones from the source kext, along with adjusting them on the fly
                assert(dynSymTabCmd->extreloff == 0);
                assert(dynSymTabCmd->nextrel == 0);
                assert(dynSymTabCmd->locreloff == 0);
                assert(dynSymTabCmd->nlocrel == 0);

#if SUPPORT_KERNEL
                // Needed for the kernel linker
                dynSymTabCmd->indirectsymoff = linkeditStartOffset + indirectTableOffset;
                // Clear local relocations (ie, old style rebases) as they were tracked earlier when we applied split seg
                dynSymTabCmd->locreloff = 0;
                dynSymTabCmd->nlocrel = 0 ;
                // Update external relocations as we need these later to resolve binds from kexts
                dynSymTabCmd->extreloff = linkeditStartOffset + externalRelocOffset;
#endif
                break;
            }
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                dyld_info_command* dyldInfo = (dyld_info_command*)cmd;

                // Rebases/binds were zeroed earlier, but we need to handle exports
                uint8_t* start = getLinkedDataBase(MovedLinkedit::Kind::exportTrie);

                // zero the old export trie buffer
                bzero(start, dyldInfo->export_size);

                if ( newTrieBytes.size() == 0 ) {
                    dyldInfo->export_size = 0;
                    dyldInfo->export_off  = 0;
                }
                else {
                    // Write the new data
                    assert(newTrieBytes.size() <= dyldInfo->export_size);
                    // The dataoff field was set earlier.  Just change the size if we got smaller
                    dyldInfo->export_size = (uint32_t)newTrieBytes.size();
                    memcpy(start, newTrieBytes.data(), newTrieBytes.size());
                }
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                linkedit_data_command* exportTrieCmd = (linkedit_data_command*)cmd;
                uint8_t*               start         = getLinkedDataBase(MovedLinkedit::Kind::exportTrie);

                // zero the old export trie buffer
                bzero(start, exportTrieCmd->datasize);

                if ( newTrieBytes.size() == 0 ) {
                    exportTrieCmd->dataoff  = 0;
                    exportTrieCmd->datasize = 0;
                }
                else {
                    // Write the new data
                    assert(newTrieBytes.size() <= exportTrieCmd->datasize);
                    // The dataoff field was set earlier.  Just change the size if we got smaller
                    exportTrieCmd->datasize = (uint32_t)newTrieBytes.size();
                    memcpy(start, newTrieBytes.data(), newTrieBytes.size());
                }
                break;
            }
            case macho_routines_command<P>::CMD:
                routinesCmd = (macho_routines_command<P>*)cmd;
                routinesCmd->set_init_address(routinesCmd->init_address() + slideForOrigAddress(routinesCmd->init_address()));
                break;
            case macho_segment_command<P>::CMD:
                segCmd             = (macho_segment_command<P>*)cmd;
                segFileOffsetDelta = (int32_t)(_mappingInfo[segIndex].cacheFileOffset.rawValue() - segCmd->fileoff());
                segCmd->set_vmaddr(_mappingInfo[segIndex].cacheVMAddress.rawValue());
                segCmd->set_vmsize(_mappingInfo[segIndex].cacheVMSize.rawValue());
                segCmd->set_fileoff(_mappingInfo[segIndex].cacheFileOffset.rawValue());
                segCmd->set_filesize(_mappingInfo[segIndex].cacheFileSize.rawValue());
                if ( segCmd->nsects() > 0 ) {
                    macho_section<P>* const sectionsStart = (macho_section<P>*)((uint8_t*)segCmd + sizeof(macho_segment_command<P>));
                    macho_section<P>* const sectionsEnd   = &sectionsStart[segCmd->nsects()];

                    for ( macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect ) {
                        bool coalescedSection = false;
#if 0
                        if ( textCoalescer.sectionWasCoalesced(sect->segname(), sect->sectname())) {
                            coalescedSection = true;
                        }
#endif

                        bool optimizedSection = false;
                        if ( (sectionCoalescer != nullptr) && sectionCoalescer->sectionWasOptimized(sect->segname(), sect->sectname())) {
                            optimizedSection = true;
                        }

#if BUILDING_APP_CACHE_UTIL
                        if ( strcmp(segCmd->segname(), "__CTF") == 0 ) {
                            // The kernel __CTF segment data is completely removed when we link the baseKC
                            if ( _mh->isStaticExecutable() )
                                coalescedSection = true;
                        }
#endif

                        if ( coalescedSection ) {
                            // Put coalesced sections at the end of the segment
                            sect->set_addr(segCmd->vmaddr() + segCmd->filesize());
                            sect->set_offset(0);
                            sect->set_size(0);
                        }
                        else {
                            sect->set_addr(sect->addr() + _segSlides[segIndex]);
                            if ( sect->offset() != 0 )
                                sect->set_offset(sect->offset() + segFileOffsetDelta);

                            // If the section was optimized but not removed, then its GOTs.  In that
                            // case, remove the flag which tells anyone to analyze this segment
                            if ( optimizedSection ) {
                                if ( (sect->flags() & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS ) {
                                    uint32_t flags = sect->flags();
                                    flags = flags & ~SECTION_TYPE;
                                    flags = flags | S_REGULAR;
                                    sect->set_flags(flags);
                                }
                            }
                        }

                    }
                }
                ++segIndex;
                break;
            case LC_UNIXTHREAD: {
                // adjust entry point of /usr/lib/dyld copied into the dyld cache
                uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
                uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);
                uint32_t index = _mh->entryAddrRegisterIndexForThreadCmd();
                if ( _mh->use64BitEntryRegs() )
                    regs64[index] += _mappingInfo[0].cacheVMAddress.rawValue();
                else
                    regs32[index] += _mappingInfo[0].cacheVMAddress.rawValue();
                }
                break;
            default:
                break;
        }
    });

    _mh->removeLoadCommand(_diagnostics, ^(const load_command* cmd, bool& remove, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_RPATH:
                _diagnostics.warning("dyld shared cache does not support LC_RPATH found in %s", _dylibID);
                remove = true;
                break;
            case LC_CODE_SIGNATURE:
            case LC_DYLIB_CODE_SIGN_DRS:
            case LC_DYLD_CHAINED_FIXUPS:
            case LC_SEGMENT_SPLIT_INFO:
                remove = true;
                break;
            default:
                break;
        }
    });

    _mh->flags |= 0x80000000;
}

template <typename P>
void Adjustor<P>::adjustSymbolTable()
{
    if ( _dynSymTabCmd == nullptr )
        return;

    macho_nlist<P>* symbolTable = (macho_nlist<P>*)getLinkedDataBase(MovedLinkedit::Kind::symbolNList);

    // adjust global symbol table entries
    macho_nlist<P>* lastExport = &symbolTable[_dynSymTabCmd->iextdefsym + _dynSymTabCmd->nextdefsym];
    for ( macho_nlist<P>* entry = &symbolTable[_dynSymTabCmd->iextdefsym]; entry < lastExport; ++entry ) {
        if ( (entry->n_type() & N_TYPE) == N_SECT )
            entry->set_n_value(entry->n_value() + slideForOrigAddress(entry->n_value()));
    }

    // adjust local symbol table entries
    macho_nlist<P>* lastLocal = &symbolTable[_dynSymTabCmd->ilocalsym + _dynSymTabCmd->nlocalsym];
    for ( macho_nlist<P>* entry = &symbolTable[_dynSymTabCmd->ilocalsym]; entry < lastLocal; ++entry ) {
        if ( (entry->n_sect() != NO_SECT) && ((entry->n_type() & N_STAB) == 0) )
            entry->set_n_value(entry->n_value() + slideForOrigAddress(entry->n_value()));
    }
}

template <typename P>
void Adjustor<P>::adjustChainedFixups()
{
#if BUILDING_APP_CACHE_UTIL
    if ( _chainedFixupsStart == nullptr )
        return;

    // Pass a start hint in to withChainStarts which takes account of the LINKEDIT shifting but we haven't
    // yet updated that LC_SEGMENT to point to the new data
    const dyld_chained_fixups_header* header = (dyld_chained_fixups_header*)_chainedFixupsStart;

    // segment_offset in dyld_chained_starts_in_segment is wrong.  We need to move it to the new segment offset
    dyld3::MachOFile::withChainStarts(_diagnostics, header, ^(const dyld_chained_starts_in_image* starts) {
        for ( uint32_t segIndex = 0; segIndex < starts->seg_count; ++segIndex ) {
            if ( starts->seg_info_offset[segIndex] == 0 )
                continue;
            dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
            segInfo->segment_offset                 = _mappingInfo[segIndex].cacheVMAddress.rawValue() - _mappingInfo[0].cacheVMAddress.rawValue();
        }
    });
#endif
}

template <typename P>
static uint64_t externalRelocBaseAddress(const MachOFileRef&                    mf,
                                         std::vector<macho_segment_command<P>*> segCmds,
                                         std::vector<uint64_t>                  segOrigStartAddresses)
{
    if ( mf->isArch("x86_64") || mf->isArch("x86_64h") ) {
#if BUILDING_APP_CACHE_UTIL
        if ( mf->isKextBundle() ) {
            // for kext bundles the reloc base address starts at __TEXT segment
            return segOrigStartAddresses[0];
        }
#endif
        // for x86_64 reloc base address starts at first writable segment (usually __DATA)
        for ( uint32_t i = 0; i < segCmds.size(); ++i ) {
            if ( segCmds[i]->initprot() & VM_PROT_WRITE )
                return segOrigStartAddresses[i];
        }
    }
    // For everyone else we start at 0
    return 0;
}

template <typename P>
void Adjustor<P>::adjustExternalRelocations()
{
    if ( _dynSymTabCmd == nullptr )
        return;

    if ( _dynSymTabCmd->nextrel == 0 )
        return;

    assert(0);

#if SUPPORT_KERNEL

    // section index 0 refers to mach_header
    uint64_t baseAddress = _mappingInfo[0].cacheVMAddress.rawValue();

    const uint64_t                  relocsStartAddress = externalRelocBaseAddress(_mh, _segCmds, _segOrigStartAddresses);
    relocation_info*                relocsStart = (relocation_info*)&_linkeditBias[_dynSymTabCmd->extreloff];
    relocation_info*                relocsEnd   = &relocsStart[_dynSymTabCmd->nextrel];
    for (relocation_info* reloc = relocsStart; reloc < relocsEnd; ++reloc) {
        // External relocations should be relative to the base address of the mach-o as otherwise they
        // probably won't fit in 32-bits.
        uint64_t newAddress = reloc->r_address + slideForOrigAddress(relocsStartAddress + reloc->r_address);
        newAddress -= baseAddress;
        reloc->r_address = (int32_t)newAddress;
        assert((uint64_t)reloc->r_address == newAddress);
    }
#endif
}

template <typename P>
void Adjustor<P>::slidePointer(int segIndex, uint64_t segOffset, uint8_t type)
{
    cache_builder::ASLR_Tracker* aslrTracker = this->_mappingInfo[segIndex].aslrTracker;
    pint_t*   mappedAddrP  = (pint_t*)((uint8_t*)_mappingInfo[segIndex].cacheLocation + segOffset);
    uint32_t* mappedAddr32 = (uint32_t*)mappedAddrP;
    pint_t    valueP;
    uint32_t  value32;
    switch ( type ) {
        case REBASE_TYPE_POINTER:
            valueP = (pint_t)P::getP(*mappedAddrP);
            P::setP(*mappedAddrP, valueP + slideForOrigAddress(valueP));
            aslrTracker->add(mappedAddrP);
            break;

        case REBASE_TYPE_TEXT_ABSOLUTE32:
            value32 = P::E::get32(*mappedAddr32);
            P::E::set32(*mappedAddr32, value32 + (uint32_t)slideForOrigAddress(value32));
            break;

        case REBASE_TYPE_TEXT_PCREL32:
            // general text relocs not support
        default:
            _diagnostics.error("unknown rebase type 0x%02X in %s", type, _dylibID);
    }
}

static bool isThumbMovw(uint32_t instruction)
{
    return ((instruction & 0x8000FBF0) == 0x0000F240);
}

static bool isThumbMovt(uint32_t instruction)
{
    return ((instruction & 0x8000FBF0) == 0x0000F2C0);
}

static uint16_t getThumbWord(uint32_t instruction)
{
    uint32_t i    = ((instruction & 0x00000400) >> 10);
    uint32_t imm4 = (instruction & 0x0000000F);
    uint32_t imm3 = ((instruction & 0x70000000) >> 28);
    uint32_t imm8 = ((instruction & 0x00FF0000) >> 16);
    return ((imm4 << 12) | (i << 11) | (imm3 << 8) | imm8);
}

static uint32_t setThumbWord(uint32_t instruction, uint16_t word)
{
    uint32_t imm4 = (word & 0xF000) >> 12;
    uint32_t i    = (word & 0x0800) >> 11;
    uint32_t imm3 = (word & 0x0700) >> 8;
    uint32_t imm8 = word & 0x00FF;
    return (instruction & 0x8F00FBF0) | imm4 | (i << 10) | (imm3 << 28) | (imm8 << 16);
}

static bool isArmMovw(uint32_t instruction)
{
    return (instruction & 0x0FF00000) == 0x03000000;
}

static bool isArmMovt(uint32_t instruction)
{
    return (instruction & 0x0FF00000) == 0x03400000;
}

static uint16_t getArmWord(uint32_t instruction)
{
    uint32_t imm4  = ((instruction & 0x000F0000) >> 16);
    uint32_t imm12 = (instruction & 0x00000FFF);
    return (imm4 << 12) | imm12;
}

static uint32_t setArmWord(uint32_t instruction, uint16_t word)
{
    uint32_t imm4  = (word & 0xF000) >> 12;
    uint32_t imm12 = word & 0x0FFF;
    return (instruction & 0xFFF0F000) | (imm4 << 16) | imm12;
}


template <typename P>
void Adjustor<P>::convertArm64eRebaseToIntermediate(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* chainPtr,
                                                    cache_builder::ASLR_Tracker* aslrTracker,
                                                    uint64_t targetSlide)
{
    assert(chainPtr->arm64e.authRebase.bind == 0);
    assert( (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E)
           || (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND)
           || (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND24)
           || (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_KERNEL) );
    dyld3::MachOLoaded::ChainedFixupPointerOnDisk orgPtr = *chainPtr;
    dyld3::MachOLoaded::ChainedFixupPointerOnDisk tmp;
    if ( chainPtr->arm64e.authRebase.auth ) {
        uint64_t targetVMAddr = orgPtr.arm64e.authRebase.target + _segOrigStartAddresses[0] + targetSlide;

#if BUILDING_APP_CACHE_UTIL
        // Note authRebase has no high8, so this is invalid if it occurs
        uint8_t high8 = targetVMAddr >> 56;
        if ( high8 ) {
            // The kernel uses the high bits in the vmAddr, so don't error there
            bool badPointer = true;
            if ( _chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_KERNEL ) {
                uint64_t vmOffset = targetVMAddr - _cacheBaseAddress;
                if ( (vmOffset >> 56) == 0 )
                    badPointer = false;
            }

            if ( badPointer ) {
                _diagnostics.error("Cannot set tag on pointer in '%s' as high bits are incompatible with pointer authentication", _dylibID);
                return;
            }
        }
#endif

        if ( (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND)
            || (_chainedFixupsFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND24) ) {
            // <rdar://60351693> the rebase target is a vmoffset, so we need to switch to tracking the target out of line
#if BUILDING_APP_CACHE_UTIL
            // The kernel linker stores all the data out of band
            aslrTracker->setAuthData(chainPtr, chainPtr->arm64e.authRebase.diversity,
                                     chainPtr->arm64e.authRebase.addrDiv, chainPtr->arm64e.authRebase.key);
            aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
            chainPtr->arm64e.rebase.target = 0; // actual target vmAddr stored in side table
            chainPtr->arm64e.rebase.high8  = 0;
            chainPtr->arm64e.rebase.next   = orgPtr.arm64e.rebase.next;
            chainPtr->arm64e.rebase.bind   = 0;
            chainPtr->arm64e.rebase.auth   = 0;
#else
            // The shared cache builder only stores the target out of band, but keeps the rest where it is
            chainPtr->arm64e.authRebase.target = 0; // actual target vmAddr stored in side table
            aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
#endif
            return;
        }

        // we need to change the rebase to point to the new address in the dyld cache, but it may not fit
        tmp.arm64e.authRebase.target = targetVMAddr;
        if ( tmp.arm64e.authRebase.target == targetVMAddr ) {
            // everything fits, just update target
            chainPtr->arm64e.authRebase.target = targetVMAddr;
            return;
        }

        // target cannot fit into rebase chain, so store target in side table
#if BUILDING_APP_CACHE_UTIL
        // The kernel linker stores all the data out of band
        aslrTracker->setAuthData(chainPtr, chainPtr->arm64e.authRebase.diversity,
                                 chainPtr->arm64e.authRebase.addrDiv, chainPtr->arm64e.authRebase.key);
        aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
        chainPtr->arm64e.rebase.target = 0; // actual target vmAddr stored in side table
        chainPtr->arm64e.rebase.high8  = 0;
        chainPtr->arm64e.rebase.next   = orgPtr.arm64e.rebase.next;
        chainPtr->arm64e.rebase.bind   = 0;
        chainPtr->arm64e.rebase.auth   = 0;
#else
        // The shared cache builder only stores the target out of band, but keeps the rest where it is
        chainPtr->arm64e.authRebase.target = 0; // actual target vmAddr stored in side table
        aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
#endif
        return;
    }
    else {
        uint64_t targetVMAddr = 0;
        switch (_chainedFixupsFormat) {
            case DYLD_CHAINED_PTR_ARM64E:
                targetVMAddr = orgPtr.arm64e.rebase.target + targetSlide;
                break;
            case DYLD_CHAINED_PTR_ARM64E_USERLAND:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND24: {
                // <rdar://60351693> the rebase target is a vmoffset, so we need to switch to tracking the target out of line
                uint64_t targetVmAddrInDylib = orgPtr.arm64e.rebase.target + _segOrigStartAddresses[0];
                uint64_t rebaseTargetVmAddrInDyldcache = targetVmAddrInDylib + targetSlide;
                aslrTracker->setRebaseTarget64(chainPtr, rebaseTargetVmAddrInDyldcache);
                orgPtr.arm64e.rebase.target = 0;
                targetVMAddr = 0;
                }
                break;
            case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                targetVMAddr = orgPtr.arm64e.rebase.target + _segOrigStartAddresses[0] + targetSlide;
                break;
            default:
                _diagnostics.error("Unknown chain format");
                return;
        }

#if BUILDING_APP_CACHE_UTIL
        // The merging code may have set the high bits, eg, to a tagged pointer
        uint8_t high8 = targetVMAddr >> 56;
        if ( chainPtr->arm64e.rebase.high8 ) {
            if ( high8 ) {
                _diagnostics.error("Cannot set tag on pointer as high bits are in use");
                return;
            }
            aslrTracker->setHigh8(chainPtr, chainPtr->arm64e.rebase.high8);
        } else {
            if ( high8 ) {
                aslrTracker->setHigh8(chainPtr, high8);
                targetVMAddr &= 0x00FFFFFFFFFFFFFF;
            }
        }
#endif

        tmp.arm64e.rebase.target = targetVMAddr;
        if ( tmp.arm64e.rebase.target == targetVMAddr ) {
            // target dyld cache address fits in plain rebase, so all we need to do is adjust that
            chainPtr->arm64e.rebase.target = targetVMAddr;
            return;
        }

        // target cannot fit into rebase chain, so store target in side table
        aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
        chainPtr->arm64e.rebase.target = 0; // actual target vmAddr stored in side table
    }
}


template <typename P>
void Adjustor<P>::convertGeneric64RebaseToIntermediate(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* chainPtr,
                                                       cache_builder::ASLR_Tracker* aslrTracker,
                                                       uint64_t targetSlide)
{
    assert( (_chainedFixupsFormat == DYLD_CHAINED_PTR_64) || (_chainedFixupsFormat == DYLD_CHAINED_PTR_64_OFFSET) );
    dyld3::MachOLoaded::ChainedFixupPointerOnDisk tmp;

    uint64_t targetVMAddr = 0;
    switch (_chainedFixupsFormat) {
        case DYLD_CHAINED_PTR_64: {
            targetVMAddr = chainPtr->generic64.rebase.target + targetSlide;
            break;
        }
        case DYLD_CHAINED_PTR_64_OFFSET: {
            // <rdar://60351693> the rebase target is a vmoffset, so we need to switch to tracking the target out of line
            targetVMAddr = chainPtr->generic64.rebase.target + _segOrigStartAddresses[0] + targetSlide;
            aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
            chainPtr->generic64.rebase.target = 0;
            return;
            break;
        }
        default:
            _diagnostics.error("Unknown chain format");
            return;
    }

    // we need to change the rebase to point to the new address in the dyld cache, but it may not fit
    tmp.generic64.rebase.target = targetVMAddr;
    if ( tmp.generic64.rebase.target == targetVMAddr ) {
        // everything fits, just update target
        chainPtr->generic64.rebase.target = targetVMAddr;
        return;
    }

    // target cannot fit into rebase chain, so store target in side table
    aslrTracker->setRebaseTarget64(chainPtr, targetVMAddr);
     chainPtr->generic64.rebase.target = 0; // actual target vmAddr stored in side table
}

template <typename P>
void Adjustor<P>::adjustReference(uint32_t kind, uint8_t* mappedAddr, uint64_t fromNewAddress, uint64_t toNewAddress,
                                  int64_t adjust, int64_t targetSlide, uint64_t imageStartAddress, uint64_t imageEndAddress,
                                  cache_builder::ASLR_Tracker* aslrTracker,
                                  uint32_t*& lastMappedAddr32, uint32_t& lastKind, uint64_t& lastToNewAddress)
{
    uint64_t value64;
    uint64_t* mappedAddr64 = 0;
    uint32_t value32;
    uint32_t* mappedAddr32 = 0;
    uint32_t instruction;
    dyld3::MachOLoaded::ChainedFixupPointerOnDisk* chainPtr;
    uint32_t newPageOffset;
    int64_t delta;
    switch ( kind ) {
        case DYLD_CACHE_ADJ_V2_DELTA_32:
            mappedAddr32 = (uint32_t*)mappedAddr;
            value32 = P::E::get32(*mappedAddr32);
            delta = (int32_t)value32;
            delta += adjust;
            if ( (delta > 0x80000000) || (-delta > 0x80000000) ) {
                _diagnostics.error("DYLD_CACHE_ADJ_V2_DELTA_32 can't be adjust by 0x%016llX in %s", adjust, _dylibID);
                return;
            }
            P::E::set32(*mappedAddr32, (int32_t)delta);
            break;
        case DYLD_CACHE_ADJ_V2_POINTER_32:
            mappedAddr32 = (uint32_t*)mappedAddr;
            if ( _chainedFixupsStart != nullptr ) {
                chainPtr = (dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)mappedAddr32;
                switch (_chainedFixupsFormat) {
                    case DYLD_CHAINED_PTR_32:
                        // ignore binds, fix up rebases to have new targets
                        if ( chainPtr->generic32.rebase.bind == 0 ) {
                            // there is not enough space in 32-bit pointer to store new vmaddr in cache in 26-bit target
                            // so store target in side table that will be applied when binds are resolved
                            aslrTracker->add(mappedAddr32);
                            uint32_t target = (uint32_t)(chainPtr->generic32.rebase.target + targetSlide);
                            aslrTracker->setRebaseTarget32(chainPtr, target);
                            chainPtr->generic32.rebase.target = 0; // actual target stored in side table
                        }
                        break;
                    default:
                        _diagnostics.error("unknown 32-bit chained fixup format %d in %s", _chainedFixupsFormat, _dylibID);
                        break;
                }
            }
#if BUILDING_APP_CACHE_UTIL
            else if ( _mh->usesClassicRelocationsInKernelCollection() ) {
                // Classic relocs are not guaranteed to be aligned, so always store them in the side table
                if ( (uint32_t)toNewAddress != (uint32_t)(E::get32(*mappedAddr32) + targetSlide) ) {
                    _diagnostics.error("bad DYLD_CACHE_ADJ_V2_POINTER_32 value not as expected at address 0x%llX in %s", fromNewAddress, _dylibID);
                    return;
                }
                aslrTracker->setRebaseTarget32(mappedAddr32, (uint32_t)toNewAddress);
                E::set32(*mappedAddr32, 0);
                aslrTracker->add(mappedAddr32);
            }
#endif
            else {
                if ( toNewAddress != (uint64_t)(E::get32(*mappedAddr32) + targetSlide) ) {
                    _diagnostics.error("bad DYLD_CACHE_ADJ_V2_POINTER_32 value not as expected at address 0x%llX in %s", fromNewAddress, _dylibID);
                    return;
                }
                E::set32(*mappedAddr32, (uint32_t)toNewAddress);
                aslrTracker->add(mappedAddr32);
            }
            break;
        case DYLD_CACHE_ADJ_V2_POINTER_64:
            mappedAddr64 = (uint64_t*)mappedAddr;
            if ( _chainedFixupsStart != nullptr ) {
                chainPtr = (dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)mappedAddr64;
                switch (_chainedFixupsFormat) {
                    case DYLD_CHAINED_PTR_ARM64E:
                    case DYLD_CHAINED_PTR_ARM64E_USERLAND:
                    case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                    case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                        // ignore binds and adjust rebases to new segment locations
                        if ( chainPtr->arm64e.authRebase.bind == 0 ) {
                            convertArm64eRebaseToIntermediate(chainPtr, aslrTracker, targetSlide);
                            // Note, the pointer remains a chain with just the target of the rebase adjusted to the new target location
                            aslrTracker->add(chainPtr);
                        }
                        break;
                    case DYLD_CHAINED_PTR_64:
                    case DYLD_CHAINED_PTR_64_OFFSET:
                        // ignore binds and adjust rebases to new segment locations
                        if ( chainPtr->generic64.rebase.bind == 0 ) {
                            convertGeneric64RebaseToIntermediate(chainPtr, aslrTracker, targetSlide);
                            // Note, the pointer remains a chain with just the target of the rebase adjusted to the new target location
                            aslrTracker->add(chainPtr);
                        }
                        break;
                    default:
                        _diagnostics.error("unknown 64-bit chained fixup format %d in %s", _chainedFixupsFormat, _dylibID);
                        break;
                }
            }
#if BUILDING_APP_CACHE_UTIL
            else if ( _mh->usesClassicRelocationsInKernelCollection() ) {
                if ( toNewAddress != (E::get64(*mappedAddr64) + targetSlide) ) {
                    _diagnostics.error("bad DYLD_CACHE_ADJ_V2_POINTER_64 value not as expected at address 0x%llX in %s", fromNewAddress, _dylibID);
                    return;
                }
                aslrTracker->setRebaseTarget64(mappedAddr64, toNewAddress);
                E::set64(*mappedAddr64, 0); // actual target vmAddr stored in side table
                aslrTracker->add(mappedAddr64);
                uint8_t high8 = toNewAddress >> 56;
                if ( high8 )
                    aslrTracker->setHigh8(mappedAddr64, high8);
            }
#endif
            else {
                if ( toNewAddress != (E::get64(*mappedAddr64) + targetSlide) ) {
                    _diagnostics.error("bad DYLD_CACHE_ADJ_V2_POINTER_64 value not as expected at address 0x%llX in %s", fromNewAddress, _dylibID);
                    return;
                }
                E::set64(*mappedAddr64, toNewAddress);
                aslrTracker->add(mappedAddr64);
#if BUILDING_APP_CACHE_UTIL
                uint8_t high8 = toNewAddress >> 56;
                if ( high8 )
                    aslrTracker->setHigh8(mappedAddr64, high8);
#endif
            }
            break;
        case DYLD_CACHE_ADJ_V2_THREADED_POINTER_64:
            // old style arm64e binary
            chainPtr = (dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)mappedAddr;
            // ignore binds, they are proccessed later
            if ( chainPtr->arm64e.authRebase.bind == 0 ) {
                convertArm64eRebaseToIntermediate(chainPtr, aslrTracker, targetSlide);
                // Note, the pointer remains a chain with just the target of the rebase adjusted to the new target location
                aslrTracker->add(chainPtr);
            }
            break;
       case DYLD_CACHE_ADJ_V2_DELTA_64:
            mappedAddr64 = (uint64_t*)mappedAddr;
            value64 = P::E::get64(*mappedAddr64);
            E::set64(*mappedAddr64, value64 + adjust);
            break;
        case DYLD_CACHE_ADJ_V2_IMAGE_OFF_32:
            if ( adjust == 0 )
                break;
            mappedAddr32 = (uint32_t*)mappedAddr;
            value32 = P::E::get32(*mappedAddr32);
            value64 = toNewAddress - imageStartAddress;
            if ( value64 > imageEndAddress ) {
                _diagnostics.error("DYLD_CACHE_ADJ_V2_IMAGE_OFF_32 can't be adjust to 0x%016llX in %s", toNewAddress, _dylibID);
                return;
            }
            P::E::set32(*mappedAddr32, (uint32_t)value64);
            break;
        case DYLD_CACHE_ADJ_V2_ARM64_ADRP:
            mappedAddr32 = (uint32_t*)mappedAddr;
            instruction = P::E::get32(*mappedAddr32);
            if ( (instruction & 0x9F000000) == 0x90000000 ) {
                int64_t pageDistance = ((toNewAddress & ~0xFFF) - (fromNewAddress & ~0xFFF));
                int64_t newPage21 = pageDistance >> 12;
                if ( (newPage21 > 2097151) || (newPage21 < -2097151) ) {
                    _diagnostics.error("DYLD_CACHE_ADJ_V2_ARM64_ADRP can't be adjusted that far in %s", _dylibID);
                    return;
                }
                instruction = (instruction & 0x9F00001F) | ((newPage21 << 29) & 0x60000000) | ((newPage21 << 3) & 0x00FFFFE0);
                P::E::set32(*mappedAddr32, instruction);
            }
            else {
                // ADRP instructions are sometimes optimized to other instructions (e.g. ADR) after the split-seg-info is generated
            }
            break;
        case DYLD_CACHE_ADJ_V2_ARM64_OFF12:
            mappedAddr32 = (uint32_t*)mappedAddr;
            instruction = P::E::get32(*mappedAddr32);
            // This is a page offset, so if we pack both the __TEXT page with the add/ldr, and
            // the destination page with the target data, then the adjust isn't correct.  Instead
            // we always want the page offset of the target, ignoring where the source add/ldr slid
            newPageOffset = (uint32_t)(toNewAddress & 0xFFF);
            if ( (instruction & 0x3B000000) == 0x39000000 ) {
                // LDR/STR imm12
                uint32_t encodedAddend = ((instruction & 0x003FFC00) >> 10);
                uint32_t newAddend = 0;
                switch ( instruction & 0xC0000000 ) {
                    case 0x00000000:
                        if ( (instruction & 0x04800000) == 0x04800000 ) {
                            if ( newPageOffset & 0xF ) {
                                _diagnostics.error("can't adjust off12 scale=16 instruction to %d bytes at mapped address=%p in %s", newPageOffset, mappedAddr, _dylibID);
                                return;
                            }
                            if ( encodedAddend*16 >= 4096 ) {
                                _diagnostics.error("off12 scale=16 instruction points outside its page at mapped address=%p in %s", mappedAddr, _dylibID);
                            }
                            newAddend = (newPageOffset/16);
                        }
                        else {
                            // scale=1
                            newAddend = newPageOffset;
                        }
                        break;
                    case 0x40000000:
                        if ( newPageOffset & 1 ) {
                            _diagnostics.error("can't adjust off12 scale=2 instruction to %d bytes at mapped address=%p in %s", newPageOffset, mappedAddr, _dylibID);
                            return;
                        }
                        if ( encodedAddend*2 >= 4096 ) {
                            _diagnostics.error("off12 scale=2 instruction points outside its page at mapped address=%p in %s", mappedAddr, _dylibID);
                            return;
                        }
                        newAddend = (newPageOffset/2);
                        break;
                    case 0x80000000:
                        if ( newPageOffset & 3 ) {
                            _diagnostics.error("can't adjust off12 scale=4 instruction to %d bytes at mapped address=%p in %s", newPageOffset, mappedAddr, _dylibID);
                            return;
                        }
                        if ( encodedAddend*4 >= 4096 ) {
                            _diagnostics.error("off12 scale=4 instruction points outside its page at mapped address=%p in %s", mappedAddr, _dylibID);
                            return;
                        }
                        newAddend = (newPageOffset/4);
                        break;
                    case 0xC0000000:
                        if ( newPageOffset & 7 ) {
                            _diagnostics.error("can't adjust off12 scale=8 instruction to %d bytes at mapped address=%p in %s", newPageOffset, mappedAddr, _dylibID);
                            return;
                        }
                        if ( encodedAddend*8 >= 4096 ) {
                            _diagnostics.error("off12 scale=8 instruction points outside its page at mapped address=%p in %s", mappedAddr, _dylibID);
                            return;
                        }
                        newAddend = (newPageOffset/8);
                        break;
                }
                uint32_t newInstruction = (instruction & 0xFFC003FF) | (newAddend << 10);
                P::E::set32(*mappedAddr32, newInstruction);
            }
            else if ( (instruction & 0xFFC00000) == 0x91000000 ) {
                // ADD imm12
                if ( instruction & 0x00C00000 ) {
                    _diagnostics.error("ADD off12 uses shift at mapped address=%p in %s", mappedAddr, _dylibID);
                    return;
                }
                uint32_t newAddend = newPageOffset;
                uint32_t newInstruction = (instruction & 0xFFC003FF) | (newAddend << 10);
                P::E::set32(*mappedAddr32, newInstruction);
            }
            else if ( instruction != 0xD503201F ) {
                // ignore imm12 instructions optimized into a NOP, but warn about others
                _diagnostics.error("unknown off12 instruction 0x%08X at 0x%0llX in %s", instruction, fromNewAddress, _dylibID);
                return;
            }
            break;
        case DYLD_CACHE_ADJ_V2_THUMB_MOVW_MOVT:
            mappedAddr32 = (uint32_t*)mappedAddr;
            // to update a movw/movt pair we need to extract the 32-bit they will make,
            // add the adjust and write back the new movw/movt pair.
            if ( lastKind == kind ) {
                if ( lastToNewAddress == toNewAddress ) {
                    uint32_t instruction1 = P::E::get32(*lastMappedAddr32);
                    uint32_t instruction2 = P::E::get32(*mappedAddr32);
                    if ( isThumbMovw(instruction1) && isThumbMovt(instruction2) ) {
                        uint16_t high = getThumbWord(instruction2);
                        uint16_t low  = getThumbWord(instruction1);
                        uint32_t full = high << 16 | low;
                        full += adjust;
                        instruction1 = setThumbWord(instruction1, full & 0xFFFF);
                        instruction2 = setThumbWord(instruction2, full >> 16);
                    }
                    else if ( isThumbMovt(instruction1) && isThumbMovw(instruction2) ) {
                        uint16_t high = getThumbWord(instruction1);
                        uint16_t low  = getThumbWord(instruction2);
                        uint32_t full = high << 16 | low;
                        full += adjust;
                        instruction2 = setThumbWord(instruction2, full & 0xFFFF);
                        instruction1 = setThumbWord(instruction1, full >> 16);
                    }
                    else {
                        _diagnostics.error("two DYLD_CACHE_ADJ_V2_THUMB_MOVW_MOVT in a row but not paried in %s", _dylibID);
                        return;
                    }
                    P::E::set32(*lastMappedAddr32, instruction1);
                    P::E::set32(*mappedAddr32, instruction2);
                    kind = 0;
                }
                else {
                    _diagnostics.error("two DYLD_CACHE_ADJ_V2_THUMB_MOVW_MOVT in a row but target different addresses in %s", _dylibID);
                    return;
                }
            }
            break;
        case DYLD_CACHE_ADJ_V2_ARM_MOVW_MOVT:
            mappedAddr32 = (uint32_t*)mappedAddr;
            // to update a movw/movt pair we need to extract the 32-bit they will make,
            // add the adjust and write back the new movw/movt pair.
            if ( lastKind == kind ) {
                if ( lastToNewAddress == toNewAddress ) {
                    uint32_t instruction1 = P::E::get32(*lastMappedAddr32);
                    uint32_t instruction2 = P::E::get32(*mappedAddr32);
                    if ( isArmMovw(instruction1) && isArmMovt(instruction2) ) {
                        uint16_t high = getArmWord(instruction2);
                        uint16_t low  = getArmWord(instruction1);
                        uint32_t full = high << 16 | low;
                        full += adjust;
                        instruction1 = setArmWord(instruction1, full & 0xFFFF);
                        instruction2 = setArmWord(instruction2, full >> 16);
                    }
                    else if ( isArmMovt(instruction1) && isArmMovw(instruction2) ) {
                        uint16_t high = getArmWord(instruction1);
                        uint16_t low  = getArmWord(instruction2);
                        uint32_t full = high << 16 | low;
                        full += adjust;
                        instruction2 = setArmWord(instruction2, full & 0xFFFF);
                        instruction1 = setArmWord(instruction1, full >> 16);
                    }
                    else {
                        _diagnostics.error("two DYLD_CACHE_ADJ_V2_ARM_MOVW_MOVT in a row but not paired in %s", _dylibID);
                        return;
                    }
                    P::E::set32(*lastMappedAddr32, instruction1);
                    P::E::set32(*mappedAddr32, instruction2);
                    kind = 0;
                }
                else {
                    _diagnostics.error("two DYLD_CACHE_ADJ_V2_ARM_MOVW_MOVT in a row but target different addresses in %s", _dylibID);
                    return;
                }
            }
            break;
        case DYLD_CACHE_ADJ_V2_ARM64_BR26: {
            if ( adjust == 0 )
                break;
            mappedAddr32 = (uint32_t*)mappedAddr;
            instruction = P::E::get32(*mappedAddr32);

            int64_t deltaToFinalTarget = toNewAddress - fromNewAddress;
            // Make sure the target is in range
            static const int64_t b128MegLimit = 0x07FFFFFF;
            if ( (deltaToFinalTarget > -b128MegLimit) && (deltaToFinalTarget < b128MegLimit) ) {
                instruction = (instruction & 0xFC000000) | ((deltaToFinalTarget >> 2) & 0x03FFFFFF);
                P::E::set32(*mappedAddr32, instruction);
                break;
            } else {
                _diagnostics.error("br26 instruction exceeds maximum range at mapped address=%p in %s", mappedAddr, _dylibID);
                return;
            }
        }
        case DYLD_CACHE_ADJ_V2_THUMB_BR22:
        case DYLD_CACHE_ADJ_V2_ARM_BR24:
            // nothing to do with calls to stubs
            break;
        default:
            _diagnostics.error("unknown split seg kind=%d in %s", kind, _dylibID);
            return;
    }
    lastKind = kind;
    lastToNewAddress = toNewAddress;
    lastMappedAddr32 = mappedAddr32;
}

template <typename P>
void Adjustor<P>::adjustReferencesUsingInfoV2(const DylibSectionCoalescer* sectionCoalescer)
{
    static const bool logDefault = false;
    bool              log        = logDefault;

    const uint8_t* infoStart = _splitSegInfoStart;
    const uint8_t* infoEnd   = _splitSegInfoEnd;
    if ( *infoStart++ != DYLD_CACHE_ADJ_V2_FORMAT ) {
        _diagnostics.error("malformed split seg info in %s", _dylibID);
        return;
    }
    // build section arrays of slide and mapped address for each section
    std::vector<uint64_t> sectionSlides;
    std::vector<uint64_t> sectionNewAddress;
    std::vector<uint8_t*> sectionMappedAddress;
    std::vector<std::string_view>   sectionNames;

    // Also track coalesced sections, if we have any
    typedef DylibSectionCoalescer::OptimizedSection OptimizedSection;
    std::vector<uint64_t>                           coalescedSectionOriginalVMAddrs;
    std::vector<const OptimizedSection*>            coalescedSectionData;
    std::vector<cache_builder::ASLR_Tracker*>       aslrTrackers;

    sectionSlides.reserve(16);
    sectionNewAddress.reserve(16);
    sectionMappedAddress.reserve(16);
    coalescedSectionOriginalVMAddrs.reserve(16);
    coalescedSectionData.reserve(16);
    aslrTrackers.reserve(16);

    // section index 0 refers to mach_header
    sectionMappedAddress.push_back((uint8_t*)_mappingInfo[0].cacheLocation);
    sectionSlides.push_back(_segSlides[0]);
    sectionNewAddress.push_back(_mappingInfo[0].cacheVMAddress.rawValue());
    sectionNames.push_back("mach_header");
    coalescedSectionOriginalVMAddrs.push_back(0);
    coalescedSectionData.push_back(nullptr);
    aslrTrackers.push_back(nullptr);

    uint64_t imageStartAddress = sectionNewAddress.front();
    uint64_t imageEndAddress   = 0;

    // section 1 and later refer to real sections
    unsigned sectionIndex = 0;
    for ( unsigned segmentIndex = 0; segmentIndex < _segCmds.size(); ++segmentIndex ) {
        macho_segment_command<P>* segCmd        = _segCmds[segmentIndex];
        macho_section<P>* const   sectionsStart = (macho_section<P>*)((char*)segCmd + sizeof(macho_segment_command<P>));
        macho_section<P>* const   sectionsEnd   = &sectionsStart[segCmd->nsects()];

        for ( macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect ) {
            sectionMappedAddress.push_back((uint8_t*)_mappingInfo[segmentIndex].cacheLocation + sect->addr() - segCmd->vmaddr());
            sectionSlides.push_back(_segSlides[segmentIndex]);
            sectionNewAddress.push_back(_mappingInfo[segmentIndex].cacheVMAddress.rawValue() + sect->addr() - segCmd->vmaddr());
            sectionNames.push_back(std::string_view(sect->sectname(), strnlen(sect->sectname(), 16)));
            coalescedSectionOriginalVMAddrs.push_back(sect->addr());
            aslrTrackers.push_back(this->_mappingInfo[segmentIndex].aslrTracker);

            if ( (sectionCoalescer != nullptr) && sectionCoalescer->sectionWasOptimized(sect->segname(), sect->sectname())) {
                // Optimized/removed sections need to track the section itself
                const OptimizedSection* optimizedSection = sectionCoalescer->getSection(sect->segname(), sect->sectname());
                coalescedSectionData.push_back(optimizedSection);
            } else {
                coalescedSectionData.push_back(nullptr);
            }

            if ( (sectionCoalescer == nullptr) || !sectionCoalescer->sectionWasRemoved(sect->segname(), sect->sectname()) )
                imageEndAddress = std::max(imageEndAddress, sectionNewAddress.back());

            ++sectionIndex;
            if ( log ) {
                fprintf(stderr, " %s/%s, sectIndex=%d, mapped at=%p\n",
                        sect->segname(), sect->sectname(), sectionIndex, sectionMappedAddress.back());
            }
        }
    }

    // Whole         :== <count> FromToSection+
    // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
    // ToOffset         :== <to-sect-offset-delta> <count> FromOffset+
    // FromOffset     :== <kind> <count> <from-sect-offset-delta>
    const uint8_t* p            = infoStart;
    uint64_t       sectionCount = read_uleb128(p, infoEnd);
    for ( uint64_t i = 0; i < sectionCount; ++i ) {
        uint32_t* lastMappedAddr32         = NULL;
        uint32_t  lastKind                 = 0;
        uint64_t  lastToNewAddress         = 0;
        uint64_t  fromSectionIndex         = read_uleb128(p, infoEnd);
        uint64_t  toSectionIndex           = read_uleb128(p, infoEnd);
        uint64_t  toOffsetCount            = read_uleb128(p, infoEnd);
        uint64_t  fromSectionSlide         = sectionSlides[fromSectionIndex];
        uint64_t  fromSectionNewAddress    = sectionNewAddress[fromSectionIndex];
        uint8_t*  fromSectionMappedAddress = sectionMappedAddress[fromSectionIndex];
        uint64_t  toSectionSlide           = sectionSlides[toSectionIndex];
        uint64_t  toSectionNewAddress      = sectionNewAddress[toSectionIndex];
        if ( log )
            printf(" from sect=%lld (mapped=%p), to sect=%lld (new addr=0x%llX):\n", fromSectionIndex, fromSectionMappedAddress, toSectionIndex, toSectionNewAddress);
        uint64_t toSectionOffset = 0;

        for ( uint64_t j = 0; j < toOffsetCount; ++j ) {
            uint64_t toSectionDelta  = read_uleb128(p, infoEnd);
            uint64_t fromOffsetCount = read_uleb128(p, infoEnd);
            toSectionOffset += toSectionDelta;
            for ( uint64_t k = 0; k < fromOffsetCount; ++k ) {
                uint64_t kind = read_uleb128(p, infoEnd);
                if ( kind > 13 ) {
                    _diagnostics.error("unknown split seg info v2 kind value (%llu) in %s", kind, _dylibID);
                    return;
                }
                uint64_t fromSectDeltaCount = read_uleb128(p, infoEnd);
                uint64_t fromSectionOffset  = 0;
                for ( uint64_t l = 0; l < fromSectDeltaCount; ++l ) {
                    uint64_t delta = read_uleb128(p, infoEnd);
                    fromSectionOffset += delta;
                    if ( log )
                        printf("   kind=%lld, from offset=0x%0llX, to offset=0x%0llX, adjust=0x%llX, targetSlide=0x%llX\n", kind, fromSectionOffset, toSectionOffset, delta, toSectionSlide);

                    // It's possible for all either of from/to sectiobs to be coalesced/optimized.
                    // Handle each of those combinations.
                    uint8_t* fromMappedAddr = nullptr;
                    uint64_t fromNewAddress = 0;
                    uint64_t fromAtomSlide  = 0;
                    if ( (coalescedSectionData[fromSectionIndex] != nullptr) && coalescedSectionData[fromSectionIndex]->sectionWillBeRemoved ) {
                        // From was coalesced and removed
                        // Note we don't do coalesced GOTs here as those are not removed.  Those will be handled with the regular logic as
                        // their section still exists

                        // We don't handle this case right now.  It would be something like CFStrings
                        assert(0);
                    } else {
                        // From was not optimized/coalesced
                        fromMappedAddr = fromSectionMappedAddress + fromSectionOffset;
                        fromNewAddress = fromSectionNewAddress + fromSectionOffset;
                        fromAtomSlide = fromSectionSlide;
                    }

                    uint64_t toNewAddress   = 0;
                    uint64_t toAtomSlide    = 0;
                    if ( coalescedSectionData[toSectionIndex] != nullptr ) {
                        // To was optimized/coalesced
                        const auto* offsetMap = &coalescedSectionData[toSectionIndex]->offsetMap;
                        auto offsetIt = offsetMap->find((uint32_t)toSectionOffset);
                        if ( coalescedSectionData[toSectionIndex]->sectionWillBeRemoved ) {
                            // If the section was removed then we have to find an entry for every atom in there
                            assert(offsetIt != offsetMap->end());
                        } else {
                            // Not all GOTs are optimized, but we should find the element somewhere
                            assert((offsetIt != offsetMap->end()) || coalescedSectionData[toSectionIndex]->unoptimizedOffsets.count((uint32_t)toSectionOffset));
                        }

                        if ( offsetIt == offsetMap->end() ) {
                            // To was not fully optimized/coalesced
                            // FIXME: Unify this with the else branch below where we didn't have a coalesced section
                            toNewAddress = toSectionNewAddress + toSectionOffset;
                            toAtomSlide = toSectionSlide;
                        } else {
                            uint64_t baseVMAddr = coalescedSectionData[toSectionIndex]->subCacheSection->cacheChunk->cacheVMAddress.rawValue();
                            toNewAddress = baseVMAddr + offsetIt->second;

                            // The 'to' section is gone, but we still need the 'to' slide.  Instead of a section slide,
                            // compute the slide for this individual atom
                            uint64_t toAtomOriginalVMAddr = coalescedSectionOriginalVMAddrs[toSectionIndex] + toSectionOffset;
                            toAtomSlide = toNewAddress - toAtomOriginalVMAddr;
                        }
                    } else {
                        // To was not optimized/coalesced
                        toNewAddress = toSectionNewAddress + toSectionOffset;
                        toAtomSlide = toSectionSlide;
                    }

                    int64_t deltaAdjust = toAtomSlide - fromAtomSlide;
                    if ( log ) {
                        printf("   kind=%lld, from offset=0x%0llX, to offset=0x%0llX, adjust=0x%llX, targetSlide=0x%llX\n",
                               kind, fromSectionOffset, toSectionOffset, deltaAdjust, toSectionSlide);
                    }
                    adjustReference((uint32_t)kind, fromMappedAddr, fromNewAddress, toNewAddress, deltaAdjust,
                                    toAtomSlide, imageStartAddress, imageEndAddress,
                                    aslrTrackers[fromSectionIndex],
                                    lastMappedAddr32, lastKind, lastToNewAddress);
                    if ( _diagnostics.hasError() )
                        return;
                }
            }
        }
    }
}

template <typename P>
void Adjustor<P>::adjustRebaseChains()
{
    const dyld_chained_fixups_header* chainHeader = (dyld_chained_fixups_header*)_chainedFixupsStart;
    dyld3::MachOFile::withChainStarts(_diagnostics, chainHeader, ^(const dyld_chained_starts_in_image* starts) {
        dyld3::MachOFile::forEachFixupChainSegment(_diagnostics, starts, ^(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stopSegment) {
            uint8_t* segmentBuffer = this->_mappingInfo[segIndex].cacheLocation;
            cache_builder::ASLR_Tracker* aslrTracker = this->_mappingInfo[segIndex].aslrTracker;

            auto handler = ^(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* fixupLoc, bool& stop) {
                switch ( segInfo->pointer_format ) {
                    case DYLD_CHAINED_PTR_64:
                        // only look at rebases
                        if ( fixupLoc->generic64.rebase.bind == 0 ) {
                            uint64_t targetVmAddrInDylib = fixupLoc->generic64.rebase.target;
                            convertGeneric64RebaseToIntermediate(fixupLoc, aslrTracker, slideForOrigAddress(targetVmAddrInDylib));
                            aslrTracker->add(fixupLoc);
                        }
                        break;
                    case DYLD_CHAINED_PTR_64_OFFSET:
                        // only look at rebases
                        if ( fixupLoc->generic64.rebase.bind == 0 ) {
                            // on input, the rebase "value" is an offset from the mach_header into an original segment
                            // we need to convert that to the vmAddr in the shared cache that maps to
                            uint64_t targetVmAddrInDylib = fixupLoc->generic64.rebase.target + _segOrigStartAddresses[0];
                            convertGeneric64RebaseToIntermediate(fixupLoc, aslrTracker, slideForOrigAddress(targetVmAddrInDylib));
                            aslrTracker->add(fixupLoc);
                        }
                        break;
                    default:
                        _diagnostics.error("unsupported chained fixup format %d", segInfo->pointer_format);
                        stop = true;
                }
            };

            dyld3::MachOFile::forEachFixupInSegmentChains(_diagnostics, segInfo, false, segmentBuffer, handler);
        });
    });
}

#if SUPPORT_KERNEL
static int uint32Sorter(const void* l, const void* r) {
    if ( *((uint32_t*)l) < *((uint32_t*)r) )
        return -1;
    else
        return 1;
}
#endif

template <typename P>
static uint64_t localRelocBaseAddress(const MachOFileRef&                    mf,
                                      std::vector<macho_segment_command<P>*> segCmds,
                                      std::vector<uint64_t>                  segOrigStartAddresses)
{
    if ( mf->isArch("x86_64") || mf->isArch("x86_64h") ) {
#if BUILDING_APP_CACHE_UTIL
        if ( ma->isKextBundle() ) {
            // for kext bundles the reloc base address starts at __TEXT segment
            return segOrigStartAddresses[0];
        }
#endif
        // for all other kinds, the x86_64 reloc base address starts at first writable segment (usually __DATA)
        for ( uint32_t i = 0; i < segCmds.size(); ++i ) {
            if ( segCmds[i]->initprot() & VM_PROT_WRITE )
                return segOrigStartAddresses[i];
        }
    }
    return segOrigStartAddresses[0];
}

#if SUPPORT_KERNEL
static bool segIndexAndOffsetForAddress(uint64_t addr, const std::vector<uint64_t>& segOrigStartAddresses,
                                        std::vector<uint64_t> segSizes, uint32_t& segIndex, uint64_t& segOffset)
{
    for (uint32_t i=0; i < segOrigStartAddresses.size(); ++i) {
        if ( (segOrigStartAddresses[i] <= addr) && (addr < (segOrigStartAddresses[i] + segSizes[i])) ) {
            segIndex  = i;
            segOffset = addr - segOrigStartAddresses[i];
            return true;
        }
    }
    return false;
}
#endif

template <typename P>
void Adjustor<P>::adjustDataPointers()
{
    if ( (_dynSymTabCmd != nullptr) && (_dynSymTabCmd->locreloff != 0) ) {
        assert(0);
#if SUPPORT_KERNEL
        // kexts may have old style relocations instead of dyldinfo rebases
        assert(_dyldInfo == nullptr);

        // old binary, walk relocations
        const uint64_t                  relocsStartAddress = localRelocBaseAddress(_mh, _segCmds, _segOrigStartAddresses);
        const relocation_info* const    relocsStart = (const relocation_info* const)&_linkeditBias[_dynSymTabCmd->locreloff];
        const relocation_info* const    relocsEnd   = &relocsStart[_dynSymTabCmd->nlocrel];
        bool                            stop = false;
        const uint8_t                   relocSize = (_mh->is64() ? 3 : 2);
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint32_t, relocAddrs, 2048);
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                _diagnostics.error("local relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
                _diagnostics.error("local relocation has wrong r_type");
                break;
            }
            relocAddrs.push_back(reloc->r_address);
        }
        if ( !relocAddrs.empty() ) {
            ::qsort(&relocAddrs[0], relocAddrs.count(), sizeof(uint32_t), &uint32Sorter);
            for (uint32_t addrOff : relocAddrs) {
                uint32_t segIndex  = 0;
                uint64_t segOffset = 0;
                if ( segIndexAndOffsetForAddress(relocsStartAddress+addrOff, _segOrigStartAddresses, _segSizes, segIndex, segOffset) ) {
                    uint8_t type = REBASE_TYPE_POINTER;
                    assert(_mh->cputype != CPU_TYPE_I386);
                    slidePointer(segIndex, segOffset, type, aslrTracker);
                }
                else {
                    _diagnostics.error("local relocation has out of range r_address");
                    break;
                }
            }
        }
        // then process indirect symbols
        // FIXME: Do we need indirect symbols?  Aren't those handled as binds?
#endif
        return;
    }

    if ( _dyldInfo == NULL )
        return;

    const uint8_t* p = this->_rebaseOpcodesStart;
    const uint8_t* end = this->_rebaseOpcodesEnd;

    uint8_t type = 0;
    int segIndex = 0;
    uint64_t segOffset = 0;
    uint64_t count;
    uint64_t skip;
    bool done = false;
    while ( !done && (p < end) ) {
        uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
        uint8_t opcode = *p & REBASE_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case REBASE_OPCODE_DONE:
                done = true;
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segIndex = immediate;
                segOffset = read_uleb128(p, end);
                break;
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                segOffset += read_uleb128(p, end);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                segOffset += immediate*sizeof(pint_t);
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for (int i=0; i < immediate; ++i) {
                    slidePointer(segIndex, segOffset, type);
                    segOffset += sizeof(pint_t);
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                count = read_uleb128(p, end);
                for (uint32_t i=0; i < count; ++i) {
                    slidePointer(segIndex, segOffset, type);
                    segOffset += sizeof(pint_t);
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                slidePointer(segIndex, segOffset, type);
                segOffset += read_uleb128(p, end) + sizeof(pint_t);
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(p, end);
                skip = read_uleb128(p, end);
                for (uint32_t i=0; i < count; ++i) {
                    slidePointer(segIndex, segOffset, type);
                    segOffset += skip + sizeof(pint_t);
                }
                break;
            default:
                _diagnostics.error("unknown rebase opcode 0x%02X in %s", opcode, _dylibID);
                done = true;
                break;
        }
    }
}

template <typename P>
void Adjustor<P>::adjustInstruction(uint8_t kind, uint8_t* textLoc, uint64_t codeToDataDelta)
{
    uint32_t* fixupLoc32 = (uint32_t*)textLoc;
    uint64_t* fixupLoc64 = (uint64_t*)textLoc;
    uint32_t  instruction;
    uint32_t  value32;
    uint64_t  value64;

    switch ( kind ) {
        case 1: // 32-bit pointer (including x86_64 RIP-rel)
            value32 = P::E::get32(*fixupLoc32);
            value32 += codeToDataDelta;
            P::E::set32(*fixupLoc32, value32);
            break;
        case 2: // 64-bit pointer
            value64 = P::E::get64(*fixupLoc64);
            value64 += codeToDataDelta;
            P::E::set64(*fixupLoc64, value64);
            break;
        case 4: // only used for i386, a reference to something in the IMPORT segment
            break;
        case 5: // used by thumb2 movw
            instruction = P::E::get32(*fixupLoc32);
            // slide is always a multiple of 4096, so only top 4 bits of lo16 will ever need adjusting
            value32     = (instruction & 0x0000000F) + ((uint32_t)codeToDataDelta >> 12);
            instruction = (instruction & 0xFFFFFFF0) | (value32 & 0x0000000F);
            P::E::set32(*fixupLoc32, instruction);
            break;
        case 6: // used by ARM movw
            instruction = P::E::get32(*fixupLoc32);
            // slide is always a multiple of 4096, so only top 4 bits of lo16 will ever need adjusting
            value32     = ((instruction & 0x000F0000) >> 16) + ((uint32_t)codeToDataDelta >> 12);
            instruction = (instruction & 0xFFF0FFFF) | ((value32 << 16) & 0x000F0000);
            P::E::set32(*fixupLoc32, instruction);
            break;
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
            // used by thumb2 movt (low nibble of kind is high 4-bits of paired movw)
            {
                instruction = P::E::get32(*fixupLoc32);
                assert((instruction & 0x8000FBF0) == 0x0000F2C0);
                // extract 16-bit value from instruction
                uint32_t i     = ((instruction & 0x00000400) >> 10);
                uint32_t imm4  = (instruction & 0x0000000F);
                uint32_t imm3  = ((instruction & 0x70000000) >> 28);
                uint32_t imm8  = ((instruction & 0x00FF0000) >> 16);
                uint32_t imm16 = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
                // combine with codeToDataDelta and kind nibble
                uint32_t targetValue    = (imm16 << 16) | ((kind & 0xF) << 12);
                uint32_t newTargetValue = targetValue + (uint32_t)codeToDataDelta;
                // construct new bits slices
                uint32_t imm4_ = (newTargetValue & 0xF0000000) >> 28;
                uint32_t i_    = (newTargetValue & 0x08000000) >> 27;
                uint32_t imm3_ = (newTargetValue & 0x07000000) >> 24;
                uint32_t imm8_ = (newTargetValue & 0x00FF0000) >> 16;
                // update instruction to match codeToDataDelta
                uint32_t newInstruction = (instruction & 0x8F00FBF0) | imm4_ | (i_ << 10) | (imm3_ << 28) | (imm8_ << 16);
                P::E::set32(*fixupLoc32, newInstruction);
            }
            break;
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x27:
        case 0x28:
        case 0x29:
        case 0x2A:
        case 0x2B:
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
            // used by arm movt (low nibble of kind is high 4-bits of paired movw)
            {
                instruction = P::E::get32(*fixupLoc32);
                // extract 16-bit value from instruction
                uint32_t imm4  = ((instruction & 0x000F0000) >> 16);
                uint32_t imm12 = (instruction & 0x00000FFF);
                uint32_t imm16 = (imm4 << 12) | imm12;
                // combine with codeToDataDelta and kind nibble
                uint32_t targetValue    = (imm16 << 16) | ((kind & 0xF) << 12);
                uint32_t newTargetValue = targetValue + (uint32_t)codeToDataDelta;
                // construct new bits slices
                uint32_t imm4_  = (newTargetValue & 0xF0000000) >> 28;
                uint32_t imm12_ = (newTargetValue & 0x0FFF0000) >> 16;
                // update instruction to match codeToDataDelta
                uint32_t newInstruction = (instruction & 0xFFF0F000) | (imm4_ << 16) | imm12_;
                P::E::set32(*fixupLoc32, newInstruction);
            }
            break;
        case 3: // used for arm64 ADRP
            instruction = P::E::get32(*fixupLoc32);
            if ( (instruction & 0x9F000000) == 0x90000000 ) {
                // codeToDataDelta is always a multiple of 4096, so only top 4 bits of lo16 will ever need adjusting
                value64 = ((instruction & 0x60000000) >> 17) | ((instruction & 0x00FFFFE0) << 9);
                value64 += codeToDataDelta;
                instruction = (instruction & 0x9F00001F) | ((value64 << 17) & 0x60000000) | ((value64 >> 9) & 0x00FFFFE0);
                P::E::set32(*fixupLoc32, instruction);
            }
            break;
        default:
            break;
    }
}

template <typename P>
void Adjustor<P>::adjustCode()
{
    // find compressed info on how code needs to be updated
    if ( _splitSegInfoStart == nullptr )
        return;

    const uint8_t* infoStart = _splitSegInfoStart;
    const uint8_t* infoEnd = _splitSegInfoEnd;

    // This encoding only works if all data segments slide by the same amount
    uint64_t codeToDataDelta = _segSlides[1] - _segSlides[0];

    // compressed data is:  [ <kind> [uleb128-delta]+ <0> ] + <0>
    for (const uint8_t* p = infoStart; (*p != 0) && (p < infoEnd);) {
        uint8_t kind = *p++;
        uint8_t* textLoc = (uint8_t*)_mappingInfo[0].cacheLocation;
        while (uint64_t delta = read_uleb128(p, infoEnd)) {
            textLoc += delta;
            adjustInstruction(kind, textLoc, codeToDataDelta);
        }
    }
}

template <typename P>
void Adjustor<P>::adjustExportsTrie(std::vector<uint8_t>& newTrieBytes)
{
    // if no export info, nothing to adjust
    uint32_t exportOffset = 0;
    uint32_t exportSize   = 0;
    if ( _dyldInfo != nullptr ) {
        exportOffset = _dyldInfo->export_off;
        exportSize   = _dyldInfo->export_size;
    }
    else if ( _exportTrieCmd != nullptr ) {
        exportOffset = _exportTrieCmd->dataoff;
        exportSize   = _exportTrieCmd->datasize;
    }

    if ( exportSize == 0 )
        return;

    // since export info addresses are offsets from mach_header, everything in __TEXT is fine
    // only __DATA addresses need to be updated
    const uint8_t*                     start = getLinkedDataBase(MovedLinkedit::Kind::exportTrie);
    const uint8_t*                     end   = &start[exportSize];
    std::vector<ExportInfoTrie::Entry> originalExports;
    if ( !ExportInfoTrie::parseTrie(start, end, originalExports) ) {
        _diagnostics.error("malformed exports trie in %s", _dylibID);
        return;
    }

    std::vector<ExportInfoTrie::Entry> newExports;
    newExports.reserve(originalExports.size());
    uint64_t baseAddress      = _segOrigStartAddresses[0];
    uint64_t baseAddressSlide = slideForOrigAddress(baseAddress);
    for ( auto& entry : originalExports ) {
        // remove symbols used by the static linker only
        if ( (strncmp(entry.name.c_str(), "$ld$", 4) == 0)
             || (strncmp(entry.name.c_str(), ".objc_class_name", 16) == 0)
             || (strncmp(entry.name.c_str(), ".objc_category_name", 19) == 0) ) {
            continue;
        }
        // adjust symbols in slid segments
        if ( (entry.info.flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) != EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE )
            entry.info.address += (slideForOrigAddress(entry.info.address + baseAddress) - baseAddressSlide);
        newExports.push_back(entry);
    }

    // rebuild export trie
    newTrieBytes.reserve(exportSize);

    ExportInfoTrie(newExports).emit(newTrieBytes);
    // align
    while ( (newTrieBytes.size() % sizeof(pint_t)) != 0 )
        newTrieBytes.push_back(0);
}

template <typename P>
void Adjustor<P>::adjustLinkeditLoadCommand(MovedLinkedit::Kind kind, uint32_t& dataoff, uint32_t& datasize)
{
    auto it = this->_linkeditInfo.find(kind);
    assert(it != this->_linkeditInfo.end());

    dataoff  = (uint32_t)it->second.dataOffset.rawValue();
    datasize = (uint32_t)it->second.dataSize.rawValue();
}

template <typename P>
uint8_t* Adjustor<P>::getLinkedDataBase(MovedLinkedit::Kind kind)
{
    auto it = this->_linkeditInfo.find(kind);
    assert(it != this->_linkeditInfo.end());

    return it->second.cacheLocation;
}

} // anonymous namespace

DylibSegmentsAdjustor::DylibSegmentsAdjustor(std::vector<MovedSegment>&& movedSegments,
                                             MovedLinkeditMap&&          movedLinkedit,
                                             NListInfo&                  nlistInfo)
    : movedSegments(std::move(movedSegments))
    , movedLinkedit(std::move(movedLinkedit))
    , nlistInfo(nlistInfo)
{
}

void DylibSegmentsAdjustor::adjustDylib(Diagnostics&      diag,
                                        CacheVMAddress    cacheBaseAddress,
                                        dyld3::MachOFile* cacheMF,
                                        std::string_view  dylibID,
                                        const uint8_t* chainedFixupsStart, const uint8_t* chainedFixupsEnd,
                                        const uint8_t* splitSegInfoStart, const uint8_t* splitSegInfoEnd,
                                        const uint8_t* rebaseOpcodesStart, const uint8_t* rebaseOpcodesEnd,
                                        const DylibSectionCoalescer* sectionCoalescer)
{
    const bool is64 = cacheMF->pointerSize() == 8;
    if ( is64 ) {
        Adjustor<Pointer64<LittleEndian>> adjustor64(diag,
                                                     cacheBaseAddress.rawValue(),
                                                     cacheMF,
                                                     dylibID.data(),
                                                     movedSegments,
                                                     movedLinkedit,
                                                     nlistInfo,
                                                     chainedFixupsStart, chainedFixupsEnd,
                                                     splitSegInfoStart, splitSegInfoEnd,
                                                     rebaseOpcodesStart, rebaseOpcodesEnd);
        adjustor64.adjustImageForNewSegmentLocations(sectionCoalescer);
    }
    else {
        Adjustor<Pointer32<LittleEndian>> adjustor32(diag,
                                                     cacheBaseAddress.rawValue(),
                                                     cacheMF,
                                                     dylibID.data(),
                                                     movedSegments,
                                                     movedLinkedit,
                                                     nlistInfo,
                                                     chainedFixupsStart, chainedFixupsEnd,
                                                     splitSegInfoStart, splitSegInfoEnd,
                                                     rebaseOpcodesStart, rebaseOpcodesEnd);
        adjustor32.adjustImageForNewSegmentLocations(sectionCoalescer);
    }
}

// FIXME: Unify with Adjustor<P>::slideForOrigAddress() above
CacheVMAddress DylibSegmentsAdjustor::adjustVMAddr(InputDylibVMAddress inputVMAddr) const
{
    for ( const MovedSegment& segment : this->movedSegments ) {
        if ( (segment.inputVMAddress <= inputVMAddr) && (inputVMAddr < (segment.inputVMAddress + segment.inputVMSize)) ) {
            VMOffset segmentVMOffset = inputVMAddr - segment.inputVMAddress;
            return segment.cacheVMAddress + segmentVMOffset;
        }
    }

    // FIXME: Don't assert
    assert(0);
    return CacheVMAddress(0ULL);
}
