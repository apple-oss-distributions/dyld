/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2018-2023 Apple Inc. All rights reserved.
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

// OS
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_priv.h>
#include <SoftLinking/WeakLinking.h>

// STL
#include <vector>
#include <tuple>
#include <set>
#include <unordered_set>
#include <string>

// mach_o
#include "Header.h"
#include "Version32.h"
#include "Universal.h"
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "SplitSeg.h"
#include "ChainedFixups.h"
#include "FunctionStarts.h"
#include "Misc.h"
#include "Instructions.h"

// common
#include "Defines.h"
#include "FileUtils.h"

// libLTO.dylib is only built for macOS
#if TARGET_OS_OSX
    #if DEBUG
        // building Debug in xcode always uses libLTO
        #define HAVE_LIBLTO 1
    #elif BUILDING_FOR_TOOLCHAIN
        // building for toolchain uses libLTO
        #define HAVE_LIBLTO 1
    #else
        // building for /usr/local/bin/ does not use libLTO
        #define HAVE_LIBLTO 0
    #endif
#else
    #define HAVE_LIBLTO 0
#endif


// llvm
#if HAVE_LIBLTO
    #include <llvm-c/Disassembler.h>
    extern "C" void lto_initialize_disassembler();  // from libLTO.dylib but not in Disassembler.h
    extern "C" int LLVMSetDisasmOptions(LLVMDisasmContextRef context, uint64_t options);\
    WEAK_LINK_FORCE_IMPORT(LLVMCreateDisasm);
    WEAK_LINK_FORCE_IMPORT(LLVMDisasmDispose);
    WEAK_LINK_FORCE_IMPORT(LLVMDisasmInstruction);
    WEAK_LINK_FORCE_IMPORT(LLVMSetDisasmOptions);
    WEAK_LINK_FORCE_IMPORT(lto_initialize_disassembler);
    #ifndef LLVMDisassembler_ReferenceType_In_ARM64_ADRP
        #define LLVMDisassembler_ReferenceType_In_ARM64_ADRP   0x100000001
    #endif
    #ifndef LLVMDisassembler_ReferenceType_In_ARM64_ADDXri
        #define LLVMDisassembler_ReferenceType_In_ARM64_ADDXri 0x100000002
    #endif
    #ifndef LLVMDisassembler_ReferenceType_In_ARM64_LDRXui
        #define LLVMDisassembler_ReferenceType_In_ARM64_LDRXui 0x100000003
    #endif
    #ifndef LLVMDisassembler_ReferenceType_In_ARM64_LDRXl
        #define LLVMDisassembler_ReferenceType_In_ARM64_LDRXl  0x100000004
    #endif
    #ifndef LLVMDisassembler_ReferenceType_In_ARM64_ADR
        #define LLVMDisassembler_ReferenceType_In_ARM64_ADR    0x100000005
    #endif
    #ifndef LLVMDisassembler_Option_PrintImmHex
        #define LLVMDisassembler_Option_PrintImmHex 2
    #endif
    asm(".linker_option \"-lLTO\"");
#endif


using mach_o::Header;
using mach_o::LinkedDylibAttributes;
using mach_o::Version32;
using mach_o::Image;
using mach_o::MappedSegment;
using mach_o::Fixup;
using mach_o::Symbol;
using mach_o::PlatformAndVersions;
using mach_o::ChainedFixups;
using mach_o::CompactUnwind;
using mach_o::Architecture;
using mach_o::SplitSegInfo;
using mach_o::Error;
using mach_o::Instructions;

typedef mach_o::ChainedFixups::PointerFormat    PointerFormat;



/*!
 * @class SymbolicatedImage
 *
 * @abstract
 *      Utility class for analyzing and printing mach-o files
 */
class SymbolicatedImage
{
public:
                    SymbolicatedImage(const Image& im);
                    ~SymbolicatedImage();

    const Image&    image() const   { return _image; }
    bool            is64() const    { return _is64; }
    uint8_t         ptrSize() const { return _ptrSize; }
    void            forEachDefinedObjCClass(void (^callback)(uint64_t classVmAddr)) const;
    void            forEachObjCCategory(void (^callback)(uint64_t categoryVmAddr)) const;
    const char*     className(uint64_t classVmAddr) const;
    const char*     superClassName(uint64_t classVmAddr) const;
    uint64_t        metaClassVmAddr(uint64_t classVmAddr) const;
    void            getProtocolNames(uint64_t classVmAddr, char names[1024]) const;
    const char*     categoryName(uint64_t categoryVmAddr) const;
    const char*     categoryClassName(uint64_t categoryVmAddr) const;
    void            forEachMethodInList(uint64_t methodListVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const;
    void            forEachMethodInClass(uint64_t classVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const;
    void            forEachMethodInCategory(uint64_t categoryVmAddr, void (^instanceCallback)(const char* methodName, uint64_t implAddr),
                                                                     void (^classCallback)(const char* methodName, uint64_t implAddr)) const;
    const char*     selectorFromObjCStub(uint64_t sectionVmAdr, const uint8_t* sectionContent, uint32_t& offset) const;

    const uint8_t*  content(const Header::SectionInfo& sectInfo) const;
    const char*     symbolNameAt(uint64_t addr) const;
    const char*     cStringAt(uint64_t addr) const;
    bool            isBind(const void* location, const Fixup::BindTarget*& bindTarget) const;
    bool            isRebase(const void* location, uint64_t& rebaseTargetVmAddr) const;
    const void*     locationFromVmAddr(uint64_t vmaddr) const;
    void            forEachSymbolRangeInSection(size_t sectNum, void (^callback)(const char* name, uint64_t addr, uint64_t size)) const;
    bool            fairplayEncryptsSomeObjcStrings() const;

    size_t          fixupCount() const { return _fixups.size(); }
    uint8_t         fixupSectNum(size_t fixupIndex) const        { return _fixups[fixupIndex].sectNum; }
    uint64_t        fixupAddress(size_t fixupIndex) const        { return _fixups[fixupIndex].address; }
    CString         fixupInSymbol(size_t fixupIndex) const       { return _fixups[fixupIndex].inSymbolName; }
    uint32_t        fixupInSymbolOffset(size_t fixupIndex) const { return _fixups[fixupIndex].inSymbolOffset; }
    const char*     fixupTypeString(size_t fixupIndex) const;
    const char*     fixupTargetString(size_t fixupIndex, bool symbolic, char buffer[4096]) const;
    CString         fixupSegment(size_t sectNum) const { return _sectionSymbols[sectNum-1].sectInfo.segmentName; }
    CString         fixupSection(size_t sectNum) const { return _sectionSymbols[sectNum-1].sectInfo.sectionName; }


    const char*     libOrdinalName(int libraryOrdinal, char buffer[128]) const { return libOrdinalName(image().header(), libraryOrdinal, buffer); }

    static const char* libOrdinalName(const Header* header, int libraryOrdinal, char buffer[128]);

#if HAVE_LIBLTO
    void                    loadDisassembler();
    const char*             lookupSymbol(uint64_t referencePC, uint64_t referenceValue, uint64_t& referenceType, const char*& referenceName);
    int                     opInfo(uint64_t pc, uint64_t offset, uint64_t opSize, /* uint64_t instSize, */int tagType, void* tagBuf);
    LLVMDisasmContextRef    llvmRef() const { return _llvmRef; }
    const char*             targetTriple() const;
    void                    setSectionContentBias(const uint8_t* p) { _disasmSectContentBias = p; }
#endif

protected:
    void            addFixup(const Fixup& fixup);
    void            findClosestSymbol(uint64_t runtimeOffset, const char*& inSymbolName, uint32_t& inSymbolOffset) const;

    struct SectionSymbols {
        struct Sym { uint64_t offsetInSection; const char* name; };
        Header::SectionInfo  sectInfo;
        std::vector<Sym>     symbols;
    };
    struct FixupInfo {
        Fixup           fixup;
        uint64_t        address             = 0;
        CString         inSymbolName;
        uint32_t        inSymbolOffset      = 0;
        uint32_t        sectNum             = 0;
    };
    const Image&                                 _image;
    std::vector<SectionSymbols>                  _sectionSymbols;
    std::vector<Fixup::BindTarget>               _fixupTargets;
    std::vector<FixupInfo>                       _fixups;
    std::unordered_map<const void*,size_t>       _fixupsMap;
    std::unordered_map<uint64_t, const char*>    _symbolsMap;
    std::unordered_map<uint64_t, const char*>    _stringLiteralsMap;
    std::vector<MappedSegment>                   _mappedSegments;
    uint64_t                                     _fairplayEncryptedStartAddr = 0;
    uint64_t                                     _fairplayEncryptedEndAddr   = 0;
    const bool                                   _is64;
    const size_t                                 _ptrSize;
    const uint64_t                               _prefLoadAddress;
#if HAVE_LIBLTO
    LLVMDisasmContextRef                         _llvmRef = nullptr;
    const uint8_t*                               _disasmSectContentBias = nullptr;
#endif
};

SymbolicatedImage::SymbolicatedImage(const Image& im)
  : _image(im), _is64(im.header()->is64()), _ptrSize(_is64 ? 8 : 4), _prefLoadAddress(im.header()->preferredLoadAddress())
{
    // build list of sections
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        _sectionSymbols.push_back({sectInfo});
    });

    // check for encrypted range
    uint32_t fairplayTextOffsetStart;
    uint32_t fairplaySize;
    if ( _image.header()->isFairPlayEncrypted(fairplayTextOffsetStart, fairplaySize) ) {
        _fairplayEncryptedStartAddr = _prefLoadAddress + fairplayTextOffsetStart;
        _fairplayEncryptedEndAddr   = _fairplayEncryptedStartAddr + fairplaySize;
    }

