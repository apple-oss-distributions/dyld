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

#include <mach-o/fixup-chains.h>

#include "Array.h"
#include "Header.h"
#include "Architecture.h"
#include "Misc.h"
#include "Policy.h"
#include "LoggingStub.h"
#include "TargetPolicy.h"
#include "Version32.h"

#if TARGET_OS_EXCLAVEKIT
#ifndef VM_PROT_READ
    #define VM_PROT_READ    1
#endif

#ifndef VM_PROT_WRITE
    #define VM_PROT_WRITE   2
#endif

#ifndef VM_PROT_EXECUTE
    #define VM_PROT_EXECUTE 4
#endif
#endif // TARGET_OS_EXCLAVEKIT

using mach_o::Architecture;
using mach_o::Platform;
using mach_o::PlatformAndVersions;
using mach_o::Policy;
using mach_o::Version32;

namespace mach_o {



//
// MARK: --- LinkedDylibAttributes ---
//

constinit const LinkedDylibAttributes LinkedDylibAttributes::regular;
constinit const LinkedDylibAttributes LinkedDylibAttributes::justWeakLink(DYLIB_USE_WEAK_LINK);
constinit const LinkedDylibAttributes LinkedDylibAttributes::justUpward(DYLIB_USE_UPWARD);
constinit const LinkedDylibAttributes LinkedDylibAttributes::justReExport(DYLIB_USE_REEXPORT);
constinit const LinkedDylibAttributes LinkedDylibAttributes::justDelayInit(DYLIB_USE_DELAYED_INIT);

void LinkedDylibAttributes::toString(char buf[64]) const
{
    buf[0] = '\0';
    if ( weakLink )
        strlcpy(buf, "weak-link", 64);
    if ( upward ) {
        if ( buf[0] != '\0' )
            strlcat(buf, ",", 64);
        strlcat(buf, "upward", 64);
    }
    if ( delayInit ) {
        if ( buf[0] != '\0' )
            strlcat(buf, ",", 64);
        strlcat(buf, "delay-init", 64);
    }
    if ( reExport ) {
        if ( buf[0] != '\0' )
            strlcat(buf, ",", 64);
        strlcat(buf, "re-export", 64);
    }
}


//
// MARK: --- methods that read mach_header ---
//

bool Header::isSharedCacheEligiblePath(const char* dylibName) {
    return (   (strncmp(dylibName, "/usr/lib/", 9) == 0)
            || (strncmp(dylibName, "/System/Library/", 16) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/usr/lib/", 27) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/System/Library/", 34) == 0)
            || (strncmp(dylibName, "/Library/Apple/usr/lib/", 23) == 0)
            || (strncmp(dylibName, "/Library/Apple/System/Library/", 30) == 0)
            || (strncmp(dylibName, "/System/DriverKit/", 18) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/usr/lib/", 29) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/Library/", 36) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/iOSSupport/usr/lib/", 47) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/iOSSupport/System/Library/", 54) == 0)
            || (strncmp(dylibName, "/System/ExclaveKit/usr/lib/", 27) == 0)
            || (strncmp(dylibName, "/System/ExclaveKit/System/Library/", 34) == 0));
}

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

