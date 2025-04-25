/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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

// OS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// STL
#include <vector>

// mach_o
#include "Header.h"
#include "Error.h"
#include "Universal.h"

// common
#include "CString.h"

// other_tools
#include "MiscFileUtils.h"
#include "os_macho_rules.h"

using mach_o::Header;
using mach_o::Error;
using mach_o::Universal;



int main(int argc, const char* argv[])
{
    std::vector<CString> paths;
    std::vector<CString> mergeRootPaths;
    CString              verifierDstRoot = NULL;
    for (int i=1; i < argc; ++i) {
        CString arg = argv[i];
        if ( arg[0] == '-' ) {
            if ( arg == "-verifier_dstroot" ) {
                verifierDstRoot = argv[++i];
                if ( verifierDstRoot.ends_with('/') )
                    verifierDstRoot = CString::dup(verifierDstRoot.substr(0, verifierDstRoot.size()-1));
            }
            else if ( arg == "-verifier_error_list" ) {
                printf("os_dylib_rpath_install_name\tOS dylibs (those in /usr/lib/ or /System/Library/) must be built with -install_name that is an absolute path - not an @rpath\n");
                printf("os_dylib_bad_install_name\tOS dylibs (those in /usr/lib/ or /System/Library/) must be built with -install_name matching their file system location\n");
                printf("os_dylib_rpath\tOS dylibs should not contain LC_RPATH load commands (from -rpath linker option)(remove LD_RUNPATH_SEARCH_PATHS Xcode build setting)\n");
                printf("os_dylib_flat_namespace\tOS dylibs should not be built with -flat_namespace\n");
                printf("os_dylib_undefined_dynamic_lookup\tOS dylibs should not be built with -undefined dynamic_lookup\n");
                printf("os_dylib_malformed\tthe mach-o file is malformed\n");
                printf("macos_in_ios_support\t/System/iOSSupport/ should only contain mach-o files that support iosmac\n");
                printf("os_dylib_exports_main\tOS dylibs should not export '_main' symbol\n");
                printf("os_dylib_mergeable\tOS dylibs (those in /usr/lib/ or /System/Library/) should not be built mergeable\n");
                return 0;
            }
            else if ( arg == "-merge_root_path" ) {
                CString mergeRoot = argv[++i];
                if ( mergeRoot != "/" )
                    mergeRootPaths.push_back(mergeRoot);
            }
            else {
                fprintf(stderr, "unknown option: %s\n", arg.c_str());
                exit(1);
            }
        }
        else {
            paths.push_back(arg);
        }
    }

    if ( verifierDstRoot.empty() ) {
        fprintf(stderr, "missing -verifier_dstroot\n");
        exit(1);
    }


    for (CString path : paths) {
        __block std::vector<VerifierError> errors;
        bool found = other_tools::withReadOnlyMappedFile(path.c_str(), ^(std::span<const uint8_t> buffer) {
            if ( const Universal* uni = Universal::isUniversal(buffer) ) {
                uni->forEachSlice(^(Universal::Slice slice, bool& stopSlice) {
                    const char* sliceArchName = slice.arch.name();
                    if ( Header::isMachO(slice.buffer) ) {
                        os_macho_verifier(path, slice.buffer, verifierDstRoot, mergeRootPaths, errors);
                    }
                    else {
                        fprintf(stderr, "%s slice in %s is not a mach-o\n", sliceArchName, path.c_str());
                    }
                });
            }
            else if ( Header::isMachO(buffer) ) {
                os_macho_verifier(path, buffer, verifierDstRoot, mergeRootPaths, errors);
            }
        });
        if ( found ) {
            for ( const VerifierError& err : errors ) {
                // print formatted output the verifier perl script expects
                printf("%s\tfatal\t%s\n", err.verifierErrorName.c_str(), err.message.message());
            }
        }
        else {
            fprintf(stderr, "file %s not found\n", path.c_str());
            return 1;
        }
     }

    return 0;
}






