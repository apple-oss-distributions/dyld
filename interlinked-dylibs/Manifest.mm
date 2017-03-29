//
//  Manifest.mm
//  dyld
//
//  Created by Louis Gerbarg on 7/23/15.
//
//

#if BOM_SUPPORT
extern "C" {
#include <Bom/Bom.h>
#include <Metabom/MBTypes.h>
#include <Metabom/MBEntry.h>
#include <Metabom/MBMetabom.h>
#include <Metabom/MBIterator.h>
};
#endif /* BOM_SUPPORT */

#include <algorithm>

#include <Foundation/Foundation.h>
#include <rootless.h>

#include "MachOFileAbstraction.hpp"
#include "FileAbstraction.hpp"
#include "Trie.hpp"
#include "Logging.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>

#include <array>
#include <vector>

#include "dsc_iterator.h"
#include "MachOProxy.h"
#include "mega-dylib-utils.h"

#include "Manifest.h"

namespace {
//FIXME this should be in a class
static bool rootless = true;
static inline NSString* cppToObjStr(const std::string& str) { return [NSString stringWithUTF8String:str.c_str()]; }

std::string fileExists(const std::string& path)
{
    const uint8_t* p = (uint8_t*)(-1);
    struct stat    stat_buf;

    std::tie(p, stat_buf, rootless) = fileCache.cacheLoad(path);
    if (p != (uint8_t*)(-1)) {
        return normalize_absolute_file_path(path);
    }

    return "";
}

} /* Anonymous namespace */

void Manifest::Results::exclude(MachOProxy* proxy, const std::string& reason)
{
    dylibs[proxy->identifier].uuid = proxy->uuid;
    dylibs[proxy->identifier].installname = proxy->installName;
    dylibs[proxy->identifier].included = false;
    dylibs[proxy->identifier].exclusionInfo = reason;
}

Manifest::Manifest(const std::set<std::string>& archs, const std::string& overlayPath, const std::string& rootPath, const std::set<std::string>& paths)
{
    std::set<std::string> processedPaths;
    std::set<std::string> unprocessedPaths = paths;
    std::set<std::string> pathsToProcess;
    std::set_difference(unprocessedPaths.begin(), unprocessedPaths.end(), processedPaths.begin(), processedPaths.end(),
        std::inserter(pathsToProcess, pathsToProcess.begin()));
    while (!pathsToProcess.empty()) {
        for (const std::string path : pathsToProcess) {
            processedPaths.insert(path);
            std::string fullPath;
            if (rootPath != "/") {
                // with -root, only look in the root path volume
                fullPath = fileExists(rootPath + path);
            } else {
                // with -overlay, look first in overlay dir
                if (!overlayPath.empty())
                    fullPath = fileExists(overlayPath + path);
                // if not in overlay, look in boot volume
                if (fullPath.empty())
                    fullPath = fileExists(path);
            }
            if (fullPath.empty())
                continue;
            auto proxies = MachOProxy::loadProxies(fullPath, path);

            for (const auto& arch : archs) {
                auto proxyI = proxies.find(arch);
                if (proxyI == proxies.end())
                    proxyI = proxies.find(fallbackArchStringForArchString(arch));
                if (proxyI == proxies.end())
                    continue;

                auto dependecies = proxyI->second->dependencies();
                for (const auto& dependency : dependecies) {
                    unprocessedPaths.insert(dependency);
                }
                _configurations["localhost"].architectures[arch].anchors.push_back(proxyI->second->identifier);
            }

            //Stuff
        }

        pathsToProcess.clear();
        std::set_difference(unprocessedPaths.begin(), unprocessedPaths.end(), processedPaths.begin(), processedPaths.end(),
            std::inserter(pathsToProcess, pathsToProcess.begin()));
    }
    MachOProxy::mapDependencies();
}

#if BOM_SUPPORT

Manifest::Manifest(const std::string& path)
    : Manifest(path, std::set<std::string>())
{
}