    // add entries for all functions for function-starts table
    if ( _image.hasFunctionStarts() ) {
        _image.functionStarts().forEachFunctionStart(0, ^(uint64_t funcAddr) {
            //printf("addr: 0x%08llX\n", funcAddr);
            char* label;
            asprintf(&label, "<anon-%08llX>", funcAddr);
            _symbolsMap[funcAddr] = label;
        });
    }
    __block bool hasLocalSymbols = false;
    if ( _image.hasSymbolTable() ) {
        // add symbols from nlist
        _image.symbolTable().forEachDefinedSymbol(^(const Symbol& symbol, uint32_t symbolIndex, bool& stop) {
            uint64_t absAddress;
            if ( !symbol.isAbsolute(absAddress) && (symbol.implOffset() != 0) ) {
                const char* symName = symbol.name().c_str();
                _symbolsMap[_prefLoadAddress+symbol.implOffset()] = symName;
                SectionSymbols& ss = _sectionSymbols[symbol.sectionOrdinal()-1];
                uint64_t offsetInSection = _prefLoadAddress+symbol.implOffset()-ss.sectInfo.address;
                ss.symbols.push_back({offsetInSection, symName});
            }
            if ( symbol.scope() == Symbol::Scope::translationUnit )
                hasLocalSymbols = true;
        });
        // add stubs and cstring literals
        std::span<const uint32_t> indirectTable = _image.indirectSymbolTable();
        __block std::vector<const char*> symbolNames;
        symbolNames.resize(_image.symbolTable().totalCount());
        _image.symbolTable().forEachSymbol(^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            symbolNames[symbolIndex] = symbolName;
        });
        _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            uint32_t type = (sectInfo.flags & SECTION_TYPE);
            if ( type == S_SYMBOL_STUBS ) {
                const int stubSize          = sectInfo.reserved2;
                int stubsIndirectStartIndex = sectInfo.reserved1;
                int stubsIndirectCount      = (int)(sectInfo.size / stubSize);
                for (int i=0; i < stubsIndirectCount; ++i) {
                    uint32_t symbolIndex = indirectTable[stubsIndirectStartIndex+i];
                    if ( (symbolIndex & (INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS)) == 0 )
                        _symbolsMap[sectInfo.address + stubSize*i] = symbolNames[symbolIndex];
                }
            }
            else if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
                int gotsIndirectStartIndex = sectInfo.reserved1;
                int gotsIndirectCount      = (int)(sectInfo.size / 8);  // FIXME: arm64_32
                for (int i=0; i < gotsIndirectCount; ++i) {
                    uint32_t symbolIndex = indirectTable[gotsIndirectStartIndex+i];
                    if ( (symbolIndex & (INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS)) == 0 )
                        _symbolsMap[sectInfo.address + 8*i] = symbolNames[symbolIndex];
                }
            }
        });
    }
    // add c-string labels
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        uint32_t type = (sectInfo.flags & SECTION_TYPE);
        if ( type == S_CSTRING_LITERALS ) {
            const char* sectionContent  = (char*)content(sectInfo);
            const char* stringStart     = sectionContent;
            uint64_t    stringAddr      = sectInfo.address;
            for (int i=0; i < sectInfo.size; ++i) {
                if ( sectionContent[i] == '\0' ) {
                    if ( *stringStart != '\0' ) {
                        _stringLiteralsMap[stringAddr] = stringStart;
                        //fprintf(stderr, "0x%08llX -> \"%s\"\n", sectInfo.address + i, stringStart);
                    }
                    stringStart = &sectionContent[i+1];
                    stringAddr  = sectInfo.address + i + 1;
                }
            }
        }
    });
    _image.withSegments(^(std::span<const MappedSegment> segments) {
        for (const MappedSegment& seg : segments) {
            _mappedSegments.push_back(seg);
        }
    });

    // build list of fixups
    _image.forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
        _fixupTargets.push_back(target);
    });
    _image.forEachFixup(^(const Fixup& fixup, bool& stop) {
        this->addFixup(fixup);
    });

    // if has ObjC and stripped
    if ( !hasLocalSymbols && _image.header()->hasObjC() ) {
        // add back stripped class and method names
        this->forEachDefinedObjCClass(^(uint64_t classVmAddr) {
            const char* classname       = this->className(classVmAddr);
            _symbolsMap[classVmAddr] = classname;
            this->forEachMethodInClass(classVmAddr, ^(const char* methodName, uint64_t implAddr) {
                char* label;
                asprintf(&label, "-[%s %s]", classname, methodName);
                _symbolsMap[implAddr] = label;
            });
            uint64_t    metaClassVmaddr = this->metaClassVmAddr(classVmAddr);
            this->forEachMethodInClass(metaClassVmaddr, ^(const char* methodName, uint64_t implAddr) {
                char* label;
                asprintf(&label, "+[%s %s]", classname, methodName);
                _symbolsMap[implAddr] = label;
            });
        });
        // add back objc stub names
        _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( (sectInfo.sectionName == "__objc_stubs") && sectInfo.segmentName.starts_with("__TEXT") ) {
                const uint8_t* sectionContent = content(sectInfo);
                uint64_t       sectionVmAdr   = sectInfo.address;
                for (uint32_t offset=0; offset < sectInfo.size; ) {
                    uint64_t    labelAddr    = sectionVmAdr+offset;
                    const char* stubSelector = this->selectorFromObjCStub(sectionVmAdr, sectionContent, offset);
                    char* label;
                    asprintf(&label, "_objc_msgSend$%s", stubSelector);
                    _symbolsMap[labelAddr] = label;
                }
            }
        });
    }

    // add synthetic symbols that depend on fixups
    for (SectionSymbols& ss : _sectionSymbols) {
        if ( (ss.sectInfo.sectionName == "__objc_selrefs") && ss.sectInfo.segmentName.starts_with("__DATA") ) {
            for ( uint32_t sectOff=0; sectOff < ss.sectInfo.size; sectOff += _ptrSize) {
                const uint8_t* loc = content(ss.sectInfo) + sectOff;
                const auto&    pos = _fixupsMap.find(loc);
                if ( pos != _fixupsMap.end() ) {
                    const Fixup& fixup = _fixups[pos->second].fixup;
                    if ( !fixup.isBind ) {
                        if ( const char* selector = this->cStringAt(_prefLoadAddress + fixup.rebase.targetVmOffset) ) {
                            char* label;
                            asprintf(&label, "selector \"%s\"", selector);
                            _symbolsMap[ss.sectInfo.address+sectOff] = label;
                        }
                    }
                }
            }
        }
        else if ( (ss.sectInfo.sectionName == "__objc_superrefs") && ss.sectInfo.segmentName.starts_with("__DATA") ) {
            for ( uint32_t sectOff=0; sectOff < ss.sectInfo.size; sectOff += _ptrSize) {
                const uint8_t* loc = content(ss.sectInfo) + sectOff;
                const auto&    pos = _fixupsMap.find(loc);
                if ( pos != _fixupsMap.end() ) {
                    const Fixup& fixup = _fixups[pos->second].fixup;
                    if ( fixup.isBind ) {
                        // FIXME
                    }
                    else {
                        const auto& symbolPos = _symbolsMap.find(_prefLoadAddress + fixup.rebase.targetVmOffset);
                        if ( symbolPos != _symbolsMap.end() ) {
                            char* label;
                            asprintf(&label, "super %s", symbolPos->second);
                            _symbolsMap[ss.sectInfo.address+sectOff] = label;
                        }
                    }
                }
            }
        }
        else if ( (ss.sectInfo.sectionName == "__cfstring") && ss.sectInfo.segmentName.starts_with("__DATA") ) {
            const size_t cfStringSize = _ptrSize * 4;
            for ( uint32_t sectOff=0; sectOff < ss.sectInfo.size; sectOff += cfStringSize) {
                const uint8_t* curContent = content(ss.sectInfo) + sectOff;
                uint64_t       stringVmAddr;
                if ( this->isRebase(&curContent[cfStringSize/2], stringVmAddr) ) {
                    if ( const char* str = this->cStringAt(stringVmAddr) ) {
                        char* label;
                        asprintf(&label, "@\"%s\"", str);
                        _symbolsMap[ss.sectInfo.address+sectOff] = label;
                    }
                    else if ( *((uint32_t*)(&curContent[3*cfStringSize/4])) == 0 ) {
                        // empty string has no cstring
                        _symbolsMap[ss.sectInfo.address+sectOff] = "@\"\"";
                    }
                }
                curContent += cfStringSize;
            }
        }
    }

    // sort symbols within section
    for (SectionSymbols& ss : _sectionSymbols) {
        std::sort(ss.symbols.begin(), ss.symbols.end(), [](const SectionSymbols::Sym& a, const SectionSymbols::Sym& b) {
            return (a.offsetInSection < b.offsetInSection);
        });
    }
    // for any sections that don't have a symbol at start, add seg/sect synthetic symbol
    for (SectionSymbols& ss : _sectionSymbols) {
        if ( ss.symbols.empty() || (ss.symbols[0].offsetInSection != 0) ) {
            // section has no symbols, so makeup one at start
            std::string* str = new std::string(ss.sectInfo.segmentName);
            str->append(",");
            str->append(ss.sectInfo.sectionName);
            ss.symbols.insert(ss.symbols.begin(), {0, str->c_str()});
        }
    }
}

bool SymbolicatedImage::fairplayEncryptsSomeObjcStrings() const
{
    if ( _fairplayEncryptedStartAddr == 0 )
        return false;

    for (const SectionSymbols& ss : _sectionSymbols) {
        if ( ss.sectInfo.address < _fairplayEncryptedEndAddr ) {
            if ( ss.sectInfo.sectionName.starts_with("__objc_") )
                return true;
        }
    }
    return false;
}


const uint8_t* SymbolicatedImage::content(const Header::SectionInfo& sectInfo) const
{
    const Header* header = image().header();
    if ( header->inDyldCache() ) {
        return (uint8_t*)(sectInfo.address + header->getSlide());
    }
    else {
        return (uint8_t*)(header) + sectInfo.fileOffset;
    }
}

void SymbolicatedImage::addFixup(const Fixup& fixup)
{
    _fixupsMap[fixup.location] = _fixups.size();
    uint64_t    segOffset     = (uint8_t*)fixup.location - (uint8_t*)(fixup.segment->content);
    uint64_t    runtimeOffset = fixup.segment->runtimeOffset + segOffset;
    uint64_t    address       = _prefLoadAddress + runtimeOffset;
    const char* inSymbolName;
    uint32_t    inSymbolOffset;
    this->findClosestSymbol(runtimeOffset, inSymbolName, inSymbolOffset);
    uint32_t    sectNum = 1;
    for ( const SectionSymbols& ss : _sectionSymbols ) {
        if ( ss.sectInfo.segmentName == fixup.segment->segName ) {
            if ( (ss.sectInfo.address <= address) && (address < ss.sectInfo.address+ss.sectInfo.size) )
                break;
        }
        sectNum++;
    }
    _fixups.push_back({fixup, address, inSymbolName, inSymbolOffset, sectNum});
}


void SymbolicatedImage::findClosestSymbol(uint64_t runtimeOffset, const char*& inSymbolName, uint32_t& inSymbolOffset) const
{
    inSymbolName    = "";
    inSymbolOffset  = 0;
    for (const SectionSymbols& ss : _sectionSymbols) {
        if ( (runtimeOffset >= ss.sectInfo.address) && (runtimeOffset < ss.sectInfo.address+ss.sectInfo.size) ) {
            // find largest symbol address that is <= target address
            const uint64_t targetSectOffset = runtimeOffset-ss.sectInfo.address;
            auto it = std::lower_bound(ss.symbols.begin(), ss.symbols.end(), targetSectOffset, [](const SectionSymbols::Sym& sym, uint64_t sectOffset) -> bool {
                return sym.offsetInSection <= sectOffset;
            });
            // lower_bound returns the symbol after the one we need
            if ( (it != ss.symbols.end()) && (it != ss.symbols.begin()) ) {
                --it;
                inSymbolName   = it->name;
                inSymbolOffset = (uint32_t)(runtimeOffset - (ss.sectInfo.address+it->offsetInSection));
            }
            else if ( ss.symbols.empty() ) {
                inSymbolName   = "";
                inSymbolOffset = 0;
            }
            else {
                inSymbolName   = ss.symbols.front().name;
                inSymbolOffset = (uint32_t)(runtimeOffset - (ss.sectInfo.address + ss.symbols.front().offsetInSection));
            }
            break;
        }
    }
}

const char* SymbolicatedImage::selectorFromObjCStub(uint64_t sectionVmAdr, const uint8_t* sectionContent, uint32_t& offset) const
{
    if ( _image.header()->arch().usesArm64Instructions() ) {
        const uint32_t* instructions = (uint32_t*)(sectionContent + offset);
        uint32_t selAdrpInstruction  = instructions[0];
        uint32_t selLdrInstruction   = instructions[1];
        if ( (selAdrpInstruction & 0x9F000000) == 0x90000000 ) {
            int32_t  adrpAddend     = (int32_t)(((selAdrpInstruction & 0x60000000) >> 29) | ((selAdrpInstruction & 0x01FFFFE0) >> 3));
            uint64_t adrpTargetAddr = ((sectionVmAdr+offset) & (-4096)) + adrpAddend*0x1000;
            if ( (selLdrInstruction & 0x3B000000) == 0x39000000 ) {
                uint64_t    ldrAddend       = ((selLdrInstruction & 0x003FFC00) >> 10) * _ptrSize;
                uint64_t    selectorVmAddr  = adrpTargetAddr + ldrAddend;
                const void* selectorContent = this->locationFromVmAddr(selectorVmAddr);
                uint64_t    rebaseTargetVmAddr;
                if ( this->isRebase(selectorContent, rebaseTargetVmAddr) ) {
                    if ( const char* selector = this->cStringAt(rebaseTargetVmAddr) ) {
                        offset += 0x20;
                        return selector;
                    }
                }
            }
        }
        offset += 0x20;
    }

    return nullptr;
}


const char* SymbolicatedImage::symbolNameAt(uint64_t addr) const
{
    const auto& pos = _symbolsMap.find(addr);
    if ( pos != _symbolsMap.end() )
        return pos->second;
    return nullptr;
}

const char* SymbolicatedImage::cStringAt(uint64_t addr) const
{
    if ( (_fairplayEncryptedStartAddr <= addr) && (addr < _fairplayEncryptedEndAddr) )
        return "##unavailable##";

    const auto& pos = _stringLiteralsMap.find(addr);
    if ( pos != _stringLiteralsMap.end() )
        return pos->second;
    return nullptr;
}

bool SymbolicatedImage::isBind(const void* location, const Fixup::BindTarget*& bindTarget) const
{
    const auto& pos = _fixupsMap.find(location);
    if ( pos == _fixupsMap.end() )
        return false;
    const Fixup& fixup = _fixups[pos->second].fixup;
    if ( !fixup.isBind )
        return false;
    bindTarget = &_fixupTargets[fixup.bind.bindOrdinal];
    return true;
}

