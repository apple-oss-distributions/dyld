/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

// stl
#include <string_view>

// Darwin
#include <ar.h>
#include <mach-o/ranlib.h>
#include <mach/mach.h>

// mach_o
#include "Archive.h"
#include "Universal.h"
#include "UnsafeHeader.h"


namespace mach_o {

constexpr std::string_view AR_EFMT1_SV(AR_EFMT1);

static inline uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}

bool Archive::Entry::hasLongName() const
{
    return std::string_view(ar_name, AR_EFMT1_SV.size()) == AR_EFMT1_SV;
}

uint64_t Archive::Entry::getLongNameSpace() const
{
    char* endptr;
    return strtoull(&ar_name[AR_EFMT1_SV.size()], &endptr, 10);
}

std::string_view Archive::Entry::name() const
{
    if ( hasLongName() ) {
        const char* start = ((char*)this) + sizeof(ar_hdr);
        uint64_t space = getLongNameSpace();
        // long file name may not be zero terminated in `ar`
        std::string_view nm = std::string_view(start, (size_t)space);
        while ( !nm.empty() && nm.back() == '\0' )
            nm = nm.substr(0, nm.size()-1);
        return nm;
    }
    else {
        // traditional ar_name is right-filled with ' '
        const char* start = this->ar_name;
        size_t      len   = 15;
        while ( start[len] == ' ')
            --len;
        return std::string_view(start, len+1);
    }
}

uint64_t Archive::Entry::modificationTime() const
{
    char temp[14];
    strncpy(temp, ar_date, 12);
    temp[12] = '\0';
    char* endptr;
    return strtoull(temp, &endptr, 10);
}

uint32_t Archive::Entry::uid() const
{
    char temp[8];
    strncpy(temp, ar_uid, 6);
    temp[6] = '\0';
    char* endptr;
    return (uint32_t)strtoull(temp, &endptr, 10);
}

uint32_t Archive::Entry::gid() const
{
    char temp[8];
    strncpy(temp, ar_gid, 6);
    temp[6] = '\0';
    char* endptr;
    return (uint32_t)strtoull(temp, &endptr, 10);
}

uint32_t Archive::Entry::perms() const
{
    char temp[16];
    strncpy(temp, ar_mode, 8);
    temp[8] = '\0';
    char* endptr;
    return (uint32_t)strtoull(temp, &endptr, 8);
}

Error Archive::Entry::content(std::span<const uint8_t>& content) const
{
    char temp[12];
    strncpy(temp, ar_size, 10);
    temp[10] = 0;
    char* endptr = nullptr;
    uint64_t size = strtoull(temp, &endptr, 10);
    if ( *endptr != 0 && *endptr != ' ' )
        return Error("archive member size contains non-numeric characters: '%s'", (const char*)temp);

    const uint8_t* data;
    // long name is included in ar_size
    if ( hasLongName() ) {
        uint64_t space = getLongNameSpace();
        assert(size >= space);
        size -= space;
        data = ((const uint8_t*)this) + sizeof(ar_hdr) + space;
    } else {
        data = ((const uint8_t*)this) + sizeof(ar_hdr);
    }

    content = std::span(data, (size_t)size);
    return Error::none();
}

Error Archive::Entry::next(Entry*& next) const
{
    next = nullptr;
    std::span<const uint8_t> content;
    if (Error err = this->content(content) )
        return err;

    const uint8_t* p = content.data() + content.size();
    // Note: for long name format, this is already aligned
    p = (uint8_t*)align((uint64_t)p, 1); // 2-byte align

    next = (Entry*)p;
    return Error::none();
}

Error Archive::Entry::valid() const
{
    if ( memcmp(ar_fmag, ARFMAG, sizeof(ar_fmag)) == 0 ) {
        return Error::none();
    }

    return Error("archive member invalid control bits");
}

std::optional<Archive> Archive::isArchive(std::span<const uint8_t> buffer)
{
    if ( buffer.size() >= archive_magic.size()
        && std::string_view((const char*)buffer.data(), archive_magic.size()) == archive_magic ) {
        return Archive(buffer);
    }
    return std::nullopt;
}

