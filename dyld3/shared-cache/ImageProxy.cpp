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


#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <uuid/uuid.h>
#include <os/log.h>

#include <string>
#include <vector>
#include <array>

#include "ImageProxy.h"
#include "FileUtils.h"
#include "StringUtils.h"
#include "MachOParser.h"
#include "LaunchCacheFormat.h"
#include "LaunchCacheWriter.h"
#include "PathOverrides.h"
#include "libdyldEntryVector.h"

namespace dyld3 {

typedef launch_cache::TargetSymbolValue   TargetSymbolValue;



///////////////////////////  ImageProxy  ///////////////////////////

ImageProxy::ImageProxy(const mach_header* mh, const BinaryImageData* imageData, uint32_t indexInGroup, bool dyldCacheIsRaw)
 : _mh(mh), _sliceFileOffset(0), _modTime(0), _inode(0), _imageBinaryData(imageData), _runtimePath(launch_cache::Image(imageData).path()),
   _groupNum(0), _indexInGroup(indexInGroup), _isSetUID(false), _dyldCacheIsRaw(dyldCacheIsRaw), _platformBinary(false), _overrideOf(ImageRef::weakImportMissing()),
   _directDependentsSet(false), _deepDependentsSet(false), _initBeforesArraySet(false), _initBeforesComputed(false),
   _invalid(launch_cache::Image(imageData).isInvalid()), _staticallyReferenced(false), _cwdMustBeThisDir(false)
{
}

ImageProxy::ImageProxy(const DyldSharedCache::MappedMachO& mapping, uint32_t groupNum, uint32_t indexInGroup, bool dyldCacheIsRaw)
 : _mh(mapping.mh), _sliceFileOffset(mapping.sliceFileOffset), _modTime(mapping.modTime), _inode(mapping.inode), _imageBinaryData(nullptr), _runtimePath(mapping.runtimePath),
   _groupNum(groupNum), _indexInGroup(indexInGroup), _isSetUID(mapping.isSetUID), _dyldCacheIsRaw(dyldCacheIsRaw), _platformBinary(mapping.protectedBySIP),
   _overrideOf(ImageRef::weakImportMissing()), _directDependentsSet(false), _deepDependentsSet(false), _initBeforesArraySet(false), _initBeforesComputed(false),
   _invalid(false), _staticallyReferenced(false), _cwdMustBeThisDir(false)
{
}


void ImageProxy::processRPaths(ImageProxyGroup& owningGroup)
{
    // parse LC_RPATH
    __block std::unordered_set<std::string> rawRpaths;
    MachOParser parser(_mh, _dyldCacheIsRaw);
    parser.forEachRPath(^(const char* rpath, bool& stop) {
        if ( rawRpaths.count(rpath) ) {
            _diag.warning("duplicate LC_RPATH (%s) in %s", rpath, _runtimePath.c_str());
            return;
        }
        rawRpaths.insert(rpath);
        std::string thisRPath = rpath;
        if ( startsWith(thisRPath, "@executable_path/") ) {
            std::string mainPath = owningGroup.mainProgRuntimePath();
            if ( mainPath.empty() && parser.isDynamicExecutable() ) {
                mainPath = _runtimePath;
            }
            if ( !mainPath.empty() ) {
                std::string newPath = mainPath.substr(0, mainPath.rfind('/')+1) + thisRPath.substr(17);
                std::string normalizedPath = owningGroup.normalizedPath(newPath);
                if ( fileExists(normalizedPath) )
                    _rpaths.push_back(normalizedPath);
                else
                    _diag.warning("LC_RPATH to nowhere (%s) in %s", rpath, _runtimePath.c_str());
                char resolvedMainPath[PATH_MAX];
                if ( (realpath(mainPath.c_str(), resolvedMainPath) != nullptr) && (mainPath.c_str() != resolvedMainPath) ) {
                    std::string realMainPath = resolvedMainPath;
                    size_t lastSlashPos = realMainPath.rfind('/');
                    std::string newRealPath = realMainPath.substr(0, lastSlashPos+1) + thisRPath.substr(17);
                    if ( realMainPath != mainPath ) {
                        for (const std::string& pre : owningGroup._buildTimePrefixes) {
                            std::string aPath = owningGroup.normalizedPath(pre + newRealPath);
                            if ( fileExists(aPath) ) {
                                _rpaths.push_back(owningGroup.normalizedPath(newRealPath));
                            }
                        }
                    }
                }
            }
            else {
                _diag.warning("LC_RPATH uses @executable_path in %s", _runtimePath.c_str());
            }
        }
        else if ( thisRPath == "@executable_path" ) {
            std::string mainPath = owningGroup.mainProgRuntimePath();
            if ( mainPath.empty() && parser.isDynamicExecutable() ) {
                mainPath = _runtimePath;
            }
            if ( !mainPath.empty() ) {
                std::string newPath = mainPath.substr(0, mainPath.rfind('/')+1);
                std::string normalizedPath = owningGroup.normalizedPath(newPath);
                _rpaths.push_back(normalizedPath);
            }
            else {
                _diag.warning("LC_RPATH uses @executable_path in %s", _runtimePath.c_str());
            }
        }
        else if ( startsWith(thisRPath, "@loader_path/") ) {
            size_t lastSlashPos = _runtimePath.rfind('/');
            std::string newPath = _runtimePath.substr(0, lastSlashPos+1) + thisRPath.substr(13);
            bool found = false;
            for (const std::string& pre : owningGroup._buildTimePrefixes) {
                std::string aPath = owningGroup.normalizedPath(pre + newPath);
                if ( fileExists(aPath) ) {
                    _rpaths.push_back(owningGroup.normalizedPath(newPath));
                    found = true;
                    break;
                }
            }
            char resolvedPath[PATH_MAX];
            if ( (realpath(_runtimePath.c_str(), resolvedPath) != nullptr) && (_runtimePath.c_str() != resolvedPath) ) {
                std::string realRunPath = resolvedPath;
                lastSlashPos = realRunPath.rfind('/');
                std::string newRealPath = realRunPath.substr(0, lastSlashPos+1) + thisRPath.substr(13);
                if ( newRealPath != newPath ) {
                    for (const std::string& pre : owningGroup._buildTimePrefixes) {
                        std::string aPath = owningGroup.normalizedPath(pre + newRealPath);
                        if ( fileExists(aPath) ) {
                            _rpaths.push_back(owningGroup.normalizedPath(newRealPath));
                            found = true;
                            break;
                        }
                    }
                }
            }
            if ( !found ) {
                // even though this path does not exist, we need to add it to must-be-missing paths
                // in case it shows up at launch time
                _rpaths.push_back(owningGroup.normalizedPath(newPath));
                _diag.warning("LC_RPATH to nowhere (%s) in %s", rpath, _runtimePath.c_str());
            }
        }
        else if ( thisRPath == "@loader_path" ) {
            size_t lastSlashPos = _runtimePath.rfind('/');
            std::string newPath = _runtimePath.substr(0, lastSlashPos+1);
            std::string normalizedPath = owningGroup.normalizedPath(newPath);
            _rpaths.push_back(normalizedPath);
        }
        else if ( rpath[0] == '@' ) {
            _diag.warning("LC_RPATH with unknown @ variable (%s) in %s", rpath, _runtimePath.c_str());
        }
        else {
            if ( rpath[0] == '/' )
                _diag.warning("LC_RPATH is absolute path (%s) in %s", rpath, _runtimePath.c_str());
            _rpaths.push_back(rpath);
        }
    });
    //if ( !_rpaths.empty() ) {
    //    fprintf(stderr, "for %s\n", _runtimePath.c_str());
    //    for (const std::string& p : _rpaths)
    //        fprintf(stderr, "   %s\n", p.c_str());
    //}
}

void ImageProxy::addDependentsDeep(ImageProxyGroup& owningGroup, RPathChain* prev, bool staticallyReferenced)
{
    // mark binaries that are statically referenced and thus will never be unloaded
    if ( staticallyReferenced )
        _staticallyReferenced = true;

    if ( _deepDependentsSet )
        return;

    // find all immediate dependents
    addDependentsShallow(owningGroup, prev);
    if ( _diag.hasError() ) {
        _invalid = true;
        return;
    }

    // recurse though each dependent
    RPathChain rchain = { this, prev, _rpaths };
    for (ImageProxy* proxy : _dependents) {
        if ( proxy == nullptr )
            continue; // skip over weak missing dependents
        if ( !proxy->_directDependentsSet )
            proxy->addDependentsDeep(owningGroup, &rchain, staticallyReferenced);
        if ( proxy->invalid() )
            _invalid = true;
    }

    _deepDependentsSet = true;
}

void ImageProxy::addDependentsShallow(ImageProxyGroup& owningGroup, RPathChain* prev)
{
    if ( _directDependentsSet )
        return;

    MachOParser thisParser(mh(), _dyldCacheIsRaw);
    dyld3::Platform thisPlatform = thisParser.platform();

    processRPaths(owningGroup);
    __block RPathChain rchain = { this, prev, _rpaths };

    thisParser.forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool &stop) {
        if ( (loadPath[0] != '/') && (loadPath[0] != '@') ) {
            _diag.warning("load path is file system relative (%s) in %s", loadPath, runtimePath().c_str());
        }
        Diagnostics depDiag;
        ImageProxy* dep = owningGroup.findImage(depDiag, loadPath, isWeak, &rchain);
        if ( (dep == nullptr) || dep->invalid() ) {
            if (isWeak) {
                // weak link against a broken dylib, pretend dylib is not there
                dep = nullptr;
            } else {
                if ( depDiag.warnings().empty() ) {
                    if ( thisParser.header()->filetype == MH_EXECUTE )
                        _diag.error("required dylib '%s' not found", loadPath);
                    else
                        _diag.error("required dylib '%s' not found, needed by '%s'", loadPath, runtimePath().c_str());
                }
                else {
                    std::string allTries;
                    for (const std::string& warn : depDiag.warnings()) {
                        if ( allTries.empty() )
                            allTries = warn;
                        else
                            allTries = allTries + ", " + warn;
                    }
                    _diag.error("required dylib '%s' not found, needed by '%s'.  Did try: %s", loadPath, runtimePath().c_str(), allTries.c_str());
                }
            }
        }
        else {
            MachOParser depParser(dep->mh(), _dyldCacheIsRaw);
            if ( _diag.noError() ) {
                // verify found image has compatible version and matching platform
                dyld3::Platform depPlatform = depParser.platform();
                if ( depPlatform != thisPlatform ) {
                    // simulator allows a few macOS libSystem dylibs
                    if ( !inLibSystem() || !dep->inLibSystem() ) {
                        _diag.error("found '%s' but it was built for different platform '%s' than required '%s'.  Needed by '%s'", dep->runtimePath().c_str(),
                                    MachOParser::platformName(depPlatform).c_str(), MachOParser::platformName(thisPlatform).c_str(), runtimePath().c_str());
                    }
                }
            }
            if ( _diag.noError() ) {
                // verify compat version
                const char* installName;
                uint32_t    foundCompatVers;
                uint32_t    foundCurrentVers;
                if ( depParser.header()->filetype != MH_DYLIB ) {
                    _diag.error("found '%s' which is not a dylib.  Needed by '%s'", dep->runtimePath().c_str(), runtimePath().c_str());
                }
                else {
                    depParser.getDylibInstallName(&installName, &foundCompatVers, &foundCurrentVers);
                    if ( foundCompatVers < compatVersion ) {
                        _diag.error("found '%s' which has compat version (%s) which is less than required (%s).  Needed by '%s'", dep->runtimePath().c_str(),
                                    MachOParser::versionString(foundCompatVers).c_str(), MachOParser::versionString(compatVersion).c_str(), runtimePath().c_str());
                    }
                }
            }
        }
        if ( _diag.hasError() ) {
            stop = true;
            _invalid = true;
        }
        _dependents.push_back(dep);
        if ( isWeak )
            _dependentsKind.push_back(launch_cache::Image::LinkKind::weak);
        else if ( isReExport )
            _dependentsKind.push_back(launch_cache::Image::LinkKind::reExport);
        else if ( isUpward )
            _dependentsKind.push_back(launch_cache::Image::LinkKind::upward);
        else
            _dependentsKind.push_back(launch_cache::Image::LinkKind::regular);
    });
    _directDependentsSet = true;
}

bool ImageProxy::inLibSystem() const
{
    return startsWith(runtimePath(), "/usr/lib/system/") || startsWith(runtimePath(), "/usr/lib/libSystem.");
}

void ImageProxy::forEachDependent(void (^handler)(ImageProxy* dep, LinkKind)) const
{
    for (int i=0; i < _dependents.size(); ++i) {
        handler(_dependents[i], _dependentsKind[i]);
    }
}


bool ImageProxy::findExportedSymbol(Diagnostics& diag, const char* symbolName, MachOParser::FoundSymbol& foundInfo) const
{
    MachOParser parser(_mh, _dyldCacheIsRaw);
    return parser.findExportedSymbol(diag, symbolName, (void*)this, foundInfo, ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        ImageProxy* proxy    = (ImageProxy*)extra;
        if ( depIndex < proxy->_dependents.size() ) {
            ImageProxy* depProxy = proxy->_dependents[depIndex];
            *foundMH    = depProxy->_mh;
            *foundExtra = (void*)depProxy;
            return true;
        }
        return false;
    });
}

bool ImageProxy::InitOrderInfo::beforeHas(ImageRef ref)
{
    ImageRef clearRef = ref;
    clearRef.clearKind();
    return ( std::find(initBefore.begin(), initBefore.end(), clearRef) != initBefore.end() );
}

