/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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

#include "SharedCacheLinker.h"
#include "AtomFileFormat.h"
#include "SharedCacheLinker_private.h"

// mach-o
#include "Architecture.h"
#include "Atom.h"
#include "Containers.h"
#include "Diagnostics.h"
#include "StringUtils.h"

// ld
#include "Layout.h"
#include "AtomFileConsolidator.h"
#include "DynamicAtom.h"
#include "JSONReader.h"
#include "Linker.h"

// Darwin
#include <mach-o/loader.h>
#include <sys/mman.h>

using namespace ld;
using namespace mach_o;
using namespace json;

struct JSONHeader
{
    uint64_t            version;
    PlatformAndVersions pvs;
    Architecture        arch;
    std::string         installName;
    std::string         options;
    const Node*         atoms=nullptr;
    const Node*         dylibs=nullptr;
    const Node*         customSections=nullptr;
};

// map to fill references to dylibIndex fixups
using DylibIndexMap = StringViewMap<uint16_t>;

static const Fixup::Kind fixupKindAndSizeFromString(CString str, uint32_t& size)
{
    if ( str == "ptr64" ) {
        size = 8;
        return Fixup::Kind::ptr64;
    }

    if ( str == "ptr32" ) {
        size = 4;
        return Fixup::Kind::ptr32;
    }

    if ( str == "arm64_auth_ptr" ) {
        size = 8;
        return Fixup::Kind::arm64_auth_ptr;
    }

    if ( str == "dylibIndex" ) {
        size = 2;
        return Fixup::Kind::dylibIndex;
    }

    return Fixup::Kind::none;
}

static Atom::ContentType contentTypeFromString(CString str)
{
    if ( str == "constText" )
        return Atom::ContentType::constText;
    if ( str == "cstring" )
        return Atom::ContentType::cstringLiteral;
    if ( str == "data" )
        return Atom::ContentType::data;
    if ( str == "constData" )
        return Atom::ContentType::constData;
    if ( str == "objcData" )
        return Atom::ContentType::objcData;
    if ( str == "objcConst" )
        return Atom::ContentType::objcConst;
    if ( str == "custom" )
        return Atom::ContentType::custom;
    // ld-prime doesn't need to understand these content types (yet)
    // so this uses a custom type with a custom section
    if ( str == "pointerHashTable" )
        return Atom::ContentType::custom;
    if ( str == "pointerHashTableKey" )
        return Atom::ContentType::custom;

    return Atom::ContentType::invalid;
}


static void parseHeader(Diagnostics& diag, JSONHeader& header, const Node& rootNode)
{
    header.version = parseRequiredInt(diag, getRequiredValue(diag, rootNode, "version"));
    if ( diag.hasError() )
        return;

    if ( header.version != 1 ) {
        diag.error("JSON version not supported: %llu", header.version);
        return;
    }

    uint32_t           rawPlatform = (uint32_t)parseRequiredInt(diag, getRequiredValue(diag, rootNode, "platform"));
    const std::string& rawPlatformVersion = parseRequiredString(diag, getRequiredValue(diag, rootNode, "platformVersion"));
    const std::string& rawArch = parseRequiredString(diag, getRequiredValue(diag, rootNode, "arch"));
    header.installName  = parseRequiredString(diag, getRequiredValue(diag, rootNode, "installName"));
    if ( diag.hasError() )
        return;

    header.arch = Architecture::byName(rawArch);
    if ( header.arch == Architecture::invalid  ) {
        diag.error("%s is not a valid architecture name", rawArch.c_str());
        return;
    }

    Platform platform(rawPlatform);
    if ( Error err = platform.valid() ) {
        diag.error(err);
        return;
    }

    Version32 ver;
    if ( Error err = Version32::fromString(rawPlatformVersion, ver) ) {
        diag.error(err);
        return;
    }

    header.pvs.platform = platform;
    header.pvs.minOS    = ver;
    header.pvs.sdk      = ver;

    header.atoms  = &getRequiredValue(diag, rootNode, "atoms");
    header.dylibs = &getRequiredValue(diag, rootNode, "dylibs");
    header.customSections = getOptionalValue(diag, rootNode, "customSections");

    if ( const Node* node = getOptionalValue(diag, rootNode, "options") ) {
        header.options = parseRequiredString(diag, *node);
    }
}

