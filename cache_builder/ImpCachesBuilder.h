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

#ifndef ImpCachesBuilder_h
#define ImpCachesBuilder_h

#include "Diagnostics.h"
#include "JSONReader.h"
#include <memory>
#include <string_view>
#include <vector>

namespace IMPCaches {
class IMPCachesBuilder;
}


namespace imp_caches
{

struct Dylib;

struct Method
{
    Method(std::string_view name);
    Method() = delete;
    ~Method() = default;
    Method(const Method&) = delete;
    Method& operator=(const Method&) = delete;
    Method(Method&&) = default;
    Method& operator=(Method&&) = default;

    std::string_view name;
};

struct Class
{
    Class(std::string_view name, bool isMetaClass, bool isRootClass);
    Class() = delete;
    ~Class() = default;
    Class(const Class&) = delete;
    Class& operator=(const Class&) = delete;
    Class(Class&&) = default;
    Class& operator=(Class&&) = default;

    std::string_view    name;
    std::vector<Method> methods;
    bool                isMetaClass     = false;
    bool                isRootClass     = false;
    const Class*        metaClass       = nullptr;
    const Class*        superClass      = nullptr;
    const Dylib*        superClassDylib = nullptr;
};

struct Category
{
    Category(std::string_view name);
    Category() = delete;
    ~Category() = default;
    Category(const Category&) = delete;
    Category& operator=(const Category&) = delete;
    Category(Category&&) = default;
    Category& operator=(Category&&) = default;

    std::string_view    name;
    std::vector<Method> instanceMethods;
    std::vector<Method> classMethods;
    const Class*        cls         = nullptr;
    const Dylib*        classDylib  = nullptr;
};

struct Dylib
{
    Dylib(std::string_view installName);
    Dylib() = delete;
    ~Dylib() = default;
    Dylib(const Dylib&) = delete;
    Dylib& operator=(const Dylib&) = delete;
    Dylib(Dylib&&) = default;
    Dylib& operator=(Dylib&&) = default;

    std::string_view        installName;
    std::vector<Class>      classes;
    std::vector<Category>   categories;
};

struct FallbackClass
{
    std::string_view installName;
    std::string_view className;
    bool isMetaClass                = false;

    bool operator==(const FallbackClass& other) const {
        return (isMetaClass == other.isMetaClass)
            && (installName == other.installName)
            && (className == other.className);
    }

    size_t hash() const {
        std::size_t seed = 0;
        seed ^= std::hash<std::string_view>()(installName) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        seed ^= std::hash<std::string_view>()(className) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        seed ^= std::hash<bool>()(isMetaClass) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        return seed;
    }
};

struct FallbackClassHash
{
    size_t operator()(const FallbackClass& value) const
    {
        return value.hash();
    }
};

struct BucketMethod
{
    std::string_view installName;
    std::string_view className;
    std::string_view methodName;
    bool isInstanceMethod;

    bool operator==(const BucketMethod& other) const {
        return isInstanceMethod == other.isInstanceMethod &&
                installName == other.installName &&
                className == other.className &&
                methodName == other.methodName;
    }

    size_t hash() const {
        std::size_t seed = 0;
        seed ^= std::hash<std::string_view>()(installName) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        seed ^= std::hash<std::string_view>()(className) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        seed ^= std::hash<std::string_view>()(methodName) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        seed ^= std::hash<bool>()(isInstanceMethod) + 0x9e3779b9 + (seed<<6) + (seed>>2);
        return seed;
    }
};

struct BucketMethodHash {
    size_t operator()(const BucketMethod& k) const {
        return k.hash();
    }
};

struct Bucket
{
    bool                    isEmptyBucket       = true;
    bool                    isInstanceMethod    = true;
    uint32_t                selOffset;
    std::string_view        installName;
    std::string_view        className;
    std::string_view        methodName;
};

struct IMPCache
{
    // If set, points to the class to fall back to if a lookup on the IMP cache fails.  Otherwise
    // is set to the superclass of this class
    std::optional<FallbackClass> fallback_class;
    uint32_t cache_shift :  5;
    uint32_t cache_mask  : 11;
    uint32_t occupied    : 14;
    uint32_t has_inlines :  1;
    uint32_t padding     :  1;
    uint32_t unused      :  31;
    uint32_t bit_one     :  1;

    std::vector<Bucket> buckets;
};

struct Builder
{
    static const bool verbose = false;

    Builder(const std::vector<Dylib>& dylibs, const dyld3::json::Node& objcOptimizations);
    ~Builder();
    Builder() = delete;
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Builder(Builder&&) = delete;
    Builder& operator=(Builder&&) = delete;

    void buildImpCaches();

    void forEachSelector(void (^handler)(std::string_view str, uint32_t bufferOffset)) const;
    std::optional<imp_caches::IMPCache> getIMPCache(uint32_t dylibIndex, std::string_view className, bool isMetaClass);

    Diagnostics                     diags;
    TimeRecorder                    time;
    const std::vector<Dylib>&       dylibs;
    const dyld3::json::Node&        objcOptimizations;

    // Note, we own this pointer, but we can't use a unique pointer without including
    // the header and we want to keep things more separated
    IMPCaches::IMPCachesBuilder*    impCachesBuilder = nullptr;
};

} // namespace imp_caches


#endif /* ImpCachesBuilder_h */
