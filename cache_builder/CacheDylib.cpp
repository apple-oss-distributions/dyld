/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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
#include "BuilderConfig.h"
#include "BuilderOptions.h"
#include "CacheDylib.h"
#include "MachOFile.h"
#include "MachOFileAbstraction.hpp"
#include "Header.h"
#include "ObjCVisitor.h"
#include "Optimizers.h"
#include "OptimizerObjC.h"
#include "StringUtils.h"
#include "Trie.hpp"

// mach_o
#include "Header.h"
#include "Image.h"
#include "FunctionVariants.h"

#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonDigestSPI.h>

#include <optional>
#include <vector>

// mach_o_writer
#include "HeaderWriter.h"

// FIXME: We should get this from cctools
#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

#define DYLD_CACHE_ADJ_V2_POINTER_32            0x01
#define DYLD_CACHE_ADJ_V2_POINTER_64            0x02
#define DYLD_CACHE_ADJ_V2_DELTA_32                0x03
#define DYLD_CACHE_ADJ_V2_DELTA_64                0x04
#define DYLD_CACHE_ADJ_V2_ARM64_ADRP            0x05
#define DYLD_CACHE_ADJ_V2_ARM64_OFF12            0x06
#define DYLD_CACHE_ADJ_V2_ARM64_BR26            0x07
#define DYLD_CACHE_ADJ_V2_ARM_MOVW_MOVT            0x08
#define DYLD_CACHE_ADJ_V2_ARM_BR24                0x09
#define DYLD_CACHE_ADJ_V2_THUMB_MOVW_MOVT        0x0A
#define DYLD_CACHE_ADJ_V2_THUMB_BR22            0x0B
#define DYLD_CACHE_ADJ_V2_IMAGE_OFF_32            0x0C
#define DYLD_CACHE_ADJ_V2_THREADED_POINTER_64   0x0D

using namespace cache_builder;
using dyld3::MachOFile;
using error::Error;
using mach_o::Header;
using mach_o::Version32;
using mach_o::Image;
using mach_o::FunctionVariantFixups;

//
// MARK: --- cache_builder::CacheDylib methods ---
//

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
CacheDylib::CacheDylib()
{
}
#endif

CacheDylib::CacheDylib(InputFile& inputFile)
    : inputFile(&inputFile)
    , inputMF(inputFile.mf)
    , inputHdr((const Header*)inputFile.mf)
    , inputLoadAddress(this->inputHdr->preferredLoadAddress())
    , installName(this->inputHdr->installName())
{
}

CacheDylib::CacheDylib(std::string_view installName)
    : inputFile(nullptr)
    , inputMF(nullptr)
    , inputHdr(nullptr)
    , inputLoadAddress(0ull)
    , installName(installName)
{
}

// If you want to watch a location, set a breakpoint here.  The way to use this is to work out
// the segment you want, and the address of the location in the *source* dylib.  This will then
// compute the equivalent location in the cache builder buffers
#if DEBUG
__attribute__((noinline))
void CacheDylib::watchMemory(const DylibSegmentChunk& segment,
                             std::string_view dylibInstallName,
                             std::string_view dylibSegmentName,
                             uint64_t dylibAddressInSegment) const
{
    if ( this->installName != dylibInstallName )
        return;
    if ( segment.segmentName != dylibSegmentName )
        return;

    printf("watchpoint set expression -w w -s 8 -- %p\n",
           segment.subCacheBuffer + dylibAddressInSegment - segment.inputVMAddress.rawValue());
    printf("watchpoint set expression -w w -s 4 -- %p\n",
           segment.subCacheBuffer + dylibAddressInSegment - segment.inputVMAddress.rawValue());
    printf("");
}
#endif

static bool hasUnalignedFixups(const MachOFile* mf)
{
    // arm64e chained fixup formats are always 8-byte aligned
    if ( mf->isArch("arm64e") )
        return false;

    uint32_t pointerMask = mf->pointerSize() - 1;

    __block Diagnostics diag;
    __block bool foundUnalignedFixup = false;
    mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        mach_o::Fixups fixups(layout);

        if ( mf->hasChainedFixups() ) {
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                fixups.forEachFixupInAllChains(diag, starts, false, ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                    if ( (fixupSegmentOffset & pointerMask) != 0 ) {
                        foundUnalignedFixup = true;
                        stop = true;
                        return;
                    }
                });
            });
        } else {
            fixups.forEachRebaseLocation_Opcodes(diag, ^(uint64_t runtimeOffset, uint32_t segmentIndex, bool &stop) {
                if ( (runtimeOffset & pointerMask) != 0 ) {
                    foundUnalignedFixup = true;
                    stop = true;
                    return;
                }
            });
            fixups.forEachBindLocation_Opcodes(diag, ^(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned int targetIndex, bool &stop) {
                if ( (runtimeOffset & pointerMask) != 0 ) {
                    foundUnalignedFixup = true;
                    stop = true;
                    return;
                }
            }, ^(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned int overrideBindTargetIndex, bool &stop) {
                if ( (runtimeOffset & pointerMask) != 0 ) {
                    foundUnalignedFixup = true;
                    stop = true;
                    return;
                }
            });
        }
    });

    diag.assertNoError();

    return foundUnalignedFixup;
}

static const bool segmentHasAuthFixups(const MachOFile* mf, uint32_t segmentIndexToSearch)
{
    // non-arm64e cannot have auth fixups
    if ( !mf->isArch("arm64e") )
        return false;

    __block Diagnostics diag;
    __block bool foundAuthFixup = false;
    mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        mach_o::Fixups fixups(layout);

        if ( mf->hasChainedFixups() ) {
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                fixups.forEachFixupChainSegment(diag, starts, ^(const dyld_chained_starts_in_segment *segInfo, uint32_t segIndex, bool &stopSegment) {
                    if ( segIndex != segmentIndexToSearch )
                        return;
                    fixups.forEachFixupInSegmentChains(diag, segInfo, segIndex, true,
                                                       ^(dyld3::MachOFile::ChainedFixupPointerOnDisk *fixupLocation, uint64_t fixupSegmentOffset, bool &stopChain) {
                        if ( fixupLocation->arm64e.rebase.auth ) {
                            foundAuthFixup = true;
                            stopChain = true;
                            stopSegment = true;
                        }
                    });
                });
            });
        }

        // Move to auth if __objc_const or __objc_data is present.
        // This allows new method lists added by the category optimizer to be signed.
        // Note the linker eagerly moves these sections to AUTH, as of rdar://111858154,
        // so it is not expected that this code ever finds anything to move, but we'll keep it to be safe
        ((const Header*)mf)->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
            if ( segInfo.segmentIndex != segmentIndexToSearch )
                return;
            if ( (sectInfo.sectionName == "__objc_const") || (sectInfo.sectionName == "__objc_data") ) {
                foundAuthFixup = true;
                stop = true;
            }
        });
    });

    return foundAuthFixup;
}

void CacheDylib::categorizeSegments(const BuilderConfig& config,
                                    objc_visitor::Visitor& objcVisitor)
{
    bool hasUnalignedFixups = ::hasUnalignedFixups(this->inputMF);
    this->inputHdr->forEachSegment(^(const Header::SegmentInfo& info, uint64_t sizeOfSections, uint32_t maxAlignOfSections, bool& stop) {
        auto addSegment = [&](DylibSegmentChunk::Kind kind) {
            // TODO: Cache VMSize/fileSize might be less than input VMSize if we deduplicate strings for example
            uint64_t inputFileSize = std::min((uint64_t)info.fileSize, sizeOfSections);
            uint64_t cacheFileSize = sizeOfSections;
            uint64_t vmSize        = sizeOfSections;

            // LINKEDIT doesn't get space any more. Its individual chunks will get their own space
            if ( info.segmentName == "__LINKEDIT" ) {
                inputFileSize = 0;
                cacheFileSize = 0;
                vmSize        = 0;
            }

            uint64_t minAlignment = 1 << maxAlignOfSections;
            // Always align __TEXT to a page as split seg can't handle less
            if ( info.segmentName == "__TEXT" )
                minAlignment = config.layout.machHeaderAlignment;
            else if ( hasUnalignedFixups )
                minAlignment = (this->inputHdr->uses16KPages() ? 0x4000 : 0x1000);

            DylibSegmentChunk segment(kind, minAlignment);
            segment.segmentName     = info.segmentName;
            segment.inputFile       = this->inputFile;
            segment.inputFileOffset = InputDylibFileOffset((uint64_t)info.fileOffset);
            segment.inputFileSize   = InputDylibFileSize(inputFileSize);
            segment.inputVMAddress  = InputDylibVMAddress(info.vmaddr);
            segment.inputVMSize     = InputDylibVMSize(info.vmsize);

            segment.cacheVMSize      = CacheVMSize(vmSize);
            segment.subCacheFileSize = CacheFileSize(cacheFileSize);

            // Santify check.  The cache buffer adds zero fill so VMSize should always be the largest.
            assert(segment.inputFileSize.rawValue() <= segment.cacheVMSize.rawValue());
            assert(segment.subCacheFileSize.rawValue() <= segment.cacheVMSize.rawValue());

            this->segments.push_back(std::move(segment));
        };

        // __TEXT
        if ( info.initProt == (VM_PROT_READ | VM_PROT_EXECUTE) ) {
            addSegment(DylibSegmentChunk::Kind::dylibText);
            return;
        }

        // DATA*
        if ( info.initProt == (VM_PROT_READ | VM_PROT_WRITE) ) {
            // If we don't have split seg v2, then all __DATA* segments must look like __DATA so that they
            // stay contiguous
            __block bool isSplitSegV2 = false;
            Diagnostics diag;
            this->inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
                mach_o::SplitSeg splitSeg(layout);

                isSplitSegV2 = splitSeg.isV2();
            });
            diag.assertNoError();

            if ( !isSplitSegV2 ) {
                addSegment(DylibSegmentChunk::Kind::dylibData);
                return;
            }

            if ( info.segmentName == "__TPRO_CONST" ) {
                addSegment(DylibSegmentChunk::Kind::tproDataConst);
                return;
            }

            if ( info.segmentName == "__OBJC_CONST" ) {
                // In arm64e, "__OBJC_CONST __objc_class_ro" contains authenticated values
                if ( config.layout.hasAuthRegion )
                    addSegment(DylibSegmentChunk::Kind::dylibAuthConst);
                else
                    addSegment(DylibSegmentChunk::Kind::dylibDataConst);
                return;
            }

            if ( info.segmentName == "__DATA_DIRTY" ) {
                addSegment(DylibSegmentChunk::Kind::dylibDataDirty);
                return;
            }

            bool hasAuthFixups = false;
            if ( (info.segmentName == "__AUTH") || (info.segmentName == "__AUTH_CONST") ) {
                hasAuthFixups = true;
            } else if ( config.layout.hasAuthRegion ) {
                // HACK: Some dylibs don't get __AUTH segments.  This matches ld64
                hasAuthFixups = segmentHasAuthFixups(this->inputMF, info.segmentIndex);
            }

            bool isConst = info.segmentName.ends_with("_CONST");
            if ( hasAuthFixups ) {
                // AUTH/AUTH_CONST
                if ( isConst ) {
                    // AUTH_CONST
                    addSegment(DylibSegmentChunk::Kind::dylibAuthConst);
                    return;
                } else {
                    // AUTH
                    addSegment(DylibSegmentChunk::Kind::dylibAuth);
                    return;
                }
            } else {
                // DATA/DATA_CONST
                if ( isConst ) {
                    // DATA_CONST
                    addSegment(DylibSegmentChunk::Kind::dylibDataConst);
                    return;
                } else {
                    // DATA
                    addSegment(DylibSegmentChunk::Kind::dylibData);
                    return;
                }
            }
        }

        // LINKEDIT/readOnly
        if ( info.initProt == (VM_PROT_READ) ) {
            if ( info.segmentName != "__LINKEDIT" ) {
                addSegment(DylibSegmentChunk::Kind::dylibReadOnly);
                return;
            }

            addSegment(DylibSegmentChunk::Kind::dylibLinkedit);
            return;
        }

        // Not text/data/linkedit.  This should have been caught by canBePlacedInDyldCache()
        assert(0);
    });
}

// The export trie might grow, as addresses outside of __TEXT will need more uleb bytes to encode when their
// addresses grow.  Estimate how much space we need to grow the given trie
static uint32_t estimateExportTrieSize(const uint8_t* start, const uint8_t* end)
{

    // FIXME: This is terrible.  We could actually estimate the result, not just calculate it
    // Eg, just assume all nodes outside __TEXT will grow by however many bytes it takes to encode about 2GB
    std::vector<uint8_t> newTrieBytes;
    Diagnostics          diag;

    if ( start == end )
        return 0;

    // since export info addresses are offsets from mach_header, everything in __TEXT is fine
    // only __DATA addresses need to be updated
    std::vector<ExportInfoTrie::Entry> originalExports;
    if ( !ExportInfoTrie::parseTrie(start, end, originalExports) ) {
        diag.error("malformed exports trie in");
        assert(0);
        return 0;
    }

    std::vector<ExportInfoTrie::Entry> newExports;
    newExports.reserve(originalExports.size());

    // Assume dylibs start at 0, and will slide to 2GB
    uint64_t baseAddress      = 0;
    uint64_t baseAddressSlide = 1ULL << 31;
    for ( auto& entry : originalExports ) {
        // remove symbols used by the static linker only
        // FIXME: This can result in the cache export-trie being smaller than the input dylib
        // But then the initial linkedit chunk doesn't contain the whole trie and adjustExportsTrie() fails
        // If we are going to allow a smaller true in the cache, then we need adjustExportsTrie() to consume the
        // trie from the input dylib, and emit a trie in to the cache.
#if 0
        if (   (strncmp(entry.name.c_str(), "$ld$", 4) == 0)
            || (strncmp(entry.name.c_str(), ".objc_class_name",16) == 0)
            || (strncmp(entry.name.c_str(), ".objc_category_name",19) == 0) ) {
            continue;
        }
#endif
        // adjust symbols in slid segments
        if ( (entry.info.flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) != EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE )
            entry.info.address += (baseAddressSlide - baseAddress);
        newExports.push_back(entry);
    }

    // rebuild export trie
    newTrieBytes.reserve(end - start);

    ExportInfoTrie(newExports).emit(newTrieBytes);
    // align
    while ( (newTrieBytes.size() % sizeof(uint64_t)) != 0 )
        newTrieBytes.push_back(0);

    // HACK: copyRawSegments() is going to first copy the original trie in to the buffer, so make
    // sure we have at least that much space
    size_t requiredSize = std::max((uint64_t)newTrieBytes.size(), (uint64_t)end - (uint64_t)start);

    return (uint32_t)requiredSize;
}

