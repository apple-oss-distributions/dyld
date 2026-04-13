/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2025 Apple Inc. All rights reserved.
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

#include <stdio.h>
#include <sys/stat.h>
#include <os/base.h>    // for OS_STRINGIFY

// c++ stl
#include <vector>
#include <string>

// common
#include "Defines.h"
#include "Utilities.h"

// mach_o
#include "Architecture.h"
#include "Error.h"

// other_tools
#include "dyld_analyzer.h"
#include "FileUtils.h"
#include "MiscFileUtils.h"


using mach_o::Architecture;
using mach_o::Error;
using mach_o::Archive;
using mach_o::Universal;

// JSON Writer helper
class JSONWriter
{
public:
    JSONWriter(bool emitNewlines = false);
    void withObject(std::string_view name, void (^callback)(void));
    void withArray(std::string_view name, size_t numElements, void (^callback)(size_t index));
    void emitValue(std::string_view name, std::string_view value);
    void emitValue(std::string_view name, uint64_t value);

    std::string result();

private:
    std::string indentation() const;
    void dropTrailingValueEnd();

    const char* newLine = "\n";
    int indent = 0;
    std::string buffer;
};

JSONWriter::JSONWriter(bool emitNewlines)
{
    newLine = emitNewlines ? "\n" : "";
}

void JSONWriter::dropTrailingValueEnd()
{
    // Last object might have trailing ",". Remove if that is the case
    if ( buffer.ends_with(",\n") || buffer.ends_with(",") ) {
        if ( buffer.ends_with(",\n") )
            buffer.resize(buffer.size() - 2);
        else
            buffer.resize(buffer.size() - 1);
        buffer += newLine;
    }
}

std::string JSONWriter::result()
{
    dropTrailingValueEnd();
    return buffer;
}

std::string JSONWriter::indentation() const
{
    return std::string(indent, ' ');
}

void JSONWriter::withObject(std::string_view name, void (^callback)(void))
{
    if ( !name.empty() )
        buffer += indentation() + "\"" + std::string(name) + "\"" + ": {" + newLine;
    else
        buffer += indentation() + "{" + newLine;

    indent += 2;
    callback();
    indent -= 2;

    dropTrailingValueEnd();
    buffer += indentation() + "}," + newLine;
}

void JSONWriter::withArray(std::string_view name, size_t numElements, void (^callback)(size_t index))
{
    if ( !name.empty() )
        buffer += indentation() + "\"" + std::string(name) + "\"" + ": [" + newLine;
    else
        buffer += indentation() + "[" + newLine;

    indent += 2;
    for ( size_t index = 0; index != numElements; ++index ) {
        callback(index);
    }
    indent -= 2;

    dropTrailingValueEnd();
    buffer += indentation() + "]," + newLine;
}

void JSONWriter::emitValue(std::string_view name, std::string_view value)
{
    buffer += indentation();
    if ( !name.empty() )
        buffer += "\"" + std::string(name) + "\"" + ": ";

    buffer += "\"" + std::string(value) + "\"" + "," + newLine;
}

void JSONWriter::emitValue(std::string_view name, uint64_t value)
{
    if ( !name.empty() )
        buffer += indentation() + "\"" + std::string(name) + "\"" + ": ";

    buffer += std::to_string(value) + "," + newLine;
}


#if BUILDING_LEGACY_TOOL
int main(int argc, const char* argv[], const char* envp[])
{
    other_tools::dyld_analyzer tool;
    return tool.run(argc, argv, envp);
}
#endif


namespace other_tools {

void dyld_analyzer::usage() const
{
    fprintf(stderr, "Usage: dyld_analyzer <options>* -dir <directory path> -o <file>\n"
            "\t-v                                   Print version and exit\n"
            "\t-measure                             Measure slices with sha256\n"
            "\t-o <file>                            Output path of results JSON\n"
            "\t-dir <path>                          Recursively look for files in given directory\n"
            );
}

void dyld_analyzer::handleFile(const CString path)
{
}

Error dyld_analyzer::handleOption(std::span<CString>& remainingArgs)
{
    CString arg = remainingArgs.front();
    if ( arg == "-v" ) {
        _printVersion = true;
    }
    else if ( arg == "-measure" ) {
        _measureSlices = true;
    }
    else if ( arg == "-o" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-o missing path");
        remainingArgs = remainingArgs.subspan(1);
        CString outPath = remainingArgs.front();
        _outPath = outPath;
    }
    else if ( arg == "-dir" ) {
        if ( remainingArgs.size() < 2 )
            return Error("-dir missing input file path");
        remainingArgs = remainingArgs.subspan(1);
        CString path  = remainingArgs.front();
        _directory = path;
    }
    return Error::none();
}

Error dyld_analyzer::processFiles(std::string& jsonEntry)
{
    // error if no directory name specified
    if ( _directory.empty() ) {
        return Error("no input directory specified");
    }

    // error if no action given
    if ( !_measureSlices ) {
        return Error("expected option such as '-measure'");
    }

    __block std::vector<CString> filePaths;

    auto shouldFilterDir = ^(const std::string& dirPath) {
        return false;
    };
    auto processFile = ^(const std::string& path, const struct stat& statBuf) {
        filePaths.push_back(CString::dup(path));
    };
    iterateDirectoryTree("", std::string(_directory), shouldFilterDir, processFile,
                         true /* process files */, true /* recurse */);

    __block std::map<std::string, std::vector<std::string>> measurementResults;
    this->getMachOsFromPaths(filePaths, false /*decendIntoStaticLibs*/, false /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        if ( inputs.empty() )
            return;

        for ( const Input& input : inputs ) {
            if ( Archive::isArchive(input.slice.buffer) ) {
                // hash the file content
                uint8_t shaResult[32];
                sha256(input.slice.buffer, std::span(shaResult));
                char measureString[80];
                bytesToHex(shaResult, sizeof(shaResult), measureString);

                std::string resultPath = std::string(input.path);
                if ( resultPath.starts_with(_directory) )
                    resultPath = resultPath.substr(_directory.size());

                // Results should start with "./" to show they are relative to the base directory
                if ( resultPath.starts_with("/") )
                    resultPath = "." + resultPath;
                else
                    resultPath = "./" + resultPath;

                measurementResults[measureString].push_back(resultPath);
            }
        }
    });

    __block JSONWriter writer;
    writer.withObject("", ^{
        writer.emitValue("version", 1);
        writer.emitValue("base-directory", _directory);
        writer.withObject("measure-sha256", ^{
            for ( auto resultIt = measurementResults.begin(); resultIt != measurementResults.end(); ++resultIt ) {
                const std::string& measurement = resultIt->first;
                const std::vector<std::string>& files = resultIt->second;
                writer.withArray(measurement, files.size(), ^(size_t index) {
                    const CString& path = files[index];
                    writer.emitValue("", path);
                });
            }
        });
    });

    jsonEntry = writer.result();

    return Error::none();
}

Error dyld_analyzer::doRun()
{
    // just -v prints version and returns
    if ( _printVersion ) {
        printf("Apple Inc. version ld-%s\n", OS_STRINGIFY(LD_VERSION));
        return Error::none();
    }

    std::string jsonEntry = "";
    if ( Error err = processFiles(jsonEntry) )
        return err;

    // Write the JSON entry to the output file.
    if ( _outPath.empty() ) {
        puts(jsonEntry.c_str());
    } else {
        if ( !safeSave(jsonEntry.data(), jsonEntry.size(), _outPath) )
            return Error("Could not write output file\n");
    }

    return Error::none();
}

} // namespace other_tools
