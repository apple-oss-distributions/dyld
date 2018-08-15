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


#include <stdint.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h> // mach_absolute_time()
#include <pthread/pthread.h>
#include <libkern/OSAtomic.h>

#include <vector>
#include <algorithm>

#include "AllImages.h"
#include "MachOParser.h"
#include "libdyldEntryVector.h"
#include "Logging.h"
#include "Loading.h"
#include "Tracing.h"
#include "LaunchCache.h"
#include "DyldSharedCache.h"
#include "PathOverrides.h"
#include "DyldCacheParser.h"

extern const char** appleParams;

// should be a header for these
struct __cxa_range_t {
    const void* addr;
    size_t      length;
};
extern "C" void __cxa_finalize_ranges(const __cxa_range_t ranges[], unsigned int count);

VIS_HIDDEN bool gUseDyld3 = false;


namespace dyld3 {

class VIS_HIDDEN LoadedImage {
public:
    enum class State { uninited=3, beingInited=2, inited=0 };
    typedef launch_cache::binary_format::Image      BinaryImage;

                        LoadedImage(const mach_header* mh, const BinaryImage* bi);
    bool                operator==(const LoadedImage& rhs) const;
    void                init(const mach_header* mh, const BinaryImage* bi);
    const mach_header*  loadedAddress() const   { return (mach_header*)((uintptr_t)_loadAddress & ~0x7ULL); }
    State               state() const           { return (State)((uintptr_t)_loadAddress & 0x3ULL); }
    const BinaryImage*  image() const           { return _image; }
    bool                neverUnload() const     { return ((uintptr_t)_loadAddress & 0x4ULL); }
    void                setState(State s)       { _loadAddress = (mach_header*)((((uintptr_t)_loadAddress) & ~0x3ULL) | (uintptr_t)s); }
    void                setNeverUnload()        { _loadAddress = (mach_header*)(((uintptr_t)_loadAddress) | 0x4ULL); }

private:
    const mach_header*  _loadAddress; // low bits: bit2=neverUnload, bit1/bit0 contain State
    const BinaryImage*  _image;
};


bool LoadedImage::operator==(const LoadedImage& rhs) const
{
    return (_image == rhs._image) && (loadedAddress() == rhs.loadedAddress());
}



struct VIS_HIDDEN DlopenCount {
    bool                operator==(const DlopenCount& rhs) const;
    const mach_header*  loadAddress;
    uintptr_t           refCount;
};

bool DlopenCount::operator==(const DlopenCount& rhs) const
{
    return (loadAddress == rhs.loadAddress) && (refCount == rhs.refCount);
}

LoadedImage::LoadedImage(const mach_header* mh, const BinaryImage* bi)
    : _loadAddress(mh), _image(bi)
{
    assert(loadedAddress() == mh);
    setState(State::uninited);
}

void LoadedImage::init(const mach_header* mh, const BinaryImage* bi)
{
    _loadAddress = mh;
    _image = bi;
    assert(loadedAddress() == mh);
    setState(State::uninited);
}

// forward reference
template <typename T, int C> class ReaderWriterChunkedVector;

template <typename T, int C>
class VIS_HIDDEN ChunkedVector {
public:
    static ChunkedVector<T,C>*  make(uint32_t count);

    void                        forEach(uint32_t& startIndex, bool& outerStop, void (^callback)(uint32_t index, const T& value, bool& stop)) const;
    void                        forEach(uint32_t& startIndex, bool& outerStop, void (^callback)(uint32_t index, T& value, bool& stop));
    T*                          add(const T& value);
    T*                          add(uint32_t count, const T values[]);
    void                        remove(uint32_t index);
    uint32_t                    count() const { return _inUseCount; }
    uint32_t                    freeCount() const { return _allocCount - _inUseCount; }
private:
    T&                          element(uint32_t index) { return ((T*)_elements)[index]; }
    const T&                    element(uint32_t index) const { return ((T*)_elements)[index]; }

    friend class ReaderWriterChunkedVector<T,C>;

    ChunkedVector<T,C>*     _next           = nullptr;
    uint32_t                _allocCount     = C;
    uint32_t                _inUseCount     = 0;
    uint8_t                 _elements[C*sizeof(T)] = { 0 };
};

template <typename T, int C>
class VIS_HIDDEN ReaderWriterChunkedVector {
public:
    T*                  add(uint32_t count, const T values[]);
    T*                  add(const T& value) { return add(1, &value); }
    T*                  addNoLock(uint32_t count, const T values[]);
    T*                  addNoLock(const T& value) { return addNoLock(1, &value); }
    void                remove(const T& value);
    uint32_t            count() const;
    void                forEachWithReadLock(void (^callback)(uint32_t index, const T& value, bool& stop)) const;
    void                forEachWithWriteLock(void (^callback)(uint32_t index, T& value, bool& stop));
    void                forEachNoLock(void (^callback)(uint32_t index, const T& value, bool& stop)) const;
    T&                  operator[](size_t index);
    uint32_t            countNoLock() const;

