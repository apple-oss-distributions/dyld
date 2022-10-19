/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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

#include "Defines.h"
#include "NewSharedCacheBuilder.h"
#include "NewAdjustDylibSegments.h"
#include "CacheDylib.h"
#include "ClosureFileSystem.h"
#include "JSONWriter.h"
#include "StringUtils.h"
#include "Array.h"
#include "DyldSharedCache.h"
#include "dyld_cache_format.h"
#include "OptimizerObjC.h"
#include "ObjCVisitor.h"
#include "Trie.hpp"
#include "JustInTimeLoader.h"
#include "OptimizerSwift.h"
#include "PrebuiltLoader.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "SwiftVisitor.h"
#include "ParallelUtils.h"

// FIXME: Remove this once we don't write to the old objc header struct.  See emitObjCOptsHeader()
#include "objc-shared-cache.h"

#include <_simple.h>
#include <list>
#include <mach-o/nlist.h>
#include <sstream>
#include <unordered_set>

using dyld3::GradedArchs;
using dyld3::MachOFile;

using dyld4::JustInTimeLoader;
using dyld4::KernelArgs;
using dyld4::Loader;
using dyld4::ProcessConfig;
using dyld4::RuntimeState;
using dyld4::SyscallDelegate;

using lsl::EphemeralAllocator;

using metadata_visitor::SwiftConformance;
using metadata_visitor::SwiftVisitor;

using namespace cache_builder;
using namespace error;

//
// MARK: --- SharedCacheBuilder setup methods ---
//

SharedCacheBuilder::SharedCacheBuilder(BuilderOptions& options, const dyld3::closure::FileSystem& fileSystem)
    : options(options)
    , fileSystem(fileSystem)
    , config(options)
{
}

void SharedCacheBuilder::forEachWarning(void (^callback)(const std::string_view& str)) const
{
    for ( const InputFile& inputFile : this->allInputFiles ) {
        if ( inputFile.hasError() ) {
            // Note, don't change the form of this message without checking in with MRM, as they
            // parse it.  We really need to add structured errors/warnings some time
            std::string reason = "Dylib located at '" + inputFile.path + "' not placed in shared cache because: ";
            callback(reason + inputFile.getError().message());
        }
    }
}

void SharedCacheBuilder::forEachCacheDylib(void (^callback)(const std::string_view& path)) const
{
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        // Note this has to return the path, not the install name, as MRM uses this to delete
        // the path from disk
        callback(cacheDylib.inputFile->path);
    }
}

void SharedCacheBuilder::forEachCacheSymlink(void (^callback)(const std::string_view& path)) const
{
    for ( const auto& aliasAndRealPath : this->dylibAliases ) {
        callback(aliasAndRealPath.first);
    }
}

void SharedCacheBuilder::addFile(const void* buffer, size_t bufferSize, std::string_view path,
                                 uint64_t inode, uint64_t modTime)
{
    Diagnostics diag;
    const bool  isOSBinary = false;
    if ( const MachOFile* mf = MachOFile::compatibleSlice(diag, buffer, bufferSize, path.data(),
                                                          this->options.platform, isOSBinary,
                                                          this->options.archs) ) {
        InputFile inputFile;
        inputFile.mf        = mf;
        inputFile.inode     = inode;
        inputFile.mtime     = modTime;
        inputFile.path      = path;
        allInputFiles.push_back(std::move(inputFile));
        return;
    }

    // On macOS, also allow iOSMac dylibs
    if ( this->options.platform == dyld3::Platform::macOS ) {
        diag.clearError();
        if ( const MachOFile* mf = MachOFile::compatibleSlice(diag, buffer, bufferSize, path.data(),
                                                              dyld3::Platform::iOSMac, isOSBinary,
                                                              this->options.archs) ) {
            InputFile inputFile;
            inputFile.mf        = mf;
            inputFile.inode     = inode;
            inputFile.mtime     = modTime;
            inputFile.path      = path;
            allInputFiles.push_back(std::move(inputFile));
            return;
        }
    }
}

void SharedCacheBuilder::setAliases(const std::vector<FileAlias>& aliases,
                                    const std::vector<FileAlias>& intermediateAliases)
{
    this->inputAliases = aliases;
    this->inputIntermediateAliases = intermediateAliases;
}

//
// MARK: --- SharedCacheBuilder build methods ---
//

// This is phase 1 of the build() process.  It looks at the input files and calculates
// the set of dylibs/executables we'll use.
// Inputs:  allInputFiles
// Outputs: cacheDylibs, exeInputFiles, nonCacheDylibInputFiles
Error SharedCacheBuilder::calculateInputs()
{
    if ( this->allInputFiles.empty() )
        return Error("Cannot build cache with no inputs");

    this->categorizeInputs();
    this->verifySelfContained();

    if ( this->cacheDylibs.empty() )
        return Error("Cannot build cache with no dylibs");

    this->sortDylibs();

    // Note this needs to be after sorting, as aliases point to the cache dylibs
    this->calculateDylibAliases();

    if ( Error error = this->calculateDylibDependents(); error.hasError() )
        return error;

    this->categorizeDylibSegments();
    this->categorizeDylibLinkedit();

    return Error();
}

// This is phase 2 of the build() process.  It looks at the input dylibs and populates
// the various Optimizer objects with estimates of the size of the global optimisations.
// Note this is not estimates for per-subCache optimizations
// Inputs:  cacheDylibs
// Outputs: Various Optimizer objects
Error SharedCacheBuilder::estimateGlobalOptimizations()
{
    this->estimateIMPCaches();
    this->findObjCDylibs();
    this->findCanonicalObjCSelectors();
    this->findCanonicalObjCClassNames();
    this->findCanonicalObjCProtocolNames();
    this->findObjCClasses();
    this->findObjCProtocols();
    this->estimateObjCHashTableSizes();
    this->calculateObjCCanonicalProtocolsSize();

    // Note, swift hash tables depends on findObjCClasses()
    this->estimateSwiftHashTableSizes();

    this->calculateCacheDylibsTrie();
    this->estimatePatchTableSize();
    this->estimateCacheLoadersSize();

    this->setupStubOptimizer();

    return Error();
}

// This is phase 3 of the build() process.  It takes the inputs and Optimizers
// from the previous phases, and creates the SubCache objects
// Inputs:  cacheDylibs, various Optimizers
// Outputs: subCaches
Error SharedCacheBuilder::createSubCaches()
{
    this->computeSubCaches();

    // Per-subCache optimizations
    if ( Error error = this->calculateSubCacheSymbolStrings(); error.hasError() )
        return error;
    if ( Error error = this->calculateUniqueGOTs(); error.hasError() )
        return error;

    this->sortSubCacheSegments();
    this->calculateSlideInfoSize();
    this->calculateCodeSignatureSize();
    this->printSubCaches();
    if ( Error error = this->computeSubCacheLayout(); error.hasError() )
        return error;
    if ( Error error = this->allocateSubCacheBuffers(); error.hasError() )
        return error;

    return Error();
}

// This is phase 4 of the build() process.  It takes the inputs and Optimizers
// from the previous phases, and creates the SubCache objects
// Inputs:  subCaches, various Optimizers
// Outputs: emitted objc strings in the subCache buffers
Error SharedCacheBuilder::preDylibEmitChunks()
{
    this->setupDylibLinkedit();

    // Note this must be after setupDylibLinkedit()
    this->setupSplitSegAdjustors();
    this->adjustObjCClasses();
    this->adjustObjCProtocols();

    // Note this could be after dylib passes, but having the strings emitted now makes
    // it easier to debug the ObjC dylib passes
    this->emitObjCSelectorStrings();
    this->emitObjCClassNameStrings();
    this->emitObjCProtocolNameStrings();
    this->emitObjCSwiftDemangledNameStrings();

    return Error();
}

// This is phase 4 of the build() process.
// It runs the passes on each of the cacheDylib's
// Inputs:  subCaches, various Optimizers
// Outputs: emitted objc strings in the subCache buffers
Error SharedCacheBuilder::runDylibPasses()
{
    Timer::Scope timedScope(this->config, "runDylibPasses time");
    Timer::AggregateTimer aggregateTimerOwner(this->config);
    auto& aggregateTimer = aggregateTimerOwner;

    // Because blocks...
    std::vector<const CacheDylib*> builderCacheDylibsOwner;
    auto& builderCacheDylibs = builderCacheDylibsOwner;
    for ( const CacheDylib& cacheDylib : this->cacheDylibs )
        builderCacheDylibs.push_back(&cacheDylib);

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        Diagnostics diag;

        cacheDylib.copyRawSegments(this->config, aggregateTimer);

        PatchInfo& dylibPatchInfo = this->patchTableOptimizer.patchInfos[cacheDylib.cacheIndex];
        cacheDylib.applySplitSegInfo(diag, this->options, this->config,
                                     aggregateTimer, this->unmappedSymbolsOptimizer);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.updateSymbolTables(diag, this->config, aggregateTimer);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.calculateBindTargets(diag, this->config, aggregateTimer, builderCacheDylibs,
                                        dylibPatchInfo);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.bind(diag, this->config, aggregateTimer, dylibPatchInfo);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.updateObjCSelectorReferences(diag, this->config, aggregateTimer, this->objcSelectorOptimizer);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.convertObjCMethodListsToOffsets(diag, this->config, aggregateTimer, this->objcSelectorOptimizer.selectorStringsChunk);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        // Note, must be after updating selector references and converting relative methods to selector offsets
        cacheDylib.sortObjCMethodLists(diag, this->config, aggregateTimer, this->objcSelectorOptimizer.selectorStringsChunk);
        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        cacheDylib.optimizeLoadsFromConstants(this->config, aggregateTimer,
                                              this->objcSelectorOptimizer.selectorStringsChunk);

        Error error = cacheDylib.emitObjCIMPCaches(this->config, aggregateTimer, this->objcIMPCachesOptimizer,
                                                   this->objcSelectorOptimizer.selectorStringsChunk);
        if ( error.hasError() )
            return error;

        cacheDylib.optimizeStubs(this->options, this->config, aggregateTimer, this->stubOptimizer,
                                 dylibPatchInfo);

        // FIPS seal corecrypto, This must be done after stub elimination (so that __TEXT,__text is not changed after sealing)
        cacheDylib.fipsSign(aggregateTimer);

        return Error();
    });

    return err;
}

// This is phase 5 of the build() process.  It takes the Optimizers
// from the previous phases, and emits them to the cache buffers
// Inputs:  subCaches, various Optimizers
// Outputs: emitted optimiations in the subCache buffers
Error SharedCacheBuilder::postDylibEmitChunks()
{
    this->optimizeTLVs();

    if ( Error error = this->emitUniquedGOTs(); error.hasError() )
        return error;

    // Note this has to be before we emit the protocol hash table
    if ( Error error = this->emitCanonicalObjCProtocols(); error.hasError() )
        return error;

    this->emitObjCHashTables();
    this->emitObjCHeaderInfo();
    if ( Error error = this->computeObjCClassLayout(); error.hasError() )
        return error;

    // Note this must be after computeObjCClassLayout() as we need it to set the flags for whether
    // we have missing weak superclasses or not
    this->emitObjCOptsHeader();

    // Note, this has to be after we've emitted the objc class hash table, and after emitting
    // the objc header info
    if ( Error error = this->emitSwiftHashTables(); error.hasError() )
        return error;

    this->emitCacheDylibsTrie();
    if ( Error error = this->emitPatchTable(); error.hasError() )
        return error;

    // Note, this must be after we emit the patch table
    if ( Error error = this->emitCacheDylibsPrebuiltLoaders(); error.hasError() )
        return error;

    // Note, this has to be after we've emitted the objc hash tables and the objc header infos
    if ( Error error = this->emitExecutablePrebuiltLoaders(); error.hasError() )
        return error;

    // This has to be after anyone using the pointers in the cache, eg, walking the objc metadata
    // As otherwise it will convert pointers to an unknown format
    this->computeSlideInfo();

    this->emitSymbolTable();
    this->emitUnmappedLocalSymbols();

    return Error();
}

// This is phase 6 of the build() process.  it does any final work to emit
// the sub caches
// Inputs: everything else
// Outputs: final emitted data in the sub caches
Error SharedCacheBuilder::finalize()
{

    // Do objc very late, as it adds segments to the mach-o, which aren't in sync with
    // the segments on the CacheDylib
    this->addObjcSegments();
    this->computeCacheHeaders();
    this->codeSign();

    return Error();
}

Error SharedCacheBuilder::build()
{
    Timer::Scope timedScope(this->config, "total build time");

    if ( Error error = this->calculateInputs(); error.hasError() )
        return error;

    if ( Error error = this->estimateGlobalOptimizations(); error.hasError() )
        return error;

    if ( Error error = this->createSubCaches(); error.hasError() )
        return error;

    if ( Error error = this->preDylibEmitChunks(); error.hasError() )
        return error;

    if ( Error error = this->runDylibPasses(); error.hasError() )
        return error;

    if ( Error error = this->postDylibEmitChunks(); error.hasError() )
        return error;

    if ( Error error = this->finalize(); error.hasError() )
        return error;

    return Error();
}

static inline uint64_t alignPage(uint64_t value)
{
    // Align to 16KB even on x86_64.  That makes it easier for arm64 machines to map in the cache.
    const uint64_t MinRegionAlignment = 0x4000;

    return ((value + MinRegionAlignment - 1) & (-MinRegionAlignment));
}

static inline CacheVMSize alignPage(CacheVMSize value)
{
    return CacheVMSize(alignPage(value.rawValue()));
}

static inline CacheFileSize alignPage(CacheFileSize value)
{
    return CacheFileSize(alignPage(value.rawValue()));
}

// Note minAlignment here is the alignment in bytes, not a shifted value.  Eg, 0x4000 for 16k alignment, not 14
static inline uint64_t alignTo(uint64_t value, uint64_t minAlignment)
{
    return (value + (minAlignment - 1)) & (-minAlignment);
}

static inline CacheVMSize alignTo(CacheVMSize value, uint64_t minAlignment)
{
    return CacheVMSize(alignTo(value.rawValue(), minAlignment));
}

static inline CacheFileSize alignTo(CacheFileSize value, uint64_t minAlignment)
{
    return CacheFileSize(alignTo(value.rawValue(), minAlignment));
}

static inline CacheVMAddress alignTo(CacheVMAddress value, uint64_t minAlignment)
{
    return CacheVMAddress(alignTo(value.rawValue(), minAlignment));
}

void SharedCacheBuilder::categorizeInputs()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "categorizeInputs time");

    for ( InputFile& inputFile : this->allInputFiles ) {
        if ( inputFile.mf->isDylib() || inputFile.mf->isDyld() ) {
            auto failureHandler = ^(const char* reason) {
                inputFile.setError(Error("%s", reason));
            };

            std::string_view installName = inputFile.mf->installName();
            std::string_view dylibPath = inputFile.path;
            if ( (installName != dylibPath) && ((this->options.platform == dyld3::Platform::macOS) || startsWith(dylibPath, "/System/Cryptexes/OS/")) ) {
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
                }
            }

            if ( inputFile.mf->canBePlacedInDyldCache(dylibPath.data(), failureHandler) ) {
                CacheDylib cacheDylib(inputFile);
                this->cacheDylibs.push_back(std::move(cacheDylib));
            }
            else {
                this->nonCacheDylibInputFiles.push_back(&inputFile);
            }
            continue;
        }

        if ( inputFile.mf->isDynamicExecutable() ) {
            auto failureHandler = ^(const char* reason) {
                inputFile.setError(Error("%s", reason));
            };
            if ( inputFile.mf->canHavePrebuiltExecutableLoader(options.platform, inputFile.path, failureHandler) ) {
                this->exeInputFiles.push_back(&inputFile);
            }

            continue;
        }
    }

    if ( this->config.log.printStats ) {
        stats.add("  inputs: found %lld cache eligible dylibs\n", (uint64_t)this->cacheDylibs.size());
        stats.add("  inputs: found %lld other dylibs\n", (uint64_t)this->nonCacheDylibInputFiles.size());
        stats.add("  inputs: using %lld eligible executables\n", (uint64_t)this->exeInputFiles.size());
    }
}

void SharedCacheBuilder::verifySelfContained()
{
    Timer::Scope timedScope(this->config, "verifySelfContained time");

    __block std::unordered_set<std::string_view> allDylibs;
    allDylibs.reserve(this->allInputFiles.size());
    for ( const InputFile& inputFile : this->allInputFiles ) {
        if ( inputFile.mf->isDylib() )
            allDylibs.insert(inputFile.mf->installName());
    }

    __block std::unordered_set<std::string_view> potentialCacheDylibs;
    potentialCacheDylibs.reserve(this->cacheDylibs.size());
    for ( const CacheDylib& cacheDylib : this->cacheDylibs )
        potentialCacheDylibs.insert(cacheDylib.installName);

    __block std::unordered_set<std::string_view> badDylibs;


    // check all dependencies to assure every dylib in cache only depends on other dylibs in cache
    __block bool doAgain = true;
    while ( doAgain ) {
        doAgain = false;
        // scan dylib list making sure all dependents are in dylib list
        for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
            //Timer::Scope timedScope(this->config, cacheDylib.installName);
            // Skip dylibs we marked bad from a previous iteration
            if ( cacheDylib.inputFile->hasError() )
                continue;

            cacheDylib.inputMF->forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                char resolvedSymlinkPath[PATH_MAX];
                if ( potentialCacheDylibs.count(loadPath) == 0 ) {
                    // The loadPath was embedded when the dylib was built, but we may be in the process of moving
                    // a dylib with symlinks from old to new paths
                    // In this case, the realpath will tell us the new location
                    if ( fileSystem.getRealPath(loadPath, resolvedSymlinkPath) ) {
                        if ( strcmp(resolvedSymlinkPath, loadPath) != 0 ) {
                            loadPath = resolvedSymlinkPath;
                        }
                    }
                }
                if ( potentialCacheDylibs.count(loadPath) == 0 ) {
                    // Break weak edges, but only if we haven't seen the dylib.
                    if ( isWeak && (allDylibs.count(loadPath) == 0) )
                        return;
                    if ( isWeak && allowedMissingWeakDylibs.count(loadPath) )
                        return;
                    std::string reason          = std::string("Could not find dependency '") + loadPath + "'";
                    cacheDylib.inputFile->setError(Error("%s", reason.c_str()));
                    badDylibs.insert(cacheDylib.installName);
                    doAgain = true;
                    stop    = true;
                    return;
                }

                if ( badDylibs.count(loadPath) ) {
                    // Break weak edges, but only if we haven't seen the dylib.
                    if ( isWeak && (allDylibs.count(loadPath) == 0) )
                        return;
                    std::string reason          = std::string("Depends on ineligible/bad dylib '") + loadPath + "'";
                    cacheDylib.inputFile->setError(Error("%s", reason.c_str()));
                    badDylibs.insert(cacheDylib.installName);
                    doAgain = true;
                    stop    = true;
                    return;
                }
            });
        }
    }

    // Add bad dylibs to the "other" dylibs for use in prebuilt loaders
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( cacheDylib.inputFile->hasError() ) {
            this->nonCacheDylibInputFiles.push_back(cacheDylib.inputFile);
            this->dylibHasMissingDependency = true;
        }
    }

    this->cacheDylibs.erase(std::remove_if(this->cacheDylibs.begin(), this->cacheDylibs.end(), [&](const CacheDylib& dylib) {
                                // Dylibs with errors must be removed from the cache
                                return dylib.inputFile->hasError();
                            }),
                            this->cacheDylibs.end());
}

void SharedCacheBuilder::calculateDylibAliases()
{
    Timer::Scope timedScope(this->config, "calculateDylibAliases time");

    std::unordered_map<std::string_view, CacheDylib*> dylibMap;
    for ( CacheDylib& cacheDylib : this->cacheDylibs )
        dylibMap[cacheDylib.installName] = &cacheDylib;

    for ( const cache_builder::FileAlias& alias : this->inputAliases ) {
        auto it = dylibMap.find(alias.realPath);
        if ( it != dylibMap.end() )
            this->dylibAliases[alias.aliasPath] = it->second;
    }
}

void SharedCacheBuilder::sortDylibs()
{
    Timer::Scope timedScope(this->config, "sortDylibs time");

    std::sort(this->cacheDylibs.begin(), this->cacheDylibs.end(), [&](const CacheDylib& a, const CacheDylib& b) {
        // HACK: See addObjCOptimizationsToSubCache() and addObjCTextChunk()
        // We put the libobjc __TEXT first in the sub cache so that offsets from it to OBJC_RO are
        // positive.  But dyld4 and objc HeaderInfo data structures rely on the cache dylibs being
        // sorted by mach_header, and moving objc first breaks the order we determine here.  So hack
        // this too and put libobjc first for now.
        bool isObjCA = (a.installName == "/usr/lib/libobjc.A.dylib");
        bool isObjCB = (b.installName == "/usr/lib/libobjc.A.dylib");
        if ( isObjCA != isObjCB )
            return isObjCA;

        const auto& orderA = options.dylibOrdering.find(std::string(a.installName));
        const auto& orderB = options.dylibOrdering.find(std::string(b.installName));
        bool        foundA = (orderA != options.dylibOrdering.end());
        bool        foundB = (orderB != options.dylibOrdering.end());

        // Order all __DATA_DIRTY segments specified in the order file first, in
        // the order specified in the file, followed by any other __DATA_DIRTY
        // segments in lexicographic order.
        if ( foundA && foundB )
            return orderA->second < orderB->second;
        else if ( foundA )
            return true;
        else if ( foundB )
            return false;

        // Sort mac before iOSMac
        bool isIOSMacA = startsWith(a.installName, "/System/iOSSupport/");
        bool isIOSMacB = startsWith(b.installName, "/System/iOSSupport/");
        if ( isIOSMacA != isIOSMacB )
            return !isIOSMacA;

        // Finally sort by install name
        return a.installName < b.installName;
    });

    // Set the indices after sorting
    uint32_t cacheIndex = 0;
    for ( CacheDylib& cacheDylib : cacheDylibs )
        cacheDylib.cacheIndex = cacheIndex++;
}

Error SharedCacheBuilder::calculateDylibDependents()
{
    Timer::Scope timedScope(this->config, "calculateDylibDependents time");

    std::unordered_map<std::string_view, const CacheDylib*> dylibMapOwner;
    auto& dylibMap = dylibMapOwner;
    for ( const CacheDylib& cacheDylib : cacheDylibs )
        dylibMap[cacheDylib.installName] = &cacheDylib;

    // Add install names too, just in case dylibs are moving
    dylibMap.insert(this->dylibAliases.begin(), this->dylibAliases.end());

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        __block Diagnostics diag;

        cacheDylib.inputMF->forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport,
                                                    bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
            CacheDylib::DependentDylib depDylib;
            if ( isUpward )
                depDylib.kind = CacheDylib::DependentDylib::Kind::upward;
            else if ( isReExport )
                depDylib.kind = CacheDylib::DependentDylib::Kind::reexport;
            else if ( isWeak )
                depDylib.kind = CacheDylib::DependentDylib::Kind::weakLink;
            else
                depDylib.kind = CacheDylib::DependentDylib::Kind::normal;

            auto it = dylibMap.find(loadPath);
            // If the dylib is missing, try real path.  This is to support moved dylibs
            // with symlinks pointing from old to new location
            if ( it == dylibMap.end() ) {
                char resolvedSymlinkPath[PATH_MAX];
                if ( fileSystem.getRealPath(loadPath, resolvedSymlinkPath) ) {
                    if ( strcmp(resolvedSymlinkPath, loadPath) != 0 ) {
                        it = dylibMap.find(resolvedSymlinkPath);
                    }
                }
            }
            if ( it != dylibMap.end() ) {
                // Found a dylib with the correct install name
                depDylib.dylib = it->second;
            }

            if ( (depDylib.dylib != nullptr) || isWeak ) {
                cacheDylib.dependents.push_back(std::move(depDylib));
            }
            else {
                diag.error("dependent dylib '%s' not found", loadPath);
                stop = true;
            }
        });

        if ( diag.hasError() )
            return Error("%s", diag.errorMessageCStr());

        return Error();
    });

    return err;
}

static void getInputDylibVisitorState(const CacheDylib& cacheDylib,
                                      std::vector<metadata_visitor::Segment>& dylibSegments,
                                      std::vector<uint64_t>& bindTargets)
{
    // Get the segment ranges.  We need this as the dylib's segments are in different buffers, not in VM layout
    __block Diagnostics diag;
    cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout &layout) {
        dylibSegments.reserve(layout.segments.size());

        mach_o::Fixups fixups(layout);
        uint16_t chainedPointerFormat = 0;
        if ( cacheDylib.inputMF->hasChainedFixups() )
            chainedPointerFormat = fixups.chainedPointerFormat();

        for ( uint32_t segIndex = 0; segIndex != layout.segments.size(); ++segIndex ) {
            const mach_o::SegmentLayout& inputSegment = layout.segments[segIndex];

            metadata_visitor::Segment segment;
            segment.startVMAddr = VMAddress(inputSegment.vmAddr);
            segment.endVMAddr   = VMAddress(inputSegment.vmAddr + inputSegment.vmSize);
            segment.bufferStart = (uint8_t*)inputSegment.buffer;
            segment.onDiskDylibChainedPointerFormat = chainedPointerFormat;
            segment.segIndex = segIndex;

            dylibSegments.push_back(std::move(segment));
        }

        // ObjC patching needs the bind targets for interposable references to the classes
        // build targets table
        if ( cacheDylib.inputMF->hasChainedFixupsLoadCommand() ) {
            fixups.forEachBindTarget_ChainedFixups(diag, ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                if ( info.libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
                    std::optional<CacheDylib::BindTargetAndName> bindTargetAndName;
                    bindTargetAndName = cacheDylib.hasExportedSymbol(diag, info.symbolName, CacheDylib::SearchMode::selfAndReexports);
                    if ( bindTargetAndName.has_value() ) {
                        const CacheDylib::BindTarget& bindTarget = bindTargetAndName->first;
                        InputDylibVMAddress resultVMAddr;
                        switch ( bindTarget.kind ) {
                            case CacheDylib::BindTarget::Kind::absolute:
                                resultVMAddr = InputDylibVMAddress(bindTarget.absolute.value);
                                break;
                            case CacheDylib::BindTarget::Kind::inputImage:{
                                // Convert from an input dylib offset to the cache dylib offset
                                const CacheDylib::BindTarget::InputImage& inputImage = bindTarget.inputImage;
                                resultVMAddr = inputImage.targetDylib->inputLoadAddress + inputImage.targetRuntimeOffset;
                                break;
                            }
                            case CacheDylib::BindTarget::Kind::cacheImage:
                                // We shouldn't find a value in a cache image, only input images.
                                diag.error("Shouldn't see cacheImage fixups at this point");
                                break;
                        }

                        bindTargets.push_back(resultVMAddr.rawValue());
                    } else {
                        bindTargets.push_back(0);
                    }
                } else {
                    bindTargets.push_back(0);
                }
            });
        }
    });
    diag.assertNoError();
}

static objc_visitor::Visitor makeInputDylibObjCVisitor(const CacheDylib& cacheDylib)
{
    std::vector<metadata_visitor::Segment> dylibSegments;
    std::vector<uint64_t> bindTargets;

    getInputDylibVisitorState(cacheDylib, dylibSegments, bindTargets);

    objc_visitor::Visitor objcVisitor(VMAddress(cacheDylib.inputLoadAddress.rawValue()),
                                      cacheDylib.inputMF,
                                      std::move(dylibSegments), std::nullopt, std::move(bindTargets));
    return objcVisitor;
}

static SwiftVisitor makeInputDylibSwiftVisitor(const CacheDylib& cacheDylib)
{
    std::vector<metadata_visitor::Segment> dylibSegments;
    std::vector<uint64_t> bindTargets;

    getInputDylibVisitorState(cacheDylib, dylibSegments, bindTargets);

    SwiftVisitor swiftVisitor(VMAddress(cacheDylib.inputLoadAddress.rawValue()),
                              cacheDylib.inputMF,
                              std::move(dylibSegments),
                              std::nullopt, std::move(bindTargets));
    return swiftVisitor;
}

// Walk every segment in the inputs, and work out which kind of segment it is
void SharedCacheBuilder::categorizeDylibSegments()
{
    Timer::Scope timedScope(this->config, "categorizeDylibSegments time");

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);
        cacheDylib.categorizeSegments(this->config, objcVisitor);

        return Error();
    });

    assert(!err.hasError());
}

// Walk every LINKEDIT load command in the inputs, and work out which kind of segment it is
void SharedCacheBuilder::categorizeDylibLinkedit()
{
    Timer::Scope timedScope(this->config, "categorizeDylibLinkedit time");

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        cacheDylib.categorizeLinkedit(this->config);
        return Error();
    });

    assert(!err.hasError());
}

static void forEachObjCMethodName(const CacheDylib& cacheDylib,
                                  void (^callback)(std::string_view str))
{
    const MachOFile* mf = cacheDylib.inputMF;
    mf->forEachSection(^(const MachOFile::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( strcmp(sectInfo.segInfo.segName, "__TEXT") != 0 )
            return;
        if ( strcmp(sectInfo.sectName, "__objc_methname") != 0 )
            return;
        if ( sectInfo.segInfo.isProtected || ((sectInfo.sectFlags & SECTION_TYPE) != S_CSTRING_LITERALS) ) {
            stop = true;
            return;
        }
        if ( malformedSectionRange ) {
            stop = true;
            return;
        }

        // Use the file offset in the section to get the correct content
        const char* content     = (const char*)mf + sectInfo.sectFileOffset;
        uint64_t    sectionSize = sectInfo.sectSize;

        const char* s   = (const char*)content;
        const char* end = s + sectionSize;
        while ( s < end ) {
            std::string_view str = s;
            callback(str);
            s += str.size() + 1;
        }

        stop = true;
    });
}

struct FoundSymbol
{
    const CacheDylib* foundInDylib = nullptr;
    VMOffset          offsetInDylib;
};

static FoundSymbol findTargetClass(Diagnostics diag,
                                   const std::vector<CacheDylib>& cacheDylibs,
                                   std::string_view symbolName, std::optional<uint32_t> cacheIndex)
{
    if ( !cacheIndex.has_value() )
        return { };

    const CacheDylib& cacheDylib = cacheDylibs[cacheIndex.value()];
    std::optional<CacheDylib::BindTargetAndName> bindTargetAndName = cacheDylib.hasExportedSymbol(diag, symbolName.data(), CacheDylib::SearchMode::selfAndReexports);
    if ( diag.hasError() )
        return { };

    if ( !bindTargetAndName.has_value() )
        return { };

    const CacheDylib::BindTarget& bindTarget = bindTargetAndName->first;
    switch ( bindTarget.kind ) {
        case CacheDylib::BindTarget::Kind::absolute:
            // We can't have an absolute target class!  Just return nothing
            return { };
        case CacheDylib::BindTarget::Kind::inputImage:{
            // Convert from an input dylib offset to the cache dylib offset
            const CacheDylib::BindTarget::InputImage& inputImage = bindTarget.inputImage;
            return { inputImage.targetDylib, inputImage.targetRuntimeOffset };
        }
        case CacheDylib::BindTarget::Kind::cacheImage:
            // We shouldn't find a value in a cache image, only input images.
            diag.error("Shouldn't see cacheImage fixups at this point");
            return { };
    }
}

