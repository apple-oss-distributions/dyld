/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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

#include <dirent.h>
#include <libgen.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <list>
#include <string>
#include <vector>

#include "ClosureFileSystemNull.h"
#include "Error.h"
#include "File.h"
#include "FileUtils.h"
#include "Header.h"
#include "Image.h"
#include "StringUtils.h"
#include "SymbolsCache.h"
#include "Universal.h"

using mach_o::LinkedDylibAttributes;
using mach_o::Error;
using mach_o::Fixup;
using mach_o::Header;
using mach_o::Image;
using mach_o::Symbol;
using mach_o::Universal;
using mach_o::Version32;

static void usage()
{
    fprintf(stderr, "Usage: dyld_symbols_cache [-verify *path*]* [-all_imports] [-all_exports]\n"
            "\t-verify                                  verify that the content in the given path doesn't introduce new symbols errors\n"
            "\t-all_binaries                            dump all binaries\n"
            "\t-all_imports                             dump all imports\n"
            "\t-all_exports                             dump all exports\n"
            "\t-all_imports_of *install_name* *symbol*  dump all imports of the given symbol from the binary with the given install name\n"
        );
}

static void forEachSymlink(const std::string& path,
                           void (^symlinkCallback)(const std::string& path),
                           void (^symlinkErrorCallback)(const std::string& symlinkPath, const std::string& errorString))
{
    std::string fullDirPath = path;
    DIR* dir = ::opendir(fullDirPath.c_str());
    if ( dir == nullptr ) {
        //fprintf(stderr, "can't read 'dir '%s', errno=%d\n", inputPath.c_str(), errno);
        return;
    }
    while (dirent* entry = readdir(dir)) {
        std::string pathWithSlash = path + (path.back() != '/' ? "/" : "");
        std::string dirAndFile = pathWithSlash + entry->d_name;
        std::string fullDirAndFile = dirAndFile;
        if ( entry->d_type == DT_LNK ) {
            char symlinkContent[2048] = { '\0' };
            if ( readlink(fullDirAndFile.c_str(), symlinkContent, sizeof(symlinkContent)) > 0 ) {
                std::string targetPath;
                if ( startsWith(symlinkContent, "/") ) {
                    targetPath = symlinkContent;
                } else {
                    char dirPath[PATH_MAX];
                    if ( dirname_r(fullDirAndFile.c_str(), dirPath) ) {
                        targetPath = realPath(pathWithSlash + "/" + symlinkContent);
                    }
                }
                if ( !targetPath.empty())
                    symlinkCallback(targetPath);
            } else {
                std::string errorString = strerror(errno);
                symlinkErrorCallback(fullDirAndFile, errorString);
            }
        }
    }
    ::closedir(dir);
}

static void printResults(std::span<ResultBinary> verifyResults,
                         std::span<std::pair<std::string, std::string>> rootErrors,
                         bool bniOutput,
                         const char* detailsLogPath)
{
    FILE* detailsLogFile = nullptr;
    if ( detailsLogPath != nullptr ) {
        detailsLogFile = fopen(detailsLogPath, "a");
        if ( detailsLogFile == nullptr ) {
            fprintf(stderr, "Could not open log file '%s' due to: %s\n\n", detailsLogPath, strerror(errno));
        } else {
            fprintf(stderr, "Additional logging available in '%s'\n", detailsLogPath);
        }
    }

    if ( detailsLogFile == nullptr )
        detailsLogFile = stderr;

    printResultSummary(verifyResults, bniOutput, stderr);

    printResultsSymbolDetails(verifyResults, detailsLogFile);

    printResultsInternalInformation(verifyResults, rootErrors, detailsLogFile);

    if ( detailsLogFile != stderr )
        fclose(detailsLogFile);
}

