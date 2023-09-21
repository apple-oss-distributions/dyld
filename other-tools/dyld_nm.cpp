/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2018-2019 Apple Inc. All rights reserved.
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

#include <vector>
#include <string>

#include "Defines.h"
#include "Array.h"

#include "Universal.h"
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "NListSymbolTable.h"
#include "Misc.h"


using mach_o::Universal;
using mach_o::Header;
using mach_o::Image;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::NListSymbolTable;


enum SortOrder { sortByName, sortByAddress, sortSymbolOrder };
enum Show      { showAll, showOnlyUndefines, showNoUndefines };
enum Format    { formatRegular, formatVerbose, formatNameOnly, formatHex };

struct PrintOptions
{
    SortOrder   sort           = sortByName;
    Show        show           = showAll;
    Format      format         = formatRegular;
    bool        skipNonGlobals = false;
    bool        printSTABs     = false;
};


static void usage()
{
    fprintf(stderr, "Usage: dyld_nm [-arch <arch>]* <options>* <mach-o file>+ \n"
            "\t-a     Display all symbol table entries, including those inserted for use by debuggers\n"
            "\t-g     Display only global (external) symbols\n"
            "\t-m     Symbol details are displayed in a human-friendly manner\n"
            "\t-n     Sort by address rather than by symbol name\n"
            "\t-p     Don't sort, display in symbol-table order\n"
            "\t-u     Display only undefined symbols\n"
            "\t-U     Don't display undefined symbols\n"
            "\t-x     Display the symbol table entry's fields in hexadecimal, along with the name as a string\n"
            "\t-j     Just display the symbol names (no value or type).\n"
        );
}

struct Entry { const char* symbolName; uint64_t value; uint8_t type; uint8_t sect; uint16_t desc; };

static void sortSymbols(std::vector<Entry>& symbols, SortOrder order)
{
    switch ( order ) {
        case sortByName:
            std::sort(symbols.begin(), symbols.end(), [](const Entry& l, const Entry& r) {
                return (strcmp(l.symbolName, r.symbolName) < 0);
            });
            break;
        case sortByAddress:
            std::sort(symbols.begin(), symbols.end(), [](const Entry& l, const Entry& r) {
                if ( l.value != r.value ) {
                    return (l.value < r.value);
                }
                return (strcmp(l.symbolName, r.symbolName) < 0);
            });
            break;
        case sortSymbolOrder:
            break;
    }
}


static bool isUndefinedSymbol(uint8_t n_type)
{
    return ((n_type & N_TYPE) ==  N_UNDF);
}

struct SectionInfo
{
                SectionInfo(std::string_view segName, std::string_view sectionName, uint8_t sectionType);

    std::string name;
    char        code;
};

SectionInfo::SectionInfo(std::string_view segName, std::string_view sectionName, uint8_t sectionType)
{
    this->name = std::string(segName) + "," + std::string(sectionName);
    if ( segName == "__TEXT" )
        this->code = 'T';
    else if ( (sectionType == S_ZEROFILL) && (sectionName == "__bss") )
        this->code = 'B';
    else if ( segName.starts_with("__DATA") )
        this->code = 'D';
    else
        this->code = 'S';
}

static void printSymbolRegular(const Entry& sym, const std::vector<SectionInfo>& sectionInfos)
{
    char c = '?';
    switch (sym.type & N_TYPE) {
        case N_UNDF:
			c = (sym.value != 0) ? 'C' : 'U';
			break;
        case N_ABS:
			c = 'A';
			break;
        case N_SECT:
            c = sectionInfos[sym.sect-1].code;
			break;
    }
    if ( (sym.type & N_EXT) == 0 )
        c = tolower(c);

    printf("%016llX %c %s\n", sym.value, c, sym.symbolName);
}