void SharedCacheBuilder::estimateIMPCaches()
{
    if ( !this->config.layout.is64 )
        return;

    if ( this->config.layout.cacheSize.rawValue() > 0x100000000 )
        return;

    // Only iOS for now
    if ( this->options.platform != dyld3::Platform::iOS )
        return;

    // Skip everything if the JSON file is empty
    if ( this->options.objcOptimizations.map.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "estimateIMPCaches time");

    // Make sure libobjc has the section we need
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( cacheDylib.installName != "/usr/lib/libobjc.A.dylib" )
            continue;

        std::string_view segmentName = this->objcIMPCachesOptimizer.sharedCacheOffsetsSegmentName;
        std::string_view sectionName = this->objcIMPCachesOptimizer.sharedCacheOffsetsSectionName;
        if ( !cacheDylib.inputMF->hasSection(segmentName.data(), sectionName.data()) ) {
            // FIXME: Surface a warning here
            // diag.warning("libobjc's magical IMP caches shared cache offsets list section missing (metadata not optimized)");
            return;
        }
    }

    // Find all the objc dylibs, classes, categories
    std::vector<imp_caches::Dylib>& dylibs = this->objcIMPCachesOptimizer.dylibs;
    dylibs.reserve(this->cacheDylibs.size());

    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        imp_caches::Dylib& dylib = dylibs.emplace_back(cacheDylib.installName);

        // Skip dylibs without chained fixups.  This simplifies binding superclasses across dylibs
        if ( !cacheDylib.inputMF->hasChainedFixupsLoadCommand() )
            continue;

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);

        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            bool isRootClass = objcClass.isRootClass(objcVisitor);
            imp_caches::Class impCacheClass(objcClass.getName(objcVisitor), objcClass.isMetaClass, isRootClass);

            objc_visitor::MethodList objcMethodList = objcClass.getBaseMethods(objcVisitor);
            uint32_t numMethods = objcMethodList.numMethods();
            impCacheClass.methods.reserve(numMethods);
            for ( uint32_t i = 0; i != numMethods; ++i ) {
                objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                imp_caches::Method impCacheMethod(objcMethod.getName(objcVisitor));
                impCacheClass.methods.push_back(std::move(impCacheMethod));
            }

            dylib.classes.push_back(std::move(impCacheClass));

            // Add to the map in case anyone needs to reference this later
            imp_caches::FallbackClass classKey = {
                .installName = cacheDylib.installName,
                .className = impCacheClass.name,
                .isMetaClass = impCacheClass.isMetaClass
            };
            ObjCIMPCachesOptimizer::InputDylibLocation inputDylibLocation = {
                &cacheDylib,
                InputDylibVMAddress(objcClass.getVMAddress().rawValue())
            };
            objcIMPCachesOptimizer.classMap[classKey] = inputDylibLocation;
        });

        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
            imp_caches::Category impCacheCategory(objcCategory.getName(objcVisitor));

            // instance methods
            {
                objc_visitor::MethodList objcMethodList = objcCategory.getInstanceMethods(objcVisitor);
                uint32_t numMethods = objcMethodList.numMethods();
                impCacheCategory.instanceMethods.reserve(numMethods);
                for ( uint32_t i = 0; i != numMethods; ++i ) {
                    objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                    imp_caches::Method impCacheMethod(objcMethod.getName(objcVisitor));
                    impCacheCategory.instanceMethods.push_back(std::move(impCacheMethod));
                }
            }

            // class methods
            {
                objc_visitor::MethodList objcMethodList = objcCategory.getClassMethods(objcVisitor);
                uint32_t numMethods = objcMethodList.numMethods();
                impCacheCategory.classMethods.reserve(numMethods);
                for ( uint32_t i = 0; i != numMethods; ++i ) {
                    objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                    imp_caches::Method impCacheMethod(objcMethod.getName(objcVisitor));
                    impCacheCategory.classMethods.push_back(std::move(impCacheMethod));
                }
            }

            dylib.categories.push_back(std::move(impCacheCategory));
        });
    }

    // Add every class to a map so that we can look them up in the next phase
    typedef std::unordered_map<VMOffset, const imp_caches::Class*, VMOffsetHash, VMOffsetEqual> DylibClasses;
    __block std::vector<DylibClasses> dylibClassMaps;
    dylibClassMaps.resize(this->cacheDylibs.size());

    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        imp_caches::Dylib& dylib = dylibs[cacheDylib.cacheIndex];
        DylibClasses& classMap = dylibClassMaps[cacheDylib.cacheIndex];

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);

        __block uint32_t classIndex = 0;
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            imp_caches::Class& impCacheClass = dylib.classes[classIndex];
            VMOffset offsetInDylib = objcClass.getVMAddress() - objcVisitor.getOnDiskDylibChainedPointerBaseAddress();
            classMap[offsetInDylib] = &impCacheClass;
            ++classIndex;
        });
    }

    // Now that all the classes and categories have been added, link them together by finding class pointers
    // and superclass pointers
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        imp_caches::Dylib& dylib = dylibs[cacheDylib.cacheIndex];

        // Skip dylibs with nothing to do
        if ( dylib.classes.empty() && dylib.categories.empty() )
            continue;

        struct BindTarget {
            std::string_view        symbolName;
            std::optional<uint32_t> targetDylibIndex;
            bool                    isWeakImport     = false;
        };

        __block std::vector<BindTarget> bindTargets;
        __block Diagnostics diag;
        cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
            mach_o::Fixups fixups(layout);

            fixups.forEachBindTarget(diag, false, 0, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                if ( info.libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
                    bindTargets.push_back({ info.symbolName, cacheDylib.cacheIndex, info.weakImport });
                } else if ( info.libOrdinal < 0 ) {
                    // A special ordinal such as weak.  Just put in a placeholder for now
                    bindTargets.push_back({ info.symbolName, std::nullopt, info.weakImport });
                } else {
                    assert(info.libOrdinal <= (int)cacheDylib.dependents.size());
                    const CacheDylib *targetDylib = cacheDylib.dependents[info.libOrdinal-1].dylib;
                    assert(info.weakImport || (targetDylib != nullptr));
                    std::optional<uint32_t> targetDylibIndex;
                    if ( targetDylib != nullptr )
                        targetDylibIndex = targetDylib->cacheIndex;
                    bindTargets.push_back({ info.symbolName, targetDylibIndex, info.weakImport });
                }

                if ( diag.hasError() )
                    stop = true;
            }, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
                // This shouldn't happen with chained fixups
                assert(0);
            });
        });
        diag.assertNoError();

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);

        // Walk each class and set the metaclass and superclass
        __block uint32_t classIndex = 0;
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            imp_caches::Class& impCacheClass = dylib.classes[classIndex];
            const DylibClasses& classMap = dylibClassMaps[cacheDylib.cacheIndex];

            // Regular classes need to set their metaclass pointer
            if ( !objcClass.isMetaClass ) {
                bool unusedPatchableClass = false;
                metadata_visitor::ResolvedValue isa = objcClass.getISA(objcVisitor, unusedPatchableClass);
                VMOffset offsetInDylib = isa.vmAddress() - objcVisitor.getOnDiskDylibChainedPointerBaseAddress();
                const imp_caches::Class* metaclass = classMap.at(offsetInDylib);
                impCacheClass.metaClass = metaclass;
            }

            // Classes and metaclasses need their superclass pointers set
            objcClass.withSuperclass(objcVisitor, ^(const dyld3::MachOFile::ChainedFixupPointerOnDisk *fixup, uint16_t pointerFormat) {
                // Skip null values
                if ( fixup->raw64 == 0 )
                    return;

                uint64_t runtimeOffset = 0;
                if ( fixup->isRebase(pointerFormat, objcVisitor.getOnDiskDylibChainedPointerBaseAddress().rawValue(), runtimeOffset) ) {
                    // Superclass is a rebase to a class in this image
                    VMOffset offsetInDylib(runtimeOffset);
                    const imp_caches::Class* superclass = classMap.at(offsetInDylib);
                    impCacheClass.superClass = superclass;
                    impCacheClass.superClassDylib = &dylib;
                } else {
                    // Hopefully a bind...
                    uint32_t bindOrdinal = 0;
                    int64_t bindAddend = 0;
                    if ( fixup->isBind(pointerFormat, bindOrdinal, bindAddend) ) {
                        const BindTarget& bindTarget = bindTargets[bindOrdinal];
                        FoundSymbol foundSymbol = findTargetClass(diag, this->cacheDylibs,
                                                                  bindTarget.symbolName, bindTarget.targetDylibIndex);
                        if ( foundSymbol.foundInDylib != nullptr ) {
                            const DylibClasses& targetDylibClassMap = dylibClassMaps[foundSymbol.foundInDylib->cacheIndex];
                            const imp_caches::Class* superclass = targetDylibClassMap.at(foundSymbol.offsetInDylib);
                            impCacheClass.superClass = superclass;
                            impCacheClass.superClassDylib = &dylibs.at(foundSymbol.foundInDylib->cacheIndex);
                        }
                    } else {
                        // Not a rebase, or a bind, or null.  What to do?
                        // For now, just don't set the superclass, as then the IMP caches builder will just skip this class
                    }
                }
            });
            diag.assertNoError();

            // Add methods to the map in case anyone needs to reference this later
            {
                objc_visitor::MethodList objcMethodList = objcClass.getBaseMethods(objcVisitor);
                uint32_t numMethods = objcMethodList.numMethods();
                for ( uint32_t i = 0; i != numMethods; ++i ) {
                    objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                    imp_caches::BucketMethod methodKey = {
                        .installName = cacheDylib.installName,
                        .className = impCacheClass.name,
                        .methodName = objcMethod.getName(objcVisitor),
                        .isInstanceMethod = !impCacheClass.isMetaClass
                    };

                    VMAddress impVMAddr = objcMethod.getIMPVMAddr(objcVisitor).value();
                    ObjCIMPCachesOptimizer::InputDylibLocation inputDylibLocation = {
                        &cacheDylib,
                        InputDylibVMAddress(impVMAddr.rawValue())
                    };
                    objcIMPCachesOptimizer.methodMap[methodKey] = inputDylibLocation;
                }
            }

            ++classIndex;
        });

        // Walk each category and set the class pointer
        __block uint32_t categoryIndex = 0;
        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
            imp_caches::Category& impCacheCategory = dylib.categories[categoryIndex];
            const DylibClasses& classMap = dylibClassMaps[cacheDylib.cacheIndex];

            objcCategory.withClass(objcVisitor, ^(const dyld3::MachOFile::ChainedFixupPointerOnDisk *fixup, uint16_t pointerFormat) {
                // Skip null values
                if ( fixup->raw64 == 0 )
                    return;

                uint64_t runtimeOffset = 0;
                if ( fixup->isRebase(pointerFormat, objcVisitor.getOnDiskDylibChainedPointerBaseAddress().rawValue(), runtimeOffset) ) {
                    // Rebase to a class in this image. Should have been optimized by ld64, but oh well.  Perhaps there's multiple
                    // +load methods to prevent that optimization
                    VMOffset offsetInDylib(runtimeOffset);

                    // Note its ok for the class to be missing.  This seems to happen with Swift
                    auto it = classMap.find(offsetInDylib);
                    if ( it != classMap.end() ) {
                        const imp_caches::Class* cls = it->second;
                        impCacheCategory.cls = cls;
                        impCacheCategory.classDylib = &dylib;
                    }
                } else {
                    // Hopefully a bind...
                    uint32_t bindOrdinal = 0;
                    int64_t bindAddend = 0;
                    if ( fixup->isBind(pointerFormat, bindOrdinal, bindAddend) ) {
                        const BindTarget& bindTarget = bindTargets[bindOrdinal];
                        FoundSymbol foundSymbol = findTargetClass(diag, this->cacheDylibs,
                                                                  bindTarget.symbolName, bindTarget.targetDylibIndex);
                        if ( foundSymbol.foundInDylib != nullptr ) {
                            const DylibClasses& targetDylibClassMap = dylibClassMaps[foundSymbol.foundInDylib->cacheIndex];
                            const imp_caches::Class* cls = targetDylibClassMap.at(foundSymbol.offsetInDylib);
                            impCacheCategory.cls = cls;
                            impCacheCategory.classDylib = &dylibs.at(foundSymbol.foundInDylib->cacheIndex);
                        }
                    } else {
                        // Not a rebase, or a bind, or null.  What to do?
                        // For now, just don't set the class, as then the IMP caches builder will just skip this category
                    }
                }
            });

            // Add methods to the map in case anyone needs to reference this later
            if ( impCacheCategory.cls != nullptr ) {
                // instance methods
                {
                    objc_visitor::MethodList objcMethodList = objcCategory.getInstanceMethods(objcVisitor);
                    uint32_t numMethods = objcMethodList.numMethods();
                    for ( uint32_t i = 0; i != numMethods; ++i ) {
                        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                        imp_caches::BucketMethod methodKey = {
                            .installName = cacheDylib.installName,
                            .className = impCacheCategory.cls->name,
                            .methodName = objcMethod.getName(objcVisitor),
                            .isInstanceMethod = true
                        };
                        VMAddress impVMAddr = objcMethod.getIMPVMAddr(objcVisitor).value();
                        ObjCIMPCachesOptimizer::InputDylibLocation inputDylibLocation = {
                            &cacheDylib,
                            InputDylibVMAddress(impVMAddr.rawValue())
                        };
                        objcIMPCachesOptimizer.methodMap[methodKey] = inputDylibLocation;
                    }
                }

                // class methods
                {
                    objc_visitor::MethodList objcMethodList = objcCategory.getClassMethods(objcVisitor);
                    uint32_t numMethods = objcMethodList.numMethods();
                    for ( uint32_t i = 0; i != numMethods; ++i ) {
                        objc_visitor::Method objcMethod = objcMethodList.getMethod(objcVisitor, i);

                        imp_caches::BucketMethod methodKey = {
                            .installName = cacheDylib.installName,
                            .className = impCacheCategory.cls->name,
                            .methodName = objcMethod.getName(objcVisitor),
                            .isInstanceMethod = false
                        };
                        VMAddress impVMAddr = objcMethod.getIMPVMAddr(objcVisitor).value();
                        ObjCIMPCachesOptimizer::InputDylibLocation inputDylibLocation = {
                            &cacheDylib,
                            InputDylibVMAddress(impVMAddr.rawValue())
                        };
                        objcIMPCachesOptimizer.methodMap[methodKey] = inputDylibLocation;
                    }
                }
            }

            ++categoryIndex;
        });
    }

    this->objcIMPCachesOptimizer.builder = std::make_unique<imp_caches::Builder>(dylibs, this->options.objcOptimizations);

    // TODO: We could probably move the perfect hash later, and calculate it in parallel, if we can put a good estimate or upper bound on it
    // We should probably keep the piece here to walk the classes as that can perhaps give us a good estimate of the size of the IMP caches
    // themselves, minus the strings which need their own buffer
    this->objcIMPCachesOptimizer.builder->buildImpCaches();

    // Push all the IMP cache selectors in to the main selectors buffer.
    // We could try have an IMP cache selectors buffer and a regular selectors buffer, but that complicates
    // a bunch of code, such as choosing canonical selectors, as we'd have 2 places to look
    // We expect to run before the selectors pass, as the IMP cache selectors have to be placed first
    assert(this->objcSelectorOptimizer.selectorsMap.empty());
    assert(this->objcSelectorOptimizer.selectorsArray.empty());

    // First push the selectors in to the array in any order.  We'll sort by offset later
    this->objcIMPCachesOptimizer.builder->forEachSelector(^(std::string_view str, uint32_t bufferOffset) {
        this->objcSelectorOptimizer.selectorsArray.emplace_back(str, bufferOffset);
        this->objcSelectorOptimizer.selectorsMap[str] = VMOffset((uint64_t)bufferOffset);
    });

    std::sort(this->objcSelectorOptimizer.selectorsArray.begin(),
              this->objcSelectorOptimizer.selectorsArray.end(),
              [](const objc::ObjCString& a, const objc::ObjCString& b) {
        return a.second < b.second;
    });

    // The selectors after this point need to start where the IMP caches ended
    assert(this->objcSelectorOptimizer.selectorStringsTotalByteSize == 0);
    if ( !this->objcSelectorOptimizer.selectorsArray.empty() ) {
        const objc::ObjCString& lastString = this->objcSelectorOptimizer.selectorsArray.back();
        uint64_t lastStringEnd = lastString.second + lastString.first.size() + 1;
        this->objcSelectorOptimizer.selectorStringsTotalByteSize = (uint32_t)lastStringEnd;
    }

    // Add space for the IMP caches themselves
    this->objcIMPCachesOptimizer.dylibIMPCaches.resize(dylibs.size());
    for ( uint32_t dylibIndex = 0; dylibIndex != dylibs.size(); ++dylibIndex ) {
        imp_caches::Dylib& dylib = dylibs[dylibIndex];
        ObjCIMPCachesOptimizer::IMPCacheMap& dylibIMPCaches = objcIMPCachesOptimizer.dylibIMPCaches[dylibIndex];
        for ( imp_caches::Class& cls : dylib.classes ) {
            std::optional<imp_caches::IMPCache> impCache = this->objcIMPCachesOptimizer.builder->getIMPCache(dylibIndex, cls.name, cls.isMetaClass);
            if ( !impCache.has_value() )
                continue;

            VMOffset currentOffset((uint64_t)this->objcIMPCachesOptimizer.impCachesTotalByteSize);
            assert((this->objcIMPCachesOptimizer.impCachesTotalByteSize % 8) == 0);
            this->objcIMPCachesOptimizer.impCachesTotalByteSize += sizeof(ImpCacheHeader_v2);
            this->objcIMPCachesOptimizer.impCachesTotalByteSize += sizeof(ImpCacheEntry_v2) * impCache->buckets.size();

            const ObjCIMPCachesOptimizer::ClassKey classKey = { cls.name, cls.isMetaClass };
            ObjCIMPCachesOptimizer::IMPCacheAndOffset impCacheAndOffset = { std::move(impCache).value(), currentOffset };
            dylibIMPCaches[classKey] = std::move(impCacheAndOffset);
        }
    }

    if ( this->config.log.printStats ) {
        stats.add("  objc: found %lld imp cache selectors\n", (uint64_t)this->objcSelectorOptimizer.selectorsMap.size());
        stats.add("  objc: using %lld bytes\n", this->objcSelectorOptimizer.selectorStringsTotalByteSize);
    }
}

// Finds all the dylibs containing objc
void SharedCacheBuilder::findObjCDylibs()
{
    // driverKit has no objc
    if ( this->options.platform == dyld3::Platform::driverKit )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findObjCDylibs time");

    assert(this->objcOptimizer.objcDylibs.empty());
    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( cacheDylib.inputMF->hasObjC() )
            this->objcOptimizer.objcDylibs.push_back(&cacheDylib);
    }

    // While we're here, track how much space we need for the opts header and header info RO/RW
    this->objcOptimizer.optsHeaderByteSize = sizeof(ObjCOptimizationHeader);
    if ( config.layout.is64 ) {
        this->objcOptimizer.headerInfoReadOnlyByteSize = sizeof(ObjCOptimizer::header_info_ro_list_t);
        this->objcOptimizer.headerInfoReadOnlyByteSize += (uint32_t)this->objcOptimizer.objcDylibs.size() * sizeof(ObjCOptimizer::header_info_ro_64_t);

        this->objcOptimizer.headerInfoReadWriteByteSize = sizeof(ObjCOptimizer::header_info_rw_list_t);
        this->objcOptimizer.headerInfoReadWriteByteSize += (uint32_t)this->objcOptimizer.objcDylibs.size() * sizeof(ObjCOptimizer::header_info_rw_64_t);
    }
    else {
        this->objcOptimizer.headerInfoReadOnlyByteSize = sizeof(ObjCOptimizer::header_info_ro_list_t);
        this->objcOptimizer.headerInfoReadOnlyByteSize += (uint32_t)this->objcOptimizer.objcDylibs.size() * sizeof(ObjCOptimizer::header_info_ro_32_t);

        this->objcOptimizer.headerInfoReadWriteByteSize = sizeof(ObjCOptimizer::header_info_rw_list_t);
        this->objcOptimizer.headerInfoReadWriteByteSize += (uint32_t)this->objcOptimizer.objcDylibs.size() * sizeof(ObjCOptimizer::header_info_rw_32_t);
    }

    if ( this->config.log.printStats ) {
        stats.add("  objc: found %lld objc dylibs\n", (uint64_t)this->objcOptimizer.objcDylibs.size());
    }
}

// Walk all the dylibs and build a map of canonical selectors
void SharedCacheBuilder::findCanonicalObjCSelectors()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findCanonicalObjCSelectors time");

    BLOCK_ACCCESSIBLE_ARRAY(std::vector<std::string_view>, dylibSelectors, cacheDylibs.size());
    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        std::vector<std::string_view>& strings = dylibSelectors[index];

        forEachObjCMethodName(cacheDylib, ^(std::string_view str) {
            strings.push_back(std::move(str));
        });

        // FIXME: Walk selector references, classes, categories, protocols, etc

        return Error();
    });

    assert(!err.hasError());

    // Merge the results in serial

    // Reserve space for 2m selectors, as we have 1.4m as of writing
    const uint32_t numSelectorsToReserve = 1 << 21;
    this->objcSelectorOptimizer.selectorsMap.reserve(numSelectorsToReserve);
    this->objcSelectorOptimizer.selectorsArray.reserve(numSelectorsToReserve);

    // Process the magic selector first, so that we know its the base of all other strings
    // This is used later for relative method lists
    // Note this may have been added by IMP caches
    constexpr std::string_view magicSelector = "\xf0\x9f\xa4\xaf";
    if ( !this->objcSelectorOptimizer.selectorsArray.empty() ) {
        const objc::ObjCString& firstString = this->objcSelectorOptimizer.selectorsArray.front();
        assert(firstString.first == magicSelector);
        assert(firstString.second == 0);
    } else {
        assert(this->objcSelectorOptimizer.selectorsMap.empty());
        assert(this->objcSelectorOptimizer.selectorsArray.empty());
        assert(this->objcSelectorOptimizer.selectorStringsTotalByteSize == 0);
        this->objcSelectorOptimizer.selectorsMap.insert({ magicSelector, VMOffset((uint64_t)this->objcSelectorOptimizer.selectorStringsTotalByteSize) });
        this->objcSelectorOptimizer.selectorsArray.emplace_back(magicSelector, this->objcSelectorOptimizer.selectorStringsTotalByteSize);
        this->objcSelectorOptimizer.selectorStringsTotalByteSize += magicSelector.size() + 1;
    }

    for ( uint32_t i = 0; i != cacheDylibs.size(); ++i ) {
        const std::vector<std::string_view>& strings = dylibSelectors[i];
        for ( const std::string_view& string : strings ) {
            auto itAndInserted = this->objcSelectorOptimizer.selectorsMap.insert({ string, VMOffset((uint64_t)this->objcSelectorOptimizer.selectorStringsTotalByteSize) });
            if ( itAndInserted.second ) {
                // We inserted the string, so push the string in to the vector
                this->objcSelectorOptimizer.selectorsArray.emplace_back(string, this->objcSelectorOptimizer.selectorStringsTotalByteSize);
                this->objcSelectorOptimizer.selectorStringsTotalByteSize += string.size() + 1;
            }
        }
    }

    if ( this->config.log.printStats ) {
        uint64_t total = 0;
        for ( uint32_t i = 0; i != cacheDylibs.size(); ++i )
            total += dylibSelectors[i].size();

        stats.add("  objc: found %lld unique selectors\n", (uint64_t)this->objcSelectorOptimizer.selectorsArray.size());
        stats.add("  objc: from %lld input selectors\n", total);
    }
}

// Walk all the dylibs and build a map of canonical class names
void SharedCacheBuilder::findCanonicalObjCClassNames()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findCanonicalObjCClassNames time");

    BLOCK_ACCCESSIBLE_ARRAY(std::vector<std::string_view>, dylibObjectNames, cacheDylibs.size());
    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        std::vector<std::string_view>& strings = dylibObjectNames[index];

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);

        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            strings.push_back(objcClass.getName(objcVisitor));
        });

        return Error();
    });

    assert(!err.hasError());

    // Merge the results in serial

    // Reserve space for 100k name strings, as we have 100k as of writing
    const uint32_t numNameStringsToReserve = 1 << 17;
    this->objcClassOptimizer.namesMap.reserve(numNameStringsToReserve);
    this->objcClassOptimizer.namesArray.reserve(numNameStringsToReserve);

    for ( uint32_t i = 0; i != cacheDylibs.size(); ++i ) {
        const std::vector<std::string_view>& strings = dylibObjectNames[i];
        for ( const std::string_view& string : strings ) {
            auto itAndInserted = this->objcClassOptimizer.namesMap.insert({ string, VMOffset((uint64_t)this->objcClassOptimizer.nameStringsTotalByteSize) });
            if ( itAndInserted.second ) {
                // We inserted the string, so push the string in to the vector
                this->objcClassOptimizer.namesArray.emplace_back(string, this->objcClassOptimizer.nameStringsTotalByteSize);
                this->objcClassOptimizer.nameStringsTotalByteSize += string.size() + 1;
            }
        }
    }

    if ( this->config.log.printStats ) {
        uint64_t total = 0;
        for ( uint32_t i = 0; i != cacheDylibs.size(); ++i )
            total += dylibObjectNames[i].size();

        stats.add("  objc: found %lld unique class names\n", (uint64_t)this->objcClassOptimizer.namesArray.size());
        stats.add("  objc: from %lld input class names\n", total);
    }
}

// Walk all the dylibs and build a map of canonical protocol names
void SharedCacheBuilder::findCanonicalObjCProtocolNames()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findCanonicalObjCProtocolNames time");

    BLOCK_ACCCESSIBLE_ARRAY(std::vector<std::string_view>, dylibObjectNames, cacheDylibs.size());
    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        std::vector<std::string_view>& strings = dylibObjectNames[index];

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);

        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
            strings.push_back(objcProtocol.getName(objcVisitor));
        });

        return Error();
    });

    assert(!err.hasError());

    // Merge the results in serial

    // Reserve space for 100k name strings, as we have 100k as of writing
    const uint32_t numNameStringsToReserve = 1 << 17;
    this->objcProtocolOptimizer.namesMap.reserve(numNameStringsToReserve);
    this->objcProtocolOptimizer.namesArray.reserve(numNameStringsToReserve);

    for ( uint32_t i = 0; i != cacheDylibs.size(); ++i ) {
        const std::vector<std::string_view>& strings = dylibObjectNames[i];
        for ( const std::string_view& string : strings ) {
            auto itAndInserted = this->objcProtocolOptimizer.namesMap.insert({ string, VMOffset((uint64_t)this->objcProtocolOptimizer.nameStringsTotalByteSize) });
            if ( itAndInserted.second ) {
                // We inserted the string, so push the string in to the vector
                this->objcProtocolOptimizer.namesArray.emplace_back(string, this->objcProtocolOptimizer.nameStringsTotalByteSize);
                this->objcProtocolOptimizer.nameStringsTotalByteSize += string.size() + 1;
            }
        }
    }

    if ( this->config.log.printStats ) {
        uint64_t total = 0;
        for ( uint32_t i = 0; i != cacheDylibs.size(); ++i )
            total += dylibObjectNames[i].size();

        stats.add("  objc: found %lld unique protocol names\n", (uint64_t)this->objcProtocolOptimizer.namesArray.size());
        stats.add("  objc: from %lld input protocol names\n", total);
    }
}

// Walk all the dylibs and build a map of ObjC classes
void SharedCacheBuilder::findObjCClasses()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findObjCClasses time");

    struct ClassInfo
    {
        const char* name;
        VMAddress   vmAddr;
    };

    BLOCK_ACCCESSIBLE_ARRAY(std::vector<ClassInfo>, dylibClasses, this->objcOptimizer.objcDylibs.size());
    Error err = parallel::forEach(this->objcOptimizer.objcDylibs, ^(size_t index, CacheDylib*& cacheDylib) {
        std::vector<ClassInfo>& classInfos = dylibClasses[index];

        __block objc_visitor::Visitor objCVisitor = makeInputDylibObjCVisitor(*cacheDylib);

        objCVisitor.forEachClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            ClassInfo classInfo = { objcClass.getName(objCVisitor), objcClass.getVMAddress() };
            classInfos.push_back(classInfo);
        });

        return Error();
    });

    assert(!err.hasError());

    // Merge the results in serial

    // Reserve space for 100k classes, as we have 100k as of writing
    const uint32_t numClassesToReserve = 1 << 17;
    this->objcClassOptimizer.classes.reserve(numClassesToReserve);

    for ( uint32_t i = 0; i != this->objcOptimizer.objcDylibs.size(); ++i ) {
        const std::vector<ClassInfo>& classInfos = dylibClasses[i];
        for ( const ClassInfo& classInfo : classInfos ) {
            this->objcClassOptimizer.classes.insert({ classInfo.name, { classInfo.vmAddr.rawValue(), i } });
        }
    }

    if ( this->config.log.printStats ) {
        stats.add("  objc: found %lld classes\n", (uint64_t)this->objcClassOptimizer.classes.size());
    }
}

// Scan a C++ or Swift length-mangled field.
static bool scanMangledField(const char*& string, const char* end,
                             const char*& field, int& length)
{
    // Leading zero not allowed.
    if ( *string == '0' )
        return false;

    length = 0;
    field  = string;
    while ( field < end ) {
        char c = *field;
        if ( !isdigit(c) )
            break;
        field++;
        if ( __builtin_smul_overflow(length, 10, &length) )
            return false;
        if ( __builtin_sadd_overflow(length, c - '0', &length) )
            return false;
    }

    string = field + length;
    return (length > 0) && (string <= end);
}

// copySwiftDemangledName
// Returns the pretty form of the given Swift-mangled class or protocol name.
// Returns std::nullopt if the string doesn't look like a mangled Swift name.
static std::optional<std::string> copySwiftDemangledName(const char* string, bool isProtocol = false)
{
    if ( !string )
        return std::nullopt;

    // Swift mangling prefix.
    if ( strncmp(string, isProtocol ? "_TtP" : "_TtC", 4) != 0 )
        return std::nullopt;
    string += 4;

    const char* end = string + strlen(string);

    // Module name.
    const char* prefix;
    int         prefixLength;
    if ( string[0] == 's' ) {
        // "s" is the Swift module.
        prefix       = "Swift";
        prefixLength = 5;
        string += 1;
    }
    else {
        if ( !scanMangledField(string, end, prefix, prefixLength) )
            return std::nullopt;
    }

    // Class or protocol name.
    const char* suffix;
    int         suffixLength;
    if ( !scanMangledField(string, end, suffix, suffixLength) )
        return std::nullopt;

    if ( isProtocol ) {
        // Remainder must be "_".
        if ( strcmp(string, "_") != 0 )
            return std::nullopt;
    }
    else {
        // Remainder must be empty.
        if ( string != end )
            return std::nullopt;
    }

    std::stringstream ss;
    ss << std::string_view(prefix, prefixLength) << '.' << std::string_view(suffix, suffixLength);
    return ss.str();
}

// Walk all the dylibs and build a map of ObjC protocols
void SharedCacheBuilder::findObjCProtocols()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "findObjCProtocols time");

    struct ProtocolInfo
    {
        const char* name;
        VMAddress   vmAddr;
        std::string swiftDemangledName;
    };

    BLOCK_ACCCESSIBLE_ARRAY(std::vector<ProtocolInfo>, dylibProtocols, this->objcOptimizer.objcDylibs.size());
    Error err = parallel::forEach(this->objcOptimizer.objcDylibs, ^(size_t index, CacheDylib*& cacheDylib) {
        std::vector<ProtocolInfo>& protocoInfos = dylibProtocols[index];

        __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(*cacheDylib);

        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objCProtocol, bool& stopProtocol) {
            // Some protocols are missing Swift demangled names.  Add it if they don't have it

            std::string swiftDemangledName;
            if ( !objCProtocol.getDemangledName(objcVisitor) ) {
                if ( std::optional<std::string> demangledName = copySwiftDemangledName(objCProtocol.getName(objcVisitor), true) ) {
                    swiftDemangledName = std::move(*demangledName);
                }
            }

            ProtocolInfo protocolInfo = { objCProtocol.getName(objcVisitor), objCProtocol.getVMAddress(), std::move(swiftDemangledName) };
            protocoInfos.push_back(std::move(protocolInfo));
        });

        return Error();
    });

    assert(!err.hasError());

    // Merge the results in serial

    // FIXME: This is a lie
    // Reserve space for 32k protocols, as we have 30k as of writing
    const uint32_t numClassesToReserve = 1 << 15;
    this->objcProtocolOptimizer.protocols.reserve(numClassesToReserve);

    for ( uint32_t i = 0; i != this->objcOptimizer.objcDylibs.size(); ++i ) {
        const std::vector<ProtocolInfo>& protocolInfos = dylibProtocols[i];
        for ( const ProtocolInfo& protocolInfo : protocolInfos ) {
            this->objcProtocolOptimizer.protocols.insert({ protocolInfo.name, { protocolInfo.vmAddr.rawValue(), i } });

            if ( !protocolInfo.swiftDemangledName.empty() ) {
                if ( !this->objcProtocolOptimizer.swiftDemangledNamesMap.contains(protocolInfo.swiftDemangledName) ) {
                    // We will insert the string, so push the string in to the list
                    this->objcProtocolOptimizer.swiftDemangledNames.push_back(protocolInfo.swiftDemangledName);

                    // Get the string from the list as it owns the string memory
                    std::string_view string(this->objcProtocolOptimizer.swiftDemangledNames.back());

                    this->objcProtocolOptimizer.swiftDemangledNamesMap[string] = VMOffset((uint64_t)this->objcProtocolOptimizer.swiftDemangledNameStringsTotalByteSize);
                    this->objcProtocolOptimizer.swiftDemangledNameStringsTotalByteSize += protocolInfo.swiftDemangledName.size() + 1;
                }
            }
        }
    }

    if ( this->config.log.printStats ) {
        stats.add("  objc: found %lld protocols\n", (uint64_t)this->objcProtocolOptimizer.protocols.size());
    }
}

static uint32_t hashTableSize(uint32_t maxElements, uint32_t perElementData)
{
    uint32_t elementsWithPadding = maxElements * 11 / 10; // if close to power of 2, perfect hash may fail, so don't get within 10% of that
    uint32_t powTwoCapacity      = 1 << (32 - __builtin_clz(elementsWithPadding - 1));
    uint32_t headerSize          = 4 * (8 + 256);
    return headerSize + powTwoCapacity / 2 + powTwoCapacity + powTwoCapacity * perElementData;
}

void SharedCacheBuilder::estimateObjCHashTableSizes()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "estimateObjCHashTableSizes time");

    // Class/protocol tables have duplicates, which need extra entries
    uint32_t numClassesWithDuplicates = 0;
    for ( uint64_t i = 0, e = this->objcClassOptimizer.classes.bucket_count(); i != e; ++i ) {
        size_t bucketSize = this->objcClassOptimizer.classes.bucket_size(i);
        if ( bucketSize > 1 )
            numClassesWithDuplicates += (uint32_t)bucketSize;
    }
    uint32_t numProtocolsWithDuplicates = 0;
    for ( uint64_t i = 0, e = this->objcProtocolOptimizer.protocols.bucket_count(); i != e; ++i ) {
        size_t bucketSize = this->objcProtocolOptimizer.protocols.bucket_size(i);
        if ( bucketSize > 1 )
            numProtocolsWithDuplicates += (uint32_t)bucketSize;
    }

    this->objcSelectorOptimizer.selectorHashTableTotalByteSize = hashTableSize((uint32_t)this->objcSelectorOptimizer.selectorsArray.size(), 5);
    this->objcClassOptimizer.classHashTableTotalByteSize       = hashTableSize((uint32_t)this->objcClassOptimizer.classes.size(), 13) + (numClassesWithDuplicates * sizeof(uint64_t));
    this->objcProtocolOptimizer.protocolHashTableTotalByteSize = hashTableSize((uint32_t)this->objcProtocolOptimizer.protocols.size(), 13) + (numProtocolsWithDuplicates * sizeof(uint64_t));

    if ( this->config.log.printStats ) {
        stats.add("  objc: selector hash table estimated size: %lld\n", (uint64_t)this->objcSelectorOptimizer.selectorHashTableTotalByteSize);
        stats.add("  objc: class hash table estimated size: %lld\n", (uint64_t)this->objcClassOptimizer.classHashTableTotalByteSize);
        stats.add("  objc: protocol hash table estimated size: %lld\n", (uint64_t)this->objcProtocolOptimizer.protocolHashTableTotalByteSize);
    }
}

void SharedCacheBuilder::calculateObjCCanonicalProtocolsSize()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "calculateObjCCanonicalProtocolsSize time");

    uint64_t protocolSize = objc_visitor::Protocol::getSize(this->config.layout.is64);

    // We emit 1 protocol for each name, choosing an arbitrary one as the canonical one
    this->objcProtocolOptimizer.canonicalProtocolsTotalByteSize = (uint32_t)(this->objcProtocolOptimizer.namesArray.size() * protocolSize);

    if ( this->config.log.printStats ) {
        stats.add("  objc: canonical protocols size: %lld\n", (uint64_t)this->objcProtocolOptimizer.canonicalProtocolsTotalByteSize);
    }
}

// Each conformance entry is 3 uint64_t's internally, plus the space for the hash table
static uint32_t swiftHashTableSize(uint32_t maxElements)
{
    // Each bucket is 5-bytes large.  1-byte for the check byte, and 4 for the offset
    const uint32_t perElementData = 5;

    // Small tables break the estimate.  Assume they are slightly larger
    maxElements = std::max(maxElements, 16U);

    uint32_t elementsWithPadding = maxElements*11/10; // if close to power of 2, perfect hash may fail, so don't get within 10% of that
    uint32_t powTwoCapacity = 1 << (32 - __builtin_clz(elementsWithPadding - 1));
    uint32_t headerSize = 4*(8+256);
    uint32_t hashTableSize = headerSize + powTwoCapacity/2 + powTwoCapacity + powTwoCapacity*perElementData;

    // Add in the 3 uint64_t's for the payload
    return hashTableSize + (3 * sizeof(uint64_t) * maxElements);
}

void SharedCacheBuilder::estimateSwiftHashTableSizes()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "estimateSwiftHashTableSizes time");

    this->swiftProtocolConformanceOptimizer.optsHeaderByteSize = sizeof(SwiftOptimizationHeader);

    __block uint32_t numTypeConformances = 0;
    __block uint32_t numMetadataConformances = 0;
    __block uint32_t numForeignConformances = 0;

    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        __block SwiftVisitor swiftVisitor = makeInputDylibSwiftVisitor(cacheDylib);

        swiftVisitor.forEachProtocolConformance(^(const SwiftConformance &swiftConformance,
                                                  bool &stopConformance) {
            typedef SwiftConformance::SwiftProtocolConformanceFlags SwiftProtocolConformanceFlags;
            typedef SwiftConformance::SwiftTypeRefPointer SwiftTypeRefPointer;

            auto flags = swiftConformance.getProtocolConformanceFlags(swiftVisitor);
            switch ( flags.typeReferenceKind() ) {
                case SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor:
                case SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor:
                    ++numTypeConformances;

                    // We don't know for sure if we have foreign metadata, as we don't know
                    // if something like a NULL weak import will happen.  For now just assume
                    // all type entries have a foreign type
                    ++numForeignConformances;
                    break;
                case SwiftProtocolConformanceFlags::TypeReferenceKind::directObjCClassName: {
                    // We have 1 metadata conformance for each class with that name
                    SwiftTypeRefPointer typeRef = swiftConformance.getTypeRef(swiftVisitor);
                    const char* className = typeRef.getClassName(swiftVisitor);
                    size_t classCount = this->objcClassOptimizer.classes.count(className);

                    // Assume we always have at least 1 class with the name.  It would be
                    // odd not to have one
                    if ( classCount == 0 )
                        classCount = 1;

                    numMetadataConformances += classCount;
                    break;
                }
                case SwiftProtocolConformanceFlags::TypeReferenceKind::indirectObjCClass:
                    ++numMetadataConformances;
                    break;
            }
        });
    }

    auto& optimizer = this->swiftProtocolConformanceOptimizer;
    optimizer.typeConformancesHashTableSize = swiftHashTableSize(numTypeConformances);
    optimizer.metadataConformancesHashTableSize = swiftHashTableSize(numMetadataConformances);
    optimizer.foreignTypeConformancesHashTableSize = swiftHashTableSize(numForeignConformances);

    if ( this->config.log.printStats ) {
        stats.add("  swift: type hash table estimated size: %lld (from %d entries)\n",
                  (uint64_t)optimizer.typeConformancesHashTableSize, numTypeConformances);
        stats.add("  swift: metadata hash table estimated size: %lld (from %d entries)\n", (uint64_t)optimizer.metadataConformancesHashTableSize, numMetadataConformances);
        stats.add("  swift: foreign metadata hash table estimated size: %lld (from %d entries)\n", (uint64_t)optimizer.foreignTypeConformancesHashTableSize, numForeignConformances);
    }
}

