/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#include <iterator>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <charconv>
#include <TargetConditionals.h>
#include <mach-o/loader.h>

#include "Platform.h"
#include "Architecture.h"

#ifndef PLATFORM_VISIONOS
  #define PLATFORM_VISIONOS 11
#endif

#ifndef PLATFORM_VISIONOSSIMULATOR
  #define PLATFORM_VISIONOSSIMULATOR 12
#endif

#ifndef PLATFORM_MACOS_EXCLAVECORE
  #define PLATFORM_MACOS_EXCLAVECORE 15
#endif

#ifndef PLATFORM_MACOS_EXCLAVEKIT
  #define PLATFORM_MACOS_EXCLAVEKIT 16
#endif

#ifndef PLATFORM_IOS_EXCLAVECORE
  #define PLATFORM_IOS_EXCLAVECORE 17
#endif

#ifndef PLATFORM_IOS_EXCLAVEKIT
  #define PLATFORM_IOS_EXCLAVEKIT 18
#endif

#ifndef PLATFORM_TVOS_EXCLAVECORE
  #define PLATFORM_TVOS_EXCLAVECORE 19
#endif

#ifndef PLATFORM_TVOS_EXCLAVEKIT
  #define PLATFORM_TVOS_EXCLAVEKIT 20
#endif

#ifndef PLATFORM_WATCHOS_EXCLAVECORE
  #define PLATFORM_WATCHOS_EXCLAVECORE 21
#endif

#ifndef PLATFORM_WATCHOS_EXCLAVEKIT
  #define PLATFORM_WATCHOS_EXCLAVEKIT 22
#endif

#ifndef PLATFORM_VISIONOS_EXCLAVECORE
  #define PLATFORM_VISIONOS_EXCLAVECORE 23
#endif

#ifndef PLATFORM_VISIONOS_EXCLAVEKIT
  #define PLATFORM_VISIONOS_EXCLAVEKIT 24
#endif



namespace mach_o {


//
// MARK: --- Epoch values ---
//

constinit const Platform::Epoch Platform::Epoch::invalid(0);
constinit const Platform::Epoch Platform::Epoch::fall2012(2012);
constinit const Platform::Epoch Platform::Epoch::fall2015(2015);
constinit const Platform::Epoch Platform::Epoch::fall2016(2016);
constinit const Platform::Epoch Platform::Epoch::fall2017(2017);
constinit const Platform::Epoch Platform::Epoch::fall2018(2018);
constinit const Platform::Epoch Platform::Epoch::fall2019(2019);
constinit const Platform::Epoch Platform::Epoch::spring2020(2020, true);
constinit const Platform::Epoch Platform::Epoch::fall2020(2020);
constinit const Platform::Epoch Platform::Epoch::fall2021(2021);
constinit const Platform::Epoch Platform::Epoch::spring2021(2021, true);
constinit const Platform::Epoch Platform::Epoch::fall2022(2022);
constinit const Platform::Epoch Platform::Epoch::fall2023(2023);
constinit const Platform::Epoch Platform::Epoch::spring2024(2024, true);
constinit const Platform::Epoch Platform::Epoch::fall2024(2024);
constinit const Platform::Epoch Platform::Epoch::spring2025(2025, true);
constinit const Platform::Epoch Platform::Epoch::fall2025(2025);



//
// MARK: --- PlatformInfo class ---
//

/*!
 * @class PlatformInfo
 *
 * @abstract
 *      Implementation details for Platform.
 */
class VIS_HIDDEN PlatformInfo
{
public:
    consteval PlatformInfo(uint32_t v, CString nm, bool isSim, bool fp, uint16_t year, const PlatformInfo* info, CString altName = {})
                             : basePlatformInfo(info), name(nm), altName(altName), value(v), baseYear(year), isSimulator(isSim), supportsFairPlayEncryption(fp) { }

    const PlatformInfo* basePlatformInfo;
    CString             name;
    CString             altName;
    uint32_t            value;
    uint16_t            baseYear;    // year 1.0 shipped
    bool                isSimulator;
    bool                isExclaveCore=false;
    bool                isExclaveKit=false;
    bool                supportsFairPlayEncryption;
    

    // Epoch is private to Platform and PlatformInfo, so convert to year/spring of use by subclasses
    Version32 versionForEpoch(Platform::Epoch e) const {
        return versionForYear(e.year(), e.isSpring());
    }

    // Epoch is private to Platform and PlatformInfo, so convert to year/spring of use by subclasses
    Platform::Epoch epochForVersion(Version32 vers) const {
        uint16_t year;
        bool     spring;
        this->yearForVersion(vers, year, spring);
        return Platform::Epoch(year, spring);
    }

