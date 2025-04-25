/*
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

#ifndef mach_o_writer_RebaseOpcodes_h
#define mach_o_writer_RebaseOpcodes_h

#include <stdint.h>
#include <stdio.h>

#include <span>
#include <vector>

#include "Error.h"
#include "Header.h"
#include "Fixups.h"

// mach_o
#include "RebaseOpcodes.h"

namespace mach_o {
struct MappedSegment;
}

namespace mach_o {

using namespace mach_o;

/*!
 * @class RebaseOpcodes
 *
 * @abstract
 *      Class to encapsulate building rebase opcodes
 */
class VIS_HIDDEN RebaseOpcodesWriter : public RebaseOpcodes
{
public:
                    // used by unit tests to build opcodes
                    struct Location { uint32_t segIndex; uint64_t segOffset; auto operator<=>(const Location&) const = default; };
                    RebaseOpcodesWriter(std::span<const Location> sortedLocs, bool is64);

private:
    std::vector<uint8_t> _opcodes;
    void                 append_uleb128(uint64_t value);
    void                 append_byte(uint8_t value);
 };



} // namespace mach_o

#endif // mach_o_writer_RebaseOpcodes_h


