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

/*

 This file suppports building and verifying against a symbols database.

 The database contains imports and exports for all shared cache eligible mach-o files
 in a given build.  It also contains the list of re-exported dylibs.

 The main tables are:
 - BINARY: Contains the path, install name, arch, etc, for a given dylib (and in future exe)
 - SYMBOL: Maps from ID to symbol name.  Used only to deduplicate symbol strings
 - SYMBOL_ID_REF: Corresponds to imported (referenced) symbols.  Is a tuple of symbol ID, and the binary IDs of the client and target dylibs
 - SYMBOL_ID_DEF: Corresponds to exported (defined) symbols.  Is a tuple of symbol ID and the dylib which defines the symbol
 - REEXPORT: Corresponds to LC_REEXPORT_DYLIB's.  Contains tuples of umbrella and client dylib.

 The symbols cache can contain arbitrary arch and platform for binaries.  A single database is expected
 to contain all platforms, such as the main OS but also driverKit, etc.

 To verify binaries against a database, the key check is whether a new binary removes a symbol still in use by
 a binary in the cache.  That is, does the new binary cause a SYMBOL_ID_REF to become invalid.  Verification
 is passed all new binaries, so only binaries in the database, and not in the roots passed in, will be verified.

 Re-exports are special.  Instead of storing all re-exports on the umbrella dylib (ie, promoting all UIKitCore SYMBOL_ID_DEF's
 up to UIKit), the actual re-export edges are just recorded.  It is the task of the verify step to walk all re-exports when
 looking to resolve symbols.  This is recursive to support arbitrary tiers of re-exports

 */

#include "SymbolsCache.h"
#include "ClosureFileSystem.h"
#include "FileUtils.h"
#include "Image.h"
#include "JSONReader.h"
#include "MachOFile.h"
#include "Misc.h"
#include "Version32.h"

#include <assert.h>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>
asm(".linker_option \"-lsqlite3\"");

const uint32_t SchemaMajorVersion = 1;

// 1 - the first version
// 2 - added UUID to Binary table
// 3 - added Project to Binary table
const uint32_t SchemaMinorVersion = 3;

const uint32_t MinSupportedSchemaVersion = 1;
const uint32_t MaxSupportedSchemaVersion = 1;

using mach_o::Error;
using mach_o::Fixup;
using mach_o::Header;
using mach_o::Image;
using mach_o::Platform;
using mach_o::PlatformAndVersions;
using mach_o::Symbol;
using mach_o::Version32;
typedef SymbolsCacheBinary::ImportedSymbol ImportedSymbol;

SymbolsCacheBinary::SymbolsCacheBinary(std::string path, Platform platform, std::string arch,
                                       std::string uuid, std::string projectName)
    : path(path), platform(platform), arch(arch), uuid(uuid), projectName(projectName)
{
}

SymbolsCache::SymbolsCache()
{
}

SymbolsCache::SymbolsCache(std::string_view dbPath)
    : dbPath(dbPath)
{
}

SymbolsCache::~SymbolsCache()
{
    if ( !dbPath.empty() && (symbolsDB != nullptr) )
        sqlite3_close(symbolsDB);
}

static Error getSchemaVersion(sqlite3* symbolsDB, Version32& version);

Error SymbolsCache::open()
{
    bool checkSchemaVersion = false;
    if ( dbPath.empty() ) {
        if ( int result = sqlite3_open(":memory:", &symbolsDB) ) {
            return Error("could not open symbols database due to: %s", sqlite3_errmsg(symbolsDB));
        }
    } else {
        // If the database exists on disk, then check its compatible
        if ( fileExists(dbPath) )
            checkSchemaVersion = true;

        if ( int result = sqlite3_open(dbPath.c_str(), &symbolsDB) ) {
            return Error("Could not open symbols database at '%s' due to: %s",
                         dbPath.c_str(), sqlite3_errmsg(symbolsDB));
        }
    }

    if ( checkSchemaVersion ) {
        Version32 version;
        if ( Error err = getSchemaVersion(this->symbolsDB, version) )
            return err;

        if ( (version.major() < MinSupportedSchemaVersion) || (version.major() > MaxSupportedSchemaVersion) ) {
            return Error("Database schema (%d) is not supported.  Only supported schemas are [%d..%d]",
                         version.major(), MinSupportedSchemaVersion, MaxSupportedSchemaVersion);
        }
    }

    return Error();
}

Error SymbolsCache::createTables()
{
    assert(symbolsDB != nullptr);

    // Create table for metadata
    {
        const char* query = "CREATE TABLE IF NOT EXISTS METADATA("
            "SCHEMA_VERSION INTEGER NOT NULL, "
            "SCHEMA_MINOR_VERSION INTEGER NOT NULL, "
            "UNIQUE(SCHEMA_VERSION, SCHEMA_MINOR_VERSION) ON CONFLICT REPLACE"
        ");";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'METADATA' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create table for binaries
    {
        const char* query = "CREATE TABLE IF NOT EXISTS BINARY("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
            "PATH TEXT NOT NULL, "
            "INSTALL_NAME TEXT, "
            "PLATFORM INTEGER NOT NULL, "
            "ARCH TEXT NOT NULL, "
            "UUID TEXT, "
            "PROJECT_NAME TEXT, "
            "UNIQUE(PATH, PLATFORM, ARCH) ON CONFLICT REPLACE"
        ");";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'BINARY' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create table for symbols
    {
        const char* query = "CREATE TABLE IF NOT EXISTS SYMBOL("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
            "NAME TEXT UNIQUE NOT NULL);";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'SYMBOL' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }

        const char* query2 = "CREATE INDEX IF NOT EXISTS SYMBOL_INDEX ON SYMBOL(NAME)";

        char* errorMessage2 = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query2, NULL, 0, &errorMessage2) ) {
            Error err = Error("Could not create index 'SYMBOL' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage2);
            return err;
        }
    }

    // Create table for symbols references
    {
        const char* query = "CREATE TABLE IF NOT EXISTS SYMBOL_ID_REF("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
            "DEF_BINARY_ID INTEGER REFERENCES BINARY NOT NULL, "
            "REF_BINARY_ID INTEGER REFERENCES BINARY NOT NULL, "
            "SYMBOL_ID INTEGER REFERENCES SYMBOL NOT NULL, "
            "UNIQUE(DEF_BINARY_ID, REF_BINARY_ID, SYMBOL_ID) ON CONFLICT REPLACE);";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'SYMBOL_ID_REF' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create view for symbols references
    {
        const char* query = "CREATE VIEW IF NOT EXISTS SYMBOL_REF(DEF_BINARY_ID, REF_BINARY_ID, SYMBOL_NAME) AS "
            "SELECT SYMBOL_ID_REF.DEF_BINARY_ID, SYMBOL_ID_REF.REF_BINARY_ID, SYMBOL.NAME AS SYMBOL_NAME "
            "FROM SYMBOL_ID_REF JOIN SYMBOL "
            "ON SYMBOL_ID_REF.SYMBOL_ID = SYMBOL.ID;";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create view 'SYMBOL_REF' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create table for symbols definitions
    {
        const char* query = "CREATE TABLE IF NOT EXISTS SYMBOL_ID_DEF("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
            "DEF_BINARY_ID INTEGER REFERENCES BINARY NOT NULL, "
            "SYMBOL_ID INTEGER REFERENCES SYMBOL NOT NULL, "
            "UNIQUE(DEF_BINARY_ID, SYMBOL_ID) ON CONFLICT REPLACE);";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'SYMBOL_ID_DEF' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create view for symbols definitions
    {
        const char* query = "CREATE VIEW IF NOT EXISTS SYMBOL_DEF(DEF_BINARY_ID, SYMBOL_NAME) AS "
            "SELECT SYMBOL_ID_DEF.DEF_BINARY_ID, SYMBOL.NAME AS SYMBOL_NAME "
            "FROM SYMBOL_ID_DEF JOIN SYMBOL "
            "ON SYMBOL_ID_DEF.SYMBOL_ID = SYMBOL.ID;";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create view 'SYMBOL_DEF' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    // Create table for re-exports
    {
        const char* query = "CREATE TABLE IF NOT EXISTS REEXPORT("
            "ID INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, "
            "BINARY_ID INTEGER REFERENCES BINARY NOT NULL, "
            "DEP_BINARY_ID INTEGER REFERENCES BINARY NOT NULL, "
            "UNIQUE(BINARY_ID, DEP_BINARY_ID) ON CONFLICT REPLACE);";

        char* errorMessage = nullptr;
        if ( int result = sqlite3_exec(symbolsDB, query, NULL, 0, &errorMessage) ) {
            Error err = Error("Could not create table 'REEXPORT' because: %s", (const char*)errorMessage);
            sqlite3_free(errorMessage);
            return err;
        }
    }

    return Error();
}

static Error columnExists(sqlite3* symbolsDB, std::string_view tableName, std::string_view columnName, bool& exists)
{
    const char* selectQuery = "SELECT COUNT(*) FROM pragma_table_info(?) WHERE name=?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'pragma_table_info' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, tableName.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'pragma_table_info' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 2, columnName.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'pragma_table_info' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<int64_t> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        results.push_back(sqlite3_column_int64(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many pragma_table_info results");
    }

    exists = results.front() != 0;

    return Error::none();
}

static Error getSchemaVersion(sqlite3* symbolsDB, Version32& version)
{
    bool minorVersionExists = false;
    if ( Error err = columnExists(symbolsDB, "METADATA", "SCHEMA_MINOR_VERSION", minorVersionExists) )
        return err;

    if ( minorVersionExists ) {
        const char* selectQuery = "SELECT SCHEMA_VERSION, SCHEMA_MINOR_VERSION FROM METADATA";
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
            Error err = Error("Could not prepare statement for table 'METADATA' because: %s", (const char*)strerror(result));
            return err;
        }

        // Get results
        std::vector<std::pair<int64_t, int64_t>> results;
        while( sqlite3_step(statement) == SQLITE_ROW ) {
            results.push_back({ sqlite3_column_int64(statement, 0), sqlite3_column_int64(statement, 1) });
        }

        sqlite3_finalize(statement);

        if ( results.empty() ) {
            version = Version32(1, 0);
            return Error::none();
        }

        if ( results.size() > 1 ) {
            return Error("Too many schema version results");
        }

        version = Version32(results.front().first, results.front().second);

        return Error::none();
    } else {
        const char* selectQuery = "SELECT SCHEMA_VERSION FROM METADATA";
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
            Error err = Error("Could not prepare statement for table 'METADATA' because: %s", (const char*)strerror(result));
            return err;
        }

        // Get results
        std::vector<int64_t> results;
        while( sqlite3_step(statement) == SQLITE_ROW ) {
            results.push_back(sqlite3_column_int64(statement, 0));
        }

        sqlite3_finalize(statement);

        if ( results.empty() ) {
            version = Version32(1, 0);
            return Error::none();
        }

        if ( results.size() > 1 ) {
            return Error("Too many schema version results");
        }

        version = Version32(results.front(), 0);

        return Error::none();
    }
}