static DynamicAtomFile* addDylib(Diagnostics& diag, File::Ordinal ordinal, AtomGroup atomGroup, const Node& node)
{
    const std::string& installName  = parseRequiredString(diag, getRequiredValue(diag, node, "installName"));
    Node        exports      = getRequiredValue(diag, node, "exports");
    if ( diag.hasError() )
        return nullptr;

    // TODO: explicit versions?
    Version32 curVer;
    Version32 compatVer;
    DynamicAtomFile* af = new DynamicAtomFile(ordinal, CString::dup(installName), atomGroup);

    __block std::vector<uint32_t> rawPlatforms;
    atomGroup.pvs.unzip(^(PlatformAndVersions pvs) {
        rawPlatforms.push_back(pvs.platform.value());
    });
    af->setDylibFileInfo(DylibFileInfo::makeDylibFileInfo(installName, curVer, compatVer, rawPlatforms, {}, {}, {}));

    DylibExportsBuilder exportsBuilder(af, curVer, compatVer);
    for ( const Node& exportNode : exports.array ) {
        const std::string& name     = parseRequiredString(diag, getRequiredValue(diag, exportNode, "name"));
        bool               weakDef  = false;

        if ( const Node* weakDefNode = getOptionalValue(diag, exportNode, "weakDef") )
            weakDef = parseRequiredBool(diag, *weakDefNode);

        if ( diag.hasError() )
            return nullptr;

        exportsBuilder.addDylibExport(name, weakDef);
    }
    exportsBuilder.finalize();
    af->setActive(false, true);
    af->reclaimAllocatorResources();
    return af;
}

static void addFixup(Diagnostics& diag, const Architecture& arch, bool usesAuthPtrs,
                     DynamicAtom* atom, uint32_t offset, Fixup::Kind kind, const Atom* targetAtom,
                     const Node* addendNode, const Node* authPtrNode)
{
    int64_t addend = 0;
    if ( addendNode )
        addend = parseRequiredInt(diag, *addendNode);

    if ( authPtrNode && kind != Fixup::Kind::arm64_auth_ptr ) {
       diag.error("only arm64_auth_ptr fixups can have 'authPtr' data");
       return;
    }

    if ( kind == Fixup::Kind::arm64_auth_ptr ) {
        uint8_t  key = 0;
        bool     addr = false;
        uint16_t diversity = 0;

        if ( !usesAuthPtrs ) {
            diag.error("arm64_auth_ptr fixup can't be used with %s architecture", arch.name());
            return;
        }

        if ( authPtrNode ) {
            if ( const Node* keyNode = getOptionalValue(diag, *authPtrNode, "key") )
                key = (uint8_t)parseRequiredInt(diag, *keyNode);
            if ( const Node* addrNode = getOptionalValue(diag, *authPtrNode, "addr") )
                addr = parseRequiredBool(diag, *addrNode);
            if ( const Node* divNode = getOptionalValue(diag, *authPtrNode, "diversity") )
                diversity = (uint16_t)parseRequiredInt(diag, *divNode);
        }

        atom->addFixupAuthPointer(offset, targetAtom, key, addr, diversity, (int32_t)addend);
        return;
    }


    // regular fixup
    atom->addFixup(kind, offset, targetAtom, addend);
}

