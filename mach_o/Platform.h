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


#ifndef mach_o_Platform_h
#define mach_o_Platform_h

#include <stdint.h>

#include <compare>
#include <string_view>

#include "CString.h"
#include "Defines.h"
#include "Version32.h"
#include "Error.h"

// Special platform value that represents zippered (macOS + macCatalyst) platform.
#define PLATFORM_ZIPPERED 0xFF000001

namespace mach_o {

class PlatformInfo;
class Policy;
class Architecture;


/*!
 * @class Platform
 *
 * @abstract
 *      A type safe wrapper for PLATFORM_* uint32_t values.
 */
class VIS_HIDDEN Platform
{
public:
                        Platform(const Platform& other) : _info(other._info) { }
                        Platform(uint32_t platformNumber);
                        Platform(): Platform(0) {}

    // checks if constructed Platform is a known platform
    Error               valid() const;

    // returns true if constructed Plaform is unknown
    bool                empty() const;

    // returns static string or "unknown"
    CString             name() const;

    // returns if this is a simulator platform
    bool                isSimulator() const;


    // return PLATFORM_ number
    uint32_t            value() const;

    // returns if binaries might be FairPlay encrypted
    bool                maybeFairPlayEncrypted() const;

    // returns if platform is valid to link with this platform
    bool                canLoad(Platform other) const;

    // returns currently running platform
    static Platform     current();
    static Platform     byName(std::string_view name);

    bool                operator==(const Platform& other) const { return (_info == other._info); }
    Platform&           operator=(const Platform& other) { _info = other._info; return *this; }

    // known platforms
    static constinit const Platform     macOS;
    static constinit const Platform     iOS;
    static constinit const Platform     tvOS;
    static constinit const Platform     watchOS;
    static constinit const Platform     bridgeOS;
    static constinit const Platform     macCatalyst;
    static constinit const Platform     zippered;
    static constinit const Platform     iOS_simulator;
    static constinit const Platform     tvOS_simulator;
    static constinit const Platform     watchOS_simulator;
    static constinit const Platform     driverKit;
    static constinit const Platform     firmware;
    static constinit const Platform     sepOS;

private:

    /*!
     * @class Epoch
     *
     * @abstract
     *      Represents major OS releases across all platforms. Internal
     *      encoding is hidden, but you can use comparison operators to
     *      determine ordering of Epochs.  This class is only used by
     *      Platform and Policy.
     */
    class Epoch
    {
    public:
        auto            operator<=>(const Epoch& other) const = default;

        static constinit const Epoch      invalid;
        static constinit const Epoch      fall2015;
        static constinit const Epoch      fall2016;
        static constinit const Epoch      fall2017;
        static constinit const Epoch      fall2018;
        static constinit const Epoch      fall2019;
        static constinit const Epoch      fall2020;
        static constinit const Epoch    spring2021;
        static constinit const Epoch      fall2021;
        static constinit const Epoch      fall2022;
        static constinit const Epoch      fall2023;
        static constinit const Epoch    spring2024;
        static constinit const Epoch      fall2024;

    private:
        friend class PlatformInfo; // to get access to year()

        unsigned            year() const { return _value/10; }
        bool                isSpring() const { return ((_value % 10) == 0); }
                  constexpr Epoch(unsigned year, bool spring=false) : _value(year*10 + (spring ? 0 : 5)) { }
        uint32_t            _value;
    };
    friend class Policy; // to get access to Epoch
    friend class PlatformInfo; // to get access to Epoch

    // returns which Epoch a particular platform version corresponds to
    Epoch               epoch(Version32) const;

    explicit constexpr  Platform(const PlatformInfo& info) : _info(&info) { }
    const PlatformInfo*  _info;
};


struct PlatformAndVersions
{

    mach_o::Platform  platform;
    mach_o::Version32 minOS;
    mach_o::Version32 sdk;

    // TODO: temporary version for zippered macCatalyst, till we figure out the exact minor version mapping
    mach_o::Version32 zipMinOS;
    mach_o::Version32 zipSdk;

    /// Zipping corresponds to mach-o build version load command semantics.
    ///     - macOS and macCatalyst load commands together create a 'zippered' platform.
    ///     - Same platforms can't be zipped together, the versions would be ambiguous.
    ///     - Valid platform can be zipped into an invalid platform with value 0 to override it, but not the other way around.
    ///         This allows to use default `PlatformAndVersions` constructor and iterate load commands to infer effective platform.
    Error       zip(const PlatformAndVersions& other);

    /// Unzip platform into an equivalent of load commands platforms.
    /// For Platform::zippered this is macOS and macCatalyst, for all other platforms it's the current platform.
    void        unzip(void(^)(PlatformAndVersions)) const;

    /// Number of load commands necessary to represent unzipped platform in mach-o.
    uint32_t    loadCommandsCount() const;

    /// Parse llvm target triples (e.g. arm64-apple-macosx12.0.0)
    Error       setFromTargetTriple(CString triple, Architecture& arch);
};

} // namespace mach_o

#endif /* mach_o_Platforms_h */