static Error getDylibID(sqlite3* symbolsDB, std::string_view installName,
                        Platform platform, std::string_view arch,
                        std::optional<int64_t>& binaryID)
{
    const char* selectQuery = "SELECT ID FROM BINARY WHERE INSTALL_NAME = ? AND PLATFORM = ? AND ARCH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, installName.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 2, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 3, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<int64_t> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        results.push_back(sqlite3_column_int64(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for dylib: %s", installName.data());
    }

    binaryID = results.front();

    return Error::none();
}

static Error getBinaryUUID(sqlite3* symbolsDB, const std::string_view path,
                           const Platform platform, const std::string_view arch,
                           std::string& binaryUUID)
{
    // Check if the DB is new enough to have the UUID column.  It appeared in 1.2
    {
        Version32 schemaVersion;
        if ( Error err = getSchemaVersion(symbolsDB, schemaVersion) )
            return err;

        if ( schemaVersion < Version32(1, 2) )
            return Error();
    }

    const char* selectQuery = "SELECT UUID FROM BINARY WHERE PATH = ? AND PLATFORM = ? AND ARCH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 2, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 3, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<std::string> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        if ( sqlite3_column_type(statement, 0) != SQLITE_NULL )
            results.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for binary UUID: %s", path.data());
    }

    binaryUUID = results.front();

    return Error::none();
}

static Error getBinaryProject(sqlite3* symbolsDB, const std::string_view path,
                              const Platform platform, const std::string_view arch,
                              std::string& projectName)
{
    // Check if the DB is new enough. The Project column appeared in version 3
    {
        Version32 schemaVersion;
        if ( Error err = getSchemaVersion(symbolsDB, schemaVersion) )
            return err;

        if ( schemaVersion < Version32(1, 3) )
            return Error();
    }

    const char* selectQuery = "SELECT PROJECT_NAME FROM BINARY WHERE PATH = ? AND PLATFORM = ? AND ARCH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 2, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 3, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<std::string> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        if ( sqlite3_column_type(statement, 0) != SQLITE_NULL )
            results.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for binary project name: %s", path.data());
    }

    projectName = results.front();

    return Error::none();
}

static Error getBinaryInstallName(sqlite3* symbolsDB, const std::string_view path,
                                  const Platform platform, const std::string_view arch,
                                  std::string& installName)
{
    const char* selectQuery = "SELECT INSTALL_NAME FROM BINARY WHERE PATH = ? AND PLATFORM = ? AND ARCH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 2, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 3, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<std::string> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        if ( sqlite3_column_type(statement, 0) != SQLITE_NULL )
            results.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for binary install name: %s", path.data());
    }

    installName = results.front();

    return Error::none();
}

