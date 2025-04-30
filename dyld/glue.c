/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#define _FORTIFY_SOURCE 0

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <TargetConditionals.h>
#if TARGET_OS_EXCLAVEKIT
  #include <liblibc/liblibc.h>
  #include <liblibc/file.h>
  extern void abort_report_np(const char* format, ...) __attribute__((noreturn,format(printf, 1, 2)));
  extern void __assert_rtn(const char* func, const char* file, int line, const char* failedexpr);
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <sys/stat.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <sys/ioctl.h>
  #include <sys/sysctl.h>
  #include <libkern/OSAtomic.h>
  #include <libc_private.h>
  #include <pthread.h>
  #include <corecrypto/ccdigest.h>
  #include <corecrypto/ccsha1.h>
  #include <corecrypto/ccsha2.h>
  #include <_simple.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <uuid/uuid.h>
#include <errno.h>

#if TARGET_OS_SIMULATOR
	#include "dyldSyscallInterface.h"
    #include <mach-o/dyld_images.h>
	#include <mach-o/loader.h>
	#include <mach-o/nlist.h>
	#include <mach/kern_return.h>
	#if __LP64__
		#define LC_SEGMENT_COMMAND			LC_SEGMENT_64
		typedef struct segment_command_64	macho_segment_command;
		typedef struct mach_header_64		macho_header;
		typedef struct nlist_64				macho_nlist;
	#else
		#define LC_SEGMENT_COMMAND			LC_SEGMENT
		typedef struct segment_command		macho_segment_command;
		typedef struct mach_header			macho_header;
		typedef struct nlist				macho_nlist;
	#endif

    #define DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE    (32*1024)
    #define DYLD_PROCESS_INFO_NOTIFY_LOAD_ID            0x1000
    #define DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID          0x2000
    #define DYLD_PROCESS_INFO_NOTIFY_MAIN_ID            0x3000

    struct dyld_process_info_image_entry {
        uuid_t                        uuid;
        uint64_t                    loadAddress;
        uint32_t                    pathStringOffset;
        uint32_t                    pathLength;
    };

    struct dyld_process_info_notify_header {
        mach_msg_header_t            header;
        uint32_t                    version;
        uint32_t                    imageCount;
        uint32_t                    imagesOffset;
        uint32_t                    stringsOffset;
        uint64_t                    timestamp;
    };
#endif

// dyld::log(const char* format, ...)
extern void _ZN5dyld43logEPKcz(const char*, ...);

// dyld::halt(const char* msg, const StructuredError*);
extern void halt(const char* msg, void* extra) __attribute__((__noreturn__)) __asm("__ZN5dyld44haltEPKcPKNS_15StructuredErrorE");

extern void dyld_fatal_error(const char* errString) __attribute__((__noreturn__));

__attribute__((__noreturn__))
void _libcpp_verbose_abort(const char* msg, ...) __asm("__ZNSt3__122__libcpp_verbose_abortEPKcz");
void _libcpp_verbose_abort(const char* msg, ...)
{
    halt(msg, NULL);
    __builtin_unreachable(); // never reached, but needed to tell the compiler that the function never returns
}


extern void __cxa_pure_virtual(void);
void __cxa_pure_virtual()
{
    abort_report_np("Pure virtual method called");
}


// called by misc code, assume using stderr
int	fprintf(FILE* file, const char* format, ...)
{
	va_list	list;
	va_start(list, format);
#if TARGET_OS_EXCLAVEKIT
    vfprintf(file, format, list);
#else
	_simple_vdprintf(STDERR_FILENO, format, list);
#endif
	va_end(list);
	return 0;
}

// called by LIBC_ABORT
void abort_report_np(const char* format, ...)
{
    char strBuf[1024];
    va_list list;
    va_start(list, format);
    vsnprintf(strBuf, sizeof(strBuf), format, list);
    va_end(list);
    halt(strBuf, NULL);
}

