
// BUILD:  $CC main.c -o $BUILD_DIR/tpro-enablement.exe

// RUN:  ./tpro-enablement.exe

#include <stdio.h>
#include "test_support.h"


int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    PASS("Success");
}


