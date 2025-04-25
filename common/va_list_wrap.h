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

#ifndef va_list_wrap_h
#define va_list_wrap_h

#include <stdarg.h>
#include <type_traits>

// a type-safe va_list wrapper
struct va_list_wrap
{
  // on x86_64 va_list is defined as a compiler synthesized type __va_list_tag[1]
  // when passed around functions that array becomes a pointer
  // so use std::decay to strip the array type and use a pointer instead
  std::decay_t<va_list> list;

  va_list_wrap(va_list list): list(list) {}
};

#endif /* mach_o_Version64_h */
