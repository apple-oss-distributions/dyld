/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#include <utility>

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <assert.h>

#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <unistd.h>
#endif

#include "Array.h"
#include "Header.h"
#include "Architecture.h"
#include "Misc.h"
#include "Policy.h"
#include "LoggingStub.h"

using mach_o::Architecture;
using mach_o::Platform;
using mach_o::PlatformAndVersions;
using mach_o::Policy;

namespace mach_o {


//
// MARK: --- DependentDylibAttributes ---
//

constinit const DependentDylibAttributes DependentDylibAttributes::regular;
constinit const DependentDylibAttributes DependentDylibAttributes::justWeakLink(DYLIB_USE_WEAK_LINK);
constinit const DependentDylibAttributes DependentDylibAttributes::justUpward(DYLIB_USE_UPWARD);
constinit const DependentDylibAttributes DependentDylibAttributes::justReExport(DYLIB_USE_REEXPORT);
constinit const DependentDylibAttributes DependentDylibAttributes::justDelayInit(DYLIB_USE_DELAYED_INIT);


//
// MARK: --- methods that read mach_header ---
//

bool Header::hasMachOMagic() const
{
    return ((mh.magic == MH_MAGIC) || (mh.magic == MH_MAGIC_64));
}

bool Header::hasMachOBigEndianMagic() const
{
    return ((mh.magic == MH_CIGAM) || (mh.magic == MH_CIGAM_64));
}

bool Header::is64() const
{
    return (mh.magic == MH_MAGIC_64);
}

uint32_t Header::machHeaderSize() const
{
    return is64() ? sizeof(mach_header_64) : sizeof(mach_header);
}

uint32_t Header::pointerSize() const
{
    if ( mh.magic == MH_MAGIC_64 )
        return 8;
    else
        return 4;
}

bool Header::uses16KPages() const
{
    switch ( mh.cputype ) {
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM64_32:
            return true;
        case CPU_TYPE_ARM:
            // iOS is 16k aligned for armv7/armv7s and watchOS armv7k is 16k aligned
            return mh.cpusubtype == CPU_SUBTYPE_ARM_V7K;
        default:
            return false;
    }
}

bool Header::isArch(const char* aName) const
{
    return (strcmp(aName, this->archName()) == 0);
}

const char* Header::archName() const
{
    return Architecture(&mh).name();
}

Architecture Header::arch() const
{
    return Architecture(&mh);
}

bool Header::inDyldCache() const
{
    return (mh.flags & MH_DYLIB_IN_CACHE);
}

bool Header::isDyldManaged() const
{
    switch ( mh.filetype ) {
        case MH_BUNDLE:
        case MH_EXECUTE:
        case MH_DYLIB:
            return ((mh.flags & MH_DYLDLINK) != 0);
        default:
            break;
    }
    return false;
}

bool Header::isDylib() const
{
    return (mh.filetype == MH_DYLIB);
}

bool Header::isBundle() const
{
    return (mh.filetype == MH_BUNDLE);
}

bool Header::isMainExecutable() const
{
    return (mh.filetype == MH_EXECUTE);
}

bool Header::isDynamicExecutable() const
{
    if ( mh.filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return hasLoadCommand(LC_LOAD_DYLINKER);
}

bool Header::isKextBundle() const
{
    return (mh.filetype == MH_KEXT_BUNDLE);
}

bool Header::isObjectFile() const
{
    return (mh.filetype == MH_OBJECT);
}

bool Header::isFileSet() const
{
    return (mh.filetype == MH_FILESET);
}

bool Header::isPIE() const
{
    return (mh.flags & MH_PIE);
}

bool Header::isPreload() const
{
    return (mh.filetype == MH_PRELOAD);
}

bool Header::hasWeakDefs() const
{
    return (mh.flags & MH_WEAK_DEFINES);
}

bool Header::usesWeakDefs() const
{
    return (mh.flags & MH_BINDS_TO_WEAK);
}

bool Header::hasThreadLocalVariables() const
{
    return (mh.flags & MH_HAS_TLV_DESCRIPTORS);
}

const Header* Header::isMachO(std::span<const uint8_t> content)
{
    if ( content.size() < sizeof(mach_header) )
        return nullptr;

    const Header* mh = (const Header*)content.data();
    if ( mh->hasMachOMagic() )
        return mh;
    return nullptr;
}

bool Header::mayHaveTextFixups() const
{
    // only i386 binaries support text fixups
    if ( mh.cputype == CPU_TYPE_I386 )
        return true;
    // and x86_64 kext bundles
    if ( isKextBundle() && (mh.cputype == CPU_TYPE_X86_64) )
        return true;
    return false;
}

bool Header::hasSubsectionsViaSymbols() const
{
    return (this->mh.flags & MH_SUBSECTIONS_VIA_SYMBOLS) != 0;
}

bool Header::noReexportedDylibs() const
{
    return (this->mh.flags & MH_NO_REEXPORTED_DYLIBS) != 0;
}

bool Header::isAppExtensionSafe() const
{
    return (this->mh.flags & MH_APP_EXTENSION_SAFE) != 0;
}

bool Header::isSimSupport() const
{
    return (this->mh.flags & MH_SIM_SUPPORT) != 0;
}


//
// MARK: --- methods for validating mach-o content ---
//

PlatformAndVersions Header::platformAndVersions() const
{
    // should be one platform load command (exception is zippered dylibs)
    __block PlatformAndVersions pvs;
    forEachPlatformLoadCommand(^(Platform platform, Version32 minOS, Version32 sdk) {
        Error err = pvs.zip({ platform, minOS, sdk });
        assert(err.noError());
    });
    return pvs;
}

Error Header::validSemanticsPlatform() const
{
    // should be one platform load command (exception is zippered dylibs)
    __block PlatformAndVersions pvs;
    __block Error           badPlatform;
    forEachPlatformLoadCommand(^(Platform platform, Version32 minOS, Version32 sdk) {
        if ( badPlatform.hasError() ) return;

        if ( Error err = platform.valid() ) {
            badPlatform = std::move(err);
            return;
        }
        badPlatform = pvs.zip({ platform, minOS, sdk });
    });
    if ( badPlatform )
        return std::move(badPlatform);

#if BUILDING_MACHO_WRITER
    if ( pvs.platform.empty() )
        return Error::none(); // allow empty platform in static linker
#endif

    return pvs.platform.valid();
}

Error Header::valid(uint64_t fileSize) const
{
    if ( fileSize < sizeof(mach_header) )
        return Error("file is too short");

    if ( !hasMachOMagic() )
        return Error("not a mach-o file (start is no MH_MAGIC[_64])");

    if ( Error err = validStructureLoadCommands(fileSize) )
        return err;

    if ( Error err = validSemanticsPlatform() )
        return err;

    // create policy object
    Policy policy(arch(), platformAndVersions(), mh.filetype, false);

    if ( Error err = validSemanticsUUID(policy) )
        return err;

    if ( Error err = validSemanticsInstallName(policy) )
        return err;

    if ( Error err = validSemanticsDependents(policy) )
        return err;

    if ( Error err = validSemanticsRPath(policy) )
        return err;

    if ( Error err = validSemanticsSegments(policy, fileSize) )
        return err;

    if ( Error err = validSemanticsLinkerOptions(policy) )
        return err;

    if ( isMainExecutable() ) {
        if ( Error err = validSemanticsMain(policy) )
            return err;
    }

    return Error::none();
}

static Error stringOverflow(const load_command* cmd, uint32_t index, uint32_t strOffset)
{
    if ( strOffset >= cmd->cmdsize )
        return Error("load command #%d string offset (%u) outside its size (%u)", index, strOffset, cmd->cmdsize);

    const char* str = (char*)cmd + strOffset;
    const char* end = (char*)cmd + cmd->cmdsize;
    for ( const char* s = str; s < end; ++s ) {
        if ( *s == '\0' ) {
            return Error::none();
        }
    }
    return Error("load command #%d string extends beyond end of load command", index);
}

Error Header::validStructureLoadCommands(uint64_t fileSize) const
{
    // check load command don't exceed file length
    const uint64_t headerAndLCSize = mh.sizeofcmds + machHeaderSize();
    if ( headerAndLCSize > fileSize ) {
        return Error("load commands length (%llu) exceeds length of file (%llu)", headerAndLCSize, fileSize);
    }

    // check for reconized filetype
    switch ( mh.filetype ) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_DYLINKER:
        case MH_BUNDLE:
        case MH_KEXT_BUNDLE:
        case MH_FILESET:
        case MH_PRELOAD:
        case MH_OBJECT:
            break;
        default:
            return Error("unknown filetype %d", mh.filetype);
    }

    // walk all load commands and sanity check them
    __block int   index = 1;
    __block Error lcError;
    auto          lcChecker = ^(const load_command* cmd, bool& stop) {
        const dylib_command*         dylibCmd;
        const rpath_command*         rpathCmd;
        const sub_umbrella_command*  umbrellaCmd;
        const sub_client_command*    clientCmd;
        const sub_library_command*   libraryCmd;
        const build_version_command* buildVersCmd;
        const segment_command*       segCmd;
        const segment_command_64*    seg64Cmd;
        const fileset_entry_command* fileSetCmd;
        switch ( cmd->cmd ) {
            case LC_ID_DYLIB:
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                lcError  = stringOverflow(cmd, index, dylibCmd->dylib.name.offset);
                break;
            case LC_RPATH:
                rpathCmd = (rpath_command*)cmd;
                lcError  = stringOverflow(cmd, index, rpathCmd->path.offset);
                break;
            case LC_SUB_UMBRELLA:
                umbrellaCmd = (sub_umbrella_command*)cmd;
                lcError     = stringOverflow(cmd, index, umbrellaCmd->sub_umbrella.offset);
                break;
            case LC_SUB_CLIENT:
                clientCmd = (sub_client_command*)cmd;
                lcError   = stringOverflow(cmd, index, clientCmd->client.offset);
                break;
            case LC_SUB_LIBRARY:
                libraryCmd = (sub_library_command*)cmd;
                lcError    = stringOverflow(cmd, index, libraryCmd->sub_library.offset);
                break;
            case LC_SYMTAB:
                if ( cmd->cmdsize != sizeof(symtab_command) )
                    lcError = Error("load command #%d LC_SYMTAB size wrong", index);
                break;
            case LC_DYSYMTAB:
                if ( cmd->cmdsize != sizeof(dysymtab_command) )
                    lcError = Error("load command #%d LC_DYSYMTAB size wrong", index);
                break;
            case LC_SEGMENT_SPLIT_INFO:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_SEGMENT_SPLIT_INFO size wrong", index);
                break;
            case LC_ATOM_INFO:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_ATOM_INFO size wrong", index);
                break;
            case LC_FUNCTION_STARTS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_FUNCTION_STARTS size wrong", index);
                break;
            case LC_DYLD_EXPORTS_TRIE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_DYLD_EXPORTS_TRIE size wrong", index);
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_DYLD_CHAINED_FIXUPS size wrong", index);
                break;
            case LC_ENCRYPTION_INFO:
                if ( cmd->cmdsize != sizeof(encryption_info_command) )
                    lcError = Error("load command #%d LC_ENCRYPTION_INFO size wrong", index);
                break;
            case LC_ENCRYPTION_INFO_64:
                if ( cmd->cmdsize != sizeof(encryption_info_command_64) )
                    lcError = Error("load command #%d LC_ENCRYPTION_INFO_64 size wrong", index);
                break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                if ( cmd->cmdsize != sizeof(dyld_info_command) )
                    lcError = Error("load command #%d LC_DYLD_INFO_ONLY size wrong", index);
                break;
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_TVOS:
            case LC_VERSION_MIN_WATCHOS:
                if ( cmd->cmdsize != sizeof(version_min_command) )
                    lcError = Error("load command #%d LC_VERSION_MIN_* size wrong", index);
                break;
            case LC_UUID:
                if ( cmd->cmdsize != sizeof(uuid_command) )
                    lcError = Error("load command #%d LC_UUID size wrong", index);
                break;
            case LC_BUILD_VERSION:
                buildVersCmd = (build_version_command*)cmd;
                if ( cmd->cmdsize != (sizeof(build_version_command) + buildVersCmd->ntools * sizeof(build_tool_version)) )
                    lcError = Error("load command #%d LC_BUILD_VERSION size wrong", index);
                break;
            case LC_MAIN:
                if ( cmd->cmdsize != sizeof(entry_point_command) )
                    lcError = Error("load command #%d LC_MAIN size wrong", index);
                break;
            case LC_SEGMENT:
                segCmd = (segment_command*)cmd;
                if ( cmd->cmdsize != (sizeof(segment_command) + segCmd->nsects * sizeof(section)) )
                    lcError = Error("load command #%d LC_SEGMENT size does not match number of sections", index);
                break;
            case LC_SEGMENT_64:
                seg64Cmd = (segment_command_64*)cmd;
                if ( cmd->cmdsize != (sizeof(segment_command_64) + seg64Cmd->nsects * sizeof(section_64)) )
                    lcError = Error("load command #%d LC_SEGMENT_64 size does not match number of sections", index);
                break;
            case LC_FILESET_ENTRY:
                fileSetCmd = (fileset_entry_command*)cmd;
                lcError    = stringOverflow(cmd, index, fileSetCmd->entry_id.offset);
                break;
            default:
                if ( cmd->cmd & LC_REQ_DYLD )
                    lcError = Error("load command #%d unknown required load command 0x%08X", index, cmd->cmd);
                break;
        }
        ++index;
        if ( lcError )
            stop = true;
    };
    if ( Error err = this->forEachLoadCommand(lcChecker) )
        return err;
    if ( lcError )
        return std::move(lcError);
    /*
    // check load commands fit in TEXT segment
    if ( this->isDyldManaged() ) {
        __block bool foundTEXT = false;
        __block Error segError;
        forEachSegment(^(const SegmentInfo& segInfo, bool& stop) {
            if ( strcmp(segInfo.segName, "__TEXT") == 0 ) {
                foundTEXT = true;
                if ( headerAndLCSize > segInfo.fileSize ) {
                    segError = Error("load commands (%llu) exceed length of __TEXT segment (%llu)", headerAndLCSize, segInfo.fileSize);
                }
                if ( segInfo.fileOffset != 0 ) {
                    segError = Error("__TEXT segment not start of mach-o (%llu)", segInfo.fileOffset);
                }
                stop = true;
            }
        });
        if ( segError )
            return std::move(segError);
        if ( !foundTEXT ) {
            return Error("missing __TEXT segment");
        }
    }
*/
    return Error::none();
}


Error Header::validSemanticsUUID(const Policy& policy) const
{
    // should have at most one LC_UUID
    __block unsigned uuidCount = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UUID )
            ++uuidCount;
    });
    if ( uuidCount > 1 )
        return Error("too many LC_UUID load commands");
    if ( (uuidCount == 0) && policy.enforceHasUUID() )
        return Error("missing LC_UUID load command");

    return Error::none();
}

