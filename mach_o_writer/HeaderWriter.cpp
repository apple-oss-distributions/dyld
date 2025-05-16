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

// mach_o
#include "Array.h"
#include "Architecture.h"
#include "Misc.h"
#include "Policy.h"
#include "LoggingStub.h"
#include "Version32.h"

// mach_o_writer
#include "HeaderWriter.h"

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
// MARK: --- methods that create and modify ---
//

HeaderWriter* HeaderWriter::make(std::span<uint8_t> buffer, uint32_t filetype, uint32_t flags, Architecture arch, bool addImplicitTextSegment)
{
    const size_t minHeaderAlignment = filetype == MH_OBJECT ? 8 : getpagesize();
    assert(((uint64_t)buffer.data() & (minHeaderAlignment - 1)) == 0);
    assert(buffer.size() >= sizeof(mach_header_64));
    bzero(buffer.data(), buffer.size());
    HeaderWriter& header = *(HeaderWriter*)buffer.data();
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
        SegmentInfo segInfo { .segmentName="__TEXT", .vmaddr=0, .vmsize=0x1000, .fileOffset=0, .fileSize=0x1000, .maxProt=(VM_PROT_READ | VM_PROT_EXECUTE), .initProt=(VM_PROT_READ | VM_PROT_EXECUTE) };
        header.addSegment(segInfo, std::array { "__text" });
    }

    return &header;
}

void HeaderWriter::save(char savedPath[PATH_MAX]) const
{
    ::strcpy(savedPath, "/tmp/mocko-XXXXXX");
    int fd = ::mkstemp(savedPath);
    if ( fd != -1 ) {
        size_t size = sizeof(mach_header_64) + mh.sizeofcmds;
        ::pwrite(fd, this, size, 0);
        ::close(fd);
    }
}

load_command* HeaderWriter::firstLoadCommand()
{
    if ( mh.magic == MH_MAGIC )
        return (load_command*)((uint8_t*)this + sizeof(mach_header));
    else
        return (load_command*)((uint8_t*)this + sizeof(mach_header_64));
}

// creates space for a new load command, but does not fill in its payload
load_command* HeaderWriter::appendLoadCommand(uint32_t cmd, uint32_t cmdSize)
{
    load_command* thisCmd = (load_command*)((uint8_t*)firstLoadCommand() + mh.sizeofcmds);
    thisCmd->cmd          = cmd;
    thisCmd->cmdsize      = cmdSize;
    mh.ncmds += 1;
    mh.sizeofcmds += cmdSize;

    return thisCmd;
}

// copies a new load command from another
void HeaderWriter::appendLoadCommand(const load_command* lc)
{
    load_command* thisCmd = (load_command*)((uint8_t*)firstLoadCommand() + mh.sizeofcmds);
    ::memcpy(thisCmd, lc, lc->cmdsize);
    mh.ncmds += 1;
    mh.sizeofcmds += lc->cmdsize;
}

void HeaderWriter::addBuildVersion(Platform platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools)
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

void HeaderWriter::addMinVersion(Platform platform, Version32 minOS, Version32 sdk)
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

void HeaderWriter::setHasThreadLocalVariables()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_HAS_TLV_DESCRIPTORS;
}

void HeaderWriter::setHasWeakDefs()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_WEAK_DEFINES;
}

void HeaderWriter::setUsesWeakDefs()
{
    assert(mh.filetype != MH_OBJECT);
    mh.flags |= MH_BINDS_TO_WEAK;
}

void HeaderWriter::setAppExtensionSafe()
{
    assert(mh.filetype == MH_DYLIB || mh.filetype == MH_DYLIB_STUB);
    mh.flags |= MH_APP_EXTENSION_SAFE;
}

void HeaderWriter::setSimSupport()
{
    assert(mh.filetype == MH_DYLIB || mh.filetype == MH_DYLIB_STUB);
    mh.flags |= MH_SIM_SUPPORT;
}

