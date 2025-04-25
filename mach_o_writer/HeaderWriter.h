/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_writer_Header_h
#define mach_o_writer_Header_h

#include "Header.h"

#include <vector>

namespace mach_o {

using namespace mach_o;

/*!
 * @class HeaderWriter
 *
 * @abstract
 *      The HeaderWriter constructor can be used to build a mach-o file dynamically for unit tests
 */
struct VIS_HIDDEN HeaderWriter : public Header
{
    HeaderWriter() = delete; // we never allocate a HeaderWriter directly, always getting one from casting a buffer

    // for building
    static HeaderWriter*  make(std::span<uint8_t> buffer, uint32_t filetype, uint32_t flags, Architecture, bool addImplicitTextSegment=true);
    
    void            save(char savedPath[PATH_MAX]) const;
    load_command*   findLoadCommand(uint32_t cmd);
    void            setHasThreadLocalVariables();
    void            setHasWeakDefs();
    void            setUsesWeakDefs();
    void            setAppExtensionSafe();
    void            setSimSupport();
    void            setNoReExportedDylibs();
    void            addPlatformInfo(Platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools={});
    void            addUniqueUUID(uuid_t copyOfUUID=nullptr);
    void            addNullUUID();
    void            updateUUID(uuid_t);
    void            addInstallName(const char* path, Version32 compatVers, Version32 currentVersion);
    void            addLinkedDylib(const char* path, LinkedDylibAttributes kind=LinkedDylibAttributes::regular, Version32 compatVers=Version32(1,0), Version32 currentVersion=Version32(1,0));
    void            setLinkedDylib(load_command* cmd, const char* path, LinkedDylibAttributes kind, Version32 compatVers, Version32 currentVersion);
    void            addLibSystem();
    void            addDylibId(CString name, Version32 compatVers, Version32 currentVersion);
    void            addDyldID();
    void            addDynamicLinker();
    void            addRPath(const char* path);
    void            setTargetTriple(const char* triple);
    void            addSourceVersion(Version64 srcVers);
    void            addDyldEnvVar(const char* envVar);
    void            addAllowableClient(const char* clientName);
    void            addUmbrellaName(const char* umbrellaName);
    void            addFairPlayEncrypted(uint32_t offset, uint32_t size);
    void            addCodeSignature(uint32_t fileOffset, uint32_t fileSize);
    void            addSegment(std::string_view segName, uint64_t vmaddr, uint64_t vmsize, uint32_t perms, uint32_t sectionCount);
    void            setMain(uint32_t offset);
    void            setCustomStackSize(uint64_t stackSize);
    void            setUnixEntry(uint64_t addr);
    void            setSymbolTable(uint32_t nlistOffset, uint32_t nlistCount, uint32_t stringPoolOffset, uint32_t stringPoolSize,
                                   uint32_t localsCount, uint32_t globalsCount, uint32_t undefCount, uint32_t indOffset, uint32_t indCount, bool dynSymtab);
    void            setBindOpcodesInfo(uint32_t rebaseOffset, uint32_t rebaseSize,
                                       uint32_t bindsOffset, uint32_t bindsSize,
                                       uint32_t weakBindsOffset, uint32_t weakBindsSize,
                                       uint32_t lazyBindsOffset, uint32_t lazyBindsSize,
                                       uint32_t exportTrieOffset, uint32_t exportTrieSize);
    void            setChainedFixupsInfo(uint32_t cfOffset, uint32_t cfSize);
    void            setExportTrieInfo(uint32_t offset, uint32_t size);
    void            setSplitSegInfo(uint32_t offset, uint32_t size);
    void            setDataInCode(uint32_t offset, uint32_t size);
    void            setFunctionStarts(uint32_t offset, uint32_t size);
    void            setFunctionVariants(uint32_t offset, uint32_t size);
    void            setFunctionVariantFixups(uint32_t offset, uint32_t size);
    void            addLinkerOption(std::span<uint8_t> buffer, uint32_t count);
    void            setAtomInfo(uint32_t offset, uint32_t size);
    void            setLinkerOptimizationHints(uint32_t offset, uint32_t size);

    void            updateRelocatableSegmentSize(uint64_t vmSize, uint32_t fileSize);
    void            setRelocatableSectionCount(uint32_t sectionCount);
    void            setRelocatableSectionInfo(uint32_t sectionIndex, const char* segName, const char* sectName, uint32_t flags, uint64_t address,
                                              uint64_t size, uint32_t fileOffset, uint16_t alignment, uint32_t relocsOffset, uint32_t relocsCount);

    void            addSegment(const SegmentInfo&, std::span<const char* const> sectionNames=std::span<const char* const>{});
    void            updateSegment(const SegmentInfo& info);
    void            updateSection(const SectionInfo& info);
    Error           removeLoadCommands(uint32_t index, uint32_t endIndex);
    // returns nullptr if there's not enough padding space available
    load_command*   insertLoadCommand(uint32_t atIndex, uint32_t cmdSize);

    struct LinkerOption
    {
        std::vector<uint8_t> buffer;
        uint32_t             count = 0;

        uint32_t lcSize() const { return ((uint32_t)buffer.size() + sizeof(linker_option_command) + 7) & (-8); }

        static LinkerOption make(std::span<CString>);
    };

    static uint32_t relocatableHeaderAndLoadCommandsSize(bool is64, uint32_t sectionCount, uint32_t platformsCount,
                                                         std::span<const LinkerOption> linkerOptions);

    using Header::findLoadCommand;

private:

    void            removeLoadCommand(void (^callback)(const load_command* cmd, bool& remove, bool& stop));
    load_command*   firstLoadCommand();
    load_command*   appendLoadCommand(uint32_t cmd, uint32_t cmdSize);
    void            appendLoadCommand(const load_command* lc);
    void            addBuildVersion(Platform, Version32 minOS, Version32 sdk, std::span<const build_tool_version> tools);
    void            addMinVersion(Platform, Version32 minOS, Version32 sdk);

};



} // namespace mach_o

#endif /* mach_o_writer_Header_h */
