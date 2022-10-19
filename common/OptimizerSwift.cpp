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

//                                 Swift Optimizations
//
// The shared cache Swift optimizations are designed to speed up protocol conformance
// lookups.
//
// Protocol conformances are stored as an array on each dylib.  To find out if a type conforms
// to a protocol, Swift must walk these arrays in all loaded dylibs.  This is then cached in
// the Swift runtime.
//
// This optimization builds a number of hash tables to speed up these lookups, and allows the
// Swift runtime to avoid caching the results from these tables.  This saves both time and memory.
//
// We start by finding all protocol conformances by walking the "__TEXT, __swift5_proto" section.
// There are several kinds of conformance:
//   1) (type*, protocol*)
//   2) (objc_class*, protocol*)
//   3) (class name*, protocol*)
//   4) (foreign metadata name*, protocol*)
//
// 1) Type Pointers
//
// These are made up of a pointer to a type, and a pointer to a protocol.
// We turn these in to shared cache offsets for the type, protocol, conformance,
// and the index of the dylib containing the conformance.  See SwiftTypeProtocolConformanceLocation.
// At runtime, we look in the table at typeConformanceHashTableCacheOffset, to see if a given type and
// protocol are in the table, and if the conformance is from a loaded image.
// Note it is possible for this table to contain duplicates.  In this case, we return the first found
// conformance, in the order we found them in the shared cache.
//
// 2) ObjC Class Pointers
//
// These are similar to type pointers, but are classed as metadata in the Swift runtime.
// Again, similarly to the above, we convert the metadata, protocol, and conformance pointers to
// shared cache offsets.  See SwiftForeignTypeProtocolConformanceLocationKey.
// At runtime, we may be passed a non-null metadata pointer.  In that case, we search the table
// reached via metadataConformanceHashTableCacheOffset, for matching a ObjC Class and Protocol,
// and check that the conformance dylib is loaded.  Again duplicates are supported.
//
// 3) ObjC Class Names
//
// In this case, we have the "const char*" name of the ObjC class to lookup.  The Swift runtime does
// this by asking the ObjC runtime for the Class with this name.  In the shared cache, we use the ObjC
// class hash table to find the Class pointers for all classes with the given name.  As we won't know
// which one is loaded, we record them all, so duplicates are likely to happen here.
// The Class pointers we find from the ObjC hash table are converted to shared cache offsets, and stored
// in the same hash table as 2) above.  All other details in 2) apply.
//
// 4) Foreign Metadata Names
//
// These names are found via the Type Pointers in 1).  We visiting a TypeDescriptor, we may
// find it has an attached Foreign Name.  This is used when the Swift runtime wants to unique a Type by
// name, not by pointer.
// In this case, names and their protocols are converted to cache offsets and stored in the hash table
// found via foreignTypeConformanceHashTableCacheOffset.
// At runtime, the Swift runtime will pass a name and protocol to look up in this table.
//
// Foreign metadata names may additionally have "ImportInfo", which describes an alternative name to use.
// This alternative name is the key we store in the map.  It can be found by the getForeignFullIdentity() method.
// The Swift runtime also knows if metadata has one of these "Full Identities", and will always pass in the
// Full Identity when calling the SPI.  At runtime, dyld does not know that a given entry in the map is
// a regular Foreign metadata name, or the Full Identity.
//
// One final quirk of Full Identity names, is that they can contain null characters.  Eg, NNSFoo\0St.
// Given this, all of the code to handle foreign metadata names, including lookups in the hash table, and
// the SPI below, take name and name length.  We never assume that the name is a null-terminated C string.
//
// SPIs
//
// The above types are stored in 3 tables: Type, Metadata, Foreign Metadata.
// These are accessed by 2 different SPIs.
//
// _dyld_find_protocol_conformance()
//
// This searches for types and metadata.  It takes Type* and Metadata* arguments
// and looks up the corresponding table, depending on which of Type* or Metadata*
// is non-null.
//
// _dyld_find_foreign_type_protocol_conformance()
//
// This looks up the given name in the Foreign Metadata table.  Matches are done
// by string comparison.  As noted above in 4), the name may contain null characters
// so all hashing, etc, is done with std::string_view which allows null characters.


#include "DyldSharedCache.h"
#include "Diagnostics.h"
#include "OptimizerObjC.h"
#include "OptimizerSwift.h"
#include "PerfectHash.h"
#include "SwiftVisitor.h"
#include "Vector.h"

#if SUPPORT_VM_LAYOUT
#include "MachOLoaded.h"
#include "MachOAnalyzer.h"
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "CacheDylib.h"
#include "Optimizers.h"
#include "NewSharedCacheBuilder.h"
#include "objc-shared-cache.h"
#endif

using metadata_visitor::ResolvedValue;
using metadata_visitor::SwiftConformance;
using metadata_visitor::SwiftVisitor;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
using cache_builder::BuilderConfig;
using cache_builder::CacheDylib;
using cache_builder::SwiftProtocolConformanceOptimizer;
#endif

// Tracks which types conform to which protocols

namespace std {
    template<>
    struct hash<SwiftTypeProtocolConformanceLocationKey>
    {
        size_t operator()(const SwiftTypeProtocolConformanceLocationKey& v) const {
            return std::hash<uint64_t>{}(v.typeDescriptorCacheOffset) ^ std::hash<uint64_t>{}(v.protocolCacheOffset);
        }
    };

    template<>
    struct equal_to<SwiftTypeProtocolConformanceLocationKey>
    {
        bool operator()(const SwiftTypeProtocolConformanceLocationKey& a,
                        const SwiftTypeProtocolConformanceLocationKey& b) const {
            return a.typeDescriptorCacheOffset == b.typeDescriptorCacheOffset && a.protocolCacheOffset == b.protocolCacheOffset;
        }
    };
}

