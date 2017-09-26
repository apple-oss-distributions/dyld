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



#ifndef __DYLD_CLOSURE_BUFFER_H__
#define __DYLD_CLOSURE_BUFFER_H__

#include "Logging.h"
#include "LaunchCache.h"
#include "PathOverrides.h"

namespace dyld3 {


// simple class for packing typed content into a vm_allocated buffer
class VIS_HIDDEN TypedContentBuffer
{
public:
    // buffer creation
                    TypedContentBuffer(size_t elementsCount, size_t elementsTotalSize);
    void            addItem(uint32_t k, const void* content, size_t len);
    void            doneBuilding();
    vm_address_t    vmBuffer() const;
    uint32_t        vmBufferSize() const;

    // buffer parsing
                TypedContentBuffer(const void* buff, size_t buffSize);
    unsigned    count(uint32_t) const;
    void        forEach(uint32_t, void (^callback)(const void* content, size_t length)) const;

    void        free();

private:
    struct Element
    {
        uint32_t    kind;
        uint32_t    contentLength;
        uint8_t     content[];

        const Element* next() const;
    };

    size_t      _size;
    Element*    _buffer;
    Element*    _currentEnd;
    bool        _readOnly;
};


class VIS_HIDDEN ClosureBuffer : public TypedContentBuffer
{
public:

    struct CacheIdent
    {
        uint8_t     cacheUUID[16];
        uint64_t    cacheAddress;
        uint64_t    cacheMappedSize;
    };

    // client creation
                        ClosureBuffer(const CacheIdent&, const char* path, const launch_cache::ImageGroupList& groups, const PathOverrides& envVars);

    // closured creation
                        ClosureBuffer(const char* errorMessage);
                        ClosureBuffer(const launch_cache::BinaryImageGroupData* imageGroupResult);
                        ClosureBuffer(const launch_cache::BinaryClosureData* closureResult);

    // client extraction
    bool                                        isError() const;
    const char*                                 errorMessage() const;
    const launch_cache::BinaryClosureData*      closure() const;
    const launch_cache::BinaryImageGroupData*   imageGroup() const;

    // closure builder usage
                        ClosureBuffer(const void* buff, size_t buffSize);
    const CacheIdent&   cacheIndent() const;
    const char*         targetPath() const;
    uint32_t            envVarCount() const;
    void                copyImageGroups(const char* envVars[]) const;
    uint32_t            imageGroupCount() const;
    void                copyImageGroups(const launch_cache::BinaryImageGroupData* imageGroups[]) const;

private:
    enum { kindEnd=0, kindCacheIdent, kindTargetPath, kindEnvVar, kindImageGroup, kindClosure, kindErrorMessage };
    static size_t       computeSize(const char* path, const launch_cache::ImageGroupList& groups, const PathOverrides& envVars);

};




} // namespace dyld3

#endif // __DYLD_CLOSURE_BUFFER_H__