void HeaderWriter::setNoReExportedDylibs()
{
    assert(mh.filetype == MH_DYLIB || mh.filetype == MH_DYLIB_STUB);
    mh.flags |= MH_NO_REEXPORTED_DYLIBS;
}

void HeaderWriter::addPlatformInfo(Platform platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools)
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

void HeaderWriter::addNullUUID()
{
    uuid_command uc;
    uc.cmd     = LC_UUID;
    uc.cmdsize = sizeof(uuid_command);
    bzero(uc.uuid, 16);
    appendLoadCommand((load_command*)&uc);
}

void HeaderWriter::addUniqueUUID(uuid_t copyOfUUID)
{
    uuid_command uc;
    uc.cmd     = LC_UUID;
    uc.cmdsize = sizeof(uuid_command);
    uuid_generate_random(uc.uuid);
    appendLoadCommand((load_command*)&uc);
    if ( copyOfUUID )
        memcpy(copyOfUUID, uc.uuid, sizeof(uuid_t));
}

void HeaderWriter::updateUUID(uuid_t uuid)
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

void HeaderWriter::addSegment(const SegmentInfo& info, std::span<const char* const> sectionNames)
{
    if ( is64() ) {
        uint32_t            lcSize = (uint32_t)(sizeof(segment_command_64) + sectionNames.size() * sizeof(section_64));
        segment_command_64* sc     = (segment_command_64*)appendLoadCommand(LC_SEGMENT_64, lcSize);
        strncpy(sc->segname, info.segmentName.begin(), 16);
        sc->vmaddr             = info.vmaddr;
        sc->vmsize             = info.vmsize;
        sc->fileoff            = info.fileOffset;
        sc->filesize           = info.fileSize;
        sc->maxprot            = info.maxProt;
        sc->initprot           = info.initProt;
        sc->nsects             = (uint32_t)sectionNames.size();
        sc->flags              = info.flags;
        section_64* const sect = (section_64*)((uint8_t*)sc + sizeof(struct segment_command_64));
        uint32_t sectionIndex  = 0;
        for ( const char* sectName : sectionNames ) {
            strncpy(sect[sectionIndex].segname, info.segmentName.begin(), 16);
            strncpy(sect[sectionIndex].sectname, sectName, 16);
            ++sectionIndex;
        }
    }
    else {
        uint32_t         lcSize = (uint32_t)(sizeof(segment_command) + sectionNames.size() * sizeof(section));
        segment_command* sc     = (segment_command*)appendLoadCommand(LC_SEGMENT, lcSize);
        strncpy(sc->segname, info.segmentName.begin(), 16);
        sc->vmaddr             = (uint32_t)info.vmaddr;
        sc->vmsize             = (uint32_t)info.vmsize;
        sc->fileoff            = info.fileOffset;
        sc->filesize           = info.fileSize;
        sc->maxprot            = info.maxProt;
        sc->initprot           = info.initProt;
        sc->nsects             = (uint32_t)sectionNames.size();
        sc->flags              = info.flags;
        section* const sect = (section*)((uint8_t*)sc + sizeof(struct segment_command));
        uint32_t sectionIndex  = 0;
        for ( const char* sectName : sectionNames ) {
            strncpy(sect[sectionIndex].segname, info.segmentName.begin(), 16);
            strncpy(sect[sectionIndex].sectname, sectName, 16);
            ++sectionIndex;
        }
    }
}

