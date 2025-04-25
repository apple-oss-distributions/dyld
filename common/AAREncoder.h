/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#ifndef AAREncoder_h
#define AAREncoder_h

#include <span>
//#include <compression_private.h>

#include "Defines.h"
#include "Allocator.h"
#include "ByteStream.h"

struct VIS_HIDDEN AAREncoder {
    AAREncoder(lsl::Allocator& allocator);
    ~AAREncoder();
    void addFile(std::string_view path, std::span<std::byte> data);
    void addSymLink(std::string_view from, std::string_view to);
//    void setAlgorithm(compression_algorithm alg);
    void encode(ByteStream& output) const;
private:
    struct File {
        const char*             path;
        std::span<std::byte>    data;
    };
    struct Link {
        const char* from;
        const char* to;
    };
    uint16_t headerSize(const File& file) const;
    uint16_t headerSize(const Link& link) const;
    void encodeFile(const File& file, ByteStream& output) const;
    void encodeLink(const Link& link, ByteStream& output) const;
    lsl::Allocator*         _allocator  = nullptr;
//    compression_algorithm   _alg        = COMPRESSION_INVALID;
    lsl::Vector<File>       _files;
    lsl::Vector<Link>       _links;
};

#endif /* AAREncoder_h */
