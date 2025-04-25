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

#ifndef mach_o_writer_BindOpcodes_h
#define mach_o_writer_BindOpcodes_h

#include <stdint.h>
#include <stdio.h>
#include <optional>
#include <span>
#include <vector>

// mach_o
#include "BindOpcodes.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class BindOpcodes
 *
 * @abstract
 *      Class to encapsulate building bind opcodes
 */
class VIS_HIDDEN BindOpcodesWriter : public BindOpcodes
{
public:
    // Note 'binds' input will be sorted by this method
    BindOpcodesWriter(std::span<LocAndTarget> binds, bool is64);

private:
    std::vector<uint8_t> _opcodes;
};

class VIS_HIDDEN LazyBindOpcodesWriter : public LazyBindOpcodes
{
public:
    // used by unit tests to build opcodes
    typedef void (^LazyStartRecorder)(size_t offset, const char* symbolName);

    // used by unit tests to build opcodes
    LazyBindOpcodesWriter(std::span<LocAndTarget> binds, bool is64, LazyStartRecorder recorder);

private:
    std::vector<uint8_t> _opcodes;
};

class VIS_HIDDEN WeakBindOpcodesWriter : public WeakBindOpcodes
{
public:
    // used by unit tests to build opcodes
    WeakBindOpcodesWriter(std::span<LocAndTarget> binds, bool is64);

private:
    std::vector<uint8_t> _opcodes;
};


} // namespace mach_o

#endif // mach_o_writer_BindOpcodes_h


