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



#ifndef __DYLD_PATH_OVERRIDES_H__
#define __DYLD_PATH_OVERRIDES_H__

#include <stdint.h>

#if !DYLD_IN_PROCESS
#include <vector>
#include <string>
#endif

#include "Logging.h"
#include "MachOParser.h"


namespace dyld3 {

class VIS_HIDDEN PathOverrides
{
public:
#if !BUILDING_LIBDYLD
    // libdyld is never unloaded
                                    ~PathOverrides();
#endif

#if DYLD_IN_PROCESS
    void                            setEnvVars(const char* envp[]);
    void                            forEachPathVariant(const char* initialPath, void (^handler)(const char* possiblePath, bool& stop)) const;
#else
                                    PathOverrides(const std::vector<std::string>& env);
    void                            forEachPathVariant(const char* initialPath, Platform platform, void (^handler)(const char* possiblePath, bool& stop)) const;
#endif

    uint32_t                        envVarCount() const;
    void                            forEachEnvVar(void (^handler)(const char* envVar)) const;
    void                            forEachInsertedDylib(void (^handler)(const char* dylibPath)) const;

private:
    void                            forEachInColonList(const char* list, void (^callback)(const char* path));
    const char**                    parseColonListIntoArray(const char* list);
    void                            freeArray(const char** array);
    void                            addEnvVar(const char* keyEqualsValue);
    const char*                     getFrameworkPartialPath(const char* path) const;
    static const char*              getLibraryLeafName(const char* path);
    void                            handleListEnvVar(const char* key, const char** list, void (^handler)(const char* envVar)) const;
    void                            handleEnvVar(const char* key, const char* value, void (^handler)(const char* envVar)) const;
    void                            forEachDylibFallback(Platform platform, void (^handler)(const char* fallbackDir, bool& stop)) const;
    void                            forEachFrameworkFallback(Platform platform, void (^handler)(const char* fallbackDir, bool& stop)) const;

    const char**                    _dylibPathOverrides         = nullptr;
    const char**                    _frameworkPathOverrides     = nullptr;
    const char**                    _dylibPathFallbacks         = nullptr;
    const char**                    _frameworkPathFallbacks     = nullptr;
    const char**                    _insertedDylibs             = nullptr;
    const char*                     _imageSuffix                = nullptr;
    const char*                     _rootPath                   = nullptr;  // simulator only
};

#if BUILDING_LIBDYLD
extern PathOverrides   gPathOverrides;
#endif


} // namespace dyld3

#endif // __DYLD_PATH_OVERRIDES_H__


