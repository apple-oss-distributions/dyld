/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_ObjC_h
#define mach_o_ObjC_h

#include "MachODefines.h"

namespace mach_o {

/*!
 * @class ObjCClass
 *
 * @abstract
 *      Class to encapsulate accessing (and one day building) objc classes
 */
struct VIS_HIDDEN ObjCClass
{
    // Note a class looks like this to libobjc:
    // template<typename PtrTy>
    // struct class_t {
    //     PtrTy isaVMAddr;
    //     PtrTy superclassVMAddr;
    //     PtrTy methodCacheBuckets;
    //     PtrTy methodCacheProperties; // aka vtable
    //     PtrTy dataVMAddrAndFastFlags;

    //     // This field is only present if this is a Swift object, ie, has the Swift
    //     // fast bits set
    //     uint32_t swiftClassFlags;
    // };

    // Note that @objc Swift classes use alt_entry to add data before the ObjC Class
    // The actual objc class_t above will be an alt_entry in to the atom

    static constexpr uint32_t getOffsetToISA(bool is64, uint32_t swiftPreamble)
    {
        return swiftPreamble + 0;
    }
    static constexpr uint32_t getOffsetToSuperclass(bool is64, uint32_t swiftPreamble)
    {
        return swiftPreamble + (is64 ? 0x08 : 0x04);
    }
    static constexpr uint32_t getOffsetToMethodCache(bool is64, uint32_t swiftPreamble)
    {
        return swiftPreamble + (is64 ? 0x10 : 0x08);
    }
    static constexpr uint32_t getOffsetToMethodCacheProperies(bool is64, uint32_t swiftPreamble)
    {
        return swiftPreamble + (is64 ? 0x18 : 0x0C);
    }
    static constexpr uint32_t getOffsetToData(bool is64, uint32_t swiftPreamble)
    {
        return swiftPreamble + (is64 ? 0x20 : 0x10);
    }
};

/*!
 * @class ObjCClassReadOnlyData
 *
 * @abstract
 *      Class to encapsulate accessing (and one day building) objc class read-only data
 */
struct VIS_HIDDEN ObjCClassReadOnlyData
{
    // Data is a pointer to the read only data for the class, ie, one of these according to objc:
    // template<typename PtrTy>
    // struct data_t {
    //     uint32_t flags;
    //     uint32_t instanceStart;
    //     // Note there is 4-bytes of alignment padding between instanceSize and ivarLayout
    //     // on 64-bit archs, but no padding on 32-bit archs.
    //     // This union is a way to model that.
    //     union {
    //         uint32_t    instanceSize;
    //         PtrTy       pad;
    //     } instanceSize;
    //     PtrTy ivarLayoutVMAddr;
    //     PtrTy nameVMAddr;
    //     PtrTy baseMethodsVMAddr;
    //     PtrTy baseProtocolsVMAddr;
    //     PtrTy ivarsVMAddr;
    //     PtrTy weakIvarLayoutVMAddr;
    //     PtrTy basePropertiesVMAddr;
    // };

    static constexpr uint32_t getOffsetToName(bool is64)
    {
        return is64 ? 0x18 : 0x10;
    }
    static constexpr uint32_t getOffsetToBaseMethods(bool is64)
    {
        return is64 ? 0x20 : 0x14;
    }
    static constexpr uint32_t getOffsetToProtocols(bool is64)
    {
        return is64 ? 0x28 : 0x18;
    }
    static constexpr uint32_t getOffsetToProperties(bool is64)
    {
        return is64 ? 0x40 : 0x24;
    }
};

/*!
 * @class ObjCCategory
 *
 * @abstract
 *      Class to encapsulate accessing (and one day building) objc categories
 */
struct VIS_HIDDEN ObjCCategory
{
    // Note a category looks like this to libobjc:
    // template<typename PtrTy>
    // struct category_t {
    //     PtrTy nameVMAddr;
    //     PtrTy clsVMAddr;
    //     PtrTy instanceMethodsVMAddr;
    //     PtrTy classMethodsVMAddr;
    //     PtrTy protocolsVMAddr;
    //     PtrTy instancePropertiesVMAddr;
    //     // Fields below this point are not always present on disk.
    //     PtrTy classPropertiesVMAddr;
    // };

