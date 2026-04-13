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
#include <sys/stat.h>

// stl
#include <string_view>
#include <optional>

// mach_o
#include "UnsafeHeader.h"

namespace mach_o {


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
        uint64_t                    mtime     = 0;
        uint32_t                    uid       = 0;
        uint32_t                    gid       = 0;
        uint32_t                    perms     = 0;

        void            setFileInfo(const struct stat& statBuf);
        void            setReproducible(); // zero out some fields
      };

    static std::optional<Archive>   isArchive(std::span<const uint8_t> buffer);

    mach_o::Error   forEachMember(void (^handler)(const Member& member, unsigned memberIndex, uint64_t memberFileOffset, bool& stop)) const;
    enum class MisalignHandling { warn, error, ignore };
    mach_o::Error   forEachMachO( void (^handler)(const Member& member, unsigned memberIndex, const mach_o::UnsafeHeader*, bool& stop),
                                  MisalignHandling misAligned=MisalignHandling::error) const;

    std::span<const uint8_t> buffer;

    constexpr static std::string_view archive_magic = "!<arch>\n";

protected:
    class Entry : public ar_hdr
    {
    public:
        std::string_view            name() const;
        uint64_t                    modificationTime() const;
        uint32_t                    uid() const;
        uint32_t                    gid() const;
        uint32_t                    perms() const;
        Error                       content(std::span<const uint8_t>& content) const;
        Error                       next(Entry*& next) const;
        Error                       valid() const;

    private:
        bool                        hasLongName() const;
        uint64_t                    getLongNameSpace() const;
    };

    Archive(std::span<const uint8_t> buffer): buffer(buffer) {}
};

} // namespace mach_o

#endif // !TARGET_OS_EXCLAVEKIT

#endif // mach_o_Archive_h
