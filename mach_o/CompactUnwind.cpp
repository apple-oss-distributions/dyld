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

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>


#include <mach-o/compact_unwind_encoding.h>

#include "CompactUnwind.h"
#include "Misc.h"


namespace mach_o {


CompactUnwind::CompactUnwind(Architecture arch, const uint8_t* start, size_t size)
: _arch(arch), _unwindTable((unwind_info_section_header*)start), _unwindTableSize(size)
{
}

Error CompactUnwind::valid() const
{
    if ( _unwindTable->version != UNWIND_SECTION_VERSION )
        return Error("invalid unwind table version");
    if ( _unwindTable->commonEncodingsArraySectionOffset > _unwindTableSize )
        return Error("common encodings out of range");
    if ( _unwindTable->commonEncodingsArraySectionOffset + _unwindTable->commonEncodingsArrayCount*4 > _unwindTableSize )
        return Error("common encodings out of range");
    if ( _unwindTable->personalityArraySectionOffset > _unwindTableSize )
        return Error("personality table out of range");
    if ( _unwindTable->personalityArraySectionOffset + _unwindTable->personalityArrayCount*4 > _unwindTableSize )
        return Error("personality table out of range");
    if ( _unwindTable->indexSectionOffset > _unwindTableSize )
        return Error("index table out of range");
    if ( _unwindTable->indexSectionOffset + _unwindTable->indexCount*12 > _unwindTableSize )
        return Error("index table out of range");

    return Error::none();
}

Error CompactUnwind::forEachFirstLevelTableEntry(void (^callback)(uint32_t funcsStartOffset, uint32_t funcsEndOffset, uint32_t secondLevelOffset, uint32_t lsdaIndexOffset)) const
{
    const unwind_info_section_header_index_entry* indexes = (unwind_info_section_header_index_entry*)(((uint8_t*)_unwindTable) + _unwindTable->indexSectionOffset);
    for (uint32_t i=0; i < _unwindTable->indexCount-1; ++i) {
        const unwind_info_section_header_index_entry& entry = indexes[i];
        const unwind_info_section_header_index_entry& next  = indexes[i+1];
        if ( entry.secondLevelPagesSectionOffset > _unwindTableSize )
            return Error("second level table offset out of range");
        callback(entry.functionOffset, next.functionOffset, entry.secondLevelPagesSectionOffset, entry.lsdaIndexArraySectionOffset);
    }
    return Error::none();
}

Error CompactUnwind::forEachSecondLevelCompressedTableEntry(const struct unwind_info_compressed_second_level_page_header* pageHeader, uint32_t pageFunsOffset, void (^callback)(const UnwindInfo&)) const
{
    const compact_unwind_encoding_t* commonEncodings = (compact_unwind_encoding_t*)(((uint8_t*)_unwindTable)+_unwindTable->commonEncodingsArraySectionOffset);
    const compact_unwind_encoding_t* personalities   = (compact_unwind_encoding_t*)(((uint8_t*)_unwindTable)+_unwindTable->personalityArraySectionOffset);
    const compact_unwind_encoding_t* pageEncodings   = (compact_unwind_encoding_t*)(((uint8_t*)pageHeader)+pageHeader->encodingsPageOffset);
    const uint32_t* entries = (uint32_t*)(((uint8_t*)pageHeader)+pageHeader->entryPageOffset);
    for (uint16_t i=0; i < pageHeader->entryCount; ++i) {
        uint8_t encodingIndex = UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX(entries[i]);
        compact_unwind_encoding_t encoding;
        if ( encodingIndex < _unwindTable->commonEncodingsArrayCount )
            encoding = commonEncodings[encodingIndex];
        else
            encoding = pageEncodings[encodingIndex-_unwindTable->commonEncodingsArrayCount];
        uint32_t funcOff = UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(entries[i])+pageFunsOffset;
        uint32_t lsdaOffset = 0;
        uint32_t personalityOffset = 0;
        if ( encoding & UNWIND_HAS_LSDA ) {
            int personalityIndex = (encoding & UNWIND_PERSONALITY_MASK) >> (__builtin_ctz(UNWIND_PERSONALITY_MASK));
            personalityOffset = personalities[personalityIndex-1];
            lsdaOffset        = findLSDA(funcOff);
        }
        callback({funcOff, encoding, lsdaOffset, personalityOffset});
    }
    return Error::none();
}

Error CompactUnwind::forEachSecondLevelRegularTableEntry(const struct unwind_info_regular_second_level_page_header* pageHeader, void (^callback)(const UnwindInfo&)) const
{
    const unwind_info_regular_second_level_entry* entries       = (unwind_info_regular_second_level_entry*)((uint8_t*)pageHeader + pageHeader->entryPageOffset);
    const compact_unwind_encoding_t*              personalities = (compact_unwind_encoding_t*)(((uint8_t*)_unwindTable)+_unwindTable->personalityArraySectionOffset);
    for (uint32_t i=0; i < pageHeader->entryCount; ++i) {
        uint32_t lsdaOffset = 0;
        uint32_t personalityOffset = 0;
        if ( entries[i].encoding & UNWIND_HAS_LSDA ) {
            int personalityIndex = (entries[i].encoding & UNWIND_PERSONALITY_MASK) >> (__builtin_ctz(UNWIND_PERSONALITY_MASK));
            personalityOffset = personalities[personalityIndex-1];
            lsdaOffset = findLSDA(entries[i].functionOffset);
        }
        callback({entries[i].functionOffset, entries[i].encoding, lsdaOffset, personalityOffset});
    }
    return Error::none();
}

uint32_t CompactUnwind::findLSDA(uint32_t funcOffset) const
{
    const unwind_info_section_header_index_entry*       indexes                        = (unwind_info_section_header_index_entry*)(((uint8_t*)_unwindTable) + _unwindTable->indexSectionOffset);
    uint32_t                                            lsdaIndexArraySectionOffset    = indexes[0].lsdaIndexArraySectionOffset;
    uint32_t                                            lsdaIndexArrayEndSectionOffset = indexes[_unwindTable->indexCount-1].lsdaIndexArraySectionOffset;
    uint32_t                                            lsdaIndexArrayCount            = (lsdaIndexArrayEndSectionOffset-lsdaIndexArraySectionOffset)/sizeof(unwind_info_section_header_lsda_index_entry);
    const unwind_info_section_header_lsda_index_entry*  lsdas                          = (unwind_info_section_header_lsda_index_entry*)(((uint8_t*)_unwindTable) + lsdaIndexArraySectionOffset);
    for (uint32_t j=0; j < lsdaIndexArrayCount; ++j) {
        if ( lsdas[j].functionOffset == funcOffset ) {
            return lsdas[j].lsdaOffset;
        }
    }
    return 0;
}


void CompactUnwind::forEachUnwindInfo(void (^callback)(const UnwindInfo&)) const
{
    __block Error err;
    Error result = forEachFirstLevelTableEntry(^(uint32_t funcsStartOffset, uint32_t funcsEndOffset, uint32_t secondLevelOffset, uint32_t lsdaIndexOffset) {
        if ( funcsStartOffset > funcsEndOffset ) {
            err = Error("first level table function offsets not sequential");
            return;
        }
        const unwind_info_compressed_second_level_page_header* secondLevelTable = (unwind_info_compressed_second_level_page_header*)(((uint8_t*)_unwindTable) + secondLevelOffset);
        if ( secondLevelTable->kind == UNWIND_SECOND_LEVEL_COMPRESSED ) {
            err = forEachSecondLevelCompressedTableEntry(secondLevelTable, funcsStartOffset, callback);
        }
        else if ( secondLevelTable->kind == UNWIND_SECOND_LEVEL_REGULAR ) {
            const unwind_info_regular_second_level_page_header* secondLevelTableReg = (unwind_info_regular_second_level_page_header*)secondLevelTable;
            err = forEachSecondLevelRegularTableEntry(secondLevelTableReg, callback);
        }
        else {
            err = Error("second level table has invalid kind");
        }
    });
}

void CompactUnwind::encodingToString(uint32_t encoding, const void* funcBytes, char strBuf[128]) const
{
    if ( _arch.usesArm64Instructions() )
        encodingToString_arm64(encoding, funcBytes, strBuf);
    else if ( _arch.usesx86_64Instructions() )
        encodingToString_x86_64(encoding, funcBytes, strBuf);
    else
        strlcpy(strBuf, "arch not supported yet", 22);
}

#define EXTRACT_BITS(value, mask) \
( (value >> __builtin_ctz(mask)) & (((1 << __builtin_popcount(mask)))-1) )

void CompactUnwind::encodingToString_arm64(uint32_t encoding, const void*, char strBuf[128]) const
{
    uint32_t stackSize;
    switch ( encoding & UNWIND_ARM64_MODE_MASK ) {
    case UNWIND_ARM64_MODE_FRAMELESS:
        stackSize = EXTRACT_BITS(encoding, UNWIND_ARM64_FRAMELESS_STACK_SIZE_MASK);
        if ( stackSize == 0 )
            strlcpy(strBuf, "no frame, no saved registers ", 128);
        else
            snprintf(strBuf, 128, "stack size=%d: ", 16 * stackSize);
        if ( encoding & UNWIND_ARM64_FRAME_X19_X20_PAIR )
            strlcat(strBuf, "x19/20 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X21_X22_PAIR )
            strlcat(strBuf, "x21/22 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X23_X24_PAIR )
            strlcat(strBuf, "x23/24 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X25_X26_PAIR )
            strlcat(strBuf, "x25/26 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X27_X28_PAIR )
            strlcat(strBuf, "x27/28 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D8_D9_PAIR )
            strlcat(strBuf, "d8/9 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D10_D11_PAIR )
            strlcat(strBuf, "d10/11 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D12_D13_PAIR )
            strlcat(strBuf, "d12/13 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D14_D15_PAIR )
            strlcat(strBuf, "d14/15 ", 128);
        break;
    case UNWIND_ARM64_MODE_FRAME:
        strlcpy(strBuf, "std frame: ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X19_X20_PAIR )
            strlcat(strBuf, "x19/20 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X21_X22_PAIR )
            strlcat(strBuf, "x21/22 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X23_X24_PAIR )
            strlcat(strBuf, "x23/24 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X25_X26_PAIR )
            strlcat(strBuf, "x25/26 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_X27_X28_PAIR )
            strlcat(strBuf, "x27/28 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D8_D9_PAIR )
            strlcat(strBuf, "d8/9 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D10_D11_PAIR )
            strlcat(strBuf, "d10/11 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D12_D13_PAIR )
            strlcat(strBuf, "d12/13 ", 128);
        if ( encoding & UNWIND_ARM64_FRAME_D14_D15_PAIR )
            strlcat(strBuf, "d14/15 ", 128);
        break;
    case UNWIND_ARM64_MODE_DWARF:
        snprintf(strBuf, 128, "dwarf offset 0x%08X, ", encoding & UNWIND_X86_64_DWARF_SECTION_OFFSET);
        break;
    default:
        if ( encoding == 0 )
            strlcpy(strBuf, "no unwind info ", 128);
        else
            strlcpy(strBuf, "unknown arm64 compact encoding ", 128);
        break;
    }
}


void CompactUnwind::encodingToString_x86_64(uint32_t encoding, const void* funcBytes, char strBuf[128]) const
{
    *strBuf = '\0';
    switch ( encoding & UNWIND_X86_64_MODE_MASK ) {
    case UNWIND_X86_64_MODE_RBP_FRAME:
        {
            uint32_t savedRegistersOffset = EXTRACT_BITS(encoding, UNWIND_X86_64_RBP_FRAME_OFFSET);
            uint32_t savedRegistersLocations = EXTRACT_BITS(encoding, UNWIND_X86_64_RBP_FRAME_REGISTERS);
            if ( savedRegistersLocations == 0 ) {
                strlcpy(strBuf, "rbp frame, no saved registers", 128);
            }
            else {
                snprintf(strBuf, 128, "rbp frame, at -%d:", savedRegistersOffset*8);
                bool needComma = false;
                for (int i=0; i < 5; ++i) {
                    if ( needComma )
                        strncat(strBuf, ",", 128);
                    else
                        needComma = true;
                    switch (savedRegistersLocations & 0x7) {
                    case UNWIND_X86_64_REG_NONE:
                        strlcat(strBuf, "-", 128);
                        break;
                    case UNWIND_X86_64_REG_RBX:
                        strlcat(strBuf, "rbx", 128);
                        break;
                    case UNWIND_X86_64_REG_R12:
                        strlcat(strBuf, "r12", 128);
                        break;
                    case UNWIND_X86_64_REG_R13:
                        strlcat(strBuf, "r13", 128);
                        break;
                    case UNWIND_X86_64_REG_R14:
                        strlcat(strBuf, "r14", 128);
                        break;
                    case UNWIND_X86_64_REG_R15:
                        strlcat(strBuf, "r15", 128);
                        break;
                    default:
                        strlcat(strBuf, "r?", 128);
                    }
                    savedRegistersLocations = (savedRegistersLocations >> 3);
                    if ( savedRegistersLocations == 0 )
                        break;
                }
            }
        }
        break;
    case UNWIND_X86_64_MODE_STACK_IMMD:
    case UNWIND_X86_64_MODE_STACK_IND:
        {
            uint32_t stackSize = EXTRACT_BITS(encoding, UNWIND_X86_64_FRAMELESS_STACK_SIZE);
            uint32_t stackAdjust = EXTRACT_BITS(encoding, UNWIND_X86_64_FRAMELESS_STACK_ADJUST);
            uint32_t regCount = EXTRACT_BITS(encoding, UNWIND_X86_64_FRAMELESS_STACK_REG_COUNT);
            uint32_t permutation = EXTRACT_BITS(encoding, UNWIND_X86_64_FRAMELESS_STACK_REG_PERMUTATION);
            if ( (encoding & UNWIND_X86_64_MODE_MASK) == UNWIND_X86_64_MODE_STACK_IND ) {
                // stack size is encoded in subl $xxx,%esp instruction
                uint32_t subl = *((uint32_t*)((uint8_t*)funcBytes+stackSize));
                snprintf(strBuf, 128, "stack size=0x%08X, ", subl + 8*stackAdjust);
            }
            else {
                snprintf(strBuf, 128, "stack size=%d, ", stackSize*8);
            }
            if ( regCount == 0 ) {
                strlcat(strBuf, "no registers saved", 128);
            }
            else {
                int permunreg[6];
                switch ( regCount ) {
                case 6:
                    permunreg[0] = permutation/120;
                    permutation -= (permunreg[0]*120);
                    permunreg[1] = permutation/24;
                    permutation -= (permunreg[1]*24);
                    permunreg[2] = permutation/6;
                    permutation -= (permunreg[2]*6);
                    permunreg[3] = permutation/2;
                    permutation -= (permunreg[3]*2);
                    permunreg[4] = permutation;
                    permunreg[5] = 0;
                    break;
                case 5:
                    permunreg[0] = permutation/120;
                    permutation -= (permunreg[0]*120);
                    permunreg[1] = permutation/24;
                    permutation -= (permunreg[1]*24);
                    permunreg[2] = permutation/6;
                    permutation -= (permunreg[2]*6);
                    permunreg[3] = permutation/2;
                    permutation -= (permunreg[3]*2);
                    permunreg[4] = permutation;
                    break;
                case 4:
                    permunreg[0] = permutation/60;
                    permutation -= (permunreg[0]*60);
                    permunreg[1] = permutation/12;
                    permutation -= (permunreg[1]*12);
                    permunreg[2] = permutation/3;
                    permutation -= (permunreg[2]*3);
                    permunreg[3] = permutation;
                    break;
                case 3:
                    permunreg[0] = permutation/20;
                    permutation -= (permunreg[0]*20);
                    permunreg[1] = permutation/4;
                    permutation -= (permunreg[1]*4);
                    permunreg[2] = permutation;
                    break;
                case 2:
                    permunreg[0] = permutation/5;
                    permutation -= (permunreg[0]*5);
                    permunreg[1] = permutation;
                    break;
                case 1:
                    permunreg[0] = permutation;
                    break;
                default:
                    strlcat(strBuf, "unsupported registers saved", 128);
                    return;
                }
                // renumber registers back to standard numbers
                int registers[6];
                bool used[7] = { false, false, false, false, false, false, false };
                for (int i=0; i < regCount; ++i) {
                    int renum = 0;
                    for (int u=1; u < 7; ++u) {
                        if ( !used[u] ) {
                            if ( renum == permunreg[i] ) {
                                registers[i] = u;
                                used[u] = true;
                                break;
                            }
                            ++renum;
                        }
                    }
                }
                bool needComma = false;
                for (int i=0; i < regCount; ++i) {
                    if ( needComma )
                        strlcat(strBuf, ",", 128);
                    else
                        needComma = true;
                    switch ( registers[i] ) {
                    case UNWIND_X86_64_REG_RBX:
                        strlcat(strBuf, "rbx", 128);
                        break;
                    case UNWIND_X86_64_REG_R12:
                        strlcat(strBuf, "r12", 128);
                        break;
                    case UNWIND_X86_64_REG_R13:
                        strlcat(strBuf, "r13", 128);
                        break;
                    case UNWIND_X86_64_REG_R14:
                        strlcat(strBuf, "r14", 128);
                        break;
                    case UNWIND_X86_64_REG_R15:
                        strlcat(strBuf, "r15", 128);
                        break;
                    case UNWIND_X86_64_REG_RBP:
                        strlcat(strBuf, "rbp", 128);
                        break;
                    default:
                        strlcat(strBuf, "r??", 128);
                    }
                }
            }
        }
        break;
    case UNWIND_X86_64_MODE_DWARF:
        snprintf(strBuf, 128, "dwarf offset 0x%08X, ", encoding & UNWIND_X86_64_DWARF_SECTION_OFFSET);
        break;
    default:
        if ( encoding == 0 )
            strlcat(strBuf, "no unwind information", 128);
        else
            strlcat(strBuf, "tbd ", 128);
    }
}

bool CompactUnwind::findUnwindInfo(uint32_t targetFunctionOffset, UnwindInfo& result) const
{
    // binary search first level table
    const unwind_info_section_header_index_entry* firstLevelTable = (unwind_info_section_header_index_entry*)(((uint8_t*)_unwindTable) + _unwindTable->indexSectionOffset);
    if ( targetFunctionOffset < firstLevelTable[0].functionOffset )
        return false;  // target before range covered by unwind info
    uint32_t low  = 0;
    uint32_t high = _unwindTable->indexCount;
    uint32_t last = high - 1;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if ( firstLevelTable[mid].functionOffset <= targetFunctionOffset ) {
            if ( (mid == last) || (firstLevelTable[mid+1].functionOffset > targetFunctionOffset) ) {
                low = mid;
                break;
            }
            else {
                low = mid + 1;
            }
        }
        else {
            high = mid;
        }
    }
    const uint32_t firstLevelIndex             = low;
    const uint32_t firstLevelFunctionOffset    = firstLevelTable[firstLevelIndex].functionOffset;
    const uint32_t firstLevelEndFunctionOffset = firstLevelTable[firstLevelIndex+1].functionOffset;
    const void*    secondLevelAddr             = (uint8_t*)_unwindTable + firstLevelTable[firstLevelIndex].secondLevelPagesSectionOffset;

    if ( targetFunctionOffset > firstLevelEndFunctionOffset )
        return false;  // target beyond range covered by unwind info

    // do a binary search of second level page index, where index[e].offset <= targetOffset < index[e+1].offset
    uint32_t pageKind    = *((uint32_t*)secondLevelAddr);
    if ( pageKind == UNWIND_SECOND_LEVEL_REGULAR ) {
        // regular page
        const unwind_info_regular_second_level_page_header* pageHeader = (unwind_info_regular_second_level_page_header*)secondLevelAddr;
        const unwind_info_regular_second_level_entry*       entries    = (unwind_info_regular_second_level_entry*)((uint8_t*)secondLevelAddr + pageHeader->entryPageOffset);
        low  = 0;
        high = pageHeader->entryCount;
        last = pageHeader->entryCount - 1;
        while ( low < high ) {
            uint32_t mid = (low + high)/2;
            if ( entries[mid].functionOffset <= targetFunctionOffset ) {
                if ( (mid == last) || (entries[mid+1].functionOffset > targetFunctionOffset) ) {
                    // next is past target address, so we found it
                    result.funcOffset = entries[mid].functionOffset;
                    result.encoding   = entries[mid].encoding;
                    result.lsdaOffset = 0;
                    result.personalityOffset = 0;
                    break;
                }
                else {
                    low = mid+1;
                }
            }
            else {
                high = mid;
            }
        }
    }
    else if ( pageKind == UNWIND_SECOND_LEVEL_COMPRESSED ) {
        // compressed page
        const unwind_info_compressed_second_level_page_header* pageHeader      = (unwind_info_compressed_second_level_page_header*)secondLevelAddr;
        const uint32_t*                                        entries         = (uint32_t*)((uint8_t*)secondLevelAddr + pageHeader->entryPageOffset);
        const uint32_t                                         targetOffset    = targetFunctionOffset - firstLevelFunctionOffset;
        const uint32_t*                                        commonEncodings = (uint32_t*)(((uint8_t*)_unwindTable)+_unwindTable->commonEncodingsArraySectionOffset);
        const uint32_t*                                        pageEncodings   = (uint32_t*)(((uint8_t*)pageHeader)+pageHeader->encodingsPageOffset);
        last = pageHeader->entryCount - 1;
        high = pageHeader->entryCount;
        while ( low < high ) {
            uint32_t mid = (low + high)/2;
            if ( UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(entries[mid]) <= targetOffset ) {
                if ( (mid == last) || (UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(entries[mid+1]) > targetOffset) ) {
                    result.funcOffset = UNWIND_INFO_COMPRESSED_ENTRY_FUNC_OFFSET(entries[mid]) + firstLevelFunctionOffset;
                    uint8_t encodingIndex = UNWIND_INFO_COMPRESSED_ENTRY_ENCODING_INDEX(entries[mid]);
                    if ( encodingIndex < _unwindTable->commonEncodingsArrayCount )
                        result.encoding = commonEncodings[encodingIndex];
                    else
                        result.encoding = pageEncodings[encodingIndex];
                    result.lsdaOffset = 0;
                    result.personalityOffset = 0;
                   break;
                }
                else {
                    low = mid+1;
                }
            }
            else {
                high = mid;
            }
        }
    }
    else {
        return false;
    }

    if ( result.encoding & UNWIND_HAS_LSDA ) {
        // binary search lsda table range for entry with exact match for functionOffset
        const void*     lsdaArrayStartAddr  = (uint8_t*)_unwindTable + firstLevelTable[firstLevelIndex].lsdaIndexArraySectionOffset;
        const uint32_t  lsdaArrayCount      = (firstLevelTable[firstLevelIndex+1].lsdaIndexArraySectionOffset - firstLevelTable[firstLevelIndex].lsdaIndexArraySectionOffset)/sizeof(unwind_info_section_header_lsda_index_entry);
        const unwind_info_section_header_lsda_index_entry* lsdaArray = (unwind_info_section_header_lsda_index_entry*)lsdaArrayStartAddr;
        low = 0;
        high = lsdaArrayCount;
        while ( low < high ) {
            uint32_t mid = (low + high)/2;
            if ( lsdaArray[mid].functionOffset == result.funcOffset ) {
                result.lsdaOffset = lsdaArray[mid].lsdaOffset;
                break;
            }
            else if ( lsdaArray[mid].functionOffset < result.funcOffset ) {
                low = mid+1;
            }
            else {
                high = mid;
            }
        }
        uint32_t personalityIndex = (result.encoding & UNWIND_PERSONALITY_MASK) >> (__builtin_ctz(UNWIND_PERSONALITY_MASK));
        if ( personalityIndex != 0 ) {
            --personalityIndex; // change 1-based to zero-based index
            if ( personalityIndex > _unwindTable->personalityArrayCount )
                 return false;
            const uint32_t* personalityArray = (uint32_t*)((uint8_t*)_unwindTable + _unwindTable->personalityArraySectionOffset);
            result.personalityOffset = personalityArray[personalityIndex];
        }
    }
    return true;
}


uint32_t CompactUnwind::compactUnwindEntrySize(bool is64)
{
    return is64 ? (4 * sizeof(uint64_t)) : (5 * sizeof(uint32_t));
}




} // namespace mach_o