static Error getBinaryID(sqlite3* symbolsDB, std::string_view path, std::string_view installName,
                         Platform platform, std::string_view arch,
                         std::optional<int64_t>& binaryID)
{
    const char* selectQuery = "SELECT ID FROM BINARY WHERE PATH = ? AND INSTALL_NAME = ? AND PLATFORM = ? AND ARCH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( installName.empty() ) {
        if ( int result = sqlite3_bind_null(statement, 2) ) {
            Error err = Error("Could not bind null for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    } else {
        if ( int result = sqlite3_bind_text(statement, 2, installName.data(), -1, SQLITE_TRANSIENT) ) {
            Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    }

    if ( int result = sqlite3_bind_int(statement, 3, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 4, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<int64_t> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        results.push_back(sqlite3_column_int64(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for: %s", path.data());
    }

    binaryID = results.front();

    return Error::none();
}

static Error addBinary(sqlite3* symbolsDB, std::string_view path, std::string_view installName,
                       Platform platform, std::string_view arch, std::string_view uuid, std::string_view projectName,
                       int64_t& binaryID)
{
    const char* insertQuery = "INSERT INTO BINARY(PATH, INSTALL_NAME, PLATFORM, ARCH, UUID, PROJECT_NAME) VALUES(?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING RETURNING BINARY.ID";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( installName.empty() ) {
        if ( int result = sqlite3_bind_null(statement, 2) ) {
            Error err = Error("Could not bind null for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    } else {
        if ( int result = sqlite3_bind_text(statement, 2, installName.data(), -1, SQLITE_TRANSIENT) ) {
            Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    }

    if ( int result = sqlite3_bind_int(statement, 3, platform.value()) ) {
        Error err = Error("Could not bind int for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 4, arch.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( uuid.empty() ) {
        if ( int result = sqlite3_bind_null(statement, 5) ) {
            Error err = Error("Could not bind null for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    } else {
        if ( int result = sqlite3_bind_text(statement, 5, uuid.data(), -1, SQLITE_TRANSIENT) ) {
            Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    }

    if ( projectName.empty() ) {
        if ( int result = sqlite3_bind_null(statement, 6) ) {
            Error err = Error("Could not bind null for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    } else {
        if ( int result = sqlite3_bind_text(statement, 6, projectName.data(), -1, SQLITE_TRANSIENT) ) {
            Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    }

    // Get results
    std::vector<int64_t> results;
    while( int result = sqlite3_step(statement) ) {
        if ( result == SQLITE_DONE )
            break;
        if ( result == SQLITE_ROW) {
            results.push_back(sqlite3_column_int64(statement, 0));
        } else {
            Error err = Error("Could not insert into table 'BINARY' because: %s", (const char*)strerror(result));
            return err;
        }
    }

    sqlite3_reset(statement);
    sqlite3_finalize(statement);

    if ( results.empty() ) {
        std::optional<int64_t> maybeBinaryID;
        if ( Error err = getBinaryID(symbolsDB, path, installName, platform, arch, maybeBinaryID) ) {
            return err;
        }

        // Its ok to skip binaries the database doesn't know about.
        if ( !maybeBinaryID.has_value() )
            return Error("No result for binary with path: %s", path.data());

        binaryID = maybeBinaryID.value();
    } else {
        if ( results.size() > 1 ) {
            return Error("Too many binary results for binary: %s", installName.data());
        }

        binaryID = results.front();
    }

    return Error::none();
}

typedef std::pair<int64_t, std::string> SymbolIDAndString;
static Error addSymbolStrings(sqlite3* symbolsDB,
                              std::span<const std::string> strings,
                              std::vector<SymbolIDAndString>& results)
{
    const char* insertQuery = "INSERT INTO SYMBOL(NAME) "
    "VALUES("
    "?"
    ") "
    "ON CONFLICT DO NOTHING RETURNING SYMBOL.ID, SYMBOL.NAME";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
        return Error("Could not prepare statement for table 'SYMBOL' because: %s", (const char*)strerror(result));
    }

    for ( std::string_view str : strings ) {
        if ( int result = sqlite3_bind_text(statement, 1, str.data(), -1, SQLITE_TRANSIENT) ) {
            return Error("Could not bind text for table 'SYMBOL' because: %s", (const char*)strerror(result));
        }

        // printf("inserting: %s %s\n", installName, symbolName);

        // Get results
        while( int result = sqlite3_step(statement) ) {
            if ( result == SQLITE_DONE )
                break;
            if ( result == SQLITE_ROW) {
                results.push_back({ sqlite3_column_int64(statement, 0), (const char*)sqlite3_column_text(statement, 1) });
            } else {
                Error err = Error("Could not insert into table 'SYMBOL' because: %s", (const char*)strerror(result));
                return err;
            }
        }

        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);

    return Error::none();
}

static Error addExports(sqlite3* symbolsDB, int64_t binaryID,
                        std::span<const std::string> exports,
                        const SymbolsCache::SymbolNameCache& symbolNameCache)
{
    const char* insertQuery = "INSERT INTO SYMBOL_ID_DEF(DEF_BINARY_ID, SYMBOL_ID) "
    "VALUES("
    "?, "
    "?"
    ")";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
        return Error("Could not prepare statement for table 'SYMBOL_ID_DEF' because: %s", (const char*)strerror(result));
    }

    for ( std::string_view symbolName : exports ) {
        auto it = symbolNameCache.find(symbolName.data());
        if ( it == symbolNameCache.end() )
            return Error("Could not find symbol name for '%s", symbolName.data());

        if ( int result = sqlite3_bind_int64(statement, 1, binaryID) ) {
            return Error("Could not bind int for table 'SYMBOL_ID_DEF' because: %s", (const char*)strerror(result));
        }

        if ( int result = sqlite3_bind_int64(statement, 2, it->second) ) {
            return Error("Could not bind int for table 'SYMBOL_ID_DEF' because: %s", (const char*)strerror(result));
        }

        if ( int result = sqlite3_step(statement); result != SQLITE_DONE ) {
            return Error("Could not insert into table 'SYMBOL_ID_DEF' because: %s", (const char*)strerror(result));
        }
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);

    return Error::none();
}

static Error addImports(sqlite3* symbolsDB, int64_t refBinaryID,
                        Platform platform, std::string_view arch,
                        std::span<const ImportedSymbol> imports,
                        const SymbolsCache::SymbolNameCache& symbolNameCache)
{
    // Add dependent binaries and record their binary IDs
    std::vector<int64_t> targetBinaryIDs;
    {
        for ( ImportedSymbol importedSymbol : imports ) {
            // The target is an install name string or the binary ID we need
            if ( const int64_t* targetBinaryID = std::get_if<int64_t>(&importedSymbol.targetBinary) ) {
                targetBinaryIDs.push_back(*targetBinaryID);
                continue;
            }

            std::string_view installNameView = std::get<std::string>(importedSymbol.targetBinary);
            int64_t targetBinaryID = 0;
            if ( Error err = addBinary(symbolsDB, installNameView, installNameView, platform, arch, "", "", targetBinaryID) )
                return err;

            targetBinaryIDs.push_back(targetBinaryID);
        }
    }

    // Add symbol refs (imports)
    {
        const char* insertQuery = "INSERT INTO SYMBOL_ID_REF(DEF_BINARY_ID, REF_BINARY_ID, SYMBOL_ID) "
        "VALUES("
        "?, "
        "?, "
        "? "
        ")";
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
            return Error("Could not prepare statement for table 'SYMBOL_ID_REF' because: %s", (const char*)strerror(result));
        }

        assert(imports.size() == targetBinaryIDs.size());
        for ( uint32_t symbolIndex = 0; symbolIndex != imports.size(); ++symbolIndex ) {
            const ImportedSymbol& importedSymbol = imports[symbolIndex];
            int64_t targetBinaryID = targetBinaryIDs[symbolIndex];

            auto it = symbolNameCache.find(importedSymbol.symbolName.data());
            if ( it == symbolNameCache.end() )
                return Error("Could not find symbol name for '%s", importedSymbol.symbolName.data());

            if ( int result = sqlite3_bind_int64(statement, 1, targetBinaryID) ) {
                return Error("Could not bind int for table 'SYMBOL_ID_REF' because: %s", (const char*)strerror(result));
            }

            if ( int result = sqlite3_bind_int64(statement, 2, refBinaryID) ) {
                return Error("Could not bind int for table 'SYMBOL_ID_REF' because: %s", (const char*)strerror(result));
            }

            if ( int result = sqlite3_bind_int64(statement, 3, it->second) ) {
                return Error("Could not bind int for table 'SYMBOL_ID_REF' because: %s", (const char*)strerror(result));
            }

            // printf("inserting: %s %s\n", installName, symbolName);

            if ( int result = sqlite3_step(statement); result != SQLITE_DONE ) {
                return Error("Could not insert into table 'SYMBOL_ID_REF' because: %s", (const char*)strerror(result));
            }
            sqlite3_reset(statement);
        }
        sqlite3_finalize(statement);
    }

    return Error::none();
}

static Error addReexports(sqlite3* symbolsDB, int64_t binaryID,
                          Platform platform, std::string_view arch,
                          std::span<const SymbolsCacheBinary::TargetBinary> reexports)
{
    // Add dependent binaries and record their binary IDs
    std::vector<int64_t> targetBinaryIDs;
    {
        for ( const SymbolsCacheBinary::TargetBinary& reexport : reexports ) {
            // The target is an install name string or the binary ID we need
            if ( const int64_t* targetBinaryID = std::get_if<int64_t>(&reexport) ) {
                targetBinaryIDs.push_back(*targetBinaryID);
                continue;
            }

            std::string_view installNameView = std::get<std::string>(reexport);
            int64_t targetBinaryID = 0;
            if ( Error err = addBinary(symbolsDB, installNameView, installNameView, platform, arch, "", "", targetBinaryID) )
                return err;

            targetBinaryIDs.push_back(targetBinaryID);
        }
    }

    // Add symbol refs (imports)
    {
        const char* insertQuery = "INSERT INTO REEXPORT(BINARY_ID, DEP_BINARY_ID) "
        "VALUES("
        "?, "
        "?"
        ")";
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
            return Error("Could not prepare statement for table 'REEXPORT' because: %s", (const char*)strerror(result));
        }

        assert(reexports.size() == targetBinaryIDs.size());
        for ( int64_t targetBinaryID : targetBinaryIDs ) {
            if ( int result = sqlite3_bind_int64(statement, 1, binaryID) ) {
                return Error("Could not bind int for table 'REEXPORT' because: %s", (const char*)strerror(result));
            }

            if ( int result = sqlite3_bind_int64(statement, 2, targetBinaryID) ) {
                return Error("Could not bind int for table 'REEXPORT' because: %s", (const char*)strerror(result));
            }

            // printf("inserting: %s %s\n", installName, symbolName);

            if ( int result = sqlite3_step(statement); result != SQLITE_DONE ) {
                return Error("Could not insert into table 'REEXPORT' because: %s", (const char*)strerror(result));
            }
            sqlite3_reset(statement);
        }
        sqlite3_finalize(statement);
    }

    return Error::none();
}

static Error addMetadata(sqlite3* symbolsDB)
{
    const char* insertQuery = "INSERT INTO METADATA(SCHEMA_VERSION, SCHEMA_MINOR_VERSION) VALUES(?, ?) ON CONFLICT DO NOTHING";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, insertQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'METADATA' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 1, SchemaMajorVersion) ) {
        Error err = Error("Could not bind text for table 'METADATA' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int(statement, 2, SchemaMinorVersion) ) {
        Error err = Error("Could not bind text for table 'METADATA' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_step(statement); result != SQLITE_DONE ) {
        return Error("Could not insert into table 'METADATA' because: %s", (const char*)strerror(result));
    }

    sqlite3_reset(statement);
    sqlite3_finalize(statement);

    return Error::none();
}

Error SymbolsCache::create()
{
    if ( Error err = open() )
        return err;
    if ( Error err = createTables() )
        return err;
    if ( Error err = addMetadata(this->symbolsDB) )
        return err;
    return Error();
}

namespace {

struct Slice
{
    const Header*   sliceHeader;
    size_t          sliceLength;
    Platform        platform;
};

struct CallbackOnError
{
    typedef void (^Callback)();
    CallbackOnError(Callback callback) : callback(callback) { }
    ~CallbackOnError() {
        if ( callback )
            callback();
    }

    Callback callback;
};

}

static Error getSlicesToAdd(const SymbolsCache::ArchPlatforms& archPlatforms,
                            const dyld3::closure::FileSystem& fileSystem,
                            const void* buffer, uint64_t bufferSize, std::string_view path,
                            std::vector<Slice>& slices)
{
    if ( path.ends_with(".metallib") )
        return Error::none();

    Error parseErr = mach_o::forEachHeader({ (uint8_t*)buffer, bufferSize }, path,
                                           ^(const Header *hdr, size_t sliceLength, bool &stop) {
        std::span<const Platform> supportedPlatforms;
        if ( archPlatforms.empty() ) {
            // support all platforms if there are no archs
        } else if ( auto it = archPlatforms.find(hdr->archName()); it != archPlatforms.end() ) {
            supportedPlatforms = it->second;
        } else {
            return;
        }

        PlatformAndVersions pvs = hdr->platformAndVersions();
        if ( pvs.platform.empty() )
            return;

        // HACK: Pretend zippered are macOS, so that the database doesn't have to care about zippering
        Platform platform;
        if ( (pvs.platform == Platform::zippered) || (pvs.platform == Platform::macCatalyst) )
            platform = Platform::macOS;
        else
            platform = pvs.platform;

        if ( !supportedPlatforms.empty() && (std::find(supportedPlatforms.begin(), supportedPlatforms.end(), platform) == supportedPlatforms.end()) )
            return;

        if ( !hdr->isDylib() && !hdr->isDynamicExecutable() )
            return;

        if ( hdr->isDylib() ) {
            std::string_view installName = hdr->installName();
            std::string_view dylibPath = path;
            if ( installName != dylibPath ) {
                // We now typically require that install names and paths match.  However symlinks may allow us to bring in a path which
                // doesn't match its install name.
                // For example:
                //   /usr/lib/libstdc++.6.0.9.dylib is a real file with install name /usr/lib/libstdc++.6.dylib
                //   /usr/lib/libstdc++.6.dylib is a symlink to /usr/lib/libstdc++.6.0.9.dylib
                // So long as we add both paths (with one as an alias) then this will work, even if dylibs are removed from disk
                // but the symlink remains.
                // Apply the same symlink crawling for dylibs that will install their contents to Cryptex paths but will have
                // install names with the cryptex paths removed.
                char resolvedSymlinkPath[PATH_MAX];
                if ( fileSystem.getRealPath(installName.data(), resolvedSymlinkPath) ) {
                    if ( resolvedSymlinkPath == dylibPath ) {
                        // Symlink is the install name and points to the on-disk dylib
                        //fprintf(stderr, "Symlink works: %s == %s\n", inputFile.path, installName.c_str());
                        dylibPath = installName;
                    }
                } else {
                    // HACK: The build record doesn't have symlinks or anything to allow the above realpath code
                    // to reason about the cryptex. So just look for it specifically
                    if ( dylibPath == (std::string("/System/Cryptexes/OS") + std::string(installName)) )
                        dylibPath = installName;
                }
            }

            const dyld3::MachOFile* mf = (const dyld3::MachOFile*)hdr;
            if ( !mf->canBePlacedInDyldCache(dylibPath.data(), false /* check objc */, ^(const char* format, ...){ }) )
                return;
        }

        slices.push_back({ hdr, sliceLength, platform });
    });

    if ( parseErr ) {
        return parseErr;
    }

    return Error::none();
}

static std::string_view leafName(std::string_view str)
{
    size_t pos = str.rfind('/');
    if ( pos == std::string_view::npos )
        return str;
    return str.substr(pos+1);
}

static mach_o::Error makeBinaryFromJSON(const SymbolsCache::ArchPlatforms& archPlatforms,
                                        const json::Node& rootNode, std::string_view path,
                                        std::string_view projectName,
                                        bool allowExecutables,
                                        std::vector<SymbolsCacheBinary>& binaries)
{
    using json::Node;

    // In XBS we expect trace files to be decompressed along with some helpful preamble.  The key for that is
    // a node called "api-version" so if we see that, we know this file has a certain structure
    Diagnostics diags;
    if ( json::getOptionalValue(diags, rootNode, "api-version") ) {
        // Walk the trace-files[] and then the contents[]
        const Node& traceFilesNode = json::getRequiredValue(diags, rootNode, "trace-files");
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

        for ( const Node& traceFileNode : traceFilesNode.array ) {
            const Node& contentsNode = json::getRequiredValue(diags, traceFileNode, "contents");
            if ( diags.hasError() )
                return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

            for ( const Node& contentNode : contentsNode.array ) {
                if ( Error err = makeBinaryFromJSON(archPlatforms, contentNode, path, projectName, allowExecutables, binaries) )
                    return err;
            }
        }

        return Error::none();
    }

    const Node& versionNode = json::getRequiredValue(diags, rootNode, "version");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    uint64_t jsonVersion = json::parseRequiredInt(diags, versionNode);
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    if ( jsonVersion > 2 ) {
        // Is it ok to silently return?  It allows old tools to ignore new JSON so maybe what we want
        return Error::none();
    }

    // Skip binaries which aren't cache eligible
    const Node* sharedCacheEligibleNode = json::getOptionalValue(diags, rootNode, "shared-cache-eligible");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    if ( (sharedCacheEligibleNode != nullptr) && sharedCacheEligibleNode->value != "yes" )
        return Error::none();

    const Node& archNode = json::getRequiredValue(diags, rootNode, "arch");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    const std::string& archName = json::parseRequiredString(diags, archNode);
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    std::span<const Platform> supportedPlatforms;
    if ( archPlatforms.empty() ) {
        // support all platforms if there are no archs
    } else if ( auto it = archPlatforms.find(archName); it != archPlatforms.end() ) {
        supportedPlatforms = it->second;
    } else {
        return Error::none();
    }

    const Node& platformsNode = json::getRequiredValue(diags, rootNode, "platforms");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    if ( platformsNode.array.empty() )
        return Error::none();

    Platform platform;
    for ( const Node& platformNode : platformsNode.array ) {
        const Node& nameNode = json::getRequiredValue(diags, platformNode, "name");
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

        const std::string& platformName = json::parseRequiredString(diags, nameNode);
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

        Platform foundPlatform = Platform::byName(platformName);

        // HACK: Pretend zippered are macOS, so that the database doesn't have to care about zippering
        if ( (foundPlatform == Platform::zippered) || (foundPlatform == Platform::macCatalyst) )
            foundPlatform = Platform::macOS;

        if ( !supportedPlatforms.empty() && (std::find(supportedPlatforms.begin(), supportedPlatforms.end(), foundPlatform) == supportedPlatforms.end()) )
            continue;

        platform = foundPlatform;
    }

    if ( Error err = platform.valid() )
        return Error::none();

    const Node* installNameNode = json::getOptionalValue(diags, rootNode, "install-name");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    const Node* finalPathNode = json::getOptionalValue(diags, rootNode, "final-output-path");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    if ( !installNameNode && !allowExecutables )
        return Error::none();

    if ( !installNameNode && !finalPathNode )
        return Error::none();

    std::string_view installName;
    if ( installNameNode != nullptr ) {
        installName = json::parseRequiredString(diags, *installNameNode);
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());
    }

    std::string_view finalPath;
    if ( finalPathNode != nullptr ) {
        finalPath = json::parseRequiredString(diags, *finalPathNode);
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());
    } else {
        finalPath = installName;
    }

    const Node* uuidNode = json::getOptionalValue(diags, rootNode, "uuid");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    std::string_view uuid;
    if ( uuidNode ) {
        uuid = json::parseRequiredString(diags, *uuidNode);
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());
    }

    std::vector<SymbolsCacheBinary::ImportedSymbol> importedSymbols;
    std::vector<SymbolsCacheBinary::TargetBinary> reexports;
    const Node* linkedDylibsNode = json::getOptionalValue(diags, rootNode, "linked-dylibs");
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    if ( (linkedDylibsNode != nullptr) && !linkedDylibsNode->array.empty() ) {
        for ( const Node& linkedDylibNode : linkedDylibsNode->array ) {
            const Node& targetInstallNameNode = json::getRequiredValue(diags, linkedDylibNode, "install-name");
            if ( diags.hasError() )
                return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

            std::string_view targetInstallName = json::parseRequiredString(diags, targetInstallNameNode);
            if ( diags.hasError() )
                return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

            if ( !Header::isSharedCacheEligiblePath(targetInstallName.data()) )
                continue;

            const Node& importedSymbolsNode = json::getRequiredValue(diags, linkedDylibNode, "imported-symbols");
            if ( diags.hasError() )
                return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

            if ( !importedSymbolsNode.array.empty() ) {
                importedSymbols.reserve(importedSymbolsNode.array.size());
                for ( const Node& importedSymbol : importedSymbolsNode.array ) {
                    importedSymbols.push_back({ std::string(targetInstallName), importedSymbol.value });
                }
            }

            const Node& attributesNode = json::getRequiredValue(diags, linkedDylibNode, "attributes");
            if ( diags.hasError() )
                return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

            if ( !attributesNode.array.empty() ) {
                for ( const Node& attributeNode : attributesNode.array ) {
                    if ( attributeNode.value == "re-export" )
                        reexports.push_back(std::string(targetInstallName));
                }
            }
        }
    }

    __block std::vector<std::string> exportedSymbols;
    if ( !installName.empty() && Header::isSharedCacheEligiblePath(installName.data()) ) {
        const Node* exportedSymbolsNode = json::getOptionalValue(diags, rootNode, "exports");
        if ( diags.hasError() )
            return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

        if ( (exportedSymbolsNode != nullptr) && !exportedSymbolsNode->array.empty() ) {
            exportedSymbols.reserve(exportedSymbolsNode->array.size());
            for ( const Node& exportedSymbol : exportedSymbolsNode->array )
                exportedSymbols.push_back(exportedSymbol.value);
        }
    }

    SymbolsCacheBinary binary(std::string(finalPath), platform, archName,
                              std::string(uuid), std::string(projectName));
    binary.installName = installName;
    binary.exportedSymbols = std::move(exportedSymbols);
    binary.importedSymbols = std::move(importedSymbols);
    binary.reexportedLibraries = std::move(reexports);
    binary.inputFileName = leafName(path);

    binaries.push_back(std::move(binary));
    return Error::none();
}

Error SymbolsCache::makeBinariesFromJSON(const ArchPlatforms& archPlatforms,
                                         const void* buffer, uint64_t bufferSize, std::string_view path,
                                         std::string_view projectName, bool allowExecutables,
                                         std::vector<SymbolsCacheBinary>& binaries)
{
    using json::Node;

    // The buffer is likely in the "JSON lines" format.  If so, parse each line as its own JSON
    {
        std::string_view wholeString((const char*)buffer, bufferSize);
        while ( !wholeString.empty() ) {
            auto nextNewLinePos = wholeString.find('\n');
            if ( nextNewLinePos == std::string_view::npos )
                break;
            std::string_view line = wholeString.substr(0, nextNewLinePos);
            wholeString = wholeString.substr(line.size() + 1);
            if ( line.empty() )
                continue;

            if ( line.starts_with('{') && line.ends_with('}') ) {
                Diagnostics diags;
                Node rootNode = json::readJSON(diags, line.data(), line.size(), false /* useJSON5 */);
                if ( diags.hasError() )
                    return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

                if ( Error err = makeBinaryFromJSON(archPlatforms, rootNode, path, projectName, allowExecutables, binaries) )
                    return err;
            } else {
                break;
            }
        }

        // If we processed the whole file as JSON lines, then nothing else to do
        if ( wholeString.empty() )
            return Error::none();
    }

    Diagnostics diags;
    Node rootNode = json::readJSON(diags, buffer, bufferSize, false /* useJSON5 */);
    if ( diags.hasError() )
        return Error("Could not parse JSON '%s' because: %s", path.data(), diags.errorMessageCStr());

    return makeBinaryFromJSON(archPlatforms, rootNode, path, projectName, allowExecutables, binaries);
}

Error SymbolsCache::makeBinaries(const ArchPlatforms& archPlatforms,
                                 const dyld3::closure::FileSystem& fileSystem,
                                 const void* buffer, uint64_t bufferSize, std::string_view path,
                                 std::string_view projectName,
                                 std::vector<SymbolsCacheBinary>& binaries)
{
    if ( path.ends_with(".json") )
        return makeBinariesFromJSON(archPlatforms, buffer, bufferSize, path, projectName, false, binaries);

    std::vector<Slice> slices;
    if ( Error err = getSlicesToAdd(archPlatforms, fileSystem, buffer, bufferSize, path, slices) )
        return err;

    if ( slices.empty() )
        return Error::none();

    for ( const Slice& slice : slices ) {
        const Header* mh = slice.sliceHeader;
        Platform platform = slice.platform;
        const char* sliceArch = mh->archName();

        Image image(slice.sliceHeader, slice.sliceLength, Image::MappingKind::unknown);

        // printf("Processing: %s", &path[0]);

        // Add def binary
        std::string_view binaryInstallName;
        if ( const char* installName = mh->installName() )
            binaryInstallName = installName;

        // Add defs (exports)
        __block std::vector<std::string> exportedSymbols;
        if ( const char* installName = mh->installName(); (installName != nullptr) && (installName[0] == '/') ) {
            if ( image.hasExportsTrie() ) {
                image.exportsTrie().forEachExportedSymbol(^(const Symbol& symbol, bool& stopExport) {
                    exportedSymbols.push_back(symbol.name().c_str());
                });
            }
        }

        // Add symbol refs (imports)
        __block std::vector<SymbolsCacheBinary::ImportedSymbol> importedSymbols;
        image.forEachBindTarget(^(const Fixup::BindTarget& targetInfo, bool& stop) {
            // TODO: We should be able to check weak-defs too, by looking at all binaries in the
            // dependency tree of this binary.
            if ( targetInfo.libOrdinal <= 0 )
                return;
            const char* depLoadPath = mh->linkedDylibLoadPath(targetInfo.libOrdinal-1);

            importedSymbols.push_back({ depLoadPath, targetInfo.symbolName.c_str() });
        });

        // Add re-exports
        __block std::vector<SymbolsCacheBinary::TargetBinary> reexports;
        if ( const char* installName = mh->installName(); (installName != nullptr) && (installName[0] == '/') ) {
            mh->forEachLinkedDylib(^(const char* loadPath, mach_o::LinkedDylibAttributes kind, Version32 compatVersion, Version32 curVersion,
                                     bool synthesizedLink, bool& stop) {
                if ( kind.reExport )
                    reexports.push_back(loadPath);
            });
        }

        // Get UUID
        std::string uuidString;
        uuid_t uuid;
        if ( mh->getUuid(uuid) ) {
            uuid_string_t uuidStrBuffer;
            uuid_unparse(uuid, uuidStrBuffer);
            uuidString = uuidStrBuffer;
        }

        std::string_view binaryPath = mh->isDylib() ? binaryInstallName : path;

        SymbolsCacheBinary binary(std::string(binaryPath), platform, sliceArch,
                                  std::string(uuidString), std::string(projectName));
        binary.installName = binaryInstallName;
        binary.exportedSymbols = std::move(exportedSymbols);
        binary.importedSymbols = std::move(importedSymbols);
        binary.reexportedLibraries = std::move(reexports);

        binaries.push_back(std::move(binary));
    }

    return Error::none();
}

Error SymbolsCache::serialize(const uint8_t*& buffer, uint64_t& bufferSize)
{
    sqlite3_exec(symbolsDB, "VACUUM", 0, 0, 0);

    sqlite3_int64 resultSize = 0;
    unsigned char* resultBuffer = sqlite3_serialize(symbolsDB, "main", &resultSize, 0);
    if ( !resultBuffer )
        return("Could not serialize symbols database");

    buffer = resultBuffer;
    bufferSize = resultSize;

    return Error();
}

// Testing
Error SymbolsCache::startTransaction()
{
    char* errorMessage = nullptr;
    if ( int result = sqlite3_exec(symbolsDB, "BEGIN", NULL, 0, &errorMessage) ) {
        Error err = Error("Could not 'BEGIN' because: %s", (const char*)errorMessage);
        sqlite3_free(errorMessage);
        return err;
    }

    return Error::none();
}

Error SymbolsCache::endTransaction()
{
    char* errorMessage = nullptr;
    Error err = Error::none();
    if ( int result = sqlite3_exec(symbolsDB, "COMMIT", NULL, 0, &errorMessage) ) {
        err = Error("Could not 'COMMIT' because: %s", (const char*)errorMessage);
        sqlite3_free(errorMessage);
    }
    return err;
}

Error SymbolsCache::rollbackTransaction()
{
    char* errorMessage = nullptr;
    Error err = Error::none();
    if ( int result = sqlite3_exec(symbolsDB, "ROLLBACK", NULL, 0, &errorMessage) ) {
        err = Error("Could not 'ROLLBACK' because: %s", (const char*)errorMessage);
        sqlite3_free(errorMessage);
    }
    return err;
}

Error SymbolsCache::addExecutableFile(std::string_view path, Platform platform, std::string_view arch,
                                      std::string_view uuid, std::string_view projectName,
                                      int64_t& binaryID)
{
    return ::addBinary(this->symbolsDB, path, "", platform, arch, uuid, projectName, binaryID);
}

Error SymbolsCache::addDylibFile(std::string_view path, std::string_view installName,
                                 Platform platform, std::string_view arch, std::string_view uuid,
                                 std::string_view projectName,
                                 int64_t& binaryID)
{
    return ::addBinary(this->symbolsDB, path, installName, platform, arch, uuid, projectName, binaryID);
}

Error SymbolsCache::addBinaries(std::vector<SymbolsCacheBinary>& binaries)
{
    // Add all entries to the BINARY table
    {
        if ( mach_o::Error err = this->startTransaction() )
            return err;

        __block Error rollbackError = Error::none();
        CallbackOnError callbackOnError(^() { rollbackError = this->rollbackTransaction(); });

        for ( SymbolsCacheBinary& binary : binaries ) {
            int64_t binaryID = 0;
            if ( binary.installName.empty() ) {
                if ( Error err = this->addExecutableFile(binary.path, binary.platform, binary.arch, binary.uuid, binary.projectName, binaryID) )
                    return err;
            } else {
                if ( Error err = this->addDylibFile(binary.path, binary.installName, binary.platform, binary.arch, binary.uuid, binary.projectName, binaryID) )
                    return err;
            }

            binary.binaryID = binaryID;
        }

        if ( mach_o::Error err = this->endTransaction() )
            return err;

        // If we succeeded then don't rollback
        callbackOnError.callback = nullptr;

        if ( rollbackError )
            return std::move(rollbackError);
    }

    // Add all entries to the SYMBOL table
    {
        if ( mach_o::Error err = this->startTransaction() )
            return err;

        __block Error rollbackError = Error::none();
        CallbackOnError callbackOnError(^() { rollbackError = this->rollbackTransaction(); });

        for ( SymbolsCacheBinary& binary : binaries ) {
            if ( !binary.exportedSymbols.empty() ) {
                std::vector<SymbolIDAndString> results;
                if ( Error err = addSymbolStrings(this->symbolsDB, binary.exportedSymbols, results) )
                    return err;

                if ( !results.empty() ) {
                    for ( const SymbolIDAndString& symbolIDAndString : results )
                        this->symbolNameCache[symbolIDAndString.second] = symbolIDAndString.first;
                }
            }

            if ( !binary.importedSymbols.empty() ) {
                std::vector<std::string> symbolNames;
                for ( const SymbolsCacheBinary::ImportedSymbol& importedSymbol : binary.importedSymbols )
                    symbolNames.push_back(importedSymbol.symbolName);

                std::vector<SymbolIDAndString> results;
                if ( Error err = addSymbolStrings(this->symbolsDB, symbolNames, results) )
                    return err;

                if ( !results.empty() ) {
                    for ( const SymbolIDAndString& symbolIDAndString : results )
                        this->symbolNameCache[symbolIDAndString.second] = symbolIDAndString.first;
                }
            }
        }

        if ( mach_o::Error err = this->endTransaction() )
            return err;

        // If we succeeded then don't rollback
        callbackOnError.callback = nullptr;

        if ( rollbackError )
            return std::move(rollbackError);
    }

    // Add all imports (SYMBOL_REF), exports(SYMBOL_DEF) and reexports
    {
        if ( mach_o::Error err = this->startTransaction() )
            return err;

        __block Error rollbackError = Error::none();
        CallbackOnError callbackOnError(^() { rollbackError = this->rollbackTransaction(); });

        for ( SymbolsCacheBinary& binary : binaries ) {
            if ( !binary.exportedSymbols.empty() ) {
                if ( Error err = addExports(this->symbolsDB, binary.binaryID.value(), binary.exportedSymbols, this->symbolNameCache) )
                    return err;
            }

            if ( !binary.importedSymbols.empty() ) {
                if ( Error err = addImports(this->symbolsDB, binary.binaryID.value(), binary.platform, binary.arch, binary.importedSymbols, this->symbolNameCache) )
                    return err;
            }

            if ( !binary.reexportedLibraries.empty() ) {
                if ( Error err = addReexports(this->symbolsDB, binary.binaryID.value(), binary.platform, binary.arch, binary.reexportedLibraries) )
                    return err;
            }
        }

        if ( mach_o::Error err = this->endTransaction() )
            return err;

        // If we succeeded then don't rollback
        callbackOnError.callback = nullptr;

        if ( rollbackError )
            return std::move(rollbackError);
    }

    return Error::none();
}

bool SymbolsCache::containsExecutable(std::string_view path) const
{
    const char* selectQuery = "SELECT PATH FROM BINARY WHERE PATH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        return false;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        return false;
    }

    // Get results
    int count = 0;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        // printf("Got result: %s\n", sqlite3_column_text(statement, 0));
        ++count;
    }

    sqlite3_finalize(statement);

    return (count != 0);
}

bool SymbolsCache::containsDylib(std::string_view path, std::string_view installName) const
{
    const char* selectQuery = "SELECT PATH FROM BINARY WHERE PATH = ? AND INSTALL_NAME = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        return false;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        return false;
    }

    if ( int result = sqlite3_bind_text(statement, 2, installName.data(), -1, SQLITE_TRANSIENT) ) {
        return false;
    }

    // Get results
    int count = 0;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        // printf("Got result: %s\n", sqlite3_column_text(statement, 0));
        ++count;
    }

    sqlite3_finalize(statement);

    return (count != 0);
}

mach_o::Error SymbolsCache::getAllBinaries(std::vector<SymbolsCacheBinary>& binaries) const
{
    bool canGetProjectName = false;

    // Check if the DB is new enough. The Project column appeared in version 3
    {
        Version32 schemaVersion;
        if ( Error err = getSchemaVersion(symbolsDB, schemaVersion) )
            return err;

        canGetProjectName = schemaVersion >= Version32(1, 3);
    }

    const char* selectQueryOld = "SELECT BINARY.PATH, BINARY.ARCH, BINARY.PLATFORM "
    "FROM BINARY "
    "ORDER BY BINARY.PATH";
    const char* selectQueryNew = "SELECT BINARY.PATH, BINARY.ARCH, BINARY.PLATFORM, BINARY.UUID, BINARY.PROJECT_NAME "
    "FROM BINARY "
    "ORDER BY BINARY.PATH";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, canGetProjectName ? selectQueryNew : selectQueryOld, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        const char* path = (const char*)sqlite3_column_text(statement, 0);
        const char* arch = (const char*)sqlite3_column_text(statement, 1);
        int64_t platform = sqlite3_column_int64(statement, 2);
        const char* uuid = nullptr;
        const char* projectName = nullptr;

        if ( canGetProjectName ) {
            if ( sqlite3_column_type(statement, 3) != SQLITE_NULL )
                uuid = (const char*)sqlite3_column_text(statement, 3);
            if ( sqlite3_column_type(statement, 4) != SQLITE_NULL )
                projectName = (const char*)sqlite3_column_text(statement, 4);
        }
        binaries.push_back({
            path, Platform((uint32_t)platform), arch,
            (uuid != nullptr ? uuid : ""),
            (projectName != nullptr ? projectName : "")
        });
    }

    sqlite3_finalize(statement);

    return Error::none();
}

std::vector<SymbolsCacheBinary::ImportedSymbol> SymbolsCache::getImports(std::string_view path) const
{
    const char* selectQuery = "SELECT DEF_BINARY.INSTALL_NAME, SYMBOL_REF.SYMBOL_NAME "
    "FROM SYMBOL_REF "
    "JOIN BINARY AS REF_BINARY ON SYMBOL_REF.REF_BINARY_ID = REF_BINARY.ID "
    "JOIN BINARY AS DEF_BINARY ON SYMBOL_REF.DEF_BINARY_ID = DEF_BINARY.ID "
    "WHERE REF_BINARY.PATH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        return { };
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        return { };
    }

    // Get results
    std::vector<SymbolsCacheBinary::ImportedSymbol> imports;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        SymbolsCacheBinary::ImportedSymbol imp;
        imp.targetBinary = (const char*)sqlite3_column_text(statement, 0);
        imp.symbolName = (const char*)sqlite3_column_text(statement, 1);
        imports.push_back(imp);
    }

    sqlite3_finalize(statement);

    return imports;
}

Error SymbolsCache::getAllImports(std::vector<SymbolsCache::ImportedSymbol>& imports) const
{
    const char* selectQuery = "SELECT REF_BINARY.ARCH, REF_BINARY.PATH, DEF_BINARY.INSTALL_NAME, SYMBOL_REF.SYMBOL_NAME "
    "FROM SYMBOL_REF "
    "JOIN BINARY AS REF_BINARY ON SYMBOL_REF.REF_BINARY_ID = REF_BINARY.ID "
    "JOIN BINARY AS DEF_BINARY ON SYMBOL_REF.DEF_BINARY_ID = DEF_BINARY.ID "
    "ORDER BY REF_BINARY.PATH, DEF_BINARY.INSTALL_NAME, SYMBOL_NAME";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'SYMBOL_REF' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        const char* archName = (const char*)sqlite3_column_text(statement, 0);
        const char* clientPath = (const char*)sqlite3_column_text(statement, 1);
        const char* installName = (const char*)sqlite3_column_text(statement, 2);
        const char* symbolName = (const char*)sqlite3_column_text(statement, 3);
        imports.push_back({ archName, clientPath, installName, symbolName });
    }

    sqlite3_finalize(statement);

    return Error::none();
}