// libc uses assert()
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-noreturn"
void __assert_rtn(const char* func, const char* file, int line, const char* failedexpr)
{
    if (func == NULL) {
		//_ZN5dyld43logEPKcz("Assertion failed: (%s), file %s, line %d.\n", failedexpr, file, line);
        abort_report_np("Assertion failed: (%s), file %s, line %d.\n", failedexpr, file, line);
    } else {
		//_ZN5dyld43logEPKcz("Assertion failed: (%s), function %s, file %s, line %d.\n", failedexpr, func, file, line);
        abort_report_np("Assertion failed: (%s), function %s, file %s, line %d.\n", failedexpr, func, file, line);
    }
}
#pragma clang diagnostic pop

#if !TARGET_OS_EXCLAVEKIT

// abort various libc.a and libc++.a functions
void abort()
{
    halt("dyld calling abort()\n", NULL);
}

// clang sometimes optimizes fprintf to fwrite
size_t fwrite(const void* ptr, size_t size, size_t nitme, FILE* stream)
{
    return write(STDERR_FILENO, ptr, size*nitme);
}

int vsnprintf(char* str, size_t size, const char*  format, va_list list)
{
    _SIMPLE_STRING s = _simple_salloc();
    int result = _simple_vsprintf(s, format, list);
    strlcpy(str, _simple_string(s), size);
    _simple_sfree(s);
    return result;
}

int snprintf(char* str, size_t size, const char* format, ...)
{
    va_list list;
    va_start(list, format);
    int result = vsnprintf(str, size, format, list);
    va_end(list);
    return result;
}

//
// The stack protector routines in lib.c bring in too much stuff, so
// make our own custom ones.
//
__attribute__((section("__TPRO_CONST,__data")))
long __stack_chk_guard = 0;

extern void __guard_setup(const char* apple[]);
void __guard_setup(const char* apple[])
{
	for (const char** p = apple; *p != NULL; ++p) {
		if ( strncmp(*p, "stack_guard=", 12) == 0 ) {
			// kernel has provide a random value for us
			for (const char* s = *p + 12; *s != '\0'; ++s) {
				char c = *s;
				long value = 0;
				if ( (c >= 'a') && (c <= 'f') )
					value = c - 'a' + 10;
				else if ( (c >= 'A') && (c <= 'F') )
					value = c - 'A' + 10;
				else if ( (c >= '0') && (c <= '9') )
					value = c - '0';
				__stack_chk_guard <<= 4;
				__stack_chk_guard |= value;
			}
			if ( __stack_chk_guard != 0 )
				return;
		}
	}
#if !TARGET_OS_SIMULATOR
  #if __LP64__
    __stack_chk_guard = ((long)arc4random() << 32) | arc4random();
  #else
    __stack_chk_guard = arc4random();
  #endif
#endif
}

extern void __stack_chk_fail(void);
void __stack_chk_fail()
{
    halt("stack buffer overrun", NULL);
}
#endif // !TARGET_OS_EXCLAVEKIT

// _pthread_reap_thread calls fprintf(stderr).
// We map fprint to _simple_vdprintf and ignore FILE* stream, so ok for it to be NULL
FILE* __stderrp = NULL;
FILE* __stdoutp = NULL;


#if 0
// libc.a sometimes missing memset
#undef memset
void* memset(void* b, int c, size_t len)
{
	uint8_t* p = (uint8_t*)b;
	for(size_t i=len; i > 0; --i)
		*p++ = c;
	return b;
}
#endif

//
// The dyld in the iOS simulator cannot do syscalls, so it calls back to
// host dyld.
//

#if TARGET_OS_SIMULATOR

int open(const char* path, int oflag, ...) {
	int retval;

	va_list args;
	va_start(args, oflag);
	retval = gSyscallHelpers->open(path, oflag, va_arg(args, int));
	va_end(args);

	return retval;
}

int close(int fd) {
	return gSyscallHelpers->close(fd);
}

int openat(int fd, const char *path, int oflag, ...)
{
    char pathBuffer[PATH_MAX] = { 0 };
    int result = fcntl(fd, F_GETPATH, pathBuffer);
    if ( result == -1 )
        return -1;
    strlcat(pathBuffer, "/", PATH_MAX);
    strlcat(pathBuffer, path, PATH_MAX);

    va_list args;
    va_start(args, oflag);
    result = gSyscallHelpers->open(pathBuffer, oflag, va_arg(args, int));
    va_end(args);

    return result;
}