void SharedCacheBuilder::calculateCacheDylibsTrie()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "calculateCacheDylibsTrie time");

    // build up all Entries in trie
    std::vector<DylibIndexTrie::Entry>        dylibEntries;
    std::unordered_map<std::string, uint32_t> dylibPathToDylibIndex;
    for ( uint32_t index = 0; index != this->cacheDylibs.size(); ++index ) {
        // FIXME: Change the Trie to std::string_view then stop making this temporary string
        const CacheDylib& cacheDylib = this->cacheDylibs[index];
        std::string installName(cacheDylib.installName);
        dylibEntries.push_back(DylibIndexTrie::Entry(installName, DylibIndex(index)));
        dylibPathToDylibIndex[installName] = index;

        // The dylib install name might not match its path, eg, libstdc++ or Cryptex paths
        // Add the path too if we have it
        if ( installName != cacheDylib.inputFile->path ) {
            dylibEntries.push_back(DylibIndexTrie::Entry(cacheDylib.inputFile->path, DylibIndex(index)));
            dylibPathToDylibIndex[cacheDylib.inputFile->path] = index;
        }
    }

    for ( const FileAlias& alias : inputAliases ) {
        const auto& pos = dylibPathToDylibIndex.find(alias.realPath);
        if ( pos != dylibPathToDylibIndex.end() ) {
            dylibEntries.push_back(DylibIndexTrie::Entry(alias.aliasPath.c_str(), pos->second));
        }
    }

    // For each alias, also see if we have intermediate aliases
    // This is the "Current -> A" symlink in say "/S/L/F/CF.fw/Current/CF"
    if ( this->options.platform == dyld3::Platform::macOS ) {
        for ( const cache_builder::FileAlias& alias : this->inputIntermediateAliases ) {
            const auto& pos = dylibPathToDylibIndex.find(alias.realPath);
            if ( pos != dylibPathToDylibIndex.end() ) {
                dylibEntries.push_back(DylibIndexTrie::Entry(alias.aliasPath.c_str(), pos->second));
            }
        }
    }

    DylibIndexTrie        dylibsTrie(dylibEntries);
    std::vector<uint8_t>& trieBytes = this->dylibTrieOptimizer.dylibsTrie;
    dylibsTrie.emit(trieBytes);
    while ( (trieBytes.size() % 8) != 0 )
        trieBytes.push_back(0);

    if ( this->config.log.printStats ) {
        stats.add("  dylibs trie estimated size: %lld\n", (uint64_t)this->dylibTrieOptimizer.dylibsTrie.size());
    }
}

void SharedCacheBuilder::estimatePatchTableSize()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "estimatePatchTableSize time");

    // The patch table consists of a series of arrays.
    // For each dylib, we have a list of all clients of that dylib
    // For each dylib we also have a list of used exports
    // For each client we then have a list of symbols used
    // And for each list of symbols, we have a list of locations to patch
    // We need to estimate a patch table based on the above lists

    __block uint32_t bindStringsLength = 0;
    __block uint32_t numBindTargets = 0;
    __block uint32_t numBinds = 0;
    uint32_t numClients = 0;
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        __block Diagnostics diag;
        cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
            mach_o::Fixups fixups(layout);
            fixups.forEachBindTarget(diag, true, 0,
                                     ^(const mach_o::Fixups::BindTargetInfo& info, bool &stop) {
                ++numBindTargets;
                bindStringsLength += strlen(info.symbolName) + 1;
            }, ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                ++numBindTargets;
                bindStringsLength += strlen(info.symbolName) + 1;
            });

            if ( cacheDylib.inputMF->hasChainedFixups() ) {
                fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                    fixups.forEachFixupInAllChains(diag, starts, false,
                                                   ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc,
                                                     uint64_t fixupSegmentOffset,
                                                     const dyld_chained_starts_in_segment* segInfo,
                                                     bool& stop) {
                        uint32_t bindOrdinal = ~0U;
                        int64_t  addend = -1;
                        if ( fixupLoc->isBind(segInfo->pointer_format, bindOrdinal, addend) )
                            ++numBinds;
                    });
                });
            } else {
                fixups.forEachBindLocation_Opcodes(diag,
                                                   ^(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                     unsigned int targetIndex, bool &stop) {
                    ++numBinds;
                }, ^(uint64_t runtimeOffset, uint32_t segmentIndex, unsigned int overrideBindTargetIndex,
                     bool &stop) {
                    ++numBinds;
                });
            }
        });

        numClients += (uint32_t)cacheDylib.dependents.size();
    }

    // Start with the patch header
    uint64_t size = sizeof(dyld_cache_patch_info_v3);

    // One of these for each dylib
    size += sizeof(dyld_cache_image_patches_v2) * this->cacheDylibs.size();

    // Estimate that 2/3 of exports are used
    size += (sizeof(dyld_cache_image_export_v2) * numBindTargets * 2) / 3;
    size += (bindStringsLength * 2) / 3;

    // 1 entry per client
    size += sizeof(dyld_cache_image_clients_v2) * numClients;

    // 1 entry per bind target
    size += sizeof(dyld_cache_patchable_export_v2) * numBindTargets;

    // 1 entry per location we bind to
    size += sizeof(dyld_cache_patchable_location_v2) * numBinds;

    this->patchTableOptimizer.patchTableTotalByteSize = size;
    
    // Reserve space for the patch infos, one per dylib
    this->patchTableOptimizer.patchInfos.resize(this->cacheDylibs.size());

    if ( this->config.log.printStats ) {
        stats.add("  patch table estimated size: %lld\n", (uint64_t)this->patchTableOptimizer.patchTableTotalByteSize);
    }
}

void SharedCacheBuilder::estimateCacheLoadersSize()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "estimateCacheLoadersSize time");

    // Dylib loaders are normally just a PrebuiltLoader, a path, and an array of dependents
    // But on macOS they may also contain patch tables
    {
        __block uint64_t size = sizeof(dyld4::PrebuiltLoaderSet);
        for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
            size += sizeof(dyld4::PrebuiltLoader);
            size += cacheDylib.installName.size() + 1;
            size += cacheDylib.inputFile->path.size() + 1;
            size = alignTo(size, alignof(dyld4::Loader::LoaderRef));
            size += sizeof(dyld4::Loader::LoaderRef) * cacheDylib.dependents.size();
            size += sizeof(Loader::DependentKind) * cacheDylib.dependents.size();
            size += sizeof(Loader::FileValidationInfo);
            size += sizeof(Loader::Region) * cacheDylib.segments.size();

            // iOSMac dylibs likely contain a patch table
            if ( (this->options.platform == dyld3::Platform::macOS)
                && startsWith(cacheDylib.installName, "/System/iOSSupport") ) {
                __block Diagnostics diag;
                cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
                    mach_o::ExportTrie exportTrie(layout);
                    exportTrie.forEachExportedSymbol(diag,
                                                     ^(const char *symbolName, uint64_t imageOffset,
                                                       uint64_t flags, uint64_t other,
                                                       const char *importName, bool &stop) {
                        size += sizeof(Loader::DylibPatch);
                    });
                });
            }
        }

        this->prebuiltLoaderBuilder.cacheDylibsLoaderSize = size;
    }

    // Estimating the size of executable loaders is hard as they may contain ObjC/Swift hash tables,
    // patch tables, etc.  For now, 16KB/executable seems about right
    this->prebuiltLoaderBuilder.executablesLoaderSize = 16_KB * this->exeInputFiles.size();

    // Estimate the trie size
    // Assume they are all at a high offset
    const uint32_t fakeOffset = 1 << 24;
    __block std::vector<DylibIndexTrie::Entry> trieEntrys;
    for ( const InputFile* inputFile : this->exeInputFiles ) {
        trieEntrys.push_back(DylibIndexTrie::Entry(inputFile->path, DylibIndex(fakeOffset)));

        // Add cdHashes to the trie so that we can look up by cdHash at runtime
        // Assumes that cdHash strings at runtime use lowercase a-f digits
        uint32_t codeSignFileOffset = 0;
        uint32_t codeSignFileSize   = 0;
        if ( inputFile->mf->hasCodeSignature(codeSignFileOffset, codeSignFileSize) ) {
            auto handler = ^(const uint8_t cdHash[20]) {
                std::string cdHashStr = "/cdhash/";
                cdHashStr.reserve(24);
                for ( int i = 0; i < 20; ++i ) {
                    uint8_t byte    = cdHash[i];
                    uint8_t nibbleL = byte & 0x0F;
                    uint8_t nibbleH = byte >> 4;
                    if ( nibbleH < 10 )
                        cdHashStr += '0' + nibbleH;
                    else
                        cdHashStr += 'a' + (nibbleH - 10);
                    if ( nibbleL < 10 )
                        cdHashStr += '0' + nibbleL;
                    else
                        cdHashStr += 'a' + (nibbleL - 10);
                }
                trieEntrys.push_back(DylibIndexTrie::Entry(cdHashStr, DylibIndex(fakeOffset)));
            };
            inputFile->mf->forEachCDHashOfCodeSignature((uint8_t*)inputFile->mf + codeSignFileOffset, codeSignFileSize,
                                                        handler);
        }
    }

    DylibIndexTrie       programTrie(trieEntrys);
    std::vector<uint8_t> trieBytes;
    programTrie.emit(trieBytes);
    this->prebuiltLoaderBuilder.executablesTrieSize = (uint32_t)alignTo((uint64_t)trieBytes.size(), 8);

    if ( this->config.log.printStats ) {
        stats.add("  dyld4 dylib Loader's estimated size: %lld\n", (uint64_t)this->prebuiltLoaderBuilder.cacheDylibsLoaderSize);
        stats.add("  dyld4 executable Loader's estimated size: %lld\n", (uint64_t)this->prebuiltLoaderBuilder.executablesLoaderSize);
        stats.add("  dyld4 executable trie estimated size: %lld\n", (uint64_t)this->prebuiltLoaderBuilder.executablesTrieSize);
    }
}

void SharedCacheBuilder::setupStubOptimizer()
{
    Timer::Scope timedScope(this->config, "setupStubOptimizer time");

    // The stub optimizer doesn't run on non-universal caches, so don't do anything there
    if ( this->options.kind != CacheKind::universal )
        return;

    this->stubOptimizer.addDefaultSymbols();

    // Walk all the dylibs, and add track any exports which are in always overridable dylibs
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( !dyld4::ProcessConfig::DyldCache::isAlwaysOverridablePath(cacheDylib.installName.data()) )
            continue;

        // Use the exports trie from the input dylib, as the cache dylib may not have an export trie
        // right now
        __block Diagnostics diag;
        cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
            mach_o::ExportTrie exportTrie(layout);

            exportTrie.forEachExportedSymbol(diag,
                                             ^(const char *symbolName, uint64_t imageOffset, uint64_t flags,
                                               uint64_t other, const char *importName, bool &stop) {
                this->stubOptimizer.neverStubEliminateStrings.push_back(symbolName);
            });
        });
        diag.assertNoError();
    }

    // Add any strings we found to the set
    auto& opt = this->stubOptimizer;
    opt.neverStubEliminate.insert(opt.neverStubEliminateStrings.begin(),
                                  opt.neverStubEliminateStrings.end());
}

void SharedCacheBuilder::computeSubCaches()
{
    Timer::Scope timedScope(this->config, "computeSubCaches time");

    // We have 3 different kinds of caches.
    // - regular: put everything in a single file
    // - large: A file is (TEXT, DATA, LINKEDIT), and we might have > 1 file
    // - split: A file is TEXT/DATA/LINKEDIT, and we've have 1 or more TEXT, and exactly 1 DATA and LINKEDIT
    if ( config.layout.large.has_value() ) {
        computeLargeSubCache();
    } else {
        computeRegularSubCache();
    }
}

// ObjC/Swift optimizations produce arrays, hash tables, string sections, etc.
// This adds all of them to the given subCache
void SharedCacheBuilder::addObjCOptimizationsToSubCache(SubCache& subCache)
{
    // Add objc header info RW
    subCache.addObjCHeaderInfoReadWriteChunk(this->config, this->objcOptimizer);

    // Add canonical objc protocols
    subCache.addObjCCanonicalProtocolsChunk(this->config, this->objcProtocolOptimizer);

    // Add objc opts header
    subCache.addObjCOptsHeaderChunk(this->objcOptimizer);

    // Add objc header info RO
    subCache.addObjCHeaderInfoReadOnlyChunk(this->objcOptimizer);

    // Add selector strings and hash table. These need to be adjacent as the table has offsets in
    // to the string section
    subCache.addObjCSelectorStringsChunk(this->objcSelectorOptimizer);
    subCache.addObjCSelectorHashTableChunk(this->objcSelectorOptimizer);

    // Add class name strings and hash table
    subCache.addObjCClassNameStringsChunk(this->objcClassOptimizer);
    subCache.addObjCClassHashTableChunk(this->objcClassOptimizer);

    // Add protocol name strings and hash table
    subCache.addObjCProtocolNameStringsChunk(this->objcProtocolOptimizer);
    subCache.addObjCProtocolHashTableChunk(this->objcProtocolOptimizer);

    // Add Swift demangled name strings found in ObjC protocol metadata
    subCache.addObjCProtocolSwiftDemangledNamesChunk(this->objcProtocolOptimizer);

    // Add ObjC IMP Caches
    subCache.addObjCIMPCachesChunk(this->objcIMPCachesOptimizer);

    // Add Swift opts header
    subCache.addSwiftOptsHeaderChunk(this->swiftProtocolConformanceOptimizer);

    // Add Swift hash tables
    subCache.addSwiftTypeHashTableChunk(this->swiftProtocolConformanceOptimizer);
    subCache.addSwiftMetadataHashTableChunk(this->swiftProtocolConformanceOptimizer);
    subCache.addSwiftForeignHashTableChunk(this->swiftProtocolConformanceOptimizer);
}

// The shared cache contains many global optimizations such as dyld4 loaders, trie's, etc.
// This adds all of them to the given subCache.
// Note objc/swift is done in addObjCOptimizationsToSubCache(), not in this method
void SharedCacheBuilder::addGlobalOptimizationsToSubCache(SubCache& subCache)
{
    // Add dylibs trie
    subCache.addCacheTrieChunk(this->dylibTrieOptimizer);

    // Add patch table
    subCache.addPatchTableChunk(this->patchTableOptimizer);

    // Add cache dylib Loader's
    subCache.addCacheDylibsLoaderChunk(this->prebuiltLoaderBuilder);

    // Add executable Loader's
    subCache.addExecutableLoaderChunk(this->prebuiltLoaderBuilder);

    // Add executable trie
    subCache.addExecutablesTrieChunk(this->prebuiltLoaderBuilder);
}

// Every subCache needs a code signature, and subCache's with DATA* need slide info.  This adds
// anything we need, based on whatever else is already in the SubCache.
void SharedCacheBuilder::addFinalChunksToSubCache(SubCache& subCache)
{
    subCache.addCacheHeaderChunk(this->cacheDylibs);

    // Add slide info for each DATA/AUTH segment.  Do this after we've added any other DATA*
    // segments
    if ( this->config.slideInfo.slideInfoFormat.has_value() )
        subCache.addSlideInfoChunks();

    // Add a code signature region
    subCache.addCodeSignatureChunk();

    // Finalize the SubCache, by removing any unused regions
    subCache.removeEmptyRegions();
}

void SharedCacheBuilder::computeRegularSubCache()
{
    // Put everything into a single file.
    SubCache subCache = SubCache::makeMainCache(this->options, true);

    // Add all the objc tables.  This must be done before we add libobjc's __TEXT
    this->addObjCOptimizationsToSubCache(subCache);

    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        bool addLinkedit = true;
        subCache.addDylib(cacheDylib, addLinkedit);
    }

    // Add all the global optimizations
    this->addGlobalOptimizationsToSubCache(subCache);

    // Reserve space in the last sub cache for dynamic config data
    subCache.addDynamicConfigChunk();

    this->addFinalChunksToSubCache(subCache);

    this->subCaches.push_back(std::move(subCache));
}

// Add stubs Chunk's for every stubs section in the given text subCache
static void addStubsChunks(const std::unordered_map<const InputFile*, CacheDylib*>& fileToDylibMap,
                           SubCache& devStubsSubCache, SubCache& customerStubsSubCache,
                           const SubCache& textSubCache)
{

    const Region& textRegion = textSubCache.regions[(uint32_t)Region::Kind::text];
    for ( const Chunk* textRegionChunk : textRegion.chunks ) {
        const DylibSegmentChunk* textChunk = textRegionChunk->isDylibSegmentChunk();
        if ( textChunk == nullptr )
            continue;
        if ( textChunk->kind != Chunk::Kind::dylibText )
            continue;

        const MachOFile* mf = textChunk->inputFile->mf;
        mf->forEachSection(^(const dyld3::MachOFile::SectionInfo &sectInfo,
                             bool malformedSectionRange, bool &stop) {
            if ( textChunk->segmentName != sectInfo.segInfo.segName )
                return;

            unsigned sectionType = (sectInfo.sectFlags & SECTION_TYPE);
            if ( sectionType != S_SYMBOL_STUBS )
                return;

            if ( strcmp(sectInfo.segInfo.segName, "__TEXT") != 0 ) {
                // stubs aren't in __TEXT.  Give up on this one for now
                return;
            }

            // Make a stubs chunk for this stubs section
            CacheDylib* cacheDylib = fileToDylibMap.at(textChunk->inputFile);
            StubsChunk* devStubsChunk = nullptr;
            StubsChunk* customerStubsChunk = nullptr;

            if ( mf->isArch("arm64e") ) {
                // For arm64e, we can only optimize __auth_stubs
                if ( !strcmp(sectInfo.sectName, "__auth_stubs") ) {
                    devStubsChunk       = &cacheDylib->developmentStubs;
                    customerStubsChunk  = &cacheDylib->customerStubs;
                }
            } else {
                // For non-arm64e, we can only optimize __stubs
                if ( !strcmp(sectInfo.sectName, "__stubs") ) {
                    devStubsChunk       = &cacheDylib->developmentStubs;
                    customerStubsChunk  = &cacheDylib->customerStubs;
                }
            }

            if ( devStubsChunk == nullptr )
                return;

            assert(devStubsChunk->segmentName.empty());
            devStubsChunk->segmentName = sectInfo.segInfo.segName;
            devStubsChunk->sectionName = sectInfo.sectName;
            devStubsChunk->subCacheFileSize = CacheFileSize(sectInfo.sectSize);
            devStubsChunk->cacheVMSize = CacheVMSize(sectInfo.sectSize);
            devStubsSubCache.addStubsChunk(devStubsChunk);

            assert(customerStubsChunk->segmentName.empty());
            customerStubsChunk->segmentName = sectInfo.segInfo.segName;
            customerStubsChunk->sectionName = sectInfo.sectName;
            customerStubsChunk->subCacheFileSize = CacheFileSize(sectInfo.sectSize);
            customerStubsChunk->cacheVMSize = CacheVMSize(sectInfo.sectSize);
            customerStubsSubCache.addStubsChunk(customerStubsChunk);
        });
    }
}

// Splits the list of subCaches to add stubs as needed.  The list will be updated to include the
// new stubs on return
static void splitSubCachesWithStubs(const BuilderOptions& options,
                                    CacheVMSize stubsLimit,
                                    const std::unordered_map<const InputFile*, CacheDylib*>& fileToDylibMap,
                                    std::list<SubCache>& subCaches)
{
    std::list<SubCache> newSubCaches;
    while ( !subCaches.empty() ) {
        SubCache subCache = std::move(subCaches.front());
        subCaches.pop_front();

        // If this is a main cache, then just move it to the new vector
        if ( subCache.isMainCache() ) {
            newSubCaches.push_back(std::move(subCache));
            continue;
        }

        assert(subCache.isSubCache());

        Region& textRegion = subCache.regions[(uint32_t)Region::Kind::text];

        bool madeNewSubCache = true;
        while ( madeNewSubCache ) {
            madeNewSubCache = false;

            CacheVMSize subCacheTextSize = CacheVMSize(0ULL);
            for ( uint64_t i = 0, e = textRegion.chunks.size(); i != e; ++i ) {
                const Chunk* chunk = textRegion.chunks[i];
                CacheVMSize textSize = chunk->cacheVMSize;

                // If we exceed the current limit, then the current subCache is complete and
                // we need to start a new one
                if ( (subCacheTextSize + textSize) > stubsLimit ) {
                    // Create a new subCache
                    newSubCaches.push_back(SubCache::makeSubCache(options));
                    SubCache& newTextSubCache = newSubCaches.back();

                    // Move all text from [0..i) to the new subCache
                    Region& newTextRegion = newTextSubCache.regions[(uint32_t)Region::Kind::text];

                    auto startIt = textRegion.chunks.begin();
                    auto endIt = startIt + i;
                    newTextRegion.chunks.insert(newTextRegion.chunks.end(), startIt, endIt);
                    textRegion.chunks.erase(startIt, endIt);

                    // Add dev/customer stubs subCache's
                    newSubCaches.push_back(SubCache::makeStubsCache(options, true));
                    auto &devStubsSubCache = newSubCaches.back();

                    newSubCaches.push_back(SubCache::makeStubsCache(options, false));
                    auto &customerStubsSubCache = newSubCaches.back();

                    addStubsChunks(fileToDylibMap, devStubsSubCache, customerStubsSubCache,
                                   newTextSubCache);

                    madeNewSubCache = true;
                    break;
                }

                subCacheTextSize += textSize;
            }
        }

        // The current subCache should have some amount of TEXT remaining, then DATA+LINKEDIT
        // Move the TEXT in to its own file too, so that we can add stubs after it
        {
            // Create a new subCache
            newSubCaches.push_back(SubCache::makeSubCache(options));
            SubCache& newTextSubCache = newSubCaches.back();

            // Move all text to the new subCache
            Region& newTextRegion = newTextSubCache.regions[(uint32_t)Region::Kind::text];
            newTextRegion.chunks = std::move(textRegion.chunks);

            // Add dev/customer stubs subCache's
            newSubCaches.push_back(SubCache::makeStubsCache(options, true));
            auto &devStubsSubCache = newSubCaches.back();

            newSubCaches.push_back(SubCache::makeStubsCache(options, false));
            auto &customerStubsSubCache = newSubCaches.back();

            addStubsChunks(fileToDylibMap, devStubsSubCache, customerStubsSubCache,
                           newTextSubCache);
        }

        // Also split the current file so that DATA/LINKEDIT are in their own files
        {
            // Create a new subCache
            newSubCaches.push_back(SubCache::makeSubCache(options));
            SubCache& newSubCache = newSubCaches.back();

            // Move all data to the new subCache
            for ( Region& oldRegion : subCache.regions ) {
                if ( oldRegion.chunks.empty() )
                    continue;

                // Move all the data regions, leave the rest
                switch ( oldRegion.kind ) {
                    case cache_builder::Region::Kind::text:
                        // Nothing to do here
                        break;
                    case cache_builder::Region::Kind::dataConst:
                    case cache_builder::Region::Kind::data:
                    case cache_builder::Region::Kind::auth:
                    case cache_builder::Region::Kind::authConst: {
                        Region& newRegion = newSubCache.regions[(uint32_t)oldRegion.kind];
                        newRegion.chunks = std::move(oldRegion.chunks);
                        break;
                    }
                    case cache_builder::Region::Kind::linkedit:
                    case cache_builder::Region::Kind::unmapped:
                    case cache_builder::Region::Kind::dynamicConfig:
                    case cache_builder::Region::Kind::codeSignature:
                    case cache_builder::Region::Kind::numKinds:
                        break;
                }
            }
        }

        // Done splitting the current subCache, so move it from the source list to the new list
        newSubCaches.push_back(std::move(subCache));
    }

    subCaches = std::move(newSubCaches);
}

void SharedCacheBuilder::makeLargeLayoutSubCaches(SubCache* firstSubCache,
                                                  std::list<SubCache>& otherCaches)
{
    SubCache* currentSubCache = firstSubCache;

    // We'll add LINKEDIT at the end.  As the shared region is <= 4GB in size, we can fit
    // all the LINKEDIT in the last subCache and still keep it in range of 32-bit offsets
    bool allLinkeditInLastSubCache = this->config.layout.allLinkeditInLastSubCache;

    // Walk all the dylibs, and create a new subCache every time we are about to cross
    // the subCacheTextLimit
    CacheVMSize subCacheTextSize(0ULL);
    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        // Peek ahead to find the __TEXT size
        CacheVMSize textSize(0ULL);
        for ( DylibSegmentChunk& segmentInfo : cacheDylib.segments ) {
            if ( segmentInfo.kind == DylibSegmentChunk::Kind::dylibText )
                textSize += segmentInfo.cacheVMSize;
        }

        // If we exceed the current limit, then the current subCache is complete and we need
        // to start a new one
        if ( (subCacheTextSize + textSize) > this->config.layout.large->subCacheTextLimit ) {
            // Create a new subCache
            otherCaches.push_back(SubCache::makeSubCache(this->options));
            currentSubCache = &otherCaches.back();

            // Reset the limit for the next subCache
            subCacheTextSize = CacheVMSize(0ULL);
        }

        subCacheTextSize += textSize;

        // The subCache with libobjc gets the header info sections
        // Add all the objc tables.  This must be done before we add libobjc's __TEXT
        if ( cacheDylib.installName == "/usr/lib/libobjc.A.dylib" )
            this->addObjCOptimizationsToSubCache(*currentSubCache);

        // We'll add LINKEDIT at the end.  As the shared region is <= 4GB in size, we can fit
        // all the LINKEDIT in the last subCache and still keep it in range of 32-bit offsets
        bool addLinkedit = !allLinkeditInLastSubCache;
        currentSubCache->addDylib(cacheDylib, addLinkedit);
    }

    // Add all the remaining content in to the final (current) subCache

    // Add linkedit chunks from dylibs, if needed
    if ( allLinkeditInLastSubCache ) {
        for ( CacheDylib& cacheDylib : this->cacheDylibs )
            currentSubCache->addLinkeditFromDylib(cacheDylib);
    }


    // Add all the global optimizations
    this->addGlobalOptimizationsToSubCache(*currentSubCache);
}

void SharedCacheBuilder::setSubCacheNames()
{
    SubCache* mainDevelopmentCache = nullptr;
    SubCache* mainCustomerCache = nullptr;
    for ( SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainDevelopmentCache() ) {
            assert(mainDevelopmentCache == nullptr);
            mainDevelopmentCache = &subCache;
            continue;
        }
        if ( subCache.isMainCustomerCache() ) {
            assert(mainCustomerCache == nullptr);
            mainCustomerCache = &subCache;
            continue;
        }
    }

    // Set the names of any subCaches
    if ( mainDevelopmentCache != nullptr ) {
        size_t subCacheIndex = 1;
        for ( SubCache* subCache : mainDevelopmentCache->subCaches ) {
            subCache->setSuffix(this->options.platform, this->options.forceDevelopmentSubCacheSuffix,
                                subCacheIndex);
            ++subCacheIndex;
        }
    }

    if ( mainCustomerCache != nullptr ) {
        size_t subCacheIndex = 1;
        for ( SubCache* subCache : mainCustomerCache->subCaches ) {
            subCache->setSuffix(this->options.platform, this->options.forceDevelopmentSubCacheSuffix,
                                subCacheIndex);
            ++subCacheIndex;
        }
    }
}

void SharedCacheBuilder::computeLargeSubCache()
{
    // Keeps track of any subCaches we add after the main cache
    std::list<SubCache> allSubCaches;

    // Split in to multple files.  Where each file gets its own TEXT/DATA*/LINKEDIT
    switch ( this->options.kind ) {
        case CacheKind::development: {
            // The first file in a development configuration is the main cache, and also some
            // amount of text and maybe data
            allSubCaches.push_back(SubCache::makeMainCache(this->options, true));
            makeLargeLayoutSubCaches(&allSubCaches.back(), allSubCaches);
            break;
        }
        case CacheKind::universal: {
            // Add main caches
            allSubCaches.push_back(SubCache::makeMainCache(this->options, true));
            allSubCaches.push_back(SubCache::makeMainCache(this->options, false));

            allSubCaches.push_back(SubCache::makeSubCache(this->options));
            makeLargeLayoutSubCaches(&allSubCaches.back(), allSubCaches);

            // Loop over all the subcaches, and split them every 110MB
            CacheVMSize stubsLimit = this->config.layout.contiguous->subCacheStubsLimit;

            // Make a map of input file -> cache dylib, as the text chunks we walk
            // only know about the input file
            std::unordered_map<const InputFile*, CacheDylib*> fileToDylibMap;
            for ( CacheDylib& cacheDylib : cacheDylibs )
                fileToDylibMap[cacheDylib.inputFile] = &cacheDylib;

            splitSubCachesWithStubs(this->options, stubsLimit, fileToDylibMap, allSubCaches);
            break;
        }
    }

    // Move all the subCaches in to the final buffer
    // We're going to assume things about the layout of the caches in the buffer, so we need
    // to start with an empty buffer to avoid breaking those assumptions
    assert(this->subCaches.empty());

    // Work out how many caches we need.  The main caches are going to take pointers to other
    // caches, so we have to get this right, and never reallocate the vector later
    uint64_t totalSubCaches = allSubCaches.size();
    if ( this->options.localSymbolsMode == LocalSymbolsMode::unmap )
        totalSubCaches += 1; // Add 1 for .symbols
    this->subCaches.reserve(totalSubCaches);

    // Move all the caches in to the vector, pointing main caches at subCaches
    {
        for ( SubCache& subCache : allSubCaches ) {
            this->subCaches.push_back(std::move(subCache));
        }
        allSubCaches.clear();

        SubCache* mainDevelopmentCache = nullptr;
        SubCache* mainCustomerCache = nullptr;
        for ( SubCache& subCache : this->subCaches ) {
            if ( subCache.isMainDevelopmentCache() ) {
                assert(mainDevelopmentCache == nullptr);
                mainDevelopmentCache = &subCache;
                continue;
            }
            if ( subCache.isMainCustomerCache() ) {
                assert(mainCustomerCache == nullptr);
                mainCustomerCache = &subCache;
                continue;
            }

            if ( subCache.isSubCache() ) {
                // Sub caches should be added to any "main" caches
                if ( mainDevelopmentCache != nullptr ) {
                    mainDevelopmentCache->subCaches.push_back(&subCache);
                }
                if ( mainCustomerCache != nullptr ) {
                    mainCustomerCache->subCaches.push_back(&subCache);
                }
                continue;
            }

            // Development stubs only get added to the main dev cache
            if ( subCache.isStubsDevelopmentCache() ) {
                assert(mainDevelopmentCache != nullptr);
                mainDevelopmentCache->subCaches.push_back(&subCache);
                continue;
            }

            // Customer stubs only get added to the main dev cache
            if ( subCache.isStubsCustomerCache() ) {
                assert(mainCustomerCache != nullptr);
                mainCustomerCache->subCaches.push_back(&subCache);
                continue;
            }

            // Unknown cache kind
            assert(0);
        }
    }

    // Reserve address space in the last sub cache for dynamic config data
    subCaches.back().addDynamicConfigChunk();

    this->setSubCacheNames();

    // Finalize all the subCaches, including any new ones we added
    for ( SubCache& subCache : this->subCaches )
        this->addFinalChunksToSubCache(subCache);
}

Error SharedCacheBuilder::copyLocalSymbols(SubCache& subCache,
                                           const std::span<LinkeditDataChunk*> symbolStringChunks,
                                           const FileToDylibMap& fileToDylibMap,
                                           const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                           const uint32_t redactedStringOffset,
                                           uint32_t& stringBufferSize,
                                           uint32_t& sourceStringSize,
                                           uint32_t& sourceStringCount)
{
    // Locals last, as they are special and possibly stripped/unmapped
    if ( options.localSymbolsMode == cache_builder::LocalSymbolsMode::strip )
        return Error();

    // Map from strings to their offsets in to the new string buffer
    auto& stringMap = subCache.symbolStringsOptimizer.stringMap;

    for ( LinkeditDataChunk* chunk : symbolStringChunks ) {
        const MachOFile* mf = chunk->inputFile->mf;
        CacheDylib* dylib = fileToDylibMap.at(chunk->inputFile);

        UnmappedSymbolsOptimizer::LocalSymbolInfo* symbolInfo = nullptr;
        if ( options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
            symbolInfo = fileToSymbolInfoMap.at(chunk->inputFile);

            if ( config.layout.is64 )
                symbolInfo->nlistStartIndex = (uint32_t)this->unmappedSymbolsOptimizer.symbolNlistChunk.nlist64.size();
            else
                symbolInfo->nlistStartIndex = (uint32_t)this->unmappedSymbolsOptimizer.symbolNlistChunk.nlist32.size();
        }

        __block Diagnostics diag;
        mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
            mach_o::SymbolTable symbolTable(layout);

            dylib->optimizedSymbols.localsStartIndex = 0;
            symbolTable.forEachLocalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
                // Note we don't need to check for stabs, exports, etc.  forEachLocalSymbol() did that for us
                std::string_view symbolString(symbolName);
                sourceStringSize += symbolString.size() + 1;
                ++sourceStringCount;

                uint32_t symbolStringOffset = ~0U;
                if ( options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
                    // copy all local symbol to unmmapped locals area
                    auto itAndInserted = this->unmappedSymbolsOptimizer.stringMap.insert({ symbolString, this->unmappedSymbolsOptimizer.stringBufferSize });
                    // If we inserted the string, then account for the space
                    if ( itAndInserted.second )
                        this->unmappedSymbolsOptimizer.stringBufferSize += symbolString.size() + 1;

                    // Add this to the list for the unmapped locals nlist
                    if ( config.layout.is64 ) {
                        struct nlist_64 newSymbol;
                        newSymbol.n_un.n_strx   = itAndInserted.first->second;
                        newSymbol.n_type        = n_type;
                        newSymbol.n_sect        = n_sect;
                        newSymbol.n_desc        = n_desc;
                        newSymbol.n_value       = n_value;
                        this->unmappedSymbolsOptimizer.symbolNlistChunk.nlist64.push_back(newSymbol);
                    } else {
                        struct nlist newSymbol;
                        newSymbol.n_un.n_strx   = itAndInserted.first->second;
                        newSymbol.n_type        = n_type;
                        newSymbol.n_sect        = n_sect;
                        newSymbol.n_desc        = n_desc;
                        newSymbol.n_value       = (uint32_t)n_value;
                        this->unmappedSymbolsOptimizer.symbolNlistChunk.nlist32.push_back(newSymbol);
                    }
                    ++symbolInfo->nlistCount;

                    // if removing local symbols, change __text symbols to "<redacted>" so backtraces don't have bogus names
                    if ( n_sect == 1 ) {
                        symbolStringOffset = redactedStringOffset;
                    } else {
                        // Symbols other than __text are dropped
                        return;
                    }
                } else {
                    // Keep this string so make space for it.
                    auto itAndInserted = stringMap.insert({ symbolString, stringBufferSize });
                    // If we inserted the string, then account for the space
                    if ( itAndInserted.second )
                        stringBufferSize += symbolString.size() + 1;

                    symbolStringOffset = itAndInserted.first->second;
                }

                // Add this to the list for the new nlist
                if ( config.layout.is64 ) {
                    struct nlist_64 newSymbol;
                    newSymbol.n_un.n_strx   = symbolStringOffset;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = n_value;
                    dylib->optimizedSymbols.nlist64.push_back(newSymbol);
                } else {
                    struct nlist newSymbol;
                    newSymbol.n_un.n_strx   = symbolStringOffset;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = (uint32_t)n_value;
                    dylib->optimizedSymbols.nlist32.push_back(newSymbol);
                }
                dylib->optimizedSymbols.localsCount++;
            });
        });

        if ( diag.hasError() )
            return Error("Couldn't get dylib layout because: %s", diag.errorMessageCStr());
    }

    return Error();
}

