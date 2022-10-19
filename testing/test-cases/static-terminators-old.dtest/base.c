#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include "test_support.h"

static bool mainCalled           = false;
static bool libCalled            = false;
static bool libCalledBeforeMain  = false;

void mainTerminated()
{
    mainCalled = true;
}

void libDynamicTerminated()
{
    libCalled = true;
    if ( !mainCalled )
        libCalledBeforeMain = true;
}


static void myTerm()
{
    if ( !mainCalled ) {
        FAIL("main's terminator not called");
    } else if ( !libCalled ) {
        FAIL("libDynamic's terminator not called");
    } else if ( !libCalledBeforeMain ) {
        FAIL("libDynamic's terminator called out of order");
    } else {
        PASS("Success");
    }
}

// don't use attribute destructor because clang will transform
// we want to be built like an old binary
__attribute__((used,section("__DATA,__mod_term_func,mod_term_funcs")))
static void* proc3 = &myTerm;

