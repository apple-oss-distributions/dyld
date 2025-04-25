/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.0 (the 'License').  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License."
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>

// mach_o
#include "Header.h"
#include "Architecture.h"

// mach_o_writer
#include "HeaderWriter.h"

#include "MemoryBuffer.h"

#include "cctools_helpers.h"


using mach_o::Header;
using mach_o::HeaderWriter;
using mach_o::Error;
using mach_o::Architecture;

VIS_HIDDEN
void make_obj_file_with_linker_options(uint32_t cpu_type, uint32_t cpu_subtype,
                                       uint32_t libHintCount, const char* libNames[],
                                       uint32_t frameworkHintCount, const char* frameworkNames[],
                                       char outPath[PATH_MAX])
{
    // estimate total load command size
    Architecture arch(cpu_type, cpu_subtype);
    uint32_t size = sizeof(mach_header_64);
    for (uint32_t i=0; i < libHintCount; ++i)
        size += Header::pointerAligned(arch.is64(), (uint32_t)(sizeof(linker_option_command) + strlen(libNames[i]) + 3));
    for (uint32_t i=0; i < frameworkHintCount; ++i)
        size += Header::pointerAligned(arch.is64(), (uint32_t)(sizeof(linker_option_command) + strlen(frameworkNames[i]) + 12));
    size_t allocationSize = (size + 0x3FFF) & (-0x4000);
    // create HeaderWriter
    WritableMemoryBuffer mhBuffer = WritableMemoryBuffer::allocate(allocationSize);
    HeaderWriter* mh = HeaderWriter::make(mhBuffer, MH_OBJECT, 0, arch, false);
    // add all auto-linking load commands
    for (uint32_t i=0; i < libHintCount; ++i) {
        const char* libName     = libNames[i];
        size_t      libBuffSize = strlen(libName) + 3; // space for -l and trailing nul
        char        libBuffer[libBuffSize];
        strlcpy(libBuffer, "-l", libBuffSize);
        strlcat(libBuffer, libName, libBuffSize);
        std::span<uint8_t> buffer{(uint8_t*)libBuffer, libBuffSize};
        mh->addLinkerOption(buffer, 1);
    }
    for (uint32_t i=0; i < frameworkHintCount; ++i) {
        const char* fwName     = frameworkNames[i];
        size_t      fwBuffSize = strlen(fwName) + 12;  // space for -framework and trailing nul
        char        fwBuffer[fwBuffSize];
        strcpy(&fwBuffer[0],  "-framework");
        strcpy(&fwBuffer[11], fwName);
        std::span<uint8_t> buffer{(uint8_t*)fwBuffer, fwBuffSize};
        mh->addLinkerOption(buffer, 2);
    }

    mh->save(outPath);
}