static void printResultsJSON(std::string_view jsonPath,
                             std::span<ResultBinary> verifyResults,
                             std::span<ExportsChangedBinary> exportsChanged)
{
    assert(!jsonPath.empty());
    if ( jsonPath == "-" ) {
        printResultsJSON(verifyResults, exportsChanged, stdout);
    } else {
        FILE* jsonFile = fopen(jsonPath.data(), "a");
        if ( jsonFile == nullptr ) {
            fprintf(stderr, "Could not open json file '%s' due to: %s\n\n", jsonPath.data(), strerror(errno));
            exit(1);
        }

        printResultsJSON(verifyResults, exportsChanged, jsonFile);

        fclose(jsonFile);
    }
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    if ( argc == 1 ) {
        usage();
        return 1;
    }

    // Generic options
    std::string symbolsDBPath;
    std::string dylibCachePath;
    bool verbose = false;

    // Verify/build options
    bool verifying = false;
    bool building = false;
    bool bniOutput = false;
    SymbolsCache::ExecutableMode executableMode = SymbolsCache::ExecutableMode::off;
    bool checkForChangedExports = false;
    bool verifyIndividually = false;
    const char* detailsLogPath = nullptr;
    const char* jsonPath = nullptr;
    std::vector<std::string> rootPaths;
    std::vector<std::string> jsonRootPaths;
    __block std::unordered_set<std::string> verifyProjects;

    // Print options
    bool printing = false;
    bool printAllBinaries = false;
    bool printAllExports = false;
    bool printAllImports = false;
    std::string allUsersOfInstallName;
    std::string allUsersOfSymbolName;

    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-verify") == 0 ) {
            verifying = true;
            if ( ++i < argc ) {
                rootPaths.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-verify missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-build") == 0 ) {
            building = true;
            if ( ++i < argc ) {
                rootPaths.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-build missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-verify_json") == 0 ) {
            verifying = true;
            if ( ++i < argc ) {
                jsonRootPaths.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-verify missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-build_json") == 0 ) {
            building = true;
            if ( ++i < argc ) {
                jsonRootPaths.push_back(argv[i]);
            }
            else {
                fprintf(stderr, "-build missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-project") == 0 ) {
            if ( ++i < argc ) {
                verifyProjects.insert(argv[i]);
            }
            else {
                fprintf(stderr, "-project missing name\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-projects") == 0 ) {
            if ( ++i < argc ) {
                if ( Error err = ld::File::forEachLine(argv[i], ^mach_o::Error(CString line) {
                    verifyProjects.insert(line.c_str());
                    return Error::none();
                })) {
                    // ignore the error
                }
            }
            else {
                fprintf(stderr, "-projects missing name\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-symbols_db") == 0 ) {
            if ( ++i < argc ) {
                symbolsDBPath = argv[i];
            }
            else {
                fprintf(stderr, "-symbols_db missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-dylib_cache") == 0 ) {
            if ( ++i < argc ) {
                dylibCachePath = argv[i];
            }
            else {
                fprintf(stderr, "-dylib_cache missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-bni") == 0 ) {
            bniOutput = true;
        }
        else if ( strcmp(arg, "-verify_executables") == 0 ) {
            executableMode = SymbolsCache::ExecutableMode::error;
        }
        else if ( strcmp(arg, "-warn_executables") == 0 ) {
            executableMode = SymbolsCache::ExecutableMode::warn;
        }
        else if ( strcmp(arg, "-details_log_path") == 0 ) {
            if ( ++i < argc ) {
                detailsLogPath = argv[i];
            }
            else {
                fprintf(stderr, "-details_log_path missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-verify_each") == 0 ) {
            verifyIndividually = true;
        }
        else if ( strcmp(arg, "-all_binaries") == 0 ) {
            printing = true;
            printAllBinaries = true;
        }
        else if ( strcmp(arg, "-all_exports") == 0 ) {
            printing = true;
            printAllExports = true;
        }
        else if ( strcmp(arg, "-all_imports") == 0 ) {
            printing = true;
            printAllImports = true;
        }
        else if ( strcmp(arg, "-all_imports_of") == 0 ) {
            printing = true;
            if ( ++i < argc ) {
                allUsersOfInstallName = argv[i];
            }
            else {
                fprintf(stderr, "-all_imports_of missing install_name\n");
                return 1;
            }
            if ( ++i < argc ) {
                allUsersOfSymbolName = argv[i];
            }
            else {
                fprintf(stderr, "-all_imports_of missing symbol name\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-verbose") == 0 ) {
            verbose = true;
        }
        else if ( strcmp(arg, "-json") == 0 ) {
            if ( ++i < argc ) {
                jsonPath = argv[i];
            }
            else {
                fprintf(stderr, "-json missing path\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-changed_exports") == 0 ) {
            checkForChangedExports = true;
        }
        else if ( strcmp(arg, "-help") == 0 ) {
            usage();
            return 0;
        }
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if ( rootPaths.empty() && jsonRootPaths.empty() && !printing ) {
        fprintf(stderr, "missing one of '-verify', '-build' or '-all_*'.  See -help\n");
        return 1;
    }

    if ( symbolsDBPath.empty() && dylibCachePath.empty() ) {
        fprintf(stderr, "missing one of '-symbols_db' or '-dylib_cache'.  See -help\n");
        return 1;
    }

    std::string driverKitPath;
    std::string exclaveKitPath;
    if ( symbolsDBPath.empty() && !dylibCachePath.empty() ) {
        symbolsDBPath = dylibCachePath + "/System/Library/dyld/dyld_symbols.db";
        driverKitPath = dylibCachePath + "/System/DriverKit/System/Library/dyld/dyld_symbols.db";
        exclaveKitPath = dylibCachePath + "/System/ExclaveKit/System/Library/dyld/dyld_symbols.db";
    }

    // There are potentially multiple caches, if driverKit/exclaveKit were in use
    std::list<SymbolsCache> caches;
    {
        SymbolsCache& cache = caches.emplace_back(symbolsDBPath);
        if ( Error err = cache.open() ) {
            fprintf(stderr, "Could not open database due to: %s\n", (const char*)err.message());
            return 1;
        }
    }

    if ( !building ) {
        // Try driverKit/exclaveKit. These are non-fatal if they are missing
        if ( fileExists(driverKitPath) ) {
            SymbolsCache& cache = caches.emplace_back(driverKitPath);
            if ( Error err = cache.open() ) {
                fprintf(stderr, "Could not open database due to: %s\n", (const char*)err.message());
                return 1;
            }
        }

        if ( fileExists(exclaveKitPath) ) {
            SymbolsCache& cache = caches.emplace_back(exclaveKitPath);
            if ( Error err = cache.open() ) {
                fprintf(stderr, "Could not open database due to: %s\n", (const char*)err.message());
                return 1;
            }
        }
    }


    if ( verbose ) {
        for ( SymbolsCache& cache : caches )
            cache.setVerbose();
    }

    if ( printAllBinaries ) {
        std::vector<SymbolsCacheBinary> allBinaries;

        for ( SymbolsCache& cache : caches ) {
            if ( Error err = cache.getAllBinaries(allBinaries) ) {
                fprintf(stderr, "Could not get all binaries due to: %s\n", (const char*)err.message());
                return 1;
            }
        }

        for ( const SymbolsCacheBinary& binary : allBinaries )
            printf("%s %s %s %s %s\n", binary.arch.c_str(), binary.platform.name().c_str(),
                   binary.path.c_str(), binary.uuid.c_str(), binary.projectName.c_str());
    }

    if ( printAllExports ) {
        std::vector<SymbolsCache::ExportedSymbol> allExports;

        for ( SymbolsCache& cache : caches ) {
            if ( Error err = cache.getAllExports(allExports) ) {
                fprintf(stderr, "Could not get all exports due to: %s\n", (const char*)err.message());
                return 1;
            }
        }

        for ( const SymbolsCache::ExportedSymbol& exportedSymbol : allExports )
            printf("%s %s %s\n", exportedSymbol.archName.c_str(), exportedSymbol.installName.c_str(),
                   exportedSymbol.symbolName.c_str());
    }

    if ( printAllImports ) {
        std::vector<SymbolsCache::ImportedSymbol> allImports;

        for ( SymbolsCache& cache : caches ) {
            if ( Error err = cache.getAllImports(allImports) ) {
                fprintf(stderr, "Could not get all imports due to: %s\n", (const char*)err.message());
                return 1;
            }
        }

        for ( const SymbolsCache::ImportedSymbol& importedSymbol : allImports )
            printf("%s %s %s %s\n", importedSymbol.archName.c_str(), importedSymbol.clientPath.c_str(),
                   importedSymbol.targetInstallName.c_str(), importedSymbol.targetSymbolName.c_str());
    }

    if ( !allUsersOfInstallName.empty() ) {
        std::vector<SymbolsCache::ImportedSymbol> allImports;

        for ( SymbolsCache& cache : caches ) {
            if ( Error err = cache.getAllImports(allImports) ) {
                fprintf(stderr, "Could not get all imports due to: %s\n", (const char*)err.message());
                return 1;
            }
        }

        for ( const SymbolsCache::ImportedSymbol& importedSymbol : allImports ) {
            if ( importedSymbol.targetInstallName != allUsersOfInstallName )
                continue;
            if ( importedSymbol.targetSymbolName != allUsersOfSymbolName )
                continue;
            printf("%s %s\n", importedSymbol.archName.c_str(), importedSymbol.clientPath.c_str());
        }
    }

    if ( rootPaths.empty() && jsonRootPaths.empty() )
        return 0;

    __block std::vector<std::pair<std::string, std::string>> rootAndFilePaths;
    __block std::vector<std::pair<std::string, std::string>> rootErrors;
    if ( !rootPaths.empty() ) {
        // Walk all root paths and see if they are directories full of ../ symlinks
        __block std::vector<std::string> additionalRootPaths;
        for ( const std::string& rootPath : rootPaths ) {
            forEachSymlink(rootPath, ^(const std::string &path) {
                additionalRootPaths.push_back(path);
            }, ^(const std::string& symlinkPath, const std::string& errorString) {
                rootErrors.push_back({ symlinkPath, errorString });
            });
        }

        rootPaths.insert(rootPaths.end(), additionalRootPaths.begin(), additionalRootPaths.end());

        for ( std::string& rootPath : rootPaths ) {
            char realRootPath[PATH_MAX];
            if ( ::realpath(rootPath.c_str(), realRootPath) != nullptr )
                rootPath = realRootPath;

            auto dirFilter = ^(const std::string& dirPath) { return false; };
            auto fileHandler = ^(const std::string& path, const struct stat& statBuf) {
                if ( statBuf.st_size > 4096 )
                    rootAndFilePaths.push_back({ rootPath, path });
            };
            iterateDirectoryTree("", rootPath, dirFilter, fileHandler, true /* process files */, true /* recurse */);
        }
    }

    if ( !jsonRootPaths.empty() ) {
        for ( std::string& rootPath : jsonRootPaths ) {
            char realRootPath[PATH_MAX];
            if ( ::realpath(rootPath.c_str(), realRootPath) != nullptr )
                rootPath = realRootPath;

            auto dirFilter = ^(const std::string& dirPath) { return false; };
            auto fileHandler = ^(const std::string& path, const struct stat& statBuf) {
                if ( path.ends_with(".json") )
                    rootAndFilePaths.push_back({ rootPath, path });
            };
            iterateDirectoryTree("", rootPath, dirFilter, fileHandler, true /* process files */, false /* recurse */);
        }
    }

    if ( rootAndFilePaths.empty() ) {
        fprintf(stderr, "Could not find any files to process\n");
        return 0;
    }

    __block std::vector<SymbolsCacheBinary> newBinaries;
    for ( const std::pair<std::string, std::string>& fileAndRootPath : rootAndFilePaths ) {
        const std::string& rootPath = fileAndRootPath.first;
        const std::string& filePath = fileAndRootPath.second;
        ld::File::ReadOnlyMapping fileMapping;

        char fileRealPath[PATH_MAX] = { '\0' };
        if ( Error err = ld::File::mapReadOnlyAt(filePath.c_str(), nullptr, fileMapping, &fileRealPath[0]) ) {
            fprintf(stderr, "Could not open file because: %s\n", err.message());
            continue;
        }
        std::string_view fileRealPathView = filePath;
        if ( fileRealPath[0] != '\0' )
            fileRealPathView = fileRealPath;

        if ( fileRealPathView.starts_with(rootPath) )
            fileRealPathView = fileRealPathView.substr(rootPath.size());

        dyld3::closure::FileSystemNull fileSystem;

        __block std::vector<SymbolsCacheBinary> binaries;
        if ( mach_o::Error err = SymbolsCache::makeBinaries({ }, fileSystem,
                                                            fileMapping.buffer.data(), fileMapping.buffer.size(),
                                                            fileRealPathView, "", binaries) ) {
            // TODO: Should we error out if the binaries are bad?  For now skip them
            continue;
        }

        // Set the root paths where we got these binaries, so that we can print where they came from later
        for ( SymbolsCacheBinary& binary : binaries )
            binary.rootPath = rootPath;

        newBinaries.insert(newBinaries.end(), binaries.begin(), binaries.end());
    }

    if ( building ) {
        // We might be building a new database, so add the tables
        assert(caches.size() == 1);
        SymbolsCache& cache = caches.front();
        if ( Error err = cache.create() ) {
            fprintf(stderr, "error: %s\n", (const char*)err.message());
            return 1;
        }
        if ( Error err = cache.addBinaries(newBinaries) ) {
            fprintf(stderr, "error: %s\n", (const char*)err.message());
            return 1;
        }
        return 0;
    }

    // verifying
    (void)verifying;
    std::vector<ResultBinary> verifyResults;
    std::vector<Error> internalWarnings;
    std::vector<ExportsChangedBinary> exportsChanged;
    std::vector<ExportsChangedBinary>* exportsChangedPtr = checkForChangedExports ? &exportsChanged : nullptr;
    bool warnOnRemovedSymbols = false;

    for ( SymbolsCache& cache : caches ) {
        if ( verifyIndividually ) {
            for ( const SymbolsCacheBinary& binary : newBinaries ) {
                if ( Error verifyErr = cache.checkNewBinaries(warnOnRemovedSymbols, executableMode,
                                                              { binary }, verifyProjects,
                                                              verifyResults, internalWarnings,
                                                              exportsChangedPtr) ) {
                    fprintf(stderr, "Could not verify binaries because: %s\n", (const char*)verifyErr.message());
                    return 1;
                }
            }
        } else {
            if ( Error verifyErr = cache.checkNewBinaries(warnOnRemovedSymbols, executableMode,
                                                          std::move(newBinaries), verifyProjects,
                                                          verifyResults, internalWarnings,
                                                          exportsChangedPtr) ) {
                fprintf(stderr, "Could not verify binaries because: %s\n", (const char*)verifyErr.message());
                return 1;
            }
        }
    }

    if ( !verifyResults.empty() ) {
        if ( jsonPath != nullptr )
            printResultsJSON(jsonPath, verifyResults, exportsChanged);
        else
            printResults(verifyResults, rootErrors, bniOutput, detailsLogPath);

        // only return 1 if we had an error, not if everything is a warning
        for ( const ResultBinary& result : verifyResults ) {
            if ( !result.warn )
                return 1;
        }
    } else {
        // no errors, but we might still want JSON output of the changed exports
        if ( jsonPath != nullptr ) {
            printResultsJSON(jsonPath, verifyResults, exportsChanged);
            return 0;
        }
    }

    if ( !internalWarnings.empty() ) {
        for ( Error& warning : internalWarnings ) {
            fprintf(stderr, "warning: %s\n", (const char*)warning.message());
        }
        return 0;
    }

    return 0;
}
