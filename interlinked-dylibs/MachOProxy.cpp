//
//  DylibProxy.cpp
//  dyld
//
//  Created by Louis Gerbarg on 1/27/16.
//
//

#include <mach-o/loader.h>
#include <mach-o/fat.h>

#include "mega-dylib-utils.h"
#include "Logging.h"

#include "Trie.hpp"
#include "MachOProxy.h"

#ifndef EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
#define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE 0x02
#endif

namespace {
std::vector<MachOProxy*> mapMachOFile(const std::string& buildPath, const std::string& path)
{
    std::vector<MachOProxy*> retval;
    const uint8_t* p = (uint8_t*)( -1 );
    struct stat stat_buf;
	bool rootless;

    std::tie(p, stat_buf, rootless) = fileCache.cacheLoad(buildPath);

    if (p == (uint8_t*)(-1)) {
        return retval;
    }

    // if fat file, process each architecture
    const fat_header* fh = (fat_header*)p;
    const mach_header* mh = (mach_header*)p;
    if ( OSSwapBigToHostInt32( fh->magic ) == FAT_MAGIC ) {
        // Fat header is always big-endian
        const fat_arch* slices = (const fat_arch*)( (char*)fh + sizeof( fat_header ) );
        const uint32_t sliceCount = OSSwapBigToHostInt32( fh->nfat_arch );
        for ( uint32_t i = 0; i < sliceCount; ++i ) {
            // FIXME Should we validate the fat header matches the slices?
            ArchPair arch( OSSwapBigToHostInt32( slices[i].cputype ), OSSwapBigToHostInt32( slices[i].cpusubtype ) );
            uint32_t fileOffset = OSSwapBigToHostInt32( slices[i].offset );
            const mach_header* th = (mach_header*)(p+fileOffset);
            if ( ( OSSwapLittleToHostInt32( th->magic ) == MH_MAGIC ) || ( OSSwapLittleToHostInt32( th->magic ) == MH_MAGIC_64 ) ) {
                uint32_t fileSize = static_cast<uint32_t>( stat_buf.st_size );
                retval.push_back(new MachOProxy(buildPath, path, stringForArch(arch), stat_buf.st_ino, stat_buf.st_mtime, fileOffset, fileSize, rootless));
                //retval[stringForArch( arch )] = new MachOProxy( path, stat_buf.st_ino, stat_buf.st_mtime, fileOffset, fileSize, rootless );
            }
        }
    } else if ( ( OSSwapLittleToHostInt32( mh->magic ) == MH_MAGIC ) || ( OSSwapLittleToHostInt32( mh->magic ) == MH_MAGIC_64 ) ) {
        ArchPair arch( OSSwapLittleToHostInt32( mh->cputype ), OSSwapLittleToHostInt32( mh->cpusubtype ) );
        uint32_t fileOffset = OSSwapBigToHostInt32( 0 );
        uint32_t fileSize = static_cast<uint32_t>( stat_buf.st_size );
        retval.push_back(new MachOProxy(buildPath, path, stringForArch(arch), stat_buf.st_ino, stat_buf.st_mtime, fileOffset, fileSize, rootless));
        //retval[stringForArch( arch )] = new MachOProxy( path, stat_buf.st_ino, stat_buf.st_mtime, fileOffset, fileSize, rootless );
    } else {
        //    warning( "file '%s' is not contain requested a MachO", path.c_str() );
    }
    return retval;
}

} /* Anonymous namespace */

template <typename P>
std::vector<std::string> MachOProxy::dependencies()
{
    const uint8_t*                     buffer = getBuffer();
    const macho_header<P>*             mh = (const macho_header<P>*)buffer;
    const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)mh + sizeof(macho_header<P>));
    const uint32_t                     cmd_count = mh->ncmds();
    const macho_load_command<P>*       cmd = cmds;
    std::vector<std::string>           retval;

    for (uint32_t i = 0; i < cmd_count; ++i) {
        switch (cmd->cmd()) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                macho_dylib_command<P>* dylib = (macho_dylib_command<P>*)cmd;
                std::string             depName = dylib->name();

                retval.push_back(depName);
            } break;
        }
        cmd = (const macho_load_command<P>*)(((uint8_t*)cmd) + cmd->cmdsize());
    }

    return retval;
}

