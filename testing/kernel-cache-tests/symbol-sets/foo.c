

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo, "1.0.0", startKext, endKext)

extern int symbol_from_bar();
extern int symbol_from_xnu_no_alias();

int foo() {
	return symbol_from_bar() + symbol_from_xnu_no_alias();
}