Error SharedCacheBuilder::copyExportedSymbols(SubCache& subCache,
                                              const std::span<LinkeditDataChunk*> symbolStringChunks,
                                              const FileToDylibMap& fileToDylibMap,
                                              const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                              std::vector<OldToNewIndicesMap>& oldToNewIndicesMaps,
                                              const uint32_t redactedStringOffset,
                                              uint32_t& stringBufferSize,
                                              uint32_t& sourceStringSize,
                                              uint32_t& sourceStringCount)
{
    // Map from strings to their offsets in to the new string buffer
    auto& stringMap = subCache.symbolStringsOptimizer.stringMap;

    for ( LinkeditDataChunk* chunk : symbolStringChunks ) {
        const MachOFile* mf = chunk->inputFile->mf;
        CacheDylib* dylib = fileToDylibMap.at(chunk->inputFile);

        OldToNewIndicesMap& oldToNewIndices = oldToNewIndicesMaps[dylib->cacheIndex];

        __block Diagnostics diag;
        mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
            mach_o::SymbolTable symbolTable(layout);

            __block uint32_t oldSymbolIndex = layout.linkedit.globalSymbolTable.entryIndex;

            dylib->optimizedSymbols.globalsStartIndex = dylib->optimizedSymbols.localsCount;
            symbolTable.forEachGlobalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
                // Note we don't need to check for stabs, exports, etc.  forEachGlobalSymbol() did that for us
                std::string_view symbolString(symbolName);
                sourceStringSize += symbolString.size() + 1;
                ++sourceStringCount;

                // Skip symbols we don't need at runtime
                if ( strncmp(symbolName, ".objc_", 6) == 0 ) {
                    ++oldSymbolIndex;
                    return;
                }
                if ( strncmp(symbolName, "$ld$", 4) == 0 ) {
                    ++oldSymbolIndex;
                    return;
                }

                auto itAndInserted = stringMap.insert({ symbolString, stringBufferSize });
                // If we inserted the string, then account for the space
                if ( itAndInserted.second )
                    stringBufferSize += symbolString.size() + 1;

                // Add this to the list for the new nlist
                if ( config.layout.is64 ) {
                    struct nlist_64 newSymbol;
                    newSymbol.n_un.n_strx   = itAndInserted.first->second;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = n_value;
                    dylib->optimizedSymbols.nlist64.push_back(newSymbol);
                } else {
                    struct nlist newSymbol;
                    newSymbol.n_un.n_strx   = itAndInserted.first->second;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = (uint32_t)n_value;
                    dylib->optimizedSymbols.nlist32.push_back(newSymbol);
                }

                uint32_t newSymbolIndex = dylib->optimizedSymbols.globalsStartIndex + dylib->optimizedSymbols.globalsCount;
                oldToNewIndices[oldSymbolIndex] = newSymbolIndex;
                ++oldSymbolIndex;

                dylib->optimizedSymbols.globalsCount++;
            });
        });

        if ( diag.hasError() )
            return Error("Couldn't get dylib layout because: %s", diag.errorMessageCStr());
    }

    return Error();
}

Error SharedCacheBuilder::copyImportedSymbols(SubCache& subCache,
                                              const std::span<LinkeditDataChunk*> symbolStringChunks,
                                              const FileToDylibMap& fileToDylibMap,
                                              const FileToSymbolInfoMap& fileToSymbolInfoMap,
                                              std::vector<OldToNewIndicesMap>& oldToNewIndicesMaps,
                                              const uint32_t redactedStringOffset,
                                              uint32_t& stringBufferSize,
                                              uint32_t& sourceStringSize,
                                              uint32_t& sourceStringCount)
{
    if ( options.localSymbolsMode == cache_builder::LocalSymbolsMode::strip )
        return Error();

    // Map from strings to their offsets in to the new string buffer
    auto& stringMap = subCache.symbolStringsOptimizer.stringMap;

    for ( LinkeditDataChunk* chunk : symbolStringChunks ) {
        const MachOFile* mf = chunk->inputFile->mf;
        CacheDylib* dylib = fileToDylibMap.at(chunk->inputFile);

        OldToNewIndicesMap& oldToNewIndices = oldToNewIndicesMaps[dylib->cacheIndex];

        __block Diagnostics diag;
        mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
            mach_o::SymbolTable symbolTable(layout);

            __block uint32_t oldSymbolIndex = layout.linkedit.undefSymbolTable.entryIndex;

            dylib->optimizedSymbols.undefsStartIndex = dylib->optimizedSymbols.localsCount + dylib->optimizedSymbols.globalsCount;
            symbolTable.forEachImportedSymbol(diag, ^(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
                std::string_view symbolString(symbolName);
                sourceStringSize += symbolString.size() + 1;
                ++sourceStringCount;

                auto itAndInserted = stringMap.insert({ symbolString, stringBufferSize });
                // If we inserted the string, then account for the space
                if ( itAndInserted.second )
                    stringBufferSize += symbolString.size() + 1;

                // Add this to the list for the new nlist
                if ( config.layout.is64 ) {
                    struct nlist_64 newSymbol;
                    newSymbol.n_un.n_strx   = itAndInserted.first->second;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = n_value;
                    dylib->optimizedSymbols.nlist64.push_back(newSymbol);
                } else {
                    struct nlist newSymbol;
                    newSymbol.n_un.n_strx   = itAndInserted.first->second;
                    newSymbol.n_type        = n_type;
                    newSymbol.n_sect        = n_sect;
                    newSymbol.n_desc        = n_desc;
                    newSymbol.n_value       = (uint32_t)n_value;
                    dylib->optimizedSymbols.nlist32.push_back(newSymbol);
                }

                uint32_t newSymbolIndex = dylib->optimizedSymbols.undefsStartIndex + dylib->optimizedSymbols.undefsCount;
                oldToNewIndices[oldSymbolIndex] = newSymbolIndex;
                ++oldSymbolIndex;

                dylib->optimizedSymbols.undefsCount++;
            });
        });

        if ( diag.hasError() )
            return Error("Couldn't get dylib layout because: %s", diag.errorMessageCStr());
    }

    return Error();
}

// This runs after we've assigned Chunk's to SubCache's, but before we've actually
// allocated the space for the SubCache's.
// This pass takes all the LINKEDIT symbol strings and deduplicates them for the given
// SubCache LINKEDIT region
Error SharedCacheBuilder::calculateSubCacheSymbolStrings()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "calculateSubCacheSymbolStrings time");

    // LinkeditChunk's don't have a pointer to their cache dylib.  Make a map for them
    std::unordered_map<const InputFile*, CacheDylib*> fileToDylibMap;
    fileToDylibMap.reserve(this->cacheDylibs.size());
    for ( CacheDylib& dylib : this->cacheDylibs )
        fileToDylibMap[dylib.inputFile] = &dylib;

    // Create an optimizer for the .symbols file, if we need it
    std::unordered_map<const InputFile*, UnmappedSymbolsOptimizer::LocalSymbolInfo*> fileToSymbolInfoMap;
    if ( this->options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
        this->unmappedSymbolsOptimizer.symbolInfos.resize(this->cacheDylibs.size());

        fileToSymbolInfoMap.reserve(this->cacheDylibs.size());
        for ( uint32_t i = 0; i != this->cacheDylibs.size(); ++i )
            fileToSymbolInfoMap[this->cacheDylibs[i].inputFile] = &this->unmappedSymbolsOptimizer.symbolInfos[i];

        // tradition for start of pool to be empty string
        this->unmappedSymbolsOptimizer.stringMap["\0"] = 0;
        ++this->unmappedSymbolsOptimizer.stringBufferSize;
    }

    for ( SubCache& subCache : this->subCaches ) {

        // Find the LINKEDIT in each SubCache, if it has any
        Region* linkeditRegion = nullptr;
        for ( Region& region : subCache.regions ) {
            if ( region.kind == Region::Kind::linkedit ) {
                linkeditRegion = &region;
                break;
            }
        }

        if ( linkeditRegion == nullptr )
            continue;

        // Find the symbol strings Chunk's in the LINKEDIT Region
        std::vector<LinkeditDataChunk*> symbolStringChunks;
        for ( Chunk* chunk : linkeditRegion->chunks ) {
            const LinkeditDataChunk* linkeditChunk = chunk->isLinkeditDataChunk();
            if ( linkeditChunk == nullptr )
                continue;
            if ( linkeditChunk->kind == cache_builder::Chunk::Kind::linkeditSymbolStrings )
                symbolStringChunks.push_back((LinkeditDataChunk*)linkeditChunk);
        }

        if ( symbolStringChunks.empty() )
            continue;

        // Got some symbol strings to deduplicate.  Walk the nlist for this dylib to work
        // out which symbols we have
        uint32_t stringBufferSize = 0;
        uint32_t sourceStringSize = 0;
        uint32_t sourceStringCount = 0;

        // Map from strings to their offsets in to the new string buffer
        auto& stringMap = subCache.symbolStringsOptimizer.stringMap;

        // Map from old -> new indices in the string table. This is used to update the indirect symbol table
        // We make 1 map per cache dylib
        std::vector<OldToNewIndicesMap> oldToNewIndicesMaps;
        oldToNewIndicesMaps.resize(this->cacheDylibs.size());

        // tradition for start of pool to be empty string
        stringMap["\0"] = 0;
        ++stringBufferSize;

        // If we are unmapping linkedit, then we need the redacted symbol
        uint32_t redactedStringOffset = ~0U;
        if ( this->options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
            redactedStringOffset = stringBufferSize;
            stringMap["<redacted>"] = stringBufferSize;
            stringBufferSize += strlen("<redacted>") + 1;
        }

        // The dsc_extractor cares about the order here.  So always do locals, then exports, then imports
        Error localsError = copyLocalSymbols(subCache, symbolStringChunks, fileToDylibMap,
                                             fileToSymbolInfoMap, redactedStringOffset,
                                             stringBufferSize, sourceStringSize, sourceStringCount);
        if ( localsError.hasError() )
            return localsError;

        Error exportsError = copyExportedSymbols(subCache, symbolStringChunks, fileToDylibMap,
                                                 fileToSymbolInfoMap, oldToNewIndicesMaps, redactedStringOffset,
                                                 stringBufferSize, sourceStringSize, sourceStringCount);
        if ( exportsError.hasError() )
            return localsError;

        Error importsError = copyImportedSymbols(subCache, symbolStringChunks, fileToDylibMap,
                                                 fileToSymbolInfoMap, oldToNewIndicesMaps, redactedStringOffset,
                                                 stringBufferSize, sourceStringSize, sourceStringCount);
        if ( importsError.hasError() )
            return localsError;

        // Delete the old unoptimized nlists
        auto isNList = [](const Chunk* chunk) {
            const LinkeditDataChunk* linkeditChunk = chunk->isLinkeditDataChunk();
            return (linkeditChunk != nullptr) && linkeditChunk->isNList();
        };
        linkeditRegion->chunks.erase(std::remove_if(linkeditRegion->chunks.begin(), linkeditRegion->chunks.end(), isNList),
                                     linkeditRegion->chunks.end());

        // Delete the old unoptimized symbol strings
        auto isSymbolStrings = [](const Chunk* chunk) {
            const LinkeditDataChunk* linkeditChunk = chunk->isLinkeditDataChunk();
            return (linkeditChunk != nullptr) && linkeditChunk->isNSymbolStrings();
        };
        linkeditRegion->chunks.erase(std::remove_if(linkeditRegion->chunks.begin(),
                                                    linkeditRegion->chunks.end(),
                                                    isSymbolStrings),
                                     linkeditRegion->chunks.end());

        // Add the new chunks to the subCache
        subCache.optimizedSymbolStrings = std::make_unique<SymbolStringsChunk>();
        subCache.optimizedSymbolStrings->kind               = cache_builder::Chunk::Kind::optimizedSymbolStrings;
        subCache.optimizedSymbolStrings->cacheVMSize        = CacheVMSize((uint64_t)stringBufferSize);
        subCache.optimizedSymbolStrings->subCacheFileSize   = CacheFileSize((uint64_t)stringBufferSize);
        linkeditRegion->chunks.push_back(subCache.optimizedSymbolStrings.get());

        // FIXME: Do we need this. No-one seems to read it from here, or could get it from the subCache instead
        subCache.symbolStringsOptimizer.symbolStringsChunk = subCache.optimizedSymbolStrings.get();

        // The dylibs need to know what symbol strings to reference in their LINKEDIT
        for ( const LinkeditDataChunk* chunk : symbolStringChunks ) {
            CacheDylib* dylib = fileToDylibMap.at(chunk->inputFile);
            dylib->subCacheSymbolStrings = subCache.optimizedSymbolStrings.get();
        }

        // Add the nlists from the dylibs to the subCache
        for ( const LinkeditDataChunk* chunk : symbolStringChunks ) {
            CacheDylib* dylib = fileToDylibMap.at(chunk->inputFile);
            NListChunk* nlistChunk = &dylib->optimizedSymbols;

            uint64_t nlistSize = 0;
            if ( config.layout.is64 )
                nlistSize = sizeof(struct nlist_64) * nlistChunk->nlist64.size();
            else
                nlistSize = sizeof(struct nlist) * nlistChunk->nlist32.size();

            nlistChunk->kind                = cache_builder::Chunk::Kind::optimizedSymbolNList;
            nlistChunk->cacheVMSize         = CacheVMSize(nlistSize);
            nlistChunk->subCacheFileSize    = CacheFileSize(nlistSize);

            linkeditRegion->chunks.push_back(nlistChunk);
        }

        if ( this->config.log.printStats ) {
            stats.add("  linkedit: deduplicated %d symbols strings to %d.  %dMB -> %dMB\n",
                      sourceStringCount, (uint32_t)stringMap.size(), sourceStringSize >> 20, stringBufferSize >> 20);
        }

        // Update the indirect symbol table for any dylib which had moved symbols
        for ( uint32_t i = 0; i != this->cacheDylibs.size(); ++i ) {
            CacheDylib& cacheDylib = this->cacheDylibs[i];
            const OldToNewIndicesMap& oldToNewIndicesMap = oldToNewIndicesMaps[i];
            if ( oldToNewIndicesMap.empty() )
                continue;

            // Walk the table on the dylib, and update any entries
            __block Diagnostics diag;
            cacheDylib.inputMF->withFileLayout(diag, ^(const mach_o::Layout& layout) {
                mach_o::SymbolTable symbolTable(layout);

                cacheDylib.indirectSymbolTable.reserve(layout.linkedit.indirectSymbolTable.entryCount);

                symbolTable.forEachIndirectSymbol(diag, ^(const char* symbolName, uint32_t symNum) {
                    if ( (symNum == INDIRECT_SYMBOL_ABS)
                        || (symNum == INDIRECT_SYMBOL_LOCAL)
                        || (symNum == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) ) ) {
                        cacheDylib.indirectSymbolTable.push_back(symNum);
                    } else {
                        uint32_t oldSymbolIndex = symNum;

                        // FIXME: oldToNewIndicesMap might not actually contain some symbols
                        // For example, forEachGlobalSymbol skips N_INDR but we need those here
                        // uint32_t newSymbolIndex = oldToNewIndicesMap.at(oldSymbolIndex);
                        auto it = oldToNewIndicesMap.find(oldSymbolIndex);
                        uint32_t newSymbolIndex = 0;
                        if ( it != oldToNewIndicesMap.end() )
                            newSymbolIndex = it->second;
                        cacheDylib.indirectSymbolTable.push_back(newSymbolIndex);
                    }
                });
            });
            diag.assertNoError();
        }
    }

    // Remove the linkedit chunks from the dylibs too.  They now use their own optimizedSymbols field
    for ( CacheDylib& dylib : this->cacheDylibs ) {
        auto isNList = [](const LinkeditDataChunk& chunk) {
            return chunk.kind == cache_builder::Chunk::Kind::linkeditSymbolNList;
        };
        dylib.linkeditChunks.remove_if(isNList);

        auto isSymbolStrings = [](const LinkeditDataChunk& chunk) {
            return chunk.kind == cache_builder::Chunk::Kind::linkeditSymbolStrings;
        };
        dylib.linkeditChunks.remove_if(isSymbolStrings);
    }

    // Create the .symbols file, if we have one
    if ( this->options.localSymbolsMode == cache_builder::LocalSymbolsMode::unmap ) {
        // Make sure we won't cause an allocation
        assert(this->subCaches.size() < this->subCaches.capacity());
        this->subCaches.push_back(SubCache::makeSymbolsCache());
        SubCache& localSymbolsSubCache = this->subCaches.back();
        localSymbolsSubCache.addUnmappedSymbols(this->config, this->unmappedSymbolsOptimizer);

        // Finalize the symbols cache
        addFinalChunksToSubCache(localSymbolsSubCache);
    }

    return Error();
}

static void parseGOTs(const CacheDylib* dylib, const DylibSegmentChunk* chunk,
                      std::string_view segmentName, std::string_view sectionName,
                      DylibSectionCoalescer::OptimizedSection& dylibOptimizedSection)
{
    const MachOFile* mf = dylib->inputMF;
    __block Diagnostics diag;

    const bool log = false;

    // Skip ineligible dylibs
    if ( !mf->hasChainedFixups() )
        return;

    // Some dylibs have auth gots in segments other than __AUTH_CONST. Skip them for now
    if ( chunk->segmentName != segmentName )
        return;

    __block bool supportsGOTUniquing = false;
    mf->withFileLayout(diag, ^(const mach_o::Layout& layout) {
        mach_o::SplitSeg splitSeg(layout);

        if ( splitSeg.isV2() )
            supportsGOTUniquing = true;
    });

    if ( !supportsGOTUniquing )
        return;

    if ( mf->isArch("x86_64") || mf->isArch("x86_64h") ) {
        __block bool oldLinker = false;
        mf->forEachSupportedBuildTool(^(dyld3::Platform platform, uint32_t tool, uint32_t version) {
            uint32_t majorVersion = version >> 16;
            // uint32_t minorVersion = (version >> 8) && 0xFF;
            // uint32_t veryMinorVersion = version && 0xFF;

            if ( tool == TOOL_LD ) {
                if ( majorVersion < 803 )
                    oldLinker = true;
            }
        });

        if ( oldLinker )
            return;
    }

    // rdar://89319146
    if ( mf->isArch("x86_64") || mf->isArch("x86_64h") ) {
        if ( !strcmp(mf->installName(), "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation") )
            return;
        if ( !strcmp(mf->installName(), "/usr/lib/system/libdispatch.dylib") )
            return;
    }

    // rdar://86911139
    if ( mf->builtForPlatform(dyld3::Platform::iOS)
        && !strcmp(mf->installName(), "/System/Library/PrivateFrameworks/CoreUI.framework/CoreUI") )
        return;

    // Dylib segment is eligible.  Walk the GOTs
    __block std::optional<dyld3::MachOAnalyzer::SectionInfo> gotSectionInfo;
    __block uint16_t chainedFixupFormat = 0;
    mf->forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( sectInfo.segInfo.segName != segmentName )
            return;
        if ( sectInfo.sectName != sectionName)
            return;
        gotSectionInfo = sectInfo;

        // As we found the section we want, also get its chained fixup format
        mf->withFileLayout(diag, ^(const mach_o::Layout& layout) {
            mach_o::Fixups fixups(layout);

            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                MachOFile::forEachFixupChainSegment(diag, starts,
                                                    ^(const dyld_chained_starts_in_segment* segmentInfo,
                                                      uint32_t segIndex, bool& stopSegment) {
                    if ( segIndex == sectInfo.segInfo.segIndex ) {
                        chainedFixupFormat = segmentInfo->pointer_format;
                        stopSegment = true;
                    }
                });
            });
        });
        assert(chainedFixupFormat != 0);

        stop = true;
    });

    if ( diag.hasError() )
        return;

    if ( !gotSectionInfo )
        return;

    __block std::vector<mach_o::Fixups::BindTargetInfo> bindTargets;
    mf->withFileLayout(diag, ^(const mach_o::Layout& layout) {
        mach_o::Fixups fixups(layout);

        fixups.forEachBindTarget(diag, false, 0, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
            bindTargets.push_back(info);
            if ( diag.hasError() )
                stop = true;
        }, ^(const mach_o::Fixups::BindTargetInfo& info, bool& stop) {
            // This shouldn't happen with chained fixups
            assert(0);
        });
    });

    if ( diag.hasError() )
        return;

    __block std::vector<const char*> dependents;
    mf->forEachDependentDylib(^(const char *loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool &stop) {
        dependents.push_back(loadPath);
    });

    auto* cacheGotSection = (CoalescedGOTSection*)dylibOptimizedSection.subCacheSection;
    DylibSectionCoalescer::DylibSectionOffsetToCacheSectionOffset& offsetMap = dylibOptimizedSection.offsetMap;

    // Walk the entries in this section
    // File layout so just add the file offset
    const uint8_t* content      = (const uint8_t*)mf + gotSectionInfo->sectFileOffset;
    const uint8_t* pos          = content;
    const uint8_t* end          = content + gotSectionInfo->sectSize;
    uint32_t       pointerSize  = mf->pointerSize();
    assert((gotSectionInfo->sectSize % pointerSize == 0));
    while ( pos != end ) {
        const dyld3::MachOLoaded::ChainedFixupPointerOnDisk* fixup = (const dyld3::MachOLoaded::ChainedFixupPointerOnDisk*)pos;
        pos += pointerSize;
        uint32_t bindOrdinal = ~0U;
        int64_t  addend = -1;
        bool isBind = fixup->isBind(chainedFixupFormat, bindOrdinal, addend);

        uint32_t sourceSectionOffset = (uint32_t)((uint64_t)fixup - (uint64_t)content);

        // Note down rebases, but otherwise skip them
        if ( !isBind ) {
            dylibOptimizedSection.unoptimizedOffsets.insert(sourceSectionOffset);
            continue;
        }

        // We don't support addends right now.  But hopefully GOTs don't need them anyway
        if ( addend != 0 )
            continue;

        const mach_o::Fixups::BindTargetInfo& bindTarget = bindTargets[bindOrdinal];

        // TODO: Weak GOTs.  See rdar://86510941
        const char* targetInstallName = nullptr;
        if ( (bindTarget.libOrdinal > 0) && ((unsigned)bindTarget.libOrdinal <= dependents.size()) ) {
            targetInstallName = dependents[bindTarget.libOrdinal - 1];
        } else {
            dylibOptimizedSection.unoptimizedOffsets.insert(sourceSectionOffset);
            continue;
        }

        MachOFile::PointerMetaData pmd(fixup, chainedFixupFormat);

        typedef CoalescedGOTSection::GOTKey Key;
        Key key = { bindTarget.symbolName, targetInstallName, pmd };

        int cacheSectionOffset = (int)(cacheGotSection->gotTargetsToOffsets.size() * pointerSize);
        auto itAndInserted = cacheGotSection->gotTargetsToOffsets.insert({ key, cacheSectionOffset });
        if ( itAndInserted.second ) {
            // We inserted the element, so its offset is already valid.  Nothing else to do

            if (log) {
                uint64_t gotOffset = ((uint64_t)pos - (uint64_t)content) - pointerSize;
                printf("%s[%lld]: %s -> (%s, %s)\n",
                       sectionName.data(), gotOffset, mf->installName(),
                       key.targetDylibName.data(), key.targetSymbolName.data());
            }
        } else {
            // Debugging only.  If we didn't include the GOT then we saved that many bytes
            cacheGotSection->savedSpace += pointerSize;
            cacheSectionOffset = itAndInserted.first->second;
        }

        // Now keep track of this offset in our source dylib as pointing to this offset
        offsetMap[sourceSectionOffset] = cacheSectionOffset;
    }

    // Record which segment/section we just visited
    uint32_t segmentIndex = gotSectionInfo->segInfo.segIndex;
    dylibOptimizedSection.segmentIndex = segmentIndex;
    dylibOptimizedSection.sectionVMOffsetInSegment = VMOffset(gotSectionInfo->sectAddr - gotSectionInfo->segInfo.vmAddr);
}

// This runs after we've assigned Chunk's to SubCache's, but before we've actually
// allocated the space for the SubCache's.
// This pass takes all the GOTs and deduplicates them for the given SubCache DATA/AUTH region
Error SharedCacheBuilder::calculateUniqueGOTs()
{
    // Skip this optimiation on simulator until we've qualified it there
    if ( this->options.isSimultor() )
        return Error();

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "calculateUniqueGOTs time");

    uint32_t pointerSize = config.layout.is64 ? 8 : 4;

    // DylibSegmentChunk's don't have a pointer to their cache dylib.  Make a map for them
    std::unordered_map<const InputFile*, CacheDylib*> fileToDylibMap;
    fileToDylibMap.reserve(this->cacheDylibs.size());
    for ( CacheDylib& dylib : this->cacheDylibs )
        fileToDylibMap[dylib.inputFile] = &dylib;

    for ( SubCache& subCache : this->subCaches ) {
        // Find the DATA_CONST/AUTH_CONST in each SubCache, if it has any
        Region* dataConstRegion = nullptr;
        Region* authConstRegion = nullptr;
        for ( Region& region : subCache.regions ) {
            if ( region.kind == Region::Kind::dataConst ) {
                dataConstRegion = &region;
                continue;
            }
            if ( region.kind == Region::Kind::authConst ) {
                authConstRegion = &region;
                continue;
            }
        }

        if ( (dataConstRegion == nullptr) && (authConstRegion == nullptr) )
            continue;

        for ( bool auth : { false, true } ) {
            if ( auth && (authConstRegion == nullptr) )
                continue;
            if ( !auth && (dataConstRegion == nullptr) )
                continue;

            Region& region = auth ? *authConstRegion : *dataConstRegion;
            std::string_view segmentName = auth ? "__AUTH_CONST" : "__DATA_CONST";
            std::string_view sectionName = auth ? "__auth_got" : "__got";
            CoalescedGOTSection& subCacheUniquedGOTs = auth ? subCache.uniquedGOTsOptimizer.authGOTs : subCache.uniquedGOTsOptimizer.regularGOTs;

            std::vector<DylibSectionCoalescer::OptimizedSection*> dylibOptimizedSections;
            dylibOptimizedSections.reserve(region.chunks.size());
            for ( const Chunk* chunk : region.chunks ) {
                const DylibSegmentChunk* segmentChunk = chunk->isDylibSegmentChunk();
                if ( !segmentChunk )
                    continue;

                if ( chunk->name() != segmentName )
                    continue;

                CacheDylib*                 dylib = fileToDylibMap.at(segmentChunk->inputFile);
                auto&                       dylibUniquedGOTs = auth ? dylib->optimizedSections.auth_gots : dylib->optimizedSections.gots;

                // Set the dylib GOTs to point to the subCache they'll be uniqued to
                dylibUniquedGOTs.subCacheSection = &subCacheUniquedGOTs;
                dylibOptimizedSections.push_back(&dylibUniquedGOTs);

                parseGOTs(dylib, segmentChunk, segmentName, sectionName, dylibUniquedGOTs);
            }

            if ( subCacheUniquedGOTs.gotTargetsToOffsets.empty() )
                continue;

            // Sort the coalesced GOTs based on the target install name.  We find GOTs in the order we parse
            // the fixups in the dylibs, but we want the final cache to keep all GOTs for the same target near
            // each other
            typedef CoalescedGOTSection::GOTKey Key;
            std::vector<Key> sortedKeys;
            sortedKeys.reserve(subCacheUniquedGOTs.gotTargetsToOffsets.size());
            for ( const auto& keyAndValue : subCacheUniquedGOTs.gotTargetsToOffsets )
                sortedKeys.push_back(keyAndValue.first);

            std::sort(sortedKeys.begin(), sortedKeys.end(),
                      [](const Key& a, const Key& b) {
                // Put libSystem first, then all the /usr/lib/system dylibs
                // That way any GOTs for re-exports from libsystem will be close to similar GOTs
                bool isLibsystemA = a.targetDylibName.find("libSystem.B.dylib") != std::string_view::npos;
                bool isLibsystemB = b.targetDylibName.find("libSystem.B.dylib") != std::string_view::npos;
                if ( isLibsystemA != isLibsystemB )
                    return isLibsystemA;

                bool isLibsystemReexportA = a.targetDylibName.find("/usr/lib/system") != std::string_view::npos;
                bool isLibsystemReexportB = b.targetDylibName.find("/usr/lib/system") != std::string_view::npos;
                if ( isLibsystemReexportA != isLibsystemReexportB )
                    return isLibsystemReexportA;

                if ( a.targetDylibName != b.targetDylibName )
                    return (a.targetDylibName < b.targetDylibName);

                // Install names are the same.  Sort by symbol name
                return a.targetSymbolName < b.targetSymbolName;
            });

            // Rewrite entries from their original offset to the new offset
            std::unordered_map<uint32_t, uint32_t> oldToNewOffsetMap;
            for ( uint32_t i = 0; i != sortedKeys.size(); ++i ) {
                const Key& key = sortedKeys[i];
                auto it = subCacheUniquedGOTs.gotTargetsToOffsets.find(key);
                assert(it != subCacheUniquedGOTs.gotTargetsToOffsets.end());

                uint32_t newCacheSectionOffset = i * pointerSize;

                // Record the offset mapping for updating the dylibs
                oldToNewOffsetMap[it->second] = newCacheSectionOffset;

                const bool log = false;
                if ( log ) {
                    printf("%s[%d]: %s\n", sectionName.data(), newCacheSectionOffset, key.targetSymbolName.data());
                }

                it->second = newCacheSectionOffset;
            }

            // Also rewrite entries in each dylib
            for ( DylibSectionCoalescer::OptimizedSection* dylibOptimizedSection : dylibOptimizedSections ) {
                for ( auto& keyAndCacheOffset : dylibOptimizedSection->offsetMap ) {
                    auto it = oldToNewOffsetMap.find(keyAndCacheOffset.second);
                    assert(it != oldToNewOffsetMap.end());
                    keyAndCacheOffset.second = it->second;
                }
            }

            // Add the new chunks to the subCache
            if ( auth ) {
                subCache.uniquedAuthGOTs                    = std::make_unique<UniquedGOTsChunk>();
                subCache.uniquedAuthGOTs->cacheVMSize       = CacheVMSize((uint64_t)subCacheUniquedGOTs.gotTargetsToOffsets.size() * pointerSize);
                subCache.uniquedAuthGOTs->subCacheFileSize  = CacheFileSize((uint64_t)subCacheUniquedGOTs.gotTargetsToOffsets.size() * pointerSize);

                region.chunks.push_back(subCache.uniquedAuthGOTs.get());

                // FIXME: Do we need this. No-one seems to read it from here, or could get it from the subCache instead
                subCache.uniquedGOTsOptimizer.authGOTsChunk = subCache.uniquedAuthGOTs.get();
                subCache.uniquedGOTsOptimizer.authGOTs.cacheChunk = subCache.uniquedGOTsOptimizer.authGOTsChunk;
            } else {
                subCache.uniquedGOTs                    = std::make_unique<UniquedGOTsChunk>();
                subCache.uniquedGOTs->cacheVMSize       = CacheVMSize((uint64_t)subCacheUniquedGOTs.gotTargetsToOffsets.size() * pointerSize);
                subCache.uniquedGOTs->subCacheFileSize  = CacheFileSize((uint64_t)subCacheUniquedGOTs.gotTargetsToOffsets.size() * pointerSize);

                region.chunks.push_back(subCache.uniquedGOTs.get());

                // FIXME: Do we need this. No-one seems to read it from here, or could get it from the subCache instead
                subCache.uniquedGOTsOptimizer.regularGOTsChunk = subCache.uniquedGOTs.get();
                subCache.uniquedGOTsOptimizer.regularGOTs.cacheChunk = subCache.uniquedGOTsOptimizer.regularGOTsChunk;
            }

            if ( this->config.log.printStats ) {
                uint64_t totalSourceGOTs = 0;
                for ( DylibSectionCoalescer::OptimizedSection* dylibOptimizedSection : dylibOptimizedSections ) {
                    totalSourceGOTs += dylibOptimizedSection->offsetMap.size();
                }
                const char* kind = auth ? "auth" : "regular";
                stats.add("  got uniquing: uniqued %lld %s GOTs to %lld GOTs\n",
                          totalSourceGOTs, kind, (uint64_t)subCacheUniquedGOTs.gotTargetsToOffsets.size());
            }
        }
    }

    return Error();
}

// Sort the segments in each subCache region.  The final subCache may have a single DATA region, but inside
// that we have __DATA and __DATA_DIRTY.  We want the __DATA_DIRTY in particular to be sorted and contiguous
void SharedCacheBuilder::sortSubCacheSegments()
{
    Timer::Scope timedScope(this->config, "sortSubCacheSegments time");

    auto textSortOrder = [](const Chunk* a, const Chunk* b) -> bool {
        // Sort the cache header before other TEXT atoms
        if ( a->sortOrder() != b->sortOrder() )
            return a->sortOrder() < b->sortOrder();

        // Note we are using a stable sort, so if the kind's aren't different, return false
        // and we'll keep Section's in the order they were added to the vector
        return false;
    };

    const auto& dirtyDataSegmentOrdering = options.dirtyDataSegmentOrdering;
    auto        dataSortOrder            = [dirtyDataSegmentOrdering](const Chunk* a, const Chunk* b) -> bool {
        // Sort DATA_DIRTY before DATA
        if ( a->sortOrder() != b->sortOrder() )
            return a->sortOrder() < b->sortOrder();

        const DylibSegmentChunk* segmentA = a->isDylibSegmentChunk();
        const DylibSegmentChunk* segmentB = b->isDylibSegmentChunk();

        if ( segmentA->kind == DylibSegmentChunk::Kind::dylibDataDirty ) {
            const auto& orderA = dirtyDataSegmentOrdering.find(segmentA->inputFile->path);
            const auto& orderB = dirtyDataSegmentOrdering.find(segmentB->inputFile->path);
            bool        foundA = (orderA != dirtyDataSegmentOrdering.end());
            bool        foundB = (orderB != dirtyDataSegmentOrdering.end());

            // Order all __DATA_DIRTY segments specified in the order file first, in the order specified in the file,
            // followed by any other __DATA_DIRTY segments in lexicographic order.
            if ( foundA && foundB )
                return orderA->second < orderB->second;
            else if ( foundA )
                return true;
            else if ( foundB )
                return false;
        }

        // Note we are using a stable sort, so if the kind's aren't different, return false
        // and we'll keep Section's in the order they were added to the vector
        return false;
    };

    auto linkeditSortOrder = [](const Chunk* a, const Chunk* b) -> bool {
        // Sort read-only segments before LINKEDIT
        if ( a->sortOrder() != b->sortOrder() )
            return a->sortOrder() < b->sortOrder();

        // Note we are using a stable sort, so if the kind's aren't different, return false
        // and we'll keep Section's in the order they were added to the vector
        return false;
    };

    // Only sort data/auth.  Everything else is already in order
    for ( SubCache& subCache : this->subCaches ) {
        for ( Region& region : subCache.regions ) {
            if ( region.kind == Region::Kind::text ) {
                std::stable_sort(region.chunks.begin(), region.chunks.end(), textSortOrder);
            }
            else if ( (region.kind == Region::Kind::data) || (region.kind == Region::Kind::auth) ) {
                std::stable_sort(region.chunks.begin(), region.chunks.end(), dataSortOrder);
            }
            else if ( region.kind == Region::Kind::linkedit ) {
                std::stable_sort(region.chunks.begin(), region.chunks.end(), linkeditSortOrder);
            }
        }
    }
}