void CacheDylib::categorizeLinkedit(const BuilderConfig& config)
{
    uint32_t pointerSize = config.layout.is64 ? 8 : 4;

    __block Diagnostics diag;
    this->inputMF->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        typedef Chunk::Kind Kind;
        auto addLinkedit = [&](Kind kind, InputDylibFileOffset inputFileOffset,
                               InputDylibFileSize inputFileSize, CacheVMSize estimatedCacheVMSize,
                               uint64_t minAlignment) {
            LinkeditDataChunk chunk(kind, minAlignment);
            chunk.inputFile       = this->inputFile;
            chunk.inputFileOffset = inputFileOffset;
            chunk.inputFileSize   = inputFileSize;

            chunk.cacheVMSize      = estimatedCacheVMSize;
            chunk.subCacheFileSize = CacheFileSize(estimatedCacheVMSize.rawValue());
            this->linkeditChunks.push_back(std::move(chunk));
        };

        switch ( cmd->cmd ) {
            case LC_SYMTAB: {
                const symtab_command* symTabCmd = (const symtab_command*)cmd;

                // NList
                uint64_t nlistEntrySize  = config.layout.is64 ? sizeof(struct nlist_64) : sizeof(struct nlist);
                uint64_t symbolTableSize = symTabCmd->nsyms * nlistEntrySize;
                addLinkedit(Kind::linkeditSymbolNList, InputDylibFileOffset((uint64_t)symTabCmd->symoff),
                            InputDylibFileSize(symbolTableSize), CacheVMSize(symbolTableSize),
                            pointerSize);

                // Symbol strings
                addLinkedit(Kind::linkeditSymbolStrings, InputDylibFileOffset((uint64_t)symTabCmd->stroff),
                            InputDylibFileSize((uint64_t)symTabCmd->strsize), CacheVMSize((uint64_t)symTabCmd->strsize),
                            1);
                break;
            }
            case LC_DYSYMTAB: {
                const dysymtab_command* dynSymTabCmd = (const dysymtab_command*)cmd;

                assert(dynSymTabCmd->tocoff == 0);
                assert(dynSymTabCmd->ntoc == 0);
                assert(dynSymTabCmd->modtaboff == 0);
                assert(dynSymTabCmd->nmodtab == 0);
                assert(dynSymTabCmd->extrefsymoff == 0);
                assert(dynSymTabCmd->nextrefsyms == 0);

                if ( dynSymTabCmd->indirectsymoff != 0 ) {
                    assert(dynSymTabCmd->nindirectsyms != 0);

                    // Indirect symbols
                    uint64_t entrySize = sizeof(uint32_t);
                    uint64_t tableSize = dynSymTabCmd->nindirectsyms * entrySize;
                    addLinkedit(Kind::linkeditIndirectSymbols, InputDylibFileOffset((uint64_t)dynSymTabCmd->indirectsymoff),
                                InputDylibFileSize(tableSize), CacheVMSize(tableSize),
                                4);
                }
                else {
                    assert(dynSymTabCmd->nindirectsyms == 0);
                }

                assert(dynSymTabCmd->extreloff == 0);
                assert(dynSymTabCmd->nextrel == 0);
                assert(dynSymTabCmd->locreloff == 0);
                assert(dynSymTabCmd->nlocrel == 0);
                break;
            }
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                // Most things should be chained fixups, but some old dylibs exist for back deployment
                const dyld_info_command* linkeditCmd = (const dyld_info_command*)cmd;

                this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
                    this->inputDylibRebaseStart   = layout.linkedit.rebaseOpcodes.buffer;
                    this->inputDylibRebaseEnd     = this->inputDylibRebaseStart + layout.linkedit.rebaseOpcodes.bufferSize;
                    this->inputDylibBindStart     = layout.linkedit.regularBindOpcodes.buffer;
                    this->inputDylibBindEnd       = this->inputDylibBindStart + layout.linkedit.regularBindOpcodes.bufferSize;
                    this->inputDylibLazyBindStart = layout.linkedit.lazyBindOpcodes.buffer;
                    this->inputDylibLazyBindEnd   = this->inputDylibLazyBindStart + layout.linkedit.lazyBindOpcodes.bufferSize;
                    this->inputDylibWeakBindStart = layout.linkedit.weakBindOpcodes.buffer;
                    this->inputDylibWeakBindEnd   = this->inputDylibWeakBindStart + layout.linkedit.weakBindOpcodes.bufferSize;

                    // The export trie is going to change size, as it might grow/shrink based on removing elements
                    // but addresses growing in size
                    const uint8_t* trieStart = layout.linkedit.exportsTrie.buffer;
                    const uint8_t* trieEnd   = trieStart + layout.linkedit.exportsTrie.bufferSize;
                    uint32_t estimatedSize = estimateExportTrieSize(trieStart, trieEnd);

                    addLinkedit(Kind::linkeditExportTrie, InputDylibFileOffset((uint64_t)linkeditCmd->export_off),
                                InputDylibFileSize((uint64_t)linkeditCmd->export_size), CacheVMSize((uint64_t)estimatedSize),
                                pointerSize);
                });
                break;
            }
            case LC_SEGMENT_SPLIT_INFO: {
                // The final cache dylib won't have split seg, but keep a pointer to the source dylib split seg, for use later
                this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
                    this->inputDylibSplitSegStart = layout.linkedit.splitSegInfo.buffer;
                    this->inputDylibSplitSegEnd   = this->inputDylibSplitSegStart + layout.linkedit.splitSegInfo.bufferSize;
                });
                break;
            }
            case LC_FUNCTION_STARTS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                addLinkedit(Kind::linkeditFunctionStarts, InputDylibFileOffset((uint64_t)linkeditCmd->dataoff),
                            InputDylibFileSize((uint64_t)linkeditCmd->datasize), CacheVMSize((uint64_t)linkeditCmd->datasize),
                            pointerSize);
                break;
            }
            case LC_DATA_IN_CODE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                addLinkedit(Kind::linkeditDataInCode, InputDylibFileOffset((uint64_t)linkeditCmd->dataoff),
                            InputDylibFileSize((uint64_t)linkeditCmd->datasize), CacheVMSize((uint64_t)linkeditCmd->datasize),
                            pointerSize);
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS: {
                // Drop chained fixups
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
                    // The export trie is going to change size, as it might grow/shrink based on removing elements
                    // but addresses growing in size
                    const uint8_t* trieStart = layout.linkedit.exportsTrie.buffer;
                    const uint8_t* trieEnd   = trieStart + layout.linkedit.exportsTrie.bufferSize;
                    uint32_t estimatedSize = estimateExportTrieSize(trieStart, trieEnd);


                    addLinkedit(Kind::linkeditExportTrie, InputDylibFileOffset((uint64_t)linkeditCmd->dataoff),
                                InputDylibFileSize((uint64_t)linkeditCmd->datasize), CacheVMSize((uint64_t)estimatedSize),
                                pointerSize);
                });
                break;
            }
            case LC_FUNCTION_VARIANTS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                addLinkedit(Kind::linkeditFunctionVariants, InputDylibFileOffset((uint64_t)linkeditCmd->dataoff),
                            InputDylibFileSize((uint64_t)linkeditCmd->datasize), CacheVMSize((uint64_t)linkeditCmd->datasize),
                            pointerSize);
                break;
            }
        }
    });
    diag.assertNoError();
}

void CacheDylib::copyRawSegments(const BuilderConfig& config, Timer::AggregateTimer& timer)
{
    const bool log = config.log.printDebug;

    Timer::AggregateTimer::Scope timedScope(timer, "dylib copyRawSegments time");

    for ( const DylibSegmentChunk& segment : this->segments ) {
        const uint8_t* srcSegment = (uint8_t*)segment.inputFile->mf + segment.inputFileOffset.rawValue();

        if ( segment.subCacheBuffer == nullptr ) {
            // Note, Linkedit isn't copied here, so will have no buffer, even though it has a size
            assert( (segment.cacheVMSize == CacheVMSize(0ULL)) || (segment.segmentName == "__LINKEDIT") );
            if ( log ) {
                config.log.log("Skipping empty segment %s\n", segment.segmentName.data());
            }
        }
        else {
            if ( log ) {
                config.log.log("Copying %s from %p to (%p..%p)\n", segment.segmentName.data(), srcSegment, segment.subCacheBuffer, segment.subCacheBuffer + segment.inputFileSize.rawValue());
            }
            memcpy(segment.subCacheBuffer, srcSegment, segment.inputFileSize.rawValue());
        }

#if DEBUG
        watchMemory(segment, "install name", "segment name", 0x0);
#endif
    }

    // Also copy linkedit in to place
    for ( const LinkeditDataChunk& chunk : this->linkeditChunks ) {
        const uint8_t* srcChunk = (uint8_t*)chunk.inputFile->mf + chunk.inputFileOffset.rawValue();
        if ( log ) {
            config.log.log("Copying from %p to (%p..%p)\n", srcChunk, chunk.subCacheBuffer, chunk.subCacheBuffer + chunk.inputFileSize.rawValue());
        }
        memcpy(chunk.subCacheBuffer, srcChunk, chunk.inputFileSize.rawValue());
    }

    // The nlist was optimized.  Its not in the linkeditChunks
    if ( !this->optimizedSymbols.nlist64.empty() ) {
        memcpy(this->optimizedSymbols.subCacheBuffer, this->optimizedSymbols.nlist64.data(),
               sizeof(struct nlist_64) * this->optimizedSymbols.nlist64.size());
    } else {
        memcpy(this->optimizedSymbols.subCacheBuffer, this->optimizedSymbols.nlist32.data(),
               sizeof(struct nlist) * this->optimizedSymbols.nlist32.size());
    }
}

void CacheDylib::applySplitSegInfo(Diagnostics& diag, const BuilderOptions& options,
                                   const BuilderConfig& config, Timer::AggregateTimer& timer,
                                   UnmappedSymbolsOptimizer& unmappedSymbolsOptimizer)
{
    Timer::AggregateTimer::Scope timedScope(timer, "dylib applySplitSegInfo time");

    __block const uint8_t* chainedFixupsStart = nullptr;
    __block const uint8_t* chainedFixupsEnd   = nullptr;
    __block const uint8_t* rebaseOpcodesStart = nullptr;
    __block const uint8_t* rebaseOpcodesEnd   = nullptr;

    this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        if ( layout.linkedit.regularBindOpcodes.hasValue() ) {
            rebaseOpcodesStart = layout.linkedit.rebaseOpcodes.buffer;
            rebaseOpcodesEnd = rebaseOpcodesStart + layout.linkedit.rebaseOpcodes.bufferSize;
        } else if ( layout.linkedit.chainedFixups.hasValue() ) {
            chainedFixupsStart = layout.linkedit.chainedFixups.buffer;
            chainedFixupsEnd = chainedFixupsStart + layout.linkedit.chainedFixups.bufferSize;
        }
    });

    adjustor->adjustDylib(diag, config.layout.cacheBaseAddress, this->cacheMF, this->installName,
                          chainedFixupsStart, chainedFixupsEnd, this->inputDylibSplitSegStart, this->inputDylibSplitSegEnd,
                          rebaseOpcodesStart, rebaseOpcodesEnd, &this->optimizedSections);

    // Not strictly part of the dylib any more, but the unmapped locals also need adjusting
    if ( options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
        typedef UnmappedSymbolsOptimizer::LocalSymbolInfo LocalSymbolInfo;
        LocalSymbolInfo& symbolInfo = unmappedSymbolsOptimizer.symbolInfos[this->cacheIndex];
        for ( uint32_t i = 0; i != symbolInfo.nlistCount; ++i ) {
            uint32_t symbolIndex = symbolInfo.nlistStartIndex + i;

            if ( config.layout.is64 ) {
                struct nlist_64& sym = unmappedSymbolsOptimizer.symbolNlistChunk.nlist64[symbolIndex];
                InputDylibVMAddress inputVMAddr(sym.n_value);
                CacheVMAddress cacheVMAddr = adjustor->adjustVMAddr(inputVMAddr);
                sym.n_value = cacheVMAddr.rawValue();
            } else {
                struct nlist& sym = unmappedSymbolsOptimizer.symbolNlistChunk.nlist32[symbolIndex];
                InputDylibVMAddress inputVMAddr((uint64_t)sym.n_value);
                CacheVMAddress cacheVMAddr = adjustor->adjustVMAddr(inputVMAddr);
                sym.n_value = (uint32_t)cacheVMAddr.rawValue();
            }
        }
    }
}

void CacheDylib::updateSymbolTables(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer)
{
    Timer::AggregateTimer::Scope timedScope(timer, "dylib updateSymbolTables time");

    for ( LinkeditDataChunk& chunk : this->linkeditChunks ) {
        if ( !chunk.isIndirectSymbols() )
            continue;

        // We found the indirect symbol table, now make sure the updated table we cached from earlier
        // is the correct size
        uint64_t newTableSize = this->indirectSymbolTable.size() * sizeof(uint32_t);
        if ( newTableSize != chunk.cacheVMSize.rawValue() ) {
            diag.error("Wrong indirect symbol table size (%lld != %lld)",
                       newTableSize, chunk.cacheVMSize.rawValue());
            return;
        }

        memcpy(chunk.subCacheBuffer, this->indirectSymbolTable.data(), newTableSize);
    }
}

std::optional<CacheDylib::BindTargetAndName> CacheDylib::findDyldMagicSymbolAddress(const char* fullSymbolName, std::string_view name) const
{
    auto nextString = [&]() -> std::string_view {
        auto pos = name.find('$');
        if ( pos == std::string_view::npos ) {
            auto str = name;
            name = "";
            return str;
        }
        auto str = name.substr(0, pos);
        name = name.substr(pos + 1);
        return str;
    };

    std::string_view type = nextString();
    if ( type == "segment" ) {
        std::string_view segmentType = nextString();
        std::string_view segmentName = nextString();

        bool isStart = (segmentType == "start");
        bool isEnd   = (segmentType == "end");

        __block std::optional<VMAddress> vmAddr;

        this->inputHdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
            if ( info.segmentName == segmentName ) {
                if ( isStart )
                    vmAddr = VMAddress(info.vmaddr);
                else if ( isEnd )
                    vmAddr = VMAddress(info.vmaddr) + VMOffset(info.vmsize);

                stop = true;
            }
        });

        if ( !vmAddr )
            return std::nullopt;

        VMOffset vmOff = *vmAddr - VMAddress(inputLoadAddress.rawValue());
        return std::make_pair(BindTarget{ BindTarget::Kind::inputImage, { .inputImage = { vmOff, this, /* weak def */ false }  } }, std::string(fullSymbolName));
    }

    return std::nullopt;
}

// FIXME: This was stolen from Loader.  try unify them again
CacheDylib::BindTargetAndName CacheDylib::resolveSymbol(Diagnostics& diag, int libOrdinal, const char* symbolName,
                                                        bool weakImport, const std::vector<const CacheDylib*>& cacheDylibs) const
{
    const CacheDylib* targetDylib = nullptr;

    BindTarget nullBindTarget = { BindTarget::Kind::absolute, { .absolute = { 0 } } };

    if ( (libOrdinal > 0) && ((unsigned)libOrdinal <= this->inputDependents.size()) ) {
        targetDylib = this->inputDependents[libOrdinal - 1].dylib;
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
        targetDylib = this;
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
        diag.error("shared cache dylibs bind to the main executable: %s\n  Referenced from: %s", symbolName, this->installName.data());
        return { nullBindTarget, "" };
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
        for ( const CacheDylib* cacheDylib : cacheDylibs ) {
            std::optional<BindTargetAndName> bindTargetAndName = cacheDylib->hasExportedSymbol(diag, symbolName, SearchMode::onlySelf);
            if ( bindTargetAndName.has_value() )
                return bindTargetAndName.value();
        }

        if ( weakImport ) {
            // ok to be missing, bind to NULL
            return { nullBindTarget, "" };
        }

        // missing symbol, but not weak-import or lazy-bound, so error
        diag.error("symbol not found in flat namespace '%s'\n  Referenced from: %s", symbolName, this->installName.data());
        return { nullBindTarget, "" };
    }
    else if ( libOrdinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP ) {
        // when dylibs in cache are build, we don't have real load order, so do weak binding differently

        // look first in /usr/lib/libc++, most will be here
        for ( const CacheDylib* cacheDylib : cacheDylibs ) {
            if ( cacheDylib->inputHdr->hasWeakDefs() && startsWith(cacheDylib->installName, "/usr/lib/libc++.") ) {
                std::optional<BindTargetAndName> bindTargetAndName = cacheDylib->hasExportedSymbol(diag, symbolName, SearchMode::onlySelf);
                if ( bindTargetAndName.has_value() )
                    return bindTargetAndName.value();

                // We found libc++, but not this symbol.  Break out of the loop as we don't need to look in other images
                break;
            }
        }

        // if not found, try looking in the images itself, most custom weak-def symbols have a copy in the image itself
        std::optional<BindTargetAndName> sellBindTargetAndName = this->hasExportedSymbol(diag, symbolName, SearchMode::onlySelf);
        if ( sellBindTargetAndName.has_value() )
            return sellBindTargetAndName.value();

        // if this image directly links with something that also defines this weak-def, use that because we know it will be loaded
        for ( const CacheDylib::DependentDylib& dependentDylib : this->inputDependents ) {
            if ( dependentDylib.kind == DependentDylib::Kind::upward )
                continue;

            // Skip missing weak dylibs
            if ( dependentDylib.kind == DependentDylib::Kind::weakLink ) {
                if ( dependentDylib.dylib == nullptr )
                    continue;
            }

            std::optional<BindTargetAndName> bindTargetAndName = dependentDylib.dylib->hasExportedSymbol(diag, symbolName, SearchMode::selfAndReexports);
            if ( bindTargetAndName.has_value() )
                return bindTargetAndName.value();
        }

        // no impl??
        diag.error("weak-def symbol (%s) not found in dyld cache\n  Referenced from: %s", symbolName, this->installName.data());
        return { nullBindTarget, "" };
    }
    else {
        diag.error("unknown library ordinal %d in %s when binding '%s'", libOrdinal, this->installName.data(), symbolName);
        return { nullBindTarget, "" };
    }
    if ( targetDylib != nullptr ) {
        if ( const char* dyldMagic = strstr(symbolName, "$dyld$") ) {
            std::string_view name = dyldMagic + 6;
            std::optional<BindTargetAndName> target;

            // only synthetic dylibs without a need for the patch table can use magic dyld symbols
            // dyld itself does not know about them so it won't be able to bind them
            if ( !needsPatchTable )
                target = targetDylib->findDyldMagicSymbolAddress(symbolName, name);
            if ( target )
                return target.value();

            const char* expectedInDylib = "unknown";
            if ( targetDylib != nullptr )
                expectedInDylib = targetDylib->installName.data();

            diag.error("Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s", symbolName, this->installName.data(), expectedInDylib);
            return { nullBindTarget, "" };
        }

        std::optional<BindTargetAndName> bindTargetAndName = targetDylib->hasExportedSymbol(diag, symbolName, SearchMode::selfAndReexports);
        if ( diag.hasError() )
            return { nullBindTarget, "" };

        if ( bindTargetAndName.has_value() )
            return bindTargetAndName.value();
    }
    if ( weakImport ) {
        // ok to be missing, bind to NULL
        return { nullBindTarget, "" };
    }

    const char* expectedInDylib = "unknown";
    if ( targetDylib != nullptr )
        expectedInDylib = targetDylib->installName.data();

    diag.error("Symbol not found: %s\n  Referenced from: %s\n  Expected in: %s", symbolName, this->installName.data(), expectedInDylib);
    return { nullBindTarget, "" };
}

