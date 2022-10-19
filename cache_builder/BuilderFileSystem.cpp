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

#include "BuilderFileSystem.h"
#include "BuilderOptions.h"

using namespace cache_builder;

//
// MARK: --- SymlinkResolver methods ---
//

static void normalizePath(std::string& path) {
    // Remove a bunch of stuff we don't need, like trailing slashes.
    while ( !path.empty() && (path.back() == '/'))
        path.pop_back();
}

void SymlinkResolver::addFile(Diagnostics& diags, std::string path) {
    if (path.front() != '/') {
        diags.error("Path must start with '/'");
        return;
    }
    if (symlinks.find(path) != symlinks.end()) {
        diags.error("Cannot add regular file as it is already a symlink");
        return;
    }
    filePaths.insert(path);
}

void SymlinkResolver::addSymlink(Diagnostics& diags, std::string fromPath, std::string toPath) {
    normalizePath(fromPath);
    normalizePath(toPath);
    if (fromPath.front() != '/') {
        diags.error("Path must start with '/'");
        return;
    }
    if (filePaths.find(fromPath) != filePaths.end()) {
        diags.error("Cannot add symlink from '%s' as it is already a regular path", fromPath.c_str());
        return;
    }
    auto itAndInserted = symlinks.insert({ fromPath, toPath });
    if (!itAndInserted.second) {
        // The path is already a symlink.  Make sure its a dupe.
        if (toPath != itAndInserted.first->second) {
            diags.error("Duplicate symlink for path '%s'", fromPath.c_str());
            return;
        }
    }
}

std::string SymlinkResolver::realPath(Diagnostics& diags, const std::string& originalPath,
                                      void (^callback)(const std::string& intermediateSymlink)) const {
    // First make sure the path doesn't have any magic in it.
    std::string path = originalPath;
    normalizePath(path);

    std::set<std::string> seenSymlinks;

    // Now see if any prefix is a symlink
    if (path.front() != '/')
        return path;

    std::string::size_type prev_pos = 0;
    while (prev_pos != std::string::npos) {
        std::string::size_type pos = path.find("/", prev_pos + 1);

        // First look to see if this path component is special, eg, ., .., etc.
        std::string component = path.substr(prev_pos, pos - prev_pos);
        if (component == "/..") {
            // Fold with the previous path component.
            if (prev_pos == 0) {
                // This is the root path, and .. applied to / is just /
                path = path.substr(3);
                prev_pos = 0;
            } else {
                std::string::size_type lastSlashPos = path.rfind("/", prev_pos - 1);
                path = path.substr(0, lastSlashPos) + path.substr(pos);
                prev_pos = lastSlashPos;
            }
            continue;
        } else if (component == "/.") {
            if (prev_pos == 0) {
                // Path starts with /./ so just remove the first one.
                path = path.substr(2);
            } else {
                if (pos == std::string::npos) {
                    // Trailing . on the path
                    path = path.substr(0, prev_pos );
                } else {
                    path = path.substr(0, prev_pos) + path.substr(pos);
                }
            }
            continue;
        } else if (component == "/") {
            // Path must contain // somewhere so strip out the duplicates.
            if (prev_pos == 0) {
                // Path starts with // so just remove the first one.
                path = path.substr(1);
            } else {
                if (pos == std::string::npos) {
                    // Trailing / on the path
                    path = path.substr(0, prev_pos);
                    prev_pos = pos;
                } else {
                    path = path.substr(0, pos) + path.substr(pos + 1);
                }
            }
            continue;
        }

        // Path is not special, so see if it is a symlink to something.
        std::string prefix = path.substr(0, pos);
        //printf("%s\n", prefix.c_str());
        auto it = symlinks.find(prefix);
        if (it == symlinks.end()) {
            // This is not a symlink so move to the next prefix.
            prev_pos = pos;
            continue;
        }

        // If we've already done this prefix then error out.
        if (seenSymlinks.count(prefix)) {
            diags.error("Loop in symlink processing for '%s'", originalPath.c_str());
            return std::string();
        }

        seenSymlinks.insert(prefix);

        // This is a symlink, so resolve the new path.
        std::string toPath = it->second;
        if (toPath.front() == '/') {
            // Symlink points to an absolute address so substitute the whole prefix for the new path
            // If we didn't substitute the last component of the path then there is also a path suffix.
            std::string pathSuffix = "";
            if (pos != std::string::npos) {
                std::string::size_type nextSlashPos = path.find("/", pos + 1);
                if (nextSlashPos != std::string::npos)
                    pathSuffix = path.substr(nextSlashPos);
            }
            path = toPath + pathSuffix;
            prev_pos = 0;
            continue;
        }

        // Symlink points to a relative path so we need to do more processing to get the real path.

        // First calculate which part of the previous prefix we'll keep.  Eg, in /a/b/c where "b -> blah", we want to keep /a here.
        std::string prevPrefix = path.substr(0, prev_pos);
        //printf("prevPrefix %s\n", prevPrefix.c_str());

        // If we didn't substitute the last component of the path then there is also a path suffix.
        std::string pathSuffix = "";
        if (prefix.size() != path.size())
            pathSuffix = path.substr(pos);

        // The new path is the remaining prefix, plus the symlink target, plus any remaining suffix from the original path.
        path = prevPrefix + "/" + toPath + pathSuffix;
        prev_pos = 0;
    }

    // Notify the caller if we found any intermediate symlinks
    if ( callback != nullptr ) {
        for ( const std::string& symlink : seenSymlinks ) {
            if ( symlink == originalPath )
                continue;
            // The intermediate symlink is hopefully a prefix on the final path
            // If so, then chop it up to get the symlink we could follow to the final path

            Diagnostics symlinkDiag;
            std::string resolvedSymlink = this->realPath(symlinkDiag, symlink);
            if ( symlinkDiag.hasError() )
                continue;

            if ( !path.starts_with(resolvedSymlink) )
                continue;

            // Now substitute the start of the path for the symlink
            std::string suffix = path.substr(resolvedSymlink.size());
            resolvedSymlink = symlink + suffix;

            // One last sanity check that we really are a valid symlink
            if ( this->realPath(symlinkDiag, resolvedSymlink).empty() )
                continue;
            if ( symlinkDiag.hasError() )
                continue;

            callback(resolvedSymlink);
        }
    }
    return path;
}