void SharedCacheBuilder::calculateSlideInfoSize()
{
    Timer::Scope timedScope(this->config, "calculateSlideInfoSize time");

    auto calculateRegionSlideInfoSize = [](BuilderConfig& builderConfig,
                                           Region::Kind regionKind, const std::vector<Region>& regions,
                                           const std::unique_ptr<cache_builder::SlideInfoChunk>& slideInfo) {
        if ( !slideInfo )
            return;

        const Region* foundRegion = nullptr;
        for ( const Region& region : regions ) {
            if ( region.kind == regionKind ) {
                foundRegion = &region;
                break;
            }
        }

        assert(foundRegion != nullptr);

        CacheVMSize totalRegionVMSize(0ULL);
        for ( const Chunk* chunk : foundRegion->chunks ) {
            totalRegionVMSize = alignTo(totalRegionVMSize, chunk->alignment());
            totalRegionVMSize += chunk->cacheVMSize;
        }
        totalRegionVMSize = alignPage(totalRegionVMSize);

        // Slide info needs a certain number of bytes per page
        uint64_t slideInfoSize = 0;
        switch ( builderConfig.slideInfo.slideInfoFormat.value() ) {
            case cache_builder::SlideInfo::SlideInfoFormat::v1:
                slideInfoSize += sizeof(dyld_cache_slide_info);
                break;
            case cache_builder::SlideInfo::SlideInfoFormat::v2:
                slideInfoSize += sizeof(dyld_cache_slide_info2);
                break;
            case cache_builder::SlideInfo::SlideInfoFormat::v3:
                slideInfoSize += sizeof(dyld_cache_slide_info3);
                break;
        }
        slideInfoSize += (totalRegionVMSize.rawValue() / builderConfig.slideInfo.slideInfoPageSize) * builderConfig.slideInfo.slideInfoBytesPerDataPage;

        slideInfo->cacheVMSize      = CacheVMSize(slideInfoSize);
        slideInfo->subCacheFileSize = CacheFileSize(slideInfoSize);
    };

    for ( const SubCache& subCache : this->subCaches ) {
        calculateRegionSlideInfoSize(this->config, Region::Kind::data, subCache.regions, subCache.dataSlideInfo);
        calculateRegionSlideInfoSize(this->config, Region::Kind::dataConst, subCache.regions, subCache.dataConstSlideInfo);
        calculateRegionSlideInfoSize(this->config, Region::Kind::auth, subCache.regions, subCache.authSlideInfo);
        calculateRegionSlideInfoSize(this->config, Region::Kind::authConst, subCache.regions, subCache.authConstSlideInfo);
    }
}

void SharedCacheBuilder::calculateCodeSignatureSize()
{
    Timer::Scope timedScope(this->config, "calculateCodeSignatureSize time");

    for ( SubCache& subCache : this->subCaches ) {
        // Note we use file size, as regions such as the unmapped symbols have a file size but not a VM size
        CacheFileSize totalSize(0ULL);
        for ( const Region& region : subCache.regions ) {
            // Region's should start page aligned
            totalSize = alignPage(totalSize);
            for ( const Chunk* chunk : region.chunks ) {
                // Skip the code signature chunk we are computing
                if ( chunk == subCache.codeSignature.get() )
                    continue;
                totalSize = alignTo(totalSize, chunk->alignment());
                totalSize += chunk->subCacheFileSize;
            }
            totalSize = alignPage(totalSize);
        }

        subCache.setCodeSignatureSize(this->options, this->config, totalSize);
    }
}

void SharedCacheBuilder::printSubCaches() const
{
    const bool printSegments = false;

    if ( !this->config.log.printStats )
        return;

    for ( const SubCache& subCache : this->subCaches ) {
        this->config.log.log("SubCache[%d]\n", (uint32_t)(&subCache - this->subCaches.data()));
        for ( const Region& region : subCache.regions ) {
            const char* regionName = nullptr;
            switch ( region.kind ) {
                case Region::Kind::text:
                    regionName = "text";
                    break;
                case Region::Kind::data:
                    regionName = "data";
                    break;
                case Region::Kind::dataConst:
                    regionName = "dataConst";
                    break;
                case Region::Kind::auth:
                    regionName = "auth";
                    break;
                case Region::Kind::authConst:
                    regionName = "authConst";
                    break;
                case Region::Kind::linkedit:
                    regionName = "linkedit";
                    break;
                case Region::Kind::unmapped:
                    regionName = "unmapped";
                    break;
                case Region::Kind::dynamicConfig:
                    regionName = "dynamicConfig";
                    break;
                case Region::Kind::codeSignature:
                    regionName = "codeSignature";
                    break;
                case Region::Kind::numKinds:
                    assert(0);
            }

            this->config.log.log("  %s (%d chunks)\n", regionName, (uint32_t)region.chunks.size());

            if ( printSegments ) {
                for ( const Chunk* chunk : region.chunks ) {
                    std::string_view name = chunk->name();
                    this->config.log.log("    %s\n", name.data());
                }
            }
        }
    }
}

// This is the arm64 layout, where we start each of TEXT/DATA/LINKEDIT 32MB after the last region,
// so that different permissions are on their own 32MNB ranges.
Error SharedCacheBuilder::computeSubCacheContiguousVMLayout()
{
    // Add padding between each region, and set the Region VMAddr's

    // We may be building for universal, in which case we have both customer and development
    // main caches, and customer/development stubs.  Other sub-caches are shared though.
    // We need to walk the subcaches starting from the main caches, and make sure to never
    // cross the streams between customer/development
    SubCache* mainDevelopmentCache = nullptr;
    SubCache* mainCustomerCache = nullptr;
    SubCache* symbolsCache = nullptr;
    for ( SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainDevelopmentCache() ) {
            assert(mainDevelopmentCache == nullptr);
            mainDevelopmentCache = &subCache;
            continue;
        }
        if ( subCache.isMainCustomerCache() ) {
            assert(mainCustomerCache == nullptr);
            mainCustomerCache = &subCache;
            continue;
        }
        if ( subCache.isSymbolsCache() ) {
            assert(symbolsCache == nullptr);
            symbolsCache = &subCache;
            continue;
        }
    }

    // We must have a development cache.  Customer is optional
    assert(mainDevelopmentCache != nullptr);

    // First walk the development cache and lay out its dylibs
    {
        std::vector<SubCache*> devSubCaches;
        devSubCaches.push_back(mainDevelopmentCache);
        devSubCaches.insert(devSubCaches.end(), mainDevelopmentCache->subCaches.begin(),
                            mainDevelopmentCache->subCaches.end());

        // Add the symbols cache.  It's cache header needs to be correctly assigned an
        // address
        if ( symbolsCache != nullptr )
            devSubCaches.push_back(symbolsCache);

        CacheVMAddress vmAddress = this->config.layout.cacheBaseAddress;
        const Region* prevRegion = nullptr;
        for ( SubCache* subCache : devSubCaches ) {
            subCache->subCacheVMAddress = vmAddress;
            for ( Region& region : subCache->regions ) {
                // Skip Region's like the code signature which does not take up VM space
                if ( !region.needsSharedCacheReserveAddressSpace() )
                    continue;

                // Add padding before this region (normally) if we change permissions
                if ( (prevRegion != nullptr) && prevRegion->needsRegionPadding(region) )
                    vmAddress += this->config.layout.contiguous->regionPadding;

                region.subCacheVMAddress = vmAddress;
                vmAddress += region.subCacheVMSize;

                prevRegion = &region;
            }
        }

        // If we have a symbols file, then we don't want to take its VMSize in to account when
        // computing the max mapped size of the cache files
        if ( symbolsCache != nullptr )
            vmAddress = symbolsCache->subCacheVMAddress;
        this->totalVMSize = CacheVMSize((vmAddress - this->config.layout.cacheBaseAddress).rawValue());
    }

    // If we have a customer cache, then walk it, and set any subCaches we don't share with
    // the dev cache
    if ( mainCustomerCache != nullptr ) {
        std::vector<SubCache*> customerSubCaches;
        customerSubCaches.push_back(mainCustomerCache);
        customerSubCaches.insert(customerSubCaches.end(), mainCustomerCache->subCaches.begin(),
                                 mainCustomerCache->subCaches.end());

        // Add the symbols cache.  It's cache header needs to be correctly assigned an
        // address
        if ( symbolsCache != nullptr )
            customerSubCaches.push_back(symbolsCache);

        CacheVMAddress vmAddress = this->config.layout.cacheBaseAddress;
        const Region* prevRegion = nullptr;
        for ( SubCache* subCache : customerSubCaches ) {
            // The dev cache already visited sub caches.  We should only set addresses in
            // main/stubs here
            if ( subCache->isSubCache() || subCache->isSymbolsCache() ) {
                assert(subCache->subCacheVMAddress == vmAddress);
            } else {
                subCache->subCacheVMAddress = vmAddress;
            }
            for ( Region& region : subCache->regions ) {
                // Skip Region's like the code signature which does not take up VM space
                if ( !region.needsSharedCacheReserveAddressSpace() )
                    continue;

                // Add padding before this region (normally) if we change permissions
                if ( (prevRegion != nullptr) && prevRegion->needsRegionPadding(region) )
                    vmAddress += this->config.layout.contiguous->regionPadding;

                if ( subCache->isSubCache() || subCache->isSymbolsCache()  ) {
                    assert(region.subCacheVMAddress == vmAddress);
                } else {
                    region.subCacheVMAddress = vmAddress;
                }
                vmAddress += region.subCacheVMSize;

                prevRegion = &region;
            }
        }

        // If we have a symbols file, then we don't want to take its VMSize in to account when
        // computing the max mapped size of the cache files
        if ( symbolsCache != nullptr )
            vmAddress = symbolsCache->subCacheVMAddress;

        CacheVMSize totalCustomerCacheSize((vmAddress - this->config.layout.cacheBaseAddress).rawValue());
        assert(this->totalVMSize == totalCustomerCacheSize);
    }

    if ( this->totalVMSize > this->config.layout.cacheSize ) {
        return Error("Cache overflow (0x%llx > 0x%llx)",
                     this->totalVMSize.rawValue(),
                     this->config.layout.cacheSize.rawValue());
    }

    return Error();
}

// This is the x86_64 sim layout, where each of TEXT/DATA/LINKEDIT has its own fixed address
Error SharedCacheBuilder::computeSubCacheDiscontiguousSimVMLayout()
{
    // Add padding between each region, and set the Region VMAddr's
    CacheVMAddress maxVMAddress = this->config.layout.cacheBaseAddress;
    assert(this->subCaches.size() == 1);
    SubCache& subCache = this->subCaches.front();
    subCache.subCacheVMAddress = this->config.layout.cacheBaseAddress;

    bool seenText = false;
    bool seenData = false;
    bool seenLinkedit = false;
    bool seenDynamicConfig = false;
    CacheVMAddress lastDataEnd;
    CacheVMAddress linkEditEnd;
    for ( Region& region : subCache.regions ) {
        switch ( region.kind ) {
            case Region::Kind::text:
                assert(!seenText);
                seenText = true;
                region.subCacheVMAddress = this->config.layout.discontiguous->simTextBaseAddress;

                // Check for overflow
                if ( region.subCacheVMSize > this->config.layout.discontiguous->simTextSize ) {
                    return Error("Overflow in text (0x%llx > 0x%llx)",
                                 region.subCacheVMSize.rawValue(),
                                 this->config.layout.discontiguous->simTextSize.rawValue());
                }
                break;
            case Region::Kind::dataConst:
            case Region::Kind::data:
            case Region::Kind::auth:
            case Region::Kind::authConst:
                if ( seenData ) {
                    // This data follows from the previous one
                    region.subCacheVMAddress = lastDataEnd;
                } else {
                    seenData = true;
                    region.subCacheVMAddress = this->config.layout.discontiguous->simDataBaseAddress;
                }
                lastDataEnd = region.subCacheVMAddress + region.subCacheVMSize;
                break;
            case Region::Kind::linkedit:
                assert(!seenLinkedit);
                seenLinkedit = true;
                region.subCacheVMAddress = this->config.layout.discontiguous->simLinkeditBaseAddress;

                // Check for overflow
                if ( region.subCacheVMSize > this->config.layout.discontiguous->simLinkeditSize ) {
                    return Error("Overflow in linkedit (0x%llx > 0x%llx)",
                                 region.subCacheVMSize.rawValue(),
                                 this->config.layout.discontiguous->simLinkeditSize.rawValue());
                }
                linkEditEnd = region.subCacheVMAddress + region.subCacheVMSize;
                break;
            case Region::Kind::dynamicConfig:
                assert(!seenDynamicConfig);
                seenDynamicConfig = true;
                // Grab space right after the linkedit
                region.subCacheVMAddress = linkEditEnd;
                // Check for overflow
                if ( region.subCacheVMSize > this->config.layout.discontiguous->simLinkeditSize ) {
                    return Error("Overflow in dynamicConfig (0x%llx > 0x%llx)",
                                 region.subCacheVMSize.rawValue(),
                                 this->config.layout.discontiguous->simLinkeditSize.rawValue());
                }
                break;
            case Region::Kind::unmapped:
            case Region::Kind::codeSignature:
                break;
            case Region::Kind::numKinds:
                assert(0);
                break;
        }

        if ( seenData ) {
            // Check for overflow
            CacheVMSize dataSize(lastDataEnd.rawValue() - this->config.layout.discontiguous->simDataBaseAddress.rawValue());
            if ( dataSize > this->config.layout.discontiguous->simDataSize ) {
                return Error("Overflow in data (0x%llx > 0x%llx)",
                             dataSize.rawValue(),
                             this->config.layout.discontiguous->simDataSize.rawValue());
            }
        }

        if ( region.needsSharedCacheReserveAddressSpace() )
            maxVMAddress = region.subCacheVMAddress + region.subCacheVMSize;
    }

    this->totalVMSize = CacheVMSize((maxVMAddress - this->config.layout.cacheBaseAddress).rawValue());

    return Error();
}

// This is the x86_64 layout, where we start each of TEXT/DATA/LINKEDIT on their own 1GB boundaries
// This handles both large and regular layouts
Error SharedCacheBuilder::computeSubCacheDiscontiguousVMLayout()
{
    // Each region will start on 1GB boundaries to get optimal page-tables.  We require regions are always less than 1GB in size
    uint64_t regionAlignment = this->config.layout.discontiguous->regionAlignment.value();

    // Add padding between each region, and set the Region VMAddr's
    CacheVMAddress vmAddress         = this->config.layout.cacheBaseAddress;
    uint32_t       prevRegionMaxProt = 0;
    for ( SubCache& subCache : this->subCaches ) {

        // Align the start of every subCache to a 1GB boundary
        vmAddress = alignTo(vmAddress, regionAlignment);

        subCache.subCacheVMAddress = vmAddress;
        for ( Region& region : subCache.regions ) {
            // Skip Region's like the code signature which does not take up VM space
            if ( !region.needsSharedCacheReserveAddressSpace() )
                continue;

            // Align to the next 1GB boundary, but only if the permissions change.
            // We don't have enough VM space to pad between DATA and DATA_CONST
            uint32_t maxProt = region.maxProt();
            if ( (prevRegionMaxProt & VM_PROT_WRITE) != (maxProt & VM_PROT_WRITE) )
                vmAddress = alignTo(vmAddress, regionAlignment);

            region.subCacheVMAddress = vmAddress;
            vmAddress += region.subCacheVMSize;

            prevRegionMaxProt = maxProt;
        }

        // Add space for Rosetta
        if ( !subCache.isSymbolsCache() ) {
            const Region* lastReadWriteRegion = nullptr;
            const Region* lastReadOnlyRegion = nullptr;
            for ( Region& region : subCache.regions ) {
                switch ( region.kind ) {
                    case Region::Kind::text:
                    case Region::Kind::unmapped:
                    case Region::Kind::codeSignature:
                    case Region::Kind::numKinds:
                        break;
                    case Region::Kind::data:
                    case Region::Kind::dataConst:
                    case Region::Kind::auth:
                    case Region::Kind::authConst:
                        lastReadWriteRegion = &region;
                        break;
                    case Region::Kind::dynamicConfig:
                    case Region::Kind::linkedit:
                        lastReadOnlyRegion = &region;
                        break;
                }
            }

            // Rosetta RO
            {
                // Take 1GB + any remaining space from the end of LINKEDIT
                CacheVMAddress endOfLinkedit = lastReadOnlyRegion->subCacheVMAddress + lastReadOnlyRegion->subCacheVMSize;

                vmAddress += CacheVMSize(1ULL << 30);
                vmAddress = alignTo(vmAddress, regionAlignment);

                uint64_t rosettaSpace = (vmAddress - endOfLinkedit).rawValue();
                subCache.rosettaReadOnlyAddr = endOfLinkedit.rawValue();
                subCache.rosettaReadOnlySize = rosettaSpace;
            }

            // Rosetta RW
            {
                CacheVMAddress endOfData = lastReadWriteRegion->subCacheVMAddress + lastReadWriteRegion->subCacheVMSize;
                CacheVMAddress startOfNextRegion = alignTo(endOfData, regionAlignment);
                uint64_t remainingSpace = (startOfNextRegion - endOfData).rawValue();

                // There should be plenty of space up to half the region, so that we have enough slide
                remainingSpace = remainingSpace / 2;

                subCache.rosettaReadWriteAddr = endOfData.rawValue();
                subCache.rosettaReadWriteSize = remainingSpace;
            }
        }
    }

    this->totalVMSize = CacheVMSize((vmAddress - this->config.layout.cacheBaseAddress).rawValue());
    
    if ( this->totalVMSize > this->config.layout.cacheSize ) {
        return Error("Cache overflow (0x%llx > 0x%llx)",
                     this->totalVMSize.rawValue(),
                     this->config.layout.cacheSize.rawValue());
    }

    return Error();
}

// In file layout, we need each Region to start page-aligned.  Within a Region, we can pack pages
// to sub-page offsets
Error SharedCacheBuilder::computeSubCacheLayout()
{
    Timer::Scope timedScope(this->config, "computeSubCacheLayout time");

    // Layout the Section's inside each Region.  The cache adds zero fill, so we always use the VM size
    // for the size of each piece, even though we are computing file layout.
    for ( SubCache& subCache : this->subCaches ) {
        CacheFileOffset subCacheFileOffset(0ULL);
        for ( Region& region : subCache.regions ) {
            // Make sure every region starts on a page aligned address.  Then subsequent aligned Section's will work
            assert((subCacheFileOffset.rawValue() % this->config.layout.pageSize) == 0);
            region.subCacheFileOffset = subCacheFileOffset;

            // We don't use a type-safe wrapper here as we are mixing and matching VM and file layout and it gets messy
            uint64_t regionFileSize = 0;
            uint64_t regionVMSize   = 0;
            bool seenUnmappedRegion = false;
            bool seenZeroFillChunk = false;
            for ( Chunk* section : region.chunks ) {
                // Align the start of the section, if needed
                assert(section->alignment() != 0);
                regionFileSize = alignTo(regionFileSize, section->alignment());
                regionVMSize   = alignTo(regionVMSize, section->alignment());

                // Update the section to know where it'll be in the subCache
                section->subCacheFileOffset = region.subCacheFileOffset + CacheFileSize(regionFileSize);

                if ( region.needsSharedCacheReserveAddressSpace() ) {
                    // We can't have a region with VM space after one without
                    assert(!seenUnmappedRegion);

                    // We support zero-fill chunks, which really don't take up file space
                    // but only if they are at the end of their Region
                    if ( section->isZeroFill() ) {
                        if ( section->subCacheFileSize.rawValue() != 0 )
                            return Error("zerofill chunk (%s) should not have a file size", section->name());
                        if ( section->cacheVMSize.rawValue() == 0 )
                            return Error("zerofill chunk (%s) should have a VM size", section->name());

                        regionVMSize += section->cacheVMSize.rawValue();

                        seenZeroFillChunk = true;
                    } else {
                        // We can't have a chunk which needs file space after a zero-fill one
                        if ( seenZeroFillChunk )
                            return Error("regular chunk (%s) after zero-fill chunk", section->name());

                        // Note we use VMSize due to zero-fill
                        assert(section->subCacheFileSize.rawValue() <= section->cacheVMSize.rawValue());
                        regionFileSize += section->cacheVMSize.rawValue();
                        regionVMSize += section->cacheVMSize.rawValue();
                    }
                }
                else {
                    // The code signature doesn't get a mapping, so we have to use its file size instead
                    regionFileSize += section->subCacheFileSize.rawValue();
                    assert(section->cacheVMSize.rawValue() == 0);
                }
            }

            // Align the size of each region
            regionFileSize          = alignPage(regionFileSize);
            regionVMSize            = alignPage(regionVMSize);
            region.subCacheFileSize = CacheFileSize(regionFileSize);
            region.subCacheVMSize   = CacheVMSize(regionVMSize);
            subCacheFileOffset += region.subCacheFileSize;
        }
    }

    // VM layout is different depending on regular/large/split
    if ( config.layout.contiguous.has_value() ) {
        if ( Error error = computeSubCacheContiguousVMLayout(); error.hasError() )
            return error;
    } else {
        if ( this->options.isSimultor() ) {
            if ( Error error = computeSubCacheDiscontiguousSimVMLayout(); error.hasError() )
                return error;
        } else {
            if ( Error error = computeSubCacheDiscontiguousVMLayout(); error.hasError() )
                return error;
        }
    }

    // Update Section VMAddr's now that we know where all the Region's are in memory
    for ( SubCache& subCache : this->subCaches ) {
        for ( Region& region : subCache.regions ) {
            for ( Chunk* section : region.chunks ) {
                // Update the section to know where it'll be in the subCache
                if ( region.needsSharedCacheReserveAddressSpace() ) {
                    // FIXME: Use something type-safe.  Is a "fileOffset - fileOffset" a "fileSize" for example?
                    uint64_t offsetInRegion = section->subCacheFileOffset.rawValue() - region.subCacheFileOffset.rawValue();
                    section->cacheVMAddress = region.subCacheVMAddress + VMOffset(offsetInRegion);
                }
            }
        }
    }

    if ( this->totalVMSize > this->config.layout.cacheSize ) {
        return Error("Cache overflow (0x%llx > 0x%llx)",
                     this->totalVMSize.rawValue(),
                     this->config.layout.cacheSize.rawValue());
    }

    return Error();
}

Error SharedCacheBuilder::allocateSubCacheBuffers()
{
    const bool log = false;

    Timer::Scope timedScope(this->config, "allocateSubCacheBuffers time");

    for ( uint32_t subCacheIndex = 0; subCacheIndex != this->subCaches.size(); ++subCacheIndex ) {
        SubCache& subCache = this->subCaches[subCacheIndex];

        // The last region has the size we need to allocate
        const Region& lastRegion = subCache.regions.back();
        uint64_t      bufferSize = (lastRegion.subCacheFileOffset + lastRegion.subCacheFileSize).rawValue();

#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
        // The MRM builder has no file system, so use an in-memory buffer
        vm_address_t fullAllocatedBuffer;
        if ( kern_return_t kr = vm_allocate(mach_task_self(), &fullAllocatedBuffer, bufferSize, VM_FLAGS_ANYWHERE); kr != 0 ) {
            return Error("could not allocate buffer because: %d", kr);
        }

        uint8_t *buffer = (uint8_t*)fullAllocatedBuffer;
        subCache.buffer     = buffer;
        subCache.bufferSize = bufferSize;
#else
        char pathTemplate[] = "/tmp/temp.XXXXXX";
        int  fd             = mkstemp(pathTemplate);
        if ( fd == -1 ) {
            // Failed to create the file
            return Error("could not create shared cache file because: %s", strerror(errno));
        }

        // Resize the file
        if ( int result = ftruncate(fd, bufferSize); result == -1 ) {
            // Failed to resize to the space we need
            return Error("could not truncate shared cache file because: %s", strerror(errno));
        }

        void* buffer = mmap(nullptr, (vm_size_t)bufferSize, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fd, 0);
        if ( buffer == MAP_FAILED ) {
            // Failed to mmap the file
            return Error("could not mmap shared cache file because: %s", strerror(errno));
        }

        // TODO: It would be great to unlink the file, so that it won't be there on disk if the builder crashes
#if 0
        // Unlink the file.  This way, it'll be removed if we crash
        if ( int result = unlinkat(fd, pathTemplate, 0) ) {
            // Failed to unlink the file
            return Error("could not unlink shared cache file because: %s", strerror(errno));
        }

        // Close the file as we don't need it now that we have the mmapped buffer
        if ( int result = close(fd) ) {
            // Failed to close the file
            return Error("could not close shared cache file because: %s", strerror(errno));
        }
#endif

        subCache.buffer     = (uint8_t*)buffer;
        subCache.bufferSize = bufferSize;
        subCache.fd         = fd;
        subCache.tempPath   = pathTemplate;

#endif // SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS

        if ( log ) {
            this->config.log.log("SubCache[%d] allocated (%p..%p)\n",
                                 subCacheIndex,
                                 buffer, (uint8_t*)buffer + bufferSize);
        }

        for ( Region& region : subCache.regions ) {
            region.subCacheBuffer = (uint8_t*)subCache.buffer + region.subCacheFileOffset.rawValue();
            for ( Chunk* section : region.chunks ) {
                // Skip empty sections, eg, LINKEDIT.
                if ( section->subCacheFileSize == CacheFileSize(0ULL) )
                    continue;
                section->subCacheBuffer = (uint8_t*)subCache.buffer + section->subCacheFileOffset.rawValue();
                assert(section->subCacheBuffer >= subCache.buffer);
                assert((section->subCacheBuffer + section->cacheVMSize.rawValue()) <= (subCache.buffer + subCache.bufferSize));
            }
        }
    }

    // Cache dylibs now have a location in the buffer.  Set them
    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        assert(!cacheDylib.segments.empty());
        assert(cacheDylib.segments[0].kind == cache_builder::DylibSegmentChunk::Kind::dylibText);
        cacheDylib.cacheMF          = (MachOFile*)cacheDylib.segments[0].subCacheBuffer;
        cacheDylib.cacheLoadAddress = cacheDylib.segments[0].cacheVMAddress;
    }

    // Chunks now have a location, so setup ASLRTrackers on anything which needs them
    for ( SubCache& subCache : this->subCaches ) {
        for ( Region& region : subCache.regions ) {
            for ( Chunk* chunk : region.chunks ) {
                if ( SlidChunk* slidChunk = chunk->isSlidChunk() ) {
                    slidChunk->tracker.setDataRegion(chunk->subCacheBuffer, chunk->cacheVMSize.rawValue());
                }
            }
        }
    }

    // Add a watchpoint for anything we need to debug
#if DEBUG
    {
        CacheVMAddress vmAddrToWatch(0x00007FFB40FB4D58ULL);
        for ( const SubCache& subCache : this->subCaches ) {
            for ( const Region& region : subCache.regions ) {
                if ( !region.needsSharedCacheReserveAddressSpace() )
                    continue;
                for ( const Chunk* chunk : region.chunks ) {
                    if ( vmAddrToWatch < chunk->cacheVMAddress )
                        continue;
                    if ( vmAddrToWatch >= (chunk->cacheVMAddress + chunk->cacheVMSize) )
                        continue;
                    VMOffset offsetInChunk = vmAddrToWatch - chunk->cacheVMAddress;
                    uint8_t* addrToWatch = chunk->subCacheBuffer + offsetInChunk.rawValue();
                    printf("watchpoint set expression -w w -s 8 -- %p\n", addrToWatch);
                    printf("");
                }
            }
        }
    }
#endif

    return Error();
}

// We threw away the LINKEDIT segment and created LinkeditChunk's instead.  This pass works out
// how large the combined LINKEDIT is for each dylib, and sets up the dylib segment appropriately
void SharedCacheBuilder::setupDylibLinkedit()
{
    Timer::Scope timedScope(this->config, "setupDylibLinkedit time");

    // Find all the LINKEDIT
    std::unordered_map<const InputFile*, const Region*> linkeditRegionsOwner;
    auto& linkeditRegions = linkeditRegionsOwner;
    for ( const SubCache& subCache : this->subCaches ) {
        for ( const Region& region : subCache.regions ) {
            if ( region.kind != Region::Kind::linkedit )
                continue;

            // Found a linkedit region.  Now track it
            for ( const Chunk* chunk : region.chunks ) {
                if ( const LinkeditDataChunk* linkeditChunk = chunk->isLinkeditDataChunk() ) {
                    linkeditRegions[linkeditChunk->inputFile] = &region;
                }
            }
        }
    }

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        // Find the linkedit segment in the dylib and set its range to the linkedit Region
        for ( DylibSegmentChunk& segment : cacheDylib.segments ) {
            if ( segment.segmentName == "__LINKEDIT" ) {

                const Region* region = linkeditRegions.at(cacheDylib.inputFile);
                // The segment should be somewhere in the segment VM range.  Then we'll reset it
                // to the start of the range
                assert(segment.cacheVMAddress >= region->subCacheVMAddress);
                assert(segment.cacheVMAddress < (region->subCacheVMAddress + region->subCacheVMSize));
                segment.cacheVMAddress = region->subCacheVMAddress;

                // The segment should have a 0 vmSize, and we'll reset to the region VMSize
                assert(segment.cacheVMSize.rawValue() == 0);
                segment.cacheVMSize = region->subCacheVMSize;

                // The segment should be somewhere in the segment file range.  Then we'll reset it
                // to the start of the range
                assert(segment.subCacheFileOffset >= region->subCacheFileOffset);
                assert(segment.subCacheFileOffset < (region->subCacheFileOffset + region->subCacheFileSize));
                segment.subCacheFileOffset = region->subCacheFileOffset;

                assert(segment.subCacheFileSize.rawValue() == 0);
                segment.subCacheFileSize = region->subCacheFileSize;

            }
        }

        return Error();
    });

    assert(!err.hasError());
}

void SharedCacheBuilder::setupSplitSegAdjustors()
{
    Timer::Scope timedScope(this->config, "setupSplitSegAdjustors time");

    Error err = parallel::forEach(this->cacheDylibs, ^(size_t index, CacheDylib& cacheDylib) {
        std::vector<MovedSegment> movedSegments;
        movedSegments.reserve(cacheDylib.segments.size());
        for ( DylibSegmentChunk& segment : cacheDylib.segments ) {
            MovedSegment movedSegment;
            // Input dylib data
            movedSegment.inputVMAddress = segment.inputVMAddress;
            movedSegment.inputVMSize    = segment.inputVMSize;

            // Cache dylib data
            movedSegment.cacheLocation   = segment.subCacheBuffer;
            movedSegment.cacheVMAddress  = segment.cacheVMAddress;
            movedSegment.cacheVMSize     = segment.cacheVMSize;
            movedSegment.cacheFileOffset = segment.subCacheFileOffset;
            movedSegment.cacheFileSize   = segment.subCacheFileSize;
            movedSegment.aslrTracker     = &segment.tracker;

            movedSegments.push_back(std::move(movedSegment));
        }

        std::unordered_map<MovedLinkedit::Kind, MovedLinkedit> movedLinkeditChunks;
        movedLinkeditChunks.reserve(cacheDylib.linkeditChunks.size());
        for ( const LinkeditDataChunk& chunk : cacheDylib.linkeditChunks ) {
            MovedLinkedit movedLinkedit;
            switch ( chunk.kind ) {
                case Chunk::Kind::linkeditSymbolNList:
                    movedLinkedit.kind = MovedLinkedit::Kind::symbolNList;
                    break;
                case Chunk::Kind::linkeditSymbolStrings:
                    movedLinkedit.kind = MovedLinkedit::Kind::symbolStrings;
                    break;
                case Chunk::Kind::linkeditIndirectSymbols:
                    movedLinkedit.kind = MovedLinkedit::Kind::indirectSymbols;
                    break;
                case Chunk::Kind::linkeditFunctionStarts:
                    movedLinkedit.kind = MovedLinkedit::Kind::functionStarts;
                    break;
                case Chunk::Kind::linkeditDataInCode:
                    movedLinkedit.kind = MovedLinkedit::Kind::dataInCode;
                    break;
                case Chunk::Kind::linkeditExportTrie:
                    movedLinkedit.kind = MovedLinkedit::Kind::exportTrie;
                    break;
                default:
                    assert(0);
                    break;
            }

            movedLinkedit.dataOffset                = chunk.subCacheFileOffset;
            movedLinkedit.dataSize                  = chunk.subCacheFileSize;
            movedLinkedit.cacheLocation             = chunk.subCacheBuffer;
            movedLinkeditChunks[movedLinkedit.kind] = std::move(movedLinkedit);
        }

        // Add the optimized nlist/symbol strings from the subCache
        assert(!movedLinkeditChunks.count(MovedLinkedit::Kind::symbolNList));
        assert(!movedLinkeditChunks.count(MovedLinkedit::Kind::symbolStrings));

        {
            MovedLinkedit movedLinkedit;
            movedLinkedit.kind                      = MovedLinkedit::Kind::symbolNList;
            movedLinkedit.dataOffset                = cacheDylib.optimizedSymbols.subCacheFileOffset;
            movedLinkedit.dataSize                  = cacheDylib.optimizedSymbols.subCacheFileSize;
            movedLinkedit.cacheLocation             = cacheDylib.optimizedSymbols.subCacheBuffer;
            movedLinkeditChunks[movedLinkedit.kind] = std::move(movedLinkedit);
        }

        {
            MovedLinkedit movedLinkedit;
            movedLinkedit.kind                      = MovedLinkedit::Kind::symbolStrings;
            movedLinkedit.dataOffset                = cacheDylib.subCacheSymbolStrings->subCacheFileOffset;
            movedLinkedit.dataSize                  = cacheDylib.subCacheSymbolStrings->subCacheFileSize;
            movedLinkedit.cacheLocation             = cacheDylib.subCacheSymbolStrings->subCacheBuffer;
            movedLinkeditChunks[movedLinkedit.kind] = std::move(movedLinkedit);
        }

        NListInfo nlistInfo;
        nlistInfo.globalsStartIndex = cacheDylib.optimizedSymbols.globalsStartIndex;
        nlistInfo.globalsCount      = cacheDylib.optimizedSymbols.globalsCount;
        nlistInfo.localsStartIndex  = cacheDylib.optimizedSymbols.localsStartIndex;
        nlistInfo.localsCount       = cacheDylib.optimizedSymbols.localsCount;
        nlistInfo.undefsStartIndex  = cacheDylib.optimizedSymbols.undefsStartIndex;
        nlistInfo.undefsCount       = cacheDylib.optimizedSymbols.undefsCount;

        cacheDylib.adjustor = std::make_unique<DylibSegmentsAdjustor>(std::move(movedSegments), std::move(movedLinkeditChunks), nlistInfo);

        return Error();
    });

    assert(!err.hasError());
}

void SharedCacheBuilder::adjustObjCClasses()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "adjustObjCClasses time");

    // Classes were stored as input dylib VMAddr's.  Convert to cache dylib VMAddr's
    for ( auto& nameAndClassInfo : this->objcClassOptimizer.classes ) {
        CacheDylib* cacheDylib = this->objcOptimizer.objcDylibs[nameAndClassInfo.second.second];

        InputDylibVMAddress inputVMAddr(nameAndClassInfo.second.first);
        CacheVMAddress      cacheVMAddr = cacheDylib->adjustor->adjustVMAddr(inputVMAddr);

        nameAndClassInfo.second.first = cacheVMAddr.rawValue();
    }
}

void SharedCacheBuilder::adjustObjCProtocols()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "adjustObjCProtocols time");

    // Protocols were stored as input dylib VMAddr's.  Convert to cache dylib VMAddr's
    for ( auto& nameAndProtocolInfo : this->objcProtocolOptimizer.protocols ) {
        CacheDylib* cacheDylib = this->objcOptimizer.objcDylibs[nameAndProtocolInfo.second.second];

        InputDylibVMAddress inputVMAddr(nameAndProtocolInfo.second.first);
        CacheVMAddress      cacheVMAddr = cacheDylib->adjustor->adjustVMAddr(inputVMAddr);

        nameAndProtocolInfo.second.first = cacheVMAddr.rawValue();
    }
}

Error SharedCacheBuilder::emitPatchTable()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "emitPatchTable time");
    
    // Skip this optimization on simulator until we've qualified it there
    __block PatchTableBuilder::PatchableClassesSet      patchableObjCClasses;
    __block PatchTableBuilder::PatchableSingletonsSet   patchableCFObj2;
    if ( !this->options.isSimultor() ) {
        for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
            __block objc_visitor::Visitor objcVisitor = makeInputDylibObjCVisitor(cacheDylib);
            objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
                InputDylibVMAddress inputVMAddr(objcClass.getVMAddress().rawValue());
                patchableObjCClasses.insert(cacheDylib.adjustor->adjustVMAddr(inputVMAddr));
            });

            // Note we have a diagnostic object here, but we don't care if it fails.  Then we'll
            // just skip singleton patching on this dylib
            Diagnostics diag;
            cacheDylib.cacheMF->forEachSingletonPatch(diag, ^(MachOFile::SingletonPatchKind kind, uint64_t runtimeOffset) {
                patchableCFObj2.insert(cacheDylib.cacheLoadAddress + VMOffset(runtimeOffset));
            });
        }
    }
    
    PatchTableBuilder builder;
    Error err = builder.build(this->cacheDylibs, this->patchTableOptimizer.patchInfos,
                              patchableObjCClasses,
                              patchableCFObj2,
                              this->config.layout.cacheBaseAddress);
    if ( err.hasError() )
        return err;
    
    auto* patchTableChunk = this->patchTableOptimizer.patchTableChunk;
    uint8_t* buffer = patchTableChunk->subCacheBuffer;
    uint64_t bufferSize = patchTableChunk->subCacheFileSize.rawValue();
    uint64_t patchInfoAddr = patchTableChunk->cacheVMAddress.rawValue();
    if ( Error error = builder.write(buffer, bufferSize, patchInfoAddr); error.hasError() )
        return error;
    
    // We don't need the patchInfos, so clear it to save memory
    this->patchTableOptimizer.patchInfos.clear();

    if ( this->config.log.printStats ) {
        uint64_t patchInfoSize = builder.getPatchTableSize();
        stats.add("  patch table: used %lld out of %lld bytes of buffer\n", patchInfoSize, bufferSize);
    }
    
    return Error();
}