std::optional<CacheDylib::BindTargetAndName> CacheDylib::hasExportedSymbol(Diagnostics& diag, const char* symbolName, SearchMode mode) const
{
    bool canSearchDependentReexports = false;
    bool searchSelf                  = false;
    switch ( mode ) {
        case SearchMode::onlySelf:
            canSearchDependentReexports = false;
            searchSelf                  = true;
            break;
        case SearchMode::selfAndReexports:
            canSearchDependentReexports = true;
            searchSelf                  = true;
            break;
    }

    __block const uint8_t* trieStart = nullptr;
    __block const uint8_t* trieEnd   = nullptr;
    this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        if ( layout.linkedit.exportsTrie.hasValue() ) {
            trieStart = layout.linkedit.exportsTrie.buffer;
            trieEnd   = trieStart + layout.linkedit.exportsTrie.bufferSize;
        }
    });

    if ( trieStart == nullptr ) {
        diag.error("shared cache dylibs must have an export trie");
        return {};
    }
    const uint8_t* node = MachOFile::trieWalk(diag, trieStart, trieEnd, symbolName);
    //state.log("    trieStart=%p, trieSize=0x%08X, node=%p, error=%s\n", trieStart, trieSize, node, diag.errorMessage());
    if ( (node != nullptr) && searchSelf ) {
        const uint8_t* p     = node;
        const uint64_t flags = MachOFile::read_uleb128(diag, p, trieEnd);
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            // re-export from another dylib, lookup there
            const uint64_t ordinal      = MachOFile::read_uleb128(diag, p, trieEnd);
            const char*    importedName = (char*)p;
            if ( importedName[0] == '\0' ) {
                importedName = symbolName;
            }
            if ( (ordinal == 0) || (ordinal > this->inputDependents.size()) ) {
                diag.error("re-export ordinal %lld in %s out of range for %s", ordinal, this->installName.data(), symbolName);
                return {};
            }
            uint32_t depIndex = (uint32_t)(ordinal - 1);
            if ( const CacheDylib* dependentDylib = this->inputDependents[depIndex].dylib )
                return dependentDylib->hasExportedSymbol(diag, importedName, mode);

            // re-exported symbol from weak-linked dependent which is missing
            return {};
        }
        else {
            if ( diag.hasError() )
                return {};
            bool     isAbsoluteSymbol = ((flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE);
            bool     isWeakDef        = (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
            bool     isFuncVariant    = (flags & EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT);
            uint64_t value            = MachOFile::read_uleb128(diag, p, trieEnd);

            if ( isAbsoluteSymbol ) {
                BindTarget result = { BindTarget::Kind::absolute, { .absolute = { value } } };
                return (BindTargetAndName) { result, symbolName };
            }

            uint16_t fvTableIndex = 0;
            if ( isFuncVariant ) {
                // next uleb128 is func-variant table index
                fvTableIndex = (uint16_t)MachOFile::read_uleb128(diag, p, trieEnd);
            }

            // Bind to image
            BindTarget result = { BindTarget::Kind::inputImage, { .inputImage = { VMOffset(value), this, isWeakDef, isFuncVariant, fvTableIndex } } };
            return (BindTargetAndName) { result, symbolName };
        }
    }

    if ( canSearchDependentReexports ) {
        // Search re-exported dylibs
        for ( const CacheDylib::DependentDylib& dependentDylib : this->inputDependents ) {
            if ( dependentDylib.kind != DependentDylib::Kind::reexport )
                continue;

            // No need for a weak check here as re-exports can't be weak

            std::optional<BindTargetAndName> bindTargetAndName = dependentDylib.dylib->hasExportedSymbol(diag, symbolName, mode);
            if ( diag.hasError() )
                return {};

            if ( bindTargetAndName.has_value() )
                return bindTargetAndName.value();
        }
    }
    return {};
}

std::vector<Error> CacheDylib::calculateBindTargets(Diagnostics& diag,
                                                    const BuilderConfig& config, Timer::AggregateTimer& timer,
                                                    const std::vector<const CacheDylib*>& cacheDylibs,
                                                    PatchInfo& dylibPatchInfo)
{
    Timer::AggregateTimer::Scope timedScope(timer, "dylib calculateBindTargets time");

    // As we are running in parallel, addresses in other dylibs may not have been shifted yet.  We may also
    // race looking at the export trie in a target dylib, while it is being shifted by AdjustDylibSegments.
    // Given that, we'll do all the analysis on the input dylibs, with knowledge of where they'll shift to

    __block std::vector<Error> errors;
    auto handleBindTarget = ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
        Diagnostics symbolDiag;
        BindTargetAndName bindTargetAndName = this->resolveSymbol(symbolDiag, libOrdinal, symbolName, weakImport, cacheDylibs);
        BindTarget&       bindTarget        = bindTargetAndName.first;
        if ( symbolDiag.hasError() ) {
            errors.push_back(Error("%s", symbolDiag.errorMessageCStr()));
            return;
        }

        // Adjust the bind target.  We have a runtime offset for the target input dylib, but we need to know where that runtime Offset will
        // map to in the target cache dylib
        switch ( bindTarget.kind ) {
            case BindTarget::Kind::absolute:
                // Skip these.  They won't change due to shifting the input dylib in to the cache
                break;
            case BindTarget::Kind::inputImage: {
                // Convert from an input dylib offset to the cache dylib offset
                BindTarget::InputImage inputImage        = bindTarget.inputImage;
                InputDylibVMAddress    targetInputVMAddr = inputImage.targetDylib->inputLoadAddress + inputImage.targetRuntimeOffset;
                CacheVMAddress         targetCacheVMAddr = inputImage.targetDylib->adjustor->adjustVMAddr(targetInputVMAddr);

                // Actually change the bindTarget to reflect the new type
                bindTarget.kind = BindTarget::Kind::cacheImage;
                bindTarget.inputImage.~InputImage();
                bindTarget.cacheImage = (BindTarget::CacheImage) { VMOffset(targetCacheVMAddr - inputImage.targetDylib->cacheLoadAddress),
                                                                   inputImage.targetDylib, inputImage.isWeakDef,
                                                                   inputImage.isFunctionVariant, inputImage.functionVariantTableIndex };
                break;
            }
            case BindTarget::Kind::cacheImage:
                diag.error("Shouldn't see cacheImage fixups at this point");
                stop = true;
                return;
        }

        bindTarget.addend = addend;
        bindTarget.isWeakImport = weakImport;
#if DEBUG
        bindTarget.name = symbolName;
#endif
        this->bindTargets.push_back(std::move(bindTarget));
        dylibPatchInfo.bindTargetNames.push_back(std::move(bindTargetAndName.second));
    };

    if ( this->inputMF->hasChainedFixups() ) {
        // Ideally we'd just walk the chained fixups command, but the macOS simulator support dylibs use
        // the old threaded rebase format, not chained fixups
        this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
            mach_o::Fixups fixups(layout);

            fixups.forEachBindTarget(diag, false, 0, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                handleBindTarget(info.libOrdinal, info.symbolName, info.addend, info.weakImport, stop);
            }, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                // This should never happen on chained fixups
                assert(0);
            });
        });
    }
    else if ( this->inputMF->hasOpcodeFixups() ) {
        // Use the fixups from the source dylib
        mach_o::LinkeditLayout linkedit;
        if ( !this->inputMF->getLinkeditLayout(diag, linkedit) ) {
            diag.error("Couldn't get dylib layout");
            return { };
        }

        // Use the segment layout from the cache dylib so that VMAddresses are correct
        __block std::vector<mach_o::SegmentLayout> segmentLayout;
        segmentLayout.reserve(this->segments.size());
        for ( const DylibSegmentChunk& dylibSegment : this->segments ) {
            mach_o::SegmentLayout segment;
            segment.vmAddr      = dylibSegment.cacheVMAddress.rawValue();
            segment.vmSize      = dylibSegment.cacheVMSize.rawValue();
            segment.fileOffset  = dylibSegment.subCacheFileOffset.rawValue();
            segment.fileSize    = dylibSegment.subCacheFileSize.rawValue();
            segment.buffer      = dylibSegment.subCacheBuffer;

            segment.kind        = mach_o::SegmentLayout::Kind::unknown;
            if ( dylibSegment.segmentName == "__TEXT" ) {
                segment.kind    = mach_o::SegmentLayout::Kind::text;
            } else if ( dylibSegment.segmentName == "__LINKEDIT" ) {
                segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
            }
            segmentLayout.push_back(segment);
        }

        // The cache segments don't have the permissions.  Get that from the load commands
        this->cacheHdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
            segmentLayout[info.segmentIndex].protections = info.initProt;
        });

        mach_o::Layout layout(this->inputMF, { segmentLayout.data(), segmentLayout.data() + segmentLayout.size() }, linkedit);
        mach_o::Fixups fixups(layout);

        bool allowLazyBinds = false;
        fixups.forEachBindTarget(diag, allowLazyBinds, 0,
            ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                handleBindTarget(info.libOrdinal, info.symbolName, info.addend, info.weakImport, stop);
            },
            ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                if ( !this->weakBindTargetsStartIndex.has_value() )
                    this->weakBindTargetsStartIndex = this->bindTargets.size();
                handleBindTarget(info.libOrdinal, info.symbolName, info.addend, info.weakImport, stop);
            });
    }
    else {
        // Cache dylibs shouldn't use old style fixups.
    }

    if ( !errors.empty() )
        diag.error("missing symbols");

    return std::move(errors);
}

void CacheDylib::bindLocation(Diagnostics& diag, const BuilderConfig& config,
                              const BindTarget& bindTarget, uint64_t addend,
                              uint32_t bindOrdinal, uint32_t segIndex,
                              dyld3::MachOFile::ChainedFixupPointerOnDisk* fixupLoc,
                              CacheVMAddress fixupVMAddr, MachOFile::PointerMetaData pmd,
                              CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                              CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                              FunctionVariantsOptimizer& functionVariantsOptimizer)
{
    switch ( bindTarget.kind ) {
        case BindTarget::Kind::absolute: {
            uint64_t targetValue = bindTarget.absolute.value + addend;

            if ( needsPatchTable ) {
                auto checkGOTs = ^(CoalescedGOTMap& gotMap, std::vector<std::vector<PatchInfo::GOTInfo>>& gotInfo) {
                    auto gotIt = gotMap.find(fixupVMAddr);
                    if ( gotIt != gotMap.end() ) {
                        // Probably a missing weak import.  Rewrite the original GOT anyway, but also the coalesced one
                        dyld_cache_patchable_location patchLoc(gotIt->second, pmd, addend, bindTarget.isWeakImport);
                        auto& gotUses = gotInfo[bindOrdinal];
                        gotUses.emplace_back((PatchInfo::GOTInfo){ patchLoc, VMOffset(targetValue) });
                        return true;
                    }
                    return false;
                };
                if ( checkGOTs(coalescedGOTs, dylibPatchInfo.bindGOTUses) ) {
                    // normal GOT
                } else if ( checkGOTs(coalescedAuthGOTs, dylibPatchInfo.bindAuthGOTUses) ) {
                    // auth GOT
                } else if ( checkGOTs(coalescedAuthPtrs, dylibPatchInfo.bindAuthPtrUses) ) {
                    // auth ptr
                }
            }

            if ( config.layout.is64 ) {
                fixupLoc->raw64 = targetValue;
            }
            else {
                fixupLoc->raw32 = (uint32_t)targetValue;
            }

            // Tell the slide info emitter to ignore this location
            this->segments[segIndex].tracker.remove(fixupLoc);
            return;
        }
        case BindTarget::Kind::inputImage: {
            diag.error("Input binds should have been converted to cache binds in %s: %d",
                       this->installName.data(), bindOrdinal);
            return;
        }
        case BindTarget::Kind::cacheImage: {
            CacheVMAddress targetDylibLoadAddress = bindTarget.cacheImage.targetDylib->cacheLoadAddress;
            CacheVMAddress targetVMAddr = targetDylibLoadAddress + bindTarget.cacheImage.targetRuntimeOffset;
            uint64_t       finalVMAddrWithAddend  = targetVMAddr.rawValue() + addend;
            if ( config.layout.is64 ) {
                uint64_t finalVMAddr = finalVMAddrWithAddend;

                uint8_t high8 = (uint8_t)(finalVMAddr >> 56);
                if ( high8 != 0 ) {
                    // Remove high8 from the vmAddr
                    finalVMAddr = finalVMAddr & 0x00FFFFFFFFFFFFFFULL;
                }

                Fixup::Cache64::setLocation(config.layout.cacheBaseAddress,
                                            fixupLoc, CacheVMAddress(finalVMAddr),
                                            high8,
                                            pmd.diversity, pmd.usesAddrDiversity, pmd.key,
                                            pmd.authenticated);
            } else {
                Fixup::Cache32::setLocation(config.layout.cacheBaseAddress,
                                            fixupLoc, CacheVMAddress(finalVMAddrWithAddend));
            }

            // Tell the slide info emitter to slide this location
            this->segments[segIndex].tracker.add(fixupLoc);

            // Work out if the location we just wrote is a coalesced GOT.  If so, NULL the current location and
            // note down the fixup to the GOT.  We can't just apply the GOT fixup, as we might be running in parallel with
            // other threads all trying to do the same thing
            if( needsPatchTable ) {
                uint64_t                   patchTableAddend = addend;
                MachOFile::PointerMetaData patchTablePMD    = pmd;
                uint64_t                   addendHigh8      = addend >> 56;
                if ( addendHigh8 != 0 ) {
                    // Put the high8 from the addend in to the high8 of the patch
                    assert(patchTablePMD.high8 == 0);
                    patchTablePMD.high8 = (uint32_t)addendHigh8;

                    // Remove high8 from the addend
                    patchTableAddend = patchTableAddend & 0x00FFFFFFFFFFFFFFULL;
                }

                VMOffset finalVMOffset = CacheVMAddress(finalVMAddrWithAddend) - config.layout.cacheBaseAddress;

                auto checkGOTs = ^(CoalescedGOTMap& gotMap, std::vector<std::vector<PatchInfo::GOTInfo>>& gotInfo) {
                    auto gotIt = gotMap.find(fixupVMAddr);
                    if ( gotIt != gotMap.end() ) {
                        dyld_cache_patchable_location patchLoc(gotIt->second, patchTablePMD, patchTableAddend, bindTarget.isWeakImport);
                        auto& gotUses = gotInfo[bindOrdinal];
                        gotUses.emplace_back((PatchInfo::GOTInfo){ patchLoc, finalVMOffset });

                        // NULL out this entry
                        if ( config.layout.is64 ) {
                            fixupLoc->raw64 = 0;
                        } else {
                            fixupLoc->raw32 = 0;
                        }

                        // Tell the slide info emitter to ignore this location
                        this->segments[segIndex].tracker.remove(fixupLoc);
                        return true;
                    }
                    return false;
                };
                if ( checkGOTs(coalescedGOTs, dylibPatchInfo.bindGOTUses) ) {
                    // normal GOT
                } else if ( checkGOTs(coalescedAuthGOTs, dylibPatchInfo.bindAuthGOTUses) ) {
                    // auth GOT
                } else if ( checkGOTs(coalescedAuthPtrs, dylibPatchInfo.bindAuthPtrUses) ) {
                    // auth ptr
                } else {
                    // Location wasn't coalesced.  So add to the regular list of uses
                    dylibPatchInfo.bindUses[bindOrdinal].emplace_back(fixupVMAddr, patchTablePMD, patchTableAddend, bindTarget.isWeakImport);
                    // if target is a function variant, record that dyld may need to update pointer at launch
                    if ( bindTarget.cacheImage.isFunctionVariant ) {
                        uint64_t fvTableVmAddr = 0;
                        uint32_t fvTableVmSize = 0;
                        for ( const LinkeditDataChunk& chunk :  bindTarget.cacheImage.targetDylib->linkeditChunks ) {
                            if ( chunk.isFunctionVariantsTable() ) {
                                fvTableVmAddr = chunk.cacheVMAddress.rawValue();
                                fvTableVmSize = (uint32_t)chunk.cacheVMSize.rawValue();
                                break;
                            }
                        }
                        dyld_cache_function_variant_entry entry;
                        entry.fixupLocVmAddr               = fixupVMAddr.rawValue();
                        entry.functionVariantTableVmAddr   = fvTableVmAddr;
                        entry.functionVariantTableSizeDiv4 = fvTableVmSize/4;
                        entry.dylibHeaderVmAddr            = bindTarget.cacheImage.targetDylib->cacheLoadAddress.rawValue();
                        entry.variantIndex                 = bindTarget.cacheImage.functionVariantTableIndex;
                        entry.pacAuth                      = pmd.authenticated;
                        entry.pacAddress                   = pmd.usesAddrDiversity;
                        entry.pacKey                       = pmd.key;
                        entry.pacDiversity                 = pmd.diversity;
                        entry.targetDylibIndex             = bindTarget.cacheImage.targetDylib->cacheIndex;
                        assert(entry.variantIndex == bindTarget.cacheImage.functionVariantTableIndex);
                        functionVariantsOptimizer.infos.push_back(entry);
                    }
                }
            }
            break;
        }
    }
}

