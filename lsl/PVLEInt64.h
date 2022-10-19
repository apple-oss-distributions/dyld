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

#ifndef PVLEInt64_h
#define PVLEInt64_h

#include <cstdint>

#include "Vector.h"

namespace lsl {
VIS_HIDDEN void emitPVLEUInt64(uint64_t value, Vector<std::byte>& data);
VIS_HIDDEN uint64_t readPVLEUInt64(std::span<std::byte>& data);
VIS_HIDDEN void emitPVLEInt64(int64_t value, Vector<std::byte>& data);
VIS_HIDDEN int64_t readPVLEInt64(std::span<std::byte>& data);
};


#endif /* PVLEInt64_h */
