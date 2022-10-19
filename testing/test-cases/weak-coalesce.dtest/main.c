
// BUILD:  $CC base.c -dynamiclib -ltest_support -install_name $RUN_DIR/libbase.dylib -o $BUILD_DIR/libbase.dylib

// BUILD:  $CC foo3.c -dynamiclib -ltest_support -install_name $RUN_DIR/libfoo3.dylib -o $BUILD_DIR/libfoo3.dylib $BUILD_DIR/libbase.dylib

// BUILD:  $CC foo2.c -dynamiclib -ltest_support -install_name $RUN_DIR/libfoo2.dylib -o $BUILD_DIR/libfoo2.dylib $BUILD_DIR/libfoo3.dylib $BUILD_DIR/libbase.dylib

// BUILD:  $CC foo1.c -dynamiclib -ltest_support -install_name $RUN_DIR/libfoo1.dylib -o $BUILD_DIR/libfoo1.dylib $BUILD_DIR/libfoo2.dylib $BUILD_DIR/libbase.dylib

// BUILD:  $CC main.c  $BUILD_DIR/libfoo1.dylib $BUILD_DIR/libbase.dylib -o $BUILD_DIR/weak-coalesce.exe

// RUN:  ./weak-coalesce.exe


#include <stdlib.h>

#include "base.h"

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    baseVerifyCoal1("in main", &coal1);
    baseVerifyCoal2("in main", &coal2);

    baseCheck();
}