static void addAtoms(Diagnostics& diag, DynamicAtomFile* af, const Node& atomsNode, const DylibIndexMap& dylibIndexMap,
        std::span<const DynamicCustomSection> customSections, size_t pointerHashTableSectIndex, size_t pointerHashTableKeySectIndex)
{
    assert(af->atoms().empty());

    // keep track of defined/undefined atom indexes in the atom file
    // indexes are needed to setup fixup targets
    StringViewMap<uint32_t> atomNameToTargetIndex;
    StringViewMap<uint32_t> dylibNameToTargetIndex;

    // atom defaults
    const Atom::Alignment ptrSizeAlign(af->is64() ? 3 : 2);
    Architecture          arch = af->arch();
    bool                  usesAuthPtrs = arch.usesArm64AuthPointers();

    // in first pass create all defined atoms to fill the name map
    for ( const Node& atomNode : atomsNode.array ) {
        const std::string& name = parseRequiredString(diag, getRequiredValue(diag, atomNode, "name"));
        const std::string& ctName = parseRequiredString(diag, getRequiredValue(diag, atomNode, "contentType"));

        if ( diag.hasError() )
            return;

        Atom::Scope         scope   = Atom::Scope::global;
        bool                weakDef = false;
        Atom::ContentType   ct      = contentTypeFromString(ctName);
        Atom::Alignment     align   = ptrSizeAlign;

        if ( ct == Atom::ContentType::invalid ) {
            diag.error("unknown content type: %s", ctName.c_str());
            return;
        }

        if ( const Node* alignNode = getOptionalValue(diag, atomNode, "p2align") ) {
            align = Atom::Alignment((int)parseRequiredInt(diag, *alignNode));

            if ( diag.hasError() )
                return;
        }

        if ( const Node* weakDefNode = getOptionalValue(diag, atomNode, "weakDef") )
            weakDef = parseRequiredBool(diag, *weakDefNode);

        // default to 1-byte alignment for string literals
        if ( ct == Atom::ContentType::cstringLiteral )
            align = Atom::Alignment(0);

        DynamicAtom* atom   = af->makeSymbolAtom(name, ct, scope, weakDef);
        atom->setAlignment(align);
        if ( atomNameToTargetIndex.find(atom->name().str()) != atomNameToTargetIndex.end() ) {
            diag.error("duplicate atom name: %s", name.c_str());
        } else {
            atomNameToTargetIndex[atom->name().str()] = atom->atomOrdinal();
        }

        std::optional<uint64_t> customSectIndex;
        if ( const Node* customSection = getOptionalValue(diag, atomNode, "section") ) {
            uint64_t sectIndex = parseRequiredInt(diag, *customSection);
            if ( diag.hasError() )
                return;
            customSectIndex = sectIndex;
        } else {
            if ( ctName == "pointerHashTable" ) {
                customSectIndex = pointerHashTableSectIndex;
            } else if ( ctName == "pointerHashTableKey" ) {
                customSectIndex = pointerHashTableKeySectIndex;;
            }
        }

        if ( customSectIndex ) {
            if ( *customSectIndex > customSections.size() ) {
                diag.error("Invalid section index (%llu) in atom %s, max allowed: %lu\n", *customSectIndex, name.c_str(), customSections.size());
                return;
            }

            atom->setCustomSection(customSections[*customSectIndex]);
        }
    }

    // now parse all atom contents and their fixups
    for ( size_t atomIndex = 0; atomIndex < atomsNode.array.size(); ++atomIndex ) {
        const Node&        atomNode = atomsNode.array[atomIndex];
        const Node&        contents = getRequiredValue(diag, atomNode, "contents");
        if ( diag.hasError() )
            return;

        // note: atoms vector of the atom file can be modified in this loop, so call atoms() each time
        DynamicAtom* atom = (DynamicAtom*)af->atoms()[atomIndex];
        std::vector<uint8_t> bytes;

        for ( const Node& contentEntry : contents.array ) {
            if ( contentEntry.map.empty() ) {
                // content bytes string
                std::string bytesFrag = parseRequiredString(diag, contentEntry);
                if ( bytesFrag.size() & 0x1 ) {
                    diag.error("odd length (%lu) of content hex string for atom: %s",
                               bytesFrag.size(),
                               atom->name().c_str());
                    return;
                }

                for ( size_t i = 0; i < bytesFrag.size() / 2; ++i ) {
                    uint8_t high;
                    uint8_t low;
                    if ( !hexCharToUInt(bytesFrag[i * 2], high) || !hexCharToUInt(bytesFrag[(i * 2) + 1], low) ) {
                        diag.error("invalid hex content");
                        return;
                    }

                    bytes.push_back((high << 4) | low);
                }
                continue;
            }

            // handle fixup
            uint32_t targetIndex;
            const std::string& targetName = parseRequiredString(diag, getRequiredValue(diag, contentEntry, "target"));
            const std::string& kindStr = parseRequiredString(diag, getRequiredValue(diag, contentEntry, "kind"));
            if ( diag.hasError() )
                return;

            uint32_t fixupSize = 0;
            Fixup::Kind kind = fixupKindAndSizeFromString(kindStr, fixupSize);
            if ( kind == Fixup::Kind::none ) {
                diag.error("unsupported fixup kind: %s", kindStr.c_str());
                return;
            }

            // special case dylibIndex fixups
            // use anon placeholder atoms to turn dylib index fixups into constant values
            // note: alternative could be to use real fixups, but we'd need to teach shared cache builder to patch them too then
            if ( kind == Fixup::Kind::dylibIndex ) {
                if ( auto targetIt = dylibNameToTargetIndex.find(targetName); targetIt != dylibNameToTargetIndex.end() ) {
                    targetIndex = targetIt->second;
                } else {
                    uint16_t dylibIndex = 0;
                    if ( auto dylibIt = dylibIndexMap.find(targetName); dylibIt != dylibIndexMap.end() )
                        dylibIndex = dylibIt->second;
                    else {
                        diag.error("dylib index not found: %s", targetName.c_str());
                        return;
                    }
                    Atom* target = af->makeAnonPlaceholder(dylibIndex);
                    targetIndex = target->atomOrdinal();
                }
            } else if ( auto targetIt = atomNameToTargetIndex.find(targetName); targetIt != atomNameToTargetIndex.end() ) {
                targetIndex = targetIt->second;
            } else {
                Atom* target = af->makeUndefine(targetName);
                targetIndex = target->atomOrdinal();
                atomNameToTargetIndex[target->name().str()] = targetIndex;
            }

            const Node* addendNode = getOptionalValue(diag, contentEntry, "addend");
            const Node* authPtrNode = getOptionalValue(diag, contentEntry, "authPtr");
            size_t newAtomSize = bytes.size() + fixupSize;
            atom->setContentAsZeros(newAtomSize);
            addFixup(diag, arch, usesAuthPtrs, atom, (uint32_t)bytes.size(), kind,
                     af->atoms()[targetIndex], addendNode, authPtrNode);
            if ( diag.hasError() )
                return;

            // resize atom's byte to make place for the fixup content
            bytes.resize(newAtomSize);

            if ( diag.hasError() )
                return;
        }
        atom->setRawContentBytes(bytes);
    }
}

