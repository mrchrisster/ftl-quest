#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Apple Clang Blocks Runtime ABI
// ============================================================================
enum {
    BLOCK_DEALLOCATING =      (0x0001),
    BLOCK_REFCOUNT_MASK =     (0xfffe),
    BLOCK_NEEDS_FREE =        (1 << 24),
    BLOCK_HAS_COPY_DISPOSE =  (1 << 25),
    BLOCK_HAS_CTOR =          (1 << 26),
    BLOCK_IS_GC =             (1 << 27),
    BLOCK_IS_GLOBAL =         (1 << 28),
    BLOCK_USE_STRET =         (1 << 29),
    BLOCK_HAS_SIGNATURE =     (1 << 30)
};

struct Block_descriptor_1 {
    uintptr_t reserved;
    uintptr_t size;
};

struct Block_descriptor_2 {
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *);
};

struct Block_descriptor_3 {
    const char *signature;
    const char *layout;
};

struct Block_literal_1 {
    void *isa; // Points to _NSConcreteGlobalBlock or _NSConcreteStackBlock
    int32_t flags;
    int32_t reserved;
    void (*invoke)(void *, ...);
    struct Block_descriptor_1 *descriptor;
};

extern void *_NSConcreteGlobalBlock[32];
extern void *_NSConcreteStackBlock[32];

// ============================================================================
// Darwin vs Bionic POSIX / System ABI Differences
// ============================================================================

// Darwin stat structure layout on arm64
struct darwin_stat64 {
    int32_t   st_dev;
    uint16_t  st_mode;
    uint16_t  st_nlink;
    uint64_t  st_ino;
    uint32_t  st_uid;
    uint32_t  st_gid;
    int32_t   st_rdev;
    int64_t   darwin_st_atime;
    int64_t   darwin_st_atimensec;
    int64_t   darwin_st_mtime;
    int64_t   darwin_st_mtimensec;
    int64_t   darwin_st_ctime;
    int64_t   darwin_st_ctimensec;
    int64_t   darwin_st_birthtime;
    int64_t   darwin_st_birthtimensec;
    int64_t   st_size;
    int64_t   st_blocks;
    int32_t   st_blksize;
    uint32_t  st_flags;
    uint32_t  st_gen;
    int32_t   st_lspare;
    int64_t   st_qspare[2];
};

int darwin_open(const char *path, int oflag, ...);
int darwin_fcntl(int fd, int cmd, ...);
int darwin_stat(const char *pathname, struct darwin_stat64 *buf);
int darwin_fstat(int fd, struct darwin_stat64 *buf);
void *darwin_fopen(const char *pathname, const char *mode);
ssize_t darwin_read(int fd, void *buf, size_t n);
ssize_t darwin_pread(int fd, void *buf, size_t n, off_t off);
size_t darwin_fread(void *buf, size_t sz, size_t n, FILE *fp);
int darwin_inflate(void *strm, int flush);
int darwin_access(const char *pathname, int mode);
int darwin_mkdir(const char *pathname, unsigned int mode);
int darwin_unlink(const char *pathname);
int darwin_rename(const char *oldpath, const char *newpath);
void *darwin_opendir(const char *name);
void *darwin_readdir(void *dirp);
int darwin_closedir(void *dirp);
void *darwin_dlsym(void *handle, const char *symbol);
long darwin_sysconf(int name);

// Darwin pthread / sync ABI bridges
int darwin_pthread_once(void *once_control, void (*init_routine)(void));
int darwin_pthread_setname_np(const char *name);
int darwin_pthread_key_create(uint64_t *key, void (*destructor)(void *));
int darwin_pthread_mutexattr_settype(void *attr, int type);
int darwin_pthread_mutex_init(void *mutex, const void *attr);
int darwin_pthread_mutex_destroy(void *mutex);
int darwin_pthread_mutex_lock(void *mutex);
int darwin_pthread_mutex_trylock(void *mutex);
int darwin_pthread_mutex_unlock(void *mutex);
int darwin_pthread_cond_init(void *cond, const void *attr);
int darwin_pthread_cond_destroy(void *cond);
int darwin_pthread_cond_signal(void *cond);
int darwin_pthread_cond_broadcast(void *cond);
int darwin_pthread_cond_wait(void *cond, void *mutex);
int darwin_pthread_cond_timedwait(void *cond, void *mutex, const struct timespec *abstime);

int darwin_setjmp(void *env);
void darwin_longjmp(void *env, int val);

// VFS Mount & Path translation: Map iOS bundle paths and sandboxed Documents to Android
void darwin_vfs_init(const char *internal_storage_path, const char *external_storage_path);
const char *darwin_vfs_resolve(const char *ios_path, char *out_buf, size_t out_len);