// Tracks which Metadata conform to which protocols

namespace std {
    template<>
    struct hash<SwiftMetadataProtocolConformanceLocationKey>
    {
        size_t operator()(const SwiftMetadataProtocolConformanceLocationKey& v) const {
            return std::hash<uint64_t>{}(v.metadataCacheOffset) ^ std::hash<uint64_t>{}(v.protocolCacheOffset);
        }
    };

    template<>
    struct equal_to<SwiftMetadataProtocolConformanceLocationKey>
    {
        bool operator()(const SwiftMetadataProtocolConformanceLocationKey& a,
                        const SwiftMetadataProtocolConformanceLocationKey& b) const {
            return a.metadataCacheOffset == b.metadataCacheOffset && a.protocolCacheOffset == b.protocolCacheOffset;
        }
    };
}

// Tracks which foreign types conform to which protocols

namespace std {
    template<>
    struct hash<SwiftForeignTypeProtocolConformanceLocationKey>
    {
        size_t operator()(const SwiftForeignTypeProtocolConformanceLocationKey& v) const {
            return std::hash<uint64_t>{}(v.rawForeignDescriptor) ^ std::hash<uint64_t>{}(v.protocolCacheOffset);
        }
    };

    template<>
    struct equal_to<SwiftForeignTypeProtocolConformanceLocationKey>
    {
        bool operator()(const SwiftForeignTypeProtocolConformanceLocationKey& a,
                        const SwiftForeignTypeProtocolConformanceLocationKey& b) const {
            return a.rawForeignDescriptor == b.rawForeignDescriptor && a.protocolCacheOffset == b.protocolCacheOffset;
        }
    };
}

// Type Hash Table methods
template<>
uint32_t SwiftHashTable::hash(const SwiftTypeProtocolConformanceLocationKey& key,
                              const uint8_t*) const {
    uint64_t val1 = objc::lookup8(key.key1Buffer(nullptr), key.key1Size(), salt);
    uint64_t val2 = objc::lookup8((uint8_t*)&key.protocolCacheOffset, sizeof(key.protocolCacheOffset), salt);
    uint64_t val = val1 ^ val2;
    uint32_t index = (uint32_t)((shift == 64) ? 0 : (val>>shift)) ^ scramble[tab[val&mask]];
    return index;
}


template<>
bool SwiftHashTable::equal(const SwiftTypeProtocolConformanceLocationKey& key,
                           const SwiftTypeProtocolConformanceLocationKey& value,
                           const uint8_t*) const {
    return memcmp(&key, &value, sizeof(SwiftTypeProtocolConformanceLocationKey)) == 0;
}

template<>
SwiftHashTable::CheckByteType SwiftHashTable::checkbyte(const SwiftTypeProtocolConformanceLocationKey& key, const uint8_t*) const
{
    const uint8_t* keyBytes = (const uint8_t*)&key;
    return ((keyBytes[0] & 0x7) << 5) | ((uint8_t)sizeof(SwiftTypeProtocolConformanceLocationKey) & 0x1f);
}

// Metadata Hash Table methods
template<>
uint32_t SwiftHashTable::hash(const SwiftMetadataProtocolConformanceLocationKey& key,
                              const uint8_t*) const {
    uint64_t val1 = objc::lookup8(key.key1Buffer(nullptr), key.key1Size(), salt);
    uint64_t val2 = objc::lookup8((uint8_t*)&key.protocolCacheOffset, sizeof(key.protocolCacheOffset), salt);
    uint64_t val = val1 ^ val2;
    uint32_t index = (uint32_t)((shift == 64) ? 0 : (val>>shift)) ^ scramble[tab[val&mask]];
    return index;
}


template<>
bool SwiftHashTable::equal(const SwiftMetadataProtocolConformanceLocationKey& key,
                           const SwiftMetadataProtocolConformanceLocationKey& value,
                           const uint8_t*) const {
    return memcmp(&key, &value, sizeof(SwiftMetadataProtocolConformanceLocationKey)) == 0;
}

template<>
SwiftHashTable::CheckByteType SwiftHashTable::checkbyte(const SwiftMetadataProtocolConformanceLocationKey& key, const uint8_t*) const
{
    const uint8_t* keyBytes = (const uint8_t*)&key;
    return ((keyBytes[0] & 0x7) << 5) | ((uint8_t)sizeof(SwiftTypeProtocolConformanceLocationKey) & 0x1f);
}

// Foreign Type Hash Table methods
template<>
uint32_t SwiftHashTable::hash(const SwiftForeignTypeProtocolConformanceLocationKey& key,
                              const uint8_t* stringBaseAddress) const {
    // Combine the hashes of the foreign type string and the protocol cache offset.
    // Then combine them to get the hash for this value
    const char* name = (const char*)stringBaseAddress + key.foreignDescriptorNameCacheOffset;
    uint64_t val1 = objc::lookup8((uint8_t*)name, key.foreignDescriptorNameLength, salt);
    uint64_t val2 = objc::lookup8((uint8_t*)&key.protocolCacheOffset, sizeof(key.protocolCacheOffset), salt);
    uint64_t val = val1 ^ val2;
    uint32_t index = (uint32_t)((shift == 64) ? 0 : (val>>shift)) ^ scramble[tab[val&mask]];
    return index;
}


template<>
bool SwiftHashTable::equal(const SwiftForeignTypeProtocolConformanceLocationKey& key,
                           const SwiftForeignTypeProtocolConformanceLocationKey& value,
                           const uint8_t*) const {
    return memcmp(&key, &value, sizeof(SwiftForeignTypeProtocolConformanceLocationKey)) == 0;
}

