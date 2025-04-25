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

// Darwin
#include <ar.h>
#include <mach-o/ranlib.h>
#include <mach/mach.h>
#include <mach/vm_map.h>

namespace mach_o {

size_t ArchiveWriter::size(std::span<const Member> members, bool extendedFormatNames)
{
    uint64_t size = archive_magic.size();

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

} // namespace mach_o
