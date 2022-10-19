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

#ifndef ASLRTracker_hpp
#define ASLRTracker_hpp

#include "Types.h"

#include <assert.h>
#include <unordered_map>
#include <vector>

namespace cache_builder
{

class ASLR_Tracker
{
public:
    ASLR_Tracker() = default;
    ~ASLR_Tracker() = default; 

    ASLR_Tracker(const ASLR_Tracker&) = delete;
    ASLR_Tracker& operator=(const ASLR_Tracker& other) = delete;
    ASLR_Tracker(ASLR_Tracker&&) = default;
    ASLR_Tracker& operator=(ASLR_Tracker&& other) = default;

    void                setDataRegion(const void* rwRegionStart, size_t rwRegionSize);
    void                add(void* loc, uint8_t level = (uint8_t)~0);
    void                setRebaseTarget32(void*p, uint32_t targetVMAddr);
    void                setRebaseTarget64(void*p, uint64_t targetVMAddr);
    void                remove(void* p);
    bool                has(void* loc, uint8_t* level = nullptr) const;

    bool                hasRebaseTarget32(void* p, uint32_t* vmAddr) const;
    bool                hasRebaseTarget64(void* p, uint64_t* vmAddr) const;

    void                forEachFixup(void (^callback)(void* loc, bool& stop));

#if BUILDING_APP_CACHE_UTIL
    // Get all the out of band rebase targets.  Used for the kernel collection builder
    // to emit the classic relocations
    std::vector<void*>  getRebaseTargets() const;

    bool                hasHigh8(void* p, uint8_t* highByte) const;
    bool                hasAuthData(void* p, uint16_t* diversity, bool* hasAddrDiv, uint8_t* key) const;
    void                setHigh8(void* p, uint8_t high8);
    void                setAuthData(void* p, uint16_t diversity, bool hasAddrDiv, uint8_t key);
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    void                disable() { _enabled = false; };
    void                clearRebaseTargetsMaps();
#endif

private:

    enum {
#if BUILDING_APP_CACHE_UTIL
        // The x86_64 kernel collection needs 1-byte aligned fixups
        kMinimumFixupAlignment = 1
#else
        // Shared cache fixups must be at least 4-byte aligned
        kMinimumFixupAlignment = 4
#endif
    };

    uint8_t*            _regionStart    = nullptr;
    uint8_t*            _regionEnd      = nullptr;
    std::vector<bool>   _bitmap;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // Only the cache builder needs to disable ASLR, not the kernel linker
    bool                _enabled        = true;
#endif

#if BUILDING_APP_CACHE_UTIL
    struct AuthData {
        uint16_t    diversity;
        bool        addrDiv;
        uint8_t     key;
    };

    std::unordered_map<void*, uint8_t>  _high8Map;
    std::unordered_map<void*, AuthData> _authDataMap;

    // For kernel collections to work out which other collection a given
    // fixup is relative to
    std::vector<uint8_t> _cacheLevels;
#endif

    std::unordered_map<void*, uint32_t> _rebaseTarget32;
    std::unordered_map<void*, uint64_t> _rebaseTarget64;
};

// Shared cache pointer values are packed so that we don't have to store too much data in
// maps on the ASLRTracker.  We don't pack in chain "next" bits, as we don't have enough bits to do so,
// but we can pack in all the other information
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
struct Fixup
{
    class Cache32
    {
    public:
        static CacheVMAddress getCacheVMAddressFromLocation(CacheVMAddress cacheBaseAddress,
                                                            const void* fixupLocation)
        {
            const Cache32* value = (Cache32*)fixupLocation;
            return cacheBaseAddress + VMOffset((uint64_t)value->cacheVMOffset);
        }

        static void setLocation(CacheVMAddress cacheBaseAddress, void* fixupLocation,
                                CacheVMAddress targetAddress)
        {
            // First zero the location (not needed on 32-bit but do for consistency with 64-bit code)
            *(uint32_t*)fixupLocation = 0;

            // Then set the value
            Cache32* value = (Cache32*)fixupLocation;
            VMOffset offset = targetAddress - cacheBaseAddress;
            value->cacheVMOffset = (uint32_t)offset.rawValue();
            assert(value->cacheVMOffset == offset.rawValue());
        }

        static void updateLocationToCacheVMAddress(CacheVMAddress cacheBaseAddress, void* fixupLocation,
                                                   CacheVMAddress targetAddress)
        {
            Cache32* value = (Cache32*)fixupLocation;
            VMOffset offset = targetAddress - cacheBaseAddress;
            value->cacheVMOffset = (uint32_t)offset.rawValue();
            assert(value->cacheVMOffset == offset.rawValue());
        }