bool ImageProxy::InitOrderInfo::upwardHas(ImageProxy* proxy)
{
    return ( std::find(danglingUpward.begin(), danglingUpward.end(), proxy) != danglingUpward.end() );
}

void ImageProxy::InitOrderInfo::removeRedundantUpwards()
{
    danglingUpward.erase(std::remove_if(danglingUpward.begin(), danglingUpward.end(),
                                        [&](ImageProxy* proxy) {
                                            ImageRef ref(0, proxy->_groupNum, proxy->_indexInGroup);
                                            return beforeHas(ref);
                                        }), danglingUpward.end());
}


//
// Every image has a list of "init-before" which means if that image was dlopen()ed
// here is the exact list of images to initialize in the exact order.  This makes
// the runtime easy.  It just walks the init-before list in order and runs each
// initializer if it has not already been run.
//
// The init-before list for each image is calculated based on the init-before list
// of each of its dependents.  It simply starts with the list of its first dependent,
// then appends the list of the next, removing entries already in the list, etc.
// Lastly if the current image has an initializer, it is appended to its init-before list.
//
// To handle cycles, when recursing to get a dependent's init-before list, any image
// whose list is still being calculated (cycle), just returns its list so far.
//
// Explicit upward links are handled in two parts.  First, in the algorithm described above,
// all upward links are ignored, which works fine as long as anything upward linked is
// downward linked at some point.  If not, it is called a "dangling upward link". Since
// nothing depends on those, they are added to the end of the final init-before list.
//

void ImageProxy::recursiveBuildInitBeforeInfo(ImageProxyGroup& owningGroup)
{
    if ( _initBeforesComputed )
        return;
    _initBeforesComputed = true; // break cycles

    if ( _imageBinaryData != nullptr ) {
        assert(_groupNum == 0);
        // if this is proxy for something in dyld cache, get its list from cache
        // and parse list into befores and upwards
        launch_cache::Image image(_imageBinaryData);
        image.forEachInitBefore(^(launch_cache::binary_format::ImageRef ref) {
            if ( (LinkKind)ref.kind() == LinkKind::upward ) {
                ImageProxyGroup* groupP = &owningGroup;
                while (groupP->_groupNum != 0)
                    groupP = groupP->_nextSearchGroup;
                launch_cache::ImageGroup dyldCacheGroup(groupP->_basedOn);
                launch_cache::Image      dyldCacheImage = dyldCacheGroup.image(ref.indexInGroup());
                Diagnostics diag;
                ImageProxy* p = groupP->findAbsoluteImage(diag, dyldCacheImage.path(), false, false);
                if ( diag.noError() )
                    _initBeforesInfo.danglingUpward.push_back(p);
            }
            else {
                _initBeforesInfo.initBefore.push_back(ref);
            }
        });
    }
    else {
        // calculate init-before list for this image by merging init-before's of all its dependent dylibs
        unsigned depIndex = 0;
        for (ImageProxy* depProxy : _dependents) {
            if ( depProxy == nullptr ) {
                assert(_dependentsKind[depIndex] == LinkKind::weak);
            }
            else {
                if ( _dependentsKind[depIndex] == LinkKind::upward ) {
                    // if this upward link is already in the list, we ignore it.  Otherwise add to front of list
                    if ( _initBeforesInfo.upwardHas(depProxy) ) {
                        // already in upward list, do nothing
                    }
                    else {
                        ImageRef ref(0, depProxy->_groupNum, depProxy->_indexInGroup);
                        if ( _initBeforesInfo.beforeHas(ref) ) {
                            // already in before list, do nothing
                        }
                        else {
                            // add to upward list
                            _initBeforesInfo.danglingUpward.push_back(depProxy);
                        }
                    }
                }
                else {
                    // compute init-befores of downward dependents
                    depProxy->recursiveBuildInitBeforeInfo(owningGroup);
                    // merge befores from this downward link into current befores list
                    for (ImageRef depInit : depProxy->_initBeforesInfo.initBefore) {
                        if ( !_initBeforesInfo.beforeHas(depInit) )
                            _initBeforesInfo.initBefore.push_back(depInit);
                    }
                    // merge upwards from this downward link into current befores list
                    for (ImageProxy* upProxy : depProxy->_initBeforesInfo.danglingUpward) {
                        ImageRef ref(0, upProxy->_groupNum, upProxy->_indexInGroup);
                        if ( _initBeforesInfo.beforeHas(ref) ) {
                            // already in current initBefore list, so ignore this upward
                        }
                        else if ( _initBeforesInfo.upwardHas(upProxy) ) {
                            // already in current danglingUpward list, so ignore this upward
                        }
                        else {
                            // append to current danglingUpward list
                            _initBeforesInfo.danglingUpward.push_back(upProxy);
                        }
                    }
                }
            }
            ++depIndex;
        }
        // eliminate any upward links added to befores list by some other dependent
        _initBeforesInfo.removeRedundantUpwards();

        // if this images has initializer(s) (or +load), add it to list
        MachOParser parser(_mh, _dyldCacheIsRaw);
        Diagnostics diag;
        if ( parser.hasInitializer(diag) || parser.hasPlusLoadMethod(diag) ) {
            launch_cache::binary_format::ImageRef ref(0, _groupNum, _indexInGroup);
            _initBeforesInfo.initBefore.push_back(ref);
        }

        //fprintf(stderr, "info for (%d, %d) %s\n",  _group, _index, _runtimePath.c_str());
        //for (ImageRef ref : _initBeforesInfo.initBefore)
        //     fprintf(stderr, "   ref = {%d, %d, %d}\n", ref.kind(), ref.group(), ref.index());
        //for (ImageProxy* p : _initBeforesInfo.danglingUpward)
        //     fprintf(stderr, "   up = %s\n", p->runtimePath().c_str());
    }
}

void ImageProxy::convertInitBeforeInfoToArray(ImageProxyGroup& owningGroup)
{
    if ( _initBeforesInfo.danglingUpward.empty() ) {
        _initBeforesArray = _initBeforesInfo.initBefore;
    }
    else {
        for (ImageRef ref : _initBeforesInfo.initBefore)
            _initBeforesArray.push_back(ref);
        bool inLibSys = inLibSystem();
        for (ImageProxy* proxy : _initBeforesInfo.danglingUpward) {
            // ignore upward dependendencies between stuff within libSystem.dylib
            if ( inLibSys && proxy->inLibSystem() )
                continue;
            proxy->getInitBeforeList(owningGroup);
            for (ImageRef depInit : proxy->_initBeforesInfo.initBefore) {
                if ( std::find(_initBeforesArray.begin(), _initBeforesArray.end(), depInit) == _initBeforesArray.end() )
                    _initBeforesArray.push_back(depInit);
            }
            ImageRef ref(0, proxy->_groupNum, proxy->_indexInGroup);
            if ( std::find(_initBeforesArray.begin(), _initBeforesArray.end(), ref) == _initBeforesArray.end() )
                _initBeforesArray.push_back(ref);
        }
    }
    //fprintf(stderr, "final init-before info for %s\n", _runtimePath.c_str());
    //for (ImageRef ref : _initBeforesArray) {
    //    fprintf(stderr, "   ref = {%d, %d, %d}\n", ref.linkKind, ref.group, ref.index);
    //}
}

const std::vector<ImageRef>& ImageProxy::getInitBeforeList(ImageProxyGroup& owningGroup)
{
    if ( !_initBeforesArraySet ) {
        _initBeforesArraySet = true; // break cycles
        recursiveBuildInitBeforeInfo(owningGroup);
        convertInitBeforeInfoToArray(owningGroup);
    }
    return _initBeforesArray;
}

ImageProxy::FixupInfo ImageProxy::buildFixups(Diagnostics& diag, uint64_t cacheUnslideBaseAddress, launch_cache::ImageGroupWriter& groupWriter) const
{
    __block ImageProxy::FixupInfo info;
    MachOParser image(_mh, _dyldCacheIsRaw);

    // add fixup for each rebase
    __block bool rebaseError = false;
    image.forEachRebase(diag, ^(uint32_t segIndex, uint64_t segOffset, uint8_t type, bool& stop) {
        dyld3::launch_cache::ImageGroupWriter::FixupType fixupType = launch_cache::ImageGroupWriter::FixupType::rebase;
        switch ( type ) {
            case REBASE_TYPE_POINTER:
                fixupType = launch_cache::ImageGroupWriter::FixupType::rebase;
                break;
            case REBASE_TYPE_TEXT_ABSOLUTE32:
                fixupType = launch_cache::ImageGroupWriter::FixupType::rebaseText;
                info.hasTextRelocs = true;
                break;
            case REBASE_TYPE_TEXT_PCREL32:
                diag.error("pcrel text rebasing not supported");
                stop = true;
                rebaseError = true;
                break;
            default:
                diag.error("unknown rebase type");
                stop = true;
                rebaseError = true;
                break;
        }
        info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeInvalid()});
        //fprintf(stderr, "rebase: segIndex=%d, segOffset=0x%0llX, type=%d\n", segIndex, segOffset, type);
    });
    if ( diag.hasError() )
        return FixupInfo();

    // add fixup for each bind
    image.forEachBind(diag, ^(uint32_t segIndex, uint64_t segOffset, uint8_t type, int libOrdinal,
                              uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop) {
        launch_cache::ImageGroupWriter::FixupType fixupType;
        switch ( type ) {
            case BIND_TYPE_POINTER:
                if ( lazy )
                    fixupType = launch_cache::ImageGroupWriter::FixupType::pointerLazyBind;
                else
                    fixupType = launch_cache::ImageGroupWriter::FixupType::pointerBind;
                break;
            case BIND_TYPE_TEXT_ABSOLUTE32:
                fixupType = launch_cache::ImageGroupWriter::FixupType::bindText;
                info.hasTextRelocs = true;
                break;
            case BIND_TYPE_TEXT_PCREL32:
                fixupType = launch_cache::ImageGroupWriter::FixupType::bindTextRel;
                info.hasTextRelocs = true;
                break;
            case BIND_TYPE_IMPORT_JMP_REL32:
                fixupType = launch_cache::ImageGroupWriter::FixupType::bindImportJmpRel;
                break;
           default:
                diag.error("malformed executable, unknown bind type (%d)", type);
                stop = true;
                return;
        }
        const ImageProxy*    depProxy    = nullptr;
        bool                isWeakDylib = false;
        if ( libOrdinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE ) {
            // -bundle_loader symbols cannot be bound ahead of time, we must look them up at load time
            uint32_t imagePathPoolOffset   =  groupWriter.addString("@main");
            uint32_t imageSymbolPoolOffset =  groupWriter.addString(symbolName);
            info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeDynamicGroupValue(imagePathPoolOffset, imageSymbolPoolOffset, weakImport)});
            return;
        }
        else if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
            // -dynamic_lookup symbols cannot be bound ahead of time, we must look them up at load time
            uint32_t imagePathPoolOffset   =  groupWriter.addString("@flat");
            uint32_t imageSymbolPoolOffset =  groupWriter.addString(symbolName);
            info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeDynamicGroupValue(imagePathPoolOffset, imageSymbolPoolOffset, weakImport)});
            return;
        }
        else if ( libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
            depProxy = this;
        }
        else if ( (libOrdinal >= 1) && (libOrdinal <= _dependents.size()) ) {
            isWeakDylib = (_dependentsKind[libOrdinal-1] == LinkKind::weak);
            depProxy    = _dependents[libOrdinal-1];
        }
        else {
            diag.error("ordinal %d not supported", libOrdinal);
            stop = true;
            return;
        }
        if ( depProxy != nullptr ) {
            MachOParser::FoundSymbol foundInfo;
            if ( depProxy->findExportedSymbol(diag, symbolName, foundInfo) ) {
                MachOParser implDylib(foundInfo.foundInDylib, _dyldCacheIsRaw);
                switch ( foundInfo.kind ) {
                    case MachOParser::FoundSymbol::Kind::headerOffset:
                    case MachOParser::FoundSymbol::Kind::resolverOffset:
                        if ( implDylib.inDyldCache() ) {
                            uint32_t cacheOffset = (uint32_t)(implDylib.preferredLoadAddress() + foundInfo.value - cacheUnslideBaseAddress + addend);
                            info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeSharedCacheOffset(cacheOffset)});
                        }
                        else {
                            ImageProxy* foundProxy   = (ImageProxy*)(foundInfo.foundExtra);
                            bool isIndirectGroupNum  = foundProxy->_groupNum >= 128;
                            uint32_t groupNum        = isIndirectGroupNum ? groupWriter.addIndirectGroupNum(foundProxy->_groupNum) : foundProxy->_groupNum;
                            info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeGroupValue(groupNum, foundProxy->_indexInGroup, foundInfo.value+addend, isIndirectGroupNum)});
                        }
                        break;
                   case MachOParser::FoundSymbol::Kind::absolute:
                        if (((((intptr_t)(foundInfo.value+addend)) << 2) >> 2) != (foundInfo.value+addend)) {
                            diag.error("absolute value %lld not supported", foundInfo.value+addend);
                            stop = true;
                            return;
                        }
                        info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeAbsolute(foundInfo.value+addend)});
                        break;
                }
            }
            else {
                if ( !weakImport ) {
                    diag.error("symbol '%s' not found, expected in '%s'", symbolName, depProxy->runtimePath().c_str());
                    stop = true;
                }
                // mark fixup needs to set fixup location to zero
                info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeAbsolute(0)});
            }
        }
        else {
            if ( isWeakDylib ) {
                // dylib not found and is weak, set pointers into it to zero
                info.fixups.push_back({segIndex, segOffset, fixupType, TargetSymbolValue::makeAbsolute(0)});
            }
            else {
                diag.error("dylib ordinal %d not found and not weak", libOrdinal);
                stop = true;
            }
         }
    });
    if ( diag.hasError() )
        return FixupInfo();

    uint32_t weakDefPathPoolOffset = groupWriter.addString("@weak_def");
    image.forEachWeakDef(diag, ^(bool strongDef, uint32_t segIndex, uint64_t segOffset, uint64_t addend, const char* symbolName, bool& stop) {
        if ( strongDef )
            return;
        // find fixup for that location and change it to be a @weakdef dynamic target
        bool altered = false;
        for (FixUp& fixup : info.fixups) {
            if ( (fixup.segOffset == segOffset) && (fixup.segIndex == segIndex) ) {
                uint32_t symbolPoolOffset =  groupWriter.addString(symbolName);
                fixup.type   = launch_cache::ImageGroupWriter::FixupType::pointerBind;
                fixup.target = TargetSymbolValue::makeDynamicGroupValue(weakDefPathPoolOffset, symbolPoolOffset, false);
                altered = true;
            }
        }
        if ( !altered ) {
            if ( image.isSlideable() ) {
                fprintf(stderr, "weak def for %s can't find underlying rebase/bind in %s\n", symbolName, runtimePath().c_str());
            }
            else {
                // no-pie executable does not have rebase for weak-def fixup, so add fixup
                uint32_t symbolPoolOffset =  groupWriter.addString(symbolName);
                info.fixups.push_back({segIndex, segOffset, launch_cache::ImageGroupWriter::FixupType::pointerBind, TargetSymbolValue::makeDynamicGroupValue(weakDefPathPoolOffset, symbolPoolOffset, false)} );
            }
        }

    });

    return info;
}


