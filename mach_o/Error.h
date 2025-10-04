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


#ifndef mach_o_Error_h
#define mach_o_Error_h

#include <TargetConditionals.h>
#include <stdarg.h>

#include "MachODefines.h"
#include "va_list_wrap.h"

namespace mach_o {

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
                    Error(const char* format, va_list_wrap vaWrap) __attribute__((format(printf, 2, 0)));
                    // va_list is declared as char*
                    // use va_list_wrap, or cast real string arguments to const char* to avoid ambiguity
                    Error(const char* format, va_list) = delete;
                    Error(Error&&); // can move
                    Error& operator=(const Error&) = delete; //  can't copy assign
                    Error& operator=(Error&&); // can move
                    ~Error();


    void            append(const char* format, ...)  __attribute__((format(printf, 2, 3)));
    bool            hasError() const { return (_buffer != nullptr); }
    bool            noError() const  { return (_buffer == nullptr); }

    // These conversion operators only allow conversions on named variables, eg, `if ( Error e = func() )`
    // They do not allow conversions of unnnamed temporaries, ie, rvalue refs: `if ( func() )`
    // This is because it is easy to know from a named variable that "true" means there's an error,
    // but might be hard to know from a function call that "true" means the function returns an error
    explicit        operator bool() const & { return hasError(); }
    explicit        operator bool() const && = delete;

    const char*     message() const;
    bool            messageContains(const char* subString) const;

    static Error    copy(const Error&);

    static Error    none() { return Error(); }

private:

#if TARGET_OS_EXCLAVEKIT
    char            _strBuf[1024];
#endif
    void*           _buffer = nullptr;
};



} // namespace mach_o

#endif /* mach_o_Error_h */
