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
#include <sys/stat.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/utils_priv.h>

// C++ STL
#include <vector>
#include <string>

// common
#include "Defines.h"
#include "Array.h"
#include "CString.h"

// mach_o
#include "Universal.h"
#include "Architecture.h"
#include "Error.h"
#include "Misc.h"
#include "Archive.h"

// mach_o_writer
#include "UniversalWriter.h"

// other_tools
#include "MiscFileUtils.h"
#include "FileUtils.h"
#include "Tool.h"
#include "lipo.h"


#ifndef CPU_SUBTYPE_ARM64E_VERSIONED_ABI_MASK
    #define CPU_SUBTYPE_ARM64E_VERSIONED_ABI_MASK 0x80000000
#endif
#ifndef CPU_SUBTYPE_ARM64E_KERNEL_ABI_MASK
    #define CPU_SUBTYPE_ARM64E_KERNEL_ABI_MASK 0x40000000
#endif


using mach_o::Universal;
using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::Archive;
using mach_o::UniversalWriter;
using mach_o::PlatformAndVersions;

#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::Lipo lipo;
    return lipo.run(argc, argv, envp);
}
#endif

namespace other_tools {

void Lipo::usage() const
{
    fprintf(stderr,
            "usage: lipo <input_file> <command> [<options> ...]\n"
            "  command is one of:\n"
            "    -create\n"
            "    -info\n"
            "    -detailed_info\n"
            "    -thin <arch>\n"
            "    -extract <arch> [-extract <arch> ...]\n"
            "    -remove <arch> [-remove <arch> ...]\n"
            "    -replace <arch> <file_name> [-replace <arch> <file_name> ...]\n"
            "    -verify_arch <arch> ...\n"
            "    -archs\n"
            "  options are one or more of:\n"
            "    -arch <arch> <input_file>\n"
            "    -o <output_file>\n"
            "    -segalign <arch> <alignment>\n"
            );
}

void Lipo::printErrorMessage(const Error& err) const
{
    if ( (this->mode == hasArch) && this->skipErrorMessage ) {
        // lipo -verify_arch prints no error message if the only problem is that the arch is missing
        return;
    }
    Tool::printErrorMessage(err);
}

Error Lipo::setMode(Mode newMode)
{
    if ( this->mode != none ) {
        // some options may be repeated
        if ( this->mode == newMode ) {
            switch (newMode) {
                case extractSlices:
                case removeSlices:
                case replaceSlices:
                    return Error::none();
                default:
                    break;
            }
        }
        return Error("only one command allowed\n");
    }
    this->mode = newMode;
    return Error::none();
}

Error Lipo::handleOption(std::span<CString>& remainingArgs)
{
    CString arg = remainingArgs.front();
    if ( arg == "-archs" ) {
        if ( Error err = setMode(listArchs) )
            return err;
    }
    else if ( arg == "-info" ) {
        if ( Error err = setMode(showInfo) )
            return err;
    }
    else if ( arg == "-detailed_info" ) {
        if ( Error err = setMode(showDetailedInfo) )
            return err;
    }
    else if ( arg == "-create" ) {
        if ( Error err = setMode(create) )
            return err;
    }
    else if ( arg == "-thin" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-thin missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        if ( Error err = setMode(extractOneSlice) )
            return err;
        thinArch = Architecture::byName(archName);
        if ( thinArch == Architecture::invalid )
            return Error("-thin %s, unknown architecture name", archName.c_str());
        if ( thinArch.isBigEndian() )
            return Error("-thin %s, big-endian architecture not supported", archName.c_str());
    }
    else if ( arg == "-extract" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-extract missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        if ( Error err = setMode(extractSlices) )
            return err;
        Architecture a = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-extract %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-extract %s, big-endian architecture not supported", archName.c_str());
        extractArchs.push_back(a);
    }
    else if ( arg == "-remove" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-remove missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        if ( Error err = setMode(removeSlices) )
            return err;
        Architecture a = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-remove %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-remove %s, big-endian architecture not supported", archName.c_str());
        removeArchs.push_back(a);
    }
    else if ( arg == "-fat64" ) {
        createFat64 = true;  // force fat64 output format
    }
    else if ( (arg == "-output") || (arg == "-o") ) {
        if ( remainingArgs.size() < 2 )
            return Error("%s missing path", arg.c_str());
        remainingArgs = remainingArgs.subspan(1);
        CString path = remainingArgs.front();
        outputPath = path;
    }
    else if ( arg == "-arch" ) {
        if ( remainingArgs.size() < 3 )
            return Error("-arch missing architecture name and input file path");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        remainingArgs = remainingArgs.subspan(1);
        CString path    = remainingArgs.front();
        Architecture a = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-arch %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-arch %s, big-endian architecture not supported", archName.c_str());
        inputPaths.push_back(path);
        inputArchOverrides[path] = a;
    }
    else if ( arg == "-replace" ) {
        if ( remainingArgs.size() < 3 )
            return Error("-replace missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        remainingArgs = remainingArgs.subspan(1);
        CString path     = remainingArgs.front();
        Architecture a = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-replace %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-replace %s, big-endian architecture not supported", archName.c_str());
        if ( Error err = setMode(replaceSlices) )
            return err;
        replacements.push_back({a, path});
    }
    else if ( arg == "-verify_arch" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-verify_arch missing architecture name");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        Architecture a = Architecture::byName(archName);
        if ( a == Architecture::invalid )
            return Error("-verify_arch %s, unknown architecture name", archName.c_str());
        if ( a.isBigEndian() )
            return Error("-verify_arch %s, big-endian architecture not supported", archName.c_str());
        if ( Error err = setMode(hasArch) )
            return err;
        verifyArch = a;
    }
    else if ( arg == "-segalign" ) {
        if ( remainingArgs.size() < 3 )
            return Error("-segalign missing <arch> <align>");
        remainingArgs = remainingArgs.subspan(1);
        CString archName = remainingArgs.front();
        Architecture arch = Architecture::byName(archName);
        if ( arch == Architecture::invalid )
            return Error("-segalign %s, unknown architecture name", archName.c_str());
        if ( arch.isBigEndian() )
            return Error("-segalign %s, big-endian architecture not supported", archName.c_str());
        remainingArgs = remainingArgs.subspan(1);
        CString alignStr = remainingArgs.front();
        char* endp;
        uint32_t value = (uint32_t)strtoul(alignStr.c_str(), &endp, 16);
        if ( *endp != '\0' )
            return Error("-segalign value '%s' not a hexadecimal number", alignStr.c_str());
        if ( value != std::bit_floor(value) )
            return Error("-segalign value '%s' is not a power of two", alignStr.c_str());
        if ( value > 0x8000 )
            return Error("-segalign value '%s' is too large (32KB align max)", alignStr.c_str());
        uint32_t p2Align   = __builtin_ctzll(value);
        archAlignments.push_back({arch, p2Align});
    }
    else {
        return Error("unknown option \"%s\"", arg.c_str());
    }
    return Error::none();
}

void Lipo::handleFile(const CString path)
{
    inputPaths.push_back(path); // whole file, no arch specifier
}

Error Lipo::doRun()
{
    switch (mode) {
        case none:
            usage();
            break;
        case listArchs:
            return doListArchs();
        case showInfo:
            return doShowInfo(false);
        case showDetailedInfo:
            return doShowInfo(true);
        case create:
            return doCreate();
        case extractOneSlice:
            return doExtractOneSlice();
        case extractSlices:
            return doExtractSlices();
        case removeSlices:
            return doRemoveSlice();
        case hasArch:
            return doVerifyArch();
        case replaceSlices:
            return doReplaceSlices();
        default:
            break;
    }
    return Error("one of -create, -thin <arch_type>, -extract <arch_type>, -remove <arch_type>, -replace <arch_type> <file_name>, -verify_arch <arch_type> ... , -archs, -info, or -detailed_info must be specified");
}

Error Lipo::doListArchs()
{
    if ( inputPaths.size() != 1 ) {
        return Error("-archs requires exactly one input file");
    }
    __block Error archsError;
    getMachOsFromPaths(inputPaths, false /*decendIntoStaticLibs*/, false /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        bool needSpace = false;
        for (const Input& input : inputs) {
            if ( input.err.noError() ) {
                if (needSpace)
                    printf(" ");
                printf("%s", input.slice.arch.name());
                needSpace = true;
            }
            else {
                archsError = Error("%s", input.err.message());
            }
        }
        if ( needSpace )
            printf("\n");   // only print if some arch was printed
    });

    return std::move(archsError);
}

const char* Lipo::capabilities(Architecture arch, char capStrBuf[128])
{
    capStrBuf[0] = '\0';
    if ( arch.usesArm64AuthPointers() ) {
        uint32_t rawBits = arch.cpuSubtype();
        if ( rawBits & CPU_SUBTYPE_ARM64E_VERSIONED_ABI_MASK ) {
            if ( rawBits & CPU_SUBTYPE_ARM64E_KERNEL_ABI_MASK)
                snprintf(capStrBuf, 128, "PTR_AUTH_VERSION KERNEL %d", CPU_SUBTYPE_ARM64_PTR_AUTH_VERSION(rawBits));
            else
                snprintf(capStrBuf, 128, "PTR_AUTH_VERSION USERSPACE %d", CPU_SUBTYPE_ARM64_PTR_AUTH_VERSION(rawBits));
        }
        else {
            snprintf(capStrBuf, 128, "0x%x", CPU_SUBTYPE_ARM64_PTR_AUTH_VERSION(rawBits));
        }
    }
    return capStrBuf;
}

Error Lipo::doShowInfo(bool detailed)
{
    if ( inputPaths.empty() ) {
        if ( detailed )
            return Error("-detailed_info requires at least one input file");
        else
            return Error("-info requires at least one input file");
    }
    __block Error result;
    for ( CString path : inputPaths ) {
        bool found = other_tools::withReadOnlyMappedFile(path.c_str(), ^(std::span<const uint8_t> buffer) {
            if ( const Universal* uni = Universal::isUniversal(buffer) ) {
                if ( Error err = uni->valid(buffer.size()) ) {
                    result = Error("%s in '%s'", err.message(), path.c_str());
                    return;
                }
                if ( detailed ) {
                    printf("Fat header in: %s\n", path.c_str());
                    uint32_t magic = __builtin_bswap32(*((uint32_t*)buffer.data()));
                    printf("fat_magic 0x%08x\n", magic);
                    printf("nfat_arch %d\n", uni->sliceCount());
                }
                else {
                    printf("Architectures in the fat file: %s are: ", path.c_str());
                }
                uni->forEachSlice(^(Universal::Slice slice, bool& stop) {
                    if ( detailed ) {
                        char capStrBuf[128];
                        printf("architecture %s\n", slice.arch.name());
                        printf("    cputype %s\n", slice.arch.cpuTypeName());
                        printf("    cpusubtype %s\n", slice.arch.cpuSubtypeName());
                        printf("    capabilities %s\n", capabilities(slice.arch, capStrBuf));
                        printf("    offset %lld\n", slice.fileOffset);
                        printf("    size %ld\n", slice.buffer.size());
                        printf("    align 2^%d (%d)\n", slice.alignment, 1 << slice.alignment);
                    }
                    else {
                        // Note: this return "arm64e" for all arm64e versions to be compatible with xbs codesigning
                        printf("%s ", slice.arch.baseName());
                    }
                });
                printf("\n");
            }
            else if ( const UnsafeHeader* mh = UnsafeHeader::isMachO(buffer) ) {
                if ( detailed )
                    printf("input file %s is not a fat file\n", path.c_str());
                printf("Non-fat file: %s is architecture: %s\n", path.c_str(), mh->arch().baseName());
            }
            else if ( UnsafeHeader::isBigEndianMachO(buffer) ) {
                printf("big-endian-mach-o: %s\n", path.c_str());
            }
            else {
                result = Error("can't figure out architecture for '%s'", path.c_str());
            }
        });
        if ( !found ) {
            return Error("file not found '%s'", path.c_str());
        }
    }
    return std::move(result);
}


// create a fat file from a list of input files (thin or fat)
Error Lipo::doCreate()
{
    if ( inputPaths.empty() )
        return Error("-create requires at least one input file");

    if ( outputPath.empty() )
        return Error("no output file specified");

    __block std::vector<const Input*>   inputsToUse;
    __block uint32_t                    filePermissions = 0644;     // output file has permission bits from last input file
    __block Error                       createError;
    // Note: decendIntoStaticLibs=false, we do not want to look at archive members, we just want to
    // look at the whole archive as a slice
    getMachOsFromPaths(inputPaths, false /*decendIntoStaticLibs*/, false /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        // support arch overrides
        std::unordered_map<const Input*, Architecture> inputToArch;
        std::vector<const Input*> emptyInputs;
        for (const Input& input : inputs) {
            if ( !replacements.empty() && (input.path == inputs[0].path) ) {
                // if we are in -replace mode and procssing main file, drop any archs that will be replace
                bool willBeReplaced = false;
                for (const ArchAndPath& ap : replacements) {
                    if ( ap.arch == input.slice.arch )
                        willBeReplaced = true;
                }
                if ( willBeReplaced )
                    continue; // don't add to inputsToUse
            }
            const auto& pos = inputArchOverrides.find(input.path);
            if ( pos != inputArchOverrides.end() ) {
                inputToArch[&input] = pos->second;
            } else {
                if ( input.err.hasError() && input.slice.arch.empty() ) {
                    emptyInputs.push_back(&input);
                    continue;
                }
                inputToArch[&input] = input.slice.arch;
            }
            inputsToUse.push_back(&input);
            filePermissions = input.memberInfo.perms;
        }

        // in -remove mode, skip over specified archs
        if ( !removeArchs.empty() ) {
            // verify arches asked to be removed actually existed
            for (Architecture arch : removeArchs) {
                bool found = false;
                for (const Input* input : inputsToUse ) {
                    if ( input->slice.arch == arch )
                        found = true;
                }
                if ( !found ) {
                    createError = Error("-remove %s specified, but fat file: '%s' 'does not contain that architecture", arch.name(), inputs[0].path.c_str());
                    return;
                }
            }
            // remove slices matching requested removals
            inputsToUse.erase(std::remove_if(inputsToUse.begin(), inputsToUse.end(), [&](const Input* mapping) {
                for (Architecture arch : removeArchs) {
                    if ( mapping->slice.arch == arch )
                        return true;
                }
                return false;
            }), inputsToUse.end());
        }

        // in -extract mode, only keep specified archs
        if ( !extractArchs.empty() ) {
            // verify arches asked to be removed actually existed
            for (Architecture arch : extractArchs) {
                bool found = false;
                for (const Input* input : inputsToUse ) {
                    if ( input->slice.arch == arch )
                        found = true;
                }
                if ( !found ) {
                    createError = Error("-extract %s specified, but fat file: '%s' 'does not contain that architecture", arch.name(), inputs[0].path.c_str());
                    return;
                }
            }
            // ignore slices not listed to be extracted
            inputsToUse.erase(std::remove_if(inputsToUse.begin(), inputsToUse.end(), [&](const Input* mapping) {
                bool doIgnore = true;
                for (Architecture arch : extractArchs) {
                    if ( mapping->slice.arch == arch )
                        doIgnore = false;
                }
                return doIgnore;
            }), inputsToUse.end());
        }

        if ( inputsToUse.empty() ) {
            for ( const Input* emptyInput : emptyInputs ) {
                fprintf(stderr, "%s: warning: %s\n", __progname, emptyInput->err.message());
            }
            createError = Error("no eligible inputs found");
            return;
        }

        // make sure there are no duplicate architectures in slices
        size_t mappingCount = inputsToUse.size();
        for (size_t i=0; i < mappingCount-1; ++i) {
            for (size_t j=i+1; j < mappingCount; ++j) {
                Architecture iArch = inputToArch[inputsToUse[i]];
                Architecture jArch = inputToArch[inputsToUse[j]];
                if ( iArch == jArch ) {
                    createError = Error("same architectures (%s) found in '%s' and '%s'",
                                        iArch.name(), inputsToUse[i]->path.c_str(), inputsToUse[j]->path.c_str());
                    return;
                }
            }
        }

        // sort slices
        std::sort(inputsToUse.begin(), inputsToUse.end(), [](const Input* left, const Input* right) -> bool {
            // if cpu types match, sort by cpu subtype
            if ( left->slice.arch.cpuType() == right->slice.arch.cpuType() )
                return ((left->slice.arch.cpuSubtype() & ~CPU_SUBTYPE_MASK) < (right->slice.arch.cpuSubtype() & ~CPU_SUBTYPE_MASK));

            // if alignment don't match, sort by alignment
            if ( left->slice.alignment != right->slice.alignment )
                return (left->slice.alignment < right->slice.alignment);

            // if same alignment, sort by cpu type
            return (left->slice.arch.cpuType() < right->slice.arch.cpuType());
        });

        // make fat file in-memory
        Universal::Slice slices[inputsToUse.size()];
        int index = 0;
        for (const Input* input : inputsToUse ) {
            slices[index].arch       = inputToArch[input];
            slices[index].buffer     = input->slice.buffer;
            slices[index].fileOffset = 0;
            slices[index].alignment  = input->slice.alignment;
            // look for -segalign override
            for (const ArchAlign& aa : archAlignments ) {
                if ( slices[index].arch == aa.arch ) {
                    slices[index].alignment = aa.p2align;
                }
            }
            ++index;
        }
        std::span<const Universal::Slice> slicesSpan(slices, inputsToUse.size());
        const UniversalWriter* uni = UniversalWriter::make(slicesSpan, createFat64);

        // double check what we just created
        if (Error err = uni->valid(uni->size()))
            createError = std::move(err);

        // write file
        if ( !uni->saveToPath(outputPath.c_str(), filePermissions) )
            createError = Error("could not write to file '%s'", outputPath.c_str());
    });
    return std::move(createError);
}