void ImageProxy::setOverrideOf(uint32_t groupNum, uint32_t indexInGroup)
{
    _overrideOf = ImageRef(0, groupNum, indexInGroup);
}


static bool alreadyInList(const std::vector<ImageProxy*>& imageList, ImageProxy* image)
{
    for (ImageProxy* proxy : imageList) {
        if ( proxy == image )
            return true;
    }
    return false;
}

void ImageProxy::addToFlatLookup(std::vector<ImageProxy*>& imageList)
{
    // add all images shallow
    bool addedSomething = false;
    for (ImageProxy* dep : _dependents) {
        if ( dep == nullptr )
            continue;
        if ( !alreadyInList(imageList, dep) ) {
            imageList.push_back(dep);
            addedSomething = true;
        }
    }
    // recurse
    if ( addedSomething ) {
        for (ImageProxy* dep : _dependents) {
            if ( dep == nullptr )
                continue;
            dep->addToFlatLookup(imageList);
        }
    }
}


///////////////////////////  ImageProxyGroup  ///////////////////////////


class StringPool
{
public:
    uint32_t            add(const std::string& str);
    size_t              size() const   { return _buffer.size(); }
    const char*         buffer() const { return &_buffer[0]; }
    void                align();
private:
    std::vector<char>                           _buffer;
    std::unordered_map<std::string, uint32_t>   _existingEntries;
};

uint32_t StringPool::add(const std::string& str)
{
    auto pos = _existingEntries.find(str);
    if ( pos != _existingEntries.end() )
        return pos->second;
    size_t len = str.size() + 1;
    size_t offset = _buffer.size();
    _buffer.insert(_buffer.end(), &str[0], &str[len]);
    _existingEntries[str] = (uint32_t)offset;
    assert(offset < 0xFFFF);
    return (uint32_t)offset;
}

void StringPool::align()
{
    while ( (_buffer.size() % 4) != 0 )
        _buffer.push_back('\0');
}

ImageProxyGroup::ImageProxyGroup(uint32_t groupNum, const DyldCacheParser& dyldCache, const launch_cache::binary_format::ImageGroup* basedOn,
                                 ImageProxyGroup* next, const std::string& mainProgRuntimePath,
                                 const std::vector<const BinaryImageGroupData*>& knownGroups,
                                 const std::vector<std::string>& buildTimePrefixes,
                                 const std::vector<std::string>& envVars,
                                 bool stubsEliminated, bool dylibsExpectedOnDisk, bool inodesAreSameAsRuntime)
    : _pathOverrides(envVars), _patchTable(nullptr), _basedOn(basedOn), _dyldCache(dyldCache), _nextSearchGroup(next), _groupNum(groupNum),
      _stubEliminated(stubsEliminated), _dylibsExpectedOnDisk(dylibsExpectedOnDisk), _inodesAreSameAsRuntime(inodesAreSameAsRuntime),
      _knownGroups(knownGroups), _buildTimePrefixes(buildTimePrefixes), _mainProgRuntimePath(mainProgRuntimePath), _platform(Platform::unknown)
{
    _archName = dyldCache.cacheHeader()->archName();
    _platform = (Platform)(dyldCache.cacheHeader()->platform());
}


ImageProxyGroup::~ImageProxyGroup()
{
    for (DyldSharedCache::MappedMachO& mapping : _ownedMappings ) {
        vm_deallocate(mach_task_self(), (vm_address_t)mapping.mh, mapping.length);
    }
    for (ImageProxy* proxy : _images) {
        delete proxy;
    }
}


std::string ImageProxyGroup::normalizedPath(const std::string& path)
{
    for (const std::string& prefix : _buildTimePrefixes) {
        std::string fullPath = prefix + path;
        if ( fileExists(fullPath) ) {
            if ( (fullPath.find("/../") != std::string::npos) || (fullPath.find("//") != std::string::npos) || (fullPath.find("/./") != std::string::npos) ) {
                char resolvedPath[PATH_MAX];
                if ( realpath(fullPath.c_str(), resolvedPath) != nullptr ) {
                    std::string resolvedUnPrefixed = &resolvedPath[prefix.size()];
                    return resolvedUnPrefixed;
                }
            }
            break;
        }
    }
    return path;
}


ImageProxy* ImageProxyGroup::findImage(Diagnostics& diag, const std::string& runtimeLoadPath, bool canBeMissing, ImageProxy::RPathChain* rChain)
{
    __block ImageProxy* result = nullptr;
    _pathOverrides.forEachPathVariant(runtimeLoadPath.c_str(), _platform, ^(const char* possiblePath, bool& stop) {
        if ( startsWith(possiblePath, "@rpath/") ) {
            std::string trailing = &possiblePath[6];
            for (const ImageProxy::RPathChain* cur=rChain; cur != nullptr; cur = cur->prev) {
                for (const std::string& rpath : cur->rpaths) {
                    std::string aPath = rpath + trailing;
                    result = findAbsoluteImage(diag, aPath, true, false);
                    if ( result != nullptr ) {
                        _pathToProxy[runtimeLoadPath] = result;
                        stop = true;
                        return;
                    }
                }
            }
            // if cannot be found via current stack of rpaths, check if already found
            auto pos = _pathToProxy.find(possiblePath);
            if ( pos != _pathToProxy.end() ) {
                result = pos->second;
                stop = true;
                return;
            }
        }
        else if ( startsWith(possiblePath, "@loader_path/") ) {
            std::string loaderFile = rChain->inProxy->runtimePath();
            size_t lastSlash = loaderFile.rfind('/');
            if ( lastSlash != std::string::npos ) {
                std::string loaderDir = loaderFile.substr(0, lastSlash);
                std::string newPath = loaderDir + &possiblePath[12];
                result = findAbsoluteImage(diag, newPath, canBeMissing, false);
                if ( result != nullptr ) {
                    _pathToProxy[runtimeLoadPath] = result;
                    stop = true;
                    return;
                }
            }
        }
        else if ( startsWith(possiblePath, "@executable_path/") ) {
            for (const ImageProxy::RPathChain* cur=rChain; cur != nullptr; cur = cur->prev) {
                if ( cur->inProxy->mh()->filetype == MH_EXECUTE ) {
                    std::string mainProg = cur->inProxy->runtimePath();
                    size_t lastSlash = mainProg.rfind('/');
                    if ( lastSlash != std::string::npos ) {
                        std::string mainDir = mainProg.substr(0, lastSlash);
                        std::string newPath = mainDir + &possiblePath[16];
                        result = findAbsoluteImage(diag, newPath, canBeMissing, false);
                        if ( result != nullptr ) {
                            _pathToProxy[runtimeLoadPath] = result;
                            stop = true;
                            return;
                        }
                    }
                }
            }
        }
        else {
            // load command is full path to dylib
            result = findAbsoluteImage(diag, possiblePath, canBeMissing, false);
            if ( result != nullptr ) {
                stop = true;
                return;
            }
        }
    });

    // when building closure, check if an added dylib is an override for something in the cache
    if ( (result != nullptr) && (_groupNum > 1) && !result->isProxyForCachedDylib() ) {
        for (ImageProxyGroup* grp = this; grp != nullptr; grp = grp->_nextSearchGroup) {
            if ( grp->_basedOn == nullptr )
                continue;
            uint32_t indexInGroup;
            launch_cache::ImageGroup imageGroup(grp->_basedOn);
            if ( imageGroup.findImageByPath(runtimeLoadPath.c_str(), indexInGroup) ) {
                result->setOverrideOf(imageGroup.groupNum(), indexInGroup);
                break;
            }
        }
    }

    return result;
}


bool ImageProxyGroup::builtImageStillValid(const launch_cache::Image& image)
{
    // only do checks when running on system
    if ( _buildTimePrefixes.size() != 1 )
        return true;
    if ( _buildTimePrefixes.front().size() != 0 )
        return true;
    if ( _platform != MachOParser::currentPlatform() )
        return true;

    struct stat statBuf;
    bool expectedOnDisk   = image.group().dylibsExpectedOnDisk();
    bool overridableDylib = image.overridableDylib();
    bool cachedDylib      = !image.isDiskImage();
    bool fileFound        = ( ::stat(image.path(), &statBuf) == 0 );

    if ( cachedDylib ) {
        if ( expectedOnDisk ) {
            if ( fileFound ) {
                // macOS case: verify dylib file info matches what it was when cache was built
                return ( (image.fileModTime() == statBuf.st_mtime) && (image.fileINode() == statBuf.st_ino) );
            }
            else {
                // macOS case: dylib missing
                return false;
            }
        }
        else {
            if ( fileFound ) {
                if ( overridableDylib ) {
                    // iOS case: internal install with dylib root
                    return false;
                }
                else {
                    // iOS case: customer install, ignore dylib on disk
                    return true;
                }
            }
            else {
                // iOS case: cached dylib not on disk as expected
                return true;
            }
        }
    }
    else {
        if ( fileFound ) {
            if ( image.validateUsingModTimeAndInode() ) {
                // macOS case: verify dylib file info matches what it was when cache was built
                return ( (image.fileModTime() == statBuf.st_mtime) && (image.fileINode() == statBuf.st_ino) );
            }
            else {
                // FIXME: need to verify file cdhash
                return true;
            }
        }
        else {
            // dylib not on disk as expected
            return false;
        }
    }
}

ImageProxy* ImageProxyGroup::findAbsoluteImage(Diagnostics& diag, const std::string& runtimeLoadPath, bool canBeMissing, bool makeErrorMessage, bool pathIsAlreadyReal)
{
    auto pos = _pathToProxy.find(runtimeLoadPath);
    if ( pos != _pathToProxy.end() )
        return pos->second;

    // see if this ImageProxyGroup is a proxy for an ImageGroup from the dyld shared cache
    if ( _basedOn != nullptr ) {
        uint32_t foundIndex;
        launch_cache::ImageGroup imageGroup(_basedOn);
        if ( imageGroup.findImageByPath(runtimeLoadPath.c_str(), foundIndex) ) {
            launch_cache::Image image = imageGroup.image(foundIndex);
            if ( builtImageStillValid(image) ) {
                ImageProxy* proxy = nullptr;
                if ( _groupNum == 0 ) {
                    const mach_header* mh = (mach_header*)((uint8_t*)(_dyldCache.cacheHeader()) + image.cacheOffset());
                    proxy = new ImageProxy(mh, image.binaryData(), foundIndex, _dyldCache.cacheIsMappedRaw());
                }
                else {
                    DyldSharedCache::MappedMachO* mapping = addMappingIfValidMachO(diag, runtimeLoadPath);
                    if ( mapping != nullptr ) {
                        proxy = new ImageProxy(*mapping, _groupNum, foundIndex, false);
                    }
                }
                if ( proxy != nullptr ) {
                    _pathToProxy[runtimeLoadPath] = proxy;
                    _images.push_back(proxy);
                    if ( runtimeLoadPath != image.path() ) {
                        // lookup path is an alias, add real path too
                        _pathToProxy[image.path()] = proxy;
                    }
                    return proxy;
                }
            }
        }
    }

    if ( _nextSearchGroup != nullptr ) {
        ImageProxy* result = _nextSearchGroup->findAbsoluteImage(diag, runtimeLoadPath, true, false);
        if ( result != nullptr )
            return result;
    }

    // see if this is a symlink to a dylib
    if ( !pathIsAlreadyReal ) {
        for (const std::string& prefix : _buildTimePrefixes) {
            std::string fullPath = prefix + runtimeLoadPath;
            if ( endsWith(prefix, "/") )
                fullPath = prefix.substr(0, prefix.size()-1) + runtimeLoadPath;
            if ( fileExists(fullPath) ) {
                std::string resolvedPath = realFilePath(fullPath);
                if ( !resolvedPath.empty() && (resolvedPath!= fullPath) ) {
                    std::string resolvedRuntimePath = resolvedPath.substr(prefix.size());
                    ImageProxy* proxy = findAbsoluteImage(diag, resolvedRuntimePath, true, false, true);
                    if ( proxy != nullptr )
                        return proxy;
                }
            }
        }
    }

    if ( (_groupNum >= 2) && (_basedOn == nullptr) ) {
        if ( (runtimeLoadPath[0] != '/') && (runtimeLoadPath[0] != '@') ) {
            for (ImageProxy* aProxy : _images) {
                if ( endsWith(aProxy->runtimePath(), runtimeLoadPath) ) {
                    aProxy->setCwdMustBeThisDir();
                    return aProxy;
                }
            }
        }

        DyldSharedCache::MappedMachO* mapping = addMappingIfValidMachO(diag, runtimeLoadPath);
        if ( mapping != nullptr ) {
            ImageProxy* proxy = new ImageProxy(*mapping, _groupNum, (uint32_t)_images.size(), false);
            _pathToProxy[runtimeLoadPath] = proxy;
            _images.push_back(proxy);
            return proxy;
        }
    }

    if ( !canBeMissing && makeErrorMessage ) {
        if ( diag.warnings().empty() ) {
            if ( diag.hasError() ) {
                std::string orgMsg = diag.errorMessage();
                diag.error("'%s' %s", runtimeLoadPath.c_str(), orgMsg.c_str());
            }
            else {
                diag.error("could not find '%s'", runtimeLoadPath.c_str());
            }
        }
        else {
            std::string allTries;
            for (const std::string& warn : diag.warnings()) {
                if ( allTries.empty() )
                    allTries = warn;
                else
                    allTries = allTries + ", " + warn;
            }
            diag.clearWarnings();
            diag.error("could not use '%s'. Did try: %s", runtimeLoadPath.c_str(), allTries.c_str());
        }
    }

    // record locations not found so it can be verified they are still missing at runtime
    _mustBeMissingFiles.insert(runtimeLoadPath);

    return nullptr;
}