    void                withReadLock(void (^withLock)()) const;
    void                withWriteLock(void (^withLock)()) const;
    void                acquireWriteLock();
    void                releaseWriteLock();
    void                dump(void (^callback)(const T& value)) const;

private:
    mutable pthread_rwlock_t    _lock           = PTHREAD_RWLOCK_INITIALIZER;
    ChunkedVector<T,C>          _firstChunk;
};


typedef void (*NotifyFunc)(const mach_header* mh, intptr_t slide);

static  ReaderWriterChunkedVector<NotifyFunc, 4>                                sLoadNotifiers;
static  ReaderWriterChunkedVector<NotifyFunc, 4>                                sUnloadNotifiers;
static  ReaderWriterChunkedVector<LoadedImage, 4>                               sLoadedImages;
static  ReaderWriterChunkedVector<DlopenCount, 4>                               sDlopenRefCounts;
static  ReaderWriterChunkedVector<const launch_cache::BinaryImageGroupData*, 4> sKnownGroups;
#if __MAC_OS_X_VERSION_MIN_REQUIRED
static     ReaderWriterChunkedVector<__NSObjectFileImage, 2>  sNSObjectFileImages;
#endif


/////////////////////  ChunkedVector ////////////////////////////

template <typename T, int C>
ChunkedVector<T,C>* ChunkedVector<T,C>::make(uint32_t count)
{
    size_t size = sizeof(ChunkedVector) + sizeof(T) * (count-C);
    ChunkedVector<T,C>* result = (ChunkedVector<T,C>*)malloc(size);
    result->_next       = nullptr;
    result->_allocCount = count;
    result->_inUseCount = 0;
    return result;
}

template <typename T, int C>
void ChunkedVector<T,C>::forEach(uint32_t& outerIndex, bool& outerStop, void (^callback)(uint32_t index, const T& value, bool& stop)) const
{
    for (uint32_t i=0; i < _inUseCount; ++i) {
        callback(outerIndex, element(i), outerStop);
        ++outerIndex;
        if ( outerStop )
            break;
    }
}

template <typename T, int C>
void ChunkedVector<T,C>::forEach(uint32_t& outerIndex, bool& outerStop, void (^callback)(uint32_t index, T& value, bool& stop))
{
    for (uint32_t i=0; i < _inUseCount; ++i) {
        callback(outerIndex, element(i), outerStop);
        ++outerIndex;
        if ( outerStop )
            break;
    }
}

template <typename T, int C>
T* ChunkedVector<T,C>::add(const T& value)
{
    return add(1, &value);
}

template <typename T, int C>
T* ChunkedVector<T,C>::add(uint32_t count, const T values[])
{
    assert(count <= (_allocCount - _inUseCount));
    T* result = &element(_inUseCount);
    memmove(result, values, sizeof(T)*count);
    _inUseCount += count;
    return result;
}

template <typename T, int C>
void ChunkedVector<T,C>::remove(uint32_t index)
{
    assert(index < _inUseCount);
    int moveCount = _inUseCount - index - 1;
    if ( moveCount >= 1 ) {
        memmove(&element(index), &element(index+1), sizeof(T)*moveCount);
    }
    _inUseCount--;
}


/////////////////////  ReaderWriterChunkedVector ////////////////////////////



template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::withReadLock(void (^work)()) const
{
    assert(pthread_rwlock_rdlock(&_lock) == 0);
    work();
    assert(pthread_rwlock_unlock(&_lock) == 0);
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::withWriteLock(void (^work)()) const
{
    assert(pthread_rwlock_wrlock(&_lock) == 0);
    work();
    assert(pthread_rwlock_unlock(&_lock) == 0);
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::acquireWriteLock()
{
    assert(pthread_rwlock_wrlock(&_lock) == 0);
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::releaseWriteLock()
{
    assert(pthread_rwlock_unlock(&_lock) == 0);
}

template <typename T, int C>
uint32_t ReaderWriterChunkedVector<T,C>::count() const
{
    __block uint32_t result = 0;
    withReadLock(^() {
        for (const ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
            result += chunk->count();
        }
    });
    return result;
}

template <typename T, int C>
uint32_t ReaderWriterChunkedVector<T,C>::countNoLock() const
{
    uint32_t result = 0;
    for (const ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
        result += chunk->count();
    }
    return result;
}

template <typename T, int C>
T* ReaderWriterChunkedVector<T,C>::addNoLock(uint32_t count, const T values[])
{
    T* result = nullptr;
    ChunkedVector<T,C>* lastChunk = &_firstChunk;
    while ( lastChunk->_next != nullptr )
        lastChunk = lastChunk->_next;

    if ( lastChunk->freeCount() >= count ) {
        // append to last chunk
        result = lastChunk->add(count, values);
    }
    else {
        // append new chunk
        uint32_t allocCount = count;
        uint32_t remainder = count % C;
        if ( remainder != 0 )
            allocCount = count + C - remainder;
        ChunkedVector<T,C>* newChunk = ChunkedVector<T,C>::make(allocCount);
        result = newChunk->add(count, values);
        lastChunk->_next = newChunk;
    }

    return result;
}

template <typename T, int C>
T* ReaderWriterChunkedVector<T,C>::add(uint32_t count, const T values[])
{
    __block T* result = nullptr;
    withWriteLock(^() {
        result = addNoLock(count, values);
    });
    return result;
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::remove(const T& valueToRemove)
{
    __block bool stopStorage = false;
    withWriteLock(^() {
        ChunkedVector<T,C>* chunkNowEmpty = nullptr;
        __block uint32_t indexStorage = 0;
        __block bool found = false;
        for (ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
            uint32_t chunkStartIndex = indexStorage;
            __block uint32_t foundIndex = 0;
            chunk->forEach(indexStorage, stopStorage, ^(uint32_t index, const T& value, bool& stop) {
                if ( value == valueToRemove ) {
                    foundIndex = index - chunkStartIndex;
                    found = true;
                    stop = true;
                }
            });
            if ( found ) {
                chunk->remove(foundIndex);
                found = false;
                if ( chunk->count() == 0 )
                    chunkNowEmpty = chunk;
            }
        }
        // if chunk is now empty, remove from linked list and free
        if ( chunkNowEmpty ) {
            for (ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
                if ( chunk->_next == chunkNowEmpty ) {
                    chunk->_next = chunkNowEmpty->_next;
                    if ( chunkNowEmpty != &_firstChunk )
                        free(chunkNowEmpty);
                    break;
                }
            }
        }
    });
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::forEachWithReadLock(void (^callback)(uint32_t index, const T& value, bool& stop)) const
{
    __block uint32_t index = 0;
    __block bool stop = false;
    withReadLock(^() {
        for (const ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
            chunk->forEach(index, stop, callback);
            if ( stop )
                break;
        }
    });
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::forEachWithWriteLock(void (^callback)(uint32_t index, T& value, bool& stop))
{
    __block uint32_t index = 0;
    __block bool stop = false;
    withReadLock(^() {
        for (ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
            chunk->forEach(index, stop, callback);
            if ( stop )
                break;
        }
    });
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::forEachNoLock(void (^callback)(uint32_t index, const T& value, bool& stop)) const
{
    uint32_t index = 0;
    bool stop = false;
    for (const ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
        chunk->forEach(index, stop, callback);
        if ( stop )
            break;
    }
}

template <typename T, int C>
T& ReaderWriterChunkedVector<T,C>::operator[](size_t targetIndex)
{
    __block T* result = nullptr;
    forEachNoLock(^(uint32_t index, T const& value, bool& stop) {
        if ( index == targetIndex ) {
            result = (T*)&value;
            stop = true;
        }
    });
    return *result;
}

template <typename T, int C>
void ReaderWriterChunkedVector<T,C>::dump(void (^callback)(const T& value)) const
{
    log("dump ReaderWriterChunkedVector at %p\n", this);
    __block uint32_t index = 0;
    __block bool stop = false;
    withReadLock(^() {
        for (const ChunkedVector<T,C>* chunk = &_firstChunk; chunk != nullptr; chunk = chunk->_next) {
            log(" chunk at %p\n", chunk);
            chunk->forEach(index, stop, ^(uint32_t i, const T& value, bool& s) {
                callback(value);
            });
        }
    });
}



/////////////////////  AllImages ////////////////////////////


AllImages gAllImages;



void AllImages::init(const BinaryClosure* closure, const void* dyldCacheLoadAddress, const char* dyldCachePath,
                     const dyld3::launch_cache::DynArray<loader::ImageInfo>& initialImages)
{
    _mainClosure        = closure;
    _initialImages      = &initialImages;
    _dyldCacheAddress   = dyldCacheLoadAddress;
    _dyldCachePath      = dyldCachePath;

    if ( _dyldCacheAddress ) {
        const DyldSharedCache* cache = (DyldSharedCache*)_dyldCacheAddress;
        const dyld_cache_mapping_info* const fileMappings = (dyld_cache_mapping_info*)((uint64_t)_dyldCacheAddress + cache->header.mappingOffset);
        _dyldCacheSlide     = (uint64_t)dyldCacheLoadAddress - fileMappings[0].address;
    }

    // Make temporary old image array, so libSystem initializers can be debugged
    uint32_t count = (uint32_t)initialImages.count();
    dyld_image_info oldDyldInfo[count];
    for (int i=0; i < count; ++i) {
        launch_cache::Image img(initialImages[i].imageData);
        oldDyldInfo[i].imageLoadAddress = initialImages[i].loadAddress;
        oldDyldInfo[i].imageFilePath    = img.path();
        oldDyldInfo[i].imageFileModDate = 0;
    }
    _oldAllImageInfos->infoArray        = oldDyldInfo;
    _oldAllImageInfos->infoArrayCount   = count;
    _oldAllImageInfos->notification(dyld_image_adding, count, oldDyldInfo);
    _oldAllImageInfos->infoArray        = nullptr;
    _oldAllImageInfos->infoArrayCount   = 0;
}

void AllImages::setProgramVars(ProgramVars* vars)
{
    _programVars = vars;
}

void AllImages::applyInitialImages()
{
    addImages(*_initialImages);
    _initialImages = nullptr;  // this was stack allocated
}

void AllImages::mirrorToOldAllImageInfos()
{
   // set infoArray to NULL to denote it is in-use
    _oldAllImageInfos->infoArray = nullptr;

    // if array not large enough, re-alloc it
    uint32_t imageCount = sLoadedImages.countNoLock();
    if ( _oldArrayAllocCount < imageCount ) {
        uint32_t newAllocCount    = imageCount + 16;
        dyld_image_info* newArray = (dyld_image_info*)malloc(sizeof(dyld_image_info)*newAllocCount);
        if ( _oldAllImageArray != nullptr ) {
            memcpy(newArray, _oldAllImageArray, sizeof(dyld_image_info)*_oldAllImageInfos->infoArrayCount);
            free(_oldAllImageArray);
        }
        _oldAllImageArray   = newArray;
        _oldArrayAllocCount = newAllocCount;
    }

    // fill out array to mirror current image list
    sLoadedImages.forEachNoLock(^(uint32_t index, const LoadedImage& loadedImage, bool& stop) {
        launch_cache::Image img(loadedImage.image());
        _oldAllImageArray[index].imageLoadAddress = loadedImage.loadedAddress();
        _oldAllImageArray[index].imageFilePath    = imagePath(loadedImage.image());
        _oldAllImageArray[index].imageFileModDate = 0;
    });

    // set infoArray back to base address of array (so other process can now read)
    _oldAllImageInfos->infoArrayCount           = imageCount;
    _oldAllImageInfos->infoArrayChangeTimestamp = mach_absolute_time();
    _oldAllImageInfos->infoArray                = _oldAllImageArray;
}

void AllImages::addImages(const launch_cache::DynArray<loader::ImageInfo>& newImages)
{
    uint32_t count = (uint32_t)newImages.count();
    assert(count != 0);

    // build stack array of LoadedImage to copy into sLoadedImages
    STACK_ALLOC_DYNARRAY(LoadedImage, count, loadedImagesArray);
    for (uint32_t i=0; i < count; ++i) {
        loadedImagesArray[i].init(newImages[i].loadAddress, newImages[i].imageData);
        if (newImages[i].neverUnload)
            loadedImagesArray[i].setNeverUnload();
    }
    sLoadedImages.add(count, &loadedImagesArray[0]);

    if ( _oldAllImageInfos != nullptr ) {
        // sync to old all image infos struct
        if ( _initialImages != nullptr ) {
            // libSystem not initialized yet, don't use locks
            mirrorToOldAllImageInfos();
        }
        else {
            sLoadedImages.withReadLock(^{
                mirrorToOldAllImageInfos();
            });
        }

        // tell debugger about new images
        dyld_image_info oldDyldInfo[count];
        for (int i=0; i < count; ++i) {
            launch_cache::Image img(newImages[i].imageData);
            oldDyldInfo[i].imageLoadAddress = newImages[i].loadAddress;
            oldDyldInfo[i].imageFilePath    = imagePath(newImages[i].imageData);
            oldDyldInfo[i].imageFileModDate = 0;
        }
        _oldAllImageInfos->notification(dyld_image_adding, count, oldDyldInfo);
    }

    // log loads
    for (int i=0; i < count; ++i) {
        launch_cache::Image img(newImages[i].imageData);
        log_loads("dyld: %s\n", imagePath(newImages[i].imageData));
    }

#if !TARGET_IPHONE_SIMULATOR
    // call kdebug trace for each image
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A))) {
        for (uint32_t i=0; i < count; ++i) {
            launch_cache::Image img(newImages[i].imageData);
            struct stat stat_buf;
            fsid_t fsid = {{ 0, 0 }};
            fsobj_id_t fsobjid = { 0, 0 };
            if (img.isDiskImage() && stat(imagePath(newImages[i].imageData), &stat_buf) == 0 ) {
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid = {{ stat_buf.st_dev, 0 }};
            }
            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, img.uuid(), fsobjid, fsid, newImages[i].loadAddress);
        }
    }
#endif
    // call each _dyld_register_func_for_add_image function with each image
    const uint32_t  existingNotifierCount = sLoadNotifiers.count();
    NotifyFunc      existingNotifiers[existingNotifierCount];
    NotifyFunc*     existingNotifierArray = existingNotifiers;
    sLoadNotifiers.forEachWithReadLock(^(uint32_t index, const NotifyFunc& func, bool& stop) {
        if ( index < existingNotifierCount )
            existingNotifierArray[index] = func;
    });
    // we don't want to hold lock while calling out, so prebuild array (with lock) then do calls on that array (without lock)
    for (uint32_t j=0; j < existingNotifierCount; ++j) {
        NotifyFunc func = existingNotifierArray[j];
        for (uint32_t i=0; i < count; ++i) {
            log_notifications("dyld: add notifier %p called with mh=%p\n", func, newImages[i].loadAddress);
            if (newImages[i].justUsedFromDyldCache) {
                func(newImages[i].loadAddress, _dyldCacheSlide);
            } else {
                MachOParser parser(newImages[i].loadAddress);
                func(newImages[i].loadAddress, parser.getSlide());
            }
        }
    }

    // call objc about images that use objc
    if ( _objcNotifyMapped != nullptr ) {
        const char*         pathsBuffer[count];
        const mach_header*  mhBuffer[count];
        uint32_t            imagesWithObjC = 0;
        for (uint32_t i=0; i < count; ++i) {
            launch_cache::Image img(newImages[i].imageData);
            if ( img.hasObjC() ) {
                pathsBuffer[imagesWithObjC] = imagePath(newImages[i].imageData);
                mhBuffer[imagesWithObjC]    = newImages[i].loadAddress;
               ++imagesWithObjC;
            }
        }
        if ( imagesWithObjC != 0 ) {
            (*_objcNotifyMapped)(imagesWithObjC, pathsBuffer, mhBuffer);
            if ( log_notifications("dyld: objc-mapped-notifier called with %d images:\n", imagesWithObjC) ) {
                for (uint32_t i=0; i < imagesWithObjC; ++i) {
                    log_notifications("dyld:  objc-mapped: %p %s\n",  mhBuffer[i], pathsBuffer[i]);
                }
            }
        }
    }

    // notify any processes tracking loads in this process
    notifyMonitorLoads(newImages);
}

void AllImages::removeImages(const launch_cache::DynArray<loader::ImageInfo>& unloadImages)
{
    uint32_t count = (uint32_t)unloadImages.count();
    assert(count != 0);

    // call each _dyld_register_func_for_remove_image function with each image
    // do this before removing image from internal data structures so that the callback can query dyld about the image
    const uint32_t  existingNotifierCount = sUnloadNotifiers.count();
    NotifyFunc      existingNotifiers[existingNotifierCount];
    NotifyFunc*     existingNotifierArray = existingNotifiers;
    sUnloadNotifiers.forEachWithReadLock(^(uint32_t index, const NotifyFunc& func, bool& stop) {
        if ( index < existingNotifierCount )
            existingNotifierArray[index] = func;
    });
    // we don't want to hold lock while calling out, so prebuild array (with lock) then do calls on that array (without lock)
    for (uint32_t j=0; j < existingNotifierCount; ++j) {
        NotifyFunc func = existingNotifierArray[j];
        for (uint32_t i=0; i < count; ++i) {
            MachOParser parser(unloadImages[i].loadAddress);
            log_notifications("dyld: remove notifier %p called with mh=%p\n", func, unloadImages[i].loadAddress);
            func(unloadImages[i].loadAddress, parser.getSlide());
        }
    }

    // call objc about images going away
    if ( _objcNotifyUnmapped != nullptr ) {
        for (uint32_t i=0; i < count; ++i) {
            launch_cache::Image img(unloadImages[i].imageData);
            if ( img.hasObjC() ) {
                (*_objcNotifyUnmapped)(imagePath(unloadImages[i].imageData), unloadImages[i].loadAddress);
                log_notifications("dyld: objc-unmapped-notifier called with image %p %s\n", unloadImages[i].loadAddress, imagePath(unloadImages[i].imageData));
            }
        }
    }

#if !TARGET_IPHONE_SIMULATOR
    // call kdebug trace for each image
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A))) {
        for (uint32_t i=0; i < count; ++i) {
            launch_cache::Image img(unloadImages[i].imageData);
            struct stat stat_buf;
            fsid_t fsid = {{ 0, 0 }};
            fsobj_id_t fsobjid = { 0, 0 };
            if (stat(imagePath(unloadImages[i].imageData), &stat_buf) == 0 ) {
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid = {{ stat_buf.st_dev, 0 }};
            }
            kdebug_trace_dyld_image(DBG_DYLD_UUID_UNMAP_A, img.uuid(), fsobjid, fsid, unloadImages[i].loadAddress);
        }
    }
#endif

    // remove each from sLoadedImages
    for (uint32_t i=0; i < count; ++i) {
        LoadedImage info(unloadImages[i].loadAddress, unloadImages[i].imageData);
        sLoadedImages.remove(info);
    }

    // sync to old all image infos struct
    sLoadedImages.withReadLock(^{
        mirrorToOldAllImageInfos();
    });

    // tell debugger about removed images
    dyld_image_info oldDyldInfo[count];
    for (int i=0; i < count; ++i) {
        launch_cache::Image img(unloadImages[i].imageData);
        oldDyldInfo[i].imageLoadAddress = unloadImages[i].loadAddress;
        oldDyldInfo[i].imageFilePath    = imagePath(unloadImages[i].imageData);
        oldDyldInfo[i].imageFileModDate = 0;
    }
    _oldAllImageInfos->notification(dyld_image_removing, count, oldDyldInfo);

    // unmap images
    for (int i=0; i < count; ++i) {
        launch_cache::Image img(unloadImages[i].imageData);
        loader::unmapImage(unloadImages[i].imageData, unloadImages[i].loadAddress);
        log_loads("dyld: unloaded %s\n", imagePath(unloadImages[i].imageData));
    }

    // notify any processes tracking loads in this process
    notifyMonitorUnloads(unloadImages);
}

void AllImages::setNeverUnload(const loader::ImageInfo& existingImage)
{
    sLoadedImages.forEachWithWriteLock(^(uint32_t index, dyld3::LoadedImage &value, bool &stop) {
        if (value.image() == existingImage.imageData) {
            value.setNeverUnload();
            stop = true;
        }
    });
}

uint32_t AllImages::count() const
{
    return sLoadedImages.count();
}


launch_cache::Image AllImages::findByLoadOrder(uint32_t index, const mach_header** loadAddress) const
{
    __block const BinaryImage* foundImage = nullptr;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( anIndex == index ) {
            foundImage   = loadedImage.image();
            *loadAddress = loadedImage.loadedAddress();
            stop = true;
        }
    });
    return launch_cache::Image(foundImage);
}

