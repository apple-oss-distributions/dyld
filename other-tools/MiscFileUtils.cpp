/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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

#include "MiscFileUtils.h"

// mach_o
#include "Archive.h"
#include "Error.h"
#include "UnsafeHeader.h"
#include "Universal.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <AvailabilityMacros.h>
#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_priv.h>

using mach_o::Archive;
using mach_o::Error;
using mach_o::UnsafeHeader;
using mach_o::Universal;
using mach_o::Architecture;

namespace other_tools
{

std::span<const uint8_t> mapFileReadOnly(CString path, struct stat* statBuf)
{
    int fd = ::open(path.c_str(), O_RDONLY, 0);
    if ( fd == -1 )
        return std::span<const uint8_t>();
    struct stat localStatBuf;
    if (statBuf == nullptr)
        statBuf = &localStatBuf;
    if ( ::fstat(fd, statBuf) == -1 )
        return std::span<const uint8_t>();
    const void* mapping = ::mmap(nullptr, (size_t)statBuf->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if ( mapping == MAP_FAILED )
        return std::span<const uint8_t>();

    return std::span((uint8_t*)mapping, (size_t)statBuf->st_size);
}

Error mapFileReadOnly(CString path, std::span<const uint8_t>& mappedBuffer, struct stat* statBuf)
{
    int fd = ::open(path.c_str(), O_RDONLY, 0);
    if ( fd == -1 ) {
        int en = errno;
        if ( en == ENOENT )
            return Error("file not found '%s'", path.c_str());
        return Error("cannot open() '%s', errno=%d", path.c_str(), en);
    }
    struct stat localStatBuf;
    if (statBuf == nullptr)
        statBuf = &localStatBuf;
    if ( ::fstat(fd, statBuf) == -1 )
        return Error("cannot stat('%s'), errno=%d", path.c_str(), errno);
    const void* mapping = ::mmap(nullptr, (size_t)statBuf->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if ( mapping == MAP_FAILED )
        return Error("cannot mmap() file '%s', errno=%d", path.c_str(), errno);

    mappedBuffer = std::span((uint8_t*)mapping, (size_t)statBuf->st_size);
    return Error::none();
}


void unmapFile(std::span<const uint8_t> buffer)
{
    ::munmap((void*)buffer.data(), buffer.size());
}

bool withReadOnlyMappedFile(const char* path, void (^handler)(std::span<const uint8_t>))
{
    return withReadOnlyMappedFile(path, ^(std::span<const uint8_t> buffer, struct stat &statBuf) {
        handler(buffer);
    });
}

bool withReadOnlyMappedFile(const char* path, void (^handler)(std::span<const uint8_t>, struct stat &statBuf))
{
    struct stat statBuf;
    std::span<const uint8_t> buf = mapFileReadOnly(path, &statBuf);
    if ( buf.empty() )
        return false;

    handler(buf, statBuf);
    unmapFile(buf);
    return true;
}


static bool inStringVector(const std::span<const char*>& vect, const char* target)
{
    for (const char* str : vect) {
        if ( strcmp(str, target) == 0 )
            return true;
    }
    return false;
}

void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter,
                             void (^handler)(const char* path, const UnsafeHeader* slice, size_t len))
{
    forSelectedSliceInPaths(paths, archFilter, /* dyld cache */ nullptr, handler);
}


void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter, const DyldSharedCache* dyldCache,
                             void (^handler)(const char* path, const UnsafeHeader* slice, size_t len))
{
    const size_t pathCount = paths.size();
    CString      cpaths[pathCount];
    for ( size_t i=0; i < pathCount; ++i )
        cpaths[i] = paths[i];
    forEachFileGetSlices(std::span<CString>(cpaths, pathCount), dyldCache, ^(CString slicePath, const struct stat& statBuf, std::span<const Universal::Slice> slices) {
        for (const Universal::Slice& slice : slices ) {
            const char* sliceArchName = slice.arch.name();
            if ( archFilter.empty() || inStringVector(archFilter, sliceArchName) ) {
                if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(slice.buffer) ) {
                    handler(slicePath.c_str(), hdr, slice.buffer.size());
                }
                else if ( std::optional<Archive> ar = Archive::isArchive(slice.buffer) ) {
                    Error err = ar->forEachMachO(^(const Archive::Member& m, unsigned memberIndex, const mach_o::UnsafeHeader* mhdr, bool& stop) {
                        char memberPath[PATH_MAX];
                        snprintf(memberPath, sizeof(memberPath), "%s(%s)", slicePath.c_str(), m.name.data());
                        handler(memberPath, mhdr, m.contents.size());
                    });
                    if ( err.hasError() )
                        fprintf(stderr, "malformed archive '%s': %s\n", slicePath.c_str(), err.message());
                }
                else {
                    fprintf(stderr, "non-mach-o in fat file '%s'\n", slicePath.c_str());
                }
            }
        }
    });
}

