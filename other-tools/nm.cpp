/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2025 Apple Inc. All rights reserved.
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

#include <stdarg.h>
#include <stdio.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <mach-o/dyld_priv.h>

// c++ stl
#include <vector>
#include <string>

// common
#include "Defines.h"
#include "Array.h"

// mach_o
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "NListSymbolTable.h"

// other_tools
#include "MiscFileUtils.h"
#include "Tool.h"
#include "nm.h"


using mach_o::UnsafeHeader;
using mach_o::Image;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::NListSymbolTable;


#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::nm tool;
    return tool.run(argc, argv, envp);
}
#endif


namespace other_tools {

void nm::usage() const
{
    fprintf(stderr, "Usage: %s [-arch <arch>]* <options>* <mach-o file>+ \n"
            "\t-a     Display all symbol table entries, including those inserted for use by debuggers\n"
            "\t-g     Display only global (external) symbols\n"
            "\t-m     Symbol details are displayed in a human-friendly manner\n"
            "\t-n     Sort by address rather than by symbol name\n"
            "\t-p     Don't sort, display in symbol-table order\n"
            "\t-u     Display only undefined symbols\n"
            "\t-U     Don't display undefined symbols\n"
            "\t-x     Display the symbol table entry's fields in hexadecimal, along with the name as a string\n"
            "\t-j     Just display the symbol names (no value or type).\n",
            __progname);
}

static bool isUndefinedSymbol(uint8_t n_type)
{
    return ((n_type & N_TYPE) ==  N_UNDF);
}

void nm::sortSymbols(std::vector<NListEntry>& symbols) const
{
    switch ( _sortOrder ) {
    case sortByName:
        std::sort(symbols.begin(), symbols.end(), [this](const NListEntry& l, const NListEntry& r) {
            if ( _reverseSort )
                return (strcmp(l._symbolName, r._symbolName) > 0);
            else
                return (strcmp(l._symbolName, r._symbolName) < 0);
        });
        break;
    case sortByAddress:
        std::sort(symbols.begin(), symbols.end(), [this](const NListEntry& l, const NListEntry& r) {
            if ( l._value == r._value ) {
                // in -n mode, sort all undefs by name
                // if two defined symbols at same address, sort by name
                bool lundef = isUndefinedSymbol(l._type);
                bool rundef = isUndefinedSymbol(r._type);
                if ( lundef == rundef ) {
                    if ( _reverseSort )
                        return (strcmp(l._symbolName, r._symbolName) > 0);
                    else
                        return (strcmp(l._symbolName, r._symbolName) < 0);
                }
                if ( _reverseSort )
                    return rundef; // in -nr mode, sort undefs to start of output
                else
                    return lundef; // in -n mode, sort undefs to end of output
            }
            else {
                if ( _reverseSort )
                    return (l._value > r._value);
                else {
                    return (l._value < r._value);
                }
            }
        });
        break;
    case sortSymbolOrder:
        break;
    }
}


nm::NListEntry::NListEntry(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc)
 : _symbolName(symbolName), _value(n_value), _type(n_type), _sect(n_sect), _desc(n_desc)
{
}

void nm::NListEntry::printRegular(const ImageInfo& imageInfo, std::span<char> outBuf) const
{
    bool printAddress = true;
    char c = '?';
    switch (_type & N_TYPE) {
        case N_UNDF:
            if ( _value == 0 ) {
                c = 'U';
                printAddress = false;
            }
            else {
                c = 'C';
            }
            break;
        case N_ABS:
            c = 'A';
            break;
        case N_SECT:
            c = imageInfo.sectionLetters[_sect-1].letter;
            break;
    }
    if ( (_type & N_EXT) == 0 )
        c = tolower(c);
    if ( printAddress  ) {
        if ( imageInfo.is64 )
            snprintf(outBuf.data(), outBuf.size(), "%016llx %c %s", _value, c, _symbolName);
        else
            snprintf(outBuf.data(), outBuf.size(), "%08llx %c %s", _value, c, _symbolName);
    }
    else {
        if ( imageInfo.is64 )
            snprintf(outBuf.data(), outBuf.size(), "                 %c %s", c, _symbolName);
        else
            snprintf(outBuf.data(), outBuf.size(), "         %c %s", c, _symbolName);
    }
}

const char* nm::NListEntry::verboseSymbolSection(const ImageInfo& imageInfo) const
{
    switch (_type & N_TYPE) {
        case N_UNDF:
            return (_value != 0) ? "common" : "undefined";
        case N_ABS:
            return "absolute";
        case N_SECT:
            return imageInfo.sectionLetters[_sect-1].name.c_str();
    }
    return "???";
}

std::string nm::NListEntry::verboseSymbolFlags(const ImageInfo& imageInfo) const
{
    std::string flags;
    if ( _type & N_EXT ) {
        if ( _type & N_PEXT ) {
            if ( _desc & N_WEAK_DEF )
                flags = "weak private external ";  // weak-def
            else
                flags = "private external ";
        }
        else {
            if ( _desc & N_WEAK_DEF ) {
                if ( _desc & N_WEAK_REF )
                    flags = "weak external automatically hidden ";
                else
                    flags = "weak external ";   // weak-def
            }
            else if ( _desc & N_WEAK_REF ) {
                flags = "weak external ";       // weak-import
            }
            else {
                flags = "external ";
            }
        }
    }
    else {
        if ( _type & N_PEXT )
            flags = "non-external (was a private external) ";
        else
            flags = "non-external ";
    }
    if ( imageInfo.isObjectFile ) {
        if ( isUndefinedSymbol(_type) && (_value != 0) ) {
            // in tentative definitions, alignment info stored in desc field
            if ( int align = GET_COMM_ALIGN(_desc) ) {
                char alignStr[32];
                snprintf(alignStr, sizeof(alignStr), "(alignment 2^%d) ", align);
                flags.insert(0, alignStr);
            }
        }
        else {
            if ( _desc & N_NO_DEAD_STRIP )
                flags += "[no dead strip] ";

            if ( (_desc & N_SYMBOL_RESOLVER) && (_type != N_UNDF) )
                flags += "[symbol resolver] ";

            if ( (_desc & N_ALT_ENTRY) && (_type != N_UNDF) )
                flags += "[alt entry] ";

            if ( (_desc & N_COLD_FUNC) && (_type != N_UNDF) )
                flags += "[cold func] ";

            if ( (_desc & N_ARM_THUMB_DEF) && (_type != N_UNDF) )
                flags += "[Thumb] ";
        }
    }

    return flags;
}

std::string nm::NListEntry::verboseTwoLevelImport(const ImageInfo& imageInfo) const
{
    uint8_t libOrdinal = GET_LIBRARY_ORDINAL(_desc);
    switch ( libOrdinal) {
        case EXECUTABLE_ORDINAL:
            return "main-executable";
        case DYNAMIC_LOOKUP_ORDINAL:
            return "flat-namespace";
        case SELF_LIBRARY_ORDINAL:
            return "this-image";
    }
    if ( libOrdinal < MAX_LIBRARY_ORDINAL ) {
        if ( libOrdinal > imageInfo.imports.size() )
            return "ordinal-too-large";
        return imageInfo.imports[libOrdinal-1];
    }
    return "unknown-ordinal";
}

const char* nm::NListEntry::stabName() const
{
    switch (_type) {
        case N_BNSYM:
            return "BNSYM";
        case N_ENSYM:
            return "ENSYM";
        case N_GSYM:
            return "GSYM";
        case N_SO:
            return "SO";
        case N_OSO:
            return "OSO";
        case N_LIB:
            return "LIB";
        case N_FUN:
            return "FUN";
        case N_STSYM:
            return "STSYM";
    }
    return "??";
}


void nm::NListEntry::printVerbose(const ImageInfo& imageInfo, std::span<char> outBuf) const
{
    if ( _type & N_STAB ) {
        if ( imageInfo.is64 )
            snprintf(outBuf.data(), outBuf.size(), "%016llx - %02x %04X %5s %s", _value, _sect, _desc, stabName(), _symbolName);
        else
            snprintf(outBuf.data(), outBuf.size(), "%08llx - %02x %04X %5s %s",  _value, _sect, _desc, stabName(), _symbolName);
    }
    else {
        const char* sectionStr = verboseSymbolSection(imageInfo);
        std::string flags      = verboseSymbolFlags(imageInfo);
        if ( isUndefinedSymbol(_type) && (_value == 0) ) {
            if ( imageInfo.isObjectFile ) {
                if ( imageInfo.is64 )
                    snprintf(outBuf.data(), outBuf.size(), "                 (%s) %s%s", sectionStr, flags.c_str(), _symbolName);
                else
                    snprintf(outBuf.data(), outBuf.size(), "         (%s) %s%s", sectionStr, flags.c_str(), _symbolName);
            }
            else {
                if ( imageInfo.is64 )
                    snprintf(outBuf.data(), outBuf.size(), "                 (%s) %s%s (from %s)", sectionStr, flags.c_str(), _symbolName, verboseTwoLevelImport(imageInfo).c_str());
                else
                    snprintf(outBuf.data(), outBuf.size(), "         (%s) %s%s (from %s)", sectionStr, flags.c_str(), _symbolName, verboseTwoLevelImport(imageInfo).c_str());
            }
        }
        else {
            if ( imageInfo.is64 )
                snprintf(outBuf.data(), outBuf.size(), "%016llx (%s) %s%s", _value, sectionStr, flags.c_str(), _symbolName);
            else
                snprintf(outBuf.data(), outBuf.size(), "%08llx (%s) %s%s", _value, sectionStr, flags.c_str(), _symbolName);
        }
    }
}

void nm::NListEntry::printHexValues(const ImageInfo& imageInfo, std::span<char> outBuf) const
{
    uint32_t poolOffset = (uint32_t)(_symbolName - imageInfo.stringPoolStart);
    if ( imageInfo.is64 )
        snprintf(outBuf.data(), outBuf.size(), "%016llx %02x %02x %04x %08x %s", _value, _type, _sect, _desc, poolOffset, _symbolName);
    else
        snprintf(outBuf.data(), outBuf.size(), "%08llx %02x %02x %04x %08x %s", _value, _type, _sect, _desc, poolOffset, _symbolName);
}

Error nm::printMachO(CString path, const UnsafeHeader* hdr, size_t len, bool printPath) const
{
    if ( printPath && !_filenamePerLine )
        printf("\n%s (for architecture %s):\n", path.c_str(), hdr->archName());
    Image image((void*)hdr, len, (hdr->inDyldCache() ? Image::MappingKind::dyldLoadedPostFixups : Image::MappingKind::wholeSliceMapped));
    if ( image.hasSymbolTable() ) {
        // gather symbols
        const NListSymbolTable& symTab = image.symbolTable();
        __block std::vector<NListEntry> symbols;
        symTab.forEachSymbol(^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            if ( ((n_type & N_STAB) == 0) || _printSTABs )
                symbols.emplace_back(symbolName, n_value, n_type, n_sect, n_desc);
        });
        sortSymbols(symbols);

        // build table of info about each section
        __block ImageInfo imageInfo;
        imageInfo.isObjectFile    = hdr->isObjectFile();
        imageInfo.is64            = hdr->is64();
        imageInfo.stringPoolStart = symTab.stringPool();
        hdr->forEachSection(^(const UnsafeHeader::SectionInfo& info, bool& stop) {
            SectionLetter sl;
            sl.name = std::string(info.segmentName) + "," + std::string(info.sectionName);
            if ( (info.segmentName == "__TEXT") && (info.sectionName == "__text") )
                sl.letter = 'T';
            else if ( (info.segmentName == "__DATA") && (info.sectionName == "__bss") )
                sl.letter = 'B';
            else if ( (info.segmentName == "__DATA") && (info.sectionName == "__data") )
                sl.letter = 'D';
            else
                sl.letter = 'S';
            imageInfo.sectionLetters.push_back(sl);
        });

        // build table of info about each imported dylib
        hdr->forEachLinkedDylib(^(const char* loadPath, mach_o::LinkedDylibAttributes, mach_o::Version32, mach_o::Version32,
                                  bool synthesizedLink, bool& stop) {
            if ( synthesizedLink )
                return;
            std::string leafName = loadPath;
            if ( const char* lastSlash = strrchr(loadPath, '/') )
                leafName = lastSlash+1;
            size_t leafNameLen = leafName.size();
            if ( (leafNameLen > 6) && (leafName.substr(leafNameLen-6) == ".dylib") )
                imageInfo.imports.push_back(leafName.substr(0,leafNameLen-6));
            else
                imageInfo.imports.push_back(leafName);
        });

        // print each symbol
        for (const NListEntry& sym : symbols) {
            if ( _skipNonGlobals && ((sym._type & N_EXT) == 0) )
                continue;
            bool onlyUndefs = false;
            switch ( _showFilter ) {
                case showAll:
                    break;
                case showOnlyUndefines:
                    if ( !isUndefinedSymbol(sym._type)  )
                        continue;
                    onlyUndefs = true;
                    break;
                case showNoUndefines:
                    if ( isUndefinedSymbol(sym._type) && (sym._value == 0) )
                        continue;
                break;
            }
            if ( _filenamePerLine )
                printf("%s: ", path.c_str());
            std::array<char, 2048> outBuf;
            switch ( _format ) {
                case formatRegular:
                    if ( onlyUndefs )
                        strlcpy(outBuf.data(), sym.name(), outBuf.size());
                    else
                        sym.printRegular(imageInfo, outBuf);
                    break;
                case formatVerbose:
                    sym.printVerbose(imageInfo, outBuf);
                    break;
                case formatNameOnly:
                    strlcpy(outBuf.data(), sym.name(), outBuf.size());
                    break;
                case formatHex:
                    sym.printHexValues(imageInfo, outBuf);
                    break;
            }
            printf("%s\n", outBuf.data());
        }
    }
    return Error::none();
}