launch_cache::Image AllImages::findByLoadAddress(const mach_header* loadAddress) const
{
    __block const BinaryImage* foundImage = nullptr;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( loadedImage.loadedAddress() == loadAddress ) {
            foundImage = loadedImage.image();
            stop = true;
        }
    });
    return launch_cache::Image(foundImage);
}

bool AllImages::findIndexForLoadAddress(const mach_header* loadAddress, uint32_t& index)
{
    __block bool result = false;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( loadedImage.loadedAddress() == loadAddress ) {
            index = anIndex;
            result = true;
            stop = true;
        }
    });
    return result;
}

void AllImages::forEachImage(void (^handler)(uint32_t imageIndex, const mach_header* loadAddress, const launch_cache::Image image, bool& stop)) const
{
	sLoadedImages.forEachWithReadLock(^(uint32_t imageIndex, const LoadedImage& loadedImage, bool& stop) {
       handler(imageIndex, loadedImage.loadedAddress(), launch_cache::Image(loadedImage.image()), stop);
    });
}

launch_cache::Image AllImages::findByOwnedAddress(const void* addr, const mach_header** loadAddress, uint8_t* permissions) const
{
    if ( _initialImages != nullptr ) {
        // being called during libSystem initialization, so sLoadedImages not allocated yet
        for (int i=0; i < _initialImages->count(); ++i) {
            const loader::ImageInfo& entry = (*_initialImages)[i];
            launch_cache::Image anImage(entry.imageData);
            if ( anImage.containsAddress(addr, entry.loadAddress, permissions) ) {
                *loadAddress = entry.loadAddress;
                return entry.imageData;
            }
        }
        return launch_cache::Image(nullptr);
    }

    // if address is in cache, do fast search of cache
    if ( (_dyldCacheAddress != nullptr) && (addr > _dyldCacheAddress) ) {
        const DyldSharedCache* dyldCache = (DyldSharedCache*)_dyldCacheAddress;
        if ( addr < (void*)((uint8_t*)_dyldCacheAddress+dyldCache->mappedSize()) ) {
            size_t cacheVmOffset = ((uint8_t*)addr - (uint8_t*)_dyldCacheAddress);
            DyldCacheParser cacheParser(dyldCache, false);
            launch_cache::ImageGroup cachedDylibsGroup(cacheParser.cachedDylibsGroup());
            uint32_t mhCacheOffset;
            uint8_t  foundPermissions;
            launch_cache::Image image(cachedDylibsGroup.findImageByCacheOffset(cacheVmOffset, mhCacheOffset, foundPermissions));
            if ( image.valid() ) {
                *loadAddress = (mach_header*)((uint8_t*)_dyldCacheAddress + mhCacheOffset);
                if ( permissions != nullptr )
                    *permissions = foundPermissions;
                return image;
            }
        }
    }

    __block const BinaryImage* foundImage = nullptr;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        launch_cache::Image anImage(loadedImage.image());
        if ( anImage.containsAddress(addr, loadedImage.loadedAddress(), permissions) ) {
            *loadAddress = loadedImage.loadedAddress();
            foundImage   = loadedImage.image();
            stop = true;
        }
    });
    return launch_cache::Image(foundImage);
}

