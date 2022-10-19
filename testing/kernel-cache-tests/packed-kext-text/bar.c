

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.bar, "1.0.0", (void*)startKext, (void*)endKext)

// Hack to force a section on __TEXT as otherwise its given the vmSize by forEachSegment
__attribute__((used, section("__TEXT, __const")))
int packHack = 0;

extern int foo();

int bar() {
	return foo();
}