bool SymbolicatedImage::isRebase(const void* location, uint64_t& rebaseTargetVmAddr) const
{
    const auto& pos = _fixupsMap.find(location);
    if ( pos == _fixupsMap.end() )
        return false;
    const Fixup& fixup = _fixups[pos->second].fixup;
    if ( fixup.isBind )
        return false;
    if ( !_is64 && _image.header()->isMainExecutable() )
        rebaseTargetVmAddr = fixup.rebase.targetVmOffset;   // arm64_32 main rebases start at 0 not start of TEXt
    else
        rebaseTargetVmAddr = _prefLoadAddress + fixup.rebase.targetVmOffset;
    return true;
}

const void* SymbolicatedImage::locationFromVmAddr(uint64_t addr) const
{
    uint64_t vmOffset = addr - _prefLoadAddress;
    for ( const MappedSegment& seg : _mappedSegments ) {
        if ( seg.readable && (seg.runtimeOffset <= vmOffset) && (vmOffset < seg.runtimeOffset+seg.runtimeSize) ) {
            return (void*)((uint8_t*)seg.content + vmOffset - seg.runtimeOffset);
        }
    }
    return nullptr;
}

// for a section with symbols in it, divides up section by symbols and call callback once for each range
// if there is no symbol at start of section, callback is called for that range with NULL for name
void SymbolicatedImage::forEachSymbolRangeInSection(size_t sectNum, void (^callback)(const char* name, uint64_t addr, uint64_t size)) const
{
    const SectionSymbols&   ss       = _sectionSymbols[sectNum-1];
    uint64_t                lastAddr = ss.sectInfo.address;
    const char*             lastName = nullptr;
    for (const SectionSymbols::Sym& sym : ss.symbols) {
        uint64_t addr = ss.sectInfo.address + sym.offsetInSection;
        if ( (lastName == nullptr) && (addr == ss.sectInfo.address) ) {
            // if first symbol is start of section, we don't need extra callback
        }
        else {
            callback(lastName, lastAddr, addr - lastAddr);
        }
        lastAddr = addr;
        lastName = sym.name;
    }
    if ( lastName != nullptr )
        callback(lastName, lastAddr, ss.sectInfo.address + ss.sectInfo.size - lastAddr);
}


const char* SymbolicatedImage::className(uint64_t classVmAddr) const
{
    uint64_t roDataFieldAddr = classVmAddr + 4*_ptrSize;
    if ( const void* roDataFieldContent = this->locationFromVmAddr(roDataFieldAddr) ) {
        uint64_t roDataVmAddr;
        if ( this->isRebase(roDataFieldContent, roDataVmAddr) ) {
            roDataVmAddr &= -4; // remove swift bits
            uint64_t nameFieldAddr = roDataVmAddr + 3*_ptrSize;
            if ( const void* nameFieldContent = this->locationFromVmAddr(nameFieldAddr) ) {
                uint64_t nameAddr;
                if ( this->isRebase(nameFieldContent, nameAddr) ) {
                    if ( const char* className = this->cStringAt(nameAddr) ) {
                        return className;
                    }
                }
            }
        }
    }
    return nullptr;
}

const char* SymbolicatedImage::superClassName(uint64_t classVmAddr) const
{
    uint64_t superClassFieldAddr = classVmAddr + _ptrSize;
    if ( const void* superClassFieldContent = this->locationFromVmAddr(superClassFieldAddr) ) {
        uint64_t                 superClassVmAddr;
        const Fixup::BindTarget* superClassBindTarget;
        if ( this->isRebase(superClassFieldContent, superClassVmAddr) ) {
            return this->className(superClassVmAddr);
        }
        else if ( this->isBind(superClassFieldContent, superClassBindTarget) ) {
            CString supername = superClassBindTarget->symbolName;
            if ( supername.starts_with("_OBJC_CLASS_$_") ) {
                return supername.substr(14).c_str();
            }
            return supername.c_str();
        }
    }
    return nullptr;
}

// if class conforms to protocols, return string of protocol names, eg. "<NSFoo, NSBar>"
void SymbolicatedImage::getProtocolNames(uint64_t classVmAddr, char names[1024]) const
{
    names[0] = '\0';
    uint64_t roDataFieldAddr = classVmAddr + 4*_ptrSize;
    if ( const void* roDataFieldContent = this->locationFromVmAddr(roDataFieldAddr) ) {
        uint64_t roDataVmAddr;
        if ( this->isRebase(roDataFieldContent, roDataVmAddr) ) {
            roDataVmAddr &= -4; // remove swift bits
            uint64_t baseProtocolsFieldAddr = roDataVmAddr + ((_ptrSize==8) ? 40 : 24);
            if ( const void* baseProtocolsFieldContent = this->locationFromVmAddr(baseProtocolsFieldAddr) ) {
                uint64_t baseProtocolsListAddr;
                if ( this->isRebase(baseProtocolsFieldContent, baseProtocolsListAddr) ) {
                    if ( const void* protocolListContent = this->locationFromVmAddr(baseProtocolsListAddr) ) {
                        uint32_t count = *(uint32_t*)protocolListContent;
                        strlcpy(names, "<", 1024);
                        bool needComma = false;
                        for (uint32_t i=0; i < count; ++i) {
                            if ( needComma )
                                strlcat(names, ", ", 1024);
                            uint64_t protocolPtrAddr = baseProtocolsListAddr + (i+1)*_ptrSize;
                            if ( const void* protocolPtrContent = this->locationFromVmAddr(protocolPtrAddr) ) {
                                uint64_t protocolAddr;
                                if ( this->isRebase(protocolPtrContent, protocolAddr) ) {
                                    uint64_t protocolNameFieldAddr = protocolAddr + _ptrSize;
                                    if ( const void* protocolNameFieldContent = this->locationFromVmAddr(protocolNameFieldAddr) ) {
                                        uint64_t protocolStringAddr;
                                        if ( this->isRebase(protocolNameFieldContent, protocolStringAddr) ) {
                                            if ( const char* ptotocolName = this->cStringAt(protocolStringAddr) ) {
                                                strlcat(names, ptotocolName, 1024);
                                            }
                                        }
                                    }
                                }
                            }
                            needComma = true;
                        }
                        strlcat(names, ">", 1024);
                   }
                }
            }
        }
    }
}

uint64_t SymbolicatedImage::metaClassVmAddr(uint64_t classVmAddr) const
{
    uint64_t metaClassFieldAddr = classVmAddr;
    if ( const void* metaClassFieldContent = this->locationFromVmAddr(metaClassFieldAddr) ) {
        uint64_t                 metaClassVmAddress;
        const Fixup::BindTarget* bindTarget;
        if ( this->isRebase(metaClassFieldContent, metaClassVmAddress) ) {
            return metaClassVmAddress;
        }
        else if ( this->isBind(metaClassFieldContent, bindTarget) ) {
            // for faster dyld cache patch, classlist is sometimes binds to self for class instead of rebase
            Symbol symbol;
            if ( _image.exportsTrie().hasExportedSymbol(bindTarget->symbolName.c_str(), symbol) ) {
                return symbol.implOffset();
            }
        }
    }
    return 0;
}

const char* SymbolicatedImage::categoryName(uint64_t categoryVmAddr) const
{
    uint64_t catNameFieldAddr = categoryVmAddr;
    if ( const void* catNameFieldContent = this->locationFromVmAddr(catNameFieldAddr) ) {
        uint64_t nameVmAddr;
        if ( this->isRebase(catNameFieldContent, nameVmAddr) ) {
            if ( const char* className = this->cStringAt(nameVmAddr) ) {
                return className;
            }
        }
    }
    return nullptr;
}

const char* SymbolicatedImage::categoryClassName(uint64_t categoryVmAddr) const
{
    uint64_t classFieldAddr = categoryVmAddr + _ptrSize;
    if ( const void* classFieldContent = this->locationFromVmAddr(classFieldAddr) ) {
        uint64_t                 classVmAddr;
        const Fixup::BindTarget* classBindTarget;
       if ( this->isRebase(classFieldContent, classVmAddr) ) {
            return this->className(classVmAddr);
        }
        else if ( this->isBind(classFieldContent, classBindTarget) ) {
            CString supername = classBindTarget->symbolName;
            if ( supername.starts_with("_OBJC_CLASS_$_") ) {
                return supername.substr(14).c_str();
            }
            return supername.c_str();
        }
    }
    return nullptr;
}