const mach_header* AllImages::findLoadAddressByImage(const BinaryImage* targetImage) const
{
    __block const mach_header* foundAddress = nullptr;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( targetImage == loadedImage.image() ) {
            foundAddress = loadedImage.loadedAddress();
            stop = true;
        }
    });
    return foundAddress;
}

const mach_header* AllImages::mainExecutable() const
{
    assert(_programVars != nullptr);
    return (const mach_header*)_programVars->mh;
}

launch_cache::Image AllImages::mainExecutableImage() const
{
    assert(_mainClosure != nullptr);
    const launch_cache::Closure            mainClosure(_mainClosure);
    const dyld3::launch_cache::ImageGroup  mainGroup           = mainClosure.group();
    const uint32_t                         mainExecutableIndex = mainClosure.mainExecutableImageIndex();
    const dyld3::launch_cache::Image       mainImage           = mainGroup.image(mainExecutableIndex);
    return mainImage;
}

void AllImages::setMainPath(const char* path )
{
    _mainExeOverridePath = path;
}

const char* AllImages::imagePath(const BinaryImage* binImage) const
{
#if __IPHONE_OS_VERSION_MIN_REQUIRED
    // on iOS and watchOS, apps may be moved on device after closure built
	if ( _mainExeOverridePath != nullptr ) {
        if ( binImage == mainExecutableImage().binaryData() )
            return _mainExeOverridePath;
    }
#endif
    launch_cache::Image image(binImage);
    return image.path();
}

