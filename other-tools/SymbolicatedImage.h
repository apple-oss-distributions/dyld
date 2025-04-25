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

#include "Header.h"
#include "Image.h"

using mach_o::Header;
using mach_o::Image;
using mach_o::MappedSegment;
using mach_o::Symbol;
using mach_o::Fixup;

// common
#include "MachODefines.h"
#include "FileUtils.h"

// libLTO.dylib is only built for macOS
#if TARGET_OS_OSX && BUILDING_DYLDINFO
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
#endif

namespace other_tools {

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
    uint64_t        prefLoadAddress() const { return _prefLoadAddress; }
    void            forEachDefinedObjCClass(void (^callback)(uint64_t classVmAddr)) const;
    void            forEachObjCCategory(void (^callback)(uint64_t categoryVmAddr)) const;
    void            forEachObjCProtocol(void (^callback)(uint64_t protocolVmAddr)) const;
    const char*     className(uint64_t classVmAddr) const;
    const char*     superClassName(uint64_t classVmAddr) const;
    uint64_t        metaClassVmAddr(uint64_t classVmAddr) const;
    void            getProtocolNames(uint64_t protocolListFieldAddr, char names[1024]) const;
    void            getClassProtocolNames(uint64_t classVmAddr, char names[1024]) const;
    const char*     categoryName(uint64_t categoryVmAddr) const;
    const char*     categoryClassName(uint64_t categoryVmAddr) const;
    const char*     protocolName(uint64_t protocolVmAddr) const;
    void            getProtocolProtocolNames(uint64_t protocolVmAddr, char names[1024]) const;
    void            forEachMethodInList(uint64_t methodListVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const;
    void            forEachMethodInClass(uint64_t classVmAddr, void (^callback)(const char* methodName, uint64_t implAddr)) const;
    void            forEachMethodInCategory(uint64_t categoryVmAddr, void (^instanceCallback)(const char* methodName, uint64_t implAddr),
                                            void (^classCallback)(const char* methodName, uint64_t implAddr)) const;
    void            forEachMethodInProtocol(uint64_t protocolVmAddr,
                                            void (^instanceCallback)(const char* methodName),
                                            void (^classCallback)(const char* methodName),
                                            void (^optionalInstanceCallback)(const char* methodName),
                                            void (^optionalClassCallback)(const char* methodName)) const;
    const char*     selectorFromObjCStub(uint64_t sectionVmAdr, const uint8_t* sectionContent, uint32_t& offset) const;

    const uint8_t*  content(const Header::SectionInfo& sectInfo) const;
    void            findClosestSymbol(uint64_t runtimeOffset, const char*& inSymbolName, uint32_t& inSymbolOffset) const;
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
    std::string_view    fixupSegment(size_t sectNum) const { return _sectionSymbols[sectNum-1].sectInfo.segmentName; }
    std::string_view    fixupSection(size_t sectNum) const { return _sectionSymbols[sectNum-1].sectInfo.sectionName; }


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
    void            addDyldCacheBasedFixups();
    void            addFixup(const Fixup& fixup);

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
} // namespace other_tools