void CacheDylib::bindWithChainedFixups(Diagnostics& diag, const BuilderConfig& config,
                                       CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                                       CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                                       FunctionVariantsOptimizer& functionVariantsOptimizer)
{
    auto fixupHandler = ^(MachOFile::ChainedFixupPointerOnDisk* fixupLoc, uint16_t chainedFormat,
                          uint32_t segIndex, CacheVMAddress fixupVMAddr,
                          bool& stopChain) {
        MachOFile::PointerMetaData pmd(fixupLoc, chainedFormat);
        uint32_t bindOrdinal;
        int64_t  embeddedAddend;
        if ( !fixupLoc->isBind(chainedFormat, bindOrdinal, embeddedAddend) ) {
            // Rebases might be stored in a side table from applying split seg.  If so, we
            // can copy their values in to place now
            if ( config.layout.is64 ) {
                uint64_t targetVMAddr;
                if ( this->segments[segIndex].tracker.hasRebaseTarget64(fixupLoc, &targetVMAddr) ) {
                    // The value is now stored in targetVMAddr.
                    // We'll use it later
                    // We should never get high8 from hasRebaseTarget64()
                    uint64_t high8 = targetVMAddr >> 56;
                    assert(high8 == 0);
                } else {
                    uint64_t runtimeOffset;
                    bool isRebase = fixupLoc->isRebase(chainedFormat, this->cacheLoadAddress.rawValue(),
                                                       runtimeOffset);
                    assert(isRebase);

                    targetVMAddr = this->cacheLoadAddress.rawValue() + runtimeOffset;

                    // Remove high8 if we have it.  The PMD has it too
                    uint64_t high8 = targetVMAddr >> 56;
                    assert(pmd.high8 == high8);
                    targetVMAddr &= 0x00FFFFFFFFFFFFFFULL;
                }

                CacheVMAddress targetCacheAddress(targetVMAddr);
                Fixup::Cache64::setLocation(config.layout.cacheBaseAddress,
                                            fixupLoc, targetCacheAddress,
                                            pmd.high8,
                                            pmd.diversity, pmd.usesAddrDiversity, pmd.key,
                                            pmd.authenticated);
            }
            else {
                uint32_t targetVMAddr;
                assert(this->segments[segIndex].tracker.hasRebaseTarget32(fixupLoc, &targetVMAddr)
                       && "32-bit archs always store target in side table");

                CacheVMAddress targetCacheAddress((uint64_t)targetVMAddr);
                Fixup::Cache32::setLocation(config.layout.cacheBaseAddress,
                                            fixupLoc, targetCacheAddress);
            }
            return;
        }

        if ( bindOrdinal >= this->bindTargets.size() ) {
            diag.error("out of range bind ordinal %d (max %lu)", bindOrdinal, this->bindTargets.size());
            stopChain   = true;
            return;
        }

        const BindTarget& targetInTable = this->bindTargets[bindOrdinal];
        uint64_t          addend        = targetInTable.addend + embeddedAddend;

        this->bindLocation(diag, config, targetInTable, addend, bindOrdinal, segIndex,
                           fixupLoc, fixupVMAddr, pmd,
                           coalescedGOTs, coalescedAuthGOTs, 
                           coalescedAuthPtrs, dylibPatchInfo, functionVariantsOptimizer);
    };

    this->inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        mach_o::Fixups fixups(layout);

        // Use the chained fixups header from the input dylib
        fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
            MachOFile::forEachFixupChainSegment(diag, starts, ^(const dyld_chained_starts_in_segment* segInfo,
                                                                uint32_t segIndex, bool& stopSegment) {

                // We now have the dyld_chained_starts_in_segment from the input dylib, but
                // we want to walk the chain in the cache dylib
                DylibSegmentChunk& segmentInfo = this->segments[segIndex];
                uint8_t* cacheDylibSegment = segmentInfo.subCacheBuffer;

                auto adaptor = ^(MachOFile::ChainedFixupPointerOnDisk* fixupLocation, bool& stop) {
                    uint64_t fixupOffsetInSegment = (uint64_t)fixupLocation - (uint64_t)cacheDylibSegment;
                    CacheVMAddress fixupVMAddr = segmentInfo.cacheVMAddress + VMOffset(fixupOffsetInSegment);
                    fixupHandler(fixupLocation, segInfo->pointer_format,
                                 segIndex, fixupVMAddr, stop);
                    if ( stop )
                        stopSegment = true;
                };

                MachOFile::forEachFixupInSegmentChains(diag, segInfo, false, cacheDylibSegment, adaptor);
            });
        });
    });
}

void CacheDylib::bindWithOpcodeFixups(Diagnostics& diag, const BuilderConfig& config,
                                      CoalescedGOTMap& coalescedGOTs, CoalescedGOTMap& coalescedAuthGOTs,
                                      CoalescedGOTMap& coalescedAuthPtrs, PatchInfo& dylibPatchInfo,
                                      FunctionVariantsOptimizer& functionVariantsOptimizer)
{
    auto handleFixup = ^(uint64_t fixupRuntimeOffset, int bindOrdinal, uint32_t segmentIndex, bool& stopSegment) {
        DylibSegmentChunk& segmentInfo = this->segments[segmentIndex];

        CacheVMAddress fixupVMAddr  = this->cacheLoadAddress + VMOffset(fixupRuntimeOffset);
        VMOffset segmentOffset      = fixupVMAddr - segmentInfo.cacheVMAddress;
        uint8_t* fixupLoc           = segmentInfo.subCacheBuffer + segmentOffset.rawValue();

        if ( bindOrdinal >= this->bindTargets.size() ) {
            diag.error("out of range bind ordinal %d (max %lu)", bindOrdinal, this->bindTargets.size());
            stopSegment   = true;
            return;
        }

        const BindTarget& targetInTable = this->bindTargets[bindOrdinal];
        uint64_t          addend        = targetInTable.addend;

        this->bindLocation(diag, config, targetInTable, addend, bindOrdinal, segmentIndex,
                           (dyld3::MachOFile::ChainedFixupPointerOnDisk*)fixupLoc,
                           fixupVMAddr, dyld3::MachOFile::PointerMetaData(),
                           coalescedGOTs, coalescedAuthGOTs, coalescedAuthPtrs, dylibPatchInfo, functionVariantsOptimizer);
    };

    // Use the fixups from the source dylib
    mach_o::LinkeditLayout linkedit;
    if ( !this->inputMF->getLinkeditLayout(diag, linkedit) ) {
        diag.error("Couldn't get dylib layout");
        return;
    }

    // Use the segment layout from the cache dylib so that VMAddresses are correct
    __block std::vector<mach_o::SegmentLayout> segmentLayout;
    segmentLayout.reserve(this->segments.size());
    for ( const DylibSegmentChunk& dylibSegment : this->segments ) {
        mach_o::SegmentLayout segment;
        segment.vmAddr      = dylibSegment.cacheVMAddress.rawValue();
        segment.vmSize      = dylibSegment.cacheVMSize.rawValue();
        segment.fileOffset  = dylibSegment.subCacheFileOffset.rawValue();
        segment.fileSize    = dylibSegment.subCacheFileSize.rawValue();
        segment.buffer      = dylibSegment.subCacheBuffer;

        segment.kind        = mach_o::SegmentLayout::Kind::unknown;
        if ( dylibSegment.segmentName == "__TEXT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::text;
        } else if ( dylibSegment.segmentName == "__LINKEDIT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
        }
        segmentLayout.push_back(segment);
    }

    // The cache segments don't have the permissions.  Get that from the load commands
    this->cacheHdr->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        segmentLayout[info.segmentIndex].protections = info.initProt;
    });

    mach_o::Layout layout(this->inputMF, { segmentLayout.data(), segmentLayout.data() + segmentLayout.size() }, linkedit);
    mach_o::Fixups fixups(layout);

    fixups.forEachRebaseLocation_Opcodes(diag, ^(uint64_t fixupRuntimeOffset, uint32_t segmentIndex, bool& stop) {
        DylibSegmentChunk& segmentInfo = this->segments[segmentIndex];

        uint64_t fixupCacheVMAddr = layout.textUnslidVMAddr() + fixupRuntimeOffset;
        uint64_t segmentOffset    = fixupCacheVMAddr - segmentInfo.cacheVMAddress.rawValue();
        uint8_t* fixupLoc         = segmentInfo.subCacheBuffer + segmentOffset;

        // Convert from rebase vmAddr to the internal cache format
        if ( config.layout.is64 ) {
            uint64_t targetVMAddr = *(uint64_t*)fixupLoc;
            CacheVMAddress targetCacheAddress(targetVMAddr);

            uint8_t high8 = (uint8_t)(targetVMAddr >> 56);
            if ( high8 != 0 ) {
                // Remove high8 from the vmAddr
                targetVMAddr = targetVMAddr & 0x00FFFFFFFFFFFFFFULL;
            }

            // Unused PointerMetadata, but just use here to get all the fields
            dyld3::MachOFile::PointerMetaData pmd;
            Fixup::Cache64::setLocation(config.layout.cacheBaseAddress,
                                        fixupLoc, CacheVMAddress(targetVMAddr),
                                        high8,
                                        pmd.diversity, pmd.usesAddrDiversity, pmd.key,
                                        pmd.authenticated);
        }
        else {
            uint32_t targetVMAddr = *(uint32_t*)fixupLoc;

            CacheVMAddress targetCacheAddress((uint64_t)targetVMAddr);
            Fixup::Cache32::setLocation(config.layout.cacheBaseAddress,
                                        fixupLoc, CacheVMAddress((uint64_t)targetVMAddr));
        }
    });

    // Do binds after rebases, in case we have lazy binds which override the rebase
    fixups.forEachBindLocation_Opcodes(diag,
        ^(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned int targetIndex, bool& stop) {
            handleFixup(runtimeOffset, targetIndex, segmentIndex, stop);
        },
        ^(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned int overrideBindTargetIndex, bool& stop) {
            assert(this->weakBindTargetsStartIndex.has_value());
            handleFixup(runtimeOffset, this->weakBindTargetsStartIndex.value() + overrideBindTargetIndex, segmentIndex, stop);
        });
}

void CacheDylib::bind(Diagnostics& diag, const BuilderConfig& config, Timer::AggregateTimer& timer,
                      PatchInfo& dylibPatchInfo, FunctionVariantsOptimizer& functionVariantsOptimizer)
{
    Timer::AggregateTimer::Scope timedScope(timer, "dylib bind time");

    // As we are running in parallel, addresses in other dylibs may not have been shifted yet.  We may also
    // race looking at the export trie in a target dylib, while it is being shifted by AdjustDylibSegments.
    // Given that, we'll look at our own cache dylib, but everyone elses input dylib, as those won't mutate

    // Map from where the GOT is located in the dylib to where its located in the coalesced section
    typedef std::unordered_map<const CacheVMAddress, CacheVMAddress, CacheVMAddressHash, CacheVMAddressEqual> CoalescedGOTsMap;
    auto mapGOTs = [](const DylibSectionCoalescer::OptimizedSection& gotSection, std::span<DylibSegmentChunk> dylibSegments,
                      CoalescedGOTsMap& coalescedGOTs) {
        if ( !gotSection.offsetMap.empty() ) {
            uint32_t segmentIndex = gotSection.segmentIndex.value();
            CacheVMAddress dylibGOTBaseVMAddr = dylibSegments[segmentIndex].cacheVMAddress + gotSection.sectionVMOffsetInSegment;
            CacheVMAddress cacheGOTBaseVMAddr = gotSection.subCacheSection->cacheChunk->cacheVMAddress;
            for ( const auto& dylibOffsetAndCacheOffset : gotSection.offsetMap ) {
                VMOffset dylibSectionOffset((uint64_t)dylibOffsetAndCacheOffset.first);
                VMOffset cacheSectionOffset((uint64_t)dylibOffsetAndCacheOffset.second);
                coalescedGOTs[dylibGOTBaseVMAddr + dylibSectionOffset] = cacheGOTBaseVMAddr + cacheSectionOffset;
            }
        }
    };
    CoalescedGOTsMap coalescedGOTs;
    CoalescedGOTsMap coalescedAuthGOTs;
    CoalescedGOTsMap coalescedAuthPtrs;

    mapGOTs(optimizedSections.gots, this->segments, coalescedGOTs);
    mapGOTs(optimizedSections.auth_gots, this->segments, coalescedAuthGOTs);
    mapGOTs(optimizedSections.auth_ptrs, this->segments, coalescedAuthPtrs);

    // Track which locations this dylib uses in other dylibs.  One per bindTarget
    dylibPatchInfo.bindUses.resize(this->bindTargets.size());
    dylibPatchInfo.bindGOTUses.resize(this->bindTargets.size());
    dylibPatchInfo.bindAuthGOTUses.resize(this->bindTargets.size());
    dylibPatchInfo.bindAuthPtrUses.resize(this->bindTargets.size());

    if ( this->inputMF->hasChainedFixups() )
        bindWithChainedFixups(diag, config, coalescedGOTs, coalescedAuthGOTs, coalescedAuthPtrs, dylibPatchInfo, functionVariantsOptimizer);
    else if ( this->inputMF->hasOpcodeFixups() ) {
        bindWithOpcodeFixups(diag, config, coalescedGOTs, coalescedAuthGOTs, coalescedAuthPtrs, dylibPatchInfo, functionVariantsOptimizer);
    } else {
        // Cache dylibs shouldn't use old style fixups.
    }

    // Now that we've bound this dylib, we can tell the ASLRTrackers on the segments to clear
    // any out of band maps
    for ( DylibSegmentChunk& segment : this->segments )
        segment.tracker.clearRebaseTargetsMaps();
}

void CacheDylib::updateObjCSelectorReferences(Diagnostics& diag, const BuilderConfig& config,
                                              Timer::AggregateTimer& timer, ObjCSelectorOptimizer& objcSelectorOptimizer)
{
    if ( !this->inputHdr->hasObjC() )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "dylib updateObjCSelectorReferences time");

    __block objc_visitor::Visitor objcVisitor = this->makeCacheObjCVisitor(config,
                                                                           objcSelectorOptimizer.selectorStringsChunk,
                                                                           nullptr,
                                                                           nullptr);

    // Update every selector reference to point to the canonical selectors
    objcVisitor.forEachSelectorReference(^(metadata_visitor::ResolvedValue& selRefValue) {
        const char* selString = (const char*)objcVisitor.resolveRebase(selRefValue).value();

        // Find the selector in the map
        auto it = objcSelectorOptimizer.selectorsMap.find(selString);
        assert(it != objcSelectorOptimizer.selectorsMap.end());

        VMOffset newSelBufferOffset = it->second;
        assert(newSelBufferOffset.rawValue() < objcSelectorOptimizer.selectorStringsChunk->cacheVMSize.rawValue());
        CacheVMAddress newSelCacheVMAddress = objcSelectorOptimizer.selectorStringsChunk->cacheVMAddress + newSelBufferOffset;

        objcVisitor.updateTargetVMAddress(selRefValue, newSelCacheVMAddress);
    });

    objcVisitor.forEachMethodList(^(objc_visitor::MethodList& objcMethodList,
                                    std::optional<metadata_visitor::ResolvedValue> extendedMethodTypes) {
        // Set both relative and pointer based lists to uniqued.  They will be after this method is done
        objcMethodList.setIsUniqued();

        // Skip uniqing relative method lists.  We know for sure they point to __objc_selrefs which were handled above
        if ( objcMethodList.usesRelativeOffsets() )
            return;

        uint32_t numMethods = objcMethodList.numMethods();
        for ( uint32_t i = 0; i != numMethods; ++i ) {
            objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

            // Get the selector reference which is implicit in the name field of the Method.
            metadata_visitor::ResolvedValue nameRef = objcMethod.getNameField(objcVisitor);

            const char* selString = (const char*)objcVisitor.resolveRebase(nameRef).value();

            // Find the selector in the map
            auto it = objcSelectorOptimizer.selectorsMap.find(selString);
            assert(it != objcSelectorOptimizer.selectorsMap.end());

            VMOffset       newSelBufferOffset   = it->second;
            CacheVMAddress newSelCacheVMAddress = objcSelectorOptimizer.selectorStringsChunk->cacheVMAddress + newSelBufferOffset;

            objcVisitor.updateTargetVMAddress(nameRef, newSelCacheVMAddress);
        }
    });
}