template <typename P>
std::vector<std::string> MachOProxy::reexports()
{
    const uint8_t*                     buffer = getBuffer();
    const macho_header<P>*             mh = (const macho_header<P>*)buffer;
    const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)mh + sizeof(macho_header<P>));
    const uint32_t                     cmd_count = mh->ncmds();
    const macho_load_command<P>*       cmd = cmds;
    std::vector<std::string>           retval;

    for (uint32_t i = 0; i < cmd_count; ++i) {
        switch (cmd->cmd()) {
        case LC_REEXPORT_DYLIB: {
            macho_dylib_command<P>* dylib = (macho_dylib_command<P>*)cmd;
            std::string             depName = dylib->name();

            retval.push_back(depName);
        } break;
        }
        cmd = (const macho_load_command<P>*)(((uint8_t*)cmd) + cmd->cmdsize());
    }

    return retval;
}

std::vector<std::string> MachOProxy::dependencies()
{
    switch (archForString(arch).arch) {
        case CPU_TYPE_ARM:
        case CPU_TYPE_I386:
            return dependencies<Pointer32<LittleEndian>>();
        case CPU_TYPE_X86_64:
        case CPU_TYPE_ARM64:
            return dependencies<Pointer64<LittleEndian>>();
            break;
        default:
            return std::vector<std::string>();
        }
}

std::vector<std::string> MachOProxy::reexports()
{
    switch (archForString(arch).arch) {
        case CPU_TYPE_ARM:
        case CPU_TYPE_I386:
            return reexports<Pointer32<LittleEndian>>();
        case CPU_TYPE_X86_64:
        case CPU_TYPE_ARM64:
            return reexports<Pointer64<LittleEndian>>();
            break;
        default:
            return std::vector<std::string>();
    }
}