static const char* verboseSymbolSection(const Entry& sym, const std::vector<SectionInfo>& sectionInfos)
{
    switch (sym.type & N_TYPE) {
        case N_UNDF:
			return (sym.value != 0) ? "common" : "undefined";
        case N_ABS:
			return "absolute";
        case N_SECT:
            return sectionInfos[sym.sect-1].name.c_str();
    }
    return "???";
}

static std::string verboseSymbolFlags(const Entry& sym, const std::vector<SectionInfo>& sectionInfos, bool isObjectFile)
{
    std::string flags;
    if ( sym.type & N_EXT ) {
        if ( sym.type & N_PEXT ) {
            if ( sym.desc & N_WEAK_DEF )
                flags = "weak private external ";
            else
                flags = "private external ";
        }
        else {
            if ( sym.desc & N_WEAK_DEF ) {
                if ( sym.desc & N_WEAK_REF )
                    flags = "weak external automatically hidden ";
                else
                    flags = "weak external ";
            }
            else
                flags = "external ";
        }
    }
    else {
        if ( sym.type & N_PEXT )
            flags = "non-external (was a private external) ";
        else
            flags = "non-external ";
    }
    if ( isObjectFile ) {
        if ( sym.desc & N_NO_DEAD_STRIP )
            flags += "[no dead strip] ";

        if ( (sym.desc & N_SYMBOL_RESOLVER) && (sym.type != N_UNDF) )
            flags += "[symbol resolver] ";

        if ( (sym.desc & N_ALT_ENTRY) && (sym.type != N_UNDF) )
            flags += "[alt entry] ";

        if ( (sym.desc & N_COLD_FUNC) && (sym.type != N_UNDF) )
            flags += "[cold func] ";
    }

    return flags;
}

static std::string verboseTwoLevelImport(const Entry& sym, const std::vector<std::string>& imports)
{
    uint8_t libOrdinal = GET_LIBRARY_ORDINAL(sym.desc);
    if ( libOrdinal < MAX_LIBRARY_ORDINAL ) {
        if ( libOrdinal > imports.size() )
            return "ordinal-too-large";
        return imports[libOrdinal-1];
    }
    else {
        switch ( libOrdinal) {
            case EXECUTABLE_ORDINAL:
                return "main-executable";
            case DYNAMIC_LOOKUP_ORDINAL:
                return "flat-namespace";
            case SELF_LIBRARY_ORDINAL:
                return "this-image";
        }
    }
    return "unknown-ordinal";
}