static void sortObjCRelativeMethodList(const BuilderConfig&                         config,
                                       const objc_visitor::Visitor&                 objcVisitor,
                                       const objc_visitor::MethodList&              objcMethodList,
                                       std::optional<metadata_visitor::ResolvedValue>   extendedMethodTypesBase)
{
    uint32_t numMethods = objcMethodList.numMethods();

    // Is this possible?  It simplifies code below, so check it anyway
    if ( numMethods == 0 )
        return;

    // Don't sort if we have a single method
    if ( numMethods == 1 )
        return;

    // At this point we assume we are using offsets directly to selectors.  This
    // is so that the Method struct can also use direct offsets and not track the
    // SEL reference VMAddrs
    assert(objcMethodList.usesOffsetsFromSelectorBuffer());

    // We can't sort relative method lists.  So turn them in to Pointer based lists and sort those instead
    struct Method
    {
        VMAddress selStringVMAddr;
        VMAddress typeStringVMAddr;
        std::optional<VMAddress> impVMAddr;
        VMAddress extendedMethodTypeVMAddr;
    };

    const uint32_t pointerSize = objcVisitor.mf()->pointerSize();

    Method methods[numMethods];
    for ( uint32_t i = 0; i != numMethods; ++i ) {
        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

        methods[i].selStringVMAddr  = objcMethod.getNameVMAddr(objcVisitor);
        methods[i].typeStringVMAddr = objcMethod.getTypesVMAddr(objcVisitor);
        methods[i].impVMAddr        = objcMethod.getIMPVMAddr(objcVisitor);

        if ( extendedMethodTypesBase.has_value() ) {
            const uint8_t* methodTypesBase = (uint8_t*)extendedMethodTypesBase->value();
            methodTypesBase += (pointerSize * i);
            metadata_visitor::ResolvedValue methodType(extendedMethodTypesBase.value(), methodTypesBase);

            // Get the VMAddr pointed to by this method type
            VMAddress targetVMAddr              = objcVisitor.resolveRebase(methodType).vmAddress();
            methods[i].extendedMethodTypeVMAddr = targetVMAddr;
        }
    }

    // Sort by selector address (not contents)
    auto sorter = [](const Method& a, const Method& b) {
        return a.selStringVMAddr < b.selStringVMAddr;
    };

    // Stable sort because method lists can contain duplicates when categories have been attached.
    std::stable_sort(&methods[0], &methods[numMethods], sorter);

    // Replace the relative methods with the sorted ones
    for ( uint32_t i = 0; i != numMethods; ++i ) {
        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

        objcMethod.setName(objcVisitor, methods[i].selStringVMAddr);
        objcMethod.setTypes(objcVisitor, methods[i].typeStringVMAddr);
        objcMethod.setIMP(objcVisitor, methods[i].impVMAddr);

        if ( extendedMethodTypesBase.has_value() ) {
            const uint8_t* methodTypesBase = (uint8_t*)extendedMethodTypesBase->value();
            methodTypesBase += (pointerSize * i);
            metadata_visitor::ResolvedValue methodType(extendedMethodTypesBase.value(), methodTypesBase);

            // Get the VMAddr pointed to by this method type
            VMAddress targetVMAddr = methods[i].extendedMethodTypeVMAddr;
            objcVisitor.updateTargetVMAddress(methodType, CacheVMAddress(targetVMAddr.rawValue()));
        }
    }
}

static void sortObjCPointerMethodList(const BuilderConfig&                          config,
                                      const objc_visitor::Visitor&                  objcVisitor,
                                      const objc_visitor::MethodList&               objcMethodList,
                                      std::optional<metadata_visitor::ResolvedValue>    extendedMethodTypesBase)
{
    uint32_t numMethods = objcMethodList.numMethods();

    // Is this possible?  It simplifies code below, so check it anyway
    if ( numMethods == 0 )
        return;

    // Don't sort if we have a single method
    if ( numMethods == 1 )
        return;

    // It's painful to sort both methods and method types at the same time, so
    // put everything in to a temporary array to sort
    struct Method
    {
        VMAddress selStringVMAddr;
        VMAddress typeStringVMAddr;
        std::optional<VMAddress> impVMAddr;
        VMAddress extendedMethodTypeVMAddr;
    };

    const uint32_t pointerSize = objcVisitor.mf()->pointerSize();

    Method methods[numMethods];
    for ( uint32_t i = 0; i != numMethods; ++i ) {
        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

        methods[i].selStringVMAddr  = objcMethod.getNameVMAddr(objcVisitor);
        methods[i].typeStringVMAddr = objcMethod.getTypesVMAddr(objcVisitor);
        methods[i].impVMAddr        = objcMethod.getIMPVMAddr(objcVisitor);

        if ( extendedMethodTypesBase.has_value() ) {
            const uint8_t* methodTypesBase = (uint8_t*)extendedMethodTypesBase->value();
            methodTypesBase += (pointerSize * i);
            metadata_visitor::ResolvedValue methodType(extendedMethodTypesBase.value(), methodTypesBase);

            // Get the VMAddr pointed to by this method type
            VMAddress targetVMAddr              = objcVisitor.resolveRebase(methodType).vmAddress();
            methods[i].extendedMethodTypeVMAddr = targetVMAddr;
        }
    }

    // Sort by selector address (not contents)
    auto sorter = [](const Method& a, const Method& b) {
        return a.selStringVMAddr < b.selStringVMAddr;
    };

    // Stable sort because method lists can contain duplicates when categories have been attached.
    std::stable_sort(&methods[0], &methods[numMethods], sorter);

    // Replace the methods with the sorted ones
    for ( uint32_t i = 0; i != numMethods; ++i ) {
        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

        objcMethod.setName(objcVisitor, methods[i].selStringVMAddr);
        objcMethod.setTypes(objcVisitor, methods[i].typeStringVMAddr);
        objcMethod.setIMP(objcVisitor, methods[i].impVMAddr);

        if ( extendedMethodTypesBase.has_value() ) {
            const uint8_t* methodTypesBase = (uint8_t*)extendedMethodTypesBase->value();
            methodTypesBase += (pointerSize * i);
            metadata_visitor::ResolvedValue methodType(extendedMethodTypesBase.value(), methodTypesBase);

            // Get the VMAddr pointed to by this method type
            VMAddress targetVMAddr = methods[i].extendedMethodTypeVMAddr;
            objcVisitor.updateTargetVMAddress(methodType, CacheVMAddress(targetVMAddr.rawValue()));
        }
    }
}

void CacheDylib::convertObjCMethodListsToOffsets(Diagnostics& diag, const BuilderConfig& config,
                                                 Timer::AggregateTimer& timer,
                                                 const Chunk*           selectorStringsChunk)
{
    if ( !this->inputHdr->hasObjC() )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "dylib convertObjCMethodListsToOffsets time");

    __block objc_visitor::Visitor objcVisitor = this->makeCacheObjCVisitor(config, selectorStringsChunk, nullptr, nullptr);

    // protocols can be listed multiple times in the _objc_protolist section, so we'll visit them multiple times here
    // We don't want to convert the method list twice, so keep track of all seen method lists
    // FIXME: Remove this once ld removes the duplicates (rdar://133008657)
    __block std::unordered_set<const void*> seenMethodLists;

    objcVisitor.forEachMethodList(^(objc_visitor::MethodList& objcMethodList,
                                    std::optional<metadata_visitor::ResolvedValue> extendedMethodTypes) {
        // Skip pointer based method lists
        if ( !objcMethodList.usesRelativeOffsets() )
            return;

        // Skip method lists we've already converted
        if ( bool inserted = seenMethodLists.insert(objcMethodList.getLocation()).second; !inserted )
            return;

        uint32_t numMethods = objcMethodList.numMethods();
        for ( uint32_t i = 0; i != numMethods; ++i ) {
            objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

            const char* selString = objcMethod.getName(objcVisitor);

            uint64_t nameOffset = (uint64_t)selString - (uint64_t)selectorStringsChunk->subCacheBuffer;
            assert((uint32_t)nameOffset == nameOffset);

            objcMethod.convertNameToOffset(objcVisitor, (uint32_t)nameOffset);
        }

        objcMethodList.setUsesOffsetsFromSelectorBuffer();
    });
}

void CacheDylib::sortObjCMethodLists(Diagnostics& diag, const BuilderConfig& config,
                                     Timer::AggregateTimer& timer,
                                     const Chunk*           selectorStringsChunk)
{
    if ( !this->inputHdr->hasObjC() )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "dylib sortObjCMethodLists time");

    __block objc_visitor::Visitor objcVisitor = this->makeCacheObjCVisitor(config, selectorStringsChunk, nullptr, nullptr);

    objcVisitor.forEachMethodList(^(objc_visitor::MethodList& objcMethodList,
                                    std::optional<metadata_visitor::ResolvedValue> extendedMethodTypes) {
        if ( objcMethodList.usesRelativeOffsets() )
            sortObjCRelativeMethodList(config, objcVisitor, objcMethodList, extendedMethodTypes);
        else
            sortObjCPointerMethodList(config, objcVisitor, objcMethodList, extendedMethodTypes);

        objcMethodList.setIsSorted();
    });
}

void CacheDylib::forEachReferenceToASelRef(Diagnostics &diags,
                                           void (^handler)(uint64_t kind, uint32_t* instrPtr, uint64_t selRefVMAddr)) const
{
    const uint8_t* infoStart = this->inputDylibSplitSegStart;
    const uint8_t* infoEnd = this->inputDylibSplitSegEnd;;

    if ( *infoStart++ != DYLD_CACHE_ADJ_V2_FORMAT ) {
        // Must be split seg v1
        return;
    }

    __block uint32_t textSectionIndex = ~0U;
    __block const uint8_t* textSectionContent = nullptr;
    __block uint32_t selRefSectionIndex = ~0U;
    __block uint64_t selRefSectionVMAddr = 0;
    // The mach_header is section 0
    __block uint32_t sectionIndex = 1;
    this->cacheHdr->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
        if ( (sectInfo.segmentName == "__TEXT" ) && (sectInfo.sectionName == "__text") ) {
            textSectionIndex = sectionIndex;
            VMOffset sectionOffsetInSegment(sectInfo.address - segInfo.vmaddr);
            textSectionContent = this->segments[sectInfo.segIndex].subCacheBuffer;
            textSectionContent += sectionOffsetInSegment.rawValue();
        }
        if ( sectInfo.segmentName.starts_with("__DATA") && (sectInfo.sectionName == "__objc_selrefs") ) {
            selRefSectionIndex = sectionIndex;
            selRefSectionVMAddr = sectInfo.address;
        }
        ++sectionIndex;
    });

    if ( (textSectionIndex == ~0U) || (selRefSectionIndex == ~0U) )
        return;

    // Whole         :== <count> FromToSection+
    // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
    // ToOffset         :== <to-sect-offset-delta> <count> FromOffset+
    // FromOffset     :== <kind> <count> <from-sect-offset-delta>
    const uint8_t* p = infoStart;
    uint64_t sectionCount = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
    for (uint64_t i=0; i < sectionCount; ++i) {
        uint64_t fromSectionIndex = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
        uint64_t toSectionIndex = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
        uint64_t toOffsetCount = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
        uint64_t toSectionOffset = 0;
        for (uint64_t j=0; j < toOffsetCount; ++j) {
            uint64_t toSectionDelta = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
            uint64_t fromOffsetCount = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
            toSectionOffset += toSectionDelta;
            for (uint64_t k=0; k < fromOffsetCount; ++k) {
                uint64_t kind = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
                if ( kind > 13 ) {
                    diags.error("bad kind (%llu) value in %s\n", kind, installName.data());
                }
                uint64_t fromSectDeltaCount = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
                uint64_t fromSectionOffset = 0;
                for (uint64_t l=0; l < fromSectDeltaCount; ++l) {
                    uint64_t delta = dyld3::MachOFile::read_uleb128(diags, p, infoEnd);
                    fromSectionOffset += delta;
                    if ( (fromSectionIndex == textSectionIndex) && (toSectionIndex == selRefSectionIndex) ) {
                        uint32_t* instrPtr = (uint32_t*)(textSectionContent + fromSectionOffset);
                        uint64_t targetVMAddr = selRefSectionVMAddr + toSectionOffset;
                        handler(kind, instrPtr, targetVMAddr);
                    }
                }
            }
        }
    }
}

void CacheDylib::optimizeLoadsFromConstants(const BuilderConfig& config,
                                            Timer::AggregateTimer& timer,
                                            const ObjCStringsChunk* selectorStringsChunk)
{
    const bool logSelectors = config.log.printDebug;

    Timer::AggregateTimer::Scope timedScope(timer, "dylib optimizeLoadsFromConstants time");

    if ( !this->cacheHdr->is64() )
        return;

    __block const uint8_t* textSectionContent = nullptr;
    __block CacheVMAddress textSectionVMAddr;
    __block const uint8_t* selRefSectionContent = nullptr;
    __block CacheVMAddress selRefSectionVMAddr;
    this->cacheHdr->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
        VMOffset sectionOffsetInSegment(sectInfo.address - segInfo.vmaddr);
        if ( ( sectInfo.segmentName == "__TEXT" ) && (sectInfo.sectionName == "__text") ) {
            textSectionContent = this->segments[sectInfo.segIndex].subCacheBuffer;
            textSectionContent += sectionOffsetInSegment.rawValue();

            textSectionVMAddr = CacheVMAddress(sectInfo.address);
        }
        if ( sectInfo.segmentName.starts_with("__DATA") && (sectInfo.sectionName == "__objc_selrefs") ) {
            selRefSectionContent = this->segments[sectInfo.segIndex].subCacheBuffer;
            selRefSectionContent += sectionOffsetInSegment.rawValue();

            selRefSectionVMAddr = CacheVMAddress(sectInfo.address);
        }
    });

    __block std::unordered_map<uint64_t, std::set<void*>> lohTracker;
    Diagnostics diag;
    this->forEachReferenceToASelRef(diag, ^(uint64_t kind, uint32_t *instrPtr, uint64_t selRefVMAddr) {
        if ( (kind == DYLD_CACHE_ADJ_V2_ARM64_ADRP) || (kind == DYLD_CACHE_ADJ_V2_ARM64_OFF12) ) {
            lohTracker[selRefVMAddr].insert(instrPtr);
        }
    });

    if ( lohTracker.empty() )
        return;

    uint64_t lohADRPCount = 0;
    uint64_t lohLDRCount = 0;

    CacheVMAddress selectorStringsStart = selectorStringsChunk->cacheVMAddress;
    CacheVMAddress selectorStringsEnd = selectorStringsStart + selectorStringsChunk->cacheVMSize;

    for (auto& targetAndInstructions : lohTracker) {
        CacheVMAddress selRefVMAddr(targetAndInstructions.first);
        std::set<void*>& instructions = targetAndInstructions.second;

        VMOffset selRefSectionOffset = selRefVMAddr - selRefSectionVMAddr;
        const void* selRefContent = selRefSectionContent + selRefSectionOffset.rawValue();

        // Load the selector and make sure its in the selector strings chunk
        CacheVMAddress selStringVMAddr = Fixup::Cache64::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress,
                                                                                       selRefContent);
        const char* selectorString = nullptr;
        if ( (selStringVMAddr >= selectorStringsStart) && (selStringVMAddr < selectorStringsEnd) ) {
            VMOffset stringOffset = selStringVMAddr - selectorStringsStart;
            selectorString = (const char*)selectorStringsChunk->subCacheBuffer + stringOffset.rawValue();
        } else {
            // This selRef doesn't point to the strings chunk, so skip it
            instructions.clear();
            continue;
        }

        // We do 2 passes over the instructions.  The first to validate them and the second
        // to actually update them.
        for (unsigned pass = 0; pass != 2; ++pass) {
            uint32_t adrpCount = 0;
            uint32_t ldrCount = 0;
            for (void* instructionAddress : instructions) {
                uint32_t& instruction = *(uint32_t*)instructionAddress;
                VMOffset instructionSectionOffset((uint64_t)instructionAddress - (uint64_t)textSectionContent);
                CacheVMAddress instructionVMAddr = textSectionVMAddr + instructionSectionOffset;

                if ( (instruction & 0x9F000000) == 0x90000000 ) {
                    // ADRP
                    int64_t pageDistance = ((selStringVMAddr.rawValue() & ~0xFFF) - (instructionVMAddr.rawValue() & ~0xFFF));
                    int64_t newPage21 = pageDistance >> 12;

                    if (pass == 0) {
                        if ( (newPage21 > 2097151) || (newPage21 < -2097151) ) {
                            if (logSelectors)
                                fprintf(stderr, "Out of bounds ADRP selector reference target\n");
                            instructions.clear();
                            break;
                        }
                        ++adrpCount;
                    }

                    if (pass == 1) {
                        instruction = (instruction & 0x9F00001F) | ((newPage21 << 29) & 0x60000000) | ((newPage21 << 3) & 0x00FFFFE0);
                        ++lohADRPCount;
                    }
                    continue;
                }

                if ( (instruction & 0x3B000000) == 0x39000000 ) {
                    // LDR/STR.  STR shouldn't be possible as this is a selref!
                    if (pass == 0) {
                        if ( (instruction & 0xC0C00000) != 0xC0400000 ) {
                            // Not a load, or dest reg isn't xN, or uses sign extension
                            if (logSelectors)
                                fprintf(stderr, "Bad LDR for selector reference optimisation\n");
                            instructions.clear();
                            break;
                        }
                        if ( (instruction & 0x04000000) != 0 ) {
                            // Loading a float
                            if (logSelectors)
                                fprintf(stderr, "Bad LDR for selector reference optimisation\n");
                            instructions.clear();
                            break;
                        }
                        ++ldrCount;
                    }

                    if (pass == 1) {
                        uint32_t ldrDestReg = (instruction & 0x1F);
                        uint32_t ldrBaseReg = ((instruction >> 5) & 0x1F);

                        // Convert the LDR to an ADD
                        instruction = 0x91000000;
                        instruction |= ldrDestReg;
                        instruction |= ldrBaseReg << 5;
                        instruction |= (selStringVMAddr.rawValue() & 0xFFF) << 10;

                        ++lohLDRCount;
                    }
                    continue;
                }

                if ( (instruction & 0xFFC00000) == 0x91000000 ) {
                    // ADD imm12
                    // We don't support ADDs.
                    if (logSelectors)
                        fprintf(stderr, "Bad ADD for selector reference optimisation\n");
                    instructions.clear();
                    break;
                }

                if (logSelectors)
                    fprintf(stderr, "Unknown instruction for selref optimisation\n");
                instructions.clear();
                break;
            }
            if (pass == 0) {
                // If we didn't see at least one ADRP/LDR in pass one then don't optimize this location
                if ((adrpCount == 0) || (ldrCount == 0)) {
                    instructions.clear();
                    break;
                }
            }
        }
    }

    if ( logSelectors ) {
        config.log.log("  Optimized %lld ADRP LOHs\n", lohADRPCount);
        config.log.log("  Optimized %lld LDR LOHs\n", lohLDRCount);
    }
}

