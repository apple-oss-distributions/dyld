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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Instructions.h"

namespace mach_o {


bool Instructions::arm64::isADRP(uint32_t instruction, AdrpInfo& info)
{
    if ( (instruction & 0x9F000000) == 0x90000000 ) {
        info.dstReg     = (instruction & 0x1F);
        uint32_t lo2    = (instruction & 0x60000000) >> 29;
        uint32_t hi19   = (instruction & 0x01FFFFE0) >> 3;
        info.pageOffset = (int32_t)(hi19 + lo2);
        if ( info.pageOffset & 0x00100000 )
            info.pageOffset |= 0xFFE00000;  // sign extend 21-bits
        return true;
    }
    return false;
}

bool Instructions::arm64::isB26(uint32_t instruction, int32_t& delta)
{
    if ( (instruction & 0x7C000000) == 0x14000000 ) {
        delta = (int32_t)((instruction & 0x03FFFFFF) << 2);
        if ( delta & 0x08000000 )
            delta |= 0xF0000000;
        return true;
    }
    return false;
}

bool Instructions::arm64::isImm12(uint32_t instruction, Imm12Info& info)
{
    if ( (instruction & 0x3B000000) != 0x39000000 ) {
        // not a load or store
        if ( (instruction & 0x7FC00000) == 0x11000000 ) {
            // is ADD instruction
            info.dstReg   = (instruction & 0x1F);
            info.srcReg   = ((instruction>>5) & 0x1F);
            info.scale    = 1;
            info.offset   = ((instruction >> 10) & 0x0FFF);
            info.kind     = Imm12Info::Kind::add;
            info.signEx   = false;
            info.isFloat  = false;
            return true;
        }
        return false;
    }
    info.dstReg   = (instruction & 0x1F);
    info.srcReg   = ((instruction>>5) & 0x1F);
    info.signEx   = false;
    info.isFloat  = ((instruction & 0x04000000) != 0 );
    switch (instruction & 0xC0C00000) {
        case 0x00000000:
            info.scale   = 1;
            info.kind    = Imm12Info::Kind::store;
            break;
        case 0x00400000:
            info.scale   = 1;
            info.kind    = Imm12Info::Kind::load;
            break;
        case 0x00800000:
            if ( info.isFloat ) {
                info.scale   = 16;
                info.kind    = Imm12Info::Kind::store;
            }
            else {
                info.scale   = 1;
                info.kind    = Imm12Info::Kind::load;
                info.signEx  = true;
            }
            break;
        case 0x00C00000:
            if ( info.isFloat ) {
                info.scale  = 16;
                info.kind   = Imm12Info::Kind::load;
            }
            else {
                info.scale  = 1;
                info.kind   = Imm12Info::Kind::load;
                info.signEx = true;
            }
            break;
        case 0x40000000:
            info.scale   = 2;
            info.kind    = Imm12Info::Kind::store;
            break;
        case 0x40400000:
            info.scale   = 2;
            info.kind    = Imm12Info::Kind::load;
            break;
        case 0x40800000:
            info.scale    = 2;
            info.kind     = Imm12Info::Kind::load;
            info.signEx   = true;
            break;
        case 0x40C00000:
            info.scale    = 2;
            info.kind     = Imm12Info::Kind::load;
            info.signEx   = true;
            break;
        case 0x80000000:
            info.scale   = 4;
            info.kind    = Imm12Info::Kind::store;
            break;
        case 0x80400000:
            info.scale   = 4;
            info.kind    = Imm12Info::Kind::load;
            break;
        case 0x80800000:
            info.scale   = 4;
            info.kind    = Imm12Info::Kind::load;
            info.signEx  = true;
            break;
        case 0xC0000000:
            info.scale   = 8;
            info.kind    = Imm12Info::Kind::store;
            break;
        case 0xC0400000:
            info.scale   = 8;
            info.kind    = Imm12Info::Kind::load;
            break;
        default:
            return false;
    }
    info.offset = ((instruction >> 10) & 0x0FFF) * info.scale;
    return true;
}



} // namespace mach_o