void SymbolicatedImage::forEachMethodInClass(uint64_t classVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const
{
    uint64_t roDataFieldAddr = classVmAddr + 4*_ptrSize;
    if ( const void* roDataFieldContent = this->locationFromVmAddr(roDataFieldAddr) ) {
        uint64_t roDataVmAddr;
        if ( this->isRebase(roDataFieldContent, roDataVmAddr) ) {
            roDataVmAddr &= -4; // remove swift bits
            uint64_t methodListFieldAddr = roDataVmAddr + ((_ptrSize==8) ? 32 : 20);
            if ( const void* methodListFieldContent = this->locationFromVmAddr(methodListFieldAddr) ) {
                uint64_t methodListAddr;
                if ( this->isRebase(methodListFieldContent, methodListAddr) ) {
                    this->forEachMethodInList(methodListAddr, callback);
                }
            }
        }
    }
}

void SymbolicatedImage::forEachMethodInCategory(uint64_t categoryVmAddr, void (^instanceCallback)(const char* methodName, uint64_t implAddr),
                                                                         void (^classCallback)(const char* methodName, uint64_t implAddr)) const
{
    uint64_t instanceMethodsFieldAddr = categoryVmAddr + 2*_ptrSize;
    if ( const void* instanceMethodsFieldContent = this->locationFromVmAddr(instanceMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(instanceMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, instanceCallback);
        }
    }
    uint64_t classMethodsFieldAddr = categoryVmAddr + 3*_ptrSize;
    if ( const void* classMethodsFieldContent = this->locationFromVmAddr(classMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(classMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, classCallback);
        }
    }
}


void SymbolicatedImage::forEachMethodInList(uint64_t methodListVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const
{
     if ( const void* methodListContent = this->locationFromVmAddr(methodListVmAddr) ) {
        const uint32_t* methodListArray = (uint32_t*)methodListContent;
        uint32_t entrySize = methodListArray[0];
        uint32_t count     = methodListArray[1];
        if ( entrySize == 0x8000000C ) {
            // relative method lists
            for (uint32_t i=0; i < count; ++i) {
                int32_t  nameOffset           = methodListArray[i*3+2];
                uint64_t methodSelectorVmAddr = methodListVmAddr+i*12+nameOffset+8;
                int32_t  implOffset           = methodListArray[i*3+4];
                uint64_t implAddr             = methodListVmAddr+i*12+implOffset+16;
                if ( const void* methodSelectorContent = this->locationFromVmAddr(methodSelectorVmAddr) ) {
                    uint64_t selectorTargetAddr;
                    if ( this->isRebase(methodSelectorContent, selectorTargetAddr) ) {
                        if ( const char* methodName = this->cStringAt(selectorTargetAddr) )
                            callback(methodName, implAddr);
                    }
                }
            }
        }
        else if ( entrySize == 24 ) {
            // 64-bit absolute method lists
            for (uint32_t i=0; i < count; ++i) {
                if ( const void* methodNameContent = this->locationFromVmAddr(methodListVmAddr+i*24+8) ) {
                    uint64_t methodNameAddr;
                    if ( this->isRebase(methodNameContent, methodNameAddr) ) {
                        if ( const char* methodName = this->cStringAt(methodNameAddr) ) {
                            if ( const void* methodImplContent = this->locationFromVmAddr(methodListVmAddr+i*24+24) ) {
                                uint64_t implAddr;
                                if ( this->isRebase(methodImplContent, implAddr) ) {
                                    callback(methodName, implAddr);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
 }


void SymbolicatedImage::forEachDefinedObjCClass(void (^callback)(uint64_t classVmAddr)) const
{
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.sectionName == "__objc_classlist") && sectInfo.segmentName.starts_with("__DATA") ) {
            const uint8_t* sectionContent    = content(sectInfo);
            const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
            const uint8_t* curContent        = sectionContent;
            while ( curContent < sectionContentEnd ) {
                const Fixup::BindTarget* bindTarget;
                uint64_t                 rebaseTargetVmAddr;
                if ( this->isRebase(curContent, rebaseTargetVmAddr) ) {
                    callback(rebaseTargetVmAddr);
                }
                else if ( this->isBind(curContent, bindTarget) ) {
                    // for faster dyld cache patch, classlist is sometimes binds to self for class instead of rebase
                    Symbol symbol;
                    if ( _image.exportsTrie().hasExportedSymbol(bindTarget->symbolName.c_str(), symbol) ) {
                        callback(symbol.implOffset());
                    }
                }
                curContent += _ptrSize;
            }
        }
    });
}

void SymbolicatedImage::forEachObjCCategory(void (^callback)(uint64_t categoryVmAddr)) const
{
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.sectionName == "__objc_catlist") && sectInfo.segmentName.starts_with("__DATA") ) {
            const uint8_t* sectionContent    = content(sectInfo);
            const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
            const uint8_t* curContent        = sectionContent;
            while ( curContent < sectionContentEnd ) {
                uint64_t   rebaseTargetVmAddr;
                if ( this->isRebase(curContent, rebaseTargetVmAddr) )
                    callback(rebaseTargetVmAddr);
                curContent += _ptrSize;
            }
        }
    });
}

const char* SymbolicatedImage::libOrdinalName(const Header* header, int libOrdinal, char buffer[128])
{
    CString name = header->libOrdinalName(libOrdinal);
    // convert:
    //  /path/stuff/Foo.framework/Foo   => Foo
    //  /path/stuff/libfoo.dylib        => libfoo
    //  /path/stuff/libfoo.A.dylib      => libfoo
    strlcpy(buffer, name.leafName().c_str(), 128);
    if ( name.ends_with(".dylib") ) {
        size_t len = strlen(buffer);
        buffer[len-6] = '\0';
        if ( (len > 8) && (buffer[len-8] == '.') )
            buffer[len-8] = '\0';
    }

    return buffer;
}

const char* SymbolicatedImage::fixupTypeString(size_t fixupIndex) const
{
    const Fixup& fixup = _fixups[fixupIndex].fixup;
    if ( fixup.isBind  ) {
        if ( fixup.authenticated )
            return "auth-bind";
        else if ( fixup.isLazyBind )
            return "lazy-bind";
        else
            return "bind";
    }
    else {
        if ( fixup.authenticated )
            return "auth-rebase";
        else
            return "rebase";
    }
}

const char* SymbolicatedImage::fixupTargetString(size_t fixupIndex, bool symbolic, char buffer[4096]) const
{
    const Fixup& fixup = _fixups[fixupIndex].fixup;
    char         authInfo[64];

    authInfo[0] = '\0';
    if ( fixup.authenticated )
        snprintf(authInfo, sizeof(authInfo), " (div=0x%04X ad=%d key=%s)", fixup.auth.diversity, fixup.auth.usesAddrDiversity, fixup.keyName());

    if ( fixup.isBind ) {
        char                     dylibBuf[128];
        const Fixup::BindTarget& bindTarget = _fixupTargets[fixup.bind.bindOrdinal];
        int64_t                  addend     = bindTarget.addend + fixup.bind.embeddedAddend;

        if ( addend != 0 )
            snprintf(buffer, 4096, "%s/%s + 0x%llX%s",      libOrdinalName(bindTarget.libOrdinal, dylibBuf), bindTarget.symbolName.c_str(), addend, authInfo);
        else if ( bindTarget.weakImport )
            snprintf(buffer, 4096, "%s/%s [weak-import]%s", libOrdinalName(bindTarget.libOrdinal, dylibBuf), bindTarget.symbolName.c_str(), authInfo);
        else
            snprintf(buffer, 4096, "%s/%s%s",               libOrdinalName(bindTarget.libOrdinal, dylibBuf), bindTarget.symbolName.c_str(), authInfo);
    }
    else {
        if ( symbolic ) {
            const char* inSymbolName;
            uint32_t    inSymbolOffset;
            this->findClosestSymbol(fixup.rebase.targetVmOffset, inSymbolName, inSymbolOffset);
            if ( strncmp(inSymbolName, "__TEXT,", 7) == 0 ) {
                const char* str = this->cStringAt(_prefLoadAddress+fixup.rebase.targetVmOffset);
                snprintf(buffer, 4096, "\"%s\"%s", str, authInfo);
            }
            else if ( inSymbolOffset == 0 ) {
                snprintf(buffer, 4096, "%s%s", inSymbolName, authInfo);
            }
            else {
                snprintf(buffer, 4096, "%s+%u%s", inSymbolName, inSymbolOffset, authInfo);
            }
        }
        else {
            snprintf(buffer, 4096, "0x%08llX%s",_prefLoadAddress+fixup.rebase.targetVmOffset, authInfo);
        }
    }
    return buffer;
}

SymbolicatedImage::~SymbolicatedImage()
{
#if HAVE_LIBLTO
    if ( _llvmRef != nullptr ) {
        LLVMDisasmDispose(_llvmRef);
        _llvmRef = nullptr;
    }
#endif
}


#if HAVE_LIBLTO

static const char* printDumpSymbolCallback(void* di, uint64_t referenceValue, uint64_t* referenceType,
                                           uint64_t referencePC, const char** referenceName)
{
    return ((SymbolicatedImage*)di)->lookupSymbol(referencePC, referenceValue, *referenceType, *referenceName);
}

static int printDumpOpInfoCallback(void* di, uint64_t pc, uint64_t offset, uint64_t opSize, /* uint64_t instSize, */
                                   int tagType, void* tagBuf)
{
    return ((SymbolicatedImage*)di)->opInfo(pc, offset, opSize, tagType, tagBuf);
}

const char* SymbolicatedImage::targetTriple() const
{
    Architecture arch = _image.header()->arch();
    if ( arch.usesArm64Instructions() )
        return "arm64e-apple-darwin";
    else if ( arch.usesx86_64Instructions() )
        return "x86_64h-apple-darwin";
    else
        return "unknown";
}

void SymbolicatedImage::loadDisassembler()
{
    // libLTO is weak linked, need to bail out if libLTO.dylib not loaded
    if ( &lto_initialize_disassembler == nullptr )
        return;

    // Hack for using llvm disassemble in libLTO.dylib
    static bool llvmDisAssemblerInitialized = false;
    if ( !llvmDisAssemblerInitialized ) {
        lto_initialize_disassembler();
        llvmDisAssemblerInitialized = true;
    }

    // instantiate llvm disassembler object
    _llvmRef = LLVMCreateDisasm(targetTriple(), this, 0, &printDumpOpInfoCallback, &printDumpSymbolCallback);
    if ( _llvmRef != nullptr )
        LLVMSetDisasmOptions(_llvmRef, LLVMDisassembler_Option_PrintImmHex);
}


const char* SymbolicatedImage::lookupSymbol(uint64_t refPC, uint64_t refValue, uint64_t& refType, const char*& refName)
{
    //printf("refPC=0x%08llX, refType=0x%llX\n", refPC, refType);
    refName = nullptr;
    if ( refType == LLVMDisassembler_ReferenceType_In_Branch ) {
        refType = LLVMDisassembler_ReferenceType_InOut_None;
        const auto& pos = _symbolsMap.find(refValue);
        if ( pos != _symbolsMap.end() ) {
            return pos->second;
        }
        return nullptr;
    }
    else if ( refType == LLVMDisassembler_ReferenceType_In_ARM64_ADR ) {
        const auto& pos = _stringLiteralsMap.find(refValue);
        if ( pos != _stringLiteralsMap.end() ) {
            refType = LLVMDisassembler_ReferenceType_Out_LitPool_CstrAddr;
            refName = pos->second;
            return nullptr;
        }
        else {
            const auto& pos2 = _symbolsMap.find(refValue);
            if ( pos2 != _symbolsMap.end() ) {
                refName = pos2->second;
                refType = LLVMDisassembler_ReferenceType_Out_LitPool_SymAddr;
                return nullptr;
            }

        }
    }
    else if ( refType == LLVMDisassembler_ReferenceType_In_ARM64_LDRXl ) {
        const auto& pos = _symbolsMap.find(refValue);
        if ( pos != _symbolsMap.end() ) {
            refType = LLVMDisassembler_ReferenceType_Out_LitPool_SymAddr;
            refName = pos->second;
            return nullptr;
        }
    }
    else if ( (refType == LLVMDisassembler_ReferenceType_In_ARM64_LDRXui) || (refType == LLVMDisassembler_ReferenceType_In_ARM64_ADDXri) ) {
        const uint32_t* instructionPtr = (uint32_t*)(&_disasmSectContentBias[refPC]);
        uint32_t thisInstruction = instructionPtr[0];
        uint32_t prevInstruction = instructionPtr[-1];
        // if prev instruction was ADRP and it matches register, the we can compute target
        Instructions::arm64::AdrpInfo  adrpInfo;
        Instructions::arm64::Imm12Info imm12Info;
        if ( Instructions::arm64::isADRP(prevInstruction, adrpInfo) && Instructions::arm64::isImm12(thisInstruction, imm12Info) ) {
            if ( adrpInfo.dstReg == imm12Info.srcReg ) {
                uint64_t targetAddr = (refPC & -4096) + adrpInfo.pageOffset*4096 + imm12Info.offset;
                const auto& pos = _symbolsMap.find(targetAddr);
                if ( pos != _symbolsMap.end() ) {
                    refName = pos->second;
                    refType = LLVMDisassembler_ReferenceType_Out_LitPool_SymAddr;
                    return nullptr;
                }
            }
        }
    }
    else if ( refType == LLVMDisassembler_ReferenceType_In_ARM64_ADRP ) {
    }
    else if ( refType == LLVMDisassembler_ReferenceType_In_PCrel_Load ) {
        const auto& pos = _stringLiteralsMap.find(refValue);
        if ( pos != _stringLiteralsMap.end() ) {
            refType = LLVMDisassembler_ReferenceType_Out_LitPool_CstrAddr;
            refName = pos->second;
            return nullptr;
        }
        const auto& pos2 = _symbolsMap.find(refValue);
        if ( pos2 != _symbolsMap.end() ) {
            refName = pos2->second;
            return nullptr;
        }
        return nullptr;
    }
    else if ( refType == LLVMDisassembler_ReferenceType_InOut_None ) {
        return nullptr;
    }
    else {
        //printf("refPC=0x%08llX, refType=0x%llX\n", refPC, refType);
    }
    return nullptr;
}

int SymbolicatedImage::opInfo(uint64_t pc, uint64_t offset, uint64_t opSize, /* uint64_t instSize, */int tagType, void* tagBuf)
{
    return 0;
}
#endif // HAVE_LIBLTO









static void printPlatforms(const Header* header)
{
    if ( header->isPreload() )
        return;
    PlatformAndVersions pvs = header->platformAndVersions();
    char osVers[32];
    char sdkVers[32];
    pvs.minOS.toString(osVers);
    pvs.sdk.toString(sdkVers);
    printf("    -platform:\n");
    printf("        platform     minOS      sdk\n");
    printf(" %15s     %-7s   %-7s\n", pvs.platform.name().c_str(), osVers, sdkVers);
}

static void printUUID(const Header* header)
{
    printf("    -uuid:\n");
    uuid_t uuid;
    if ( header->getUuid(uuid) ) {
        uuid_string_t uuidString;
        uuid_unparse_upper(uuid, uuidString);
        printf("        %s\n", uuidString);
    }
}

static void permString(uint32_t permFlags, char str[4])
{
    str[0] = (permFlags & VM_PROT_READ)    ? 'r' : '.';
    str[1] = (permFlags & VM_PROT_WRITE)   ? 'w' : '.';
    str[2] = (permFlags & VM_PROT_EXECUTE) ? 'x' : '.';
    str[3] = '\0';
}

static void printSegments(const Header* header)
{
    if ( header->isPreload() ) {
        printf("    -segments:\n");
        printf("       file-offset vm-addr       segment      section         sect-size  seg-size perm\n");
        __block std::string_view lastSegName;
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( sectInfo.segmentName != lastSegName ) {
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char permChars[8];
                permString(sectInfo.segPerms, permChars);
                printf("        0x%06X   0x%09llX    %-16s                   %6lluKB   %s\n",
                       sectInfo.fileOffset, sectInfo.address, sectInfo.segmentName.c_str(), segVmSize/1024, permChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%06X   0x%09llX              %-16s %7llu\n", sectInfo.fileOffset, sectInfo.address, sectInfo.sectionName.c_str(), sectInfo.size);
        });
    }
    else if ( header->inDyldCache() ) {
        printf("    -segments:\n");
        printf("        unslid-addr   segment   section        sect-size  seg-size perm\n");
        __block std::string_view lastSegName;
        __block uint64_t         segVmAddr    = 0;
        __block uint64_t         startVmAddr  = header->segmentVmAddr(0);
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( sectInfo.segmentName != lastSegName ) {
                segVmAddr = header->segmentVmAddr(sectInfo.segIndex);
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char permChars[8];
                permString(sectInfo.segPerms, permChars);
                printf("        0x%09llX    %-16s                  %6lluKB  %s\n",
                       segVmAddr, sectInfo.segmentName.c_str(), segVmSize/1024, permChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%09llX             %-16s %7llu\n", startVmAddr+sectInfo.address, sectInfo.sectionName.c_str(), sectInfo.size);
        });
    }
    else {
        printf("    -segments:\n");
        printf("        load-offset   segment  section       sect-size  seg-size perm\n");
        __block std::string_view lastSegName;
        __block uint64_t         textSegVmAddr = 0;
        header->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
            if ( lastSegName.empty() ) {
                textSegVmAddr  = header->segmentVmAddr(sectInfo.segIndex);
            }
            if ( sectInfo.segmentName != lastSegName ) {
                uint64_t segVmAddr = header->segmentVmAddr(sectInfo.segIndex);
                uint64_t segVmSize = header->segmentVmSize(sectInfo.segIndex);
                char permChars[8];
                permString(sectInfo.segPerms, permChars);
                printf("        0x%08llX    %-16s                  %6lluKB %s\n",
                       segVmAddr-textSegVmAddr, sectInfo.segmentName.c_str(), segVmSize/1024, permChars);
                lastSegName = sectInfo.segmentName;
            }
                printf("        0x%08llX             %-16s %6llu\n", sectInfo.address, sectInfo.sectionName.c_str(), sectInfo.size);
        });
    }
}

