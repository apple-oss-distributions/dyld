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

#ifndef mach_o_SymbolTable_h
#define mach_o_SymbolTable_h

#include <stdint.h>
#include <mach-o/nlist.h>

#include <span>
#include <vector>

#include "Error.h"
#include "MemoryBuffer.h"
#include "Symbol.h"

#if BUILDING_MACHO_WRITER
#include "Containers.h"
#endif

#ifndef N_LIB
#define N_LIB    0x68
#endif

namespace mach_o {


struct DebugNoteFileInfo;

/*!
 * @class NListSymbolTable
 *
 * @abstract
 *      Class to encapsulate accessing and building an nlist symbol table in mach-o
 */
class VIS_HIDDEN NListSymbolTable
{
public:
    struct DebugNoteItem     { uint64_t addr=0; uint64_t size=0; const char* name=nullptr; uint8_t type=0; uint8_t sectNum=0; };
    struct DebugNote         { const DebugNoteFileInfo* fileInfo; std::vector<DebugNoteItem> items; };

#if BUILDING_MACHO_WRITER
    struct DebugBuilderNoteItem
    {
        uint64_t        addr=0;
        uint64_t        size=0;

        union {
            // when using convenience NList constructor this has to point to
            // the note's name, otherwise the pointer won't be used, so callers
            // can use the `userData` field to store their own context.
            // ld's layout uses this to store atoms and implement efficient reuse
            // of the string pool strings.
            void*       userData=nullptr;
            const char* name;
        };

        uint8_t         type=0;
        uint8_t         sectNum=0;
        uint32_t        stringPoolOffset=0;
    };

    struct DebugBuilderNote
    {
        const DebugNoteFileInfo*            fileInfo;
        std::vector<DebugBuilderNoteItem>   items;
        uint32_t                            srcDirPoolOffset=0;
        uint32_t                            srcNamePoolOffset=0;
        uint32_t                            originLibPathPoolOffset=0;
        uint32_t                            objPathPoolOffset=0;
    };

    struct NListLayout
    {
        std::span<const Symbol>             globals;
        std::span<const uint32_t>           globalsStrx;
        std::span<const uint32_t>           reexportStrx;
        std::span<const Symbol>             undefs;
        std::span<const uint32_t>           undefsStrx;
        std::span<const Symbol>             locals;
        std::span<const uint32_t>           localsStrx;
        std::span<const DebugBuilderNote>   debugNotes;
        uint32_t                            debugNotesNListCount=0;
    };
#endif // BUILDING_MACHO_WRITER

                    // encapsulates symbol table in a final linked image
                    NListSymbolTable(uint32_t preferredLoadAddress, const struct nlist*, uint32_t nlistCount, const char* stringPool, uint32_t stringPoolSize,
                                 uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount);
                    NListSymbolTable(uint64_t preferredLoadAddress, const struct nlist_64*, uint32_t nlistCount, const char* stringPool, uint32_t stringPoolSize,
                                 uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount);
#if BUILDING_MACHO_WRITER
                    // Convenience NList constructors used in unit tests
                    // \{
                    NListSymbolTable(std::span<const Symbol> symbols, uint64_t prefLoadAddr, bool is64, std::span<DebugBuilderNote> debugNotes={}, bool zeroModTimes=false,
                                     bool objectFile=false);
                    NListSymbolTable(std::span<const Symbol> globals, std::span<const Symbol> undefs, std::span<const Symbol> locals, std::span<DebugBuilderNote> debugNotes, uint64_t prefLoadAddr, bool is64, bool zeroModTimes);
                    // \}

                    // NList constructor with a precomputed layout and nlist buffer
                    NListSymbolTable(NListLayout layout, std::span<uint8_t> nlistBuffer, uint64_t prefLoadAddr, bool is64, bool zeroModTimes);

    static uint32_t countDebugNoteNLists(std::span<const DebugBuilderNote> debugNotes);
#endif

    Error           valid(uint64_t maxVmOffset) const;
    bool            hasExportedSymbol(const char* symbolName, Symbol& symbol) const;
    void            forEachSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const;
    void            forEachExportedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const;
    void            forEachDefinedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const;
    bool            findClosestDefinedSymbol(uint64_t unslidAddr, Symbol& symbol) const;
    void            forEachUndefinedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const;
    void            forEachSymbol(void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop)) const;
    void            forEachDebugNote(bool freeFileInfo, void (^callback)(const DebugNote& note, bool& stop)) const;
    void            forEachDebugNote(void (^callback)(const DebugNote& note, bool& stop)) const { forEachDebugNote(/* freeFileInfo */ true, callback); }
    uint64_t        nValueFromSymbolIndex(uint32_t symbolIndex) const;
    const char*     stringPool() const { return _stringPool; }
    uint32_t        stringPoolSize() const { return _stringPoolSize; }
    const void*     nlistArray() const { return ((_nlist64 != nullptr) ? (void*)_nlist64 : (void*)_nlist32); }
    uint32_t        localsCount() const  { return _localsCount; }
    uint32_t        globalsCount() const { return _globalsCount; }
    uint32_t        undefsCount() const  { return _undefsCount; }
    uint32_t        totalCount() const   { return _localsCount+_globalsCount+_undefsCount; }
    uint32_t        nlistSize() const    { return _nlist32 ? (totalCount() * sizeof(struct nlist)) : (totalCount() * sizeof(struct nlist_64)); }
    uint32_t        undefsStartIndex() const;

private:
    int             libOrdinalFromDesc(uint16_t n_desc) const;
    Symbol          symbolFromNList(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc) const;
    struct nlist    nlistFromSymbol(const Symbol&, uint32_t strx, uint32_t reexportStrx);
    struct nlist_64 nlist64FromSymbol(const Symbol&, uint32_t strx, uint32_t reexportStrx);
    bool            forEachSymbol(uint32_t startSymbolIndex, uint32_t symbolCount,
                                  void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop)) const;