static Error getBinaryIDForPath(sqlite3* symbolsDB, std::string_view path,
                                std::optional<int64_t>& binaryID)
{
    const char* selectQuery = "SELECT ID FROM BINARY WHERE PATH = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 1, path.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for table 'BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    std::vector<int64_t> results;
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        results.push_back(sqlite3_column_int64(statement, 0));
    }

    sqlite3_finalize(statement);

    if ( results.empty() )
        return Error::none();

    if ( results.size() > 1 ) {
        return Error("Too many binary results for: %s", path.data());
    }

    binaryID = results.front();

    return Error::none();
}

static Error getExports(sqlite3* symbolsDB, int64_t binaryID,
                        std::vector<std::string>& exports)
{
    const char* selectQuery = "SELECT SYMBOL_NAME FROM SYMBOL_DEF WHERE SYMBOL_DEF.DEF_BINARY_ID = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'SYMBOL_DEF' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int64(statement, 1, binaryID) ) {
        Error err = Error("Could not bind int for table 'SYMBOL_DEF' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        exports.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    return Error::none();
}

std::vector<std::string> SymbolsCache::getExports(std::string_view path) const
{
    std::optional<int64_t> binaryID;
    if ( Error err = getBinaryIDForPath(this->symbolsDB, path, binaryID) ) {
        return { };
    }

    // Its ok to skip binaries the database doesn't know about.
    if ( !binaryID.has_value() )
        return { };

    // Get the exports from the database
    std::vector<std::string> exports;
    if ( Error err = ::getExports(this->symbolsDB, binaryID.value(), exports) ) {
        return { };
    }

    return exports;
}

Error SymbolsCache::getAllExports(std::vector<ExportedSymbol>& exports) const
{
    const char* selectQuery = "SELECT BINARY.ARCH, BINARY.INSTALL_NAME, SYMBOL_DEF.SYMBOL_NAME "
    "FROM SYMBOL_DEF JOIN BINARY ON SYMBOL_DEF.DEF_BINARY_ID = BINARY.ID "
    "ORDER BY INSTALL_NAME, SYMBOL_NAME";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for table 'SYMBOL_DEF' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        const char* archName = (const char*)sqlite3_column_text(statement, 0);
        const char* installName = (const char*)sqlite3_column_text(statement, 1);
        const char* symbolName = (const char*)sqlite3_column_text(statement, 2);
        exports.push_back({ archName, installName, symbolName });
    }

    sqlite3_finalize(statement);

    return Error::none();
}

