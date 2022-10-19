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

#ifndef Error_hpp
#define Error_hpp

#include "Defines.h"

#include <stdarg.h>

namespace error
{

/*!
 * @class Error
 *
 * @abstract
 *      Class for capturing error messages.
 *      Can be constructed with printf style format strings.
 *      Returned by mach-o "valid" methods.
 */
class VIS_HIDDEN [[nodiscard]] Error
{
public:
                    Error() = default;
                    Error(const char* format, ...)  __attribute__((format(printf, 2, 3)));
                    Error(const char* format, va_list list);
                    Error(const Error&) = delete;  // can't copy
                    Error(Error&&); // can move
                    Error& operator=(const Error&) = delete; //  can't copy assign
                    Error& operator=(Error&&); // can move
                    ~Error();


    bool            hasError() const { return (_buffer != nullptr); }
    bool            noError() const  { return (_buffer == nullptr); }
    explicit        operator bool() const { return hasError(); }
    const char*     message() const;
    bool            messageContains(const char* subString) const;

    static Error    none() { return Error(); }

private:

    void*           _buffer = nullptr;
};

} // namespace error

#endif /* Error_hpp */
