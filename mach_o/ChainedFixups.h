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

#ifndef mach_o_ChainedFixups_h
#define mach_o_ChainedFixups_h

#include <stdint.h>
#include <stdio.h>
#include <mach-o/fixup-chains.h>

#include <span>
#if BUILDING_MACHO_WRITER
  #include <vector>
#endif

#include "Error.h"
#include "Header.h"
#include "Fixups.h"


namespace mach_o {

class Image;
struct MappedSegment;

/*!
 * @class ChainedFixups
 *
 * @abstract
 *      Class to encapsulate interpretting and building chained fixups
 */
class VIS_HIDDEN ChainedFixups
{
public:
                    // encapsulates chained fixups from a final linked image
                    ChainedFixups(const dyld_chained_fixups_header* fixupInfo, size_t size);

    // encapsulates everything about a pointer_format
    class PointerFormat
    {
    public:
                                     PointerFormat(const PointerFormat&) = default;
        static bool                  valid(uint16_t pointer_format);
        static const PointerFormat&  make(uint16_t pointer_format);
        
        virtual uint16_t         value() const = 0;
        virtual const char*      name() const = 0;
        virtual const char*      description() const = 0;
        virtual bool             is64() const = 0;
        virtual bool             supportsAuth() const = 0;
        virtual uint32_t         minNext() const = 0;   // aka stride, 4 or 8
        virtual uint32_t         maxNext() const = 0;   // max distance next chain entry could be
        virtual uint64_t         maxRebaseTargetOffset(bool authenticated) const = 0;
        virtual bool             supportsBinds() const = 0;
        virtual uint32_t         maxBindOrdinal(bool authenticated) const = 0;
        virtual int32_t          bindMaxEmbeddableAddend(bool authenticated) const = 0;
        virtual int32_t          bindMinEmbeddableAddend(bool authenticated) const = 0;
        virtual const void*      nextLocation(const void* loc) const = 0;
        void                     forEachFixupLocationInChain(const void* chainStartLoc, uint64_t prefLoadAddr, const MappedSegment* seg,
                                                             void (^callback)(const Fixup& info, bool& stop)) const;
        virtual Fixup            parseChainEntry(const void* loc, const MappedSegment* seg, uint64_t preferedLoadAddress=0) const = 0;
#if BUILDING_MACHO_WRITER
        virtual void             writeChainEntry(const Fixup& fixup, const void* nextLoc, uint64_t preferedLoadAddress) const = 0;
#endif

    protected:
                                 PointerFormat() { }
    };

#if BUILDING_MACHO_WRITER

    // Information we need to encode a single segment with chained fixups
    struct SegmentFixupsInfo {
        MappedSegment             mappedSegment;
        std::span<const Fixup>    fixups;
        uint32_t                  numPageExtras;
    };

#if BUILDING_UNIT_TESTS
                    // used by unit tests to build chained fixups
                    ChainedFixups(std::span<const Fixup::BindTarget> bindTargets,
                                  std::span<const Fixup> fixups,
                                  std::span<const MappedSegment> segments,
                                  uint64_t preferredLoadAddress,
                                  const PointerFormat& pf, uint32_t pageSize, bool setDataChains);
#endif

                    // used by Layout to build chained fixups
                    ChainedFixups(std::span<const Fixup::BindTarget> bindTargets,
                                  std::span<const SegmentFixupsInfo> segments,
                                  uint64_t preferredLoadAddress,
                                  const PointerFormat& pf, uint32_t pageSize, bool setDataChains);



    static size_t           linkeditSize(std::span<const Fixup::BindTarget> bindTargets,
                                         std::span<const SegmentFixupsInfo> segments,
                                         uint32_t pageSize);

    // Fills in the SegmentFixupsInfo::numPageExtras field for every segment with page extras
    static void             calculateSegmentPageExtras(std::span<SegmentFixupsInfo> segments,
                                                       const PointerFormat& pointerFormat,
                                                       uint32_t pageSize);

#endif

    Error                   valid(std::span<const MappedSegment> segments) const;
    uint32_t                pageSize() const;
    const PointerFormat&    pointerFormat() const;
    void                    forEachFixupChainStartLocation(std::span<const MappedSegment> segments,
                                                           void (^callback)(const void* chainStart, uint32_t segIndex, const PointerFormat&, bool& stop)) const;
    void                    forEachBindTarget(void (^callback)(const Fixup::BindTarget&, bool& stop)) const;

    const dyld_chained_fixups_header*  bytes(size_t& size) const;

    static const char*      importsFormatName(uint32_t format);
    const char*             importsFormatName() const;


private:
    Error                                   forEachBindTarget(void (^callback)(int libOrdinal, const char* symbolName, int64_t addend, bool weakImport, bool& stop)) const;
    const dyld_chained_starts_in_segment*   startsForSegment(uint32_t segIndex) const;
    static Error                            importsFormat(std::span<const Fixup::BindTarget> bindTargets, uint16_t& importsFormat, size_t& stringPoolSize);

#if BUILDING_MACHO_WRITER
    void                                    buildFixups(std::span<const Fixup::BindTarget> bindTargets,
                                                        std::span<const SegmentFixupsInfo> segments,
                                                        uint64_t preferredLoadAddress,
                                                        const PointerFormat& pf, uint32_t pageSize, bool setDataChains);
    static uint32_t                         addSymbolString(CString symbolName, std::vector<char>& pool);
#endif

    const dyld_chained_fixups_header*          _fixupsHeader = nullptr;
    size_t                                     _fixupsSize   = 0;
#if BUILDING_MACHO_WRITER
    Error                                      _buildError;
    std::vector<uint8_t>                       _bytes;
#endif
 };



} // namespace mach_o

#endif // mach_o_ChainedFixups_h


