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

#include <climits>
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>

// mach_o
#include "ExportsTrie.h"
#include "Symbol.h"
#include "Misc.h"
#include "Header.h"

namespace mach_o {

//
// MARK: --- GenericTrie methods ---
//

// construct from an already built trie
GenericTrie::GenericTrie(const uint8_t* start, size_t size) : _trieStart(start), _trieEnd(start+size)
{
}

uint32_t GenericTrie::entryCount() const
{
    if ( _trieStart == _trieEnd )
        return 0;
    __block uint32_t result = 0;
    bool             stop = false;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& innerStop) {
        ++result;
    });
    return result;
}

void GenericTrie::forEachEntry(void (^callback)(const Entry& entry, bool& stop)) const
{
    // ld64 will emit an empty export trie load command as a placeholder to show there are no exports.
    // In that case, don't start recursing as we'll immediately think we ran of the end of the buffer.
    if ( _trieStart == _trieEnd )
        return;
    bool stop = false;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& innerStop) {
        Entry entry { name, nodePayload };
        callback(entry, innerStop);
    });
}

Error GenericTrie::recurseTrie(const uint8_t* p, dyld3::OverflowSafeArray<char>& cummulativeString, int curStrOffset, bool& stop,
                                  void (^callback)(const char* name, std::span<const uint8_t> nodePayload, bool& stop)) const
{
    if ( p >= _trieEnd ) {
        return Error("malformed trie, node past end");
    }
    bool           malformed;
    const uint64_t terminalSize = read_uleb128(p, _trieEnd, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    const uint8_t* children = p + terminalSize;
    if ( children > _trieEnd )
        return Error("malformed trie, terminalSize extends beyond trie data");

    if ( terminalSize != 0 ) {
        if ( callback )
            callback(cummulativeString.data(), std::span<const uint8_t>(p, p+terminalSize), stop);
        if ( stop )
            return Error::none();
    }
    const uint8_t  childrenCount = *children++;
    const uint8_t* s             = children;
    for ( uint8_t i = 0; (i < childrenCount) && !stop; ++i ) {
        int edgeStrLen = 0;
        while ( *s != '\0' ) {
            cummulativeString.resize(curStrOffset + edgeStrLen + 1);
            cummulativeString[curStrOffset + edgeStrLen] = *s++;
            ++edgeStrLen;
            if ( s > _trieEnd )
                return Error("malformed trie node, child node name extends beyond trie data");
        }
        cummulativeString.resize(curStrOffset + edgeStrLen + 1);
        cummulativeString[curStrOffset + edgeStrLen] = *s++;
        uint64_t childNodeOffset                     = read_uleb128(s, _trieEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        if ( childNodeOffset == 0 )
            return Error("malformed trie, childNodeOffset==0");
        if ( Error err = this->recurseTrie(_trieStart + childNodeOffset, cummulativeString, curStrOffset + edgeStrLen, stop, callback) )
            return err;
    }
    return Error::none();
}

#if 1
void GenericTrie::dump() const
{
    fprintf(stderr, "trie terminal nodes:\n");
    if ( _trieStart == _trieEnd )
        return;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
    bool stop = false;
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& trieStop) {
        fprintf(stderr, "  0x%04lX: ", (long)(nodePayload.data()-_trieStart - uleb128_size(nodePayload.size())));
        for (size_t i=0; i < nodePayload.size(); ++i)
            fprintf(stderr, "0x%02X ", nodePayload[i]);
        fprintf(stderr, "%s\n", name);
    });
}
#endif

bool GenericTrie::hasEntry(const char* name, std::span<const uint8_t>& terminalPayload) const
{
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint32_t, visitedNodeOffsets, 256);
    visitedNodeOffsets.push_back(0);
    const uint8_t* p                = _trieStart;
    while ( p < _trieEnd ) {
        bool     malformed;
        uint64_t terminalSize = *p++;
        if ( terminalSize > 127 ) {
            // except for re-export-with-rename, all terminal sizes fit in one byte
            --p;
            terminalSize = read_uleb128(p, _trieEnd, malformed);
            if ( malformed )
                return false;
        }
        if ( (*name == '\0') && (terminalSize != 0) ) {
            terminalPayload = std::span<const uint8_t>(p, (size_t)terminalSize);
            return true;
        }
        const uint8_t* children = p + terminalSize;
        if ( children > _trieEnd ) {
            // diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
            return false;
        }
        uint8_t childrenRemaining = *children++;
        p                         = children;
        uint64_t nodeOffset       = 0;
        for ( ; childrenRemaining > 0; --childrenRemaining ) {
            const char* ss        = name;
            bool        wrongEdge = false;
            // scan whole edge to get to next edge
            // if edge is longer than target symbol name, don't read past end of symbol name
            char c = *p;
            while ( c != '\0' ) {
                if ( !wrongEdge ) {
                    if ( c != *ss )
                        wrongEdge = true;
                    ++ss;
                }
                ++p;
                c = *p;
            }
            if ( wrongEdge ) {
                // advance to next child
                ++p; // skip over zero terminator
                // skip over uleb128 until last byte is found
                while ( (*p & 0x80) != 0 )
                    ++p;
                ++p; // skip over last byte of uleb128
                if ( p > _trieEnd ) {
                    return false;
                }
            }
            else {
                // the symbol so far matches this edge (child)
                // so advance to the child's node
                ++p;
                nodeOffset = read_uleb128(p, _trieEnd, malformed);
                if ( malformed )
                    return false;
                if ( (nodeOffset == 0) || (&_trieStart[nodeOffset] > _trieEnd) )
                    return false;
                name = ss;
                break;
            }
        }
        if ( nodeOffset != 0 ) {
            if ( nodeOffset > (uint64_t)(_trieEnd - _trieStart) )
                return false;
            // check for cycles
            for ( uint32_t visitedOffset : visitedNodeOffsets ) {
                if ( visitedOffset == nodeOffset )
                    return false;
            }
            visitedNodeOffsets.push_back((uint32_t)nodeOffset);
            p = &_trieStart[nodeOffset];
        }
        else {
            p = _trieEnd;
        }
    }
    return false;
}

