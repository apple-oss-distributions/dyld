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
#include "Header.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class ArchiveWriter
 *
 * @abstract
 *      Abstraction for building static archives
 */
class VIS_HIDDEN ArchiveWriter : public Archive
{
public:
    // for building
    static size_t   size(std::span<const Member> members, bool extendedFormatNames = true);
    static Error    make(std::span<uint8_t> buffer, std::span<const Member> members, bool extendedFormatNames = true);

private:

    ArchiveWriter(std::span<const uint8_t> buffer) : Archive(buffer) {}
};
}

#endif // mach_o_writer_Archive_h
