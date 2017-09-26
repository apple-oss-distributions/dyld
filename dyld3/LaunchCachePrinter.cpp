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

#include <string>
#include <map>
#include <vector>

#include "LaunchCache.h"
#include "LaunchCacheFormat.h"

#if !DYLD_IN_PROCESS

namespace dyld3 {
namespace launch_cache {

struct Node
{
    std::string                 value;
    std::map<std::string, Node> map;
    std::vector<Node>           array;
};

static std::string hex(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%llX", value);
    return buff;
}

static std::string hex5(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%05llX", value);
    return buff;
}

static std::string decimal(uint64_t value) {
    char buff[64];
    sprintf(buff, "%llu", value);
    return buff;
}

static Node buildImageNode(const Image& image, const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails)
{
    __block Node imageNode;

    if ( image.isInvalid() )
        return imageNode;

    const ImageGroup group = image.group();
    imageNode.map["path"].value = image.path();
    __block Node imageAliases;
    group.forEachAliasOf(group.indexInGroup(image.binaryData()), ^(const char* aliasPath, uint32_t aliasPathHash, bool& stop) {
        Node anAlias;
        anAlias.value = aliasPath;
        imageAliases.array.push_back(anAlias);
    });
    if ( !imageAliases.array.empty() )
        imageNode.map["aliases"] = imageAliases;
    uuid_string_t uuidStr;
    uuid_unparse(*image.uuid(), uuidStr);
    imageNode.map["uuid"].value = uuidStr;
    imageNode.map["has-objc"].value = (image.hasObjC() ? "true" : "false");
    imageNode.map["has-weak-defs"].value = (image.hasWeakDefs() ? "true" : "false");
    imageNode.map["never-unload"].value = (image.neverUnload() ? "true" : "false");
    imageNode.map["platform-binary"].value = (image.isPlatformBinary() ? "true" : "false");
    if ( group.groupNum() == 0 )
        imageNode.map["overridable-dylib"].value = (image.overridableDylib() ? "true" : "false");
    if ( image.cwdMustBeThisDir() )
        imageNode.map["cwd-must-be-this-dir"].value = "true";
    if ( image.isDiskImage() ) {
        uint32_t csFileOffset;
        uint32_t csSize;
        if ( image.hasCodeSignature(csFileOffset, csSize) ) {
            imageNode.map["code-sign-location"].map["offset"].value = hex(csFileOffset);
            imageNode.map["code-sign-location"].map["size"].value = hex(csSize);
        }
        uint32_t fpTextOffset;
        uint32_t fpSize;
        if ( image.isFairPlayEncrypted(fpTextOffset, fpSize) ) {
            imageNode.map["fairplay-encryption-location"].map["offset"].value = hex(fpTextOffset);
            imageNode.map["fairplay-encryption-location"].map["size"].value = hex(fpSize);
        }
        if ( image.validateUsingModTimeAndInode() ) {
            imageNode.map["file-mod-time"].value = hex(image.fileModTime());
            imageNode.map["file-inode"].value = hex(image.fileINode());
        }
        else {
            const uint8_t* cdHash = image.cdHash16();
            std::string cdHashStr;
            cdHashStr.reserve(32);
            for (int j=0; j < 16; ++j) {
                uint8_t byte = cdHash[j];
                uint8_t nibbleL = byte & 0x0F;
                uint8_t nibbleH = byte >> 4;
                if ( nibbleH < 10 )
                    cdHashStr += '0' + nibbleH;
                else
                    cdHashStr += 'a' + (nibbleH-10);
                if ( nibbleL < 10 )
                    cdHashStr += '0' + nibbleL;
                else
                    cdHashStr += 'a' + (nibbleL-10);
            }
            imageNode.map["file-cd-hash-16"].value = cdHashStr;
        }
        imageNode.map["total-vm-size"].value = hex(image.vmSizeToMap());
        uint64_t sliceOffset = image.sliceOffsetInFile();
        if ( sliceOffset != 0 )
            imageNode.map["file-offset-of-slice"].value = hex(sliceOffset);
        if ( image.hasTextRelocs() )
            imageNode.map["has-text-relocs"].value = "true";
        image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
            Node segInfoNode;
            segInfoNode.map["file-offset"].value = hex(fileOffset);
            segInfoNode.map["file-size"].value = hex(fileSize);
            segInfoNode.map["vm-size"].value = hex(vmSize);
            segInfoNode.map["permissions"].value = hex(permissions);
            imageNode.map["mappings"].array.push_back(segInfoNode);
        });
        if ( printFixups ) {
            image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &segStop) {
                MemoryRange segContent = { nullptr, vmSize };
                std::string segName = "segment-" + decimal(segIndex);
                __block Node segmentFixupsNode;
                image.forEachFixup(segIndex, segContent, ^(uint64_t segOffset, Image::FixupKind kind, TargetSymbolValue value, bool& stop) {
                    switch ( kind ) {
                    case Image::FixupKind::rebase32:
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = "32-bit rebase";
                        break;
                    case Image::FixupKind::rebase64:
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = "64-bit rebase";
                        break;
                    case Image::FixupKind::rebaseText32 :
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = "32-bit text rebase";
                        break;
                    case Image::FixupKind::bind32:
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = std::string("32-bit bind, target=") + value.asString(group);
                        break;
                    case Image::FixupKind::bind64:
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = std::string("64-bit bind, target=") + value.asString(group);
                        break;
                    case Image::FixupKind::bindText32 :
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = std::string("32-bit text abs bind, target=") + value.asString(group);
                        break;
                    case Image::FixupKind::bindTextRel32 :
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = std::string("32-bit text rel bind, target=") + value.asString(group);
                        break;
                    case Image::FixupKind::bindImportJmp32 :
                        segmentFixupsNode.map[segName].map[hex5(segOffset)].value = std::string("32-bit IMPORT JMP rel bind, target=") + value.asString(group);
                        break;
                    }
                });
                if ( segmentFixupsNode.map[segName].map.size() != 0 ) {
                    imageNode.map["fixups"].array.push_back(segmentFixupsNode);
                }
            });
        }
    }
    else {
        imageNode.map["patch-start-index"].value = decimal(image.patchStartIndex());
        imageNode.map["patch-count"].value = decimal(image.patchCount());
    }

    // add dependents
    image.forEachDependentImage(groupList, ^(uint32_t depIndex, Image depImage, Image::LinkKind kind, bool& stop) {
        Node depMapNode;
        depMapNode.map["path"].value = depImage.path();
        if ( printDependentsDetails ) {
            ImageGroup depGroup = depImage.group();
            uint32_t indexInGroup = depGroup.indexInGroup(depImage.binaryData());
            depMapNode.map["group-index"].value = decimal(depGroup.groupNum());
            depMapNode.map["index-in-group"].value = decimal(indexInGroup);
        }
        switch ( kind ) {
            case Image::LinkKind::regular:
                depMapNode.map["link"].value = "regular";
                break;
            case Image::LinkKind::reExport:
                depMapNode.map["link"].value = "re-export";
                break;
            case Image::LinkKind::upward:
                depMapNode.map["link"].value = "upward";
                break;
            case Image::LinkKind::weak:
                depMapNode.map["link"].value = "weak";
                break;
        }
        imageNode.map["dependents"].array.push_back(depMapNode);
    });
    // add things to init before this image
    __block Node initBeforeNode;
    image.forEachInitBefore(groupList, ^(Image beforeImage) {
        Node beforeNode;
        beforeNode.value = beforeImage.path();
        imageNode.map["initializer-order"].array.push_back(beforeNode);
    });

    // add initializers
    image.forEachInitializer(nullptr, ^(const void* initializer) {
        Node initNode;
        initNode.value = hex((long)initializer);
        imageNode.map["initializer-offsets"].array.push_back(initNode);
    });

    // add override info if relevant
    group.forEachImageRefOverride(groupList, ^(Image standardDylib, Image overrideDylib, bool& stop) {
        if ( overrideDylib.binaryData() == image.binaryData() ) {
            imageNode.map["override-of-cached-dylib"].value = standardDylib.path();
        }
    });

    // add dtrace info
    image.forEachDOF(nullptr, ^(const void* section) {
        Node initNode;
        initNode.value = hex((long)section);
        imageNode.map["dof-offsets"].array.push_back(initNode);
    });

    return imageNode;
}