// dyld4 needs a fake "main.exe" to set up the state.
// On macOS this *has* to come from an actual executable, as choosing a zippered
// dylib may incorrectly lead to setting up the ProcessConfig as iOSMac.
// Simulators don't have executables yet so choose a dylib there
static const MachOFile* getFakeMainExecutable(const BuilderOptions& options,
                                              std::span<CacheDylib> cacheDylibs,
                                              std::span<InputFile*> executableFiles)
{
    if ( options.isSimultor() ) {
        std::string_view installName = "/usr/lib/libSystem.B.dylib";
        for ( const CacheDylib& cacheDylib : cacheDylibs ) {
            if ( cacheDylib.installName == installName ) {
                assert(cacheDylib.cacheMF != nullptr);
                return cacheDylib.cacheMF;
            }
        }
    } else {
        const char* binPath = "/usr/bin/";
        if ( options.platform == dyld3::Platform::driverKit )
            binPath = "/System/Library/DriverExtensions/";
        for ( const InputFile* exeFile : executableFiles ) {
            if ( startsWith(exeFile->path, binPath) )
                return exeFile->mf;
        }
    }
    return nullptr;
}

struct LayoutBuilder
{
    LayoutBuilder(std::span<CacheDylib> cacheDylibs, std::span<InputFile*> executableFiles);
    ~LayoutBuilder()                    = default;
    LayoutBuilder(const LayoutBuilder&) = delete;
    LayoutBuilder(LayoutBuilder&&)      = delete;
    LayoutBuilder& operator=(const LayoutBuilder&) = delete;
    LayoutBuilder& operator=(LayoutBuilder&&) = delete;

    const mach_o::Layout& getCacheDylibLayout(uint32_t index) const;
    const mach_o::Layout& getExecutableLayout(uint32_t index) const;

private:
    std::vector<std::vector<mach_o::SegmentLayout>> dylibSegmentLayout;
    std::vector<mach_o::LinkeditLayout>             dylibLinkeditLayout;
    std::vector<mach_o::Layout>                     dylibLayouts;
    std::vector<std::vector<mach_o::SegmentLayout>> executableSegmentLayout;
    std::vector<mach_o::LinkeditLayout>             executableLinkeditLayout;
    std::vector<mach_o::Layout>                     executableLayouts;
};

LayoutBuilder::LayoutBuilder(std::span<CacheDylib> cacheDylibs, std::span<InputFile*> executableFiles)
{
    if ( !cacheDylibs.empty() ) {
        // Get the segment layout
        this->dylibSegmentLayout.reserve(cacheDylibs.size());
        for ( const CacheDylib& cacheDylib : cacheDylibs ) {
            __block std::vector<mach_o::SegmentLayout> segments;
            segments.reserve(cacheDylib.segments.size());
            for ( const DylibSegmentChunk& dylibSegment : cacheDylib.segments ) {
                mach_o::SegmentLayout segment;
                segment.vmAddr      = dylibSegment.cacheVMAddress.rawValue();
                segment.vmSize      = dylibSegment.cacheVMSize.rawValue();
                segment.fileOffset  = dylibSegment.subCacheFileOffset.rawValue();
                segment.fileSize    = dylibSegment.subCacheFileSize.rawValue();
                segment.buffer      = dylibSegment.subCacheBuffer;

                segment.kind        = mach_o::SegmentLayout::Kind::unknown;
                if ( dylibSegment.segmentName == "__TEXT" ) {
                    segment.kind    = mach_o::SegmentLayout::Kind::text;
                } else if ( dylibSegment.segmentName == "__LINKEDIT" ) {
                    segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
                }

                segments.push_back(segment);
            }

            // The cache segments don't have the permissions.  Get that from the load commands
            cacheDylib.cacheMF->forEachSegment(^(const MachOFile::SegmentInfo& info, bool& stop) {
                segments[info.segIndex].protections = info.protections;
            });
            this->dylibSegmentLayout.push_back(std::move(segments));
        }

        // Get the linkedit layout
        this->dylibLinkeditLayout.reserve(cacheDylibs.size());
        for ( const CacheDylib& cacheDylib : cacheDylibs ) {
            mach_o::LinkeditLayout linkeditLayout;
            for ( const auto& kindAndLinkdit : cacheDylib.adjustor->movedLinkedit ) {
                switch ( kindAndLinkdit.first ) {
                    case MovedLinkedit::Kind::symbolNList:
                    case MovedLinkedit::Kind::symbolStrings:
                    case MovedLinkedit::Kind::indirectSymbols:
                        // We probably don't need these in the Loader, as the export trie should
                        // have everything we need.  Skip for now
                        break;
                    case MovedLinkedit::Kind::functionStarts:
                    case MovedLinkedit::Kind::dataInCode:
                        // We don't need these in the Loader's.  Skip it
                        break;
                    case MovedLinkedit::Kind::exportTrie:
                        linkeditLayout.exportsTrie.buffer      = kindAndLinkdit.second.cacheLocation;
                        linkeditLayout.exportsTrie.bufferSize  = (uint32_t)kindAndLinkdit.second.dataSize.rawValue();
                        linkeditLayout.exportsTrie.entryCount  = 0; // Not needed here
                        linkeditLayout.exportsTrie.hasLinkedit = true;
                        break;
                    case MovedLinkedit::Kind::numKinds:
                        // This should never happen
                        assert(false);
                        break;
                }
            }
            this->dylibLinkeditLayout.push_back(std::move(linkeditLayout));
        }

        // Get the rest of the layout
        this->dylibLayouts.reserve(cacheDylibs.size());
        for ( uint32_t dylibIndex = 0; dylibIndex != cacheDylibs.size(); ++dylibIndex ) {
            const CacheDylib&                   cacheDylib = cacheDylibs[dylibIndex];
            std::vector<mach_o::SegmentLayout>& segments   = dylibSegmentLayout[dylibIndex];

            mach_o::Layout layout(cacheDylib.cacheMF, segments, dylibLinkeditLayout[dylibIndex]);
            this->dylibLayouts.push_back(layout);
        }
    }

    if ( !executableFiles.empty() ) {
        // Get the segment layout
        this->executableSegmentLayout.reserve(executableFiles.size());
        for ( const InputFile* executableFile : executableFiles ) {
            __block std::vector<mach_o::SegmentLayout> segments;
            executableFile->mf->forEachSegment(^(const MachOFile::SegmentInfo& info, bool& stop) {
                // Note file layout here, not VM layout
                uint8_t*              segmentBuffer = (uint8_t*)executableFile->mf + info.fileOffset;
                mach_o::SegmentLayout segment;
                segment.vmAddr      = info.vmAddr;
                segment.vmSize      = info.vmSize;
                segment.fileOffset  = info.fileOffset;
                segment.fileSize    = info.fileSize;
                segment.buffer      = segmentBuffer;
                segment.protections = info.protections;

                segment.kind        = mach_o::SegmentLayout::Kind::unknown;
                if ( !strcmp(info.segName, "__TEXT") ) {
                    segment.kind    = mach_o::SegmentLayout::Kind::text;
                } else if ( !strcmp(info.segName, "__LINKEDIT") ) {
                    segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
                }

                segments.push_back(segment);
            });
            this->executableSegmentLayout.push_back(std::move(segments));
        }

        // Get the linkedit layout
        this->executableLinkeditLayout.reserve(executableFiles.size());
        for ( const InputFile* executableFile : executableFiles ) {
            __block mach_o::LinkeditLayout linkeditLayout;

            Diagnostics diag;
            executableFile->mf->withFileLayout(diag, ^(const mach_o::Layout &layout) {
                linkeditLayout = layout.linkedit;
            });
            diag.assertNoError();

            this->executableLinkeditLayout.push_back(std::move(linkeditLayout));
        }

        // Get the rest of the layout
        this->executableLayouts.reserve(executableFiles.size());
        for ( uint32_t executableIndex = 0; executableIndex != executableFiles.size(); ++executableIndex ) {
            const InputFile*                          executableFile = executableFiles[executableIndex];
            std::vector<mach_o::SegmentLayout>&       segments       = executableSegmentLayout[executableIndex];

            mach_o::Layout layout(executableFile->mf, segments, executableLinkeditLayout[executableIndex]);
            this->executableLayouts.push_back(layout);
        }
    }
}

const mach_o::Layout& LayoutBuilder::getCacheDylibLayout(uint32_t index) const
{
    return this->dylibLayouts[index];
}

const mach_o::Layout& LayoutBuilder::getExecutableLayout(uint32_t index) const
{
    return this->executableLayouts[index];
}

static Error buildDylibJITLoaders(const BuilderOptions&                 builderOptions,
                                  const dyld3::closure::FileSystem&     fileSystem,
                                  dyld4::RuntimeState&                  state,
                                  std::span<CacheDylib>                 cacheDylibs,
                                  std::span<cache_builder::FileAlias>   aliases,
                                  std::vector<JustInTimeLoader*>&       jitLoaders)
{
    __block std::unordered_map<std::string_view, JustInTimeLoader*> loadersMap;
    __block std::unordered_map<std::string_view, uint32_t> loadersIndexMap;

    // make one pass to build the map so we can detect unzippered twins
    for ( const CacheDylib& cacheDylib : cacheDylibs )
        loadersIndexMap[cacheDylib.installName] = cacheDylib.cacheIndex;

    LayoutBuilder layoutBuilder(cacheDylibs, {});

    for ( uint32_t dylibIndex = 0; dylibIndex != cacheDylibs.size(); ++dylibIndex ) {
        const CacheDylib&     cacheDylib = cacheDylibs[dylibIndex];
        const mach_o::Layout& layout     = layoutBuilder.getCacheDylibLayout(dylibIndex);

        //printf("mh=%p, %s\n", mh, installName);
        bool     catalystTwin = false;
        uint32_t macTwinIndex = 0;
        if ( startsWith(cacheDylib.installName, "/System/iOSSupport/") ) {
            auto it = loadersIndexMap.find(cacheDylib.installName.substr(18));
            if ( it != loadersIndexMap.end() ) {
                catalystTwin = true;
                macTwinIndex = it->second;
            }
        }
        // inode and mtime are only valid if dylibs will remain on disk, ie, the simulator cache builder case
        bool              fileIDValid = !builderOptions.dylibsRemovedFromDisk;
        dyld4::FileID     fileID(cacheDylib.inputFile->inode, 0, cacheDylib.inputFile->mtime, fileIDValid);
        JustInTimeLoader* jitLoader        = JustInTimeLoader::makeJustInTimeLoaderDyldCache(state, cacheDylib.cacheMF, cacheDylib.installName.data(), cacheDylib.cacheIndex, fileID, catalystTwin, macTwinIndex, &layout);
        loadersMap[cacheDylib.installName] = jitLoader;
        jitLoaders.push_back(jitLoader);
    }
    for ( const cache_builder::FileAlias& alias : aliases ) {
        JustInTimeLoader* a = loadersMap[alias.aliasPath];
        JustInTimeLoader* r = loadersMap[alias.realPath];
        if ( a != nullptr )
            loadersMap[alias.realPath] = a;
        else if ( r != nullptr ) {
            loadersMap[alias.aliasPath] = r;
        }
    }

    Loader::LoadOptions::Finder loaderFinder = ^(Diagnostics& loadDiag, dyld3::Platform, const char* loadPath, const dyld4::Loader::LoadOptions& options) {
        auto pos = loadersMap.find(loadPath);
        if ( pos != loadersMap.end() ) {
            return (const Loader*)pos->second;
        }

        // Handle symlinks containing relative paths.  Unfortunately the only way to do this right now is with the fake file system
        char buffer[PATH_MAX];
        if ( fileSystem.getRealPath(loadPath, buffer) ) {
            pos = loadersMap.find(buffer);
            if ( pos != loadersMap.end() ) {
                return (const Loader*)pos->second;
            }
        }

        if ( !options.canBeMissing )
            loadDiag.error("dependent dylib '%s' not found", loadPath);
        return (const Loader*)nullptr;
    };

    Loader::LoadOptions options;
    options.staticLinkage = true;
    options.launching     = true;
    options.canBeDylib    = true;
    options.finder        = loaderFinder;
    for ( const Loader* ldr : state.loaded ) {
        Diagnostics loadDiag;
        ((Loader*)ldr)->loadDependents(loadDiag, state, options);
        if ( loadDiag.hasError() ) {
            return Error("%s, loading dependents of %s", loadDiag.errorMessageCStr(), ldr->path());
        }
    }

    return Error();
}

// Returns true if the cache should be considered like a development one for building loaders
// Currently all caches are "development", as we don't know if we'll boot a universal cache as
// customer or development, so have to build for the lowest common denominator
static bool isDevelopmentSharedCache(const BuilderOptions& options)
{
    // This is pointless, but just in case we ever added a customer kind again, lets use
    // switch coverage
    switch ( options.kind ) {
        case cache_builder::CacheKind::development:
        case cache_builder::CacheKind::universal:
            return true;
    }
}

Error SharedCacheBuilder::emitCacheDylibsPrebuiltLoaders()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "emitCacheDylibsPrebuiltLoaders time");

    const MachOFile* mainExecutable = getFakeMainExecutable(this->options, this->cacheDylibs,
                                                            this->exeInputFiles);
    if ( !mainExecutable )
        return Error("Could not find a main executable for building cache loaders");

    const LayoutBuilder  layoutBuilder(cacheDylibs, { });
    EphemeralAllocator   processConfigAlloc;
    __block dyld4::Vector<ProcessConfig::DyldCache::CacheDylib> processConfigDylibs(processConfigAlloc);

    for ( uint32_t dylibIndex = 0; dylibIndex != this->cacheDylibs.size(); ++dylibIndex ) {
        const CacheDylib&     cacheDylib = this->cacheDylibs[dylibIndex];
        const mach_o::Layout& layout     = layoutBuilder.getCacheDylibLayout(dylibIndex);

        uint64_t inode = 0;
        uint64_t mtime = 0;
        if ( !this->options.dylibsRemovedFromDisk ) {
            inode = cacheDylib.inputFile->inode;
            mtime = cacheDylib.inputFile->mtime;
        }

        ProcessConfig::DyldCache::CacheDylib dylib;
        dylib.mf     = cacheDylib.cacheMF;
        dylib.inode  = inode;
        dylib.mTime  = mtime;
        dylib.layout = &layout;
        processConfigDylibs.push_back(dylib);
    }

    // build PrebuiltLoaderSet of all dylibs in cache
    KernelArgs         kernArgs(mainExecutable, { "test.exe" }, {}, {});
    SyscallDelegate    osDelegate;
    EphemeralAllocator alloc;
    ProcessConfig      processConfig(&kernArgs, osDelegate, alloc);
    RuntimeState       state(processConfig, alloc);

    // FIXME: This is terrible and needs to be a real reset method
    processConfig.dyldCache.cacheBuilderDylibs = &processConfigDylibs;
    processConfig.dyldCache.dylibsExpectedOnDisk = !this->options.dylibsRemovedFromDisk;
    processConfig.dyldCache.development = isDevelopmentSharedCache(this->options);
    processConfig.dyldCache.patchTable = PatchTable(this->patchTableOptimizer.patchTableChunk->subCacheBuffer,
                                                    this->patchTableOptimizer.patchTableChunk->cacheVMAddress.rawValue());

    // build JITLoaders for all dylibs in cache
    std::vector<JustInTimeLoader*> jitLoaders;
    Error error = buildDylibJITLoaders(this->options, this->fileSystem, state,
                                       this->cacheDylibs, this->inputAliases, jitLoaders);
    if ( error.hasError() )
        return error;

    // now make a PrebuiltLoaderSet from all the JustInTimeLoaders for all the dylibs in the shared cache
    STACK_ALLOC_ARRAY(const Loader*, allDylibs, state.loaded.size());
    for ( const Loader* ldr : state.loaded )
        allDylibs.push_back(ldr);
    Diagnostics diag;
    auto*    cachedDylibsLoaderSet = dyld4::PrebuiltLoaderSet::makeDyldCachePrebuiltLoaders(diag, state, allDylibs);
    if ( diag.hasError() )
        return Error("Could not build dylib loaders because: %s", diag.errorMessageCStr());
    uint64_t prebuiltLoaderSetSize = cachedDylibsLoaderSet->size();

    const PrebuiltLoaderChunk* loaderChunk = this->prebuiltLoaderBuilder.cacheDylibsLoaderChunk;

    // check for fit
    uint64_t bufferSize = loaderChunk->subCacheFileSize.rawValue();

    if ( this->config.log.printStats ) {
        stats.add("  dyld4 dylib Loader's : used %lld out of %lld bytes of buffer\n", prebuiltLoaderSetSize, bufferSize);
    }

    if ( prebuiltLoaderSetSize > bufferSize ) {
        return Error("cache buffer too small to hold dylibs PrebuiltLoaderSet (prebuiltLoaderSet size=%lluKB, buffer size=%lldMB)",
                                           prebuiltLoaderSetSize / 1024, bufferSize / 1024 / 1024);
    }

    // copy the PrebuiltLoaderSet for dylibs into the cache
    ::memcpy(loaderChunk->subCacheBuffer, cachedDylibsLoaderSet, prebuiltLoaderSetSize);
    cachedDylibsLoaderSet->deallocate();

    this->prebuiltLoaderBuilder.cachedDylibsLoaderSet = (const dyld4::PrebuiltLoaderSet*)loaderChunk->subCacheBuffer;

    return Error();
}

// Finds the protocol class in libobjc, or returns an error if its not found.
// If found, sets the VMAddr and (if needed) PMD outputs.
static Error findProtocolClass(const BuilderConfig& config,
                               const std::vector<CacheDylib*>& objcDylibs,
                               VMAddress& protocolClassVMAddr, MachOFile::PointerMetaData& protocolClassPMD)
{
    for ( CacheDylib* cacheDylib : objcDylibs ) {
        if ( cacheDylib->installName == "/usr/lib/libobjc.A.dylib" ) {
            __block InputDylibVMAddress inputOptPtrsVMAddress;
            __block uint64_t            sectionSize = 0;
            __block bool                found       = false;
            cacheDylib->inputMF->forEachSection(^(const dyld3::MachOAnalyzer::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
                if ( (strncmp(sectInfo.segInfo.segName, "__DATA", 6) != 0) && (strncmp(sectInfo.segInfo.segName, "__AUTH", 6) != 0) )
                    return;
                if ( strcmp(sectInfo.sectName, "__objc_opt_ptrs") != 0 )
                    return;

                inputOptPtrsVMAddress = InputDylibVMAddress(sectInfo.sectAddr);
                sectionSize           = sectInfo.sectSize;

                found = true;
                stop  = true;
            });

            if ( !found ) {
                return Error("libobjc's pointer list section missing (metadata not optimized)");
            }

            // Note the section looks like this.  We don't really need a struct for now as its so simple:
            // List of offsets in libobjc that the shared cache optimization needs to use.
            // template <typename T>
            // struct objc_opt_pointerlist_tt {
            //     T protocolClass;
            // };
            // typedef struct objc_opt_pointerlist_tt<uintptr_t> objc_opt_pointerlist_t;
            if ( sectionSize < cacheDylib->inputMF->pointerSize() ) {
                return Error("libobjc's pointer list section is too small (metadata not optimized)");
            }

            CacheVMAddress cacheOptPtrsVMAddr = cacheDylib->adjustor->adjustVMAddr(inputOptPtrsVMAddress);

            objc_visitor::Visitor objcVisitor = cacheDylib->makeCacheObjCVisitor(config, nullptr, nullptr);

            metadata_visitor::ResolvedValue protocolClassValue = objcVisitor.getValueFor(VMAddress(cacheOptPtrsVMAddr.rawValue()));
            protocolClassVMAddr                           = objcVisitor.resolveRebase(protocolClassValue).vmAddress();

            if ( config.layout.hasAuthRegion ) {
                // The protocol fixup isn't a chained fixup as its in a cache dylib.  Instead its the caches
                // own format
                uint16_t    authDiversity  = 0;
                bool        authIsAddr     = false;
                uint8_t     authKey        = 0;
                bool isAuth = Fixup::Cache64::hasAuthData(protocolClassValue.value(), authDiversity, authIsAddr, authKey);
                if ( !isAuth )
                    return Error("libobjc's protocol wasn't authenticated");

                protocolClassPMD.diversity          = authDiversity;
                protocolClassPMD.high8              = 0;
                protocolClassPMD.authenticated      = 1;
                protocolClassPMD.key                = authKey;
                protocolClassPMD.usesAddrDiversity  = authIsAddr;
            }
            return Error();
        }
    }

    return Error("Could not find libobjc");
}

Error SharedCacheBuilder::emitExecutablePrebuiltLoaders()
{
    if ( this->exeInputFiles.empty() )
        return Error();

    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "emitExecutablePrebuiltLoaders time");

    const bool log         = false;

    auto* cachedDylibsLoaderSet = this->prebuiltLoaderBuilder.cachedDylibsLoaderSet;
    assert(cachedDylibsLoaderSet != nullptr);

    // We need to find the Protocol class from libojc
    VMOffset objcProtocolClassCacheOffset;
    if ( !this->objcOptimizer.objcDylibs.empty() ) {
        VMAddress                  protocolClassVMAddr;
        MachOFile::PointerMetaData protocolClassPMD;
        Error error = findProtocolClass(this->config, this->objcOptimizer.objcDylibs, protocolClassVMAddr, protocolClassPMD);
        if ( error.hasError() )
            return error;

        VMAddress cacheBaseAddress(this->config.layout.cacheBaseAddress.rawValue());
        objcProtocolClassCacheOffset = protocolClassVMAddr - cacheBaseAddress;
    }

    const LayoutBuilder  layoutBuilder(cacheDylibs, this->exeInputFiles);
    const LayoutBuilder* layoutBuilderPtr = &layoutBuilder;
    EphemeralAllocator   processConfigAlloc;
    dyld4::Vector<ProcessConfig::DyldCache::CacheDylib> processConfigDylibsOwner(processConfigAlloc);
    auto& processConfigDylibs = processConfigDylibsOwner;

    for ( uint32_t dylibIndex = 0; dylibIndex != this->cacheDylibs.size(); ++dylibIndex ) {
        const CacheDylib&     cacheDylib = this->cacheDylibs[dylibIndex];
        const mach_o::Layout& layout     = layoutBuilder.getCacheDylibLayout(dylibIndex);

        uint64_t inode = 0;
        uint64_t mtime = 0;
        if ( !this->options.dylibsRemovedFromDisk ) {
            inode = cacheDylib.inputFile->inode;
            mtime = cacheDylib.inputFile->mtime;
        }

        ProcessConfig::DyldCache::CacheDylib dylib;
        dylib.mf     = cacheDylib.cacheMF;
        dylib.inode  = inode;
        dylib.mTime  = mtime;
        dylib.layout = &layout;
        processConfigDylibs.push_back(dylib);
    }

    // Add on-disk dylibs which might be linked by apps we are building executable closures for
    SyscallDelegate::PathToMapping otherMappingOwner;
    auto& otherMapping = otherMappingOwner;
    for ( const InputFile* inputFile : this->nonCacheDylibInputFiles ) {
        if ( log ) {
            fprintf(stderr, "more other: %s\n", inputFile->path.c_str());
        }

        // Assume last segment file size is the overall file size
        __block uint64_t fileSize = 0;
        inputFile->mf->forEachSegment(^(const MachOFile::SegmentInfo& info, bool& stop) {
            fileSize = std::max(fileSize, info.fileOffset + info.fileSize);
        });
        otherMapping[inputFile->path] = { inputFile->mf, fileSize };
    }

    std::vector<const dyld4::PrebuiltLoaderSet*> executableLoadersOwner;
    auto& executableLoaders = executableLoadersOwner;
    executableLoaders.resize(this->exeInputFiles.size());

    // Clean up the sets once they go out of scope.  We use a complicated struct here just in case we hit an error path
    struct ScopedDeleter
    {
        ScopedDeleter(std::vector<const dyld4::PrebuiltLoaderSet*>& loaderSets) : loaderSets(loaderSets) { }
        ~ScopedDeleter() {
            for ( const auto* loaderSet : loaderSets ) {
                if ( loaderSet != nullptr )
                    loaderSet->deallocate();
            }
        }
        std::vector<const dyld4::PrebuiltLoaderSet*>& loaderSets;
    };

    ScopedDeleter deleter(executableLoaders);
    Error err = parallel::forEach(this->exeInputFiles, ^(size_t index, InputFile*& exeFile) {
        const mach_o::Layout& exeLayout = layoutBuilderPtr->getExecutableLayout((uint32_t)index);

        if ( log ) {
            printf("osExecutable: %s\n", exeFile->path.c_str());
        }

        const MachOFile* mainMF = exeFile->mf;
        KernelArgs       kernArgs(mainMF, { "test.exe" }, {}, {});
        SyscallDelegate  osDelegate;
        osDelegate._mappedOtherDylibs = otherMapping;
        osDelegate._gradedArchs       = &this->options.archs;
        //osDelegate._dyldCache           = dyldCache;
        EphemeralAllocator alloc;
        ProcessConfig      processConfig(&kernArgs, osDelegate, alloc);
        RuntimeState       state(processConfig, alloc);
        RuntimeState*      statePtr = &state;
        Diagnostics        launchDiag;

        processConfig.reset(mainMF, exeFile->path.c_str(), nullptr);
        state.resetCachedDylibsArrays(cachedDylibsLoaderSet);

        // FIXME: This is terrible and needs to be a real reset method
        processConfig.dyldCache.cacheBuilderDylibs = &processConfigDylibs;
        processConfig.dyldCache.dylibsExpectedOnDisk = !this->options.dylibsRemovedFromDisk;
        processConfig.dyldCache.development = isDevelopmentSharedCache(this->options);

        if ( !this->objcOptimizer.objcDylibs.empty() ) {
            processConfig.dyldCache.objcClassHashTable = (const objc::ClassHashTable*)this->objcClassOptimizer.classHashTableChunk->subCacheBuffer;
            processConfig.dyldCache.objcSelectorHashTable = (const objc::SelectorHashTable*)this->objcSelectorOptimizer.selectorHashTableChunk->subCacheBuffer;
            processConfig.dyldCache.objcProtocolHashTable = (const objc::ProtocolHashTable*)this->objcProtocolOptimizer.protocolHashTableChunk->subCacheBuffer;
            processConfig.dyldCache.objcHeaderInfoRO = (const objc::HeaderInfoRO*)this->objcOptimizer.headerInfoReadOnlyChunk->subCacheBuffer;
            processConfig.dyldCache.objcHeaderInfoRW = (const objc::HeaderInfoRW*)this->objcOptimizer.headerInfoReadWriteChunk->subCacheBuffer;
            processConfig.dyldCache.objcHeaderInfoROUnslidVMAddr = this->objcOptimizer.headerInfoReadOnlyChunk->cacheVMAddress.rawValue();
            processConfig.dyldCache.objcProtocolClassCacheOffset = objcProtocolClassCacheOffset.rawValue();
            processConfig.dyldCache.unslidLoadAddress = config.layout.cacheBaseAddress.rawValue();
        }

        Loader::LoadOptions::Finder loaderFinder = ^(Diagnostics& diag, dyld3::Platform plat, const char* loadPath, const dyld4::Loader::LoadOptions& loadOptions) {
            // when building macOS cache, there may be some incorrect catalyst paths
            if ( (plat == dyld3::Platform::iOSMac) && (strncmp(loadPath, "/System/iOSSupport/", 19) != 0) ) {
                char altPath[PATH_MAX];
                strlcpy(altPath, "/System/iOSSupport", PATH_MAX);
                strlcat(altPath, loadPath, PATH_MAX);
                if ( const dyld4::PrebuiltLoader* ldr = cachedDylibsLoaderSet->findLoader(altPath) )
                    return (const Loader*)ldr;
            }

            // check if path is a dylib in the dyld cache, then use its PrebuiltLoader
            if ( const dyld4::PrebuiltLoader* ldr = cachedDylibsLoaderSet->findLoader(loadPath) )
                return (const Loader*)ldr;

            // call through to getLoader() which will expand @paths
            const Loader* ldr = Loader::getLoader(diag, *statePtr, loadPath, loadOptions);
            return (const Loader*)ldr;
        };

        if ( Loader* mainLoader = JustInTimeLoader::makeLaunchLoader(launchDiag, state, mainMF, exeFile->path.c_str(), &exeLayout) ) {
            __block dyld4::MissingPaths missingPaths;
            auto                        missingLogger = ^(const char* mustBeMissingPath) {
                missingPaths.addPath(mustBeMissingPath);
            };
            Loader::LoadChain   loadChainMain { nullptr, mainLoader };
            Loader::LoadOptions loadOptions;
            loadOptions.staticLinkage       = true;
            loadOptions.launching           = true;
            loadOptions.canBeDylib          = true;
            loadOptions.rpathStack          = &loadChainMain;
            loadOptions.finder              = loaderFinder;
            loadOptions.pathNotFoundHandler = missingLogger;
            mainLoader->loadDependents(launchDiag, state, loadOptions);
            if ( launchDiag.hasError() ) {
                //fprintf(stderr, "warning: can't build PrebuiltLoader for '%s': %s\n", exeFile->path.c_str(), launchDiag.errorMessageCStr());
                if ( log )
                    printf("skip  %s\n", exeFile->path.c_str());
                // FIXME: Propagate errors
                return Error();
            }
            state.setMainLoader(mainLoader);
            const dyld4::PrebuiltLoaderSet* prebuiltAppSet = dyld4::PrebuiltLoaderSet::makeLaunchSet(launchDiag, state, missingPaths);
            if ( launchDiag.hasError() ) {
                //fprintf(stderr, "warning: can't build PrebuiltLoaderSet for '%s': %s\n", exeFile->path.c_str(), launchDiag.errorMessageCStr());
                if ( log )
                    printf("skip  %s\n", exeFile->path.c_str());

                // FIXME: Propagate errors
                return Error();
            }
            if ( prebuiltAppSet != nullptr ) {
                executableLoaders[index] = prebuiltAppSet;
                if ( log )
                    printf("%5lu %s\n", prebuiltAppSet->size(), exeFile->path.c_str());
                //state.setProcessPrebuiltLoaderSet(prebuiltAppSet);
                //prebuiltAppSet->print(state, stderr);
            }
        }
        else {
            fprintf(stderr, "warning: can't build PrebuiltLoaderSet for '%s': %s\n", exeFile->path.c_str(), launchDiag.errorMessageCStr());
        }

        return Error();
    });

    assert(!err.hasError());

    std::map<std::string_view, const dyld4::PrebuiltLoaderSet*> prebuiltsMap;
    uint64_t prebuiltsSpace = 0;
    for ( uint64_t i = 0; i != this->exeInputFiles.size(); ++i ) {
        const InputFile*                exeFile   = this->exeInputFiles[i];
        const dyld4::PrebuiltLoaderSet* loaderSet = executableLoaders[i];
        if ( loaderSet == nullptr )
            continue;

        prebuiltsMap[exeFile->path.c_str()] = loaderSet;
        prebuiltsSpace += alignTo(loaderSet->size(), 8);
    }

    const PrebuiltLoaderChunk* loaderChunk = this->prebuiltLoaderBuilder.executablesLoaderChunk;
    uint64_t loaderBufferSize = loaderChunk->subCacheFileSize.rawValue();

    if ( this->config.log.printStats ) {
        stats.add("  dyld4 executable Loader's : used %lld out of %lld bytes of buffer\n", prebuiltsSpace, loaderBufferSize);
    }

    if ( prebuiltsSpace > loaderBufferSize ) {
        if ( dylibHasMissingDependency ) {
            // At least one dylib was evicted.  If it was soemthing common, like UIKit/AppKit, then its going to
            // end up being included in every executable loader and the buffer will overflow
            this->warning("cache buffer too small to hold executable PrebuiltLoaderSet (prebuiltLoaderSet size=%lluKB, buffer size=%lldKB)",
                          prebuiltsSpace / 1024, loaderBufferSize / 1024);

            // For now, just empty the map.  That'll let us emit an empty Trie and PBLS
            prebuiltsMap.clear();
        } else {
            return Error("cache buffer too small to hold executable PrebuiltLoaderSet (prebuiltLoaderSet size=%lluKB, buffer size=%lldKB)",
                         prebuiltsSpace / 1024, loaderBufferSize / 1024);
        }
    }

    // copy all PrebuiltLoaderSets into cache

    uint8_t* poolBase = loaderChunk->subCacheBuffer;
    __block std::vector<DylibIndexTrie::Entry> trieEntrys;
    uint32_t                                   currentPoolOffset = 0;
    for ( const auto& entry : prebuiltsMap ) {
        const dyld4::PrebuiltLoaderSet* pbls = entry.second;
        // FIXME: Use a string_view if we change Trie to accept it
        std::string path = entry.first.data();
        trieEntrys.push_back(DylibIndexTrie::Entry(path, DylibIndex(currentPoolOffset)));

        // Add cdHashes to the trie so that we can look up by cdHash at runtime
        // Assumes that cdHash strings at runtime use lowercase a-f digits
        const dyld4::PrebuiltLoader* mainPbl = pbls->atIndex(0);
        mainPbl->withCDHash(^(const uint8_t* cdHash) {
            std::string cdHashStr = "/cdhash/";
            cdHashStr.reserve(24);
            for ( int i = 0; i < 20; ++i ) {
                uint8_t byte    = cdHash[i];
                uint8_t nibbleL = byte & 0x0F;
                uint8_t nibbleH = byte >> 4;
                if ( nibbleH < 10 )
                    cdHashStr += '0' + nibbleH;
                else
                    cdHashStr += 'a' + (nibbleH - 10);
                if ( nibbleL < 10 )
                    cdHashStr += '0' + nibbleL;
                else
                    cdHashStr += 'a' + (nibbleL - 10);
            }
            trieEntrys.push_back(DylibIndexTrie::Entry(cdHashStr, DylibIndex(currentPoolOffset)));
        });

        size_t size = pbls->size();
        ::memcpy(poolBase + currentPoolOffset, pbls, size);
        currentPoolOffset += alignTo(size, 8);
    }

    const CacheTrieChunk* trieChunk = this->prebuiltLoaderBuilder.executableTrieChunk;

    // build trie of indexes into closures list
    DylibIndexTrie       programTrie(trieEntrys);
    std::vector<uint8_t> trieBytes;
    programTrie.emit(trieBytes);
    while ( (trieBytes.size() % 8) != 0 )
        trieBytes.push_back(0);

    uint64_t trieBufferSize = trieChunk->subCacheFileSize.rawValue();
    if ( trieBytes.size() > trieBufferSize ) {
        return Error("cache buffer too small to hold executable trie (trie size=%lldKB, buffer size=%lldKB)",
                     (uint64_t)trieBytes.size() / 1024, trieBufferSize / 1024);
    }

    ::memcpy(trieChunk->subCacheBuffer, &trieBytes[0], trieBytes.size());

    return Error();
}

