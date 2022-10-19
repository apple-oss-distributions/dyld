#!/bin/bash

echo -e "#include \"PrebuiltLoader.h\"\nint foo() { return sizeof(dyld4::PrebuiltLoader)+sizeof(dyld4::PrebuiltLoaderSet)+sizeof(dyld4::ObjCBinaryInfo)+sizeof(dyld4::Loader::DylibPatch)+sizeof(dyld4::Loader::FileValidationInfo); }\n" > ${DERIVED_FILE_DIR}/test.cpp
xcrun -sdk macosx clang++ -std=c++2a -w -Wno-incompatible-sysroot -fsyntax-only -Xclang -fdump-record-layouts -Icommon -Idyld -Iinclude -Icache-builder -Icache_builder -Ilsl ${DERIVED_FILE_DIR}/test.cpp > ${DERIVED_FILE_DIR}/test.out
grep -A100 "class dyld4::PrebuiltLoader"                ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pbl.ast
grep -A100 "struct dyld4::PrebuiltLoaderSet"            ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pbls.ast
grep -A100 "struct dyld4::ObjCBinaryInfo"               ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pblsobjc.ast
grep -A100 "struct dyld4::Loader::DylibPatch"           ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/dylibpatch.ast
grep -A100 "struct dyld4::Loader::FileValidationInfo"   ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/dylibpatch.ast
cat ${DERIVED_FILE_DIR}/pbl.ast ${DERIVED_FILE_DIR}/pbls.ast ${DERIVED_FILE_DIR}/pblsobjc.ast ${DERIVED_FILE_DIR}/dylibpatch.ast | md5 | awk '{print "#define PREBUILTLOADER_VERSION 0x" substr($0,0,8)}' > ${DERIVED_FILE_DIR}/PrebuiltLoader_version.h


