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

#ifndef mach_o_Symbol_h
#define mach_o_Symbol_h

#include <stdint.h>

#include "Defines.h"
#include "CString.h"

namespace mach_o {

/*!
 * @class Symbol
 *
 * @abstract
 *      Abstraction for symbols in mach-o final linked executables
 */
class VIS_HIDDEN Symbol
{
public:
    Symbol() = default;
    bool    operator==(const Symbol&) const;
    bool    operator!=(const Symbol& other) const { return !operator==(other); }

    enum class Scope: uint8_t { translationUnit, wasLinkageUnit, linkageUnit, autoHide, global, globalNeverStrip };

    CString      name() const                   { return _name; }
    uint64_t     implOffset() const;            // fails for re-exports and absolute
    Scope        scope() const                  { return _scope; }    // global vs local symbol
    bool         isWeakDef() const              { return _weakDef; }
    bool         dontDeadStrip() const          { return _dontDeadStrip; }
    bool         cold() const                   { return _cold; }
    bool         isThreadLocal() const          { return (_kind == Kind::threadLocal); }
    bool         isDynamicResolver(uint64_t& resolverStubOffset) const;
    bool         isReExport(int& libOrdinal, const char*& importName) const;
    bool         isAbsolute(uint64_t& absAddress) const;
    bool         isUndefined() const;
    bool         isUndefined(int& libOrdinal, bool& weakImport) const;
    bool         isRegular(uint64_t& implOffset) const;
    bool         isThreadLocal(uint64_t& implOffset) const;
    bool         isTentativeDef() const;
    bool         isTentativeDef(uint64_t& size, uint8_t& p2align) const;
    uint8_t      sectionOrdinal() const { return _sectOrdinal; }
    bool         isAltEntry(uint64_t& implOffset) const;
    bool         isAltEntry() const { return _kind == Kind::altEntry; }

    void         setName(const char* n);
    void         setimplOffset(uint64_t);
    void         setDontDeadStrip()         { _dontDeadStrip = true; }
    void         setCold()                  { _cold = true; }
    void         setWeakDef()               { _weakDef = true; }
    void         setNotWeakDef()            { _weakDef = false; }

    static Symbol makeRegularExport(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold, bool neverStrip=false);
    static Symbol makeRegularHidden(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeRegularLocal(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeRegularWasPrivateExtern(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeWeakDefAutoHide(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);   // given the current encoding in mach-o, only weak-defs can be auto-hide
    static Symbol makeWeakDefExport(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeAltEntry(CString name, uint64_t imageOffset, uint8_t sectNum, Scope s, bool dontDeadStrip, bool cold, bool weakDef);
    static Symbol makeWeakDefHidden(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeWeakDefWasPrivateExtern(CString name, uint64_t imageOffset, uint8_t sectNum, bool dontDeadStrip, bool cold);
    static Symbol makeDynamicResolver(CString name, uint8_t sectNum, uint64_t stubImageOffset, uint64_t funcImageOffset);
    static Symbol makeThreadLocalExport(CString name, uint64_t imageOffset, uint8_t sectOrd, bool dontDeadStrip, bool cold, bool weakDef);
    static Symbol makeAbsoluteExport(CString name, uint64_t address, bool dontDeadStrip);
    static Symbol makeAbsoluteLocal(CString name, uint64_t address, bool dontDeadStrip);
    static Symbol makeReExport(CString name, int libOrdinal, const char* importName=nullptr, Symbol::Scope=Symbol::Scope::global);
    static Symbol makeUndefined(CString name, int libOrdinal, bool weakImport=false);
    static Symbol makeTentativeDef(CString name, uint64_t size, uint8_t alignP2, bool dontDeadStrip, bool cold);
    static Symbol makeHiddenTentativeDef(CString name, uint64_t size, uint8_t alignP2, bool dontDeadStrip, bool cold);

private:
    Symbol(CString name) : _name(name) { }
    enum class Kind: uint8_t { regular, altEntry, resolver, absolute, reExport, threadLocal, tentative, undefine };
    CString      _name                  = "";
    uint64_t     _implOffset            = 0;   // resolver => offset to stub, re-exports,undefined => libOrdinal, absolute => address, tentative => size
    union {
        const char* importName;
        uint64_t    resolverStubOffset = 0;
    } _u;
    Kind         _kind                  = Kind::regular;
    uint8_t      _sectOrdinal           = 0;
    Scope        _scope                 = Scope::translationUnit;  // global vs local
    bool         _weakDef               = false;  // regular only
    bool         _dontDeadStrip         = false;  // regular only
    bool         _cold                  = false;  // regular only
    bool         _weakImport            = false;  // undefines only
};
static_assert(sizeof(Symbol) == 24+2*sizeof(void*));

} // namespace mach_o

#endif // mach_o_Symbol_h