Manifest::Manifest(const std::string& path, const std::set<std::string>& overlays)
{
    NSMutableDictionary* manifestDict = [NSMutableDictionary dictionaryWithContentsOfFile:cppToObjStr(path)];
    std::map<std::string, std::string>           metabomTagMap;
    std::map<std::string, std::set<std::string>> metabomExcludeTagMap;
    std::map<std::string, std::set<std::string>> metabomRestrictedTagMap;
    std::vector<std::pair<std::string, MachOProxy*>> configProxies;

    setMetabomFile([manifestDict[@"metabomFile"] UTF8String]);

    for (NSString* project in manifestDict[@"projects"]) {
        for (NSString* source in manifestDict[@"projects"][project]) {
            addProjectSource([project UTF8String], [source UTF8String]);
        }
    }

    for (NSString* configuration in manifestDict[@"configurations"]) {
        std::string configStr = [configuration UTF8String];
        std::string configTag = [manifestDict[@"configurations"][configuration][@"metabomTag"] UTF8String];
        metabomTagMap[configTag] = configStr;

        if (manifestDict[@"configurations"][configuration][@"metabomExcludeTags"]) {
            for (NSString* excludeTag in manifestDict[@"configurations"][configuration][@"metabomExcludeTags"]) {
                metabomExcludeTagMap[configStr].insert([excludeTag UTF8String]);
                _configurations[configStr].metabomExcludeTags.insert([excludeTag UTF8String]);
            }
        }

        if (manifestDict[@"configurations"][configuration][@"metabomRestrictTags"]) {
            for (NSString* restrictTag in manifestDict[@"configurations"][configuration][@"metabomRestrictTags"]) {
                metabomRestrictedTagMap[configStr].insert([restrictTag UTF8String]);
                _configurations[configStr].metabomRestrictTags.insert([restrictTag UTF8String]);
            }
        }

        _configurations[configStr].metabomTag = configTag;
        _configurations[configStr].platformName =
            [manifestDict[@"configurations"][configuration][@"platformName"] UTF8String];
    }

    setVersion([manifestDict[@"manifest-version"] unsignedIntValue]);
    setBuild([manifestDict[@"build"] UTF8String]);
    if (manifestDict[@"dylibOrderFile"]) {
        setDylibOrderFile([manifestDict[@"dylibOrderFile"] UTF8String]);
    }
    if (manifestDict[@"dirtyDataOrderFile"]) {
        setDirtyDataOrderFile([manifestDict[@"dirtyDataOrderFile"] UTF8String]);
    }

    auto    metabom = MBMetabomOpen(metabomFile().c_str(), false);
    auto    metabomEnumerator = MBIteratorNewWithPath(metabom, ".", "");
    MBEntry entry;

    auto bomSemaphore = dispatch_semaphore_create(32);
    auto bomGroup = dispatch_group_create();
    auto bomQueue = dispatch_queue_create("com.apple.dyld.cache.metabom.bom", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    auto archQueue = dispatch_queue_create("com.apple.dyld.cache.metabom.arch", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    auto manifestQueue = dispatch_queue_create("com.apple.dyld.cache.metabom.arch", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));

    // FIXME error handling (NULL metabom)

    //First we iterate through the bom and build our objects

    while ((entry = MBIteratorNext(metabomEnumerator))) {
        dispatch_semaphore_wait(bomSemaphore, DISPATCH_TIME_FOREVER);
        cacheBuilderDispatchGroupAsync(bomGroup, manifestQueue, [this, &bomSemaphore, &archQueue, &bomGroup, &bomQueue, &metabom, entry, &overlays, &metabomTagMap, &metabomRestrictedTagMap, &metabomExcludeTagMap, &manifestDict, &configProxies] {
            BOMFSObject  fsObject = nullptr;
            std::string  entryPath;
            BOMFSObjType entryType;
            cacheBuilderDispatchSync(bomQueue, [&entry, &fsObject, &entryPath, &entryType] {
                fsObject = MBEntryGetFSObject(entry);
                entryPath = BOMFSObjectPathName(fsObject);
                if (entryPath[0] == '.') {
                    entryPath.erase(0, 1);
                }
                entryType = BOMFSObjectType(fsObject);
            });

            MBTag tag;
            auto  tagCount = MBEntryGetNumberOfProjectTags(entry);
            if ( entryType == BOMFileType && BOMFSObjectIsBinaryObject(fsObject) && MBEntryGetNumberOfProjectTags(entry) != 0 && tagCount != 0 ) {
                if (tagCount == 1) {
                    MBEntryGetProjectTags(entry, &tag);
                } else {
                    MBTag* tags = (MBTag*)malloc(sizeof(MBTag) * tagCount);
                    MBEntryGetProjectTags(entry, tags);

                    //Sigh, we can have duplicate entries for the same tag, so build a set to work with
                    std::set<std::string> tagStrs;
                    std::map<std::string, MBTag> tagStrMap;
                    for (auto i = 0; i < tagCount; ++i) {
                        cacheBuilderDispatchSync(bomQueue, [i, &metabom, &tagStrs, &tagStrMap, &tags] {
                            tagStrs.insert(MBMetabomGetProjectForTag(metabom, tags[i]));
                            tagStrMap.insert(std::make_pair(MBMetabomGetProjectForTag(metabom, tags[i]), tags[i]));
                        });
                    }

                    if (tagStrs.size() > 1) {
                        std::string projects;
                        for (const auto& tagStr : tagStrs) {
                            if (!projects.empty())
                                projects += ", ";

                            projects += "'" + tagStr + "'";
                        }
                        warning("Bom entry '%s' is claimed by multiple projects: %s, taking first entry", entryPath.c_str(), projects.c_str());
                    }
                    tag = tagStrMap[*tagStrs.begin()];
                    free(tags);
                }

                std::string projectName;
                cacheBuilderDispatchSync(bomQueue, [&projectName, &metabom, &tag] {
                    projectName = MBMetabomGetProjectForTag(metabom, tag);
                });

                std::map<std::string, MachOProxy*> proxies;
                for (const auto& overlay : overlays) {
                    proxies = MachOProxy::loadProxies(overlay + "/" + entryPath, entryPath);
                    if (proxies.size() > 0)
                        break;
                }

                if (proxies.size() == 0) {
                    proxies = MachOProxy::loadProxies(projectPath(projectName) + "/" + entryPath, entryPath);
                }

                tagCount = MBEntryGetNumberOfPackageTags(entry);
                MBTag* tags = (MBTag*)malloc(sizeof(MBTag) * tagCount);
                MBEntryGetPackageTags(entry, tags);
                std::set<std::string> tagStrs;

                cacheBuilderDispatchSync(bomQueue, [&] {
                    for (auto i = 0; i < tagCount; ++i) {
                        tagStrs.insert(MBMetabomGetPackageForTag(metabom, tags[i]));
                    }
                });

                for (auto& proxy : proxies) {
                    for (const auto& tagStr : tagStrs) {
                        // Does the configuration exist
                        auto configuration = metabomTagMap.find(tagStr);
                        if (configuration == metabomTagMap.end())
                            continue;
                        auto restrictions = metabomRestrictedTagMap.find(configuration->second);
                        if (restrictions != metabomRestrictedTagMap.end() && !is_disjoint(restrictions->second, tagStrs)) {
                            _configurations[configuration->second].restrictedInstallnames.insert(proxy.second->installName);
                        }
                        // Is the configuration excluded
                        auto exclusions = metabomExcludeTagMap.find(configuration->second);
                        if (exclusions != metabomExcludeTagMap.end() && !is_disjoint(exclusions->second, tagStrs)) {
                            continue;
                        }
                        cacheBuilderDispatchGroupAsync(bomGroup, archQueue, [this, &manifestDict, &configProxies, configuration, proxy, tagStr] {
                            if ([manifestDict[@"configurations"][cppToObjStr(configuration->second)][@"architectures"]
                                    containsObject:cppToObjStr(proxy.second->arch)]) {
                                _configurations[configuration->second].architectures[proxy.second->arch].anchors.push_back(proxy.second->identifier);
                            }
                        });
                    }
                }
            }
            dispatch_semaphore_signal(bomSemaphore);
        });
    }

    dispatch_group_wait(bomGroup, DISPATCH_TIME_FOREVER);
    MBIteratorFree(metabomEnumerator);
    MBMetabomFree(metabom);
    MachOProxy::mapDependencies();
}

#endif

template <typename P>
bool checkLink(MachOProxy* proxy, const uint8_t* p, const uint8_t* end)
{
    bool                     retval = true;
    std::vector<std::string> dylibs = proxy->dependencies();

    std::string symbolName;
    int         libraryOrdinal = 0;
    bool        weakImport = false;
    bool        done = false;

    while (!done && (p < end)) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                done = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                libraryOrdinal = immediate;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                libraryOrdinal = (int)read_uleb128(p, end);
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // the special ordinals are negative numbers
                if (immediate == 0)
                    libraryOrdinal = 0;
                else {
                    int8_t signExtended = BIND_OPCODE_MASK | immediate;
                    libraryOrdinal = signExtended;
                }
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ((immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0);
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                (void)read_sleb128(p, end);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            case BIND_OPCODE_ADD_ADDR_ULEB:
                (void)read_uleb128(p, end);
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                (void)read_uleb128(p, end);
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                (void)read_uleb128(p, end);
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            case BIND_OPCODE_DO_BIND: {
                if (libraryOrdinal <= 0)
                    break;
                if ( libraryOrdinal > dylibs.size() ) {
                    warning("Illegal library ordinal (%d) in dylib %s bind opcode (max ordinal %lu)", libraryOrdinal, proxy->path.c_str(), dylibs.size());
                    retval = false;
                }
                else {
                    auto dependencyProxy = MachOProxy::forInstallnameAndArch(dylibs[libraryOrdinal - 1], proxy->arch);
                    if (!weakImport && (!dependencyProxy || !dependencyProxy->providesSymbol(symbolName))) {
                        warning("Could not find symbol %s in dylib %s for %s", symbolName.c_str(), dylibs[libraryOrdinal - 1].c_str(), proxy->path.c_str());
                        retval = false;
                    }
                }
            } break;
            default:
                warning("bad bind opcode in binary 0x%02X in %s", *p, proxy->path.c_str());
        }
    }

    return retval;
}

