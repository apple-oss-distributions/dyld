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
#include "Header.h"
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
using mach_o::Header;
using mach_o::Universal;

namespace other_tools
{

bool withReadOnlyMappedFile(const char* path, void (^handler)(std::span<const uint8_t>))
{
    struct stat statbuf;
    if ( ::stat(path, &statbuf) == -1 )
        return false;
    int fd = ::open(path, O_RDONLY, 0);
    if ( fd == -1 )
        return false;
    const void* mapping = ::mmap(nullptr, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if ( mapping == MAP_FAILED )
        return false;

    handler(std::span((uint8_t*)mapping, (size_t)statbuf.st_size));

    ::munmap((void*)mapping, (size_t)statbuf.st_size);
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
                             void (^handler)(const char* path, const Header* slice, size_t len))
{
    forSelectedSliceInPaths(paths, archFilter, /* dyld cache */ nullptr, handler);
}

void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter, const DyldSharedCache* dyldCache,
                             void (^handler)(const char* path, const Header* slice, size_t len))
{
    const auto handleArchive = [handler](const char* path, const Archive& ar) {
        Error err1 = ar.forEachMachO(^(const Archive::Member& m, const mach_o::Header * header, bool &stop) {
            char objPath[PATH_MAX];
            snprintf(objPath, sizeof(objPath), "%s(%s)", path, m.name.data());
            handler(objPath, header, m.contents.size());
        });
        if ( err1.hasError() )
            fprintf(stderr, "malformed archive '%s': %s\n", path, err1.message());
    };

    for (const char* path : paths) {
        bool found = withReadOnlyMappedFile(path, ^(std::span<const uint8_t> buffer) {
            if ( const Universal* uni = Universal::isUniversal(buffer) ) {
                uni->forEachSlice(^(Universal::Slice slice, bool& stopSlice) {
                    const char* sliceArchName = slice.arch.name();
                    if ( archFilter.empty() || inStringVector(archFilter, sliceArchName) ) {
                        if ( std::optional<Archive> ar = Archive::isArchive(slice.buffer) ) {
                            handleArchive(path, *ar);
                        }
                        else if ( Header::isMachO(slice.buffer) ) {
                            handler(path, (Header*)slice.buffer.data(), slice.buffer.size());
                        }
                        else {
                            fprintf(stderr, "%s slice in %s is not a mach-o\n", sliceArchName, path);
                        }
                    }
                });
            }
            else if ( const Header* mh = Header::isMachO(buffer) ) {
                if ( archFilter.empty() || inStringVector(archFilter, mh->archName()) )
                    handler(path, (Header*)buffer.data(), buffer.size());
            }
            else if ( std::optional<Archive> ar = Archive::isArchive(buffer) ) {
                handleArchive(path, *ar);
            }
        });

        // dyld_for_each_installed_shared_cache() only available in macOS 12 aligned platforms
        // and we only build this code for earlier versions on macOS
#if !TARGET_OS_OSX || (MAC_OS_X_VERSION_MIN_REQUIRED >= 120000)
        if ( !found ) {
            size_t                           cacheLen;
            if ( dyldCache == nullptr )
                dyldCache   = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLen);
            __block const DyldSharedCache*  dyldCacheDK = nullptr;
            __block const char*             currentArch = dyldCache->archName();
            if ( strncmp(path, "/System/DriverKit/", 18) == 0 ) {
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
                    if ( dyldCacheDK->hasImagePath(path, imageIndex) ) {
                        const mach_header* mh = dyldCacheDK->getIndexedImageEntry(imageIndex);
                        handler(path, (Header*)mh, (size_t)(-1));
                    }
                }
            }
            else if ( dyldCache != nullptr ) {
                // see if path is in current dyld shared cache
                uint32_t imageIndex;
                if ( dyldCache->hasImagePath(path, imageIndex) ) {
                    const mach_header* mh = dyldCache->getIndexedImageEntry(imageIndex);
                    handler(path, (Header*)mh, (size_t)(-1));
                }
            }
        }
#else
        (void)found;
#endif

    }
}

} // namespace other_tools
