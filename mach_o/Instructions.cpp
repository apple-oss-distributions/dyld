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
#include <stdlib.h>

#include "Instructions.h"

namespace mach_o {


//
// MARK: --- arm64 methods ---
//

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

bool Instructions::arm64::setADRPTarget(uint32_t& instruction, uint64_t instructionAddr, uint64_t targetAddr)
{
    if ( (instruction & 0x9F000000) != 0x90000000 )
        return false;

    int64_t  delta = (targetAddr & (-4096)) - (instructionAddr & (-4096));
    const int64_t fourGBLimit  = 0xFFFFF000;
    if ( (delta > fourGBLimit) || (delta < (-fourGBLimit)) )
        return false;

    uint32_t immhi  = (delta >> 9) & (0x00FFFFE0);
    uint32_t immlo  = (delta << 17) & (0x60000000);
    instruction = (instruction & 0x9F00001F) | immlo | immhi;
    return true;
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

bool Instructions::arm64::setB26Target(uint32_t& instruction, uint64_t instructionAddr, uint64_t targetAddr)
{
    if ( (instruction & 0x7C000000) != 0x14000000 )
        return false;

    int64_t delta = targetAddr - instructionAddr;
    const int64_t bl_128MegLimit = 0x07FFFFFF;
    if ( abs(delta) > bl_128MegLimit )
        return false;

    uint32_t imm26 = (delta >> 2) & 0x03FFFFFF;
    instruction = (instruction & 0xFC000000) | imm26;
    return true;
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

bool Instructions::arm64::setImm12(uint32_t& instruction, uint16_t imm12)
{
    Imm12Info info;
    if ( !isImm12(instruction, info) )
        return false;

    instruction = (instruction & 0xFFC003FF) | ((uint32_t)imm12 << 10);
    return true;
}

bool Instructions::arm64::changeLDRtoADD(uint32_t& instruction, uint16_t imm12)
{
    uint32_t masked = (instruction & 0xFFC00000);
    if ( (masked != 0xF9400000) && (masked != 0xB9400000) && (masked != 0x91000000) )
        return false; // not an LDR

    instruction = 0x91000000 | ((uint32_t)imm12 << 10) | (instruction & 0x000003FF);
    return true;
}


//
// MARK: --- arm methods ---
//


bool Instructions::arm::isBranch24(uint32_t instruction, uint32_t instructionAddr, BranchKind& kind, uint32_t& targetAddr)
{
    // NOTE: b and bl can have 4-bit condition, but blx cannot.
    // we do not support conditions because you cannot transform bl to blx
    if ( (instruction & 0xFF000000) == 0xEB000000 )
        kind = bl;
    else if ( (instruction & 0xFE000000) == 0xFA000000 )
        kind = blx;
    else if ( (instruction & 0x0F000000) == 0x0A000000 )
        kind = b;
    else
        return false;

    uint32_t    imm24    = (instruction & 0x00FFFFFF) << 2;
    int32_t     delta    = imm24;
    if ( imm24 & 0x02000000 )
        delta |= 0xFC000000;    // sign extend

    // If this is BLX, H bit in instruction is used to branch to thumb instructions that start on 2-byte address
    if ( (kind == blx) && (instruction & 0x01000000) )
        delta += 2;

    targetAddr = instructionAddr + 8 + delta;    // in B/BL/BLX pc-rel base is 8-bytes from start of instruction
    return true;
}

bool Instructions::arm::isThumbBranch22(uint32_t instruction, uint32_t instructionAddr, BranchKind& kind, uint32_t& targetAddr)
{
    if ( (instruction & 0xD000F800) == 0xD000F000 )
        kind = bl;
    else if ( (instruction & 0xD000F800) == 0xC000F000 )
        kind = blx;
    else if ( (instruction & 0xD000F800) == 0x9000F000 )
        kind = b;
    else
        return false;

    // decode instruction
    uint32_t    s       = (instruction >> 10) & 0x1;
    uint32_t    j1      = (instruction >> 29) & 0x1;
    uint32_t    j2      = (instruction >> 27) & 0x1;
    uint32_t    imm10   = instruction & 0x3FF;
    uint32_t    imm11   = (instruction >> 16) & 0x7FF;
    uint32_t    i1      = (j1 == s);
    uint32_t    i2      = (j2 == s);
    uint32_t    undelta = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
    int32_t     delta   = undelta;
    if ( s )
        delta |= 0xFE000000; // sign extend

    // For branches, the delta added will be +4 from the pc
    targetAddr  = instructionAddr + 4 + delta;

    // If the instruction was blx, force the low 2 bits to be clear
    if ( kind == blx )
        targetAddr &= 0xFFFFFFFC;

    return true;
}

bool Instructions::arm::makeBranch24(BranchKind kind, uint64_t instructionAddr, uint64_t targetAddr, bool targetIsThumb, uint32_t& instruction)
{
    int64_t       delta     = targetAddr - (instructionAddr+8); // pcrel to start of BL instruction + 8
    const int64_t b24Limit  = 0x01FFFFFF;
    if ( (delta > b24Limit) || (delta < (-b24Limit)) )
        return false;

    if ( kind != b ) {
        if ( targetIsThumb ) {
            uint32_t opcode = 0xFA000000;  // blx
            uint32_t disp   = (uint32_t)(delta >> 2) & 0x00FFFFFF;
            uint32_t h_bit  = (uint32_t)(delta << 23) & 0x01000000;
            instruction     = opcode | h_bit | disp;
        }
        else {
            uint32_t opcode = 0xEB000000;  //  bl
            uint32_t disp   = (uint32_t)(delta >> 2) & 0x00FFFFFF;
            instruction     = opcode | disp;
        }
        return true;
    }
    else if ( targetIsThumb ) {
        return false; // can't branch from ARM to THUMB
    }
    else {
        // simple arm-to-arm branch
        uint32_t opcode = 0xEA000000;  // b
        instruction     = (opcode & 0xFF000000) | ((uint32_t)(delta >> 2) & 0x00FFFFFF);
        return true;
    }
}


bool Instructions::arm::makeThumbBranch22(BranchKind kind, uint64_t instructionAddr, uint64_t targetAddr, bool targetIsThumb, uint32_t& instruction)
{
    int64_t delta = targetAddr - (instructionAddr + 4); // pcrel to start of BL instruction + 4
    const int64_t b22Limit  = 0x00FFFFFF;               // Note: thumb1 has only a +/-4MB range.  We only support thumb2 which has a +/-16MB branch range
    if ( (delta > b22Limit) || (delta < (-b22Limit)) )
        return false;

    // The instruction is really two 16-bit instructions:
    // The first instruction contains the high 11 bits of the displacement.
    // The second instruction contains the low 11 bits of the displacement, as well as differentiating bl and blx.
    uint32_t s     = (uint32_t)(delta >> 24) & 0x1;
    uint32_t i1    = (uint32_t)(delta >> 23) & 0x1;
    uint32_t i2    = (uint32_t)(delta >> 22) & 0x1;
    uint32_t imm10 = (uint32_t)(delta >> 12) & 0x3FF;
    uint32_t imm11 = (uint32_t)(delta >> 1) & 0x7FF;
    uint32_t j1    = (i1 == s);
    uint32_t j2    = (i2 == s);

    if ( kind != b ) {
        if ( targetIsThumb )
            instruction = 0xD000F000; // need bl
        else
            instruction = 0xC000F000; // need blx
    }
    else if ( !targetIsThumb ) {
        return false; // can't branch from THUMB to ARM
    }
    else {
        instruction = 0x9000F000; // keep b
    }
    uint32_t nextDisp = (j1 << 13) | (j2 << 11) | imm11;
    uint32_t firstDisp = (s << 10) | imm10;
    instruction |= (nextDisp << 16) | firstDisp;
    return true;
}

bool Instructions::arm::isMovt(uint32_t instruction, uint16_t& value)
{
    if ( (instruction & 0x0FF00000) != 0x03400000 )
        return false;

    uint32_t imm4 = ((instruction & 0x000F0000) >> 16);
    uint32_t imm12 = (instruction & 0x00000FFF);
    value = (imm4 << 12) | imm12;
    return true;
}

bool Instructions::arm::setMovt(uint32_t& instruction, uint16_t value)
{
    if ( (instruction & 0x0FF00000) != 0x03400000 )
        return false;

    uint32_t imm4  = (value & 0x0000F000) >> 12;
    uint32_t imm12 = value & 0x00000FFF;
    instruction = (instruction & 0xFFF0F000) | (imm4 << 16) | imm12;
    return true;
}

bool Instructions::arm::isMovw(uint32_t instruction, uint16_t& value)
{
    if ( (instruction & 0x0FF00000) != 0x03000000 )
        return false;

    uint32_t imm4 = ((instruction & 0x000F0000) >> 16);
    uint32_t imm12 = (instruction & 0x00000FFF);
    value = (imm4 << 12) | imm12;
    return true;
}

bool Instructions::arm::setMovw(uint32_t& instruction, uint16_t value)
{
    if ( (instruction & 0x0FF00000) != 0x03000000 )
        return false;

    uint32_t imm4  = (value & 0x0000F000) >> 12;
    uint32_t imm12 = value & 0x00000FFF;
    instruction = (instruction & 0xFFF0F000) | (imm4 << 16) | imm12;
    return true;
}


bool Instructions::arm::isThumbMovt(uint32_t instruction, uint16_t& value)
{
    if ( (instruction & 0x8000fbf0) != 0x0000f2c0 )
        return false;

    uint32_t i    = ((instruction & 0x00000400) >> 10);
    uint32_t imm4 =  (instruction & 0x0000000F);
    uint32_t imm3 = ((instruction & 0x70000000) >> 28);
    uint32_t imm8 = ((instruction & 0x00FF0000) >> 16);
    value = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    return true;
}

bool Instructions::arm::setThumbMovt(uint32_t& instruction, uint16_t value)
{
    if ( (instruction & 0x8000fbf0) != 0x0000f2c0 )
        return false;

    uint32_t imm4 = (value & 0xF000) >> 12;
    uint32_t i =    (value & 0x0800) >> 11;
    uint32_t imm3 = (value & 0x0700) >> 8;
    uint32_t imm8 = (value & 0x00FF);
    instruction = (instruction & 0x8F00FBF0) | imm4 | (i << 10) | (imm3 << 28) | (imm8 << 16);
    return true;
}

bool Instructions::arm::isThumbMovw(uint32_t instruction, uint16_t& value)
{
    if ( (instruction & 0x8000fbf0) != 0x0000f240 )
        return false;

    uint32_t i    = ((instruction & 0x00000400) >> 10);
    uint32_t imm4 =  (instruction & 0x0000000F);
    uint32_t imm3 = ((instruction & 0x70000000) >> 28);
    uint32_t imm8 = ((instruction & 0x00FF0000) >> 16);
    value = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    return true;
}

bool Instructions::arm::setThumbMovw(uint32_t& instruction, uint16_t value)
{
    if ( (instruction & 0x8000fbf0) != 0x0000f240 )
        return false;

    uint32_t imm4 = (value & 0xF000) >> 12;
    uint32_t i =    (value & 0x0800) >> 11;
    uint32_t imm3 = (value & 0x0700) >> 8;
    uint32_t imm8 = (value & 0x00FF);
    instruction = (instruction & 0x8F00FBF0) | imm4 | (i << 10) | (imm3 << 28) | (imm8 << 16);
    return true;
}


#if INTERNAL_BUILD

//
// MARK: --- riscv methods ---
//

bool Instructions::riscv::isJ20(uint32_t instruction, uint64_t instructionAddr, uint64_t& targetAddr)
{
    uint8_t opcode = instruction & 0x0000007F;
    if ( opcode != 0x6F )
        return false;

    uint32_t imm20      = (instruction >> 31) & 0x1;
    uint32_t imm10_1    = (instruction >> 21) & 0x3FF;
    uint32_t imm11      = (instruction >> 20) & 0x1;
    uint32_t imm19_12   = (instruction >> 12) & 0xFF;
    int64_t  delta      = (imm20 << 20) | (imm19_12 << 12) | (imm11 << 11) | (imm10_1 << 1);
    if ( imm20 )
        delta |= 0xFFFFFFFFFFF00000ULL; // sign extend

    targetAddr = instructionAddr + delta;
    return true;
}

bool Instructions::riscv::setJ20Target(uint64_t instructionAddr, uint64_t targetAddr, uint32_t& instruction)
{
    if ( instructionAddr & 1 )
        return false;
    if ( targetAddr & 1 )
        return false;

    int64_t delta = targetAddr - instructionAddr;
    const int64_t omeMegLimit = 0x000FFFFE;
    if ( (delta > omeMegLimit) || (delta < (-omeMegLimit)) )
        return false;

    uint32_t imm20      = (delta >> 20) & 0x1;
    uint32_t imm19_12   = (delta >> 12) & 0xFF;
    uint32_t imm11      = (delta >> 11) & 0x1;
    uint32_t imm10_1    = (delta >>  1) & 0x3FF;

    instruction = (instruction & 0x00000FFF) | (imm20 << 31) | (imm10_1 << 21) | (imm11 << 20) | (imm19_12 << 12);
    return true;
}

bool Instructions::riscv::isLUI(uint32_t instruction)
{
    uint8_t opcode = instruction & 0x0000007F;
    return (opcode == 0x00000037);
}

bool Instructions::riscv::isLo12(uint32_t instruction, Lo12Kind& kind, int16_t& value, uint8_t& srcRegister)
{
    uint8_t opcode = (instruction & 0x0000007F);
    uint8_t funct3 = ((instruction >> 12) & 0x07);

    srcRegister = ((instruction >> 15) & 0x1F);
    if ( opcode == 0x13 ) {
        value = ((instruction >> 20) & 0xFFF);
        if ( value & 0x0800 )
            value |= 0xF000; // sign extend imm12 to 16-bits
        if ( funct3 == 0 )
            kind = Lo12Kind::addi;
        else
            kind = Lo12Kind::other_Itype;
        return true;
    }
    else if ( opcode == 0x03 ) {
        value = ((instruction >> 20) & 0xFFF);
        if ( value & 0x0800 )
            value |= 0xF000; // sign extend imm12 to 16-bits
       if ( funct3 == 2 )
            kind = Lo12Kind::lw;
        else
            kind = Lo12Kind::other_Itype;
        return true;
    }
    else if ( opcode == 0x07 ) {
        value = ((instruction >> 20) & 0xFFF);
        if ( value & 0x0800 )
            value |= 0xF000; // sign extend imm12 to 16-bits
        kind  = Lo12Kind::other_Itype; // flw, fld, or flq
        return true;
    }
    else if ( opcode == 0x23 ) {
        value = ((instruction >> 7) & 0x1F) | ((instruction >> 20) & 0xFE0);
        if ( value & 0x0800 )
            value |= 0xF000; // sign extend imm12 to 16-bits
        kind  = Lo12Kind::other_Stype; // sw, sb, sh
        return true;
    }
    else if ( opcode == 0x027 ) {
        value = ((instruction >> 7) & 0x1F) | ((instruction >> 20) & 0xFE0);
        if ( value & 0x0800 )
            value |= 0xF000; // sign extend imm12 to 16-bits
        kind  = Lo12Kind::other_Stype;  // fsw, fsd, or fsq
        return true;
    }

    return false;
}

bool Instructions::riscv::setLo12(uint16_t lo12, uint32_t& instruction)
{
    Lo12Kind kind;
    int16_t value;
    uint8_t  srcRegister;
    if ( !isLo12(instruction, kind, value, srcRegister) )
        return false;
    switch (kind) {
        case addi:
        case lw:
        case other_Itype:
            instruction = (instruction & 0x000FFFFF) | ((uint32_t)(lo12 & 0x0FFF) << 20);
            return true;
        case other_Stype:
            instruction = (instruction & 0x01FFF07F) | (((uint32_t)lo12 & 0x0FE0) << 20) | (((uint32_t)lo12 & 0x001F) << 7);
            return true;
    }
    return false;
}

bool Instructions::riscv::isAUIPC(uint32_t auipcInstruction, uint8_t& dstRegister)
{
    uint8_t opcode = auipcInstruction & 0x0000007F;
    if ( opcode != 0x00000017 )
        return false;

    dstRegister = ((auipcInstruction >> 7) & 0x1F);
    return true;
}

bool Instructions::riscv::setAUIPCTarget(uint32_t& instruction, uint64_t instructionAddr, uint64_t targetAddr)
{
    int64_t delta = targetAddr - instructionAddr;
    const int64_t twoGBLimit  = 0x7FFFF000;               // Note: riscv32 will always be in range, riscv64 might not be
    if ( (delta > twoGBLimit) || (delta < (-twoGBLimit)) )
        return false;

    if ( delta & 0x800 ) {
        // paired addi or lw instruction sign extends its 12-bit imm, so we need to compensate
        delta += 0x1000;
    }
    uint32_t imm20 = (delta & 0xFFFFF000);
    instruction = (instruction & 0x00000FFF) | imm20;
    return true;
}

bool Instructions::riscv::setLUITarget(uint32_t& instruction, uint64_t targetAddr)
{
    const int64_t fourGBLimit  = 0x7FFFF000;               // Note: riscv32 will always be in range, riscv64 might not be
    if ( targetAddr > fourGBLimit )
        return false;

    if ( targetAddr & 0x800 ) {
        // paired addi or lw instruction sign extends its 12-bit imm, so we need to compensate
        targetAddr += 0x1000;
    }
    uint32_t imm20 = (targetAddr & 0xFFFFF000);
    instruction = (instruction & 0x00000FFF) | imm20;
    return true;
}

bool Instructions::riscv::forceADDI(uint16_t lo12, uint32_t& instruction)
{
    uint32_t opcode         = instruction & 0x1F;
    uint32_t funct3         = (instruction >> 12) & 7;
    if ( (opcode == 0x13) && (funct3 == 0) ) {
        // already ADDI
        instruction = (instruction & 0x000FFFFF) | ((uint32_t)(lo12 & 0xFFF) << 20);
        return true;
    }
    else if ( (opcode == 0x3) && (funct3 == 2) ) {
        // turn LW instruction (GOT load) into ADDI
        instruction = (instruction & 0x000F8F80) | 0x00000013 | ((uint32_t)(lo12 & 0xFFF) << 20);
        return true;
    }
    else {
        // not a convertable instructin
        return false;
    }
}


#endif // INTERNAL_BUILD

} // namespace mach_o