void SharedCacheBuilder::emitSymbolTable()
{
    Timer::Scope timedScope(this->config, "emitSymbolTable time");

    for ( SubCache& subCache : this->subCaches ) {
        if ( subCache.symbolStringsOptimizer.symbolStringsChunk == nullptr )
            continue;

        uint8_t* buffer = subCache.symbolStringsOptimizer.symbolStringsChunk->subCacheBuffer;

        for ( const auto& stringAndPos : subCache.symbolStringsOptimizer.stringMap ) {
            const std::string_view& str = stringAndPos.first;
            const uint32_t bufferOffset = stringAndPos.second;

            memcpy(buffer + bufferOffset, str.data(), str.size());
        }
    }
}

void SharedCacheBuilder::emitUnmappedLocalSymbols()
{
    if ( this->options.localSymbolsMode != LocalSymbolsMode::unmap )
        return;

    Timer::Scope timedScope(this->config, "emitUnmappedLocalSymbols time");

    auto& optimizer = this->unmappedSymbolsOptimizer;

    const uint32_t entriesOffset = sizeof(dyld_cache_local_symbols_info);
    const uint32_t entriesCount  = (uint32_t)optimizer.symbolInfos.size();
    const uint32_t nlistOffset   = (uint32_t)(optimizer.symbolNlistChunk.subCacheFileOffset.rawValue() - optimizer.unmappedSymbolsChunk.subCacheFileOffset.rawValue());
    const uint32_t nlistCount    = (uint32_t)std::max(optimizer.symbolNlistChunk.nlist32.size(), optimizer.symbolNlistChunk.nlist64.size());
    const uint32_t stringsSize   = (uint32_t)optimizer.symbolStringsChunk.subCacheFileSize.rawValue();
    const uint32_t stringsOffset = (uint32_t)(optimizer.symbolStringsChunk.subCacheFileOffset.rawValue() - optimizer.unmappedSymbolsChunk.subCacheFileOffset.rawValue());

    // Emit the header and symbol info
    {
        dyld_cache_local_symbols_info* infoHeader = (dyld_cache_local_symbols_info*)optimizer.unmappedSymbolsChunk.subCacheBuffer;
        // fill in header info
        infoHeader->nlistOffset       = nlistOffset;
        infoHeader->nlistCount        = nlistCount;
        infoHeader->stringsOffset     = stringsOffset;
        infoHeader->stringsSize       = stringsSize;
        infoHeader->entriesOffset     = entriesOffset;
        infoHeader->entriesCount      = entriesCount;

        // copy info for each dylib
        dyld_cache_local_symbols_entry_64* entries = (dyld_cache_local_symbols_entry_64*)(((uint8_t*)infoHeader)+entriesOffset);
        for (uint32_t i = 0; i < entriesCount; ++i) {
            entries[i].dylibOffset        = (this->cacheDylibs[i].cacheLoadAddress - this->config.layout.cacheBaseAddress).rawValue();
            entries[i].nlistStartIndex    = optimizer.symbolInfos[i].nlistStartIndex;
            entries[i].nlistCount         = optimizer.symbolInfos[i].nlistCount;
        }
    }

    // Emit nlists
    if ( this->config.layout.is64 ) {
        memcpy(optimizer.symbolNlistChunk.subCacheBuffer, optimizer.symbolNlistChunk.nlist64.data(), optimizer.symbolNlistChunk.subCacheFileSize.rawValue());
    } else {
        memcpy(optimizer.symbolNlistChunk.subCacheBuffer, optimizer.symbolNlistChunk.nlist32.data(), optimizer.symbolNlistChunk.subCacheFileSize.rawValue());
    }

    // Emit strings
    {
        uint8_t* buffer = optimizer.symbolStringsChunk.subCacheBuffer;

        for ( const auto& stringAndPos : optimizer.stringMap ) {
            const std::string_view& str = stringAndPos.first;
            const uint32_t bufferOffset = stringAndPos.second;

            memcpy(buffer + bufferOffset, str.data(), str.size());
        }
    }
}

void SharedCacheBuilder::emitObjCSelectorStrings()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCSelectorStrings time");

    // Find the subCache with the strings
    for ( SubCache& subCache : this->subCaches ) {
        if ( !subCache.objcSelectorStrings )
            continue;

        uint8_t* const pos = subCache.objcSelectorStrings->subCacheBuffer;
        for ( const objc::ObjCString& stringAndOffset : this->objcSelectorOptimizer.selectorsArray ) {
            const std::string_view& str = stringAndOffset.first;
            memcpy(pos + stringAndOffset.second, str.data(), str.size());
        }
    }
}

void SharedCacheBuilder::emitObjCClassNameStrings()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCClassNameStrings time");

    // Find the subCache with the strings
    for ( SubCache& subCache : this->subCaches ) {
        if ( !subCache.objcClassNameStrings )
            continue;

        uint8_t* const pos = subCache.objcClassNameStrings->subCacheBuffer;
        for ( const objc::ObjCString& stringAndOffset : this->objcClassOptimizer.namesArray ) {
            const std::string_view& str = stringAndOffset.first;
            memcpy(pos + stringAndOffset.second, str.data(), str.size());
        }
    }
}

void SharedCacheBuilder::emitObjCProtocolNameStrings()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCProtocolNameStrings time");

    // Find the subCache with the strings
    for ( SubCache& subCache : this->subCaches ) {
        if ( !subCache.objcProtocolNameStrings )
            continue;

        uint8_t* const pos = subCache.objcProtocolNameStrings->subCacheBuffer;
        for ( const objc::ObjCString& stringAndOffset : this->objcProtocolOptimizer.namesArray ) {
            const std::string_view& str = stringAndOffset.first;
            memcpy(pos + stringAndOffset.second, str.data(), str.size());
        }
    }
}

void SharedCacheBuilder::emitObjCSwiftDemangledNameStrings()
{
    Timer::Scope timedScope(this->config, "emitObjCSwiftDemangledNameStrings time");

    // Find the subCache with the strings
    for ( SubCache& subCache : this->subCaches ) {
        if ( !subCache.objcSwiftDemangledNameStrings )
            continue;

        uint8_t* pos = subCache.objcSwiftDemangledNameStrings->subCacheBuffer;
        for ( const std::string& str : this->objcProtocolOptimizer.swiftDemangledNames ) {
            memcpy(pos, str.data(), str.size());
            pos += str.size() + 1;
        }
    }
}

void SharedCacheBuilder::emitObjCHashTables()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCHashTables time");

    Diagnostics diag;

    // Find the subCache with the hash tables
    cache_builder::ObjCSelectorHashTableChunk* selectorsHashTable = nullptr;
    cache_builder::ObjCClassHashTableChunk*    classesHashTable   = nullptr;
    cache_builder::ObjCProtocolHashTableChunk* protocolsHashTable = nullptr;
    for ( SubCache& subCache : this->subCaches ) {
        if ( subCache.objcSelectorsHashTable ) {
            assert(selectorsHashTable == nullptr);
            selectorsHashTable = subCache.objcSelectorsHashTable.get();
        }
        if ( subCache.objcClassesHashTable ) {
            assert(classesHashTable == nullptr);
            classesHashTable = subCache.objcClassesHashTable.get();
        }
        if ( subCache.objcProtocolsHashTable ) {
            assert(protocolsHashTable == nullptr);
            protocolsHashTable = subCache.objcProtocolsHashTable.get();
        }
    }

    assert(selectorsHashTable != nullptr);
    assert(classesHashTable != nullptr);
    assert(protocolsHashTable != nullptr);

    // Emit the selectors hash table
    {
        Timer::Scope innerTimedScope(this->config, "emitObjCHashTables (selectors) time");

        objc::SelectorHashTable* selopt = new (selectorsHashTable->subCacheBuffer) objc::SelectorHashTable;
        selopt->write(diag, this->objcSelectorOptimizer.selectorStringsChunk->cacheVMAddress.rawValue(),
                      this->objcSelectorOptimizer.selectorHashTableChunk->cacheVMAddress.rawValue(),
                      selectorsHashTable->subCacheFileSize.rawValue(), this->objcSelectorOptimizer.selectorsArray);

        assert(!diag.hasError());
    }

    // Emit the classes hash table
    {
        Timer::Scope innerTimedScope(this->config, "emitObjCHashTables (classes) time");

        objc::ClassHashTable* classopt = new (classesHashTable->subCacheBuffer) objc::ClassHashTable;
        classopt->write(diag, this->objcClassOptimizer.classNameStringsChunk->cacheVMAddress.rawValue(),
                        this->objcClassOptimizer.classHashTableChunk->cacheVMAddress.rawValue(),
                        this->config.layout.cacheBaseAddress.rawValue(), classesHashTable->subCacheFileSize.rawValue(),
                        this->objcClassOptimizer.namesArray, this->objcClassOptimizer.classes);

        assert(!diag.hasError());
    }

    // Emit the protocols hash table
    {
        Timer::Scope innerTimedScope(this->config, "emitObjCHashTables (protocols) time");

        objc::protocol_map       protocolMap;
        objc::ProtocolHashTable* protocolopt = new (protocolsHashTable->subCacheBuffer) objc::ProtocolHashTable;
        protocolopt->write(diag, this->objcProtocolOptimizer.protocolNameStringsChunk->cacheVMAddress.rawValue(),
                           this->objcProtocolOptimizer.protocolHashTableChunk->cacheVMAddress.rawValue(),
                           this->config.layout.cacheBaseAddress.rawValue(), protocolsHashTable->subCacheFileSize.rawValue(),
                           this->objcProtocolOptimizer.namesArray, this->objcProtocolOptimizer.protocols);

        assert(!diag.hasError());
    }
}

// The given value is in the section.  Returns the VM address of that location
static CacheVMAddress getVMAddressInSection(const Chunk& section, const void* value)
{
    assert(value >= section.subCacheBuffer);
    assert(value < (section.subCacheBuffer + section.subCacheFileSize.rawValue()));

    uint64_t offsetInSection = (uint64_t)value - (uint64_t)section.subCacheBuffer;
    return section.cacheVMAddress + VMOffset(offsetInSection);
}

void SharedCacheBuilder::emitObjCHeaderInfo()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCHeaderInfo time");

    // Emit header info RO
    auto* readOnlyList    = (ObjCOptimizer::header_info_ro_list_t*)this->objcOptimizer.headerInfoReadOnlyChunk->subCacheBuffer;
    readOnlyList->count   = (uint32_t)this->objcOptimizer.objcDylibs.size();
    readOnlyList->entsize = this->config.layout.is64 ? sizeof(ObjCOptimizer::header_info_ro_64_t) : sizeof(ObjCOptimizer::header_info_ro_32_t);

    for ( uint32_t i = 0; i != readOnlyList->count; ++i ) {
        CacheDylib& cacheDylib = *this->objcOptimizer.objcDylibs[i];

        __block CacheVMAddress  cacheImageInfoAddress;
        __block uint8_t*        cacheImageInfoBuffer = nullptr;
        cacheDylib.forEachCacheSection(^(std::string_view segmentName, std::string_view sectionName,
                                         uint8_t* sectionBuffer, CacheVMAddress sectionVMAddr,
                                         CacheVMSize sectionVMSize, bool& stop) {
            if ( !segmentName.starts_with("__DATA") )
                return;
            if ( sectionName != "__objc_imageinfo" )
                return;

            cacheImageInfoAddress = sectionVMAddr;
            cacheImageInfoBuffer = sectionBuffer;
            stop = true;
        });

        assert(cacheImageInfoBuffer != nullptr);

        void*          arrayElement     = &readOnlyList->arrayBase[0] + (i * readOnlyList->entsize);
        CacheVMAddress machHeaderVMAddr = cacheDylib.cacheLoadAddress;

        if ( this->config.layout.is64 ) {
            ObjCOptimizer::header_info_ro_64_t* element = (ObjCOptimizer::header_info_ro_64_t*)arrayElement;

            // mhdr_offset
            CacheVMAddress headerOffsetVMAddr = getVMAddressInSection(*this->objcOptimizer.headerInfoReadOnlyChunk, &element->mhdr_offset);
            int64_t       headerOffset       = machHeaderVMAddr.rawValue() - headerOffsetVMAddr.rawValue();
            element->mhdr_offset             = headerOffset;
            // Check for truncation
            assert(element->mhdr_offset == headerOffset);

            // info_offset
            CacheVMAddress infoOffsetVMAddr = getVMAddressInSection(*this->objcOptimizer.headerInfoReadOnlyChunk, &element->info_offset);
            int64_t       infoOffset       = cacheImageInfoAddress.rawValue() - infoOffsetVMAddr.rawValue();
            element->info_offset           = infoOffset;
            // Check for truncation
            assert(element->info_offset == infoOffset);
        }
        else {
            ObjCOptimizer::header_info_ro_32_t* element = (ObjCOptimizer::header_info_ro_32_t*)arrayElement;

            // mhdr_offset
            CacheVMAddress headerOffsetVMAddr = getVMAddressInSection(*this->objcOptimizer.headerInfoReadOnlyChunk, &element->mhdr_offset);
            int64_t       headerOffset       = machHeaderVMAddr.rawValue() - headerOffsetVMAddr.rawValue();
            element->mhdr_offset             = (int32_t)headerOffset;
            // Check for truncation
            assert(element->mhdr_offset == headerOffset);

            // info_offset
            CacheVMAddress infoOffsetVMAddr = getVMAddressInSection(*this->objcOptimizer.headerInfoReadOnlyChunk, &element->info_offset);
            int64_t       infoOffset       = cacheImageInfoAddress.rawValue() - infoOffsetVMAddr.rawValue();
            element->info_offset           = (int32_t)infoOffset;
            // Check for truncation
            assert(element->info_offset == infoOffset);
        }

        // Set the dylib to be optimized, which lets it use this header info
        struct objc_image_info {
            int32_t version;
            uint32_t flags;
        };
        objc_image_info* info = (objc_image_info*)cacheImageInfoBuffer;
        info->flags = info->flags | (1 << 3);
    }

    // Emit header info RW
    auto* readWriteList    = (ObjCOptimizer::header_info_rw_list_t*)this->objcOptimizer.headerInfoReadWriteChunk->subCacheBuffer;
    readWriteList->count   = (uint32_t)this->objcOptimizer.objcDylibs.size();
    readWriteList->entsize = this->config.layout.is64 ? sizeof(ObjCOptimizer::header_info_rw_64_t) : sizeof(ObjCOptimizer::header_info_rw_32_t);

    for ( uint32_t i = 0; i != readWriteList->count; ++i ) {
        void* arrayElement = &readWriteList->arrayBase[0] + (i * readWriteList->entsize);
        if ( this->config.layout.is64 ) {
            bzero(arrayElement, sizeof(ObjCOptimizer::header_info_rw_64_t));
        }
        else {
            bzero(arrayElement, sizeof(ObjCOptimizer::header_info_rw_32_t));
        }
    }
}

void SharedCacheBuilder::emitObjCOptsHeader()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return;

    Timer::Scope timedScope(this->config, "emitObjCOptsHeader time");

    CacheVMAddress cacheBaseAddress = this->config.layout.cacheBaseAddress;

    auto* headerChunk = this->objcOptimizer.optsHeaderChunk;

    uint32_t headerFlags = 0;
    switch ( this->options.kind ) {
        case CacheKind::development:
            break;
        case CacheKind::universal:
            headerFlags |= objc_opt::IsProduction;
            break;
    }
    if ( !this->objcOptimizer.foundMissingWeakSuperclass )
        headerFlags |= objc_opt::NoMissingWeakSuperclasses;
    headerFlags |= objc_opt::LargeSharedCache;

    assert(headerChunk->subCacheFileSize.rawValue() == sizeof(ObjCOptimizationHeader));
    ObjCOptimizationHeader* header                  = (ObjCOptimizationHeader*)headerChunk->subCacheBuffer;
    header->version                                 = 1;
    header->flags                                   = headerFlags;
    header->headerInfoROCacheOffset                 = 0;
    header->headerInfoRWCacheOffset                 = 0;
    header->selectorHashTableCacheOffset            = 0;
    header->classHashTableCacheOffset               = 0;
    header->protocolHashTableCacheOffset            = 0;
    header->relativeMethodSelectorBaseAddressOffset = 0;

    // TODO: Do we need to check if these sections have content?
    header->headerInfoROCacheOffset                 = (this->objcOptimizer.headerInfoReadOnlyChunk->cacheVMAddress - cacheBaseAddress).rawValue();
    header->headerInfoRWCacheOffset                 = (this->objcOptimizer.headerInfoReadWriteChunk->cacheVMAddress - cacheBaseAddress).rawValue();
    header->selectorHashTableCacheOffset            = (this->objcSelectorOptimizer.selectorHashTableChunk->cacheVMAddress - cacheBaseAddress).rawValue();
    header->classHashTableCacheOffset               = (this->objcClassOptimizer.classHashTableChunk->cacheVMAddress - cacheBaseAddress).rawValue();
    header->protocolHashTableCacheOffset            = (this->objcProtocolOptimizer.protocolHashTableChunk->cacheVMAddress - cacheBaseAddress).rawValue();
    header->relativeMethodSelectorBaseAddressOffset = (this->objcSelectorOptimizer.selectorStringsChunk->cacheVMAddress - cacheBaseAddress).rawValue();

    // Also fill in the fields in the objc section.
    // FIXME: Remove this once libobjc and lldb can use SPI or the above shared cache struct
    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( cacheDylib.installName != "/usr/lib/libobjc.A.dylib" )
            continue;

        cacheDylib.cacheMF->forEachSection(^(const dyld3::MachOFile::SectionInfo &sectInfo,
                                             bool malformedSectionRange, bool &stop) {
            if (strcmp(sectInfo.segInfo.segName, "__TEXT") != 0)
                return;
            if (strcmp(sectInfo.sectName, "__objc_opt_ro") != 0)
                return;

            // Find the buffer for the section
            stop = true;

            const DylibSegmentChunk& segment = cacheDylib.segments[sectInfo.segInfo.segIndex];

            VMAddress sectionVMAddr(sectInfo.sectAddr);
            VMAddress segmentVMAddr(sectInfo.segInfo.vmAddr);
            VMOffset sectionOffsetInSegment = sectionVMAddr - segmentVMAddr;
            uint8_t* sectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();

            // All fields in the old header are offsets from the header.  This is how much to
            // shift them by
            uint64_t headerCacheOffset = sectInfo.sectAddr - this->config.layout.cacheBaseAddress.rawValue();

            // Found the section, now write the content
            objc_opt::objc_opt_t* libROHeader = (objc_opt::objc_opt_t *)sectionBuffer;
            libROHeader->flags                                   = header->flags;
            libROHeader->selopt_offset                           = (uint32_t)(header->selectorHashTableCacheOffset - headerCacheOffset);
            libROHeader->unused_clsopt_offset                    = 0;
            libROHeader->unused_protocolopt_offset               = 0;
            libROHeader->headeropt_ro_offset                     = (uint32_t)(header->headerInfoROCacheOffset - headerCacheOffset);
            libROHeader->headeropt_rw_offset                     = (uint32_t)(header->headerInfoRWCacheOffset - headerCacheOffset);
            libROHeader->unused_protocolopt2_offset              = 0;
            libROHeader->largeSharedCachesClassOffset            = (uint32_t)(header->classHashTableCacheOffset - headerCacheOffset);
            libROHeader->largeSharedCachesProtocolOffset         = (uint32_t)(header->protocolHashTableCacheOffset - headerCacheOffset);
            libROHeader->relativeMethodSelectorBaseAddressOffset = (header->relativeMethodSelectorBaseAddressOffset - headerCacheOffset);
        });
    }
}

// FIXME: If we delete CacheVMAddress then we don't need a template
template <typename VMAddrType>
static void updateFixupRebaseTarget(const BuilderConfig& config,
                                    MachOFile::ChainedFixupPointerOnDisk* ref, uint16_t chainedPointerFormat,
                                    VMAddrType newVMAddress, VMAddrType cacheBaseAddress)
{
    VMOffset cacheVMOffset = newVMAddress - cacheBaseAddress;

    if ( (chainedPointerFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND) || (chainedPointerFormat == DYLD_CHAINED_PTR_ARM64E_USERLAND24) ) {
        assert(!ref->arm64e.rebase.auth);
        ref->arm64e.rebase.target = cacheVMOffset.rawValue();
        assert(ref->arm64e.rebase.target == cacheVMOffset.rawValue());
    }
    else if ( chainedPointerFormat == DYLD_CHAINED_PTR_64_OFFSET ) {
        ref->generic64.rebase.target = cacheVMOffset.rawValue();
        assert(ref->generic64.rebase.target == cacheVMOffset.rawValue());
    }
    else if ( !config.layout.is64 ) {
        // 32-bit cache dylibs don't have enough bits for the chain, so we use raw VMAddr's instead
        assert(chainedPointerFormat == 0);

        ref->raw32 = (uint32_t)newVMAddress.rawValue();
    }
    else {
        assert(0);
    }
}

// Struct matching dyld4::LibdyldDyld4Section to be used with a variable pointer size.
// This is so we can use it in the shared cache builder, which is always
// 64-bit but can emit 32-bit structs
template <typename P>
struct FixedSizeLibdyldDyld4Section {
    P apis;
    P allImageInfos;
    P defaultVars[5];
    P dyldLookupFuncAddr;
    P tlv_get_addrAddr;
};

static_assert(sizeof(FixedSizeLibdyldDyld4Section<intptr_t>) == sizeof(dyld4::LibdyldDyld4Section));

void SharedCacheBuilder::optimizeTLVs()
{
    Stats        stats(this->config);
    Timer::Scope timedScope(this->config, "optimizeTLVs time");

    typedef CacheDylib::BindTargetAndName BindTargetAndName;
    typedef CacheDylib::SearchMode SearchMode;

    __block Diagnostics diag;

    //
    // Find libpthread to find the available pthread key range
    // Find libdyld to make the thunks point to tlv_get_addr
    //
    const CacheDylib* pthreadDylib = nullptr;
    const CacheDylib* libdyldDylib = nullptr;
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        if ( cacheDylib.installName.ends_with("/libsystem_pthread.dylib") )
            pthreadDylib = &cacheDylib;
        else if ( cacheDylib.installName.ends_with("/libdyld.dylib") )
            libdyldDylib = &cacheDylib;
    }

    if ( (pthreadDylib == nullptr) || (libdyldDylib == nullptr) ) {
        this->warning("Could not find libpthread or libdyld (TLVs not optimized)");
        return;
    }

    // Find the tlv_get_addrAddr from inside the __dyld4 section
    __block CacheVMAddress getAddrVMAddr;
    __block bool foundTLVGetAddr = false;
    libdyldDylib->cacheMF->forEachSection(^(const MachOFile::SectionInfo &sectInfo,
                                            bool malformedSectionRange, bool &stop) {
        if ( strcmp(sectInfo.sectName, "__dyld4") != 0 )
            return;

        if ( (strncmp(sectInfo.segInfo.segName, "__DATA", 6) != 0)
            && (strncmp(sectInfo.segInfo.segName, "__AUTH", 6) != 0) )
            return;

        // Found the section we need.  Now to check if its valid
        stop = true;

        const DylibSegmentChunk& segment = libdyldDylib->segments[sectInfo.segInfo.segIndex];

        VMAddress sectionVMAddr(sectInfo.sectAddr);
        VMAddress segmentVMAddr(sectInfo.segInfo.vmAddr);
        VMOffset sectionOffsetInSegment = sectionVMAddr - segmentVMAddr;
        uint8_t* sectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();


        if ( this->config.layout.is64 ) {
            typedef FixedSizeLibdyldDyld4Section<uint64_t> dyld4_section_t;
            if ( sectInfo.sectSize < sizeof(dyld4_section_t) ) {
                // Old libdyld without the field we need
                return;
            }

            const dyld4_section_t* dyldSection = (dyld4_section_t*)sectionBuffer;
            CacheVMAddress cacheBaseAddress = this->config.layout.cacheBaseAddress;
            getAddrVMAddr = Fixup::Cache64::getCacheVMAddressFromLocation(cacheBaseAddress,
                                                                          &dyldSection->tlv_get_addrAddr);
        } else {
            typedef FixedSizeLibdyldDyld4Section<uint32_t> dyld4_section_t;
            if ( sectInfo.sectSize < sizeof(dyld4_section_t) ) {
                // Old libdyld without the field we need
                return;
            }

            const dyld4_section_t* dyldSection = (dyld4_section_t*)sectionBuffer;
            CacheVMAddress cacheBaseAddress = this->config.layout.cacheBaseAddress;
            getAddrVMAddr = Fixup::Cache32::getCacheVMAddressFromLocation(cacheBaseAddress,
                                                                          &dyldSection->tlv_get_addrAddr);
        }

        foundTLVGetAddr = true;
    });

    if ( !foundTLVGetAddr ) {
        this->warning("Could not find tlv_get_addr (TLVs not optimized)");
        return;
    }

    // We read the value for this symbol to know the first key we can allocate for TLVs
    // We then have to stop optimizing if and when we reach "end", that's the
    // maximum number of keys allocated to us by libpthread.
    // Keys have to lie within [start, end] (closed range)
    // As of Sydney, there are 80 keys available with 35 used (47 on Rome)

    auto getSymbol = ^(const char* symbolName) {
        std::optional<BindTargetAndName> symbol = pthreadDylib->hasExportedSymbol(diag, symbolName, SearchMode::onlySelf);
        if ( !symbol.has_value() ) {
            this->warning("libpthread's TSD optimization symbols missing (TLVs not optimized)");
            return (const void*)nullptr;
        }

        // hasExportedSymbol() returns the address in the input image.  Convert to cache addresses
        if ( symbol->first.kind == CacheDylib::BindTarget::Kind::inputImage ) {
            const CacheDylib::BindTarget::InputImage& inputImage = symbol->first.inputImage;
            CacheVMAddress vmAddr = inputImage.targetDylib->cacheLoadAddress + inputImage.targetRuntimeOffset;
            for ( const DylibSegmentChunk& segment : inputImage.targetDylib->segments ) {
                CacheVMAddress segmentStartAddr = segment.cacheVMAddress;
                CacheVMAddress segmentEndAddr = segmentStartAddr + segment.cacheVMSize;
                if ( (vmAddr >= segmentStartAddr) && (vmAddr < segmentEndAddr) ) {
                    VMOffset offsetInSegment = vmAddr - segmentStartAddr;
                    return (const void*)(segment.subCacheBuffer + offsetInSegment.rawValue());
                }
            }
            this->warning("libpthread's TSD optimization symbol is not in cache dylib (TLVs not optimized)");
            return (const void*)nullptr;
        } else {
            this->warning("libpthread's TSD optimization symbol is wrong kind (TLVs not optimized)");
            return (const void*)nullptr;
        }
    };

    const uint32_t* firstKey = (const uint32_t*)getSymbol("__pthread_tsd_shared_cache_first");
    const uint32_t* lastKey = (const uint32_t*)getSymbol("__pthread_tsd_shared_cache_last");
    if ( (firstKey == nullptr) || (lastKey == nullptr) ) {
        // We should have emitted a warning in getSymbol().
        return;
    }

    // Closed range.
    const uint32_t availableKeyCount = *lastKey - *firstKey + 1;

    __block uint32_t tlvCount = 0;

    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        if (tlvCount > availableKeyCount) {
            return;
        }

        if ( !cacheDylib.cacheMF->hasThreadLocalVariables() )
            continue;

        // Get the next available key (one key per dylib)
        int key = *firstKey + tlvCount++;

        if ( tlvCount > availableKeyCount ) {
            // See above, we have to stop optimizing when we have used
            // all the keys libpthread has set aside for us.
            // The enumeration happens in cache order, so in theory
            // we optimize the dylibs which are in most processes first.
            // Any dylibs that we drop here are supposed not to have a
            // significant memory impact.
            this->warning("Out of available shared cache keys, stopping TLV optimization");
            return;
        }

        cacheDylib.cacheMF->forEachSection(^(const MachOFile::SectionInfo& sectInfo,
                                             bool malformedSectionRange, bool& stop) {
            if ( (sectInfo.sectFlags & SECTION_TYPE) != S_THREAD_LOCAL_VARIABLES )
                return;

            DylibSegmentChunk& segment = cacheDylib.segments[sectInfo.segInfo.segIndex];

            VMAddress sectionVMAddr(sectInfo.sectAddr);
            VMAddress segmentVMAddr(sectInfo.segInfo.vmAddr);
            VMOffset sectionOffsetInSegment = sectionVMAddr - segmentVMAddr;
            uint8_t* sectionBuffer = segment.subCacheBuffer + sectionOffsetInSegment.rawValue();

            if ( this->config.layout.is64 ) {
                struct tlv_thunk_t
                {
                    uint64_t thunk;
                    uint64_t key;
                    uint64_t offset;
                };

                uint32_t count = (uint32_t)(sectInfo.sectSize / sizeof(tlv_thunk_t));
                tlv_thunk_t* thunkBuffer = (tlv_thunk_t*)sectionBuffer;
                for ( uint32_t i = 0; i != count; ++i ) {
                    tlv_thunk_t& tlvThunk = thunkBuffer[i];

                    // Set the key to the next available key
                    tlvThunk.key = key;

                    // Set the thunk to tlv_get_addr()
                    uint8_t high8 = 0;
                    uint16_t authDiversity = 0;
                    bool authHasAddrDiv = false;
                    uint8_t authKey = ptrauth_key_asia;
                    bool isAuth = this->config.layout.hasAuthRegion;
                    Fixup::Cache64::setLocation(this->config.layout.cacheBaseAddress, &tlvThunk.thunk,
                                                getAddrVMAddr, high8,
                                                authDiversity, authHasAddrDiv, authKey, isAuth);

                    // Add to ASLR tracker
                    segment.tracker.add(&tlvThunk.thunk);
                }
            } else {
                struct tlv_thunk_t
                {
                    uint32_t thunk;
                    uint32_t key;
                    uint32_t offset;
                };

                uint32_t count = (uint32_t)(sectInfo.sectSize / sizeof(tlv_thunk_t));
                tlv_thunk_t* thunkBuffer = (tlv_thunk_t*)sectionBuffer;
                for ( uint32_t i = 0; i != count; ++i ) {
                    tlv_thunk_t& tlvThunk = thunkBuffer[i];

                    // Set the key to the next available key
                    tlvThunk.key = key;

                    // Set the thunk to tlv_get_addr()
                    Fixup::Cache32::setLocation(this->config.layout.cacheBaseAddress, &tlvThunk.thunk,
                                                getAddrVMAddr);

                    // Add to ASLR tracker
                    segment.tracker.add(&tlvThunk.thunk);
                }
            }
        });
    }

    if ( this->config.log.printStats ) {
        stats.add("  TLVs: optimized using %d shared cache keys\n", tlvCount);
    }
}


Error SharedCacheBuilder::emitUniquedGOTs()
{
    Timer::Scope timedScope(this->config, "emitUniquedGOTs time");

    // DylibSegmentChunk's don't have a pointer to their cache dylib.  Make a map for them
    std::unordered_map<const InputFile*, CacheDylib*> fileToDylibMap;
    fileToDylibMap.reserve(this->cacheDylibs.size());
    for ( CacheDylib& dylib : this->cacheDylibs )
        fileToDylibMap[dylib.inputFile] = &dylib;

    for ( SubCache& subCache : this->subCaches ) {
        // Find the DATA_CONST/AUTH_CONST in each SubCache, if it has any
        Region* dataConstRegion = nullptr;
        Region* authConstRegion = nullptr;
        for ( Region& region : subCache.regions ) {
            if ( region.kind == Region::Kind::dataConst ) {
                dataConstRegion = &region;
                continue;
            }
            if ( region.kind == Region::Kind::authConst ) {
                authConstRegion = &region;
                continue;
            }
        }

        if ( (dataConstRegion == nullptr) && (authConstRegion == nullptr) )
            continue;

        for ( bool auth : { false, true } ) {
            if ( auth && (authConstRegion == nullptr) )
                continue;
            if ( !auth && (dataConstRegion == nullptr) )
                continue;

            Region& region = auth ? *authConstRegion : *dataConstRegion;
            CoalescedGOTSection& subCacheUniquedGOTs = auth ? subCache.uniquedGOTsOptimizer.authGOTs : subCache.uniquedGOTsOptimizer.regularGOTs;
            if ( subCacheUniquedGOTs.cacheChunk == nullptr )
                continue;

            UniquedGOTsChunk* subCacheGOTChunk = subCacheUniquedGOTs.cacheChunk->isUniquedGOTsChunk();

            std::set<const void*> seenFixups;
            std::vector<PatchInfo::GOTInfo> gots;
            for ( const Chunk* chunk : region.chunks ) {
                const DylibSegmentChunk* segmentChunk = chunk->isDylibSegmentChunk();
                if ( !segmentChunk )
                    continue;

                CacheDylib*                 cacheDylib = fileToDylibMap.at(segmentChunk->inputFile);
                PatchInfo& dylibPatchInfo = this->patchTableOptimizer.patchInfos[cacheDylib->cacheIndex];

                // Walk all the binds in this dylib, looking for GOT uses of the bind
                assert(cacheDylib->bindTargets.size() == dylibPatchInfo.bindGOTUses.size());
                assert(cacheDylib->bindTargets.size() == dylibPatchInfo.bindAuthGOTUses.size());
                for ( uint32_t bindIndex = 0; bindIndex != cacheDylib->bindTargets.size(); ++bindIndex ) {
                    const CacheDylib::BindTarget& bindTarget = cacheDylib->bindTargets[bindIndex];

                    std::vector<PatchInfo::GOTInfo>* bindUses = nullptr;
                    if ( auth ) {
                        bindUses = &dylibPatchInfo.bindAuthGOTUses[bindIndex];
                    } else {
                        bindUses = &dylibPatchInfo.bindGOTUses[bindIndex];
                    }

                    // For absolute binds, just set the pointers and move on
                    if ( bindTarget.kind == CacheDylib::BindTarget::Kind::absolute ) {
                        for ( const PatchInfo::GOTInfo& got : *bindUses ) {
                            CacheVMAddress gotVMAddr = got.patchInfo.cacheVMAddr;
                            assert(gotVMAddr >= subCacheGOTChunk->cacheVMAddress);
                            assert(gotVMAddr < (subCacheGOTChunk->cacheVMAddress + subCacheGOTChunk->cacheVMSize));
                            VMOffset cacheSectionVMOffset = gotVMAddr - subCacheGOTChunk->cacheVMAddress;

                            const void* fixupLoc = subCacheGOTChunk->subCacheBuffer + cacheSectionVMOffset.rawValue();
                            if ( this->config.layout.is64 ) {
                                *(uint64_t*)fixupLoc = got.targetValue.rawValue();
                            } else {
                                *(uint32_t*)fixupLoc = (uint32_t)got.targetValue.rawValue();
                            }
                        }
                        continue;
                    }

                    gots.insert(gots.end(), bindUses->begin(), bindUses->end());
                }
            }

            // Found all the GOTs/authGOTS for this subCache.  Now we need to emit them
            for ( const PatchInfo::GOTInfo& got : gots ) {
                CacheVMAddress gotVMAddr = got.patchInfo.cacheVMAddr;
                assert(gotVMAddr >= subCacheGOTChunk->cacheVMAddress);
                assert(gotVMAddr < (subCacheGOTChunk->cacheVMAddress + subCacheGOTChunk->cacheVMSize));
                VMOffset cacheSectionVMOffset = gotVMAddr - subCacheGOTChunk->cacheVMAddress;

                void* rawFixupLoc = subCacheGOTChunk->subCacheBuffer + cacheSectionVMOffset.rawValue();

                // Ignore dupes
                if ( seenFixups.count(rawFixupLoc) )
                    continue;

                seenFixups.insert(rawFixupLoc);

                CacheVMAddress targetVMAddr = config.layout.cacheBaseAddress + got.targetValue;
                if ( this->config.layout.is64 ) {
                    uint64_t high8 = 0;
                    uint64_t finalVMAddr = targetVMAddr.rawValue();
                    if ( !got.patchInfo.authenticated ) {
                        high8 = (finalVMAddr >> 56);
                        if ( high8 != 0 ) {
                            // Remove high8 from the vmAddr
                            finalVMAddr = finalVMAddr & 0x00FFFFFFFFFFFFFFULL;
                        }
                    }
                    Fixup::Cache64::setLocation(this->config.layout.cacheBaseAddress,
                                                rawFixupLoc,
                                                CacheVMAddress(finalVMAddr),
                                                high8,
                                                got.patchInfo.discriminator,
                                                got.patchInfo.usesAddressDiversity, got.patchInfo.key,
                                                got.patchInfo.authenticated);
                } else {
                    Fixup::Cache32::setLocation(this->config.layout.cacheBaseAddress,
                                                rawFixupLoc,
                                                CacheVMAddress(targetVMAddr));
                }

                subCacheGOTChunk->tracker.add(rawFixupLoc);
            }
        }
    }

    return Error();
}