Error Header::validSemanticsInstallName(const Policy& policy) const
{
    __block const char* installName = nullptr;
    __block int         foundCount  = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ID_DYLIB ) {
            const dylib_command* dylibCmd = (dylib_command*)cmd;
            installName                   = (char*)dylibCmd + dylibCmd->dylib.name.offset;
            ++foundCount;
        }
    });
    if ( foundCount > 1 )
        return Error("multiple LC_ID_DYLIB found");

    if ( this->isDylib() ) {
        if ( installName == nullptr )
            return Error("MH_DYLIB is missing LC_ID_DYLIB");
#if 0 // FIXME: need path plumbed down
        if ( policy.enforceInstallNamesAreRealPaths() ) {
            // new binary, so check that part after @xpath/ is real (not symlinks)
            if ( (strncmp(installName, "@loader_path/", 13) == 0) || (strncmp(installName, "@executable_path/", 17) == 0) ) {
                if ( const char* s = strchr(installName, '/') ) {
                    while (strncmp(s, "/..", 3) == 0)
                        s += 3;
                    const char* trailingInstallPath = s;
                    const char* trailingRealPath = &path[strlen(path)-strlen(trailingInstallPath)];
                    if ( strcmp(trailingRealPath, trailingInstallPath) != 0 ) {
                        Error("install name '%s' contains symlinks", installName);
                    }
                }
            }
        }
#endif
    }
    else {
        if ( installName != nullptr )
            return Error("found LC_ID_DYLIB found in non-MH_DYLIB");
    }

    return Error::none();
}

Error Header::validSemanticsDependents(const Policy& policy) const
{
    // gather info
    __block Error dupDepError;
    __block int   depCount = 0;
    const char*   depPathsBuffer[256];
    const char**  depPaths = depPathsBuffer;
    const bool    enforceNoDupDylibs = policy.enforceNoDuplicateDylibs();
    const bool    hasWarningHandler = mach_o::hasWarningHandler();
    // don't use forEachDependentDylib, because it synthesizes libSystem.dylib
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                const char*          loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                if ( (depCount < 256) && ( enforceNoDupDylibs || hasWarningHandler ) ) {
                    for ( int i = 0; i < depCount; ++i ) {
                        if ( strcmp(loadPath, depPaths[i]) == 0 ) {
                            if ( enforceNoDupDylibs ) {
                                dupDepError = Error("duplicate dependent dylib '%s'", loadPath);
                                stop        = true;
                            } else
                                warning(this, "duplicate dependent dylib are deprecated ('%s')", loadPath);
                        }
                    }
                    depPaths[depCount] = loadPath;
                }
                ++depCount;
            } break;
        }
    });
    if ( dupDepError )
        return std::move(dupDepError);

    // all new binaries must link with something
    if ( this->isDyldManaged() && policy.enforceHasLinkedDylibs() && (depCount == 0) ) {
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
        const char* installName = this->installName();
        bool isLibSystem = false;
        if (installName != nullptr) {
            if ( this->builtForPlatform(Platform::driverKit, true) ) {
                const char* libSystemDir = "/System/DriverKit/usr/lib/system/";
                if ( strncmp(installName, libSystemDir, strlen(libSystemDir)) != 0 )
                    isLibSystem = true;
            } else if ( this->platformAndVersions().platform.isExclaveKit() ) {
                const char* libSystemDir = "/System/ExclaveKit/usr/lib/system/";
                if ( strncmp(installName, libSystemDir, strlen(libSystemDir)) != 0 )
                    isLibSystem = true;
            } else {
                const char* libSystemDir = "/usr/lib/system/";
                if ( strncmp(installName, libSystemDir, strlen(libSystemDir)) != 0 )
                    isLibSystem = true;
            }
        }
        if ( !isLibSystem )
            return Error("missing LC_LOAD_DYLIB (must link with at least libSystem.dylib)");
    }
    return Error::none();
}

Error Header::validSemanticsRPath(const Policy& policy) const
{
    const bool enforceNoDupRPath = policy.enforceNoDuplicateRPaths();
    if ( !enforceNoDupRPath && !hasWarningHandler() )
        return Error::none();

    __block Error dupRPathError;
    __block int   rpathCount = 0;
    const char*   rpathsBuffer[64];
    const char**  rpaths = rpathsBuffer;
    forEachRPath(^(const char* rPath, bool& stop) {
        if ( rpathCount < 64 ) {
            for ( int i = 0; i < rpathCount; ++i ) {
                if ( strcmp(rPath, rpaths[i]) == 0 ) {
                    // rdar://115775065 (ld-prime warns about duplicate LC_RPATH in external libraries)
                    // there's no need to warn here, only error when the policy should be enforced
                    // it's because ld now filters out and warns about duplicate -rpath options when linking
                    if ( enforceNoDupRPath ) {
                        dupRPathError = Error("duplicate LC_RPATH '%s'", rPath);
                        stop          = true;
                    }
                }
            }
            rpaths[rpathCount] = rPath;
        }
        ++rpathCount;
    });
    return std::move(dupRPathError);
}

