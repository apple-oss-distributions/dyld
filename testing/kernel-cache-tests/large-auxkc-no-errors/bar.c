

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

__attribute__((used))
char largeBuffer[512 * 1024 * 1024];

extern int foo();

int bar() {
	return foo();
}