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

import System

// Swift's FilePath works completely in process without making syscalls, so it cannot resolve symlinks.
// This matches dyld's usage, where we need to resolve symlinks iin archives or the shared cache, not
// an actual filesystem, so we can extended it by passing in a set of symlinks and/or a base path to
// use resolving a path.
internal extension FilePath {
    mutating func resolving(from path: FilePath = "", symlinks:[FilePath:FilePath]? = nil) {
        if #available(macOS 12.0, *) {
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
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }
    func resolved(from path: FilePath = "", symlinks:[FilePath:FilePath]? = nil) -> FilePath {
        var result = self
        result.resolving(from: path, symlinks: symlinks)
        return result
    }
}