void HeaderWriter::updateSection(const SectionInfo& info)
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            segment_command_64* segCmd = (segment_command_64*)cmd;
            if (info.segmentName == segCmd->segname) {
                section_64* const  sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
                section_64* const  sectionsEnd   = &sectionsStart[segCmd->nsects];
                for ( section_64* sect=sectionsStart; sect < sectionsEnd; ++sect ) {
                    if ( strncmp(info.sectionName.begin(), sect->sectname, 16) == 0 ) {
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
                    if ( strncmp(info.sectionName.begin(), sect->sectname, 16) == 0 ) {
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

void HeaderWriter::updateSegment(const SegmentInfo& info)
{
    forEachLoadCommandSafe(^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            segment_command_64* segCmd = (segment_command_64*)cmd;
            if (info.segmentName == segCmd->segname) {
                segCmd->vmaddr             = info.vmaddr;
                segCmd->vmsize             = info.vmsize;
                segCmd->fileoff            = info.fileOffset;
                segCmd->filesize           = info.fileSize;
                segCmd->maxprot            = info.maxProt;
                segCmd->initprot           = info.initProt;
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
                segCmd->maxprot            = info.maxProt;
                segCmd->initprot           = info.initProt;
                stop = true;
                return;
            }
        }
    });
}

Error HeaderWriter::removeLoadCommands(uint32_t index, uint32_t endIndex)
{
    if ( index == endIndex )
        return Error::none();

    __block uint8_t* lcStart = nullptr;
    __block uint8_t* lcRemoveStart = nullptr;
    __block uint8_t* lcRemoveEnd = nullptr;

    __block uint32_t currentIndex = 0;
    forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
        if ( lcStart == nullptr )
            lcStart = (uint8_t*)cmd;

        if ( currentIndex == index ) {
            lcRemoveStart = (uint8_t*)cmd;
        } else if ( currentIndex == endIndex ) {
            lcRemoveEnd = (uint8_t*)cmd;
        }

        ++currentIndex;
        stop = lcRemoveStart != nullptr && lcRemoveEnd != nullptr;
    });

    if ( lcRemoveStart == nullptr || lcRemoveEnd == nullptr )
        return Error("invalid load command range to remove");

    uint8_t* lcEnd = lcStart+mh.sizeofcmds;
    assert(lcRemoveStart >= lcStart && lcRemoveStart <= lcEnd);
    assert(lcRemoveEnd >= lcStart && lcRemoveEnd <= lcEnd);
    memmove(lcRemoveStart, lcRemoveEnd, (lcEnd-lcRemoveEnd));

    mh.ncmds = mh.ncmds - (endIndex - index);
    mh.sizeofcmds = mh.sizeofcmds - (uint32_t)(lcRemoveEnd-lcRemoveStart);
    return Error::none();
}

load_command* HeaderWriter::insertLoadCommand(uint32_t atIndex, uint32_t cmdSize)
{
    if ( loadCommandsFreeSpace() < cmdSize )
        return nullptr;

    if ( pointerAligned(cmdSize) != cmdSize ) // command size needs to be pointer aligned
        return nullptr;

    uint8_t* lcStart = (uint8_t*)firstLoadCommand();
    __block uint8_t* insertLocation = nullptr;
    if ( atIndex == 0 ) {
        insertLocation = lcStart;
    } else if ( atIndex == mh.ncmds ) {
        insertLocation = lcStart+mh.sizeofcmds;
    } else {
        __block uint32_t current = 0;
        forEachLoadCommandSafe(^(const load_command *cmd, bool &stop) {
            if ( current == atIndex ) {
                insertLocation = (uint8_t*)cmd;
                stop = true;
            }
            ++current;
        });
        if ( insertLocation == nullptr ) // invalid insert index
            return nullptr;
    }

    // move existing load commands after the new location
    memmove(insertLocation+cmdSize, insertLocation, (lcStart+mh.sizeofcmds)-insertLocation);

    // update header
    mh.ncmds += 1;
    mh.sizeofcmds += cmdSize;

    // set initial size
    load_command* lcOut = (load_command*)insertLocation;
    bzero(lcOut, cmdSize);
    lcOut->cmdsize = cmdSize;
    return lcOut;
}

void HeaderWriter::addInstallName(const char* name, Version32 compatVers, Version32 currentVersion)
{
    uint32_t       alignedSize      = pointerAligned((uint32_t)(sizeof(dylib_command) + strlen(name) + 1));
    dylib_command* ic               = (dylib_command*)appendLoadCommand(LC_ID_DYLIB, alignedSize);
    ic->dylib.name.offset           = sizeof(dylib_command);
    ic->dylib.current_version       = currentVersion.value();
    ic->dylib.compatibility_version = compatVers.value();
    strcpy((char*)ic + ic->dylib.name.offset, name);
}