    virtual Version32 versionForYear(uint16_t year, bool spring) const {
        return majorVersionFromBaseYear(year, spring);
    }
    virtual void yearForVersion(Version32 vers, uint16_t& year, bool& spring) const {
        yearForMajorVersion(vers, year, spring);
    }
    virtual uint16_t minorVersionForSpring(uint16_t major) const {
        // most spring releses are X.4
        return 4;
    }


protected:
    friend class Platform;
    static constinit const PlatformInfo* knownPlatformInfos[];

    // version bumped by 1.0 each Fall, started at baseYear
    Version32 majorVersionFromBaseYear(uint16_t year, bool spring) const {
        unsigned major = year - baseYear;
        uint16_t minor = 0;
        if ( spring ) {
            --major;
            minor = minorVersionForSpring(major);
        }
        return Version32(major, minor);
    }

    // version bumped by 0.1 each Fall
    Version32 tenVersionFromBaseYear(uint16_t year, bool spring, uint16_t tenBaseYear) const {
        // version is 10.xx
        uint16_t subVersion = year - tenBaseYear;
        uint8_t  dot        = 0;
        if ( spring ) {
            --subVersion;
            dot = 4;
        }
        return Version32(10, subVersion, dot);
    }

    void yearForMajorVersion(Version32 vers, uint16_t& year, bool& spring) const {
        // version is >= 11.0
        year = baseYear + vers.major();
        spring = (vers.minor() >= minorVersionForSpring(vers.major()));
        // Say the 2023 fall release has the year 2023, then we want the following spring release,
        // what availability calls 2023(e) to actually be in calendar year 2024
        if ( spring )
            ++year;
    }

    void yearForTenMinorVersion(Version32 vers, uint16_t tenBaseYear, uint16_t& year, bool& spring) const {
        // version is 10.x
        year = tenBaseYear + ((vers.value() - 0x000A0000) >> 8);
        spring = ((vers.value() & 0x000000FF) >= 0x04);
        if ( spring )
            --year;
    }
};


class VIS_HIDDEN PlatformInfo_macOS : public PlatformInfo
{
public:
    consteval PlatformInfo_macOS() : PlatformInfo(PLATFORM_MACOS, "macOS", false, false, 2009, &PlatformInfo_macOS::singleton, "macOSX") { }

    Version32 versionForYear(uint16_t year, bool spring) const override {
        if ( (year > 2020) || ((year ==2020) && !spring) )
            return majorVersionFromBaseYear(year, spring);       // 2020 - 2009 -> 11.0
        else
            return tenVersionFromBaseYear(year, spring, 2004);   // 2019 - 2004 -> 10.15
    }

    void yearForVersion(Version32 vers, uint16_t& year, bool& spring) const override {
        if ( vers >= Version32(11,0) )
            yearForMajorVersion(vers, year, spring);             // 11.0 -> 2020
        else
            yearForTenMinorVersion(vers, 2004, year, spring);    // 10.15 -> 2019
    }

    uint16_t minorVersionForSpring(uint16_t major) const override {
        // The past releases have been 11.3, 12.3, 13.3, so assume that pattern for those releases.
        if ( major <= 13 )
            return 3;

        // 14.4 needs a 4
        // also assume that later releases are .4, just to be conservative
        return 4;
    }

    static const PlatformInfo_macOS singleton;
protected:
    consteval PlatformInfo_macOS(uint32_t v, CString nm, bool isSim, const PlatformInfo* info = &PlatformInfo_macOS::singleton) 
                                    : PlatformInfo(v, nm, isSim, false, 2009, info) { }
};
const PlatformInfo_macOS        PlatformInfo_macOS::singleton;


class VIS_HIDDEN PlatformInfo_iOS : public PlatformInfo
{
public:
    consteval PlatformInfo_iOS() : PlatformInfo(PLATFORM_IOS, "iOS", false, true, 2006, &PlatformInfo_iOS::singleton) { }

    static const PlatformInfo_iOS singleton;
protected:
    consteval PlatformInfo_iOS(uint32_t v, CString nm, bool isSim, const PlatformInfo* info = &PlatformInfo_iOS::singleton, CString altName = {})
                                : PlatformInfo(v, nm, isSim, !isSim, 2006, info, altName) { }
};
const PlatformInfo_iOS      PlatformInfo_iOS::singleton;


// iOS_simulator uses same versioning as iOS
class VIS_HIDDEN PlatformInfo_iOS_simulator : public PlatformInfo_iOS
{
public:
    consteval PlatformInfo_iOS_simulator() : PlatformInfo_iOS(PLATFORM_IOSSIMULATOR, "iOS-simulator", true) { }

