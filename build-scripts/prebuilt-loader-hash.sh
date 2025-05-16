#!/bin/bash
set -e

echo -e "#include \"PrebuiltLoader.h\"\n#include \"PrebuiltObjC.h\"\n#include \"OptimizerObjC.h\"\nint foo() { return sizeof(dyld4::PrebuiltLoader)+sizeof(mach_o::LinkedDylibAttributes)+sizeof(dyld4::PrebuiltLoaderSet)+sizeof(dyld4::ObjCBinaryInfo)+sizeof(dyld4::Loader::DylibPatch)+sizeof(dyld4::Loader::FileValidationInfo)+sizeof(prebuilt_objc::ObjCSelectorMapOnDisk)+sizeof(prebuilt_objc::ObjCObjectMapOnDisk)+sizeof(objc::SelectorHashTable); }\n" > ${DERIVED_FILE_DIR}/test.cpp

# always preprocess headers using macOS SDK
# use "env -i" to remove other env vars set by xcode to keep side-channel info about real platform away from xcrun and clang
env -i DEVELOPER_DIR=${DEVELOPER_DIR} xcrun -v -sdk macosx.internal clang++ -target arm64-apple-macos13.0 -DBUILDING_DYLD=1 -std=c++2a -w -Wno-incompatible-sysroot -fsyntax-only -Xclang -fdump-record-layouts -Icommon -Idyld -Iinclude -Icache-builder -Icache_builder -Ilsl -Imach_o -Iinclude/mach-o ${DERIVED_FILE_DIR}/test.cpp > ${DERIVED_FILE_DIR}/test.out

grep -A100 "class dyld4::PrebuiltLoader"                                            ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pbl.ast
grep -A100 "struct dyld4::PrebuiltLoaderSet"                                        ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pbls.ast
grep -A6   "struct dyld4::PrebuiltLoader::BindTargetRef::Absolute"                  ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/btabs.ast
grep -A100 "struct dyld4::ObjCBinaryInfo"                                           ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pblsobjc.ast
grep -A100 "union mach_o::LinkedDylibAttributes"                                    ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/pbldeps.ast
grep -A100 "struct dyld4::Loader::DylibPatch"                                       ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/dylibpatch.ast
grep -A100 "struct dyld4::Loader::FileValidationInfo"                               ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/fvi.ast
grep -A100 "class dyld3::MapView<struct prebuilt_objc::ObjCStringKeyOnDisk"         ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/objc_sels.ast
grep -A100 "class dyld3::MultiMapView<struct prebuilt_objc::ObjCStringKeyOnDisk"    ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/objc_objects.ast
grep -A100 "class objc::SelectorHashTable"                                          ${DERIVED_FILE_DIR}/test.out | grep -B100 -m1 sizeof= > ${DERIVED_FILE_DIR}/shared_cache_objc_sels.ast
cat ${DERIVED_FILE_DIR}/pbl.ast ${DERIVED_FILE_DIR}/pbls.ast ${DERIVED_FILE_DIR}/pblsobjc.ast ${DERIVED_FILE_DIR}/pbldeps.ast ${DERIVED_FILE_DIR}/dylibpatch.ast ${DERIVED_FILE_DIR}/fvi.ast ${DERIVED_FILE_DIR}/objc_sels.ast ${DERIVED_FILE_DIR}/objc_objects.ast ${DERIVED_FILE_DIR}/shared_cache_objc_sels.ast ${DERIVED_FILE_DIR}/btabs.ast | md5 | awk '{print "#define PREBUILTLOADER_VERSION 0x" substr($0,0,8)}' > ${DERIVED_FILE_DIR}/PrebuiltLoader_version.h