ssize_t pread(int fd, void* buf, size_t nbytes, off_t offset) {
	return gSyscallHelpers->pread(fd, buf , nbytes, offset);
}

ssize_t write(int fd, const void *buf, size_t nbytes) {
	return gSyscallHelpers->write(fd, buf , nbytes);
}

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
	return gSyscallHelpers->mmap(addr, len, prot, flags, fd, offset);
}

int munmap(void* addr, size_t len) {
	return gSyscallHelpers->munmap(addr, len);
}

int madvise(void* addr, size_t len, int advice) {
	return gSyscallHelpers->madvise(addr, len, advice);
}

int stat(const char* path, struct stat* buf) {
	return gSyscallHelpers->stat(path, buf);
}

int fcntl(int fd, int cmd, ...) {
        int retval;

        va_list args;
        va_start(args, cmd);
        retval = gSyscallHelpers->fcntl(fd, cmd, va_arg(args, void *));
        va_end(args);

	return retval;
}

int ioctl(int fd, unsigned long request, ...) {
        int retval;

        va_list args;
        va_start(args, request);
        retval = gSyscallHelpers->ioctl(fd, request, va_arg(args, void *));
        va_end(args);

	return retval;
}

int issetugid() {
	return gSyscallHelpers->issetugid();
}

char* getcwd(char* buf, size_t size) {
	return gSyscallHelpers->getcwd(buf, size);
}

char* realpath(const char* file_name, char* resolved_name) {
	return gSyscallHelpers->realpath(file_name, resolved_name);
}



kern_return_t vm_allocate(vm_map_t target_task, vm_address_t *address,
						  vm_size_t size, int flags) {
	return gSyscallHelpers->vm_allocate(target_task, address, size, flags);
}

kern_return_t vm_deallocate(vm_map_t target_task, vm_address_t address,
							vm_size_t size) {
	return gSyscallHelpers->vm_deallocate(target_task, address, size);
}

kern_return_t vm_protect(vm_map_t target_task, vm_address_t address,
							vm_size_t size, boolean_t max, vm_prot_t prot) {
	return gSyscallHelpers->vm_protect(target_task, address, size, max, prot);
}


void _ZN5dyld43logEPKcz(const char* format, ...) {
	va_list	list;
	va_start(list, format);
	gSyscallHelpers->vlog(format, list);
	va_end(list);
}

extern void _ZN4dyld4vlogEPKcP13__va_list_tag(const char* format, va_list list);
void _ZN4dyld4vlogEPKcP13__va_list_tag(const char* format, va_list list)
{
	gSyscallHelpers->vlog(format, list);
}


extern void _ZN4dyld4warnEPKcz(const char* format, ...);
void _ZN4dyld4warnEPKcz(const char* format, ...) {
	va_list	list;
	va_start(list, format);
	gSyscallHelpers->vwarn(format, list);
	va_end(list);
}


int pthread_mutex_lock(pthread_mutex_t* m) {
	return gSyscallHelpers->pthread_mutex_lock(m);
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
	return gSyscallHelpers->pthread_mutex_unlock(m);
}

mach_port_t mach_thread_self() {
	return gSyscallHelpers->mach_thread_self();
}

kern_return_t mach_port_deallocate(ipc_space_t task, mach_port_name_t name) {
	return gSyscallHelpers->mach_port_deallocate(task, name);
}

mach_port_name_t task_self_trap() {
	return gSyscallHelpers->task_self_trap();
}

kern_return_t mach_timebase_info(mach_timebase_info_t info) {
	return gSyscallHelpers->mach_timebase_info(info);
}

bool myOSAtomicCompareAndSwapPtrBarrier(void* old, void* new, void * volatile *value) __asm("_OSAtomicCompareAndSwapPtrBarrier");
bool myOSAtomicCompareAndSwapPtrBarrier(void* old, void* new, void * volatile *value) {
	return gSyscallHelpers->OSAtomicCompareAndSwapPtrBarrier(old, new, value);
}

void myOSMemoryBarrier(void) __asm("_OSMemoryBarrier");
void myOSMemoryBarrier()  {
	return gSyscallHelpers->OSMemoryBarrier();
}

uint64_t mach_absolute_time(void) {
	return gSyscallHelpers->mach_absolute_time();
}

