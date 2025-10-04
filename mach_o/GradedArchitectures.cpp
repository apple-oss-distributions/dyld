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

#if __x86_64__
  #include <mach/mach.h>
  #include <mach/host_info.h>
  #include <mach/mach_host.h>
#endif


#include "GradedArchitectures.h"
#include "SupportedArchs.h"


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

void GradedArchitectures::forEachArch(bool isOSBinary, void (^handler)(Architecture arch)) const {
    if ( _requiresOSBinaries && !isOSBinary )
        return;
    
    for (uint32_t i=0; i < _archCount; ++i ) {
        handler(*_archs[i]);
    }
}

#if __x86_64__  && !TARGET_OS_SIMULATOR
static bool isHaswell()
{
    // FIXME: figure out a commpage way to check this
    struct host_basic_info info;
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    mach_port_t hostPort = mach_host_self();
    kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), hostPort);
    return (result == KERN_SUCCESS) && (info.cpu_subtype == CPU_SUBTYPE_X86_64_H);
}
#endif

const GradedArchitectures&  GradedArchitectures::currentLaunch(const char* simArches)
{
#if TARGET_OS_SIMULATOR
    // on Apple Silicon, there is both an arm64 and an x86_64 (under rosetta) simulators
    // You cannot tell if you are running under rosetta, so CoreSimulator sets SIMULATOR_ARCHS
    if ( strcmp(simArches, "arm64 x86_64") == 0 )
        return launch_simAppleSilicon;
    else
        return launch_mac;
#elif TARGET_OS_OSX
  #if __arm64__
    return launch_macAppleSilicon;
  #else
    return isHaswell() ? launch_macHaswell : launch_mac;
  #endif
#else
    // all other platforms use same grading for executables as dylibs
    return currentLoad(true, false);
#endif
    
}

const GradedArchitectures&  GradedArchitectures::currentLoad(bool keysOff, bool platformBinariesOnly)
{
#if __arm64e__
    if ( platformBinariesOnly )
        return (keysOff ? load_arm64e_keysOff_osBinaryOnly : load_arm64e_osBinaryOnly);
    else
        return (keysOff ? load_arm64e_keysOff : load_arm64e);
#elif __ARM64_ARCH_8_32__
    return load_watchSeries4;
#elif __arm64__
    return load_arm64;
#elif __x86_64__
 #if TARGET_OS_SIMULATOR
    return load_mac;
  #else
    return isHaswell() ? load_macHaswell : load_mac;
  #endif
#else
    #error unknown platform
#endif
}

const GradedArchitectures& GradedArchitectures::forName(std::string_view archName, bool keysOff, bool isKernel)
{
    if ( archName == "x86_64h" )
        return load_macHaswell;
    else if ( archName == "x86_64" )
        return load_mac;
#if SUPPORT_ARCH_arm64e
    else if ( archName == "arm64e" )
        return isKernel ? load_arm64e_kernel : (keysOff ? load_arm64e_keysOff : load_arm64e);
    else if ( archName == "arm64e.kernel" )
        return load_arm64e_kernel;
#endif
    else if ( archName == "arm64" )
        return load_arm64;
    else if ( archName == "armv7k" )
        return load_watchSeries3;
#if SUPPORT_ARCH_arm64_32
    else if ( archName == "arm64_32" )
        return load_watchSeries4;
#endif
    assert(0 && "unknown arch name");
}

// This is all goop to allow these to be statically compiled down (no initializer needed)
static const Architecture* archs_mac[]                  = { &Architecture::x86_64 };
static const Architecture* archs_macHaswell[]           = { &Architecture::x86_64h, &Architecture::x86_64 };
static const Architecture* archs_arm64[]                = { &Architecture::arm64, &Architecture::arm64_alt };
static const Architecture* archs_arm64e[]               = { &Architecture::arm64e };
static const Architecture* archs_arm64e_kernel[]        = { &Architecture::arm64e_kernel };
static const Architecture* archs_arm64e_keysOff[]       = { &Architecture::arm64e, &Architecture::arm64e_v1, &Architecture::arm64, &Architecture::arm64_alt };
static const Architecture* archs_watchSeries3[]         = { &Architecture::armv7k };
static const Architecture* archs_watchSeries4[]         = { &Architecture::arm64_32 };
static const Architecture* archs_AppleSilicon[]         = { &Architecture::arm64e, &Architecture::arm64, &Architecture::x86_64 };
static const Architecture* archs_iOS[]                  = { &Architecture::arm64e, &Architecture::arm64e_v1, &Architecture::arm64, &Architecture::arm64_alt};


constinit const GradedArchitectures GradedArchitectures::load_mac(                        archs_mac,            sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::load_macHaswell(                 archs_macHaswell,     sizeof(archs_macHaswell));
constinit const GradedArchitectures GradedArchitectures::load_arm64(                      archs_arm64,          sizeof(archs_arm64));
constinit const GradedArchitectures GradedArchitectures::load_arm64e(                     archs_arm64e,         sizeof(archs_arm64e));
constinit const GradedArchitectures GradedArchitectures::load_arm64e_kernel(              archs_arm64e_kernel,  sizeof(archs_arm64e_kernel));
constinit const GradedArchitectures GradedArchitectures::load_arm64e_keysOff(             archs_arm64e_keysOff, sizeof(archs_arm64e_keysOff));
constinit const GradedArchitectures GradedArchitectures::load_arm64e_osBinaryOnly(        archs_arm64e,         sizeof(archs_arm64e), true);
constinit const GradedArchitectures GradedArchitectures::load_arm64e_keysOff_osBinaryOnly(archs_arm64e_keysOff, sizeof(archs_arm64e_keysOff), true);
constinit const GradedArchitectures GradedArchitectures::load_watchSeries3(               archs_watchSeries3,   sizeof(archs_watchSeries3));
constinit const GradedArchitectures GradedArchitectures::load_watchSeries4(               archs_watchSeries4,   sizeof(archs_watchSeries4));


// pre-built objects for use to see if a program is launchable
constinit const GradedArchitectures GradedArchitectures::launch_iOS(            archs_iOS,          sizeof(archs_iOS));
constinit const GradedArchitectures GradedArchitectures::launch_mac(            archs_mac,          sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::launch_macHaswell(     archs_macHaswell,   sizeof(archs_macHaswell));
constinit const GradedArchitectures GradedArchitectures::launch_macAppleSilicon(archs_AppleSilicon, sizeof(archs_AppleSilicon));
constinit const GradedArchitectures GradedArchitectures::launch_sim(            archs_mac,          sizeof(archs_mac));
constinit const GradedArchitectures GradedArchitectures::launch_simAppleSilicon(archs_arm64,        sizeof(archs_arm64));


} // namespace mach_o





