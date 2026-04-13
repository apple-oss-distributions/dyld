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

#ifndef mach_o_writer_Archive_h
#define mach_o_writer_Archive_h

// stl
#include <string_view>
#include <optional>
#include <vector>

// mach_o
#include "Archive.h"
#include "UnsafeHeader.h"

struct ranlib;
struct ranlib_64;


namespace mach_o {

/*!
 * @class ArchiveWriter
 *
 * @abstract
 *      Abstraction for building static archives
 */
class VIS_HIDDEN ArchiveWriter : public Archive
{
public:
    class VIS_HIDDEN Entry : Archive::Entry
    {
    public:
        static uint64_t      entrySize(bool extendedFormatNames, std::string_view name, uint64_t contentSize);
        static size_t        write(std::span<uint8_t> buffer, bool extendedFormatNames, std::string_view name,
                                   std::span<const uint8_t> content, uint64_t mtime,
                                   uint32_t uid, uint32_t gid, uint32_t perms);
    private:
        static uint64_t      extendedFormatNameSize(std::string_view name);
    };

    static size_t   size(std::span<const Member> members, bool extendedFormatNames = true);
    static Error    make(std::span<uint8_t> buffer, std::span<const Member> members, bool extendedFormatNames = true);

    // magic file added to archive that contains a table mapping symbol names to .o files in the archive
    struct ToC
    {
        struct NameAndValue { CString name; uint64_t value; };
        static size_t   size(std::span<const NameAndValue> symbolNamesAndMemberIndexes, bool toc64);
        static Error    make(std::span<const NameAndValue> symbolNamesAndMemberIndexes, bool preferSorted, bool toc64, std::span<uint8_t> result, CString& filename);
        static Error    update(std::string_view tocFileName, std::span<const uint64_t> memberIndexToOffsets, std::span<uint8_t> toc);
        static Error    forEachSymbol(  std::span<const uint8_t> toc, void (^handler)(CString symbol, ranlib*));
        static Error    forEachSymbol64(std::span<const uint8_t> toc, void (^handler)(CString symbol, ranlib_64*));
   };
};

} // namespace mach_o

#endif // mach_o_writer_Archive_h
