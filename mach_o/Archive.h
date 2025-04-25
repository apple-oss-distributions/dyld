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

#ifndef mach_o_Archive_h
#define mach_o_Archive_h

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

// Darwin
#include <ar.h>

// stl
#include <string_view>
#include <optional>

// mach_o
#include "Header.h"

namespace mach_o {

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

// if a member file in a static library has this name, then force load it
#define ALWAYS_LOAD_MEMBER_NAME "__ALWAYS_LOAD.o"

/*!
 * @class Archive
 *
 * @abstract
 *      Abstraction for static archives
 */
class VIS_HIDDEN Archive
{
public:

    struct Member
    {
        std::string_view            name;
        std::span<const uint8_t>    contents;
        uint64_t                    mtime;
        unsigned                    memberIndex;
    };

    static std::optional<Archive>   isArchive(std::span<const uint8_t> buffer);

    mach_o::Error   forEachMember(void (^handler)(const Member&, bool& stop)) const;
    mach_o::Error   forEachMachO(void (^handler)(const Member&, const mach_o::Header*, bool& stop)) const;

    std::span<const uint8_t> buffer;

    constexpr static std::string_view archive_magic = "!<arch>\n";

protected:

    Archive(std::span<const uint8_t> buffer): buffer(buffer) {}
};
}

#endif // !TARGET_OS_EXCLAVEKIT

#endif // mach_o_Archive_h