#if !TARGET_OS_EXCLAVEKIT
template <typename SG, typename SC>
Error Header::validSegment(const Policy& policy, uint64_t wholeFileSize, const SG* seg) const
{
    if ( greaterThanAddOrOverflow(seg->fileoff, seg->filesize, wholeFileSize) )
        return Error("segment '%s' load command content extends beyond end of file", seg->segname);

    // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
    if ( !isObjectFile() ) {
        if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) )
            return Error("segment '%s' filesize exceeds vmsize", seg->segname);
    }

    // check permission bits
    if ( (seg->initprot & 0xFFFFFFF8) != 0 ) {
        return Error("%s segment permissions has invalid bits set (0x%08X)", seg->segname, seg->initprot);
    }
    if ( policy.enforceTextSegmentPermissions() ) {
        if ( (strcmp(seg->segname, "__TEXT") == 0) && (seg->initprot != (VM_PROT_READ | VM_PROT_EXECUTE)) )
            return Error("__TEXT segment permissions is not 'r-x'");
    }
    if ( policy.enforceReadOnlyLinkedit() ) {
        if ( (strcmp(seg->segname, "__LINKEDIT") == 0) && (seg->initprot != VM_PROT_READ) )
            return Error("__LINKEDIT segment permissions is not 'r--'");
    }
    if ( policy.enforceDataSegmentPermissions() ) {
        if ( (strcmp(seg->segname, "__DATA") == 0) && (seg->initprot != (VM_PROT_READ | VM_PROT_WRITE)) )
            return Error("__DATA segment permissions is not 'rw-'");
        if ( strcmp(seg->segname, "__DATA_CONST") == 0 ) {
            if ( seg->initprot != (VM_PROT_READ | VM_PROT_WRITE) )
                return Error("__DATA_CONST segment permissions is not 'rw-'");
            if ( (seg->flags & SG_READ_ONLY) == 0 ) {
                if ( this->isDylib() && this->hasSplitSegInfo() ) {
                    // dylibs in dyld cache are allowed to not have SG_READ_ONLY set
                }
                else {
                    return Error("__DATA_CONST segment missing SG_READ_ONLY flag");
                }
            }
        }
    }

    // check for vmaddr wrapping
    if ( (seg->vmaddr + seg->vmsize) < seg->vmaddr )
        return Error("'%s' segment vm range wraps", seg->segname);

    // check sections are within its segment
    const SC* const sectionsStart = (SC*)((char*)seg + sizeof(SG));
    const SC* const sectionsEnd   = &sectionsStart[seg->nsects];
    for ( const SC* sect = sectionsStart; (sect < sectionsEnd); ++sect ) {
        if ( (int64_t)(sect->size) < 0 ) {
            return Error("section '%s' size too large 0x%lX", sect->sectname, (size_t)sect->size);
        }
        else if ( sect->addr < seg->vmaddr ) {
            return Error("section '%s' start address 0x%lX is before containing segment's address 0x%0lX", sect->sectname, (size_t)sect->addr, (size_t)seg->vmaddr);
        }
        else if ( policy.enforceSectionsInSegment() && (sect->addr + sect->size > seg->vmaddr + seg->vmsize) ) {
            return Error("section '%s' end address 0x%lX is beyond containing segment's end address 0x%0lX", sect->sectname, (size_t)(sect->addr + sect->size), (size_t)(seg->vmaddr + seg->vmsize));
        }
    }

    return Error::none();
}

#endif // !TARGET_OS_EXCLAVEKIT
struct Interval
{
    bool     overlaps(const Interval& other) const;
    uint64_t start;
    uint64_t end;
};

bool Interval::overlaps(const Interval& other) const
{
    return ((other.start < this->end) && (other.end > this->start));
}

Error Header::validSemanticsSegments(const Policy& policy, uint64_t fileSize) const
{
    // check each segment load command in isolation
    struct SegRange
    {
        Interval    vm;
        Interval    file;
        const char* name;
    };
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(SegRange, ranges, 12);
    __block Error     lcError;
    __block bool      hasTEXT              = false;
    __block bool      hasLINKEDIT          = false;
    __block uint64_t segmentIndexText     = 0;
    __block uint64_t segmentIndexLinkedit = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg64 = (segment_command_64*)cmd;
            if ( strcmp(seg64->segname, "__TEXT") == 0 ) {
                hasTEXT          = true;
                segmentIndexText = ranges.count();
            }
            else if ( strcmp(seg64->segname, "__LINKEDIT") == 0 ) {
                hasLINKEDIT          = true;
                segmentIndexLinkedit = ranges.count();
            }
            lcError = validSegment<segment_command_64, section_64>(policy, fileSize, seg64);
            ranges.push_back({ { seg64->vmaddr, seg64->vmaddr + seg64->vmsize }, { seg64->fileoff, seg64->fileoff + seg64->filesize }, seg64->segname });
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg32 = (segment_command*)cmd;
            if ( strcmp(seg32->segname, "__TEXT") == 0 ) {
                hasTEXT          = true;
                segmentIndexText = ranges.count();
            }
            else if ( strcmp(seg32->segname, "__LINKEDIT") == 0 ) {
                hasLINKEDIT          = true;
                segmentIndexLinkedit = ranges.count();
            }
            lcError = validSegment<segment_command, section>(policy, fileSize, seg32);
            ranges.push_back({ { seg32->vmaddr, seg32->vmaddr + seg32->vmsize }, { seg32->fileoff, seg32->fileoff + seg32->filesize }, seg32->segname });
        }
        if ( lcError )
            stop = true;
    });
    if ( lcError )
        return std::move(lcError);

    // dynamic binaries have further restrictions
    if ( isDyldManaged() ) {
        if ( hasTEXT ) {
            if ( ranges[segmentIndexText].file.start != 0 )
                return Error("__TEXT segment fileoffset is not zero");
            const uint32_t headerAndLCSize = machHeaderSize() + mh.sizeofcmds;
            if ( ranges[segmentIndexText].file.end < headerAndLCSize )
                return Error("load commands do not fit in __TEXT segment");
        }
        else {
            return Error("missing __TEXT segment");
        }
        // FIXME: LINKEDIT checks need to move to Analyzer
        //if ( !hasLINKEDIT )
        //    return Error("missing __LINKEDIT segment");
    }

    // check for overlapping segments, by looking at every possible pair of segments
    for ( const SegRange& r1 : ranges ) {
        for ( const SegRange& r2 : ranges ) {
            if ( &r1 == &r2 )
                continue;
            if ( r1.vm.overlaps(r2.vm) )
                return Error("vm range of segment '%s' overlaps segment '%s'", r1.name, r2.name);
            if ( r1.file.overlaps(r2.file) )
                return Error("file range of segment '%s' overlaps segment '%s'", r1.name, r2.name);
        }
    }

    // check segment load command order matches file content order which matches vm order
    // skip dyld cache because segments are moved around too much
    if ( policy.enforceSegmentOrderMatchesLoadCmds() && !inDyldCache() ) {
        const SegRange* last = nullptr;
        for ( const SegRange& r : ranges ) {
            if ( last != nullptr ) {
                if ( (r.file.start < last->file.start) && (r.file.start != r.file.end) )
                    return Error("segment '%s' file offset out of order", r.name);
                if ( r.vm.start < last->vm.start ) {
                    if ( isFileSet() && (strcmp(r.name, "__PRELINK_INFO") == 0) ) {
                        // __PRELINK_INFO may have no vmaddr set
                    }
                    else {
                        return Error("segment '%s' vm address out of order", r.name);
                    }
                }
            }
            last = &r;
        }
    }

    return Error::none();
}

Error Header::validSemanticsMain(const Policy& policy) const
{
    if ( this->inDyldCache() && policy.enforceMainFlagsCorrect() )
        return Error("MH_EXECUTE has MH_DYLIB_IN_CACHE bit set");

    // validate the correct number of LC_MAIN or LC_UNIXTHREAD
    __block Error    lcError;
    __block uint64_t startAddress              = 0;
    __block const entry_point_command* mainCmd = nullptr;
    __block const thread_command* threadCmd    = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_MAIN:
                if ( mainCmd != nullptr )
                    lcError = Error("multiple LC_MAIN load commands");
                mainCmd = (entry_point_command*)cmd;
                break;
            case LC_UNIXTHREAD:
                if ( threadCmd != nullptr )
                    lcError = Error("multiple LC_UNIXTHREAD load commands");
                threadCmd = (thread_command*)cmd;
                if ( !entryAddrFromThreadCmd(threadCmd, startAddress) )
                    lcError = Error("invalid LC_UNIXTHREAD");
                break;
        }
    });
    if ( lcError )
        return std::move(lcError);
    if ( (mainCmd != nullptr) && (threadCmd != nullptr) )
        return Error("can't have LC_MAIN and LC_UNIXTHREAD load commands");
    if ( this->builtForPlatform(Platform::driverKit) ) {
        if ( (mainCmd != nullptr) || (threadCmd != nullptr) )
            return Error("LC_MAIN not allowed for driverkit");
    }
    else {
        if ( (mainCmd == nullptr) && (threadCmd == nullptr) )
            return Error("missing LC_MAIN or LC_UNIXTHREAD in main executable");
    }

    // FIXME: validate LC_MAIN or LC_UNIXTHREAD points into executable segment
    return Error::none();
}

Error Header::validSemanticsLinkerOptions(const Policy& policy) const
{
    __block Error     lcError;

    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( cmd->cmd == LC_LINKER_OPTION ) {
            const char*     begin = (char*)cmd + sizeof(linker_option_command);
            const char*     end = (char*)cmd + cmd->cmdsize;
            const uint32_t  count = ((linker_option_command*)cmd)->count;
            for ( uint32_t i = 0; i < count; ++i ) {
                const char* next = begin + strlen(begin) + 1;
                if ( next > end ) {
                    lcError = Error("malformed LC_LINKER_OPTION command");
                    stop = true;
                    return;
                }
                begin = next;
            }
        }
    });

    return std::move(lcError);
}

Error Header::forEachLoadCommand(void (^callback)(const load_command* cmd, bool& stop)) const
{
    bool                stop      = false;
    const load_command* startCmds = nullptr;
    if ( mh.magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char*)this + sizeof(mach_header_64));
    else if ( mh.magic == MH_MAGIC )
        startCmds = (load_command*)((char*)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return Error("big endian mach-o file");
    else {
        const uint32_t* h = (uint32_t*)this;
        return Error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h[1]);
    }
    if ( mh.filetype > 12 )
        return Error("unknown mach-o filetype (%u)", mh.filetype);
    //    const uint32_t ptrSize = this->pointerSize();
    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + mh.sizeofcmds);
    const load_command*       cmd     = startCmds;
    for ( uint32_t i = 1; (i <= mh.ncmds) && !stop; ++i ) {
        const load_command* nextCmd = (load_command*)((char*)cmd + cmd->cmdsize);
        if ( cmd >= cmdsEnd ) {
            return Error("malformed load command (%d of %d) at %p with mh=%p, off end of load commands", i, mh.ncmds, cmd, this);
        }
        if ( cmd->cmdsize < 8 ) {
            return Error("malformed load command (%d of %d) at %p with mh=%p, size (0x%X) too small", i, mh.ncmds, cmd, this, cmd->cmdsize);
        }
#if 0
        // check the cmdsize is pointer aligned
        if ( checks.pointerAlignedLoadCommands ) {
            if ( (cmd->cmdsize % ptrSize) != 0 ) {
                return Error("malformed load command (%d of %d) at %p with mh=%p, size (0x%X) is not pointer sized", i, mh.ncmds, cmd, this, cmd->cmdsize);
            }
        }
#endif
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            return Error("malformed load command (%d of %d) at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, mh.ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
        }
        callback(cmd, stop);
        cmd = nextCmd;
    }
    return Error::none();
}

// This forEach is only used after the load commands have been validated, so no need to return Error and handle it
void Header::forEachLoadCommandSafe(void (^callback)(const load_command* cmd, bool& stop)) const
{
    if ( Error err = forEachLoadCommand(callback) )
        assert("Header::forEachLoadCommand()");
}

bool Header::hasLoadCommand(uint32_t cmdNum) const
{
    __block bool hasLC = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == cmdNum ) {
            hasLC = true;
            stop  = true;
        }
    });
    return hasLC;
}

