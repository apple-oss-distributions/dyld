//
//  Manifest.h
//  dyld
//
//  Created by Louis Gerbarg on 7/23/15.
//
//

#ifndef Manifest_h
#define Manifest_h

#include <map>
#include <set>
#include <string>
#include <vector>

#include <unordered_map>
#include <unordered_set>

#include <assert.h>

struct MachOProxy;

extern void terminate(const char* format, ...) __printflike(1, 2) __attribute__((noreturn));
extern std::string toolDir();

struct SharedCache;
struct Manifest;

struct Manifest {
    struct Project {
        std::vector<std::string> sources;
    };

    struct File {
        MachOProxy* proxy;

        File( MachOProxy* P ) : proxy( P ) {}
    };

    struct Anchor {
        ImageIdentifier identifier;
		bool required;
        Anchor( const ImageIdentifier& I ) : identifier( I ) {}
    };

    struct SegmentInfo {
        std::string name;
        uint64_t    startAddr;
        uint64_t    endAddr;
	};

	struct SegmentInfoHasher {
		std::size_t operator()(const SegmentInfo &x) const {
			return std::hash<std::string>()(x.name) ^ std::hash<uint64_t>()(x.startAddr) ^ std::hash<uint64_t>()(x.endAddr);
		}
	};

	struct CacheInfo {
		std::vector<SegmentInfo> regions;
		std::string cdHash;
	};

	struct DylibInfo {
		bool included;
		std::string exclusionInfo;
		UUID uuid;
        std::string installname;
		std::vector<SegmentInfo> segments;
		DylibInfo(void) : included(true) {}
	};

	struct Results {
        std::string                             failure;
        std::map<ImageIdentifier, DylibInfo>    dylibs;
        std::vector<std::string>                warnings;
        CacheInfo                               developmentCache;
        CacheInfo                               productionCache;
        DylibInfo& dylibForInstallname(const std::string& installname)
        {
            auto i = find_if(dylibs.begin(), dylibs.end(), [&installname](std::pair<ImageIdentifier, DylibInfo> d) { return d.second.installname == installname; });
            assert(i != dylibs.end());
            return i->second;
        }
        void exclude(MachOProxy* proxy, const std::string& reason);
    };

	struct Architecture {
		std::vector<Anchor> anchors;
		mutable Results results;

        bool operator==(const Architecture& O) const
        {
            for (auto& dylib : results.dylibs) {
                if (dylib.second.included) {
                    auto Odylib = O.results.dylibs.find(dylib.first);
                    if (Odylib == O.results.dylibs.end()
                        || Odylib->second.included == false
                        || Odylib->second.uuid != dylib.second.uuid)
                        return false;
                }
            }

            for (const auto& Odylib : O.results.dylibs) {
                if (Odylib.second.included) {
                    auto dylib = results.dylibs.find(Odylib.first);
                    if (dylib == results.dylibs.end()
                        || dylib->second.included == false
                        || dylib->second.uuid != Odylib.second.uuid)
                        return false;
                }
            }

            return true;
        }

        bool operator!=(const Architecture& other) const { return !(*this == other); }
    };

	struct Configuration {
        std::string platformName;
        std::string metabomTag;
        std::set<std::string> metabomExcludeTags;
        std::set<std::string> metabomRestrictTags;
        std::set<std::string> restrictedInstallnames;
        std::map<std::string, Architecture> architectures;

        bool operator==(const Configuration& O) const
        {
            return architectures == O.architectures;
        }

        bool operator!=(const Configuration& other) const { return !(*this == other); }

        const Architecture& architecture(const std::string& architecture) const
        {
            assert(architectures.find(architecture) != architectures.end());
            return architectures.find(architecture)->second;
        }

        void forEachArchitecture(std::function<void(const std::string& archName)> lambda)
        {
            for (const auto& architecutre : architectures) {
                lambda(architecutre.first);
            }
        }
	};

