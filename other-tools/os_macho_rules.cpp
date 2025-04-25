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
#include <TargetConditionals.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <uuid/uuid.h>
#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_priv.h>
#include <SoftLinking/WeakLinking.h>

// STL
#include <vector>
#include <tuple>
#include <set>
#include <unordered_set>
#include <string>

// mach_o
#include "Header.h"
#include "Version32.h"
#include "Universal.h"
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "SplitSeg.h"
#include "ChainedFixups.h"
#include "FunctionStarts.h"
#include "Misc.h"
#include "Instructions.h"
#include "FunctionVariants.h"

// common
#include "Defines.h"
#include "FileUtils.h"

// other_tools
#include "MiscFileUtils.h"
#include "os_macho_rules.h"

using mach_o::Header;
using mach_o::Version32;
using mach_o::Image;
using mach_o::Fixup;
using mach_o::Symbol;
using mach_o::Platform;
using mach_o::PlatformAndVersions;
using mach_o::ChainedFixups;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::Universal;
using mach_o::Image;


// some binaries are not for customer OS installs, so need less checks done
static bool debugVariantPath(CString path)
{
    static CString debugSuffixes[] = { "_asan.dylib", "_asan", "_debug.dylib", "_debug", "_profile", "_profile.dylib",
                                       "_trace", "_trace.dylib", "_tsan", "_tsan.dylib", "_ubsan" , "_ubsan.dylib" };
    for (CString suffix : debugSuffixes) {
        if ( path.ends_with(suffix) )
            return true;
    }
    return false;
}

static void verifyOSDylibInstallName(const Image& image, CString installLocationInDstRoot, CString verifierDstRoot, std::vector<VerifierError>& errors)
{
    // Don't allow @rpath to be used as -install_name for OS dylibs
    CString installName = image.header()->installName();
    if ( installName.starts_with("@rpath/") ) {
        errors.emplace_back("os_dylib_rpath_install_name");
        errors.back().message = Error("-install_name uses @rpath in arch %s", image.header()->archName());
    }
    else if ( installName.contains("//") ) {
        errors.emplace_back("os_dylib_bad_install_name");
        errors.back().message = Error("-install_name does not match install location in arch %s", image.header()->archName());
    }
    else {
        // Verify -install_name matches actual path of dylib
        if ( installLocationInDstRoot != installName ) {
            // see if install name is a symlink to actual file
            bool symlinkToDylib = false;
            char absDstRootPath[PATH_MAX];
            if ( ::realpath(verifierDstRoot.c_str(), absDstRootPath) != nullptr ) {
                char fullInstallNamePath[PATH_MAX];
                strlcpy(fullInstallNamePath, absDstRootPath, PATH_MAX);
                strlcat(fullInstallNamePath, installName.c_str(), PATH_MAX);
                char absInstallNamePath[PATH_MAX];
                if ( ::realpath(fullInstallNamePath, absInstallNamePath) != NULL ) {
                    char fullLocationName[PATH_MAX];
                    strlcpy(fullLocationName, absDstRootPath, PATH_MAX);
                    strlcat(fullLocationName, installLocationInDstRoot.c_str(), PATH_MAX);
                    char absFullLocationNamePath[PATH_MAX];
                    if ( ::realpath(fullLocationName, absFullLocationNamePath) != NULL ) {
                        if ( strcmp(absInstallNamePath, absFullLocationNamePath) == 0 )
                            symlinkToDylib = true;
                    }
                }
            }
            if ( !symlinkToDylib ) {
                errors.emplace_back("os_dylib_bad_install_name");
                errors.back().message = Error("-install_name does not match install location in arch %s", image.header()->archName());
            }
        }
    }
}

static void verifyOSDylibNoRpaths(const Image& image, std::vector<VerifierError>& errors)
{
    // Don't allow OS dylibs to add rpaths
    __block bool definesRPaths = false;
    image.header()->forEachRPath(^(const char* rPath, bool& stop) {
        definesRPaths = true;
    });
    if ( definesRPaths ) {
        errors.emplace_back("os_dylib_rpath");
        errors.back().message = Error("contains LC_RPATH load command in arch %s", image.header()->archName());
    }
}

static void verifyOSDylibNotMergeable(const Image& image, std::vector<VerifierError>& errors)
{
    // Don't allow OS dylibs to have mergable info by default
    uint32_t aiFileOffset;
    uint32_t aiSize;
    if ( image.header()->hasAtomInfo(aiFileOffset, aiSize) ) {
        bool allowAtomInfo = false;
        // rdar://136999565 (Teach mach-o verifier about LC_ATOM_INFO)
#if LD_DEFAULT_ADD_MERGEABLE_METADATA
        allowAtomInfo = true;
#endif
        if ( getenv("LD_DEFAULT_ADD_MERGEABLE_METADATA") != nullptr )
            allowAtomInfo = true;
        if ( !allowAtomInfo ) {
            errors.emplace_back("os_dylib_mergeable");
            errors.back().message = Error("is a mergable dylib for arch %s", image.header()->archName());
        }
    }
}