static void printLinkedDylibs(const Header* mh)
{
    if ( mh->isPreload() )
        return;
    printf("    -linked_dylibs:\n");
    printf("        attributes     load path\n");
    mh->forEachLinkedDylib(^(const char* loadPath, LinkedDylibAttributes depAttrs, Version32 compatVersion, Version32 curVersion, bool& stop) {
        std::string attributes;
        if ( depAttrs.upward )
            attributes += "upward ";
        if ( depAttrs.delayInit )
            attributes += "delay-init ";
        if ( depAttrs.weakLink )
            attributes += "weak-link ";
        if ( depAttrs.reExport )
            attributes += "re-export ";
        printf("        %-12s   %s\n", attributes.c_str(), loadPath);
    });
}

static void printInitializers(const Image& image)
{
    printf("    -inits:\n");
    SymbolicatedImage symImage(image);

    // print static initializers
    image.forEachInitializer(^(uint32_t initOffset) {
        const char* initName       = "?";
        uint64_t    unslidInitAddr = image.header()->preferredLoadAddress() + initOffset;
        Symbol      symbol;
        uint64_t    addend         = 0;
        initName = symImage.symbolNameAt(initOffset);
        if ( initName == nullptr ) {
            if ( image.symbolTable().findClosestDefinedSymbol(unslidInitAddr, symbol) ) {
                initName = symbol.name().c_str();
                uint64_t symbolAddr = image.header()->preferredLoadAddress() + symbol.implOffset();
                addend = unslidInitAddr - symbolAddr;
            }
        }
        if ( initName == nullptr )
            initName = "";
        if ( addend == 0 )
            printf("        0x%08X  %s\n", initOffset, initName);
        else
            printf("        0x%08X  %s + %llu\n", initOffset, initName, addend);
    });

    // print static terminators
    if ( !image.header()->isArch("arm64e") ) {
        image.forEachClassicTerminator(^(uint32_t termOffset) {
            const char* termName       = "?";
            uint64_t    unslidInitAddr = image.header()->preferredLoadAddress() + termOffset;
            Symbol      symbol;
            uint64_t    addend         = 0;
            termName = symImage.symbolNameAt(termOffset);
            if ( termName == nullptr ) {
                if ( image.symbolTable().findClosestDefinedSymbol(unslidInitAddr, symbol) ) {
                    termName = symbol.name().c_str();
                    uint64_t symbolAddr = image.header()->preferredLoadAddress() + symbol.implOffset();
                    addend = unslidInitAddr - symbolAddr;
                }
            }
            if ( termName == nullptr )
                termName = "";
            if ( addend == 0 )
                printf("        0x%08X  %s [terminator]\n", termOffset, termName);
            else
                printf("        0x%08X  %s + %llu [terminator]\n", termOffset, termName, addend);
        });
    }

    // print +load methods
    // TODO: rdar://122190141 (Enable +load initializers in dyld_info)
    //if ( image.header()->hasObjC() ) {
    //    const SymbolicatedImage* symImagePtr = &symImage; // for no copy in block...
    //    symImage.forEachDefinedObjCClass(^(uint64_t classVmAddr) {
    //        const char*  classname       = symImagePtr->className(classVmAddr);
    //        uint64_t     metaClassVmaddr = symImagePtr->metaClassVmAddr(classVmAddr);
    //        symImagePtr->forEachMethodInClass(metaClassVmaddr, ^(const char* methodName, uint64_t implAddr) {
    //            if ( strcmp(methodName, "load") == 0 )
    //                printf("        0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
    //        });
    //    });
    //    symImage.forEachObjCCategory(^(uint64_t categoryVmAddr) {
    //        const char* catname   = symImagePtr->categoryName(categoryVmAddr);
    //        const char* classname = symImagePtr->categoryClassName(categoryVmAddr);
    //        symImagePtr->forEachMethodInCategory(categoryVmAddr,
    //                                             ^(const char* instanceMethodName, uint64_t implAddr) {},
    //                                             ^(const char* classMethodName,    uint64_t implAddr) {
    //            if ( strcmp(classMethodName, "load") == 0 )
    //                printf("        0x%08llX  +[%s(%s) %s]\n", implAddr, classname, catname, classMethodName);
    //        });
    //    });
    //}
}

static void printChainInfo(const Image& image)
{
    printf("    -fixup_chains:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    if ( image.hasChainedFixups() ) {
        const ChainedFixups& chainedFixups = image.chainedFixups();
        size_t chainHeaderSize;
        if ( const dyld_chained_fixups_header* chainHeader = chainedFixups.bytes(chainHeaderSize) ) {
            printf("      fixups_version:   0x%08X\n",  chainHeader->fixups_version);
            printf("      starts_offset:    0x%08X\n",  chainHeader->starts_offset);
            printf("      imports_offset:   0x%08X\n",  chainHeader->imports_offset);
            printf("      symbols_offset:   0x%08X\n",  chainHeader->symbols_offset);
            printf("      imports_count:    %d\n",      chainHeader->imports_count);
            printf("      imports_format:   %d (%s)\n", chainHeader->imports_format, ChainedFixups::importsFormatName(chainHeader->imports_format));
            printf("      symbols_format:   %d\n",      chainHeader->symbols_format);
            const dyld_chained_starts_in_image* starts = (dyld_chained_starts_in_image*)((uint8_t*)chainHeader + chainHeader->starts_offset);
            for (int i=0; i < starts->seg_count; ++i) {
                if ( starts->seg_info_offset[i] == 0 )
                    continue;
                const dyld_chained_starts_in_segment* seg = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[i]);
                if ( seg->page_count == 0 )
                    continue;
                const uint8_t* segEnd = ((uint8_t*)seg + seg->size);
                const PointerFormat& pf = PointerFormat::make(seg->pointer_format);
                printf("        seg[%d]:\n", i);
                printf("          page_size:       0x%04X\n",      seg->page_size);
                printf("          pointer_format:  %d (%s)(%s)\n", seg->pointer_format, pf.name(), pf.description());
                printf("          segment_offset:  0x%08llX\n",    seg->segment_offset);
                printf("          max_pointer:     0x%08X\n",      seg->max_valid_pointer);
                printf("          pages:         %d\n",            seg->page_count);
                for (int pageIndex=0; pageIndex < seg->page_count; ++pageIndex) {
                    if ( (uint8_t*)(&seg->page_start[pageIndex]) >= segEnd ) {
                        printf("         start[% 2d]:  <<<off end of dyld_chained_starts_in_segment>>>\n", pageIndex);
                        continue;
                    }
                    uint16_t offsetInPage = seg->page_start[pageIndex];
                    if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
                        continue;
                    if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
                        // 32-bit chains which may need multiple starts per page
                        uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
                        bool chainEnd = false;
                        while (!chainEnd) {
                            chainEnd = (seg->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                            offsetInPage = (seg->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                            printf("         start[% 2d]:  0x%04X\n",   pageIndex, offsetInPage);
                            ++overflowIndex;
                        }
                    }
                    else {
                        // one chain per page
                        printf("             start[% 2d]:  0x%04X\n",   pageIndex, offsetInPage);
                    }
                }
            }
        }
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(fwPointerFormat);
        printf("  pointer_format:  %d (%s)\n", fwPointerFormat, pf.description());

        for (uint32_t i=0; i < fwStartsCount; ++i) {
            const uint32_t startVmOffset = fwStarts[i];
            printf("    start[% 2d]: vm offset: 0x%04X\n", i, startVmOffset);
        }
    }
}

