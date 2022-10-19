
#include "../kernel-test-runner.h"

#include <mach/kmod.h>

int startKext() {
    return 0;
}
int endKext() {
    return 0;
}

KMOD_EXPLICIT_DECL(com.apple.pageable, "1.0.0", startKext, endKext)

int pageableExport() {
	return 1;
}

int pageableExportDirect() {
    return 42;
}