void AllImages::setInitialGroups()
{
    DyldCacheParser cacheParser((DyldSharedCache*)_dyldCacheAddress, false);
    sKnownGroups.addNoLock(cacheParser.cachedDylibsGroup());
    sKnownGroups.addNoLock(cacheParser.otherDylibsGroup());
    launch_cache::Closure closure(_mainClosure);
    sKnownGroups.addNoLock(closure.group().binaryData());
}

const launch_cache::binary_format::ImageGroup* AllImages::cachedDylibsGroup()
{
    return sKnownGroups[0];
}

const launch_cache::binary_format::ImageGroup* AllImages::otherDylibsGroup()
{
    return sKnownGroups[1];
}

const AllImages::BinaryImageGroup* AllImages::mainClosureGroup()
{
    return sKnownGroups[2];
}

uint32_t AllImages::currentGroupsCount() const
{
    return sKnownGroups.count();
}

void AllImages::copyCurrentGroups(ImageGroupList& groups) const
{
    sKnownGroups.forEachWithReadLock(^(uint32_t index, const dyld3::launch_cache::binary_format::ImageGroup* const &grpData, bool &stop) {
        if ( index < groups.count() )
            groups[index] = grpData;
    });
}

void AllImages::copyCurrentGroupsNoLock(ImageGroupList& groups) const
{
    sKnownGroups.forEachNoLock(^(uint32_t index, const dyld3::launch_cache::binary_format::ImageGroup* const &grpData, bool &stop) {
        if ( index < groups.count() )
            groups[index] = grpData;
    });
}

const mach_header* AllImages::alreadyLoaded(uint64_t inode, uint64_t mtime, bool bumpRefCount)
{
    __block const mach_header* result = nullptr;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        launch_cache::Image img(loadedImage.image());
        if ( img.validateUsingModTimeAndInode() ) {
            if ( (img.fileINode() == inode) && (img.fileModTime() == mtime) ) {
                result = loadedImage.loadedAddress();
                if ( bumpRefCount && !loadedImage.neverUnload() )
                    incRefCount(loadedImage.loadedAddress());
                stop = true;
            }
        }
    });
    return result;
}

const mach_header* AllImages::alreadyLoaded(const char* path, bool bumpRefCount)
{
    __block const mach_header* result = nullptr;
    uint32_t targetHash = launch_cache::ImageGroup::hashFunction(path);
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        launch_cache::Image img(loadedImage.image());
        if ( (img.pathHash() == targetHash) && (strcmp(path, imagePath(loadedImage.image())) == 0) ) {
            result = loadedImage.loadedAddress();
            if ( bumpRefCount && !loadedImage.neverUnload() )
                incRefCount(loadedImage.loadedAddress());
            stop = true;
        }
    });
    if ( result == nullptr ) {
        // perhaps there was an image override
        launch_cache::ImageGroup mainGroup(mainClosureGroup());
        STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, currentGroupsCount(), currentGroupsList);
        copyCurrentGroups(currentGroupsList);
        mainGroup.forEachImageRefOverride(currentGroupsList, ^(launch_cache::Image standardDylib, launch_cache::Image overrideDyilb, bool& stop) {
            if ( strcmp(standardDylib.path(), path) == 0 ) {
                result = alreadyLoaded(overrideDyilb.path(), bumpRefCount);
                stop = true;
            }
        });
    }
    return result;
}

const mach_header* AllImages::alreadyLoaded(const BinaryImage* binImage, bool bumpRefCount)
{
    const mach_header* result = findLoadAddressByImage(binImage);
    if ( result != nullptr ) {
        launch_cache::Image loadedImage(binImage);
        if ( bumpRefCount && !loadedImage.neverUnload() )
            incRefCount(result);
    }
    return result;
}

void AllImages::incRefCount(const mach_header* loadAddress)
{
    __block bool found = false;
    sDlopenRefCounts.forEachWithWriteLock(^(uint32_t index, DlopenCount& entry, bool& stop) {
        if ( entry.loadAddress == loadAddress ) {
            found = true;
            entry.refCount += 1;
            stop = true;
        }
    });
    if ( !found ) {
        DlopenCount newEnty = { loadAddress, 1 };
        sDlopenRefCounts.add(newEnty);
    }
}

void AllImages::decRefCount(const mach_header* loadAddress)
{
    __block bool refCountNowZero = false;
    sDlopenRefCounts.forEachWithWriteLock(^(uint32_t index, DlopenCount& entry, bool& stop) {
        if ( entry.loadAddress == loadAddress ) {
            entry.refCount -= 1;
            stop = true;
            if ( entry.refCount == 0 )
                refCountNowZero = true;
        }
    });
    if ( refCountNowZero ) {
        DlopenCount delEnty = { loadAddress, 0 };
        sDlopenRefCounts.remove(delEnty);
        garbageCollectImages();
    }
}


#if __MAC_OS_X_VERSION_MIN_REQUIRED
__NSObjectFileImage* AllImages::addNSObjectFileImage()
{
    // look for empty slot first
    __block __NSObjectFileImage* result = nullptr;
    sNSObjectFileImages.forEachWithWriteLock(^(uint32_t index, __NSObjectFileImage& value, bool& stop) {
        if ( (value.path == nullptr) && (value.memSource == nullptr) ) {
            result = &value;
            stop = true;
        }
    });
    if ( result != nullptr )
        return result;

    // otherwise allocate new slot
    __NSObjectFileImage empty;
    return sNSObjectFileImages.add(empty);
}

bool AllImages::hasNSObjectFileImage(__NSObjectFileImage* ofi)
{
    __block bool result = false;
    sNSObjectFileImages.forEachNoLock(^(uint32_t index, const __NSObjectFileImage& value, bool& stop) {
        if ( &value == ofi ) {
            result = ((value.memSource != nullptr) || (value.path != nullptr));
            stop = true;
        }
    });
    return result;
}

void AllImages::removeNSObjectFileImage(__NSObjectFileImage* ofi)
{
    sNSObjectFileImages.forEachWithWriteLock(^(uint32_t index, __NSObjectFileImage& value, bool& stop) {
        if ( &value == ofi ) {
            // mark slot as empty
            ofi->path        = nullptr;
            ofi->memSource   = nullptr;
            ofi->memLength   = 0;
            ofi->loadAddress = nullptr;
            ofi->binImage    = nullptr;
            stop = true;
        }
    });
}
#endif


