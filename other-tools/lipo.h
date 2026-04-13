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

#ifndef LipoTool_h
#define LipoTool_h

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/utils_priv.h>

// C++ STL
#include <vector>
#include <string>

// common
#include "Defines.h"
#include "CString.h"

// mach_o
#include "Architecture.h"
#include "Error.h"

// other_tools
#include "Tool.h"

using mach_o::Architecture;
using mach_o::Error;


namespace other_tools {

struct VIS_HIDDEN Lipo : public other_tools::Tool
{
    Error   handleOption(std::span<CString>& remainingArgs) override;
    void    handleFile(const CString path) override;
    Error   doRun() override;
    void    usage() const override;
    void    printErrorMessage(const Error& err) const override;

#if !BUILDING_UNIT_TESTS
private:
#endif
    struct ArchAlign
    {
        Architecture arch;
        uint32_t     p2align;
    };
    typedef std::unordered_map<CString, Architecture> PathToArch;
    struct ArchAndPath { Architecture arch; CString path; };
    enum Mode { none, listArchs, create, showInfo, showDetailedInfo, extractOneSlice, extractSlices, removeSlices, replaceSlices, thin, hasArch };
    Mode                      mode        = none;
    CString                   outputPath;
    std::vector<CString>      inputPaths;
    PathToArch                inputArchOverrides;
    bool                      createFat64      = false;
    bool                      skipErrorMessage = false; // used with -verify_arch
    Architecture              thinArch;
    Architecture              verifyArch;
    std::vector<Architecture> extractArchs;
    std::vector<Architecture> removeArchs;
    std::vector<ArchAlign>    archAlignments;
    std::vector<ArchAndPath>  replacements;

    Error               doListArchs();
    Error               doShowInfo(bool detailed);
    Error               doCreate();
    Error               doExtractOneSlice();
    Error               doExtractSlices();
    Error               doRemoveSlice();
    Error               doVerifyArch();
    Error               doReplaceSlices();
    Error               setMode(Mode);
    static const char*  capabilities(Architecture arch, char capStrBuf[128]);
};

} // namespace other_tools

#endif // LipoTool_h
