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

#ifndef mach_o_writer_ChainedFixups_h
#define mach_o_writer_ChainedFixups_h

#include <stdint.h>
#include <stdio.h>
#include <mach-o/fixup-chains.h>

#include <span>
#include <vector>

// mach_o
#include "ChainedFixups.h"
#include "Error.h"
#include "Header.h"
#include "Fixups.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class ChainedFixups
 *
 * @abstract
 *      Class to encapsulate building chained fixups
 */
class VIS_HIDDEN ChainedFixupsWriter : public ChainedFixups
{
public:
    // Information we need to encode a single segment with chained fixups
    struct SegmentFixupsInfo {
        MappedSegment             mappedSegment;
        std::span<const Fixup>    fixups;
        uint32_t                  numPageExtras;
    };

    // used by unit tests to build chained fixups
    ChainedFixupsWriter(std::span<const Fixup::BindTarget> bindTargets,
                        std::span<const Fixup> fixups,
                        std::span<const MappedSegment> segments,
                        uint64_t preferredLoadAddress,
                        const PointerFormat& pf, uint32_t pageSize,
                        bool setDataChains,
                        bool startsInSection=false, bool useFileOffsets=false);

    // used by Layout to build chained fixups
    ChainedFixupsWriter(std::span<const Fixup::BindTarget> bindTargets,
                        std::span<const SegmentFixupsInfo> segments,
                        uint64_t preferredLoadAddress,
                        const PointerFormat& pf, uint32_t pageSize,
                        bool setDataChains,
                        bool startsInSection=false, bool useFileOffsets=false);


    static Error            importsFormat(std::span<const Fixup::BindTarget> bindTargets, uint16_t& importsFormat, size_t& stringPoolSize);

    static size_t           linkeditSize(std::span<const Fixup::BindTarget> bindTargets,
                                         std::span<const SegmentFixupsInfo> segments,
                                         const PointerFormat& pointerFormat, uint32_t pageSize);
    static size_t           startsSectionSize(std::span<const SegmentFixupsInfo> segments, const PointerFormat& pointerFormat);

    // Fills in the SegmentFixupsInfo::numPageExtras field for every segment with page extras
    static void             calculateSegmentPageExtras(std::span<SegmentFixupsInfo> segments,
                                                       const PointerFormat& pointerFormat,
                                                       uint32_t pageSize);

    Error           valid(uint64_t preferredLoadAddress, std::span<const MappedSegment> segments, bool startsInSection=false) const;

    const uint8_t*  bytes(size_t& size) const;

    void                                    buildLinkeditFixups(std::span<const Fixup::BindTarget> bindTargets,
                                                                std::span<const SegmentFixupsInfo> segments,
                                                                uint64_t preferredLoadAddress,
                                                                const PointerFormat& pf, uint32_t pageSize,
                                                                bool setDataChains);
    void                                    buildStartsSectionFixups(std::span<const SegmentFixupsInfo> segments,
                                                                     const PointerFormat& pf,
                                                                     bool useFileOffsets, uint64_t preferredLoadAddress);
    static uint32_t                         addSymbolString(CString symbolName, std::vector<char>& pool);

    Error                   _buildError;
    std::vector<uint8_t>    _bytes;
};


} // namespace mach_o

#endif // mach_o_writer_ChainedFixups_h