template <typename P>
std::string MachOProxy::machoParser(bool ignoreUncacheableDylibsInExecutables)
{
    const uint8_t*                     buffer = getBuffer();
    bool                               hasSplitSegInfo = false;
    bool                               hasDylidInfo = false;
    const macho_header<P>*             mh = (const macho_header<P>*)buffer;
    const macho_symtab_command<P>*     symTab = nullptr;
    const macho_dysymtab_command<P>*   dynSymTab = nullptr;
    const macho_load_command<P>* const cmds = (macho_load_command<P>*)((uint8_t*)mh + sizeof(macho_header<P>));
    const macho_dyld_info_command<P>*  dyldInfo = nullptr;
    const uint32_t                     cmd_count = mh->ncmds();
    const macho_load_command<P>*       cmd = cmds;
    uint64_t                           baseAddr = 0;
    _filetype = mh->filetype();
    if (_filetype == MH_DYLIB_STUB) {
        return "stub dylib";
    }
    if (_filetype == MH_DSYM) {
        return "DSYM";
    }
    for (uint32_t i = 0; i < cmd_count; ++i) {
        switch (cmd->cmd()) {
        case LC_ID_DYLIB: {
            macho_dylib_command<P>* dylib = (macho_dylib_command<P>*)cmd;
            if (dylib->name()[0] != '/') {
                if (strncmp(dylib->name(), "@rpath", 6) == 0)
                    return "@rpath cannot be used in -install_name for OS dylibs";
                else
                    return "-install_name is not an absolute path";
            }
            installName = dylib->name();
            installNameOffsetInTEXT = (uint32_t)((uint8_t*)cmd - buffer) + dylib->name_offset();
            addAlias(path);
        } break;
        case LC_UUID: {
            const macho_uuid_command<P>* uuidCmd = (macho_uuid_command<P>*)cmd;
            uuid = UUID(uuidCmd->uuid());
        } break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: {
            macho_dylib_command<P>* dylib = (macho_dylib_command<P>*)cmd;
            std::string             depName = dylib->name();
            if ( isExecutable() && ignoreUncacheableDylibsInExecutables && !has_prefix(depName, "/usr/lib/") && !has_prefix(depName, "/System/Library/") ) {
                // <rdar://problem/25918268> in update_dyld_shared_cache don't warn if root executable links with something not eligible for shared cache
                break;
            }
            else if ( depName[0] != '/' ) {
                return "linked against a dylib whose -install_name was non-absolute (e.g. @rpath)";
            }
        } break;
        case macho_segment_command<P>::CMD: {
            const macho_segment_command<P>* segCmd = (macho_segment_command<P>*)cmd;
            MachOProxySegment               seg;
            seg.name = segCmd->segname();
            seg.size = align(segCmd->vmsize(), 12);
            seg.vmaddr = segCmd->vmaddr();
            seg.diskSize = (uint32_t)segCmd->filesize();
            seg.fileOffset = (uint32_t)segCmd->fileoff();
            seg.protection = segCmd->initprot();
            if (segCmd->nsects() > 0) {
                seg.p2align = 0;
                const macho_section<P>* const sectionsStart = (macho_section<P>*)((uint8_t*)segCmd + sizeof(macho_segment_command<P>));
                const macho_section<P>* const sectionsLast = &sectionsStart[segCmd->nsects() - 1];
                const macho_section<P>* const sectionsEnd = &sectionsStart[segCmd->nsects()];
                for (const macho_section<P>* sect = sectionsStart; sect < sectionsEnd; ++sect) {
                    if (sect->align() > seg.p2align)
                        seg.p2align = sect->align();
                }
                seg.sizeOfSections = sectionsLast->addr() + sectionsLast->size() - segCmd->vmaddr();
            } else {
                seg.p2align = 12;
            }
            segments.push_back(seg);
            if (seg.name == "__TEXT") {
                baseAddr =  seg.vmaddr;
            }
        } break;
        case LC_SEGMENT_SPLIT_INFO:
            hasSplitSegInfo = true;
            break;
        case LC_SYMTAB:
            symTab = (macho_symtab_command<P>*)cmd;
            break;
        case LC_DYSYMTAB:
            dynSymTab = (macho_dysymtab_command<P>*)cmd;
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY:
            dyldInfo = (macho_dyld_info_command<P>*)cmd;
            hasDylidInfo = true;
            break;
        }
        cmd = (const macho_load_command<P>*)(((uint8_t*)cmd) + cmd->cmdsize());
    }

    identifier = uuid;

    if (!hasDylidInfo) {
        return "built for old OS";
    }

    if (dyldInfo && dyldInfo->bind_size() != 0) {
        _bind_offset = dyldInfo->bind_off();
        _bind_size = dyldInfo->bind_size();
    }

    if (dyldInfo && dyldInfo->lazy_bind_size() != 0) {
        _lazy_bind_offset = dyldInfo->lazy_bind_off();
        _lazy_bind_size = dyldInfo->lazy_bind_size();
    }

    // if no export info, no _exports map to build
    if (dyldInfo && dyldInfo->export_size() != 0) {
        std::vector<ExportInfoTrie::Entry> exports;
        const uint8_t*                     exportsStart = &buffer[dyldInfo->export_off()];
        const uint8_t*                     exportsEnd = &exportsStart[dyldInfo->export_size()];
        if (!ExportInfoTrie::parseTrie(exportsStart, exportsEnd, exports)) {
            terminate("malformed exports trie in %s", path.c_str());
        }

        for (const ExportInfoTrie::Entry& entry : exports) {
            if (!_exports[entry.name].isAbsolute) {
                for (const auto& seg : segments) {
                    if (seg.size > 0 && (seg.vmaddr - baseAddr) <= entry.info.address && entry.info.address < (seg.vmaddr - baseAddr) + seg.size) {
                        _exports[entry.name].segmentOffset = entry.info.address - (seg.vmaddr - baseAddr);
                        _exports[entry.name].segmentName = seg.name;
                        break;
                    }
                }
            } else {
                _exports[entry.name].segmentOffset = (uint64_t)entry.info.address;
                _exports[entry.name].segmentName = "";
            }

            switch (entry.info.flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) {
                case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                    if ((entry.info.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER)) {
                        _exports[entry.name].isResolver = true;
                    }
                    if (entry.info.flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
                        SymbolInfo& info = _exports[entry.name];
                        info.isSymbolReExport = true;
                        info.reExportDylibIndex = (int)entry.info.other;
                        if (!entry.info.importName.empty())
                            info.reExportName = entry.info.importName;
                    }
                    break;
                case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
                    _exports[entry.name].isThreadLocal = true;
                    break;
                case EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE:
                    _exports[entry.name].isAbsolute = true;
                    break;
                default:
                    terminate("non-regular symbol binding not supported for %s in %s", entry.name.c_str(), path.c_str());
                    break;
            }
        }
    }

    if (!isDylib()) {
        return "";
    }

    if ((mh->flags() & MH_TWOLEVEL) == 0) {
        return "built with -flat_namespace";
    }

    if (!hasSplitSegInfo) {
        bool inUsrLib = (installName.size() > 9) && (installName.substr(0, 9) == "/usr/lib/");
        bool inSystemLibrary = (installName.size() > 16) && (installName.substr(0, 16) == "/System/Library/");
        if (!inUsrLib && !inSystemLibrary) {
            return "-install_name not /usr/lib/* or /System/Library/*";
        }
        return "no shared region info";
    }

    if ((symTab == nullptr) && (dynSymTab == nullptr)) {
        return "no symbol table";
    }

    if (installName.empty()) {
        return "dylib missing install name";
    }

    // scan undefines looking for invalid ordinals
    const macho_nlist<P>* symbolTable = (macho_nlist<P>*)((uint8_t*)mh + symTab->symoff());
    const uint32_t        startUndefs = dynSymTab->iundefsym();
    const uint32_t        endUndefs = startUndefs + dynSymTab->nundefsym();
    for (uint32_t i = startUndefs; i < endUndefs; ++i) {
        uint8_t ordinal = GET_LIBRARY_ORDINAL(symbolTable[i].n_desc());
        if (ordinal == DYNAMIC_LOOKUP_ORDINAL) {
            return "built with '-undefined dynamic_lookup'";
        } else if (ordinal == EXECUTABLE_ORDINAL) {
            return "built with -bundle_loader";
        }
    }

    return "";
}