kern_return_t thread_switch(mach_port_name_t thread_name,
							int option, mach_msg_timeout_t option_time) {
	if ( gSyscallHelpers->version < 2 )
		return KERN_FAILURE;
	return gSyscallHelpers->thread_switch(thread_name, option, option_time);
}

DIR* opendir(const char* path) {
	if ( gSyscallHelpers->version < 3 )
		return NULL;
	return gSyscallHelpers->opendir(path);
}

int	readdir_r(DIR* dirp, struct dirent* entry, struct dirent **result) {
	if ( gSyscallHelpers->version < 3 )
		return EPERM;
	return gSyscallHelpers->readdir_r(dirp, entry, result);
}

// HACK: readdir() is not used in dyld_sim, but it is pulled in by libc.a, then dead stripped.
struct dirent* readdir(DIR *dirp) {
    halt("dyld_sim readdir() not supported\n", NULL);
}

int closedir(DIR* dirp) {
	if ( gSyscallHelpers->version < 3 )
		return EPERM;
	return gSyscallHelpers->closedir(dirp);
}

int mprotect(void* addr, size_t len, int prot)
{
    return vm_protect(mach_task_self(), (vm_address_t)addr,  len, false, prot);
}


#define SUPPORT_HOST_10_11  1

#if SUPPORT_HOST_10_11
typedef int               (*FuncPtr_proc_regionfilename)(int pid, uint64_t address, void* buffer, uint32_t bufferSize);
typedef pid_t             (*FuncPtr_getpid)(void);
typedef bool              (*FuncPtr_mach_port_insert_right)(ipc_space_t task, mach_port_name_t name, mach_port_t poly, mach_msg_type_name_t polyPoly);
typedef kern_return_t     (*FuncPtr_mach_port_allocate)(ipc_space_t, mach_port_right_t, mach_port_name_t*);
typedef mach_msg_return_t (*FuncPtr_mach_msg)(mach_msg_header_t *, mach_msg_option_t , mach_msg_size_t , mach_msg_size_t , mach_port_name_t , mach_msg_timeout_t , mach_port_name_t);
typedef void              (*FuncPtr_mach_msg_destroy)(mach_msg_header_t *);
typedef kern_return_t     (*FuncPtr_mach_port_construct)(ipc_space_t task, mach_port_options_ptr_t options, mach_port_context_t context, mach_port_name_t *name);
typedef kern_return_t     (*FuncPtr_mach_port_destruct)(ipc_space_t task, mach_port_name_t name, mach_port_delta_t srdelta, mach_port_context_t guard);

static FuncPtr_proc_regionfilename		 proc_proc_regionfilename = NULL;
static FuncPtr_getpid                    proc_getpid = NULL;
static FuncPtr_mach_port_insert_right    proc_mach_port_insert_right = NULL;
static FuncPtr_mach_port_allocate        proc_mach_port_allocate = NULL;
static FuncPtr_mach_msg                  proc_mach_msg = NULL;
static FuncPtr_mach_msg_destroy          proc_mach_msg_destroy = NULL;
static FuncPtr_mach_port_construct       proc_mach_port_construct = NULL;
static FuncPtr_mach_port_destruct        proc_mach_port_destruct = NULL;

static mach_port_t* sNotifyReplyPorts = NULL;
static bool*        sZombieNotifiers = NULL;

