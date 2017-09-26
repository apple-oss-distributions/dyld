#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld-interposing.h>


char buffer[100000];
char* p = buffer;

void* mymalloc(size_t size)
{
    // bump ptr allocate twice the size and fill second half with '#'
    char* result = p;
    p += size;
    memset(p, '#', size);
    p += size;
    p = (char*)(((long)p + 15) & (-16)); // 16-byte align next malloc
    return result;
}

void myfree(void* p)
{
}

DYLD_INTERPOSE(mymalloc, malloc)
DYLD_INTERPOSE(myfree, free)
