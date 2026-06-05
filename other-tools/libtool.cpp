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
#include <sys/stat.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <mach-o/dyld_priv.h>
#include <os/base.h>    // for OS_STRINGIFY
#include <libgen.h>

// c++ stl
#include <vector>
#include <string>
#include <fstream>

// common
#include "Defines.h"
#include "Array.h"
#include "Utilities.h"

// mach_o
#include "Architecture.h"
#include "Image.h"
#include "Error.h"
#include "NListSymbolTable.h"
#include "Symbol.h"

// mach_o writer
#include "ArchiveWriter.h"
#include "UniversalWriter.h"

// other_tools
#include "MiscFileUtils.h"
#include "Tool.h"
#include "FileUtils.h"
#include "libtool.h"
#include "cctools_helpers.h"

// ld
#include "File.h"
#include "DependencyInfo.h"


using mach_o::UnsafeHeader;
using mach_o::Image;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::NListSymbolTable;
using mach_o::Symbol;
using mach_o::Archive;
using mach_o::ArchiveWriter;
using mach_o::UniversalWriter;
using mach_o::Platform;
using mach_o::Version32;

#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::libtool tool;
    return tool.run(argc, argv, envp);
}
#endif


namespace other_tools {

void libtool::usage() const
{
    if ( _ranlibMode ) {
        fprintf(stderr, "Usage: ranlib <options>* <staticlibpath>\n"
                "\t-c                                   Add tentative-defintins to table-of-contents\n"
                "\t-s                                   Generate sorted table-of-contents (default)\n"
                "\t-a                                   Generate unsorted table-of-contents (for very old linkers)\n"
                "\t-t                                   Used to mean \"touch\" the members, now ignored\n"
                "\t-f                                   Warn if the static lib is universal\n"
                "\t-q                                   Do nothing if static lib is universal\n"
                "\t-D                                   Reproducible builds, zero out archive mtime info\n"
                );
    }
    else {
        fprintf(stderr, "Usage: %s -static [-arch_only <arch>] <options>* <mach-o file>+ -o <file>\n"
                "\t-static                              Required.  Means create a static library\n"
                "\t-arch_only <arch>                    Build a once-arch static library from fat files\n"
                "\t-o <file>                            Output path of static library\n"
                "\t-filelist <file>                     Reads list of input files from specified file\n"
                "\t-no_warning_for_no_symbols           Don't warn about .o files that have no global symbols\n"
                "\t-toc64                               Force 64-bit table-of-contents\n"
                "\t-c                                   Add tentative-defintins to table-of-contents\n"
                "\t-s                                   Generate sorted table-of-contents (default)\n"
                "\t-a                                   Generate unsorted table-of-contents (for very old linkers)\n"
                "\t-D                                   Reproducible builds, zero out archive mtime info\n"
                "\t-V                                   Prints the version of libtool\n"
                "\t-encode_sdk_libraries_as_references  Encode static libs from SDK as auto-link hints instead of merging\n"
                "\t-ref-framework <Foo>                 Encode static Foo.framework as auto-link hint instead of merging\n"
                "\t-ref-l<foo>                          Encode libfoo.a as auto-link hint instead of merging\n"
                "\t-dependency_info <file>              Write an xcode dependency file\n"
                ,__progname);
    }
}

void libtool::handleFile(const CString path)
{
    _files.push_back(path);
}

Error libtool::handleOption(std::span<CString>& remainingArgs)
{
    CString arg = remainingArgs.front();
    if ( arg == "-arch_only" ) {
        if ( _ranlibMode )
            return Error("unknown option -arch_only");
        if ( remainingArgs.size() < 2 )
            return Error("-arch_only missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString      archName = remainingArgs.front();
        Architecture a        = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-arch_only %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-arch_only %s, big-endian architecture not supported", archName.c_str());
        _archOnly = a;
    }
    else if ( arg == "-o" ) {
        if ( _ranlibMode )
            return Error("unknown option -o");
        if ( remainingArgs.size() < 2 )
            return Error("-o missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString outPath = remainingArgs.front();
        _outPath = outPath;
    }
    else if ( arg == "-static" ) {
        if ( _ranlibMode )
            return Error("unknown option -static");
        // expected
    }
    else if ( arg == "-dynamic" ) {
        if ( _ranlibMode )
            return Error("unknown option -dynamic");
        return Error("-dynamic is obsolute.  Use 'clang -dynamiclib' to create a dylib");
    }
    else if ( arg == "-filelist" ) {
        if ( _ranlibMode )
            return Error("unknown option -filelist");
        if ( remainingArgs.size() < 2 )
            return Error("-filelist missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString       inputsPath = remainingArgs.front();
        CString       dirname;
        std::ifstream file(inputsPath.c_str());
        if ( !file ) {
            // check if open failed because format is `-filelist path,dirname`
            size_t commaPos = inputsPath.rfind(',');
            if ( commaPos != std::string_view::npos ) {
                // do have a comma, try path before comma
                dirname     = inputsPath.substr(commaPos + 1);
                inputsPath  = inputsPath.dupSubstr(0, commaPos);
                // try opening with this shorter path
                file.open(inputsPath.c_str());
            }
        }
        if ( !file )
            return Error("-filelist file '%s' could not be opened, errno=%d (%s)", inputsPath.c_str(), errno, strerror(errno));
        if ( Error err = ld::File::forEachLine(file, ^(CString line) {
            CString path;
            if ( !dirname.empty() )
                path = CString::concat(std::array<std::string_view, 3>{dirname, "/", line});
            else
                path = line.dup();
            _files.push_back(path);
            return Error::none();
        }) ) {
            return Error("-filelist: %s", err.message());
        }
    }
    else if ( arg == "-syslibroot" ) {
        if ( _ranlibMode )
            return Error("unknown option -syslibroot");
        // was handled in processArgs()
        remainingArgs = remainingArgs.subspan(1);
    }
    else if ( arg.starts_with("-L") ) {
        // handled in processArgs()
    }
    else if ( arg.starts_with("-F") ) {
        // handled in processArgs()
    }
    else if ( arg.starts_with("-l") ) {
        if ( arg.size() <= 2 )
            return Error("-l<name> missing name");
        CString libName = arg.substr(2);
        bool found = false;
        for (CString libSearchDir : _libSearchPaths) {
            std::string path = libSearchDir.c_str();
            if ( !path.ends_with("/") )
                path += "/";
            path += "lib";
            path += libName.c_str();
            path += ".a";
            if ( fileExists(path) ) {
                if ( _sdkLibsAsRefs && !_sdkPath.empty() && path.starts_with(_sdkPath) ) {
                    // lib is in SDK, record as an auto-link hint
                    _autoLinkLibs.push_back(arg);
                }
                else {
                    // regular .a, will be incorporated into output lib
                    _files.push_back(CString::dup(path.c_str()));
                }
                found = true;
                break;
            }
        }
        if (!found)
            return Error("could not find 'lib%s.a' for option -l%s", libName.c_str(), libName.c_str());
    }
    else if ( arg.starts_with("-ref-l") ) {
        if ( arg.size() <= 6 )
            return Error("-ref-l<name> missing name");
        CString libName = arg.substr(6);
        _autoLinkLibs.push_back(libName);
    }
    else if ( arg.starts_with("-ref-framework") ) {
        if ( remainingArgs.size() < 2 )
            return Error("-ref-framework missing framework name");
        remainingArgs = remainingArgs.subspan(1);
        CString fwName = remainingArgs.front();
        _autoLinkFrameworks.push_back(fwName);
    }
    else if ( arg == "-dependency_info" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-ref-dependency_info missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString depPath = remainingArgs.front();
        _dependencyPath = depPath;
    }
    else if ( arg == "-toc64" ) {
        _forceTOC64 = true;
    }
    else if ( arg == "-no_warning_for_no_symbols" ) {
        _warnNoSymbols = false;
    }
    else if ( arg == "-warn_duplicate_member_names" ) {
        _warnDupMembers = true;
    }
    else if ( arg == "-V" ) {
        _printVersion = true;
    }
    else if ( arg == "-v" ) {
        // does nothing
        // cctools 'libtool -v' would print out when libtool called 'ld' or 'lipo'
        _verbose = true;
    }
    else if ( arg == "-trace_file" ) {
        // used for testing.  B&I sets env var LD_TRACE_FILE instead
        if ( remainingArgs.size() < 2 )
            return Error("-ref-dependency_info missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString tracePath = remainingArgs.front();
         if ( tracePath.empty() )
            return Error("-trace_file missing path");
        _traceFilePath = tracePath;
    }
    else if ( arg == "-trace_symbols_file" ) {
        // used for testing.  B&I sets env var LD_TRACE_SYMBOLS_DIR instead
        if ( remainingArgs.size() < 2 )
            return Error("-ref-trace_symbols_file missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString tracePath = remainingArgs.front();
        if ( !tracePath.ends_with(".json") )
            return Error("-trace_symbols_file needs to end in '.json'");
        _traceSymbolsFilePath = tracePath;
    }
    else if ( (arg == "-framework") || (arg == "-exported_symbols_list") || (arg == "-unexported_symbols_list") ) {
        fprintf(stderr, "%s: warning: %s ignored creating static libraries\n", __progname, arg.c_str());
        remainingArgs = remainingArgs.subspan(1); // option has extra arg
    }
    else if ( (arg == "-all_load")  ) {
        fprintf(stderr, "%s: warning: %s ignored creating static libraries\n", __progname, arg.c_str());
    }
    else if ( arg == "-encode_sdk_libraries_as_references" ) {
        // handled in processArgs()
    }
    else {
        // all other libtool options are single chars and can be packed in one option
        for (char c : arg.substr(1)) {
            switch (c) {
            case 's':
                _sortedTOC = true;
                break;
            case 'a':
                _sortedTOC = false;
                break;
            case 'c':
                _commonsInToc = true;
                break;
            case 'f':
                _ranlib_f = true;
                break;
            case 'q':
                if ( !_ranlibMode )
                    return Error("unknown option -q");
                _ranlib_q = true;
                break;
            case 't':
                if ( !_ranlibMode )
                    return Error("unknown option -t");
                // "touch" mode in ranlib does nothing
                break;
            case 'L':
                break;
            case 'T':
                _longNames = false;
                break;
            case 'D':
                _reproducible = true;
                break;
            default:
                return Error("unknown option letter: '%c' in '%s'", c, arg.c_str());
            }
        }
    }
    return Error::none();
}


void libtool::processProgName(CString argv0)
{
    if ( argv0.ends_with("ranlib") )
        _ranlibMode = true;
}

void libtool::processEnvVars(const char* const envp[])
{
    // B&I sets ZERO_AR_DATE to make builds reproducible
    if ( const char* zeroDates = _simple_getenv((const char**)envp, "ZERO_AR_DATE") )
        _reproducible = true;

    // B&I sets LD_TRACE_FILE to file project should write json about dependency info
    if ( const char* tracePath = _simple_getenv((const char**)envp, "LD_TRACE_FILE") ) {
        // don't bother emiting trace file at all if it's set to /dev/null
        if ( CString(tracePath) != "/dev/null" )
            _traceFilePath = tracePath;
    }

    if ( !_traceFilePath.empty() ) {
        char dirPath[PATH_MAX];
        if ( const char* traceFileDir = getenv("LD_TRACE_SYMBOLS_DIR") ) {
            strncpy(dirPath, traceFileDir, PATH_MAX);
        }
        else {
            dirname_r(_traceFilePath.c_str(), dirPath);
            strlcat(dirPath, "/.LD_TRACE_SYMBOLS_DIR", PATH_MAX);
        }
        _traceSymbolsDirPath = CString::dup(dirPath);

        std::string                                         pid     = std::to_string(getpid());
        std::string                                         ppid    = std::to_string(getppid());
        std::chrono::time_point<std::chrono::system_clock>  now     = std::chrono::system_clock::now();
        std::string                                         seconds = std::to_string(now.time_since_epoch().count());
        std::string                                         path    = std::string(dirPath) + "/" + ppid + "." + pid + "." + seconds;
        _traceSymbolsFilePath = CString::dup(path);
    }
}

std::vector<CString> libtool::getObjectGlobalSymbols(const Input& input) const
{
    __block std::vector<CString> results;
    if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(input.slice.buffer) ) {
        Image image(hdr, input.slice.buffer.size(), Image::MappingKind::wholeSliceMapped);
        if ( image.hasSymbolTable() ) {
            image.symbolTable().forEachExportedSymbol(^(const Symbol&symbol, uint32_t symbolIndex, bool& stop) {
                results.push_back(symbol.name());
            });
            if ( _commonsInToc ) {
                // if -c option is used, also add tentative-defs to ToC
                image.symbolTable().forEachUndefinedSymbol(^(const Symbol& symbol, uint32_t symbolIndex, bool& stop) {
                    if ( symbol.isTentativeDef() )
                        results.push_back(symbol.name());
                });
            }
        }
    }
    else if ( UnsafeHeader::isBitCodeHeader(input.slice.buffer) ) {
#if HAVE_LIBLTO
        // Every file gets its own parsing context
        lto_module_t mod = ::lto_module_create_in_local_context(input.slice.buffer.data(), input.slice.buffer.size(), input.path.leafName().c_str());

        // lto_module_create_in_codegen_context doesn't handle errors correctly.  It returns the error code in the error case, not NULL.
        // Check for a range of bad values, including NULL
        if ( (uint64_t)mod < 65536 ) {
            fprintf(stderr,"%s: warning: could not parse bitcode object file %s: '%s', using libLTO version '%s'",
                    __progname, input.path.leafName().c_str(), ::lto_get_error_message(), ::lto_get_version());
        }
        else {
            // get names of all global symbols in bitcode module
            uint32_t count = ::lto_module_get_num_symbols(mod);
            for (uint32_t i=0; i < count; ++i) {
                const char*           name        = ::lto_module_get_symbol_name(mod, i);
                lto_symbol_attributes attr        = ::lto_module_get_symbol_attribute(mod, i);
                switch ( attr & LTO_SYMBOL_SCOPE_MASK) {
                    case LTO_SYMBOL_SCOPE_HIDDEN:
                    case LTO_SYMBOL_SCOPE_DEFAULT:
                    case LTO_SYMBOL_SCOPE_DEFAULT_CAN_BE_HIDDEN:
                        switch ( attr & LTO_SYMBOL_DEFINITION_MASK ) {
                            case LTO_SYMBOL_DEFINITION_REGULAR:
                            case LTO_SYMBOL_DEFINITION_WEAK:
                                results.push_back(CString::dup(name));
                                break;
                            case LTO_SYMBOL_DEFINITION_TENTATIVE:
                                if ( _commonsInToc )
                                    results.push_back(CString::dup(name));
                                break;
                            case LTO_SYMBOL_DEFINITION_WEAKUNDEF:
                            case LTO_SYMBOL_DEFINITION_UNDEFINED:
                                break;
                            default:
                                break;
                        }
                        break;
                    default:
                        break;
                }
            }
            ::lto_module_dispose(mod);
        }
#endif
    }
    // warn about .o files with no global symbols, unless in ranlib mode, or .o file is special auto link file
    if ( _warnNoSymbols && results.empty() && (input.memberInfo.name != ALWAYS_LOAD_MEMBER_NAME) && !_ranlibMode )
        fprintf(stderr, "%s: warning: '%s' has no symbols\n", __progname, input.path.leafName().c_str());

    return results;
}

static std::string memberNameAndTime(const Archive::Member& member)
{
    std::string tuple(member.name);
    tuple += "-";
    tuple += std::to_string(member.mtime);
    return tuple;
}

// create archive into vm_allocated()ed results buffer
Error libtool::makeThinStaticLib(std::span<const Input*> inputs, std::span<const uint8_t>& result) const
{
    // figure out if will fit in traditional table-of-content format, or if needs 64-bit version
    uint64_t totalSize = 0;
    for (const Input* input : inputs) {
        totalSize += input->memberInfo.contents.size();
    }
    const bool toc64 = this->_forceTOC64 || (totalSize > 0xFFF00000ULL);

    // build table-of-contents file (but file offsets in each ranlib entry not known yet)
    std::vector<ArchiveWriter::ToC::NameAndValue>  symbolAndMemberIndexes;
    uint64_t memberIndex = 2;
    for ( const Input* input: inputs ) {
        std::vector<CString> globals = getObjectGlobalSymbols(*input);
        for ( CString name : globals )
            symbolAndMemberIndexes.push_back({name, memberIndex});
        ++memberIndex;
    }
    size_t             tocSize = ArchiveWriter::ToC::size(symbolAndMemberIndexes, toc64);
    uint8_t*           tocPtr  = new uint8_t[tocSize];
    std::span<uint8_t> tocBuffer(tocPtr, tocSize);
    CString            tocFilename;
    if ( Error err = ArchiveWriter::ToC::make(symbolAndMemberIndexes, _sortedTOC, toc64, tocBuffer, tocFilename) )
        return err;

    // build archive
    std::vector<Archive::Member> members;
    Archive::Member symMember;
    symMember.name      = tocFilename;
    symMember.contents  = tocBuffer;
    symMember.perms     = 0100644;
    if ( _reproducible ) {
        // for reproducible builds that create identical binaries on different machines, zero transient fields
        symMember.setReproducible();
    }
    else {
        // give SYMDEF file reasonable defaults for mtime, uid, gid, and perms
        struct timespec now;
        ::clock_gettime(CLOCK_REALTIME, &now);
        // set SYMDEF mod-time to be 5 seconds into future
        symMember.mtime     = now.tv_sec+5;
        symMember.uid       = getuid();
        symMember.gid       = getgid();
    }
    members.push_back(symMember);
    std::unordered_set<std::string>      seenMemberTuples;
    std::unordered_set<std::string_view> seenMemberNames;
    for ( const Input* input : inputs ) {
        Archive::Member member = input->memberInfo;
        if ( _reproducible )
            member.setReproducible();
        while ( seenMemberTuples.contains(memberNameAndTime(member)) ) {
            // rdar://115565973 if there are duplicate file names in the archive, lldb will not be able to match
            // the OSO debug note to the correct .o file in the archive. We fix that by altering the mod-time of
            // duplicate file entries, so the combo of <name,mtime> is unique.
            member.mtime += 1;
        }
        members.push_back(member);
        seenMemberTuples.insert(memberNameAndTime(member));
        if ( _warnDupMembers ) {
            if ( seenMemberNames.contains(member.name) )
                fprintf(stderr, "%s: warning: duplicate member name '%.*s'\n", __progname, (int)member.name.size(), member.name.data());
            seenMemberNames.insert(member.name);
        }
    }

    // if any -ref args are used in -static mode, build extra .o file that lists them
    char                        extraFileTempPath[PATH_MAX];
    std::span<const uint8_t>    extraFileTempBuffer;
    if ( !_autoLinkLibs.empty() || !_autoLinkFrameworks.empty() ) {
        std::vector<const char*> libNames;
        std::vector<const char*> fwNames;
        for (CString n : _autoLinkLibs)
            libNames.push_back(n.c_str());
        for (CString n : _autoLinkFrameworks)
            fwNames.push_back(n.c_str());
        make_obj_file_with_linker_options(inputs[0]->slice.arch.cpuType(), inputs[0]->slice.arch.cpuSubtype(),
                                          (int)libNames.size(), &libNames[0],  (int)fwNames.size(), &fwNames[0], extraFileTempPath);
        extraFileTempBuffer = mapFileReadOnly(extraFileTempPath);
        Archive::Member extraMember;
        extraMember.name      = ALWAYS_LOAD_MEMBER_NAME;
        extraMember.contents  = extraFileTempBuffer;
        extraMember.perms     = 0100644;
        members.push_back(extraMember);
    }

    size_t             libSize = ArchiveWriter::size(members, _longNames);
    uint8_t*           libPtr;
    kern_return_t      kr      = ::vm_allocate(mach_task_self(), (vm_address_t*)&libPtr, (vm_size_t)libSize, VM_FLAGS_ANYWHERE);
    if ( kr != KERN_SUCCESS )
        return Error("vm_allocate(%lu) failed", libSize);
    std::span<uint8_t> libBuffer(libPtr, libSize);
    Error makeErr = ArchiveWriter::make(libBuffer, members, _longNames);
    delete[] tocPtr;
    if ( makeErr.hasError() ) {
        delete[] libPtr;
        return makeErr;
    }

    // now that we have all the member offsets, update the toc
    std::optional<Archive>          ar = Archive::isArchive(libBuffer);
    __block std::vector<uint64_t>   memberIndexToOffsets;
    __block std::span<uint8_t>      tocContent;
    __block std::string_view        tocFileName;
    assert(ar.has_value());
    memberIndexToOffsets.resize(inputs.size()+3);
    Error err = ar->forEachMember(^(const Archive::Member& m, unsigned mIndex, uint64_t mFileOffset, bool& stop) {
        if ( mIndex == 1 ) {
            // first file it table-of-contents
            tocContent  = std::span<uint8_t>((uint8_t*)m.contents.data(), m.contents.size());
            tocFileName = m.name;
        }
        else {
            memberIndexToOffsets[mIndex] = mFileOffset;
        }
    });
    if (Error tocErr = ArchiveWriter::ToC::update(tocFileName, memberIndexToOffsets, tocContent)) {
        delete[] libPtr;
        return tocErr;
    }

    // write TRACE_SYMBOLS_FILE if requested
    if ( !_traceSymbolsFilePath.empty() ) {
        Architecture traceArch = inputs.empty() ? Architecture::invalid : inputs[0]->slice.arch;
        Error traceErr = writeTraceSymbolsFile(traceArch, libBuffer, symbolAndMemberIndexes);
        if ( traceErr )
            fprintf(stderr, "%s: warning: %s\n", __progname, traceErr.message());
    }

    // if extra file added, remove temp copy
    if ( !_autoLinkLibs.empty() || !_autoLinkFrameworks.empty() ) {
        unlink(extraFileTempPath);
        unmapFile(extraFileTempBuffer);
    }

    // return allocated static library buffer
    result = libBuffer;
    return Error::none();
}

Error libtool::writeTraceSymbolsFile(Architecture arch, std::span<const uint8_t> fileBuffer, std::span<const ArchiveWriter::ToC::NameAndValue> exportNames) const
{
    if ( !_traceSymbolsDirPath.empty() ) {
        if ( int result = mkpath_np(_traceSymbolsDirPath.c_str(), 0755) ) {
            if ( result != EEXIST )
                return Error("call to mkpath_np(%s) failed due to: %s", _traceSymbolsDirPath.c_str(), strerror(errno));
        }
    }

    __block std::string jsonEntry = "{";

    jsonEntry += " \"version\":\"2\",";
    jsonEntry += " \"minor-version\":1,";
    jsonEntry += " \"name\":\"" + std::string(_outPath.leafName()) + "\",";
    CString archName = (arch == Architecture::invalid ? "none" : arch.name());
    jsonEntry += " \"arch\":\"" + std::string(archName) + "\",";
    // if mach-o, record the platform (skip for bitcode files)
    if ( std::optional<Archive> ar = Archive::isArchive(fileBuffer) ) {
        __block std::set<std::string> seenPlatformVersions;
        Error err = ar->forEachMachO(^(const Archive::Member& m, unsigned memberIndex, const UnsafeHeader* mhdr, bool& stop) {
            if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(m.contents) ) {
                __block std::string platformInfo;
                __block bool        needsPlatformComma = false;
                hdr->forEachPlatformLoadCommand(^(Platform platform, Version32 minOS, Version32 sdk) {
                    if ( needsPlatformComma )
                        platformInfo += ",";
                    else
                        needsPlatformComma = true;
                    platformInfo += " { ";
                    platformInfo += "\"name\" : \"" + std::string(platform.name().c_str()) + "\", ";
                    platformInfo += "\"min-version\" : { \"major\": \"" + std::to_string(minOS.major()) + "\", \"minor\": \"" + std::to_string(minOS.minor()) + "\" } }";
                });
                seenPlatformVersions.insert(platformInfo);
           }
        });
        if ( !seenPlatformVersions.empty() ) {
            jsonEntry += " \"platforms\": [";
            bool  needsComma = false;
            for (const std::string& pl : seenPlatformVersions ) {
                if ( needsComma )
                    jsonEntry += ",";
                needsComma = true;
                jsonEntry += pl;
            }
            jsonEntry += " ],";
        }
    }
    // show all non-local symbols in .o file as "exports"
    jsonEntry += " \"exports\": [";
    bool needsComma = false;
    for (const ArchiveWriter::ToC::NameAndValue& nameAndValue : exportNames) {
        if ( needsComma )
            jsonEntry += ",";
        jsonEntry += " \"";
        jsonEntry += nameAndValue.name;
        jsonEntry += "\"";
        needsComma = true;
    }
    jsonEntry += " ], ";

    // hash the file content
    uint8_t shaResult[32];
    sha256(fileBuffer, std::span(shaResult));
    char measureString[80];
    bytesToHex(shaResult, sizeof(shaResult), measureString);
    jsonEntry += " \"measure-sha256\": \"" + std::string(measureString) + "\"";
    jsonEntry += " }\n";

    // Write the JSON entry to the trace file.
    // Path needs arch to be added to distinguish the different slices
    std::string path = std::string(_traceSymbolsFilePath);
    if ( path.ends_with(".json") )
        path = path.substr(0, path.size() - 5);
    path += std::string(".") + arch.name() + ".json";
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT , 0666);
    if ( fd == -1 )
        return Error("can't open symbols trace file (%s): %s", path.c_str(), (const char*)strerror(errno));

    ::ftruncate(fd, 0);
    ::write(fd, jsonEntry.data(), jsonEntry.size());
    // best effort to write.  If there was an error, do not fail the build
    ::close(fd);
    return Error();
}


Tool::ErrorMap libtool::makeStaticLibrary(std::span<const CString> inputPaths, std::span<const uint8_t>& result, std::vector<CString>& archivesUsed) const
{
    __block std::vector<const Input*>   inputsToUse;
    __block ErrorMap                    errors;
    getMachOsFromPaths(inputPaths, true /*decendIntoStaticLibs*/, false /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        // filter out slices that don't match requested architecture
        for (const Input& input : inputs) {
            if ( !_archOnly.empty() && (input.slice.arch != _archOnly) ) {
                // filter out non matching slices
                bool notArch = true;
                // special case arm64e.kernel
                if ( (input.slice.arch == Architecture::arm64e_kernel) && (_archOnly == Architecture::arm64e) )
                    notArch = false;
                if ( notArch )
                    continue;
            }
            if ( input.err.hasError() )
                fprintf(stderr, "%s: warning: %s\n", __progname, input.err.message());
            else
                inputsToUse.push_back(&input);
        }
        // scan slices to see if output will be a fat file or not, and scan for archive inputs
        std::unordered_set<std::string_view> archNames;
        uint64_t                             totalSize = 0;
        for (const Input* input : inputsToUse) {
            archNames.insert(input->slice.arch.name());
            totalSize += input->memberInfo.contents.size();
            if ( input->path.ends_with(")") ) {
                size_t pos = input->path.rfind('(');
                if ( pos != std::string_view::npos ) {
                    std::string_view libPath = input->path.substr(0, pos);
                    // don't add if already used
                    bool alreadyAdded = false;
                    for (CString p : archivesUsed) {
                        if ( p == libPath ) {
                            alreadyAdded = true;
                            break;
                        }
                    }
                    if ( !alreadyAdded ) {
                        // find matching CString
                        for (CString pth : inputPaths) {
                            if ( pth == libPath ) {
                                archivesUsed.push_back(pth);
                                break;
                            }
                        }
                    }
                }

            }
        }
        if ( archNames.contains("arm64e") && archNames.contains("arm64e.kernel") ) {
            // for compatibility with some xnu builds, make a static lib with mixed ABIs
            fprintf(stderr, "%s: warning: static library will have mixture of arm64e ABIs\n", __progname);
            if (Error err = makeThinStaticLib(inputsToUse, result))
                errors[_outPath] = std::move(err);
        }
        else if ( archNames.size() >= 2 ) {
            // fat output, group inputs by arch, and make archive for each group
            std::vector<Universal::Slice> thinLibs;
            for (std::string_view archName : archNames) {
                // make list of files for this arch
                std::vector<const Input*> perArchInputs;
                for (const Input* input : inputsToUse) {
                    if ( input->slice.arch.name() == archName )
                        perArchInputs.push_back(input);
                }
                // build thin static lib with just matching arch inputs
                std::span<const uint8_t> perArchLibFile;
                if (Error err = makeThinStaticLib(perArchInputs, perArchLibFile)) {
                    errors[_outPath] = std::move(err);
                    return;
                }
                thinLibs.push_back({Architecture::byName(archName), perArchLibFile, 0, 3});
            }
            // sort slices
            std::sort(thinLibs.begin(), thinLibs.end(), [](const Universal::Slice& left, const Universal::Slice& right) -> bool {
                // if cpu types match, sort by cpu subtype
                if ( left.arch.cpuType() == right.arch.cpuType() )
                    return ((left.arch.cpuSubtype() & ~CPU_SUBTYPE_MASK) < (right.arch.cpuSubtype() & ~CPU_SUBTYPE_MASK));

                // if alignment don't match, sort by alignment
                if ( left.alignment != right.alignment )
                    return (left.alignment < right.alignment);

                // if same alignment, sort by cpu type
                return (left.arch.cpuType() < right.arch.cpuType());
            });
            // make univeral file out of this static libs
            const UniversalWriter* uw = UniversalWriter::make(thinLibs);
            result = uw->content();
            // free intermediate thin libraries
            for (const Universal::Slice& slice : thinLibs) {
                vm_deallocate(mach_task_self(), (vm_address_t)slice.buffer.data(), slice.buffer.size());
            }
            if ( _ranlibMode && _ranlib_f )
                fprintf(stderr, "ranlib: warning: '%s' will be fat and ar(1) will not be able to operate on it\n", _files[0].c_str());
        }
        else {
            // non-fat archive output
            if (Error err = makeThinStaticLib(inputsToUse, result))
                errors[_outPath] = std::move(err);
        }
    }, (_ranlibMode ? Archive::MisalignHandling::ignore : Archive::MisalignHandling::error)); // ranlib can read .a files with misaligned members
    return std::move(errors);
}

void libtool::appendUsedArchivesToTraceFile(std::span<CString> inputArchives)
{
    // build whole json dictionary on one line
    // ex: {"archives":["/tmp/libfoo.a"]}
    bool needComma = false;
    std::string str = "{\"archives\":[";
    for ( CString path : inputArchives ) {
        if ( needComma )
            str += ",";
        str += "\"";
        str += path;
        str += "\"";
        needComma = true;
    }
    str += "]}\n";

    // append json line to LD_TRACE_FILE in one write() call
    // so that file system will atomically append it to file
     int fd = ::open(_traceFilePath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
    if ( fd != -1 ) {
        ::write(fd, str.data(), str.size());
        // best effort to write.  If there was an error, do not fail the build
        ::close(fd);
    }
}

Error libtool::processArgs(std::span<CString> args)
{
    // peek ahead for -L options so that we can handle -l option in command line order
    for (int i=0; i < args.size(); ++i) {
        CString arg = args[i];
        if ( arg == "-syslibroot" ) {
            CString sdkPath = args[++i];
            if ( sdkPath.empty() )
                return Error("-syslibroot missing path");
            _sdkPath = sdkPath;
        }
        else if ( arg.starts_with("-L") ) {
            if ( arg.size() <= 2 )
                return Error("-L<path> missing path");
            CString rest = arg.substr(2);
            _libSearchPaths.push_back(rest);
        }
        else if ( arg.starts_with("-F") ) {
            // handled in processArgs()
            if ( arg.size() <= 2 )
                return Error("-F<path> missing path");
            CString fpath = arg.substr(2);
            _frameworkSearchPaths.push_back(fpath);
        }
        else if ( arg == "-encode_sdk_libraries_as_references" ) {
            _sdkLibsAsRefs = true;
        }
   }

    // fallback to standard dirs in SDK to search
    if ( !_sdkPath.empty() ) {
        CString sdkUsrLib = CString::concat(std::array<std::string_view, 2>{_sdkPath, "/usr/lib/"});
        _libSearchPaths.push_back(sdkUsrLib.c_str());
        CString sdkUsrLocalLib = CString::concat(std::array<std::string_view, 2>{_sdkPath, "/usr/local/lib/"});
        _libSearchPaths.push_back(sdkUsrLocalLib.c_str());
    }

    // call superclass which calls handleOption()
    Error err = Tool::processArgs(args);
    if ( err )
        return err;

    bool onlyVersion = _printVersion && _outPath.empty();

    // error if no file names specified
    if ( !onlyVersion && _files.empty() && _autoLinkLibs.empty() && _autoLinkFrameworks.empty() )
        return Error("no input file(s) specified");

    if ( !onlyVersion && !_ranlibMode && _outPath.empty() )
        return Error("no output file (-o) specified");

    return Error();
}

Error libtool::doRun()
{
    // just -V prints version and returns
    if ( _printVersion ) {
        printf("Apple Inc. version cctools_ld-%s\n", versionNumberStr().c_str());
        if ( _files.empty() && _outPath.empty() )
            return Error::none();
    }

    if ( _ranlibMode ) {
        // `ranlib` reads and writes to each file on command line
        for (CString file : _files) {
            // process this one file and write back to it
            _outPath = file;

            // 'ranlib -q' does nothing on fatish archives
            if ( _ranlib_q ) {
                __block bool isFatish = false;
                withReadOnlyMappedFile(_outPath.c_str(), ^(std::span<const uint8_t> fileContent){
                    if ( Universal::isUniversal(fileContent) )
                        isFatish = true;
                    else if ( auto ar = Archive::isArchive(fileContent) ) {
                        // also check if this is a static library with fat .o files
                        Error err = ar->forEachMember(^(const Archive::Member& member, unsigned mIndex, uint64_t mFileOffset, bool& stop) {
                            if ( Universal::isUniversal(member.contents) ) {
                                isFatish = true;
                                stop     = true;
                            }
                        });
                    }
                });
                if ( isFatish )
                    continue;
            }

            // rewrite ToC on specific file
            std::span<const CString> curArchivePath = std::span(&file, 1);
            std::span<const uint8_t> outBuffer;
            std::vector<CString>     inputArchives;
            Tool::ErrorMap errors = makeStaticLibrary(curArchivePath, outBuffer, inputArchives);
            if ( errors.empty() ) {
                bool success = safeSave(outBuffer.data(), outBuffer.size(), file);
                vm_deallocate(mach_task_self(), (vm_address_t)outBuffer.data(), outBuffer.size());
                if ( !success )
                    return Error("error writing file '%s'", file.c_str());

                // generate dependency info for B&I
                if ( !inputArchives.empty() && !_traceFilePath.empty() )
                    appendUsedArchivesToTraceFile(inputArchives);
            }
        }
        return Error::none();
    }

    // process all files specified on command line and only look at archs specified
    std::span<const uint8_t> outBuffer;
    std::vector<CString>     inputArchives;
    Tool::ErrorMap errors = makeStaticLibrary(_files, outBuffer, inputArchives);
    if ( errors.empty() ) {
        bool success = safeSave(outBuffer.data(), outBuffer.size(), _outPath);
        vm_deallocate(mach_task_self(), (vm_address_t)outBuffer.data(), outBuffer.size());
        if ( !success )
            return Error("error writing file '%s'", _outPath.c_str());

        // generate dependency info for xcode
        if ( !_dependencyPath.empty() ) {
            _dependencies.addOutputFileDependency(_outPath);
            for (CString file : _files)
                _dependencies.addInputFileDependency(file);
            if (Error err = _dependencies.write(_dependencyPath, versionNumberStr())) {
                // failure to write dependency file does not file the library creation
                fprintf(stderr, "%s: warning: %s\n", __progname, err.message());
            }
        }

        // generate dependency info for B&I
        if ( !inputArchives.empty() && !_traceFilePath.empty() )
            appendUsedArchivesToTraceFile(inputArchives);

        return Error::none();
    }

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
