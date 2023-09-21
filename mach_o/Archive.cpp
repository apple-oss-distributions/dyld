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

#include "Archive.h"

// stl
#include <string_view>

// Darwin
#include <ar.h>
#include <mach-o/ranlib.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

namespace mach_o {

namespace {

constexpr std::string_view AR_EFMT1_SV(AR_EFMT1);

class Entry : ar_hdr
{
public:
    void                        getName(char *, int) const;
    uint64_t                    modificationTime() const;
    Error                       content(std::span<const uint8_t>& content) const;
    Error                       next(Entry*& next) const;
    Error                       valid() const;

    static uint64_t             extendedFormatNameSize(std::string_view name);
    static uint64_t             entrySize(bool extendedFormatNames, std::string_view name, uint64_t contentSize);
    static size_t               write(std::span<uint8_t> buffer, bool extendedFormatNames, std::string_view name, uint64_t mktime, std::span<const uint8_t> content);

private:
    bool                        hasLongName() const;
    uint64_t                    getLongNameSpace() const;
};

static inline uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}

uint64_t Entry::extendedFormatNameSize(std::string_view name)
{
    // In extended format the name is stored after the member header.
    // It's always \0 terminated, it's padded to 8 bytes and also contains
    // an extra padding for the member header. This makes sure that member
    // contents are always 8 bytes aligned.
    return align(name.size() + 1, 3) + align(sizeof(Entry), 3) - sizeof(Entry);
}

uint64_t Entry::entrySize(bool extendedFormatNames, std::string_view name, uint64_t contentSize)
{
    if ( extendedFormatNames ) {
        return sizeof(Entry) + extendedFormatNameSize(name) + align(contentSize, 3);
    }
    return sizeof(Entry) + align(contentSize, 3);
}

size_t Entry::write(std::span<uint8_t> buffer, bool extendedFormatNames, std::string_view name, uint64_t mktime, std::span<const uint8_t> content)
{
    Entry* entry = (Entry*)buffer.data();

    const uint64_t alignedNameSize       = extendedFormatNames ? extendedFormatNameSize(name) : 0;
    // Content is 8-bytes aligned and padded with \n characters.
    const uint64_t alignedContentSize   = align(content.size(), 3);
    const uint64_t headerSize           = sizeof(Entry) + alignedNameSize;
    const uint64_t totalSize            = headerSize + alignedContentSize;
    assert(totalSize == entrySize(extendedFormatNames, name, content.size()));
    assert(buffer.size() >= (totalSize));
    bzero(buffer.data(), headerSize);

    snprintf(entry->ar_date, sizeof(Entry::ar_date), "%llu", mktime);
    memcpy(entry->ar_fmag, ARFMAG, sizeof(Entry::ar_fmag));

    if ( extendedFormatNames ) {
        snprintf(entry->ar_size, sizeof(Entry::ar_size), "%llu", alignedContentSize + alignedNameSize);
        snprintf(entry->ar_name, sizeof(Entry::ar_name), AR_EFMT1 "%llu", alignedNameSize);

        char* nameBuffer = (char*)(entry + 1);
        memcpy(nameBuffer, name.data(), name.size());
        nameBuffer[name.size()] = 0;
    } else {
        snprintf(entry->ar_size, sizeof(Entry::ar_size), "%llu", alignedContentSize);

        // Note that the truncated name doesn't need to be \0 terminated
        std::string_view shortName = name.substr(0, sizeof(Entry::ar_name));
        memcpy(entry->ar_name, shortName.data(), shortName.size());
    }

    uint8_t* contentStart = buffer.data() + sizeof(Entry) + alignedNameSize;
    memcpy(contentStart, content.data(), content.size());
    // Pad content alignment with \n characters
    if ( content.size() != alignedContentSize )
        memset(contentStart + content.size(), '\n', alignedContentSize - content.size());

    return totalSize;
}

bool Entry::hasLongName() const
{
    return std::string_view(ar_name, AR_EFMT1_SV.size()) == AR_EFMT1_SV;
}

uint64_t Entry::getLongNameSpace() const
{
    char* endptr;
    return strtoull(&ar_name[AR_EFMT1_SV.size()], &endptr, 10);
}

void Entry::getName(char *buf, int bufsz) const
{
  if ( hasLongName() ) {
      uint64_t len = getLongNameSpace();
      assert(bufsz >= len+1);
      strncpy(buf, ((char*)this)+sizeof(ar_hdr), len);
      buf[len] = '\0';
  } else {
      assert(bufsz >= 16+1);
      strncpy(buf, ar_name, 16);
      buf[16] = '\0';
      char* space = strchr(buf, ' ');
      if ( space != NULL )
          *space = '\0';
  }
}

uint64_t Entry::modificationTime() const
{
    char temp[14];
    strncpy(temp, ar_date, 12);
    temp[12] = '\0';
    char* endptr;
    return strtoull(temp, &endptr, 10);
}

Error Entry::content(std::span<const uint8_t>& content) const
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

