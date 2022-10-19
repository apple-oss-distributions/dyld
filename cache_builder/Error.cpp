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

#include <iostream>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <errno.h>
#include <_simple.h>
#include <unistd.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <libc_private.h>

#include "Error.h"

namespace error {


Error::Error(Error&& other)
{
    _buffer = other._buffer;
    other._buffer = nullptr;
}

Error& Error::operator=(Error&& other)
{
    _buffer = other._buffer;
    other._buffer = nullptr;
    return *this;
}

Error::~Error()
{
   if ( _buffer )
        _simple_sfree(_buffer);
    _buffer = nullptr;
}

Error::Error(const char* format, ...)
{
    va_list    list;
    va_start(list, format);
    if ( _buffer == nullptr )
        _buffer = _simple_salloc();
    _simple_vsprintf(_buffer, format, list);
    va_end(list);
}

Error::Error(const char* format, va_list list)
{
    if ( _buffer == nullptr )
        _buffer = _simple_salloc();
// FIXME: Remove ignore once rdar://84603673 is resolved.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    _simple_vsprintf(_buffer, format, list);
#pragma clang diagnostic pop
}

const char* Error::message() const
{
    return _buffer ? _simple_string(_buffer) : "";
}

bool Error::messageContains(const char* subString) const
{
    if ( _buffer == nullptr )
        return false;
    return (strstr(_simple_string(_buffer), subString) != nullptr);
}

} // namespace error