        // This is a bit of a hack.  We don't know for sure that this value is null if its all zeroes
        // as technically its a 0 offset from the cache base address.  But there's no good reason
        // for anyone to point to the cache header.  Anyone using this is probably doing something
        // like parsing objc, which has no legitimate reason to be pointing to the cache header
        static bool isNull(const void* fixupLocation)
        {
            const Cache32* value = (Cache32*)fixupLocation;
            return value->cacheVMOffset == 0;
        }

    private:
        // This could really be a cacheVMAddress, but for consistency with 64-bit, lets use an offset
        uint32_t cacheVMOffset;
    };
    static_assert(sizeof(Cache32) == sizeof(uint32_t));

    class Cache64
    {
    public:
        static CacheVMAddress getCacheVMAddressFromLocation(CacheVMAddress cacheBaseAddress,
                                                            const void* fixupLocation)
        {
            const Cache64* value = (Cache64*)fixupLocation;
            if ( value->regular.isAuthenticated )
                return cacheBaseAddress + VMOffset(value->auth.cacheVMOffset);
            else
                return cacheBaseAddress + VMOffset(value->regular.cacheVMOffset);
        }

        static void updateLocationToCacheVMAddress(CacheVMAddress cacheBaseAddress, void* fixupLocation,
                                                   CacheVMAddress targetAddress)
        {
            Cache64* value = (Cache64*)fixupLocation;
            if ( value->regular.isAuthenticated ) {
                VMOffset offset = targetAddress - cacheBaseAddress;
                value->auth.cacheVMOffset = offset.rawValue();
                assert(value->auth.cacheVMOffset == offset.rawValue());
            } else {
                VMOffset offset = targetAddress - cacheBaseAddress;
                value->regular.cacheVMOffset = offset.rawValue();
                assert(value->regular.cacheVMOffset == offset.rawValue());
            }
        }

        static void setLocation(CacheVMAddress cacheBaseAddress, void* fixupLocation,
                                CacheVMAddress targetAddress, uint8_t high8,
                                uint16_t authDiversity, bool hasAddrDiv, uint8_t authKey,
                                bool isAuth)
        {
            VMOffset offset = targetAddress - cacheBaseAddress;

            // First zero the location
            *(uint64_t*)fixupLocation = 0;

            // Then set the value
            Cache64* value = (Cache64*)fixupLocation;
            if ( isAuth ) {
                value->auth.cacheVMOffset = offset.rawValue();
                value->auth.authDiversity           = authDiversity;
                value->auth.authKey                 = authKey;
                value->auth.hasAddrDiv              = hasAddrDiv ? 1 : 0;
                value->auth.isAuthenticated         = 1;

                assert(value->auth.cacheVMOffset == offset.rawValue());
            } else {
                value->regular.cacheVMOffset = offset.rawValue();
                value->regular.high8 = high8;
                value->regular.unused = 0;
                value->regular.isAuthenticated = 0;

                assert(value->regular.cacheVMOffset == offset.rawValue());
            }
        }

        static uint8_t getHigh8(const void* fixupLocation)
        {
            const Cache64* value = (Cache64*)fixupLocation;
            if ( value->auth.isAuthenticated )
                return 0;

            return value->regular.high8;
        }

        static bool hasAuthData(void* fixupLocation, uint16_t& diversity, bool& hasAddrDiv, uint8_t& key)
        {
            const Cache64* value = (Cache64*)fixupLocation;
            if ( !value->auth.isAuthenticated )
                return false;

            diversity = value->auth.authDiversity;
            hasAddrDiv = value->auth.hasAddrDiv;
            key = value->auth.authKey;
            return true;
        }

        // This is a bit of a hack.  We don't know for sure that this value is null if its all zeroes
        // as technically its a 0 offset from the cache base address.  But there's no good reason
        // for anyone to point to the cache header.  Anyone using this is probably doing something
        // like parsing objc, which has no legitimate reason to be pointing to the cache header
        static bool isNull(const void* fixupLocation)
        {
            const Cache64* value = (Cache64*)fixupLocation;
            if ( value->regular.isAuthenticated ) {
                return value->auth.cacheVMOffset == 0;
            } else {
                return value->regular.cacheVMOffset == 0;
            }
        }

    private:
        struct Auth {
            uint64_t cacheVMOffset      : 44;
            uint64_t authDiversity      : 16;
            uint64_t authKey            : 2;
            uint64_t hasAddrDiv         : 1;
            uint64_t isAuthenticated    : 1; // == 1
        };
        struct Regular {
            uint64_t cacheVMOffset      : 44;
            uint64_t high8              : 8;
            uint64_t unused             : 11;
            uint64_t isAuthenticated    : 1; // == 0
        };

        union {
            Auth    auth;
            Regular regular;
        };
    };
    static_assert(sizeof(Cache64) == sizeof(uint64_t));
};
#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

} // namespace cache_builder

#endif /* ASLRTracker_hpp */
