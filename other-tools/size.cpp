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
#include <mach-o/dyld_priv.h>
#include <mach-o/utils_priv.h>

// C++ stl
#include <vector>
#include <string>

// common
#include "Defines.h"
#include "Array.h"

// mach_o
#include "Architecture.h"
#include "Error.h"
#include "UnsafeHeader.h"

// other_tools
#include "MiscFileUtils.h"
#include "Tool.h"

using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;

namespace other_tools {

struct VIS_HIDDEN size : public Tool
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
    // print size info
    void    printSegmentSizes(const UnsafeHeader* hdr) const;
    void    printSections(const UnsafeHeader* hdr) const;

    std::vector<CString>    _files;
    std::vector<CString>    _cmdLineArchs;
    bool                    _printSections  = false;
    bool                    _printAddresses = false;
    bool                    _printHexSizes  = false;
};


void size::usage() const
{
    fprintf(stderr, "Usage: %s [-arch <arch>]* <options>* <mach-o file>+ \n"
            "\t-m     Display all sections with their sizes\n"
            "\t-l     When used with the -m option, also print the addresses and offsets of the sections and segments\n"
            "\t-x     When used with the -m option, print the values in hexadecimal rather than decimal\n",
         __progname);
}

void size::printSegmentSizes(const UnsafeHeader* hdr) const
{
    __block uint64_t textSize   = 0;
    __block uint64_t dataSize   = 0;
    __block uint64_t otherSizes = 0;
    __block uint64_t allSizes   = 0;
    hdr->forEachSegment(^(const UnsafeHeader::SegmentInfo& info, bool& stop) {
        allSizes += info.vmsize;
        if ( info.segmentName == "__TEXT" ) {
            textSize = info.vmsize;
        }
        else if ( info.segmentName == "__DATA" ) {
            dataSize = info.vmsize;
        }
        else {
            otherSizes += info.vmsize;
        }
    });
    printf("__TEXT\t__DATA\t__OBJC\tothers\tdec\thex\n");
    printf("%lld\t%lld\t%lld\t%lld\t%lld\t%llx\n", textSize, dataSize, 0LL, otherSizes, allSizes, allSizes);
}

void size::printSections(const UnsafeHeader* hdr) const
{
    bool isObjectFile = hdr->isObjectFile();
    __block uint64_t segmentsTotalSize   = 0;
    hdr->forEachSegment(^(const UnsafeHeader::SegmentInfo& segInfo, bool& stop) {
        segmentsTotalSize += segInfo.vmsize;
        printf("Segment %.*s: ", (int)segInfo.segmentName.size(), segInfo.segmentName.data());
        if ( _printHexSizes )
            printf("0x%llx", segInfo.vmsize);
        else
            printf("%lld", segInfo.vmsize);
        if ( _printAddresses )
            printf(" (addr 0x%llX fileoff %d)", segInfo.vmaddr, segInfo.fileOffset);
        printf("\n");
        __block uint64_t sectionsTotalSize = 0;
        hdr->forEachSection(^(const UnsafeHeader::SectionInfo& info, bool& stop2) {
            if ( !isObjectFile && (segInfo.segmentName != info.segmentName) )
                return;
            sectionsTotalSize += info.size;
            if ( isObjectFile )
                printf("\tSection (%.*s, %.*s): ", (int)info.segmentName.size(), info.segmentName.data(), (int)info.sectionName.size(), info.sectionName.data());
            else
                printf("\tSection %.*s: ", (int)info.sectionName.size(), info.sectionName.data());
            if ( _printHexSizes )
                printf("0x%llx", info.size);
            else
                printf("%lld", info.size);
            if ( _printAddresses ) {
                if ( info.isZeroFill() )
                    printf(" (addr 0x%llX zerofill)", info.address);
                else
                    printf(" (addr 0x%llX offset %d)", info.address, info.fileOffset);
            }
            printf("\n");
        });
        if ( sectionsTotalSize != 0 )
            printf("\ttotal %lld\n", sectionsTotalSize);
    });
    printf("total %lld\n", segmentsTotalSize);
}

Error size::printMachO(CString path, const UnsafeHeader* hdr, size_t len, bool printPath) const
{
    if ( printPath )
        printf("%s (for architecture %s):\n", path.c_str(), hdr->archName());
    if ( _printSections ) {
        printSections(hdr);
    }
    else {
        printSegmentSizes(hdr);
    }
    return Error::none();
}

void size::handleFile(const CString path)
{
    _files.push_back(path);
}

Error size::handleOption(std::span<CString>& remainingArgs)
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
        // all other size options are single chars and can be packed in one option
        for (char c : arg.substr(1)) {
            switch (c) {
                case 'm':
                    _printSections = true;
                    break;
                case 'l':
                    _printAddresses = true;
                    break;
                case 'x':
                    _printHexSizes = true;
                    break;
                default:
                    return Error("unknown option letter: '%c'", c);
            }
        }
    }
    return Error::none();
}

Error size::doRun()
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


#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::size tool;
    return tool.run(argc, argv, envp);
}
#endif