    const std::map<std::string, Project>& projects()
    {
        return _projects;
    }

    const Configuration& configuration(const std::string& configuration) const
    {
        assert(_configurations.find(configuration) != _configurations.end());
        return _configurations.find(configuration)->second;
    }

    void forEachConfiguration(std::function<void(const std::string& configName)> lambda)
    {
        for (const auto& configuration : _configurations) {
            lambda(configuration.first);
        }
    }

    void addProjectSource(const std::string& project, const std::string& source, bool first = false)
    {
        auto& sources = _projects[project].sources;
        if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
            if (first) {
                sources.insert(sources.begin(), source);
            } else {
                sources.push_back(source);
            }
        }
    }

    const std::string projectPath(const std::string& projectName)
    {
        auto project = _projects.find(projectName);
        if (project == _projects.end())
            return "";
        if (project->second.sources.size() == 0)
            return "";
        return project->second.sources[0];
    }

    
    const bool empty(void) {
        for (const auto& configuration : _configurations) {
            if (configuration.second.architectures.size() != 0)
                return false;
        }
        return true;
    }
    
    const std::string dylibOrderFile() const { return _dylibOrderFile; };
    void setDylibOrderFile(const std::string& dylibOrderFile) { _dylibOrderFile = dylibOrderFile; };

    const std::string dirtyDataOrderFile() const { return  _dirtyDataOrderFile; };
    void setDirtyDataOrderFile(const std::string& dirtyDataOrderFile) { _dirtyDataOrderFile = dirtyDataOrderFile; };

    const std::string metabomFile() const { return _metabomFile; };
    void setMetabomFile(const std::string& metabomFile) { _metabomFile = metabomFile; };

    const std::string platform() const { return _platform; };
    void setPlatform(const std::string& platform) { _platform = platform; };

    const std::string& build() const { return _build; };
    void setBuild(const std::string& build) { _build = build; };
    const uint32_t                   version() const { return _manifestVersion; };
    void setVersion(const uint32_t manifestVersion) { _manifestVersion = manifestVersion; };
    bool                           normalized;

    Manifest(void) {}
    Manifest(const std::set<std::string>& archs, const std::string& overlayPath, const std::string& rootPath, const std::set<std::string>& paths);
#if BOM_SUPPORT
    Manifest(const std::string& path);
    Manifest(const std::string& path, const std::set<std::string>& overlays);
#endif
    void write(const std::string& path);
    void canonicalize(void);
    void calculateClosure(bool enforeceRootless);
    bool sameContentsAsCacheAtPath(const std::string& configuration, const std::string& architecture,
        const std::string& path) const;
    void remove(const std::string& config, const std::string& arch);
    MachOProxy* removeLargestLeafDylib(const std::string& configuration, const std::string& architecture);
    bool checkLinks();
    void runConcurrently(dispatch_queue_t queue, dispatch_semaphore_t concurrencyLimitingSemaphore, std::function<void(const std::string configuration, const std::string architecture)> lambda);
    bool filterForConfig(const std::string& configName);

private:
    uint32_t    _manifestVersion;
    std::string _build;
    std::string _dylibOrderFile;
    std::string _dirtyDataOrderFile;
    std::string _metabomFile;
    std::string _platform;
    std::map<std::string, Project>       _projects;
    std::map<std::string, Configuration> _configurations;
    void removeDylib(MachOProxy* proxy, const std::string& reason, const std::string& configuration, const std::string& architecture,
        std::unordered_set<ImageIdentifier>& processedIdentifiers);
    void calculateClosure(const std::string& configuration, const std::string& architecture);
    void canonicalizeDylib(const std::string& installname);
    template <typename P>
    void canonicalizeDylib(const std::string& installname, const uint8_t* p);
    void        addImplicitAliases(void);
    MachOProxy* dylibProxy(const std::string& installname, const std::string& arch);
};


#endif /* Manifest_h */
