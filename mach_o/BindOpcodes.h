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

#ifndef mach_o_BindOpcodes_h
#define mach_o_BindOpcodes_h

#include <stdint.h>
#include <stdio.h>

#include <optional>
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
 * @class BindOpcodes
 *
 * @abstract
 *      Class to encapsulate accessing and building bind opcodes
 */
class VIS_HIDDEN BindOpcodes
{
public:
                    // encapsulates Bind opcodes from a final linked image
                    BindOpcodes(const uint8_t* start, size_t size, bool is64);

    struct BindTarget {
        const char* symbolName=nullptr;
        int         libOrdinal=0;
        bool        weakImport=false;
        bool        strongOverrideOfWeakDef = false;
        int64_t     addend=0;
    };

    struct LocAndTarget {
        bool operator==(const BindOpcodes::LocAndTarget& other) const;
        uint32_t           segIndex=0;
        uint64_t           segOffset=0;
        const BindTarget*  target=nullptr;
    };

#if BUILDING_MACHO_WRITER
    enum class BuilderKind { regular, lazy, weak };

                    // used by unit tests to build opcodes
    typedef void (^LazyStartRecorder)(size_t offset, const char* symbolName);

                    // Note 'binds' input will be sorted by this method
                    BindOpcodes(std::span<LocAndTarget> binds, bool is64, BuilderKind kind = BuilderKind::regular, LazyStartRecorder starts=nullptr);
#endif

    Error           valid(std::span<const MappedSegment> segments, uint32_t dylibCount, bool allowTextFixups=false, bool onlyFixupsInWritableSegments=true) const;
    void            forEachBindLocation(void (^callback)(const LocAndTarget&, bool& stop)) const;
    void            forEachBindTarget(void (^callback)(const Fixup::BindTarget& target, bool& stop), void (^strongHandler)(const char* symbolName)) const;
    void            forEachBindLocation(std::span<const MappedSegment> segments, void (^callback)(const Fixup& fixup, bool& stop)) const;
    void            printOpcodes(FILE* output, int indent=0) const;
    const uint8_t*  bytes(size_t& size) const;

protected:
    virtual bool                hasDoneBetweenBinds() const;
    virtual std::optional<int>  implicitLibraryOrdinal() const;
    struct SegRange { std::string_view segName; uint64_t vmSize; bool readable; bool writable; bool executable; };

    Error           forEachBind(void (^handler)(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset,
                                                bool libraryOrdinalSet, int libOrdinal, const char* symbolName, bool weakImport, int64_t addend,
                                                bool targetOrAddendChanged, bool& stop),
                                void (^strongHandler)(const char* symbolName)) const;

    const uint8_t*       _opcodesStart;
    const uint8_t*       _opcodesEnd;
    const uint32_t       _pointerSize;
#if BUILDING_MACHO_WRITER
    std::vector<uint8_t> _opcodes;
    
    void                 append_byte(uint8_t value);
    void                 append_uleb128(uint64_t value);
    void                 append_sleb128(int64_t value);
    void                 append_string(const char* str);
#endif
};

class VIS_HIDDEN LazyBindOpcodes : public BindOpcodes
{
public:
                    // encapsulates Bind opcodes from a final linked image
                    LazyBindOpcodes(const uint8_t* start, size_t size, bool is64) : BindOpcodes(start, size, is64) { }

#if BUILDING_MACHO_WRITER
                    // used by unit tests to build opcodes
                    LazyBindOpcodes(std::span<LocAndTarget> binds, bool is64, BindOpcodes::LazyStartRecorder recorder)
                       : BindOpcodes(binds, is64, BuilderKind::lazy, recorder) { }
#endif

protected:
    bool            hasDoneBetweenBinds() const override final;

};

class VIS_HIDDEN WeakBindOpcodes : public BindOpcodes
{
public:
                    // encapsulates Bind opcodes from a final linked image
                    WeakBindOpcodes(const uint8_t* start, size_t size, bool is64) : BindOpcodes(start, size, is64) { }

#if BUILDING_MACHO_WRITER
                    // used by unit tests to build opcodes
                    WeakBindOpcodes(std::span<LocAndTarget> binds, bool is64) : BindOpcodes(binds, is64, BuilderKind::weak) { }
#endif

protected:
    std::optional<int>  implicitLibraryOrdinal() const override final;
};


} // namespace mach_o

#endif // mach_o_BindOpcodes_h