bool checkLink(MachOProxy* proxy)
{
    switch (archForString(proxy->arch).arch) {
        case CPU_TYPE_ARM:
        case CPU_TYPE_I386:
            return (checkLink<Pointer32<LittleEndian>>(proxy, proxy->getBindStart(), proxy->getBindEnd())
                    && checkLink<Pointer32<LittleEndian>>(proxy, proxy->getLazyBindStart(), proxy->getLazyBindEnd()));
        case CPU_TYPE_ARM64:
        case CPU_TYPE_X86_64:
            return (checkLink<Pointer64<LittleEndian>>(proxy, proxy->getBindStart(), proxy->getBindEnd())
                    && checkLink<Pointer64<LittleEndian>>(proxy, proxy->getLazyBindStart(), proxy->getLazyBindEnd()));
        default:
            terminate("unsupported arch 0x%08X", archForString(proxy->arch).arch);
    }
}

bool Manifest::checkLinks()
{
    dispatch_queue_t     linkCheckQueue = dispatch_get_global_queue(QOS_CLASS_DEFAULT, NULL);
    dispatch_semaphore_t linkCheckSemphore = dispatch_semaphore_create(32);

    dispatch_group_t linkCheckGroup = dispatch_group_create();

    runConcurrently(linkCheckQueue, linkCheckSemphore, [this](const std::string configuration, const std::string architecture) {
        for (const auto& anchor : this->configuration(configuration).architecture(architecture).anchors) {
            const auto identifier = anchor.identifier;
            const auto proxy = MachOProxy::forIdentifier(identifier, architecture);
            if (proxy->isExecutable()) {
                checkLink(proxy);
            }
        }
    });

    dispatch_group_wait(linkCheckGroup, DISPATCH_TIME_FOREVER);

    return true;
}

