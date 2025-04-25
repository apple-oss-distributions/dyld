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

#include <stdint.h>

#include "Defines.h"
#include "Header.h"
#include "Error.h"
#include "LibSystemHelpers.h"
#include "ThreadLocalVariables.h"
#include "DyldSharedCache.h"

using dyld4::LibSystemHelpers;
using mach_o::Error;

extern "C" void* _tlv_get_addr(dyld::ThreadLocalVariables::Thunk*);

namespace dyld {



// call by dyld via libSystemHelpers->setUpThreadLocals() at launch and during dlopen()
Error ThreadLocalVariables::setUpImage(const DyldSharedCache* cache, const Header* hdr)
{
// driverkit and main OS use same dyld, but driverkit process do not support thread locals
#if __has_feature(tls)
    if ( hdr->inDyldCache() ) {
        if ( Error err = this->initializeThunksInDyldCache(cache, hdr) )
            return err;
    }
    else {
        if ( Error err = this->initializeThunksFromDisk(hdr) )
            return err;
    }
    return Error::none();
#else
    return Error::none();
#endif // __has_feature(tls)
}

#if __has_feature(tls)
void ThreadLocalVariables::findInitialContent(const Header* hdr, std::span<const uint8_t>& initialContent, bool& allZeroFill)
{
    allZeroFill = true;
#if BUILDING_UNIT_TESTS
    allZeroFill    = _allZeroFillContent;
    initialContent = _initialContent;
#else
    // find initial content for all TLVs in image
    intptr_t slide = (intptr_t)hdr->getSlide();
    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        switch (sectInfo.flags & SECTION_TYPE) {
            case S_THREAD_LOCAL_REGULAR:
                allZeroFill = false;
                [[clang::fallthrough]];
            case S_THREAD_LOCAL_ZEROFILL:
                if ( initialContent.empty() ) {
                    // first of N contiguous TLV template sections, record as if this was only section
                    initialContent = std::span<const uint8_t>((const uint8_t*)(sectInfo.address + slide), (size_t)sectInfo.size);
                }
                else {
                    // non-first of N contiguous TLV template sections, accumlate values
                    size_t newSize = (uintptr_t)sectInfo.address + (uintptr_t)sectInfo.size + slide - (uintptr_t)initialContent.data();
                    initialContent = std::span<const uint8_t>(initialContent.data(), newSize);
                }
                break;
        }
    });
#endif
}

// most images have just one __thread_vars section, but some have one in __DATA and one in __DATA_DIRTY
Error ThreadLocalVariables::forEachThunkSpan(const Header* hdr, Error (^visit)(std::span<Thunk>))
{
#if BUILDING_UNIT_TESTS
    return visit(_thunks);
#else
    __block Error setUpErr;
    // find section with array of TLV thunks
    // and also initial content for all TLVs in image
    intptr_t                         slide   = (intptr_t)hdr->getSlide();
    hdr->forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.flags & SECTION_TYPE) == S_THREAD_LOCAL_VARIABLES ) {
            if ( sectInfo.size % sizeof(Thunk) != 0) {
                setUpErr = Error("size (%llu) of thread-locals section %.*s is not a multiple of %lu",
                                 sectInfo.size, (int)sectInfo.sectionName.size(), sectInfo.sectionName.data(), sizeof(Thunk));
                stop = true;
                return;
            }
            if ( sectInfo.size >= sizeof(Thunk) ) {
                std::span<Thunk> thunks = std::span<Thunk>((Thunk*)(sectInfo.address + slide), (size_t)(sectInfo.size/sizeof(Thunk)));
                setUpErr = visit(thunks);
            }
        }
    });
    return std::move(setUpErr);
#endif
}

static void finalizeListTLV(void* list)
{
#if BUILDING_LIBDYLD
    // Called by libc/pthreads when the current thread is going away
    sThreadLocalVariables.finalizeList(list);
#endif // BUILDING_LIBDYLD
}


// This is called during libSystem initialization, which passes libSystemHelpers from libdyld down to dyld.
// _libSystem_initialize() -> _dyld_initialize() -> APIs::_libdyld_initialize() -> ThreadLocalVariables::initialize()
void ThreadLocalVariables::initialize()
{
    // assign pthread_key for per-thread terminators
    // Note: if a thread is terminated, the value for this key is cleaned up by calling finalizeList()
    dyld_thread_key_create(&_terminatorsKey, &finalizeListTLV);
}

