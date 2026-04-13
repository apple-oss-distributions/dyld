/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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

import os
import System

// Swift's FilePath works completely in process without making syscalls, so it cannot resolve symlinks.
// This matches dyld's usage, where we need to resolve symlinks iin archives or the shared cache, not
// an actual filesystem, so we can extended it by passing in a set of symlinks and/or a base path to
// use resolving a path.
internal extension FilePath {
    mutating func resolving(from path: FilePath = "", symlinks:[FilePath:FilePath]? = nil) {
        self = path.pushing(self)
        self.lexicallyNormalize()
        guard let symlinks else { return }
        for _ in 0..<MAXSYMLINKS {
            var foundLink = false
            for (source, target) in symlinks {
                guard self.starts(with:source) else { continue }
                self.removeLastComponent()
                self.push(target)
                self.lexicallyNormalize()
                foundLink = true
            }
            guard foundLink else { break }
        }
    }
    func resolved(from path: FilePath = "", symlinks:[FilePath:FilePath]? = nil) -> FilePath {
        var result = self
        result.resolving(from: path, symlinks: symlinks)
        return result
    }
    // This is sort of an abuse of file path in that it actually does an FS opetation, but it logically makes sense here
    var realPath: FilePath? {
        let buffer = ManagedBuffer<CChar, CChar>.create(minimumCapacity: Int(PATH_MAX)) {_ in
            return 0
        }
        return buffer.withUnsafeMutablePointerToElements {
            guard Darwin.realpath(self.string, $0) != nil else {
                return nil
            }
            return FilePath(String(cString:$0))
        }
    }
    // This is even more of of abuse, but since the Swift stdlib does not include any nice posix file wrappers and FileManager
    // is not likely to work in any code we share with dyld we need this functionality. Until we write nice Posix FS wrappers just
    // leave it here
    var children: [FilePath]? {
        var result = [FilePath]()
        guard let dirp = Darwin.opendir(self.string) else {
            return nil
        }
        defer {
            Darwin.closedir(dirp)
        }
        while let dirEntry =  Darwin.readdir(dirp) {
            // We cannot use pointee because that can copy, and Swift does not provide any ergonomic way to get to the underlying data
            // since people should not generally do that, but here we are, so we get the raw pointer and directly load the fields
            // we need to.

            // We assume dirent is greater than a UInt16 (and we can therefore use aligned loads) and that _DARWIN_FEATURE_64_BIT_INODE is
            // set (so we know the types of fields). Both of these are true on all current platforms.
            assert(MemoryLayout<dirent>.alignment > MemoryLayout<UInt16>.alignment)
            assert(_DARWIN_FEATURE_64_BIT_INODE != 0)

            let rawPointer = UnsafeRawPointer(dirEntry)
            let dirType = rawPointer.load(fromByteOffset:MemoryLayout<dirent>.offset(of: \.d_type)!, as:UInt8.self)

            guard dirType == DT_REG || dirType == DT_DIR else {
                continue
            }

            let componentSize = rawPointer.load(fromByteOffset:MemoryLayout<dirent>.offset(of: \.d_namlen)!, as:UInt16.self)+1
            let componentStartIndex = MemoryLayout<dirent>.offset(of: \.d_name)!
            let componentBuffer = UnsafeRawBufferPointer(start:rawPointer+componentStartIndex, count:Int(componentSize))

            guard let componentString = componentBuffer.bindMemory(to: UInt8.self).baseAddress.flatMap(String.init(cString:))  else {
                continue
            }
            guard componentString != ".." && componentString != "." else { continue }
            guard let component = FilePath.Component(componentString) else {
                continue
            }
            result.append(self.appending(component))
        }
        return result
    }
}
