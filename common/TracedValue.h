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
#ifndef __TRACED_VALUE__
#define __TRACED_VALUE__

// std
#include <memory>

namespace ld {

/// TracedValue wrapps  options with some default value that might be explicity
/// overwritten. The wrapper is constructed with `isDefault` flag set to true,
/// but whenever a new value is assigned to an existing wrapper instance
/// isDefault becomes false too.
template<typename Val>
struct TracedValue {

  protected:
  Val val;

  public:
  bool isDefault;

  TracedValue() = delete;
  constexpr TracedValue(Val val, bool isDefault = true) noexcept: val(std::move(val)), isDefault(isDefault) {}
  constexpr TracedValue(TracedValue&&) = default;
  constexpr TracedValue(const TracedValue&) = default;

  constexpr operator Val() const { return val; }
  constexpr Val get() const { return val; }

  constexpr TracedValue& operator=(TracedValue&& other)
  {
    val = std::move(other.val);
    isDefault = false;
    return *this;
  }

  constexpr TracedValue& operator=(const TracedValue& other)
  {
    val = other.val;
    isDefault = false;
    return *this;
  }

  constexpr TracedValue& operator=(Val newValue)
  {
    val = std::move(newValue);
    isDefault = false;
    return *this;
  }

  // Set new value without changing the isDefault flag.
  constexpr void overwrite(Val newDefault)
  {
    val = std::move(newDefault);
  }

  constexpr void overwrite(Val newVal, bool newIsDefault) {
    val = std::move(newVal);
    isDefault = std::move(newIsDefault);
  }
};

struct TracedBool: public TracedValue<bool> {

  TracedBool() = delete;
  constexpr TracedBool(bool val, bool isDefault = true): TracedValue(val, isDefault) {}

  constexpr operator bool() const { return val; }
  constexpr bool isForceOn() const { return !isDefault && val; }
  constexpr bool isForceOff() const { return !isDefault && !val; }
};
}
#endif // __TRACED_VALUE__