DyldSharedCache::MappedMachO* ImageProxyGroup::addMappingIfValidMachO(Diagnostics& diag, const std::string& runtimePath, bool ignoreMainExecutables)
{
    bool fileFound = false;
    for (const std::string& prefix : _buildTimePrefixes) {
        std::string fullPath = prefix + runtimePath;
        struct stat statBuf;
        if ( stat(fullPath.c_str(), &statBuf) != 0 )
            continue;
        fileFound = true;
        // map whole file and determine if it is mach-o or a fat file
        int fd = ::open(fullPath.c_str(), O_RDONLY);
        if ( fd < 0 ) {
            diag.warning("file not open()able '%s' errno=%d", fullPath.c_str(), errno);
            continue;
        }
        size_t len = (size_t)statBuf.st_size;
        size_t offset = 0;
        const void* p = ::mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
        if ( p != MAP_FAILED ) {
            size_t sliceLen;
            size_t sliceOffset;
            bool missingSlice;
            Diagnostics fatDiag;
            if ( FatUtil::isFatFileWithSlice(fatDiag, p, len, _archName, sliceOffset, sliceLen, missingSlice) ) {
                // unmap whole file
                ::munmap((void*)p, len);
                // remap just slice
                p = ::mmap(NULL, sliceLen, PROT_READ, MAP_PRIVATE, fd, sliceOffset);
                if ( p != MAP_FAILED ) {
                    offset = sliceOffset;
                    len    = sliceLen;
                }
            }
            else if ( fatDiag.hasError() ) {
                diag.warning("%s", fatDiag.errorMessage().c_str());
            }
            if ( (p != MAP_FAILED) && !missingSlice && MachOParser::isValidMachO(diag, _archName, _platform, p, len, fullPath, ignoreMainExecutables) ) {
                bool issetuid = (statBuf.st_mode & (S_ISUID|S_ISGID));
                bool sip = false; // FIXME
                _ownedMappings.emplace_back(runtimePath, (mach_header*)p, len, issetuid, sip, offset, statBuf.st_mtime, statBuf.st_ino);
                ::close(fd);
                return &_ownedMappings.back();
            }
            else if (p != MAP_FAILED) {
                ::munmap((void*)p, len);
            }
        }
        ::close(fd);
    }
    if ( !fileFound )
        diag.warning("file not found '%s'", runtimePath.c_str());

    return nullptr;
}

static bool dontExamineDir(const std::string& dirPath)
{
    return endsWith(dirPath, ".app") || endsWith(dirPath, ".xctoolchain") || endsWith(dirPath, ".sdk") || endsWith(dirPath, ".platform");
}

void ImageProxyGroup::addExtraMachOsInBundle(const std::string& appDir)
{
    iterateDirectoryTree("", appDir, ^(const std::string& dirPath) { return dontExamineDir(dirPath); }, ^(const std::string& path, const struct stat& statBuf) {
        // ignore files that don't have 'x' bit set (all runnable mach-o files do)
        const bool hasXBit = ((statBuf.st_mode & S_IXOTH) == S_IXOTH);
        if ( !hasXBit )
            return;

        // ignore files too small
        if ( statBuf.st_size < 0x1000 )
            return;

        // if the file is mach-o, add to list
        if ( _pathToProxy.find(path) == _pathToProxy.end() ) {
            Diagnostics  machoDiag;
            DyldSharedCache::MappedMachO* mapping = addMappingIfValidMachO(machoDiag, path, true);
            if ( mapping != nullptr ) {
                ImageProxy* proxy = new ImageProxy(*mapping, _groupNum, (uint32_t)_images.size(), false);
                if ( proxy != nullptr ) {
                    _pathToProxy[path] = proxy;
                    _images.push_back(proxy);
                }
            }
        }
    });
}

// used when building dyld shared cache
ImageProxyGroup* ImageProxyGroup::makeDyldCacheDylibsGroup(Diagnostics& diag, const DyldCacheParser& dyldCache,
                                                           const std::vector<DyldSharedCache::MappedMachO>& cachedDylibs,
                                                           const std::vector<std::string>& buildTimePrefixes,
                                                           const PatchTable& patchTable, bool stubEliminated, bool dylibsExpectedOnDisk)
{
    std::vector<std::string> emptyEnvVars; // Note: this method only used when constructing dyld cache where envs are not used
    std::vector<const BinaryImageGroupData*> noExistingGroups;
    ImageProxyGroup* groupProxy = new ImageProxyGroup(0, dyldCache, nullptr, nullptr, "", noExistingGroups, buildTimePrefixes, emptyEnvVars, stubEliminated, dylibsExpectedOnDisk);
    groupProxy->_patchTable = &patchTable;

    // add every dylib in shared cache to _images
    uint32_t indexInGroup = 0;
    for (const DyldSharedCache::MappedMachO& mapping : cachedDylibs) {
        ImageProxy* proxy = new ImageProxy(mapping, 0, indexInGroup++, true);
        groupProxy->_images.push_back(proxy);
        groupProxy->_pathToProxy[mapping.runtimePath] = proxy;
    }

    // verify libdyld is compatible
    ImageRef libdyldEntryImageRef = ImageRef::makeEmptyImageRef();
    uint32_t libdyldEntryOffset;
    groupProxy->findLibdyldEntry(diag, libdyldEntryImageRef, libdyldEntryOffset);
    if ( diag.hasError() ) {
        delete groupProxy;
        return nullptr;
    }

    // wire up dependents
    bool hadError = false;
    for (size_t i=0; i < groupProxy->_images.size(); ++i) {
        // note: addDependentsShallow() can append to _images, so can't use regular iterator
        ImageProxy* proxy = groupProxy->_images[i];
        proxy->addDependentsShallow(*groupProxy);
        if ( proxy->diagnostics().hasError() ) {
            hadError = true;
            diag.copy(proxy->diagnostics());
            break;
        }
    }

    if ( hadError ) {
        delete groupProxy;
        return nullptr;
    }

    return groupProxy;
}


// used when building dyld shared cache
ImageProxyGroup* ImageProxyGroup::makeOtherOsGroup(Diagnostics& diag, const DyldCacheParser& dyldCache, ImageProxyGroup* cachedDylibsGroup,
                                                   const std::vector<DyldSharedCache::MappedMachO>& otherDylibsAndBundles,
                                                   bool inodesAreSameAsRuntime, const std::vector<std::string>& buildTimePrefixes)
{
    std::vector<std::string> emptyEnvVars; // Note: this method only used when constructing dyld cache where envs are not used
    const BinaryImageGroupData* cachedDylibsGroupData = dyldCache.cachedDylibsGroup();
    std::vector<const BinaryImageGroupData*> existingGroups = { cachedDylibsGroupData };
    ImageProxyGroup          dyldCacheDylibProxyGroup(0, dyldCache, cachedDylibsGroupData, nullptr,           "", existingGroups, buildTimePrefixes, emptyEnvVars);
    ImageProxyGroup* groupProxy = new ImageProxyGroup(1, dyldCache, nullptr,               cachedDylibsGroup, "", existingGroups, buildTimePrefixes, emptyEnvVars,
                                                      false, true, inodesAreSameAsRuntime);

    // add every dylib/bundle in "other: list to _images
    uint32_t indexInGroup = 0;
    for (const DyldSharedCache::MappedMachO& mapping : otherDylibsAndBundles) {
        ImageProxy* proxy = new ImageProxy(mapping, 1, indexInGroup++, true);
        groupProxy->_images.push_back(proxy);
        groupProxy->_pathToProxy[mapping.runtimePath] = proxy;
    }

    // wire up dependents
    for (size_t i=0; i < groupProxy->_images.size(); ++i) {
        // note: addDependentsShallow() can append to _images, so can't use regular iterator
        ImageProxy* proxy = groupProxy->_images[i];
        // note: other-dylibs can only depend on dylibs in this group or group 0, so no need for deep dependents
        proxy->addDependentsShallow(*groupProxy);
        if ( proxy->diagnostics().hasError() ) {
            diag.warning("adding dependents to %s: %s", proxy->runtimePath().c_str(), proxy->diagnostics().errorMessage().c_str());
            proxy->markInvalid();
        }
    }
    // propagate invalidness
    __block bool somethingInvalid;
    do {
        somethingInvalid = false;
        for (ImageProxy* proxy : groupProxy->_images) {
            proxy->forEachDependent(^(ImageProxy* dep, LinkKind) {
                if ( (dep != nullptr) && dep->invalid() && !proxy->invalid()) {
                    proxy->markInvalid();
                    somethingInvalid = true;
                }
            });
        }
    } while (somethingInvalid);

    return groupProxy;
}

// used by closured for dlopen of unknown dylibs
const BinaryImageGroupData* ImageProxyGroup::makeDlopenGroup(Diagnostics& diag, const DyldCacheParser& dyldCache, uint32_t groupNum,
                                                             const std::vector<const BinaryImageGroupData*>& existingGroups,
                                                             const std::string& imagePath, const std::vector<std::string>& envVars)
{
    const std::vector<std::string>& noBuildTimePrefixes = {""};
    ImageProxyGroup dyldCacheDylibProxyGroup(0, dyldCache, existingGroups[0], nullptr,                   "",        existingGroups, noBuildTimePrefixes, envVars);
    ImageProxyGroup dyldCacheOtherProxyGroup(1, dyldCache, nullptr,           &dyldCacheDylibProxyGroup, "",        existingGroups, noBuildTimePrefixes, envVars);
    ImageProxyGroup dlopenGroupProxy(groupNum,  dyldCache, nullptr,           &dyldCacheOtherProxyGroup, imagePath, existingGroups, noBuildTimePrefixes, envVars, false, true, true);

    DyldSharedCache::MappedMachO* topMapping = dlopenGroupProxy.addMappingIfValidMachO(diag, imagePath, true);
    if ( topMapping == nullptr ) {
        if ( diag.noError() ) {
            const std::set<std::string>& warnings = diag.warnings();
            if ( warnings.empty() )
                diag.error("no loadable mach-o in %s", imagePath.c_str());
            else
                diag.error("%s", (*warnings.begin()).c_str());
        }
        return nullptr;
    }

    ImageProxy* topImageProxy = new ImageProxy(*topMapping, groupNum, 0, false);
    if ( topImageProxy == nullptr ) {
        diag.error("can't find slice matching dyld cache in %s", imagePath.c_str());
        return nullptr;
    }
    dlopenGroupProxy._images.push_back(topImageProxy);
    dlopenGroupProxy._pathToProxy[imagePath] = topImageProxy;

    // add all dylibs needed by dylib and are not in dyld cache
    topImageProxy->addDependentsDeep(dlopenGroupProxy, nullptr, false);
    if ( topImageProxy->diagnostics().hasError() ) {
        diag.copy(topImageProxy->diagnostics());
        return nullptr;
    }

    const BinaryImageGroupData* result = dlopenGroupProxy.makeImageGroupBinary(diag);

    return result;
}


