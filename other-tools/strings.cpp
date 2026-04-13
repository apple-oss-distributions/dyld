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
#include <stdlib.h>
#include <stdio.h>
#include <mach-o/dyld_priv.h>

// C++ stl
#include <vector>

// common
#include "Defines.h"
#include "Array.h"

// mach_o
#include "Architecture.h"
#include "Error.h"

// other_tools
#include "MiscFileUtils.h"
#include "Tool.h"

using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;


namespace other_tools {

struct VIS_HIDDEN strings : public Tool
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

private:
    // find all strings in a range of bytes
    void    printStrings(std::span<const uint8_t> range, uint64_t startFileOffset) const;
    void    printStringsFromStdin() const;

    enum class Format { decimal, hex, octal };

    std::vector<CString>    _files;
    std::vector<CString>    _cmdLineArchs;
    bool                    _allSections  = false;
    bool                    _printOffset  = false;
    bool                    _wholeFile    = false;
    int                     _minStringLen = 4;
    Format                  _offsetFormat = Format::decimal;
};

void strings::usage() const
{
    fprintf(stderr, "Usage: %s [-arch <arch>]* <options>* <file>+ \n"
            "\t-a         Look for strings in all sections\n"
            "\t-o         Preceed each string by the file offset\n"
            "\t-t d|o|x   Used with -o to specific the offset format (o=octal,x=hex)\n"
            "\t-n <num>   Minimum string length (default is 4)\n"
            "\t-          Examine whole file instead of just cstring sections\n",
            __progname);
}

//
// @function printStrings
//
// @abstract
//      Scans a range of bytes looking for runs of printable (ASCII) characters.
//      Only prints string if it is at least _minStringLen chars long.
//
// @param range
//      Range of bytes to scan for strings. Could be section or whole file.
//
// @param startFileOffset
//      If _printOffset is set, the file offset of the range's start.
//
void strings::printStrings(std::span<const uint8_t> range, uint64_t startFileOffset) const
{
    size_t stringStartIndex = 0;
    size_t stringLen        = 0;
    size_t rangeEndIndex    = range.size()-1;
    for (size_t i = 0; i < range.size(); i++) {
        bool    lastChar = (i == rangeEndIndex);
        uint8_t c        = range[i];
        if ( isprint(c) && !lastChar ) {
            // in string, keep scanning
            stringLen++;
        }
        else {
            // found end of string, see if long enough
            if ( stringLen >= _minStringLen ) {
                if ( _printOffset ) {
                    switch ( _offsetFormat ) {
                    case Format::decimal:
                        printf("%lld", startFileOffset+stringStartIndex);
                        break;
                    case Format::hex:
                        printf("%llx", startFileOffset+stringStartIndex);
                        break;
                    case Format::octal:
                        printf("%llo", startFileOffset+stringStartIndex);
                        break;
                    }
                    printf(" ");
                }
                if ( lastChar && (c != '\n') )
                    printf("%.*s\n", (int)stringLen + 1, range.data()+stringStartIndex);
                else
                    printf("%.*s\n", (int)stringLen, range.data()+stringStartIndex);
            }
            // start search for next string
            stringStartIndex = i + 1;
            stringLen = 0;
        }
    }
}

void strings::printStringsFromStdin() const
{
    char        buf[1024];
    size_t      stringStartIndex = 0;
    size_t      stringLen        = 0;
    size_t      index            = 0;

    // <rdar://problem/54055310> Unix Conformance 2019
    setlocale(LC_ALL, "");

    do {
        int c = getc(stdin);
        if ( isprint(c) && (stringLen < (sizeof(buf)-2)) ) {
            buf[stringLen++] = c;
        }
        else {
            // found end of string, see if long enough
            if ( stringLen >= _minStringLen ) {
                if ( _printOffset ) {
                    switch ( _offsetFormat ) {
                        case Format::decimal:
                            printf("%ld", stringStartIndex);
                            break;
                        case Format::hex:
                            printf("%lx", stringStartIndex);
                            break;
                        case Format::octal:
                            printf("%lo", stringStartIndex);
                            break;
                    }
                    printf(" ");
                }
                if ( c != '\n' )
                    printf("%.*s\n", (int)stringLen + 1, buf);
                else
                    printf("%.*s\n", (int)stringLen, buf);
            }
            // start search for next string
            stringStartIndex = index + 1;
            stringLen = 0;
        }
        ++index;
    } while ( !ferror(stdin) && !feof(stdin) );
}


Error strings::printMachO(CString path, const UnsafeHeader* hdr, size_t len, bool printPath) const
{
    if ( printPath )
        printf("%s (for architecture %s):\n", path.c_str(), hdr->archName());
    if ( _wholeFile ) {
        std::span<const uint8_t> range((const uint8_t*)hdr, len);
        printStrings(range, 0);
    }
    else {
        // print strings in any sections except code sections
        hdr->forEachSection(^(const UnsafeHeader::SectionInfo& info, bool& stop) {
            if ( info.isZeroFill() )
                return;
            if ( (info.flags & S_ATTR_PURE_INSTRUCTIONS) && !_allSections )
                return;
            std::span<const uint8_t> range((const uint8_t*)hdr + info.fileOffset, info.size);
            printStrings(range, info.fileOffset);
        });
    }
    return Error::none();
}


void strings::handleFile(CString path)
{
    _files.push_back(path);
}

Error strings::handleOption(std::span<CString>& remainingArgs)
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
    else if ( arg == "-a" ) {
        _allSections = true;
    }
    else if ( arg == "-o" ) {
        _printOffset = true;
    }
    else if ( arg == "-t" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-t missing format char");
        remainingArgs = remainingArgs.subspan(1);
        CString format = remainingArgs.front();
        if ( format.empty() )
            return Error("option \"-t\" missing format character");
        if ( format == "d") {
            _offsetFormat = Format::decimal;
        }
        else if ( format == "o" ) {
            _offsetFormat = Format::octal;
        }
        else if ( format == "x") {
            _offsetFormat = Format::hex;
        }
        else {
            return Error("invalid option for \"-t\"");
        }
    }
    else if ( arg == "-n" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-n missing number");
        remainingArgs = remainingArgs.subspan(1);
        CString lenStr = remainingArgs.front();
        char* endp;
        _minStringLen = (uint32_t)strtoul(lenStr.c_str(), &endp, 10);
        if ( *endp != '\0' )
            return Error("invalid decimal number in \"-n\" option");
    }
    else if ( isdigit(arg.at(1)) ) {
        // note: -8 is same as "-n 8"
        CString lenStr = arg.substr(1);
        char* endp;
        _minStringLen = (uint32_t)strtoul(lenStr.c_str(), &endp, 10);
        if ( *endp != '\0' )
            return Error("invalid decimal number in \"%s\" option", arg.c_str());
    }
    else if ( arg == "-" ) {
        _wholeFile = true;
    }
    else {
        return Error("unknown option \"%s\"", arg.c_str());
    }
    return Error::none();
}

Error strings::doRun()
{
    // if no file names specified, read from stdin
    if ( _files.size() == 0 ) {
        printStringsFromStdin();
        return Error::none();
    }
    
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


#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::strings tool;
    return tool.run(argc, argv, envp);
}
#endif