Error SharedCacheBuilder::emitCanonicalObjCProtocols()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return Error();

    Timer::Scope timedScope(this->config, "emitCanonicalObjCProtocols time");

    const bool log = false;

    // We need to find the Protocol class from libojc
    VMAddress                  protocolClassVMAddr;
    MachOFile::PointerMetaData protocolClassPMD;
    Error error = findProtocolClass(this->config, this->objcOptimizer.objcDylibs, protocolClassVMAddr, protocolClassPMD);
    if ( error.hasError() )
        return error;

    // Build ObjCVisitors for all the objc dylibs.  This is assuming we need at least 1 protocol from
    // each dylib, so its not worth doing this lazily
    std::vector<objc_visitor::Visitor> objcVisitors;
    objcVisitors.reserve(this->objcOptimizer.objcDylibs.size());

    for ( CacheDylib* cacheDylib : this->objcOptimizer.objcDylibs ) {
        objcVisitors.push_back(cacheDylib->makeCacheObjCVisitor(config, nullptr,
                                                                this->objcProtocolOptimizer.canonicalProtocolsChunk));
    }

    // The offset in the protocol buffer for the next protocol to emit
    __block VMOffset newProtocolOffset(0ULL);

    // Maps from existing protocols to the new canonical definition for that protocol
    __block std::unordered_map<VMAddress, VMAddress, VMAddressHash, VMAddressEqual> canonicalProtocolMap;

    for ( const objc::ObjCString& stringAndOffset : this->objcProtocolOptimizer.namesArray ) {
        const std::string_view& protocolName = stringAndOffset.first;
        if ( log ) {
            printf("Processing protocol: %s\n", protocolName.data());
        }

        auto protocolIt = this->objcProtocolOptimizer.protocols.find(protocolName.data());
        assert(protocolIt != this->objcProtocolOptimizer.protocols.end());

        uint64_t protocolVMAddr = protocolIt->second.first;
        uint64_t dylibObjCIndex = protocolIt->second.second;

        assert(dylibObjCIndex < this->objcOptimizer.objcDylibs.size());
        objc_visitor::Visitor& objcVisitor = objcVisitors[dylibObjCIndex];

        if ( log ) {
            printf("  at 0x%llx in %s\n", protocolVMAddr, objcVisitor.mf()->installName());
        }

        __block bool foundProtocol = false;
        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
            if ( objcProtocol.getVMAddress().rawValue() != protocolVMAddr )
                return;

            foundProtocol = true;
            stopProtocol  = true;

            // Protocols in the cache dylibs might be smaller than the new one. We copy whatever fields we can
            uint32_t oldSize = objcProtocol.getSize(objcVisitor);
            uint32_t newSize = objc_visitor::Protocol::getSize(this->config.layout.is64);

            // Make sure we have space
            assert((newProtocolOffset.rawValue() + newSize) <= this->objcProtocolOptimizer.canonicalProtocolsChunk->cacheVMSize.rawValue());
            uint8_t* newProtocolPos = this->objcProtocolOptimizer.canonicalProtocolsChunk->subCacheBuffer + newProtocolOffset.rawValue();
            memcpy(newProtocolPos, objcProtocol.getLocation(), oldSize);

            uint64_t newProtocolVMAddr = (this->objcProtocolOptimizer.canonicalProtocolsChunk->cacheVMAddress + newProtocolOffset).rawValue();
            objc_visitor::Protocol newProtocol(objcVisitor.getValueFor(VMAddress(newProtocolVMAddr)));

            // Protocols don't normally have an ISA, so set it to the protocol class in libobjc
            if ( !newProtocol.getISAVMAddr(objcVisitor).has_value() ) {
                newProtocol.setISA(objcVisitor, protocolClassVMAddr, protocolClassPMD);
            }

            if ( oldSize < newSize ) {
                // Protocol object is old. Populate new fields.
                newProtocol.setSize(objcVisitor, newSize);
            }

            // Some protocol objects are big enough to have the demangledName field but don't initialize it.
            // Initialize it here if it is not already set.
            if ( !newProtocol.getDemangledName(objcVisitor) ) {
                VMAddress protocolNameVMAddr = newProtocol.getNameVMAddr(objcVisitor);
                if ( std::optional<std::string> demangledName = copySwiftDemangledName(newProtocol.getName(objcVisitor), true) ) {
                    // Find the name in the map.  It should have been added in findObjCProtocols()
                    auto it = this->objcProtocolOptimizer.swiftDemangledNamesMap.find(*demangledName);
                    assert(it != this->objcProtocolOptimizer.swiftDemangledNamesMap.end());

                    VMOffset demangledNameBufferOffset = it->second;
                    assert(demangledNameBufferOffset.rawValue() < this->objcProtocolOptimizer.swiftDemangledNameStringsChunk->cacheVMSize.rawValue());
                    CacheVMAddress demangleNameVMAddr = this->objcProtocolOptimizer.swiftDemangledNameStringsChunk->cacheVMAddress + demangledNameBufferOffset;

                    protocolNameVMAddr = VMAddress(demangleNameVMAddr.rawValue());
                }
                newProtocol.setDemangledName(objcVisitor, protocolNameVMAddr);
            }
            newProtocol.setFixedUp(objcVisitor);
            newProtocol.setIsCanonical(objcVisitor);

            // Redirect the protocol table at our new object.
            // Note we update all entries as this is a multimap
            auto protocolRange = this->objcProtocolOptimizer.protocols.equal_range(protocolName.data());
            for ( auto it = protocolRange.first; it != protocolRange.second; ++it ) {
                canonicalProtocolMap[VMAddress(it->second.first)] = VMAddress(newProtocolVMAddr);
                it->second.first                                  = newProtocolVMAddr;
            }

            // Add new fixup entries.
            // FIXME: Make this a forEachFixup
            std::vector<void*> fixups;
            newProtocol.addFixups(objcVisitor, fixups);
            for ( void* fixup : fixups )
                this->objcProtocolOptimizer.canonicalProtocolsChunk->tracker.add(fixup);

            newProtocolOffset += newSize;
        });
        assert(foundProtocol);
    }

    // Update all clients to use the new canonical protocols
    // Protocols are referenced by __objc_protorefs, classes, categories, and other protocols.
    // We update all of these references.  But we do NOT update __objc_protolist to point to the new canonical protocols
    // __objc_protolist continues to point to the original protocols, in case the objc runtime needs them
    for ( uint32_t i = 0; i != objcVisitors.size(); ++i ) {
        objc_visitor::Visitor& objcVisitor = objcVisitors[i];

        // Update every protocol reference to point to the canonical protocols
        objcVisitor.forEachProtocolReference(^(metadata_visitor::ResolvedValue& protocolRef) {
            VMAddress protocolVMAddr = objcVisitor.resolveRebase(protocolRef).vmAddress();

            // Find the protocol in the map
            auto it = canonicalProtocolMap.find(protocolVMAddr);
            assert(it != canonicalProtocolMap.end());
            objcVisitor.updateTargetVMAddress(protocolRef, CacheVMAddress(it->second.rawValue()));
        });

        auto visitProtocolList = ^(objc_visitor::ProtocolList objcProtocolList) {
            uint64_t numProtocols = objcProtocolList.numProtocols(objcVisitor);
            for ( uint64_t protocolIndex = 0; protocolIndex != numProtocols; ++protocolIndex ) {
                objc_visitor::Protocol objcProtocol = objcProtocolList.getProtocol(objcVisitor, protocolIndex);

                VMAddress protocolVMAddr = objcProtocol.getVMAddress();

                // Find the protocol in the map
                auto it = canonicalProtocolMap.find(protocolVMAddr);
                // It seems to be ok if the protocol is missing.  On a class for example, both
                // the class and metaclass will refer to the name protocol list, so if we are the metaclass
                // then the class already updated it.
                // We only continue to visit the metaclass as the old code did too, and perhaps its required
                if ( it != canonicalProtocolMap.end() )
                    objcProtocolList.setProtocol(objcVisitor, protocolIndex, it->second);
            }
        };

        // Protocol lists in classes
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            visitProtocolList(objcClass.getBaseProtocols(objcVisitor));
        });

        // Protocol lists in categories
        objcVisitor.forEachCategory(^(const objc_visitor::Category& objcCategory, bool& stopCategory) {
            visitProtocolList(objcCategory.getProtocols(objcVisitor));
        });

        // Protocol lists in protocols
        objcVisitor.forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool& stopProtocol) {
            visitProtocolList(objcProtocol.getProtocols(objcVisitor));
        });
    }

    return Error();
}

Error SharedCacheBuilder::computeObjCClassLayout()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return Error();

    Timer::Scope timedScope(this->config, "computeObjCClassLayout time");

    const bool log = false;

    // We need to walk all classes in all dylibs.  Each dylib needs its own objc visitor object
    std::vector<objc_visitor::Visitor> objcVisitors;
    objcVisitors.reserve(this->cacheDylibs.size());

    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        objcVisitors.push_back(cacheDylib.makeCacheObjCVisitor(config, nullptr, nullptr));
    }

    // Check for missing superclasses, but only error on customer/universal caches
    {
        __block Error error;
        for ( objc_visitor::Visitor& objcVisitor : objcVisitors ) {
            objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
                if ( objcClass.isRootClass(objcVisitor) )
                    return;

                std::optional<VMAddress> superclass = objcClass.getSuperclassVMAddr(objcVisitor);
                if ( !superclass.has_value() ) {
                    if ( this->options.kind == CacheKind::universal )
                        error = Error("Superclass of class '%s' is weak-import"
                                      "and missing.  Referenced in %s",
                                      objcClass.getName(objcVisitor),
                                      objcVisitor.mf()->installName());
                    stopClass = true;
                    this->objcOptimizer.foundMissingWeakSuperclass = true;
                }
            });
            if ( this->objcOptimizer.foundMissingWeakSuperclass )
                break;
        }
        if ( error.hasError() )
            return std::move(error);
    }

    // Walk all classes, starting from root classes, and compute their layout
    struct ClassInfo
    {
        objc_visitor::Visitor *                 objcVisitor = nullptr;
        objc_visitor::Class                     classPos;
        std::vector<ClassInfo*>                 subClasses;
    };
    __block std::vector<ClassInfo> classInfos;
    __block std::unordered_map<VMAddress, uint32_t, VMAddressHash, VMAddressEqual> classMap;
    __block std::unordered_map<VMAddress, uint32_t, VMAddressHash, VMAddressEqual> metaclassMap;

    // First add all the classes to the map
    for ( objc_visitor::Visitor& objcVisitor : objcVisitors ) {
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            VMAddress classVMAddr = objcClass.getVMAddress();
            if ( objcClass.isMetaClass )
                metaclassMap[classVMAddr] = (uint32_t)classInfos.size();
            else
                classMap[classVMAddr] = (uint32_t)classInfos.size();

            if ( log ) {
                printf("%s: [0x%08llx] %s%s\n", objcVisitor.mf()->installName(), classVMAddr.rawValue(),
                       objcClass.getName(objcVisitor), objcClass.isMetaClass ? " (meta)" : "");
            }

            ClassInfo classInfo = { &objcVisitor, objcClass };
            classInfos.push_back(classInfo);
        });
    }

    // Next add all the parent->child links
    for ( objc_visitor::Visitor& objcVisitor : objcVisitors ) {
        objcVisitor.forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool& stopClass) {
            // Don't add parent->child links to root classes. They have no superclass
            if ( objcClass.isRootClass(objcVisitor) )
                return;

            auto& map = objcClass.isMetaClass ? metaclassMap : classMap;

            VMAddress                classVMAddr      = objcClass.getVMAddress();
            std::optional<VMAddress> superclassVMAddr = objcClass.getSuperclassVMAddr(objcVisitor);

            // Skip classes with no superclass
            if ( !superclassVMAddr.has_value() )
                return;

            auto classIt = map.find(classVMAddr);
            assert(classIt != map.end());
            ClassInfo& classInfo = classInfos[classIt->second];

            auto superclassIt = map.find(superclassVMAddr.value());
            assert(superclassIt != map.end());
            ClassInfo& superclassInfo = classInfos[superclassIt->second];

            superclassInfo.subClasses.push_back(&classInfo);
        });
    }

    std::list<ClassInfo*> worklist;

    // Find the root classes
    for ( ClassInfo& classInfo : classInfos ) {
        objc_visitor::Class& objcClass = classInfo.classPos;
        if ( objcClass.isRootClass(*classInfo.objcVisitor) ) {
            // We don't need to add the root classes to the worklist, as they are already done
            // But track them as being processed
            worklist.insert(worklist.end(), classInfo.subClasses.begin(), classInfo.subClasses.end());
        }
    }

    while ( !worklist.empty() ) {
        const ClassInfo* classInfo = worklist.front();
        worklist.pop_front();
        worklist.insert(worklist.end(), classInfo->subClasses.begin(), classInfo->subClasses.end());
        bool elidedSomething = false;
        const objc_visitor::Class& objcClass = classInfo->classPos;

        auto& map = objcClass.isMetaClass ? metaclassMap : classMap;

        std::optional<VMAddress> superclassVMAddr = objcClass.getSuperclassVMAddr(*classInfo->objcVisitor);
        auto                     superclassIt     = map.find(superclassVMAddr.value());
        assert(superclassIt != map.end());
        ClassInfo&             superclassInfo = classInfos[superclassIt->second];
        const objc_visitor::Class& objcSuperClass = superclassInfo.classPos;

        int32_t diff = objcSuperClass.getInstanceSize(*superclassInfo.objcVisitor) - objcClass.getInstanceStart(*classInfo->objcVisitor);
        if ( diff > 0 ) {
            objc_visitor::IVarList ivars = objcClass.getIVars(*classInfo->objcVisitor);
            uint32_t numIVars = ivars.numIVars();

            // Compute max alignment from all the fields
            uint32_t maxAlignment = 1;
            for ( uint32_t i = 0; i != numIVars; ++i ) {
                objc_visitor::IVar ivar = ivars.getIVar(*classInfo->objcVisitor, i);
                uint32_t alignment = ivar.getAlignment(*classInfo->objcVisitor);
                if ( alignment == ~0U )
                    alignment = this->config.layout.is64 ? 8 : 4;
                else
                    alignment = 1 << alignment;
                maxAlignment = std::max(maxAlignment, alignment);
            }

            // Compute a slide value that preserves that alignment
            uint32_t alignMask = maxAlignment - 1;
            if ( diff & alignMask )
                diff = (diff + alignMask) & ~alignMask;

            for ( uint32_t i = 0; i != numIVars; ++i ) {
                objc_visitor::IVar ivar = ivars.getIVar(*classInfo->objcVisitor, i);

                std::optional<uint32_t> offset = ivar.getOffset(*classInfo->objcVisitor);

                // skip anonymous bitfields
                if ( !offset.has_value() )
                    continue;

                // skip ivars that swiftc has optimized away
                if ( ivar.elided(*classInfo->objcVisitor) ) {
                    if ( log ) {
                        if ( !elidedSomething )
                            printf("adjusting ivars for %s\n", objcClass.getName(*classInfo->objcVisitor));
                        elidedSomething = true;
                        printf("  eliding ivar %s\n", ivar.getName(*classInfo->objcVisitor));
                    }
                    continue;
                }

                uint32_t oldOffset = (uint32_t)offset.value();
                uint32_t newOffset = oldOffset + diff;
                ivar.setOffset(*classInfo->objcVisitor, newOffset);
            }

            objcClass.setInstanceStart(*classInfo->objcVisitor, objcClass.getInstanceStart(*classInfo->objcVisitor) + diff);
            objcClass.setInstanceSize(*classInfo->objcVisitor, objcClass.getInstanceSize(*classInfo->objcVisitor) + diff);
        }
    }

    return Error();
}

Error SharedCacheBuilder::emitSwiftHashTables()
{
    if ( this->objcOptimizer.objcDylibs.empty() )
        return Error();

    Timer::Scope timedScope(this->config, "emitSwiftHashTables time");

    // HACK: We know Swift will resolve pointers across dylib boundaries.  The SwiftVisitor
    // requires that it can identify the buffer for every pointer.  It won't resolve to a pointer
    // in our dylib, so we should add all the regions in the cache builder
    std::vector<metadata_visitor::Segment> extraRegions;
    for ( const SubCache& subCache : this->subCaches ) {
        for ( const Region& region : subCache.regions ) {
            if ( !region.needsSharedCacheMapping() )
                continue;
            CacheVMAddress endVMAddr = region.subCacheVMAddress + region.subCacheVMSize;
            metadata_visitor::Segment segment;
            segment.startVMAddr     = VMAddress(region.subCacheVMAddress.rawValue());
            segment.endVMAddr       = VMAddress(endVMAddr.rawValue());
            segment.bufferStart     = region.subCacheBuffer;
            segment.segIndex        = ~0U;
            segment.onDiskDylibChainedPointerFormat = std::nullopt;

            extraRegions.push_back(segment);
        }
    }

    Diagnostics diag;
    auto objcClassOpt = (objc::ClassHashTable*)this->objcClassOptimizer.classHashTableChunk->subCacheBuffer;
    buildSwiftHashTables(this->config, diag, this->cacheDylibs,
                         extraRegions, objcClassOpt,
                         this->objcOptimizer.headerInfoReadOnlyChunk->subCacheBuffer,
                         this->objcOptimizer.headerInfoReadWriteChunk->subCacheBuffer,
                         this->objcOptimizer.headerInfoReadOnlyChunk->cacheVMAddress,
                         this->swiftProtocolConformanceOptimizer);

    if ( diag.hasError() )
        return Error("Couldn't build Swift protocol opts because: %s", diag.errorMessageCStr());

    return Error();
}

void SharedCacheBuilder::emitCacheDylibsTrie()
{
    Timer::Scope timedScope(this->config, "emitCacheDylibsTrie time");

    assert(this->dylibTrieOptimizer.dylibsTrieChunk->subCacheFileSize.rawValue() == this->dylibTrieOptimizer.dylibsTrie.size());

    memcpy(this->dylibTrieOptimizer.dylibsTrieChunk->subCacheBuffer, this->dylibTrieOptimizer.dylibsTrie.data(), this->dylibTrieOptimizer.dylibsTrie.size());
}

void SharedCacheBuilder::computeSlideInfo()
{
    Timer::Scope timedScope(this->config, "computeSlideInfo time");

    if ( !this->config.slideInfo.slideInfoFormat.has_value() ) {
        assert(this->options.isSimultor());
    }

    Error err = parallel::forEach(this->subCaches, ^(size_t index, SubCache& subCache) {
        return subCache.computeSlideInfo(this->config);
    });

    assert(!err.hasError());
}

uint64_t SharedCacheBuilder::getMaxSlide() const
{
    if ( !config.slideInfo.slideInfoFormat.has_value() ) {
        // Simulator caches can't slide
        return 0;
    }

    CacheVMSize maxSlide(~0ULL);
    if ( this->config.layout.discontiguous.has_value() ) {
        // Large x86_64 caches.  All TEXT/DATA/LINKEDIT are on their own 1GB ranges
        // The max slide keeps them within their ranges.
        // TODO: Check if we can just slide these arbitrarily within the VM space,
        // now that thair slid ranges will always be on 1GB boundaries.

        CacheVMSize subCacheLimit(this->config.layout.discontiguous->regionAlignment.value());
        for ( const SubCache& subCache : this->subCaches ) {
            // .symbols files don't contribute to maxSlide
            if ( subCache.isSymbolsCache() )
                continue;

            const Region* firstDataRegion = nullptr;
            const Region* lastDataRegion = nullptr;
            for ( const Region& region : subCache.regions ) {
                switch ( region.kind ) {
                    case Region::Kind::text:
                    case Region::Kind::dynamicConfig:
                    case Region::Kind::linkedit:
                        maxSlide = std::min(maxSlide, subCacheLimit - region.subCacheVMSize);
                        break;
                    case Region::Kind::data:
                    case Region::Kind::dataConst:
                    case Region::Kind::auth:
                    case Region::Kind::authConst:
                        if ( firstDataRegion == nullptr )
                            firstDataRegion = &region;
                        lastDataRegion = &region;
                        break;
                    case Region::Kind::unmapped:
                    case Region::Kind::codeSignature:
                    case Region::Kind::numKinds:
                        break;
                }
            }

            CacheVMAddress startOfData = firstDataRegion->subCacheVMAddress;
            CacheVMAddress endOfData = lastDataRegion->subCacheVMAddress + lastDataRegion->subCacheVMSize;
            CacheVMSize dataRegionSize((endOfData - startOfData).rawValue());
            maxSlide = std::min(maxSlide, subCacheLimit - dataRegionSize);
        }
        return maxSlide.rawValue();
    }

    // We must be a largeContiguous cache. Others were dealt with above in the x86_64 and/or sim cases
    assert(this->config.layout.contiguous.has_value());

    // Start off making sure we can't slide past the end of the cache
    CacheVMAddress maxVMAddress(0ULL);
    for ( const Region& region : this->subCaches.back().regions ) {
        if ( !region.needsSharedCacheReserveAddressSpace() )
            continue;

        CacheVMAddress endOfRegion = region.subCacheVMAddress + region.subCacheVMSize;
        maxVMAddress = std::max(maxVMAddress, endOfRegion);
    }

    CacheVMAddress endOfSharedRegion = this->config.layout.cacheBaseAddress + this->config.layout.cacheSize;
    maxSlide = CacheVMSize((endOfSharedRegion - maxVMAddress).rawValue());

    // <rdar://problem/49852839> branch predictor on arm64 currently only looks at low 32-bits,
    // so try not slide cache more than 2GB
    CacheVMAddress endOfText(0ULL);
    for ( const SubCache& subCache : this->subCaches ) {
        for ( const Region& region : subCache.regions ) {
            if ( region.kind != Region::Kind::text )
                continue;

            endOfText = region.subCacheVMAddress + region.subCacheVMSize;
        }
    }

    const uint64_t twoGB = 0x80000000ULL;
    uint64_t sizeUpToTextEnd = (endOfText - this->config.layout.cacheBaseAddress).rawValue();
    if ( sizeUpToTextEnd <= twoGB )
        maxSlide = CacheVMSize(twoGB - sizeUpToTextEnd);

    return maxSlide.rawValue();
}

void SharedCacheBuilder::addObjcSegments()
{
    Timer::Scope timedScope(this->config, "addObjcSegments time");
    Timer::AggregateTimer aggregateTimerOwner(this->config);
    auto& aggregateTimer = aggregateTimerOwner;

    for ( CacheDylib& cacheDylib : this->cacheDylibs ) {
        Diagnostics diag;
        cacheDylib.addObjcSegments(diag, aggregateTimer,
                                   this->objcOptimizer.headerInfoReadOnlyChunk,
                                   this->objcProtocolOptimizer.protocolHashTableChunk,
                                   this->objcOptimizer.headerInfoReadWriteChunk,
                                   this->objcProtocolOptimizer.canonicalProtocolsChunk);
    }
}

void SharedCacheBuilder::computeCacheHeaders()
{
    Timer::Scope timedScope(this->config, "computeCacheHeaders time");

    for ( SubCache& subCache : this->subCaches )
        subCache.writeCacheHeader(this->options, this->config, this->cacheDylibs);

    // Content for the first (main) subCache only
    __block uint32_t osVersion                  = 0;
    __block uint32_t altPlatform                = 0;
    __block uint32_t altOsVersion               = 0;
    CacheVMAddress   dyldInCacheUnslidAddr      = CacheVMAddress(0ULL);
    CacheVMAddress   dyldInCacheEntryUnslidAddr = CacheVMAddress(0ULL);
    {
        // look for libdyld.dylib and record OS verson info into cache header
        for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
            if ( endsWith(cacheDylib.installName, "/libdyld.dylib") ) {
                cacheDylib.inputMF->forEachSupportedPlatform(^(dyld3::Platform platform, uint32_t minOS, uint32_t sdk) {
                    if ( platform == options.platform ) {
                        osVersion = minOS;
                    }
                    else {
                        altPlatform  = (uint32_t)platform;
                        altOsVersion = minOS;
                    }
                });
            }
            else if ( cacheDylib.installName == "/usr/lib/dyld" ) {
                // record in header where dyld is located in cache
                dyldInCacheUnslidAddr = cacheDylib.cacheLoadAddress;
                uint64_t dyldEntryOffset;
                bool     usesCRT;
                if ( cacheDylib.cacheMF->getEntry(dyldEntryOffset, usesCRT) ) {
                    // the "pc" value in the LC_UNIXTHREAD was adjusted when dyld was placed in the cache
                    dyldInCacheEntryUnslidAddr = dyldInCacheUnslidAddr + VMOffset(dyldEntryOffset);
                }
            }
        }
    }

    // Fill in info for the main caches.  This must be after addCacheHeaderImageInfo().
    for ( SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainCache() )  {
            subCache.addMainCacheHeaderInfo(this->options, this->config,
                                            this->cacheDylibs,
                                            this->totalVMSize, getMaxSlide(),
                                            osVersion, altPlatform, altOsVersion,
                                            dyldInCacheUnslidAddr, dyldInCacheEntryUnslidAddr,
                                            this->dylibTrieOptimizer,
                                            this->objcOptimizer, this->swiftProtocolConformanceOptimizer,
                                            this->patchTableOptimizer, this->prebuiltLoaderBuilder);
            continue;
        }

        if ( subCache.isSymbolsCache() )
            subCache.addSymbolsCacheHeaderInfo(this->unmappedSymbolsOptimizer);
    }
}

void SharedCacheBuilder::codeSign()
{
    Timer::Scope timedScope(this->config, "codeSign time");

    // The first subCache has the UUIDs of all the others in its cache header.
    // We need to compute those first before measuring the first subCache

    // FIXME: Propagate errors
    Diagnostics diag;

    // Note we don't do this in parallel, as we already loop over the pages in parallel
    for ( SubCache& subCache : this->subCaches ) {
        // Skip main caches.  We'll do them later
        if ( subCache.isMainCache() )
            continue;
        subCache.codeSign(diag, this->options, this->config);
        assert(!diag.hasError());
    }

    for ( SubCache& mainSubCache : this->subCaches ) {
        if ( !mainSubCache.isMainCache() )
            continue;

        // Copy UUIDS from sub caches
        Chunk&               mainCacheHeaderChunk   = *mainSubCache.cacheHeader.get();
        dyld_cache_header*   mainCacheHeader        = (dyld_cache_header*)mainCacheHeaderChunk.subCacheBuffer;
        dyld_subcache_entry* subCacheEntries        = (dyld_subcache_entry*)((uint8_t*)mainCacheHeaderChunk.subCacheBuffer + mainCacheHeader->subCacheArrayOffset);

        if ( !mainSubCache.subCaches.empty() ) {
            for ( uint32_t index = 0; index != mainSubCache.subCaches.size(); ++index ) {
                const SubCache* subCache = mainSubCache.subCaches[index];
                assert(subCache->isSubCache() || subCache->isStubsCache());

                const Chunk&             subCacheHeaderChunk   = *subCache->cacheHeader.get();
                const dyld_cache_header* subCacheHeader        = (dyld_cache_header*)subCacheHeaderChunk.subCacheBuffer;
                memcpy(subCacheEntries[index].uuid, subCacheHeader->uuid, sizeof(subCacheHeader->uuid));
            }
        }

        // Add the locals if we have it
        if ( this->options.localSymbolsMode == LocalSymbolsMode::unmap ) {
            for ( SubCache& subCache : this->subCaches ) {
                if ( !subCache.isSymbolsCache() )
                    continue;

                const Chunk&             subCacheHeaderChunk   = *subCache.cacheHeader.get();
                const dyld_cache_header* subCacheHeader        = (dyld_cache_header*)subCacheHeaderChunk.subCacheBuffer;
                memcpy(mainCacheHeader->symbolFileUUID, subCacheHeader->uuid, sizeof(subCacheHeader->uuid));
            }
        }

        // Codesign the main cache now that all its subCaches have been updated in its header
        mainSubCache.codeSign(diag, this->options, this->config);
        assert(!diag.hasError());
    }
}

//
// MARK: --- SharedCacheBuilder other methods ---
//

static const std::string cdHashToString(const uint8_t hash[20])
{
    char buff[48];
    for (int i = 0; i < 20; ++i)
        snprintf(&buff[2*i], sizeof(buff), "%2.2x", hash[i]);
    return buff;
}

void SharedCacheBuilder::getResults(std::vector<CacheBuffer>& results) const
{
    for ( const SubCache& subCache : this->subCaches ) {
        CacheBuffer buffer;
        buffer.bufferData = subCache.buffer;
        buffer.bufferSize = subCache.bufferSize;

        buffer.cdHash = cdHashToString(subCache.cdHash);
        buffer.uuid   = subCache.uuidString;

        buffer.cacheFileSuffix = subCache.fileSuffix;

        buffer.usedByCustomerConfig = subCache.shouldKeepCache(false, true);
        buffer.usedByDevelopmentConfig = subCache.shouldKeepCache(true, false);

        // The builder executable also passes back the fd.  This should typically be used instead of the data buffer
#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
        buffer.fd       = subCache.fd;
        buffer.tempPath = subCache.tempPath;
#endif

        results.push_back(std::move(buffer));
    }
}

std::string SharedCacheBuilder::getMapFileBuffer() const
{
    std::string result;
    result.reserve(256*1024);

    for ( const SubCache& subCache : this->subCaches ) {
        for ( const Region& region : subCache.regions ) {
            const char* prot = "";
            switch ( region.kind ) {
                case Region::Kind::text:
                    prot = "EX";
                    break;
                case Region::Kind::data:
                case Region::Kind::dataConst:
                case Region::Kind::auth:
                case Region::Kind::authConst:
                    prot = "RW";
                    break;
                case Region::Kind::linkedit:
                    prot = "RO";
                    break;
                case Region::Kind::unmapped:
                case Region::Kind::codeSignature:
                case Region::Kind::dynamicConfig:
                case Region::Kind::numKinds:
                    continue;
            }
            uint64_t vmAddr = region.subCacheVMAddress.rawValue();
            uint64_t vmSize = region.subCacheVMSize.rawValue();

            char lineBuffer[256];
            if ( vmSize > 1024*1024 )
                snprintf(lineBuffer, sizeof(lineBuffer), "mapping  %s %4lluMB 0x%0llX -> 0x%0llX\n", prot, vmSize/(1024*1024), vmAddr, vmAddr+vmSize);
            else
                snprintf(lineBuffer, sizeof(lineBuffer), "mapping  %s %4lluKB 0x%0llX -> 0x%0llX\n", prot, vmSize/1024,        vmAddr, vmAddr+vmSize);
            result += lineBuffer;
        }
    }

    // TODO:  add linkedit breakdown
    result += "\n\n";

    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        result += std::string(cacheDylib.installName) + "\n";
        for ( const DylibSegmentChunk& segmentChunk : cacheDylib.segments ) {
            const char* name = segmentChunk.segmentName.data();
            uint64_t vmAddr = segmentChunk.cacheVMAddress.rawValue();
            uint64_t vmSize = segmentChunk.cacheVMSize.rawValue();

            char lineBuffer[256];
            snprintf(lineBuffer, sizeof(lineBuffer), "\t%16s 0x%08llX -> 0x%08llX\n", name, vmAddr, vmAddr + vmSize);
            result += lineBuffer;
        }
        result += "\n";
    }

    return result;
}

// MRM map file generator
std::string SharedCacheBuilder::generateJSONMap(std::string_view disposition,
                                                const SubCache& mainSubCache) const
{
    uint64_t baseAddress = this->config.layout.cacheBaseAddress.rawValue();

    assert(mainSubCache.isMainCache());

    dyld3::json::Node cacheNode;

    cacheNode.map["version"].value = "1";
    cacheNode.map["disposition"].value = disposition;
    cacheNode.map["base-address"].value = dyld3::json::hex(baseAddress);
    cacheNode.map["uuid"].value = mainSubCache.uuidString;

    dyld3::json::Node imagesNode;
    for ( const CacheDylib& cacheDylib : this->cacheDylibs ) {
        dyld3::json::Node imageNode;
        imageNode.map["path"].value = cacheDylib.installName;
        const dyld3::MachOFile* mf = cacheDylib.cacheMF;
        uuid_t uuid;
        if ( mf->getUuid(uuid) ) {
            uuid_string_t uuidStr;
            uuid_unparse(uuid, uuidStr);
            imageNode.map["uuid"].value = uuidStr;
        }

        __block dyld3::json::Node segmentsNode;
        mf->forEachSegment(^(const dyld3::MachOAnalyzer::SegmentInfo &info, bool &stop) {
            dyld3::json::Node segmentNode;
            segmentNode.map["name"].value = info.segName;
            segmentNode.map["start-vmaddr"].value = dyld3::json::hex(info.vmAddr);
            segmentNode.map["end-vmaddr"].value = dyld3::json::hex(info.vmAddr + info.vmSize);

            // Add sections in verbose mode
            segmentsNode.array.push_back(segmentNode);
        });
        imageNode.map["segments"] = segmentsNode;
        imagesNode.array.push_back(imageNode);
    }

    cacheNode.map["images"] = imagesNode;

    std::stringstream stream;
    printJSON(cacheNode, 0, stream);

    return stream.str();
}

std::string SharedCacheBuilder::developmentLoggingPrefix() const
{
    // On universal caches, we need to add the .development to the end of the prefix generated
    // earlier.  In all other cases, the logging prefix is correct
    switch ( this->options.kind ) {
        case CacheKind::development:
            return this->options.logPrefix;
        case CacheKind::universal:
            return this->options.logPrefix + ".development";
    }
}

std::string SharedCacheBuilder::customerLoggingPrefix() const
{
    // The customer logging prefix is already correct on all cache kinds
    return this->options.logPrefix;
}

std::string SharedCacheBuilder::developmentJSONMap(std::string_view disposition) const
{
    for ( const SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainDevelopmentCache() )
            return this->generateJSONMap(disposition, subCache);
    }

    assert("Expected main dev cache");
    return "";
}

std::optional<std::string> SharedCacheBuilder::customerJSONMap(std::string_view disposition) const
{
    for ( const SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainCustomerCache() )
            return this->generateJSONMap(disposition, subCache);
    }

    return std::nullopt;
}

std::string SharedCacheBuilder::developmentCacheUUID() const
{
    for ( const SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainDevelopmentCache() )
            return subCache.uuidString;
    }

    assert("Expected main dev cache");
    return "";
}

std::optional<std::string> SharedCacheBuilder::customerCacheUUID() const
{
    for ( const SubCache& subCache : this->subCaches ) {
        if ( subCache.isMainCustomerCache() )
            return subCache.uuidString;
    }

    return std::nullopt;
}

void SharedCacheBuilder::warning(const char *format, ...)
{
    va_list list;
    va_start(list, format);
    void* buffer = _simple_salloc();
    _simple_vsprintf(buffer, format, list);
    this->warnings.push_back((const char*)buffer);
    _simple_sfree(buffer);
    va_end(list);

}

__attribute__((used))
void SharedCacheBuilder::debug(const char* installName) const
{
    for ( const CacheDylib& dylib : this->cacheDylibs ) {
        if ( dylib.installName == installName ) {
            fprintf(stderr, "Found %s\n", installName);
            for ( const DylibSegmentChunk& segment : dylib.segments ) {
                fprintf(stderr, "%16s, VM 0x%llx -> 0x%llx, file 0x%llx -> 0x%llx\n",
                        segment.segmentName.data(),
                        segment.cacheVMAddress.rawValue(), segment.cacheVMAddress.rawValue() + segment.cacheVMSize.rawValue(),
                        segment.subCacheFileOffset.rawValue(), segment.subCacheFileOffset.rawValue() + segment.inputFileSize.rawValue());
            }
            return;
        }
    }

    fprintf(stderr, "Didn't find a dylib with install name: %s\n", installName);
}