uint32_t Header::ncmds() const
{
    return mh.ncmds;
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
        case MH_DYLIB:
        case MH_BUNDLE:
            return true;
        case MH_EXECUTE:
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

bool Header::isDylibOrStub() const
{
    return (mh.filetype == MH_DYLIB) || (mh.filetype == MH_DYLIB_STUB);
}

bool Header::isDylibStub() const
{
    return (mh.filetype == MH_DYLIB_STUB);
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

bool Header::usesTwoLevelNamespace() const
{
    return (mh.flags & MH_TWOLEVEL);
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
    // Kernel Collections (MH_FILESET) don't have a platform. Skip them
    if ( isFileSet() )
        return Error::none();

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

    if ( pvs.platform.empty() && gHeaderAllowEmptyPlatform )
        return Error::none(); // allow empty platform in static linker

    // preloads usually don't have a platform
    if ( isPreload() )
        return Error::none();

    // static executables also may not have one
    if ( isStaticExecutable() )
        return Error::none();

    return pvs.platform.valid();
}

Error Header::valid(uint64_t fileSize) const
{
    if ( fileSize < sizeof(mach_header) )
        return Error("file is too small (length=%llu)", fileSize);

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

    if ( Error err = validSemanticsLinkedDylibs(policy) )
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
        case MH_DYLIB_STUB:
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
            case LC_FUNCTION_VARIANTS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_FUNCTION_VARIANTS size wrong", index);
                break;
            case LC_FUNCTION_VARIANT_FIXUPS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    lcError = Error("load command #%d LC_FUNCTION_VARIANT_FIXUPS size wrong", index);
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

    if ( this->isDylibOrStub() ) {
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

Error Header::validSemanticsLinkedDylibs(const Policy& policy) const
{
    // gather info
    __block Error dupDepError;
    __block int   depCount = 0;
    const char*   depPathsBuffer[256];
    const char**  depPaths = depPathsBuffer;
    const bool    enforceNoDupDylibs = policy.enforceNoDuplicateDylibs();
    const bool    hasWarningHandler = mach_o::hasWarningHandler();
    // don't use forEachLinkedDylib, because it synthesizes libSystem.dylib
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
                                dupDepError = Error("duplicate linked dylib '%s'", loadPath);
                                stop        = true;
                            } else
                                warning(this, "duplicate linked dylib are deprecated ('%s')", loadPath);
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
        CString installName = this->installName();
        CString libSystemDir = platformAndVersions().platform.libSystemDir();
        if ( !installName.starts_with(libSystemDir) )
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
    // <rdar://problem/144216621> dyld should support non-allocatable __DWARF segment (golang)
    if ( !isObjectFile() && (seg->filesize > seg->vmsize) ) {
        // non-mapped segments must have vmsize==0 and initprot==0
        if ( (seg->vmsize == 0) && (seg->initprot == 0) )
            return Error::none(); // no more checks of segment because it is not mapped
        else
            return Error("segment '%s' filesize exceeds vmsize", seg->segname);
    }

    // check permission bits
    if ( (seg->initprot & 0xFFFFFFF8) != 0 ) {
        return Error("%s segment permissions has invalid bits set (0x%08X)", seg->segname, seg->initprot);
    }
    if ( this->isDyldManaged() && policy.enforceTextSegmentPermissions() ) {
        if ( (strcmp(seg->segname, "__TEXT") == 0) && (seg->initprot != (VM_PROT_READ | VM_PROT_EXECUTE)) )
            return Error("__TEXT segment permissions is not 'r-x'");
    }
    if ( policy.enforceReadOnlyLinkedit() ) {
        if ( (strcmp(seg->segname, "__LINKEDIT") == 0) && (seg->initprot != VM_PROT_READ) )
            return Error("__LINKEDIT segment permissions is not 'r--'");
    }
    // dylib stubs don't have LC_SEGMENT_SPLIT_INFO, so they trip on SG_READ_ONLY check because
    // the split seg info exception can't be applied
    if ( !isDylibStub() && policy.enforceDataSegmentPermissions() ) {
        if ( (strcmp(seg->segname, "__DATA") == 0) && (seg->initprot != (VM_PROT_READ | VM_PROT_WRITE)) )
            return Error("__DATA segment permissions is not 'rw-'");
        if ( strcmp(seg->segname, "__DATA_CONST") == 0 ) {
            if ( seg->initprot != (VM_PROT_READ | VM_PROT_WRITE) )
                return Error("__DATA_CONST segment permissions is not 'rw-'");
            if ( (seg->flags & SG_READ_ONLY) == 0 ) {
                bool isSplitSegMarker = false;
                if ( this->isDylibOrStub() && this->hasSplitSegInfo(isSplitSegMarker) && !isSplitSegMarker ) {
                    // dylibs intended for dyld cache are allowed to not have SG_READ_ONLY set
                }
                else if ( this->inDyldCache() ) {
                    // dylibs in dyld cache are allowed to not have SG_READ_ONLY set
                }
                else if ( this->isStaticExecutable() ) {
                    // static excutables don't use dyld so have no way to make DATA_CONST read-only
                }
                else if ( policy.enforceDataConstSegmentPermissions() ) {
                    return Error("__DATA_CONST segment missing SG_READ_ONLY flag");
                }
            }
        }
    }

    // check for vmaddr wrapping
    if ( (seg->vmaddr + seg->vmsize) < seg->vmaddr )
        return Error("'%s' segment vm range wraps", seg->segname);

    // dylib stubs have no section data
    if ( !isDylibStub() ) {
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
    }

    return Error::none();
}
#endif // !TARGET_OS_EXCLAVEKIT

struct VIS_HIDDEN Interval
{
    bool     overlaps(const Interval& other) const;
    uint64_t size() const { return end - start; }
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
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg64 = (segment_command_64*)cmd;
            if ( strcmp(seg64->segname, "__TEXT") == 0 ) {
                hasTEXT          = true;
                segmentIndexText = ranges.count();
            }
            else if ( strcmp(seg64->segname, "__LINKEDIT") == 0 ) {
                hasLINKEDIT          = true;
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
            if ( !this->inDyldCache() && (ranges[segmentIndexText].file.start != 0) )
                return Error("__TEXT segment fileoffset is not zero");
            const uint32_t headerAndLCSize = machHeaderSize() + mh.sizeofcmds;
            if ( ranges[segmentIndexText].file.size() < headerAndLCSize )
                return Error("load commands do not fit in __TEXT segment filesize");
            if ( ranges[segmentIndexText].vm.size() < headerAndLCSize )
                return Error("load commands do not fit in __TEXT segment vmsize");
        }
        else {
            return Error("missing __TEXT segment");
        }
        (void)hasLINKEDIT;
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
            // can't compare file offsets for segments in dyld cache because they may be offsets into different files
            if ( !this->inDyldCache() && r1.file.overlaps(r2.file) )
                return Error("file range of segment '%s' overlaps segment '%s'", r1.name, r2.name);
        }
    }

    // check segment load command order matches file content order which matches vm order
    // skip dyld cache because segments are moved around too much
    if ( policy.enforceSegmentOrderMatchesLoadCmds() && !inDyldCache() && (ranges.count() > 1) ) {
        for (int i=1; i < ranges.count()-1; ++i) {
            const SegRange& a = ranges[i-1];
            const SegRange& b = ranges[i];
            if ( (b.file.start < a.file.start) && (b.file.start != b.file.end) )
                return Error("segment '%s' file offset out of order", a.name);
            if ( b.vm.start < a.vm.start ) {
                if ( isFileSet() && (strcmp(b.name, "__PRELINK_INFO") == 0) ) {
                    // __PRELINK_INFO may have no vmaddr set
                }
                else {
                    return Error("segment '%s' vm address out of order", b.name);
                }
            }
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

bool Header::isDylinker() const {
    if ( mh.filetype != MH_DYLINKER ) {
        return false;
    }
    return true;
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

    if ( foundPlatform )
        return;

    if ( !gHeaderAddImplicitPlatform ) {
        // no implicit platforms in static linker
        // but for object files and -preload files only, we need to support linking against old macos dylibs
        if ( isObjectFile() || isPreload() || isKextBundle() || isStaticExecutable() )
            return;
    }

    // old binary with no explicit platform
#if TARGET_OS_OSX
    if ( (mh.cputype == CPU_TYPE_X86_64) || (mh.cputype == CPU_TYPE_I386) )
        handler(Platform::macOS, Version32(10, 5), Version32(10, 5)); // guess it is a macOS 10.5 binary
    // <rdar://problem/75343399>
    // The Go linker emits non-standard binaries without a platform and we have to live with it.
    if ( mh.cputype == CPU_TYPE_ARM64 )
        handler(Platform::macOS, Version32(11, 0), Version32(11, 0)); // guess it is a macOS 11.0 binary
#endif
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

bool Header::builtForSimulator() const {
    return platformAndVersions().platform.isSimulator();
}

void Header::forEachBuildTool(void (^handler)(Platform platform, uint32_t tool, uint32_t version)) const
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool &stop) {
        switch ( cmd->cmd ) {
            case LC_BUILD_VERSION: {
                const build_version_command* buildCmd = (build_version_command *)cmd;
                for ( uint32_t i = 0; i != buildCmd->ntools; ++i ) {
                    uint32_t offset = sizeof(build_version_command) + (i * sizeof(build_tool_version));
                    if ( offset >= cmd->cmdsize )
                        break;

                    const build_tool_version* firstTool = (const build_tool_version*)(&buildCmd[1]);
                    handler(Platform(buildCmd->platform), firstTool[i].tool, firstTool[i].version);
                }
            }
        }
    });
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
        if ( cmd->cmd == LC_ID_DYLIB || cmd->cmd == LC_ID_DYLINKER) {
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

bool Header::sourceVersion(Version64 &version) const
{
    __block bool found = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SOURCE_VERSION ) {
            const source_version_command* svc = (const source_version_command*)cmd;
            version = Version64(svc->version);
            found = true;
            stop  = true;
        }
    });
    
    return found;
}


const char* Header::linkedDylibLoadPath(uint32_t depIndex) const
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

uint32_t Header::linkedDylibCount(bool* allDepsAreNormal) const
{
    if ( allDepsAreNormal != nullptr )
        *allDepsAreNormal = true;
    __block unsigned count   = 0;
    this->forEachLinkedDylib(^(const char* loadPath, LinkedDylibAttributes kind, Version32 , Version32 , bool , bool& stop) {
        if ( allDepsAreNormal != nullptr ) {
            if ( kind != LinkedDylibAttributes::regular )
                *allDepsAreNormal = false;  // record if any linkages were weak, re-export, upward, or delay-init
        }
        ++count;
    });
    return count;
}

LinkedDylibAttributes Header::loadCommandToDylibKind(const dylib_command* dylibCmd)
{
    LinkedDylibAttributes attr;
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

void Header::forEachLinkedDylib(void (^callback)(const char* loadPath, LinkedDylibAttributes kind,
                                                 Version32 compatVersion, Version32 curVersion,
                                                 bool synthesizedLink, bool& stop)) const
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
                callback(loadPath, loadCommandToDylibKind(dylibCmd), Version32(dylibCmd->dylib.compatibility_version), Version32(dylibCmd->dylib.current_version), false, stop);
                ++count;
                if ( stop )
                    stopped = true;
                break;
            }
        }
    });

    // everything must link with something
    if ( (count == 0) && !stopped ) {
        // The dylibs that make up libSystem can link with nothing
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
        if ( this->builtForPlatform(Platform::driverKit, true) ) {
            if ( !this->isDylib() || (strncmp(this->installName(), "/System/DriverKit/usr/lib/system/", 33) != 0) )
                callback("/System/DriverKit/usr/lib/libSystem.B.dylib", LinkedDylibAttributes::regular, Version32(1, 0), Version32(1, 0), true, stopped);
        } else if ( this->platformAndVersions().platform.isExclaveKit() ) {
            if ( !this->isDylib() || (strncmp(this->installName(), "/System/ExclaveKit/usr/lib/system/", 34) != 0) )
                callback("/System/ExclaveKit/usr/lib/libSystem.dylib", LinkedDylibAttributes::regular, Version32(1, 0), Version32(1, 0), true, stopped);
        }
        else {
            if ( !this->isDylib() || (strncmp(this->installName(), "/usr/lib/system/", 16) != 0) )
                callback("/usr/lib/libSystem.B.dylib", LinkedDylibAttributes::regular, Version32(1, 0), Version32(1, 0), true, stopped);
        }
    }
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
                if ( equals != nullptr ) {
                    callback(keyEqualsValue, stop);
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
        case CPU_TYPE_ARM64_32:
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

    // <rdar://problem/13622786> ignore code signatures in macOS binaries built with pre-10.9 tools
    if ( mh.cputype == CPU_TYPE_X86_64 ) {
        PlatformAndVersions pvs = platformAndVersions();
        if ( (pvs.platform == Platform::macOS) && (pvs.sdk < Version32(10,9)) )
            return false;
    }

    return result;
}

bool Header::hasLinkerOptimizationHints(uint32_t& fileOffset, uint32_t& size) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_LINKER_OPTIMIZATION_HINT ) {
            linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            fileOffset                    = sigCmd->dataoff;
            size                          = sigCmd->datasize;
            result                        = true;
            stop                          = true;
        }
    });
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