// used when building dyld shared cache
BinaryClosureData* ImageProxyGroup::makeClosure(Diagnostics& diag, const DyldCacheParser& dyldCache, ImageProxyGroup* cachedDylibsGroup,
                                                ImageProxyGroup* otherOsDylibs, const DyldSharedCache::MappedMachO& mainProgMapping,
                                                bool inodesAreSameAsRuntime, const std::vector<std::string>& buildTimePrefixes)
{
    // _basedOn can not be set until ImageGroup is built
    if ( cachedDylibsGroup->_basedOn == nullptr ) {
        cachedDylibsGroup->_basedOn = dyldCache.cachedDylibsGroup();
    }
    const BinaryImageGroupData* cachedDylibsGroupData = dyldCache.cachedDylibsGroup();
    const BinaryImageGroupData* otherDylibsGroupData = dyldCache.otherDylibsGroup();
    std::vector<const BinaryImageGroupData*> existingGroups = { cachedDylibsGroupData, otherDylibsGroupData };
    std::vector<std::string> emptyEnvVars; // Note: this method only used when constructing dyld cache where envs are not used
    ImageProxyGroup mainClosureGroupProxy(2, dyldCache, nullptr, otherOsDylibs, mainProgMapping.runtimePath, existingGroups, buildTimePrefixes,
                                          emptyEnvVars, false, true, inodesAreSameAsRuntime);

    ImageProxy* mainProxy = new ImageProxy(mainProgMapping, 2, 0, true);
    if ( mainProxy == nullptr ) {
        diag.error("can't find slice matching dyld cache in %s", mainProgMapping.runtimePath.c_str());
        return nullptr;
    }
    mainClosureGroupProxy._images.push_back(mainProxy);
    mainClosureGroupProxy._pathToProxy[mainProgMapping.runtimePath] = mainProxy;

    return mainClosureGroupProxy.makeClosureBinary(diag, mainProxy, false);
}


bool ImageProxyGroup::addInsertedDylibs(Diagnostics& diag)
{
    __block bool success = true;
    _pathOverrides.forEachInsertedDylib(^(const char* dylibPath) {
        ImageProxy* insertProxy = findAbsoluteImage(diag, dylibPath, false, true);
        if ( insertProxy == nullptr )
            success = false;
    });
    return success;
}

static DyldCacheParser findDyldCache(Diagnostics& diag, const ClosureBuffer::CacheIdent& cacheIdent, task_t requestor, bool* dealloc)
{
    *dealloc = false;
#if !defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || (__MAC_OS_X_VERSION_MIN_REQUIRED >= 101300)
    size_t currentCacheSize;
    const DyldSharedCache* currentCache = (const DyldSharedCache*)_dyld_get_shared_cache_range(&currentCacheSize);
    if ( currentCache != nullptr ) {
        uuid_t currentCacheUUID;
        currentCache->getUUID(currentCacheUUID);
        if ( memcmp(currentCacheUUID, cacheIdent.cacheUUID, 16) == 0 ) 
            return DyldCacheParser((const DyldSharedCache*)currentCache, false);
    }
#endif
    if ( requestor == mach_task_self() ) {
        // handle dyld_closure_util case where -cache_file option maps raw cache file into this process
        const DyldSharedCache* altCache = (DyldSharedCache*)cacheIdent.cacheAddress;
        uuid_t altCacheUUID;
        altCache->getUUID(altCacheUUID);
        if ( memcmp(altCacheUUID, cacheIdent.cacheUUID, 16) == 0 )
            return DyldCacheParser(altCache, true); // only one cache can be mapped into process, so this must be raw
        else
            diag.error("dyld cache uuid has changed");
    }
#if BUILDING_CLOSURED
    else {
        // handle case where requestor to closured is running with a different dyld cache that closured
        uint8_t cacheBuffer[4096];
        mach_vm_size_t actualReadSize = sizeof(cacheBuffer);
        kern_return_t r;
        r = mach_vm_read_overwrite(requestor, cacheIdent.cacheAddress, sizeof(cacheBuffer), (vm_address_t)&cacheBuffer, &actualReadSize);
        if ( r != KERN_SUCCESS ) {
            diag.error("unable to read cache header from requesting process (addr=0x%llX), kern err=%d", cacheIdent.cacheAddress, r);
            return DyldCacheParser(nullptr, false);
        }
        const dyld_cache_header* header = (dyld_cache_header*)cacheBuffer;
        const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)(cacheBuffer + header->mappingOffset);
        vm_address_t bufferAddress = 0;
        r = vm_allocate(mach_task_self(), &bufferAddress, (long)cacheIdent.cacheMappedSize, VM_FLAGS_ANYWHERE);
        if ( r != KERN_SUCCESS ) {
            diag.error("unable to allocate space to copy custom dyld cache (size=0x%llX), kern err=%d", cacheIdent.cacheMappedSize, r);
            return DyldCacheParser(nullptr, false);
        }
        uint64_t slide =  cacheIdent.cacheAddress - mappings[0].address;
        for (int i=0; i < 3; ++i) {
            mach_vm_address_t mappedAddress = bufferAddress + (mappings[i].address - mappings[0].address);
            mach_vm_size_t    mappedSize    = mappings[i].size;
            vm_prot_t         curProt       = VM_PROT_READ;
            vm_prot_t         maxProt       = VM_PROT_READ;
            r = mach_vm_remap(mach_task_self(), &mappedAddress, mappedSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                              requestor, mappings[i].address+slide, true, &curProt, &maxProt, VM_INHERIT_NONE);
            if ( r != KERN_SUCCESS ) {
                 diag.error("unable to mach_vm_remap region %d custom dyld cache (request addr=0x%llX, size=0x%llX), kern err=%d, localBuffer=0x%lX, localMapTarget=0x%llX",
                           i, mappings[i].address+slide, mappedSize, r, (long)bufferAddress, mappedAddress);
                 return DyldCacheParser(nullptr, false);
            }
            if ( curProt != VM_PROT_READ )
                vm_protect(mach_task_self(), (long)mappedAddress, (long)mappedSize, false, VM_PROT_READ);
       }
       *dealloc = true;
       return DyldCacheParser((DyldSharedCache*)bufferAddress, false);  // assumes cache in other process is mapped as three regions
    }
#endif
    return DyldCacheParser(nullptr, false);
}

BinaryClosureData* ImageProxyGroup::makeClosure(Diagnostics& diag, const ClosureBuffer& buffer, task_t requestor, const std::vector<std::string>& buildTimePrefixes)
{
    // unpack buffer
    bool deallocCacheCopy;
    DyldCacheParser dyldCache = findDyldCache(diag, buffer.cacheIndent(), requestor, &deallocCacheCopy);
    if ( diag.hasError() )
        return nullptr;
    const char* mainProg = buffer.targetPath();
    std::vector<std::string> envVars;
    int envCount = buffer.envVarCount();
    const char* envVarCStrings[envCount];
    buffer.copyImageGroups(envVarCStrings);
    for (int i=0; i < envCount; ++i) {
        envVars.push_back(envVarCStrings[i]);
    }

    // make ImageProxyGroups: 0, 1, 2
    const BinaryImageGroupData* cachedDylibsGroupData = dyldCache.cachedDylibsGroup();
    const BinaryImageGroupData* otherDylibsGroupData  = dyldCache.otherDylibsGroup();
    std::vector<std::string> realBuildTimePrefixes;
    for (const std::string& prefix : buildTimePrefixes)  {
        char resolvedPath[PATH_MAX];
        if ( realpath(prefix.c_str(), resolvedPath) != nullptr )
            realBuildTimePrefixes.push_back(resolvedPath);
        else
            realBuildTimePrefixes.push_back(prefix);
    }
    std::vector<const BinaryImageGroupData*> existingGroups = { cachedDylibsGroupData, otherDylibsGroupData };
    ImageProxyGroup dyldCacheDylibProxyGroup(0, dyldCache, cachedDylibsGroupData, nullptr,                   "",       existingGroups, realBuildTimePrefixes, envVars);
    ImageProxyGroup dyldCacheOtherProxyGroup(1, dyldCache, otherDylibsGroupData,  &dyldCacheDylibProxyGroup, "",       existingGroups, realBuildTimePrefixes, envVars);
    ImageProxyGroup mainClosureGroupProxy(   2, dyldCache, nullptr,               &dyldCacheOtherProxyGroup, mainProg, existingGroups, realBuildTimePrefixes, envVars, false, true, true);

    // add any DYLD_INSERTED_LIBRARIES then main program into closure
    BinaryClosureData* result = nullptr;
    if ( mainClosureGroupProxy.addInsertedDylibs(diag) ) {
        ImageProxy* proxy = mainClosureGroupProxy.findAbsoluteImage(diag, mainProg, false, true);
        if ( proxy != nullptr ) {
            // build closure
            result = mainClosureGroupProxy.makeClosureBinary(diag, proxy, false);
        }
    }

    // if client has a different cache, unmap our copy
    if ( deallocCacheCopy )
        vm_deallocate(mach_task_self(), (vm_address_t)dyldCache.cacheHeader(), (long)buffer.cacheIndent().cacheMappedSize);

    return result;
}

ClosureBuffer closured_CreateImageGroup(const ClosureBuffer& input)
{
    Diagnostics diag;
    const BinaryImageGroupData* newGroup = ImageProxyGroup::makeDlopenGroup(diag, input, mach_task_self(), {""});

    if ( diag.noError() ) {
        // on success return the ImageGroup binary in the ClosureBuffer
        dyld3::ClosureBuffer result(newGroup);
        free((void*)newGroup);
        return result;
    }
    else {
        // on failure return the error message in the ClosureBuffer
        dyld3::ClosureBuffer err(diag.errorMessage().c_str());
        return err;
    }
}

const BinaryImageGroupData* ImageProxyGroup::makeDlopenGroup(Diagnostics& diag, const ClosureBuffer& buffer, task_t requestor, const std::vector<std::string>& buildTimePrefixes)
{
    // unpack buffer
    bool deallocCacheCopy;
    DyldCacheParser dyldCache = findDyldCache(diag, buffer.cacheIndent(), requestor, &deallocCacheCopy);
    if ( diag.hasError() )
        return nullptr;

    const char* targetDylib = buffer.targetPath();
    std::vector<std::string> envVars;
    int envCount = buffer.envVarCount();
    const char* envVarCStrings[envCount];
    buffer.copyImageGroups(envVarCStrings);
    for (int i=0; i < envCount; ++i) {
        envVars.push_back(envVarCStrings[i]);
    }
    uint32_t groupCount = buffer.imageGroupCount() + 2;
    const launch_cache::BinaryImageGroupData* groupDataPtrs[groupCount];
    groupDataPtrs[0] = dyldCache.cachedDylibsGroup();
    groupDataPtrs[1] = dyldCache.otherDylibsGroup();
    buffer.copyImageGroups(&groupDataPtrs[2]);

    // build an ImageProxyGroup for each existing group, and one for new group being constructed
    std::vector<const launch_cache::BinaryImageGroupData*> existingGroups;
    std::vector<std::unique_ptr<ImageProxyGroup>> proxies;
    ImageProxyGroup* prevProxy = nullptr;
    for (uint32_t i=0; i < groupCount; ++i) {
        const launch_cache::BinaryImageGroupData* groupData = groupDataPtrs[i];
        existingGroups.push_back(groupData);
        launch_cache::ImageGroup group(groupData);
        uint32_t groupNum = group.groupNum();
        assert(groupNum == proxies.size());
        proxies.emplace_back(new ImageProxyGroup(groupNum, dyldCache, groupData, prevProxy, "", existingGroups, buildTimePrefixes, envVars));
        prevProxy = proxies.back().get();
    }
    ImageProxyGroup dlopenGroupProxy(groupCount, dyldCache, nullptr, prevProxy, targetDylib, existingGroups, buildTimePrefixes, envVars);

    // find and mmap() top level dylib
    DyldSharedCache::MappedMachO* topMapping = dlopenGroupProxy.addMappingIfValidMachO(diag, targetDylib, true);
    if ( topMapping == nullptr ) {
        std::string allWarnings;
        for (const std::string& warn : diag.warnings()) {
            if ( allWarnings.empty() )
                allWarnings = warn;
            else
                allWarnings = allWarnings + ", " + warn;
        }
        diag.clearWarnings();
        diag.error("%s", allWarnings.c_str());
        if ( deallocCacheCopy )
            vm_deallocate(mach_task_self(), (vm_address_t)dyldCache.cacheHeader(), (long)buffer.cacheIndent().cacheMappedSize);
        return nullptr;
    }

    // make ImageProxy for top level dylib
    ImageProxy* topImageProxy = new ImageProxy(*topMapping, groupCount, 0, false);
    if ( topImageProxy == nullptr ) {
        diag.error("can't find slice matching dyld cache in %s", targetDylib);
        if ( deallocCacheCopy )
            vm_deallocate(mach_task_self(), (vm_address_t)dyldCache.cacheHeader(), (long)buffer.cacheIndent().cacheMappedSize);
        return nullptr;
    }
    dlopenGroupProxy._images.push_back(topImageProxy);
    dlopenGroupProxy._pathToProxy[targetDylib] = topImageProxy;

    // add all dylibs needed by dylib and are not in dyld cache
    topImageProxy->addDependentsDeep(dlopenGroupProxy, nullptr, false);
    if ( topImageProxy->diagnostics().hasError() ) {
        diag.copy(topImageProxy->diagnostics());
        if ( deallocCacheCopy )
            vm_deallocate(mach_task_self(), (vm_address_t)dyldCache.cacheHeader(), (long)buffer.cacheIndent().cacheMappedSize);
        return nullptr;
    }

    // construct ImageGroup from ImageProxies
    const BinaryImageGroupData* result = dlopenGroupProxy.makeImageGroupBinary(diag);

    // clean up
    if ( deallocCacheCopy )
        vm_deallocate(mach_task_self(), (vm_address_t)dyldCache.cacheHeader(), (long)buffer.cacheIndent().cacheMappedSize);

    return result;
}




