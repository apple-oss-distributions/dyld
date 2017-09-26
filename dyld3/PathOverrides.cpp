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
#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <sys/stat.h> 
#include <fcntl.h>
#include <sys/errno.h>
#include <unistd.h>

#include "PathOverrides.h"



namespace dyld3 {

#if BUILDING_LIBDYLD
PathOverrides   gPathOverrides;
#endif


// based on ANSI-C strstr()
static const char* strrstr(const char* str, const char* sub) 
{
    const size_t sublen = strlen(sub);
    for(const char* p = &str[strlen(str)]; p != str; --p) {
        if ( strncmp(p, sub, sublen) == 0 )
            return p;
    }
    return NULL;
}


#if DYLD_IN_PROCESS
void PathOverrides::setEnvVars(const char* envp[])
{
    for (const char** p = envp; *p != NULL; p++) {
        addEnvVar(*p);
    }
}

#else
PathOverrides::PathOverrides(const std::vector<std::string>& env)
{
    for (const std::string& envVar : env) {
        addEnvVar(envVar.c_str());
    }
}
#endif

#if !BUILDING_LIBDYLD
// libdyld is never unloaded
PathOverrides::~PathOverrides()
{
    freeArray(_dylibPathOverrides);
    freeArray(_frameworkPathOverrides);
    freeArray(_frameworkPathFallbacks);
    freeArray(_dylibPathFallbacks);
}
#endif


void PathOverrides::handleEnvVar(const char* key, const char* value, void (^handler)(const char* envVar)) const
{
    if ( value == nullptr )
        return;
    size_t allocSize = strlen(key) + strlen(value) + 2;
    char buffer[allocSize];
    strlcpy(buffer, key, allocSize);
    strlcat(buffer, "=", allocSize);
    strlcat(buffer, value, allocSize);
    handler(buffer);
}

void PathOverrides::handleListEnvVar(const char* key, const char** list, void (^handler)(const char* envVar)) const
{
    if ( list == nullptr )
        return;
    size_t allocSize = strlen(key) + 2;
    for (const char** lp=list; *lp != nullptr; ++lp)
        allocSize += strlen(*lp)+1;
    char buffer[allocSize];
    strlcpy(buffer, key, allocSize);
    strlcat(buffer, "=", allocSize);
    bool needColon = false;
    for (const char** lp=list; *lp != nullptr; ++lp) {
        if ( needColon )
            strlcat(buffer, ":", allocSize);
        strlcat(buffer, *lp, allocSize);
        needColon = true;
    }
    handler(buffer);
}

void PathOverrides::forEachEnvVar(void (^handler)(const char* envVar)) const
{
    handleListEnvVar("DYLD_LIBRARY_PATH",            _dylibPathOverrides,      handler);
    handleListEnvVar("DYLD_FRAMEWORK_PATH",          _frameworkPathOverrides,  handler);
    handleListEnvVar("DYLD_FALLBACK_FRAMEWORK_PATH", _frameworkPathFallbacks,  handler);
    handleListEnvVar("DYLD_FALLBACK_LIBRARY_PATH",   _dylibPathFallbacks,      handler);
    handleListEnvVar("DYLD_INSERT_LIBRARIES",        _insertedDylibs,          handler);
    handleEnvVar(    "DYLD_IMAGE_SUFFIX",            _imageSuffix,             handler);
    handleEnvVar(    "DYLD_ROOT_PATH",               _rootPath,                handler);
}

uint32_t PathOverrides::envVarCount() const
{
    uint32_t count = 0;
    if ( _dylibPathOverrides != nullptr )
        ++count;
    if ( _frameworkPathOverrides != nullptr )
        ++count;
    if ( _frameworkPathFallbacks != nullptr )
        ++count;
    if ( _dylibPathFallbacks != nullptr )
        ++count;
    if ( _insertedDylibs != nullptr )
        ++count;
    if ( _imageSuffix != nullptr )
        ++count;
    if ( _rootPath != nullptr )
        ++count;
    return count;
}

void PathOverrides::forEachInsertedDylib(void (^handler)(const char* dylibPath)) const
{
    if ( _insertedDylibs == nullptr )
        return;
    for (const char** lp=_insertedDylibs; *lp != nullptr; ++lp)
        handler(*lp);
}

void PathOverrides::addEnvVar(const char* keyEqualsValue)
{
    const char* equals = strchr(keyEqualsValue, '=');
    if ( equals != NULL ) {
        const char* value = &equals[1];
        const size_t keyLen = equals-keyEqualsValue;
        char key[keyLen+1];
        strncpy(key, keyEqualsValue, keyLen);
        key[keyLen] = '\0';
        if ( strcmp(key, "DYLD_LIBRARY_PATH") == 0 ) {
            _dylibPathOverrides = parseColonListIntoArray(value);
        }
        else if ( strcmp(key, "DYLD_FRAMEWORK_PATH") == 0 ) {
            _frameworkPathOverrides = parseColonListIntoArray(value);
        }
        else if ( strcmp(key, "DYLD_FALLBACK_FRAMEWORK_PATH") == 0 ) {
            _frameworkPathFallbacks = parseColonListIntoArray(value);
        }
        else if ( strcmp(key, "DYLD_FALLBACK_LIBRARY_PATH") == 0 ) {
            _dylibPathFallbacks = parseColonListIntoArray(value);
        }
        else if ( strcmp(key, "DYLD_INSERT_LIBRARIES") == 0 ) {
            _insertedDylibs = parseColonListIntoArray(value);
        }
        else if ( strcmp(key, "DYLD_IMAGE_SUFFIX") == 0 ) {
            _imageSuffix = value;
        }
        else if ( strcmp(key, "DYLD_ROOT_PATH") == 0 ) {
            _rootPath = value;
        }
    }
}

void PathOverrides::forEachInColonList(const char* list, void (^handler)(const char* path))
{
    char buffer[strlen(list)+1];
    const char* t = list;
    for (const char* s=list; *s != '\0'; ++s) {
        if (*s != ':')
            continue;
        size_t len = s - t;
        memcpy(buffer, t, len);
        buffer[len] = '\0';
        handler(buffer);
        t = s+1;
    }
    handler(t);
}

const char** PathOverrides::parseColonListIntoArray(const char* list)
{
    __block int count = 1;
    forEachInColonList(list, ^(const char* path) {
        ++count;
    });
    const char** array = (const char**)malloc(count*sizeof(char*));
    __block const char** p = array;
    forEachInColonList(list, ^(const char* path) {
        *p++ = strdup(path);
    });
    *p = nullptr;
    return array;
}

void PathOverrides::freeArray(const char** array)
{
    if ( array == nullptr )
        return;

    for (const char** p=array; *p != nullptr; ++p) {
        free((void*)*p);
    }
    free(array);
}

void PathOverrides::forEachDylibFallback(Platform platform, void (^handler)(const char* fallbackDir, bool& stop)) const
{
    bool stop = false;
    if ( _dylibPathFallbacks != nullptr ) {
        for (const char** fp=_dylibPathFallbacks; *fp != nullptr; ++fp) {
            handler(*fp, stop);
            if ( stop )
                return;
        }
    }
    else {
        switch ( platform ) {
            case Platform::macOS:
                // "$HOME/lib"
                handler("/usr/local/lib", stop);  // FIXME: not for restricted processes
                if ( !stop )
                    handler("/usr/lib", stop);
                break;
            case Platform::iOS:
            case Platform::watchOS:
            case Platform::tvOS:
            case Platform::bridgeOS:
            case Platform::unknown:
                handler("/usr/local/lib", stop);
                if ( !stop )
                    handler("/usr/lib", stop);
                break;
        }
    }
}

void PathOverrides::forEachFrameworkFallback(Platform platform, void (^handler)(const char* fallbackDir, bool& stop)) const
{
    bool stop = false;
    if ( _frameworkPathFallbacks != nullptr ) {
        for (const char** fp=_frameworkPathFallbacks; *fp != nullptr; ++fp) {
            handler(*fp, stop);
            if ( stop )
                return;
        }
    }
    else {
        switch ( platform ) {
            case Platform::macOS:
                // "$HOME/Library/Frameworks"
                handler("/Library/Frameworks", stop);   // FIXME: not for restricted processes
                // "/Network/Library/Frameworks"
                if ( !stop )
                    handler("/System/Library/Frameworks", stop);
                break;
            case Platform::iOS:
            case Platform::watchOS:
            case Platform::tvOS:
            case Platform::bridgeOS:
            case Platform::unknown:
                handler("/System/Library/Frameworks", stop);
                break;
        }
    }
}

void PathOverrides::forEachPathVariant(const char* initialPath,
#if !DYLD_IN_PROCESS
                                       Platform platform,
#endif
                                       void (^handler)(const char* possiblePath, bool& stop)) const
{
#if DYLD_IN_PROCESS
    Platform platform = MachOParser::currentPlatform();
#endif
    __block bool stop = false;

    // check for overrides
    const char* frameworkPartialPath = getFrameworkPartialPath(initialPath);
    if ( frameworkPartialPath != nullptr ) {
        const size_t frameworkPartialPathLen = strlen(frameworkPartialPath);
        // look at each DYLD_FRAMEWORK_PATH directory
        if ( _frameworkPathOverrides != nullptr ) {
            for (const char** fp=_frameworkPathOverrides; *fp != nullptr; ++fp) {
                char npath[strlen(*fp)+frameworkPartialPathLen+8];
                strcpy(npath, *fp);
                strcat(npath, "/");
                strcat(npath, frameworkPartialPath);
                handler(npath, stop);
                if ( stop )
                    return;
            }
        }
    }
    else {
        const char* libraryLeafName = getLibraryLeafName(initialPath);
        const size_t libraryLeafNameLen = strlen(libraryLeafName);
        // look at each DYLD_LIBRARY_PATH directory
        if ( _dylibPathOverrides != nullptr ) {
            for (const char** lp=_dylibPathOverrides; *lp != nullptr; ++lp) {
                char libpath[strlen(*lp)+libraryLeafNameLen+8];
                strcpy(libpath, *lp);
                strcat(libpath, "/");
                strcat(libpath, libraryLeafName);
                handler(libpath, stop);
                if ( stop )
                    return;
            }
        }
    }

    // try original path
    handler(initialPath, stop);
    if ( stop )
        return;

    // check fallback paths
    if ( frameworkPartialPath != nullptr ) {
        const size_t frameworkPartialPathLen = strlen(frameworkPartialPath);
        // look at each DYLD_FALLBACK_FRAMEWORK_PATH directory
        forEachFrameworkFallback(platform, ^(const char* dir, bool& innerStop) {
            char npath[strlen(dir)+frameworkPartialPathLen+8];
            strcpy(npath, dir);
            strcat(npath, "/");
            strcat(npath, frameworkPartialPath);
            handler(npath, innerStop);
            if ( innerStop )
                stop = innerStop;
        });

    }
   else {
        const char* libraryLeafName = getLibraryLeafName(initialPath);
        const size_t libraryLeafNameLen = strlen(libraryLeafName);
        // look at each DYLD_FALLBACK_LIBRARY_PATH directory
        forEachDylibFallback(platform, ^(const char* dir, bool& innerStop) {
            char libpath[strlen(dir)+libraryLeafNameLen+8];
            strcpy(libpath, dir);
            strcat(libpath, "/");
            strcat(libpath, libraryLeafName);
            handler(libpath, innerStop);
            if ( innerStop )
                stop = innerStop;
        });
    }
}


//
// Find framework path
//
//  /path/foo.framework/foo                             =>   foo.framework/foo    
//  /path/foo.framework/Versions/A/foo                  =>   foo.framework/Versions/A/foo
//  /path/foo.framework/Frameworks/bar.framework/bar    =>   bar.framework/bar
//  /path/foo.framework/Libraries/bar.dylb              =>   NULL
//  /path/foo.framework/bar                             =>   NULL
//
// Returns nullptr if not a framework path
//
const char* PathOverrides::getFrameworkPartialPath(const char* path) const
{
    const char* dirDot = strrstr(path, ".framework/");
    if ( dirDot != nullptr ) {
        const char* dirStart = dirDot;
        for ( ; dirStart >= path; --dirStart) {
            if ( (*dirStart == '/') || (dirStart == path) ) {
                const char* frameworkStart = &dirStart[1];
                if ( dirStart == path )
                    --frameworkStart;
                size_t len = dirDot - frameworkStart;
                char framework[len+1];
                strncpy(framework, frameworkStart, len);
                framework[len] = '\0';
                const char* leaf = strrchr(path, '/');
                if ( leaf != nullptr ) {
                    if ( strcmp(framework, &leaf[1]) == 0 ) {
                        return frameworkStart;
                    }
                    if (  _imageSuffix != nullptr ) {
                        // some debug frameworks have install names that end in _debug
                        if ( strncmp(framework, &leaf[1], len) == 0 ) {
                            if ( strcmp( _imageSuffix, &leaf[len+1]) == 0 )
                                return frameworkStart;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}


const char* PathOverrides::getLibraryLeafName(const char* path)
{
    const char* start = strrchr(path, '/');
    if ( start != nullptr )
        return &start[1];
    else
        return path;
}

} // namespace dyld3