void forEachFileGetSlices(std::span<const CString> paths, const DyldSharedCache* dyldCache,
                          void (^callback)(CString slicePath, const struct stat& statBuf, std::span<const mach_o::Universal::Slice> slices))
{
    for (CString path : paths) {
        struct stat statBuf;
        std::span<const uint8_t> fileBuffer = mapFileReadOnly(path.c_str(), &statBuf);
        if ( !fileBuffer.empty() ) {
            if ( const Universal* uni = Universal::isUniversal(fileBuffer) ) {
                // if fat file, call handler with all slices
                Universal::Slice    slicesArray[16];
                Universal::Slice*   slices = slicesArray; // work around blocks bugs
                __block size_t      sliceCount = 0;
                uni->forEachSlice(^(Universal::Slice slice, bool& stop) {
                    slices[sliceCount++] = slice;
                });
                callback(path, statBuf, std::span<const Universal::Slice>(slicesArray, sliceCount));
            }
            else if ( const UnsafeHeader* mh = UnsafeHeader::isMachO(fileBuffer) ) {
                // if thin mach-o file, call handler with the one slice
                Universal::Slice slice{ mh->arch(), fileBuffer };
                callback(path, statBuf, std::span<const Universal::Slice>(&slice, 1));
            }
            else if ( std::optional<Archive> ar = Archive::isArchive(fileBuffer) ) {
                // if static library, call handler with whole library
                __block Architecture staticLibArch;
                Error err = ar->forEachMember(^(const Archive::Member& member, unsigned memberIndex, uint64_t memberFileOffset, bool& stop) {
                    if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(member.contents) ) {
                        staticLibArch = hdr->arch();
                        stop = true;
                    }
                });
                Universal::Slice slice{ staticLibArch, fileBuffer };
                callback(path, statBuf, std::span<const Universal::Slice>(&slice, 1));
            }
            else {
                // unknown file type
                Universal::Slice slice{ Architecture(), fileBuffer };
                callback(path, statBuf, std::span<const Universal::Slice>(&slice, 1));
            }

            // Done with the buffer
            unmapFile(fileBuffer);
        }
        else {
            // if path not found, check if the path is in the dyld cache
            bool                             found = false;
            size_t                           cacheLen;
            if ( dyldCache == nullptr )
                dyldCache   = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLen);
#if !TARGET_OS_OSX || (MAC_OS_X_VERSION_MIN_REQUIRED >= 120000)
            __block const DyldSharedCache*  dyldCacheDK = nullptr;
            __block const char*             currentArch = dyldCache->archName();
            bzero(&statBuf, sizeof(statBuf)); // dylibs in shared cache have no stat() info
            if ( path.starts_with("/System/DriverKit/") ) {
                if ( dyldCacheDK == nullptr ) {
                    dyld_for_each_installed_shared_cache(^(dyld_shared_cache_t cacheRef) {
                        //__block bool firstCacheFile = false;
                        dyld_shared_cache_for_each_file(cacheRef, ^(const char* aCacheFilePath) {
                            // skip non-driverkit caches
                            if ( strncmp(aCacheFilePath, "/System/DriverKit/", 18) != 0 )
                                return;

                            // skip cache files for all but matching arch with no extension
                            const char* founddk = strstr(aCacheFilePath, currentArch);
                            if ( founddk == nullptr || strlen(founddk) != strlen(currentArch) )
                                return;

                            std::vector<const DyldSharedCache*> dyldCaches = DyldSharedCache::mapCacheFiles(aCacheFilePath);
                            if ( dyldCaches.empty() )
                                return;
                            dyldCacheDK = dyldCaches.front();
                        });
                    });
                }
                if ( dyldCacheDK != nullptr ) {
                    uint32_t imageIndex;
                    if ( dyldCacheDK->hasImagePath(path.c_str(), imageIndex) ) {
                        const UnsafeHeader* hdr = (UnsafeHeader*)dyldCacheDK->getIndexedImageEntry(imageIndex);
                        Universal::Slice slice{ hdr->arch(), std::span<const uint8_t>((uint8_t*)hdr, 0xF0000000) };
                        callback(path, statBuf, std::span<const Universal::Slice>(&slice, 1));
                        found = true;
                    }
                }
            }
            else
#endif

            if ( dyldCache != nullptr ) {
                // see if path is in current dyld shared cache
                uint32_t imageIndex;
                if ( dyldCache->hasImagePath(path.c_str(), imageIndex) ) {
                    const UnsafeHeader* hdr = (UnsafeHeader*)dyldCache->getIndexedImageEntry(imageIndex);
                    Universal::Slice slice{ hdr->arch(), std::span<const uint8_t>((uint8_t*)hdr, 0xF0000000) };
                    callback(path, statBuf, std::span<const Universal::Slice>(&slice, 1));
                    found = true;
                }
            }
            // do callback with an empty buffer to signify path not found
            if ( !found ) {
                Universal::Slice emptySlice{ Architecture(), std::span<const uint8_t>() };
                callback(path, statBuf, std::span<const Universal::Slice>(&emptySlice, 1));
            }
        }
    }
}

} // namespace other_tools
