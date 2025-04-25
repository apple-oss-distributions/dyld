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
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include "Algorithm.h"

// mach_o
#include "Symbol.h"
#include "Misc.h"

// mach_o_writer
#include "NListSymbolTableWriter.h"

using mach_o::DebugNoteFileInfo;

namespace mach_o {

//
// MARK: --- NListSymbolTable building methods ---
//

uint32_t NListSymbolTableWriter::countDebugNoteNLists(std::span<const DebugBuilderNote> debugNotes)
{
    uint32_t debugStabNlists=0;
    bool startedSO=false;

    for ( const DebugBuilderNote& note : debugNotes ) {
        if ( note.fileInfo->srcDir().empty() && note.fileInfo->srcName().empty() ) {
            debugStabNlists += 1;
        } else {
            if ( !startedSO ) {
                startedSO = true;
                debugStabNlists += 1;
            }
            debugStabNlists += 4;
            if ( note.fileInfo->hasOriginLibInfo() )
                debugStabNlists += 1;

            for ( const DebugBuilderNoteItem& item : note.items ) {
                if ( item.type == N_FUN )
                    debugStabNlists += 4;
                else
                    debugStabNlists += 1;
            }
        }
    }
    return debugStabNlists;
}

template <typename T>
void NListSymbolTableWriter::addStabsFromDebugNotes(std::span<const DebugBuilderNote> debugNotes, bool zeroModTimes, NListBuffer& nlists)
{
    typedef __typeof(T::n_value) V;

    bool startedSOs = false;
    for (const DebugBuilderNote& note : debugNotes) {
        uint32_t mtime = (zeroModTimes ? 0 : note.fileInfo->objModTime());
        if ( note.srcDirPoolOffset == 0 && note.srcNamePoolOffset == 0 ) {
            nlists.add(T{{note.objPathPoolOffset},  N_AST,  0,                       0, (V)mtime});
        }
        else {
            if ( !startedSOs )
                nlists.add(T{{1}, N_SO, 1, 0, 0}); // match ld64 which always started debug notes with an "end SO"
            // Put this before the other N_SO's.  We can't put it right before the N_OSO as lldb expects the N_OSO
            // to be immediately preceded by the N_SO
            if ( note.originLibPathPoolOffset != 0 ) {
                nlists.add(T{{note.originLibPathPoolOffset}, N_LIB, 0, 0, 0});
            }
            startedSOs = true;
            nlists.add(T{{note.srcDirPoolOffset},  N_SO,  0,                           0, 0});
            nlists.add(T{{note.srcNamePoolOffset}, N_SO,  0,                           0, 0});
            nlists.add(T{{note.objPathPoolOffset}, N_OSO, note.fileInfo->objSubType(), 1, (V)mtime});
            for (const DebugBuilderNoteItem& item : note.items) {
                uint32_t stringPoolOffset = item.stringPoolOffset;
                switch ( item.type ) {
                case N_FUN:
                    // for functions, we use four symbols to record the name, address, size, and sectNum
                    nlists.add(T{{1},                                N_BNSYM, item.sectNum, 0, (V)item.addr});
                    nlists.add(T{{stringPoolOffset}, N_FUN,   item.sectNum, 0, (V)item.addr});
                    nlists.add(T{{1},                                N_FUN,   0,            0, (V)item.size});
                    nlists.add(T{{1},                                N_ENSYM, item.sectNum, 0, (V)item.addr});
                    break;
                case N_STSYM:
                    // for static variables, we record the name, address, and sectNum
                    nlists.add(T{{stringPoolOffset}, N_STSYM, item.sectNum, 0, (V)item.addr});
                    break;
                case N_GSYM:
                    // for global variables, we record just the name
                    nlists.add(T{{stringPoolOffset}, N_GSYM,  0,            0, 0});
                    break;
                default:
                    assert(false && "invalid debug note item");
                    break;
                }
            }
            nlists.add(T{{1},                           N_SO, 1, 0, 0});
        }
    }
}


/*!
 * @class NListStringPoolBuffer
 *
 * @abstract
 *      Simple NList string pool buffer, used in unit-tests.
 */
struct NListStringPoolBuffer
{
    std::vector<char> buffer;
    uint32_t          pos = 0;

    NListStringPoolBuffer()
    {
        add(' ');
        add('\0');
    }

    NListStringPoolBuffer(const NListStringPoolBuffer&) = delete;
    NListStringPoolBuffer(NListStringPoolBuffer&&) = default;
    NListStringPoolBuffer& operator=(const NListStringPoolBuffer&) = delete;
    NListStringPoolBuffer& operator=(NListStringPoolBuffer&&) = default;