void HeaderWriter::addLinkedDylib(const char* path, LinkedDylibAttributes depAttrs, Version32 compatVers, Version32 currentVersion)
{
    uint32_t cmd  = 0;
    uint32_t size = sizeForLinkedDylibCommand(path, depAttrs, cmd);

    load_command* lc = appendLoadCommand(0, size);
    setLinkedDylib(lc, path, depAttrs, compatVers, currentVersion);
}

void HeaderWriter::setLinkedDylib(load_command* lc, const char* path, LinkedDylibAttributes depAttrs, Version32 compatVers, Version32 currentVersion)
{
    uint32_t cmd = 0;
    uint32_t size = sizeForLinkedDylibCommand(path, depAttrs, cmd);
    assert(lc->cmdsize == size);

    if ( cmd ) {
        // make traditional load command
        dylib_command* dc               = (dylib_command*)lc;
        dc->cmd                         = cmd;
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
        dylib_use_command* dc     = (dylib_use_command*)lc;
        dc->cmd                   = cmd;
        dc->nameoff               = sizeof(dylib_use_command);
        dc->current_version       = currentVersion.value();
        dc->compat_version        = 0x00010000; // unused, but looks like 1.0 to old tools
        dc->marker                = 0x1a741800; // magic value that means dylib_use_command
        dc->flags                 = depAttrs.raw;
        strcpy((char*)dc + dc->nameoff, path);
    }
}

void HeaderWriter::addLibSystem()
{
    addLinkedDylib("/usr/lib/libSystem.B.dylib");
}

void HeaderWriter::addDylibId(CString name, Version32 compatVers, Version32 currentVersion)
{
    uint32_t       alignedSize          = pointerAligned((uint32_t)(sizeof(dylib_command) + name.size() + 1));
    dylib_command* dc                   = (dylib_command*)appendLoadCommand(LC_ID_DYLIB, alignedSize);
    dc->dylib.name.offset               = sizeof(dylib_command);
    dc->dylib.timestamp                 = 1; // needs to be some constant value that is different than linked dylib
    dc->dylib.current_version           = currentVersion.value();
    dc->dylib.compatibility_version     = compatVers.value();
    strcpy((char*)dc + dc->dylib.name.offset, name.c_str());
}