void Manifest::runConcurrently(dispatch_queue_t queue, dispatch_semaphore_t concurrencyLimitingSemaphore, std::function<void(const std::string configuration, const std::string architecture)> lambda)
{
    dispatch_group_t runGroup = dispatch_group_create();
    for (auto& config : _configurations) {
        for (auto& architecture : config.second.architectures) {
            dispatch_semaphore_wait(concurrencyLimitingSemaphore, DISPATCH_TIME_FOREVER);
            cacheBuilderDispatchGroupAsync(runGroup, queue, [&] {
                WarningTargets targets;
                targets.first = this;
                targets.second.insert(std::make_pair(config.first, architecture.first));
                auto ctx = std::make_shared<LoggingContext>(config.first + "/" + architecture.first, targets);
                setLoggingContext(ctx);
                lambda(config.first, architecture.first);
                dispatch_semaphore_signal(concurrencyLimitingSemaphore);
            });
        }
    }

    dispatch_group_wait(runGroup, DISPATCH_TIME_FOREVER);
}

bool Manifest::filterForConfig(const std::string& configName)
{
    for (const auto configuration : _configurations) {
        if (configName == configuration.first) {
            std::map<std::string, Configuration> filteredConfigs;
            filteredConfigs[configName] = configuration.second;

            _configurations = filteredConfigs;

            for (auto& arch : configuration.second.architectures) {
                arch.second.results = Manifest::Results();
            }
            return true;
        }
    }
    return false;
}

