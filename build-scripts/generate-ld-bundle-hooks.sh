#!/bin/sh

set -e

if [ -z "${SRCROOT}" ]
then
	echo "You must define SRCROOT"
	exit 1
fi

echo "SRCROOT is: ${SRCROOT}"

passes_dir="${SRCROOT}/ld/passes"
if [ ! -d "${passes_dir}" ]
then
	echo "Expected directory at ${passes_dir}"
	exit 1
fi

bundle_hook_source="${passes_dir}/BundleForClassHook.mm"
if [ ! -f "${bundle_hook_source}" ]
then
	echo "Expected file at ${bundle_hook_source}"
	exit 1
fi

copyright="
/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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
"

function doPlatform() {
	sdk="$1"
	platform="$2"
	clang_platform="$3"
	arch="$4"
	echo "$sdk $platform $clang_platform $arch"

	result_file="${passes_dir}/BundleForClass_${platform}_${arch}.h"
	echo "result: $result_file"

	set +e
	xcrun -sdk "${sdk}" --show-sdk-path 2> /dev/null > /dev/null
	result=$?
	set -e
	if [ $result -ne 0 ]
	then
		echo "Skipping missing SDK: $sdk"
		return
	fi

	tmpfile=$(mktemp /tmp/ld-bundle.XXXXXX)
	xcrun -sdk "${sdk}" clang -std=c++11 -fno-exceptions -O2 -c "${bundle_hook_source}" -target "${arch}-apple-${clang_platform}" -o "${tmpfile}"

	echo "${copyright}" > $result_file
	echo "__attribute__((aligned((4096))))" >> $result_file
	echo "static unsigned char BundleForClassHook_${platform}_${arch}[] = {" >> $result_file
	cat "${tmpfile}" | xxd --include >> $result_file
	echo "};" >> $result_file
}

doPlatform "macosx" "macos" "macos11.0" "x86_64"
doPlatform "macosx" "macos" "macos11.0" "arm64"
doPlatform "macosx" "macos" "macos11.0" "arm64e"
doPlatform "iphoneos" "ios" "ios12.0" "arm64"
doPlatform "iphoneos" "ios" "ios12.0" "arm64e"
doPlatform "iphonesimulator" "ios_sim" "ios12.0.0-simulator" "x86_64"
doPlatform "iphonesimulator" "ios_sim" "ios12.0.0-simulator" "arm64"
doPlatform "appletvos" "tvos" "tvos12.0" "arm64"
doPlatform "appletvos" "tvos" "tvos12.0" "arm64e"
doPlatform "appletvsimulator" "tvos_sim" "tvos12.0.0-simulator" "x86_64"
doPlatform "appletvsimulator" "tvos_sim" "tvos12.0.0-simulator" "arm64"
doPlatform "watchos" "watchos" "watchos9.0" "arm64_32"
doPlatform "watchos" "watchos" "watchos9.0" "arm64"
doPlatform "watchos" "watchos" "watchos9.0" "arm64e"
doPlatform "watchsimulator" "watchos_sim" "watchos12.0.0-simulator" "x86_64"
doPlatform "watchsimulator" "watchos_sim" "watchos12.0.0-simulator" "arm64"
doPlatform "xros" "xros" "xros1.0" "arm64"
doPlatform "xros" "xros" "xros1.0" "arm64e"
doPlatform "xrsimulator" "xros_sim" "xros1.0.0-simulator" "x86_64"
doPlatform "xrsimulator" "xros_sim" "xros1.0.0-simulator" "arm64"