const bool MachOProxy::isDylib()
{
    return (_filetype == MH_DYLIB);
}

const bool MachOProxy::isExecutable()
{
    return (_filetype == MH_EXECUTE);
}

static std::map<ImageIdentifier, MachOProxy*> identifierMap;
std::map<std::pair<std::string, std::string>, MachOProxy*> archMap;
static dispatch_queue_t identifierQueue;

MachOProxy* MachOProxy::forIdentifier(const ImageIdentifier& identifier, const std::string preferredArch)
{
    auto i = identifierMap.find(identifier);
    // We need an identifier
    if (i == identifierMap.end())
        return nullptr;

    // Is the identifier the arch we want?
    if (i->second->arch == preferredArch)
        return i->second;

    // Fallback to a slow path to try to find a best fit
    return forInstallnameAndArch(i->second->installName, preferredArch);
}

MachOProxy* MachOProxy::forInstallnameAndArch(const std::string& installname, const std::string& arch)
{
    auto i = archMap.find(std::make_pair(installname, arch));
    if (i == archMap.end())
        i = archMap.find(std::make_pair(installname, fallbackArchStringForArchString(arch)));
    if (i != archMap.end())
        return i->second;
    return nullptr;
}

void MachOProxy::mapDependencies()
{
    // Build a complete map of all installname/alias,archs to their proxies
    runOnAllProxies(false, [&](MachOProxy* proxy) {
        archMap[std::make_pair(proxy->path, proxy->arch)] = proxy;
        for (auto& alias : proxy->installNameAliases) {
            archMap[std::make_pair(alias, proxy->arch)] = proxy;
        }
    });

    //Wire up the dependencies
    runOnAllProxies(false, [&](MachOProxy* proxy) {
        auto dependencyInstallnames = proxy->dependencies();
        for (auto dependencyInstallname : dependencyInstallnames) {
            auto dependencyProxy = forInstallnameAndArch(dependencyInstallname, proxy->arch);
            if (dependencyProxy == nullptr) {
                proxy->error = "Missing dependency: " + dependencyInstallname;
            } else {
                proxy->requiredIdentifiers.push_back(dependencyProxy->identifier);
                dependencyProxy->dependentIdentifiers.push_back(proxy->identifier);
            }
        }

        auto reexportInstallnames = proxy->reexports();
        for (auto reexportInstallname : reexportInstallnames) {
            auto reexportProxy = forInstallnameAndArch(reexportInstallname, proxy->arch);
            if (reexportProxy == nullptr) {
                proxy->error = "Missing reexport dylib: " + reexportInstallname;
            } else {
                proxy->_reexportProxies.push_back(reexportProxy);
            }
        }

    });
}