//
// MARK: --- ExportsTrie methods ---
//


uint32_t ExportsTrie::symbolCount() const
{
    return this->entryCount();
}

bool ExportsTrie::hasExportedSymbol(const char* symbolName, Symbol& symbol) const
{
    std::span<const uint8_t> terminalPayload;
    if ( this->hasEntry(symbolName, terminalPayload) ) {
        Entry entry = { symbolName, terminalPayload };
        if ( terminalPayloadToSymbol(entry, symbol).noError() )
            return true;
    }
    return false;
}

void ExportsTrie::forEachExportedSymbol(void (^callback)(const Symbol& symbol, bool& stop)) const
{
    this->forEachEntry(^(const Entry& entry, bool& stop) {
        Symbol symbol;
        if ( terminalPayloadToSymbol(entry, symbol).noError() )
             callback(symbol, stop);
    });
}

Error ExportsTrie::valid(uint64_t maxVmOffset) const
{
    if ( _trieStart == _trieEnd )
        return Error::none();
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
    bool stop = false;
    __block Error contentErr;
    Error recurseErr = this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& trieStop) {
        Entry   entry { name, nodePayload };
        Symbol  symbol;
        if ( Error err = terminalPayloadToSymbol(entry, symbol) ) {
            contentErr = std::move(err);
            trieStop = true;
            return;
        }
        uint64_t    absAddress;
        int         libOrdinal;
        const char* importName;
        if ( !symbol.isAbsolute(absAddress) && !symbol.isReExport(libOrdinal, importName) ) {
            uint64_t vmOffset = symbol.implOffset();
            if ( vmOffset > maxVmOffset ) {
                contentErr = Error("vmOffset too large for %s", symbol.name().c_str());
                trieStop = true;
            }
        }
    });
    if ( recurseErr )
        return recurseErr;
    return std::move(contentErr);
}

Error ExportsTrie::terminalPayloadToSymbol(const Entry& entry, Symbol& symInfo) const
{
    bool           malformed;
    const uint8_t* p          = entry.terminalPayload.data();
    const uint8_t* end        = p+entry.terminalPayload.size();
    uint64_t       flags      = read_uleb128(p, end, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    if ( (flags >> 6) != 0 )
        return Error("unknown exports flag bits");

    uint8_t  kind  = (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK);
    uint64_t value = read_uleb128(p, end, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    if ( kind == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE )
        symInfo = Symbol::makeAbsolute(entry.name.data(), value, /* dont dead strip */ false, Symbol::Scope::global);
    else if ( kind == EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL )
        symInfo = Symbol::makeThreadLocalExport(entry.name.data(), value, 0, false, false, flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
    else if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
        const char* importName = entry.name.data();
        if ( *p != '\0' ) {
            importName = (char*)p;
            while (*p != '\0')
                ++p;
        }
        ++p;
        symInfo = Symbol::makeReExport(entry.name.data(), (int)value, importName);
        if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
            symInfo.setWeakDef();
    }
    else if ( flags & EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT ) {
        uint64_t tableIndex = read_uleb128(p, end, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        symInfo = Symbol::makeFunctionVariantExport(entry.name.data(), 0, value, (uint32_t)tableIndex);
    }
    else if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
        uint64_t funcOffset = read_uleb128(p, end, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        symInfo = Symbol::makeDynamicResolver(entry.name.data(), 1, value, funcOffset);
    }
    else if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
        symInfo = Symbol::makeWeakDefExport(entry.name.data(), value, 0, false, false);
    else
        symInfo = Symbol::makeRegularExport(entry.name.data(), value, 0, false, false);

    return Error::none();
}


//
// MARK: --- DylibsPathTrie methods ---
//

bool DylibsPathTrie::entryToIndex(std::span<const uint8_t> payload, uint32_t& dylibIndex) const
{
    const uint8_t* p = payload.data();
    const uint8_t* end = p+payload.size();
    bool malformed;
    uint64_t value = read_uleb128(p, end, malformed);
    if ( !malformed ) {
        dylibIndex = (uint32_t)value;
        return true;
    }
    return false;
}

bool DylibsPathTrie::hasPath(const char* path, uint32_t& dylibIndex) const
{
    std::span<const uint8_t> terminalPayload;
    if ( this->hasEntry(path, terminalPayload) ) {
        if ( entryToIndex(terminalPayload, dylibIndex) )
            return true;
    }
    return false;
}

void DylibsPathTrie::forEachDylibPath(void (^callback)(const DylibAndIndex& info, bool& stop)) const
{
    this->forEachEntry(^(const Entry& entry, bool& stop) {
        DylibAndIndex info;
        info.path  = entry.name;
        if ( entryToIndex(entry.terminalPayload, info.index) )
            callback(info, stop);
    });
}


} // namespace mach_o