bool Header::isStaticExecutable() const
{
    if ( mh.filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return !hasLoadCommand(LC_LOAD_DYLINKER);
}

//
// MARK: --- methods that read Platform load commands ---
//

void Header::forEachPlatformLoadCommand(void (^handler)(Platform platform, Version32 minOS, Version32 sdk)) const
{
    __block bool foundPlatform = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        const build_version_command* buildCmd = (build_version_command*)cmd;
        const version_min_command*   versCmd  = (version_min_command*)cmd;
        uint32_t                     sdk;
        switch ( cmd->cmd ) {
            case LC_BUILD_VERSION:
                handler(Platform(buildCmd->platform), Version32(buildCmd->minos), Version32(buildCmd->sdk));
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_MACOSX:
                sdk = versCmd->sdk;
                // The original LC_VERSION_MIN_MACOSX did not have an sdk field, assume sdk is same as minOS for those old binaries
                if ( sdk == 0 )
                    sdk = versCmd->version;
                handler(Platform::macOS, Version32(versCmd->version), Version32(sdk));
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_IPHONEOS:
                if ( (mh.cputype == CPU_TYPE_X86_64) || (mh.cputype == CPU_TYPE_I386) )
                    handler(Platform::iOS_simulator, Version32(versCmd->version), Version32(versCmd->sdk)); // old sim binary
                else
                    handler(Platform::iOS, Version32(versCmd->version), Version32(versCmd->sdk));
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_TVOS:
                if ( mh.cputype == CPU_TYPE_X86_64 )
                    handler(Platform::tvOS_simulator, Version32(versCmd->version), Version32(versCmd->sdk)); // old sim binary
                else
                    handler(Platform::tvOS, Version32(versCmd->version), Version32(versCmd->sdk));
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_WATCHOS:
                if ( (mh.cputype == CPU_TYPE_X86_64) || (mh.cputype == CPU_TYPE_I386) )
                    handler(Platform::watchOS_simulator, Version32(versCmd->version), Version32(versCmd->sdk)); // old sim binary
                else
                    handler(Platform::watchOS, Version32(versCmd->version), Version32(versCmd->sdk));
                foundPlatform = true;
                break;
        }
    });
#ifdef BUILDING_MACHO_WRITER
    // no implicit platforms in static linker
    // but for object files only, we need to support linking against old macos dylibs
    if ( isObjectFile() )
        return;
#endif

    if ( !foundPlatform ) {
        // old binary with no explicit platform
#if TARGET_OS_OSX
        if ( (mh.cputype == CPU_TYPE_X86_64) | (mh.cputype == CPU_TYPE_I386) )
            handler(Platform::macOS, Version32(10, 5), Version32(10, 5)); // guess it is a macOS 10.5 binary
        // <rdar://problem/75343399>
        // The Go linker emits non-standard binaries without a platform and we have to live with it.
        if ( mh.cputype == CPU_TYPE_ARM64 )
            handler(Platform::macOS, Version32(11, 0), Version32(11, 0)); // guess it is a macOS 11.0 binary
#endif
    }
}

bool Header::builtForPlatform(Platform reqPlatform, bool onlyOnePlatform) const
{
    PlatformAndVersions pvs = platformAndVersions();

    if ( pvs.platform == reqPlatform )
        return true;

    if ( onlyOnePlatform )
        return false;

    __block bool match = false;
    pvs.unzip(^(PlatformAndVersions pvers) {
        match |= pvers.platform == reqPlatform;
    });

    return match;
}

bool Header::isZippered() const
{
    return platformAndVersions().platform == Platform::zippered;
}

bool Header::allowsAlternatePlatform() const
{
    __block bool result = false;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__allow_alt_plat") && info.segmentName.starts_with("__DATA") ) {
            result = true;
            stop   = true;
        }
    });
    return result;
}

const char* Header::installName() const
{
    const char* name;
    Version32   compatVersion;
    Version32   currentVersion;
    if ( getDylibInstallName(&name, &compatVersion, &currentVersion) )
        return name;
    return nullptr;
}

bool Header::getDylibInstallName(const char** installName, Version32* compatVersion, Version32* currentVersion) const
{
    __block bool found = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ID_DYLIB ) {
            const dylib_command* dylibCmd = (dylib_command*)cmd;
            *compatVersion                = Version32(dylibCmd->dylib.compatibility_version);
            *currentVersion               = Version32(dylibCmd->dylib.current_version);
            *installName                  = (char*)dylibCmd + dylibCmd->dylib.name.offset;
            found                         = true;
            stop                          = true;
        }
    });
    return found;
}

bool Header::getUuid(uuid_t uuid) const
{
    __block bool found = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UUID ) {
            const uuid_command* uc = (const uuid_command*)cmd;
            memcpy(uuid, uc->uuid, sizeof(uuid_t));
            found = true;
            stop  = true;
        }
    });
    if ( !found )
        bzero(uuid, sizeof(uuid_t));
    return found;
}


const char* Header::dependentDylibLoadPath(uint32_t depIndex) const
{
    __block uint32_t     curIndex  = 0;
    __block const char*  result    = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                if ( curIndex == depIndex )
                    result = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                ++curIndex;
            } break;
        }
    });
    return result;
}

uint32_t Header::dependentDylibCount(bool* allDepsAreNormal) const
{
    if ( allDepsAreNormal != nullptr )
        *allDepsAreNormal = true;
    __block unsigned count   = 0;
    this->forEachDependentDylib(^(const char* loadPath, DependentDylibAttributes kind, Version32 , Version32 , bool& stop) {
        if ( allDepsAreNormal != nullptr ) {
            if ( kind != DependentDylibAttributes::regular )
                *allDepsAreNormal = false;  // record if any linkages were weak, re-export, upward, or delay-init
        }
        ++count;
    });
    return count;
}

DependentDylibAttributes Header::loadCommandToDylibKind(const dylib_command* dylibCmd)
{
    DependentDylibAttributes attr;
    const dylib_use_command* dylib2Cmd = (dylib_use_command*)dylibCmd;
    if ( (dylib2Cmd->marker == 0x1a741800) && (dylib2Cmd->nameoff == sizeof(dylib_use_command)) ) {
        attr.raw = (uint8_t)dylib2Cmd->flags;
        return attr;
    }
    switch ( dylibCmd->cmd ) {
        case LC_LOAD_DYLIB:
            return attr;
        case LC_LOAD_WEAK_DYLIB:
            attr.weakLink = true;
            return attr;
        case LC_REEXPORT_DYLIB:
            attr.reExport = true;
            return attr;
        case LC_LOAD_UPWARD_DYLIB:
            attr.upward = true;
            return attr;
    }
    assert(0 && "not a dylib load command");
}

void Header::forEachDependentDylib(void (^callback)(const char* loadPath, DependentDylibAttributes kind,
                                                    Version32 compatVersion, Version32 curVersion, bool& stop)) const
{
    __block unsigned count   = 0;
    __block bool     stopped = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                const char*          loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                callback(loadPath, loadCommandToDylibKind(dylibCmd), Version32(dylibCmd->dylib.compatibility_version), Version32(dylibCmd->dylib.current_version), stop);
                ++count;
                if ( stop )
                    stopped = true;
                break;
            }
        }
    });
#if BUILDING_DYLD
    // everything must link with something
    if ( (count == 0) && !stopped ) {
        // The dylibs that make up libSystem can link with nothing
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
        if ( this->builtForPlatform(Platform::driverKit, true) ) {
            if ( !this->isDylib() || (strncmp(this->installName(), "/System/DriverKit/usr/lib/system/", 33) != 0) )
                callback("/System/DriverKit/usr/lib/libSystem.B.dylib", DependentDylibAttributes::regular, Version32(1, 0), Version32(1, 0), stopped);
        } else if ( this->platformAndVersions().platform.isExclaveKit() ) {
            if ( !this->isDylib() || (strncmp(this->installName(), "/System/ExclaveKit/usr/lib/system/", 34) != 0) )
                callback("/System/ExclaveKit/usr/lib/libSystem.dylib", DependentDylibAttributes::regular, Version32(1, 0), Version32(1, 0), stopped);
        }
        else {
            if ( !this->isDylib() || (strncmp(this->installName(), "/usr/lib/system/", 16) != 0) )
                callback("/usr/lib/libSystem.B.dylib", DependentDylibAttributes::regular, Version32(1, 0), Version32(1, 0), stopped);
        }
    }
#endif
}

void Header::forDyldEnv(void (^callback)(const char* envVar, bool& stop)) const
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_DYLD_ENVIRONMENT ) {
            const dylinker_command* envCmd         = (dylinker_command*)cmd;
            const char*             keyEqualsValue = (char*)envCmd + envCmd->name.offset;
            // only process variables that start with DYLD_ and end in _PATH
            if ( (strncmp(keyEqualsValue, "DYLD_", 5) == 0) ) {
                const char* equals = strchr(keyEqualsValue, '=');
                if ( equals != NULL ) {
                    if ( strncmp(&equals[-5], "_PATH", 5) == 0 ) {
                        callback(keyEqualsValue, stop);
                    }
                }
            }
        }
    });
}

bool Header::entryAddrFromThreadCmd(const thread_command* cmd, uint64_t& addr) const
{
    const uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
    const uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);
    uint32_t        flavor = *((uint32_t*)(((char*)cmd) + 8));
    switch ( mh.cputype ) {
        case CPU_TYPE_I386:
            if ( flavor == 1 ) {   // i386_THREAD_STATE
                addr = regs32[10]; // i386_thread_state_t.eip
                return true;
            }
            break;
        case CPU_TYPE_X86_64:
            if ( flavor == 4 ) {   // x86_THREAD_STATE64
                addr = regs64[16]; // x86_thread_state64_t.rip
                return true;
            }
            break;
        case CPU_TYPE_ARM:
            if ( flavor == 1 ) {   // ARM_THREAD_STATE
                addr = regs32[15]; // arm_thread_state_t.pc
                return true;
            }
            break;
        case CPU_TYPE_ARM64:
            if ( flavor == 6 ) {   // ARM_THREAD_STATE64
                addr = regs64[32]; // arm_thread_state64_t.__pc
                return true;
            }
            break;
    }
    return false;
}

// returns false if entry point not found
bool Header::getEntry(uint64_t& offset, bool& usesCRT) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_MAIN ) {
            entry_point_command* mainCmd = (entry_point_command*)cmd;
            offset                       = mainCmd->entryoff;
            usesCRT                      = false;
            result                       = true;
            stop                         = true;
        }
        else if ( cmd->cmd == LC_UNIXTHREAD ) {
            uint64_t startAddress;
            if ( entryAddrFromThreadCmd((thread_command*)cmd, startAddress) ) {
                offset  = startAddress - preferredLoadAddress();
                usesCRT = true;
                result  = true;
            }
            stop = true;
        }
    });
    return result;
}