void MachOProxy::runOnAllProxies(bool concurrently, std::function<void(MachOProxy* proxy)> lambda)
{
    dispatch_group_t runGroup = dispatch_group_create();
    dispatch_queue_t runQueue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, NULL);

    for (auto& identifier : identifierMap) {
        if (concurrently) {
            cacheBuilderDispatchGroupAsync(runGroup, runQueue, [&] {
                lambda(identifier.second);
            });
        } else {
            lambda(identifier.second);
        }
    }

    dispatch_group_wait(runGroup, DISPATCH_TIME_FOREVER);
}

std::map<std::string, MachOProxy*> MachOProxy::loadProxies(const std::string& buildPath, const std::string& path, bool warnOnProblems, bool ignoreUncacheableDylibsInExecutables)
{
    std::vector<MachOProxy*> slices = mapMachOFile(buildPath, path);
    std::map<std::string, MachOProxy*> retval;

    for ( auto& slice : slices ) {
        std::string errorMessage;
        verboseLog( "analyzing file '%s'", path.c_str() );
        switch (archForString(slice->arch).arch) {
            case CPU_TYPE_ARM:
            case CPU_TYPE_I386:
                errorMessage = slice->machoParser<Pointer32<LittleEndian>>(ignoreUncacheableDylibsInExecutables);
                break;
            case CPU_TYPE_X86_64:
            case CPU_TYPE_ARM64:
                errorMessage = slice->machoParser<Pointer64<LittleEndian>>(ignoreUncacheableDylibsInExecutables);
                break;
            default:
                errorMessage = "unsupported arch '" + slice->arch + "'";
                break;
        }

        if (errorMessage.empty()) {
            static dispatch_once_t onceToken;
            dispatch_once(&onceToken, ^{
                identifierQueue = dispatch_queue_create("com.apple.dyld.cache.metabom.ids", DISPATCH_QUEUE_SERIAL);
            });
            retval[slice->arch] = slice;
            dispatch_sync(identifierQueue, ^{
                identifierMap[slice->identifier] = slice;
            });
        } else {
            if (warnOnProblems)
                warning("%s (%s)", errorMessage.c_str(), path.c_str());
        }
    }

    return retval;
}

const uint8_t* MachOProxy::getBuffer() {
    const uint8_t* p = (uint8_t*)( -1 );
    struct stat stat_buf;
	bool rootless;
    std::tie(p, stat_buf, rootless) = fileCache.cacheLoad(buildPath);
    return p + fatFileOffset;
}

bool MachOProxy::addAlias( const std::string& alias ) {
    if (!has_prefix(alias, "/usr/lib/") && !has_prefix(alias, "/System/Library/"))
        return false;
    if ( alias != installName && installNameAliases.count( alias ) == 0 ) {
        installNameAliases.insert( alias );
        return true;
    }
    return false;
}