void Manifest::calculateClosure(bool enforceRootless)
{
    auto closureSemaphore = dispatch_semaphore_create(32);
    auto closureGroup = dispatch_group_create();
    auto closureQueue = dispatch_queue_create("com.apple.dyld.cache.closure", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0));
    rootless = enforceRootless;

    for (auto& config : _configurations) {
        for (auto& arch : config.second.architectures) {
            dispatch_semaphore_wait(closureSemaphore, DISPATCH_TIME_FOREVER);
            cacheBuilderDispatchGroupAsync(closureGroup, closureQueue, [&] {
                calculateClosure(config.first, arch.first);
                dispatch_semaphore_signal(closureSemaphore);
            });
        }
    }

    dispatch_group_wait(closureGroup, DISPATCH_TIME_FOREVER);
}

void Manifest::remove(const std::string& config, const std::string& arch)
{
    if (_configurations.count(config))
        _configurations[config].architectures.erase(arch);
}

bool
Manifest::sameContentsAsCacheAtPath(const std::string& configuration, const std::string& architecture, const std::string& path) const {
    std::set<std::pair<std::string, UUID>> cacheDylibs;
    std::set<std::pair<std::string, UUID>> manifestDylibs;
    struct stat statbuf;
    if (::stat(path.c_str(), &statbuf) == -1) {
        // <rdar://problem/25912438> don't warn if there is no existing cache file
        if (errno != ENOENT)
            warning("stat() failed for dyld shared cache at %s, errno=%d", path.c_str(), errno);
        return false;
	}

	int cache_fd = ::open(path.c_str(), O_RDONLY);
	if ( cache_fd < 0 ) {
		warning("open() failed for shared cache file at %s, errno=%d", path.c_str(), errno);
		return false;
	}

	const void *mappedCache = ::mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
	if (mappedCache == MAP_FAILED) {
		::close(cache_fd);
			warning("mmap() for shared cache at %s failed, errno=%d", path.c_str(), errno);
			return false;
	}
	::close(cache_fd);

        if (_configurations.count(configuration) == 0
            || _configurations.find(configuration)->second.architectures.count(architecture) == 0)
            return false;

        Architecture existingArch;
        (void)dyld_shared_cache_iterate(mappedCache, (uint32_t)statbuf.st_size,
            [&existingArch, &architecture](const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo) {
                UUID      uuid = *dylibInfo->uuid;
                DylibInfo info;
                info.uuid = uuid;
                existingArch.results.dylibs[ImageIdentifier(uuid)] = info;
            });

        return (existingArch == _configurations.find(configuration)->second.architectures.find(architecture)->second);
}

void Manifest::removeDylib(MachOProxy* proxy, const std::string& reason, const std::string& configuration,
    const std::string& architecture, std::unordered_set<ImageIdentifier>& processedIdentifiers)
{
    auto configIter = _configurations.find(configuration);
    if (configIter == _configurations.end())
        return;
    auto archIter = configIter->second.architectures.find( architecture );
    if ( archIter == configIter->second.architectures.end() ) return;
    auto& archManifest = archIter->second;

    if (archManifest.results.dylibs.count(proxy->identifier) == 0) {
        archManifest.results.dylibs[proxy->identifier].uuid = proxy->uuid;
        archManifest.results.dylibs[proxy->identifier].installname = proxy->installName;
        processedIdentifiers.insert(proxy->identifier);
    }
    archManifest.results.exclude(MachOProxy::forIdentifier(proxy->identifier, architecture), reason);

    processedIdentifiers.insert(proxy->identifier);

    for (const auto& dependent : proxy->dependentIdentifiers) {
        auto dependentProxy = MachOProxy::forIdentifier(dependent, architecture);
        auto dependentResultIter = archManifest.results.dylibs.find(dependentProxy->identifier);
        if ( dependentProxy &&
             ( dependentResultIter == archManifest.results.dylibs.end() || dependentResultIter->second.included == true ) ) {
            removeDylib(dependentProxy, "Missing dependency: " + proxy->installName, configuration, architecture,
                processedIdentifiers);
        }
    }
}