bool Header::hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_CODE_SIGNATURE ) {
            linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            fileOffset                    = sigCmd->dataoff;
            size                          = sigCmd->datasize;
            result                        = true;
            stop                          = true;
        }
    });
    // FIXME: may need to ignore codesigs from pre 10.9 macOS binaries
    return result;
}

bool Header::hasIndirectSymbolTable(uint32_t& fileOffset, uint32_t& count) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_DYSYMTAB) {
            dysymtab_command* dySymCmd = (dysymtab_command*)cmd;
            fileOffset  = dySymCmd->indirectsymoff;
            count       = dySymCmd->nindirectsyms;
            result      = true;
            stop        = true;
        }
    });
    return result;
}

bool Header::hasSplitSegInfo() const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO) {
            result      = true;
            stop        = true;
        }
    });
    return result;
}

bool Header::hasAtomInfo(uint32_t& fileOffset, uint32_t& size) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ATOM_INFO) {
            linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            fileOffset                    = sigCmd->dataoff;
            size                          = sigCmd->datasize;
            result      = true;
            stop        = true;
        }
    });
    return result;
}


uint32_t Header::segmentCount() const
{
    __block uint32_t count = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_SEGMENT:
            case LC_SEGMENT_64:
                ++count;
                break;
        }
    });
    return count;
}

uint64_t Header::preferredLoadAddress() const
{
    __block uint64_t textVmAddr = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) {
                textVmAddr = segCmd->vmaddr;
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) {
                textVmAddr = segCmd->vmaddr;
                stop = true;
            }
        }
    });
    return textVmAddr;
}

int64_t Header::getSlide() const
{
    return (long)this - (long)(this->preferredLoadAddress());
}

bool Header::hasDataConst() const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( (segCmd->flags & SG_READ_ONLY) != 0 )
                result = true;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            if ( (segCmd->flags & SG_READ_ONLY) != 0 )
                result = true;
        }
    });
    return result;
}

std::string_view Header::segmentName(uint32_t segIndex) const
{
    __block std::string_view result;
    __block uint32_t    segCount = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( segIndex == segCount ) {
            result = info.segmentName;
            stop   = true;
        }
        ++segCount;
    });
    return result;
}

// LC_SEGMENT stores names as char[16] potentially without a null terminator.  This returns a string_view for the given name
static std::string_view name16(const char name[16])
{
    size_t length = strnlen(name, 16);
    return std::string_view(name, length);
}

void Header::forEachSegment(void (^callback)(const SegmentInfo& infos, bool& stop)) const
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            SegmentInfo segInfo { .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=(uint32_t)segCmd->fileoff, .fileSize=(uint32_t)segCmd->filesize, .flags=segCmd->flags, .perms=(uint8_t)segCmd->initprot };
            callback(segInfo, stop);
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            SegmentInfo segInfo { .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=segCmd->fileoff, .fileSize=segCmd->filesize, .flags=segCmd->flags, .perms=(uint8_t)segCmd->initprot };
            callback(segInfo, stop);
        }
    });
}

void Header::forEachSection(void (^callback)(const SectionInfo&, bool& stop)) const
{
    __block uint64_t prefLoadAddr = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd        = (segment_command_64*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 )
                prefLoadAddr = segCmd->vmaddr;
            const section_64* const   sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const   sectionsEnd   = &sectionsStart[segCmd->nsects];

            for ( const section_64* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                std::string_view sectName = name16(sect->sectname);
                std::string_view segName = name16(sect->segname);
                SectionInfo info = { segName, sectName, (uint32_t)segCmd->initprot, sect->flags, sect->align, sect->addr - prefLoadAddr, sect->size, sect->offset,
                                     sect->reloff, sect->nreloc, sect->reserved1, sect->reserved2};
                callback(info, stop);
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd        = (segment_command*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 )
                prefLoadAddr = segCmd->vmaddr;
            const section* const   sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const   sectionsEnd   = &sectionsStart[segCmd->nsects];
            for ( const section* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                std::string_view sectName = name16(sect->sectname);
                std::string_view segName = name16(sect->segname);
                SectionInfo info = { segName, sectName, (uint32_t)segCmd->initprot, sect->flags, sect->align, sect->addr - prefLoadAddr, sect->size, sect->offset,
                                     sect->reloff, sect->nreloc, sect->reserved1, sect->reserved2};
                callback(info, stop);
            }
        }
    });
}

// add any LINKEDIT content file-offset in load commands to this to get content
const uint8_t* Header::computeLinkEditBias(bool zeroFillExpanded) const
{
    // When there is no zerofill expansion, just add fileoffset of LINKEDIT content to mach_header to get content
    // If there is zerofill expansion, then zerofillExpansionAmount() needs to be added in too
    if ( zeroFillExpanded )
        return (uint8_t*)this + zerofillExpansionAmount();
    else
        return (uint8_t*)this;
}

// When loaded by dyld, LINKEDIT is farther from mach_header than in file
bool Header::hasZerofillExpansion() const
{
    return (zerofillExpansionAmount() != 0);
}

uint64_t Header::zerofillExpansionAmount() const
{
    // need to find LINKEDIT and TEXT to compute difference of file offsets vs vm offsets
    __block uint64_t result         = 0;
    __block uint64_t textVmAddr     = 0;
    __block uint64_t textFileOffset = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) {
                textVmAddr     = segCmd->vmaddr;
                textFileOffset = segCmd->fileoff;
            }
            else if ( strcmp(segCmd->segname, "__LINKEDIT") == 0 ) {
                uint64_t vmOffsetToLinkedit   = segCmd->vmaddr - textVmAddr;
                uint64_t fileOffsetToLinkedit = segCmd->fileoff;
                result                        = vmOffsetToLinkedit - fileOffsetToLinkedit;
                stop                          = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) {
                textVmAddr     = segCmd->vmaddr;
                textFileOffset = segCmd->fileoff;
            }
            else if ( strcmp(segCmd->segname, "__LINKEDIT") == 0 ) {
                uint64_t vmOffsetToLinkedit   = segCmd->vmaddr - textVmAddr;
                uint64_t fileOffsetToLinkedit = segCmd->fileoff - textFileOffset;
                result                        = vmOffsetToLinkedit - fileOffsetToLinkedit;
                stop                          = true;
            }
        }
    });
    return result;
}

bool Header::hasCustomStackSize(uint64_t& size) const {
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_MAIN ) {
            const entry_point_command* entryPointCmd = (entry_point_command*)cmd;
            size    = entryPointCmd->stacksize;
            result  = true;
            stop    = true;
        }
    });
    return result;
}

bool Header::isRestricted() const
{
    __block bool result = false;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( (info.segmentName == "__RESTRICT") && (info.sectionName == "__restrict") ) {
            result = true;
            stop   = true;
        }
    });
    return result;
}

bool Header::hasInterposingTuples() const
{
    __block bool hasInterposing = false;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( ((info.flags & SECTION_TYPE) == S_INTERPOSING) || ((info.sectionName == "__interpose") && (info.segmentName.starts_with("__DATA") || info.segmentName.starts_with("__AUTH"))) ) {
            hasInterposing = true;
            stop           = true;
        }
    });
    return hasInterposing;
}

bool Header::hasObjC() const
{
    __block bool hasObjCInfo = false;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__objc_imageinfo") && info.segmentName.starts_with("__DATA") ) {
            hasObjCInfo = true;
            stop        = true;
        }
    });
    return hasObjCInfo;
}

bool Header::hasEncryptionInfo(uint32_t& cryptId, uint32_t& textOffset, uint32_t& size) const
{
    if ( const encryption_info_command* encCmd = findFairPlayEncryptionLoadCommand() ) {
        cryptId = encCmd->cryptid;
        textOffset = encCmd->cryptoff;
        size       = encCmd->cryptsize;
        return true;
    }
    textOffset = 0;
    size       = 0;
    return false;
}

bool Header::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const
{
    // Note: cryptid is 0 in just-built apps.  The AppStore sets cryptid to 1
    uint32_t cryptId = 0;
    return hasEncryptionInfo(cryptId, textOffset, size) && cryptId == 1;
}

bool Header::canBeFairPlayEncrypted() const
{
    return (findFairPlayEncryptionLoadCommand() != nullptr);
}

const encryption_info_command* Header::findFairPlayEncryptionLoadCommand() const
{
    __block const encryption_info_command* result = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_ENCRYPTION_INFO) || (cmd->cmd == LC_ENCRYPTION_INFO_64) ) {
            result = (encryption_info_command*)cmd;
            stop   = true;
        }
    });
    return result;
}

bool Header::hasChainedFixups() const
{
    // arm64e always uses chained fixups
    if ( Architecture(&mh) == Architecture::arm64e ) {
        // Not all binaries have fixups at all so check for the load commands
        return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
    }
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool Header::hasChainedFixupsLoadCommand() const
{
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool Header::hasOpcodeFixups() const
{
    return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_INFO);
}

void Header::forEachRPath(void (^callback)(const char* rPath, bool& stop)) const
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_RPATH ) {
            const char* rpath = (char*)cmd + ((struct rpath_command*)cmd)->path.offset;
            callback(rpath, stop);
        }
    });
}

void Header::forEachLinkerOption(void (^callback)(const char* opt, bool& stop)) const
{
    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( cmd->cmd == LC_LINKER_OPTION ) {
            const char*     begin = (char*)cmd + sizeof(linker_option_command);
            const uint32_t  count = ((linker_option_command*)cmd)->count;
            for ( uint32_t i = 0; i < count; ++i ) {
                const char* next = begin + strlen(begin) + 1;
                callback(begin, stop);
                begin = next;
            }
        }
    });
}

void Header::forAllowableClient(void (^callback)(const char* clientName, bool& stop)) const
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SUB_CLIENT ) {
            const char* clientName = (char*)cmd + ((struct sub_client_command*)cmd)->client.offset;
            callback(clientName, stop);
        }
    });
}

const char* Header::umbrellaName() const
{
    __block const char* result = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SUB_FRAMEWORK ) {
            result = (char*)cmd + ((struct sub_framework_command*)cmd)->umbrella.offset;
        }
    });
    return result;
}


uint32_t Header::headerAndLoadCommandsSize() const
{
    return machHeaderSize() + mh.sizeofcmds;
}