// -thin:  extract a single slice out of a fat file and write to a new file
Error Lipo::doExtractOneSlice()
{
    if ( inputPaths.size() != 1 )
        return Error("-thin requires exactly one input file");

    if ( outputPath.empty() )
        return Error("no output file specified");

    __block bool notFat        = false;
    __block bool writeFailure  = false;
    CString      inputFilePath = inputPaths[0];
    bool found = other_tools::withReadOnlyMappedFile(inputFilePath.c_str(), ^(std::span<const uint8_t> buffer, struct stat& statBuf) {
        if ( const Universal* uni = Universal::isUniversal(buffer) ) {
            uni->forEachSlice(^(Universal::Slice slice, bool& stop) {
                if (slice.arch == thinArch) {
                    // found requested slice, write it out as stand alone file
                    Archive::Member mInfo;
                    mInfo.setFileInfo(statBuf);
                    writeFailure = !safeSave(slice.buffer.data(), slice.buffer.size(), outputPath, mInfo.perms);
                    stop = true;
                }
            });
        }
        else {
            notFat = true;
        }
    });
    if ( !found )
        return Error("can't open input file '%s'", inputFilePath.c_str());
    if ( notFat )
        return Error("input file '%s' must be a fat file when -thin option used", inputFilePath.c_str());
    if ( writeFailure )
        return Error("could not write output file '%s'", outputPath.c_str());

    return Error::none();
}