// Look up sycalls in host dyld needed by coresymbolication_ routines in dyld_sim
static void findHostFunctions() {
	// Only look up symbols once
	if ( proc_mach_msg != NULL )
		return;

	struct dyld_all_image_infos* imageInfo = (struct dyld_all_image_infos*)(gSyscallHelpers->getProcessInfo());
	const struct mach_header* hostDyldMH = imageInfo->dyldImageLoadAddress;

	// find symbol table and slide of host dyld
	uintptr_t slide = 0;
	const macho_nlist* symbolTable = NULL;
	const char* symbolTableStrings = NULL;
	const struct dysymtab_command* dynSymbolTable = NULL;
	const uint32_t cmd_count = hostDyldMH->ncmds;
	const struct load_command* const cmds = (struct load_command*)(((char*)hostDyldMH)+sizeof(macho_header));
	const struct load_command* cmd = cmds;
	const uint8_t* linkEditBase = NULL;
	for (uint32_t i = 0; i < cmd_count; ++i) {
		switch (cmd->cmd) {
			case LC_SEGMENT_COMMAND:
				{
					const macho_segment_command* seg = (macho_segment_command*)cmd;
					if ( (seg->fileoff == 0) && (seg->filesize != 0) )
						slide = (uintptr_t)hostDyldMH - seg->vmaddr;
					if ( strcmp(seg->segname, "__LINKEDIT") == 0 )
						linkEditBase = (uint8_t*)(seg->vmaddr - seg->fileoff + slide);
				}
				break;
			case LC_SYMTAB:
				{
					const struct symtab_command* symtab = (struct symtab_command*)cmd;
					if ( linkEditBase == NULL )
						return;
					symbolTableStrings = (const char*)&linkEditBase[symtab->stroff];
					symbolTable = (macho_nlist*)(&linkEditBase[symtab->symoff]);
				}
				break;
			case LC_DYSYMTAB:
				dynSymbolTable = (struct dysymtab_command*)cmd;
				break;
		}
		cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
	}
	if ( symbolTableStrings == NULL )
		return;
	if ( dynSymbolTable == NULL )
		return;

	// scan local symbols in host dyld looking for load/unload functions
	const macho_nlist* const localsStart = &symbolTable[dynSymbolTable->ilocalsym];
	const macho_nlist* const localsEnd= &localsStart[dynSymbolTable->nlocalsym];
	for (const macho_nlist* s = localsStart; s < localsEnd; ++s) {
 		if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
			const char* name = &symbolTableStrings[s->n_un.n_strx];
			if ( strcmp(name, "_proc_regionfilename") == 0 )
				proc_proc_regionfilename = (FuncPtr_proc_regionfilename)(s->n_value + slide);
			else if ( strcmp(name, "_getpid") == 0 )
				proc_getpid = (FuncPtr_getpid)(s->n_value + slide);
			else if ( strcmp(name, "mach_port_insert_right") == 0 )
				proc_mach_port_insert_right = (FuncPtr_mach_port_insert_right)(s->n_value + slide);
			else if ( strcmp(name, "_mach_port_allocate") == 0 )
				proc_mach_port_allocate = (FuncPtr_mach_port_allocate)(s->n_value + slide);
			else if ( strcmp(name, "_mach_msg") == 0 )
                proc_mach_msg = (FuncPtr_mach_msg)(s->n_value + slide);
            else if (strcmp(name, "__ZN4dyldL17sNotifyReplyPortsE"))
                sNotifyReplyPorts = (mach_port_t*)(s->n_value + slide);
            else if (strcmp(name, "__ZN4dyldL16sZombieNotifiersE"))
                sZombieNotifiers = (bool *)(s->n_value + slide);
		}
	}
}