static const char* stabName(const Entry& sym)
{
    switch (sym.type) {
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


static void printSymbolVerbose(const Entry& sym, const std::vector<SectionInfo>& sectionInfos, const std::vector<std::string>& imports, bool isObjectFile)
{
    if ( sym.type & N_STAB ) {
        printf("%016llx - %02x %04X %5s %s\n", sym.value, sym.sect, sym.desc, stabName(sym), sym.symbolName);
    }
    else {
        const char* sectionStr = verboseSymbolSection(sym, sectionInfos);
        std::string flags      = verboseSymbolFlags(sym, sectionInfos, isObjectFile);
        if ( !isObjectFile && isUndefinedSymbol(sym.type) )
            printf("                 (%s) %s%s (from %s)\n", sectionStr, flags.c_str(), sym.symbolName, verboseTwoLevelImport(sym, imports).c_str());
        else
            printf("%016llx (%s) %s%s\n", sym.value, sectionStr, flags.c_str(), sym.symbolName);
    }
}

static void printSymbolNameOnly(const Entry& sym)
{
     printf("%s\n", sym.symbolName);
}

static void printSymbolHex(const Entry& sym, const char* stringPool)
{
    printf("%016llx %02X %02x %04X %08X %s\n", sym.value, sym.type, sym.sect, sym.desc, (uint32_t)(sym.symbolName - stringPool), sym.symbolName);
}

int main(int argc, const char* argv[])
{
    if ( argc == 1 ) {
        usage();
        return 0;
    }

    PrintOptions                printOptions;
    std::vector<const char*>    files;
    std::vector<const char*>    cmdLineArchs;
    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-arch") == 0 ) {
            if ( ++i < argc ) {
                cmdLineArchs.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-arch missing architecture name");
                return 1;
            }
        }
        else if ( arg[0] == '-' ) {
            for (const char* s=&arg[1]; *s != '\0'; ++s) {
                switch (*s) {
                    case 'a':
                        printOptions.printSTABs = true;
                        break;
                    case 'g':
                        printOptions.skipNonGlobals = true;
                        break;
                    case 'j':
                        printOptions.format = formatNameOnly;
                        break;
                    case 'm':
                        printOptions.format = formatVerbose;
                        break;
                    case 'n':
                        printOptions.sort = sortByAddress;
                        break;
                    case 'p':
                        printOptions.sort = sortSymbolOrder;
                        break;
                    case 'u':
                        printOptions.show = showOnlyUndefines;
                        break;
                    case 'U':
                        printOptions.show = showNoUndefines;
                        break;
                    case 'x':
                        printOptions.format = formatHex;
                        break;
                    default:
                        fprintf(stderr, "nm: unknown option letter: '%c'\n", *s);
                        return 1;
                }
            }
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

    mach_o::forSelectedSliceInPaths(files, cmdLineArchs, ^(const char* path, const Header* header, size_t sliceLen) {
        printf("%s [%s]:\n", path, header->archName());
        Image image((void*)header, sliceLen, (header->inDyldCache() ? Image::MappingKind::dyldLoadedPostFixups : Image::MappingKind::wholeSliceMapped));
        if ( image.hasSymbolTable() ) {
            // gather symbols
            const NListSymbolTable& symTab = image.symbolTable();
            __block std::vector<Entry> symbols;
            symTab.forEachSymbol(^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
                if ( ((n_type & N_STAB) == 0) || printOptions.printSTABs )
                    symbols.push_back({symbolName, n_value, n_type, n_sect, n_desc});
            });
            sortSymbols(symbols, printOptions.sort);

            // build table of info about each section
            __block std::vector<SectionInfo> sectionInfos;
            header->forEachSection(^(const Header::SectionInfo& info, bool &stop) {
                uint8_t sectionType = (info.flags & SECTION_TYPE);
                sectionInfos.emplace_back(info.segmentName, info.sectionName, sectionType);
            });

            // build table of info about each imported dylib
            __block std::vector<std::string> imports;
            header->forEachDependentDylib(^(const char* loadPath, bool, bool, bool, mach_o::Version32, mach_o::Version32, bool& stop) {
                std::string leafName = loadPath;
                if ( const char* lastSlash = strrchr(loadPath, '/') )
                    leafName = lastSlash+1;
                size_t leafNameLen = leafName.size();
                if ( (leafNameLen > 6) && (leafName.substr(leafNameLen-6) == ".dylib") )
                    imports.push_back(leafName.substr(0,leafNameLen-6));
                else
                    imports.push_back(leafName);
            });

            // print each symbol
            for (const Entry& sym : symbols) {
                if ( printOptions.skipNonGlobals && ((sym.type & N_EXT) == 0) )
                    continue;
                switch ( printOptions.show ) {
                    case showAll:
                         break;
                    case showOnlyUndefines:
                         if ( !isUndefinedSymbol(sym.type)  )
                            continue;
                         break;
                    case showNoUndefines:
                         if ( isUndefinedSymbol(sym.type) )
                            continue;
                        break;
                }

                switch ( printOptions.format ) {
                    case formatRegular:
                        printSymbolRegular(sym, sectionInfos);
                        break;
                    case formatVerbose:
                        printSymbolVerbose(sym, sectionInfos, imports, header->isObjectFile());
                        break;
                    case formatNameOnly:
                         printSymbolNameOnly(sym);
                        break;
                   case formatHex:
                        printSymbolHex(sym, symTab.stringPool());
                        break;
                }
            }
        }
    });

}
