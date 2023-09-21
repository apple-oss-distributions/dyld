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

#ifndef mach_o_RebaseOpcodes_h
#define mach_o_RebaseOpcodes_h

#include <stdint.h>
#include <stdio.h>

#include <span>
#if BUILDING_MACHO_WRITER
  #include <vector>
#endif

#include "Error.h"
#include "Header.h"
#include "Fixups.h"


namespace mach_o {

struct MappedSegment;

/*!
 * @class RebaseOpcodes
 *
 * @abstract
 *      Class to encapsulate accessing and building rebase opcodes
 */
class VIS_HIDDEN RebaseOpcodes
{
public:
                    // encapsulates rebase opcodes from a final linked image
                    RebaseOpcodes(const uint8_t* start, size_t size, bool is64);
#if BUILDING_MACHO_WRITER
                    // used by unit tests to build opcodes
                    struct Location { uint32_t segIndex; uint64_t segOffset; auto operator<=>(const Location&) const = default; };
                    RebaseOpcodes(std::span<const Location> sortedLocs, bool is64);
#endif

    Error           valid(std::span<const MappedSegment> segments, bool allowTextFixups=false, bool onlyFixupsInWritableSegments=true) const;
    void            forEachRebaseLocation(std::span<const MappedSegment> segments, uint64_t prefLoadAdder, void (^callback)(const Fixup& fixup, bool& stop)) const;
    void            forEachRebaseLocation(void (^callback)(uint32_t segIndex, uint64_t segOffset, bool& stop)) const;
    void            printOpcodes(FILE* output, int indent=0) const;
    const uint8_t*  bytes(size_t& size) const;

private:
    struct SegRange { std::string_view segName; uint64_t vmSize; bool readable; bool writable; bool executable; };

    Error           forEachRebase(void (^handler)(const char* opcodeName, int type, bool segIndexSet,
                                                  uint8_t segmentIndex, uint64_t segmentOffset, bool& stop)) const;

    const uint8_t*       _opcodesStart;
    const uint8_t*       _opcodesEnd;
    const uint32_t       _pointerSize;
#if BUILDING_MACHO_WRITER
    std::vector<uint8_t> _opcodes;
    void                 append_uleb128(uint64_t value);
    void                 append_byte(uint8_t value);
#endif
 };



} // namespace mach_o

#endif // mach_o_RebaseOpcodes_h


