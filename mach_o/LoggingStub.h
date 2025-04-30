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

#ifndef mach_o_LoggingStub_h
#define mach_o_LoggingStub_h

#include <cstdarg>

#include "MachODefines.h"
#include "va_list_wrap.h"

namespace mach_o
{

using WarningHandler = void(*)(const void* context, const char* format, va_list_wrap);
void setWarningHandler(WarningHandler) VIS_HIDDEN;
bool hasWarningHandler() VIS_HIDDEN;

__attribute__((format(printf, 2, 3)))
void warning(const void* context, const char* format, ...) VIS_HIDDEN;
}

#endif // mach_o_LoggingStub_h
