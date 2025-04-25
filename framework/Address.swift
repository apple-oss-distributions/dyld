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

// Provide type safe address primitives

protocol Address: Comparable, Strideable, CustomStringConvertible {
    var value: UInt64 { get }
    init(_ : UInt64)
}

extension Address {
    static func < (lhs: Self, rhs: Self) -> Bool {
        return lhs.value < rhs.value
    }
    func distance(to other: Self) -> Int64 {
        return Int64(bitPattern:other.value &- value)
    }
    func advanced(by n: Int64) -> Self {
        return Self(value &+ UInt64(bitPattern:n))
    }
    var pageAligned: Bool { return (self.value & UInt64(vm_page_mask)) == 0 }

    var description: String {
        return "0x\(String(self.value, radix: 16, uppercase: true))"
    }
}

struct PreferredAddress: Address {
    init(_ value: Int64) {
        self.value = UInt64(bitPattern:value)
    }
    init(_ value: UInt64) {
        self.value = value
    }
    let value: UInt64
    static func +(left: PreferredAddress, right: UInt64) -> PreferredAddress {
        return PreferredAddress(left.value + right)
    }
    static func -(left: PreferredAddress, right: PreferredAddress) -> UInt64 {
        return left.value - right.value
    }
    static func +(left: PreferredAddress, right: Slide) -> RebasedAddress {
        // It is possible (but rare) for the preferred load addres to be above the rebased address, resulting in a negative slide, so allow overflow
        return RebasedAddress(left.value &+ right.value)
    }
}

struct RebasedAddress: Address {
    init(_ value: Int64) {
        self.value = UInt64(bitPattern:value)
    }
    init?(_ value: Int64?) {
        guard let value else { return nil }
        self.value = UInt64(bitPattern:value)
    }
    init(_ value: UInt64) {
        self.value = value
    }
    init(_ value: UInt32) {
        self.value = UInt64(value)
    }
    let value: UInt64
    
    static func -(left: RebasedAddress, right: PreferredAddress) -> Slide {
        // It is possible (but rare) for the preferred load addres to be above the rebased address, resulting in a negative slide, so allow overflow
        return Slide(left.value &- right.value)
    }
}

struct Slide {
    init(_ value:UInt64) {
        self.value = value
    }
    let value: UInt64
}