template<>
SwiftHashTable::CheckByteType SwiftHashTable::checkbyte(const SwiftForeignTypeProtocolConformanceLocationKey& key, const uint8_t* stringBaseAddress) const
{
    const char* name = (const char*)stringBaseAddress + key.foreignDescriptorNameCacheOffset;
    const uint8_t* keyBytes = (const uint8_t*)name;
    return ((keyBytes[0] & 0x7) << 5) | ((uint8_t)key.foreignDescriptorNameLength & 0x1f);
}

// Foreign Type Hash Table methods, using a string as a key
template<>
uint32_t SwiftHashTable::hash(const SwiftForeignTypeProtocolConformanceLookupKey& key,
                              const uint8_t* stringBaseAddress) const {
    // Combine the hashes of the foreign type string and the protocol cache offset.
    // Then combine them to get the hash for this value
    const std::string_view& name = key.foreignDescriptorName;
    uint64_t val1 = objc::lookup8((uint8_t*)name.data(), name.size(), salt);
    uint64_t val2 = objc::lookup8((uint8_t*)&key.protocolCacheOffset, sizeof(key.protocolCacheOffset), salt);
    uint64_t val = val1 ^ val2;
    uint32_t index = (uint32_t)((shift == 64) ? 0 : (val>>shift)) ^ scramble[tab[val&mask]];
    return index;
}


template<>
bool SwiftHashTable::equal(const SwiftForeignTypeProtocolConformanceLocationKey& key,
                           const SwiftForeignTypeProtocolConformanceLookupKey& value,
                           const uint8_t* stringBaseAddress) const {
    std::string_view keyName((const char*)key.key1Buffer(stringBaseAddress), key.key1Size());
    return (key.protocolCacheOffset == value.protocolCacheOffset) && (keyName == value.foreignDescriptorName);
}

template<>
SwiftHashTable::CheckByteType SwiftHashTable::checkbyte(const SwiftForeignTypeProtocolConformanceLookupKey& key,
                                                        const uint8_t* stringBaseAddress) const
{
    const std::string_view& name = key.foreignDescriptorName;
    const uint8_t* keyBytes = (const uint8_t*)name.data();
    return ((keyBytes[0] & 0x7) << 5) | ((uint8_t)name.size() & 0x1f);
}

