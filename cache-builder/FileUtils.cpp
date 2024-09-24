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


#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>

#include <string>
#include <fstream>
#include <sstream>

#include "FileUtils.h"
#include "StringUtils.h"
#include "Diagnostics.h"
#include "JSONReader.h"

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "BuilderOptions.h"
#include "NewSharedCacheBuilder.h"
#endif


void iterateDirectoryTree(const std::string& pathPrefix, const std::string& path, bool (^dirFilter)(const std::string& path), void (^fileCallback)(const std::string& path, const struct stat&), bool processFiles, bool recurse)
{
    std::string fullDirPath = pathPrefix + path;
    DIR* dir = ::opendir(fullDirPath.c_str());
    if ( dir == nullptr ) {
        //fprintf(stderr, "can't read 'dir '%s', errno=%d\n", inputPath.c_str(), errno);
        return;
    }
    while (dirent* entry = readdir(dir)) {
        struct stat statBuf;
        std::string dirAndFile = path + (path.back() != '/' ? "/" : "") + entry->d_name;
        std::string fullDirAndFile = pathPrefix + dirAndFile;
         switch ( entry->d_type ) {
            case DT_REG:
                if ( processFiles ) {
                    if ( ::lstat(fullDirAndFile.c_str(), &statBuf) == -1 )
                        break;
                    if ( ! S_ISREG(statBuf.st_mode)  )
                        break;
                    fileCallback(dirAndFile, statBuf);
                }
                break;
            case DT_DIR:
                if ( strcmp(entry->d_name, ".") == 0 )
                    break;
                if ( strcmp(entry->d_name, "..") == 0 )
                    break;
                if ( dirFilter(dirAndFile) )
                    break;
                if (recurse)
                    iterateDirectoryTree(pathPrefix, dirAndFile, dirFilter, fileCallback, processFiles, true);
                break;
            case DT_LNK:
                // don't follow symlinks, dylib will be found through absolute path
#if BUILDING_SIM_CACHE_BUILDER
                // But special case simulator WebKit and related frameworks that are installed into Cryptex path with symlinks
                if ( recurse && dirAndFile.starts_with("/System/Library/") && dirAndFile.ends_with(".framework") ) {
                    char symlinkContent[2048];
                    if ( readlink(fullDirAndFile.c_str(), symlinkContent, sizeof(symlinkContent)) > 0 ) {
                        if ( strncmp(symlinkContent, "../../../System/Cryptexes/OS/System/Library/", 44) == 0 )  {
                            iterateDirectoryTree(pathPrefix, dirAndFile, dirFilter, fileCallback, processFiles, true);
                       }
                    }
                }
#endif
                break;
        }
    }
    ::closedir(dir);
}


bool safeSave(const void* buffer, size_t bufferLen, const std::string& path)
{
    std::string pathTemplate = path + "-XXXXXX";
    size_t templateLen = strlen(pathTemplate.c_str())+2;
    char pathTemplateSpace[templateLen];
    strlcpy(pathTemplateSpace, pathTemplate.c_str(), templateLen);
    int fd = mkstemp(pathTemplateSpace);
    if ( fd != -1 ) {
        ssize_t writtenSize = pwrite(fd, buffer, bufferLen, 0);
        if ( (size_t)writtenSize == bufferLen ) {
            ::fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH); // mkstemp() makes file "rw-------", switch it to "rw-r--r--"
            if ( ::rename(pathTemplateSpace, path.c_str()) == 0) {
                ::close(fd);
                return true; // success
            }
        }
        ::close(fd);
        ::unlink(pathTemplateSpace);
    }
    return false; // failure
}

const void* mapFileReadOnly(const char* path, size_t& mappedSize)
{
    struct stat statBuf;
    if ( ::stat(path, &statBuf) != 0 )
        return nullptr;

    int fd = ::open(path, O_RDONLY);
    if ( fd < 0 )
        return nullptr;

    const void *p = ::mmap(NULL, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if ( p != MAP_FAILED ) {
        mappedSize = (size_t)statBuf.st_size;
        return p;
    }

    return nullptr;
}

bool fileExists(const std::string& path)
{
    struct stat statBuf;
    return ( ::stat(path.c_str(), &statBuf) == 0 );
}

// There is an order file specifying the order in which dylibs are laid out in
// general, as well as an order file specifying the order in which __DATA_DIRTY
// segments are laid out in particular.
//
// The syntax is one dylib (install name) per line.  Blank lines are ignored.
// Comments start with the # character.
std::unordered_map<std::string, uint32_t> parseOrderFile(const std::string& orderFileData) {
    std::unordered_map<std::string, uint32_t> order;

    if (orderFileData.empty())
        return order;

    std::stringstream myData(orderFileData);

    uint32_t count = 0;
    std::string line;
    while ( std::getline(myData, line) ) {
        size_t pos = line.find('#');
        if ( pos != std::string::npos )
            line.resize(pos);
        while ( !line.empty() && isspace(line.back()) ) {
            line.pop_back();
        }
        if ( !line.empty() )
            order[line] = count++;
    }
    return order;
}

std::string loadOrderFile(const std::string& orderFilePath) {
    std::string order;

    size_t size = 0;
    char* data = (char*)mapFileReadOnly(orderFilePath.c_str(), size);
    if (data) {
        order = std::string(data, size);
        ::munmap((void*)data, size);
    }

    return order;
}


std::string toolDir()
{
    char buffer[PATH_MAX];
    uint32_t bufsize = PATH_MAX;
    int result = _NSGetExecutablePath(buffer, &bufsize);
    if ( result == 0 ) {
        std::string path = buffer;
        size_t pos = path.rfind('/');
        if ( pos != std::string::npos )
            return path.substr(0,pos+1);
    }
    //warning("tool directory not found");
    return "/tmp/";
}

std::string basePath(const std::string& path)
{
    std::string::size_type slash_pos = path.rfind("/");
    if (slash_pos != std::string::npos) {
        slash_pos++;
        return path.substr(slash_pos);
    } else {
        return path;
    }
}

std::string dirPath(const std::string& path)
{
    std::string::size_type slash_pos = path.rfind("/");
    if (slash_pos != std::string::npos) {
        slash_pos++;
        return path.substr(0, slash_pos);
    } else {
        char cwd[MAXPATHLEN];
        (void)getcwd(cwd, MAXPATHLEN);
        return cwd;
    }
}

std::string realPath(const std::string& path)
{
    char resolvedPath[PATH_MAX];
    if (realpath(dirPath(path).c_str(), &resolvedPath[0]) != nullptr) {
        return std::string(resolvedPath) + "/" + basePath(path);
    } else {
        return "";
    }
}

std::string realFilePath(const std::string& path)
{
    char resolvedPath[PATH_MAX];
    if ( realpath(path.c_str(), resolvedPath) != nullptr )
        return std::string(resolvedPath);
    else
        return "";
}


std::string normalize_absolute_file_path(std::string path) {
    std::vector<std::string> components;
    std::vector<std::string> processed_components;
    std::stringstream ss(path);
    std::string retval;
    std::string item;

    while (std::getline(ss, item, '/')) {
        components.push_back(item);
    }

    if (components[0] == ".") {
        retval = ".";
    }

    for (auto& component : components) {
        if (component.empty() || component == ".")
            continue;
        else if (component == ".." && processed_components.size())
            processed_components.pop_back();
        else
            processed_components.push_back(component);
    }

    for (auto & component : processed_components) {
        retval = retval + "/" + component;
    }

    return retval;
}