    static const PlatformInfo_iOS_simulator singleton;
};
const PlatformInfo_iOS_simulator         PlatformInfo_iOS_simulator::singleton;


// tvOS uses same versioning as iOS
class VIS_HIDDEN PlatformInfo_tvOS : public PlatformInfo_iOS
{
public:
    consteval PlatformInfo_tvOS() : PlatformInfo_iOS(PLATFORM_TVOS, "tvOS", false, &PlatformInfo_tvOS::singleton) { }

    static const PlatformInfo_tvOS  singleton;
protected:
    consteval PlatformInfo_tvOS(uint32_t v, CString nm, bool isSim, const PlatformInfo* info = &PlatformInfo_tvOS::singleton)
                                : PlatformInfo_iOS(v, nm, isSim, info) { }
};
const PlatformInfo_tvOS      PlatformInfo_tvOS::singleton;


// tvOS_simulator uses same versioning as tvOS
class VIS_HIDDEN PlatformInfo_tvOS_simulator : public PlatformInfo_tvOS
{
public:
    consteval PlatformInfo_tvOS_simulator() : PlatformInfo_tvOS(PLATFORM_TVOSSIMULATOR, "tvOS-simulator", true) { }

    static const PlatformInfo_tvOS_simulator    singleton;
};
const PlatformInfo_tvOS_simulator  PlatformInfo_tvOS_simulator::singleton;




// catalyst uses same versioning as iOS
class VIS_HIDDEN PlatformInfo_macCatalyst : public PlatformInfo_iOS
{
public:
    consteval PlatformInfo_macCatalyst() : PlatformInfo_iOS(PLATFORM_MACCATALYST, "macCatalyst", false, &PlatformInfo_iOS::singleton, "Mac Catalyst") {
        supportsFairPlayEncryption = false;
    }

    static const PlatformInfo_macCatalyst   singleton;
};
const PlatformInfo_macCatalyst PlatformInfo_macCatalyst::singleton;

class VIS_HIDDEN PlatformInfo_zippered : public PlatformInfo_macOS
{
public:
    consteval PlatformInfo_zippered() : PlatformInfo_macOS(PLATFORM_ZIPPERED, "zippered(macOS/Catalyst)", false) { }

    static const PlatformInfo_zippered   singleton;
};
const PlatformInfo_zippered PlatformInfo_zippered::singleton;


class VIS_HIDDEN PlatformInfo_watchOS : public PlatformInfo
{
public:
    consteval PlatformInfo_watchOS() : PlatformInfo(PLATFORM_WATCHOS, "watchOS", false, true, 2013, &PlatformInfo_watchOS::singleton) { }

    static const PlatformInfo_watchOS   singleton;
protected:
    consteval PlatformInfo_watchOS(uint32_t v, CString nm, bool isSim, const PlatformInfo* info = &PlatformInfo_watchOS::singleton)
                                    : PlatformInfo(v, nm, isSim, !isSim, 2013, info) { }
};
const PlatformInfo_watchOS    PlatformInfo_watchOS::singleton;


// watchOS_simulator uses same versioning as watchOS
class VIS_HIDDEN PlatformInfo_watchOS_simulator: public PlatformInfo_watchOS
{
public:
    consteval PlatformInfo_watchOS_simulator() : PlatformInfo_watchOS(PLATFORM_WATCHOSSIMULATOR, "watchOS-simulator", true) { }

    static const PlatformInfo_watchOS_simulator     singleton;
};
const PlatformInfo_watchOS_simulator PlatformInfo_watchOS_simulator::singleton;


class VIS_HIDDEN PlatformInfo_bridgeOS : public PlatformInfo
{
public:
    consteval PlatformInfo_bridgeOS() : PlatformInfo(PLATFORM_BRIDGEOS, "bridgeOS", false, false, 2015, &PlatformInfo_bridgeOS::singleton) { }

    uint16_t minorVersionForSpring(uint16_t major) const override {
        // The past 2 releases have been 7.3 and 8.3, so assume that pattern for those releases.
        if ( major <= 8 )
            return 3;

        // use .4 for future just in case it changes.  We'd rather be conservative for future
        // than accidentally opt in something we shouldn't
        return 4;
    }

    static const PlatformInfo_bridgeOS singleton;
protected:
    consteval PlatformInfo_bridgeOS(uint32_t v, CString nm) : PlatformInfo(v, nm, false, false, 2015, &PlatformInfo_bridgeOS::singleton) { }
};
const PlatformInfo_bridgeOS         PlatformInfo_bridgeOS::singleton;



class VIS_HIDDEN PlatformInfo_driverKit : public PlatformInfo
{
public:
    consteval PlatformInfo_driverKit() : PlatformInfo(PLATFORM_DRIVERKIT, "driverKit", false, true, 2000, &PlatformInfo_driverKit::singleton) { }

