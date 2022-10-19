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

#ifndef Types_h
#define Types_h

#include <optional>
#include <stdint.h>

// A VMOffset can be in the cache or an input dylib.  It's mostly going to be
// used to translate from one to the other
struct VMOffset
{
    __attribute__((nodebug))
    VMOffset() = default;
    explicit inline VMOffset(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    VMOffset(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A VM Address.  It's not specific to cache dylibs vs input dylibs
struct VMAddress
{
    __attribute__((nodebug))
    VMAddress() = default;
    explicit inline VMAddress(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    VMAddress(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    // Use an optional here and in following structs to see if it helps with tracking uninitialized values.
    // If its too error prone we can remove it.
    std::optional<uint64_t> value;
};

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// A VM Address inside the shared cache
struct CacheVMAddress
{
    __attribute__((nodebug))
    CacheVMAddress() = default;
    explicit inline CacheVMAddress(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    CacheVMAddress(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    // Use an optional here and in following structs to see if it helps with tracking uninitialized values.
    // If its too error prone we can remove it.
    std::optional<uint64_t> value;
};

// A VM Size within a cache dylib.  Eg, the size of a segment
struct CacheVMSize
{
    __attribute__((nodebug))
    CacheVMSize() = default;
    explicit inline CacheVMSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    CacheVMSize(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};
// The difference between 2 VM addresses might be a size, or might be an offset
// This handles both, allowing for implicit conversions to the one we need
struct VMOffsetOrCacheVMSize
{
    __attribute__((nodebug))
    VMOffsetOrCacheVMSize() = default;
    explicit inline VMOffsetOrCacheVMSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    VMOffsetOrCacheVMSize(int32_t) = delete;

    operator VMOffset() const
    {
        return VMOffset(value.value());
    }

    operator CacheVMSize() const
    {
        return CacheVMSize(value.value());
    }

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A file size within a cache dylib.  Eg, the file size of a segment
struct CacheFileSize
{
    __attribute__((nodebug))
    CacheFileSize() = default;
    explicit inline CacheFileSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    CacheFileSize(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A file offset within a cache dylib.  Eg, the file size of a segment
struct CacheFileOffset
{
    __attribute__((nodebug))
    CacheFileOffset() = default;
    explicit inline CacheFileOffset(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    CacheFileOffset(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};
#endif

// A VM Address inside a dylib which is a cache input
struct InputDylibVMAddress
{
    __attribute__((nodebug))
    InputDylibVMAddress() = default;
    explicit inline InputDylibVMAddress(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    InputDylibVMAddress(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A VM Size within a cache input dylib.  Eg, the size of a segment
struct InputDylibVMSize
{
    __attribute__((nodebug))
    InputDylibVMSize() = default;
    explicit inline InputDylibVMSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    InputDylibVMSize(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// The difference between 2 VM addresses might be a size, or might be an offset
// This handles both, allowing for implicit conversions to the one we need
struct VMOffsetOrInputDylibVMSize
{
    __attribute__((nodebug))
    VMOffsetOrInputDylibVMSize() = default;
    explicit inline VMOffsetOrInputDylibVMSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    VMOffsetOrInputDylibVMSize(int32_t) = delete;

    operator VMOffset() const
    {
        return VMOffset(value.value());
    }

    operator InputDylibVMSize() const
    {
        return InputDylibVMSize(value.value());
    }

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A file offset within an input dylib.  Eg, the file size of a segment
struct InputDylibFileOffset
{
    __attribute__((nodebug))
    InputDylibFileOffset() = default;
    explicit inline InputDylibFileOffset(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    InputDylibFileOffset(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

// A file size within an input dylib.  Eg, the file size of a segment
struct InputDylibFileSize
{
    __attribute__((nodebug))
    InputDylibFileSize() = default;
    explicit inline InputDylibFileSize(uint64_t value) : value(value) { }

    // Don't allow implicit int32_t cosntruction, as it will promote to uint64_t incorrectly
    InputDylibFileSize(int32_t) = delete;

    // FIXME: Eventually try remove all of these if we can.
    __attribute__((nodebug))
    inline uint64_t rawValue() const { return value.value(); }

private:
    std::optional<uint64_t> value;
};

//
// MARK: --- VMAddress methods ---
//

// VMAddr + VMOffset -> VMAddr
static inline VMAddress operator+(const VMAddress& a, const VMOffset& b)
{
    return VMAddress(a.rawValue() + b.rawValue());
}

static inline VMAddress& operator+=(VMAddress& a, const VMOffset& b)
{
    a = VMAddress(a.rawValue() + b.rawValue());
    return a;
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// VMAddr + VMSize -> VMAddr
static inline VMAddress operator+(const VMAddress& a, const CacheVMSize& b)
{
    return VMAddress(a.rawValue() + b.rawValue());
}

// VMAddr + VMSize -> VMAddr
static inline VMAddress& operator+=(VMAddress& a, const CacheVMSize& b)
{
    a = VMAddress(a.rawValue() + b.rawValue());
    return a;
}
#endif

// VMAddr - VMAddr -> VMOffset
static inline VMOffset operator-(const VMAddress& a, const VMAddress& b)
{
    return VMOffset(a.rawValue() - b.rawValue());
}

static inline bool operator<(const VMAddress& a, const VMAddress& b)
{
    return a.rawValue() < b.rawValue();
}

static inline bool operator>=(const VMAddress& a, const VMAddress& b)
{
    return a.rawValue() >= b.rawValue();
}

static inline bool operator==(const VMAddress& a, const VMAddress& b)
{
    return a.rawValue() == b.rawValue();
}

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
// VMAddr | literal -> VMAddr
static inline VMAddress operator|(const VMAddress& a, int i)
{
    return VMAddress(a.rawValue() | i);
}
#endif

//
// MARK: --- CacheVMAddress methods ---
//


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// VMAddr + VMOffset -> VMAddr
static inline CacheVMAddress operator+(const CacheVMAddress& a, const VMOffset& b)
{
    return CacheVMAddress(a.rawValue() + b.rawValue());
}

// VMAddr + VMSize -> VMAddr
static inline CacheVMAddress operator+(const CacheVMAddress& a, const CacheVMSize& b)
{
    return CacheVMAddress(a.rawValue() + b.rawValue());
}

// VMAddr + VMSize -> VMAddr
static inline CacheVMAddress& operator+=(CacheVMAddress& a, const CacheVMSize& b)
{
    a = CacheVMAddress(a.rawValue() + b.rawValue());
    return a;
}

static inline bool operator==(const CacheVMAddress& a, const CacheVMAddress& b)
{
    return a.rawValue() == b.rawValue();
}

// VMAddr - VMAddr -> VMOffsetOrSize
static inline VMOffsetOrCacheVMSize operator-(const CacheVMAddress& a, const CacheVMAddress& b)
{
    return VMOffsetOrCacheVMSize(a.rawValue() - b.rawValue());
}

static inline bool operator<(const CacheVMAddress& a, const CacheVMAddress& b)
{
    return a.rawValue() < b.rawValue();
}

static inline bool operator>=(const CacheVMAddress& a, const CacheVMAddress& b)
{
    return a.rawValue() >= b.rawValue();
}
#endif

//
// MARK: --- CacheVMSize methods ---
//

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// VMSize + VMSize -> VMSize
static inline CacheVMSize operator+(const CacheVMSize& a, const CacheVMSize& b)
{
    return CacheVMSize(a.rawValue() + b.rawValue());
}

// VMSize + VMSize -> VMSize
static inline CacheVMSize& operator+=(CacheVMSize& a, const CacheVMSize& b)
{
    a = CacheVMSize(a.rawValue() + b.rawValue());
    return a;
}

// VMSize - VMSize -> VMSize
static inline CacheVMSize operator-(const CacheVMSize& a, const CacheVMSize& b)
{
    return CacheVMSize(a.rawValue() - b.rawValue());
}

static inline bool operator>(const CacheVMSize& a, const CacheVMSize& b)
{
    return a.rawValue() > b.rawValue();
}

static inline bool operator<(const CacheVMSize& a, const CacheVMSize& b)
{
    return a.rawValue() < b.rawValue();
}

static inline bool operator<=(const CacheVMSize& a, const CacheVMSize& b)
{
    return a.rawValue() <= b.rawValue();
}

static inline bool operator==(const CacheVMSize& a, const CacheVMSize& b)
{
    return a.rawValue() == b.rawValue();
}
#endif

//
// MARK: --- CacheFileSize methods ---
//


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// file size + file size -> file size
static inline CacheFileSize operator+(const CacheFileSize& a, const CacheFileSize& b)
{
    return CacheFileSize(a.rawValue() + b.rawValue());
}

// VMSize + VMSize -> VMSize
static inline CacheFileSize& operator+=(CacheFileSize& a, const CacheFileSize& b)
{
    a = CacheFileSize(a.rawValue() + b.rawValue());
    return a;
}

static inline bool operator>(const CacheFileSize& a, const CacheFileSize& b)
{
    return a.rawValue() > b.rawValue();
}

static inline bool operator==(const CacheFileSize& a, const CacheFileSize& b)
{
    return a.rawValue() == b.rawValue();
}
#endif

//
// MARK: --- CacheFileOffset methods ---
//

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
// file offset + file size -> file offset
static inline CacheFileOffset operator+(const CacheFileOffset& a, const CacheFileSize& b)
{
    return CacheFileOffset(a.rawValue() + b.rawValue());
}

// file offset + file size -> file offset
static inline CacheFileOffset& operator+=(CacheFileOffset& a, const CacheFileSize& b)
{
    a = CacheFileOffset(a.rawValue() + b.rawValue());
    return a;
}

static inline bool operator==(const CacheFileOffset& a, const CacheFileOffset& b)
{
    return a.rawValue() == b.rawValue();
}

static inline bool operator>=(const CacheFileOffset& a, const CacheFileOffset& b)
{
    return a.rawValue() >= b.rawValue();
}

static inline bool operator<(const CacheFileOffset& a, const CacheFileOffset& b)
{
    return a.rawValue() < b.rawValue();
}
#endif

//
// MARK: --- InputDylibVMAddress methods ---
//

// VMAddr + VMSize -> VMAddr
static inline InputDylibVMAddress operator+(const InputDylibVMAddress& a, const InputDylibVMSize& b)
{
    return InputDylibVMAddress(a.rawValue() + b.rawValue());
}

// VMAddr + VMOffset -> VMAddr
static inline InputDylibVMAddress operator+(const InputDylibVMAddress& a, const VMOffset& b)
{
    return InputDylibVMAddress(a.rawValue() + b.rawValue());
}

static inline bool operator<(const InputDylibVMAddress& a, const InputDylibVMAddress& b)
{
    return a.rawValue() < b.rawValue();
}

static inline bool operator<=(const InputDylibVMAddress& a, const InputDylibVMAddress& b)
{
    return a.rawValue() <= b.rawValue();
}

//
// MARK: --- InputDylibVMSize methods ---
//

//
// MARK: --- VMOffset methods ---
//

// VMAddr - VMAddr -> VMOffsetOrSize
static inline VMOffsetOrInputDylibVMSize operator-(const InputDylibVMAddress& a, const InputDylibVMAddress& b)
{
    return VMOffsetOrInputDylibVMSize(a.rawValue() - b.rawValue());
}

// VMOffset + offset -> VMOffset
// TODO: Decide if we want methods like this, which take raw integers and might be easy to accidentally use
static inline VMOffset& operator+=(VMOffset& a, uint64_t b)
{
    a = VMOffset(a.rawValue() + b);
    return a;
}


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
struct CacheVMAddressHash
{
    size_t operator()(const CacheVMAddress& value) const
    {
        return std::hash<uint64_t> {}(value.rawValue());
    }
};

struct CacheVMAddressEqual
{
    bool operator()(const CacheVMAddress& a, const CacheVMAddress& b) const
    {
        return a.rawValue() == b.rawValue();
    }
};

struct CacheVMAddressLessThan
{
    bool operator()(const CacheVMAddress& a, const CacheVMAddress& b) const
    {
        return a.rawValue() < b.rawValue();
    }
};
#endif

struct VMAddressHash
{
    size_t operator()(const VMAddress& value) const
    {
        return std::hash<uint64_t> {}(value.rawValue());
    }
};

struct VMAddressEqual
{
    bool operator()(const VMAddress& a, const VMAddress& b) const
    {
        return a.rawValue() == b.rawValue();
    }
};

struct VMOffsetHash
{
    size_t operator()(const VMOffset& value) const
    {
        return std::hash<uint64_t> {}(value.rawValue());
    }
};

struct VMOffsetEqual
{
    bool operator()(const VMOffset& a, const VMOffset& b) const
    {
        return a.rawValue() == b.rawValue();
    }
};

#endif /* Types_h */
