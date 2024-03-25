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

#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <mach-o/nlist.h>

#include "Symbol.h"


namespace mach_o {


//
// MARK: --- Symbol methods ---
//
Symbol Symbol::makeRegularExport(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold, bool neverStrip)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectNum;
    symbol._scope       = neverStrip ? Scope::globalNeverStrip : Scope::global;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeRegularHidden(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectNum;
    symbol._scope       = Scope::linkageUnit;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeRegularLocal(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectNum;
    symbol._scope       = Scope::translationUnit;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeRegularWasPrivateExtern(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectNum;
    symbol._scope       = Scope::wasLinkageUnit;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeWeakDefExport(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectOrd;
    symbol._scope       = Scope::global;
    symbol._weakDef     = true;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeWeakDefAutoHide(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectOrd;
    symbol._scope       = Scope::autoHide;
    symbol._weakDef     = true;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeWeakDefHidden(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectOrd;
    symbol._scope       = Scope::linkageUnit;
    symbol._weakDef     = true;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeWeakDefWasPrivateExtern(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind        = Kind::regular;
    symbol._sectOrdinal = sectOrd;
    symbol._scope       = Scope::wasLinkageUnit;
    symbol._weakDef     = true;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeAltEntry(CString name, uint64_t imageOffset, uint8_t sectOrd, Scope scope, bool dontDeadStrip, bool cold, bool weakDef)
{
    Symbol symbol(name);
    symbol._kind        = Kind::altEntry;
    symbol._sectOrdinal = sectOrd;
    symbol._scope       = scope;
    symbol._weakDef     = weakDef;
    symbol._implOffset  = imageOffset;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeDynamicResolver(CString name, uint8_t sectNum, uint64_t stubImageOffset, uint64_t funcImageOffset)
{
    // FIXME: do we need to support non-exported resolver functions?
    Symbol symbol(name);
    symbol._kind        = Kind::resolver;
    symbol._scope       = Scope::global;
    symbol._sectOrdinal = sectNum;
    symbol._implOffset  = funcImageOffset;
    symbol._u.resolverStubOffset = stubImageOffset;
    return symbol;
}

Symbol Symbol::makeThreadLocalExport(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold, bool weakDef)
{
    Symbol symbol(name);
    symbol._kind        = Kind::threadLocal;
    symbol._scope       = Scope::global;
    symbol._sectOrdinal = sectOrd;
    symbol._implOffset  = imageOffset;
    symbol._weakDef     = weakDef;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeAbsoluteExport(CString name, uint64_t address, bool dontDeadStrip)
{
    Symbol symbol(name);
    symbol._kind        = Kind::absolute;
    symbol._scope       = Scope::global;
    symbol._implOffset  = address;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    return symbol;
}

Symbol Symbol::makeAbsoluteLocal(CString name, uint64_t address, bool dontDeadStrip)
{
    Symbol symbol(name);
    symbol._kind        = Kind::absolute;
    symbol._scope       = Scope::translationUnit;
    symbol._implOffset  = address;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    return symbol;
}

Symbol Symbol::makeReExport(CString name, int libOrdinal, const char* importName, Symbol::Scope scope)
{
    Symbol symbol(name);
    symbol._kind         = Kind::reExport;
    symbol._scope        = scope;
    symbol._implOffset   = libOrdinal;
    symbol._u.importName = importName;
    return symbol;
}

Symbol Symbol::makeUndefined(CString name, int libOrdinal, bool weakImport)
{
    Symbol symbol(name);
    symbol._kind         = Kind::undefine;
    symbol._scope        = Scope::global;
    symbol._implOffset   = libOrdinal;
    symbol._weakImport   = weakImport;
    return symbol;
}

Symbol Symbol::makeTentativeDef(CString name, uint64_t size, uint8_t alignP2, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind         = Kind::tentative;
    symbol._scope        = Scope::global;
    symbol._sectOrdinal  = alignP2;         // sectOrdinal is not used for tent-def, so stuff alignment there
    symbol._implOffset   = size;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

Symbol Symbol::makeHiddenTentativeDef(CString name, uint64_t size, uint8_t alignP2, bool dontDeadStrip, bool cold)
{
    Symbol symbol(name);
    symbol._kind         = Kind::tentative;
    symbol._scope        = Scope::linkageUnit;
    symbol._sectOrdinal  = alignP2;        // sectOrdinal is not used for tent-def, so stuff alignment there
    symbol._implOffset   = size;
    if ( dontDeadStrip )
        symbol.setDontDeadStrip();
    if ( cold )
        symbol.setCold();
    return symbol;
}

bool Symbol::operator==(const Symbol& other) const
{
    if ( _name != other._name )
        return false;
    if ( _implOffset != other._implOffset )
        return false;
    if ( _kind != other._kind )
        return false;
    if ( _scope != other._scope )
        return false;
    if ( _weakDef != other._weakDef )
        return false;
    if ( _kind == Kind::reExport ) {
        if ( (_u.importName != nullptr) && (other._u.importName != nullptr) && (strcmp(_u.importName, other._u.importName) != 0) )
            return false;
        if ( _u.importName != other._u.importName )
            return false;
    }
    else if ( _kind == Kind::resolver ) {
        if ( _u.resolverStubOffset != other._u.resolverStubOffset )
            return false;
    }
    return true;
}


uint64_t Symbol::implOffset() const
{
    assert((_kind != Kind::reExport) && (_kind != Kind::absolute));
    return _implOffset;
}

bool Symbol::isDynamicResolver(uint64_t& resolverStubOffset) const
{
    if ( _kind != Kind::resolver )
        return false;
    resolverStubOffset = _u.resolverStubOffset;
    return true;
}

bool Symbol::isReExport(int& libOrdinal, const char*& importName) const
{
    if ( _kind != Kind::reExport )
        return false;
    libOrdinal = (int)_implOffset;
    importName = (_u.importName != nullptr) ? _u.importName : _name.c_str();
    return true;
}

bool Symbol::isAbsolute(uint64_t& absAddress) const
{
    if ( _kind != Kind::absolute )
        return false;
    absAddress = _implOffset;
    return true;
}

bool Symbol::isUndefined() const
{
    return _kind == Kind::undefine;
}

bool Symbol::isUndefined(int& libOrdinal, bool& weakImport) const
{
    if ( _kind != Kind::undefine )
        return false;
    libOrdinal = (int)_implOffset;
    weakImport = _weakImport;
    return true;
}

bool Symbol::isRegular(uint64_t& implOffset) const
{
    if ( _kind != Kind::regular )
        return false;
    implOffset = _implOffset;
    return true;
}

bool Symbol::isThreadLocal(uint64_t& implOffset) const
{
    if ( _kind != Kind::threadLocal )
        return false;
    implOffset = _implOffset;
    return true;
}

bool Symbol::isAltEntry(uint64_t& implOffset) const
{
    if ( _kind != Kind::altEntry )
        return false;
    implOffset = _implOffset;
    return true;
}

bool Symbol::isTentativeDef() const
{
    return _kind == Kind::tentative;
}

bool Symbol::isTentativeDef(uint64_t& size, uint8_t& p2align) const
{
    if ( _kind != Kind::tentative )
        return false;
    size    =_implOffset;
    p2align = _sectOrdinal;    // sectOrdinal not used by tent-def, so alignment was stored there
    return true;
}

void Symbol::setName(const char* newName)
{
    _name = newName;
}

void Symbol::setimplOffset(uint64_t newOffset)
{
    _implOffset = newOffset;
}


} // namespace mach_o
