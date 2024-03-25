/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_Fixups_h
#define mach_o_Fixups_h

#include "CString.h"

#include "Defines.h"

namespace mach_o {



struct MappedSegment
{
    uint64_t            runtimeOffset;
    uint64_t            runtimeSize;
    void*               content;
    std::string_view    segName;
    bool                readable;
    bool                writable;
    bool                executable;
};



/*!
 * @class Fixup
 *
 * @abstract
 *      Class for encapsulating everything about a fixup.
 *
 *
 */
struct Fixup
{
    const void*             location;
    const MappedSegment*    segment;
    bool                    authenticated = false;
    struct {
        uint8_t         key               :  2 = 0,
                        usesAddrDiversity :  1 = 0;
        uint16_t        diversity              = 0;
    }  auth;
    bool                    isBind;
    bool                    isLazyBind; 
    union {
        struct {
            uint32_t    bindOrdinal;   // index into BindTarget array
            int32_t     embeddedAddend;
        } bind;
        struct {
            uint64_t    targetVmOffset; // includes high8
        } rebase;
    };

    bool            operator==(const Fixup& other) const;
    bool            operator!=(const Fixup& other) const { return !this->operator==(other); }
    bool            operator<(const Fixup& other) const  { return (this->location < other.location); }
    const char*     keyName() const;
    
    struct BindTarget
    {
        CString     symbolName;
        int         libOrdinal  = 0;
        bool        weakImport  = false;
        int64_t     addend      = 0;
    };

    // constructor for a non-auth bind
    Fixup(const void* loc, const MappedSegment* seg, uint32_t bindOrdinal, int32_t embeddedAddend, bool lazy=false)
        : location(loc), segment(seg), isBind(true), isLazyBind(lazy)
    {
        bind.bindOrdinal    = bindOrdinal;
        bind.embeddedAddend = embeddedAddend;
    }

    // constructor for a non-auth rebase
    Fixup(const void* loc, const MappedSegment* seg, uint64_t targetVmOffset)
        : location(loc), segment(seg), isBind(false), isLazyBind(false)
    {
        rebase.targetVmOffset = targetVmOffset;
    }

    // constructor for an auth bind
    Fixup(const void* loc, const MappedSegment* seg, uint32_t bindOrdinal, int32_t embeddedAddend, uint8_t key, bool usesAD, uint16_t div)
        : location(loc), segment(seg), isBind(true), isLazyBind(false)
    {
        bind.bindOrdinal           = bindOrdinal;
        bind.embeddedAddend        = embeddedAddend;
        authenticated              = true;
        auth.key                   = key;
        auth.usesAddrDiversity     = usesAD;
        auth.diversity             = div;
    }

    // constructor for an auth rebase
    Fixup(const void* loc, const MappedSegment* seg, uint64_t targetVmOffset, uint8_t key, bool usesAD, uint16_t div)
        : location(loc), segment(seg), isBind(false), isLazyBind(false)
    {
        rebase.targetVmOffset   = targetVmOffset;
        authenticated           = true;
        auth.key                = key;
        auth.usesAddrDiversity  = usesAD;
        auth.diversity          = div;
    }


};




} // namespace mach_o

#endif /* mach_o_Fixups_h */