static Error getReexports(sqlite3* symbolsDB, int64_t binaryID,
                          std::vector<std::string>& reexports)
{
    const char* selectQuery = "SELECT INSTALL_NAME "
    "FROM BINARY JOIN REEXPORT ON BINARY.ID = REEXPORT.DEP_BINARY_ID "
    "WHERE REEXPORT.BINARY_ID = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for join 'BINARY/REEXPORT' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int64(statement, 1, binaryID) ) {
        Error err = Error("Could not bind int for join 'BINARY/REEXPORT' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        reexports.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    return Error::none();
}

std::vector<std::string> SymbolsCache::getReexports(std::string_view path) const
{
    std::optional<int64_t> binaryID;
    if ( Error err = getBinaryIDForPath(this->symbolsDB, path, binaryID) ) {
        return { };
    }

    // Its ok to skip binaries the database doesn't know about.
    if ( !binaryID.has_value() )
        return { };

    // Get the reexports from the database
    std::vector<std::string> reexports;
    if ( Error err = ::getReexports(this->symbolsDB, binaryID.value(), reexports) ) {
        return { };
    }

    return reexports;
}

static Error getUsesOfExport(sqlite3* symbolsDB, int64_t binaryID,
                             std::string_view exportedSymbol,
                             std::vector<std::string>& clientBinaryPaths)
{
    const char* selectQuery = "SELECT BINARY.PATH "
    "FROM SYMBOL_REF JOIN BINARY "
    "ON SYMBOL_REF.REF_BINARY_ID = BINARY.ID "
    "WHERE SYMBOL_REF.DEF_BINARY_ID = ? AND SYMBOL_REF.SYMBOL_NAME = ?";
    sqlite3_stmt *statement = nullptr;
    if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
        Error err = Error("Could not prepare statement for join 'SYMBOL_REF/BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_int64(statement, 1, binaryID) ) {
        Error err = Error("Could not bind int for join 'SYMBOL_REF/BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    if ( int result = sqlite3_bind_text(statement, 2, exportedSymbol.data(), -1, SQLITE_TRANSIENT) ) {
        Error err = Error("Could not bind text for join 'SYMBOL_REF/BINARY' because: %s", (const char*)strerror(result));
        return err;
    }

    // Get results
    while( sqlite3_step(statement) == SQLITE_ROW ) {
        clientBinaryPaths.push_back((const char*)sqlite3_column_text(statement, 0));
    }

    sqlite3_finalize(statement);

    return Error::none();
}

struct BinaryKey
{
    std::string_view installNameOrPath;
    Platform platform;
    std::string_view arch;
};

static bool operator==(const BinaryKey& a, const BinaryKey& b) {
    return (a.installNameOrPath == b.installNameOrPath) && (a.platform == b.platform) && (a.arch == b.arch);
}

namespace std
{

template<>
struct std::hash<BinaryKey>
{
    uint64_t operator()(const BinaryKey& val) const
    {
        uint64_t hash = 0;
        hash = hash ^ std::hash<std::string_view>{}(val.installNameOrPath) << 0;
        hash = hash ^ std::hash<uint32_t>{}(val.platform.value()) << 32;
        hash = hash ^ std::hash<std::string_view>{}(val.arch) << 48;
        return hash;
    }
};

} // namespace std

Error SymbolsCache::checkNewBinaries(bool warnOnRemovedSymbols, ExecutableMode executableMode,
                                     std::vector<SymbolsCacheBinary>&& binaries,
                                     const BinaryProjects& binaryProjects,
                                     std::vector<ResultBinary>& results,
                                     std::vector<mach_o::Error>& internalWarnings,
                                     std::vector<ExportsChangedBinary>* changedExports) const
{
    // Split out in to OS dylibs vs other binaries
    // We only want to verify the exports from OS binaries
    std::vector<SymbolsCacheBinary>                     osDylibs;
    std::vector<SymbolsCacheBinary*>                    otherBinaries;
    std::unordered_map<BinaryKey, SymbolsCacheBinary*>  osDylibMap;
    std::unordered_map<BinaryKey, SymbolsCacheBinary*>  newClientsMap;

    for ( SymbolsCacheBinary& binary : binaries ) {
        if ( binary.installName.starts_with('/') )
            osDylibs.push_back(binary);
        else
            otherBinaries.push_back(&binary);
    }

    for ( SymbolsCacheBinary& binary : osDylibs ) {
        osDylibMap[{ binary.installName, binary.platform, binary.arch }] = &binary;
        newClientsMap[{ binary.path, binary.platform, binary.arch }] = &binary;
    }

    for ( SymbolsCacheBinary* binary : otherBinaries )
        newClientsMap[{ binary->path, binary->platform, binary->arch }] = binary;

    // Early exit if no binaries with new exports.  Not sure if we'd ever want to verify
    // the other binaries anyway. In theory their imports should be valid as they were just rebuilt
    if ( osDylibs.empty() )
        return Error::none();

    // Promote re-exports to make it look like the top-level dylib exports them. This will line up with
    // imports from other binaries which are looking for the exports in the top-level dylib
    {
        std::list<SymbolsCacheBinary*> worklist;
        for ( SymbolsCacheBinary& binary : osDylibs )
            worklist.push_back(&binary);

        std::unordered_map<BinaryKey, SymbolsCacheBinary*> processedBinaries;
        std::unordered_map<BinaryKey, std::unique_ptr<SymbolsCacheBinary>> databaseBinaries;
        while ( !worklist.empty() ) {
            SymbolsCacheBinary* binary = worklist.front();
            worklist.pop_front();

            if ( binary->installName.empty() )
                continue;

            // If we have no re-exports, then this binary is done
            if ( binary->reexportedLibraries.empty() ) {
                processedBinaries[{ binary->installName, binary->platform, binary->arch }] = binary;
                continue;
            }

            // Check if we need to put this binary back in the worklist to wait on deps
            bool waitOnDeps = false;
            for ( const SymbolsCacheBinary::TargetBinary& reexportTarget : binary->reexportedLibraries ) {
                std::string reexport = std::get<std::string>(reexportTarget);
                if ( !processedBinaries.count({ reexport, binary->platform, binary->arch }) ) {
                    // Unprocessed dep.  Let see if its even a dep we know about
                    if ( osDylibMap.find({ reexport, binary->platform, binary->arch }) != osDylibMap.end() ) {
                        // new binary. We'll get to it later, so just put this back in the queue
                        waitOnDeps = true;
                        break;
                    } else if ( databaseBinaries.count({ reexport, binary->platform, binary->arch }) ) {
                        // We know about this binary, but didn't process it yet
                        waitOnDeps = true;
                        break;
                    } else {
                        // unknown binary.  Let see if its in the database
                        std::optional<int64_t> binaryID;
                        if ( Error err = getDylibID(this->symbolsDB, reexport, binary->platform, binary->arch, binaryID) ) {
                            continue;
                        }

                        // Its ok to skip binaries the database doesn't know about.
                        if ( !binaryID.has_value() )
                            continue;

                        // Get the exports from the database
                        std::vector<std::string> exports;
                        if ( Error err = ::getExports(this->symbolsDB, binaryID.value(), exports) ) {
                            // FIXME: What should we do here? For now log the error and skip the binary
                            internalWarnings.push_back(Error("Skipping re-exported binary due to getExports(): %s", err.message()));
                            continue;
                        }

                        // Get the exports from the database
                        std::vector<std::string> reexports;
                        if ( Error err = ::getReexports(this->symbolsDB, binaryID.value(), reexports) ) {
                            // FIXME: What should we do here? For now log the error and skip the binary
                            internalWarnings.push_back(Error("Skipping re-exported binary due to getReexports(): %s", err.message()));
                            continue;
                        }

                        std::unique_ptr<SymbolsCacheBinary> newBinary = std::make_unique<SymbolsCacheBinary>(reexport, binary->platform, binary->arch, "", "");
                        newBinary->path = reexport;
                        newBinary->installName = reexport;
                        newBinary->exportedSymbols = std::move(exports);

                        for ( const std::string& reexportedLibrary : reexports )
                            newBinary->reexportedLibraries.push_back(reexportedLibrary);

                        worklist.push_back(newBinary.get());
                        databaseBinaries[{ newBinary->installName, binary->platform, binary->arch }] = std::move(newBinary);

                        waitOnDeps = true;
                        break;
                    }
                }
            }

            if ( waitOnDeps ) {
                worklist.push_back(binary);
                continue;
            }

            // All deps that we could find should be done.  Promote their symbols up to this binary
            for ( SymbolsCacheBinary::TargetBinary reexportTarget : binary->reexportedLibraries ) {
                std::string_view reexport = std::get<std::string>(reexportTarget);
                if ( auto it = processedBinaries.find({ reexport, binary->platform, binary->arch }); it != processedBinaries.end() ) {
                    binary->exportedSymbols.insert(binary->exportedSymbols.end(),
                                                   it->second->exportedSymbols.begin(), it->second->exportedSymbols.end());
                }
            }
            processedBinaries[{ binary->installName, binary->platform, binary->arch }] = binary;
        }
    }

    std::map<std::string, std::string_view> rootsPathsForErrorCases;

    // For each OS dylib, compare its exports against the exports in the database.  If it removes a
    // symbol then error out if that symbol has refs
    for ( SymbolsCacheBinary& binary : osDylibs ) {
        std::optional<int64_t> binaryID;
        if ( Error err = getDylibID(this->symbolsDB, binary.installName, binary.platform, binary.arch, binaryID) ) {
            // FIXME: What should we do here? For now log the error and skip the binary
            internalWarnings.push_back(Error("Skipping binary due to getDylibID(): %s", err.message()));
            continue;
        }

        // Its ok to skip binaries the database doesn't know about.
        if ( !binaryID.has_value() ) {
            if ( verbose )
                printf("Skipping binary as it doesn't exist in the database: %s\n", binary.installName.c_str());
            continue;
        }

        // Get the exports from the database
        std::vector<std::string> exports;
        if ( Error err = ::getExports(this->symbolsDB, binaryID.value(), exports) ) {
            // FIXME: What should we do here? For now log the error and skip the binary
            internalWarnings.push_back(Error("Skipping binary due to getExports(): %s", err.message()));
            continue;
        }

        // Add in symbols from re-exports
        {
            // Get the exports from the database
            std::vector<std::string> reexports;
            if ( Error err = ::getReexports(this->symbolsDB, binaryID.value(), reexports) ) {
                // FIXME: What should we do here? For now log the error and skip the binary
                internalWarnings.push_back(Error("Skipping re-exported binary due to getReexports(): %s", err.message()));
                continue;
            }

            if ( !reexports.empty() ) {
                std::list<std::string> worklist;
                worklist.insert(worklist.end(), reexports.begin(), reexports.end());

                std::set<std::string> processedBinaries;
                std::vector<int64_t> reexportedBinaries;
                while ( !worklist.empty() ) {
                    std::string reexport = worklist.front();
                    worklist.pop_front();

                    if ( processedBinaries.count(reexport) )
                        continue;

                    // unknown binary.  Let see if its in the database
                    std::optional<int64_t> reexportBinaryID;
                    if ( Error err = getDylibID(this->symbolsDB, reexport, binary.platform, binary.arch, reexportBinaryID) ) {
                        continue;
                    }

                    // Its ok to skip binaries the database doesn't know about.
                    if ( !reexportBinaryID.has_value() )
                        continue;

                    processedBinaries.insert(reexport);
                    reexportedBinaries.push_back(reexportBinaryID.value());

                    // See if there are more re-exports to add
                    std::vector<std::string> nextReexports;
                    if ( Error err = ::getReexports(this->symbolsDB, reexportBinaryID.value(), nextReexports) ) {
                        // FIXME: What should we do here? For now log the error and skip the binary
                        internalWarnings.push_back(Error("Skipping re-exported binary due to getReexports(): %s", err.message()));
                        continue;
                    }

                    worklist.insert(worklist.end(), nextReexports.begin(), nextReexports.end());
                }

                for ( int64_t reexportedBinaryID : reexportedBinaries ) {
                    std::vector<std::string> reexportedExports;
                    if ( Error err = ::getExports(this->symbolsDB, reexportedBinaryID, reexportedExports) ) {
                        // FIXME: What should we do here? For now log the error and skip the binary
                        internalWarnings.push_back(Error("Skipping binary due to getExports(): %s", err.message()));
                        continue;
                    }

                    exports.insert(exports.end(), reexportedExports.begin(), reexportedExports.end());
                }
            }
        }

        std::string binaryProject;
        if ( Error err = getBinaryProject(this->symbolsDB, binary.path, binary.platform, binary.arch, binaryProject) ) {
            // No project is ok. We can continue without it
        }

        // Work out if any exports were removed
        std::set<std::string_view> removedExports;
        removedExports.insert(exports.begin(), exports.end());
        for ( std::string_view exp : binary.exportedSymbols )
            removedExports.erase(exp);

        if ( changedExports != nullptr ) {
            // Find out if we added exports
            std::set<std::string_view> addedExports;
            addedExports.insert(binary.exportedSymbols.begin(), binary.exportedSymbols.end());
            for ( std::string_view exp : exports )
                addedExports.erase(exp);

            for ( std::string_view exp : removedExports ) {
                ExportsChangedBinary result;
                result.installName = binary.path;
                result.arch = binary.arch;
                result.uuid = binary.uuid;
                result.projectName = binaryProject;
                result.symbolName = exp;
                result.wasAdded = false;

                changedExports->push_back(std::move(result));
            }

            for ( std::string_view exp : addedExports ) {
                ExportsChangedBinary result;
                result.installName = binary.path;
                result.arch = binary.arch;
                result.uuid = binary.uuid;
                result.projectName = binaryProject;
                result.symbolName = exp;
                result.wasAdded = true;

                changedExports->push_back(std::move(result));
            }
        }

        if ( removedExports.empty() ) {
            if ( verbose )
                printf("Skipping binary as it didn't remove any used exports: %s\n", binary.installName.c_str());
            continue;
        }

        // HACK!: A few B&I projects build multiple copies of the same binary, and those are confused for each other
        // For now skip errors from these projects until we can handle them.  They'll still be caught when using a mach-o, just not JSON
        if ( binary.inputFileName.ends_with(".json") ) {
            if ( binary.installName == "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox" )
                continue;
            if ( binary.installName == "/usr/lib/libNFC_HAL.dylib" )
                continue;
            if ( binary.installName == "/System/Library/PrivateFrameworks/WiFiPeerToPeer.framework/WiFiPeerToPeer" )
                continue;
            if ( binary.installName == "/usr/lib/libz.1.dylib" )
                continue;

            // Filter out LAR and _tests projects
            // Note project name looks something like: dyld_tests-version.json
            if ( binary.inputFileName.find("_tests-") != std::string_view::npos )
                continue;
            if ( binary.inputFileName.find("_lar-") != std::string_view::npos )
                continue;
        }

        // If we removed exports, now we need to see if they have uses
        for ( std::string_view exp : removedExports ) {
            std::vector<std::string> clientPaths;
            if ( Error err = getUsesOfExport(this->symbolsDB, binaryID.value(), exp, clientPaths) ) {
                // FIXME: What should we do here? For now log the error and skip the binary export
                internalWarnings.push_back(Error("Skipping binary export due to getUsesOfExport(): %s", err.message()));
                continue;
            }

            // No uses.  Skip this one
            if ( clientPaths.empty() ) {
                if ( warnOnRemovedSymbols )
                    internalWarnings.push_back(Error("Binary '%s' removing unused export: '%s'", binary.path.data(), exp.data()));
                continue;
            }

            for ( std::string_view path : clientPaths ) {
                // If this client was also rebuilt, then filter it out if it doesn't use this symbol any more
                std::string clientUUID;
                std::string clientRootPath;
                std::string clientProject;
                bool warnOnClient = false;

                // Skip executables and non-shared cache dylibs if we aren't verifying them
                {
                    std::string clientInstallName;
                    if ( Error err = getBinaryInstallName(this->symbolsDB, path, binary.platform, binary.arch, clientInstallName) ) {
                        // Skip binaries if their install name generates some kind of error
                        internalWarnings.push_back(Error("Skipping binary export due to getBinaryInstallName(): %s", err.message()));
                        continue;
                    }

                    bool isCacheEligible = false;
                    if ( !clientInstallName.empty() ) {
                        isCacheEligible = Header::isSharedCacheEligiblePath(clientInstallName.data());
                    }

                    switch ( executableMode ) {
                        case ExecutableMode::off:
                            // This means we're verifying only shared cache dylibs.  Skip everything else
                            if ( !isCacheEligible )
                                continue;
                            break;
                        case ExecutableMode::warn:
                            // If we later find issues with this client, record them as errors
                            // if its from the shared cache, but warnings otherwise
                            if ( !isCacheEligible )
                                warnOnClient = true;
                            break;
                        case ExecutableMode::error:
                            // anu issues found here will be errors
                            break;
                    }
                }

                if ( Error err = getBinaryProject(this->symbolsDB, path, binary.platform, binary.arch, clientProject) ) {
                    // No project is ok. We can continue without it
                }
                if ( auto it = newClientsMap.find({ path, binary.platform, binary.arch }); it != newClientsMap.end() ) {
                    // FIXME: Do we need a map?
                    SymbolsCacheBinary* clientBinary = it->second;
                    auto importIt = std::find_if(clientBinary->importedSymbols.begin(),
                                                 clientBinary->importedSymbols.end(),
                                                 [&](const SymbolsCacheBinary::ImportedSymbol& elt) {
                        if ( elt.symbolName != exp )
                            return false;
                        if ( const std::string* installName = std::get_if<std::string>(&elt.targetBinary) )
                            return *installName == binary.installName;
                        return false;
                    });
                    if ( importIt == clientBinary->importedSymbols.end() ) {
                        // No uses of this export, skip this one
                        continue;
                    }
                    clientRootPath = clientBinary->rootPath;
                    clientUUID = clientBinary->uuid;
                } else {
                    // See if the broken client is actually a project we have a root for.  If so, ignore it
                    // as perhaps it was deleted or moved
                    if ( !binaryProjects.empty() && !clientProject.empty() ) {
                        if ( binaryProjects.count(clientProject) )
                            continue;
                    }

                    // See if we can get a UUID from the database
                    if ( Error err = getBinaryUUID(this->symbolsDB, path, binary.platform, binary.arch, clientUUID) ) {
                        // No UUID is ok. We can continue without it
                    }
                }

                ResultBinary result;
                result.installName = binary.path;
                result.arch = binary.arch;
                result.uuid = binary.uuid;
                result.rootPath = binary.rootPath;
                result.projectName = binaryProject;
                result.warn = warnOnClient;

                result.client.path = path;
                result.client.uuid = clientUUID;
                result.client.rootPath = clientRootPath;
                result.client.projectName = clientProject;
                result.client.symbolName = exp;

                results.push_back(std::move(result));
            }
        }
    }

    return Error::none();
}

Error SymbolsCache::dump() const
{
    std::vector<std::string> tableNames;
    {
        const char* selectQuery = "SELECT tbl_name FROM sqlite_master WHERE type = 'table';";
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery, -1, &statement, 0) ) {
            Error err = Error("Could not prepare statement for tables because: %s", (const char*)strerror(result));
            return err;
        }

        // Get results
        while( sqlite3_step(statement) == SQLITE_ROW ) {
            tableNames.push_back((const char*)sqlite3_column_text(statement, 0));
        }

        sqlite3_finalize(statement);
    }

    if ( tableNames.empty() ) {
        printf("Empty database\n");
        return Error::none();
    }

    for ( std::string_view tableName : tableNames ) {
        printf("Table: %s\n", tableName.data());

        std::string selectQuery = std::string("SELECT * FROM ") + tableName.data();
        sqlite3_stmt *statement = nullptr;
        if ( int result = sqlite3_prepare_v2(symbolsDB, selectQuery.c_str(), -1, &statement, 0) ) {
            Error err = Error("Could not prepare statement for table '%s' because: %s",
                              tableName.data(), (const char*)strerror(result));
            return err;
        }

        // Get results
        while( sqlite3_step(statement) == SQLITE_ROW ) {
            int numColumns = sqlite3_column_count(statement);
            bool needsComma = false;
            for ( int i = 0; i != numColumns; ++i ) {
                if ( needsComma )
                    printf(", ");
                printf("%s", (const char*)sqlite3_column_text(statement, i));
                needsComma = true;
            }
            printf("\n");
        }
        printf("\n");

        sqlite3_finalize(statement);
    }

    return Error::none();
}