Error CacheDylib::setObjCImpCachesPointers(const BuilderConfig& config,
                                           const ObjCIMPCachesOptimizer& objcIMPCachesOptimizer,
                                           const ObjCStringsChunk* selectorStringsChunk)
{
    if ( this->installName != "/usr/lib/libobjc.A.dylib" )
        return Error();

    Diagnostics diag;

    // New libobjc's have a magic symbol for the offsets
    std::string_view symbolName = objcIMPCachesOptimizer.sharedCacheOffsetsSymbolName;
    std::optional<BindTargetAndName> bindTargetAndName;
    bindTargetAndName = this->hasExportedSymbol(diag, symbolName.data(), SearchMode::onlySelf);
    if ( diag.hasError() )
        return Error("Couldn't build IMP caches because: %s", diag.errorMessageCStr());

    if ( !bindTargetAndName )
        return Error("Couldn't build IMP caches because: couldn't find imp caches symbol");

    BindTarget& bindTarget = bindTargetAndName->first;
    if ( bindTarget.kind != BindTarget::Kind::inputImage )
        return Error("Couldn't build IMP caches because: symbol is wrong kind");

    BindTarget::InputImage inputImage        = bindTarget.inputImage;
    InputDylibVMAddress    targetInputVMAddr = inputImage.targetDylib->inputLoadAddress + inputImage.targetRuntimeOffset;
    CacheVMAddress         targetCacheVMAddr = inputImage.targetDylib->adjustor->adjustVMAddr(targetInputVMAddr);

    // Find the segment for the content
    for ( DylibSegmentChunk& segment : this->segments ) {
        if ( targetCacheVMAddr < segment.cacheVMAddress )
            continue;
        if ( targetCacheVMAddr >= (segment.cacheVMAddress + segment.cacheVMSize) )
            continue;

        VMOffset offsetInSegment = targetCacheVMAddr - segment.cacheVMAddress;
        uint8_t* content = segment.subCacheBuffer + offsetInSegment.rawValue();

        // Section looks like
        // struct objc_opt_imp_caches_pointerlist_tt {
        //     T selectorStringVMAddrStart;
        //     T selectorStringVMAddrEnd;
        //     T inlinedSelectorsVMAddrStart;
        //     T inlinedSelectorsVMAddrEnd;
        // };

        CacheVMAddress selectorStringStartVMAddr = selectorStringsChunk->cacheVMAddress;
        CacheVMAddress selectorStringEndVMAddr = selectorStringStartVMAddr + selectorStringsChunk->cacheVMSize;
        if ( config.layout.is64 ) {
            uint8_t* selectorStringStart = content;
            uint8_t* selectorStringEnd = content + 8;

            dyld3::MachOFile::PointerMetaData pmd;
            Fixup::Cache64::setLocation(config.layout.cacheBaseAddress,
                                        selectorStringStart, selectorStringStartVMAddr,
                                        pmd.high8, pmd.diversity, pmd.usesAddrDiversity,
                                        pmd.key, pmd.authenticated);
            Fixup::Cache64::setLocation(config.layout.cacheBaseAddress,
                                        selectorStringEnd, selectorStringEndVMAddr,
                                        pmd.high8, pmd.diversity, pmd.usesAddrDiversity,
                                        pmd.key, pmd.authenticated);

            segment.tracker.add(selectorStringStart);
            segment.tracker.add(selectorStringEnd);
        } else {
            uint8_t* selectorStringStart = content;
            uint8_t* selectorStringEnd = content + 4;

            dyld3::MachOFile::PointerMetaData pmd;
            Fixup::Cache32::setLocation(config.layout.cacheBaseAddress,
                                        selectorStringStart, selectorStringStartVMAddr);
            Fixup::Cache32::setLocation(config.layout.cacheBaseAddress,
                                        selectorStringStart, selectorStringEndVMAddr);

            segment.tracker.add(selectorStringStart);
            segment.tracker.add(selectorStringEnd);
        }

        return Error();
    }

    return Error("Couldn't build IMP caches because: couldn't find section for imp caches symbol");
}

Error CacheDylib::emitObjCIMPCaches(const BuilderConfig& config, Timer::AggregateTimer& timer,
                                    const ObjCIMPCachesOptimizer& objcIMPCachesOptimizer,
                                    const ObjCStringsChunk* selectorStringsChunk)
{
    if ( !objcIMPCachesOptimizer.builder )
        return Error();

    const bool log = config.log.printDebug;

    Timer::AggregateTimer::Scope timedScope(timer, "emitObjCIMPCaches time");

    const ObjCIMPCachesOptimizer::IMPCacheMap& dylibIMPCaches = objcIMPCachesOptimizer.dylibIMPCaches[this->cacheIndex];

    // libobjc needs to know about some offsets, even if it didn't get IMP caches itself
    Error pointersErr = this->setObjCImpCachesPointers(config, objcIMPCachesOptimizer,
                                                       selectorStringsChunk);
    if ( pointersErr.hasError() )
        return pointersErr;

    // Skip dylibs without chained fixups.  This simplifies binding superclasses across dylibs
    if ( !this->inputMF->hasChainedFixupsLoadCommand() )
        return Error();

    __block objc_visitor::Visitor objcVisitor = this->makeCacheObjCVisitor(config, nullptr, nullptr, nullptr);

    // Walk the classes in this dylib, and see if any have an IMP cache
    objcVisitor.forEachClassAndMetaClass(^(objc_visitor::Class& objcClass, bool& stopClass) {
        const ObjCIMPCachesOptimizer::ClassKey classKey = { objcClass.getName(objcVisitor), objcClass.isMetaClass };
        auto it = dylibIMPCaches.find(classKey);
        if ( it == dylibIMPCaches.end() ) {
            // No IMP cache for this dylib
            return;
        }

        // Get the cache we are going to emit
        const imp_caches::IMPCache& impCache = it->second.first;

        // Get the offset in the IMPCache buffer for this IMP cache
        VMOffset impCacheOffset = it->second.second;

        // Skip dylibs where the "vtable" address is set
        if ( objcClass.getMethodCachePropertiesVMAddr(objcVisitor).has_value() )
            return;

        MachOFile::PointerMetaData PMD;
        if ( config.layout.hasAuthRegion && (objcIMPCachesOptimizer.libobjcImpCachesVersion >= 4) ) {
            PMD.diversity         = 0x9cff; // hash of "originalPreoptCache"
            PMD.high8             = 0;
            PMD.authenticated     = 1;
            PMD.key               = 2;     // DA
            PMD.usesAddrDiversity = 1;
        }

        // Set the "vtable" to point to the cache
        CacheVMAddress impCacheVMAddr = objcIMPCachesOptimizer.impCachesChunk->cacheVMAddress + impCacheOffset;
        objcClass.setMethodCachePropertiesVMAddr(objcVisitor, VMAddress(impCacheVMAddr.rawValue()), PMD);

        // Tell the slide info emitter to slide this location
        metadata_visitor::ResolvedValue vtableField = objcClass.getMethodCachePropertiesField(objcVisitor);
        this->segments[vtableField.segmentIndex()].tracker.add(vtableField.value());

        // TODO: This is where we could check the version if needed.  For now we know objc
        // is new enough for the V2 format.
        uint8_t* impCachePos = objcIMPCachesOptimizer.impCachesChunk->subCacheBuffer;
        impCachePos += impCacheOffset.rawValue();

        // Convert from VMAddress to CacheVMAddress as the objc visitor uses VMAddress internally
        CacheVMAddress classVMAddr = CacheVMAddress(objcClass.getVMAddress().rawValue());

        ImpCacheHeader_v2* impCacheHeader = (ImpCacheHeader_v2*)impCachePos;
        VMOffset fallbackOffset;
        if ( impCache.fallback_class.has_value() ) {
            auto classIt = objcIMPCachesOptimizer.classMap.find(impCache.fallback_class.value());
            assert(classIt != objcIMPCachesOptimizer.classMap.end());
            const ObjCIMPCachesOptimizer::InputDylibLocation& inputDylibClass = classIt->second;

            CacheVMAddress superclassVMAddr = inputDylibClass.first->adjustor->adjustVMAddr(inputDylibClass.second);
            fallbackOffset = superclassVMAddr - classVMAddr;
        } else {
            // The default fallback class is the superclass
            VMAddress superclassVMAddr(0ULL);
            std::optional<VMAddress> optionalSuperclassVMAddr = objcClass.getSuperclassVMAddr(objcVisitor);
            if ( optionalSuperclassVMAddr.has_value() )
                superclassVMAddr = optionalSuperclassVMAddr.value();

            fallbackOffset = superclassVMAddr - VMAddress(classVMAddr.rawValue());
        }

        impCacheHeader->fallback_class_offset = fallbackOffset.rawValue();
        impCacheHeader->cache_shift = impCache.cache_shift;
        impCacheHeader->cache_mask = impCache.cache_mask;
        impCacheHeader->occupied = impCache.occupied;
        impCacheHeader->has_inlines = impCache.has_inlines;
        impCacheHeader->padding = impCache.padding;
        impCacheHeader->unused = impCache.unused;
        impCacheHeader->bit_one = impCache.bit_one;

        // Emit the buckets
        uint8_t* firstBucketPos = impCachePos + sizeof(*impCacheHeader);
        ImpCacheEntry_v2* currentBucket = (ImpCacheEntry_v2*)firstBucketPos;
        for ( const imp_caches::Bucket& bucket : impCache.buckets ) {
            if ( bucket.isEmptyBucket ) {
                currentBucket->selOffset = 0x3FFFFFF;
                currentBucket->impOffset = 0;
            } else {
                imp_caches::BucketMethod bucketMethod = {
                    .installName = bucket.installName,
                    .className = bucket.className,
                    .methodName = bucket.methodName,
                    .isInstanceMethod = bucket.isInstanceMethod
                };
                auto bucketIt = objcIMPCachesOptimizer.methodMap.find(bucketMethod);
                assert(bucketIt != objcIMPCachesOptimizer.methodMap.end());

                const ObjCIMPCachesOptimizer::InputDylibLocation& bucketInputLocation = bucketIt->second;
                CacheVMAddress methodVMAddr = bucketInputLocation.first->adjustor->adjustVMAddr(bucketInputLocation.second);
                VMOffset impVMOffset = classVMAddr - methodVMAddr;

                int64_t selOffset = (int64_t)bucket.selOffset;
                int64_t impOffset = (int64_t)impVMOffset.rawValue();

                assert(impOffset % 4 == 0); // dest and source should be aligned
                impOffset >>= 2;
                // objc assumes the imp offset always has
                // its two bottom bits set to 0, this lets us have
                // 4x more reach

                assert(impOffset < 1ll << 39);
                assert(-impOffset < 1ll << 39);
                assert(selOffset < 0x4000000);
                currentBucket->selOffset = selOffset;
                currentBucket->impOffset = impOffset;

                if ( log ) {
                    const uint8_t* selString = selectorStringsChunk->subCacheBuffer + currentBucket->selOffset;
                    uint64_t bucketIndex = currentBucket - (ImpCacheEntry_v2*)firstBucketPos;
                    config.log.log("[IMP Caches] Coder[%lld]: %#08llx (sel: %#08llx, imp %#08llx) %s\n",
                                   bucketIndex, methodVMAddr.rawValue(),
                                   selOffset, impOffset, (const char*)selString);
                }
            }
            ++currentBucket;
        }
    });

    return Error();
}

// This dylib may have uniqued GOTs.  This returns a map from the address of the uniqued GOT
// to the target of that GOT.
CacheDylib::GOTToTargetMap CacheDylib::getUniquedGOTTargets(const PatchInfo& dylibPatchInfo) const
{
    CacheDylib::GOTToTargetMap gotToTargetMap;

    for ( UniquedGOTKind sectionKind : { UniquedGOTKind::regular, UniquedGOTKind::authGot, UniquedGOTKind::authPtr } ) {
        std::span<const std::vector<PatchInfo::GOTInfo>> bindGOTUses;
        switch ( sectionKind ) {
            case UniquedGOTKind::regular:
                bindGOTUses = dylibPatchInfo.bindGOTUses;
                break;
            case UniquedGOTKind::authGot:
                bindGOTUses = dylibPatchInfo.bindAuthGOTUses;
                break;
            case UniquedGOTKind::authPtr:
                bindGOTUses = dylibPatchInfo.bindAuthPtrUses;
                break;
        }

        assert(this->bindTargets.size() == bindGOTUses.size());
        for ( uint32_t bindIndex = 0; bindIndex != this->bindTargets.size(); ++bindIndex ) {
            const BindTarget& bindTarget = this->bindTargets[bindIndex];

            // Skip binds with no uses
            const std::vector<PatchInfo::GOTInfo>& clientUses = bindGOTUses[bindIndex];
            if ( clientUses.empty() )
                continue;

            // Skip absolute binds.  Perhaps we should track these, but we lost the information to patch them
            if ( bindTarget.kind == CacheDylib::BindTarget::Kind::absolute )
                continue;

            assert(bindTarget.kind == BindTarget::Kind::cacheImage);
            const BindTarget::CacheImage& cacheImageTarget = bindTarget.cacheImage;
            CacheVMAddress bindTargetVMAddr = cacheImageTarget.targetDylib->cacheLoadAddress + cacheImageTarget.targetRuntimeOffset;


            for ( const PatchInfo::GOTInfo& gotInfo : clientUses ) {
                gotToTargetMap[gotInfo.patchInfo.cacheVMAddr] = bindTargetVMAddr;
            }
        }
    }

    return gotToTargetMap;
}