static void printImports(const Image& image)
{
    printf("    -imports:\n");
    __block uint32_t bindOrdinal = 0;
    if ( image.hasChainedFixups() ) {
        image.chainedFixups().forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
            char buffer[128];
            const char* weakStr = (target.weakImport ? "[weak-import]" : "");
            if ( target.addend == 0 )
                printf("      0x%04X  %s %s (from %s)\n", bindOrdinal, target.symbolName.c_str(), weakStr, SymbolicatedImage::libOrdinalName(image.header(), target.libOrdinal, buffer));
            else
                printf("      0x%04X  %s+0x%llX %s (from %s)\n", bindOrdinal, target.symbolName.c_str(), target.addend, weakStr, SymbolicatedImage::libOrdinalName(image.header(), target.libOrdinal, buffer));
            ++bindOrdinal;
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachUndefinedSymbol(^(const Symbol& symbol, uint32_t symIndex, bool& stop) {
            int  libOrdinal;
            bool weakImport;
            if ( symbol.isUndefined(libOrdinal, weakImport) ) {
                char buffer[128];
                const char* weakStr = (weakImport ? "[weak-import]" : "");
                printf("      %s %s (from %s)\n", symbol.name().c_str(), weakStr, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
            }
        });
    }
}


static void printChainDetails(const Image& image)
{
    printf("    -fixup_chain_details:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    if ( image.hasChainedFixups() ) {
        const PointerFormat& pf = image.chainedFixups().pointerFormat();
        image.forEachFixup(^(const Fixup& info, bool& stop) {
            uint64_t vmOffset = (uint8_t*)info.location - (uint8_t*)image.header();
            const void* nextLoc = pf.nextLocation(info.location);
            uint32_t next = 0;
            if ( nextLoc != nullptr )
                next = (uint32_t)((uint8_t*)nextLoc - (uint8_t*)info.location)/pf.minNext();
            if ( info.isBind ) {
                if ( image.header()->is64() ) {
                    const char* authPrefix = "     ";
                    char authInfoStr[128] = "";
                    if ( info.authenticated ) {
                        authPrefix = "auth-";
                        snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                                 info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                    }
                    char addendInfo[32] = "";
                    if ( info.bind.embeddedAddend != 0 )
                        snprintf(addendInfo, sizeof(addendInfo), ", addend: %d", info.bind.embeddedAddend);
                    printf("  0x%08llX:  raw: 0x%016llX    %sbind: (next: %03d, %sbindOrdinal: 0x%06X%s)\n",
                            vmOffset, *((uint64_t*)info.location), authPrefix, next, authInfoStr, info.bind.bindOrdinal, addendInfo);
                }
                else {
                    printf("  0x%08llX:  raw: 0x%08X     bind: (next: %02d bindOrdinal: 0x%07X)\n",
                            vmOffset, *((uint32_t*)info.location), next, info.bind.bindOrdinal);

                }
            }
            else {
                uint8_t high8 = 0; // FIXME:
                if ( image.header()->is64() ) {
                    const char* authPrefix = "     ";
                    char authInfoStr[128] = "";
                    if ( info.authenticated ) {
                        authPrefix = "auth-";
                        snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                                 info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                    }
                    char high8Info[32] = "";
                    if ( high8 != 0 )
                        snprintf(high8Info, sizeof(high8Info), ", high8: 0x%02X", high8);
                    printf("  0x%08llX:  raw: 0x%016llX  %srebase: (next: %03d, %starget: 0x%011llX%s)\n",
                            vmOffset, *((uint64_t*)info.location), authPrefix, next, authInfoStr, info.rebase.targetVmOffset, high8Info);
                }
                else {
                    printf("  0x%08llX:  raw: 0x%08X  rebase: (next: %02d target: 0x%07llX)\n",
                            vmOffset, *((uint32_t*)info.location), next, info.rebase.targetVmOffset);

                }
            }
        });
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        uint64_t prefLoadAddr = image.header()->preferredLoadAddress();
        image.forEachFixup(^(const Fixup& info, bool& stop) {
            uint64_t segOffset = (uint8_t*)info.location - (uint8_t*)info.segment->content;
            uint64_t vmAddr    = prefLoadAddr + info.segment->runtimeOffset + segOffset;
            uint8_t  high8    = 0; // FIXME:
            if ( image.header()->is64() ) {
                const char* authPrefix = "     ";
                char authInfoStr[128] = "";
                if ( info.authenticated ) {
                    authPrefix = "auth-";
                    snprintf(authInfoStr, sizeof(authInfoStr), "key: %s, addrDiv: %d, diversity: 0x%04X, ",
                             info.keyName(), info.auth.usesAddrDiversity, info.auth.diversity);
                }
                char high8Info[32] = "";
                if ( high8 != 0 )
                    snprintf(high8Info, sizeof(high8Info), ", high8: 0x%02X", high8);
                printf("  0x%08llX:  raw: 0x%016llX  %srebase: (%starget: 0x%011llX%s)\n",
                        vmAddr, *((uint64_t*)info.location), authPrefix, authInfoStr, info.rebase.targetVmOffset, high8Info);
            }
            else {
                printf("  0x%08llX:  raw: 0x%08X  rebase: (target: 0x%07llX)\n",
                        vmAddr, *((uint32_t*)info.location), info.rebase.targetVmOffset);
            }
        });
    }
}

static void printChainHeader(const Image& image)
{
    printf("    -fixup_chain_header:\n");

    uint16_t           fwPointerFormat;
    uint32_t           fwStartsCount;
    const uint32_t*    fwStarts;
    if ( image.hasChainedFixups() ) {
        const ChainedFixups& chainedFixups = image.chainedFixups();
        size_t               chainHeaderSize;
        if ( const dyld_chained_fixups_header* chainsHeader = chainedFixups.bytes(chainHeaderSize) ) {
            printf("        dyld_chained_fixups_header:\n");
            printf("            fixups_version  0x%08X\n", chainsHeader->fixups_version);
            printf("            starts_offset   0x%08X\n", chainsHeader->starts_offset);
            printf("            imports_offset  0x%08X\n", chainsHeader->imports_offset);
            printf("            symbols_offset  0x%08X\n", chainsHeader->symbols_offset);
            printf("            imports_count   0x%08X\n", chainsHeader->imports_count);
            printf("            imports_format  0x%08X\n", chainsHeader->imports_format);
            printf("            symbols_format  0x%08X\n", chainsHeader->symbols_format);
            const dyld_chained_starts_in_image* starts = (dyld_chained_starts_in_image*)((uint8_t*)chainsHeader + chainsHeader->starts_offset);
            printf("        dyld_chained_starts_in_image:\n");
            printf("            seg_count              0x%08X\n", starts->seg_count);
            for ( uint32_t i = 0; i != starts->seg_count; ++i )
                printf("            seg_info_offset[%d]     0x%08X\n", i, starts->seg_info_offset[i]);
            for (uint32_t segIndex = 0; segIndex < starts->seg_count; ++segIndex) {
                if ( starts->seg_info_offset[segIndex] == 0 )
                    continue;
                printf("        dyld_chained_starts_in_segment:\n");
                const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
                printf("            size                0x%08X\n", segInfo->size);
                printf("            page_size           0x%08X\n", segInfo->page_size);
                printf("            pointer_format      0x%08X\n", segInfo->pointer_format);
                printf("            segment_offset      0x%08llX\n", segInfo->segment_offset);
                printf("            max_valid_pointer   0x%08X\n", segInfo->max_valid_pointer);
                printf("            page_count          0x%08X\n", segInfo->page_count);
            }
            printf("        targets:\n");
            chainedFixups.forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
                printf("            symbol          %s\n", target.symbolName.c_str());
            });
        }
    }
    else if ( image.header()->hasFirmwareChainStarts(&fwPointerFormat, &fwStartsCount, &fwStarts) ) {
        const ChainedFixups::PointerFormat& pf = ChainedFixups::PointerFormat::make(fwPointerFormat);
        printf("        firmware chains:\n");
        printf("          pointer_format:  %d (%s)\n", fwPointerFormat, pf.description());
    }
}

static void printSymbolicFixups(const Image& image)
{
    printf("    -symbolic_fixups:\n");

    SymbolicatedImage symImage(image);
    uint64_t lastSymbolBaseAddr = 0;
    for (size_t i=0; i < symImage.fixupCount(); ++i) {
        CString  inSymbolName     = symImage.fixupInSymbol(i);
        uint64_t inSymbolAddress  = symImage.fixupAddress(i);
        uint32_t inSymbolOffset   = symImage.fixupInSymbolOffset(i);
        uint64_t inSymbolBaseAddr = inSymbolAddress - inSymbolOffset;
        if ( inSymbolBaseAddr != lastSymbolBaseAddr )
            printf("%s:\n", inSymbolName.c_str());
        char targetStr[4096];
        printf("           +0x%04X %11s  %s\n",           inSymbolOffset, symImage.fixupTypeString(i), symImage.fixupTargetString(i, true, targetStr));
        lastSymbolBaseAddr = inSymbolBaseAddr;
    }
}

static void printExports(const Image& image)
{
    printf("    -exports:\n");
    printf("        offset      symbol\n");
    if ( image.hasExportsTrie() ) {
        image.exportsTrie().forEachExportedSymbol(^(const Symbol& symbol, bool& stop) {
            uint64_t        resolverFuncOffset;
            uint64_t        absAddress;
            int             libOrdinal;
            const char*     importName;
            const char*     symbolName = symbol.name().c_str();
            if ( symbol.isReExport(libOrdinal, importName) ) {
                char buffer[128];
                if ( strcmp(importName, symbolName) == 0 )
                    printf("        [re-export] %s (from %s)\n", symbolName, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
                else
                    printf("        [re-export] %s (%s from %s)\n", symbolName, importName, SymbolicatedImage::libOrdinalName(image.header(), libOrdinal, buffer));
            }
            else if ( symbol.isAbsolute(absAddress) ) {
                printf("        0x%08llX  %s [absolute]\n", absAddress, symbolName);
            }
            else if ( symbol.isThreadLocal() ) {
                printf("        0x%08llX  %s [per-thread]\n", symbol.implOffset(), symbolName);
            }
            else if ( symbol.isDynamicResolver(resolverFuncOffset) ) {
                printf("        0x%08llX  %s [resolver=0x%08llX]\n", symbol.implOffset(), symbolName, resolverFuncOffset);
            }
            else if ( symbol.isWeakDef() ) {
                printf("        0x%08llX  %s [weak-def]\n", symbol.implOffset(), symbolName);
            }
            else {
                printf("        0x%08llX  %s\n", symbol.implOffset(), symbolName);
            }
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachExportedSymbol(^(const Symbol& symbol, uint32_t symIndex, bool& stop) {
            const char*     symbolName = symbol.name().c_str();
            uint64_t        absAddress;
            if ( symbol.isAbsolute(absAddress) ) {
                printf("        0x%08llX  %s [absolute]\n", absAddress, symbolName);
            }
            else if ( symbol.isWeakDef() ) {
                printf("        0x%08llX  %s [weak-def]\n", symbol.implOffset(), symbolName);
            }
            else {
                printf("        0x%08llX  %s\n", symbol.implOffset(), symbolName);
            }
        });
    }
    else {
        printf("no exported symbol information\n");
    }
}

static void printFixups(const Image& image)
{
    printf("    -fixups:\n");
    SymbolicatedImage symImage(image);
    printf("        segment         section          address             type   target\n");
    for (size_t i=0; i < symImage.fixupCount(); ++i) {
        uint8_t sectNum = symImage.fixupSectNum(i);
        char targetStr[4096];
        printf("        %-12s    %-16s 0x%08llX   %11s  %s\n",
               symImage.fixupSegment(sectNum).c_str(), symImage.fixupSection(sectNum).c_str(),
               symImage.fixupAddress(i), symImage.fixupTypeString(i), symImage.fixupTargetString(i, false, targetStr));
    }
}

static void printObjC(const Image& image)
{
    printf("    -objc:\n");
    // build list of all fixups
    SymbolicatedImage symInfo(image);

    if ( symInfo.fairplayEncryptsSomeObjcStrings() )
        printf("        warning: FairPlay encryption of __TEXT will make printing ObjC info unreliable\n");

    symInfo.forEachDefinedObjCClass(^(uint64_t classVmAddr) {
        char protocols[1024];
        const char* classname = symInfo.className(classVmAddr);
        const char* supername = symInfo.superClassName(classVmAddr);
        symInfo.getProtocolNames(classVmAddr, protocols);
        printf("        @interface %s : %s %s\n", classname, supername, protocols);
        // walk instance methods
        symInfo.forEachMethodInClass(classVmAddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  -[%s %s]\n", implAddr, classname, methodName);
        });
        // walk class methods
        uint64_t metaClassVmaddr = symInfo.metaClassVmAddr(classVmAddr);
        symInfo.forEachMethodInClass(metaClassVmaddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
        });
        printf("        @end\n");
    });

    symInfo.forEachObjCCategory(^(uint64_t categoryVmAddr) {
        const char* catname   = symInfo.categoryName(categoryVmAddr);
        const char* classname = symInfo.categoryClassName(categoryVmAddr);
        printf("        @interface %s(%s)\n", classname, catname);
        symInfo.forEachMethodInCategory(categoryVmAddr, ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  -[%s %s]\n", implAddr, classname, methodName);
        },
                                        ^(const char* methodName, uint64_t implAddr) {
            printf("          0x%08llX  +[%s %s]\n", implAddr, classname, methodName);
        });
        printf("        @end\n");
    });
}



#if 0
static void printSwiftProtocolConformances(const dyld3::MachOAnalyzer* ma,
                                           const DyldSharedCache* dyldCache, size_t cacheLen)
{
    Diagnostics diag;

    __block std::vector<std::string> chainedFixupTargets;
    ma->forEachChainedFixupTarget(diag, ^(int libOrdinal, const char *symbolName, uint64_t addend, bool weakImport, bool &stop) {
        chainedFixupTargets.push_back(symbolName);
    });

    printf("    -swift-proto:\n");
    printf("        address             protocol-target     type-descriptor-target\n");

    uint64_t loadAddress = ma->preferredLoadAddress();

    uint64_t sharedCacheRelativeSelectorBaseVMAddress = dyldCache->sharedCacheRelativeSelectorBaseVMAddress();
    __block metadata_visitor::SwiftVisitor swiftVisitor(dyldCache, ma, VMAddress(sharedCacheRelativeSelectorBaseVMAddress));
    swiftVisitor.forEachProtocolConformance(^(const metadata_visitor::SwiftConformance &swiftConformance, bool &stopConformance) {
        VMAddress protocolConformanceVMAddr = swiftConformance.getVMAddress();
        metadata_visitor::SwiftPointer protocolPtr = swiftConformance.getProtocolPointer(swiftVisitor);
        metadata_visitor::SwiftConformance::SwiftTypeRefPointer typeRef = swiftConformance.getTypeRef(swiftVisitor);
        metadata_visitor::SwiftPointer typePtr = typeRef.getTargetPointer(swiftVisitor);
        const char* protocolConformanceFixup = "";
        const char* protocolFixup = "";
        const char* typeDescriptorFixup = "";

        uint64_t protocolRuntimeOffset = protocolPtr.targetValue.vmAddress().rawValue() - loadAddress;
        uint64_t typeRefRuntimeOffset = typePtr.targetValue.vmAddress().rawValue() - loadAddress;

        // If we have indirect fixups, see if we can find the names
        if ( !protocolPtr.isDirect ) {
            uint8_t* fixup = (uint8_t*)ma + protocolRuntimeOffset;
            const auto* fixupLoc = (const ChainedFixupPointerOnDisk*)fixup;
            uint32_t bindOrdinal = 0;
            int64_t addend = 0;
            if ( fixupLoc->isBind(DYLD_CHAINED_PTR_ARM64E_USERLAND, bindOrdinal, addend) ) {
                protocolFixup = chainedFixupTargets[bindOrdinal].c_str();
            }
        }
        if ( !typePtr.isDirect ) {
            uint8_t* fixup = (uint8_t*)ma + typeRefRuntimeOffset;
            const auto* fixupLoc = (const ChainedFixupPointerOnDisk*)fixup;
            uint32_t bindOrdinal = 0;
            int64_t addend = 0;
            if ( fixupLoc->isBind(DYLD_CHAINED_PTR_ARM64E_USERLAND, bindOrdinal, addend) ) {
                protocolFixup = chainedFixupTargets[bindOrdinal].c_str();
            }
        }
        printf("        0x%016llX(%s)  %s0x%016llX(%s)  %s0x%016llX(%s)\n",
               protocolConformanceVMAddr.rawValue(), protocolConformanceFixup,
               protocolPtr.isDirect ? "" : "*", protocolRuntimeOffset, protocolFixup,
               typePtr.isDirect ? "" : "*", typeRefRuntimeOffset, typeDescriptorFixup);
    });
}
#endif

