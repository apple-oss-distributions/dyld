/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

// TODO: Do some nice ASCII artwork for the schema, for now the validation code will serve as documentation

//protocol ValidatorKeysProtocol: OptionSet {
//    static func validate(keyString: borrowing BPList.FastString, value: BPList.Object) throws(AtlasError) -> Self
//}

let debugValidation: Bool = false

protocol AtlasSchemaValidator {
    // validate the schema of a bplist object corresponding to a give class, recusrively
    // All validation failures throw, so we can (eventually) encode richer failure information
    associatedtype ValidatorKeys: OptionSet
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys
    static var requiredKeys: ValidatorKeys { get }
    static func checkComplexKeyRelationships(keys: ValidatorKeys) throws(AtlasError)
}

extension AtlasSchemaValidator {
    static func validate(bplist: BPList.Object) throws(AtlasError) {
        var validatedKeys = ValidatorKeys()
        do throws(BPListError) {
            try bplist.asDictionary().forEach { (key, value) in
                guard let newKey = try? validate(keyString:key, value:value) else { return }
                //FIXME: Validate types on optional keys
                validatedKeys.insert(newKey as! Self.ValidatorKeys.Element)
            }
        } catch {
            throw .bplistError(error)
        }
        guard validatedKeys.isSuperset(of:Self.requiredKeys) else {
            throw .missingRequiredPlistField
        }
        try checkComplexKeyRelationships(keys:validatedKeys)
    }
    static func checkComplexKeyRelationships(keys: ValidatorKeys) throws(AtlasError) {}
    static func validateString(key: ValidatorKeys, value: BPList.Object) throws(AtlasError) -> ValidatorKeys {
        do throws(BPListError) {
            _ = try value.asFastString()
        } catch {
            throw .bplistError(error)
        }
        return key
    }
    static func validateInt64(key: ValidatorKeys, value: BPList.Object) throws(AtlasError) -> ValidatorKeys {
        do throws(BPListError) {
            _ = try value.asInt64()
        } catch {
            throw .bplistError(error)
        }
        return key
    }
    static func validateUuid(key: ValidatorKeys, value: BPList.Object) throws(AtlasError) -> ValidatorKeys {
        var data: RawSpan
        do throws(BPListError) {
            data = try value.asData()
        } catch {
            throw .bplistError(error)
        }
        guard data.byteCount == 16 else { throw .truncatedUuid }
        return key
    }
    static func validateData(key: ValidatorKeys, value: BPList.Object, size:Int? = nil) throws(AtlasError) -> ValidatorKeys {
        var data: RawSpan
        do throws(BPListError) {
            data = try value.asData()
        } catch {
            throw .bplistError(error)
        }
        if let size {
            guard data.byteCount == size else { throw .truncatedData }
        }
        return key
    }
    @_lifetime(copy value)
    static func validateArray(value: BPList.Object) throws(AtlasError) -> BPList.ArrayGuts {
        do throws(BPListError) {
            return try value.asArray()
        } catch {
            throw .bplistError(error)
        }
    }
}

