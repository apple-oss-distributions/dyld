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

/*
 Portions derived from:
 
 ------------------------------------------------------------------------------
 // MurmurHash2 was written by Austin Appleby, and is placed in the public
 // domain. The author hereby disclaims copyright to this source code.
 Source is https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp
------------------------------------------------------------------------------
*/

#include "MurmurHash.h"

uint64_t murmurHash(const void* key, int len, uint64_t seed)
{
  const uint64_t magic = 0xc6a4a7935bd1e995ULL;
  const int salt = 47;

  uint64_t hash = seed ^ (len * magic);

  const uint64_t * data = (const uint64_t *)key;
  const uint64_t * end = data + (len/8);

  while(data != end)
  {
    uint64_t val = *data++;

    val *= magic;
    val ^= val >> salt;
    val *= magic;

    hash ^= val;
    hash *= magic;
  }

  const unsigned char * data2 = (const unsigned char*)data;

  switch(len & 7)
  {
      case 7: hash ^= uint64_t(data2[6]) << 48; break;
      case 6: hash ^= uint64_t(data2[5]) << 40; break;
      case 5: hash ^= uint64_t(data2[4]) << 32; break;
      case 4: hash ^= uint64_t(data2[3]) << 24; break;
      case 3: hash ^= uint64_t(data2[2]) << 16; break;
      case 2: hash ^= uint64_t(data2[1]) << 8; break;
      case 1: hash ^= uint64_t(data2[0]); break;
  };
    hash *= magic;

    hash ^= hash >> salt;
    hash *= magic;
    hash ^= hash >> salt;

  return hash;
}