static void printSharedRegion(const Image& image)
{
    printf("    -shared_region:\n");

    if ( !image.hasSplitSegInfo() ) {
        printf("        no shared region info\n");
        return;
    }

    const SplitSegInfo& splitSeg = image.splitSegInfo();
    if ( splitSeg.isV1() ) {
        printf("        shared region v1\n");
        return;
    }

    if ( splitSeg.hasMarker() ) {
        printf("        no shared region info (marker present)\n");
        return;
    }

    __block std::vector<std::pair<std::string, std::string>> sectionNames;
    __block std::vector<uint64_t>                            sectionVMAddrs;
    sectionNames.emplace_back("","");
    sectionVMAddrs.push_back(0);
    image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        sectionNames.emplace_back(sectInfo.segmentName, sectInfo.sectionName);
        sectionVMAddrs.push_back(sectInfo.address);
    });
    printf("        from      to\n");
    Error err = splitSeg.forEachReferenceV2(^(const SplitSegInfo::Entry& entry, bool& stop) {
        std::string_view fromSegmentName    = sectionNames[(uint32_t)entry.fromSectionIndex].first;
        std::string_view fromSectionName    = sectionNames[(uint32_t)entry.fromSectionIndex].second;
        std::string_view toSegmentName      = sectionNames[(uint32_t)entry.toSectionIndex].first;
        std::string_view toSectionName      = sectionNames[(uint32_t)entry.toSectionIndex].second;
        uint64_t fromVMAddr                 = sectionVMAddrs[(uint32_t)entry.fromSectionIndex] + entry.fromSectionOffset;
        uint64_t toVMAddr                   = sectionVMAddrs[(uint32_t)entry.toSectionIndex]   + entry.toSectionOffset;
        printf("        %-16s %-16s 0x%08llx      %-16s %-16s 0x%08llx\n",
               fromSegmentName.data(), fromSectionName.data(), fromVMAddr,
               toSegmentName.data(), toSectionName.data(), toVMAddr);
    });
}

static void printFunctionStarts(const Image& image)
{
    printf("    -function_starts:\n");
    SymbolicatedImage symImage(image);
    if ( image.hasFunctionStarts() ) {
        uint64_t loadAddress = image.header()->preferredLoadAddress();
        image.functionStarts().forEachFunctionStart(loadAddress, ^(uint64_t addr) {
            const char* name = symImage.symbolNameAt(addr);
            if ( name == nullptr )
                name = "";
            printf("        0x%08llX  %s\n", addr, name);
        });
    }
    else {
        printf("        no function starts info\n");
    }
}

static void printOpcodes(const Image& image)
{
    printf("    -opcodes:\n");
    if ( image.hasRebaseOpcodes() ) {
        printf("        rebase opcodes:\n");
        image.rebaseOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no rebase opcodes\n");
    }
    if ( image.hasBindOpcodes() ) {
        printf("        bind opcodes:\n");
        image.bindOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no bind opcodes\n");
    }
    if ( image.hasLazyBindOpcodes() ) {
        printf("        lazy bind opcodes:\n");
        image.lazyBindOpcodes().printOpcodes(stdout, 10);
    }
    else {
        printf("        no lazy bind opcodes\n");
    }
    // FIXME: add support for weak binds
}

static void printUnwindTable(const Image& image)
{
    printf("    -unwind:\n");
    if ( image.hasCompactUnwind() ) {
        printf("        address       encoding\n");
        uint64_t loadAddress = image.header()->preferredLoadAddress();
        const CompactUnwind& cu = image.compactUnwind();
        cu.forEachUnwindInfo(^(const CompactUnwind::UnwindInfo& info) {
            const void* funcBytes = (uint8_t*)image.header() + info.funcOffset;
            char encodingString[128];
            cu.encodingToString(info.encoding, funcBytes, encodingString);
            char lsdaString[32];
            lsdaString[0] = '\0';
            if ( info.lsdaOffset != 0 )
                snprintf(lsdaString, sizeof(lsdaString), " lsdaOffset=0x%08X", info.lsdaOffset);
            printf("        0x%08llX   0x%08X (%-56s)%s\n", info.funcOffset + loadAddress, info.encoding, encodingString, lsdaString);
        });
    }
    else {
        printf("        no compact unwind table\n");
    }
}

static void dumpHex(SymbolicatedImage& symImage, const Header::SectionInfo& sectInfo, size_t sectNum)
{
    const uint8_t*   sectionContent = symImage.content(sectInfo);
    const uint8_t*   bias           = sectionContent - (long)sectInfo.address;
    uint8_t          sectType       = (sectInfo.flags & SECTION_TYPE);
    bool             isZeroFill     = ((sectType == S_ZEROFILL) || (sectType == S_THREAD_LOCAL_ZEROFILL));
    symImage.forEachSymbolRangeInSection(sectNum, ^(const char* symbolName, uint64_t symbolAddr, uint64_t size) {
        if ( symbolName != nullptr ) {
            if ( (symbolAddr == sectInfo.address) && (strchr(symbolName, ',') != nullptr) ) {
                // don't print synthesized name for section start (e.g. "__DATA_CONST,__auth_ptr")
            }
            else {
                printf("%s:\n", symbolName);
            }
        }
        for (int i=0; i < size; ++i) {
            if ( (i & 0xF) == 0 )
                printf("0x%08llX: ", symbolAddr+i);
            uint8_t byte = (isZeroFill ? 0 : bias[symbolAddr + i]);
            printf("%02X ", byte);
            if ( (i & 0xF) == 0xF )
                printf("\n");
        }
        if ( (size & 0xF) != 0x0 )
            printf("\n");
    });
}

static void disassembleSection(SymbolicatedImage& symImage, const Header::SectionInfo& sectInfo, size_t sectNum)
{
#if HAVE_LIBLTO
    symImage.loadDisassembler();
    if ( symImage.llvmRef() != nullptr ) {
        // disassemble content
        const uint8_t* sectionContent    = symImage.content(sectInfo);
        const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
        const uint8_t* curContent = sectionContent;
        uint64_t       curPC      = sectInfo.address;
        symImage.setSectionContentBias(sectionContent - sectInfo.address);
        while ( curContent < sectionContentEnd ) {
            // add label if there is one for this PC
            if ( const char* symName = symImage.symbolNameAt(curPC) )
                printf("%s:\n", symName);
            char line[256];
            size_t len = LLVMDisasmInstruction(symImage.llvmRef(), (uint8_t*)curContent, sectInfo.size, curPC, line, sizeof(line));
            // llvm disassembler uses tabs to align operands, but that can look wonky, so convert to aligned spaces
            char instruction[16];
            char operands[256];
            char comment[256];
            comment[0] = '\0';
            if ( len == 0 ) {
                uint32_t value32;
                strcpy(instruction, ".long");
                memcpy(&value32, curContent, 4);
                snprintf(operands, sizeof(operands), "0x%08X", value32);
                //printf("0x%08llX \t.long\t0x%08X\n", curPC, value32);
                len = 4;
            }
            else {
                // parse: "\tinstr\toperands"
                if ( char* secondTab = strchr(&line[1],'\t') ) {
                    size_t instrLen = secondTab - &line[1];
                    if ( instrLen < sizeof(instruction) ) {
                        memcpy(instruction, &line[1], instrLen);
                        instruction[instrLen] = '\0';
                    }
                    // llvm diassembler addens wonky comments like "literal pool symbol address", improve wording
                    strlcpy(operands, &line[instrLen+2], sizeof(operands));
                    if ( char* literalComment = strstr(operands, "; literal pool symbol address: ") ) {
                        strlcpy(comment, "; ", sizeof(comment));
                        strlcat(comment, literalComment+31,sizeof(comment));
                        literalComment[0] = '\0'; // truncate operands
                    }
                    else if ( char* literalComment2 = strstr(operands, "## literal pool symbol address: ") ) {
                        strlcpy(comment, "; ", sizeof(comment));
                        strlcat(comment, literalComment2+32,sizeof(comment));
                        literalComment2[0] = '\0'; // truncate operands
                    }
                    else if ( char* literalComment3 = strstr(operands, "## literal pool for: ") ) {
                        strlcpy(comment, "; string literal: ", sizeof(comment));
                        strlcat(comment, literalComment3+21,sizeof(comment));
                        literalComment3[0] = '\0'; // truncate operands
                    }
                    else if ( char* numberComment = strstr(operands, "; 0x") ) {
                        strlcpy(comment, numberComment, sizeof(comment));
                        numberComment[0] = '\0';  // truncate operands
                    }
                }
                else {
                    strlcpy(instruction, &line[1], sizeof(instruction));
                    operands[0] = '\0';
                }
                printf("0x%09llX   %-8s %-20s %s\n", curPC, instruction, operands, comment);
            }
            curContent += len;
            curPC      += len;
        }
        return;
    }
#endif
    // disassembler not available, dump code in hex
    dumpHex(symImage, sectInfo, sectNum);
}

static void printQuotedString(const char* str)
{
    if ( (strchr(str, '\n') != nullptr) || (strchr(str, '\t') != nullptr) ) {
        printf("\"");
        for (const char* s=str; *s != '\0'; ++s) {
            if ( *s == '\n' )
                printf("\\n");
            else if ( *s == '\t' )
                printf("\\t");
            else
                printf("%c", *s);
        }
        printf("\"");
    }
    else {
        printf("\"%s\"", str);
    }
}

static void dumpCStrings(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const char* sectionContent  = (char*)symInfo.content(sectInfo);
    const char* stringStart     = sectionContent;
    for (int i=0; i < sectInfo.size; ++i) {
        if ( sectionContent[i] == '\0' ) {
            if ( *stringStart != '\0' ) {
                printf("0x%08llX ", sectInfo.address + i);
                printQuotedString(stringStart);
                printf("\n");
            }
            stringStart = &sectionContent[i+1];
        }
    }
}

static void dumpCFStrings(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const size_t   cfStringSize      = symInfo.is64() ? 32 : 16;
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        printf("0x%08llX\n", curAddr);
        const Fixup::BindTarget* bindTarget;
        if ( symInfo.isBind(sectionContent, bindTarget) ) {
            printf("    class: %s\n", bindTarget->symbolName.c_str());
            printf("    flags: 0x%08X\n", *((uint32_t*)(&curContent[cfStringSize/4])));
            uint64_t stringVmAddr;
            if ( symInfo.isRebase(&curContent[cfStringSize/2], stringVmAddr) ) {
                if ( const char* str = symInfo.cStringAt(stringVmAddr) ) {
                    printf("   string: ");
                    printQuotedString(str);
                    printf("\n");
                }
            }
            printf("   length: %u\n", *((uint32_t*)(&curContent[3*cfStringSize/4])));
        }
        curContent += cfStringSize;
        curAddr    += cfStringSize;
    }
}

static void dumpGOT(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        const Fixup::BindTarget* bindTarget;
        uint64_t                 rebaseTargetVmAddr;
        const char*              targetName = "";
        printf("0x%08llX  ", curAddr);
        if ( symInfo.isBind(curContent, bindTarget) ) {
            printf("%s\n", bindTarget->symbolName.c_str());
        }
        else if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            targetName = symInfo.symbolNameAt(rebaseTargetVmAddr);
            if ( targetName != nullptr )
                printf("%s\n", targetName);
            else
                printf("0x%08llX\n", rebaseTargetVmAddr);
        }
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}

static void dumpClassPointers(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        uint64_t   rebaseTargetVmAddr;
        if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            printf("0x%08llX:  0x%08llX ", curAddr, rebaseTargetVmAddr);
            if ( const char* targetName = symInfo.symbolNameAt(rebaseTargetVmAddr) )
                printf("%s", targetName);
            printf("\n");
        }
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}