    static constexpr uint32_t getOffsetToName(bool is64)
    {
        return 0;
    }
    static constexpr uint32_t getOffsetToClass(bool is64)
    {
        return is64 ? 0x08 : 0x04;
    }
    static constexpr uint32_t getOffsetToInstanceMethods(bool is64)
    {
        return is64 ? 0x10 : 0x08;
    }
    static constexpr uint32_t getOffsetToClassMethods(bool is64)
    {
        return is64 ? 0x18 : 0x0C;
    }
    static constexpr uint32_t getOffsetToProtocols(bool is64)
    {
        return is64 ? 0x20 : 0x10;
    }
    static constexpr uint32_t getOffsetToInstanceProperties(bool is64)
    {
        return is64 ? 0x28 : 0x14;
    }
    static constexpr uint32_t getOffsetToClassProperties(bool is64)
    {
        return is64 ? 0x30 : 0x18;
    }
};

struct VIS_HIDDEN ObjCMethodList
{
    //
    // Note a method list looks like this to libobjc:
    // {
    //     uint32_t    entsize;
    //     uint32_t    count;
    //     uint8_t     methodArrayBase[]; // Note this is the start the array method_t[0]
    // }

    // This matches the bits in the objc runtime
    enum : uint32_t {
        methodListIsUniqued             = 0x1,
        methodListIsSorted              = 0x2,

        methodListUsesSelectorOffsets   = 0x40000000,
        methodListIsRelative            = 0x80000000,

        // The size is bits 2 through 16 of the entsize field
        // The low 2 bits are uniqued/sorted as above.  The upper 16-bits
        // are reserved for other flags
        methodListSizeMask              = 0x0000FFFC
    };

    // The method list stores the size of each element:
    // - pointer based method lists: 3 * uintptr_t
    // - relative method lists: 3 * uint32_t
    uint32_t getMethodSize() const;
    uint32_t getMethodCount() const;

    // Returns true if this is a relative method list.  False if its pointer based
    bool usesRelativeOffsets() const;
};

struct VIS_HIDDEN ObjCProtocolList
{
    static constexpr uint32_t headerSize(bool is64)
    {
        return is64 ? 0x08 : 0x04;
    }
    uint32_t count(bool is64) const;

    // Note a protocol list looks like this to libobjc:
    // template<typename PtrTy>
    // struct protocol_list_t {
    //     PtrTy       count;
    //     PtrTy       list[];
    // };
};

/*!
 * @class ObjCProtocol
 *
 * @abstract
 *      Class to encapsulate accessing (and one day building) objc protocols
 */
struct VIS_HIDDEN ObjCProtocol
{
    // Note a category looks like this to libobjc:
    // template<typename PtrTy>
    // struct protocol_t {
    //     PtrTy       isaVMAddr;
    //     PtrTy       nameVMAddr;
    //     PtrTy       protocolsVMAddr;
    //     PtrTy       instanceMethodsVMAddr;
    //     PtrTy       classMethodsVMAddr;
    //     PtrTy       optionalInstanceMethodsVMAddr;
    //     PtrTy       optionalClassMethodsVMAddr;
    //     PtrTy       instancePropertiesVMAddr;
    //     uint32_t    size;
    //     uint32_t    flags;
    //     // Fields below this point are not always present on disk.
    //     PtrTy       extendedMethodTypesVMAddr;
    //     PtrTy       demangledNameVMAddr;
    //     PtrTy       classPropertiesVMAddr;
    // };

    static constexpr uint32_t getOffsetToName(bool is64)
    {
        return is64 ? 0x8 : 0x4;
    }
    static constexpr uint32_t getOffsetToInstanceMethods(bool is64)
    {
        return is64 ? 0x18 : 0xC;
    }
    static constexpr uint32_t getOffsetToClassMethods(bool is64)
    {
        return is64 ? 0x20 : 0x10;
    }
    static constexpr uint32_t getOffsetToOptionalInstanceMethods(bool is64)
    {
        return is64 ? 0x28 : 0x14;
    }
    static constexpr uint32_t getOffsetToOptionalClassMethods(bool is64)
    {
        return is64 ? 0x30 : 0x18;
    }
};

struct VIS_HIDDEN ObjCPropertyList
{
    //
    // Note a property list looks like this to libobjc:
    // {
    //     uint32_t    entsize;
    //     uint32_t    count;
    //     uint8_t     propertyArrayBase[]; // Note this is the start the array property_t[0]
    // }

    // The list stores the size of each element:
    uint32_t getPropertySize() const;
    uint32_t getPropertyCount() const;
};

/*!
 * @class ObjCImageInfo
 *
 * @abstract
 *      Class to encapsulate accessing (and one day building) objc image info
 */
struct VIS_HIDDEN ObjCImageInfo
{
    // struct objc_image_info  {
    //     uint32_t    version;
    //     uint32_t    flags;
    // };

    // FIXME: Get all the other values and perhaps make these an enum
    static constexpr uint64_t OBJC_IMAGE_SIGNED_CLASS_RO               = (1<<4);
    static constexpr uint64_t OBJC_IMAGE_HAS_CATEGORY_CLASS_PROPERTIES = (1<<6);
};

} // namespace mach_o

#endif /* mach_o_ObjC_h */
