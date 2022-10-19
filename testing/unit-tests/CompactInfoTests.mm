//
//  CompactInfoTests.m
//  CompactInfoTests
//
//  Created by Louis Gerbarg on 10/17/21.
//

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach/task.h>

#include "FileManager.h"
#include "ProcessAtlas.h"
//#include "CompactInfo.h"
#include "DyldRuntimeState.h"
#include "DyldProcessConfig.h"
#include "DyldAPIs.h"
#include "MockO.h"
#include "MachOFile.h"
#include "Vector.h"
#include "dyld_cache_format.h"


using dyld4::Atlas::ProcessSnapshot;
using dyld4::Atlas::Image;
using dyld4::FileRecord;
using dyld4::Atlas::SharedCache;

using lsl::Allocator;
using lsl::EphemeralAllocator;
using lsl::Vector;
using lsl::OrderedSet;
using lsl::UniquePtr;


#import "DyldTestCase.h"

@interface CompactInfoTests : DyldTestCase

@end

@implementation CompactInfoTests

- (void)testCompactInfoEncoder {
    SyscallDelegate syscalls;
    syscalls._bypassMockFS = true;
    TestState testState(syscalls, {}, {});
    auto allocator = EphemeralAllocator();
    auto fileManager = FileManager(allocator, &syscalls);
    ProcessSnapshot process(allocator, fileManager, false);
//    kern_return_t kr = KERN_SUCCESS;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    task_dyld_info_data_t task_dyld_info;
    XCTAssertEqual(task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count), KERN_SUCCESS);
    XCTAssertNotEqual(task_dyld_info.all_image_info_addr, MACH_VM_MIN_ADDRESS);
    auto all_image_infos = (dyld_all_image_infos*)task_dyld_info.all_image_info_addr;
    auto cacheFileRecord = fileManager.fileRecordForVolumeDevIDAndObjID(all_image_infos->sharedCacheFSID, all_image_infos->sharedCacheFSObjID);
    auto sharedCache = SharedCache(allocator, std::move(cacheFileRecord), process.identityMapper(), all_image_infos->sharedCacheBaseAddress, all_image_infos->processDetachedFromSharedRegion);
//    fprintf(stderr, "addSharedCache: %s\n", sharedCahceImage.file().getPath(testState.apis));
    process.addSharedCache(std::move(sharedCache));
    uint32_t imageCount = _dyld_image_count();
    __block OrderedSet<uint64_t> removableAddresses(allocator);

    for (auto i = 0; i < imageCount; ++i) {
        const struct mach_header* mh =  _dyld_get_image_header(i);
        if (mh->flags & MH_DYLIB_IN_CACHE) {
            process.addSharedCacheImage(mh);
        } else {
            uuid_t rawUUID;
            lsl::UUID machoUUID;
            if (((MachOFile*)mh)->getUuid(rawUUID)) {
                machoUUID = rawUUID;
            }
            const char* imagePath = _dyld_get_image_name(i);
            if (self.randomBool) {
                struct stat sb;
                XCTAssertNotEqual(stat(_dyld_get_image_name(i), &sb), -1);
                auto file = fileManager.fileRecordForStat(sb);
                auto image = Image(allocator, std::move(file), process.identityMapper(), mh);
                process.addImage(std::move(image));
                removableAddresses.insert((uint64_t)mh);
            } else {
                auto file = fileManager.fileRecordForPath(imagePath);
                auto image = Image(allocator, std::move(file), process.identityMapper(), mh);
                process.addImage(std::move(image));
                removableAddresses.insert((uint64_t)mh);
            }
        }
    }
//    process.dump();

    auto compactInfo = process.serialize();
    auto deserializedProcessSnapshot = ProcessSnapshot(allocator, fileManager, false, compactInfo);
//    deserializedProcessSnapshot.dump();

    __block bool foundDifferenceBetweenUnchanhgedInfos = false;
    deserializedProcessSnapshot.forEachImageNotIn(process, ^(Image *image) {
        foundDifferenceBetweenUnchanhgedInfos = true;
    });
    XCTAssertFalse(foundDifferenceBetweenUnchanhgedInfos);
    process.forEachImageNotIn(deserializedProcessSnapshot, ^(Image *image) {
        foundDifferenceBetweenUnchanhgedInfos = true;
    });
    XCTAssertFalse(foundDifferenceBetweenUnchanhgedInfos);

    // Remove a random number of removable images
    uint64_t removeCount = [self uniformRandomFrom:1 to:removableAddresses.size()];
    for (auto i = 0 ; i < removeCount; ++i) {
        // Choose which removable image to remove by selecting a random index
        uint64_t removeIdx = [self uniformRandomFrom:0 to:removableAddresses.size()-1];
        // Remove the element at idx;
        removableAddresses.erase(std::next(removableAddresses.begin(), removeIdx));
    }
    for (auto address : removableAddresses) {
        process.removeImageAtAddress(address);
    }

    __block bool foundRemovedImage = false;
    process.forEachImageNotIn(deserializedProcessSnapshot, ^(Image *image) {
        foundRemovedImage = true;
    });
    XCTAssertFalse(foundRemovedImage);
    deserializedProcessSnapshot.forEachImageNotIn(process, ^(Image *image) {
        // We should always be able to remove the image, if it is not an image we know about something is wrong.
        XCTAssertEqual(removableAddresses.erase(image->rebasedAddress()), 1);
    });
    XCTAssertTrue(removableAddresses.empty());
//    process.dump();
//    deserializedProcessSnapshot.dump();
}

@end
