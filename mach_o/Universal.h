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


#ifndef mach_o_Universal_h
#define mach_o_Universal_h

#include <stdint.h>
#include <mach-o/fat.h>

#include <span>

#include "Architecture.h"
#include "GradedArchitectures.h"
#include "Header.h"

namespace mach_o {


/*!
 * @class Universal
 *
 * @abstract
 *      Abstraction for fat files
 */
struct VIS_HIDDEN Universal
{
    struct Slice
    {
        Architecture arch;
        std::span<const uint8_t> buffer;
    };

    // for examining universal files
    static const Universal* isUniversal(std::span<const uint8_t> fileContent);
    Error                   valid(uint64_t fileSize) const;
    void                    forEachSlice(void (^callback)(Slice slice, bool& stop)) const;
    bool                    bestSlice(const GradedArchitectures& ga, bool osBinary, Slice& slice) const;
    const char*             archNames(char strBuf[256]) const;
    const char*             archAndPlatformNames(char strBuf[512]) const;

#if BUILDING_MACHO_WRITER
    // for building
    static const Universal* make(std::span<const Header*>, bool forceFat64=false, bool arm64offEnd=false);

    static const Universal* make(std::span<const Slice>, bool forceFat64=false, bool arm64offEnd=false);
    void                    save(char savedPath[PATH_MAX]) const;
    uint64_t                size() const;
    void                    free() const;   // only called on object allocated by make()
#endif

private:
    Error                   validSlice(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceLen) const;
    void                    forEachSlice(void (^callback)(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop)) const;

                            Universal();
    void                    addMachO(const Header*);
    enum { kMaxSliceCount = 16 };


    alignas(4096) fat_header   fh;
};


} // namespace mach_o

#endif /* mach_o_Universal_h */
