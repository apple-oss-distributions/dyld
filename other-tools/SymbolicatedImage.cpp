/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2018-2024 Apple Inc. All rights reserved.
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

#include "SymbolicatedImage.h"

// OS
#include <SoftLinking/WeakLinking.h>

// mach-o
#include "Instructions.h"

// llvm
#if HAVE_LIBLTO
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

using mach_o::Architecture;
using mach_o::Instructions;

namespace other_tools {

SymbolicatedImage::SymbolicatedImage(const Image& im)
: _image(im), _is64(im.header()->is64()), _ptrSize(_is64 ? 8 : 4), _prefLoadAddress(im.header()->preferredLoadAddress())
{
    // build list of sections
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        std::string sectName(sectInfo.segmentName);
        sectName += ",";
        sectName += sectInfo.sectionName;
        _sectionSymbols.push_back({sectName, sectInfo});
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
            if ( !symbol.isAbsolute(absAddress) && (symbol.implOffset() != 0) && symbol.sectionOrdinal() < _sectionSymbols.size() ) {
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
                    if ( (symbolIndex & (INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS)) == 0 && symbolIndex < symbolNames.size() )
                        _symbolsMap[sectInfo.address + stubSize*i] = symbolNames[symbolIndex];
                }
            }
            else if ( type == S_NON_LAZY_SYMBOL_POINTERS ) {
                int gotsIndirectStartIndex = sectInfo.reserved1;
                int gotsIndirectCount      = (int)(sectInfo.size / 8);  // FIXME: arm64_32
                for (int i=0; i < gotsIndirectCount; ++i) {
                    uint32_t symbolIndex = indirectTable[gotsIndirectStartIndex+i];
                    if ( (symbolIndex & (INDIRECT_SYMBOL_LOCAL|INDIRECT_SYMBOL_ABS)) == 0 && symbolIndex < symbolNames.size() )
                        _symbolsMap[sectInfo.address + 8*i] = symbolNames[symbolIndex];
                }
            }
        });
    }
    // options like -T in strip removes global Swift symbols from the nlist, but they'll
    // still present in the exports trie so we should parse both
    if ( _image.hasExportsTrie() ) {
        _image.exportsTrie().forEachExportedSymbol(^(const Symbol &symbol, bool &stop) {
            uint64_t absAddress=0;
            int ordinal=0;
            const char* importName = nullptr;
            if ( symbol.isAbsolute(absAddress) || symbol.isReExport(ordinal, importName) )
                return;

            uint64_t addr = _prefLoadAddress+symbol.implOffset();
            for ( SectionSymbols& sect : _sectionSymbols ) {
                if ( addr >= sect.sectInfo.address && addr < (sect.sectInfo.address+sect.sectInfo.size) ) {
                    if ( auto it = _symbolsMap.find(addr); it == _symbolsMap.end() ) {
                        const char* copiedSym = strdup(symbol.name().c_str());
                        sect.symbols.push_back({ addr-sect.sectInfo.address, copiedSym });
                        _symbolsMap[addr] = copiedSym;
                    } else if ( CString(it->second) != symbol.name() ) {
                        // different symbol names at the same location
                        sect.symbols.push_back({ addr-sect.sectInfo.address, strdup(symbol.name().c_str()) });
                    }
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
    if ( image().header()->inDyldCache() ) {
        // images in dyld cache have no fixup info in LINKEDIT
        addDyldCacheBasedFixups();
    }
    else {
        // on disk image can use regular fixup inspection
        _image.forEachBindTarget(^(const Fixup::BindTarget& target, bool& stop) {
            _fixupTargets.push_back(target);
        });
        _image.forEachFixup(^(const Fixup& fixup, bool& stop) {
            this->addFixup(fixup);
        });
    }
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


void SymbolicatedImage::addDyldCacheBasedFixups()
{
    // FIXME:
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
                inSymbolName   = ss.sectStartName.c_str();
                inSymbolOffset = (uint32_t)(runtimeOffset - ss.sectInfo.address);
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

// if object conforms to protocols, return string of protocol names, eg. "<NSFoo, NSBar>"
void SymbolicatedImage::getProtocolNames(uint64_t protocolListFieldAddr, char names[1024]) const
{
    if ( const void* baseProtocolsFieldContent = this->locationFromVmAddr(protocolListFieldAddr) ) {
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

// if class conforms to protocols, return string of protocol names, eg. "<NSFoo, NSBar>"
void SymbolicatedImage::getClassProtocolNames(uint64_t classVmAddr, char names[1024]) const
{
    names[0] = '\0';
    uint64_t roDataFieldAddr = classVmAddr + 4*_ptrSize;
    if ( const void* roDataFieldContent = this->locationFromVmAddr(roDataFieldAddr) ) {
        uint64_t roDataVmAddr;
        if ( this->isRebase(roDataFieldContent, roDataVmAddr) ) {
            roDataVmAddr &= -4; // remove swift bits
            uint64_t baseProtocolsFieldAddr = roDataVmAddr + ((_ptrSize==8) ? 40 : 24);
            getProtocolNames(baseProtocolsFieldAddr, names);
        }
    }
}

// if protocol conforms to protocols, return string of protocol names, eg. "<NSFoo, NSBar>"
void SymbolicatedImage::getProtocolProtocolNames(uint64_t protocolVmAddr, char names[1024]) const
{
    names[0] = '\0';
    uint64_t baseProtocolsFieldAddr = protocolVmAddr + 2*_ptrSize;
    getProtocolNames(baseProtocolsFieldAddr, names);
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

const char* SymbolicatedImage::protocolName(uint64_t protocolVmAddr) const
{
    uint64_t protocolNameFieldAddr = protocolVmAddr + _ptrSize;
    if ( const void* protocolNameFieldContent = this->locationFromVmAddr(protocolNameFieldAddr) ) {
        uint64_t nameVmAddr;
        if ( this->isRebase(protocolNameFieldContent, nameVmAddr) ) {
            if ( const char* protocolName = this->cStringAt(nameVmAddr) ) {
                return protocolName;
            }
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

void SymbolicatedImage::forEachMethodInProtocol(uint64_t protocolVmAddr,
                                                void (^instanceCallback)(const char* methodName),
                                                void (^classCallback)(const char* methodName),
                                                void (^optionalInstanceCallback)(const char* methodName),
                                                void (^optionalClassCallback)(const char* methodName)) const
{
    uint64_t instanceMethodsFieldAddr = protocolVmAddr + 3*_ptrSize;
    if ( const void* instanceMethodsFieldContent = this->locationFromVmAddr(instanceMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(instanceMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, ^(const char* methodName, uint64_t implAddr) {
                instanceCallback(methodName);
            });
        }
    }
    uint64_t classMethodsFieldAddr = protocolVmAddr + 4*_ptrSize;
    if ( const void* classMethodsFieldContent = this->locationFromVmAddr(classMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(classMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, ^(const char* methodName, uint64_t implAddr) {
                classCallback(methodName);
            });
        }
    }
    uint64_t optionalInstanceMethodsFieldAddr = protocolVmAddr + 5*_ptrSize;
    if ( const void* instanceMethodsFieldContent = this->locationFromVmAddr(optionalInstanceMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(instanceMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, ^(const char* methodName, uint64_t implAddr) {
                optionalInstanceCallback(methodName);
            });
        }
    }
    uint64_t optionalClassMethodsFieldAddr = protocolVmAddr + 6*_ptrSize;
    if ( const void* classMethodsFieldContent = this->locationFromVmAddr(optionalClassMethodsFieldAddr) ) {
        uint64_t  methodListVmAddr;
        if ( this->isRebase(classMethodsFieldContent, methodListVmAddr) ) {
            this->forEachMethodInList(methodListVmAddr, ^(const char* methodName, uint64_t implAddr) {
                optionalClassCallback(methodName);
            });
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

void SymbolicatedImage::forEachObjCProtocol(void (^callback)(uint64_t protocolVmAddr)) const
{
    _image.header()->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.sectionName == "__objc_protolist" ) {
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
} // namespace other_tools
