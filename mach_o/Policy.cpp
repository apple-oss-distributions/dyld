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

#include "Defines.h"
#include "Error.h"
#include "Platform.h"
#include "Architecture.h"
#include "Version32.h"
#include "Policy.h"


namespace mach_o {


//
// MARK: --- Policy methods ---
//

Policy::Policy(Architecture arch, PlatformAndVersions pvs, uint32_t filetype, bool pathMayBeInSharedCache)
 : _featureEpoch(pvs.platform.epoch(pvs.minOS)), _enforcementEpoch(pvs.platform.epoch(pvs.sdk)),
   _arch(arch), _pvs(pvs), _filetype(filetype), _pathMayBeInSharedCache(pathMayBeInSharedCache)
{
}

bool Policy::dyldLoadsOutput() const
{
    switch (_filetype) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
        case MH_DYLINKER:
            return true;
    }
    return false;
}


// features
Policy::Usage Policy::useBuildVersionLoadCommand() const
{
    if ( _pvs.platform == Platform::bridgeOS )
        return Policy::mustUse;
    return (_featureEpoch >= Platform::Epoch::fall2018) ? Policy::preferUse : Policy::mustNotUse;
}

Policy::Usage Policy::useDataConst() const
{
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


Policy::Usage Policy::useChainedFixups() const
{
    // fixups are for userland binaries
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    // there is no chained fixups for old archs
    if ( !_arch.usesArm64Instructions() && !_arch.usesx86_64Instructions() ) {
        return Policy::mustNotUse;
    }

    // in general Fall2020 OSs supported chained fixups
    Platform::Epoch chainedFixupsEpoch = Platform::Epoch::fall2020;

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
    }

    // use chained fixups for newer OS releases
    if ( _featureEpoch >= chainedFixupsEpoch )
        return Policy::preferUse;

    return Policy::mustNotUse;
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
    // Only userland uses LC_SOURCE_VERSION
    if ( !dyldLoadsOutput() )
        return Policy::preferDontUse;

    if ( _featureEpoch >= Platform::Epoch::fall2015 )
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
    if ( _filetype == MH_DYLIB && _pathMayBeInSharedCache )
        return true;
    return false;
}

bool Policy::canUseDelayInit() const
{
    // runtime support added in Fall 2024
    return ( _featureEpoch >= Platform::Epoch::fall2024 );
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
    return (_enforcementEpoch >= Platform::Epoch::fall2021);
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
    return (_enforcementEpoch >= Platform::Epoch::fall2024);
}

bool Policy::enforceDataSegmentPermissions() const
{
    // dylibs in shared region don't set SG_READ_ONLY because of __objc_const
    if ( _pathMayBeInSharedCache )
        return false;
    return (_enforcementEpoch >= Platform::Epoch::fall2023);
}









} // namespace mach_o