uint32_t Header::fileSize() const
{
    if ( isObjectFile() ) {
        // .o files do not have LINKEDIT segment, so use end of symbol table as file size
        __block uint32_t size = 0;
        forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
            if ( cmd->cmd == LC_SYMTAB ) {
                const symtab_command* symTab = (symtab_command*)cmd;
                size = symTab->stroff + symTab->strsize;
                stop = true;
            }
        });
        return size;
    }

    // compute file size from LINKEDIT fileoffset + filesize
    __block uint32_t lastSegmentOffset = 0;
    __block uint32_t lastSegmentSize = 0;
    forEachSegment(^(const SegmentInfo &infos, bool &stop) {
        if ( infos.fileOffset >= lastSegmentOffset ) {
            lastSegmentOffset = infos.fileOffset;
            lastSegmentSize = std::max(infos.fileSize, lastSegmentSize);
        }
    });
    if ( lastSegmentSize == 0 )
        return headerAndLoadCommandsSize();

    uint32_t size;
    if ( __builtin_add_overflow(lastSegmentOffset, lastSegmentSize, &size)
        || size < headerAndLoadCommandsSize() )
        assert("malformed mach-o, size smaller than header and load commands");

    return size;
}

//
// MARK: --- methods that create and modify ---
//

#if BUILDING_MACHO_WRITER

Header* Header::make(std::span<uint8_t> buffer, uint32_t filetype, uint32_t flags, Architecture arch, bool addImplicitTextSegment)
{
    const size_t minHeaderAlignment = filetype == MH_OBJECT ? 8 : getpagesize();
    assert(((uint64_t)buffer.data() & (minHeaderAlignment - 1)) == 0);
    assert(buffer.size() >= sizeof(mach_header_64));
    bzero(buffer.data(), buffer.size());
    Header& header = *(Header*)buffer.data();
    mach_header& mh = header.mh;
    if ( arch.isBigEndian() ) {
        mh.magic      = arch.is64() ? MH_CIGAM_64 : MH_CIGAM;
        mh.filetype   = OSSwapBigToHostInt32(filetype);
        mh.ncmds      = 0;
        mh.sizeofcmds = OSSwapBigToHostInt32(MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL);
        mh.flags      = OSSwapBigToHostInt32(flags);
        arch.set(mh);
        return &header; // can only construct mach_header for big-endian
    }
    else {
        mh.magic      = arch.is64() ? MH_MAGIC_64 : MH_MAGIC;
        mh.filetype   = filetype;
        mh.ncmds      = 0;
        mh.sizeofcmds = 0;
        mh.flags      = flags;
        arch.set(mh);
    }
    if ( addImplicitTextSegment && (filetype != MH_OBJECT) ) {
        SegmentInfo segInfo { .segmentName="__TEXT", .vmaddr=0, .vmsize=0x1000, .fileOffset=0, .fileSize=0x1000, .perms=(VM_PROT_READ | VM_PROT_EXECUTE) };
        header.addSegment(segInfo, std::array { "__text" });
    }

    return &header;
}

void Header::save(char savedPath[PATH_MAX]) const
{
    ::strcpy(savedPath, "/tmp/mocko-XXXXXX");
    int fd = ::mkstemp(savedPath);
    if ( fd != -1 ) {
        ::pwrite(fd, this, sizeof(Header), 0);
        ::close(fd);
    }
}

uint32_t Header::pointerAligned(uint32_t value) const
{
    // mach-o requires all load command sizes to be a multiple the pointer size
    if ( is64() )
        return ((value + 7) & (-8));
    else
        return ((value + 3) & (-4));
}

load_command* Header::firstLoadCommand()
{
    if ( mh.magic == MH_MAGIC )
        return (load_command*)((uint8_t*)this + sizeof(mach_header));
    else
        return (load_command*)((uint8_t*)this + sizeof(mach_header_64));
}

// creates space for a new load command, but does not fill in its payload
load_command* Header::appendLoadCommand(uint32_t cmd, uint32_t cmdSize)
{
    load_command* thisCmd = (load_command*)((uint8_t*)firstLoadCommand() + mh.sizeofcmds);
    thisCmd->cmd          = cmd;
    thisCmd->cmdsize      = cmdSize;
    mh.ncmds += 1;
    mh.sizeofcmds += cmdSize;

    return thisCmd;
}

// copies a new load command from another
void Header::appendLoadCommand(const load_command* lc)
{
    load_command* thisCmd = (load_command*)((uint8_t*)firstLoadCommand() + mh.sizeofcmds);
    ::memcpy(thisCmd, lc, lc->cmdsize);
    mh.ncmds += 1;
    mh.sizeofcmds += lc->cmdsize;
}

void Header::addBuildVersion(Platform platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools)
{
    assert(platform != Platform::zippered && "can't add a build command for Platform::zippered, it must be split");
    uint32_t               lcSize = (uint32_t)(sizeof(build_version_command) + tools.size() * sizeof(build_tool_version));
    build_version_command* bv     = (build_version_command*)appendLoadCommand(LC_BUILD_VERSION, lcSize);
    bv->platform = platform.value();
    bv->minos    = minOS.value();
    bv->sdk      = sdk.value();
    bv->ntools   = (uint32_t)tools.size();
    if ( bv->ntools != 0 )
        memcpy((uint8_t*)bv + sizeof(build_version_command), &tools[0], tools.size() * sizeof(build_tool_version));
}

void Header::addMinVersion(Platform platform, Version32 minOS, Version32 sdk)
{
    version_min_command vc;
    vc.cmdsize = sizeof(version_min_command);
    vc.version = minOS.value();
    vc.sdk     = sdk.value();
    if ( platform == Platform::macOS )
        vc.cmd = LC_VERSION_MIN_MACOSX;
    else if ( (platform == Platform::iOS) || (platform == Platform::iOS_simulator) )
        vc.cmd = LC_VERSION_MIN_IPHONEOS;
    else if ( (platform == Platform::watchOS) || (platform == Platform::watchOS_simulator) )
        vc.cmd = LC_VERSION_MIN_WATCHOS;
    else if ( (platform == Platform::tvOS) || (platform == Platform::tvOS_simulator) )
        vc.cmd = LC_VERSION_MIN_TVOS;
    else
        assert(0 && "unknown platform");
    appendLoadCommand((load_command*)&vc);
}

void Header::setHasThreadLocalVariables()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_HAS_TLV_DESCRIPTORS;
}

void Header::setHasWeakDefs()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_WEAK_DEFINES;
}

void Header::setUsesWeakDefs()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_BINDS_TO_WEAK;
}

void Header::setAppExtensionSafe()
{
    assert(mh.filetype == MH_DYLIB);
    mh.flags |= MH_APP_EXTENSION_SAFE;
}

void Header::setSimSupport()
{
    assert(mh.filetype == MH_DYLIB);
    mh.flags |= MH_SIM_SUPPORT;
}

void Header::setNoReExportedDylibs()
{
    assert(mh.filetype == MH_DYLIB);
    mh.flags |= MH_NO_REEXPORTED_DYLIBS;
}

void Header::addPlatformInfo(Platform platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools)
{
    Architecture arch(&mh);
    Policy policy(arch, { platform, minOS, sdk }, mh.filetype);
    switch ( policy.useBuildVersionLoadCommand() ) {
        case Policy::preferUse:
        case Policy::mustUse:
            // three macOS dylibs under libSystem need to be built with old load commands to support old simulator runtimes
            if ( isSimSupport() && (platform == Platform::macOS) && ((arch == Architecture::x86_64) || (arch == Architecture::i386)) )
                addMinVersion(platform, minOS, sdk);
            else
                addBuildVersion(platform, minOS, sdk, tools);
            break;
        case Policy::preferDontUse:
        case Policy::mustNotUse:
            addMinVersion(platform, minOS, sdk);
            break;
    }
}

void Header::addNullUUID()
{
    uuid_command uc;
    uc.cmd     = LC_UUID;
    uc.cmdsize = sizeof(uuid_command);
    bzero(uc.uuid, 16);
    appendLoadCommand((load_command*)&uc);
}

void Header::addUniqueUUID(uuid_t copyOfUUID)
{
    uuid_command uc;
    uc.cmd     = LC_UUID;
    uc.cmdsize = sizeof(uuid_command);
    uuid_generate_random(uc.uuid);
    appendLoadCommand((load_command*)&uc);
    if ( copyOfUUID )
        memcpy(copyOfUUID, uc.uuid, sizeof(uuid_t));
}

void Header::updateUUID(uuid_t uuid)
{
    __block bool found = false;
    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( cmd->cmd == LC_UUID ) {
            memcpy(((uuid_command*)cmd)->uuid, uuid, 16);
            found = true;
            stop = true;
        }
    });
    assert(found && "updateUUID called without a LC_UUID command");
}

void Header::addSegment(const SegmentInfo& info, std::span<const char* const> sectionNames)
{
    if ( is64() ) {
        uint32_t            lcSize = (uint32_t)(sizeof(segment_command_64) + sectionNames.size() * sizeof(section_64));
        segment_command_64* sc     = (segment_command_64*)appendLoadCommand(LC_SEGMENT_64, lcSize);
        strncpy(sc->segname, info.segmentName.data(), 16);
        sc->vmaddr             = info.vmaddr;
        sc->vmsize             = info.vmsize;
        sc->fileoff            = info.fileOffset;
        sc->filesize           = info.fileSize;
        sc->initprot           = info.perms;
        sc->maxprot            = info.perms;
        sc->nsects             = (uint32_t)sectionNames.size();
        sc->flags              = info.flags;
        section_64* const sect = (section_64*)((uint8_t*)sc + sizeof(struct segment_command_64));
        uint32_t sectionIndex  = 0;
        for ( const char* sectName : sectionNames ) {
            strncpy(sect[sectionIndex].segname, info.segmentName.data(), 16);
            strncpy(sect[sectionIndex].sectname, sectName, 16);
            ++sectionIndex;
        }
    }
    else {
        uint32_t         lcSize = (uint32_t)(sizeof(segment_command) + sectionNames.size() * sizeof(section));
        segment_command* sc     = (segment_command*)appendLoadCommand(LC_SEGMENT, lcSize);
        strncpy(sc->segname, info.segmentName.data(), 16);
        sc->vmaddr             = (uint32_t)info.vmaddr;
        sc->vmsize             = (uint32_t)info.vmsize;
        sc->fileoff            = info.fileOffset;
        sc->filesize           = info.fileSize;
        sc->initprot           = info.perms;
        sc->maxprot            = info.perms;
        sc->nsects             = (uint32_t)sectionNames.size();
        sc->flags              = info.flags;
        section* const sect = (section*)((uint8_t*)sc + sizeof(struct segment_command));
        uint32_t sectionIndex  = 0;
        for ( const char* sectName : sectionNames ) {
            strncpy(sect[sectionIndex].segname, info.segmentName.data(), 16);
            strncpy(sect[sectionIndex].sectname, sectName, 16);
            ++sectionIndex;
        }
    }
}