std::vector<cache_builder::FileAlias> SymlinkResolver::getResolvedSymlinks(void (^callback)(const std::string& error)) const
{
    std::vector<cache_builder::FileAlias> aliases;
    for (auto& fromPathAndToPath : symlinks) {
        Diagnostics diags;
        std::string newPath = realPath(diags, fromPathAndToPath.first);
        if (diags.hasError()) {
            callback(diags.errorMessage());
            continue;
        }

        if (filePaths.count(newPath)) {
            aliases.push_back({ newPath, fromPathAndToPath.first });
            // printf("symlink ('%s' -> '%s') resolved to '%s'\n", fromPathAndToPath.first.c_str(), fromPathAndToPath.second.c_str(), newPath.c_str());
        }
    }
    return aliases;
}

std::vector<cache_builder::FileAlias> SymlinkResolver::getIntermediateSymlinks() const
{
   auto unusedCallback = ^(const std::string& error) { };
   std::vector<cache_builder::FileAlias> aliases = this->getResolvedSymlinks(unusedCallback);

   std::vector<cache_builder::FileAlias> intermediateAliases;
   for ( const cache_builder::FileAlias& alias : aliases ) {
       Diagnostics diag;

       __block std::vector<std::string> seenSymlinks;
       auto callback = ^(const std::string& intermediateSymlink) {
           seenSymlinks.push_back(intermediateSymlink);
       };
       std::string realPath = this->realPath(diag, alias.aliasPath, callback);

       for ( const std::string& intermediateSymlink : seenSymlinks ) {
           intermediateAliases.push_back({ alias.realPath, intermediateSymlink });
       }
   }

   return intermediateAliases;
}

//
// MARK: --- FileSystemMRM methods ---
//

bool FileSystemMRM::getRealPath(const char possiblePath[MAXPATHLEN], char realPath[MAXPATHLEN]) const
{
    Diagnostics diag;
    std::string resolvedPath = symlinkResolver.realPath(diag, possiblePath);
    if (diag.hasError()) {
        diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
        diag.clearError();
        return false;
    }

    // FIXME: Should we only return real paths of files which point to macho's?  For now that is what we are doing
    auto it = fileMap.find(resolvedPath);
    if (it == fileMap.end())
        return false;

    memcpy(realPath, resolvedPath.c_str(), std::min((size_t)MAXPATHLEN, resolvedPath.size() + 1));
    return true;
}

