#include <stddef.h>

#include "test_support.h"

extern void libDynamicTerminated();


static void myTerm()
{
    LOG("foo static terminator");
    libDynamicTerminated();
}

// don't use attribute destructor because clang will transform
// we want to be built like an old binary
__attribute__((used,section("__DATA,__mod_term_func,mod_term_funcs")))
static void* proc2 = &myTerm;
