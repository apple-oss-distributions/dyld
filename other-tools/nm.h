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

// C++ STL
#include <vector>
#include <string>

// mach_o
#include "Architecture.h"
#include "Error.h"
#include "Misc.h"

// other_tools
#include "Tool.h"


using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;

namespace other_tools {

struct nm : public Tool
{
    // called by Tool::processArgs() to handle args starting with '-'
    Error   handleOption(std::span<CString>& remainingArgs) override;

    // called by Tool::processArgs() to handle args that do not starting with '-'
    void    handleFile(const CString path) override;

    // called after Tool::processArgs()
    Error   doRun() override;

    // called to print out command line argument options
    void    usage() const override;

    // called by Tool::printSlices() to print specific mach-o file
    Error   printMachO(CString path, const UnsafeHeader* hdr, size_t len, bool printPath) const override;

#if !BUILDING_UNIT_TESTS
private:
#endif
    struct NListEntry;
    struct SectionLetter { std::string name; char letter; };
    struct ImageInfo     { std::vector<SectionLetter> sectionLetters; std::vector<std::string> imports; const char* stringPoolStart; bool is64=false;  bool isObjectFile=false; };

    void            sortSymbols(std::vector<NListEntry>& symbols) const;

    enum SortOrder { sortByName, sortByAddress, sortSymbolOrder };
    enum Show      { showAll, showOnlyUndefines, showNoUndefines };
    enum Format    { formatRegular, formatVerbose, formatNameOnly, formatHex };

    std::vector<CString>        _files;
    std::vector<CString>        _cmdLineArchs;                      // -arch
    SortOrder                   _sortOrder       = sortByName;      // -n or -p
    Show                        _showFilter      = showAll;         // -u or -U
    Format                      _format          = formatRegular;   // -m or -j
    bool                        _filenamePerLine = false;           // -o
    bool                        _reverseSort     = false;           // -r
    bool                        _skipNonGlobals  = false;           // -g
    bool                        _printSTABs      = false;           // -a

    // contains info about one symbol
    struct NListEntry {
                        NListEntry(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc);
        const char*     name() const { return _symbolName; }
        void            printRegular(const ImageInfo& imageInfo, std::span<char> outBuf) const;
        void            printVerbose(const ImageInfo& imageInfo, std::span<char> outBuf) const;   // -m
        void            printHexValues(const ImageInfo& imageInfo, std::span<char> outBuf) const; // -x
#if !BUILDING_UNIT_TESTS
    private:
#endif
        friend struct nm;
        const char*     verboseSymbolSection(const ImageInfo& imageInfo) const;
        std::string     verboseSymbolFlags(const ImageInfo& imageInfo) const;
        std::string     verboseTwoLevelImport(const ImageInfo& imageInfo) const;
        const char*     stabName() const;

        const char*     _symbolName;
        uint64_t        _value;
        uint8_t         _type;
        uint8_t         _sect;
        uint16_t        _desc;
    };

};


} // namespace other_tools

