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

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "GradedArchitectures.h"


namespace mach_o {


bool GradedArchitectures::hasCompatibleSlice(std::span<const Architecture> slices, bool isOSBinary, uint32_t& bestSliceIndex) const
{
    if ( _requiresOSBinaries && !isOSBinary )
        return false;
    // by walking in arch order, the first match we find is the best
    for (uint32_t a=0; a < _archCount; ++a ) {
        for (uint32_t s=0; s < slices.size(); ++s ) {
            if ( slices[s] == *_archs[a] ) {
                bestSliceIndex = s;
                return true;
            }
        }
    }
    return false;
}

bool GradedArchitectures::isCompatible(Architecture arch, bool isOSBinary) const
{
    if ( _requiresOSBinaries && !isOSBinary )
        return false;
    for (uint32_t i=0; i < _archCount; ++i ) {
        if ( arch == *_archs[i] )
            return true;
    }
    return false;
}

// This is all goop to allow these to be statically compiled down (no initializer needed)
static const Architecture* archs_mac[]                  = { &Architecture::x86_64 };
static const Architecture* archs_macHaswell[]           = { &Architecture::x86_64h, &Architecture::x86_64 };
static const Architecture* archs_arm64[]                = { &Architecture::arm64, &Architecture::arm64_alt };
static const Architecture* archs_arm64e[]               = { &Architecture::arm64e };
static const Architecture* archs_arm64e_keysOff[]       = { &Architecture::arm64e, &Architecture::arm64e_v1, &Architecture::arm64, &Architecture::arm64_alt };
static const Architecture* archs_watchSeries3[]         = { &Architecture::armv7k };
static const Architecture* archs_watchSeries4[]         = { &Architecture::arm64_32 };
#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
static const Architecture* archs_AppleSilicon[]         = { &Architecture::arm64e, &Architecture::arm64, &Architecture::x86_64 };
static const Architecture* archs_iOS[]                  = { &Architecture::arm64e, &Architecture::arm64e_v1, &Architecture::arm64, &Architecture::arm64_alt};
#endif

constinit const GradedArchitectures GradedArchitectures::load_mac(                archs_mac,            sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::load_macHaswell(         archs_macHaswell,     sizeof(archs_macHaswell));
constinit const GradedArchitectures GradedArchitectures::load_arm64(              archs_arm64,          sizeof(archs_arm64));
constinit const GradedArchitectures GradedArchitectures::load_arm64e(             archs_arm64e,         sizeof(archs_arm64e));
constinit const GradedArchitectures GradedArchitectures::load_arm64e_keysOff(     archs_arm64e_keysOff, sizeof(archs_arm64e_keysOff));
constinit const GradedArchitectures GradedArchitectures::load_arm64e_osBinaryOnly(archs_arm64e,         sizeof(archs_arm64e), true);
constinit const GradedArchitectures GradedArchitectures::load_watchSeries3(       archs_watchSeries3,   sizeof(archs_watchSeries3));
constinit const GradedArchitectures GradedArchitectures::load_watchSeries4(       archs_watchSeries4,   sizeof(archs_watchSeries4));


#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
// pre-built objects for use to see if a program is launchable
constinit const GradedArchitectures GradedArchitectures::launch_iOS(            archs_iOS,          sizeof(archs_iOS));
constinit const GradedArchitectures GradedArchitectures::launch_mac(            archs_mac,          sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::launch_macHaswell(     archs_macHaswell,   sizeof(archs_macHaswell));
constinit const GradedArchitectures GradedArchitectures::launch_macAppleSilicon(archs_AppleSilicon, sizeof(archs_AppleSilicon));
constinit const GradedArchitectures GradedArchitectures::launch_sim(            archs_mac,          sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::launch_simAppleSilicon(archs_arm64,        sizeof(archs_arm64));
#endif


} // namespace mach_o