extension Snapshot.Impl: AtlasSchemaValidator {
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let proc     = ValidatorKeys(rawValue: 1 << 0)
        static let plat     = ValidatorKeys(rawValue: 1 << 1)
        static let time     = ValidatorKeys(rawValue: 1 << 2)
        static let stat     = ValidatorKeys(rawValue: 1 << 3)
        static let flags    = ValidatorKeys(rawValue: 1 << 4)
        static let imgs     = ValidatorKeys(rawValue: 1 << 5)
        static let aots     = ValidatorKeys(rawValue: 1 << 6)
        static let envp     = ValidatorKeys(rawValue: 1 << 7)
        static let dsc1     = ValidatorKeys(rawValue: 1 << 8)
        static let metr     = ValidatorKeys(rawValue: 1 << 9)

        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    static var requiredKeys: ValidatorKeys { return [.proc, .plat, .time, .stat, .imgs] }
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "proc":    return try validateInt64(key:.proc, value:value)
        case "plat":    return try validateInt64(key:.plat, value:value)
        case "time":    return try validateInt64(key:.time, value:value)
        case "stat":    return try validateInt64(key:.stat, value:value)
        case "flags":   return try validateInt64(key:.flags, value:value)
        case "imgs":
            let values = try validateArray(value:value)
            try values.forEach { (img) throws(AtlasError) in
                try Image.Impl.validate(bplist:img)
            }
            return .imgs
        case "aots":
            let values = try validateArray(value:value)
            try values.forEach { (img) throws(AtlasError) in
                try AOTImage.Impl.validate(bplist:img)
            }
            return .aots
        case "envp":
            try Environment.Impl.validate(bplist:value)
            return .envp
        case "metr":
            do throws(BPListError) {
                let metrics =  try value.asDictionary()
                try metrics.forEach { (key, value) throws(BPListError) in
                    _ = try value.asInt64()
                }
            } catch {
                throw .bplistError(error)
            }
            return .metr
        case "dsc1":
            try SharedCache.ProcessRecord.validate(bplist:value)
            return .dsc1
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension Environment.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [] }
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let root = ValidatorKeys(rawValue: 1 << 0)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "root":    return try validateString(key:.root, value:value)
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension SharedCache.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.uuid, .padr, .psze, .size, .imgs] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let uuid     = ValidatorKeys(rawValue: 1 << 0)
        static let padr     = ValidatorKeys(rawValue: 1 << 1)
        static let psze     = ValidatorKeys(rawValue: 1 << 2)
        static let size     = ValidatorKeys(rawValue: 1 << 3)
        static let snme     = ValidatorKeys(rawValue: 1 << 4)
        static let suid     = ValidatorKeys(rawValue: 1 << 6)
        static let imgs     = ValidatorKeys(rawValue: 1 << 7)
        static let aots     = ValidatorKeys(rawValue: 1 << 8)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "uuid":    return try validateUuid(key:.uuid, value:value)
        case "padr":    return try validateInt64(key:.padr, value:value)
        case "psze":    return try validateInt64(key:.psze, value:value)
        case "size":    return try validateInt64(key:.size, value:value)
        case "snme":    return try validateString(key:.snme, value:value)
        case "suid":    return try validateUuid(key:.suid, value:value)
        case "imgs":
            let values = try validateArray(value:value)
            try values.forEach { (img) throws(AtlasError) in
                try Image.Impl.validate(bplist:img)
            }
            return .imgs
        case "aots":
            let values = try validateArray(value:value)
            try values.forEach { (aot) throws(AtlasError) in
                try AOTImage.Impl.validate(bplist:aot)
            }
            return .aots
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension SharedCache.ProcessRecord: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.uuid, .bitm, .addr, .file] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let uuid     = ValidatorKeys(rawValue: 1 << 0)
        static let bitm     = ValidatorKeys(rawValue: 1 << 1)
        static let addr     = ValidatorKeys(rawValue: 1 << 2)
        static let file     = ValidatorKeys(rawValue: 1 << 3)
        static let aadr     = ValidatorKeys(rawValue: 1 << 4)
        static let auid     = ValidatorKeys(rawValue: 1 << 5)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "uuid":    return try validateUuid(key:.uuid, value:value)
        case "bitm":    return try validateData(key:.bitm, value:value)
        case "addr":    return try validateInt64(key:.addr, value:value)
        case "file":    return try validateString(key:.file, value:value)
        case "aadr":    return try validateInt64(key:.aadr, value:value)
        case "auid":    return try validateUuid(key:.auid, value:value)
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension SubCache.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.uuid, .name, .size, .voff, .padr, .suid, .maps] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let uuid     = ValidatorKeys(rawValue: 1 << 0)
        static let name     = ValidatorKeys(rawValue: 1 << 1)
        static let size     = ValidatorKeys(rawValue: 1 << 2)
        static let voff     = ValidatorKeys(rawValue: 1 << 3)
        static let padr     = ValidatorKeys(rawValue: 1 << 4)
        static let suid     = ValidatorKeys(rawValue: 1 << 5)
        static let maps     = ValidatorKeys(rawValue: 1 << 6)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "uuid":    return try validateUuid(key:.uuid, value:value)
        case "name":    return try validateString(key:.name, value:value)
        case "size":    return try validateInt64(key:.size, value:value)
        case "voff":    return try validateInt64(key:.voff, value:value)
        case "padr":    return try validateInt64(key:.padr, value:value)
        case "suid":    return try validateUuid(key:.suid, value:value)
        case "maps":
            let values = try validateArray(value:value)
            try values.forEach { (map) throws(AtlasError) in
                try SubCacheMapping.validate(bplist:map)
            }
            return .maps
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

