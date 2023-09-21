/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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
#include <assert.h>

#include "Fixups.h"

namespace mach_o {

const char* Fixup::keyName() const
{
    assert(authenticated);
    static const char* const names[] = {
        "IA", "IB", "DA", "DB"
    };
    return names[this->auth.key];
}

bool Fixup::operator==(const Fixup& other) const
{
    if ( location != other.location )
        return false;
    if ( segment != other.segment )
        return false;
    if ( authenticated != other.authenticated )
        return false;
    if ( authenticated ) {
        if ( auth.key != other.auth.key )
            return false;
        if ( auth.usesAddrDiversity != other.auth.usesAddrDiversity )
            return false;
        if ( auth.diversity != other.auth.diversity )
            return false;
    }
    if ( isBind != other.isBind )
        return false;
    if ( isBind ) {
        if ( bind.bindOrdinal != other.bind.bindOrdinal )
            return false;
        if ( bind.embeddedAddend != other.bind.embeddedAddend )
            return false;
    }
    else {
        if ( rebase.targetVmOffset != other.rebase.targetVmOffset )
            return false;
    }
    return true;
}


} // namespace mach_o