class VIS_HIDDEN Reaper
{
public:
                        Reaper(uint32_t count, const LoadedImage** unloadables, bool* inUseArray);
    void                garbageCollect();
    void                finalizeDeadImages();

private:
    typedef launch_cache::binary_format::Image      BinaryImage;

    void                markDirectlyDlopenedImagesAsUsed();
    void                markDependentOfInUseImages();
    void                markDependentsOf(const LoadedImage*);
    bool                loadAddressIsUnloadable(const mach_header* loadAddr, uint32_t& index);
    bool                imageIsUnloadable(const BinaryImage* binImage, uint32_t& foundIndex);
    uint32_t            inUseCount();
    void                dump(const char* msg);

    const LoadedImage** _unloadablesArray;
    bool*               _inUseArray;
    uint32_t            _arrayCount;
    uint32_t            _deadCount;
};

Reaper::Reaper(uint32_t count, const LoadedImage** unloadables, bool* inUseArray)
 : _unloadablesArray(unloadables), _inUseArray(inUseArray),_arrayCount(count)
{
}


bool Reaper::loadAddressIsUnloadable(const mach_header* loadAddr, uint32_t& foundIndex)
{
    for (uint32_t i=0; i < _arrayCount; ++i) {
        if ( _unloadablesArray[i]->loadedAddress() == loadAddr ) {
            foundIndex = i;
            return true;
        }
    }
    return false;
}

bool Reaper::imageIsUnloadable(const BinaryImage* binImage, uint32_t& foundIndex)
{
    for (uint32_t i=0; i < _arrayCount; ++i) {
        if ( _unloadablesArray[i]->image() == binImage ) {
            foundIndex = i;
            return true;
        }
    }
    return false;
}

void Reaper::markDirectlyDlopenedImagesAsUsed()
{
    sDlopenRefCounts.forEachWithReadLock(^(uint32_t refCountIndex, const dyld3::DlopenCount& dlEntry, bool& stop) {
        if ( dlEntry.refCount != 0 ) {
            uint32_t foundIndex;
            if ( loadAddressIsUnloadable(dlEntry.loadAddress, foundIndex) ) {
                _inUseArray[foundIndex] = true;
            }
         }
    });
}

uint32_t Reaper::inUseCount()
{
    uint32_t count = 0;
    for (uint32_t i=0; i < _arrayCount; ++i) {
        if ( _inUseArray[i] )
            ++count;
    }
    return count;
}

void Reaper::markDependentsOf(const LoadedImage* entry)
{
    const launch_cache::Image image(entry->image());
    STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, gAllImages.currentGroupsCount(), currentGroupsList);
    gAllImages.copyCurrentGroups(currentGroupsList);
    image.forEachDependentImage(currentGroupsList, ^(uint32_t depIndex, dyld3::launch_cache::Image depImage, dyld3::launch_cache::Image::LinkKind kind, bool& stop) {
        uint32_t foundIndex;
        if ( !depImage.neverUnload() && imageIsUnloadable(depImage.binaryData(), foundIndex) ) {
            _inUseArray[foundIndex] = true;
        }
    });
}

void Reaper::markDependentOfInUseImages()
{
    for (uint32_t i=0; i < _arrayCount; ++i) {
        if ( _inUseArray[i] )
            markDependentsOf(_unloadablesArray[i]);
    }
}

void Reaper::dump(const char* msg)
{
    //log("%s:\n", msg);
    for (uint32_t i=0; i < _arrayCount; ++i) {
        dyld3::launch_cache::Image image(_unloadablesArray[i]->image());
        //log("  in-used=%d  %s\n", _inUseArray[i], image.path());
    }
}

void Reaper::garbageCollect()
{
    //dump("all unloadable images");

    // mark all dylibs directly dlopen'ed as in use
    markDirectlyDlopenedImagesAsUsed();

    //dump("directly dlopen()'ed marked");

    // iteratively mark dependents of in-use dylibs as in-use until in-use count stops changing
    uint32_t lastCount = inUseCount();
    bool countChanged = false;
    do {
        markDependentOfInUseImages();
        //dump("dependents marked");
        uint32_t newCount = inUseCount();
        countChanged = (newCount != lastCount);
        lastCount = newCount;
    } while (countChanged);

    _deadCount = _arrayCount - inUseCount();
}

void Reaper::finalizeDeadImages()
{
    if ( _deadCount == 0 )
        return;
    __cxa_range_t ranges[_deadCount];
    __cxa_range_t* rangesArray = ranges;
    __block unsigned int rangesCount = 0;
    for (uint32_t i=0; i < _arrayCount; ++i) {
        if ( _inUseArray[i] )
            continue;
        dyld3::launch_cache::Image image(_unloadablesArray[i]->image());
        image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &stop) {
            if ( permissions & VM_PROT_EXECUTE ) {
                rangesArray[rangesCount].addr   = (char*)(_unloadablesArray[i]->loadedAddress()) + vmOffset;
                rangesArray[rangesCount].length = (size_t)vmSize;
                ++rangesCount;
           }
        });
    }
    __cxa_finalize_ranges(ranges, rangesCount);
}


// This function is called at the end of dlclose() when the reference count goes to zero.
// The dylib being unloaded may have brought in other dependent dylibs when it was loaded.
// Those dependent dylibs need to be unloaded, but only if they are not referenced by
// something else.  We use a standard mark and sweep garbage collection.
//
// The tricky part is that when a dylib is unloaded it may have a termination function that
// can run and itself call dlclose() on yet another dylib.  The problem is that this
// sort of gabage collection is not re-entrant.  Instead a terminator's call to dlclose()
// which calls garbageCollectImages() will just set a flag to re-do the garbage collection
// when the current pass is done.
//
// Also note that this is done within the sLoadedImages writer lock, so any dlopen/dlclose
// on other threads are blocked while this garbage collections runs
//
void AllImages::garbageCollectImages()
{
    // if some other thread is currently GC'ing images, let other thread do the work
    int32_t newCount = OSAtomicIncrement32(&_gcCount);
    if ( newCount != 1 )
        return;

    do {
        const uint32_t      loadedImageCount                    = sLoadedImages.count();
        const LoadedImage*  unloadables[loadedImageCount];
        bool                unloadableInUse[loadedImageCount];
        const LoadedImage** unloadablesArray                    = unloadables;
        bool*               unloadableInUseArray                = unloadableInUse;
        __block uint32_t    unloadableCount                     = 0;
        // do GC with lock, so no other images can be added during GC
        sLoadedImages.withReadLock(^() {
            sLoadedImages.forEachNoLock(^(uint32_t index, const LoadedImage& entry, bool& stop) {
                const launch_cache::Image image(entry.image());
                if ( !image.neverUnload() && !entry.neverUnload() ) {
                    unloadablesArray[unloadableCount] = &entry;
                    unloadableInUseArray[unloadableCount] = false;
                    //log("unloadable[%d] %p %s\n", unloadableCount, entry.loadedAddress(), image.path());
                    ++unloadableCount;
                }
            });
            // make reaper object to do garbage collection and notifications
            Reaper reaper(unloadableCount, unloadablesArray, unloadableInUseArray);
            reaper.garbageCollect();

            // FIXME: we should sort dead images so higher level ones are terminated first

            // call cxa_finalize_ranges of dead images
            reaper.finalizeDeadImages();

            // FIXME: call static terminators of dead images

            // FIXME: DOF unregister
        });

        //log("sLoadedImages before GC removals:\n");
        //sLoadedImages.dump(^(const LoadedImage& entry) {
        //    const launch_cache::Image image(entry.image());
        //    log("   loadAddr=%p, path=%s\n", entry.loadedAddress(), image.path());
        //});

        // make copy of LoadedImages we want to remove
        // because unloadables[] points into ChunkVector we are shrinking
        uint32_t removalCount = 0;
        for (uint32_t i=0; i < unloadableCount; ++i) {
            if ( !unloadableInUse[i] )
                ++removalCount;
        }
        if ( removalCount > 0 ) {
            STACK_ALLOC_DYNARRAY(loader::ImageInfo, removalCount, unloadImages);
            uint32_t removalIndex = 0;
            for (uint32_t i=0; i < unloadableCount; ++i) {
                if ( !unloadableInUse[i] ) {
                    unloadImages[removalIndex].loadAddress = unloadables[i]->loadedAddress();
                    unloadImages[removalIndex].imageData   = unloadables[i]->image();
                    ++removalIndex;
                }
            }
            // remove entries from sLoadedImages
            removeImages(unloadImages);

            //log("sLoadedImages after GC removals:\n");
            //sLoadedImages.dump(^(const LoadedImage& entry) {
            //    const launch_cache::Image image(entry.image());
            //    //log("   loadAddr=%p, path=%s\n", entry.loadedAddress(), image.path());
            //});
        }

        // if some other thread called GC during our work, redo GC on its behalf
        newCount = OSAtomicDecrement32(&_gcCount);
    }
    while (newCount > 0);
}