static Node buildImageGroupNode(const ImageGroup& group, const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails)
{
    Node images;
    uint32_t imageCount = group.imageCount();
    images.array.reserve(imageCount);
    for (uint32_t i=0; i < imageCount; ++i) {
         images.array.push_back(buildImageNode(group.image(i), groupList, printFixups, printDependentsDetails));
    }
    return images;
}

static Node buildClosureNode(const Closure& closure, const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails)
{
    __block Node root;

    // add env-vars if they exist
    closure.forEachEnvVar(^(const char* keyEqualValue, bool& stop) {
        const char* equ = strchr(keyEqualValue, '=');
        if ( equ != nullptr ) {
            char key[512];
            strncpy(key, keyEqualValue, equ-keyEqualValue);
            key[equ-keyEqualValue] = '\0';
            root.map["env-vars"].map[key].value = equ+1;
        }
    });

    // add missing files array if they exist
    closure.forEachMustBeMissingFile(^(const char* path, bool& stop) {
        Node fileNode;
        fileNode.value = path;
        root.map["must-be-missing-files"].array.push_back(fileNode);
    });

    const uint8_t* cdHash = closure.cdHash();
    std::string cdHashStr;
    cdHashStr.reserve(24);
    for (int i=0; i < 20; ++i) {
        uint8_t byte = cdHash[i];
        uint8_t nibbleL = byte & 0x0F;
        uint8_t nibbleH = byte >> 4;
        if ( nibbleH < 10 )
            cdHashStr += '0' + nibbleH;
        else
            cdHashStr += 'a' + (nibbleH-10);
        if ( nibbleL < 10 )
            cdHashStr += '0' + nibbleL;
        else
            cdHashStr += 'a' + (nibbleL-10);
    }
    if ( cdHashStr != "0000000000000000000000000000000000000000" )
        root.map["cd-hash"].value = cdHashStr;

    // add uuid of dyld cache this closure requires
    closure.dyldCacheUUID();
    uuid_string_t cacheUuidStr;
    uuid_unparse(*closure.dyldCacheUUID(), cacheUuidStr);
    root.map["dyld-cache-uuid"].value = cacheUuidStr;

    // add top level images
    Node& rootImages = root.map["root-images"];
    uint32_t initImageCount = closure.mainExecutableImageIndex();
    rootImages.array.resize(initImageCount+1);
    for (uint32_t i=0; i <= initImageCount; ++i) {
        const Image image = closure.group().image(i);
        uuid_string_t uuidStr;
        uuid_unparse(*image.uuid(), uuidStr);
        rootImages.array[i].value = uuidStr;
    }
    root.map["initial-image-count"].value = decimal(closure.initialImageCount());

    // add images
    root.map["images"] = buildImageGroupNode(closure.group(), groupList, printFixups, printDependentsDetails);
    root.map["group-num"].value = decimal(closure.group().groupNum());

    if ( closure.mainExecutableUsesCRT() )
        root.map["main-offset"].value = hex(closure.mainExecutableEntryOffset());
    else
        root.map["start-offset"].value = hex(closure.mainExecutableEntryOffset());

    root.map["libdyld-entry-offset"].value = hex(closure.libdyldVectorOffset());

    root.map["restricted"].value = (closure.isRestricted() ? "true" : "false");

    root.map["library-validation"].value = (closure.usesLibraryValidation() ? "true" : "false");

    __block Node cacheOverrides;
    closure.group().forEachDyldCacheSymbolOverride(^(uint32_t patchTableIndex, uint32_t imageIndexInClosure, uint32_t imageOffset, bool& stop) {
        Node patch;
        patch.map["patch-index"].value = decimal(patchTableIndex);
        patch.map["replacement"].value = "{closure[" + decimal(imageIndexInClosure) + "]+" + hex(imageOffset) + "}";
        cacheOverrides.array.push_back(patch);
    });
    if ( !cacheOverrides.array.empty() )
        root.map["dyld-cache-overrides"].array = cacheOverrides.array;

    return root;
}

