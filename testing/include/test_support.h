#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <sys/param.h>

#if __LP64__
extern struct mach_header_64 __dso_handle;
#else
extern struct mach_header __dso_handle;
#endif

static bool sIsATTY = false;
static const char * sTestName = NULL;
static uint64_t sTestCount = 0;

__attribute__((constructor))
static
void BEGIN(int argc, const char* argv[], const char* envp[])  {
    // Set up values we need to print in PASS() and FAIL()
    sIsATTY = isatty(fileno(stdout));
    sTestName = argv[0];
    // Early returnif this not the main executbale, we only need to print the [BEGIN] line once
    if (__dso_handle.filetype != MH_EXECUTE) {
        return;
    }
    printf("[BEGIN]");
    for (uint32_t i = 0; envp[i] != NULL; ++i) {
        if (strncmp("DYLD_", envp[i], 5) == 0) {
            printf(" %s", envp[i]);
        }
    }
    char buffer[MAXPATHLEN];
    uint32_t bufsize = MAXPATHLEN;
    if (_NSGetExecutablePath(buffer, &bufsize) == 0) {
        printf(" %s", buffer);
    } else {
        printf(" %s", argv[0]);
    }
    for (uint32_t i = 1; i < argc; ++i) {
        printf (" %s", argv[i]);
    }
    printf("\n");
}

__attribute__((format(printf, 1, 2)))
static
void PASS(const char *format, ...)  {
    if (sIsATTY) {
        printf("[\033[0;32mPASS\033[0m] %s (%llu): ", sTestName, sTestCount++);
    } else {
        printf("[PASS] %s (%llu): ", sTestName, sTestCount++);
    }
    va_list args;
    va_start (args, format);
    vprintf (format, args);
    va_end (args);
    printf("\n");
}

__attribute__((format(printf, 1, 2)))
static
void FAIL(const char *format, ...) {
    if (sIsATTY) {
        printf("[\033[0;31mFAIL\033[0m] %s (%llu): ", sTestName, sTestCount++);
    } else {
        printf("[FAIL] %s (%llu): ", sTestName, sTestCount++);
    }
    va_list args;
    va_start (args, format);
    vprintf (format, args);
    va_end (args);
    printf("\n");
}