void nm::handleFile(const CString path)
{
    _files.push_back(path);
}

Error nm::handleOption(std::span<CString>& remainingArgs)
{
    CString arg = remainingArgs.front();
    if ( arg == "-arch" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-arch missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        if ( archName != "all" ) {
            Architecture a = Architecture::byName(archName);
            if ( a == Architecture::invalid )
                return Error("-arch %s, unknown architecture name", archName.c_str());
            if ( a.isBigEndian() )
                return Error("-arch %s, big-endian architecture not supported", archName.c_str());
        }
        _cmdLineArchs.push_back(archName);
    }
    else {
        // all other nm options are single chars and can be packed in one option
        for (char c : arg.substr(1)) {
            switch (c) {
            case 'a':
                _printSTABs = true;
                break;
            case 'g':
                _skipNonGlobals = true;
                break;
            case 'j':
                _format = formatNameOnly;
                break;
            case 'm':
                _format = formatVerbose;
                break;
            case 'n':
                _sortOrder = sortByAddress;
                break;
            case 'o':
                _filenamePerLine = true;
                break;
            case 'p':
                _sortOrder = sortSymbolOrder;
                break;
            case 'r':
                _reverseSort = true;
                break;
            case 'u':
                _showFilter = showOnlyUndefines;
                break;
            case 'U':
                _showFilter = showNoUndefines;
                break;
            case 'x':
                _format = formatHex;
                break;
            default:
                return Error("unknown option letter: '%c'", c);
            }
        }
    }
    return Error::none();
}


Error nm::doRun()
{
    // if no file names specified, default to "a.out"
    if ( _files.size() == 0 )
        _files.push_back("a.out");

    // process all files specified on command line and only look at archs specified
    Tool::ErrorMap errors = printSlices(_files, _cmdLineArchs);
    if ( errors.empty() )
        return Error::none();
    if ( errors.size() == 1 ) {
        const auto& onlyErr = errors.begin();
        return std::move(onlyErr->second);
    }
    for (const auto& entry : errors) {
        fprintf(stderr, "%s: %s\n", __progname, entry.second.message());
    }
    return Error("multiple errors");
}


} // namespace other_tools