static void indentBy(uint32_t spaces, FILE* out) {
    for (int i=0; i < spaces; ++i) {
        fprintf(out, " ");
    }
}

static void printJSON(const Node& node, uint32_t indent, FILE* out)
{
    if ( !node.map.empty() ) {
        fprintf(out, "{");
        bool needComma = false;
        for (const auto& entry : node.map) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n");
            indentBy(indent+2, out);
            fprintf(out, "\"%s\": ", entry.first.c_str());
            printJSON(entry.second, indent+2, out);
            needComma = true;
        }
        fprintf(out, "\n");
        indentBy(indent, out);
        fprintf(out, "}");
    }
    else if ( !node.array.empty() ) {
        fprintf(out, "[");
        bool needComma = false;
        for (const auto& entry : node.array) {
            if ( needComma )
                fprintf(out, ",");
            fprintf(out, "\n");
            indentBy(indent+2, out);
            printJSON(entry, indent+2, out);
            needComma = true;
        }
        fprintf(out, "\n");
        indentBy(indent, out);
        fprintf(out, "]");
    }
    else {
        fprintf(out, "\"%s\"", node.value.c_str());
    }
    if ( indent == 0 )
        fprintf(out, "\n");
}


void Image::printAsJSON(const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails, FILE* out) const
{
    Node image = buildImageNode(*this, groupList, printFixups, printDependentsDetails);
    printJSON(image, 0, out);
}