bool Header::hasSplitSegInfo(bool& isSplitSegMarker) const
{
    __block bool result = false;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO) {
            linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            isSplitSegMarker = (sigCmd->datasize == 0);
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

bool Header::hasFunctionsVariantTable(uint64_t& runtimeOffset) const
{
    __block uint32_t fileOffset = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_FUNCTION_VARIANTS ) {
            const linkedit_data_command* leDataCmd = (linkedit_data_command*)cmd;
            fileOffset  = leDataCmd->dataoff;
            stop        = true;
        }
    });
    if ( fileOffset == 0 )
        return false;

    runtimeOffset = fileOffset + zerofillExpansionAmount();
    return true;
}


CString Header::libOrdinalName(int libOrdinal) const
{
    switch ( libOrdinal) {
        case BIND_SPECIAL_DYLIB_SELF:
            return "<this-image>";
        case BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE:
            return "<main-executable>";
        case BIND_SPECIAL_DYLIB_FLAT_LOOKUP:
            return "<flat-namespace>";
        case BIND_SPECIAL_DYLIB_WEAK_LOOKUP:
            return "<weak-def-coalesce>";
        default:
            if ( (libOrdinal > 0) && (libOrdinal <= this->linkedDylibCount()) )
                return this->linkedDylibLoadPath(libOrdinal-1);
            break;
    }
    return "<invalid-lib-ordinal>";
}

const load_command* Header::findLoadCommand(uint32_t& index, bool (^predicate)(const load_command* lc)) const
{
    index = 0;
    __block const load_command* lc = nullptr;
    __block uint32_t currentIndex = 0;
    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( predicate(cmd) ) {
            index = currentIndex;
            lc = cmd;
            stop = true;
        }

        ++currentIndex;
    });

    return lc;
}

void Header::findLoadCommandRange(uint32_t& index, uint32_t& endIndex, bool (^predicate)(const load_command* lc)) const
{
    index = UINT32_MAX;
    endIndex = UINT32_MAX;
    __block uint32_t currentIndex = 0;
    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( predicate(cmd) ) {
            if ( index == UINT32_MAX ) { // first match, set indices
                index    = currentIndex;
                endIndex = index+1;
            } else {
                assert(endIndex == currentIndex); // range must be contiguous
                ++endIndex;
            }
        } else if (index != UINT32_MAX) {
            // not a match, so stop looking if the first match had already been found
            stop = true;
        }

        ++currentIndex;
    });

    // clear index if none matched
    if ( index == UINT32_MAX ) {
        index = 0;
        endIndex = 0;
    }
}


const dylib_command* Header::isDylibLoadCommand(const load_command* lc)
{
    switch (lc->cmd) {
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
        case LC_REEXPORT_DYLIB:
            return (dylib_command*)lc;
        default:
            return nullptr;
    }
}

uint32_t Header::sizeForLinkedDylibCommand(const char *path, LinkedDylibAttributes depAttrs, uint32_t& traditionalCmd) const
{
    traditionalCmd = 0;
    if (      depAttrs == LinkedDylibAttributes::regular )
        traditionalCmd = LC_LOAD_DYLIB;
    else if ( depAttrs == LinkedDylibAttributes::justWeakLink )
        traditionalCmd = LC_LOAD_WEAK_DYLIB;
    else if ( depAttrs == LinkedDylibAttributes::justUpward )
        traditionalCmd = LC_LOAD_UPWARD_DYLIB;
    else if ( depAttrs == LinkedDylibAttributes::justReExport )
        traditionalCmd = LC_REEXPORT_DYLIB;

    if ( traditionalCmd )
        return pointerAligned((uint32_t)(sizeof(dylib_command) + strlen(path) + 1));

    // use new style load command
    return pointerAligned((uint32_t)(sizeof(dylib_use_command) + strlen(path) + 1));
}


const thread_command* Header::unixThreadLoadCommand() const {
    __block const thread_command* command = nullptr;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UNIXTHREAD ) {
            command = (const thread_command*)cmd;
            stop = true;
        }
    });
    return command;
}


const char* Header::protectionString(uint32_t flags, char str[8])
{
    str[0] = (flags & VM_PROT_READ)    ? 'r' : '.';
    str[1] = (flags & VM_PROT_WRITE)   ? 'w' : '.';
    str[2] = (flags & VM_PROT_EXECUTE) ? 'x' : '.';
    str[3] = '\0';
    return str;
}

static const char* sectionAttributes(uint32_t sectFlags)
{
    if ( sectFlags == 0 )
        return "";

    switch ( sectFlags & SECTION_TYPE ) {
        case S_ZEROFILL:
            return "S_ZEROFILL";
        case S_CSTRING_LITERALS:
            return "S_CSTRING_LITERALS";
        case S_NON_LAZY_SYMBOL_POINTERS:
            return "S_NON_LAZY_SYMBOL_POINTERS";
        case S_THREAD_LOCAL_VARIABLES:
            return "S_THREAD_LOCAL_VARIABLES";
        case S_INIT_FUNC_OFFSETS:
            return "S_INIT_FUNC_OFFSETS";
        case S_SYMBOL_STUBS:
            return "S_SYMBOL_STUBS";
    }

    if ( sectFlags == S_ATTR_PURE_INSTRUCTIONS )
        return "S_ATTR_PURE_INSTRUCTIONS";
    if ( sectFlags == (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS) )
        return "S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS";

    return "";
}