#if BUILDING_MACHO_WRITER

    struct SymbolPartition
    {
        std::vector<Symbol>     locals;
        std::vector<Symbol>     globals;
        std::vector<Symbol>     undefs;

        SymbolPartition(std::span<const Symbol> symbol, bool objectFile);
    };

    struct NListBuffer
    {

        WritableMemoryBuffer storage;
        std::span<uint8_t> buffer;

        NListBuffer(std::span<uint8_t> buffer): buffer(buffer) {}
        NListBuffer(std::span<nlist_64> buffer): buffer((uint8_t*)buffer.data(), buffer.size_bytes()) {}
        NListBuffer(std::span<struct nlist> buffer): buffer((uint8_t*)buffer.data(), buffer.size_bytes()) {}

        NListBuffer(size_t bufferSize)
        {
            storage = WritableMemoryBuffer::allocate(bufferSize);
            buffer = storage;
        }

        NListBuffer() {}


        void add(nlist_64 nlist)
        {
            assert(buffer.size() >= (sizeof(nlist_64)));
            *(nlist_64*)buffer.data() = nlist;
            buffer = buffer.subspan(sizeof(nlist_64));
        }

        void add(struct nlist nlist)
        {
            assert(buffer.size() >= (sizeof(struct nlist)));
            *(struct nlist*)buffer.data() = nlist;
            buffer = buffer.subspan(sizeof(struct nlist));
        }
    };

                    NListSymbolTable(const SymbolPartition& partition, std::span<DebugBuilderNote> debugNotes,
                            uint64_t prefLoadAddr, bool is64, bool zeroModTimes);

                    NListSymbolTable(NListLayout layout, NListBuffer nlist, std::vector<char> stringPool,
                            uint64_t prefLoadAddr, bool is64, bool zeroModTimes);

    template <typename T>
    void            addStabsFromDebugNotes(std::span<const DebugBuilderNote> debugNotes, bool zeroModTimes, NListBuffer& nlists);
#endif

    uint64_t                _preferredLoadAddress;
    const char*             _stringPool;
    const struct nlist*     _nlist32;
    const struct nlist_64*  _nlist64;
    uint32_t                _stringPoolSize;
    uint32_t                _nlistCount;
    uint32_t                _localsCount;
    uint32_t                _globalsCount;
    uint32_t                _undefsCount;
#if BUILDING_MACHO_WRITER
    NListBuffer             _nlistBuffer;
    std::vector<char>       _stringPoolBuffer;
#endif
};


/*!
 * @class DebugNoteFileInfo
 *
 * @abstract
 *      A position and read-only blob, encapsulating debug file info.
 */
struct DebugNoteFileInfo
{
public:
                                    DebugNoteFileInfo(const DebugNoteFileInfo&) = delete;
                                    DebugNoteFileInfo(DebugNoteFileInfo&&) = delete;
                                    DebugNoteFileInfo& operator=(const DebugNoteFileInfo&) = delete;
                                    DebugNoteFileInfo& operator=(DebugNoteFileInfo&&) = delete;

    static const DebugNoteFileInfo* make(CString srcDir, CString srcName, CString objPath, uint32_t objModTime=0, uint8_t objSubType=0, CString libPath=CString(), CString originLibPath=CString());
    static mach_o::Error            valid(std::span<const uint8_t> buffer);
    const DebugNoteFileInfo*        copy() const;

    CString                         srcDir() const          { return _srcDir; }
    CString                         srcName() const         { return _srcName; }
    CString                         objPath() const         { return _objPath; }
    uint32_t                        objModTime() const      { return _objModTime; }
    uint8_t                         objSubType() const      { return _objSubType; }
    CString                         originLibPath() const   { return _originLibPath; }
    CString                         libPath() const         { return _libPath; }

    bool                            hasLibInfo() const { return !_libPath.empty();  }
    bool                            hasOriginLibInfo() const { return !_originLibPath.empty();  }
    bool                            shouldbeUpdated(CString LibPath) const;
    void                            dump() const;

private:
    uint32_t  _objModTime;
    uint32_t  _objSubType;
    CString   _srcDir;
    CString   _srcName;
    CString   _objPath;
    CString   _libPath;
    CString   _originLibPath;
};




} // namespace mach_o

#endif // mach_o_SymbolTable_h