//
// MARK: --- helper methods to output results ---
//

void printResultSummary(std::span<ResultBinary> verifyResults, bool bniOutput,
                        FILE* summaryLogFile)
{
    std::set<std::string> errorClientProjects;
    std::set<std::string> warnClientProjects;

    // Get the projects which are errors, then the list which are only warnings
    for ( const ResultBinary& result : verifyResults ) {
        if ( result.warn )
            continue;

        if ( result.client.projectName.empty())
            continue;

        errorClientProjects.insert(result.client.projectName);
    }

    // Get the projects which are errors, then the list which are only warnings
    for ( const ResultBinary& result : verifyResults ) {
        if ( !result.warn )
            continue;

        if ( result.client.projectName.empty())
            continue;

        // Skip projects also in the error list
        if ( errorClientProjects.count(result.client.projectName) )
            continue;

        warnClientProjects.insert(result.client.projectName);
    }

    if ( errorClientProjects.empty() && warnClientProjects.empty() )
        return;

    fprintf(summaryLogFile, "--- Summary ---\n\n");

    if ( !errorClientProjects.empty() )
        fprintf(summaryLogFile, "Error: some projects have removed symbols\n\n");
    else
        fprintf(summaryLogFile, "Warning: some projects have removed symbols\n\n");

    fprintf(summaryLogFile, "Expected resolution is to rebuild dependencies\n\n");

    auto printProjects = [&](const std::set<std::string>& clientProjects) {
        if ( bniOutput ) {
            fprintf(summaryLogFile, "Run command: xbs dispatch addProjects");
            for ( std::string_view project : clientProjects )
                fprintf(summaryLogFile, " %s", project.data());
        } else {
            fprintf(summaryLogFile, "Add the following to your submission notes, or container\n");
            fprintf(summaryLogFile, "  REBUILD_DEPENDENCIES=");
            bool needsComma = false;
            for ( std::string_view project : clientProjects ) {
                if ( needsComma )
                    fprintf(summaryLogFile, ",");
                else
                    needsComma = true;
                fprintf(summaryLogFile, "%s", project.data());
            }
        }
        fprintf(summaryLogFile, "\n\n");
    };

    if ( !errorClientProjects.empty() )
        printProjects(errorClientProjects);

    if ( !warnClientProjects.empty() )
        printProjects(warnClientProjects);
}