VIS_HIDDEN
const launch_cache::binary_format::Image* AllImages::messageClosured(const char* path, const char* apiName, const char* closuredErrorMessages[3], int& closuredErrorMessagesCount)
{
    __block const launch_cache::binary_format::Image* result = nullptr;
    sKnownGroups.withWriteLock(^() {
        ClosureBuffer::CacheIdent cacheIdent;
        bzero(&cacheIdent, sizeof(cacheIdent));
        if ( _dyldCacheAddress != nullptr ) {
            const DyldSharedCache* dyldCache = (DyldSharedCache*)_dyldCacheAddress;
            dyldCache->getUUID(cacheIdent.cacheUUID);
            cacheIdent.cacheAddress     = (unsigned long)_dyldCacheAddress;
            cacheIdent.cacheMappedSize  = dyldCache->mappedSize();
        }
        gPathOverrides.forEachPathVariant(path, ^(const char* possiblePath, bool& stopVariants) {
            struct stat statBuf;
            if ( stat(possiblePath, &statBuf) == 0 ) {
                if ( S_ISDIR(statBuf.st_mode) ) {
                    log_apis("   %s: path is directory: %s\n", apiName, possiblePath);
                    if ( closuredErrorMessagesCount < 3 )
                        closuredErrorMessages[closuredErrorMessagesCount++] = strdup("not a file");
                }
                else {
                    // file exists, ask closured to build info for it
                    STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, sKnownGroups.countNoLock(), currentGroupsList);
                    gAllImages.copyCurrentGroupsNoLock(currentGroupsList);
                    dyld3::launch_cache::DynArray<const dyld3::launch_cache::binary_format::ImageGroup*> nonCacheGroupList(currentGroupsList.count()-2, &currentGroupsList[2]);
                    const dyld3::launch_cache::binary_format::ImageGroup* closuredCreatedGroupData = nullptr;
                    ClosureBuffer closureBuilderInput(cacheIdent, path, nonCacheGroupList, gPathOverrides);
                    ClosureBuffer closureBuilderOutput = dyld3::closured_CreateImageGroup(closureBuilderInput);
                    if ( !closureBuilderOutput.isError() ) {
                        vm_protect(mach_task_self(), closureBuilderOutput.vmBuffer(), closureBuilderOutput.vmBufferSize(), false, VM_PROT_READ);
                        closuredCreatedGroupData = closureBuilderOutput.imageGroup();
                        log_apis("   %s: closured built ImageGroup for path: %s\n", apiName, possiblePath);
                        sKnownGroups.addNoLock(closuredCreatedGroupData);
                        launch_cache::ImageGroup group(closuredCreatedGroupData);
                        result = group.imageBinary(0);
                        stopVariants = true;
                    }
                    else {
                        log_apis("   %s: closured failed for path: %s, error: %s\n", apiName, possiblePath, closureBuilderOutput.errorMessage());
                        if ( closuredErrorMessagesCount < 3 ) {
                            closuredErrorMessages[closuredErrorMessagesCount++] = strdup(closureBuilderOutput.errorMessage());
                        }
                        closureBuilderOutput.free();
                    }
                }
            }
            else {
                log_apis("   %s: file does not exist for path: %s\n", apiName, possiblePath);
            }
        });
    });

    return result;
}

const AllImages::BinaryImage* AllImages::findImageInKnownGroups(const char* path)
{
    __block const AllImages::BinaryImage* result = nullptr;
    sKnownGroups.forEachWithReadLock(^(uint32_t index, const dyld3::launch_cache::binary_format::ImageGroup* const& grpData, bool& stop) {
        launch_cache::ImageGroup group(grpData);
        uint32_t ignore;
        if ( const AllImages::BinaryImage* binImage = group.findImageByPath(path, ignore) ) {
            result = binImage;
            stop = true;
        }
    });
    return result;
}

bool AllImages::imageUnloadable(const launch_cache::Image& image, const mach_header* loadAddress) const
{
    // check if statically determined in clousre that this can never be unloaded
    if ( image.neverUnload() )
        return false;

    // check if some runtime decision made this be never-unloadable
    __block bool foundAsNeverUnload = false;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( loadedImage.loadedAddress() == loadAddress ) {
            stop = true;
            if ( loadedImage.neverUnload() )
                foundAsNeverUnload = true;
        }
    });
    if ( foundAsNeverUnload )
        return false;

    return true;
}

void AllImages::addLoadNotifier(NotifyFunc func)
{
    // callback about already loaded images
    const uint32_t      existingCount = sLoadedImages.count();
    const mach_header*  existingMHs[existingCount];
    const mach_header** existingArray = existingMHs;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        if ( anIndex < existingCount )
            existingArray[anIndex] = loadedImage.loadedAddress();
    });
    // we don't want to hold lock while calling out, so prebuild array (with lock) then do calls on that array (without lock)
    for (uint32_t i=0; i < existingCount; i++) {
        MachOParser parser(existingArray[i]);
        log_notifications("dyld: add notifier %p called with mh=%p\n", func, existingArray[i]);
        func(existingArray[i], parser.getSlide());
    }

    // add to list of functions to call about future loads
    sLoadNotifiers.add(func);
}

void AllImages::addUnloadNotifier(NotifyFunc func)
{
    // add to list of functions to call about future unloads
    sUnloadNotifiers.add(func);
}

