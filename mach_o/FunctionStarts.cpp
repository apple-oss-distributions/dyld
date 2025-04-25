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

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "FunctionStarts.h"
#include "Misc.h"


namespace mach_o {


FunctionStarts::FunctionStarts(const uint8_t* start, size_t size)
  : _funcStartsBegin(start), _funcStartsEnd(start+size)
{
}

Error FunctionStarts::valid(uint64_t maxFuncOffset) const
{
    uint64_t runtimeOffset = 0;
    for (const uint8_t* p=_funcStartsBegin; p < _funcStartsEnd; ) {
        bool malformed;
        uint64_t value = read_uleb128(p, _funcStartsEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128 in function-starts data");
        // a delta of zero marks end of functionStarts stream
        if ( value == 0 ) {
            while ( p < _funcStartsEnd ) {
                if ( *p++ != 0 )
                    return Error("padding at end of function-starts not all zeros");
            }
            return Error::none();
        }
        runtimeOffset += value;
        if ( runtimeOffset > maxFuncOffset )
            return Error("functions-starts has entry beyond end of TEXT");
    };
    return Error("functions-starts not zero terminated");
}

void FunctionStarts::forEachFunctionStart(uint64_t loadAddr, void (^callback)(uint64_t funcAddr)) const
{
    uint64_t runtimeOffset = 0;
    for (const uint8_t* p=_funcStartsBegin; p < _funcStartsEnd; ) {
        bool malformed;
        uint64_t value = read_uleb128(p, _funcStartsEnd, malformed);
        // a delta of zero marks end of functionStarts stream
        if ( malformed || (value == 0) )
            return;
        runtimeOffset += value;
        callback(loadAddr+runtimeOffset);
    };
}




} // namespace mach_o