void Header::printLoadCommands(FILE* out, unsigned indentLevel) const
{
    char indentBuffer[indentLevel+2];
    for (unsigned i=0; i < indentLevel; ++i)
        indentBuffer[i] = ' ';
    indentBuffer[indentLevel] = '\0';
    const char* indent = indentBuffer;
    __block uint32_t lcIndex = 0;
    this->forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        fprintf(out, "%sLoad command #%d\n", indent, lcIndex);
        const dylib_command*                dylibCmd;
        const segment_command*              seg32Cmd;
        const segment_command_64*           seg64Cmd;
        const linkedit_data_command*        leDataCmd;
        const symtab_command*               symTabCmd;
        const dysymtab_command*             dynSymCmd;
        const uuid_command*                 uuidCmd;
        const version_min_command*          versMinCmd;
        const build_version_command*        buildVerCmd;
        const rpath_command*                rpathCmd;
        const dylinker_command*             dyldEnvCmd;
        const entry_point_command*          mainCmd;
        const sub_client_command*           subClientCmd;
        const routines_command*             inits32Cmd;
        const routines_command_64*          inits64Cmd;
        const encryption_info_command*      encrypt32Cmd;
        const encryption_info_command_64*   encrypt64Cmd;
        const dyld_info_command*            dyldInfoCmd;
        const target_triple_command*        tripleCmd;
        char                                versStr[64];
        char                                maxProtStr[8];
        char                                initProtStr[8];
        char                                linkAttrStr[64];
        uuid_string_t                       uuidStr;
        const section_64*                   sections64;
        const section*                      sections32;
        LinkedDylibAttributes               linkAttrs;
        const uint8_t*                      bytes;
        const uint32_t*                     words;
        uint64_t                            vers64;
        switch (cmd->cmd) {
            case LC_SEGMENT:
                seg32Cmd = (segment_command*)cmd;
                fprintf(out, "%s             cmd: LC_SEGMENT\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",       indent, cmd->cmdsize);
                fprintf(out, "%s         segname: \"%.*s\"\n",   indent, (int)name16(seg32Cmd->segname).size(), name16(seg32Cmd->segname).data());
                fprintf(out, "%s          vmaddr: 0x%08X\n",     indent, seg32Cmd->vmaddr);
                fprintf(out, "%s          vmsize: 0x%08X\n",     indent, seg32Cmd->vmsize);
                fprintf(out, "%s         fileoff: 0x%08X\n",     indent, seg32Cmd->fileoff);
                fprintf(out, "%s        filesize: 0x%08X\n",     indent, seg32Cmd->filesize);
                fprintf(out, "%s         maxprot: %s\n",         indent, Header::protectionString(seg32Cmd->maxprot, maxProtStr));
                fprintf(out, "%s        initprot: %s\n",         indent, Header::protectionString(seg32Cmd->initprot, initProtStr));
                if ( seg32Cmd->flags & SG_READ_ONLY )
                    fprintf(out, "%s          flags: SG_READ_ONLY\n",indent);
                fprintf(out, "%s          nsects: %d\n",         indent, seg32Cmd->nsects);
                sections32 = (section*)((char*)seg32Cmd + sizeof(struct segment_command));
                for (int i=0; i < seg32Cmd->nsects; ++i) {
                    fprintf(out, "%s        sections[%d].sectname: \"%.*s\"\n", indent, i, (int)name16(sections32[i].sectname).size(), name16(sections32[i].sectname).data());
                    fprintf(out, "%s        sections[%d].addr:     0x%08X\n",   indent, i, sections32[i].addr);
                    fprintf(out, "%s        sections[%d].size:     0x%08X\n",   indent, i, sections32[i].size);
                    if ( sections32[i].offset != 0 )
                        fprintf(out, "%s        sections[%d].offset:   0x%08X\n",  indent, i, sections32[i].offset);
                    fprintf(out, "%s        sections[%d].align:    2^%d\n",        indent, i, sections32[i].align);
                    if ( sections32[i].flags != 0 )
                        fprintf(out, "%s        sections[%d].type:      %s\n",     indent, i, sectionAttributes(sections32[i].flags));
                    if ( sections32[i].reserved1 != 0 )
                        fprintf(out, "%s        sections[%d].reserved1: %d\n",     indent, i, sections32[i].reserved1);
                    if ( sections32[i].reserved2 != 0 )
                        fprintf(out, "%s        sections[%d].reserved2: %d\n",     indent, i, sections32[i].reserved2);
                }
                break;
            case LC_SYMTAB:
                symTabCmd = (symtab_command*)cmd;
                fprintf(out, "%s             cmd: LC_SYMTAB\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",      indent, cmd->cmdsize);
                fprintf(out, "%s          symoff: 0x%X\n",      indent, symTabCmd->symoff);
                fprintf(out, "%s           nsyms: %d\n",        indent, symTabCmd->nsyms);
                fprintf(out, "%s          stroff: 0x%X\n",      indent, symTabCmd->stroff);
                fprintf(out, "%s         strsize: 0x%X\n",      indent, symTabCmd->strsize);
                break;
            case LC_UNIXTHREAD:
                words = (uint32_t*)cmd;
                fprintf(out, "%s             cmd: LC_UNIXTHREAD\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",          indent, cmd->cmdsize);
                if ( words[2] == 6 ) { // ARM_THREAD_STATE64
                    fprintf(out, "%s              pc: 0x%llX\n",        indent, *(uint64_t*)(&words[68]));
                }
                else if ( words[2] == 4 ) { // x86_THREAD_STATE64
                    fprintf(out, "%s              pc: 0x%llX\n",        indent, *(uint64_t*)(&words[36]));
                }
                else if ( words[2] == 1 ) { // x86_THREAD_STATE
                    fprintf(out, "%s              pc: 0x%X\n",          indent, words[14]);
                }
                break;
            case LC_DYSYMTAB:
                dynSymCmd = (dysymtab_command*)cmd;
                fprintf(out, "%s             cmd: LC_DYSYMTAB\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",        indent, cmd->cmdsize);
                fprintf(out, "%s       ilocalsym: %d\n",          indent, dynSymCmd->ilocalsym);
                fprintf(out, "%s       nlocalsym: %d\n",          indent, dynSymCmd->nlocalsym);
                fprintf(out, "%s      iextdefsym: %d\n",          indent, dynSymCmd->iextdefsym);
                fprintf(out, "%s      nextdefsym: %d\n",          indent, dynSymCmd->nextdefsym);
                fprintf(out, "%s       iundefsym: %d\n",          indent, dynSymCmd->iundefsym);
                fprintf(out, "%s       nundefsym: %d\n",          indent, dynSymCmd->nundefsym);
                fprintf(out, "%s  indirectsymoff: 0x%08X\n",      indent, dynSymCmd->indirectsymoff);
                fprintf(out, "%s   nindirectsyms: %d\n",          indent, dynSymCmd->nindirectsyms);
                fprintf(out, "%s       ilocalsym: %d\n",          indent, dynSymCmd->ilocalsym);
               break;
            case LC_LOAD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                linkAttrs = loadCommandToDylibKind(dylibCmd);
                fprintf(out, "%s             cmd: LC_LOAD_DYLIB\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",          indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",        indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                if ( linkAttrs.raw != 0 ) {
                    linkAttrs.toString(linkAttrStr);
                    fprintf(out, "%s          attrs: %s\n",             indent, linkAttrStr);
                }
                fprintf(out, "%s        cur-vers: %s\n",            indent, Version32(dylibCmd->dylib.current_version).toString(versStr));
                fprintf(out, "%s     compat-vers: %s\n",            indent, Version32(dylibCmd->dylib.compatibility_version).toString(versStr));
                break;
            case LC_ID_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_ID_DYLIB\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",        indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",      indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                fprintf(out, "%s        cur-vers: %s\n",          indent, Version32(dylibCmd->dylib.current_version).toString(versStr));
                fprintf(out, "%s     compat-vers: %s\n",          indent, Version32(dylibCmd->dylib.compatibility_version).toString(versStr));
                break;
            case LC_LOAD_DYLINKER:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_LOAD_DYLINKER\n",  indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",            indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                break;
            case LC_ID_DYLINKER:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_ID_DYLINKER\n",    indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",            indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                break;
            case LC_ROUTINES:
                inits32Cmd = (routines_command*)cmd;
                fprintf(out, "%s             cmd: LC_ROUTINES\n",       indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s    init_address: 0x%X\n",              indent, inits32Cmd->init_address);
                break;
            case LC_SUB_FRAMEWORK:
                subClientCmd = (sub_client_command*)cmd;
                fprintf(out, "%s             cmd: LC_SUB_FRAMEWORK\n",  indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s        umbrella: \"%s\"\n",            indent, (char*)subClientCmd + subClientCmd->client.offset);
                break;
            case LC_SUB_UMBRELLA:
                subClientCmd = (sub_client_command*)cmd;
                fprintf(out, "%s             cmd: LC_SUB_UMBRELLA\n",   indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s    sub_umbrella: \"%s\"\n",            indent, (char*)subClientCmd + subClientCmd->client.offset);
                break;
            case LC_SUB_CLIENT:
                subClientCmd = (sub_client_command*)cmd;
                fprintf(out, "%s             cmd: LC_SUB_CLIENT\n",     indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s          client: \"%s\"\n",            indent, (char*)subClientCmd + subClientCmd->client.offset);
                break;
            case LC_SUB_LIBRARY:
                subClientCmd = (sub_client_command*)cmd;
                fprintf(out, "%s             cmd: LC_SUB_LIBRARY\n",    indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s     sub_library: \"%s\"\n",            indent, (char*)subClientCmd + subClientCmd->client.offset);
                break;
            case LC_LOAD_WEAK_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_LOAD_WEAK_DYLIB\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",            indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                fprintf(out, "%s        cur-vers: %s\n",                indent, Version32(dylibCmd->dylib.current_version).toString(versStr));
                fprintf(out, "%s     compat-vers: %s\n",                indent, Version32(dylibCmd->dylib.compatibility_version).toString(versStr));
                break;
            case LC_SEGMENT_64:
                seg64Cmd = (segment_command_64*)cmd;
                fprintf(out, "%s             cmd: LC_SEGMENT_64\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",          indent, cmd->cmdsize);
                fprintf(out, "%s         segname: \"%.*s\"\n",      indent, (int)name16(seg64Cmd->segname).size(), name16(seg64Cmd->segname).data());
                fprintf(out, "%s          vmaddr: 0x%016llX\n",     indent, seg64Cmd->vmaddr);
                fprintf(out, "%s          vmsize: 0x%016llX\n",     indent, seg64Cmd->vmsize);
                fprintf(out, "%s         fileoff: 0x%08llX\n",      indent, seg64Cmd->fileoff);
                fprintf(out, "%s        filesize: 0x%08llX\n",      indent, seg64Cmd->filesize);
                fprintf(out, "%s         maxprot: %s\n",            indent, Header::protectionString(seg64Cmd->initprot, maxProtStr));
                fprintf(out, "%s        initprot: %s\n",            indent, Header::protectionString(seg64Cmd->initprot, initProtStr));
                if ( seg64Cmd->flags & SG_READ_ONLY )
                    fprintf(out, "%s          flags: SG_READ_ONLY\n",indent);
                fprintf(out, "%s          nsects: %d\n",            indent, seg64Cmd->nsects);
                sections64 = (section_64*)((char*)seg64Cmd + sizeof(struct segment_command_64));
                for (int i=0; i < seg64Cmd->nsects; ++i) {
                    fprintf(out, "%s        sections[%d].sectname: \"%.*s\"\n",    indent, i, (int)name16(sections64[i].sectname).size(), name16(sections64[i].sectname).data());
                    fprintf(out, "%s        sections[%d].addr:     0x%016llX\n",   indent, i, sections64[i].addr);
                    fprintf(out, "%s        sections[%d].size:     0x%016llX\n",   indent, i, sections64[i].size);
                    if ( sections64[i].offset != 0 )
                        fprintf(out, "%s        sections[%d].offset:   0x%08X\n",      indent, i, sections64[i].offset);
                    fprintf(out, "%s        sections[%d].align:    2^%d\n",        indent, i, sections64[i].align);
                    if ( sections64[i].flags != 0 )
                        fprintf(out, "%s        sections[%d].type:      %s\n",     indent, i, sectionAttributes(sections64[i].flags));
                    if ( sections64[i].reserved1 != 0 )
                        fprintf(out, "%s        sections[%d].reserved1: %d\n",     indent, i, sections64[i].reserved1);
                    if ( sections64[i].reserved2 != 0 )
                        fprintf(out, "%s        sections[%d].reserved2: %d\n",     indent, i, sections64[i].reserved2);
                }
                break;
            case LC_ROUTINES_64:
                inits64Cmd = (routines_command_64*)cmd;
                fprintf(out, "%s             cmd: LC_ROUTINES\n",       indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s    init_address: 0x%llX\n",            indent, inits64Cmd->init_address);
                break;
            case LC_UUID:
                uuidCmd = (uuid_command*)cmd;
                uuid_unparse_upper(uuidCmd->uuid, uuidStr);
                fprintf(out, "%s             cmd: LC_UUID\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",    indent, cmd->cmdsize);
                fprintf(out, "%s            uuid: %s\n",      indent, uuidStr);
               break;
            case LC_RPATH:
                rpathCmd = (rpath_command*)cmd;
                fprintf(out, "%s             cmd: LC_RPATH\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",     indent, cmd->cmdsize);
                fprintf(out, "%s           rpath:\"%s\"\n",    indent, (char*)cmd + rpathCmd->path.offset);
                break;
            case LC_CODE_SIGNATURE:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_CODE_SIGNATURE\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",              indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",              indent, leDataCmd->datasize);
                break;
            case LC_SEGMENT_SPLIT_INFO:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_SEGMENT_SPLIT_INFO\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                  indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",                  indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",                  indent, leDataCmd->datasize);
                break;
            case LC_REEXPORT_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_REEXPORT_DYLIB\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",            indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                fprintf(out, "%s        cur-vers: %s\n",                indent, Version32(dylibCmd->dylib.current_version).toString(versStr));
                fprintf(out, "%s     compat-vers: %s\n",                indent, Version32(dylibCmd->dylib.compatibility_version).toString(versStr));
                break;
            case LC_ENCRYPTION_INFO:
                encrypt32Cmd = (encryption_info_command*)cmd;
                fprintf(out, "%s             cmd: LC_ENCRYPTION_INFO\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s        cryptoff: 0x%X\n",              indent, encrypt32Cmd->cryptoff);
                fprintf(out, "%s       cryptsize: 0x%X\n",              indent, encrypt32Cmd->cryptsize);
                fprintf(out, "%s         cryptid: 0x%X\n",              indent, encrypt32Cmd->cryptid);
                break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                dyldInfoCmd = (dyld_info_command*)cmd;
                fprintf(out, "%s             cmd: LC_DYLD_INFO%s\n",     indent, (cmd->cmd == LC_DYLD_INFO_ONLY ? "_ONLY" : ""));
                fprintf(out, "%s         cmdsize: 0x%X\n",               indent, cmd->cmdsize);
                fprintf(out, "%s      rebase_off: 0x%X\n",               indent, dyldInfoCmd->rebase_off);
                fprintf(out, "%s     rebase_size: 0x%X\n",               indent, dyldInfoCmd->rebase_size);
                fprintf(out, "%s        bind_off: 0x%X\n",               indent, dyldInfoCmd->bind_off);
                fprintf(out, "%s       bind_size: 0x%X\n",               indent, dyldInfoCmd->bind_size);
                fprintf(out, "%s   weak_bind_off: 0x%X\n",               indent, dyldInfoCmd->weak_bind_off);
                fprintf(out, "%s  weak_bind_size: 0x%X\n",               indent, dyldInfoCmd->weak_bind_size);
                fprintf(out, "%s   lazy_bind_off: 0x%X\n",               indent, dyldInfoCmd->lazy_bind_off);
                fprintf(out, "%s  lazy_bind_size: 0x%X\n",               indent, dyldInfoCmd->lazy_bind_size);
                fprintf(out, "%s      export_off: 0x%X\n",               indent, dyldInfoCmd->export_off);
                fprintf(out, "%s     export_size: 0x%X\n",               indent, dyldInfoCmd->export_size);
                break;
            case LC_LOAD_UPWARD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                fprintf(out, "%s             cmd: LC_LOAD_UPWARD_DYLIB\n",  indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                  indent, cmd->cmdsize);
                fprintf(out, "%s            name: \"%s\"\n",                indent, (char*)dylibCmd + dylibCmd->dylib.name.offset);
                fprintf(out, "%s        cur-vers: %s\n",                    indent, Version32(dylibCmd->dylib.current_version).toString(versStr));
                fprintf(out, "%s     compat-vers: %s\n",                    indent, Version32(dylibCmd->dylib.compatibility_version).toString(versStr));
                break;
            case LC_VERSION_MIN_MACOSX:
                versMinCmd = (version_min_command*)cmd;
                fprintf(out, "%s             cmd: LC_VERSION_MIN_MACOSX\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                  indent, cmd->cmdsize);
                fprintf(out, "%s          min-OS: %s\n",                    indent, Version32(versMinCmd->version).toString(versStr));
                fprintf(out, "%s             sdk: %s\n",                    indent, Version32(versMinCmd->sdk).toString(versStr));
                break;
            case LC_VERSION_MIN_IPHONEOS:
                versMinCmd = (version_min_command*)cmd;
                fprintf(out, "%s             cmd: LC_VERSION_MIN_IPHONEOS\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                   indent, cmd->cmdsize);
                fprintf(out, "%s          min-OS: %s\n",                     indent, Version32(versMinCmd->version).toString(versStr));
                fprintf(out, "%s             sdk: %s\n",                     indent, Version32(versMinCmd->sdk).toString(versStr));
                break;
            case LC_FUNCTION_STARTS:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_FUNCTION_STARTS\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",              indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",              indent, leDataCmd->datasize);
                break;
            case LC_DYLD_ENVIRONMENT:
                dyldEnvCmd = (dylinker_command*)cmd;
                fprintf(out, "%s             cmd: LC_DYLD_ENVIRONMENT\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",     indent, cmd->cmdsize);
                fprintf(out, "%s            name:\"%s\"\n",    indent, (char*)dyldEnvCmd + dyldEnvCmd->name.offset);
                break;
            case LC_MAIN:
                mainCmd = (entry_point_command*)cmd;
                fprintf(out, "%s             cmd: LC_MAIN\n",       indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",          indent, cmd->cmdsize);
                fprintf(out, "%s        entryoff: 0x%llX\n",        indent, mainCmd->entryoff);
                if ( mainCmd->stacksize != 0 )
                    fprintf(out, "%s       stacksize: 0x%llX\n",        indent, mainCmd->stacksize);
                break;
            case LC_DATA_IN_CODE:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_DATA_IN_CODE\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",            indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",            indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",            indent, leDataCmd->datasize);
                break;
            case LC_SOURCE_VERSION:
                memcpy(&vers64, ((uint8_t*)cmd)+8, 8);  // in 32-bit arches load command may not be 8-byte aligned, so can't use source_version_command.version.
                fprintf(out, "%s             cmd: LC_SOURCE_VERSION\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s         version: %s\n",                indent, Version64(vers64).toString(versStr));
                break;
            case LC_ENCRYPTION_INFO_64:
                encrypt64Cmd = (encryption_info_command_64*)cmd;
                fprintf(out, "%s             cmd: LC_ENCRYPTION_INFO\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s        cryptoff: 0x%X\n",              indent, encrypt64Cmd->cryptoff);
                fprintf(out, "%s       cryptsize: 0x%X\n",              indent, encrypt64Cmd->cryptsize);
                fprintf(out, "%s         cryptid: 0x%X\n",              indent, encrypt64Cmd->cryptid);
                break;
            case LC_VERSION_MIN_TVOS:
                versMinCmd = (version_min_command*)cmd;
                fprintf(out, "%s             cmd: LC_VERSION_MIN_TVOS\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",               indent, cmd->cmdsize);
                fprintf(out, "%s          min-OS: %s\n",                 indent, Version32(versMinCmd->version).toString(versStr));
                fprintf(out, "%s             sdk: %s\n",                 indent, Version32(versMinCmd->sdk).toString(versStr));
                break;
            case LC_VERSION_MIN_WATCHOS:
                versMinCmd = (version_min_command*)cmd;
                fprintf(out, "%s             cmd: LC_VERSION_MIN_WATCHOS\n",indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                  indent, cmd->cmdsize);
                fprintf(out, "%s          min-OS: %s\n",                    indent, Version32(versMinCmd->version).toString(versStr));
                fprintf(out, "%s             sdk: %s\n",                    indent, Version32(versMinCmd->sdk).toString(versStr));
                break;
            case LC_BUILD_VERSION:
                buildVerCmd = (build_version_command*)cmd;
                fprintf(out, "%s             cmd: LC_BUILD_VERSION\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                  indent, cmd->cmdsize);
                fprintf(out, "%s        platform: %s\n",                    indent, Platform(buildVerCmd->platform).name().c_str());
                fprintf(out, "%s          min-OS: %s\n",                    indent, Version32(buildVerCmd->minos).toString(versStr));
                fprintf(out, "%s             sdk: %s\n",                    indent, Version32(buildVerCmd->sdk).toString(versStr));
                if ( buildVerCmd->ntools != 0 ) {
                    const build_tool_version*  toolVersions = (build_tool_version*)((uint8_t*)cmd + sizeof(build_version_command));
                    for (uint32_t i=0; i < buildVerCmd->ntools; ++i ) {
                        const char* toolName = "unknown";
                        switch ( toolVersions[i].tool ) {
                            case TOOL_CLANG:
                                toolName = "clang";
                                break;
                            case TOOL_SWIFT:
                                toolName = "swiftc";
                                break;
                            case TOOL_LD:
                                toolName = "ld";
                                break;
                            case TOOL_LLD:
                                toolName = "lld";
                                break;
                        }
                        fprintf(out, "%s         tool[%d].name:    %s\n",                    indent, i, toolName);
                        fprintf(out, "%s         tool[%d].version: %s\n",                    indent, i, Version32(toolVersions[i].version).toString(versStr));
                    }
                }
                break;
            case LC_DYLD_EXPORTS_TRIE:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_DYLD_EXPORTS_TRIE\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                 indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",                 indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",                 indent, leDataCmd->datasize);
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_DYLD_CHAINED_FIXUPS\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",                   indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",                   indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",                   indent, leDataCmd->datasize);
                break;
            case LC_FUNCTION_VARIANTS:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_FUNCTION_VARIANTS\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",            indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",            indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",            indent, leDataCmd->datasize);
                break;
            case LC_FUNCTION_VARIANT_FIXUPS:
                leDataCmd = (linkedit_data_command*)cmd;
                fprintf(out, "%s             cmd: LC_FUNCTION_VARIANT_FIXUPS\n", indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",            indent, cmd->cmdsize);
                fprintf(out, "%s         dataoff: 0x%X\n",            indent, leDataCmd->dataoff);
                fprintf(out, "%s        datasize: 0x%X\n",            indent, leDataCmd->datasize);
                break;
            case LC_TARGET_TRIPLE:
                tripleCmd = (target_triple_command*)cmd;
                fprintf(out, "%s             cmd: LC_TARGET_TRIPLE\n",  indent);
                fprintf(out, "%s         cmdsize: 0x%X\n",              indent, cmd->cmdsize);
                fprintf(out, "%s          triple:\"%s\"\n",             indent, (char*)cmd + tripleCmd->triple.offset);
                break;
            default:
                bytes = (uint8_t*)cmd + 8;
                fprintf(out, "%s             cmd: 0x%X\n",                   indent, cmd->cmd);
                fprintf(out, "%s         cmdsize: 0x%X\n",                   indent, cmd->cmdsize);
                fprintf(out, "%s           bytes: ",                         indent);
                for (int i=0; i < cmd->cmdsize; ++i) {
                    if ( ((i & 0xF) == 0) && (i != 0) )
                        fprintf(out, "%s                  ", indent);
                    fprintf(out, "%02X ", bytes[i]);
                    if ( (i & 0xF) == 0xF )
                        fprintf(out, "\n");
                }
                if ( (cmd->cmdsize & 0xF) != 0x0 )
                    fprintf(out, "\n");
                break;
        }
        ++lcIndex;
    });

    
}

bool Header::loadableIntoProcess(Platform processPlatform, CString path, bool internalInstall) const
{
    if ( this->builtForPlatform(processPlatform) )
        return true;

    // Some host macOS dylibs can be loaded into simulator processes
    if (processPlatform.isSimulator() && this->builtForPlatform(Platform::macOS)) {
        static constinit CString const macOSHost[] = {
            "/usr/lib/system/libsystem_kernel.dylib",
            "/usr/lib/system/libsystem_platform.dylib",
            "/usr/lib/system/libsystem_pthread.dylib",
            "/usr/lib/system/libsystem_platform_debug.dylib",
            "/usr/lib/system/libsystem_pthread_debug.dylib",
            "/usr/lib/system/host/liblaunch_sim.dylib",
        };
        if ( std::ranges::any_of(macOSHost, [&](const CString& libPath) { return libPath == path; }) ) {
            return true;
        }
    }

    // If this is being called on main executable where we expect a macOS program, Catalyst programs are also runnable
    if ( this->isMainExecutable() && (processPlatform == Platform::macOS) && this->builtForPlatform(Platform::macCatalyst, true) )
        return true;
#if (TARGET_OS_OSX && TARGET_CPU_ARM64)
    if ( this->isMainExecutable() && (processPlatform == Platform::macOS) && this->builtForPlatform(Platform::iOS, true) )
        return true;
#endif

    // allow iOS executables to use visionOS dylibs
    if ( (processPlatform == Platform::iOS) && this->builtForPlatform(Platform::visionOS, true) )
        return true;

    // allow iOS_Sim executables to use visionOS_Sim dylibs
    if ( (processPlatform == Platform::iOS_simulator) && this->builtForPlatform(Platform::visionOS_simulator, true) )
        return true;

    bool iOSonMac = (processPlatform == Platform::macCatalyst);
#if (TARGET_OS_OSX && TARGET_CPU_ARM64)
    // allow iOS binaries in iOSApp
    if ( processPlatform == Platform::iOS ) {
        // can load Catalyst binaries into iOS process
        if ( this->builtForPlatform(Platform::macCatalyst) )
            return true;
        iOSonMac = true;
    }
#endif
    // macOS dylibs can be loaded into iOSMac processes
    if ( iOSonMac && this->builtForPlatform(Platform::macOS, true) )
        return true;

    return false;
}

bool Header::hasPlusLoadMethod() const
{
    __block bool result = false;
    // in new objc runtime compiler puts classes/categories with +load method in specical section
    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( !info.segmentName.starts_with("__DATA") )
            return;
        if ( (info.sectionName == "__objc_nlclslist") || (info.sectionName == "__objc_nlcatlist") ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

// returns empty string if no triple specified
CString Header::targetTriple() const
{
    __block CString result;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_TARGET_TRIPLE ) {
            const char* targetTriple = (char*)cmd + ((struct target_triple_command*)cmd)->triple.offset;
            result = targetTriple;
        }
    });
    return result;
}