void printResultsSymbolDetails(std::span<ResultBinary> verifyResults, FILE* detailsLogFile)
{
    struct ProjectResult
    {
        struct Client
        {
            std::string path;
            std::string uuid;
            std::set<std::string> symbols;
        };

        struct ClientProject
        {
            // map from path to its results
            std::map<std::string, Client> clients;
        };

        struct Dylib
        {
            std::string uuid;

            // map from project name to its clients
            std::map<std::string, ClientProject> clientProjects;
        };

        // map from install name to its results
        std::map<std::string, Dylib> dylibs;
    };

    // map from project name to its results
    // loop twice. First iteration prints errors, second prints warnings
    for ( bool errors : { true, false } ) {
        std::map<std::string, ProjectResult> failingProjects;
        for ( const ResultBinary& result : verifyResults ) {
            if ( result.warn ) {
                // result is just a warning.  Ok if we are generating warnings, but not if errors
                if ( errors )
                    continue;
            } else {
                // result is an error.  Ok if we are generating errors, but not if warnings
                if ( !errors )
                    continue;
            }

            std::string projectName = result.projectName.empty() ? "<unknown project>" : result.projectName;
            std::string clientProjectName = result.client.projectName.empty() ? "<unknown project>" : result.client.projectName;

            ProjectResult&          projectResult = failingProjects[projectName];
            ProjectResult::Dylib&   dylib = projectResult.dylibs[result.installName];

            dylib.uuid = result.uuid;
            ProjectResult::ClientProject&   clientProject = dylib.clientProjects[clientProjectName];
            ProjectResult::Client&          client = clientProject.clients[result.client.path];
            client.path = result.client.path;
            client.uuid = result.client.uuid;
            client.symbols.insert(result.client.symbolName);
        }

        if ( failingProjects.empty() )
            continue;

        fprintf(detailsLogFile, "--- Detailed symbol information (%s) ---\n\n",
                errors ? "errors" : "warnings");

        for ( std::pair<std::string, ProjectResult> result : failingProjects ) {
            fprintf(detailsLogFile, "%s:\n", result.first.data());
            for ( std::pair<std::string, ProjectResult::Dylib> dylib : result.second.dylibs ) {
                std::string dylibUUID;
                if ( !dylib.second.uuid.empty())
                    dylibUUID = " (" + dylib.second.uuid + ")";
                fprintf(detailsLogFile, "  %s%s:\n", dylib.first.data(), dylibUUID.data());

                for ( std::pair<std::string, ProjectResult::ClientProject> clientProject : dylib.second.clientProjects ) {
                    fprintf(detailsLogFile, "    %s:\n", clientProject.first.data());
                    for ( std::pair<std::string, ProjectResult::Client> client : clientProject.second.clients ) {
                        std::string clientUUID;
                        if ( !client.second.uuid.empty())
                            clientUUID = " (" + client.second.uuid + ")";
                        fprintf(detailsLogFile, "      %s%s:\n", client.first.data(), clientUUID.data());
                        for ( std::string_view symbolName : client.second.symbols )
                            fprintf(detailsLogFile, "        %s\n", symbolName.data());
                    }
                }
            }
            fprintf(detailsLogFile, "\n");
        }
    }
}