    static const PlatformInfo_driverKit singleton;
};
const PlatformInfo_driverKit         PlatformInfo_driverKit::singleton;


// firmware does not using versioning or epochs
class VIS_HIDDEN PlatformInfo_firmware : public PlatformInfo
{
public:
    consteval PlatformInfo_firmware() : PlatformInfo(PLATFORM_FIRMWARE, "firmware", false, false, 0, &PlatformInfo_firmware::singleton, "free standing") { }

    Version32 versionForYear(uint16_t year, bool spring) const override {
        return Version32(1,0);
    }
    void yearForVersion(Version32 vers, uint16_t& year, bool& spring) const override {
        year = 2020;
        spring = false;
    }

    static const PlatformInfo_firmware singleton;
};
const PlatformInfo_firmware         PlatformInfo_firmware::singleton;


// sepOS does not using versioning or epochs
class VIS_HIDDEN PlatformInfo_sepOS : public PlatformInfo
{
public:
    consteval PlatformInfo_sepOS() : PlatformInfo(PLATFORM_SEPOS, "sepOS", false, false, 0, &PlatformInfo_sepOS::singleton) { }

    Version32 versionForYear(uint16_t year, bool spring) const override {
        return Version32(1,0);
    }
    void yearForVersion(Version32 vers, uint16_t& year, bool& spring) const override {
        year = 2020;
        spring = false;
    }
    static const PlatformInfo_sepOS singleton;
};
const PlatformInfo_sepOS         PlatformInfo_sepOS::singleton;


// macOS_exclaveCore OS versioning follows macOS versioning
class VIS_HIDDEN PlatformInfo_macOS_exclaveCore : public PlatformInfo_macOS
{
public:
    consteval PlatformInfo_macOS_exclaveCore() : PlatformInfo_macOS(PLATFORM_MACOS_EXCLAVECORE, "macOS-exclaveCore", false, &PlatformInfo_macOS_exclaveCore::singleton)
    {
        isExclaveCore = true;
    }

    static const PlatformInfo_macOS_exclaveCore     singleton;
};
const PlatformInfo_macOS_exclaveCore PlatformInfo_macOS_exclaveCore::singleton;

// macOS_exclaveKit OS versioning follows macOS versioning
class VIS_HIDDEN PlatformInfo_macOS_exclaveKit : public PlatformInfo_macOS
{
public:
    consteval PlatformInfo_macOS_exclaveKit() : PlatformInfo_macOS(PLATFORM_MACOS_EXCLAVEKIT, "macOS-exclaveKit", false, &PlatformInfo_macOS_exclaveKit::singleton)
    {
        isExclaveKit = true;
    }

    static const PlatformInfo_macOS_exclaveKit     singleton;
};
const PlatformInfo_macOS_exclaveKit PlatformInfo_macOS_exclaveKit::singleton;


// iOS_exclaveCore OS versioning follows iOS versioning
class VIS_HIDDEN PlatformInfo_iOS_exclaveCore : public PlatformInfo_iOS
{
public:
    consteval PlatformInfo_iOS_exclaveCore() : PlatformInfo_iOS(PLATFORM_IOS_EXCLAVECORE, "iOS-exclaveCore", false, &PlatformInfo_iOS_exclaveCore::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveCore = true;
    }

    static const PlatformInfo_iOS_exclaveCore     singleton;
};
const PlatformInfo_iOS_exclaveCore PlatformInfo_iOS_exclaveCore::singleton;

// iOS_exclaveKit OS versioning follows iOS versioning
class VIS_HIDDEN PlatformInfo_iOS_exclaveKit : public PlatformInfo_iOS
{
public:
    consteval PlatformInfo_iOS_exclaveKit() : PlatformInfo_iOS(PLATFORM_IOS_EXCLAVEKIT, "iOS-exclaveKit", false, &PlatformInfo_iOS_exclaveKit::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveKit = true;
    }

    static const PlatformInfo_iOS_exclaveKit     singleton;
};
const PlatformInfo_iOS_exclaveKit PlatformInfo_iOS_exclaveKit::singleton;


// tvOS_exclaveCore OS versioning follows tvOS versioning
class VIS_HIDDEN PlatformInfo_tvOS_exclaveCore : public PlatformInfo_tvOS
{
public:
    consteval PlatformInfo_tvOS_exclaveCore() : PlatformInfo_tvOS(PLATFORM_TVOS_EXCLAVECORE, "tvOS-exclaveCore", false, &PlatformInfo_tvOS_exclaveCore::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveCore = true;
    }

    static const PlatformInfo_tvOS_exclaveCore     singleton;
};
const PlatformInfo_tvOS_exclaveCore PlatformInfo_tvOS_exclaveCore::singleton;

// tvOS_exclaveKit OS versioning follows tvOS versioning
class VIS_HIDDEN PlatformInfo_tvOS_exclaveKit : public PlatformInfo_tvOS
{
public:
    consteval PlatformInfo_tvOS_exclaveKit() : PlatformInfo_tvOS(PLATFORM_TVOS_EXCLAVEKIT, "tvOS-exclaveKit", false, &PlatformInfo_tvOS_exclaveKit::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveKit = true;
    }