// -extract [-extract]*:  extract some slices out of a fat file and write to a new fat file
Error Lipo::doExtractSlices()
{
    if ( inputPaths.size() != 1 )
        return Error("-extract requires exactly one input file");

    // doCreate() will filter by extractArchs
    return doCreate();
}


// -remove [-remove]*: remove one or more slices from a fat file
Error Lipo::doRemoveSlice()
{
    if ( inputPaths.size() != 1 )
        return Error("-remove requires exactly one input file");

    // doCreate() will filter by removeArchs
    return doCreate();
}


// -replace [-replace]*: replace one or more slices in a fat file with other content
Error Lipo::doReplaceSlices()
{
    if ( inputPaths.size() != 1 )
        return Error("-replace requires exactly one input file");

    // we emulate -replace by using all files as input files, then filtering out specific slices
    for ( const ArchAndPath& ap : replacements ) {
        inputPaths.push_back(ap.path);
    }

    // doCreate() will filter by removeArchs
    return doCreate();
}

// verify a fat file contains a specific arch
Error Lipo::doVerifyArch()
{
    if ( inputPaths.size() != 1 )
        return Error("-verify_arch requires exactly one input file");

    __block bool doesHaveArch = false;
    __block Error archsError;
    getMachOsFromPaths(inputPaths, false /*decendIntoStaticLibs*/, false /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        for (const Input& input : inputs) {
            if ( input.err.noError() ) {
                if ( input.slice.arch == verifyArch )
                    doesHaveArch = true;
            }
            else {
                archsError = Error("%s", input.err.message());
            }
        }
    });
    if ( !doesHaveArch && archsError.noError() ) {
        archsError = Error("'%s' is missing arch %s ", inputPaths[0].c_str(), verifyArch.name());
        this->skipErrorMessage = true;
    }

    return std::move(archsError);
}


} // namespace other_tools