bool FileSystemMRM::loadFile(const char* path, dyld3::closure::LoadedFileInfo& info,
                             char realerPath[MAXPATHLEN], void (^error)(const char* format, ...)) const
{
        Diagnostics diag;
        std::string resolvedPath = symlinkResolver.realPath(diag, path);
        if (diag.hasError()) {
            diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
            diag.clearError();
            return false;
        }

        auto it = fileMap.find(resolvedPath);
        if (it == fileMap.end())
            return false;

        if (resolvedPath == path)
            realerPath[0] = '\0';
        else
            memcpy(realerPath, resolvedPath.c_str(), std::min((size_t)MAXPATHLEN, resolvedPath.size() + 1));

        // The file exists at this exact path.  Lets use it!
        const FileInfo& fileInfo = files[it->second];

        info.fileContent                = fileInfo.data;
        info.fileContentLen             = fileInfo.length;
        info.sliceOffset                = 0;
        info.sliceLen                   = fileInfo.length;
        info.isOSBinary                 = true;
        info.inode                      = fileInfo.inode;
        info.mtime                      = fileInfo.mtime;
        info.unload                     = nullptr;
        info.path                       = path;
        return true;
    }

void FileSystemMRM::unloadFile(const dyld3::closure::LoadedFileInfo& info) const
{
    if (info.unload)
        info.unload(info);
}

void FileSystemMRM::unloadPartialFile(dyld3::closure::LoadedFileInfo& info,
                                      uint64_t keepStartOffset, uint64_t keepLength) const
{
    // Note we don't actually unload the data here, but we do want to update the offsets for other data structures to track where we are
    info.fileContent = (const void*)((char*)info.fileContent + keepStartOffset);
    info.fileContentLen = keepLength;
}

bool FileSystemMRM::fileExists(const char* path, uint64_t* inode, uint64_t* mtime,
                               bool* issetuid, bool* inodesMatchRuntime) const
{
    Diagnostics diag;
    std::string resolvedPath = symlinkResolver.realPath(diag, path);
    if (diag.hasError()) {
        diag.verbose("MRM error: %s\n", diag.errorMessage().c_str());
        diag.clearError();
        return false;
    }

    auto it = fileMap.find(resolvedPath);
    if (it == fileMap.end())
        return false;

    // The file exists at this exact path.  Lets use it!
    const FileInfo& fileInfo = files[it->second];
    if (inode)
        *inode = fileInfo.inode;
    if (mtime)
        *mtime = fileInfo.mtime;
    if (issetuid)
        *issetuid = false;
    if (inodesMatchRuntime)
        *inodesMatchRuntime = false;
    return true;
}

bool FileSystemMRM::addFile(const char* path, uint8_t* data, uint64_t size, Diagnostics& diag,
                            FileFlags fileFlags,
             uint64_t inode, uint64_t modTime) {
    auto iteratorAndInserted = fileMap.insert(std::make_pair(path, files.size()));
    if (!iteratorAndInserted.second) {
        diag.error("Already have content for path: '%s'", path);
        return false;
    }

    symlinkResolver.addFile(diag, path);
    if (diag.hasError())
        return false;

    if ( (inode == 0) && (modTime == 0) ) {
        // on platforms where MRM builds the cache, inode is just a placeholder
        // Note its safe to just use the index here as we only compare it during closure building
        // and never record it in the closures
        inode = files.size() + 1;
        modTime = 0;
    }

    files.push_back((FileInfo){ path, data, size, fileFlags, modTime, inode });
    return true;
}

bool FileSystemMRM::addSymlink(const char* fromPath, const char* toPath, Diagnostics& diag)
{
    symlinkResolver.addSymlink(diag, fromPath, toPath);
    return !diag.hasError();
}

void FileSystemMRM::forEachFileInfo(std::function<void(const char* path, const void* buffer, size_t bufferSize,
                                        FileFlags fileFlags, uint64_t inode, uint64_t modTime)> lambda)
{
    for (const FileInfo& fileInfo : files)
        lambda(fileInfo.path.c_str(), fileInfo.data, fileInfo.length, fileInfo.flags,
               fileInfo.inode, fileInfo.mtime);
}

size_t FileSystemMRM::fileCount() const
{
    return files.size();
}

std::vector<cache_builder::FileAlias> FileSystemMRM::getResolvedSymlinks(void (^callback)(const std::string& error)) const
{
    return symlinkResolver.getResolvedSymlinks(callback);
}

std::vector<cache_builder::FileAlias> FileSystemMRM::getIntermediateSymlinks() const
{
    return symlinkResolver.getIntermediateSymlinks();
}