void Header::updateSection(const SectionInfo& info)
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            segment_command_64* segCmd = (segment_command_64*)cmd;
            if (info.segmentName == segCmd->segname) {
                section_64* const  sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
                section_64* const  sectionsEnd   = &sectionsStart[segCmd->nsects];
                for ( section_64* sect=sectionsStart; sect < sectionsEnd; ++sect ) {
                    if ( strncmp(info.sectionName.data(), sect->sectname, 16) == 0 ) {
                        sect->addr      = info.address;
                        sect->size      = info.size;
                        sect->offset    = info.fileOffset;
                        sect->align     = info.alignment;
                        sect->reloff    = info.relocsOffset;
                        sect->nreloc    = info.relocsCount;
                        sect->flags     = info.flags;
                        sect->reserved1 = info.reserved1;
                        sect->reserved2 = info.reserved2;
                        sect->reserved3 = 0;
                        stop = true;
                        return;
                    }
                }
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            segment_command* segCmd = (segment_command*)cmd;
            if (info.segmentName == segCmd->segname) {
                section* const  sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
                section* const  sectionsEnd   = &sectionsStart[segCmd->nsects];
                for ( section* sect=sectionsStart; sect < sectionsEnd; ++sect ) {
                    if ( strncmp(info.sectionName.data(), sect->sectname, 16) == 0 ) {
                        sect->addr      = (uint32_t)info.address;
                        sect->size      = (uint32_t)info.size;
                        sect->offset    = info.fileOffset;
                        sect->align     = info.alignment;
                        sect->reloff    = info.relocsOffset;
                        sect->nreloc    = info.relocsCount;
                        sect->flags     = info.flags;
                        sect->reserved1 = info.reserved1;
                        sect->reserved2 = info.reserved2;
                        stop = true;
                        return;
                    }
                }
            }
        }
    });
}

void Header::updateSegment(const SegmentInfo& info)
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            segment_command_64* segCmd = (segment_command_64*)cmd;
            if (info.segmentName == segCmd->segname) {
                segCmd->vmaddr             = info.vmaddr;
                segCmd->vmsize             = info.vmsize;
                segCmd->fileoff            = info.fileOffset;
                segCmd->filesize           = info.fileSize;
                segCmd->initprot           = info.perms;
                segCmd->maxprot            = info.perms;
                stop = true;
                return;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            segment_command* segCmd = (segment_command*)cmd;
            if (info.segmentName == segCmd->segname) {
                segCmd->vmaddr             = (uint32_t)info.vmaddr;
                segCmd->vmsize             = (uint32_t)info.vmsize;
                segCmd->fileoff            = info.fileOffset;
                segCmd->filesize           = info.fileSize;
                segCmd->initprot           = info.perms;
                segCmd->maxprot            = info.perms;
                stop = true;
                return;
            }
        }
    });
}


void Header::addInstallName(const char* name, Version32 compatVers, Version32 currentVersion)
{
    uint32_t       alignedSize      = pointerAligned((uint32_t)(sizeof(dylib_command) + strlen(name) + 1));
    dylib_command* ic               = (dylib_command*)appendLoadCommand(LC_ID_DYLIB, alignedSize);
    ic->dylib.name.offset           = sizeof(dylib_command);
    ic->dylib.current_version       = currentVersion.value();
    ic->dylib.compatibility_version = compatVers.value();
    strcpy((char*)ic + ic->dylib.name.offset, name);
}

void Header::addDependentDylib(const char* path, DependentDylibAttributes depAttrs, Version32 compatVers, Version32 currentVersion)
{
    uint32_t cmd = 0;
    if (      depAttrs == DependentDylibAttributes::regular )
        cmd = LC_LOAD_DYLIB;
    else if ( depAttrs == DependentDylibAttributes::justWeakLink )
        cmd = LC_LOAD_WEAK_DYLIB;
    else if ( depAttrs == DependentDylibAttributes::justUpward )
        cmd = LC_LOAD_UPWARD_DYLIB;
    else if ( depAttrs == DependentDylibAttributes::justReExport )
        cmd = LC_REEXPORT_DYLIB;

    if ( cmd ) {
        // make traditional load command
        uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(dylib_command) + strlen(path) + 1));
        dylib_command* dc          = (dylib_command*)appendLoadCommand(cmd, alignedSize);
        dc->dylib.name.offset           = sizeof(dylib_command);
        dc->dylib.current_version       = currentVersion.value();
        dc->dylib.compatibility_version = compatVers.value();
        dc->dylib.timestamp             = 2; // needs to be some constant value that is different than dylib id load command;
        strcpy((char*)dc + dc->dylib.name.offset, path);
    }
    else {
        // make new style load command with extra flags field
        if ( depAttrs.weakLink )
            cmd = LC_LOAD_WEAK_DYLIB;
        else
            cmd = LC_LOAD_DYLIB;
        uint32_t        alignedSize = pointerAligned((uint32_t)(sizeof(dylib_use_command) + strlen(path) + 1));
        dylib_use_command* dc          = (dylib_use_command*)appendLoadCommand(cmd, alignedSize);
        dc->nameoff               = sizeof(dylib_use_command);
        dc->current_version       = currentVersion.value();
        dc->compat_version        = 0x00010000; // unused, but looks like 1.0 to old tools
        dc->marker                = 0x1a741800; // magic value that means dylib_use_command
        dc->flags                 = depAttrs.raw;
        strcpy((char*)dc + dc->nameoff, path);
    }
}

void Header::addLibSystem()
{
    addDependentDylib("/usr/lib/libSystem.B.dylib");
}

void Header::addDylibId(CString name, Version32 compatVers, Version32 currentVersion)
{
    uint32_t       alignedSize          = pointerAligned((uint32_t)(sizeof(dylib_command) + name.size() + 1));
    dylib_command* dc                   = (dylib_command*)appendLoadCommand(LC_ID_DYLIB, alignedSize);
    dc->dylib.name.offset               = sizeof(dylib_command);
    dc->dylib.timestamp                 = 1; // needs to be some constant value that is different than dependent dylib
    dc->dylib.current_version           = currentVersion.value();
    dc->dylib.compatibility_version     = compatVers.value();
    strcpy((char*)dc + dc->dylib.name.offset, name.c_str());
}

void Header::addDyldID()
{
    const char* path = "/usr/lib/dyld";
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc       = (dylinker_command*)appendLoadCommand(LC_ID_DYLINKER, alignedSize);
    dc->name.offset = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void Header::addDynamicLinker()
{
    const char* path = "/usr/lib/dyld";
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc       = (dylinker_command*)appendLoadCommand(LC_LOAD_DYLINKER, alignedSize);
    dc->name.offset = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void Header::addFairPlayEncrypted(uint32_t offset, uint32_t size)
{
    if ( is64() ) {
        encryption_info_command_64 en64;
        en64.cmd       = LC_ENCRYPTION_INFO_64;
        en64.cmdsize   = sizeof(encryption_info_command_64);
        en64.cryptoff  = offset;
        en64.cryptsize = size;
        en64.cryptid   = 0;
        en64.pad       = 0;
        appendLoadCommand((load_command*)&en64);
    }
    else {
        encryption_info_command en32;
        en32.cmd       = LC_ENCRYPTION_INFO;
        en32.cmdsize   = sizeof(encryption_info_command);
        en32.cryptoff  = offset;
        en32.cryptsize = size;
        en32.cryptid   = 0;
        appendLoadCommand((load_command*)&en32);
    }
}

void Header::addRPath(const char* path)
{
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(rpath_command) + strlen(path) + 1));
    rpath_command* rc          = (rpath_command*)appendLoadCommand(LC_RPATH, alignedSize);
    rc->path.offset            = sizeof(rpath_command);
    strcpy((char*)rc + rc->path.offset, path);
}

void Header::addDyldEnvVar(const char* path)
{
    uint32_t          alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc          = (dylinker_command*)appendLoadCommand(LC_DYLD_ENVIRONMENT, alignedSize);
    dc->name.offset               = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void Header::addAllowableClient(const char* clientName)
{
    uint32_t            alignedSize = pointerAligned((uint32_t)(sizeof(sub_client_command) + strlen(clientName) + 1));
    sub_client_command* ac          = (sub_client_command*)appendLoadCommand(LC_SUB_CLIENT, alignedSize);
    ac->client.offset               = sizeof(sub_client_command);
    strcpy((char*)ac + ac->client.offset, clientName);
}

void Header::addUmbrellaName(const char* umbrellaName)
{
    uint32_t            alignedSize = pointerAligned((uint32_t)(sizeof(sub_framework_command) + strlen(umbrellaName) + 1));
    sub_framework_command* ac       = (sub_framework_command*)appendLoadCommand(LC_SUB_FRAMEWORK, alignedSize);
    ac->umbrella.offset             = sizeof(sub_framework_command);
    strcpy((char*)ac + ac->umbrella.offset, umbrellaName);
}

void Header::addSourceVersion(Version64 vers)
{
    source_version_command svc;
    svc.cmd       = LC_SOURCE_VERSION;
    svc.cmdsize   = sizeof(source_version_command);
    svc.version   = vers.value();
    appendLoadCommand((load_command*)&svc);
}

void Header::setMain(uint32_t offset)
{
    entry_point_command ec;
    ec.cmd       = LC_MAIN;
    ec.cmdsize   = sizeof(entry_point_command);
    ec.entryoff  = offset;
    ec.stacksize = 0;
    appendLoadCommand((load_command*)&ec);
}

void Header::setCustomStackSize(uint64_t stackSize) {
    __block bool found = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if (cmd->cmd == LC_MAIN) {
            entry_point_command* ec = (entry_point_command*)cmd;
            ec->stacksize           = stackSize;
            found                   = true;
            stop                    = true;
        }
    });
    assert(found);
}

void Header::setUnixEntry(uint64_t startAddr)
{
    // FIXME: support other archs
    if ( (mh.cputype == CPU_TYPE_ARM64) || (mh.cputype == CPU_TYPE_ARM64_32) ) {
        uint32_t   lcSize = 288;
        uint32_t*  words     = (uint32_t*)appendLoadCommand(LC_UNIXTHREAD, lcSize);
        words[2] = 6;   // flavor = ARM_THREAD_STATE64
        words[3] = 68;  // count  = ARM_EXCEPTION_STATE64_COUNT
        bzero(&words[4], lcSize-16);
        *(uint64_t*)(&words[68]) = startAddr;  // register pc = startAddr
    }
    else if ( mh.cputype == CPU_TYPE_X86_64 ) {
        uint32_t   lcSize = 184;
        uint32_t*  words     = (uint32_t*)appendLoadCommand(LC_UNIXTHREAD, lcSize);
        words[2] = 4;   // flavor = x86_THREAD_STATE64
        words[3] = 42;  // count  = x86_THREAD_STATE64_COUNT
        bzero(&words[4], lcSize-16);
        *(uint64_t*)(&words[36]) = startAddr;  // register pc = startAddr
    }
    else {
        assert(0 && "arch not supported");
    }
}

void Header::addCodeSignature(uint32_t fileOffset, uint32_t fileSize)
{
    linkedit_data_command lc;
    lc.cmd       = LC_CODE_SIGNATURE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = fileOffset;
    lc.datasize  = fileSize;
    appendLoadCommand((load_command*)&lc);
}