MachOProxy* Manifest::removeLargestLeafDylib( const std::string& configuration, const std::string& architecture ) {
    std::set<ImageIdentifier> activeIdentifiers;
    std::set<MachOProxy*> leafDylibs;

    auto configIter = _configurations.find(configuration);
    if (configIter == _configurations.end())
        terminate("Internal error");
    ;
    auto archIter = configIter->second.architectures.find( architecture );
    if ( archIter == configIter->second.architectures.end() ) terminate( "Internal error" );
    ;
    for ( const auto& dylibInfo : archIter->second.results.dylibs ) {
        if ( dylibInfo.second.included ) {
            activeIdentifiers.insert(dylibInfo.first);
        }
    }
    for (const auto& identifier : activeIdentifiers) {
        auto dylib = MachOProxy::forIdentifier(identifier, architecture);
        bool dependents = false;
        for (const auto& depedent : dylib->dependentIdentifiers) {
            if (depedent != identifier && activeIdentifiers.count(depedent)) {
                dependents = true;
                break;
            }
        }
        if ( !dependents ) {
            leafDylibs.insert( dylib );
        }
    }
    if ( leafDylibs.empty() ) {
        terminate( "No leaf dylibs to evict" );
    }
    MachOProxy* largestLeafDylib = nullptr;
    for ( const auto& dylib : leafDylibs ) {
        if ( largestLeafDylib == nullptr || dylib->fileSize > largestLeafDylib->fileSize ) {
            largestLeafDylib = dylib;
        }
    }
    std::unordered_set<ImageIdentifier> empty;
    removeDylib( largestLeafDylib, "VM space overflow", configuration, architecture, empty );
    return largestLeafDylib;
}

void Manifest::calculateClosure( const std::string& configuration, const std::string& architecture ) {
    auto&                               archManifest = _configurations[configuration].architectures[architecture];
    std::unordered_set<ImageIdentifier> newIdentifiers;

    for ( auto& anchor : archManifest.anchors ) {
        newIdentifiers.insert(anchor.identifier);
    }

    std::unordered_set<ImageIdentifier> processedIdentifiers;

    while (!newIdentifiers.empty()) {
        std::unordered_set<ImageIdentifier> identifiersToProcess = newIdentifiers;
        newIdentifiers.clear();

        for (const auto& identifier : identifiersToProcess) {
            if (processedIdentifiers.count(identifier) > 0) {
                continue;
            }

            auto proxy = MachOProxy::forIdentifier(identifier, architecture);

            if (proxy == nullptr) {
                // No path
                continue;
            }

            // HACK: This is a policy decision we may want to revisit.
            if (!proxy->isDylib()) {
                continue;
            }

            if (!proxy->error.empty()) {
                archManifest.results.exclude(proxy, proxy->error);
                processedIdentifiers.insert(proxy->identifier);
                continue;
            }

            if (proxy->isDylib()) {
                if (_configurations[configuration].restrictedInstallnames.count(proxy->installName) != 0) {
                    removeDylib(proxy, "Dylib '" + proxy->installName + "' removed due to explict restriction", configuration, architecture,
                        processedIdentifiers);
                    continue;
                }

                if (archManifest.results.dylibs.count(proxy->identifier) == 0) {
                    archManifest.results.dylibs[proxy->identifier].uuid = proxy->uuid;
                    archManifest.results.dylibs[proxy->identifier].installname = proxy->installName;
                    archManifest.results.dylibs[proxy->identifier].included = true;

                    processedIdentifiers.insert(proxy->identifier);
                }
            }

            for (const auto& dependency : proxy->requiredIdentifiers) {
                if (processedIdentifiers.count(dependency) == 0) {
                    newIdentifiers.insert(dependency);
                }
            }
        }
	}
}