// Used by closured and dyld_closure_util
BinaryClosureData* ImageProxyGroup::makeClosure(Diagnostics& diag, const DyldCacheParser& dyldCache,
                                                const std::string& mainProg, bool includeDylibsInDir,
                                                const std::vector<std::string>& buildTimePrefixes,
                                                const std::vector<std::string>& envVars)
{
    const BinaryImageGroupData* cachedDylibsGroupData = dyldCache.cachedDylibsGroup();
    const BinaryImageGroupData* otherDylibsGroupData  = dyldCache.otherDylibsGroup();
    std::vector<std::string> realBuildTimePrefixes;
    for (const std::string& prefix : buildTimePrefixes)  {
        char resolvedPath[PATH_MAX];
        if ( realpath(prefix.c_str(), resolvedPath) != nullptr )
            realBuildTimePrefixes.push_back(resolvedPath);
        else
            realBuildTimePrefixes.push_back(prefix);
    }
    std::vector<const BinaryImageGroupData*> existingGroups = { cachedDylibsGroupData, otherDylibsGroupData };
    ImageProxyGroup dyldCacheDylibProxyGroup(0, dyldCache, cachedDylibsGroupData, nullptr,                   "",       existingGroups, realBuildTimePrefixes, envVars);
    ImageProxyGroup dyldCacheOtherProxyGroup(1, dyldCache, otherDylibsGroupData,  &dyldCacheDylibProxyGroup, "",       existingGroups, realBuildTimePrefixes, envVars);
    ImageProxyGroup mainClosureGroupProxy(   2, dyldCache, nullptr,               &dyldCacheOtherProxyGroup, mainProg, existingGroups, realBuildTimePrefixes, envVars, false, true, true);

    // add any DYLD_INSERTED_LIBRARIES into closure
    if ( !mainClosureGroupProxy.addInsertedDylibs(diag) )
        return nullptr;

    ImageProxy* proxy = mainClosureGroupProxy.findAbsoluteImage(diag, mainProg, false, true);
    if ( proxy == nullptr )
        return nullptr;

    return mainClosureGroupProxy.makeClosureBinary(diag, proxy, includeDylibsInDir);
}

const char* sSkipPrograms_macOS[] = {
    "/Applications/iBooks.app/Contents/MacOS/iBooks",
};

const char* sSkipPrograms_embeddedOSes[] = {
    "/sbin/launchd",
    "/usr/local/sbin/launchd.debug",
    "/usr/local/sbin/launchd.development"
};

BinaryClosureData* ImageProxyGroup::makeClosureBinary(Diagnostics& diag, ImageProxy* mainProgProxy, bool includeDylibsInDir)
{
    assert(mainProgProxy != nullptr);
    assert(_images.size() >= 1);

    // check black list
    if ( _platform == Platform::macOS ) {
        for (const char* skipProg : sSkipPrograms_macOS) {
            if ( mainProgProxy->runtimePath() == skipProg ) {
                diag.error("black listed program");
                return nullptr;
            }
        }
    } else {
        for (const char* skipProg : sSkipPrograms_embeddedOSes) {
            if ( mainProgProxy->runtimePath() == skipProg ) {
                diag.error("black listed program");
                return nullptr;
            }
        }
    }

    _mainExecutableIndex = (uint32_t)_images.size() - 1;
    // add all dylibs needed by main excutable and are not in dyld cache
    mainProgProxy->addDependentsDeep(*this, nullptr, true);
    if ( mainProgProxy->diagnostics().hasError() ) {
        diag.copy(mainProgProxy->diagnostics());
        return nullptr;
    }

    // if main program is in .app bundle, look for other mach-o files to add to closure for use by dlopen
    bool isAppMainExecutable = false;
    std::string appDir;
    std::string leafName = basePath(mainProgProxy->runtimePath());
    size_t posAppX = mainProgProxy->runtimePath().rfind(std::string("/") + leafName + ".appex/");
    size_t posApp  = mainProgProxy->runtimePath().rfind(std::string("/") + leafName + ".app/");
    if (  posAppX != std::string::npos ) {
        appDir = mainProgProxy->runtimePath().substr(0, posAppX+leafName.size()+7);
        isAppMainExecutable = true;
    }
    else if ( posApp != std::string::npos ) {
        appDir = mainProgProxy->runtimePath().substr(0, posApp+leafName.size()+5);
        isAppMainExecutable = true;
    }
    if ( isAppMainExecutable ) {
        addExtraMachOsInBundle(appDir);
        for (size_t i=0; i < _images.size(); ++i) {
            // note: addDependentsDeep() can append to _images, so can't use regular iterator
            ImageProxy* aProxy = _images[i];
            ImageProxy::RPathChain base = { aProxy, nullptr, mainProgProxy->rpaths() };
            aProxy->addDependentsDeep(*this, &base, false);
            if ( aProxy->diagnostics().hasError() ) {
                aProxy->markInvalid();
                diag.warning("%s could not be added to closure because %s", aProxy->runtimePath().c_str(), aProxy->diagnostics().errorMessage().c_str());
            }
        }
    }
    else if ( includeDylibsInDir ) {
        size_t pos = mainProgProxy->runtimePath().rfind('/');
        if ( pos != std::string::npos ) {
            std::string mainDir = mainProgProxy->runtimePath().substr(0, pos);
            addExtraMachOsInBundle(mainDir);
            for (size_t i=0; i < _images.size(); ++i) {
                // note: addDependentsDeep() can append to _images, so can't use regular iterator
                ImageProxy* aProxy = _images[i];
                aProxy->addDependentsDeep(*this, nullptr, false);
            }
        }
    }

    // add addition dependents of any inserted libraries
    if ( _mainExecutableIndex != 0 ) {
        for (uint32_t i=0; i < _mainExecutableIndex; ++i) {
            _images[i]->addDependentsDeep(*this, nullptr, true);
            if ( _images[i]->diagnostics().hasError() )
                return nullptr;
        }
    }

    // gather warnings from all statically dependent images
    for (ImageProxy* proxy : _images) {
        if ( !proxy->staticallyReferenced() && proxy->diagnostics().hasError() )
            continue;
        diag.copy(proxy->diagnostics());
        if ( diag.hasError() ) {
            return nullptr;
        }
    }

    // get program entry
    MachOParser mainExecutableParser(mainProgProxy->mh(), _dyldCache.cacheIsMappedRaw());
    bool usesCRT;
    uint32_t entryOffset;
    mainExecutableParser.getEntry(entryOffset, usesCRT);

    // build ImageGroupWriter
    launch_cache::ImageGroupWriter groupWriter(_groupNum, mainExecutableParser.uses16KPages(), mainExecutableParser.is64(), _dylibsExpectedOnDisk, _inodesAreSameAsRuntime);
    populateGroupWriter(diag, groupWriter);
    if ( diag.hasError() )
        return nullptr;

    // pre-compute libSystem and libdyld into closure
    ImageRef libdyldEntryImageRef = ImageRef::makeEmptyImageRef();
    uint32_t libdyldEntryOffset;
    findLibdyldEntry(diag, libdyldEntryImageRef, libdyldEntryOffset);
    if ( diag.hasError() )
        return nullptr;
    ImageRef libSystemImageRef = ImageRef::makeEmptyImageRef();

    findLibSystem(diag, mainExecutableParser.isSimulatorBinary(), libSystemImageRef);
    if ( diag.hasError() )
        return nullptr;

    // build info about missing files and env vars
    __block StringPool            stringPool;
    __block std::vector<uint32_t> envVarOffsets;
    std::vector<uint16_t>         missingFileComponentOffsets;
    stringPool.add(" ");
    for (const std::string& path : _mustBeMissingFiles) {
        size_t start = 1;
        size_t slashPos = path.find('/', start);
        while (slashPos != std::string::npos) {
            std::string component = path.substr(start, slashPos - start);
            uint16_t offset = stringPool.add(component);
            missingFileComponentOffsets.push_back(offset);
            start = slashPos + 1;
            slashPos = path.find('/', start);
        }
        std::string lastComponent = path.substr(start);
        uint16_t offset = stringPool.add(lastComponent);
        missingFileComponentOffsets.push_back(offset);
        missingFileComponentOffsets.push_back(0);  // mark end of a path
    }
    missingFileComponentOffsets.push_back(0);  // mark end of all paths
    if ( missingFileComponentOffsets.size() & 1 )
        missingFileComponentOffsets.push_back(0);  // 4-byte align array
    __block uint32_t envVarCount = 0;
    _pathOverrides.forEachEnvVar(^(const char* envVar) {
       envVarOffsets.push_back(stringPool.add(envVar));
       ++envVarCount;
    });

    // 4-byte align string pool size
    stringPool.align();

    // malloc a buffer and fill in ImageGroup part
    uint32_t groupSize             = groupWriter.size();
    uint32_t missingFilesArraySize = (uint32_t)((missingFileComponentOffsets.size()*sizeof(uint16_t) + 3) & (-4));
    uint32_t envVarsSize           = (uint32_t)(envVarOffsets.size()*sizeof(uint32_t));
    uint32_t stringPoolSize        = (uint32_t)stringPool.size();
    size_t allocSize = sizeof(launch_cache::binary_format::Closure)
                     + groupSize
                     + missingFilesArraySize
                     + envVarsSize
                     + stringPoolSize;
    BinaryClosureData* clo = (BinaryClosureData*)malloc(allocSize);
    groupWriter.finalizeTo(diag, _knownGroups, &clo->group);
    launch_cache::ImageGroup cloGroup(&clo->group);
    launch_cache::Image      mainImage(cloGroup.imageBinary(_mainExecutableIndex));

    uint32_t maxImageLoadCount = groupWriter.maxLoadCount(diag, _knownGroups, &clo->group);

    if ( mainImage.isInvalid() ) {
        free((void*)clo);
        diag.error("depends on invalid dylib");
        return nullptr;
    }

    // fill in closure attributes
    clo->magic                          = launch_cache::binary_format::Closure::magicV1;
    clo->usesCRT                        = usesCRT;
    clo->isRestricted                   = mainProgProxy->isSetUID() || mainExecutableParser.isRestricted();
    clo->usesLibraryValidation          = mainExecutableParser.usesLibraryValidation();
    clo->padding                        = 0;
    clo->missingFileComponentsOffset    = offsetof(launch_cache::binary_format::Closure, group) + groupSize;
    clo->dyldEnvVarsOffset              = clo->missingFileComponentsOffset + missingFilesArraySize;
    clo->dyldEnvVarsCount               = envVarCount;
    clo->stringPoolOffset               = clo->dyldEnvVarsOffset + envVarsSize;
    clo->stringPoolSize                 = stringPoolSize;
    clo->libSystemRef                   = libSystemImageRef;
    clo->libDyldRef                     = libdyldEntryImageRef;
    clo->libdyldVectorOffset            = libdyldEntryOffset;
    clo->mainExecutableIndexInGroup     = _mainExecutableIndex;
    clo->mainExecutableEntryOffset      = entryOffset;
    clo->initialImageCount              = maxImageLoadCount;
    _dyldCache.cacheHeader()->getUUID(clo->dyldCacheUUID);

    if ( !mainExecutableParser.getCDHash(clo->mainExecutableCdHash) ) {
        // if no code signature, fill in 16-bytes with UUID then 4 bytes of zero
        bzero(clo->mainExecutableCdHash, 20);
        mainExecutableParser.getUuid(clo->mainExecutableCdHash);
    }
    if ( missingFilesArraySize != 0 )
        memcpy((uint8_t*)clo + clo->missingFileComponentsOffset, &missingFileComponentOffsets[0], missingFileComponentOffsets.size()*sizeof(uint16_t));
    if ( envVarsSize != 0 )
        memcpy((uint8_t*)clo + clo->dyldEnvVarsOffset, &envVarOffsets[0], envVarsSize);
    if ( stringPool.size() != 0 )
        memcpy((uint8_t*)clo + clo->stringPoolOffset, stringPool.buffer(), stringPool.size());

    return clo;
}

const BinaryImageGroupData* ImageProxyGroup::makeImageGroupBinary(Diagnostics& diag, const char* const neverEliminateStubs[])
{
    const bool continueIfErrors = (_groupNum == 1);
    bool uses16KPages = true;
    bool is64 = true;
    if ( !_images.empty() ) {
        MachOParser firstParser(_images.front()->mh(), _dyldCache.cacheIsMappedRaw());
        uses16KPages = firstParser.uses16KPages();
        is64         = firstParser.is64();
    }
    launch_cache::ImageGroupWriter groupWriter(_groupNum, uses16KPages, is64, _dylibsExpectedOnDisk, _inodesAreSameAsRuntime);
    populateGroupWriter(diag, groupWriter, neverEliminateStubs);
    if ( diag.hasError() )
        return nullptr;

    // malloc a buffer and fill in ImageGroup part
    BinaryImageGroupData* groupData = (BinaryImageGroupData*)malloc(groupWriter.size());
    groupWriter.finalizeTo(diag, _knownGroups, groupData);

    if ( !continueIfErrors && groupWriter.isInvalid(0) ) {
        free((void*)groupData);
        diag.error("depends on invalid dylib");
        return nullptr;
    }

    return groupData;
}


