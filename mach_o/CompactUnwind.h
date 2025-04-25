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


#ifndef mach_o_CompactUnwind_h
#define mach_o_CompactUnwind_h

#include <span>
#include <stdint.h>

#include "MachODefines.h"
#include "Error.h"
#include "Architecture.h"

namespace mach_o {

/*!
 * @class CompactUnwind
 *
 * @abstract
 *      Abstraction `__TEXT,__unwind_info` section
 */
class VIS_HIDDEN CompactUnwind
{
public:
                        // construct from a mach-o __TEXT,__unwind_info section
                        CompactUnwind(Architecture, const uint8_t* start, size_t size);

    struct UnwindInfo { uint32_t funcOffset; uint32_t encoding=0; uint32_t lsdaOffset=0; uint32_t personalityOffset=0; };
    Error               valid() const;
    void                forEachUnwindInfo(void (^callback)(const UnwindInfo&)) const;
    bool                findUnwindInfo(uint32_t funcOffset, UnwindInfo& info) const;
    void                encodingToString(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;

    static uint32_t     compactUnwindEntrySize(bool is64);

protected:
                        // used by the CompactUnwindWriter subclass
                        CompactUnwind() = default;

private:
    Error               forEachFirstLevelTableEntry(void (^callback)(uint32_t funcsStartOffset, uint32_t funcsEndOffset, uint32_t secondLevelOffset, uint32_t lsdaIndexOffset)) const;
    Error               forEachSecondLevelRegularTableEntry(const struct unwind_info_regular_second_level_page_header*, void (^callback)(const UnwindInfo&)) const;
    Error               forEachSecondLevelCompressedTableEntry(const struct unwind_info_compressed_second_level_page_header*, uint32_t pageFunsOffset, void (^callback)(const UnwindInfo&)) const;
    void                encodingToString_arm64(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;
    void                encodingToString_x86_64(uint32_t encoding, const void* funcBytes, char strBuf[128]) const;
    uint32_t            findLSDA(uint32_t funcOffset) const;

protected:
    Architecture                                _arch;
    const struct unwind_info_section_header*    _unwindTable      = nullptr;
    size_t                                      _unwindTableSize  = 0;
};


} // namespace mach_o

#endif // mach_o_CompactUnwind_h
