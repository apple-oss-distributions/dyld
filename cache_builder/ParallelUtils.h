/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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

#ifndef ParallelUtils_hpp
#define ParallelUtils_hpp

#include "Array.h"
#include "Error.h"

#include <dispatch/dispatch.h>
#include <span>
#include <vector>

namespace parallel
{

template<typename T>
static error::Error forEach(std::span<T> array, error::Error (^callback)(size_t index, T& element))
{
    const bool RunInSerial = false;

    dispatch_queue_t queue = RunInSerial ? dispatch_queue_create("serial", DISPATCH_QUEUE_SERIAL) : DISPATCH_APPLY_AUTO;

    BLOCK_ACCCESSIBLE_ARRAY(error::Error, errors, array.size());

    dispatch_apply(array.size(), queue, ^(size_t iteration) {
        T& element = array[iteration];
        errors[iteration] = callback(iteration, element);
    });

    if ( RunInSerial )
        dispatch_release(queue);

    // Return the first error we find
    for ( uint32_t i = 0; i != array.size(); ++i ) {
        error::Error& err = errors[i];
        if ( err.hasError() )
            return std::move(err);
    }

    return error::Error();
}

// Because "could not match 'span' against 'vector'", for some reason
template<typename T>
static error::Error forEach(std::vector<T>& array, error::Error (^callback)(size_t index, T& element))
{
    return forEach(std::span<T>(array), callback);
}

} // namespace parallel

#endif /* ParallelUtils_hpp */
