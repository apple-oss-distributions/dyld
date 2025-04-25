/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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

#ifndef PreBuiltObjC_h
#define PreBuiltObjC_h

#if SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

#include "Array.h"
#include "Map.h"
#include "DyldSharedCache.h"
#include "PrebuiltLoader.h"
#include "Types.h"
#include <optional>

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
#include <unordered_set>
#endif

struct SelectorFixup;

namespace objc {
class SelectorHashTable;
class ClassHashTable;
class ProtocolHashTable;
}

// This defines all the methods clients need to use the maps.
// These aren't on the structs, as this allows use to keep the map
// implementation opaque
namespace prebuilt_objc {

// We want to take the maps built by the PrebuiltObjC pass, and serialize them in to the closure
// By using the same hash function for both the intermediate map during PrebuiltObjC, as well as at runtime,
// we can avoid rehashing all the keys when we serialize.  We do the same in the Swift hash tables,
// by always using the murmur hash.
uint64_t hashStringKey(const std::string_view& str);

// This is the key to the map from string to value
struct ObjCStringKeyOnDisk
{
    dyld4::PrebuiltLoader::BindTargetRef stringTarget;
};

// Points to a class/protocol on-disk
struct ObjCObjectOnDiskLocation
{
    dyld4::PrebuiltLoader::BindTargetRef objectLocation;
};

struct EqualObjCStringKeyOnDisk {
    // We also support looking these up as strings
    static bool equal(const std::string_view& a, const std::string_view& b, void* state) {
        return a == b;
    }

#if SUPPORT_VM_LAYOUT
    // We also support looking these up as strings
    static bool equal(const ObjCStringKeyOnDisk& a, const std::string_view& b, void* state) {
        char* strA = nullptr;

        dyld4::RuntimeState* runtimeState = (dyld4::RuntimeState*)state;
        strA = (char*)a.stringTarget.value(*runtimeState);

        return equal(std::string_view(strA), b, state);
    }

    static bool equal(const ObjCStringKeyOnDisk& a, const ObjCStringKeyOnDisk& b, void* state) {
        char* strA = nullptr;
        char* strB = nullptr;

        dyld4::RuntimeState* runtimeState = (dyld4::RuntimeState*)state;
        strA = (char*)a.stringTarget.value(*runtimeState);
        strB = (char*)b.stringTarget.value(*runtimeState);

        return equal(std::string_view(strA), std::string_view(strB), state);
    }
#endif // SUPPORT_VM_LAYOUT
};

struct HashObjCStringKeyOnDisk {
    // We also support looking these up as strings
    // This is also the ONLY hash implementation we use for all of the
    static uint64_t hash(const std::string_view& str, void* state) {
        return hashStringKey(str);
    }

#if SUPPORT_VM_LAYOUT
    static uint64_t hash(const ObjCStringKeyOnDisk& v, void* state) {
        dyld4::RuntimeState* runtimeState = (dyld4::RuntimeState*)state;
        const char* str = (char*)v.stringTarget.value(*runtimeState);
        return hash(std::string_view(str), state);
    }
#endif // SUPPORT_VM_LAYOUT
};

// This is the key to the map from string to value
struct ObjCStringKey
{
    std::string_view    string;
};

// Points to a selector in a specific dylib
struct ObjCSelectorLocation
{
    dyld4::PrebuiltLoader::BindTarget nameLocation;
};

// Points to the name and impl for a class/protocol
struct ObjCObjectLocation
{
    dyld4::PrebuiltLoader::BindTarget nameLocation;
    dyld4::PrebuiltLoader::BindTarget objectLocation;
};

typedef ObjCObjectLocation ObjCClassLocation;
typedef ObjCObjectLocation ObjCProtocolLocation;

struct EqualObjCStringKey {
    static bool equal(const ObjCStringKey& a, const ObjCStringKey& b, void* state) {
        return EqualObjCStringKeyOnDisk::equal(a.string, b.string, state);
    }
};

struct HashObjCStringKey {
    static uint64_t hash(const ObjCStringKey& v, void* state) {
        return hashStringKey(v.string);
    }
};

// These are the maps we use to build the PrebuiltObjC
typedef dyld3::Map<ObjCStringKey, ObjCSelectorLocation, HashObjCStringKey, EqualObjCStringKey> ObjCSelectorMap;
typedef dyld3::MultiMap<ObjCStringKey, ObjCObjectLocation, HashObjCStringKey, EqualObjCStringKey> ObjCObjectMap;
using ObjCClassMap = ObjCObjectMap;
using ObjCProtocolMap = ObjCObjectMap;

// And these are the maps we serialized in to the closure
typedef dyld3::MapView<ObjCStringKeyOnDisk, void, HashObjCStringKeyOnDisk, EqualObjCStringKeyOnDisk> ObjCSelectorMapOnDisk;
typedef dyld3::MultiMapView<ObjCStringKeyOnDisk, ObjCObjectOnDiskLocation, HashObjCStringKeyOnDisk, EqualObjCStringKeyOnDisk> ObjCObjectMapOnDisk;
using ObjCClassMapOnDisk = ObjCObjectMapOnDisk;
using ObjCProtocolMapOnDisk = ObjCObjectMapOnDisk;

// Methods for accessing the maps

void forEachSelectorStringEntry(const void* selMap,
                                void (^handler)(const dyld4::PrebuiltLoader::BindTargetRef& target));

#if SUPPORT_VM_LAYOUT
// These are the methods used by dyld at runtime to find selectors, classes, protocols
const char* findSelector(dyld4::RuntimeState* state, const ObjCSelectorMapOnDisk& map,
                         const char* selName);
void forEachClass(dyld4::RuntimeState* state, const ObjCClassMapOnDisk& classMap, const char* className,
                  void (^handler)(const dyld3::Array<const dyld4::PrebuiltLoader::BindTargetRef*>& values));
void forEachProtocol(dyld4::RuntimeState* state, const ObjCProtocolMapOnDisk& protocolMap, const char* protocolName,
                     void (^handler)(const dyld3::Array<const dyld4::PrebuiltLoader::BindTargetRef*>& values));
#endif

void forEachClass(const void* classMap,
                  void (^handler)(const dyld4::PrebuiltLoader::BindTargetRef& nameTarget,
                                  const dyld3::Array<const dyld4::PrebuiltLoader::BindTargetRef*>& values));
void forEachProtocol(const void* protocolMap,
                     void (^handler)(const dyld4::PrebuiltLoader::BindTargetRef& nameTarget,
                                     const dyld3::Array<const dyld4::PrebuiltLoader::BindTargetRef*>& values));

} // namespace prebuilt_objc