Error Archive::forEachMember(void (^handler)(const Member&, unsigned memberIndex, uint64_t memberFileOffset, bool& stop)) const
{
    const Entry* current = (Entry*)(buffer.data() + archive_magic.size());
    const Entry* const end = (Entry*)(buffer.data() + buffer.size());

    bool stop = false;
    unsigned memberIndex = 1;
    while ( !stop && current < end ) {
        if ( (current + 1) > end )
            return Error("malformed archive, member exceeds file size");

        if ( Error err = current->valid() )
            return err;

        std::span<const uint8_t> content;
        if ( Error err = current->content(content) )
            return err;

        Entry* next;
        if ( Error err = current->next(next) )
            return err;
        if ( next > end )
            return Error("malformed archive, member exceeds file size");

        uint64_t fileOffset = (long)current - (long)buffer.data();
        Member member;
        member.name     = current->name();
        member.contents = content;
        member.mtime    = current->modificationTime();
        member.uid      = current->uid();
        member.gid      = current->gid();
        member.perms    = current->perms();
        handler(member, memberIndex, fileOffset, stop);

        current = next;
        memberIndex++;
    }

    return Error::none();
}

Error Archive::forEachMachO(void (^handler)(const Member&, unsigned memberIndex, const mach_o::UnsafeHeader*, bool& stop), MisalignHandling misAlignedMember) const
{
    __block Error  err           = Error::none();
    __block bool   hadSymdefFile = false;
    Error iterErr = forEachMember(^(const Member& member, unsigned memberIndex, uint64_t memberFileOffset, bool& stop) {
        if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(member.contents) ) {
            bool misAligned = false;
            if ( hdr->is64() )
                misAligned = ( ((long)member.contents.data() & 7) != 0 );
            else
                misAligned = ( ((long)member.contents.data() & 3) != 0 );
            if ( misAligned ) {
                // static libs made with `ar` may have unaligned members, but `libtool` should always make aligned members
                switch ( misAlignedMember ) {
                    case MisalignHandling::error:
                        if ( hdr->is64() )
                            err = Error("64-bit mach-o member '%.*s' not 8-byte aligned", (int)member.name.size(), member.name.data());
                        else
                            err = Error("32-bit mach-o member '%.*s' not 4-byte aligned", (int)member.name.size(), member.name.data());
                        break;
                    case MisalignHandling::ignore:
                        handler(std::move(member), memberIndex, hdr, stop);
                        break;
                    case MisalignHandling::warn:
                        if ( hdr->is64() )
                            fprintf(stderr, "warning: 64-bit mach-o member '%.*s' not 8-byte aligned\n", (int)member.name.size(), member.name.data());
                        else
                            fprintf(stderr, "warning: 32-bit mach-o member '%.*s' not 4-byte aligned\n", (int)member.name.size(), member.name.data());
                        handler(std::move(member), memberIndex, hdr, stop);
                        break;
                }
            }
            else {
                handler(std::move(member), memberIndex, hdr, stop);
            }
        }
        else if ( UnsafeHeader::isBitCodeHeader(member.contents) ) {
            handler(std::move(member), memberIndex, nullptr, stop);
        }
        else if ( const Universal* uni = Universal::isUniversal(member.contents) ) {
            // Note: a fat .o file in a static library is malformed,
            // but some makefile base systems use `ar` to create the static lib from fat .o files
            // then run `ranlib` which needs to normalize the static lib
            handler(std::move(member), memberIndex, nullptr, stop);
        }
        else {
            if ( member.name == SYMDEF    || member.name == SYMDEF_SORTED ||
                 member.name == SYMDEF_64 || member.name == SYMDEF_64_SORTED ) {
                if ( hadSymdefFile ) {
                    err = Error("multiple SYMDEF member files found in an archive");
                } else {
                    hadSymdefFile = true;
                    return;
                }
            } else {
                err = Error("archive member '%.*s' not a mach-o file", (int)member.name.size(), member.name.data()) ;
            }
            stop = true;
        }
    });

    if ( iterErr )
        return iterErr;

    return std::move(err);
}

void Archive::Member::setFileInfo(const struct stat& statBuf)
{
    this->uid   = statBuf.st_uid;
    this->gid   = statBuf.st_gid;
    this->mtime = statBuf.st_mtime;
    this->perms = statBuf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
}

// zero out some fields for reproducible builds
void Archive::Member::setReproducible()
{
    this->uid   = 0;
    this->gid   = 0;
    this->mtime = 0;
}


} // namespace mach_o

#endif // !TARGET_OS_EXCLAVEKIT