    static const PlatformInfo_tvOS_exclaveKit     singleton;
};
const PlatformInfo_tvOS_exclaveKit PlatformInfo_tvOS_exclaveKit::singleton;


// watchOS_exclaveCore OS versioning follows watchOS versioning
class VIS_HIDDEN PlatformInfo_watchOS_exclaveCore : public PlatformInfo_watchOS
{
public:
    consteval PlatformInfo_watchOS_exclaveCore() : PlatformInfo_watchOS(PLATFORM_WATCHOS_EXCLAVECORE, "watchOS-exclaveCore", false, &PlatformInfo_watchOS_exclaveCore::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveCore = true;
    }

    static const PlatformInfo_watchOS_exclaveCore  singleton;
};
const PlatformInfo_watchOS_exclaveCore PlatformInfo_watchOS_exclaveCore::singleton;

// watchOS_exclaveKit OS versioning follows watchOS versioning
class VIS_HIDDEN PlatformInfo_watchOS_exclaveKit : public PlatformInfo_watchOS
{
public:
    consteval PlatformInfo_watchOS_exclaveKit() : PlatformInfo_watchOS(PLATFORM_WATCHOS_EXCLAVEKIT, "watchOS-exclaveKit", false, &PlatformInfo_watchOS_exclaveKit::singleton)
    {
        supportsFairPlayEncryption = false;
        isExclaveKit = true;
    }

    static const PlatformInfo_watchOS_exclaveKit  singleton;
};
const PlatformInfo_watchOS_exclaveKit PlatformInfo_watchOS_exclaveKit::singleton;

class VIS_HIDDEN PlatformInfo_visionOS : public PlatformInfo
{
public:
    consteval PlatformInfo_visionOS() : PlatformInfo(PLATFORM_VISIONOS, "visionOS", false, true, 2022, &PlatformInfo_visionOS::singleton, "xrOS") {}

    uint16_t minorVersionForSpring(uint16_t major) const override {
        // The first spring release is 1.1
        if ( major == 1 )
            return 1;

        // use .4 for future just in case it changes from the above.  We'd rather be conservative
        // for future than accidentally opt in something we shouldn't
        return 4;
    }

    static const PlatformInfo_visionOS singleton;
protected:
    consteval PlatformInfo_visionOS(uint32_t v, CString nm, bool isSim, const PlatformInfo* info = &PlatformInfo_visionOS::singleton, CString altName = {}) : PlatformInfo(v, nm, isSim, !isSim, 2022, info, altName) { }
};
const PlatformInfo_visionOS          PlatformInfo_visionOS::singleton;

class VIS_HIDDEN PlatformInfo_visionOS_simulator : public PlatformInfo_visionOS
{
public:
    consteval PlatformInfo_visionOS_simulator() : PlatformInfo_visionOS(PLATFORM_VISIONOSSIMULATOR, "visionOS-simulator", true, &PlatformInfo_visionOS::singleton, "xrOS-simulator") { }

    static const PlatformInfo_visionOS_simulator    singleton;
};
const PlatformInfo_visionOS_simulator          PlatformInfo_visionOS_simulator::singleton;

class VIS_HIDDEN PlatformInfo_visionOS_exclaveCore : public PlatformInfo_visionOS
{
public:
    consteval PlatformInfo_visionOS_exclaveCore() : PlatformInfo_visionOS(PLATFORM_VISIONOS_EXCLAVECORE, "visionOS-exclaveCore", false, &PlatformInfo_visionOS_exclaveCore::singleton, "xrOS-exclaveCore")
    {
        supportsFairPlayEncryption = false;
        isExclaveCore = true;
    }

    static const PlatformInfo_visionOS_exclaveCore  singleton;
};
const PlatformInfo_visionOS_exclaveCore          PlatformInfo_visionOS_exclaveCore::singleton;

class VIS_HIDDEN PlatformInfo_visionOS_exclaveKit : public PlatformInfo_visionOS
{
public:
    consteval PlatformInfo_visionOS_exclaveKit() : PlatformInfo_visionOS(PLATFORM_VISIONOS_EXCLAVEKIT, "visionOS-exclaveKit", false, &PlatformInfo_visionOS_exclaveKit::singleton, "xrOS-exclaveKit")
    {
        supportsFairPlayEncryption = false;
        isExclaveKit = true;
    }