    content = std::span(data, size);
    return Error::none();
}

Error Entry::next(Entry*& next) const
{
    next = nullptr;
    std::span<const uint8_t> content;
    if (Error err = this->content(content) )
        return err;

    const uint8_t* p = content.data() + content.size();
    p = (uint8_t*)align((uint64_t)p, 2); // 4-byte align
    next = (Entry*)p;
    return Error::none();
}

Error Entry::valid() const
{
    if ( memcmp(ar_fmag, ARFMAG, sizeof(ar_fmag)) == 0 ) {
        return Error::none();
    }

    return Error("archive member invalid control bits");
}

constexpr static std::string_view archive_magic = "!<arch>\n";
}

std::optional<Archive> Archive::isArchive(std::span<const uint8_t> buffer)
{
    if ( buffer.size() >= archive_magic.size()
        && std::string_view((const char*)buffer.data(), archive_magic.size()) == archive_magic ) {
        return Archive(buffer);
    }
    return std::nullopt;
}

Error Archive::forEachMember(void (^handler)(const Member&, bool& stop)) const
{
    const Entry* current = (Entry*)(buffer.data() + archive_magic.size());
    const Entry* const end = (Entry*)(buffer.data() + buffer.size());

    std::array<char, 256> nameBuffer;
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

        current->getName(nameBuffer.data(), nameBuffer.size());
        handler(Member{ nameBuffer.data(), content, current->modificationTime(), memberIndex }, stop);
        current = next;
        memberIndex++;
    }

    return Error::none();
}

static bool isBitCodeHeader(std::span<const uint8_t> contents)
{
    return (contents[0] == 0xDE) && (contents[1] == 0xC0) && (contents[2] == 0x17) && (contents[3] == 0x0B);
}

Error Archive::forEachMachO(void (^handler)(const Member&, const mach_o::Header*, bool& stop)) const
{
    __block Error err = Error::none();
    __block bool hadSymdefFile = false;

    Error iterErr = forEachMember(^(const Member& member, bool &stop) {
        if ( const Header* header = Header::isMachO(member.contents) ) {
            handler(std::move(member), header, stop);
        }
        else if ( isBitCodeHeader(member.contents) ) {
            handler(std::move(member), nullptr, stop);
        }
        else {
            if ( member.name == SYMDEF || member.name == SYMDEF_SORTED ||
                    member.name == SYMDEF_64 || member.name == SYMDEF_64_SORTED ) {
                if ( hadSymdefFile ) {
                    err = Error("multiple SYMDEF member files found in an archive");
                } else {
                    hadSymdefFile = true;
                    return;
                }
            } else {
                err = Error("archive member '%s' not a mach-o file", member.name.data()) ;
            }
            stop = true;
        }
    });

    if ( iterErr )
        return iterErr;

    return std::move(err);
}

#if BUILDING_MACHO_WRITER
size_t Archive::size(std::span<const Member> members, bool extendedFormatNames)
{
    uint64_t size = archive_magic.size();

    for ( const Member& m : members ) {
        size += Entry::entrySize(extendedFormatNames, m.name, m.contents.size());
    }

    return size;
}

Error Archive::make(std::span<uint8_t> buffer, std::span<const Member> members, bool extendedFormatNames)
{
    if ( buffer.size() < archive_magic.size() ) {
        return Error("buffer to small");
    }

    std::span<uint8_t> remainingSpace = buffer;
    memcpy(remainingSpace.data(), archive_magic.data(), archive_magic.size());
    remainingSpace = remainingSpace.subspan(archive_magic.size());

    for ( const Member& m : members ) {
        size_t writtenBytes = Entry::write(remainingSpace, extendedFormatNames, m.name, m.mtime, m.contents);
        if ( writtenBytes > remainingSpace.size() ) {
            assert(false && "invalid buffer size");
            return Error("buffer to small");
        }

        remainingSpace = remainingSpace.subspan(writtenBytes);
    }

    assert(remainingSpace.empty());
    if ( isArchive(buffer).has_value() )
        return Error::none();
    return Error("error writing archive");
}
#endif
}
