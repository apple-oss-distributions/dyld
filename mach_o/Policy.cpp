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

#include "Error.h"
#include "Platform.h"
#include "Architecture.h"
#include "Version32.h"
#include "Policy.h"
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>


namespace mach_o {


//
// MARK: --- Policy methods ---
//

Policy::Policy(Architecture arch, PlatformAndVersions pvs, uint32_t filetype, bool pathMayBeInSharedCache, bool kernel, bool staticExec)
 : _featureEpoch(pvs.platform.epoch(pvs.minOS)), _enforcementEpoch(pvs.platform.epoch(pvs.sdk)),
   _arch(arch), _pvs(pvs), _filetype(filetype), _pathMayBeInSharedCache(pathMayBeInSharedCache), _kernel(kernel), _staticExec(staticExec)
{
}

bool Policy::dyldLoadsOutput() const
{
    if ( _kernel )
        return false;

    if ( _staticExec )
        return false;

    switch (_filetype) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_DYLIB_STUB:
        case MH_BUNDLE:
        case MH_DYLINKER:
            return true;
    }
    return false;
}

bool Policy::kernelOrKext() const
{
    if ( _kernel )
        return true;
    return _filetype == MH_KEXT_BUNDLE;
}

// features
Policy::Usage Policy::useBuildVersionLoadCommand() const
{
    if ( _pvs.platform == Platform::bridgeOS )
        return Policy::mustUse;

    // all arm64 variants are new and use LC_BUILD_VERSION
    if ( _arch == Architecture::arm64 ) {
        // except for pre-12.0 iOS and tvOS devices
        if ( ((_pvs.platform == Platform::iOS) || (_pvs.platform == Platform::tvOS)) && (_featureEpoch < Platform::Epoch::fall2018) )
            return Policy::mustNotUse;
        return Policy::mustUse;
    }

    return (_featureEpoch >= Platform::Epoch::fall2018) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useDataConst() const
{
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    if ( _pvs.platform == Platform::firmware )
        return Policy::preferDontUse;

    return (_featureEpoch >= Platform::Epoch::fall2019) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useConstClassRefs() const
{
    return (_featureEpoch >= Platform::Epoch::spring2024) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useGOTforClassRefs() const
{
    return (_featureEpoch >= Platform::Epoch::fall2024) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useConstInterpose() const
{
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    return (_featureEpoch >= Platform::Epoch::fall2024) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useChainedFixups() const
{
    // arm64e kernel/kext use chained fixups
    if ( kernelOrKext() && _arch.usesArm64AuthPointers() )
        return Policy::mustUse;

    // firmware may use chained fixups, but has to opt-in
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    // there is no chained fixups for old archs
    if ( !_arch.usesArm64Instructions() && !_arch.usesx86_64Instructions() ) {
        return Policy::mustNotUse;
    }

    // in general Fall2020 OSs supported chained fixups
    Platform::Epoch chainedFixupsEpoch = Platform::Epoch::fall2020;

    if ( _pvs.platform == Platform::iOS ) // chained fixups on iOS since 13.4
        chainedFixupsEpoch = Platform::Epoch::spring2020;

    // simulators support is later than OS support
    if ( _pvs.platform.isSimulator() )
        chainedFixupsEpoch = Platform::Epoch::fall2021;

    // macOS support was delayed a year for builders to update OS
    if ( _pvs.platform == Platform::macOS ) {
        chainedFixupsEpoch = Platform::Epoch::fall2021;

        // x86 main executables might be tools and might need to run on older builders
        if ( _arch.usesx86_64Instructions() && (_filetype == MH_EXECUTE) ) {
            chainedFixupsEpoch = Platform::Epoch::fall2022;
        }

        // builders run on x86, for arm64e we allow chained fixups on 11.0 for the software update stack
        // rdar://118859281 (arm64e: Libraries need support for 11.0 deployment targets)
        if ( _arch.usesArm64AuthPointers() )
            chainedFixupsEpoch = Platform::Epoch::fall2020;
    }

    // use chained fixups for newer OS releases
    if ( _featureEpoch >= chainedFixupsEpoch )
        return Policy::preferUse;

    return Policy::mustNotUse;
}

uint16_t Policy::chainedFixupsFormat() const
{
    if ( _arch.usesArm64AuthPointers() ) {
        if ( !dyldLoadsOutput() )
            return DYLD_CHAINED_PTR_ARM64E_KERNEL;

        // 24-bit binds supported since iOS 15.0 and aligned releases
        if ( _featureEpoch >= Platform::Epoch::fall2021 )
            return DYLD_CHAINED_PTR_ARM64E_USERLAND24;

        return DYLD_CHAINED_PTR_ARM64E;
    } else if ( _arch.is64() ) {
        if ( !dyldLoadsOutput() )
            return DYLD_CHAINED_PTR_64_OFFSET;

        if ( _featureEpoch >= Platform::Epoch::fall2021 )
            return DYLD_CHAINED_PTR_64_OFFSET;

        return DYLD_CHAINED_PTR_64;
    } else {
        if ( dyldLoadsOutput() )
            return DYLD_CHAINED_PTR_32;
        return DYLD_CHAINED_PTR_32_FIRMWARE;
    }
}

Policy::Usage Policy::useOpcodeFixups() const
{
    // opcode fixups introduced in macOS 10.6
    if ( _arch.usesx86_64Instructions() && (_pvs.platform == Platform::macOS) && (_pvs.minOS < Version32(10,6)) )
        return Policy::mustNotUse;

    // if not pre-macOS 10.6, then complement useChainedFixups()
    switch ( this->useChainedFixups() ) {
        case preferUse:
            return preferDontUse;
        case mustUse:
            return mustNotUse;
        case preferDontUse:
            return preferUse;
        case mustNotUse:
            return mustUse;
    }
}

Policy::Usage Policy::useRelativeMethodLists() const
{
    // don't even look for objc on non-userland binaries
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    // main executables might be tools and might need to run on older builders
    if ( _arch.usesx86_64Instructions() && (_filetype == MH_EXECUTE) )
        return Policy::preferDontUse;

    // use chained fixups for newer OS releases
    if ( _featureEpoch >= Platform::Epoch::fall2020 )
        return Policy::preferUse;

    return Policy::mustNotUse;
}

Policy::Usage Policy::optimizeClassPatching() const
{
    if ( _filetype != MH_DYLIB )
        return Policy::mustNotUse;

    if ( _featureEpoch >= Platform::Epoch::fall2022 )
        return Policy::preferUse;

    return Policy::mustNotUse;
}

Policy::Usage Policy::optimizeSingletonPatching() const
{
    if ( _filetype != MH_DYLIB )
        return Policy::mustNotUse;

    if ( _featureEpoch >= Platform::Epoch::fall2022 )
        return Policy::preferUse;

    return Policy::mustNotUse;
}

Policy::Usage Policy::useAuthStubsInKexts() const
{
    if ( _arch.usesArm64AuthPointers() && (_filetype == MH_KEXT_BUNDLE) && (_featureEpoch >= Platform::Epoch::fall2021) )
        return Policy::mustUse;
    return Policy::preferDontUse;
}

Policy::Usage Policy::useDataConstForSelRefs() const
{
    // only dylibs that go into the dyld cache can use sel-refs in DATA_CONST
    if ( !_pathMayBeInSharedCache )
        return Policy::preferDontUse;

    // if minOS is new enough, enable
    if ( _featureEpoch >= Platform::Epoch::fall2021 )
        return Policy::preferUse;

    return Policy::preferDontUse;
}

Policy::Usage Policy::useSourceVersionLoadCommand() const
{
    // objects/firmware don't use LC_SOURCE_VERSION
    switch (_filetype) {
        case MH_OBJECT:
        case MH_PRELOAD:
            return Policy::preferDontUse;
        default:
            break;
    }

    if ( _featureEpoch >= Platform::Epoch::fall2012 )
        return Policy::preferUse;

    return Policy::preferDontUse;
}

Policy::Usage Policy::useLegacyLinkedit() const
{
    if ( dyldLoadsOutput() ) {
        // older releases didn't have a regular year-based version bump, so check the exact versions
        if ( _pvs.platform == Platform::macOS && _pvs.minOS < Version32(10, 6) )
            return Policy::mustUse;
        if ( _pvs.platform == Platform::iOS && _pvs.minOS < Version32(3, 1) )
            return Policy::mustUse;
    }

    return Policy::preferDontUse;
}

bool Policy::use4KBLoadCommandsPadding() const
{
    if ( (_filetype == MH_DYLIB || _filetype == MH_DYLIB_STUB) && _pathMayBeInSharedCache )
        return true;
    return false;
}

bool Policy::canUseDelayInit() const
{
    // runtime support added in Fall 2024
    return ( _featureEpoch >= Platform::Epoch::fall2024 );
}

bool Policy::useProtectedStack() const
{
    return false;
}

// enforcements
bool Policy::enforceReadOnlyLinkedit() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2015);
}

bool Policy::enforceLinkeditContentAlignment() const
{
    return (_filetype != MH_OBJECT) && (_enforcementEpoch >= Platform::Epoch::fall2018);
}

bool Policy::enforceOneFixupEncoding() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2018);
}

bool Policy::enforceSegmentOrderMatchesLoadCmds() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2019);
}

