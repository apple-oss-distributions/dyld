/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef SymbolsCache_h
#define SymbolsCache_h

#include "Defines.h"
#include "Error.h"
#include "Platform.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <string>
#include <variant>
#include <vector>

typedef struct sqlite3 sqlite3;

namespace dyld3
{
    namespace closure
    {
        class FileSystem;
    }
}

struct VIS_HIDDEN SymbolsCacheBinary
{
    SymbolsCacheBinary(std::string path, mach_o::Platform platform, std::string arch,
                       std::string uuid, std::string projectName);

    typedef std::variant<std::string, int64_t> TargetBinary;

    struct ImportedSymbol
    {
        // This is an install name or the binary ID if we know the target
        TargetBinary targetBinary;
        std::string symbolName;
    };

    std::string                         path;
    mach_o::Platform                    platform;
    std::string                         arch;
    std::string                         installName;
    std::vector<std::string>            exportedSymbols;
    std::vector<ImportedSymbol>         importedSymbols;
    std::vector<TargetBinary>           reexportedLibraries;
    std::string                         rootPath;
    std::string                         uuid;
    std::string                         projectName;

    // Cache for binaryID once we have it in the database
    std::optional<int64_t>              binaryID;

    // This is used for JSON input files, to see whether to emit errors or not
    std::string                         inputFileName;
};

// Represents a binary for which we found errors/warnings in checkBinaries
struct ResultBinary
{
    struct ClientBinary
    {
        std::string path;
        std::string uuid;
        std::string rootPath;
        std::string projectName;
        std::string symbolName;
    };

    std::string     installName;
    std::string     arch;
    std::string     uuid;
    std::string     rootPath;
    std::string     projectName;
    ClientBinary    client;

    // defaults to erroring out, but can change to just a warning
    bool            warn = false;
};

// Represents a binary for which exports changed
struct ExportsChangedBinary
{
    std::string     symbolName;
    std::string     installName;
    std::string     arch;
    std::string     uuid;
    std::string     projectName;
    bool            wasAdded = false; // false -> was removed, true -> was added
};

class VIS_HIDDEN SymbolsCache
{
public:
    // Makes an in-memory symbols cache
    SymbolsCache();

    // Load/make an on-disk symbols cache
    SymbolsCache(std::string_view dbPath);

    ~SymbolsCache();

    typedef std::unordered_map<std::string, int64_t> SymbolNameCache;

    mach_o::Error create();

    // Note the caller should free the buffer
    mach_o::Error serialize(const uint8_t*& buffer, uint64_t& bufferSize);

    typedef std::unordered_map<std::string, std::vector<mach_o::Platform>> ArchPlatforms;

    static mach_o::Error makeBinariesFromJSON(const ArchPlatforms& archPlatforms,
                                              const void* buffer, uint64_t bufferSize, std::string_view path,
                                              std::string_view projectName,
                                              bool allowExecutables,
                                              std::vector<SymbolsCacheBinary>& binaries);

    static mach_o::Error makeBinaries(const ArchPlatforms& archPlatforms,
                                      const dyld3::closure::FileSystem& fileSystem,
                                      const void* buffer, uint64_t bufferSize, std::string_view path,
                                      std::string_view projectName,
                                      std::vector<SymbolsCacheBinary>& binaries);

    typedef std::unordered_set<std::string> BinaryProjects;

    enum class ExecutableMode
    {
        off,
        warn,
        error
    };

    // This is the main method that drives verification of new content in the build.
    // We parse them in to the input here, then this method checks if the database will
    // have new missing symbol errors as a result of applying these binaries to the database
    // Note the return value is an error if there was some issue querying the database, while
    // the output 'errors' is about errors on binaries themselves, not database errors
    mach_o::Error checkNewBinaries(bool warnOnRemovedSymbols, ExecutableMode executableMode,
                                   std::vector<SymbolsCacheBinary>&& binaries,
                                   const BinaryProjects& binaryProjects,
                                   std::vector<ResultBinary>& results,
                                   std::vector<mach_o::Error>& internalWarnings,
                                   std::vector<ExportsChangedBinary>* exportsChanged = nullptr) const;

    mach_o::Error addBinaries(std::vector<SymbolsCacheBinary>& binaries);

    // Used for querying a cache for testing
    bool containsExecutable(std::string_view path) const;
    bool containsDylib(std::string_view path, std::string_view installName) const;
    std::vector<SymbolsCacheBinary::ImportedSymbol> getImports(std::string_view path) const;
    std::vector<std::string> getExports(std::string_view path) const;
    std::vector<std::string> getReexports(std::string_view path) const;

    mach_o::Error getAllBinaries(std::vector<SymbolsCacheBinary>& binaries) const;

    struct ExportedSymbol {
        std::string archName;
        std::string installName;
        std::string symbolName;
    };
    mach_o::Error getAllExports(std::vector<ExportedSymbol>& exports) const;

    struct ImportedSymbol {
        std::string archName;
        std::string clientPath;
        std::string targetInstallName;
        std::string targetSymbolName;
    };
    mach_o::Error getAllImports(std::vector<ImportedSymbol>& imports) const;

    mach_o::Error open();

    __attribute__((used))
    mach_o::Error dump() const;

    void setVerbose() { this->verbose = true; }

private:
    mach_o::Error startTransaction();
    mach_o::Error endTransaction();
    mach_o::Error rollbackTransaction();
    mach_o::Error createTables();
    

    std::string                   dbPath;
    sqlite3*                      symbolsDB = nullptr;
    bool                          verbose = false;

    // Adding millions of symbls to the DB is slow. Cache them
    struct ArchAndInstallName
    {
        std::string installName;
        std::string arch;

        std::size_t operator()(const ArchAndInstallName& v) const
        {
            return std::hash<std::string>()(v.installName) ^ std::hash<std::string>()(v.arch);
        }

        bool operator==(const ArchAndInstallName& other) const
        {
            return (this->installName == other.installName) && (this->arch == other.arch);
        }

    };

    std::unordered_map<ArchAndInstallName, int64_t, ArchAndInstallName> binaryCache;
    SymbolNameCache symbolNameCache;

    // Used for testing:
    mach_o::Error addExecutableFile(std::string_view path,
                                    mach_o::Platform platform, std::string_view arch,
                                    std::string_view uuid, std::string_view projectName,
                                    int64_t& binaryID);
    mach_o::Error addDylibFile(std::string_view path, std::string_view installName,
                               mach_o::Platform platform, std::string_view arch,
                               std::string_view uuid, std::string_view projectName,
                               int64_t& binaryID);
};

// helper methods to print the output from the symbols cache
VIS_HIDDEN void printResultSummary(std::span<ResultBinary> verifyResults, bool bniOutput,
                                   FILE* summaryLogFile);

VIS_HIDDEN void printResultsSymbolDetails(std::span<ResultBinary> verifyResults, FILE* detailsLogFile);

VIS_HIDDEN void printResultsInternalInformation(std::span<ResultBinary> verifyResults,
                                                std::span<std::pair<std::string, std::string>> rootErrors,
                                                FILE* detailsLogFile);

VIS_HIDDEN void printResultsJSON(std::span<ResultBinary> verifyResults,
                                 std::span<ExportsChangedBinary> exportsChanged,
                                 FILE* jsonFile);

#endif /* SymbolsCache_h */
