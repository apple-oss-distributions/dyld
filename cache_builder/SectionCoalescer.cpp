/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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

#include "SectionCoalescer.h"


// Returns true if the section was removed from the source dylib after being optimized
bool DylibSectionCoalescer::sectionWasRemoved(std::string_view segmentName,
                                              std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) ) {
        // Some sections, eg, GOTs, are optimized but not removed
        if ( !section->sectionWillBeRemoved )
            return false;

        return !section->offsetMap.empty();
    }

    return false;
}

// Returns true if the section was optimized.  It may or may not have been removed too, see sectionWasRemoved().
bool DylibSectionCoalescer::sectionWasOptimized(std::string_view segmentName,
                                                std::string_view sectionName) const
{
    if ( const OptimizedSection* section = this->getSection(segmentName, sectionName) )
        return !section->offsetMap.empty();

    return false;
}

DylibSectionCoalescer::OptimizedSection* DylibSectionCoalescer::getSection(std::string_view segmentName,
                                                                           std::string_view sectionName)
{
    if (segmentName.size() > 16)
        segmentName = segmentName.substr(0, 16);
    if (sectionName.size() > 16)
        sectionName = sectionName.substr(0, 16);

    if ( segmentName == "__TEXT" ) {
        if ( sectionName == "__objc_classname" )
            return &this->objcClassNames;
        if ( sectionName == "__objc_methname" )
            return &this->objcMethNames;
        if ( sectionName == "__objc_methtype" )
            return &this->objcMethTypes;
    } else if ( segmentName == "__DATA_CONST" ) {
        if ( sectionName == "__got" )
            return &this->gots;
    } else if ( segmentName == "__AUTH_CONST" ) {
        if ( sectionName == "__auth_got" )
            return &this->auth_gots;
    }

    return nullptr;
}

const DylibSectionCoalescer::OptimizedSection* DylibSectionCoalescer::getSection(std::string_view segmentName,
                                                                                 std::string_view sectionName) const
{
    return ((DylibSectionCoalescer*)this)->getSection(segmentName, sectionName);
}