// Look up sycalls in host dyld needed by coresymbolication_ routines in dyld_sim
static bool findHostLibSystemFunctions() {
    // Only look up symbols once
    if (proc_mach_msg_destroy != NULL && proc_mach_port_construct != NULL && proc_mach_port_destruct != NULL)
        return true;

    const struct mach_header* hostLibSystemMH = NULL;
    struct dyld_all_image_infos* imageInfo = (struct dyld_all_image_infos*)(gSyscallHelpers->getProcessInfo());
    const struct dyld_image_info* infoArray = imageInfo->infoArray;
    if (infoArray == NULL)
        return false;
    uint32_t imageCount = imageInfo->infoArrayCount;
    for (uint32_t i = 0; i<imageCount; ++i) {
        if (strcmp("/usr/lib/system/libsystem_kernel.dylib", infoArray[i].imageFilePath) == 0) {
            //Found the kernel interface
            hostLibSystemMH = infoArray[i].imageLoadAddress;
            break;
        }
    }
    if (hostLibSystemMH == NULL)
        return false;

    // find symbol table and slide of host dyld
    uintptr_t slide = 0;
    const macho_nlist* symbolTable = NULL;
    const char* symbolTableStrings = NULL;
    const struct dysymtab_command* dynSymbolTable = NULL;
    const uint32_t cmd_count = hostLibSystemMH->ncmds;
    const struct load_command* const cmds = (struct load_command*)(((char*)hostLibSystemMH)+sizeof(macho_header));
    const struct load_command* cmd = cmds;
    const uint8_t* linkEditBase = NULL;
    for (uint32_t i = 0; i < cmd_count; ++i) {
        switch (cmd->cmd) {
            case LC_SEGMENT_COMMAND:
            {
                const macho_segment_command* seg = (macho_segment_command*)cmd;
                if ( (seg->fileoff == 0) && (seg->filesize != 0) )
                    slide = (uintptr_t)hostLibSystemMH - seg->vmaddr;
                if ( strcmp(seg->segname, "__LINKEDIT") == 0 )
                    linkEditBase = (uint8_t*)(seg->vmaddr - seg->fileoff + slide);
            }
                break;
            case LC_SYMTAB:
            {
                const struct symtab_command* symtab = (struct symtab_command*)cmd;
                if ( linkEditBase == NULL )
                    return false;
                symbolTableStrings = (const char*)&linkEditBase[symtab->stroff];
                symbolTable = (macho_nlist*)(&linkEditBase[symtab->symoff]);
            }
                break;
            case LC_DYSYMTAB:
                dynSymbolTable = (struct dysymtab_command*)cmd;
                break;
        }
        cmd = (const struct load_command*)(((char*)cmd)+cmd->cmdsize);
    }
    if ( symbolTableStrings == NULL )
        return false;
    if ( dynSymbolTable == NULL )
        return false;

    // scan local symbols in host dyld looking for load/unload functions
    const macho_nlist* const localsStart = &symbolTable[dynSymbolTable->iextdefsym];
    const macho_nlist* const localsEnd= &localsStart[dynSymbolTable->nextdefsym];
    for (const macho_nlist* s = localsStart; s < localsEnd; ++s) {
        if ( ((s->n_type & N_TYPE) == N_SECT) && ((s->n_type & N_STAB) == 0) ) {
            const char* name = &symbolTableStrings[s->n_un.n_strx];
            if ( strcmp(name, "_mach_msg_destroy") == 0 )
                proc_mach_msg_destroy = (FuncPtr_mach_msg_destroy)(s->n_value + slide);
            else if ( strcmp(name, "_mach_port_construct") == 0 )
                proc_mach_port_construct = (FuncPtr_mach_port_construct)(s->n_value + slide);
            else if ( strcmp(name, "_mach_port_destruct") == 0 )
                proc_mach_port_destruct = (FuncPtr_mach_port_destruct)(s->n_value + slide);
        }
    }
    return (proc_mach_msg_destroy != NULL && proc_mach_port_construct != NULL && proc_mach_port_destruct != NULL);
}
#endif


int proc_regionfilename(int pid, uint64_t address, void* buffer, uint32_t bufferSize)
{
	if ( gSyscallHelpers->version >= 5 )
		return gSyscallHelpers->proc_regionfilename(pid, address, buffer, bufferSize);
#if SUPPORT_HOST_10_11
	findHostFunctions();
	if ( proc_proc_regionfilename )
		return (*proc_proc_regionfilename)(pid, address, buffer, bufferSize);
	else
		return 0;
#else
	return 0;
#endif
}

pid_t getpid()
{
	if ( gSyscallHelpers->version >= 5 )
		return gSyscallHelpers->getpid();
#if SUPPORT_HOST_10_11
	findHostFunctions();
	return (*proc_getpid)();
#else
	return 0;
#endif
}

kern_return_t mach_port_insert_right(ipc_space_t task, mach_port_name_t name, mach_port_t poly, mach_msg_type_name_t polyPoly)
{
	if ( gSyscallHelpers->version >= 5 )
		return gSyscallHelpers->mach_port_insert_right(task, name, poly, polyPoly);
#if SUPPORT_HOST_10_11
	findHostFunctions();
	if ( proc_mach_port_insert_right )
		return (*proc_mach_port_insert_right)(task, name, poly, polyPoly);
	else
		return KERN_NOT_SUPPORTED;
#else
	return KERN_NOT_SUPPORTED;
#endif
}

