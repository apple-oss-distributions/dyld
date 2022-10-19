
#include <mach-o/loader.h>

extern struct mach_header __dso_handle;

const struct mach_header* foo()
{
    return &__dso_handle;
}