void printResultsInternalInformation(std::span<ResultBinary> verifyResults,
                                     std::span<std::pair<std::string, std::string>> rootErrors,
                                     FILE* detailsLogFile)
{
    std::set<std::string> usedRootPaths;
    for ( const ResultBinary& result : verifyResults ) {
        if ( !result.rootPath.empty() )
            usedRootPaths.insert(result.rootPath);
        if ( !result.client.rootPath.empty() )
            usedRootPaths.insert(result.client.rootPath);
    }

    if ( !usedRootPaths.empty() || !rootErrors.empty() ) {
        fprintf(detailsLogFile, "--- Internal information ---\n\n");
    }

    if ( !usedRootPaths.empty() ) {
        fprintf(detailsLogFile, "Note, the following root paths were used in the above errors:\n");
        for ( std::string_view usedRootPath : usedRootPaths ) {
            fprintf(detailsLogFile, "    %s\n", usedRootPath.data());
        }
        fprintf(detailsLogFile, "\n");
    }

    if ( !rootErrors.empty() ) {
        fprintf(detailsLogFile, "Note, the following root paths were inaccessible:\n");
        for ( const auto& rootPathAndError : rootErrors ) {
            fprintf(detailsLogFile, "    %s due to '%s'\n", rootPathAndError.first.data(), rootPathAndError.second.data());
        }
        fprintf(detailsLogFile, "\n");
    }
}

void printResultsJSON(std::span<ResultBinary> verifyResults,
                      std::span<ExportsChangedBinary> exportsChanged,
                      FILE* jsonFile)
{
    fprintf(jsonFile, "{\n");

    {
        fprintf(jsonFile, "  \"removed-used-symbols\" : [\n");

        bool needsComma = false;
        for ( const ResultBinary& binary : verifyResults ) {
            if ( needsComma )
                fprintf(jsonFile, ",\n");
            else
                needsComma = true;

            bool defInSharedCache = Header::isSharedCacheEligiblePath(binary.installName.c_str());
            bool useInSharedCache = Header::isSharedCacheEligiblePath(binary.client.path.c_str());

            fprintf(jsonFile, "    {\n");

            fprintf(jsonFile, "      \"arch\" : \"%s\",\n", binary.arch.c_str());
            fprintf(jsonFile, "      \"symbol-name\" : \"%s\",\n", binary.client.symbolName.c_str());

            fprintf(jsonFile, "      \"def-uuid\" : \"%s\",\n", binary.uuid.c_str());
            fprintf(jsonFile, "      \"def-project-name\" : \"%s\",\n", binary.projectName.c_str());
            fprintf(jsonFile, "      \"def-install-name\" : \"%s\",\n", binary.installName.c_str());
            fprintf(jsonFile, "      \"def-shared-cache-eligible\" : \"%s\",\n", defInSharedCache ? "yes" : "no");

            fprintf(jsonFile, "      \"use-uuid\" : \"%s\",\n", binary.client.uuid.c_str());
            fprintf(jsonFile, "      \"use-project-name\" : \"%s\",\n", binary.client.projectName.c_str());
            fprintf(jsonFile, "      \"use-path\" : \"%s\",\n", binary.client.path.c_str());
            fprintf(jsonFile, "      \"use-shared-cache-eligible\" : \"%s\"\n", useInSharedCache ? "yes" : "no");
            fprintf(jsonFile, "    }");
        }
        fprintf(jsonFile, "\n");

        fprintf(jsonFile, "  ],\n");
    }

    {
        fprintf(jsonFile, "  \"added-exports\" : [\n");

        bool needsComma = false;
        for ( const ExportsChangedBinary& binary : exportsChanged ) {
            if ( !binary.wasAdded )
                continue;

            if ( needsComma )
                fprintf(jsonFile, ",\n");
            else
                needsComma = true;

            bool inSharedCache = Header::isSharedCacheEligiblePath(binary.installName.c_str());

            fprintf(jsonFile, "    {\n");

            fprintf(jsonFile, "      \"arch\" : \"%s\",\n", binary.arch.c_str());
            fprintf(jsonFile, "      \"symbol-name\" : \"%s\",\n", binary.symbolName.c_str());
            fprintf(jsonFile, "      \"uuid\" : \"%s\",\n", binary.uuid.c_str());
            fprintf(jsonFile, "      \"project-name\" : \"%s\",\n", binary.projectName.c_str());
            fprintf(jsonFile, "      \"install-name\" : \"%s\",\n", binary.installName.c_str());
            fprintf(jsonFile, "      \"shared-cache-eligible\" : \"%s\"\n", inSharedCache ? "yes" : "no");
            fprintf(jsonFile, "    }");
        }
        fprintf(jsonFile, "\n");

        fprintf(jsonFile, "  ],\n");
    }

    {
        fprintf(jsonFile, "  \"removed-exports\" : [\n");

        bool needsComma = false;
        for ( const ExportsChangedBinary& binary : exportsChanged ) {
            if ( binary.wasAdded )
                continue;

            if ( needsComma )
                fprintf(jsonFile, ",\n");
            else
                needsComma = true;

            bool inSharedCache = Header::isSharedCacheEligiblePath(binary.installName.c_str());

            fprintf(jsonFile, "    {\n");

            fprintf(jsonFile, "      \"arch\" : \"%s\",\n", binary.arch.c_str());
            fprintf(jsonFile, "      \"symbol-name\" : \"%s\",\n", binary.symbolName.c_str());
            fprintf(jsonFile, "      \"uuid\" : \"%s\",\n", binary.uuid.c_str());
            fprintf(jsonFile, "      \"project-name\" : \"%s\",\n", binary.projectName.c_str());
            fprintf(jsonFile, "      \"install-name\" : \"%s\",\n", binary.installName.c_str());
            fprintf(jsonFile, "      \"shared-cache-eligible\" : \"%s\"\n", inSharedCache ? "yes" : "no");
            fprintf(jsonFile, "    }");
        }
        fprintf(jsonFile, "\n");

        fprintf(jsonFile, "  ]\n");
    }

    fprintf(jsonFile, "}\n");
}