void Manifest::write( const std::string& path ) {
    if ( path.empty() ) return;

    NSMutableDictionary* cacheDict = [[NSMutableDictionary alloc] init];
    NSMutableDictionary* projectDict = [[NSMutableDictionary alloc] init];
    NSMutableDictionary* configurationsDict = [[NSMutableDictionary alloc] init];
    NSMutableDictionary* resultsDict = [[NSMutableDictionary alloc] init];

    cacheDict[@"manifest-version"] = @(version());
    cacheDict[@"build"] = cppToObjStr(build());
    cacheDict[@"dylibOrderFile"] = cppToObjStr(dylibOrderFile());
    cacheDict[@"dirtyDataOrderFile"] = cppToObjStr(dirtyDataOrderFile());
    cacheDict[@"metabomFile"] = cppToObjStr(metabomFile());

    cacheDict[@"projects"] = projectDict;
    cacheDict[@"results"] = resultsDict;
    cacheDict[@"configurations"] = configurationsDict;

    for (const auto& project : projects()) {
        NSMutableArray* sources = [[NSMutableArray alloc] init];

        for ( const auto& source : project.second.sources ) {
            [sources addObject:cppToObjStr( source )];
        }

        projectDict[cppToObjStr( project.first )] = sources;
    }

    for (auto& configuration : _configurations) {
        NSMutableArray* archArray = [[NSMutableArray alloc] init];
        for ( auto& arch : configuration.second.architectures ) {
            [archArray addObject:cppToObjStr( arch.first )];
        }

        NSMutableArray* excludeTags = [[NSMutableArray alloc] init];
        for ( const auto& excludeTag : configuration.second.metabomExcludeTags ) {
            [excludeTags addObject:cppToObjStr( excludeTag )];
        }

        configurationsDict[cppToObjStr( configuration.first )] = @{
            @"platformName" : cppToObjStr( configuration.second.platformName ),
            @"metabomTag" : cppToObjStr( configuration.second.metabomTag ),
            @"metabomExcludeTags" : excludeTags,
            @"architectures" : archArray
        };
    }

    for (auto& configuration : _configurations) {
        NSMutableDictionary* archResultsDict = [[NSMutableDictionary alloc] init];
        for ( auto& arch : configuration.second.architectures ) {
            NSMutableDictionary* dylibsDict = [[NSMutableDictionary alloc] init];
            NSMutableArray* warningsArray = [[NSMutableArray alloc] init];
            NSMutableDictionary* devRegionsDict = [[NSMutableDictionary alloc] init];
            NSMutableDictionary* prodRegionsDict = [[NSMutableDictionary alloc] init];
                        NSString *prodCDHash = cppToObjStr(arch.second.results.productionCache.cdHash);
			NSString *devCDHash = cppToObjStr(arch.second.results.developmentCache.cdHash);

            for ( auto& dylib : arch.second.results.dylibs ) {
                NSMutableDictionary* dylibDict = [[NSMutableDictionary alloc] init];
                if ( dylib.second.included ) {
                    NSMutableDictionary* segments = [[NSMutableDictionary alloc] init];
                    dylibDict[@"included"] = @YES;
                    for ( auto& segment : dylib.second.segments ) {
                        segments[cppToObjStr( segment.name )] =
                            @{ @"startAddr" : @( segment.startAddr ),
                               @"endAddr" : @( segment.endAddr ) };
                    }
                    dylibDict[@"segments"] = segments;
                } else {
                    dylibDict[@"included"] = @NO;
                    dylibDict[@"exclusionInfo"] = cppToObjStr(dylib.second.exclusionInfo);
                }
                dylibsDict[cppToObjStr( dylib.second.installname )] = dylibDict;
            }

            for ( auto& region : arch.second.results.developmentCache.regions ) {
                devRegionsDict[cppToObjStr( region.name )] =
                    @{ @"startAddr" : @( region.startAddr ),
                       @"endAddr" : @( region.endAddr ) };
            }

            for ( auto& region : arch.second.results.productionCache.regions ) {
                prodRegionsDict[cppToObjStr( region.name )] =
                    @{ @"startAddr" : @( region.startAddr ),
                       @"endAddr" : @( region.endAddr ) };
            }

            for ( auto& warning : arch.second.results.warnings ) {
                [warningsArray addObject:cppToObjStr( warning )];
            }

            BOOL built = arch.second.results.failure.empty();
            archResultsDict[cppToObjStr( arch.first )] = @{
                @"dylibs" : dylibsDict,
                @"built" : @( built ),
                @"failure" : cppToObjStr( arch.second.results.failure ),
                @"productionCache" : @{@"cdhash" : prodCDHash, @"regions" : prodRegionsDict},
                @"developmentCache" : @{@"cdhash" : devCDHash, @"regions" : devRegionsDict},
                @"warnings" : warningsArray
            };
        }
        resultsDict[cppToObjStr( configuration.first )] = archResultsDict;
    }

    NSError* error = nil;
        NSData *outData = [NSPropertyListSerialization dataWithPropertyList:cacheDict
																 format:NSPropertyListBinaryFormat_v1_0
																options:0
																  error:&error];
	(void)[outData writeToFile:cppToObjStr(path) atomically:YES];
}