void ImageProxyGroup::findLibdyldEntry(Diagnostics& diag, ImageRef& ref, uint32_t& vmOffsetInLibDyld)
{
    Diagnostics libDyldDiag;
    ImageProxy* libDyldProxy = findImage(libDyldDiag, "/usr/lib/system/libdyld.dylib", false, nullptr);
    if ( libDyldProxy == nullptr ) {
        diag.error("can't find libdyld.dylib");
        return;
    }
    ref = ImageRef(0, libDyldProxy->groupNum(), libDyldProxy->indexInGroup());

    // find offset of "dyld3::entryVectorForDyld" in libdyld.dylib
    Diagnostics entryDiag;
    MachOParser::FoundSymbol dyldEntryInfo;
    MachOParser libDyldParser(libDyldProxy->mh(), _dyldCache.cacheIsMappedRaw());
    if ( !libDyldParser.findExportedSymbol(entryDiag, "__ZN5dyld318entryVectorForDyldE", nullptr, dyldEntryInfo, nullptr) ) {
        diag.error("can't find dyld entry point into libdyld.dylib");
        return;
    }
    vmOffsetInLibDyld = (uint32_t)dyldEntryInfo.value;
    const LibDyldEntryVector* entry = (LibDyldEntryVector*)(libDyldParser.content(vmOffsetInLibDyld));
    if ( entry == nullptr ) {
        diag.error("dyld entry point at offset 0x%0X not found in libdyld.dylib", vmOffsetInLibDyld);
        return;
    }
    if ( entry->vectorVersion != LibDyldEntryVector::kCurrentVectorVersion )
        diag.error("libdyld.dylib vector version is incompatible with this dyld cache builder");
    else if ( entry->binaryFormatVersion != launch_cache::binary_format::kFormatVersion )
        diag.error("libdyld.dylib closures binary format version is incompatible with this dyld cache builder");
}

void ImageProxyGroup::findLibSystem(Diagnostics& diag, bool forSimulator, ImageRef& ref)
{
    Diagnostics libSysDiag;
    ImageProxy* libSystemProxy = findImage(libSysDiag, forSimulator ? "/usr/lib/libSystem.dylib" : "/usr/lib/libSystem.B.dylib" , false, nullptr);
    if ( libSystemProxy == nullptr ) {
        diag.error("can't find libSystem.dylib");
        return;
    }
    ref = ImageRef(0, libSystemProxy->groupNum(), libSystemProxy->indexInGroup());
}


std::vector<ImageProxy*> ImageProxyGroup::flatLookupOrder()
{
    std::vector<ImageProxy*> results;
    // start with main executable and any inserted dylibs
    for (uint32_t i=0; i <= _mainExecutableIndex; ++i)
        results.push_back(_images[i]);

    // recursive add dependents of main executable
    _images[_mainExecutableIndex]->addToFlatLookup(results);

    // recursive add dependents of any inserted dylibs
    for (uint32_t i=0; i < _mainExecutableIndex; ++i)
        _images[i]->addToFlatLookup(results);

    return results;
}