void AllImages::setObjCNotifiers(_dyld_objc_notify_mapped map, _dyld_objc_notify_init init, _dyld_objc_notify_unmapped unmap)
{
    _objcNotifyMapped   = map;
    _objcNotifyInit     = init;
    _objcNotifyUnmapped = unmap;

    // callback about already loaded images
    uint32_t                    maxCount = count();
    const char*                 pathsBuffer[maxCount];
    const mach_header*          mhBuffer[maxCount];
    __block const char**        paths = pathsBuffer;
    __block const mach_header** mhs = mhBuffer;
    __block uint32_t            imagesWithObjC = 0;
    sLoadedImages.forEachWithReadLock(^(uint32_t anIndex, const LoadedImage& loadedImage, bool& stop) {
        launch_cache::Image img(loadedImage.image());
        if ( img.hasObjC() ) {
            mhs[imagesWithObjC]   = loadedImage.loadedAddress();
            paths[imagesWithObjC] = imagePath(loadedImage.image());
            ++imagesWithObjC;
       }
    });
    if ( imagesWithObjC != 0 ) {
        (*map)(imagesWithObjC, pathsBuffer, mhBuffer);
        if ( log_notifications("dyld: objc-mapped-notifier called with %d images:\n", imagesWithObjC) ) {
            for (uint32_t i=0; i < imagesWithObjC; ++i) {
                log_notifications("dyld:  objc-mapped: %p %s\n",  mhBuffer[i], pathsBuffer[i]);
            }
        }
    }
}

void AllImages::vmAccountingSetSuspended(bool suspend)
{
#if __arm__ || __arm64__
    // <rdar://problem/29099600> dyld should tell the kernel when it is doing fix-ups caused by roots
    log_fixups("vm.footprint_suspend=%d\n", suspend);
    int newValue = suspend ? 1 : 0;
    int oldValue = 0;
    size_t newlen = sizeof(newValue);
    size_t oldlen = sizeof(oldValue);
    sysctlbyname("vm.footprint_suspend", &oldValue, &oldlen, &newValue, newlen);
#endif
}

void AllImages::applyInterposingToDyldCache(const launch_cache::binary_format::Closure* closure, const dyld3::launch_cache::DynArray<loader::ImageInfo>& initialImages)
{
    launch_cache::Closure    mainClosure(closure);
    launch_cache::ImageGroup mainGroup = mainClosure.group();
    DyldCacheParser cacheParser((DyldSharedCache*)_dyldCacheAddress, false);
    const launch_cache::binary_format::ImageGroup* dylibsGroupData = cacheParser.cachedDylibsGroup();
    launch_cache::ImageGroup dyldCacheDylibGroup(dylibsGroupData);
    __block bool suspendedAccounting = false;
    mainGroup.forEachDyldCacheSymbolOverride(^(uint32_t patchTableIndex, const launch_cache::binary_format::Image* imageData, uint32_t imageOffset, bool& stop) {
        bool foundInImages = false;
        for (int i=0; i < initialImages.count(); ++i) {
            if ( initialImages[i].imageData == imageData ) {
                foundInImages = true;
                uintptr_t replacement = (uintptr_t)(initialImages[i].loadAddress) + imageOffset;
                dyldCacheDylibGroup.forEachDyldCachePatchLocation(_dyldCacheAddress, patchTableIndex, ^(uintptr_t* locationToPatch, uintptr_t addend, bool& innerStop) {
                    if ( !suspendedAccounting ) {
                        vmAccountingSetSuspended(true);
                        suspendedAccounting = true;
                    }
                    log_fixups("dyld: cache fixup: *%p = %p\n", locationToPatch, (void*)replacement);
                    *locationToPatch = replacement + addend;
                });
                break;
            }
        }
        if ( !foundInImages ) {
            launch_cache::Image img(imageData);
            log_fixups("did not find loaded image to patch into cache: %s\n", img.path());
        }
    });
    if ( suspendedAccounting )
        vmAccountingSetSuspended(false);
}

void AllImages::runLibSystemInitializer(const mach_header* libSystemAddress, const launch_cache::binary_format::Image* libSystemBinImage)
{
    // run all initializers in image
    launch_cache::Image libSystemImage(libSystemBinImage);
    libSystemImage.forEachInitializer(libSystemAddress, ^(const void* func) {
        Initializer initFunc = (Initializer)func;
        dyld3::kdebug_trace_dyld_duration(DBG_DYLD_TIMING_STATIC_INITIALIZER, (uint64_t)func, 0, ^{
            initFunc(NXArgc, NXArgv, environ, appleParams, _programVars);
        });
        log_initializers("called initialzer %p in %s\n", initFunc, libSystemImage.path());
    });

    // mark libSystem.dylib as being init, so later recursive-init would re-run it
    sLoadedImages.forEachWithWriteLock(^(uint32_t anIndex, LoadedImage& loadedImage, bool& stop) {
        if ( loadedImage.loadedAddress() == libSystemAddress ) {
            loadedImage.setState(LoadedImage::State::inited);
            stop = true;
        }
    });
}

void AllImages::runInitialzersBottomUp(const mach_header* imageLoadAddress)
{
    launch_cache::Image topImage = findByLoadAddress(imageLoadAddress);
    if ( topImage.isInvalid() )
        return;

    // closure contains list of intializers to run in-order
    STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, currentGroupsCount(), currentGroupsList);
    copyCurrentGroups(currentGroupsList);
    topImage.forEachInitBefore(currentGroupsList, ^(launch_cache::Image imageToInit) {
        // find entry
        __block LoadedImage* foundEntry = nullptr;
        sLoadedImages.forEachWithReadLock(^(uint32_t index, const LoadedImage& entry, bool& stop) {
            if ( entry.image() == imageToInit.binaryData() ) {
                foundEntry = (LoadedImage*)&entry;
                stop = true;
            }
        });
        assert(foundEntry != nullptr);
        pthread_mutex_lock(&_initializerLock);
            // Note, due to the large lock in dlopen, we can't be waiting on another thread
            // here, but its possible that we are in a dlopen which is initialising us again
            if ( foundEntry->state() == LoadedImage::State::beingInited ) {
                log_initializers("dyld: already initializing '%s'\n", imagePath(imageToInit.binaryData()));
            }
            // at this point, the image is either initialized or not
            // if not, initialize it on this thread
            if ( foundEntry->state() == LoadedImage::State::uninited ) {
                foundEntry->setState(LoadedImage::State::beingInited);
                // release initializer lock, so other threads can run initializers
                pthread_mutex_unlock(&_initializerLock);
                // tell objc to run any +load methods in image
                if ( (_objcNotifyInit != nullptr) && imageToInit.mayHavePlusLoads() ) {
                    log_notifications("dyld: objc-init-notifier called with mh=%p, path=%s\n", foundEntry->loadedAddress(), imagePath(imageToInit.binaryData()));
                    (*_objcNotifyInit)(imagePath(imageToInit.binaryData()), foundEntry->loadedAddress());
                }
                // run all initializers in image
                imageToInit.forEachInitializer(foundEntry->loadedAddress(), ^(const void* func) {
                    Initializer initFunc = (Initializer)func;
                    dyld3::kdebug_trace_dyld_duration(DBG_DYLD_TIMING_STATIC_INITIALIZER, (uint64_t)func, 0, ^{
                        initFunc(NXArgc, NXArgv, environ, appleParams, _programVars);
                    });
                    log_initializers("dyld: called initialzer %p in %s\n", initFunc, imageToInit.path());
                });
                // reaquire initializer lock to switch state to inited
                pthread_mutex_lock(&_initializerLock);
                foundEntry->setState(LoadedImage::State::inited);
            }
        pthread_mutex_unlock(&_initializerLock);
    });
}


} // namespace dyld3