Error linkerMakeFromJSON(Linker& linker, std::span<const char> jsonData, std::span<const char*> dylibList, const char* outputPath)
{
    // read input JSON
    Diagnostics jsonDiag;
    Node rootNode = readJSON(jsonDiag, jsonData.data(), jsonData.size(), false /* useJSON5 */);
    if ( jsonDiag.hasError() )
        return jsonDiag.toError();

    // parse JSON header to construct linker options and get root atom nodes
    JSONHeader header;
    parseHeader(jsonDiag, header, rootNode);
    if ( jsonDiag.hasError() )
        return jsonDiag.toError();

    // configure linker
    char verStr[32];
    header.pvs.minOS.toString(verStr);
    std::vector<CString> rawArgv{"-arch", header.arch.name(),
        "-platform_version", header.pvs.platform.name(), verStr, verStr,
        "-dylib", "-o", outputPath,
        "-install_name", strdup(header.installName.data()),
        "-add_lldb_no_nlist_section" // rdar://146167046 (Please add `__TEXT,__lldb_no_nlist` section to libswiftPrespecialized.dylib)
    };
    // convert raw options string into options vector
    // this only splits options by a whitespace, no special logic to escape quotes or so
    CString extraArgv = header.options;
    while ( !extraArgv.empty() ) {
        if ( auto spacePos = extraArgv.find(' '); spacePos != CString::npos ) {
            rawArgv.push_back(extraArgv.dupSubstr(0, spacePos));
            extraArgv = extraArgv.substr(spacePos + 1);
        } else {
            rawArgv.push_back(extraArgv.dup());
            extraArgv = CString();
        }
    }

    ArgVector argv(std::move(rawArgv));
    File::Ordinal baseOrdinal = argv.nextFileOrdinal();
    if ( Error err = linker.setOptions(std::move(argv)) )
        return jsonDiag.toError();

    // create atom file to hold all content atoms
    baseOrdinal = baseOrdinal.nextFileListOrdinal();
    DynamicAtomFile* af = new DynamicAtomFile(baseOrdinal, "json.o",
                                              {linker.options().output.arch, linker.options().output.pvs});

    // create dylib atom files and their exports
    bool hasLibSystem = false;
    baseOrdinal = baseOrdinal.nextFileListOrdinal();
    File::Ordinal reservedLibSystemOrdinal = baseOrdinal;

    for ( const Node& dylib : header.dylibs->array ) {
        baseOrdinal = baseOrdinal.nextFileListOrdinal();

        DynamicAtomFile* dylibAf = addDylib(jsonDiag, baseOrdinal, af->atomsGroup(), dylib);
        if ( jsonDiag.hasError() )
            return jsonDiag.toError();

        assert(dylibAf);
        linker.addAtomFile(dylibAf);

        if ( const DylibFileInfo* dylibInfo = dylibAf->dylibFileInfo() )
            hasLibSystem |= dylibInfo->installName().contains("libSystem");
    }

    // always link with libSystem
    if ( !hasLibSystem ) {
        DynamicAtomFile* libSystem = new DynamicAtomFile(reservedLibSystemOrdinal, "/usr/lib/libSystem.B.dylib", af->atomsGroup());

        __block std::vector<uint32_t> rawPlatforms;
        af->atomsGroup().pvs.unzip(^(PlatformAndVersions pvs) {
            rawPlatforms.push_back(pvs.platform.value());
        });
        libSystem->setDylibFileInfo(DylibFileInfo::makeDylibFileInfo("/usr/lib/libSystem.B.dylib",
                                    Version32(), Version32(), rawPlatforms, {}, {}, {}));
        linker.addAtomFile(libSystem);
    }

    DylibIndexMap dylibIndexMap;
    for ( uint16_t i = 0; i < dylibList.size(); ++i )
        dylibIndexMap[dylibList[i]] = i;

    std::vector<DynamicCustomSection> customSections;
    if ( header.customSections ) {
        if ( header.customSections->array.empty() ) {
            return Error("customSections can't be an empty, either add sections or remove the field entirely");
        }

        for ( const Node& sectNode : header.customSections->array ) {
            const std::string& perms = parseRequiredString(jsonDiag, getRequiredValue(jsonDiag, sectNode, "segPerms"));
            const std::string& segName = parseRequiredString(jsonDiag, getRequiredValue(jsonDiag, sectNode, "segName"));
            const std::string& sectName = parseRequiredString(jsonDiag, getRequiredValue(jsonDiag, sectNode, "sectName"));
            if ( jsonDiag.hasError() )
                return jsonDiag.toError();

            uint32_t segPerms = 0;
            if ( perms.find('r') != std::string::npos )
                segPerms |= PROT_READ;
            if ( perms.find('w') != std::string::npos )
                segPerms |= PROT_WRITE;
            if ( perms.find('x') != std::string::npos )
                segPerms |= PROT_EXEC;

            uint32_t sectionFlags = S_REGULAR;
            if ( const Node* flagsNode = getOptionalValue(jsonDiag, sectNode, "sectFlags") )
                sectionFlags = (uint32_t)parseRequiredInt(jsonDiag, *flagsNode);
            if ( jsonDiag.hasError() )
                return jsonDiag.toError();

            customSections.push_back(af->makeCustomSection(segPerms, sectionFlags, strdup(segName.c_str()), strdup(sectName.c_str())));
        }
    }

    // reserve slots for pointer hash table sections
    size_t pointerHashTableSectIndex = customSections.size();
    customSections.push_back(af->makeCustomSection(PROT_READ | PROT_WRITE, S_REGULAR, "__DATA_CONST", "__ptrhashtab"));
    size_t pointerHashTableKeySectIndex = customSections.size();
    customSections.push_back(af->makeCustomSection(PROT_READ | PROT_WRITE, S_REGULAR, "__DATA_CONST", "__ptrhashtabkey"));

    // add all content atoms
    addAtoms(jsonDiag, af, *header.atoms, dylibIndexMap, customSections, pointerHashTableSectIndex, pointerHashTableKeySectIndex);
    if ( jsonDiag.hasError() )
        return jsonDiag.toError();

    linker.addAtomFile(af);
    return Error::none();
}

const char* ldMakeDylibFromJSON(std::span<const char> jsonData, std::span<const char*> dylibList, const char* outputPath)
{
    Linker linker;

    if ( Error err = linkerMakeFromJSON(linker, jsonData, dylibList, outputPath) )
        return strdup(err.message());

    if ( Error err = linker.run() )
        return strdup(err.message());

    return nullptr;
}