void HeaderWriter::addDyldID()
{
    const char* path = "/usr/lib/dyld";
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc       = (dylinker_command*)appendLoadCommand(LC_ID_DYLINKER, alignedSize);
    dc->name.offset = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void HeaderWriter::addDynamicLinker()
{
    const char* path = "/usr/lib/dyld";
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc       = (dylinker_command*)appendLoadCommand(LC_LOAD_DYLINKER, alignedSize);
    dc->name.offset = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void HeaderWriter::addFairPlayEncrypted(uint32_t offset, uint32_t size)
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

void HeaderWriter::addRPath(const char* path)
{
    uint32_t       alignedSize = pointerAligned((uint32_t)(sizeof(rpath_command) + strlen(path) + 1));
    rpath_command* rc          = (rpath_command*)appendLoadCommand(LC_RPATH, alignedSize);
    rc->path.offset            = sizeof(rpath_command);
    strcpy((char*)rc + rc->path.offset, path);
}

void HeaderWriter::setTargetTriple(const char* triple)
{
    uint32_t               alignedSize = pointerAligned((uint32_t)(sizeof(target_triple_command) + strlen(triple) + 1));
    target_triple_command* rc          = (target_triple_command*)appendLoadCommand(LC_TARGET_TRIPLE, alignedSize);
    rc->triple.offset                  = sizeof(target_triple_command);
    strcpy((char*)rc + rc->triple.offset, triple);
}

void HeaderWriter::addDyldEnvVar(const char* path)
{
    uint32_t          alignedSize = pointerAligned((uint32_t)(sizeof(dylinker_command) + strlen(path) + 1));
    dylinker_command* dc          = (dylinker_command*)appendLoadCommand(LC_DYLD_ENVIRONMENT, alignedSize);
    dc->name.offset               = sizeof(dylinker_command);
    strcpy((char*)dc + dc->name.offset, path);
}

void HeaderWriter::addAllowableClient(const char* clientName)
{
    uint32_t            alignedSize = pointerAligned((uint32_t)(sizeof(sub_client_command) + strlen(clientName) + 1));
    sub_client_command* ac          = (sub_client_command*)appendLoadCommand(LC_SUB_CLIENT, alignedSize);
    ac->client.offset               = sizeof(sub_client_command);
    strcpy((char*)ac + ac->client.offset, clientName);
}

void HeaderWriter::addUmbrellaName(const char* umbrellaName)
{
    uint32_t            alignedSize = pointerAligned((uint32_t)(sizeof(sub_framework_command) + strlen(umbrellaName) + 1));
    sub_framework_command* ac       = (sub_framework_command*)appendLoadCommand(LC_SUB_FRAMEWORK, alignedSize);
    ac->umbrella.offset             = sizeof(sub_framework_command);
    strcpy((char*)ac + ac->umbrella.offset, umbrellaName);
}

void HeaderWriter::addSourceVersion(Version64 vers)
{
    source_version_command svc;
    svc.cmd       = LC_SOURCE_VERSION;
    svc.cmdsize   = sizeof(source_version_command);
    svc.version   = vers.value();
    appendLoadCommand((load_command*)&svc);
}

void HeaderWriter::setMain(uint32_t offset)
{
    entry_point_command ec;
    ec.cmd       = LC_MAIN;
    ec.cmdsize   = sizeof(entry_point_command);
    ec.entryoff  = offset;
    ec.stacksize = 0;
    appendLoadCommand((load_command*)&ec);
}

void HeaderWriter::setCustomStackSize(uint64_t stackSize) {
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

void HeaderWriter::setUnixEntry(uint64_t startAddr)
{
    // FIXME: support other archs
    if ( (mh.cputype == CPU_TYPE_ARM64) || (mh.cputype == CPU_TYPE_ARM64_32) ) {
        uint32_t   lcSize = threadLoadCommandsSize();
        uint32_t*  words     = (uint32_t*)appendLoadCommand(LC_UNIXTHREAD, lcSize);
        words[2] = 6;   // flavor = ARM_THREAD_STATE64
        words[3] = 68;  // count  = ARM_EXCEPTION_STATE64_COUNT * 2 <=> 34 uint64_t's
        bzero(&words[4], lcSize-16);
        memcpy(&words[68], &startAddr, 8);  // register pc = startAddr
    }
    else if ( mh.cputype == CPU_TYPE_X86_64 ) {
        uint32_t   lcSize = threadLoadCommandsSize();
        uint32_t*  words     = (uint32_t*)appendLoadCommand(LC_UNIXTHREAD, lcSize);
        words[2] = 4;   // flavor = x86_THREAD_STATE64
        words[3] = 42;  // count  = x86_THREAD_STATE64_COUNT
        bzero(&words[4], lcSize-16);
        memcpy(&words[36], &startAddr, 8);  // register pc = startAddr
    }
    else if ( mh.cputype == CPU_TYPE_ARM ) {
        uint32_t   lcSize = threadLoadCommandsSize();
        uint32_t*  words     = (uint32_t*)appendLoadCommand(LC_UNIXTHREAD, lcSize);
        words[2] = 1;   // flavor = ARM_THREAD_STATE
        words[3] = 17;  // count  = ARM_THREAD_STATE_COUNT
        bzero(&words[4], lcSize-16);
        words[15] = (uint32_t)startAddr;  // register pc = startAddr
    }
    else {
        assert(0 && "arch not supported");
    }
}

void HeaderWriter::addCodeSignature(uint32_t fileOffset, uint32_t fileSize)
{
    linkedit_data_command lc;
    lc.cmd       = LC_CODE_SIGNATURE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = fileOffset;
    lc.datasize  = fileSize;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setBindOpcodesInfo(uint32_t rebaseOffset, uint32_t rebaseSize,
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

void HeaderWriter::setChainedFixupsInfo(uint32_t cfOffset, uint32_t cfSize)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DYLD_CHAINED_FIXUPS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = cfOffset;
    lc.datasize  = cfSize;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setExportTrieInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DYLD_EXPORTS_TRIE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setFunctionVariants(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_FUNCTION_VARIANTS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setFunctionVariantFixups(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_FUNCTION_VARIANT_FIXUPS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setSplitSegInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_SEGMENT_SPLIT_INFO;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setDataInCode(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_DATA_IN_CODE;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setFunctionStarts(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_FUNCTION_STARTS;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setAtomInfo(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_ATOM_INFO;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setLinkerOptimizationHints(uint32_t offset, uint32_t size)
{
    linkedit_data_command lc;
    lc.cmd       = LC_LINKER_OPTIMIZATION_HINT;
    lc.cmdsize   = sizeof(linkedit_data_command);
    lc.dataoff   = offset;
    lc.datasize  = size;
    appendLoadCommand((load_command*)&lc);
}

void HeaderWriter::setSymbolTable(uint32_t nlistOffset, uint32_t nlistCount, uint32_t stringPoolOffset, uint32_t stringPoolSize,
                                  uint32_t localsCount, uint32_t globalsCount, uint32_t undefCount, uint32_t indOffset, uint32_t indCount, bool dynSymtab)
{
    symtab_command stc;
    stc.cmd       = LC_SYMTAB;
    stc.cmdsize   = sizeof(symtab_command);
    stc.symoff    = nlistOffset;
    stc.nsyms     = nlistCount;
    stc.stroff    = stringPoolOffset;
    stc.strsize   = stringPoolSize;
    appendLoadCommand((load_command*)&stc);

    if ( dynSymtab ) {
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
}

void HeaderWriter::addLinkerOption(std::span<uint8_t> buffer, uint32_t count)
{
    uint32_t cmdSize = pointerAligned(sizeof(linker_option_command) + (uint32_t)buffer.size());

    linker_option_command* lc = (linker_option_command*)appendLoadCommand(LC_LINKER_OPTION, cmdSize);
    lc->cmd     = LC_LINKER_OPTION;
    lc->cmdsize = cmdSize;
    lc->count   = count;
    memcpy((uint8_t*)(lc + 1), buffer.data(), buffer.size());
}

HeaderWriter::LinkerOption HeaderWriter::LinkerOption::make(std::span<CString> opts)
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

load_command* HeaderWriter::findLoadCommand(uint32_t cmdNum)
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

void HeaderWriter::removeLoadCommand(void (^callback)(const load_command* cmd, bool& remove, bool& stop))
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

uint32_t HeaderWriter::relocatableHeaderAndLoadCommandsSize(bool is64, uint32_t sectionCount, uint32_t platformsCount, std::span<const LinkerOption> linkerOptions)
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

    for ( LinkerOption opt : linkerOptions ) {
        size += opt.lcSize();
    }
    return size;
}

void HeaderWriter::setRelocatableSectionCount(uint32_t sectionCount)
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
        sc->maxprot            = 7;
        sc->initprot           = 7;
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
        sc->maxprot            = 7;
        sc->initprot           = 7;
        sc->nsects             = sectionCount;
        // section info to be filled in later by setRelocatableSectionInfo()
        bzero((uint8_t*)sc + sizeof(segment_command), sectionCount * sizeof(struct section));
    }
}

void HeaderWriter::updateRelocatableSegmentSize(uint64_t vmSize, uint32_t fileSize)
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


void HeaderWriter::setRelocatableSectionInfo(uint32_t sectionIndex, const char* segName, const char* sectName,
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


} // namespace dyld3
