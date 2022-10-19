
#include "../kernel-test-runner.h"

#include <mach/kmod.h>

int startKext() {
    return 0;
}
int endKext() {
    return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo, "1.0.0", startKext, endKext)

extern int bar();
__typeof(&bar) barPtr = &bar;

int foo() {
	return barPtr() + 4;
}

__attribute__((constructor))
int test(const TestRunnerFunctions* hostFuncs) {
    LOG("test(): start");
    // pageable, bar, and foo each added 1, 2, 4, so we need to return 7 to know this worked
	int v = foo();
    if ( v != 7 ) {
        FAIL("foo() returned %d vs expected 7", v);
    }
    LOG("test(): end");
    return 0;
}

int fooDirect() {
    return bar() + 4;
}

// Test direct pointer fixups, ie, not via a GOT
__attribute__((constructor))
int testDirect(const TestRunnerFunctions* hostFuncs) {
    LOG("testDirect(): start");
    // pageable, bar, and foo each added 1, 2, 4, so we need to return 7 to know this worked
    int v = fooDirect();
    if ( v != 7 ) {
        FAIL("fooDirect() returned %d vs expected 7", v);
    }
    LOG("testDirect(): end");
    return 0;
}