Error ThreadLocalVariables::initializeThunksFromDisk(const Header* hdr)
{
    // each dylib gets a new key used for all thread-locals in that dylib
    dyld_thread_key_t key;
#if BUILDING_UNIT_TESTS
    key = _key;
#else
    if ( dyld_thread_key_create(&key, &::free) )
        return Error("pthread_key_create() failed");
#endif

    // find initial content for all TLVs in image
    std::span<const uint8_t>  initialContent;
    bool                      allZeroFill;
    findInitialContent(hdr, initialContent, allZeroFill);

    // set the thunk function pointer and key for every thread local variable
    Error err = forEachThunkSpan(hdr, ^(std::span<Thunk> thunks) {
        for ( Thunk& thunk : thunks ) {
            uintptr_t    offset  = thunk.offset;
            if ( offset > initialContent.size() )
                return Error("malformed thread-local, offset=0x%lX is larger than total size=0x%lX", thunk.offset, initialContent.size() );
#if __LP64__
            if ( initialContent.size() > 0xFFFFFFFFUL )
                return Error("unsupported thread-local, larger than 4GB");
            if ( key > 0xFFFFFFFFUL )
                return Error("thread_key_t %lu, larger than uint32_t", key);
            TLV_Thunkv2* thunkv2 = (TLV_Thunkv2*)&thunk;
            thunkv2->func                = (void*)&_tlv_get_addr;
            thunkv2->key                 = (uint32_t)key;
            thunkv2->offset              = (uint32_t)offset;
            thunkv2->initialContentDelta = (uint32_t)(initialContent.data() - (uint8_t*)(&thunkv2->initialContentDelta));
            thunkv2->initialContentSize  = (uint32_t)initialContent.size();
            // if initial content is all zeros, no need to store delta to initialContent
            if ( allZeroFill )
                thunkv2->initialContentDelta = 0;
            if (verbose) fprintf(stderr, "initializeThunksFromDisk(%p): thunk=%p, key=%d, offset=0x%08X, delta=0x%08X, size=%d\n",
                                 hdr, thunkv2, thunkv2->key, thunkv2->offset, thunkv2->initialContentDelta, thunkv2->initialContentSize);
#else
            if ( key > 0xFFFF )
                return Error("thread_key_t %lu, larger than uint16_t", key);
            TLV_Thunkv2_32* thunkv2 = (TLV_Thunkv2_32*)&thunk;
            thunkv2->func                = (void*)&_tlv_get_addr;
            thunkv2->key                 = (uint16_t)key;
            thunkv2->offset              = (uint16_t)offset;
            // if initial content is all zeros, store size, otherwise store delta to mach_header so runtime can find __thread_ sections
            if ( allZeroFill ) {
                if ( initialContent.size() > 0x7FFFFFFF )
                    return Error("unsupported thread-local, larger than 2GB of zero-fill");
                thunkv2->machHeaderDelta = (uint32_t)initialContent.size();
            }
            else {
                thunkv2->machHeaderDelta = (int32_t)((uint8_t*)hdr - (uint8_t*)&thunkv2->machHeaderDelta);
            }
            if (verbose) fprintf(stderr, "initializeThunksFromDisk(%p): thunk=%p, key=%d, offset=0x%04X, machHeaderDelta=0x%08X\n",
                                hdr, thunkv2, thunkv2->key, thunkv2->offset, thunkv2->machHeaderDelta);
#endif
        }
        return Error::none();
    });
    return err;
}