CacheDylib::OldToNewStubMap CacheDylib::buildStubMaps(const BuilderConfig& config,
                                                      const StubOptimizer& stubOptimizer,
                                                      const PatchInfo& dylibPatchInfo)
{
    __block OldToNewStubMap oldToNewStubMap;

    __block Diagnostics diag;
    __block uint32_t stubsLeftInterposable = 0;

    // Find all the indirect symbol names from the source dylib
    // Record all the indirect symbols
    __block std::vector<std::string_view> indirectSymbols;
    this->inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
        mach_o::SymbolTable symbols(layout);

        indirectSymbols.reserve(layout.linkedit.indirectSymbolTable.entryCount);

        symbols.forEachIndirectSymbol(diag, ^(const char* symbolName, uint32_t symNum) {
            indirectSymbols.push_back(symbolName);
        });
    });
    diag.assertNoError();

    GOTToTargetMap uniquedGOTMap = getUniquedGOTTargets(dylibPatchInfo);

    // GOTs may have been optimized.  We'll either end up in a GOT or auth GOT, depending on arch
    __block metadata_visitor::Visitor visitor = this->makeCacheVisitor(config);

    // Get the target of the GOT.  It might be uniqued so look there too
    auto getGOTTarget = ^(uint64_t targetLPAddr) {
        std::optional<VMAddress> targetVMAddr;

        CacheVMAddress gotCacheVMAddr(targetLPAddr);
        VMAddress gotVMAddr(targetLPAddr);
        if ( auto it = uniquedGOTMap.find(gotCacheVMAddr); it != uniquedGOTMap.end() ) {
            targetVMAddr = VMAddress(it->second.rawValue());
        } else {
            metadata_visitor::ResolvedValue gotValue = visitor.getValueFor(gotVMAddr);
            targetVMAddr = visitor.resolveOptionalRebaseToVMAddress(gotValue);
        }

        return targetVMAddr;
    };

    // Walk all the stubs in the stubs sections
    this->cacheHdr->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
        unsigned sectionType = (sectInfo.flags & SECTION_TYPE);
        if ( sectionType != S_SYMBOL_STUBS )
            return;

        // We can only optimize certain stubs sections, depending on the arch
        if ( sectInfo.sectionName != this->developmentStubs.sectionName )
            return;
        if ( sectInfo.segmentName != this->developmentStubs.segmentName )
            return;

        // reserved1/reserved2 tell us how large stubs are, and our offset in to the symbol table
        const uint64_t indirectTableOffset = sectInfo.reserved1;
        const uint64_t stubsSize = sectInfo.reserved2;
        const uint64_t stubsCount = sectInfo.size / stubsSize;

        CacheVMAddress stubsSectionBaseAddress(sectInfo.address);

        // Work out where the stub buffer is in the cache
        const DylibSegmentChunk& segment = this->segments[segInfo.segmentIndex];
        CacheVMAddress segmentBaseAddress = segment.cacheVMAddress;
        VMOffset sectionOffsetInSegment = stubsSectionBaseAddress - segmentBaseAddress;
        const uint8_t* sectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();

        for ( uint64_t stubIndex = 0; stubIndex != stubsCount; ++stubIndex ) {
            uint64_t stubOffset = stubsSize * stubIndex;
            CacheVMAddress oldStubVMAddr = stubsSectionBaseAddress + CacheVMSize(stubOffset);
            CacheVMAddress newStubVMAddr = this->developmentStubs.cacheVMAddress + VMOffset(stubOffset);
            const uint8_t* stubInstrs = sectionBuffer + stubOffset;

            uint64_t symbolIndex = indirectTableOffset + stubIndex;
            if ( symbolIndex >= indirectSymbolTable.size() ) {
                diag.warning("Symbol index (%lld) exceeds length of symbol table (%lld)",
                             symbolIndex, (uint64_t)indirectSymbolTable.size());
                continue;
            }

            std::string_view symName = indirectSymbols[symbolIndex];
            if ( stubOptimizer.neverStubEliminate.count(symName) ) {
                stubsLeftInterposable++;
                continue;
            }

            if ( this->cacheHdr->isArch("arm64") ) {
                uint64_t targetLPAddr = StubOptimizer::gotAddrFromArm64Stub(diag, this->installName,
                                                                            stubInstrs,
                                                                            oldStubVMAddr.rawValue());

                if ( targetLPAddr == 0 )
                    continue;

                std::optional<VMAddress> gotTargetVMAddr = getGOTTarget(targetLPAddr);
                if ( !gotTargetVMAddr.has_value() )
                    continue;

                // Track the stub for later
                oldToNewStubMap[oldStubVMAddr] = newStubVMAddr;

                // Emit this stub in to the stub islands for this dylib
                {
                    // Dev stub
                    uint8_t* newStubBuffer = developmentStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64StubToGOT(newStubBuffer, newStubVMAddr.rawValue(),
                                                          targetLPAddr);
                }
                {
                    // Customer stub
                    uint8_t* newStubBuffer = customerStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64StubTo(newStubBuffer, newStubVMAddr.rawValue(),
                                                       gotTargetVMAddr->rawValue());
                }
            } else if ( this->cacheHdr->isArch("arm64e") ) {
                uint64_t targetLPAddr = StubOptimizer::gotAddrFromArm64eStub(diag, this->installName,
                                                                             stubInstrs,
                                                                             oldStubVMAddr.rawValue());

                if ( targetLPAddr == 0 )
                    continue;

                std::optional<VMAddress> gotTargetVMAddr = getGOTTarget(targetLPAddr);
                if ( !gotTargetVMAddr.has_value() )
                    continue;

                // Track the stub for later
                oldToNewStubMap[oldStubVMAddr] = newStubVMAddr;

                // Emit this stub in to the stub islands for this dylib
                {
                    // Dev stub
                    uint8_t* newStubBuffer = developmentStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64eStubToGOT(newStubBuffer, newStubVMAddr.rawValue(),
                                                           targetLPAddr);
                }
                {
                    // Customer stub
                    uint8_t* newStubBuffer = customerStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64eStubTo(newStubBuffer, newStubVMAddr.rawValue(),
                                                        gotTargetVMAddr->rawValue());
                }
            } else if ( this->cacheHdr->isArch("arm64_32") ) {
                uint64_t targetLPAddr = StubOptimizer::gotAddrFromArm64_32Stub(diag, this->installName,
                                                                               stubInstrs,
                                                                               oldStubVMAddr.rawValue());

                if ( targetLPAddr == 0 )
                    continue;

                std::optional<VMAddress> gotTargetVMAddr = getGOTTarget(targetLPAddr);
                if ( !gotTargetVMAddr.has_value() )
                    continue;

                // Track the stub for later
                oldToNewStubMap[oldStubVMAddr] = newStubVMAddr;

                // Emit this stub in to the stub islands for this dylib
                {
                    // Dev stub
                    uint8_t* newStubBuffer = developmentStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64_32StubToGOT(newStubBuffer, newStubVMAddr.rawValue(),
                                                             targetLPAddr);
                }
                {
                    // Customer stub
                    uint8_t* newStubBuffer = customerStubs.subCacheBuffer + stubOffset;
                    StubOptimizer::generateArm64_32StubTo(newStubBuffer, newStubVMAddr.rawValue(),
                                                          gotTargetVMAddr->rawValue());
                }
            } else {
                // Unknown arch
                assert(0);
            }
        }
    });

    return oldToNewStubMap;
}

void CacheDylib::forEachCallSiteToAStub(Diagnostics& diag, const CallSiteHandler handler)
{
    // Get the section layout and split seg info from the source dylib
    __block uint64_t textSectionIndex = ~0U;
    __block uint64_t stubSectionIndex = ~0U;
    __block uint8_t* textSectionBuffer = nullptr;
    __block uint64_t textSectionVMAddr = ~0ULL;
    __block uint64_t stubSectionVMAddr = ~0ULL;

    // Find the sections
    {
        // Section #0 is the mach_header
        __block uint32_t sectionIndex = 1;
        this->cacheHdr->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
            if ( sectInfo.segmentName == "__TEXT" ) {
                if ( sectInfo.sectionName == "__text" ) {
                    textSectionIndex = sectionIndex;
                    textSectionVMAddr = sectInfo.address;

                    // Work out the buffer for the text section
                    const DylibSegmentChunk& segment = this->segments[segInfo.segmentIndex];
                    CacheVMAddress segmentBaseAddress = segment.cacheVMAddress;
                    CacheVMAddress sectionBaseAddress(sectInfo.address);
                    VMOffset sectionOffsetInSegment = sectionBaseAddress - segmentBaseAddress;
                    textSectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();
                } else if ( sectInfo.sectionName == "__stubs" ) {
                    // On arm64e devices, we ignore __stubs and only handle __auth_stubs
                    if ( !this->cacheHdr->isArch("arm64e") ) {
                        stubSectionIndex = sectionIndex;
                        stubSectionVMAddr = sectInfo.address;
                    }
                } else if ( sectInfo.sectionName == "__auth_stubs" ) {
                    // On arm64e devices, we ignore __stubs and only handle __auth_stubs
                    if ( this->cacheHdr->isArch("arm64e") ) {
                        stubSectionIndex = sectionIndex;
                        stubSectionVMAddr = sectInfo.address;
                    }
                }
            }
            ++sectionIndex;
        });
    }

    if ( textSectionIndex == ~0U )
        return;
    if ( stubSectionIndex == ~0U )
        return;

    this->inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
        const uint8_t* infoStart = layout.linkedit.splitSegInfo.buffer;
        const uint8_t* infoEnd   = infoStart + layout.linkedit.splitSegInfo.bufferSize;
        if ( *infoStart++ != DYLD_CACHE_ADJ_V2_FORMAT ) {
            diag.error("malformed split seg info in %s", this->installName.data());
            return;
        }

        // Whole         :== <count> FromToSection+
        // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
        // ToOffset      :== <to-sect-offset-delta> <count> FromOffset+
        // FromOffset    :== <kind> <count> <from-sect-offset-delta>
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
                        diag.error("bad kind (%llu) value in %s\n", kind, this->installName.data());
                        return;
                    }
                    uint64_t fromSectDeltaCount = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
                    uint64_t fromSectionOffset = 0;
                    for (uint64_t l=0; l < fromSectDeltaCount; ++l) {
                        uint64_t delta = dyld3::MachOFile::read_uleb128(diag, p, infoEnd);
                        fromSectionOffset += delta;
                        if ( (fromSectionIndex == textSectionIndex) && (toSectionIndex == stubSectionIndex) ) {
                            uint32_t* instrPtr = (uint32_t*)(textSectionBuffer + fromSectionOffset);
                            uint64_t instrAddr = textSectionVMAddr + fromSectionOffset;
                            uint64_t stubAddr = stubSectionVMAddr + toSectionOffset;
                            uint32_t instruction = *instrPtr;
                            if ( handler(kind, instrAddr, stubAddr, instruction) ) {
                                *instrPtr = instruction;
                            }
                        }
                    }
                }
            }
        }
    });
}

// In a universal cache, dylibs should not longer use their own __stubs, but instead redirect to a stubs
// subCache.  There will be 1 stubs cache for customer and another for development
void CacheDylib::optimizeStubs(const BuilderOptions& options, const BuilderConfig& config,
                               Timer::AggregateTimer& timer,
                               const StubOptimizer& stubOptimizer,
                               const PatchInfo& dylibPatchInfo)
{
    if ( options.kind != CacheKind::universal )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "optimizeStubs time");

    OldToNewStubMap oldToNewStubMap = this->buildStubMaps(config, stubOptimizer, dylibPatchInfo);

    __block Diagnostics diag;

    // Walk the split seg info from the input dylib, as its been removed from the cache dylib
    this->forEachCallSiteToAStub(diag, ^(uint8_t kind, uint64_t callSiteAddr, uint64_t stubAddr,
                                         uint32_t& instruction) {
        if ( kind != DYLD_CACHE_ADJ_V2_ARM64_BR26 )
            return false;
        // skip all but BL or B
        if ( (instruction & 0x7C000000) != 0x14000000 )
            return false;
        // compute target of branch instruction
        int32_t brDelta = (instruction & 0x03FFFFFF) << 2;
        if ( brDelta & 0x08000000 )
            brDelta |= 0xF0000000;
        uint64_t targetAddr = callSiteAddr + (int64_t)brDelta;
        if ( targetAddr != stubAddr ) {
            diag.warning("stub target mismatch");
            return false;
        }

        // ignore branch if not to a stub we want to optimize
        CacheVMAddress oldStubAddr(stubAddr);
        auto it = oldToNewStubMap.find(oldStubAddr);
        if ( it == oldToNewStubMap.end() )
            return false;

        CacheVMAddress newStubAddr = it->second;

        int64_t deltaToNewStub = newStubAddr.rawValue() - callSiteAddr;
        static const int64_t b128MegLimit = 0x07FFFFFF;
        if ( (deltaToNewStub <= -b128MegLimit) || (deltaToNewStub >= b128MegLimit) ) {
            diag.error("%s call could not reach stub island at offset 0x%llx",
                       this->installName.data(), deltaToNewStub);
            return false;
        }

        instruction = (instruction & 0xFC000000) | ((deltaToNewStub >> 2) & 0x03FFFFFF);
        return true;
    });
}

void CacheDylib::fipsSign(Timer::AggregateTimer& timer)
{
    // We only need corecrypto.  Skip everything else
    if ( this->installName != "/usr/lib/system/libcorecrypto.dylib" )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "fipsSign time");

    // find location in libcorecrypto.dylib to store hash of __text section
    __block const void* textLocation = nullptr;
    __block CacheVMSize textSize;
    __block const void* hashStoreLocation = nullptr;
    __block CacheVMSize hashStoreSize;
    this->forEachCacheSection(^(std::string_view segmentName, std::string_view sectionName,
                                uint8_t *sectionBuffer, CacheVMAddress sectionVMAddr,
                                CacheVMSize sectionVMSize, bool &stop) {
        if ( (segmentName == "__TEXT") && (sectionName == "__text") ) {
            textLocation = sectionBuffer;
            textSize = sectionVMSize;
        } else if ( (segmentName == "__TEXT") && (sectionName == "__fips_hmacs") ) {
            hashStoreLocation = sectionBuffer;
            hashStoreSize = sectionVMSize;
        }
    });

    if ( hashStoreLocation == nullptr ) {
        // FIXME: Plumb up a warning.  We can't make this an error as some platforms don't have this dylib
        // _diagnostics.warning("Could not find __TEXT/__fips_hmacs section in libcorecrypto.dylib, skipping FIPS sealing");
        return;
    }

    if ( hashStoreSize.rawValue() != 32 ) {
        // FIXME: Plumb up a warning.  We can't make this an error as some platforms don't have this dylib
        // _diagnostics.warning("__TEXT/__fips_hmacs section in libcorecrypto.dylib is not 32 bytes in size, skipping FIPS sealing");
        return;
    }

    if ( textLocation == nullptr ) {
        // FIXME: Plumb up a warning.  We can't make this an error as some platforms don't have this dylib
        // _diagnostics.warning("Could not find __TEXT/__text section in libcorecrypto.dylib, skipping FIPS sealing");
        return;
    }

    // store hash directly into hashStoreLocation
    unsigned char hmac_key = 0;
    CCHmac(kCCHmacAlgSHA256, &hmac_key, 1, textLocation, textSize.rawValue(), (void*)hashStoreLocation);
}