kern_return_t mach_port_allocate(ipc_space_t task, mach_port_right_t right, mach_port_name_t* name)
{
	if ( gSyscallHelpers->version >= 5 )
		return gSyscallHelpers->mach_port_allocate(task, right, name);
#if SUPPORT_HOST_10_11
	findHostFunctions();
	return (*proc_mach_port_allocate)(task, right, name);
#else
	return KERN_NOT_SUPPORTED;
#endif
}

kern_return_t mach_msg(mach_msg_header_t* msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size, mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify)
{
	if ( gSyscallHelpers->version >= 5 )
		return gSyscallHelpers->mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
#if SUPPORT_HOST_10_11
	findHostFunctions();
	return (*proc_mach_msg)(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
#else
	return KERN_NOT_SUPPORTED;
#endif
}

void mach_msg_destroy(mach_msg_header_t *msg) {
    if ( gSyscallHelpers->version >= 12 ) {
        gSyscallHelpers->mach_msg_destroy(msg);
        return;
    }
#if SUPPORT_HOST_10_11
    if (findHostLibSystemFunctions()) {
        (*proc_mach_msg_destroy)(msg);
    }
#endif
}

kern_return_t mach_port_construct(ipc_space_t task, mach_port_options_ptr_t options, mach_port_context_t context, mach_port_name_t *name) {
    if ( gSyscallHelpers->version >= 12 ) {
        return gSyscallHelpers->mach_port_construct(task, options, context, name);
    }
#if SUPPORT_HOST_10_11
    if (findHostLibSystemFunctions()) {
        return (*proc_mach_port_construct)(task, options, context, name);
    }
#endif
    return KERN_NOT_SUPPORTED;
}

kern_return_t mach_port_destruct(ipc_space_t task, mach_port_name_t name, mach_port_delta_t srdelta, mach_port_context_t guard) {
    if ( gSyscallHelpers->version >= 12 ) {
        return gSyscallHelpers->mach_port_destruct(task, name, srdelta, guard);
    }
#if SUPPORT_HOST_10_11
    if (findHostLibSystemFunctions()) {
        return (*proc_mach_port_destruct)(task, name, srdelta, guard);
    }
#endif
    return KERN_NOT_SUPPORTED;
}

void abort_with_payload(uint32_t reason_namespace, uint64_t reason_code, void* payload, uint32_t payload_size, const char* reason_string, uint64_t reason_flags)
{
	if ( gSyscallHelpers->version >= 6 )
		gSyscallHelpers->abort_with_payload(reason_namespace, reason_code, payload, payload_size, reason_string, reason_flags);
    halt(reason_string, NULL);
}

kern_return_t   task_info(task_name_t target_task, task_flavor_t flavor, task_info_t task_info_out, mach_msg_type_number_t *task_info_outCnt) {
    if ( gSyscallHelpers->version >= 8 )
        return gSyscallHelpers->task_info(target_task, flavor, task_info_out, task_info_outCnt);
    return KERN_NOT_SUPPORTED;
}

kern_return_t   thread_info(thread_inspect_t target_act, thread_flavor_t flavor, thread_info_t thread_info_out, mach_msg_type_number_t *thread_info_outCnt) {
    if ( gSyscallHelpers->version >= 8 )
        return gSyscallHelpers->task_info(target_act, flavor, thread_info_out, thread_info_outCnt);
    return KERN_NOT_SUPPORTED;
}

bool kdebug_is_enabled(uint32_t code) {
    if ( gSyscallHelpers->version >= 8 )
        return gSyscallHelpers->kdebug_is_enabled(code);
    return false;
}

int kdebug_trace(uint32_t code, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    if ( gSyscallHelpers->version >= 8 )
        return gSyscallHelpers->kdebug_trace(code, arg1, arg2, arg3, arg4);
    return 0;
}

uint64_t kdebug_trace_string(uint32_t debugid, uint64_t str_id, const char *str) {
    if ( gSyscallHelpers->version >= 9 )
        return gSyscallHelpers->kdebug_trace_string(debugid, str_id, str);
    return 0;
}

int amfi_check_dyld_policy_self(uint64_t inFlags, uint64_t* outFlags)
{
    if ( gSyscallHelpers->version >= 10 )
        return gSyscallHelpers->amfi_check_dyld_policy_self(inFlags, outFlags);
    *outFlags = 0x3F;  // on old kernel, simulator process get all flags
    return 0;
}

kern_return_t vm_copy(vm_map_t task, vm_address_t source_address, vm_size_t size, vm_address_t dest_address)
{
    if ( gSyscallHelpers->version >= 13 )
        return gSyscallHelpers->vm_copy(task, source_address, size, dest_address);
    return KERN_FAILURE;
}

int fstat(int fd, struct stat* buf)
{
    if ( gSyscallHelpers->version >= 13 )
        return gSyscallHelpers->fstat(fd, buf);
    return -1;
}

ssize_t fsgetpath(char* result, size_t resultBufferSize, fsid_t* fsID, uint64_t objID)
{
    if ( gSyscallHelpers->version >= 15 )
        return gSyscallHelpers->fsgetpath(result, resultBufferSize, fsID, objID);
    return -1;
}

int getattrlistbulk(int fd, void* attrList, void* attrBuf, size_t bufSize, uint64_t options)
{
    if ( gSyscallHelpers->version >= 16 )
        return gSyscallHelpers->getattrlistbulk(fd, attrList, attrList, bufSize, options);
    return -1;
}

#ifdef __LP64__
int getattrlist(const char* path, void* attrList, void * attrBuf, size_t attrBufSize, unsigned int options)
#else /* __LP64__ */
int getattrlist(const char* path, void* attrList, void * attrBuf, size_t attrBufSize, unsigned long options)
#endif
{
    if ( gSyscallHelpers->version >= 17 )
        return gSyscallHelpers->getattrlist(path, attrList, attrBuf, attrBufSize, options);
    return -1;
}

int getfsstat(struct statfs *buf, int bufsize, int flags) {
    if ( gSyscallHelpers->version >= 17 )
        return gSyscallHelpers->getfsstat(buf, bufsize, flags);
    return -1;
}

int sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen)
{
    if ( gSyscallHelpers->version >= 18 )
        return gSyscallHelpers->sysctlbyname(name, oldp, oldlenp, newp, newlen);
    return -1;
}