void ImageProxyGroup::populateGroupWriter(Diagnostics& diag, launch_cache::ImageGroupWriter& groupWriter, const char* const neverEliminateStubs[])
{
    const bool buildingDylibsInCache = (_groupNum == 0);
    const bool continueIfErrors      = (_groupNum == 1);

    std::unordered_set<std::string>  neverStubEliminate;
    if ( neverEliminateStubs != nullptr ) {
        for (const char* const* nes=neverEliminateStubs; *nes != nullptr; ++nes)
            neverStubEliminate.insert(*nes);
    }
    
    // pass 1: add all images
    const uint64_t cacheUnslideBaseAddress = _dyldCache.cacheHeader()->unslidLoadAddress();
    const uint32_t imageCount = (uint32_t)_images.size();
    groupWriter.setImageCount(imageCount);
    for (uint32_t i=0; i < imageCount; ++i) {
        MachOParser imageParser(_images[i]->mh(), _dyldCache.cacheIsMappedRaw());
        assert((imageParser.inDyldCache() == buildingDylibsInCache) && "all images must be same type");
        // add info for each image
        groupWriter.setImagePath(i, _images[i]->runtimePath().c_str());
        groupWriter.setImageIsBundle(i, (imageParser.fileType() == MH_BUNDLE));
        bool hasObjC = imageParser.hasObjC();
        groupWriter.setImageHasObjC(i, hasObjC);
        bool isEncrypted = imageParser.isEncrypted();
        groupWriter.setImageIsEncrypted(i, isEncrypted);
        bool mayHavePlusLoad = false;
        if ( hasObjC ) {
            mayHavePlusLoad = isEncrypted || imageParser.hasPlusLoadMethod(diag);
            groupWriter.setImageMayHavePlusLoads(i, mayHavePlusLoad);
        }
        groupWriter.setImageHasWeakDefs(i, imageParser.hasWeakDefs());
        groupWriter.setImageMustBeThisDir(i, _images[i]->cwdMustBeThisDir());
        groupWriter.setImageIsPlatformBinary(i, _images[i]->isPlatformBinary());
        groupWriter.setImageOverridableDylib(i, !_stubEliminated || (neverStubEliminate.count(_images[i]->runtimePath()) != 0));
        uuid_t uuid;
        if ( imageParser.getUuid(uuid) )
            groupWriter.setImageUUID(i, uuid);
        if ( _inodesAreSameAsRuntime ) {
            groupWriter.setImageFileMtimeAndInode(i, _images[i]->fileModTime(), _images[i]->fileInode());
        }
        else {
            uint8_t cdHash[20];
            if ( !imageParser.getCDHash(cdHash) )
                bzero(cdHash, 20);
            // if image is not code signed, cdHash filled with all zeros
            groupWriter.setImageCdHash(i, cdHash);
        }
        if ( !buildingDylibsInCache ) {
            groupWriter.setImageSliceOffset(i, _images[i]->sliceFileOffset());
            uint32_t fairPlayTextOffset;
            uint32_t fairPlaySize;
            if ( imageParser.isFairPlayEncrypted(fairPlayTextOffset, fairPlaySize) )
                groupWriter.setImageFairPlayRange(i, fairPlayTextOffset, fairPlaySize);
            uint32_t codeSigOffset;
            uint32_t codeSigSize;
            if ( imageParser.hasCodeSignature(codeSigOffset, codeSigSize) )
                groupWriter.setImageCodeSignatureLocation(i, codeSigOffset, codeSigSize);
        }
        groupWriter.setImageDependentsCount(i, imageParser.dependentDylibCount());
        // add segments to image
        groupWriter.setImageSegments(i, imageParser, cacheUnslideBaseAddress);
        // add initializers to image
        __block std::vector<uint32_t> initOffsets;
        imageParser.forEachInitializer(diag, ^(uint32_t offset) {
            initOffsets.push_back(offset);
        });
        groupWriter.setImageInitializerOffsets(i, initOffsets);
        if ( diag.hasError() && !continueIfErrors ) {
            return;
        }
        // add DOFs to image
        __block std::vector<uint32_t> dofOffsets;
        imageParser.forEachDOFSection(diag, ^(uint32_t offset) {
            dofOffsets.push_back(offset);
        });
        groupWriter.setImageDOFOffsets(i, dofOffsets);
        if ( diag.hasError() && !continueIfErrors ) {
            return;
        }
        bool neverUnload = false;
        if ( buildingDylibsInCache )
            neverUnload = true;
        if ( _images[i]->staticallyReferenced() )
            neverUnload = true;
        if ( imageParser.hasObjC() && (imageParser.fileType() == MH_DYLIB) )
            neverUnload = true;
        if ( imageParser.hasThreadLocalVariables() )
            neverUnload = true;
        if ( !dofOffsets.empty() )
            neverUnload = true;
        groupWriter.setImageNeverUnload(i, neverUnload);
        if ( _images[i]->invalid() )
            groupWriter.setImageInvalid(i);
        // record if this is an override of an OS dylib
        ImageRef stdRef = _images[i]->overrideOf();
        if ( stdRef != ImageRef::weakImportMissing() ) {
            ImageRef thisImageRef(0, _groupNum, i);
            groupWriter.addImageIsOverride(stdRef, thisImageRef);
        }

        // add alias if runtimepath does not match installName
        if ( imageParser.fileType() == MH_DYLIB ) {
            const char* installName = imageParser.installName();
            if ( installName[0] == '/' ) {
                if ( _images[i]->runtimePath() != installName ) {
                    // add install name as an alias
                    groupWriter.addImageAliasPath(i, installName);
                }
            }
            // IOKit.framework on embedded uses not flat bundle, but clients dlopen() it as if it were flat
            if ( buildingDylibsInCache && (_platform != Platform::macOS) && (_images[i]->runtimePath() == "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit") ) {
                groupWriter.addImageAliasPath(i, "/System/Library/Frameworks/IOKit.framework/IOKit");
            }
        }
    }

    // pass 2: add all dependencies (now that we have indexes defined)
    for (uint32_t i=0; (i < imageCount) && diag.noError(); ++i) {
        // add dependents to image
        __block uint32_t depIndex = 0;
        _images[i]->forEachDependent(^(ImageProxy* dep, LinkKind kind) {
            if ( dep == nullptr ) {
                if ( kind == LinkKind::weak )
                    groupWriter.setImageDependent(i, depIndex, launch_cache::binary_format::ImageRef::weakImportMissing());
                else
                    groupWriter.setImageInvalid(i);
            }
            else {
                launch_cache::binary_format::ImageRef ref((uint8_t)kind, dep->groupNum(), dep->indexInGroup());
                groupWriter.setImageDependent(i, depIndex, ref);
            }
            ++depIndex;
        });
    }

    // pass 3: invalidate any images dependent on invalid images)
    if ( continueIfErrors ) {
        const launch_cache::binary_format::ImageRef missingRef = launch_cache::binary_format::ImageRef::weakImportMissing();
        __block bool somethingInvalidated = false;
        do {
            somethingInvalidated = false;
            for (uint32_t i=0; i < imageCount; ++i) {
                if ( groupWriter.isInvalid(i) )
                    continue;
                uint32_t depCount = groupWriter.imageDependentsCount(i);
                for (uint32_t depIndex=0; depIndex < depCount; ++depIndex) {
                    launch_cache::binary_format::ImageRef ref = groupWriter.imageDependent(i, depIndex);
                    if ( ref == missingRef )
                        continue;
                    if ( ref.groupNum() == _groupNum ) {
                        if ( groupWriter.isInvalid(ref.indexInGroup()) ) {
                            // this image depends on something invalid, so mark it invalid
                            //fprintf(stderr, "warning: image %s depends on invalid %s\n", _images[i]->runtimePath().c_str(), _images[ref.index()]->runtimePath().c_str());
                            groupWriter.setImageInvalid(i);
                            somethingInvalidated = true;
                            break;
                        }
                    }
                }
            }
        } while (somethingInvalidated);
    }

    // pass 4: add fixups for each image, if needed
    bool someBadFixups = false;
    if ( !buildingDylibsInCache ) {
        // compute fix ups for all images
        __block std::vector<ImageProxy::FixupInfo> fixupInfos;
        fixupInfos.resize(imageCount);
        for (uint32_t imageIndex=0; imageIndex < imageCount; ++imageIndex) {
            if ( groupWriter.isInvalid(imageIndex) )
                continue;
            Diagnostics fixupDiag;
            fixupInfos[imageIndex] = _images[imageIndex]->buildFixups(fixupDiag, cacheUnslideBaseAddress, groupWriter);
            if ( fixupDiag.hasError() ) {
                // disable image in group
                someBadFixups = true;
                groupWriter.setImageInvalid(imageIndex);
                if ( continueIfErrors ) {
                    diag.warning("fixup problem in %s: %s", _images[imageIndex]->runtimePath().c_str(), fixupDiag.errorMessage().c_str());
                    continue;
                }
                else {
                    diag.error("fixup problem in %s: %s", _images[imageIndex]->runtimePath().c_str(), fixupDiag.errorMessage().c_str());
                    return;
                }
            }
        }
        // if building closure, build patches to shared cache
        if ( _groupNum == 2) {
            std::unordered_set<ImageProxy*> staticImagesWithWeakDefs;
            ImageProxyGroup* cacheGroup = _nextSearchGroup->_nextSearchGroup;
            assert(cacheGroup->_basedOn != nullptr);
            launch_cache::ImageGroup dyldCacheGroup(cacheGroup->_basedOn);
            for (uint32_t imageIndex=0; imageIndex < imageCount; ++imageIndex) {
                if ( groupWriter.isInvalid(imageIndex) )
                    continue;
                ImageProxy* thisProxy = _images[imageIndex];
                // Only process interposing info on dylibs statically linked into closure
                if ( !thisProxy->staticallyReferenced() )
                    continue;
                MachOParser imageParser(thisProxy->mh(), _dyldCache.cacheIsMappedRaw());
                // if any images in closure interpose on something in dyld cache, record the cache patches needed
                imageParser.forEachInterposingTuple(diag, ^(uint32_t segIndex, uint64_t replacementSegOffset, uint64_t replaceeSegOffset, uint64_t replacementContent, bool& tupleStop) {
                    if ( _groupNum != 2 ) {
                        groupWriter.setImageInvalid(imageIndex);
                        return;
                    }
                    TargetSymbolValue interposeReplacee    = TargetSymbolValue::makeInvalid();
                    TargetSymbolValue interposeReplacement = TargetSymbolValue::makeInvalid();
                    for (const FixUp& fixup : fixupInfos[imageIndex].fixups) {
                        if ( fixup.segIndex != segIndex )
                            continue;
                        if ( fixup.segOffset == replacementSegOffset ) {
                            if ( fixup.type == launch_cache::ImageGroupWriter::FixupType::rebase ) {
                                uint64_t offsetInImage = replacementContent - imageParser.preferredLoadAddress();
                                interposeReplacement = TargetSymbolValue::makeGroupValue(2, imageIndex, offsetInImage, false);
                            }
                            else {
                                diag.warning("bad interposing implementation in %s", _images[imageIndex]->runtimePath().c_str());
                                return;
                            }
                        }
                        else if ( fixup.segOffset ==  replaceeSegOffset ) {
                            if ( fixup.type == launch_cache::ImageGroupWriter::FixupType::pointerBind ) {
                                interposeReplacee = fixup.target;
                            }
                            else {
                                diag.warning("bad interposing target in %s", _images[imageIndex]->runtimePath().c_str());
                                return;
                            }
                        }
                    }
                    // scan through fixups of other images in closure looking to see what functions this entry references
                    for (uint32_t otherIndex=0; otherIndex < imageCount; ++otherIndex) {
                        if ( otherIndex == imageIndex )
                            continue;
                        for (FixUp& fixup : fixupInfos[otherIndex].fixups) {
                            switch ( fixup.type ) {
                                case launch_cache::ImageGroupWriter::FixupType::pointerBind:
                                case launch_cache::ImageGroupWriter::FixupType::pointerLazyBind:
                                    // alter fixup to use interposed function instead of requested
                                    if ( fixup.target == interposeReplacee )
                                        fixup.target = interposeReplacement;
                                    break;
                                case launch_cache::ImageGroupWriter::FixupType::rebase:
                                case launch_cache::ImageGroupWriter::FixupType::rebaseText:
                                case launch_cache::ImageGroupWriter::FixupType::ignore:
                                case launch_cache::ImageGroupWriter::FixupType::bindText:
                                case launch_cache::ImageGroupWriter::FixupType::bindTextRel:
                                case launch_cache::ImageGroupWriter::FixupType::bindImportJmpRel:
                                   break;
                            }
                        }
                    }
                    if ( interposeReplacee.isInvalid() || interposeReplacement.isInvalid() ) {
                        diag.error("malformed interposing section in %s", _images[imageIndex]->runtimePath().c_str());
                        tupleStop = true;
                        return;
                    }
                    // record any overrides in shared cache that will need to be applied at launch time
                    uint64_t offsetInCache;
                    if ( interposeReplacee.isSharedCacheTarget(offsetInCache) ) {
                        uint32_t patchTableIndex;
                        if ( dyldCacheGroup.hasPatchTableIndex((uint32_t)offsetInCache, patchTableIndex) ) {
                            uint32_t    replacementGroupNum;
                            uint32_t    replacementIndexInGroup;
                            uint64_t    replacementOffsetInImage;
                            assert(interposeReplacement.isGroupImageTarget(replacementGroupNum, replacementIndexInGroup, replacementOffsetInImage));
                            assert(replacementGroupNum == 2);
                            assert(replacementIndexInGroup < (1 << 8));
                            if ( replacementOffsetInImage >= 0xFFFFFFFFULL ) {
                                diag.warning("bad interposing implementation in %s", _images[imageIndex]->runtimePath().c_str());
                                return;
                            }
                            DyldCacheOverride cacheOverride;
                            cacheOverride.patchTableIndex = patchTableIndex;
                            cacheOverride.imageIndex      = replacementIndexInGroup;
                            cacheOverride.imageOffset     = replacementOffsetInImage;
                            _cacheOverrides.push_back(cacheOverride);
                        }
                    }
                });
                if ( diag.hasError() && !continueIfErrors ) {
                    return;
                }
                // if any dylibs in the closure override a dyld cache dylib, then record the cache patches needed
                ImageRef overrideOf = thisProxy->overrideOf();
                if ( (overrideOf != ImageRef::makeEmptyImageRef()) && (overrideOf.groupNum() == 0) ) {
                     //fprintf(stderr, "need to patch %s into cache\n", thisProxy->runtimePath().c_str());
                    const launch_cache::Image imageInCache = dyldCacheGroup.image(overrideOf.indexInGroup());
                    const mach_header* imageInCacheMH = (mach_header*)((char*)(_dyldCache.cacheHeader()) + imageInCache.cacheOffset());
                    MachOParser inCacheParser(imageInCacheMH, _dyldCache.cacheIsMappedRaw());
                    // walk all exported symbols in dylib in cache
                    inCacheParser.forEachExportedSymbol(diag, ^(const char* symbolName, uint64_t imageOffset, bool isReExport, bool &stop) {
                        if ( isReExport )
                            return;
                        uint32_t cacheOffsetOfSymbol = (uint32_t)(imageInCache.cacheOffset() + imageOffset);
                        //fprintf(stderr, "  patch cache offset 0x%08X which is %s\n", cacheOffsetOfSymbol, symbolName);
                        // for each exported symbol, see if it is in patch table (used by something else in cache)
                        uint32_t patchTableIndex;
                        if ( dyldCacheGroup.hasPatchTableIndex(cacheOffsetOfSymbol, patchTableIndex) ) {
                            //fprintf(stderr, "  need patch cache offset 0x%08X\n", cacheOffsetOfSymbol);
                            // lookup address of symbol in override dylib and add patch info
                            MachOParser::FoundSymbol foundInfo;
                            if ( imageParser.findExportedSymbol(diag, symbolName, nullptr, foundInfo, nullptr) ) {
                                DyldCacheOverride cacheOverride;
                                assert(patchTableIndex < (1 << 24));
                                assert(thisProxy->indexInGroup() < (1 << 8));
                                assert(foundInfo.value < (1ULL << 32));
                                cacheOverride.patchTableIndex = patchTableIndex;
                                cacheOverride.imageIndex      = thisProxy->indexInGroup();
                                cacheOverride.imageOffset     = foundInfo.value;
                                _cacheOverrides.push_back(cacheOverride);
                            }
                        }
                    });
                }
                // save off all images in closure with weak defines
                if ( thisProxy->mh()->flags & (MH_WEAK_DEFINES|MH_BINDS_TO_WEAK) ) {
                    staticImagesWithWeakDefs.insert(thisProxy);
                }
            }
            // if any dylibs in the closure override a weak symbol in a cached dylib, then record the cache patches needed
            if ( !staticImagesWithWeakDefs.empty() ) {
                // build list of all weak def symbol names
                __block std::unordered_map<std::string, DyldCacheOverride> weakSymbols;
                for (ImageProxy* proxy : staticImagesWithWeakDefs ) {
                    MachOParser weakDefParser(proxy->mh(), _dyldCache.cacheIsMappedRaw());
                    weakDefParser.forEachWeakDef(diag, ^(bool strongDef, uint32_t segIndex, uint64_t segOffset, uint64_t addend, const char* symbolName, bool& stop) {
                        weakSymbols[symbolName] = { 0, 0, 0 };
                    });
                }
                // do a flat namespace walk of all images
                std::vector<ImageProxy*> flatSearchOrder = flatLookupOrder();
                for (ImageProxy* proxy : flatSearchOrder) {
                    // only look at images that participate in weak coalescing
                    if ( (proxy->mh()->flags & (MH_WEAK_DEFINES|MH_BINDS_TO_WEAK)) == 0 )
                        continue;
                    // look only at images in closure
                    if ( proxy->groupNum() == 2 ) {
                        MachOParser weakDefParser(proxy->mh(), _dyldCache.cacheIsMappedRaw());
                        // check if this closure image defines any of the not-yet found weak symbols
                        for (auto& entry : weakSymbols ) {
                            if ( entry.second.imageOffset != 0 )
                                continue;
                            Diagnostics weakDiag;
                            MachOParser::FoundSymbol foundInfo;
                            if ( weakDefParser.findExportedSymbol(weakDiag, entry.first.c_str(), nullptr, foundInfo, nullptr) ) {
                                assert(proxy->indexInGroup() < (1 << 8));
                                if ( foundInfo.value >= (1ULL << 32) ) {
                                    diag.warning("bad weak symbol address in %s", proxy->runtimePath().c_str());
                                    return;
                                }
                                entry.second.imageIndex  = proxy->indexInGroup();
                                entry.second.imageOffset = foundInfo.value;
                            }
                        }
                    }
                }
                for (ImageProxy* proxy : flatSearchOrder) {
                    // only look at images that participate in weak coalescing
                    if ( (proxy->mh()->flags & (MH_WEAK_DEFINES|MH_BINDS_TO_WEAK)) == 0 )
                        continue;
                    // look only at images in dyld cache
                    if ( proxy->groupNum() == 0 ) {
                        const launch_cache::Image imageInCache = dyldCacheGroup.image(proxy->indexInGroup());
                        MachOParser inCacheParser(proxy->mh(), _dyldCache.cacheIsMappedRaw());
                        Diagnostics cacheDiag;
                        for (auto& entry : weakSymbols) {
                            if ( entry.second.imageOffset == 0 )
                                continue;
                            Diagnostics weakDiag;
                            MachOParser::FoundSymbol foundInfo;
                            if ( inCacheParser.findExportedSymbol(weakDiag, entry.first.c_str(), nullptr, foundInfo, nullptr) ) {
                                uint32_t cacheOffsetOfSymbol = (uint32_t)(imageInCache.cacheOffset() + foundInfo.value);
                                // see if this symbol is in patch table (used by something else in cache)
                                uint32_t patchTableIndex;
                                if ( dyldCacheGroup.hasPatchTableIndex(cacheOffsetOfSymbol, patchTableIndex) ) {
                                    //fprintf(stderr, "  need patch cache offset 0x%08X\n", cacheOffsetOfSymbol);
                                    DyldCacheOverride cacheOverride;
                                    cacheOverride.patchTableIndex = patchTableIndex;
                                    cacheOverride.imageIndex      = entry.second.imageIndex;
                                    cacheOverride.imageOffset     = entry.second.imageOffset;
                                    _cacheOverrides.push_back(cacheOverride);
                                }
                            }
                        }
                    }
                }
            }
        }
        // record fixups for each image
        for (uint32_t imageIndex=0; imageIndex < imageCount; ++imageIndex) {
            groupWriter.setImageFixups(diag, imageIndex, fixupInfos[imageIndex].fixups, fixupInfos[imageIndex].hasTextRelocs);
        }
    }

    // pass 5: invalidate any images dependent on invalid images)
    if ( someBadFixups && continueIfErrors ) {
        __block bool somethingInvalidated = false;
        do {
            somethingInvalidated = false;
            for (uint32_t i=0; i < imageCount; ++i) {
                if ( groupWriter.isInvalid(i) )
                    continue;
                uint32_t depCount = groupWriter.imageDependentsCount(i);
                for (uint32_t depIndex=0; depIndex < depCount; ++depIndex) {
                    launch_cache::binary_format::ImageRef ref = groupWriter.imageDependent(i, depIndex);
                    if ( ref.groupNum() == _groupNum ) {
                        if ( groupWriter.isInvalid(ref.indexInGroup()) ) {
                            // this image depends on something invalid, so mark it invalid
                            //fprintf(stderr, "warning: image %s depends on invalid %s\n", _images[i]->runtimePath().c_str(), _images[ref.index()]->runtimePath().c_str());
                            groupWriter.setImageInvalid(i);
                            somethingInvalidated = true;
                            break;
                        }
                    }
                }
            }
        } while (somethingInvalidated);
    }

    // pass 6: compute initializer lists for each image
    const bool log = false;
    for (uint32_t imageIndex=0; imageIndex < imageCount; ++imageIndex) {
        if ( groupWriter.isInvalid(imageIndex) )
            continue;

        auto inits = _images[imageIndex]->getInitBeforeList(*this);
        if ( log && buildingDylibsInCache ) {
            fprintf(stderr, "%s\n   init list: ", _images[imageIndex]->runtimePath().c_str());
            for (launch_cache::binary_format::ImageRef ref : inits) {
                if ( ref.groupNum() == 0 ) {
                    std::string dep = _images[ref.indexInGroup()]->runtimePath();
                    size_t off = dep.rfind('/');
                    fprintf(stderr, "%s, ", dep.substr(off+1).c_str());
                }
            }
            fprintf(stderr, "\n");
        }
        groupWriter.setImageInitBefore(imageIndex, inits);
    }

    // pass 7: compute DOFs
    for (uint32_t imageIndex=0; imageIndex < imageCount; ++imageIndex) {
        if ( groupWriter.isInvalid(imageIndex) )
            continue;

        auto inits = _images[imageIndex]->getInitBeforeList(*this);
        if ( log && buildingDylibsInCache ) {
            fprintf(stderr, "%s\n   DOFs: ", _images[imageIndex]->runtimePath().c_str());
            for (launch_cache::binary_format::ImageRef ref : inits) {
                if ( ref.groupNum() == 0 ) {
                    std::string dep = _images[ref.indexInGroup()]->runtimePath();
                    size_t off = dep.rfind('/');
                    fprintf(stderr, "%s, ", dep.substr(off+1).c_str());
                }
            }
            fprintf(stderr, "\n");
        }
        groupWriter.setImageInitBefore(imageIndex, inits);
    }

    // pass 8: add patch table entries iff this is dyld cache ImageGroup
    assert(buildingDylibsInCache == (_patchTable != nullptr));
    if ( _patchTable != nullptr ) {
        for (uint32_t i=0; i < imageCount; ++i) {
            const auto pos = _patchTable->find(_images[i]->mh());
            if ( pos != _patchTable->end() ) {
                for (const auto& entry : pos->second ) {
                    uint32_t defFunctionOffset = entry.first;
                    groupWriter.setImagePatchLocations(i, defFunctionOffset, entry.second);
                }
            }
        }
    }

    // if this is a main closure group with an interposing dylib, add cache overrides
    if ( !_cacheOverrides.empty() ) {
        groupWriter.setGroupCacheOverrides(_cacheOverrides);
    }

    // align string pool
    groupWriter.alignStringPool();
}



} // namespace dyld3


