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

                    // encapsulates symbol table in a final linked image
                    NListSymbolTable(uint32_t preferredLoadAddress, const struct nlist*, uint32_t nlistCount, const char* stringPool, uint32_t stringPoolSize,
                                 uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount);
                    NListSymbolTable(uint64_t preferredLoadAddress, const struct nlist_64*, uint32_t nlistCount, const char* stringPool, uint32_t stringPoolSize,
                                 uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount);

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
    uint32_t        totalCount() const   { return _nlistCount; }
    uint32_t        nlistSize() const    { return _nlist32 ? (totalCount() * sizeof(struct nlist)) : (totalCount() * sizeof(struct nlist_64)); }
    uint32_t        undefsStartIndex() const;
    bool            symbolAtIndex(uint32_t symbolIndex, Symbol& symbol) const;

protected:
    // only for use by NListSymbolTableWriter
    NListSymbolTable() = default;

    int             libOrdinalFromDesc(uint16_t n_desc) const;
    Symbol          symbolFromNList(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc) const;
    bool            forEachSymbol(uint32_t startSymbolIndex, uint32_t symbolCount,
                                  void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop)) const;

    uint64_t                _preferredLoadAddress;
    const char*             _stringPool;
    const struct nlist*     _nlist32;
    const struct nlist_64*  _nlist64;
    uint32_t                _stringPoolSize;
    uint32_t                _nlistCount;
    uint32_t                _localsCount;
    uint32_t                _globalsCount;
    uint32_t                _undefsCount;
};


/*!
 * @class DebugNoteFileInfo
 *
 * @abstract
 *      A position and read-only blob, encapsulating debug file info.
 */
struct VIS_HIDDEN DebugNoteFileInfo
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