    uint32_t add(CString str);
    uint32_t add(std::span<const char> bytes);
    uint32_t add(char ch);
    uint32_t size() { return pos; }
    char*    data() { return buffer.data(); }

    void     finalize(bool is64)
    {
        uint32_t pointerSize = is64 ? 8 : 4;
        while ( size() % pointerSize )
            add('\0');
    }

    std::pair<uint32_t, char*> reserve(size_t);
};

NListSymbolTableWriter::SymbolPartition::SymbolPartition(std::span<const Symbol> symbols, bool objectFile)
{
    for (const Symbol& symbol : symbols) {
        int      libOrdinal;
        bool     weakImport;
        uint64_t size;
        uint8_t  p2Align;
        if ( symbol.isUndefined(libOrdinal, weakImport) || symbol.isTentativeDef(size, p2Align) )
            undefs.push_back(symbol);
        else if ( symbol.scope() == Symbol::Scope::global )
            globals.push_back(symbol);
        else if ( (symbol.scope() == Symbol::Scope::linkageUnit) && objectFile )
            globals.push_back(symbol); // in .o files hidden symbols are in globals range
        else if ( (symbol.scope() == Symbol::Scope::autoHide) && objectFile )
            globals.push_back(symbol); // in .o files hidden symbols are in globals range
        else
            locals.push_back(symbol);
    }

    // for historical binary search reasons, globals are sorted by name
    std::sort(globals.begin(), globals.end(), [&](const Symbol& a, const Symbol& b) {
        return a.name() < b.name();
    });
    // undefs are sorted by name
    std::sort(undefs.begin(), undefs.end(), [&](const Symbol& a, const Symbol& b) {
        return a.name() < b.name();
    });
    // locals are already sorted by their position in their section.  We don't need to sort them again
}

NListSymbolTableWriter::NListSymbolTableWriter(std::span<const Symbol> symbols, uint64_t prefLoadAddr, bool is64, std::span<DebugBuilderNote> debugNotes,
                                   bool zeroModTimes, bool objectFile)
    : NListSymbolTableWriter(SymbolPartition(symbols, objectFile), debugNotes, prefLoadAddr, is64, zeroModTimes)
{}


NListSymbolTableWriter::NListSymbolTableWriter(const SymbolPartition& partition, std::span<DebugBuilderNote> debugNotes,
                 uint64_t prefLoadAddr, bool is64, bool zeroModTimes)
    : NListSymbolTableWriter(partition.globals, partition.undefs, partition.locals, debugNotes, prefLoadAddr, is64, zeroModTimes)
{}

NListSymbolTableWriter::NListSymbolTableWriter(std::span<const Symbol> globals, std::span<const Symbol> undefs,
                                               std::span<const Symbol> locals, std::span<DebugBuilderNote> debugNotes,
                                               uint64_t prefLoadAddr, bool is64, bool zeroModTimes)
    : NListSymbolTable()
{
    uint32_t numDebugNlist = countDebugNoteNLists(debugNotes);
    size_t nlistSize = (locals.size() + globals.size() + undefs.size() + numDebugNlist) * (is64 ? sizeof(nlist_64) : sizeof(struct nlist));

    NListStringPoolBuffer stringPoolBuffer;

    size_t strxAllCount = globals.size() * 2 + locals.size() + undefs.size();
    std::vector<uint32_t> strxAll(strxAllCount);
    std::span<uint32_t> globalsStrx = std::span(strxAll).subspan(0, globals.size());
    std::span<uint32_t> reexportsStrx = std::span(globalsStrx.end().base(), globals.size());
    std::span<uint32_t> undefsStrx = std::span(reexportsStrx.end().base(), undefs.size());
    std::span<uint32_t> localsStrx = std::span(undefsStrx.end().base(), locals.size());
    for ( size_t i = 0; i < globals.size(); ++i ) {
        const Symbol& s = globals[i];
        globalsStrx[i] = stringPoolBuffer.add(s.name());
        int32_t ordinal;
        const char* importName=nullptr;
        if ( s.isReExport(ordinal, importName) )
            reexportsStrx[i] = stringPoolBuffer.add(s.name());
    }
    for ( size_t i = 0; i < undefs.size(); ++i ) {
        const Symbol& s = undefs[i];
        undefsStrx[i] = stringPoolBuffer.add(s.name());
    }
    for ( size_t i = 0; i < locals.size(); ++i ) {
        const Symbol& s = locals[i];
        localsStrx[i] = stringPoolBuffer.add(s.name());
    }

    for ( DebugBuilderNote& debugNote : debugNotes ) {
        if ( CString srcDir = debugNote.fileInfo->srcDir(); !srcDir.empty() )
            debugNote.srcDirPoolOffset = stringPoolBuffer.add(srcDir);
        if ( CString srcName = debugNote.fileInfo->srcName(); !srcName.empty() )
            debugNote.srcNamePoolOffset = stringPoolBuffer.add(srcName);
        if ( CString originLibPath = debugNote.fileInfo->originLibPath(); !originLibPath.empty() )
            debugNote.originLibPathPoolOffset=stringPoolBuffer.add(originLibPath);
        if ( CString objPath = debugNote.fileInfo->objPath(); !objPath.empty() )
            debugNote.objPathPoolOffset=stringPoolBuffer.add(objPath);

        for ( DebugBuilderNoteItem& item : debugNote.items ) {
            item.stringPoolOffset=stringPoolBuffer.add(item.name);
        }
    }
    stringPoolBuffer.finalize(is64);

    *this = NListSymbolTableWriter(NListLayout{ globals, globalsStrx, reexportsStrx, undefs, undefsStrx, locals, localsStrx, debugNotes, numDebugNlist }, NListBuffer(nlistSize), std::move(stringPoolBuffer.buffer), prefLoadAddr, is64, zeroModTimes);
}

NListSymbolTableWriter::NListSymbolTableWriter(NListLayout layout, std::span<uint8_t> nlistBuffer, uint64_t prefLoadAddr, bool is64, bool zeroModTimes): NListSymbolTableWriter(layout, NListBuffer(nlistBuffer), {}, prefLoadAddr, is64, zeroModTimes) {}

NListSymbolTableWriter::NListSymbolTableWriter(NListLayout layout, NListBuffer nlist, std::vector<char> stringPoolBuffer, uint64_t prefLoadAddr, bool is64, bool zeroModTimes)
    : _nlistBuffer(std::move(nlist)), _stringPoolBuffer(std::move(stringPoolBuffer))
{
    // partition symbols into locals, globals, and undefs
    _localsCount    = (uint32_t)layout.locals.size() + layout.debugNotesNListCount;
    _globalsCount   = (uint32_t)layout.globals.size();
    _undefsCount    = (uint32_t)layout.undefs.size();
    _nlistCount     = _localsCount + _globalsCount + _undefsCount;

    assert(layout.globals.size()   == layout.globalsStrx.size());
    assert(layout.globals.size()   == layout.reexportStrx.size());
    assert(layout.undefs.size()    == layout.undefsStrx.size());
    assert(layout.locals.size()    == layout.localsStrx.size());
    std::span<uint8_t> nlistBuffer = _nlistBuffer.buffer;

    // convert each symbol to nlist
    _preferredLoadAddress = prefLoadAddr;
    if ( is64 ) {
        assert(nlistBuffer.size() == (_localsCount + _globalsCount + _undefsCount) * sizeof(nlist_64));

        // symbol table strings are added in the order of globals, imports, locals
        // but the nlist itself is emitted as locals, globals, imports.
        // So we'll walk in the string order, and then create the nlist after

        std::span<nlist_64> nlist64Buffer = std::span<nlist_64>((nlist_64*)nlistBuffer.data(), nlistBuffer.size() / sizeof(nlist_64));
        std::span<nlist_64> globalsBuffer(nlist64Buffer.subspan(_localsCount, _globalsCount));
        std::span<nlist_64> undefsBuffer(nlist64Buffer.subspan(_localsCount + _globalsCount, _undefsCount));
        std::span<nlist_64> localsBuffer(nlist64Buffer.subspan(0, _localsCount));

        dispatchForEach(layout.globals, [this, globalsBuffer, &layout](size_t i, const Symbol& sym) {
            globalsBuffer[i] = nlist64FromSymbol(sym, layout.globalsStrx[i], layout.reexportStrx[i]);
        });
        dispatchForEach(layout.undefs, [this, undefsBuffer, &layout](size_t i, const Symbol& sym) {
            undefsBuffer[i] = nlist64FromSymbol(sym, layout.undefsStrx[i], 0);
        });
        dispatchForEach(layout.locals, [this, localsBuffer, &layout](size_t i, const Symbol& sym) {
            localsBuffer[i] = nlist64FromSymbol(sym, layout.localsStrx[i], 0);
        });

        NListBuffer stabsBuffer = localsBuffer.subspan(layout.locals.size());
        assert((stabsBuffer.buffer.size() / sizeof(nlist_64)) == layout.debugNotesNListCount);
        addStabsFromDebugNotes<nlist_64>(layout.debugNotes, zeroModTimes, stabsBuffer);
    }
    else {
        // symbol table strings are added in the order of globals, imports, locals
        // but the nlist itself is emitted as locals, globals, imports.
        // So we'll walk in the string order, and then create the nlist after

        std::span<struct nlist> nlist32Buffer = std::span<struct nlist>((struct nlist*)nlistBuffer.data(), nlistBuffer.size() / sizeof(struct nlist));
        std::span<struct nlist> globalsBuffer(nlist32Buffer.subspan(_localsCount, _globalsCount));
        std::span<struct nlist> undefsBuffer(nlist32Buffer.subspan(_localsCount + _globalsCount, _undefsCount));
        std::span<struct nlist> localsBuffer(nlist32Buffer.subspan(0, _localsCount));

        dispatchForEach(layout.globals, [this, globalsBuffer, &layout](size_t i, const Symbol& sym) {
            globalsBuffer[i] = nlistFromSymbol(sym, layout.globalsStrx[i], layout.reexportStrx[i]);
        });
        dispatchForEach(layout.undefs, [this, undefsBuffer, &layout](size_t i, const Symbol& sym) {
            undefsBuffer[i] = nlistFromSymbol(sym, layout.undefsStrx[i], 0);
        });
        dispatchForEach(layout.locals, [this, localsBuffer, &layout](size_t i, const Symbol& sym) {
            localsBuffer[i] = nlistFromSymbol(sym, layout.localsStrx[i], 0);
        });

        NListBuffer stabsBuffer = localsBuffer.subspan(layout.locals.size());
        assert((stabsBuffer.buffer.size() / sizeof(struct nlist)) == layout.debugNotesNListCount);
        addStabsFromDebugNotes<struct nlist>(layout.debugNotes, zeroModTimes, stabsBuffer);
    }

    // fill in all ivars as if this came from a mach-o file
    _preferredLoadAddress = prefLoadAddr;
    _stringPool           = _stringPoolBuffer.data();
    _nlist32              = is64 ? nullptr : (struct nlist*)nlistBuffer.data();
    _nlist64              = is64 ? (nlist_64*)nlistBuffer.data() : nullptr;
    _stringPoolSize       = (uint32_t)_stringPoolBuffer.size();
}

static uint8_t ntypeFromSymbol(const Symbol& symbol)
{
    switch ( symbol.scope() ) {
        case Symbol::Scope::global:
        case Symbol::Scope::globalNeverStrip:
        case Symbol::Scope::autoHide:
            return N_EXT;
        case Symbol::Scope::linkageUnit:
            return N_EXT | N_PEXT;
        case Symbol::Scope::translationUnit:
            return 0;
        case Symbol::Scope::wasLinkageUnit:
            return N_PEXT;
   }
}

static uint16_t weakDefDesc(const Symbol& symbol)
{
    uint16_t desc = 0;
    if ( symbol.isWeakDef() ) {
        switch ( symbol.scope() ) {
            case Symbol::Scope::globalNeverStrip:
            case Symbol::Scope::global:
            case Symbol::Scope::linkageUnit:
            case Symbol::Scope::wasLinkageUnit:
                desc = N_WEAK_DEF;
                break;
            case Symbol::Scope::autoHide:
                desc = N_WEAK_DEF | N_WEAK_REF;
                break;
            case Symbol::Scope::translationUnit:
                break;
        }
    }
    return desc;
}

struct nlist_64 NListSymbolTableWriter::nlist64FromSymbol(const Symbol& symbol, uint32_t strx, uint32_t reexportStrx)
{
    struct nlist_64 result;
    int             libOrdinal;
    uint64_t        absAddress;
    bool            weakImport;
    uint64_t        implOffset;
    uint64_t        size;
    uint64_t        stubOffset;
    uint8_t         p2align;
    const char*     importName;
    uint32_t        fvtIndex;
    if ( symbol.isTentativeDef(size, p2align) ) {
        result.n_un.n_strx = strx;
        result.n_type      = N_UNDF | ntypeFromSymbol(symbol);
        result.n_sect      = 0;
        result.n_desc      = 0;
        result.n_value     = size;
        SET_COMM_ALIGN(result.n_desc,p2align);
    }
    else if ( symbol.isUndefined(libOrdinal, weakImport) ) {
        result.n_un.n_strx = strx;
        result.n_type      = N_UNDF | N_EXT;
        result.n_sect      = 0;
        result.n_desc      = (libOrdinal << 8) | (weakImport ? N_WEAK_REF : 0);
        result.n_value     = 0;
    }
    else if ( symbol.isAbsolute(absAddress) )  {
        uint16_t desc = 0;
        if ( symbol.scope() == Symbol::Scope::globalNeverStrip )
            desc |= REFERENCED_DYNAMICALLY;

        result.n_un.n_strx = strx;
        result.n_type      = N_ABS | ntypeFromSymbol(symbol);
        result.n_sect      = symbol.sectionOrdinal();
        result.n_desc      = desc;
        result.n_value     = absAddress;
    }
    else if ( symbol.isRegular(implOffset) || symbol.isThreadLocal(implOffset) ) {
        uint16_t desc = weakDefDesc(symbol);
        if ( symbol.dontDeadStrip() )
            desc |= N_NO_DEAD_STRIP;
        if ( symbol.cold() )
            desc |= N_COLD_FUNC;
        if ( symbol.scope() == Symbol::Scope::globalNeverStrip )
            desc |= REFERENCED_DYNAMICALLY;
        if ( symbol.isThumb() )
            desc |= N_ARM_THUMB_DEF;
        result.n_un.n_strx = strx;
        result.n_type      = N_SECT | ntypeFromSymbol(symbol);
        result.n_sect      = symbol.sectionOrdinal();
        result.n_desc      = desc;
        result.n_value     = _preferredLoadAddress + implOffset;
    }
    else if ( symbol.isFunctionVariant(fvtIndex) ) {
        // Note:  currently cannot use no-dead-strip, cold, or dynamically-reference with funtion variants
        result.n_un.n_strx = strx;
        result.n_type      = N_SECT | ntypeFromSymbol(symbol);
        result.n_sect      = symbol.sectionOrdinal();
        result.n_desc      = 0;
        result.n_value     = _preferredLoadAddress + symbol.implOffset();
    }
    else if ( symbol.isAltEntry(implOffset) ) {
        uint64_t desc = N_ALT_ENTRY | weakDefDesc(symbol);
        if ( symbol.dontDeadStrip() )
            desc |= N_NO_DEAD_STRIP;
        result.n_un.n_strx = strx;
        result.n_type      = N_SECT | ntypeFromSymbol(symbol);
        result.n_sect      = symbol.sectionOrdinal();
        result.n_desc      = desc;
        result.n_value     = _preferredLoadAddress + implOffset;
    }
    else if ( symbol.isReExport(libOrdinal, importName) ) {
        // re-exports can't be local, they're always global in linked images,
        // in object files they can have global/linkage unit scope or be undefined.
        assert(symbol.scope() != Symbol::Scope::translationUnit && "re-exports can't have a translation unit");
        result.n_un.n_strx = strx;
        result.n_type      = N_INDR | ntypeFromSymbol(symbol);
        result.n_sect      = 0;
        result.n_desc      = 0;
        result.n_value     = reexportStrx;
    }
    else if ( symbol.isDynamicResolver(stubOffset) ) {
        result.n_un.n_strx = strx;
        result.n_type      = N_SECT | ntypeFromSymbol(symbol);
        result.n_sect      = symbol.sectionOrdinal();
        result.n_desc      = N_SYMBOL_RESOLVER;
        result.n_value     = _preferredLoadAddress + symbol.implOffset();
    }
    else {
        assert(false && "unhandled symbol kind");
    }