static void dumpStringPointers(const SymbolicatedImage& symInfo, const Header::SectionInfo& sectInfo)
{
    const uint8_t* sectionContent    = symInfo.content(sectInfo);
    const uint8_t* sectionContentEnd = sectionContent + sectInfo.size;
    const uint8_t* curContent        = sectionContent;
    uint64_t       curAddr           = sectInfo.address;
    while ( curContent < sectionContentEnd ) {
        uint64_t   rebaseTargetVmAddr;
        printf("0x%08llX  ", curAddr);
        if ( symInfo.isRebase(curContent, rebaseTargetVmAddr) ) {
            if ( const char* selector = symInfo.cStringAt(rebaseTargetVmAddr) )
                printQuotedString(selector);
        }
        printf("\n");
        curContent += symInfo.ptrSize();
        curAddr    += symInfo.ptrSize();
    }
}

static void printDisassembly(const Image& image)
{
    __block SymbolicatedImage symImage(image);
    __block size_t sectNum = 1;
    image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.flags & (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS) ) {
            printf("(%s,%s) section:\n", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
            disassembleSection(symImage, sectInfo, sectNum);
        }
        ++sectNum;
    });
}

static void usage()
{
    fprintf(stderr, "Usage: dyld_info [-arch <arch>]* <options>* <mach-o file>+ | -all_dir <dir> \n"
            "\t-platform                   print platform (default if no options specified)\n"
            "\t-segments                   print segments (default if no options specified)\n"
            "\t-linked_dylibs              print all dylibs this image links against (default if no options specified)\n"
            "\t-inits                      print initializers\n"
            "\t-fixups                     print locations dyld will rebase/bind\n"
            "\t-exports                    print all exported symbols\n"
            "\t-imports                    print all symbols needed from other dylibs\n"
            "\t-fixup_chains               print info about chain format and starts\n"
            "\t-fixup_chain_details        print detailed info about every fixup in chain\n"
            "\t-fixup_chain_header         print detailed info about the fixup chains header\n"
            "\t-symbolic_fixups            print ranges of each atom of DATA with symbol name and fixups\n"
          //"\t-swift_protocols            print swift protocols\n"
            "\t-objc                       print objc classes, categories, etc\n"
            "\t-shared_region              print shared cache (split seg) info\n"
            "\t-function_starts            print function starts information\n"
            "\t-opcodes                    print opcodes information\n"
            "\t-uuid                       print UUID of binary\n"
            "\t-disassemble                print all code sections using disassembler\n"
            "\t-section <seg> <sect>       print content of section, formatted by section type\n"
            "\t-all_sections               print content of all sections, formatted by section type\n"
            "\t-section_bytes <seg> <sect> print content of section, as raw hex bytes\n"
            "\t-all_sections_bytes         print content of all sections, formatted as raw hex bytes\n"
            "\t-validate_only              only prints an malformedness about file(s)\n"
        );
}

struct SegSect { std::string_view segmentName; std::string_view sectionName; };
typedef std::vector<SegSect> SegSectVector;

static bool hasSegSect(const SegSectVector& vec, const Header::SectionInfo& sectInfo) {
    for (const SegSect& ss : vec) {
        if ( (ss.segmentName == sectInfo.segmentName) && (ss.sectionName == sectInfo.sectionName) )
            return true;
    }
    return false;
}


struct PrintOptions
{
    bool            platform            = false;
    bool            segments            = false;
    bool            linkedDylibs        = false;
    bool            initializers        = false;
    bool            exports             = false;
    bool            imports             = false;
    bool            fixups              = false;
    bool            fixupChains         = false;
    bool            fixupChainDetails   = false;
    bool            fixupChainHeader    = false;
    bool            symbolicFixups      = false;
    bool            objc                = false;
    bool            swiftProtocols      = false;
    bool            sharedRegion        = false;
    bool            functionStarts      = false;
    bool            opcodes             = false;
    bool            unwind              = false;
    bool            uuid                = false;
    bool            disassemble         = false;
    bool            allSections         = false;
    bool            allSectionsHex      = false;
    bool            validateOnly        = false;
    SegSectVector   sections;
    SegSectVector   sectionsHex;
};


int main(int argc, const char* argv[])
{
    if ( argc == 1 ) {
        usage();
        return 0;
    }

    bool                             someOptionSpecified = false;
    PrintOptions                     printOptions;
    __block std::vector<const char*> files;
            std::vector<const char*> cmdLineArchs;
    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-platform") == 0 ) {
            printOptions.platform = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-segments") == 0 ) {
            printOptions.segments = true;
            someOptionSpecified = true;
        }
        else if ( (strcmp(arg, "-linked_dylibs") == 0) || (strcmp(arg, "-dependents") == 0) ) {
            printOptions.linkedDylibs = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-inits") == 0 ) {
            printOptions.initializers = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixups") == 0 ) {
            printOptions.fixups = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chains") == 0 ) {
            printOptions.fixupChains = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chain_details") == 0 ) {
            printOptions.fixupChainDetails = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-fixup_chain_header") == 0 ) {
            printOptions.fixupChainHeader = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-symbolic_fixups") == 0 ) {
            printOptions.symbolicFixups = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-exports") == 0 ) {
            printOptions.exports = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-imports") == 0 ) {
            printOptions.imports = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-objc") == 0 ) {
            printOptions.objc = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-swift_protocols") == 0 ) {
            printOptions.swiftProtocols = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-shared_region") == 0 ) {
            printOptions.sharedRegion = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-function_starts") == 0 ) {
            printOptions.functionStarts = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-opcodes") == 0 ) {
            printOptions.opcodes = true;
        }
        else if ( strcmp(arg, "-unwind") == 0 ) {
            printOptions.unwind = true;
        }
        else if ( strcmp(arg, "-uuid") == 0 ) {
            printOptions.uuid = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-disassemble") == 0 ) {
            printOptions.disassemble = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-section") == 0 ) {
            const char* segName  = argv[++i];
            const char* sectName = argv[++i];
            if ( !segName || !sectName ) {
                fprintf(stderr, "-section requires segment-name and section-name");
                return 1;
            }
            printOptions.sections.push_back({segName, sectName});
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-all_sections") == 0 ) {
            printOptions.allSections = true;
        }
        else if ( strcmp(arg, "-section_bytes") == 0 ) {
            const char* segName  = argv[++i];
            const char* sectName = argv[++i];
            if ( !segName || !sectName ) {
                fprintf(stderr, "-section_bytes requires segment-name and section-name");
                return 1;
            }
            printOptions.sectionsHex.push_back({segName, sectName});
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-all_sections_bytes") == 0 ) {
            printOptions.allSectionsHex = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-validate_only") == 0 ) {
            printOptions.validateOnly = true;
            someOptionSpecified = true;
        }
        else if ( strcmp(arg, "-arch") == 0 ) {
            if ( ++i < argc ) {
                cmdLineArchs.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-arch missing architecture name");
                return 1;
            }
        }
        else if ( strcmp(arg, "-all_dir") == 0 ) {
            if ( ++i < argc ) {
                const char* searchDir = argv[i];
                iterateDirectoryTree("", searchDir, ^(const std::string& dirPath) { return false; },
                                     ^(const std::string& path, const struct stat& statBuf) { if ( statBuf.st_size > 4096 ) files.push_back(strdup(path.c_str())); },
                                     true /* process files */, true /* recurse */);

            }
            else {
                fprintf(stderr, "-all_dir directory");
                return 1;
            }
        }
        else if ( strcmp(arg, "-all_dyld_cache") == 0 ) {
            size_t                  cacheLen;
            const DyldSharedCache*  dyldCache   = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLen);
            dyldCache->forEachImage(^(const mach_header* mh, const char* installName) {
                files.push_back(installName);
            });
        }
        else if ( arg[0] == '-' ) {
            fprintf(stderr, "dyld_info: unknown option: %s\n", arg);
            return 1;
        }
        else {
            files.push_back(arg);
        }
    }

    // check some files specified
    if ( files.size() == 0 ) {
        usage();
        return 0;
    }

    // if no options specified, use default set
    if ( !someOptionSpecified ) {
        printOptions.platform     = true;
        printOptions.uuid         = true;
        printOptions.segments     = true;
        printOptions.linkedDylibs = true;
    }

    __block bool sliceFound = false;
    forSelectedSliceInPaths(files, cmdLineArchs, ^(const char* path, const Header* header, size_t sliceLen) {
        if ( header == nullptr )
            return; // non-mach-o file found
        sliceFound = true;
        printf("%s [%s]:\n", path, header->archName());
        if ( header->isObjectFile() )
            return;
        Image image((void*)header, sliceLen, (header->inDyldCache() ? Image::MappingKind::dyldLoadedPostFixups : Image::MappingKind::wholeSliceMapped));
        if ( Error err = image.validate() ) {
            printf("   %s\n", err.message());
            return;
        }
        if ( !printOptions.validateOnly ) {
            if ( printOptions.platform )
                printPlatforms(image.header());

            if ( printOptions.uuid )
                printUUID(image.header());

            if ( printOptions.segments )
                 printSegments(image.header());

            if ( printOptions.linkedDylibs )
                printLinkedDylibs(image.header());

            if ( printOptions.initializers )
                printInitializers(image);

            if ( printOptions.exports )
                printExports(image);

            if ( printOptions.imports )
                printImports(image);

            if ( printOptions.fixups )
                printFixups(image);

            if ( printOptions.fixupChains )
                printChainInfo(image);

            if ( printOptions.fixupChainDetails )
                printChainDetails(image);

            if ( printOptions.fixupChainHeader )
                printChainHeader(image);

            if ( printOptions.symbolicFixups )
                printSymbolicFixups(image);

            if ( printOptions.opcodes )
                printOpcodes(image);

            if ( printOptions.functionStarts )
                printFunctionStarts(image);

            if ( printOptions.unwind )
                printUnwindTable(image);

            if ( printOptions.objc )
                printObjC(image);

            // FIXME: implement or remove
            //if ( printOptions.swiftProtocols )
            //    printSwiftProtocolConformances(ma, dyldCache, cacheLen);

            if ( printOptions.sharedRegion )
                printSharedRegion(image);

            if ( printOptions.disassemble )
                printDisassembly(image);

            if ( printOptions.allSections || !printOptions.sections.empty() ) {
                __block SymbolicatedImage symImage(image);
                __block size_t sectNum = 1;
                image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                    if ( printOptions.allSections || hasSegSect(printOptions.sections, sectInfo) ) {
                        printf("(%s,%s) section:\n", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
                        if ( sectInfo.flags & (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS) ) {
                            disassembleSection(symImage, sectInfo, sectNum);
                        }
                        else if ( (sectInfo.flags & SECTION_TYPE) == S_CSTRING_LITERALS ) {
                            dumpCStrings(symImage, sectInfo);
                        }
                        else if ( (sectInfo.flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS ) {
                            dumpGOT(image, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__cfstring") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpCFStrings(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_classrefs") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpGOT(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_classlist") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpClassPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_catlist") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpClassPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__objc_selrefs") && sectInfo.segmentName.starts_with("__DATA") ) {
                            dumpStringPointers(symImage, sectInfo);
                        }
                        else if ( (sectInfo.sectionName == "__info_plist") && sectInfo.segmentName.starts_with("__TEXT") ) {
                            dumpCStrings(image, sectInfo);
                        }
                        // FIXME: other section types
                        else {
                            dumpHex(symImage, sectInfo, sectNum);
                        }
                    }
                    ++sectNum;
                });
            }

            if ( printOptions.allSectionsHex || !printOptions.sectionsHex.empty() ) {
                __block SymbolicatedImage symImage(image);
                __block size_t sectNum = 1;
                image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
                    if ( printOptions.allSectionsHex || hasSegSect(printOptions.sectionsHex, sectInfo) ) {
                        printf("(%s,%s) section:\n", sectInfo.segmentName.c_str(), sectInfo.sectionName.c_str());
                        dumpHex(symImage, sectInfo, sectNum);
                    }
                    ++sectNum;
                });
            }
        }
    });

    if ( !sliceFound && (files.size() == 1) ) {
        if ( cmdLineArchs.empty() ) {
            fprintf(stderr, "dyld_info: '%s' file not found\n", files[0]);
            // FIXME: projects compatibility (rdar://121555064)
            if ( printOptions.linkedDylibs )
                return 0;
        }
        else
            fprintf(stderr, "dyld_info: '%s' does not contain specified arch(s)\n", files[0]);
        return 1;
    }

    return 0;
}