// Math & Architecture Register Shims
struct sincosf_res {
    float s;
    float c;
};

struct sincosf_res __sincosf_stret(float x);
float ___exp10(float x);
int *___error(void);
void _OSMemoryBarrier(void);

// Mach Timing & Semaphore Subsystem
struct mach_timebase_info_t {
    uint32_t numer;
    uint32_t denom;
};

struct mach_timespec_t {
    unsigned int tv_sec;
    int tv_nsec;
};

uint64_t _mach_absolute_time(void);
int _mach_timebase_info(struct mach_timebase_info_t *info);
int _mach_wait_until(uint64_t deadline);
int _semaphore_create(int task, int *out_sem, int policy, int value);
int _semaphore_destroy(int task, int sem_id);
int _semaphore_signal(int sem_id);
int _semaphore_wait(int sem_id);
int _semaphore_timedwait(int sem_id, struct mach_timespec_t wait_time);
int _mach_host_self(void);
int _host_page_size(int host, size_t *out_size);
int _host_statistics(int host, int type, void *out_info, unsigned int *count);
int _task_info(int task, int flavor, void *task_info_out, unsigned int *count);
int _sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
void __dyld_register_func_for_add_image(void (*func)(const void *header, intptr_t slide));

// CoreFoundation Shims
extern void *___CFConstantStringClassReference[32];
extern void *kCFAllocatorNull;
extern double kCFCoreFoundationVersionNumber;
extern uint32_t darwin_mach_task_self;

struct DarwinRuneLocale {
    uint8_t header[60];
    uint32_t runetype[256];
    int32_t maplower[256];
    int32_t mapupper[256];
    uint8_t trailer[100];
};
extern struct DarwinRuneLocale g_darwin_default_rune_locale;

void darwin_assert_rtn(const char *func, const char *file, int line, const char *msg);
int darwin_tolower(int c);
int darwin_toupper(int c);
void darwin_bzero(void *s, size_t n);
void darwin_memset_pattern16(void *b, const void *pattern16, size_t len);
void *darwin_hash_create(int count);
void *darwin_hash_search(void *table, const char *key, void *datum, void **replaced);
int darwin_sprintf_chk(char *str, int flag, size_t slen, const char *format, ...);
int darwin_asprintf(char **ret, const char *format, ...);
const void *apple_locale_use_facet(const void *loc, void *id);
uint32_t darwin_get_plane_tex(int plane);
uint32_t darwin_get_plane_fbo(int plane);
// 5-plane diorama routing (debug). Enable via darwin_planes_request(); takes effect at the next frame boundary.
void darwin_set_main_fbo(uint32_t fbo, int w, int h);
void darwin_planes_request(bool enabled);
void darwin_planes_frame_boundary();
bool darwin_planes_enabled();
void darwin_dump_planes(const char *dir, int frame);
void darwin_trace_request();
uint32_t darwin_get_scene_fbo(int *w, int *h);
void darwin_get_scene_size(int *w, int *h);
bool darwin_scene_known();
void darwin_get_plane_size(int *w, int *h);



void *CFStringCreateWithCStringNoCopy(void *alloc, const char *cStr, uint32_t encoding, void *contentsDeallocator);
const char *CFStringGetCStringPtr(void *theString, uint32_t encoding);
int CFStringGetCString(void *theString, char *buffer, size_t bufferSize, uint32_t encoding);
void CFRelease(void *cf);
void *CFBundleGetMainBundle(void);
void *CFBundleGetInfoDictionary(void *bundle);
void *CFBundleCopyLocalizedString(void *bundle, void *key, void *value, void *tableName);

// AudioToolbox RemoteIO AudioUnit Bridge
typedef int OSStatus;
typedef uint32_t AudioUnitPropertyID;
typedef uint32_t AudioUnitScope;
typedef uint32_t AudioUnitElement;

OSStatus _AudioComponentFindNext(void *inComponent, void *inDesc);
OSStatus _AudioComponentInstanceNew(void *inComponent, void **outInstance);
OSStatus _AudioComponentInstanceDispose(void *inInstance);
OSStatus _AudioUnitInitialize(void *inUnit);
OSStatus _AudioUnitUninitialize(void *inUnit);
OSStatus _AudioOutputUnitStart(void *inUnit);
OSStatus _AudioOutputUnitStop(void *inUnit);
OSStatus _AudioUnitSetProperty(
    void *inUnit,
    AudioUnitPropertyID inID,
    AudioUnitScope inScope,
    AudioUnitElement inElement,
    const void *inData,
    uint32_t inDataSize
);

#ifdef __cplusplus
}
#endif
