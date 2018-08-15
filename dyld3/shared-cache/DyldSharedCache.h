/*
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


#ifndef DyldSharedCache_h
#define DyldSharedCache_h

#include <set>
#include <string>
#include <vector>
#include <unordered_map>

#include "dyld_cache_format.h"
#include "Diagnostics.h"
#include "MachOParser.h"


namespace  dyld3 {
  namespace launch_cache {
    namespace binary_format {
      struct Closure;
      struct ImageGroup;
      struct Image;
    }
  }
}


class VIS_HIDDEN DyldSharedCache
{
public:

    enum CodeSigningDigestMode
    {
        SHA256only = 0,
        SHA1only   = 1,
        Agile      = 2
    };

    struct CreateOptions
    {
        std::string                                 archName;
        dyld3::Platform                             platform;
        bool                                        excludeLocalSymbols;
        bool                                        optimizeStubs;
        bool                                        optimizeObjC;
        CodeSigningDigestMode                       codeSigningDigestMode;
        bool                                        agileSignatureChooseSHA256CdHash;
        bool                                        dylibsRemovedDuringMastering;
        bool                                        inodesAreSameAsRuntime;
        bool                                        cacheSupportsASLR;
        bool                                        forSimulator;
        bool                                        verbose;
        bool                                        evictLeafDylibsOnOverflow;
        std::unordered_map<std::string, unsigned>   dylibOrdering;
        std::unordered_map<std::string, unsigned>   dirtyDataSegmentOrdering;
        std::vector<std::string>                    pathPrefixes;
        std::string                                 loggingPrefix;
    };

    struct MappedMachO
    {
                                    MappedMachO()
                                            : mh(nullptr), length(0), isSetUID(false), protectedBySIP(false), sliceFileOffset(0), modTime(0), inode(0) { }
                                    MappedMachO(const std::string& path, const mach_header* p, size_t l, bool isu, bool sip, uint64_t o, uint64_t m, uint64_t i)
                                            : runtimePath(path), mh(p), length(l), isSetUID(isu), protectedBySIP(sip), sliceFileOffset(o), modTime(m), inode(i) { }

        std::string                 runtimePath;
        const mach_header*          mh;
        size_t                      length;
        uint64_t                    isSetUID        :  1,
                                    protectedBySIP  :  1,
                                    sliceFileOffset : 62;
        uint64_t                    modTime;                // only recorded if inodesAreSameAsRuntime
        uint64_t                    inode;                  // only recorded if inodesAreSameAsRuntime
    };

    struct CreateResults
    {
        const DyldSharedCache*          cacheContent    = nullptr;    // caller needs to vm_deallocate() when done
        size_t                          cacheLength     = 0;
        std::string                     errorMessage;
        std::set<std::string>           warnings;
        std::set<const mach_header*>    evictions;
        bool                            agileSignature = false;
        std::string                     cdHashFirst;
        std::string                     cdHashSecond;
    };


    // This function verifies the set of dylibs that will go into the cache are self contained.  That the depend on no dylibs
    // outset the set.  It will call back the loader function to try to find any mising dylibs.
    static bool verifySelfContained(std::vector<MappedMachO>& dylibsToCache, MappedMachO (^loader)(const std::string& runtimePath), std::vector<std::pair<DyldSharedCache::MappedMachO, std::set<std::string>>>& excluded);


    //
    // This function is single threaded and creates a shared cache. The cache file is created in-memory.
    //
    // Inputs:
    //      options:        various per-platform flags
    //      dylibsToCache:  a list of dylibs to include in the cache
    //      otherOsDylibs:  a list of other OS dylibs and bundle which should have load info added to the cache
    //      osExecutables:  a list of main executables which should have closures created in the cache
    //
    // Returns:
    //    On success:
    //         cacheContent: start of the allocated cache buffer which must be vm_deallocated after the caller writes out the buffer.
    //         cacheLength:  size of the allocated cache buffer
    //         cdHash:       hash of the code directory of the code blob of the created cache
    //         warnings:     all warning messsages generated during the creation of the cache
    //
    //    On failure:
    //         cacheContent: nullptr
    //         errorMessage: the string describing why the cache could not be created
    //         warnings:     all warning messsages generated before the failure
    //
    static CreateResults create(const CreateOptions&             options,
                                const std::vector<MappedMachO>&  dylibsToCache,
                                const std::vector<MappedMachO>&  otherOsDylibs,
                                const std::vector<MappedMachO>&  osExecutables);


    //
    // Returns a text "map" file as a big string
    //
    std::string         mapFile() const;


    //
    // Returns the architecture name of the shared cache, e.g. "arm64"
    //
    std::string         archName() const;


    //
    // Returns the platform the cache is for
    //
    uint32_t            platform() const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImage(void (^handler)(const mach_header* mh, const char* installName)) const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImageEntry(void (^handler)(const char* path, uint64_t mTime, uint64_t inode)) const;


    //
    // Iterates over each dylib in the cache
    //
    void                forEachImageTextSegment(void (^handler)(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName)) const;


    //
    // Iterates over each of the three regions in the cache
    //
    void                forEachRegion(void (^handler)(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions)) const;


    //
    // returns address the cache would load at if unslid
    //
    uint64_t            unslidLoadAddress() const;


    //
    // returns UUID of cache
    //
    void                getUUID(uuid_t uuid) const;


    //
    // returns the vm size required to map cache
    //
    uint64_t            mappedSize() const;


    dyld_cache_header header;
};








#endif /* DyldSharedCache_h */