void Header::forEachSingletonPatch(void (^handler)(uint64_t runtimeOffset)) const
{
    uint32_t ptrSize = pointerSize();
    uint32_t elementSize = (2 * ptrSize);
    uint64_t loadAddress = preferredLoadAddress();
    forEachSection(^(const SectionInfo &sectInfo, bool &stop) {
        if ( sectInfo.sectionName != "__const_cfobj2" )
            return;
        stop = true;

        if ( (sectInfo.size % elementSize) != 0 ) {
            return;
        }

        if ( sectInfo.reserved2 != elementSize ) {
            // The linker must have rejected one or more of the elements in the section, so
            // didn't set the reserved2 to let us patch
            return;
        }

        for ( uint64_t offset = 0; offset != sectInfo.size; offset += elementSize ) {
            uint64_t targetRuntimeOffset = (sectInfo.address + offset) - loadAddress;
            handler(targetRuntimeOffset);
        }
    });
}

bool Header::hasObjCMessageReferences() const {
    uint64_t sectionRuntimeOffset, sectionSize;
    return findObjCDataSection("__objc_msgrefs", sectionRuntimeOffset, sectionSize);
}

bool Header::findObjCDataSection(CString sectionName, uint64_t& sectionRuntimeOffset, uint64_t& sectionSize) const
{
    uint64_t baseAddress = preferredLoadAddress();

    __block bool foundSection = false;
    forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( !sectInfo.segmentName.starts_with("__DATA") )
            return;
        if ( sectInfo.sectionName != sectionName )
            return;
        foundSection         = true;
        sectionRuntimeOffset = sectInfo.address - baseAddress;
        sectionSize          = sectInfo.size;
        stop                 = true;
    });
    return foundSection;
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