    return result;
}

// avoid duplicating code by filling in nlist_64 and converting to nlist
struct nlist NListSymbolTableWriter::nlistFromSymbol(const Symbol& symbol, uint32_t strx, uint32_t reexportStrx)
{
    struct nlist_64 result64 = nlist64FromSymbol(symbol, strx, reexportStrx);
    struct nlist result;
    result.n_un.n_strx = result64.n_un.n_strx;
    result.n_type      = result64.n_type;
    result.n_sect      = result64.n_sect;
    result.n_desc      = result64.n_desc;
    result.n_value     = (uint32_t)result64.n_value;
    return result;
}

std::pair<uint32_t, char*> NListStringPoolBuffer::reserve(size_t size)
{
    size_t startPos = pos;
    pos += size;
    buffer.resize(buffer.size() + size);
    return std::make_pair(startPos, buffer.data() + startPos);
}

uint32_t NListStringPoolBuffer::add(std::span<const char> bytes)
{
    auto [startPos, ptr] = reserve(bytes.size());
    memcpy(ptr, bytes.data(), bytes.size());
    return startPos;
}

uint32_t NListStringPoolBuffer::add(char ch)
{
    auto [startPos, ptr] = reserve(1);
    *ptr = ch;
    return startPos;
}

uint32_t NListStringPoolBuffer::add(CString cstr)
{
    return add(std::span(cstr.c_str(), cstr.size() + 1));
}

} // namespace mach_o
