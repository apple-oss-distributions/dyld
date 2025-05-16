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

#include "Cksum.h"
#include "AAREncoder.h"

using lsl::Allocator;

#define PARALLEL_COMPRESS_BLOCK_SIZE (0x4000000)

// Figure out how large of integer is needed to store the value
// Figure out how large of integer is needed to store the value
static uint8_t byteSizeForValue(size_t value) {
    // Check to see if value fits by generating an inverse mask of 2^result-1 and see if any bits leak
    for(uint8_t i = 1; i < 8; i<<=1) {
        if ((value & ~((1ULL<<(i*8))-1)) == 0) {
            return i;
        }
    }
    return 8;
}

uint16_t AAREncoder::headerSize(const File& file) const {
    size_t headerSize = 0;
    return headerSize;
}

void AAREncoder::encodeFile(const File& file, ByteStream& output) const {
    uint32_t checksum = cksum(file.data);
    output.insert(output.end(), file.data.begin(), file.data.end());
}

uint16_t AAREncoder::headerSize(const Link& link) const {
    size_t headerSize = 0;
    return headerSize;
}

void AAREncoder::encodeLink(const Link& link, ByteStream& output) const {
}

AAREncoder::AAREncoder(lsl::Allocator& allocator) : _allocator(&allocator), _files(allocator), _links(allocator) {}
AAREncoder::~AAREncoder() {
    for (auto& file : _files) {
        _allocator->free((void*)file.path);
    }
    for (auto& link : _links) {
        _allocator->free((void*)link.to);
        _allocator->free((void*)link.from);
    }
}
void AAREncoder::addFile(std::string_view path, std::span<std::byte> data) {
    char* pathStr = (char*)_allocator->malloc(path.size()+1);
    memcpy(pathStr, path.data(), path.size());
    pathStr[path.size()] = 0;
    _files.push_back({pathStr, data});
}
void AAREncoder::addSymLink(std::string_view from, std::string_view to) {
    char* fromStr   = (char*)_allocator->malloc(from.size()+1);
    char* toStr     = (char*)_allocator->malloc(to.size()+1);
    memcpy(fromStr, from.data(), from.size());
    fromStr[from.size()] = 0;
    memcpy(toStr, to.data(), to.size());
    toStr[to.size()] = 0;
    _links.push_back({ fromStr, toStr });
}

void AAREncoder::encode(ByteStream& output) const {
    ByteStream fileStream = ByteStream(*_allocator);

    for (auto link : _links) {
        encodeLink(link, fileStream);
    }
    for (auto file : _files) {
        encodeFile(file, fileStream);
    }
    output.insert(output.end(), fileStream.begin(), fileStream.end());
}
