

#include "../kmod.h"

int startKext() {
	return 0;
}
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo, "1.0.0", startKext, endKext)

int g = 0;

__attribute__((section(("__TEXT, __text"))))
int* gPtr = &g;

int foo() {
	return *gPtr;
}
