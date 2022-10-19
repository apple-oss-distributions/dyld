/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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

#ifndef BuilderFileSystem_hpp
#define BuilderFileSystem_hpp

#include "ClosureFileSystem.h"
#include "mrm_shared_cache_builder.h"
#include "Diagnostics.h"
#include "FileUtils.h"

#include <map>
#include <string>
#include <vector>

namespace cache_builder
{

class SymlinkResolver {
public:
    SymlinkResolver() { }

    void addFile(Diagnostics& diags, std::string path);

    void addSymlink(Diagnostics& diags, std::string fromPath, std::string toPath);

    std::string realPath(Diagnostics& diags, const std::string& path,
                         void (^callback)(const std::string& intermediateSymlink) = nullptr) const;

    std::vector<cache_builder::FileAlias> getResolvedSymlinks(void (^callback)(const std::string& error)) const;

    std::vector<cache_builder::FileAlias> getIntermediateSymlinks() const;

private:
    std::set<std::string> filePaths;
    std::map<std::string, std::string> symlinks;
};

struct FileInfo {
    std::string     path;
    const uint8_t*  data;
    const uint64_t  length;
    FileFlags       flags;
    uint64_t        mtime;
    uint64_t        inode;
};

class FileSystemMRM : public dyld3::closure::FileSystem
{
public:
    FileSystemMRM() : FileSystem() { }

    bool getRealPath(const char possiblePath[MAXPATHLEN], char realPath[MAXPATHLEN]) const override;

    bool loadFile(const char* path, dyld3::closure::LoadedFileInfo& info,
                  char realerPath[MAXPATHLEN], void (^error)(const char* format, ...)) const override;

    void unloadFile(const dyld3::closure::LoadedFileInfo& info) const override;

    void unloadPartialFile(dyld3::closure::LoadedFileInfo& info,
                           uint64_t keepStartOffset, uint64_t keepLength) const override;

    bool fileExists(const char* path, uint64_t* inode=nullptr, uint64_t* mtime=nullptr,
                    bool* issetuid=nullptr, bool* inodesMatchRuntime = nullptr) const override;

    // MRM file APIs
    bool addFile(const char* path, uint8_t* data, uint64_t size, Diagnostics& diag, FileFlags fileFlags,
                 uint64_t inode, uint64_t modTime);

    bool addSymlink(const char* fromPath, const char* toPath, Diagnostics& diag);

    void forEachFileInfo(std::function<void(const char* path, const void* buffer, size_t bufferSize,
                                            FileFlags fileFlags, uint64_t inode, uint64_t modTime)> lambda);

    size_t fileCount() const;

    std::vector<cache_builder::FileAlias> getResolvedSymlinks(void (^callback)(const std::string& error)) const;

    std::vector<cache_builder::FileAlias> getIntermediateSymlinks() const;

private:
    std::vector<FileInfo> files;
    std::map<std::string, uint64_t> fileMap;
    SymlinkResolver symlinkResolver;
};

} // namespace cache_builder

#endif /* BuilderFileSystem_hpp */
