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

#include "MemoryBuffer.h"
#include <algorithm>
#include <mach-o/loader.h>
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if !TARGET_OS_EXCLAVEKIT
#include <mach-o/stab.h>
#endif

#include "Symbol.h"
#include "NListSymbolTable.h"
#include "Misc.h"

namespace mach_o {

//
// MARK: --- NListSymbolTable inspection methods ---
//

NListSymbolTable::NListSymbolTable(uint32_t preferredLoadAddress, const struct nlist* symbols, uint32_t nlistCount,
                         const char* stringPool, uint32_t stringPoolSize,
                         uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount)
  : _preferredLoadAddress(preferredLoadAddress), _stringPool(stringPool), _nlist32(symbols), _nlist64(nullptr),
    _stringPoolSize(stringPoolSize), _nlistCount(nlistCount), _localsCount(localsCount), _globalsCount(globalsCount), _undefsCount(undefsCount)
{
}

NListSymbolTable::NListSymbolTable(uint64_t preferredLoadAddress, const struct nlist_64* symbols, uint32_t nlistCount,
                         const char* stringPool, uint32_t stringPoolSize,
                         uint32_t localsCount, uint32_t globalsCount, uint32_t undefsCount)
  : _preferredLoadAddress(preferredLoadAddress), _stringPool(stringPool), _nlist32(nullptr), _nlist64(symbols),
    _stringPoolSize(stringPoolSize), _nlistCount(nlistCount), _localsCount(localsCount), _globalsCount(globalsCount), _undefsCount(undefsCount)
{
}

Error NListSymbolTable::valid(uint64_t maxVmOffset) const
{
    // FIXME
    return Error::none();
}


int NListSymbolTable::libOrdinalFromDesc(uint16_t n_desc) const
{
    // -flat_namespace is always flat lookup
//    if ( (this->flags & MH_TWOLEVEL) == 0 )
//        return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

    // extract byte from undefined symbol entry
    int libIndex = GET_LIBRARY_ORDINAL(n_desc);
    switch ( libIndex ) {
        case SELF_LIBRARY_ORDINAL:
            return BIND_SPECIAL_DYLIB_SELF;

        case DYNAMIC_LOOKUP_ORDINAL:
            return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

        case EXECUTABLE_ORDINAL:
            return BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE;
    }

    return libIndex;
}


Symbol NListSymbolTable::symbolFromNList(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc) const
{
    bool dontDeadStrip = (n_desc & N_NO_DEAD_STRIP);
    bool cold          = (n_desc & N_COLD_FUNC);
    bool isThumb       = (n_desc & N_ARM_THUMB_DEF);
    switch ( n_type & N_TYPE ) {
        case N_UNDF:
            if ( n_value == 0 )
                return Symbol::makeUndefined(symbolName, libOrdinalFromDesc(n_desc), ((n_desc & N_WEAK_REF) != 0));
            else if ( n_type & N_PEXT )
                return Symbol::makeHiddenTentativeDef(symbolName, n_value, GET_COMM_ALIGN(n_desc), dontDeadStrip, cold);
            else
                return Symbol::makeTentativeDef(symbolName, n_value, GET_COMM_ALIGN(n_desc), dontDeadStrip, cold);
        case N_ABS: {
            Symbol::Scope scope = Symbol::Scope::global;

            if ( (n_type & N_EXT) == 0 ) {
                if ( n_type & N_PEXT )
                    scope = Symbol::Scope::wasLinkageUnit;
                else
                    scope = Symbol::Scope::translationUnit;
            }
            else if ( n_type & N_PEXT )
                scope = Symbol::Scope::linkageUnit;
            else
                scope = Symbol::Scope::global;

            return Symbol::makeAbsolute(symbolName, n_value, dontDeadStrip, scope);
        }
        case N_INDR: {
            const char* importName = symbolName;
            if ( n_value < _stringPoolSize )
                importName = _stringPool + n_value;
            if ( (n_type & N_EXT) == 0 ) {
                if ( (n_type & N_PEXT ))
                    return Symbol::makeReExport(symbolName, 0, importName, Symbol::Scope::wasLinkageUnit);
                else
                    return Symbol::makeReExport(symbolName, 0, importName, Symbol::Scope::translationUnit);
            }
            else if ( (n_type & N_PEXT ) )
                return Symbol::makeReExport(symbolName, 0, importName, Symbol::Scope::linkageUnit);
            else
                return Symbol::makeReExport(symbolName, 0, importName, Symbol::Scope::global);
        }
        case N_SECT: {
            if ( (n_type & N_EXT) == 0 ) {
                if ( n_desc & N_ALT_ENTRY ) {
                    if ( n_type & N_PEXT )
                        return Symbol::makeAltEntry(symbolName, n_value - _preferredLoadAddress, n_sect, Symbol::Scope::wasLinkageUnit, dontDeadStrip, cold, (n_desc & N_WEAK_DEF) != 0);
                    else
                        return Symbol::makeAltEntry(symbolName, n_value - _preferredLoadAddress, n_sect, Symbol::Scope::translationUnit, dontDeadStrip, cold, (n_desc & N_WEAK_DEF) != 0);
                }
                else if ( n_type & N_PEXT ) {
                    if ( n_desc & N_WEAK_DEF )
                        return Symbol::makeWeakDefWasPrivateExtern(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
                    else
                        return Symbol::makeRegularWasPrivateExtern(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
                } else
                    return Symbol::makeRegularLocal(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
            }
            else if ( n_type & N_PEXT ) {
                if ( n_desc & N_ALT_ENTRY )
                    return Symbol::makeAltEntry(symbolName, n_value - _preferredLoadAddress, n_sect, Symbol::Scope::linkageUnit, dontDeadStrip, cold, (n_desc & N_WEAK_DEF) != 0);
                else if ( n_desc & N_WEAK_DEF )
                    return Symbol::makeWeakDefHidden(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
                else if ( n_desc & N_SYMBOL_RESOLVER ) // rdar://123349256 (ld-prime needs to handle internal resolvers)
                    return Symbol::makeDynamicResolver(symbolName, n_sect, 0, n_value - _preferredLoadAddress, Symbol::Scope::linkageUnit);
                else
                    return Symbol::makeRegularHidden(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
            }
            else if ( n_desc & N_ALT_ENTRY ) {
                return Symbol::makeAltEntry(symbolName, n_value - _preferredLoadAddress, n_sect, Symbol::Scope::global, dontDeadStrip, cold, (n_desc & N_WEAK_DEF) != 0);
            }
            else if ( (n_desc & (N_WEAK_DEF|N_WEAK_REF)) == (N_WEAK_DEF|N_WEAK_REF) ) {
                return Symbol::makeWeakDefAutoHide(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
            }
            else if ( n_desc & N_WEAK_DEF ) {
                return Symbol::makeWeakDefExport(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, isThumb);
            }
            else if ( n_desc & N_SYMBOL_RESOLVER ) {
                return Symbol::makeDynamicResolver(symbolName, n_sect, 0, n_value - _preferredLoadAddress);
            }
            else {
                bool neverStrip = (n_desc & REFERENCED_DYNAMICALLY);
                return Symbol::makeRegularExport(symbolName, n_value - _preferredLoadAddress, n_sect, dontDeadStrip, cold, neverStrip, isThumb);
            }
        }
    }
    return Symbol();
}

void NListSymbolTable::forEachExportedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const
{
    if ( (_localsCount == 0) && (_globalsCount == 0) && (_undefsCount == 0) && (_nlistCount != 0) ) {
        // if no LC_DYSYMTAB, need to scan whole table and selectively find global symbols
        forEachSymbol(^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            uint8_t type = n_type & N_TYPE;
            if ( (n_type & N_EXT) && ((type == N_SECT) || (type == N_ABS) || (type == N_INDR)) && ((n_type & N_STAB) == 0))
                callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    } else {
        uint32_t globalsStartIndex = _localsCount;
        forEachSymbol(globalsStartIndex, _globalsCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            uint8_t type = n_type & N_TYPE;
            if ( (n_type & N_EXT) && ((type == N_SECT) || (type == N_ABS) || (type == N_INDR)) && ((n_type & N_STAB) == 0))
                callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    }
}

void NListSymbolTable::forEachDefinedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const
{
    if ( (_localsCount == 0) && (_globalsCount == 0) && (_undefsCount == 0) && (_nlistCount != 0) ) {
        // if no LC_DYSYMTAB, need to scan whole table and selectively find defined symbols
        forEachSymbol(0, _nlistCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            uint8_t type = n_type & N_TYPE;
            if ( ((type == N_SECT) || (type == N_ABS)) && ((n_type & N_STAB) == 0) )
                callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    }
    else {
        forEachSymbol(0, _localsCount+_globalsCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            uint8_t type = n_type & N_TYPE;
            if ( ((type == N_SECT) || (type == N_ABS)) && ((n_type & N_STAB) == 0) )
                callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    }
}

void NListSymbolTable::forEachSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const
{
    forEachSymbol(0, _nlistCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
        if ( (n_type & N_STAB) == 0 )
            callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
    });
}

void NListSymbolTable::forEachSymbol(void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop)) const
{
    forEachSymbol(0, _nlistCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
        callback(symbolName, n_value, n_type, n_sect, n_desc, symbolIndex, stop);
    });
}

bool NListSymbolTable::forEachSymbol(uint32_t startSymbolIndex, uint32_t symbolCount,
                                void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop)) const
{
    bool stop = false;
    for (uint32_t i = 0; (i < symbolCount) && !stop; ++i ) {
        if ( _nlist64 != nullptr ) {
            const struct nlist_64& sym = _nlist64[startSymbolIndex + i];
            if ( sym.n_un.n_strx > _stringPoolSize )
                continue;
            callback(&_stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, startSymbolIndex + i, stop);
        }
        else {
            const struct nlist& sym = _nlist32[startSymbolIndex + i];
            if ( sym.n_un.n_strx > _stringPoolSize )
                continue;
            callback(&_stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, startSymbolIndex + i, stop);
        }
    }
    return stop;
}

bool NListSymbolTable::symbolAtIndex(uint32_t symbolIndex, Symbol& symbol) const
{
    if ( symbolIndex >= _nlistCount )
        return false;

    if ( _nlist64 ) {
        const struct nlist_64& sym = _nlist64[symbolIndex];
        if ( sym.n_un.n_strx > _stringPoolSize )
            return false;
        symbol = symbolFromNList(&_stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc);
        return true;
    }

    if ( _nlist32 ) {
        const struct nlist& sym = _nlist32[symbolIndex];
        if ( sym.n_un.n_strx > _stringPoolSize )
            return false;
        symbol = symbolFromNList(&_stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc);
        return true;
    }

    return false;
}

bool NListSymbolTable::findClosestDefinedSymbol(uint64_t unslidAddr, Symbol& sym) const
{
    __block uint64_t    bestNValue   = 0;
    __block const char* bestName     = nullptr;
    __block uint16_t    bestNDesc    = 0;
    __block uint8_t     bestNType    = 0;
    __block uint8_t     bestNSect    = 0;
    auto nlistChecker = ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
        if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_STAB) == 0) ) {
            if ( (bestNValue < n_value) && (n_value <= unslidAddr) ) {
                bestNValue  = n_value;
                bestNDesc   = n_desc;
                bestNType   = n_type;
                bestNSect   = n_sect;
                bestName    = symbolName;
           }
        }
    };

    // first walk all global symbols, then all locals, recording closet symbol <= to target
    const uint32_t globalsStartIndex = _localsCount;
    const uint32_t localsStartIndex  = 0;
    if ( !this->forEachSymbol(globalsStartIndex, _globalsCount, nlistChecker) )
          this->forEachSymbol(localsStartIndex, _localsCount, nlistChecker);

    if ( bestName != nullptr ) {
        sym = symbolFromNList(bestName, bestNValue, bestNType, bestNSect, bestNDesc);
        return true;
    }
    return false;
}

uint32_t NListSymbolTable::undefsStartIndex() const
{
    if ( (_localsCount == 0) && (_globalsCount == 0) && (_undefsCount == 0) && (_nlistCount != 0) )
        return 0; // no LC_DYSYMTAB, any symbol can be undefined
    return _localsCount+_globalsCount;
}

void NListSymbolTable::forEachUndefinedSymbol(void (^callback)(const Symbol& symbol, uint32_t symbolIndex, bool& stop)) const
{
    if ( (_localsCount == 0) && (_globalsCount == 0) && (_undefsCount == 0) && (_nlistCount != 0) ) {
        // if no LC_DYSYMTAB, need to scan whole table and selectively find undefined symbols
        forEachSymbol(0, _nlistCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            if ( ((n_type & N_TYPE) == N_UNDF) && ((n_type & N_STAB) == 0) )
                callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    }
    else {
        uint32_t undefinesStartIndex = _localsCount+_globalsCount;
        forEachSymbol(undefinesStartIndex, _undefsCount, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
            callback(symbolFromNList(symbolName, n_value, n_type, n_sect, n_desc), symbolIndex, stop);
        });
    }
}

uint64_t NListSymbolTable::nValueFromSymbolIndex(uint32_t symbolIndex) const
{
    assert(symbolIndex < _nlistCount);
    if ( _nlist64 != nullptr )
        return _nlist64[symbolIndex].n_value;
    else
        return _nlist32[symbolIndex].n_value;
}

#if !TARGET_OS_EXCLAVEKIT
void NListSymbolTable::forEachDebugNote(bool freeFileInfo, void (^callback)(const DebugNote& note, bool& stop)) const
{
    __block CString   currentSrcDir = nullptr;
    __block CString   currentSrcName = nullptr;
    __block CString   currentObjPath = nullptr;
    __block uint32_t  currentObjModTime = 0;
    __block uint32_t  currentObjSubType = 0;
    __block CString   currentOriginlibPath = nullptr;
    __block DebugNote currentNote;
    // if no LC_DYSYMTAB, need to scan whole table and selectively find stab symbols
    uint32_t count = _localsCount;
    if ( (_localsCount == 0) && (_globalsCount == 0) && (_undefsCount == 0) && (_nlistCount != 0) )
        count = _nlistCount;
    forEachSymbol(0, count, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, uint32_t symbolIndex, bool& stop) {
        if ( (n_type & N_STAB) == 0 )
            return;
        switch ( n_type ) {
            case N_SO:
                if ( n_sect == 1 ) {
                    // end of file
                    // ld64 wrote extra "end SO" at start of debug notes, we need to skip over that
                    if ( !currentSrcName.empty() ) {
                        // build temp DebugNoteFileInfo just for use during the callback
                        const DebugNoteFileInfo* fileInfo = DebugNoteFileInfo::make(currentSrcDir, currentSrcName, currentObjPath, currentObjModTime, currentObjSubType, "", currentOriginlibPath);
                        currentNote.fileInfo = fileInfo;
                        callback(currentNote, stop);
                        currentNote.fileInfo = nullptr;
                        currentSrcDir = nullptr;
                        currentSrcName = nullptr;
                        currentObjPath = nullptr;
                        currentObjModTime = 0;
                        currentObjSubType = 0;
                        currentOriginlibPath = nullptr;
                        if ( freeFileInfo )
                            free((void*)fileInfo);
                    }
                }
                else {
                    currentNote.items.clear();
                    size_t len = strlen(symbolName);
                    if ( (len > 1) && (symbolName[len-1] == '/') )
                        currentSrcDir = symbolName;
                    else
                        currentSrcName = symbolName;
                }
                break;
            case N_OSO:
                currentObjPath    = symbolName;
                currentObjModTime = (uint32_t)n_value;
                currentObjSubType = n_sect;
                break;
            case N_LIB:
                currentOriginlibPath    = symbolName;
                break;
            case N_BNSYM:
                currentNote.items.push_back({n_value, 0, nullptr, N_FUN, n_sect});
                break;
            case N_FUN:
                if ( n_sect != 0 )
                    currentNote.items.back().name = symbolName;
                else
                    currentNote.items.back().size = n_value;
                break;
            case N_ENSYM:
                break;
            case N_STSYM:
                currentNote.items.push_back({n_value, 0, symbolName, N_STSYM, n_sect});
                break;
            case N_GSYM:
                currentNote.items.push_back({0, 0, symbolName, N_GSYM, 0});
                break;
            default:
                // ignore other stabs
                break;
        }
    });
}
#endif // !TARGET_OS_EXCLAVEKIT

const DebugNoteFileInfo* DebugNoteFileInfo::make(CString srcDir, CString srcName, CString objPath, uint32_t objModTime, uint8_t objSubType, CString libPath, CString originLibPath)
{
    DebugNoteFileInfo* result = (DebugNoteFileInfo*)calloc(1, sizeof(DebugNoteFileInfo));
    result->_objModTime    = objModTime;
    result->_objSubType    = objSubType;
    result->_srcDir        = srcDir;
    result->_srcName       = srcName;
    result->_objPath       = objPath;
    result->_libPath       = libPath;
    result->_originLibPath = originLibPath;

    return result;
}

Error DebugNoteFileInfo::valid(std::span<const uint8_t> buffer)
{
    return Error::none();
}

const DebugNoteFileInfo* DebugNoteFileInfo::copy() const
{
    return DebugNoteFileInfo::make(_srcDir, _srcName, _objPath, _objModTime, _objSubType, _libPath, _originLibPath);
}
bool DebugNoteFileInfo::shouldbeUpdated(CString libPath) const
{
    // .o -> .dylib
    if ( !this->hasLibInfo() && !this->hasOriginLibInfo() )
        return true;

    if ( strcmp(libPath.c_str(), this->libPath().c_str()) != 0 )
        return true;

    return false;
}

__attribute__((used))
void DebugNoteFileInfo::dump() const
{
    fprintf(stdout, "scrDir:      %s\n", srcDir().c_str());
    fprintf(stdout, "scrName:     %s\n", srcName().c_str());
    fprintf(stdout, "objPath:     %s\n", objPath().c_str());
    fprintf(stdout, "objModTime:  0x%08X\n", this->objModTime());
    fprintf(stdout, "objSubType:  0X%02X\n", this->objSubType());
    fprintf(stdout, "libPath:     %s\n", hasLibInfo() ? libPath().c_str() : "N/A");
    fprintf(stdout, "origlibPath: %s\n", hasOriginLibInfo() ? originLibPath().c_str() : "N/A");
}


} // namespace mach_o