int* __error(void) {
    return gSyscallHelpers->errnoAddress();
}

extern void mach_init(void);
void mach_init() {
    mach_task_self_ = task_self_trap();
    //_task_reply_port = _mach_reply_port();
}

mach_port_t mach_task_self_ = MACH_PORT_NULL;

extern int myerrno_fallback  __asm("_errno");
int myerrno_fallback = 0;


vm_size_t vm_kernel_page_mask = 0xFFF;
vm_size_t vm_page_size = 0x1000;

#endif  // TARGET_OS_SIMULATOR


void* _NSConcreteStackBlock[32];
void* _NSConcreteGlobalBlock[32];

extern void _Block_object_assign(void * p1, const void * p2, const int p3);
void _Block_object_assign(void * p1, const void * p2, const int p3)
{
    halt("_Block_object_assign()", NULL);
}

extern void _Block_object_dispose(const void* object, int flags);
void _Block_object_dispose(const void* object, int flags)
{
    // only support stack blocks in dyld: BLOCK_FIELD_IS_BYREF=8
    if ( flags != 8 )
        halt("_Block_object_dispose()", NULL);
}



#if !TARGET_OS_SIMULATOR
int memset_s(void* s, size_t smax, int c, size_t n);
int memset_s(void* s, size_t smax, int c, size_t n)
{
    int err = 0;
    if (s == NULL)
        return EINVAL;
    if (n > smax) {
        err = EOVERFLOW;
        n = smax;
    }
    memset(s, c, n);
    return err;
}

// <rdar://problem/69456906> dyld should mark _dyld_debugger_notification `noinline`
extern void _dyld_debugger_notification(int mode, unsigned long count, uint64_t machHeaders[]) __attribute__ ((noinline));
void _dyld_debugger_notification(int mode, unsigned long count, uint64_t machHeaders[])
{
    // Do nothing.  This exists for the debugger to set a break point on to see what images have been loaded or unloaded.
}

#endif

// We need this for std::find (see <rdar://113594237>)
extern wchar_t* wmemchr(const wchar_t *s, wchar_t c, size_t n);
wchar_t* wmemchr(const wchar_t* str, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (str[i] == c) {
            return (wchar_t*)&str[i];
        }
    }
    return NULL;
}
