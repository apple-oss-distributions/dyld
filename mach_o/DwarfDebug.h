/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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


#ifndef mach_o_DwarfDebug_h
#define mach_o_DwarfDebug_h

#include <span>
#include <stdint.h>

#include "Defines.h"
#include "Error.h"
#include "Architecture.h"

namespace mach_o {

/*!
 * @class DwarfDebug
 *
 * @abstract
 *      parses info from __DWARF sections
 */
class VIS_HIDDEN DwarfDebug
{
public:
                        // construct from a mach-o __DWARF,__debug* sections
                        DwarfDebug(std::span<const uint8_t> debugInfo, std::span<const uint8_t> abbrev,
                                   std::span<const uint8_t> strings, std::span<const uint8_t> stringOffs);
    const char*         sourceFileDir() const  { return _tuDir; }
    const char*         sourceFileName() const { return _tuFileName; }

private:
    void                parseCompilationUnit();
    const char*         getDwarfString(uint64_t form, const uint8_t*& di, bool dwarf64);
    const char*         getStrxString(uint64_t idx, bool dwarf64);
    bool                skip_form(const uint8_t*& offset, const uint8_t* end, uint64_t form, uint8_t addr_size, bool dwarf64);

    std::span<const uint8_t> _debugInfo;
    std::span<const uint8_t> _abbrev;
    std::span<const uint8_t> _strings;
    std::span<const uint8_t> _stringOffsets;
    const char*              _tuDir       = nullptr;
    const char*              _tuFileName  = nullptr;
};


} // namespace mach_o

#endif // mach_o_DwarfDebug_h