bool Policy::enforceTextSegmentPermissions() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2019);
}

bool Policy::enforceFixupsInWritableSegments() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2019);
}

bool Policy::enforceCodeSignatureAligned() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2019);
}

bool Policy::enforceSectionsInSegment() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2020);
}

bool Policy::enforceHasLinkedDylibs() const
{
    return (_enforcementEpoch >= Platform::Epoch::spring2025);
}

bool Policy::enforceInstallNamesAreRealPaths() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2021);
}

bool Policy::enforceHasUUID() const
{
    return (_filetype != MH_OBJECT) && (_enforcementEpoch >= Platform::Epoch::fall2021);
}

bool Policy::enforceMainFlagsCorrect() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2021);
}

bool Policy::enforceNoDuplicateDylibs() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2024);
}

bool Policy::enforceNoDuplicateRPaths() const
{
    return (_enforcementEpoch >= Platform::Epoch::spring2025);
}

bool Policy::enforceDataSegmentPermissions() const
{
    return (_enforcementEpoch >= Platform::Epoch::fall2025);
}

bool Policy::enforceDataConstSegmentPermissions() const
{
    // dylibs in shared region don't set SG_READ_ONLY because of __objc_const
    if ( _pathMayBeInSharedCache )
        return false;
    return (_enforcementEpoch >= Platform::Epoch::spring2025);
}

bool Policy::enforceImageListRemoveMainExecutable() const
{
    // Old simulators add the main executable to all_image_info in the simulator process, not in the host
    return (_enforcementEpoch <= Platform::Epoch::fall2022);
}

bool Policy::enforceSetSimulatorSharedCachePath() const
{
    // Old simulators do not correctly fill out the private cache fields in the all_image_info, so do it for them
    return (_enforcementEpoch <= Platform::Epoch::fall2021);
}









} // namespace mach_o

