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

#ifndef DyldAnalyzer_h
#define DyldAnalyzer_h

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

// other_tools
#include "Tool.h"

using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::Archive;

namespace other_tools {

struct dyld_analyzer : public Tool
{
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

    Error processFiles(std::string& jsonEntry);

    CString                     _directory;
    CString                     _outPath;               // -o <path>
    bool                        _printVersion = false;  // print version and return
    bool                        _measureSlices = false; // measure slices with sha256
};


} // namespace other_tools


#endif // DyldAnalyzer_h
