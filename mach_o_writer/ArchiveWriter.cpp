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

// mach_o_writer
#include "ArchiveWriter.h"

// stl
#include <string_view>
#include <unordered_set>

// Darwin
#include <ar.h>
#include <mach-o/ranlib.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

namespace mach_o {


static inline constexpr uint64_t align(uint64_t addr, uint8_t p2)
{
    uint64_t mask = (1 << p2);
    return (addr + mask - 1) & (-mask);
}

uint64_t ArchiveWriter::Entry::extendedFormatNameSize(std::string_view name)
{
    // In extended format the name is stored after the member header.
    // It's always \0 terminated, it's padded to 8 bytes and also contains
    // an extra padding for the member header. This makes sure that member
    // contents are always 8 bytes aligned.
    // Note: this is not optimal padding (which would be `align(sizeof(Entry) + name.size() + 1, 3) - sizeof(Entry)`)
    // but matches what cctools libtool did to make output bit identical
    return align(name.size(), 3) + align(sizeof(Entry), 3) - sizeof(Entry);
}

uint64_t ArchiveWriter::Entry::entrySize(bool extendedFormatNames, std::string_view name, uint64_t contentSize)
{
    if ( extendedFormatNames ) {
        return sizeof(Entry) + extendedFormatNameSize(name) + align(contentSize, 3);
    }
    return sizeof(Entry) + align(contentSize, 1);
}


size_t ArchiveWriter::Entry::write(std::span<uint8_t> buffer, bool extendedFormatNames, std::string_view name,
                                   std::span<const uint8_t> content, uint64_t mtime,
                                   uint32_t uid, uint32_t gid, uint32_t perms)
{
    Entry* entry = (Entry*)buffer.data();

    const uint64_t alignedNameSize      = extendedFormatNames ? extendedFormatNameSize(name) : 0;
    // In traditional format, filenames limited to 16 bytes, and content size is rounded up to be 2-byte aligned
    // In extended format, filename length is unlimited, and content size 8-bytes aligned and padded with \n characters.
    const uint64_t alignedContentSize   = align(content.size(), (extendedFormatNames ? 3 : 1));
    const uint64_t headerSize           = sizeof(Entry) + alignedNameSize;
    const uint64_t totalSize            = headerSize + alignedContentSize;
    const uint64_t listedSize           = extendedFormatNames ? (alignedContentSize + alignedNameSize) : alignedContentSize;
    assert(totalSize == entrySize(extendedFormatNames, name, content.size()));
    assert(buffer.size() >= (totalSize));

    // fields do not have '\0' terminator, just ' ' between fields
    // rdar://158740791 (libtool-prime produces malformed static archives for large GIDs)
    // note: each field is explicitly limited in length, but we need +1
    // for snprintf to be able to print the redundant \0 terminator
    snprintf(entry->ar_date, sizeof(Entry) - offsetof(Entry, ar_date) + 1,
       "%-*ld%-*u%-*u%-*o%-*ld%-*s",
       (int)sizeof(entry->ar_date), (long int)mtime,
       (int)sizeof(entry->ar_uid),  (unsigned short)uid,
       (int)sizeof(entry->ar_gid),  (unsigned short)gid,
       (int)sizeof(entry->ar_mode), (unsigned int)perms,
       (int)sizeof(entry->ar_size), (long)listedSize,
       (int)sizeof(entry->ar_fmag), ARFMAG);

    if ( extendedFormatNames ) {
        // put "1/N" into fixed size name field and store full name after ar_hdr and before file content
        snprintf(entry->ar_name, sizeof(entry->ar_name), "%s%-*llu",
                 AR_EFMT1, (int)(sizeof(entry->ar_name) - strlen(AR_EFMT1)), alignedNameSize);
        entry->ar_name[sizeof(entry->ar_name)-1] = ' '; // snprintf zero terminates, but we don't want that
        char* nameBuffer = (char*)(entry + 1);
        memcpy(nameBuffer, name.data(), name.size());
        nameBuffer[name.size()] = 0;
    } else {
        // Note that the truncated name doesn't need to be \0 terminated
        std::string_view shortName = name.substr(0, sizeof(Entry::ar_name));
        memcpy(entry->ar_name, shortName.data(), shortName.size());
        memset(&entry->ar_name[shortName.size()], ' ', sizeof(Entry::ar_name)-shortName.size());
    }

    uint8_t* contentStart = buffer.data() + sizeof(Entry) + alignedNameSize;
    memcpy(contentStart, content.data(), content.size());
    // Pad content alignment with \n characters
    if ( content.size() != alignedContentSize )
        memset(contentStart + content.size(), '\n', (size_t)alignedContentSize - content.size());

    return (size_t)totalSize;
}


size_t ArchiveWriter::size(std::span<const Member> members, bool extendedFormatNames)
{
    size_t size = archive_magic.size();

    for ( const Member& m : members ) {
        size += Entry::entrySize(extendedFormatNames, m.name, m.contents.size());
    }

    return size;
}

Error ArchiveWriter::make(std::span<uint8_t> buffer, std::span<const Member> members, bool extendedFormatNames)
{
    if ( buffer.size() < archive_magic.size() ) {
        return Error("buffer to small");
    }

    std::span<uint8_t> remainingSpace = buffer;
    memcpy(remainingSpace.data(), archive_magic.data(), archive_magic.size());
    remainingSpace = remainingSpace.subspan(archive_magic.size());

    for ( const Member& m : members ) {
        size_t writtenBytes = Entry::write(remainingSpace, extendedFormatNames, m.name, m.contents,
                                           m.mtime, m.uid, m.gid, m.perms);
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

size_t ArchiveWriter::ToC::size(std::span<const NameAndValue> symbolNamesAndMemberIndexes, bool toc64)
{
    size_t offsetSize = toc64 ? 8 : 4;
    // SYMDEF ranlib array
    size_t size = offsetSize + symbolNamesAndMemberIndexes.size()*(toc64 ? sizeof(ranlib_64) : sizeof(ranlib));

    // SYMDEF strings
    size += offsetSize;
    for (const NameAndValue& entry : symbolNamesAndMemberIndexes)
        size += (entry.name.size() + 1);

    // 8-byte align the size
    size = (size+7) & (-8);

    return size;
}

// structure of toc file is:
//  32[64]bit byte size of ranlib[_64] array
//  ranlib[_64] array
//  32[64]-bit byte size of string pool
//  string pool
template <typename R, typename U>
static Error makeRanLibArray(std::span<const ArchiveWriter::ToC::NameAndValue> symbolNamesAndMemberIndexes,
                             bool preferSorted, std::span<uint8_t> result, CString& filename)
{
    // make ranlib/ranlib_64 array
    size_t arraySize = symbolNamesAndMemberIndexes.size()*sizeof(R);
    if ( result.size() < (sizeof(U)+arraySize) )
        return Error("toc buffer size too small");
    U*      head    = (U*)result.data();
    R*      entries = (R*)(result.data() + sizeof(U));
    U*      strHead = (U*)(result.data() + sizeof(U) + arraySize);
    char*   strings = (char*)(result.data() + 2*sizeof(U) + arraySize);
    U       offset  = 0;
    *head = (U)arraySize;
    std::unordered_set<CString> foundSymbols;
    bool                        foundDuplicateSymbols = false;
    for (const ArchiveWriter::ToC::NameAndValue& entry : symbolNamesAndMemberIndexes) {
        if ( foundSymbols.count(entry.name) > 0 )
            foundDuplicateSymbols = true;
        else
            foundSymbols.insert(entry.name);
        if ( (sizeof(U)+arraySize+offset+entry.name.size()+1) > result.size() )
            return Error("toc string pool too small");
        strcpy(strings+offset, entry.name.c_str());
        entries->ran_un.ran_strx = offset;
        entries->ran_off         = (U)entry.value;  // ran_off is index at this point, and change to offset later
        offset += (entry.name.size()+1);
        ++entries;
    }
    *strHead = (offset+7) & (-8);
    // cannot used sorted format if there are duplicates
    if ( preferSorted && !foundDuplicateSymbols ) {
        R* ranlibStart = (R*)(result.data() + sizeof(U));
        qsort_b(ranlibStart, symbolNamesAndMemberIndexes.size(), sizeof(R), ^(const void* l, const void* r) {
            const char* lname = strings + ((R*)l)->ran_un.ran_strx;
            const char* rname = strings + ((R*)r)->ran_un.ran_strx;
            return strcmp(lname, rname);
        });
        filename = ((sizeof(U) == 8) ? SYMDEF_64_SORTED : SYMDEF_SORTED);
    }
    else {
        filename = ((sizeof(U) == 8) ? SYMDEF_64 : SYMDEF);
    }
    return Error::none();
}

Error ArchiveWriter::ToC::make(std::span<const NameAndValue> symbolNamesAndMemberIndexes, bool preferSorted,
                               bool toc64, std::span<uint8_t> result, CString& filename)
{
    if ( toc64 ) {
        // make ranlib_64 array file
        return makeRanLibArray<ranlib_64, uint64_t>(symbolNamesAndMemberIndexes, preferSorted, result, filename);
    }
    else {
        // make ranlib array file
        return makeRanLibArray<ranlib, uint32_t>(symbolNamesAndMemberIndexes, preferSorted, result, filename);
    }
    return Error::none();
}

template <typename R, typename U>
static Error forEachSymbolTemplate(std::span<const uint8_t> toc, void (^handler)(CString symbol, R*) )
{
    U*   head     = (U*)toc.data();
    U    count    = (*head)/sizeof(R);
    R*   entries  = (R*)(toc.data() + sizeof(U));
    U*   strHead  = (U*)(toc.data() + sizeof(U) + *head);
    U    strSize  = *strHead;
    const char* strings  = (char*)(toc.data() + 2*sizeof(U) + *head);
    for (U i=0; i < count; ++i) {
        U strOff  = entries[i].ran_un.ran_strx;
        if ( strOff > strSize )
            return Error("toc error, entry %lu, string offset 0x%08lX out of range", (long)i, (long)strOff);
        const char* sym  = strings+strOff;
        handler(sym, &entries[i]);
    }
    return Error::none();
}

Error ArchiveWriter::ToC::forEachSymbol(std::span<const uint8_t> toc, void (^handler)(CString symbol, ranlib*))
{
    return forEachSymbolTemplate<ranlib,uint32_t>(toc, handler);
}

Error ArchiveWriter::ToC::forEachSymbol64(std::span<const uint8_t> toc, void (^handler)(CString symbol, ranlib_64*))
{
    return forEachSymbolTemplate<ranlib_64,uint64_t>(toc, handler);
}

Error ArchiveWriter::ToC::update(std::string_view tocFileName, std::span<const uint64_t> memberIndexToOffsets, std::span<uint8_t> toc)
{
    if ( (tocFileName == SYMDEF) || (tocFileName == SYMDEF_SORTED) ) {
        __block Error symNotFoundErr;
        Error iterErr = forEachSymbol(toc, ^(CString symbolName, ranlib* entry) {
            assert(entry->ran_off < memberIndexToOffsets.size());
            uint64_t memberOffset = memberIndexToOffsets[entry->ran_off];
            assert(memberOffset < 0xFFFFFFFF);
            entry->ran_off = (uint32_t)memberOffset;
        });
        if ( symNotFoundErr.hasError() )
            return std::move(symNotFoundErr);
        return iterErr;
    }
    else if ( (tocFileName == SYMDEF_64) || (tocFileName == SYMDEF_64_SORTED) ) {
        __block Error symNotFoundErr;
        Error iterErr = forEachSymbol64(toc, ^(CString symbolName,  ranlib_64* entry) {
            entry->ran_off = memberIndexToOffsets[(size_t)entry->ran_off];
        });
        if ( symNotFoundErr.hasError() )
            return std::move(symNotFoundErr);
        return iterErr;
    }

    return Error("unknown static library table-of-contents");
}


} // namespace mach_o