static void verifyOSDylibDoesNotExportMain(const Image& image, std::vector<VerifierError>& errors)
{
    if ( image.hasExportsTrie() ) {
        Symbol symbol;
        if ( image.exportsTrie().hasExportedSymbol("_main", symbol) ) {
            errors.emplace_back("os_dylib_exports_main");
            errors.back().message = Error("dylibs should not export '_main' symbol in arch %s", image.header()->archName());
        }
    }
}

static void verifyNoFlatLookups(const Image& image, std::vector<VerifierError>& errors)
{
    if ( !image.header()->usesTwoLevelNamespace() ) {
        errors.emplace_back("os_dylib_flat_namespace");
        errors.back().message = Error("built with -flat_namespace in arch %s", image.header()->archName());
        return;
    }

    if ( image.hasChainedFixups() ) {
        Error err = image.chainedFixups().forEachBindTarget(^(int libOrdinal, const char* symbolName, int64_t addend, bool weakImport, bool& stop) {
            if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
                errors.emplace_back("os_dylib_undefined_dynamic_lookup");
                errors.back().message = Error("built with -undefined dynamic_lookup for symbol %s in arch %s", symbolName, image.header()->archName());
            }
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachUndefinedSymbol(^(const Symbol& symbol, uint32_t symbolIndex, bool& stop) {
            int   libOrdinal;
            bool  weakImport;
            if ( symbol.isUndefined(libOrdinal, weakImport) ) {
                if ( (uint8_t)libOrdinal == DYNAMIC_LOOKUP_ORDINAL ) {
                    errors.emplace_back("os_dylib_undefined_dynamic_lookup");
                    errors.back().message = Error("built with -undefined dynamic_lookup for symbol %s in arch %s", symbol.name().c_str(), image.header()->archName());
                }
            }
        });
    }
}

static void verifyiOSMac(const Image& image, CString installLocationInDstRoot, std::vector<VerifierError>& errors)
{
    if ( installLocationInDstRoot.starts_with("/System/iOSSupport/") ) {
        // everything in /System/iOSSupport/ should be iOSMac only
        PlatformAndVersions pvs = image.header()->platformAndVersions();
        if ( pvs.platform != Platform::macCatalyst ) {
            errors.emplace_back("macos_in_ios_support");
            errors.back().message = Error("non-catalyst in /System/iOSSupport/ in arch %s", image.header()->archName());
        }
    }
    else {
        // maybe some day warn about iOSMac only stuff not in /System/iOSSupport/
    }
}

static void checkDylib(const Image& image, CString installLocationInDstRoot, CString verifierDstRoot, std::vector<VerifierError>& errors)
{
    if ( Header::isSharedCacheEligiblePath(installLocationInDstRoot.c_str()) ) {
        verifyOSDylibInstallName(image, installLocationInDstRoot, verifierDstRoot, errors);
        verifyOSDylibNoRpaths(image, errors);
        verifyOSDylibDoesNotExportMain(image, errors);
        verifyOSDylibNotMergeable(image, errors);
    }
}

// used by machocheck tool and by unit tests
void os_macho_verifier(CString path, std::span<const uint8_t> buffer, CString verifierDstRoot,
                       const std::vector<CString>& mergeRootPaths, std::vector<VerifierError>& errors)
{
    Image image(buffer.data(), buffer.size(), Image::MappingKind::wholeSliceMapped);
    if ( Error err = image.validate() ) {
        errors.emplace_back("os_dylib_malformed");
        errors.back().message = std::move(err);
        return;
    }

    // don't run checks on dylibs that will not be in customer OS installs
    if ( debugVariantPath(path) )
        return;

    // don't run checks on dylibs that are embedded in an app bundle
    if ( path.contains(".app/") )
        return;

    // dylib specific checks
    if ( path.starts_with(verifierDstRoot) ) {
        CString installLocationInDstRoot = path.substr(verifierDstRoot.size());
        if ( image.header()->isDylib() ) {
            if ( mergeRootPaths.empty() ) {
                checkDylib(image, installLocationInDstRoot, verifierDstRoot, errors);
            }
            else {
                // merge roots are when the project puts the binary in $DSTROOT/usr/lib,
                // but B&I moves it to /Applications/Xcode.app/Content/Toolchains/Foo.xctoolchain/usr/lib
                for (CString mergeRoot : mergeRootPaths) {
                    char fullerPath[PATH_MAX];
                    strlcpy(fullerPath, mergeRoot.c_str(), PATH_MAX);
                    strlcat(fullerPath, installLocationInDstRoot.c_str(), PATH_MAX);
                    checkDylib(image, fullerPath, verifierDstRoot, errors);
                }
            }
        }
        verifyiOSMac(image, installLocationInDstRoot, errors);
    }
    verifyNoFlatLookups(image, errors);
}






