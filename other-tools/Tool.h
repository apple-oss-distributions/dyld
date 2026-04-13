/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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


#ifndef Tool_h
#define Tool_h

#include <stdint.h>

// STL
#include <vector>

// common
#include "CString.h"

// mach_o
#include "Error.h"
#include "UnsafeHeader.h"
#include "Archive.h"
#include "Universal.h"

// libLTO.dylib is only built for macOS
#if TARGET_OS_OSX && BUILDING_FOR_TOOLCHAIN && __has_include(<llvm-c/lto.h>)
    #include <llvm-c/lto.h>
    #define HAVE_LIBLTO 1
#else
    #define HAVE_LIBLTO 0
#endif


using mach_o::Error;
using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Universal;
using mach_o::Archive;


namespace other_tools {

struct VIS_HIDDEN Tool
{
    int             run(int argc, const char* argv[], const char* const envp[]);

#if BUILDING_UNIT_TESTS
    Error           processTestArgs(const std::vector<const char*>& args, const std::vector<const char*>& envVars={});
#else
protected:
#endif
    virtual Error   handleOption(std::span<CString>& remainingArgs) = 0;  // arg is remainingArgs.front(), and multi-arg options can get to rest
    virtual void    handleFile(CString path) = 0;
    virtual void    usage() const = 0;
    virtual Error   doRun() = 0;
    virtual void    printErrorMessage(const Error& err) const;
    virtual void    processProgName(CString argv0) { }
    virtual void    processEnvVars(const char* const envp[]) { }
    virtual Error   processArgs(std::span<CString>);
    Error           processArgs(int argc, const char* const argv[]);
    virtual CString versionNumberStr() const;
    
    std::vector<CString> expandResponseFile(const char* responseFile);

    typedef std::unordered_map<CString, Error> ErrorMap;
    ErrorMap            printSlices(std::span<CString> files, std::span<CString> cmdLineArchs) const;
    virtual Error       printMachO(CString path, const UnsafeHeader* hdr, size_t len, bool printPath) const { return Error::none(); }

    Error           printSpecificSlice(CString path, bool printPath, std::span<const Universal::Slice> slices, Architecture arch, bool& found) const;
    Error           printHostSpecificSlice(CString path, bool printPath, std::span<const Universal::Slice> slices, bool& found) const;
    Error           printSlice(CString path, std::span<const uint8_t> buffer, bool printPath) const;


    // For each supplied path, walk process every slice of a fat file and potentially every member of a static library.
    // Then call callback once with all mach-o/bitcode slices.
    // For paths that are inaccessible or are not mach-o, err is set in coresponding inputs.err
    // The file buffers used in the callback are freed when getMachOsFromPaths returns
    struct Input { CString path; Error err; mach_o::Universal::Slice slice; Archive::Member memberInfo; };
    static void     getMachOsFromPaths(std::span<const CString> paths, bool decendIntoStaticLibs, bool searchDyldCache,
                                       void (^callback)(std::span<const Input> inputs),
                                       Archive::MisalignHandling misalignedMember=Archive::MisalignHandling::error);

#if HAVE_LIBLTO
    static bool     getBitCodeFileArch(CString path, std::span<const uint8_t> buffer, Architecture& arch);
#endif
    
private:
    static void     appendInputs(CString path, std::string_view memberName, const struct stat& statBuf, std::span<const uint8_t> fileBuffer,
                                 bool decendIntoStaticLibs, std::vector<Input>& slices, std::vector<const char*>& stringsToFree,
                                 Archive::MisalignHandling misalign=Archive::MisalignHandling::error);

};


} // namespace other_tools

#endif // Tool_h