    static const PlatformInfo_visionOS_exclaveKit  singleton;
};
const PlatformInfo_visionOS_exclaveKit          PlatformInfo_visionOS_exclaveKit::singleton;




// for constructing a Platform() by value
constinit const PlatformInfo* PlatformInfo::knownPlatformInfos[] = {
    &PlatformInfo_macOS::singleton,
    &PlatformInfo_iOS::singleton,
    &PlatformInfo_tvOS::singleton,
    &PlatformInfo_watchOS::singleton,
    &PlatformInfo_bridgeOS::singleton,
    &PlatformInfo_macCatalyst::singleton,
    &PlatformInfo_zippered::singleton,
    &PlatformInfo_iOS_simulator::singleton,
    &PlatformInfo_tvOS_simulator::singleton,
    &PlatformInfo_watchOS_simulator::singleton,
    &PlatformInfo_driverKit::singleton,
    &PlatformInfo_firmware::singleton,
    &PlatformInfo_sepOS::singleton,
    &PlatformInfo_visionOS::singleton,
    &PlatformInfo_visionOS_simulator::singleton,
    &PlatformInfo_macOS_exclaveCore::singleton,
    &PlatformInfo_macOS_exclaveKit::singleton,
    &PlatformInfo_iOS_exclaveCore::singleton,
    &PlatformInfo_iOS_exclaveKit::singleton,
    &PlatformInfo_tvOS_exclaveCore::singleton,
    &PlatformInfo_tvOS_exclaveKit::singleton,
    &PlatformInfo_watchOS_exclaveCore::singleton,
    &PlatformInfo_watchOS_exclaveKit::singleton,
    &PlatformInfo_visionOS_exclaveCore::singleton,
    &PlatformInfo_visionOS_exclaveKit::singleton,

};



//
// MARK: --- Platform methods ---
//


Platform::Platform(uint32_t platformNumber) : _info(nullptr), _value(0)
{
    for (const PlatformInfo* p : PlatformInfo::knownPlatformInfos) {
        assert(p->value != 0 && "PlatformInfo value uninitialized, this might be a problem with C++ static initializers order");
        if ( p->value == platformNumber ) {
            _info = p;
            return;
        }
    }
    _value = platformNumber;
}

Platform Platform::byName(std::string_view name) {
    auto normalizedStringComp = [](char c1, char c2) {
        return (c1 == c2 || toupper(c1) == toupper(c2)
                || (c1 == ' ' && c2 == '-') || (c1 == '-' && c2 == ' '));
    };
    // check if this is a platform name in all platforms
    for ( const PlatformInfo* p : PlatformInfo::knownPlatformInfos ) {
        std::string_view pname = p->name;
        if ( std::equal(name.begin(), name.end(), pname.begin(), pname.end(), normalizedStringComp) )
            return Platform(*p);

        pname = p->altName;
        if ( std::equal(name.begin(), name.end(), pname.begin(), pname.end(), normalizedStringComp) )
            return Platform(*p);
    }

    // check if this is a raw platform number
    uint32_t num = 0;
    const char* end = name.data() + name.size();
    if ( std::from_chars_result res = std::from_chars(name.data(), end, num);
            res.ec == std::errc() && res.ptr == end
            && Platform(num).valid().noError() )
        return Platform(num);

    // Hack for -macabi
    if ( name == "ios-macabi" )
        return Platform::macCatalyst;

    return Platform(0);
}

Error Platform::valid() const
{
    if ( _info == nullptr )
        return Error("unknown platform");
    return Error();
}

bool Platform::empty() const
{
    return (_info == nullptr) && _value == 0;
}

CString Platform::name() const
{
    if ( _info == nullptr ) {
        if ( _value != 0 )
            return "future";
        return "unknown";
    }
    return _info->name;
}

Platform Platform::basePlatform() const
{
    if ( _info == nullptr ) {
        if ( _value != 0 )
            return *this;
        return Platform();
    }
    return Platform(*(_info->basePlatformInfo));
}

bool Platform::isSimulator() const
{
    if ( _info == nullptr )
        return false;
    return _info->isSimulator;
}

bool Platform::isExclaveCore() const
{
    if ( _info == nullptr )
        return false;
    return _info->isExclaveCore;
}

bool Platform::isExclaveKit() const
{
    if ( _info == nullptr )
        return false;
    return _info->isExclaveKit;
}


bool Platform::maybeFairPlayEncrypted() const
{
    return (_info != nullptr) && _info->supportsFairPlayEncryption;
}

bool Platform::canLoad(Platform other) const
{
    // can always link/load something built for same platform
    if ( _info == other._info )
        return true;

    // macOS and catalyst can link against zippered dylibs
    if ( other == Platform::zippered ) {
        if ( *this == Platform::macOS )
            return true;
        if ( *this == Platform::macCatalyst )
            return true;
    }

    return false;
}

CString Platform::libSystemDir() const
{
    if ( *this == Platform::sepOS )
        return "";
    if ( *this == Platform::firmware )
        return "";
    if ( isExclaveCore() )
        return "";

    if ( *this == Platform::driverKit )
        return "/System/DriverKit/usr/lib/system/";
    if ( isExclaveKit() )
        return "/System/ExclaveKit/usr/lib/system/";
    return "/usr/lib/system/";
}

uint32_t Platform::value() const
{
    return _info ? _info->value : _value;
}

Platform::Epoch Platform::epoch(Version32 v) const
{
    if ( _info != nullptr )
        return _info->epochForVersion(v);
    else
        return Epoch::invalid;
}


Platform Platform::current()
{
#if TARGET_OS_SIMULATOR
  #if TARGET_OS_WATCH
    return watchOS_simulator;
  #elif TARGET_OS_TV
    return tvOS_simulator;
  #elif TARGET_OS_VISION
    return visionOS_simulator;  // before TARGET_OS_IOS because TARGET_OS_IOS is set on visionOS internal SDK
  #elif TARGET_OS_IOS
    return iOS_simulator;
  #endif
#elif TARGET_OS_EXCLAVEKIT
  #if TARGET_OS_WATCH
    return watchOS_exclaveKit;
  #elif TARGET_OS_TV
    return tvOS_exclaveKit;
  #elif TARGET_OS_IOS
    return iOS_exclaveKit;
  #elif TARGET_OS_VISION
    return visionOS_exclaveKit;
  #else
    return macOS_exclaveKit;
  #endif
#elif TARGET_OS_BRIDGE
    return bridgeOS;
#elif TARGET_OS_WATCH
    return watchOS;
#elif TARGET_OS_TV
    return tvOS;
#elif TARGET_OS_VISION
    return visionOS;  // before TARGET_OS_IOS because TARGET_OS_IOS is set on visionOS internal SDK
#elif TARGET_OS_IOS
    return iOS;
#elif TARGET_OS_OSX
    return macOS;
#elif TARGET_OS_DRIVERKIT
    return driverKit;
#else
  #error unknown platform
#endif
}

constinit const Platform Platform::macOS(             PlatformInfo_macOS::singleton);
constinit const Platform Platform::iOS(               PlatformInfo_iOS::singleton);
constinit const Platform Platform::tvOS(              PlatformInfo_tvOS::singleton);
constinit const Platform Platform::watchOS(           PlatformInfo_watchOS::singleton);
constinit const Platform Platform::bridgeOS(          PlatformInfo_bridgeOS::singleton);
constinit const Platform Platform::macCatalyst(       PlatformInfo_macCatalyst::singleton);
constinit const Platform Platform::zippered(          PlatformInfo_zippered::singleton);
constinit const Platform Platform::iOS_simulator(     PlatformInfo_iOS_simulator::singleton);
constinit const Platform Platform::tvOS_simulator(    PlatformInfo_tvOS_simulator::singleton);
constinit const Platform Platform::watchOS_simulator( PlatformInfo_watchOS_simulator::singleton);
constinit const Platform Platform::driverKit(         PlatformInfo_driverKit::singleton);
constinit const Platform Platform::firmware(          PlatformInfo_firmware::singleton);
constinit const Platform Platform::sepOS(             PlatformInfo_sepOS::singleton);
constinit const Platform Platform::visionOS(          PlatformInfo_visionOS::singleton);
constinit const Platform Platform::visionOS_simulator(PlatformInfo_visionOS_simulator::singleton);
constinit const Platform Platform::macOS_exclaveCore(     PlatformInfo_macOS_exclaveCore::singleton);
constinit const Platform Platform::macOS_exclaveKit(      PlatformInfo_macOS_exclaveKit::singleton);
constinit const Platform Platform::iOS_exclaveCore(       PlatformInfo_iOS_exclaveCore::singleton);
constinit const Platform Platform::iOS_exclaveKit(        PlatformInfo_iOS_exclaveKit::singleton);
constinit const Platform Platform::tvOS_exclaveCore(      PlatformInfo_tvOS_exclaveCore::singleton);
constinit const Platform Platform::tvOS_exclaveKit(       PlatformInfo_tvOS_exclaveKit::singleton);
constinit const Platform Platform::watchOS_exclaveCore(   PlatformInfo_watchOS_exclaveCore::singleton);
constinit const Platform Platform::watchOS_exclaveKit(    PlatformInfo_watchOS_exclaveKit::singleton);
constinit const Platform Platform::visionOS_exclaveCore(  PlatformInfo_visionOS_exclaveCore::singleton);
constinit const Platform Platform::visionOS_exclaveKit(   PlatformInfo_visionOS_exclaveKit::singleton);


Error PlatformAndVersions::zip(const PlatformAndVersions& other)
{
    // Use other platform to initialize an empty platform.
    if ( platform.empty() ) {
        *this = other;
        return Error::none();
    }

    if ( Error err = other.platform.valid() ) {
        return Error("can't zip with invalid platform");
    }

    if ( platform == other.platform ) {
        *this = other;
        return Error::none();
    }

    if ( platform == Platform::macOS && other.platform == Platform::macCatalyst ) {
        *this = { Platform::zippered, minOS, sdk, other.minOS, other.sdk };
        return Error::none();
    }

    if ( other.platform == Platform::macOS && platform == Platform::macCatalyst ) {
        *this = { Platform::zippered, other.minOS, other.sdk, minOS, sdk };
        return Error::none();
    }

    // Handle additional -macos_version_min/-maccatalyst_version_min when we are already zippered
    if ( platform == Platform::zippered ) {
        if ( other.platform == Platform::macCatalyst ) {
            *this = { Platform::zippered, minOS, sdk, other.minOS, other.sdk };
            return Error::none();
        }
        if ( other.platform == Platform::macOS ) {
            *this = { Platform::zippered, other.minOS, other.sdk, zipMinOS, zipSdk };
            return Error::none();
        }
    }

    return Error("incompatible platforms: %s - %s", platform.name().c_str(), other.platform.name().c_str());
}

void PlatformAndVersions::unzip(void(^ callback)(PlatformAndVersions)) const
{
    if ( platform != Platform::zippered ) {
        callback(*this);
        return;
    }

    callback({Platform::macOS, minOS, sdk});
    callback({Platform::macCatalyst, zipMinOS, zipSdk});
}

uint32_t PlatformAndVersions::loadCommandsCount() const
{
    return (platform == Platform::zippered ? 2 : 1);
}


Error PlatformAndVersions::setFromTargetTriple(CString triple, Architecture& outArch)
{
    char splitBuf[triple.size()+1];
    strlcpy(splitBuf, triple.c_str(), sizeof(splitBuf));

    // replace dash with nul so there are 3 or 4 strings: arch, vendor, osVersion, env
    const char* arch   = splitBuf;
    const char* vendor = nullptr;
    const char* osVers = nullptr;
    const char* env    = nullptr;
    for (int i=0; i < sizeof(splitBuf); i++) {
        if ( splitBuf[i] == '-' ) {
            splitBuf[i] = '\0';
            if ( vendor == nullptr )
                vendor = &splitBuf[i+1];
            else if ( osVers == nullptr )
                osVers = &splitBuf[i+1];
            else if ( env == nullptr )
                env = &splitBuf[i+1];
            else
                return Error("more than three dashes in target triple '%s'", triple.c_str());
        }
    }
    if ( (vendor == nullptr) || (osVers == nullptr) )
        return Error("missing dashes in target triple '%s'", triple.c_str());


    // also return architecture from triple
    outArch = Architecture::byName(arch);
    if ( outArch == Architecture() )
        return Error("unknown architecture in target triple '%s'", triple.c_str());

    // firmware uses triples like "thumbv7m-apple-unknown-macho"
    if ( strcmp(osVers, "unknown") == 0 ) {
        this->platform = Platform::firmware;
        this->minOS    = Version32(0,0);
        this->sdk      = Version32(0,0);
        return Error::none();
    }

    // split osVersion into osName and minOS version
    const size_t osVersLen = strlen(osVers);
    char osName[triple.size()+1];
    char minOSVers[osVersLen+1];
    strlcpy(osName, osVers, sizeof(osName));
    minOSVers[0] = '\0';
    for (int i=0; i < osVersLen; i++) {
        if ( isdigit(osName[i]) ) {
            strlcpy(minOSVers, &osName[i], sizeof(minOSVers));
            osName[i] = '\0';
            break;
        }
    }

    //  macosx is historical name
    if ( strcmp(osName, "macosx") == 0 )
        strlcpy(osName, "macos", sizeof(osName));

    // Apple sub-platforms are fourth part of triple, but need to be added to OS name to make platform name
    // e.g. "arm64-apple-tvos16.0-simulator" --> "tvos-simulator"
    if ( env != nullptr ) {
        strlcat(osName, "-", sizeof(osName));
        strlcat(osName, env, sizeof(osName));
    }
    this->platform = Platform::byName(osName);
    if ( this->platform.empty() )
        return Error("unknown OS in target triple '%s'", triple.c_str());

    if ( (strlen(minOSVers) == 0) && (this->platform != Platform::firmware) )
        return Error("missing OS version in target triple '%s'", triple.c_str());

    // get minOS from version trailing OS name in triple
    if (Error err = Version32::fromString(minOSVers, this->minOS))
        return err;
    
    // SDK version is not encoded in triple
    this->sdk = Version32(0,0);

    return Error::none();
}




} // namespace mach_o