void ImageGroup::printAsJSON(const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails, FILE* out) const
{
    Node root;
    root.map["images"] = buildImageGroupNode(*this, groupList, printFixups, printDependentsDetails);
    root.map["group-num"].value = decimal(groupNum());
    root.map["dylibs-expected-on-disk"].value = (dylibsExpectedOnDisk() ? "true" : "false");
	printJSON(root, 0, out);
}

void ImageGroup::printStatistics(FILE* out) const
{
    __block uint32_t totalRebases = 0;
    __block uint32_t totalBinds   = 0;
    for (uint32_t i=0; i < imageCount(); ++i) {
        Image img(image(i));
        img.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &segStop) {
            MemoryRange segContent = { nullptr, vmSize };
            img.forEachFixup(segIndex, segContent, ^(uint64_t segOffset, Image::FixupKind kind, TargetSymbolValue value, bool& stop) {
                if ( kind == Image::FixupKind::rebase64 )
                    ++totalRebases;
                else
                    ++totalBinds;
            });
        });
    }

    fprintf(out, "ImageGroup:\n");
    fprintf(out, "  image-count:            % 5d\n", _binaryData->imagesPoolCount);
    fprintf(out, "  alias-count:            % 5d\n", _binaryData->imageAliasCount);
    fprintf(out, "  segments-count:         % 5d\n", _binaryData->segmentsPoolCount);
    fprintf(out, "  dependents-count:       % 5d\n", _binaryData->dependentsPoolCount);
    fprintf(out, "  targets-count:          % 5d\n", _binaryData->targetsPoolCount);
    fprintf(out, "  rebase-count:           % 5d\n", totalRebases);
    fprintf(out, "  bind-count:             % 5d\n", totalBinds);
    fprintf(out, "  fixups-size:            % 8d bytes\n",  _binaryData->fixupsPoolSize);
    fprintf(out, "  targets-size:           % 8ld bytes\n", _binaryData->targetsPoolCount * sizeof(uint64_t));
    fprintf(out, "  strings-size:           % 8d bytes\n",  _binaryData->stringsPoolSize);
    fprintf(out, "  dofs-size:              % 8ld bytes\n",  _binaryData->dofOffsetPoolCount * sizeof(uint32_t));
    fprintf(out, "  indirect-groups-size:   % 8ld bytes\n",  _binaryData->indirectGroupNumPoolCount * sizeof(uint32_t));
}


void Closure::printAsJSON(const ImageGroupList& groupList, bool printFixups, bool printDependentsDetails, FILE* out) const
{
    Node root = buildClosureNode(*this, groupList, printFixups, printDependentsDetails);
    printJSON(root, 0, out);
}

void Closure::printStatistics(FILE* out) const
{
    fprintf(out, "closure size: %lu\n", size());
    group().printStatistics(out);
}



} // namespace launch_cache
} // namespace dyld3

#endif