namespace dyld4 {

class BumpAllocator;

struct HashUInt16 {
    static size_t hash(const uint16_t& v, void* state) {
        return std::hash<uint16_t>{}(v);
    }
};

struct EqualUInt16 {
    static bool equal(uint16_t s1, uint16_t s2, void* state) {
        return s1 == s2;
    }
};

//
// PrebuiltObjC computes read-only optimized data structures to store in the PrebuiltLoaderSet
//
struct PrebuiltObjC
{
    typedef std::pair<VMAddress, const Loader*>                                   SharedCacheLoadedImage;
    typedef dyld3::Map<uint16_t, SharedCacheLoadedImage, HashUInt16, EqualUInt16> SharedCacheImagesMapTy;
    typedef dyld3::CStringMapTo<Loader::BindTarget>                               DuplicateClassesMapTy;

    // Mao from const char* to name
    typedef prebuilt_objc::ObjCSelectorMap                                        SelectorMapTy;

    // Maps from const char* to { name, impl }
    typedef prebuilt_objc::ObjCObjectMap                                          ObjectMapTy;
    typedef prebuilt_objc::ObjCClassMap                                           ClassMapTy;
    typedef prebuilt_objc::ObjCProtocolMap                                        ProtocolMapTy;

    enum ObjCStructKind { classes, protocols };

    struct ObjCOptimizerImage {

        ObjCOptimizerImage(const JustInTimeLoader* jitLoader, uint64_t loadAddress, uint32_t pointerSize);

        ~ObjCOptimizerImage() = default;
        ObjCOptimizerImage(ObjCOptimizerImage&&) = default;
        ObjCOptimizerImage& operator=(ObjCOptimizerImage&&) = default;

        ObjCOptimizerImage(const ObjCOptimizerImage&) = delete;
        ObjCOptimizerImage& operator=(const ObjCOptimizerImage&) = delete;

        void visitReferenceToObjCSelector(const objc::SelectorHashTable* objcSelOpt,
                                          SelectorMapTy& appSelectorMap,
                                          VMOffset selectorStringVMAddr, VMOffset selectorReferenceVMAddr,
                                          const char* selectorString);

        void visitReferenceToObjCProtocol(const objc::SelectorHashTable* objcSelOpt,
                                          ProtocolMapTy& appSelectorMap,
                                          VMOffset selectorStringVMAddr, VMOffset selectorReferenceVMAddr,
                                          const char* selectorString);

        void visitClass(const VMAddress dyldCacheBaseAddress,
                        const objc::ClassHashTable* objcClassOpt,
                        SharedCacheImagesMapTy& sharedCacheImagesMap,
                        DuplicateClassesMapTy& duplicateSharedCacheClasses,
                        InputDylibVMAddress classVMAddr, InputDylibVMAddress classNameVMAddr, const char* className);

        void visitProtocol(const objc::ProtocolHashTable* objcProtocolOpt,
                           SharedCacheImagesMapTy& sharedCacheImagesMap,
                           InputDylibVMAddress protocolVMAddr, InputDylibVMAddress protocolNameVMAddr, const char* protocolName);

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
        void calculateMissingWeakImports(RuntimeState& state);
#endif

        // Returns true if the given vm address is a pointer to null
        bool isNull(InputDylibVMAddress vmAddr, const void* address) const;