Error ThreadLocalVariables::initializeThunksInDyldCache(const DyldSharedCache* cache, const Header* hdr)
{
    // if cache builder runs out of static keys, it leaves the thunks looking like they do on disk (key == 0)
    __block bool notOptimized = false;
    Error err = forEachThunkSpan(hdr, ^(std::span<Thunk> thunks) {
        int staticKey = 0;
        // FIXME: simplify once new cache format is standard
        if ( cache->header.newFormatTLVs ) {
#if __LP64__
            TLV_Thunkv2*    thunkv2 = (TLV_Thunkv2*)&thunks[0];
#else
            TLV_Thunkv2_32* thunkv2 = (TLV_Thunkv2_32*)&thunks[0];
#endif
            staticKey = thunkv2->key;
        }
        else {
            staticKey = (int)thunks[0].key;
        }
        if ( staticKey == 0 ) {
            notOptimized = true;
            if (verbose) fprintf(stderr, "  initializeThunksInDyldCache(%p) thunks=%p not optimized in dyld cache\n", hdr, &thunks[0]);
            return Error::none();
        }
        else {
            // dyld cache builder assigned a static key for these TLVs but we need to register
            // that free() should be called on the key's value if the thread goes away
            dyld_thread_key_init_np(staticKey, &::free);
        }
        // thunks in the dyld shared cache are normally correct, but we may need to be correct them if root of libdyld.dylib is in use
        void* getAddrFunc = (void*)&_tlv_get_addr;
        for ( Thunk& thunk : thunks ) {
            if ( thunk.func!= getAddrFunc )
                thunk.func = getAddrFunc;
        }
        return Error::none();
    });
    if ( notOptimized )
        return initializeThunksFromDisk(hdr);

    if ( err.hasError() )
        return err;

    if ( !cache->header.newFormatTLVs )
        return Error("dyld cache thread-local format too old");

    return Error::none();;
}

void ThreadLocalVariables::addTermFunc(TermFunc func, void* objAddr)
{
    // NOTE: this does not need locks because it only operates on current thread data
    TerminatorList* list = (TerminatorList*)::dyld_thread_getspecific(_terminatorsKey);
    if ( list == nullptr ) {
        // Note: use system malloc because it is thread safe and does not require dyld's allocator to be made r/w
        list = (TerminatorList*)::malloc(sizeof(TerminatorList));
        bzero(list, sizeof(TerminatorList));
        dyld_thread_setspecific(_terminatorsKey, list);
    }
    // go to end of chain
    while (list->next != nullptr)
        list = list->next;
    // make sure there is space to add another element
    if ( list->count == 7 ) {
        // if list is full, add a chain
        TerminatorList* nextList = (TerminatorList*)::malloc(sizeof(TerminatorList));
        bzero(nextList, sizeof(TerminatorList));
        list->next = nextList;
        list = nextList;
    }
    list->elements[list->count++] = { func, objAddr };
}

// <rdar://problem/13741816>
// called by exit() before it calls cxa_finalize() so that thread_local
// objects are destroyed before global objects.
// Note this is only called on macOS, and by libc.
// iOS only destroys tlv's when each thread is destroyed and libpthread calls
// tlv_finalize as that is the pointer we provided when we created the key
void ThreadLocalVariables::exit()
{
    if ( TerminatorList* list = (TerminatorList*)::dyld_thread_getspecific(_terminatorsKey) ) {
        // detach storage from thread while freeing it
        dyld_thread_setspecific(_terminatorsKey, nullptr);
        // Note, if new thread locals are added to our during this termination,
        // they will be on a new list, but the list we have here
        // is one we own and need to destroy it
        this->finalizeList(list);
    }
}

void ThreadLocalVariables::TerminatorList::reverseWalkChain(void (^visit)(TerminatorList*))
{
    if ( this->next != nullptr )
        this->next->reverseWalkChain(visit);
    visit(this);
}

// on entry, libc has set the TSD slot to nullptr and passed us the previous value
// this is done to handle destructors that re-animate the key value
void ThreadLocalVariables::finalizeList(void* l)
{
    TerminatorList* list = (TerminatorList*)l;
    // call term functions in reverse order of construction
    list->reverseWalkChain(^(TerminatorList* chain) {
        for ( uintptr_t i = chain->count; i > 0; --i ) {
            const Terminator& entry = chain->elements[i - 1];
            if ( entry.termFunc != nullptr )
                (*entry.termFunc)(entry.objAddr);

            // If a new tlv was added via tlv_atexit during the termination function just called, then we need to immediately destroy it
            TerminatorList* newlist = (TerminatorList*)(::dyld_thread_getspecific(_terminatorsKey));
            if ( newlist != nullptr ) {
                // Set the list to NULL so that if yet another tlv is registered, we put it in a new list
                dyld_thread_setspecific(_terminatorsKey, nullptr);
                this->finalizeList(newlist);
            }
        }
    });

    // free entire chain
    list->reverseWalkChain(^(TerminatorList* chain) {
        ::free(chain);
    });
}