template <typename P>
static void addObjcSegments(Diagnostics& diag, const dyld3::MachOFile* objcMF,
                            CacheVMAddress readOnlyVMAddr, CacheVMSize readOnlyVMSize,
                            CacheFileOffset readOnlyFileOffset,
                            CacheVMAddress readWriteVMAddr, CacheVMSize readWriteVMSize,
                            CacheFileOffset readWriteFileOffset)
{
    // validate there is enough free space to add the load commands
    uint32_t freeSpace = ((const Header*)objcMF)->loadCommandsFreeSpace();
    const uint32_t segSize = sizeof(macho_segment_command<P>);
    if ( freeSpace < 2*segSize ) {
        diag.warning("not enough space in libojbc.dylib to add load commands for objc optimization regions");
        return;
    }

    // find location of LINKEDIT LC_SEGMENT load command, we need to insert new segments before it
    uint32_t linkeditIndex = 0;
    uint8_t* linkeditSeg = nullptr;
    linkeditSeg = (uint8_t*)((mach_o::Header*)objcMF)->findLoadCommand(linkeditIndex, ^bool(const load_command *lc) {
        CString segmentName;
        if ( lc->cmd == LC_SEGMENT )
            segmentName = ((const segment_command*)lc)->segname;
        else if ( lc->cmd == LC_SEGMENT_64 )
            segmentName = ((const segment_command_64*)lc)->segname;
        
        return segmentName == "__LINKEDIT";
    });
    
    if ( linkeditSeg == nullptr ) {
        diag.warning("__LINKEDIT not found in libojbc.dylib");
        return;
    }

    // move load commands to make room to insert two new ones before LINKEDIT segment load command
    uint8_t* endOfLoadCommands = (uint8_t*)objcMF + sizeof(macho_header<P>) + objcMF->sizeofcmds;
    uint32_t remainingSize = (uint32_t)(endOfLoadCommands - linkeditSeg);
    memmove(linkeditSeg+2*segSize, linkeditSeg, remainingSize);

    // insert new segments
    macho_segment_command<P>* roSeg = (macho_segment_command<P>*)(linkeditSeg);
    macho_segment_command<P>* rwSeg = (macho_segment_command<P>*)(linkeditSeg+sizeof(macho_segment_command<P>));
    roSeg->set_cmd(macho_segment_command<P>::CMD);
    roSeg->set_cmdsize(segSize);
    roSeg->set_segname("__OBJC_RO");
    roSeg->set_vmaddr(readOnlyVMAddr.rawValue());
    roSeg->set_vmsize(readOnlyVMSize.rawValue());
    roSeg->set_fileoff(readOnlyFileOffset.rawValue());
    roSeg->set_filesize(readOnlyVMSize.rawValue());
    roSeg->set_maxprot(VM_PROT_READ);
    roSeg->set_initprot(VM_PROT_READ);
    roSeg->set_nsects(0);
    roSeg->set_flags(0);
    rwSeg->set_cmd(macho_segment_command<P>::CMD);
    rwSeg->set_cmdsize(segSize);
    rwSeg->set_segname("__OBJC_RW");
    rwSeg->set_vmaddr(readWriteVMAddr.rawValue());
    rwSeg->set_vmsize(readWriteVMSize.rawValue());
    rwSeg->set_fileoff(readWriteFileOffset.rawValue());
    rwSeg->set_filesize(readWriteVMSize.rawValue());
    rwSeg->set_maxprot(VM_PROT_WRITE|VM_PROT_READ);
    rwSeg->set_initprot(VM_PROT_WRITE|VM_PROT_READ);
    rwSeg->set_nsects(0);
    rwSeg->set_flags(0);

    // update mach_header to account for new load commands
    macho_header<P>* mh = (macho_header<P>*)objcMF;
    mh->set_sizeofcmds(mh->sizeofcmds() + 2*segSize);
    mh->set_ncmds(mh->ncmds()+2);
}

void CacheDylib::addObjcSegments(Diagnostics& diag, Timer::AggregateTimer& timer,
                                 const ObjCHeaderInfoReadOnlyChunk* headerInfoReadOnlyChunk,
                                 const ObjCImageInfoChunk* imageInfoChunk,
                                 const ObjCProtocolHashTableChunk* protocolHashTableChunk,
                                 const ObjCPreAttachedCategoriesChunk* preAttachedCategoriesChunk,
                                 const ObjCHeaderInfoReadWriteChunk* headerInfoReadWriteChunk,
                                 const ObjCCanonicalProtocolsChunk* canonicalProtocolsChunk)
{
    // We only need objc.  Skip everything else
    if ( this->installName != "/usr/lib/libobjc.A.dylib" )
        return;

    Timer::AggregateTimer::Scope timedScope(timer, "addObjcSegments time");

    // Find the ranges for OBJC_RO and OBJC_RW

    // Read-only
    // Note these asserts are just to make sure we use the correct chunks for the start/end
    static_assert(Chunk::Kind::objcHeaderInfoRO < Chunk::Kind::objcImageInfo);
    static_assert(Chunk::Kind::objcImageInfo < Chunk::Kind::objcStrings);
    static_assert(Chunk::Kind::objcStrings < Chunk::Kind::objcSelectorsHashTable);
    static_assert(Chunk::Kind::objcSelectorsHashTable < Chunk::Kind::objcClassesHashTable);
    static_assert(Chunk::Kind::objcClassesHashTable < Chunk::Kind::objcProtocolsHashTable);
    static_assert(Chunk::Kind::objcProtocolsHashTable < Chunk::Kind::objcIMPCaches);
    static_assert(Chunk::Kind::objcIMPCaches < Chunk::Kind::objcPreAttachedCategories);

    CacheFileOffset readOnlyFileOffset = headerInfoReadOnlyChunk->subCacheFileOffset;
    CacheVMAddress readOnlyVMAddr = headerInfoReadOnlyChunk->cacheVMAddress;
    CacheVMSize readOnlyVMSize = (preAttachedCategoriesChunk->cacheVMAddress + preAttachedCategoriesChunk->cacheVMSize) - readOnlyVMAddr;


    // Read-write
    static_assert(Chunk::Kind::objcHeaderInfoRW < Chunk::Kind::objcCanonicalProtocols);

    CacheFileOffset readWriteFileOffset = headerInfoReadWriteChunk->subCacheFileOffset;
    CacheVMAddress readWriteVMAddr = headerInfoReadWriteChunk->cacheVMAddress;
    CacheVMSize readWriteVMSize = (canonicalProtocolsChunk->cacheVMAddress + canonicalProtocolsChunk->cacheVMSize) - readWriteVMAddr;

    if ( this->inputHdr->is64() ) {
        typedef Pointer64<LittleEndian> P;
        addObjcSegments<P>(diag, this->cacheMF,
                           readOnlyVMAddr, readOnlyVMSize, readOnlyFileOffset,
                           readWriteVMAddr, readWriteVMSize, readWriteFileOffset);
    } else {
        typedef Pointer32<LittleEndian> P;
        addObjcSegments<P>(diag, this->cacheMF,
                           readOnlyVMAddr, readOnlyVMSize, readOnlyFileOffset,
                           readWriteVMAddr, readWriteVMSize, readWriteFileOffset);
    }
}

void CacheDylib::removeLinkedDylibs(Diagnostics& diag)
{
    mach_o::HeaderWriter* header = (mach_o::HeaderWriter*)cacheHdr;
    uint32_t        lcLibSystemIndex = 0;
    if ( !header->findLoadCommand(lcLibSystemIndex, ^bool(const load_command *lc) {
        const dylib_command* dyliblc = mach_o::Header::isDylibLoadCommand(lc);
        if ( !dyliblc ) return false;

        const char* loadPath = (char*)dyliblc + dyliblc->dylib.name.offset;
        return strstr(loadPath, "libSystem");
    }) ) {
        diag.error("can't remove linked dylibs from %s, expected to find libSystem dependency", header->installName());
        return;
    }

    uint32_t lcDylibStart = 0;
    uint32_t lcDylibEnd = 0;
    header->findLoadCommandRange(lcDylibStart, lcDylibEnd, ^bool(const load_command *lc) {
        return mach_o::Header::isDylibLoadCommand(lc) != nullptr;
    });
    // libSystem was found, so the range of dylib load commands also must not be empty
    assert(lcDylibStart != lcDylibEnd);
    assert(lcLibSystemIndex >= lcDylibStart);

    if ( lcDylibStart != lcLibSystemIndex ) {
        diag.error("expected libSystem to be the first linked dylib of %s, but it's ordinal is: %u",
                header->installName(), lcLibSystemIndex-lcDylibStart);
        return;
    }

    // This removes all load commands after LC_LOAD_DYLIB of libSystem
    if ( mach_o::Error err = header->removeLoadCommands(lcLibSystemIndex+1, lcDylibEnd) )
        diag.error(err);
}

void CacheDylib::addLinkedDylib(Diagnostics& diag, const CacheDylib& dylib)
{
    const char* dylibInstallName = nullptr;
    Version32   compatVersion;
    Version32   currentVersion;
    dylib.inputHdr->getDylibInstallName(&dylibInstallName, &compatVersion, &currentVersion);

    // find the range of all LC_LOAD* commands, new dylib will be added as last
    uint32_t lcLoadStart = 0;
    uint32_t lcLoadEnd = 0;

    mach_o::HeaderWriter* header = (mach_o::HeaderWriter*)this->cacheHdr;
    header->findLoadCommandRange(lcLoadStart, lcLoadEnd, ^bool(const load_command *lc) {
        return mach_o::Header::isDylibLoadCommand(lc) != nullptr;
    });

    if ( lcLoadEnd == 0 ) {
        // there should be at least one already
        diag.error("%s has no linked dylibs", header->installName());
        return;
    }

    // determine command size
    mach_o::LinkedDylibAttributes attr = mach_o::LinkedDylibAttributes::regular;
    uint32_t traditionalCmd = 0;
    uint32_t cmdSize = header->sizeForLinkedDylibCommand(dylibInstallName, attr, traditionalCmd);

    // insert command
    load_command* lc = header->insertLoadCommand(lcLoadEnd, cmdSize);
    if ( lc == nullptr ) {
        diag.error("not enough space in %s to add %s load command", header->installName(), dylibInstallName);
        return;
    }
    header->setLinkedDylib(lc, dylibInstallName, attr, mach_o::Version32(compatVersion), mach_o::Version32(currentVersion));
}

objc_visitor::Visitor CacheDylib::makeCacheObjCVisitor(const BuilderConfig& config,
                                                       const Chunk* selectorStringsChunk,
                                                       const ObjCCanonicalProtocolsChunk* canonicalProtocolsChunk,
                                                       const ObjCPreAttachedCategoriesChunk* categoriesChunk) const
{
    // Get the segment ranges.  We need this as the dylib's segments are in different buffers, not in VM layout
    std::vector<metadata_visitor::Segment> cacheSegments;
    cacheSegments.reserve(this->segments.size());
    for ( uint32_t segIndex = 0; segIndex != this->segments.size(); ++segIndex ) {
        const DylibSegmentChunk& segmentInfo = this->segments[segIndex];

        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(segmentInfo.cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((segmentInfo.cacheVMAddress + segmentInfo.cacheVMSize).rawValue());
        segment.bufferStart = segmentInfo.subCacheBuffer;

        // Cache dylibs never have a chained format. They always use the Fixup struct
        segment.onDiskDylibChainedPointerFormat = { };

        // We need to know what segment we are in, so that we can find the ASLRTracker for the segment
        segment.segIndex = segIndex;

        cacheSegments.push_back(std::move(segment));
    }

    // Add the selector strings chunk too.  That way we can resolve references which land on it
    if ( selectorStringsChunk != nullptr ) {
        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(selectorStringsChunk->cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((selectorStringsChunk->cacheVMAddress + selectorStringsChunk->cacheVMSize).rawValue());
        segment.bufferStart = selectorStringsChunk->subCacheBuffer;

        // Note we don't have a chainedPointerFormat as the selectors don't slide
        segment.onDiskDylibChainedPointerFormat = { };

        cacheSegments.push_back(std::move(segment));
    }

    // Add the canonical protocols chunk too.  That way we can resolve references which land on it
    if ( canonicalProtocolsChunk != nullptr ) {
        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(canonicalProtocolsChunk->cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((canonicalProtocolsChunk->cacheVMAddress + canonicalProtocolsChunk->cacheVMSize).rawValue());
        segment.bufferStart = canonicalProtocolsChunk->subCacheBuffer;

        // Cache segments never have a chained format. They always use the Fixup struct
        segment.onDiskDylibChainedPointerFormat = { };

        cacheSegments.push_back(std::move(segment));
    }

    // Add the categories data chunk too.  That way we can resolve references which land on it
    if ( categoriesChunk != nullptr ) {
        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(categoriesChunk->cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((categoriesChunk->cacheVMAddress + categoriesChunk->cacheVMSize).rawValue());
        segment.bufferStart = categoriesChunk->subCacheBuffer;

        // Cache segments never have a chained format. They always use the Fixup struct
        segment.onDiskDylibChainedPointerFormat = { };

        cacheSegments.push_back(std::move(segment));
    }

    VMAddress selectorStringsAddress;
    if ( selectorStringsChunk != nullptr )
        selectorStringsAddress = VMAddress(selectorStringsChunk->cacheVMAddress.rawValue());

    std::vector<uint64_t> unusedBindTargets;
    objc_visitor::Visitor objcVisitor(config.layout.cacheBaseAddress, this->cacheMF,
                                      std::move(cacheSegments), selectorStringsAddress, std::move(unusedBindTargets));
    return objcVisitor;
}

metadata_visitor::SwiftVisitor CacheDylib::makeCacheSwiftVisitor(const BuilderConfig& config,
                                                                 std::span<metadata_visitor::Segment> extraRegions) const
{
    // Get the segment ranges.  We need this as the dylib's segments are in different buffers, not in VM layout
    std::vector<metadata_visitor::Segment> cacheSegments;
    cacheSegments.reserve(this->segments.size());
    for ( uint32_t segIndex = 0; segIndex != this->segments.size(); ++segIndex ) {
        const DylibSegmentChunk& segmentInfo = this->segments[segIndex];

        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(segmentInfo.cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((segmentInfo.cacheVMAddress + segmentInfo.cacheVMSize).rawValue());
        segment.bufferStart = segmentInfo.subCacheBuffer;

        // Cache dylibs never have a chained format. They always use the Fixup struct
        segment.onDiskDylibChainedPointerFormat = { };

        // We need to know what segment we are in, so that we can find the ASLRTracker for the segment
        segment.segIndex = segIndex;

        cacheSegments.push_back(std::move(segment));
    }

    cacheSegments.insert(cacheSegments.end(), extraRegions.begin(), extraRegions.end());

    std::vector<uint64_t> unusedBindTargets;
    metadata_visitor::SwiftVisitor swiftVisitor(config.layout.cacheBaseAddress, this->cacheMF,
                                                std::move(cacheSegments),
                                                VMAddress(0ULL),
                                                std::move(unusedBindTargets));
    return swiftVisitor;
}

metadata_visitor::Visitor CacheDylib::makeCacheVisitor(const BuilderConfig& config) const
{
    // Get the segment ranges.  We need this as the dylib's segments are in different buffers, not in VM layout
    std::vector<metadata_visitor::Segment> cacheSegments;
    cacheSegments.reserve(this->segments.size());
    for ( uint32_t segIndex = 0; segIndex != this->segments.size(); ++segIndex ) {
        const DylibSegmentChunk& segmentInfo = this->segments[segIndex];

        metadata_visitor::Segment segment;
        segment.startVMAddr = VMAddress(segmentInfo.cacheVMAddress.rawValue());
        segment.endVMAddr   = VMAddress((segmentInfo.cacheVMAddress + segmentInfo.cacheVMSize).rawValue());
        segment.bufferStart = segmentInfo.subCacheBuffer;

        // Cache dylibs never have a chained format. They always use the Fixup struct
        segment.onDiskDylibChainedPointerFormat = { };

        // We need to know what segment we are in, so that we can find the ASLRTracker for the segment
        segment.segIndex = segIndex;

        cacheSegments.push_back(std::move(segment));
    }

    auto addGots = [&cacheSegments](const DylibSectionCoalescer::OptimizedSection& gotSection) {
        if ( gotSection.subCacheSection != nullptr ) {
            auto* chunk = gotSection.subCacheSection->cacheChunk;
            if ( chunk != nullptr ) {
                metadata_visitor::Segment segment;
                segment.startVMAddr = VMAddress(chunk->cacheVMAddress.rawValue());
                segment.endVMAddr   = VMAddress((chunk->cacheVMAddress + chunk->cacheVMSize).rawValue());
                segment.bufferStart = chunk->subCacheBuffer;

                // Cache segments never have a chained format. They always use the Fixup struct
                segment.onDiskDylibChainedPointerFormat = { };

                cacheSegments.push_back(std::move(segment));
            }
        }
    };

    // Add the GOTs too, if we have them
    addGots(optimizedSections.gots);
    addGots(optimizedSections.auth_gots);
    addGots(optimizedSections.auth_ptrs);

    std::vector<uint64_t> unusedBindTargets;
    metadata_visitor::Visitor visitor(config.layout.cacheBaseAddress, this->cacheMF,
                                      std::move(cacheSegments), { },
                                      std::move(unusedBindTargets));
    return visitor;
}

void CacheDylib::forEachCacheSection(void (^callback)(std::string_view segmentName,
                                                      std::string_view sectionName,
                                                      uint8_t* sectionBuffer,
                                                      CacheVMAddress sectionVMAddr,
                                                      CacheVMSize sectionVMSize,
                                                      bool& stop))
{
    this->inputHdr->forEachSection(^(const Header::SegmentInfo &segInfo, const Header::SectionInfo &sectInfo,
                                    bool &stop) {
        const DylibSegmentChunk& segment = this->segments[sectInfo.segIndex];

        VMAddress sectionVMAddr(sectInfo.address);
        VMAddress segmentVMAddr(segInfo.vmaddr);
        VMOffset sectionOffsetInSegment = sectionVMAddr - segmentVMAddr;
        uint8_t* sectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();
        CacheVMAddress cacheVMAddr = segment.cacheVMAddress + sectionOffsetInSegment;

        callback(sectInfo.segmentName, sectInfo.sectionName,
                 sectionBuffer, cacheVMAddr, CacheVMSize(sectInfo.size), stop);
    });
}
