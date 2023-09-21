/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_GradedArchitectures_h
#define mach_o_GradedArchitectures_h

#include <stdint.h>

#include <span>

#include "Defines.h"
#include "Architecture.h"


namespace mach_o {

/*!
 * @class GradedArchitectures
 *
 * @abstract
 *      Encapsulates a prioritized list of architectures
 *      Used to select slice from a fat file.
 *      Never dynamically constructed. Instead one of the existing static members is used.
 */
class VIS_HIDDEN GradedArchitectures {
public:

    bool                    hasCompatibleSlice(std::span<const Architecture> slices, bool isOSBinary, uint32_t& bestSliceIndex) const;
    bool                    isCompatible(Architecture arch, bool isOSBinary=false) const;

    bool                    checksOSBinary() const { return _requiresOSBinaries; }

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
    static const GradedArchitectures&  currentLaunch(const char* simArches); // for emulating how the kernel chooses which slice to exec()
#endif
    static const GradedArchitectures&  currentLoad(bool keysOff, bool platformBinariesOnly);

    // pre-built objects for use by dyld to see if a slice is loadable
    static constinit const GradedArchitectures load_mac;
    static constinit const GradedArchitectures load_macHaswell;
    static constinit const GradedArchitectures load_arm64;                 // iPhone 8 (no PAC)
    static constinit const GradedArchitectures load_arm64e;                // AppleSilicon or A12 and later
    static constinit const GradedArchitectures load_arm64e_keysOff;        // running AppStore app with keys disabled
    static constinit const GradedArchitectures load_arm64e_osBinaryOnly;   // don't load third party arm64e code
    static constinit const GradedArchitectures load_watchSeries3;
    static constinit const GradedArchitectures load_watchSeries4;

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
    // pre-built objects for use to see if a program is launchable
    static constinit const GradedArchitectures launch_iOS;                // arm64e iOS
    static constinit const GradedArchitectures launch_mac;                // Intel macs
    static constinit const GradedArchitectures launch_macHaswell;         // Intel macs with haswell cpu
    static constinit const GradedArchitectures launch_macAppleSilicon;    // Apple Silicon macs
    static constinit const GradedArchitectures launch_sim;                // iOS simulator for Intel macs
    static constinit const GradedArchitectures launch_simAppleSilicon;    // iOS simulator for Apple Silicon macs
#endif

private:

        constexpr GradedArchitectures(const Architecture* const a[], uint32_t size, bool requiresOSBinaries=false)
                    : _archs(a), _archCount(size/sizeof(Architecture)), _requiresOSBinaries(requiresOSBinaries) { }
                  GradedArchitectures(const GradedArchitectures&) = delete;

    // Note: this is structured so that the static members (e.g. load_arm64) can be statically built (no initializer)
    const Architecture* const* _archs;
    uint32_t            const  _archCount;
    bool                const  _requiresOSBinaries;
};

} // namespace mach_o

#endif /* mach_o_GradedArchitectures_h */
