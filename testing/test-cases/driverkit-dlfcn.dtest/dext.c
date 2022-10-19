#include <dlfcn.h>
#include <mach-o/dyld_priv.h>
#include <os/log.h>
#include <string.h>

#if __has_feature(ptrauth_calls)
    #include <ptrauth.h>
#endif

static const void* stripPointer(const void* ptr)
{
#if __has_feature(ptrauth_calls)
    return __builtin_ptrauth_strip(ptr, ptrauth_key_asia);
#else
    return ptr;
#endif
}

extern struct mach_header __dso_handle;

int bar()
{
    return 0;
}

int foo()
{
    Dl_info info;
    if ( dladdr(&bar, &info) == 0 ) {
        os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn: dladdr failed for 'bar'");
        return 2;
    }
    if ( strncmp(info.dli_sname, "bar", 3) != 0 ) {
        os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn: dli_sname is '%s' instead of 'bar'", info.dli_sname);
        return 3;
    }
    if ( info.dli_saddr != stripPointer(&bar) ) {
        os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn: dli_saddr is not &bar");
        return 4;
    }
    if ( info.dli_fbase != &__dso_handle ) {
        os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn: dli_fbase is not image that contains &bar");
        return 5;
    }
    return 0;
}

int dext_main(void)
{
    os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn");

    __typeof(&foo) symFunc = (__typeof(&foo))dlsym(RTLD_DEFAULT, "foo");
    if ( symFunc == NULL ) {
        os_log(OS_LOG_DEFAULT, "dyld-driverkit-dlfcn: could not find symbol 'foo'");
        return 1;
    }

    return symFunc();
}

__attribute__((constructor))
void init(void)
{
    _dyld_register_driverkit_main((void (*)())dext_main);
}