void Header::setBindOpcodesInfo(uint32_t rebaseOffset, uint32_t rebaseSize,
                                uint32_t bindsOffset, uint32_t bindsSize,
                                uint32_t weakBindsOffset, uint32_t weakBindsSize,
                                uint32_t lazyBindsOffset, uint32_t lazyBindsSize,
                                uint32_t exportTrieOffset, uint32_t exportTrieSize)
{
    dyld_info_command lc;
    lc.cmd              = LC_DYLD_INFO_ONLY;
    lc.cmdsize          = sizeof(dyld_info_command);
    lc.rebase_off       = rebaseOffset;
    lc.rebase_size      = rebaseSize;
    lc.bind_off         = bindsOffset;
    lc.bind_size        = bindsSize;
    lc.weak_bind_off    = weakBindsOffset;
    lc.weak_bind_size   = weakBindsSize;
    lc.lazy_bind_off    = lazyBindsOffset;
    lc.lazy_bind_size   = lazyBindsSize;
    lc.export_off       = exportTrieOffset;
    lc.export_size      = exportTrieSize;
    appendLoadCommand((load_command*)&lc);
}

void Header::setChainedFixupsInfo(uint32_t cfOffset, uint32_t cfSize)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DYLD_CHAINED_FIXUPS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = cfOffset;
    lc.datasize  = cfSize;
    appendLoadCommand((load_command*)&lc);
}

void Header::setExportTrieInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DYLD_EXPORTS_TRIE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void Header::setSplitSegInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_SEGMENT_SPLIT_INFO;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void Header::setDataInCode(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DATA_IN_CODE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void Header::setFunctionStarts(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_FUNCTION_STARTS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void Header::setAtomInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_ATOM_INFO;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void Header::setSymbolTable(uint32_t nlistOffset, uint32_t nlistCount, uint32_t stringPoolOffset, uint32_t stringPoolSize,
                            uint32_t localsCount, uint32_t globalsCount, uint32_t undefCount, uint32_t indOffset, uint32_t indCount)
{
    symtab_command stc;
    stc.cmd       = LC_SYMTAB;
    stc.cmdsize   = sizeof(symtab_command);
    stc.symoff    = nlistOffset;
    stc.nsyms     = nlistCount;
    stc.stroff    = stringPoolOffset;
    stc.strsize   = stringPoolSize;
    appendLoadCommand((load_command*)&stc);

    dysymtab_command dstc;
    bzero(&dstc, sizeof(dstc));
    dstc.cmd            = LC_DYSYMTAB;
    dstc.cmdsize        = sizeof(dysymtab_command);
    dstc.ilocalsym      = 0;
    dstc.nlocalsym      = localsCount;
    dstc.iextdefsym     = localsCount;
    dstc.nextdefsym     = globalsCount;
    dstc.iundefsym      = localsCount+globalsCount;
    dstc.nundefsym      = undefCount;
    dstc.indirectsymoff = indOffset;
    dstc.nindirectsyms  = indCount;
    appendLoadCommand((load_command*)&dstc);
}

void Header::addLinkerOption(std::span<uint8_t> buffer, uint32_t count)
{
    uint32_t cmdSize = pointerAligned(sizeof(linker_option_command) + (uint32_t)buffer.size());

    linker_option_command* lc = (linker_option_command*)appendLoadCommand(LC_LINKER_OPTION, cmdSize);
    lc->cmd     = LC_LINKER_OPTION;
    lc->cmdsize = cmdSize;
    lc->count   = count;
    memcpy((uint8_t*)(lc + 1), buffer.data(), buffer.size());
}

Header::LinkerOption Header::LinkerOption::make(std::span<CString> opts)
{
    LinkerOption out;
    out.count = (uint32_t)opts.size();
    assert(out.count == opts.size());
    for ( CString option : opts ) {
        if ( option.empty() )
            continue;
        size_t previousSize = out.buffer.size();
        out.buffer.resize(previousSize + option.size() + 1);
        option.strcpy((char*)out.buffer.data() + previousSize);
    }
    return out;
}

load_command* Header::findLoadCommand(uint32_t cmdNum)
{
    __block load_command* result = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == cmdNum ) {
            result = (load_command*)cmd;
            stop   = true;
        }
    });
    return result;
}

void Header::removeLoadCommand(void (^callback)(const load_command* cmd, bool& remove, bool& stop))
{
    bool                stop      = false;
    const load_command* startCmds = nullptr;
    if ( mh.magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char*)this + sizeof(mach_header_64));
    else if ( mh.magic == MH_MAGIC )
        startCmds = (load_command*)((char*)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return; // can't process big endian mach-o
    else {
        //const uint32_t* h = (uint32_t*)this;
        //diag.error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h [1]);
        return; // not a mach-o file
    }
    const load_command* const cmdsEnd        = (load_command*)((char*)startCmds + mh.sizeofcmds);
    auto                      cmd            = (load_command*)startCmds;
    const uint32_t            origNcmds      = mh.ncmds;
    unsigned                  bytesRemaining = mh.sizeofcmds;
    for ( uint32_t i = 0; i < origNcmds; ++i ) {
        bool remove  = false;
        auto nextCmd = (load_command*)((char*)cmd + cmd->cmdsize);
        if ( cmd->cmdsize < 8 ) {
            //diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) too small", i, mh.ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            //diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, mh.ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
            return;
        }
        callback(cmd, remove, stop);
        if ( remove ) {
            mh.sizeofcmds -= cmd->cmdsize;
            ::memmove((void*)cmd, (void*)nextCmd, bytesRemaining);
            mh.ncmds--;
        }
        else {
            bytesRemaining -= cmd->cmdsize;
            cmd = nextCmd;
        }
        if ( stop )
            break;
    }
    if ( cmd )
        ::bzero(cmd, bytesRemaining);
}

uint32_t Header::relocatableHeaderAndLoadCommandsSize(bool is64, uint32_t sectionCount, uint32_t platformsCount, std::span<const Header::LinkerOption> linkerOptions)
{
    uint32_t size =  0;
    if ( is64 ) {
        size += sizeof(mach_header_64);
        size += sizeof(segment_command_64);
        size += sizeof(section_64) * sectionCount;
    }
    else {
        size += sizeof(mach_header);
        size += sizeof(segment_command);
        size += sizeof(section) * sectionCount;
    }
    size += sizeof(symtab_command);
    size += sizeof(dysymtab_command);
    size += sizeof(build_version_command) * platformsCount;
    size += sizeof(linkedit_data_command);

    for ( Header::LinkerOption opt : linkerOptions ) {
        size += opt.lcSize();
    }
    return size;
}

void Header::setRelocatableSectionCount(uint32_t sectionCount)
{
    assert(mh.filetype == MH_OBJECT);
    if ( is64() ) {
        uint32_t            lcSize = (uint32_t)(sizeof(segment_command_64) + sectionCount * sizeof(section_64));
        segment_command_64* sc = (segment_command_64*)appendLoadCommand(LC_SEGMENT_64, lcSize);
        sc->segname[0]         = '\0';   // MH_OBJECT has one segment with no name
        sc->vmaddr             = 0;
        sc->vmsize             = 0;   // adjusted in updateRelocatableSegmentSize()
        sc->fileoff            = 0;
        sc->filesize           = 0;   // adjusted in updateRelocatableSegmentSize()
        sc->initprot           = 7;
        sc->maxprot            = 7;
        sc->nsects             = sectionCount;
        // section info to be filled in later by setRelocatableSectionInfo()
        bzero((uint8_t*)sc + sizeof(segment_command_64), sectionCount * sizeof(section_64));
    }
    else {
        uint32_t            lcSize = (uint32_t)(sizeof(segment_command) + sectionCount * sizeof(section));
        segment_command* sc = (segment_command*)appendLoadCommand(LC_SEGMENT, lcSize);
        sc->segname[0]         = '\0';   // MH_OBJECT has one segment with no name
        sc->vmaddr             = 0;
        sc->vmsize             = 0x1000; // FIXME: need dynamic segment layout
        sc->fileoff            = 0;
        sc->filesize           = 0x1000;
        sc->initprot           = 7;
        sc->maxprot            = 7;
        sc->nsects             = sectionCount;
        // section info to be filled in later by setRelocatableSectionInfo()
        bzero((uint8_t*)sc + sizeof(segment_command), sectionCount * sizeof(struct section));
    }
}

void Header::updateRelocatableSegmentSize(uint64_t vmSize, uint32_t fileSize)
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT ) {
            segment_command* sc = (segment_command*)cmd;
            sc->vmsize   = (uint32_t)vmSize;
            sc->filesize = fileSize;
            stop = true;
        }
        else if ( cmd->cmd == LC_SEGMENT_64 ) {
            segment_command_64* sc = (segment_command_64*)cmd;
            sc->vmsize   = vmSize;
            sc->filesize = fileSize;
            stop = true;
        }
    });
}


void Header::setRelocatableSectionInfo(uint32_t sectionIndex, const char* segName, const char* sectName,
                                       uint32_t flags, uint64_t address, uint64_t size, uint32_t fileOffset,
                                       uint16_t alignment, uint32_t relocsOffset, uint32_t relocsCount)
{
    __block struct section*    section32 = nullptr;
    __block struct section_64* section64 = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT ) {
            struct section* sections = (struct section*)((uint8_t*)cmd + sizeof(segment_command));
            section32 = &sections[sectionIndex];
            stop = true;
        }
        else if ( cmd->cmd == LC_SEGMENT_64 ) {
            struct section_64* sections = (struct section_64*)((uint8_t*)cmd + sizeof(segment_command_64));
            section64 = &sections[sectionIndex];
            stop = true;
        }
    });
    if ( section64 != nullptr ) {
        strncpy(section64->segname,  segName, 16);
        strncpy(section64->sectname, sectName, 16);
        section64->addr      = address;
        section64->size      = size;
        section64->offset    = fileOffset;
        section64->align     = alignment;
        section64->reloff    = relocsOffset;
        section64->nreloc    = relocsCount;
        section64->flags     = flags;
        section64->reserved1 = 0;
        section64->reserved2 = 0;
        section64->reserved3 = 0;
    }
    else if ( section32 != nullptr ) {
        strncpy(section32->segname,  segName, 16);
        strncpy(section32->sectname, sectName, 16);
        section32->addr      = (uint32_t)address;
        section32->size      = (uint32_t)size;
        section32->offset    = fileOffset;
        section32->align     = alignment;
        section32->reloff    = relocsOffset;
        section32->nreloc    = relocsCount;
        section32->flags     = flags;
        section32->reserved1 = 0;
        section32->reserved2 = 0;
    }
}

#endif // BUILDING_MACHO_WRITER


} // namespace dyld3
