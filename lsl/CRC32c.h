/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#ifndef CRC32c_h
#define CRC32c_h

#include <span>
#include <cstdint>

#include "Defines.h"

namespace lsl {
struct CRC32cImpl;
struct VIS_HIDDEN CRC32c {
    CRC32c();
    CRC32c(CRC32cImpl&);
    operator uint32_t();
    void operator()(uint8_t);
    void operator()(uint16_t);
    void operator()(uint32_t);
    void operator()(uint64_t);
    void operator()(const std::span<std::byte>);
    void reset();
    static CRC32c softwareChecksumer();
    static CRC32c hardwareChecksumer();
private:
    CRC32cImpl& _impl;
    uint32_t    _crc = 0xffffffff;
};
};

#endif /* CRC32c_h */