uint32_t Header::loadCommandsFreeSpace() const
{
    __block uint32_t firstSectionFileOffset = UINT32_MAX;
    __block uint32_t firstSegmentFileOffset = UINT32_MAX;
    forEachSection(^(const SectionInfo& sectInfo, bool &stop) {
        firstSectionFileOffset = sectInfo.fileOffset;
        firstSegmentFileOffset = segmentFileOffset(sectInfo.segIndex);
        stop = true;
    });

    // no segment load commands yet, so explicit size limit
    if ( firstSegmentFileOffset == UINT32_MAX )
        return UINT32_MAX;

    uint32_t headerSize = (mh.magic == MH_MAGIC_64) ? sizeof(mach_header_64) : sizeof(mach_header);
    uint32_t existSpaceUsed = mh.sizeofcmds + headerSize;
    return firstSectionFileOffset - firstSegmentFileOffset - existSpaceUsed;
}

uint64_t Header::preferredLoadAddress() const
{
    __block uint64_t textVmAddr = 0;
    forEachSegment(^(const SegmentInfo &info, bool &stop) {
        if ( info.segmentName == "__TEXT" ) {
            textVmAddr = info.vmaddr;
            stop = true;
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
    __block std::string_view    result;
    __block uint32_t            segCount = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( segIndex == segCount ) {
            result = info.segmentName;
            stop   = true;
        }
        ++segCount;
    });
    return result;
}

uint64_t Header::segmentVmSize(uint32_t segIndex) const
{
    __block uint64_t  result   = 0;
    __block uint32_t  segCount = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( segIndex == segCount ) {
            result = info.vmsize;
            stop   = true;
        }
        ++segCount;
    });
    return result;
}