// called lazily when TLV is first accessed
void* ThreadLocalVariables::instantiateVariable(const Thunk& thunk)
{
#if TARGET_OS_EXCLAVEKIT
    // On ExclaveKit, the assembly code for _tlv_get_addr cannot access thread specific data
    // instead we access it here from C.
    TLV_Thunkv2* ekThunk = (TLV_Thunkv2*)&thunk;
    if ( void* result = ::tss_get(ekThunk->key) )
        return result;
#endif // TARGET_OS_EXCLAVEKIT

    void*               buffer = nullptr;
    dyld_thread_key_t   key    = 0;
#if __LP64__
    TLV_Thunkv2* thunkv2 = (TLV_Thunkv2*)&thunk;
    key    = thunkv2->key;
    if ( thunkv2->initialContentDelta != 0 ) {
        // initial content of thread-locals is non-zero so copy initial bytes from template
        buffer = ::malloc(thunkv2->initialContentSize);
        const uint8_t* initialContent = (uint8_t*)(&thunkv2->initialContentDelta) + thunkv2->initialContentDelta;
        memcpy(buffer, initialContent, thunkv2->initialContentSize);
        if (verbose) fprintf(stderr, "instantiateVariable(%p) buffer=%p, init-content=%p size=%d\n", &thunk, buffer, initialContent, thunkv2->initialContentSize);
    }
    else {
        // initial content of thread-locals is all zeros
        buffer = ::calloc(thunkv2->initialContentSize, 1);
        if (verbose) fprintf(stderr, "instantiateVariable(%p) buffer=%p, zero-fill, size=%d\n", &thunk, buffer, thunkv2->initialContentSize);
    }
#else
    TLV_Thunkv2_32* thunkv2 = (TLV_Thunkv2_32*)&thunk;
    if ( thunkv2->machHeaderDelta < 0 ) {
        // in non-zerofill case, machHeaderDelta is delta to mach_header
        key    = thunkv2->key;
        std::span<const uint8_t> bytes((uint8_t*)&thunkv2->machHeaderDelta + thunkv2->machHeaderDelta, -thunkv2->machHeaderDelta);
        if ( const Header* hdr = Header::isMachO(bytes) ) {
            std::span<const uint8_t>  initialContent;
            bool                      allZeroFill;
            findInitialContent(hdr, initialContent, allZeroFill);
            if ( initialContent.empty() ) {
                fprintf(stderr, "ThreadLocalVariables::getInitialContent(%p) failed\n", hdr);
                return nullptr;  // abort? something has gone wrong
            }
            buffer = ::malloc(initialContent.size());
            memcpy(buffer, initialContent.data(), initialContent.size());
            if (verbose) fprintf(stderr, "instantiateVariable(%p) buffer=%p, init-content=%p size=%lu\n", thunkv2, buffer, initialContent.data(), initialContent.size());
        }
        else {
            fprintf(stderr, "ThreadLocalVariables::instantiateVariable(%p) cannot find mach-o header\n", thunkv2);
            return nullptr;  // abort? something has gone wrong
        }
    }
    else {
        // in zerofill case, machHeaderDelta is size to allocate
        buffer = ::calloc(thunkv2->machHeaderDelta, 1);
        key    = thunkv2->key;
        if (verbose) fprintf(stderr, "instantiateVariable(%p) buffer=%p, zero-fill, size=%d\n", thunkv2, buffer, thunkv2->machHeaderDelta);
    }
#endif

    // set this thread's value for key to be the new buffer.
    dyld_thread_setspecific(key, buffer);

    return buffer;
}
#endif // __has_feature(tls)


#if BUILDING_LIBDYLD
ThreadLocalVariables sThreadLocalVariables;
#endif

#if BUILDING_UNIT_TESTS
void ThreadLocalVariables::setMock(int tlvKey, std::span<Thunk> thunks, std::span<const uint8_t> content)
{
    _key                = tlvKey;
    _thunks             = thunks;
    _initialContent     = content;
    _allZeroFillContent = true;
    for (uint8_t byte : content) {
        if ( byte != 0 ) {
            _allZeroFillContent = false;
            break;
        }
    }
}
#endif


} // namespace
