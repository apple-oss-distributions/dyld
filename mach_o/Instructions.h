/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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


#ifndef mach_o_Instructions_h
#define mach_o_Instructions_h

#include <stdint.h>

#include "Defines.h"


namespace mach_o {




/*!
 * @class Instructions
 *
 * @abstract
 *      Helpers to parse instructions
 */
struct VIS_HIDDEN Instructions
{
    /*!
     * @class arm64
     *
     * @abstract
     *      Helpers to parse arm64 instructions
     */
    class VIS_HIDDEN arm64
    {
    public:

        struct Imm12Info
        {
            enum class Kind          { add, load, store };

            uint8_t         dstReg;
            uint8_t         srcReg;
            uint8_t         scale;      // 1,2,4,8, or 16
            uint32_t        offset;     // imm12 after scaling
            Kind            kind;
            bool            signEx;     // if load is sign extended
            bool            isFloat;    // if destReg is FP/SIMD
        };

        struct AdrpInfo
        {
            uint8_t         dstReg;
            int32_t         pageOffset;
        };

        static bool isImm12(uint32_t instruction, Imm12Info& info);
        static bool isADRP(uint32_t instruction, AdrpInfo& info);
        static bool isB26(uint32_t instruction, int32_t& delta);
    };

};


} // namespace mach_o

#endif /* mach_o_Instructions_h */