uint64_t Header::segmentVmAddr(uint32_t segIndex) const
{
    __block uint64_t  result   = 0;
    __block uint32_t  segCount = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( segIndex == segCount ) {
            result = info.vmaddr;
            stop   = true;
        }
        ++segCount;
    });
    return result;
}

uint32_t Header::segmentFileOffset(uint32_t segIndex) const
{
    __block uint32_t  result   = 0;
    __block uint32_t  segCount = 0;
    this->forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( segIndex == segCount ) {
            result = info.fileOffset;
            stop   = true;
        }
        ++segCount;
    });
    return result;
}

// LC_SEGMENT stores names as char[16] potentially without a null terminator.
std::string_view Header::name16(const char name[16])
{
    size_t length = strnlen(name, 16);
    if ( length < 16 )
        return std::string_view(name, length);

    return std::string_view(name, 16);
}

void Header::forEachSegment(void (^callback)(const SegmentInfo& infos, bool& stop)) const
{
    __block uint16_t segmentIndex = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=(uint32_t)segCmd->fileoff, .fileSize=(uint32_t)segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            callback(segInfo, stop);
            ++segmentIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=segCmd->fileoff, .fileSize=segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            callback(segInfo, stop);
            ++segmentIndex;
        }
    });
}

void Header::forEachSegment(void (^callback)(const SegmentInfo& infos, uint64_t sizeOfSections, uint32_t maxAlignOfSections, bool& stop)) const
{
    __block uint16_t segmentIndex = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint32_t maxAlignOfSections = 0;
            const section_64* const sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section_64* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > maxAlignOfSections )
                    maxAlignOfSections = sect->align;
            }
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=(uint32_t)segCmd->fileoff, .fileSize=(uint32_t)segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            callback(segInfo, sizeOfSections, maxAlignOfSections, stop);
            ++segmentIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint32_t maxAlignOfSections = 0;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > maxAlignOfSections )
                    maxAlignOfSections = sect->align;
            }
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=segCmd->fileoff, .fileSize=segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            callback(segInfo, sizeOfSections, maxAlignOfSections, stop);
            ++segmentIndex;
        }
    });
}

void Header::forEachSection(void (^callback)(const SectionInfo&, bool& stop)) const
{
    __block uint16_t segmentIndex = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd        = (segment_command_64*)cmd;
            const section_64* const   sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const   sectionsEnd   = &sectionsStart[segCmd->nsects];
            for ( const section_64* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                SectionInfo info = {
                    // Note: use of segCmd->segname is for bin compat for copy protection in rdar://146096183
                    .sectionName=name16(sect->sectname), .segmentName=name16(segCmd->segname), .segIndex=segmentIndex,
                    .segMaxProt=(uint32_t)segCmd->maxprot, .segInitProt=(uint32_t)segCmd->initprot,
                    .flags=sect->flags, .alignment=sect->align, .address=sect->addr, .size=sect->size, .fileOffset=sect->offset,
                    .relocsOffset=sect->reloff, .relocsCount=sect->nreloc, .reserved1=sect->reserved1, .reserved2=sect->reserved2
                };
                callback(info, stop);
            }
            ++segmentIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd        = (segment_command*)cmd;
            const section* const   sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const   sectionsEnd   = &sectionsStart[segCmd->nsects];
            for ( const section* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                SectionInfo info = {
                    .sectionName=name16(sect->sectname), .segmentName=name16(sect->segname), .segIndex=segmentIndex,
                    .segMaxProt=(uint32_t)segCmd->maxprot, .segInitProt=(uint32_t)segCmd->initprot,
                    .flags=sect->flags, .alignment=sect->align, .address=sect->addr, .size=sect->size, .fileOffset=sect->offset,
                    .relocsOffset=sect->reloff, .relocsCount=sect->nreloc, .reserved1=sect->reserved1, .reserved2=sect->reserved2
                };
                callback(info, stop);
            }
            ++segmentIndex;
        }
    });
}

void Header::forEachSection(void (^callback)(const SegmentInfo&, const SectionInfo&, bool& stop)) const
{
    __block uint16_t segmentIndex = 0;
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64*   segCmd          = (segment_command_64*)cmd;
            const section_64* const     sectionsStart   = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const     sectionsEnd     = &sectionsStart[segCmd->nsects];
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=(uint32_t)segCmd->fileoff, .fileSize=(uint32_t)segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            for ( const section_64* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                SectionInfo info = {
                    .sectionName=name16(sect->sectname), .segmentName=name16(sect->segname), .segIndex=segmentIndex,
                    .segMaxProt=(uint32_t)segCmd->maxprot, .segInitProt=(uint32_t)segCmd->initprot,
                    .flags=sect->flags, .alignment=sect->align, .address=sect->addr, .size=sect->size, .fileOffset=sect->offset,
                    .relocsOffset=sect->reloff, .relocsCount=sect->nreloc, .reserved1=sect->reserved1, .reserved2=sect->reserved2
                };
                callback(segInfo, info, stop);
            }
            ++segmentIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command*  segCmd          = (segment_command*)cmd;
            const section* const    sectionsStart   = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const    sectionsEnd     = &sectionsStart[segCmd->nsects];
            SegmentInfo segInfo {
                .segmentName=name16(segCmd->segname), .vmaddr=segCmd->vmaddr, .vmsize=segCmd->vmsize,
                .fileOffset=segCmd->fileoff, .fileSize=segCmd->filesize, .flags=segCmd->flags,
                .segmentIndex=segmentIndex, .maxProt=(uint8_t)segCmd->maxprot, .initProt=(uint8_t)segCmd->initprot,
            };
            for ( const section* sect = sectionsStart; !stop && (sect < sectionsEnd); ++sect ) {
                SectionInfo info = {
                    .sectionName=name16(sect->sectname), .segmentName=name16(sect->segname), .segIndex=segmentIndex,
                    .segMaxProt=(uint32_t)segCmd->maxprot, .segInitProt=(uint32_t)segCmd->initprot,
                    .flags=sect->flags, .alignment=sect->align, .address=sect->addr, .size=sect->size, .fileOffset=sect->offset,
                    .relocsOffset=sect->reloff, .relocsCount=sect->nreloc, .reserved1=sect->reserved1, .reserved2=sect->reserved2
                };
                callback(segInfo, info, stop);
            }
            ++segmentIndex;
        }
    });
}