// Foreign metadata names might not be a regular C string.  Instead they might be
// a NULL-separated array of C strings.  The "full identity" is the result including any
// intermidiate NULL characters.  Eg, "NNSFoo\0St" would be a legitimate result
std::string_view getForeignFullIdentity(const char* arrayStart)
{
    // Track the extent of the current component.
    const char* componentStart = arrayStart;
    const char* componentEnd = componentStart + strlen(arrayStart);

    // Set initial range to the extent of the user-facing name.
    const char* identityBeginning = componentStart;
    const char* identityEnd = componentEnd;

    // Start examining the following array components, starting past the NUL
    // terminator of the user-facing name:
    while (true) {
        // Advance past the NUL terminator.
        componentStart = componentEnd + 1;
        componentEnd = componentStart + strlen(componentStart);

        // If the component is empty, then we're done.
        if (componentStart == componentEnd)
            break;

        // Switch on the component type at the beginning of the component.
        switch (componentStart[0]) {
            case 'N':
                // ABI name, set identity beginning and end.
                identityBeginning = componentStart + 1;
                identityEnd = componentEnd;
                break;
            case 'S':
            case 'R':
                // Symbol namespace or related entity name, set identity end.
                identityEnd = componentEnd;
                break;
            default:
                // Ignore anything else.
                break;
        }
    }

    size_t stringSize = identityEnd - identityBeginning;
    return std::string_view(identityBeginning, stringSize);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

template<typename PerfectHashT, typename KeyT, typename TargetT>
void SwiftHashTable::write(const PerfectHashT& phash, const lsl::Vector<KeyT>& keyValues,
                           const lsl::Vector<TargetT>& targetValues,
                           const uint8_t* targetValuesBufferBaseAddress)
{
    // Set header
    capacity = phash.capacity;
    occupied = phash.occupied;
    shift = phash.shift;
    mask = phash.mask;
    sentinelTarget = sentinel;
    roundedTabSize = std::max(phash.mask+1, 4U);
    salt = phash.salt;

    // Set hash data
    for (uint32_t i = 0; i < 256; i++) {
        scramble[i] = phash.scramble[i];
    }
    for (uint32_t i = 0; i < phash.mask+1; i++) {
        tab[i] = phash.tab[i];
    }

    dyld3::Array<TargetOffsetType> targetsArray = targets();
    dyld3::Array<CheckByteType> checkBytesArray = checkBytes();

    // Set offsets to the sentinel
    for (uint32_t i = 0; i < phash.capacity; i++) {
        targetsArray[i] = sentinel;
    }
    // Set checkbytes to 0
    for (uint32_t i = 0; i < phash.capacity; i++) {
        checkBytesArray[i] = 0;
    }

    // Set real value offsets and checkbytes
    uint32_t offsetOfTargetBaseFromMap = (uint32_t)((uint64_t)targetValuesBufferBaseAddress - (uint64_t)this);
    bool skipNext = false;
    uint32_t keyIndex = 0;

    // Walk all targets.  Keys will exist only for the first target in a sequence with the key
    for ( const TargetT& targetValue : targetValues ) {
        // Skip chains of duplicates
        bool skipThisEntry = skipNext;
        skipNext = targetValue.nextIsDuplicate;
        if ( skipThisEntry )
            continue;

        // Process this key as it wasn't skipped
        const KeyT& key = keyValues[keyIndex];
        ++keyIndex;

        uint32_t h = hash(key, nullptr);
        uint32_t offsetOfTargetValueInArray = (uint32_t)((uint64_t)&targetValue - (uint64_t)targetValues.data());
        assert(targetsArray[h] == sentinel);
        targetsArray[h] = offsetOfTargetBaseFromMap + offsetOfTargetValueInArray;
        assert(checkBytesArray[h] == 0);
        checkBytesArray[h] = checkbyte(key, nullptr);
    }

    assert(keyIndex == keyValues.size());
}

static bool operator<(const SwiftTypeProtocolConformanceLocation& a,
                      const SwiftTypeProtocolConformanceLocation& b) {
    if ( a.typeDescriptorCacheOffset != b.typeDescriptorCacheOffset )
        return a.typeDescriptorCacheOffset < b.typeDescriptorCacheOffset;
    if ( a.protocolCacheOffset != b.protocolCacheOffset )
        return a.protocolCacheOffset < b.protocolCacheOffset;
    if ( a.raw != b.raw )
        return a.raw < b.raw;
    return false;
}

static bool operator<(const SwiftMetadataProtocolConformanceLocation& a,
                      const SwiftMetadataProtocolConformanceLocation& b) {
    if ( a.metadataCacheOffset != b.metadataCacheOffset )
        return a.metadataCacheOffset < b.metadataCacheOffset;
    if ( a.protocolCacheOffset != b.protocolCacheOffset )
        return a.protocolCacheOffset < b.protocolCacheOffset;
    if ( a.raw != b.raw )
        return a.raw < b.raw;
    return false;
}

static bool operator<(const SwiftForeignTypeProtocolConformanceLocation& a,
                      const SwiftForeignTypeProtocolConformanceLocation& b) {
    if ( a.foreignDescriptorNameCacheOffset != b.foreignDescriptorNameCacheOffset )
        return a.foreignDescriptorNameCacheOffset < b.foreignDescriptorNameCacheOffset;
    if ( a.foreignDescriptorNameLength != b.foreignDescriptorNameLength )
        return a.foreignDescriptorNameLength < b.foreignDescriptorNameLength;
    if ( a.protocolCacheOffset != b.protocolCacheOffset )
        return a.protocolCacheOffset < b.protocolCacheOffset;
    if ( a.raw != b.raw )
        return a.raw < b.raw;
    return false;
}

// Find the protocol conformances in the given dylib and add them to the vector
static void findProtocolConformances(Diagnostics& diags,
                                     VMAddress sharedCacheBaseAddress,
                                     const objc::ClassHashTable* objcClassOpt,
                                     const void* headerInfoRO, const void* headerInfoRW,
                                     VMAddress headerInfoROUnslidVMAddr,
                                     const SwiftVisitor& swiftVisitor,
                                     CacheVMAddress dylibCacheAddress,
                                     std::string_view installName,
                                     std::unordered_map<std::string_view, uint64_t>& canonicalForeignNameOffsets,
                                     std::unordered_map<uint64_t, std::string_view>& foundForeignNames,
                                     lsl::Vector<SwiftTypeProtocolConformanceLocation>& foundTypeProtocolConformances,
                                     lsl::Vector<SwiftMetadataProtocolConformanceLocation>& foundMetadataProtocolConformances,
                                     lsl::Vector<SwiftForeignTypeProtocolConformanceLocation>& foundForeignTypeProtocolConformances)
{
    const bool is64 = (swiftVisitor.pointerSize == 8);

    swiftVisitor.forEachProtocolConformance(^(const SwiftConformance &swiftConformance, bool &stopConformance) {
        typedef SwiftConformance::SwiftProtocolConformanceFlags SwiftProtocolConformanceFlags;
        typedef SwiftConformance::SwiftTypeRefPointer SwiftTypeRefPointer;
        typedef SwiftConformance::TypeContextDescriptor TypeContextDescriptor;

        std::optional<uint16_t> objcIndex;
        objcIndex = objc::getPreoptimizedHeaderRWIndex(headerInfoRO, headerInfoRW,
                                                       headerInfoROUnslidVMAddr.rawValue(),
                                                       dylibCacheAddress.rawValue(),
                                                       is64);
        if ( !objcIndex.has_value() ) {
            diags.error("Could not find objc header info for Swift dylib: %s", installName.data());
            stopConformance = true;
            return;
        }

        uint16_t dylibObjCIndex = *objcIndex;

        // Get the protocol, and skip missing weak imports
        std::optional<VMAddress> protocolVMAddr = swiftConformance.getProtocolVMAddr(swiftVisitor);
        if ( !protocolVMAddr.has_value() )
            return;
        VMOffset protocolVMOffset = protocolVMAddr.value() - sharedCacheBaseAddress;

        VMAddress conformanceVMAddr = swiftConformance.getVMAddress();
        VMOffset conformanceVMOffset = conformanceVMAddr - sharedCacheBaseAddress;

        SwiftTypeRefPointer typeRef = swiftConformance.getTypeRef(swiftVisitor);
        SwiftProtocolConformanceFlags flags = swiftConformance.getProtocolConformanceFlags(swiftVisitor);
        switch ( flags.typeReferenceKind() ) {
            case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor:
            case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor: {
                std::optional<ResolvedValue> typeDescValue = typeRef.getTypeDescriptor(swiftVisitor);
                if ( typeDescValue.has_value() ) {
                    VMAddress typeDescVMAddr = typeDescValue->vmAddress();
                    VMOffset typeDescVMOffset = typeDescVMAddr - sharedCacheBaseAddress;

                    // Type descriptors might be foreign.  This means that the runtime needs to use their name to identify them
                    TypeContextDescriptor typeDesc(typeDescValue.value());
                    if ( typeDesc.isForeignMetadata() ) {
                        ResolvedValue typeDescNameValue = typeDesc.getName(swiftVisitor);
                        const char* typeDescName = (const char*)typeDescNameValue.value();
                        std::string_view fullName(typeDescName);
                        if ( typeDesc.hasImportInfo() )
                            fullName = getForeignFullIdentity(typeDescName);

                        // We only have 16-bits for the length.  Hopefully that is enough!
                        if ( fullName.size() >= (1 << 16) ) {
                            diags.error("Protocol conformance exceeded name length of 16-bits");
                            stopConformance = true;
                            return;
                        }

                        // The full mame may have moved adjusted the offset we want to record
                        VMOffset fullNameVMOffset((uint64_t)fullName.data() - (uint64_t)typeDescName);

                        VMAddress nameVMAddr = typeDescNameValue.vmAddress() + fullNameVMOffset;
                        VMOffset nameVMOffset = nameVMAddr - sharedCacheBaseAddress;

                        auto itAndInserted = canonicalForeignNameOffsets.insert({ fullName, nameVMOffset.rawValue() });
                        if ( itAndInserted.second ) {
                            // We inserted the name, so record it
                            foundForeignNames[nameVMOffset.rawValue()] = fullName;
                        } else {
                            // We didn't insert the name, so use the offset already there for this name
                            nameVMOffset = VMOffset(itAndInserted.first->second);
                        }

                        SwiftForeignTypeProtocolConformanceLocation protoLoc;
                        protoLoc.protocolConformanceCacheOffset = conformanceVMOffset.rawValue();
                        protoLoc.dylibObjCIndex = dylibObjCIndex;
                        protoLoc.foreignDescriptorNameCacheOffset = nameVMOffset.rawValue();
                        protoLoc.foreignDescriptorNameLength = fullName.size();
                        protoLoc.protocolCacheOffset = protocolVMOffset.rawValue();
                        foundForeignTypeProtocolConformances.push_back(protoLoc);
                    }

                    SwiftTypeProtocolConformanceLocation protoLoc;
                    protoLoc.protocolConformanceCacheOffset = conformanceVMOffset.rawValue();
                    protoLoc.dylibObjCIndex = dylibObjCIndex;
                    protoLoc.typeDescriptorCacheOffset = typeDescVMOffset.rawValue();
                    protoLoc.protocolCacheOffset = protocolVMOffset.rawValue();
                    foundTypeProtocolConformances.push_back(protoLoc);
                }
                break;
            }
            case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::directObjCClassName: {
                const char* className = typeRef.getClassName(swiftVisitor);

                objcClassOpt->forEachClass(className, ^(uint64_t classCacheOffset, uint16_t dylibObjCIndexForClass,
                                                        bool &stopClasses) {
                    // exactly one matching class
                    SwiftMetadataProtocolConformanceLocation protoLoc;
                    protoLoc.protocolConformanceCacheOffset = conformanceVMOffset.rawValue();
                    protoLoc.dylibObjCIndex = dylibObjCIndex;
                    protoLoc.metadataCacheOffset = classCacheOffset;
                    protoLoc.protocolCacheOffset = protocolVMOffset.rawValue();
                    foundMetadataProtocolConformances.push_back(protoLoc);
                });
                break;
            }
            case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::indirectObjCClass: {
                std::optional<ResolvedValue> classPos = typeRef.getClass(swiftVisitor);
                if ( classPos.has_value() ) {
                    VMAddress classVMAddr = classPos->vmAddress();
                    VMOffset classVMOffset = classVMAddr - sharedCacheBaseAddress;

                    SwiftMetadataProtocolConformanceLocation protoLoc;
                    protoLoc.protocolConformanceCacheOffset = conformanceVMOffset.rawValue();
                    protoLoc.dylibObjCIndex = dylibObjCIndex;
                    protoLoc.metadataCacheOffset = classVMOffset.rawValue();
                    protoLoc.protocolCacheOffset = protocolVMOffset.rawValue();
                    foundMetadataProtocolConformances.push_back(protoLoc);
                }
                break;
            }
        }
    });
}

static void make_perfect(const lsl::Vector<SwiftTypeProtocolConformanceLocationKey>& targets,
                         objc::PerfectHash& phash)
{
    dyld3::OverflowSafeArray<objc::PerfectHash::key> keys;

    /* read in the list of keywords */
    keys.reserve(targets.size());
    for (const SwiftTypeProtocolConformanceLocationKey& target : targets) {
        objc::PerfectHash::key mykey;
        mykey.name1_k = (uint8_t*)target.key1Buffer(nullptr);
        mykey.len1_k  = (uint32_t)target.key1Size();
        mykey.name2_k = (uint8_t*)target.key2Buffer(nullptr);
        mykey.len2_k  = (uint32_t)target.key2Size();
        keys.push_back(mykey);
    }

    objc::PerfectHash::make_perfect(keys, phash);
}

static void emitTypeHashTable(Diagnostics& diag, lsl::Allocator& allocator,
                              lsl::Vector<SwiftTypeProtocolConformanceLocation>& conformances,
                              cache_builder::SwiftProtocolConformancesHashTableChunk* hashTableChunk)
{
    // Prepare the protocols by sorting them and looking for duplicates
    std::sort(conformances.begin(), conformances.end());
    for (uint64_t i = 1; i < conformances.size(); ++i) {
        // Check if this protocol is the same as the previous one
        auto& prev = conformances[i - 1];
        auto& current = conformances[i];
        if ( std::equal_to<SwiftTypeProtocolConformanceLocationKey>()(prev, current) )
            prev.nextIsDuplicate = 1;
    }

    lsl::Vector<SwiftTypeProtocolConformanceLocationKey> conformanceKeys(allocator);
    for (const auto& protoLoc : conformances) {
        if ( protoLoc.nextIsDuplicate )
            continue;
        conformanceKeys.push_back(protoLoc);
    }

    // Build the perfect hash table for type conformances
    objc::PerfectHash perfectHash;
    make_perfect(conformanceKeys, perfectHash);
    size_t hashTableSize = SwiftHashTable::size(perfectHash);

    size_t conformanceBufferSize = (conformances.size() * sizeof(*conformances.data()));

    size_t totalBufferSize = hashTableSize + conformanceBufferSize;
    if ( totalBufferSize > hashTableChunk->subCacheFileSize.rawValue() ) {
        diag.error("Swift type hash table exceeds buffer size (%lld > %lld)",
                   (uint64_t)totalBufferSize, hashTableChunk->subCacheFileSize.rawValue());
        return;
    }

    // Emit the table
    uint8_t* hashTableBuffer = hashTableChunk->subCacheBuffer;
    uint8_t* valuesBuffer = hashTableBuffer + hashTableSize;

    ((SwiftHashTable*)hashTableBuffer)->write(perfectHash, conformanceKeys,
                                              conformances, valuesBuffer);
    memcpy(valuesBuffer, conformances.data(), conformanceBufferSize);
}

static void make_perfect(const lsl::Vector<SwiftMetadataProtocolConformanceLocationKey>& targets,
                         objc::PerfectHash& phash)
{
    dyld3::OverflowSafeArray<objc::PerfectHash::key> keys;

    /* read in the list of keywords */
    keys.reserve(targets.size());
    for (const SwiftMetadataProtocolConformanceLocationKey& target : targets) {
        objc::PerfectHash::key mykey;
        mykey.name1_k = (uint8_t*)target.key1Buffer(nullptr);
        mykey.len1_k  = (uint32_t)target.key1Size();
        mykey.name2_k = (uint8_t*)target.key2Buffer(nullptr);
        mykey.len2_k  = (uint32_t)target.key2Size();
        keys.push_back(mykey);
    }

    objc::PerfectHash::make_perfect(keys, phash);
}

static void emitMetadataHashTable(Diagnostics& diag, lsl::Allocator& allocator,
                                  lsl::Vector<SwiftMetadataProtocolConformanceLocation>& conformances,
                                  cache_builder::SwiftProtocolConformancesHashTableChunk* hashTableChunk)
{
    // Prepare the protocols by sorting them and looking for duplicates
    std::sort(conformances.begin(), conformances.end());
    for (uint64_t i = 1; i < conformances.size(); ++i) {
        // Check if this protocol is the same as the previous one
        auto& prev = conformances[i - 1];
        auto& current = conformances[i];
        if ( std::equal_to<SwiftMetadataProtocolConformanceLocationKey>()(prev, current) )
            prev.nextIsDuplicate = 1;
    }

    lsl::Vector<SwiftMetadataProtocolConformanceLocationKey> conformanceKeys(allocator);
    for (const auto& protoLoc : conformances) {
        if ( protoLoc.nextIsDuplicate )
            continue;
        conformanceKeys.push_back(protoLoc);
    }

    // Build the perfect hash table for metadata
    objc::PerfectHash perfectHash;
    make_perfect(conformanceKeys, perfectHash);
    size_t hashTableSize = SwiftHashTable::size(perfectHash);

    size_t conformanceBufferSize = (conformances.size() * sizeof(*conformances.data()));

    size_t totalBufferSize = hashTableSize + conformanceBufferSize;
    if ( totalBufferSize > hashTableChunk->subCacheFileSize.rawValue() ) {
        diag.error("Swift metadata hash table exceeds buffer size (%lld > %lld)",
                   (uint64_t)totalBufferSize, hashTableChunk->subCacheFileSize.rawValue());
        return;
    }

    // Emit the table
    uint8_t* hashTableBuffer = hashTableChunk->subCacheBuffer;
    uint8_t* valuesBuffer = hashTableBuffer + hashTableSize;

    ((SwiftHashTable*)hashTableBuffer)->write(perfectHash, conformanceKeys,
                                              conformances, valuesBuffer);
    memcpy(valuesBuffer, conformances.data(), conformanceBufferSize);
}

static void make_perfect(const lsl::Vector<SwiftForeignTypeProtocolConformanceLookupKey>& targets,
                         const std::unordered_map<uint64_t, std::string_view>& foundForeignNames,
                         objc::PerfectHash& phash)
{
    dyld3::OverflowSafeArray<objc::PerfectHash::key> keys;

    /* read in the list of keywords */
    keys.reserve(targets.size());
    for (const SwiftForeignTypeProtocolConformanceLookupKey& target : targets) {
        objc::PerfectHash::key mykey;
        mykey.name1_k = (uint8_t*)target.foreignDescriptorName.data();
        mykey.len1_k  = (uint32_t)target.foreignDescriptorName.size();
        mykey.name2_k = (uint8_t*)&target.protocolCacheOffset;
        mykey.len2_k  = (uint32_t)sizeof(target.protocolCacheOffset);
        keys.push_back(mykey);
    }

    objc::PerfectHash::make_perfect(keys, phash);
}

static void emitForeignTypeHashTable(Diagnostics& diag, lsl::Allocator& allocator,
                                     lsl::Vector<SwiftForeignTypeProtocolConformanceLocation>& conformances,
                                     const std::unordered_map<uint64_t, std::string_view>& foundForeignNames,
                                     cache_builder::SwiftProtocolConformancesHashTableChunk* hashTableChunk)
{
    // Prepare the protocols by sorting them and looking for duplicates
    std::sort(conformances.begin(), conformances.end());
    for (uint64_t i = 1; i < conformances.size(); ++i) {
        // Check if this protocol is the same as the previous one
        auto& prev = conformances[i - 1];
        auto& current = conformances[i];
        if ( std::equal_to<SwiftForeignTypeProtocolConformanceLocationKey>()(prev, current) )
            prev.nextIsDuplicate = 1;
    }

    // Note, we use SwiftForeignTypeProtocolConformanceLookupKey as we don't have the cache
    // buffer available for name offsets in to the cache
    lsl::Vector<SwiftForeignTypeProtocolConformanceLookupKey> conformanceKeys(allocator);
    for (const auto& protoLoc : conformances) {
        if ( protoLoc.nextIsDuplicate )
            continue;

        // HACK: As we are in the cache builder, we don't have an easy way to resolve cache offsets
        // Given that, we can't just take the cache address and add the name offset to get the string
        // Instead, we'll look it up in the map
        uint64_t nameOffset = protoLoc.foreignDescriptorNameCacheOffset;
        auto it = foundForeignNames.find(nameOffset);
        assert(it != foundForeignNames.end());

        SwiftForeignTypeProtocolConformanceLookupKey lookupKey;
        lookupKey.foreignDescriptorName = it->second;
        lookupKey.protocolCacheOffset = protoLoc.protocolCacheOffset;
        conformanceKeys.push_back(lookupKey);
    }

    // Build the perfect hash table for foreign types
    objc::PerfectHash perfectHash;
    make_perfect(conformanceKeys, foundForeignNames, perfectHash);
    size_t hashTableSize = SwiftHashTable::size(perfectHash);

    size_t conformanceBufferSize = (conformances.size() * sizeof(*conformances.data()));

    size_t totalBufferSize = hashTableSize + conformanceBufferSize;
    if ( totalBufferSize > hashTableChunk->subCacheFileSize.rawValue() ) {
        diag.error("Swift foreign type hash table exceeds buffer size (%lld > %lld)",
                   (uint64_t)totalBufferSize, hashTableChunk->subCacheFileSize.rawValue());
        return;
    }

    // Emit the table
    uint8_t* hashTableBuffer = hashTableChunk->subCacheBuffer;
    uint8_t* valuesBuffer = hashTableBuffer + hashTableSize;

    ((SwiftHashTable*)hashTableBuffer)->write(perfectHash, conformanceKeys,
                                              conformances, valuesBuffer);
    memcpy(valuesBuffer, conformances.data(), conformanceBufferSize);
}

static void emitHeader(const BuilderConfig& config, SwiftProtocolConformanceOptimizer& opt)
{
    CacheVMAddress cacheBaseAddress = config.layout.cacheBaseAddress;
    VMOffset typeOffset = opt.typeConformancesHashTable->cacheVMAddress - cacheBaseAddress;
    VMOffset metadataOffset = opt.metadataConformancesHashTable->cacheVMAddress - cacheBaseAddress;
    VMOffset foreignOffset = opt.foreignTypeConformancesHashTable->cacheVMAddress - cacheBaseAddress;

    auto* swiftOptimizationHeader = (SwiftOptimizationHeader*)opt.optsHeaderChunk->subCacheBuffer;
    swiftOptimizationHeader->version = 1;
    swiftOptimizationHeader->padding = 0;
    swiftOptimizationHeader->typeConformanceHashTableCacheOffset = typeOffset.rawValue();
    swiftOptimizationHeader->metadataConformanceHashTableCacheOffset = metadataOffset.rawValue();
    swiftOptimizationHeader->foreignTypeConformanceHashTableCacheOffset = foreignOffset.rawValue();
}

static void checkHashTables()
{
#if 0
    // Check that the hash tables work!
    for (const auto& target : foundTypeProtocolConformances) {
        const SwiftHashTable* hashTable = (const SwiftHashTable*)typeConformanceHashTableBuffer;
        const auto* protocolTarget = hashTable->getValue<SwiftTypeProtocolConformanceLocation>(target, nullptr);
        assert(protocolTarget != nullptr);
        if ( !protocolTarget->nextIsDuplicate ) {
            // No duplicates, so we should match
            assert(memcmp(protocolTarget, &target, sizeof(SwiftTypeProtocolConformanceLocation)) == 0);
        } else {
            // One of the duplicates should match
            bool foundMatch = false;
            while ( true ) {
                if ( memcmp(protocolTarget, &target, sizeof(SwiftTypeProtocolConformanceLocation)) == 0 ) {
                    foundMatch = true;
                    break;
                }
                if ( !protocolTarget->nextIsDuplicate )
                    break;
                protocolTarget = ++protocolTarget;
            }
            assert(foundMatch);
        }
    }
    for (const auto& target : foundMetadataProtocolConformances) {
        const SwiftHashTable* hashTable = (const SwiftHashTable*)metadataConformanceHashTableBuffer;
        const auto* protocolTarget = hashTable->getValue<SwiftMetadataProtocolConformanceLocation>(target, nullptr);
        assert(protocolTarget != nullptr);
        if ( !protocolTarget->nextIsDuplicate ) {
            // No duplicates, so we should match
            assert(memcmp(protocolTarget, &target, sizeof(SwiftMetadataProtocolConformanceLocation)) == 0);
        } else {
            // One of the duplicates should match
            bool foundMatch = false;
            while ( true ) {
                if ( memcmp(protocolTarget, &target, sizeof(SwiftMetadataProtocolConformanceLocation)) == 0 ) {
                    foundMatch = true;
                    break;
                }
                if ( !protocolTarget->nextIsDuplicate )
                    break;
                protocolTarget = ++protocolTarget;
            }
            assert(foundMatch);
        }
    }
    for (const auto& target : foundForeignTypeProtocolConformances) {
        const SwiftHashTable* hashTable = (const SwiftHashTable*)foreignTypeConformanceHashTableBuffer;
        const auto* protocolTarget = hashTable->getValue<SwiftForeignTypeProtocolConformanceLocation>(target, (const uint8_t*)dyldCache);
        assert(protocolTarget != nullptr);
        if ( !protocolTarget->nextIsDuplicate ) {
            // No duplicates, so we should match
            assert(memcmp(protocolTarget, &target, sizeof(SwiftForeignTypeProtocolConformanceLocation)) == 0);
        } else {
            // One of the duplicates should match
            bool foundMatch = false;
            while ( true ) {
                if ( memcmp(protocolTarget, &target, sizeof(SwiftForeignTypeProtocolConformanceLocation)) == 0 ) {
                    foundMatch = true;
                    break;
                }
                if ( !protocolTarget->nextIsDuplicate )
                    break;
                protocolTarget = ++protocolTarget;
            }
            assert(foundMatch);
        }
    }
    // Check the foreign table again, with a string key, as that is what the SPI will use
    for (const auto& target : foundForeignTypeProtocolConformances) {
        const SwiftHashTable* hashTable = (const SwiftHashTable*)foreignTypeConformanceHashTableBuffer;

        const char* typeName = (const char*)dyldCache + target.foreignDescriptorNameCacheOffset;
        assert((const uint8_t*)typeName == target.key1Buffer((const uint8_t*)dyldCache));
        // The type name might include null characters, if it has additional import info
        std::string_view fullName(typeName, target.key1Size());
        SwiftForeignTypeProtocolConformanceLookupKey lookupKey = { fullName, target.protocolCacheOffset };

        const auto* protocolTarget = hashTable->getValue<SwiftForeignTypeProtocolConformanceLookupKey, SwiftForeignTypeProtocolConformanceLocation>(lookupKey, (const uint8_t*)dyldCache);
        assert(protocolTarget != nullptr);
        if ( !protocolTarget->nextIsDuplicate ) {
            // No duplicates, so we should match
            assert(memcmp(protocolTarget, &target, sizeof(SwiftForeignTypeProtocolConformanceLocation)) == 0);
        } else {
            // One of the duplicates should match
            bool foundMatch = false;
            while ( true ) {
                if ( memcmp(protocolTarget, &target, sizeof(SwiftForeignTypeProtocolConformanceLocation)) == 0 ) {
                    foundMatch = true;
                    break;
                }
                if ( !protocolTarget->nextIsDuplicate )
                    break;
                protocolTarget = ++protocolTarget;
            }
            assert(foundMatch);
        }
    }
#endif
}

void buildSwiftHashTables(const BuilderConfig& config,
                          Diagnostics& diag, const std::span<CacheDylib> cacheDylibs,
                          std::span<metadata_visitor::Segment> extraRegions,
                          const objc::ClassHashTable* objcClassOpt,
                          const void* headerInfoRO, const void* headerInfoRW,
                          CacheVMAddress headerInfoROUnslidVMAddr,
                          SwiftProtocolConformanceOptimizer& swiftProtocolConformanceOptimizer)
{
    lsl::EphemeralAllocator allocator;
    lsl::Vector<SwiftTypeProtocolConformanceLocation> foundTypeProtocolConformances(allocator);
    lsl::Vector<SwiftMetadataProtocolConformanceLocation> foundMetadataProtocolConformances(allocator);
    lsl::Vector<SwiftForeignTypeProtocolConformanceLocation> foundForeignTypeProtocolConformances(allocator);

    std::unordered_map<std::string_view, uint64_t> canonicalForeignNameOffsets;
    std::unordered_map<uint64_t, std::string_view> foundForeignNames;
    for ( const CacheDylib& cacheDylib : cacheDylibs ) {
        SwiftVisitor swiftVisitor = cacheDylib.makeCacheSwiftVisitor(config, extraRegions);
        findProtocolConformances(diag, VMAddress(config.layout.cacheBaseAddress.rawValue()),
                                 objcClassOpt,
                                 headerInfoRO, headerInfoRW,
                                 VMAddress(headerInfoROUnslidVMAddr.rawValue()),
                                 swiftVisitor,
                                 cacheDylib.cacheLoadAddress, cacheDylib.installName,
                                 canonicalForeignNameOffsets,
                                 foundForeignNames,
                                 foundTypeProtocolConformances,
                                 foundMetadataProtocolConformances,
                                 foundForeignTypeProtocolConformances);
        if ( diag.hasError() )
            return;
    }

    // We have all the conformances.  Now build the hash tables
    emitTypeHashTable(diag, allocator,
                      foundTypeProtocolConformances,
                      swiftProtocolConformanceOptimizer.typeConformancesHashTable);
    if ( diag.hasError() )
        return;
    emitMetadataHashTable(diag, allocator,
                          foundMetadataProtocolConformances,
                          swiftProtocolConformanceOptimizer.metadataConformancesHashTable);
    if ( diag.hasError() )
        return;
    emitForeignTypeHashTable(diag, allocator,
                             foundForeignTypeProtocolConformances,
                             foundForeignNames,
                             swiftProtocolConformanceOptimizer.foreignTypeConformancesHashTable);
    if ( diag.hasError() )
        return;

    // Make sure the hash tables work
    checkHashTables();

    // Emit the header to point to everything else
    emitHeader(config, swiftProtocolConformanceOptimizer);
}

#endif // BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