// We don't directly expose the SubCacheMapping via API, so there is no struct to extend. We do parse it to create the cache mapper though, so we need
// to validate it.
struct SubCacheMapping: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.padr, .size, .foff, .prot] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let padr     = ValidatorKeys(rawValue: 1 << 0)
        static let size     = ValidatorKeys(rawValue: 1 << 1)
        static let foff     = ValidatorKeys(rawValue: 1 << 2)
        static let prot     = ValidatorKeys(rawValue: 1 << 3)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "padr":    return try validateInt64(key:.padr, value:value)
        case "size":    return try validateInt64(key:.size, value:value)
        case "foff":    return try validateInt64(key:.foff, value:value)
        case "prot":    return try validateInt64(key:.prot, value:value)
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension Image.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.segs] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let padr     = ValidatorKeys(rawValue: 1 << 0)
        static let addr     = ValidatorKeys(rawValue: 1 << 1)
        static let uuid     = ValidatorKeys(rawValue: 1 << 2)
        static let name     = ValidatorKeys(rawValue: 1 << 3)
        static let file     = ValidatorKeys(rawValue: 1 << 4)
        static let segs     = ValidatorKeys(rawValue: 1 << 5)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "uuid":    return try validateUuid(key:.uuid, value:value)
        case "padr":    return try validateInt64(key:.padr, value:value)
        case "addr":    return try validateInt64(key:.addr, value:value)
        case "name":    return try validateString(key:.name, value:value)
        case "file":    return try validateString(key:.file, value:value)
        case "segs":
            let values = try validateArray(value:value)
            try values.forEach { (seg) throws(AtlasError) in
                try Segment.Impl.validate(bplist:seg)
            }
            return .segs
        default:        return ValidatorKeys(rawValue:0)
        }
        //FIXME: Add complex validate for cnditional fields
    }
    
    static func checkComplexKeyRelationships(keys: ValidatorKeys) throws(AtlasError) {
        if !keys.contains(.addr) && !keys.contains(.padr) {
            throw .missingAddressField
        }
    }
}

extension Segment.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.name, .size, .fsze, .padr, .perm] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let name     = ValidatorKeys(rawValue: 1 << 0)
        static let size     = ValidatorKeys(rawValue: 1 << 1)
        static let fsze     = ValidatorKeys(rawValue: 1 << 2)
        static let padr     = ValidatorKeys(rawValue: 1 << 3)
        static let perm     = ValidatorKeys(rawValue: 1 << 4)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "name":    return try validateString(key:.name, value:value)
        case "size":    return try validateInt64(key:.size, value:value)
        case "fsze":    return try validateInt64(key:.fsze, value:value)
        case "padr":    return try validateInt64(key:.padr, value:value)
        case "perm":    return try validateInt64(key:.perm, value:value)
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

extension AOTImage.Impl: AtlasSchemaValidator {
    static var requiredKeys: ValidatorKeys { return [.xadr, .aadr, .asze, .ikey] }
    
    struct ValidatorKeys: OptionSet {
        let rawValue: UInt64
        static let xadr     = ValidatorKeys(rawValue: 1 << 0)
        static let aadr     = ValidatorKeys(rawValue: 1 << 1)
        static let asze     = ValidatorKeys(rawValue: 1 << 2)
        static let ikey     = ValidatorKeys(rawValue: 1 << 3)
        init(rawValue: UInt64) {
            self.rawValue = rawValue
        }
    }
    
    static func validate(keyString: BPList.FastString, value:BPList.Object) throws(AtlasError) -> ValidatorKeys {
        switch keyString.stringValue {
        case "xadr":    return try validateInt64(key:.xadr, value:value)
        case "aadr":    return try validateInt64(key:.aadr, value:value)
        case "asze":    return try validateInt64(key:.asze, value:value)
        case "ikey":    return try validateData(key:.ikey, value:value, size:32)
        default:        return ValidatorKeys(rawValue:0)
        }
    }
}

