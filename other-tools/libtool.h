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

#ifndef LibTool_h
#define LibTool_h

#include <stdarg.h>
#include <stdio.h>

// C++ STL
#include <vector>
#include <string>

// mach_o
#include "Architecture.h"
#include "Error.h"
#include "Misc.h"
#include "Archive.h"

// mach_o_writer
#include "ArchiveWriter.h"

// other_tools
#include "Tool.h"

// ld
#include "DependencyInfo.h"

using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::Archive;
using mach_o::ArchiveWriter;

namespace other_tools {

struct libtool : public Tool
{
    // called by Tool::run() to process all args, we use it to peek ahead for -L
    Error   processArgs(std::span<CString> args) override;

    // called by Tool::run() to check if libtool or ranlib is being invokedd
    void    processProgName(CString argv0) override;

    // called by Tool::run() to check for any env vars that effect tool run
    void    processEnvVars(const char* const envp[]) override;

    // called by Tool::processArgs() to handle args starting with '-'
    Error   handleOption(std::span<CString>& remainingArgs) override;

    // called by Tool::processArgs() to handle args that do not starting with '-'
    void    handleFile(const CString path) override;

    // called after Tool::processArgs()
    Error   doRun() override;

    // called to print out command line argument options
    void    usage() const override;

#if !BUILDING_UNIT_TESTS
private:
#endif

    ErrorMap        makeStaticLibrary(std::span<const CString> inputPaths, std::span<const uint8_t>& result, std::vector<CString>& inputArchives) const;
    Error           makeThinStaticLib(std::span<const Input*> inputSlices, std::span<const uint8_t>& result) const;
    
    void                 appendUsedArchivesToTraceFile(std::span<CString> inputArchives);
    std::vector<CString> getObjectGlobalSymbols(const Input& input) const;
    Error                writeTraceSymbolsFile(Architecture arch, std::span<const uint8_t> fileBuffer, std::span<const ArchiveWriter::ToC::NameAndValue> exportNames) const;


    std::vector<CString>        _files;
    Architecture                _archOnly;                          // -arch_only <arch>
    CString                     _outPath;                           // -o <path>
    bool                        _ranlibMode    = false;             // ranlib being run (vs libtool)
    bool                        _forceTOC64    = false;             // -toc64
    bool                        _sortedTOC     = true;              // -a or -s
    bool                        _reproducible  = false;             // -D or ZERO_AR_DATE
    bool                        _longNames     = true;              // -T   (support filenames > 16 chars)
    bool                        _warnNoSymbols = true;              // -no_warning_for_no_symbols
    bool                        _warnDupMembers= false;             // -warn_duplicate_member_names
    bool                        _commonsInToc  = false;             // -c
    bool                        _printVersion  = false;             // -V
    bool                        _verbose       = false;             // -v
    bool                        _sdkLibsAsRefs = false;             // -encode_sdk_libraries_as_references
    bool                        _ranlib_q      = false;             // ranlib -q
    bool                        _ranlib_f      = false;             // ranlib -f
    CString                     _sdkPath;                           // -syslibroot <path>
    std::vector<CString>        _libSearchPaths;                    // -L<path>
    std::vector<CString>        _frameworkSearchPaths;              // -F<path>
    CString                     _dependencyPath;                    // -dependency_info <path>
    ld::DependencyInfo          _dependencies;
    CString                     _traceFilePath;                     // -trace_file or LD_TRACE_FILE
    CString                     _traceSymbolsDirPath;               // directory relative to LD_TRACE_FILE or LD_TRACE_SYMBOLS_DIR
    CString                     _traceSymbolsFilePath;              // -trace_symbol_file or file in _traceSymbolsDirPath
    std::vector<CString>        _autoLinkLibs;
    std::vector<CString>        _autoLinkFrameworks;
};


} // namespace other_tools


#endif // LibTool_h