        // On object here is either a class or protocol, which both look the same to our optimisation
        struct ObjCObject {
            const char* name;
            VMOffset    nameRuntimeOffset;
            VMOffset    valueRuntimeOffset;
        };

        struct VMOffsetHash
        {
            static size_t hash(const VMOffset& value, void* state) {
                return std::hash<uint64_t>{}(value.rawValue());
            }
            size_t operator()(const VMOffset& value) const
            {
                return hash(value, nullptr);
            }
        };

        struct VMOffsetEqual
        {
            static bool equal(const VMOffset& a, const VMOffset& b, void* state) {
                return a.rawValue() == b.rawValue();
            }
            bool operator()(const VMOffset& a, const VMOffset& b) const
            {
                return equal(a, b, nullptr);
            }
        };

        const JustInTimeLoader*         jitLoader               = nullptr;
        uint32_t                        pointerSize             = 0;
        InputDylibVMAddress             loadAddress;
        Diagnostics                     diag;

        // Class and protocol optimisation data structures
        dyld3::OverflowSafeArray<ObjCObject>                                    classLocations;
        dyld3::OverflowSafeArray<ObjCObject>                                    protocolLocations;
        dyld3::OverflowSafeArray<bool>                                          protocolISAFixups;
        DuplicateClassesMapTy                                                   duplicateSharedCacheClassMap;
        dyld3::Map<VMOffset, uint32_t, VMOffsetHash, VMOffsetEqual>             protocolIndexMap;

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL
        struct VMAddressHash
        {
            size_t operator()(const InputDylibVMAddress& value) const
            {
                return std::hash<uint64_t> {}(value.rawValue());
            }
        };

        struct VMAddressEqual
        {
            bool operator()(const InputDylibVMAddress& a, const InputDylibVMAddress& b) const
            {
                return a.rawValue() == b.rawValue();
            }
        };

        std::unordered_set<InputDylibVMAddress, VMAddressHash, VMAddressEqual>  missingWeakImports;
#endif

        // Selector optimization data structures
        dyld3::OverflowSafeArray<PrebuiltLoader::BindTargetRef> selectorFixups;
        SelectorMapTy                                           selectorMap;

        // Protocol optimization data structures
        dyld3::OverflowSafeArray<PrebuiltLoader::BindTargetRef> protocolFixups;

        ObjCBinaryInfo                                          binaryInfo;
    };

    PrebuiltObjC() = default;
    ~PrebuiltObjC() = default;

    void make(Diagnostics& diag, RuntimeState& state);

    // Adds the results from this image to the tables for the whole app
    void commitImage(const ObjCOptimizerImage& image);

    // Generates the hash tables for classes and protocols.  Selectors are done already
    // We need to do this so that Swift can use the classMap later
    void generateHashTables();

    // Serialize the hash tables in to the given allocator
    uint32_t serializeSelectorMap(dyld4::BumpAllocator& alloc) const;
    uint32_t serializeClassMap(dyld4::BumpAllocator& alloc) const;
    uint32_t serializeProtocolMap(dyld4::BumpAllocator& alloc) const;

    // Generates the fixups for each individual image
    void generatePerImageFixups(RuntimeState& state, uint32_t pointerSize);

    // Serializes the per-image objc fixups for the given loader.
    // Returns 0 if no per-image fixups exist.  Otherwise returns their offset
    uint32_t serializeFixups(const Loader& jitLoader, BumpAllocator& allocator);

    static void forEachSelectorReferenceToUnique(RuntimeState& state,
                                                 const Loader* ldr,
                                                 uint64_t loadAddress,
                                                 const ObjCBinaryInfo& binaryInfo,
                                                 void (^callback)(uint64_t selectorReferenceRuntimeOffset,
                                                                  uint64_t selectorStringRuntimeOffset,
                                                                  const char* selectorString));

    // Intermediate data which doesn't get saved to the PrebuiltLoader(Set)
    dyld3::OverflowSafeArray<ObjCOptimizerImage>    objcImages;
    SelectorMapTy                                   selectorMap;
    ClassMapTy                                      classMap        = { nullptr };
    ProtocolMapTy                                   protocolMap     = { nullptr };
    DuplicateClassesMapTy                           duplicateSharedCacheClassMap;
    bool                                            hasClassDuplicates  = false;
    bool                                            builtObjC           = false;

    // These data structures all get saved to the PrebuiltLoaderSet
    VMOffset objcProtocolClassCacheOffset;

    // Per-image info, which is saved to the PrebuiltLoader's
    struct ObjCImageFixups {
        ObjCBinaryInfo                                          binaryInfo;
        dyld3::OverflowSafeArray<uint8_t>                       protocolISAFixups;
        dyld3::OverflowSafeArray<PrebuiltLoader::BindTargetRef> selectorReferenceFixups;
        dyld3::OverflowSafeArray<PrebuiltLoader::BindTargetRef> protocolReferenceFixups;
    };

    // Indexed by the app Loader index
    dyld3::OverflowSafeArray<ObjCImageFixups> imageFixups;
};

} // namespace dyld4

#endif // SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS

#endif // PreBuiltObjC_h


