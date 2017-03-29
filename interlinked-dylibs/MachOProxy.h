//
//  DylibProxy.h
//  dyld
//
//  Created by Louis Gerbarg on 1/27/16.
//
//

#ifndef MachOProxy_h
#define MachOProxy_h

#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstdint>

#include <sys/stat.h>

#include "mega-dylib-utils.h"

struct MachOProxy;

struct MachOProxySegment {
    std::string name;
    uint64_t    size;
    uint64_t    sizeOfSections;
    uint64_t    vmaddr;
    uint32_t    diskSize;
    uint32_t    fileOffset;
    uint8_t     p2align;
    uint8_t     protection;
};

struct MachOProxy {
    MachOProxy(const std::string& bp, const std::string& p, const std::string& a, ino_t i, time_t t, uint32_t o, uint32_t s, bool r)
        : buildPath(bp)
        , path(p)
        , arch(a)
        , fatFileOffset(o)
        , fileSize(s)
        , lastModTime(t)
        , inode(i)
        , installNameOffsetInTEXT(0)
        , rootlessProtected(r)
        , _bind_offset(0)
        , _bind_size(0)
        , queue(dispatch_queue_create("com.apple.dyld.proxy", NULL))
    {
    }

    struct SymbolInfo {
        SymbolInfo() {}
        std::string segmentName;
        uint64_t    segmentOffset = 0;
        bool        isResolver = false;
        bool        isAbsolute = false;
        bool        isSymbolReExport = false;
        bool        isThreadLocal = false;
        int         reExportDylibIndex = 0;
        std::string reExportName;
    };

    const std::string           buildPath;
    const std::string           path;
    const std::string           arch;
    const uint32_t              fatFileOffset;
    const uint32_t              fileSize;
    const time_t                lastModTime;
    const ino_t                 inode;
    const bool                  rootlessProtected;
    dispatch_queue_t            queue;
    std::string                 installName;
    std::set<std::string>       installNameAliases;
    uint32_t                    installNameOffsetInTEXT;
    std::string                 error;
    std::vector<ImageIdentifier>   requiredIdentifiers;
    std::vector<ImageIdentifier>   dependentIdentifiers;
    UUID                        uuid;
    ImageIdentifier             identifier;
    std::vector<MachOProxySegment> segments;

    const uint8_t* getBuffer();
    const uint8_t* getBindStart() { return &(getBuffer())[_bind_offset]; }
    const uint8_t* getBindEnd() { return &(getBuffer())[_bind_offset + _bind_size]; }
    const uint8_t* getLazyBindStart() { return &(getBuffer())[_lazy_bind_offset]; }
    const uint8_t* getLazyBindEnd() { return &(getBuffer())[_lazy_bind_offset + _lazy_bind_size]; }

    const bool     isDylib();
    const bool     isExecutable();
    bool addAlias(const std::string& alias);
    static void mapDependencies();

    const uint64_t addressOf(const std::string& symbol, const std::map<const MachOProxy*, std::vector<SharedCache::SegmentInfo>>& segmentMap)
    {
        auto info = symbolInfo(symbol);
        assert(info != nullptr);
        if (info->isAbsolute)
            return info->segmentOffset;
        auto proxyI = segmentMap.find(this);
        assert(proxyI != segmentMap.end());

        for (const auto& seg : proxyI->second) {
            if (seg.base->name == info->segmentName) {
                assert(!info->segmentName.empty());
                return seg.address + info->segmentOffset;
            }
        }

        return 0;
    }

    SymbolInfo* symbolInfo(const std::string& symbol)
    {
        auto i = _exports.find(symbol);
        if (i != _exports.end())
            return &i->second;
        return nullptr;
    }

    bool providesSymbol(const std::string& symbol)
    {
        if (_exports.find(symbol) != _exports.end())
            return true;

        for (const auto& proxy : _reexportProxies) {
            if (proxy->providesSymbol(symbol))
                return true;
        }
        return false;
    }
    static std::map<std::string, MachOProxy*> loadProxies(const std::string& prefix, const std::string& path, bool warnOnProblems = false, bool ignoreUncacheableDylibsInExecutables = false);
    static void runOnAllProxies(bool concurrently, std::function<void(MachOProxy* proxy)> lambda);
    static MachOProxy* forIdentifier(const ImageIdentifier& identifier, const std::string preferredArch);
    static MachOProxy* forInstallnameAndArch(const std::string& installname, const std::string& arch);

    std::vector<std::string> dependencies();
    std::vector<std::string> reexports();

private:
    uint32_t _filetype;
    std::map<std::string, SymbolInfo> _exports;
    uint32_t                 _bind_offset;
    uint32_t                 _bind_size;
    uint32_t                 _lazy_bind_offset;
    uint32_t                 _lazy_bind_size;
    std::vector<MachOProxy*> _reexportProxies;

    template <typename P>
    std::string machoParser(bool ignoreUncacheableDylibsInExecutables);

    template <typename P>
    std::vector<std::string> dependencies();

    template <typename P>
    std::vector<std::string> reexports();
};


#endif /* MachOProxy_h */