std::span<const uint8_t> Header::findSectionContent(CString segName, CString sectName, bool useVmOffset) const
{
    __block std::span<const uint8_t> result;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( (info.segmentName == segName) && (info.sectionName == sectName) ) {
            const uint8_t* sectionContent = nullptr;
            if ( useVmOffset ) {
                // dyld loaded image, find section based on vmaddr
                sectionContent = (uint8_t*)this + (info.address - this->preferredLoadAddress());
            }
            else {
                // file mapped image, use file offsets to get content
                sectionContent = (uint8_t*)this + info.fileOffset;
            }
            stop = true;
            result = std::span<const uint8_t>(sectionContent, (size_t)info.size);
        }
    });
    return result;
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
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( strcmp(segCmd->segname, "__TEXT") == 0 ) {
                textVmAddr     = segCmd->vmaddr;
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
            }
            else if ( strcmp(segCmd->segname, "__LINKEDIT") == 0 ) {
                uint64_t vmOffsetToLinkedit   = segCmd->vmaddr - textVmAddr;
                uint64_t fileOffsetToLinkedit = segCmd->fileoff;
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
    this->forEachInterposingSection(^(const SectionInfo& info, bool& stop) {
        hasInterposing = true;
        stop           = true;
    });
    return hasInterposing;
}

void Header::forEachInterposingSection(void (^callback)(const SectionInfo&, bool& stop)) const
{
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( ((info.flags & SECTION_TYPE) == S_INTERPOSING) || ((info.sectionName == "__interpose") && (info.segmentName.starts_with("__DATA") || info.segmentName.starts_with("__AUTH"))) ) {
            callback(info, stop);
        }
    });
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

bool Header::hasFunctionVariantFixups() const
{
    return hasLoadCommand(LC_FUNCTION_VARIANT_FIXUPS);
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

uint32_t Header::threadLoadCommandsSize(const Architecture& arch)
{
    uint32_t cmdSize = sizeof(thread_command);
    if ( arch.sameCpu(Architecture::arm64) )
        cmdSize = Header::pointerAligned(true, 16 + 34 * 8); // base size + ARM_EXCEPTION_STATE64_COUNT * 8
    else if ( arch.sameCpu(Architecture::arm64_32) )
        cmdSize = Header::pointerAligned(false, 16 + 34 * 8); // base size + ARM_EXCEPTION_STATE64_COUNT * 8
    else if ( arch.sameCpu(Architecture::x86_64) )
        cmdSize = Header::pointerAligned(true, 16 + 42 * 4); // base size + x86_THREAD_STATE64_COUNT * 4
    else if ( arch.usesThumbInstructions() || arch.usesArm32Instructions() )
        cmdSize = Header::pointerAligned(false, 16 + 17 * 4); // base size + ARM_THREAD_STATE_COUNT * 4
    else
        assert(0 && "unsupported arch for thread load command");
    return cmdSize;
}

uint32_t Header::threadLoadCommandsSize() const
{
    return Header::threadLoadCommandsSize(arch());
}

uint32_t Header::headerAndLoadCommandsSize() const
{
    return machHeaderSize() + mh.sizeofcmds;
}

uint32_t Header::pointerAligned(bool is64, uint32_t value)
{
    // mach-o requires all load command sizes to be a multiple the pointer size
    if ( is64 )
        return ((value + 7) & (-8));
    else
        return ((value + 3) & (-4));
}

uint32_t Header::pointerAligned(uint32_t value) const
{
    return Header::pointerAligned(is64(), value);
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
        if ( size != 0 )
            return size;
        return headerAndLoadCommandsSize();
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

struct OldThreadsStartSection
{
    uint32_t        padding : 31,
                    stride8 : 1;
    uint32_t        chain_starts[1];
};

// ld64 can't sometimes determine the size of __thread_starts accurately,
// because these sections have to be given a size before everything is laid out,
// and you don't know the actual size of the chains until everything is
// laid out. In order to account for this, the linker puts trailing 0xFFFFFFFF at
// the end of the section, that must be ignored when walking the chains. This
// patch adjust the section size accordingly.
static uint32_t adjustStartsCount(uint32_t startsCount, const uint32_t* starts) {
    for ( int i = startsCount; i > 0; --i )
    {
        if ( starts[i - 1] == 0xFFFFFFFF )
            startsCount--;
        else
            break;
    }
    return startsCount;
}

bool Header::hasFirmwareChainStarts(uint16_t* pointerFormat, uint32_t* startsCount, const uint32_t** starts) const
{
    if ( !this->isPreload() && !this->isStaticExecutable() )
        return false;

    __block bool result = false;
    this->forEachSection(^(const SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__chain_starts") && (info.segmentName == "__TEXT") ) {
            const dyld_chained_starts_offsets* startsSect = (dyld_chained_starts_offsets*)((uint8_t*)this + info.fileOffset);
            if ( pointerFormat != nullptr )
                *pointerFormat = startsSect->pointer_format;
            if ( startsCount != nullptr )
                *startsCount   = (info.size >= 4) ? startsSect->starts_count : 0;
            if ( starts != nullptr )
                *starts        = &startsSect->chain_starts[0];
            result             = true;
            stop               = true;
        }
        else if ( (info.sectionName == "__thread_starts") && (info.segmentName == "__TEXT") ) {
            const OldThreadsStartSection* sect = (OldThreadsStartSection*)((uint8_t*)this + info.fileOffset);
            if ( pointerFormat != nullptr )
                *pointerFormat = sect->stride8 ? DYLD_CHAINED_PTR_ARM64E : DYLD_CHAINED_PTR_ARM64E_FIRMWARE;
            if ( startsCount != nullptr )
                *startsCount   = adjustStartsCount((uint32_t)(info.size/4) - 1, sect->chain_starts);
            if ( starts != nullptr )
                *starts        = sect->chain_starts;
            result             = true;
            stop               = true;
        }
    });
    return result;
}

bool Header::hasFirmwareRebaseRuns() const
{
    return forEachFirmwareRebaseRuns(nullptr);
}

// content of __TEXT,__rebase_info section
struct RebaseInfo
{
    uint32_t  startAddress;
    uint8_t   runs[];   // value of even indexes is how many pointers in a row are rebases, value of odd indexes times 4 is memory to skip over
                        // two zero values in a row signals the end of the run
};


bool Header::forEachFirmwareRebaseRuns(void (^handler)(uint32_t offset, bool& stop)) const
{
    if ( !this->isPreload() && !this->isStaticExecutable() )
        return false;

    __block const RebaseInfo* rr  = nullptr;
    __block const RebaseInfo* end = nullptr;
    this->forEachSection(^(const SectionInfo& info, bool& sectStop) {
        if ( (info.sectionName == "__rebase_info") && (info.segmentName == "__TEXT") ) {
            sectStop = true;
            rr       = (RebaseInfo*)((uint8_t*)this + info.fileOffset);
            end      = (RebaseInfo*)((uint8_t*)rr + info.size);
        }
    });
    if ( rr == nullptr )
        return false;

    if ( handler != nullptr ) {
        bool stop = false;
        while ( (rr < end) && !stop ) {
            uint32_t address = rr->startAddress;
            int      index   = 0;
            bool     done    = false;
            while ( !done && !stop ) {
                uint8_t count = rr->runs[index];
                if ( count == 0 ) {
                    // two 0x00 in a row mean the run is complete
                    if ( rr->runs[index+1] == 0 ) {
                        ++index;
                        done = true;
                    }
                }
                else {
                    if ( index & 1 ) {
                        // odd runs index => how much to jump forward
                        address += ((count-1) * 4);
                    }
                    else {
                        // even runs index => how many pointers in a row that need rebasing
                        for (int i=0; i < count; ++i) {
                            handler(address, stop);
                            address += 4;
                        }
                    }
                }
                ++index;
            }
            // 4-byte align for next run
            index = (index+3) & (-4);
            rr  = (RebaseInfo*)(&rr->runs[index]);
        }
    }

    return true;
}


} // namespace dyld3
