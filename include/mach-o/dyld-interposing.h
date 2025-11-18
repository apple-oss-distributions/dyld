/*
 * Copyright (c) 2005-2008 Apple Computer, Inc. All rights reserved.
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

#if !defined(_DYLD_INTERPOSING_H_)
#define _DYLD_INTERPOSING_H_

/*
 *  Example:
 *
 *  static
 *  int
 *  my_open(const char* path, int flags, mode_t mode)
 *  {
 *    int value;
 *    // do stuff before open (including changing the arguments)
 *    value = open(path, flags, mode);
 *    // do stuff after open (including changing the return value(s))
 *    return value;
 *  }
 *  DYLD_INTERPOSE(my_open, open)
 */

#if __has_feature(ptrauth_intrinsics) && __has_include(<ptrauth.h>)
#include <ptrauth.h>
  #define __DYLD_INTERPOSE_PTRAUTH_SCHEMA(_replacement, _replacee, _fieldname) \
    __ptrauth(ptrauth_key_process_independent_code,\
    /*address discriminated*/ 1, \
    __builtin_ptrauth_string_discriminator("DYLD_INTERPOSE" __FILE_NAME__ #_replacement #_replacee "::" #_fieldname) \
   )
#else
  #define __DYLD_INTERPOSE_PTRAUTH_SCHEMA(_replacement, _replacee, _fieldname)
#endif

#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct {\
        const void* __DYLD_INTERPOSE_PTRAUTH_SCHEMA(_replacement, _replacee, replacement) replacement;\
        const void* __DYLD_INTERPOSE_PTRAUTH_SCHEMA(_replacement, _replacee, replacee) replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose,interposing"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

#endif
