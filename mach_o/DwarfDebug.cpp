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

#include "DwarfDebug.h"
#include "Misc.h"
#include "dwarf.h"


namespace mach_o {


DwarfDebug::DwarfDebug(std::span<const uint8_t> debugInfo, std::span<const uint8_t> abbrev,
                       std::span<const uint8_t> strings, std::span<const uint8_t> stringOffs)
  : _debugInfo(debugInfo), _abbrev(abbrev), _strings(strings), _stringOffsets(stringOffs)
{
    parseCompilationUnit();
}

void DwarfDebug::parseCompilationUnit()
{
    const char* tuDir  = nullptr;
    const char* tuName = nullptr;

    // Too small to be a real debug_info section
    if ( _debugInfo.size() < 12)
        return;

    const uint8_t* debug_info   = _debugInfo.data();
    const uint8_t* debug_abbrev = _abbrev.data();
    const uint8_t* next_cu      = debug_info;

    while ((uint64_t)(next_cu - debug_info) < _debugInfo.size() ) {
        const uint8_t* di = next_cu;
        uint32_t       sz32;
        memcpy(&sz32, di, 4);    // support unaligned loads
        uint64_t       sz = sz32;
        di += 4;
        const bool     dwarf64 = (sz == 0xffffffff);
        if (dwarf64) {
            sz = *(uint64_t*)di;
            di += 8;
        }
        else if (sz > 0xffffff00) {
            // Unknown dwarf format
            return;
        }

        next_cu = di + sz;

        uint16_t vers;
        memcpy(&vers, di, 2);    // support unaligned loads
        if (vers < 2 || vers > 5) {
            // DWARF version wrong for this code.
            // Chances are we could continue anyway, but we don't know for sure.
            return;
        }

        // Verify claimed size
        uint64_t min_length;
        uint64_t total_size;
        if (vers < 5) {
            total_size = sz + (di - debug_info);
            min_length = dwarf64 ? 23 : 11;
        }
        else {
            total_size = sz + (dwarf64 ? 12 : 4);
            min_length = dwarf64 ? 32 : 20;
        }
        di += 2;  // advance past 16-bit vers field

        if (total_size > _debugInfo.size() || sz <= min_length)
            return;

        uint8_t address_size;
        if (vers == 5) {
            /* Verify unit type */
            char unit_type = (*(char *)di);
            if (unit_type != DW_UT_compile)
                continue;
            di += 1;
            /* Read address size */
            address_size = *di++;
        } else // zero-initialize address_size to silence uninitialized variable warning
            address_size = 0;
        // Find the debug_abbrev section
        uint64_t abbrev_base;
        if ( dwarf64 ) {
            memcpy(&abbrev_base, di, 8);
            di += 8;
        }
        else {
            uint32_t base32;
            memcpy(&base32, di, 4);
            di += 4;
            abbrev_base = base32;
        }

        if (abbrev_base > _debugInfo.size())
            return;
        const uint8_t* da   = debug_abbrev + abbrev_base;
        const uint8_t* enda = debug_abbrev + _abbrev.size();

        if (vers < 5)
            address_size = *di++;

        // Find the abbrev number we're looking for
        const uint8_t* end = di + sz;
        bool malformed;
        uint64_t abbrev = read_uleb128(di, end, malformed);
        if (malformed || abbrev == (uint64_t)(-1))
            return;

        // Skip through the debug_abbrev section looking for that abbrev
        for (;;)
        {
            uint64_t this_abbrev = read_uleb128(da, enda, malformed);
            if (this_abbrev == abbrev)
                break;   // This is almost always taken
            read_uleb128(da, enda, malformed); // Skip the tag
            if (da == enda)
                return;
            da++;  // Skip the DW_CHILDREN_* value

            uint64_t attr;
            do {
                attr = read_uleb128(da, enda, malformed);
                read_uleb128(da, enda, malformed);
            } while (attr != 0 && attr != (uint64_t) -1);
            if (attr != 0)
                return;
        }

        // Check that the abbrev is one for a DW_TAG_compile_unit
        if (read_uleb128(da, enda, malformed) != DW_TAG_compile_unit)
            return;
        if (da == enda)
            return;
        da++;  // Skip the DW_CHILDREN_* value

        // Now, go through the DIE looking for DW_AT_name and DW_AT_comp_dir
        bool skip_to_next_cu = false;
        while (!skip_to_next_cu) {
            uint64_t attr = read_uleb128(da, enda, malformed);
            uint64_t form = read_uleb128(da, enda, malformed);
            if (attr == (uint64_t)(-1))
                return;
            else if (attr == 0)
                break;
            if (form == DW_FORM_indirect)
                form = read_uleb128(di, end, malformed);

            switch (attr) {
                case DW_AT_name:
                    tuName = getDwarfString(form, di, dwarf64);
                    /* Swift object files may contain two CUs: One
                       describes the Swift code, one is created by the
                       clang importer. Skip over the CU created by the
                       clang importer as it may be empty. */
                    if (strcmp(tuName, "<swift-imported-modules>") == 0)
                        skip_to_next_cu = true;
                    break;
                case DW_AT_comp_dir:
                    tuDir = getDwarfString(form, di, dwarf64);
                    break;
                default:
                    if (!skip_form(di, end, form, address_size, dwarf64))
                        return ;
            }
        }
        if ( (tuName != nullptr) && (tuName[0] == '/') ) {
            // DW_AT_name has full path, break it up into dir and leaf name
            char* copy = strdup(tuName);
            char* lastSlash = strrchr(copy, '/');
            lastSlash[1] = '\0';
            _tuDir      = copy;
            _tuFileName = strrchr(tuName, '/') + 1;
            return;
        }
        else if ( (tuDir != nullptr) && (tuName != nullptr) ) {
            // DW_AT_name is relative path from DW_AT_comp_dir
            // concate then break it up into dir and leaf name
            char* copy;
            asprintf(&copy, "%s/%s", tuDir, tuName);
            char* lastSlash = strrchr(copy, '/');
            lastSlash[1] = '\0';
            _tuDir = copy;
            if ( const char* last = strrchr(tuName, '/') )
                _tuFileName = &last[1];
            else
                _tuFileName = tuName;
            return;
        }
    }

 }

// Skip over a DWARF attribute of form FORM
bool DwarfDebug::skip_form(const uint8_t*& offset, const uint8_t* end, uint64_t form, uint8_t addr_size, bool dwarf64)
{
    int64_t sz=0;
    bool    malformed;

    switch (form) {
        case DW_FORM_addr:
            sz = addr_size;
            break;
        case DW_FORM_block2:
            if (end - offset < 2)
                return false;
            sz = 2 + *((uint16_t*)offset);
            break;
        case DW_FORM_block4:
            if (end - offset < 4)
                return false;
            sz = 2 + *((uint32_t*)offset);
            break;
        case DW_FORM_data2:
        case DW_FORM_ref2:
            sz = 2;
            break;
        case DW_FORM_data4:
        case DW_FORM_ref4:
            sz = 4;
            break;
        case DW_FORM_data8:
        case DW_FORM_ref8:
            sz = 8;
            break;
        case DW_FORM_string:
            if ( offset == end )
                return false;
            // rdar://124698722 (off-by-one error when decoding DW_FORM_string)
            offset += strnlen((char*)offset, (end-offset-1)) + 1;
            return true;
        case DW_FORM_data1:
        case DW_FORM_flag:
        case DW_FORM_ref1:
            sz = 1;
            break;
        case DW_FORM_block:
            sz = read_uleb128(offset, end, malformed);
            return true; // offset already updated by read_uleb128()
            break;
        case DW_FORM_block1:
            if (offset == end)
                return false;
            sz = 1 + *offset;
            break;
        case DW_FORM_sdata:
        case DW_FORM_udata:
        case DW_FORM_ref_udata:
            sz = read_uleb128(offset, end, malformed);
            return true; // offset already updated by read_uleb128()
            break;
        case DW_FORM_addrx:
        case DW_FORM_strx:
        case DW_FORM_rnglistx:
            sz = read_uleb128(offset, end, malformed);
            return true; // offset already updated by read_uleb128()
            break;
        case DW_FORM_addrx1:
        case DW_FORM_strx1:
            sz = 1;
            break;
        case DW_FORM_addrx2:
        case DW_FORM_strx2:
            sz = 2;
            break;
        case DW_FORM_addrx3:
        case DW_FORM_strx3:
            sz = 3;
            break;
        case DW_FORM_addrx4:
        case DW_FORM_strx4:
            sz = 4;
            break;
        case DW_FORM_strp:
        case DW_FORM_ref_addr:
            sz = 4;
            break;
        case DW_FORM_sec_offset:
            sz = (dwarf64 ? 8 : 4);
            break;
        case DW_FORM_exprloc:
            sz = read_uleb128(offset, end, malformed);
            return true; // offset already updated by read_uleb128()
            break;
        case DW_FORM_flag_present:
            sz = 0;
            break;
        case DW_FORM_ref_sig8:
            sz = 8;
            break;
        default:
            return false;
    }
    if (end - offset < sz)
        return false;
    offset += sz;
    return true;
}


const char* DwarfDebug::getDwarfString(uint64_t form, const uint8_t*& di, bool dwarf64)
{
    uint32_t offset;
    uint16_t off16;
    const char* dwarfStrings;
    const char* result = NULL;
    switch (form) {
        case DW_FORM_string:
            result = (const char*)di;
            di += strlen(result) + 1;
            break;
        case DW_FORM_strx1:
            offset = *(const char*)di;
            di += 1;
            result = getStrxString(offset, dwarf64);
            break;
        case DW_FORM_strx2:
            memcpy(&off16, di, 2);
            offset = off16;
            di += 2;
            result = getStrxString(offset, dwarf64);
            break;
        case DW_FORM_strx4:
            memcpy(&offset, di, 4);
            di += 4;
            result = getStrxString(offset, dwarf64);
            break;
        case DW_FORM_strp:
            memcpy(&offset, di, 4);
            dwarfStrings = (char*)_strings.data();
            if ( offset < _strings.size() )
                result = &dwarfStrings[offset];
            //else
                //warning("dwarf DW_FORM_strp (offset=0x%08X) is too big in %s", offset, this->_path);
            di += 4;
            break;
        default:
            //warning("unknown dwarf string encoding (form=%lld) in %s", form, this->_path);
            break;
    }
    return result;
}


const char* DwarfDebug::getStrxString(uint64_t idx, bool dwarf64)
{
    const uint8_t* p = _stringOffsets.data();

    /* the debug_str_offsets section has an independent 64 or 32 bit header */
    uint64_t sz = *(uint32_t*)p;
    p += 4;
    if (sz == 0xffffffff) {
        sz = *((uint64_t*)p);
        p += 8;
    }
    else if (sz > 0xffffff00) {
        //warning("Unknown DWARF format in __debug_str_offs %s", this->_path);
        return nullptr;
    }
    if ( sz >= _stringOffsets.size() ) {
        //warning("Inconsistent size of __debug_str_offs in %s", this->_path);
        return nullptr;
    }

    uint16_t version = *((uint16_t*)p);
    if (version != 5)
        return nullptr;
    p += 4; // version + padding

    uint64_t offset = idx * (dwarf64 ? 8 : 4);
    if ( offset >= _stringOffsets.size() ) {
        //warning("dwarf DW_FORM_strx (index=%llu) is too big in %s", idx, this->_path);
        return nullptr;
    }
    if (dwarf64)
        offset = *((uint64_t*)(&p[offset]));
    else
        offset = *((uint32_t*)(&p[offset]));
    const char* dwarfStrings = (char*)_strings.data();
    if ( offset >= _strings.size() ) {
        //warning("dwarf DW_FORM_strx (offset=0x%08llX) is too big in %s", offset, this->_path);
        return nullptr;
    }
    return &dwarfStrings[offset];
}



} // namespace mach_o

