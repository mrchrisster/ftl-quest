#include "macho_loader.h"
#include "darwin_abi.h"
#include "objc_shim.h"
#include "eagl_shim.h"
#include "foundation_shim.h"
#include "uikit_shim.h"
#include "apple_string_shim.h"

#include <sys/mman.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <zlib.h>
#include <pthread.h>
#include <android/log.h>

#define TAG "FTL_MACHO_LOADER"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FAT_MAGIC 0xCAFEBABE
#define MH_MAGIC_64 0xFEEDFACF
#define CPU_TYPE_ARM64 0x0100000C

#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x02
#define LC_DYSYMTAB 0x0B
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY (0x22 | 0x80000000)
#define LC_MAIN (0x28 | 0x80000000)

#define BIND_OPCODE_MASK 0xF0
#define BIND_IMMEDIATE_MASK 0x0F
#define BIND_OPCODE_DONE 0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB 0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM 0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM 0x50
#define BIND_OPCODE_SET_ADDEND_SLEB 0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OPCODE_ADD_ADDR_ULEB 0x80
#define BIND_OPCODE_DO_BIND 0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB 0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED 0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0

// Dummy notification CFStrings; GOT holds pointer-to-NSString* (so bind GOT to g_dummy_cfstring_ptrs[i])
static struct {
    void *isa;
    uintptr_t flags;
    const char *str;
    size_t len;
} g_dummy_cfstrings[10] = {
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionInterruptionNotification", 38 },
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionInterruptionTypeKey", 33 },
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionMediaServicesWereResetNotification", 48 },
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionPortHeadphones", 28 },
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionRouteChangeNotification", 38 },
    { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionRouteChangePreviousRouteKey", 42 },
    { ___CFConstantStringClassReference, 0x7C8, "GCControllerDidConnectNotification", 34 },
    { ___CFConstantStringClassReference, 0x7C8, "GCControllerDidDisconnectNotification", 37 },
    { ___CFConstantStringClassReference, 0x7C8, "GKPlayerAuthenticationDidChangeNotificationName", 47 },
    { ___CFConstantStringClassReference, 0x7C8, "kCFRunLoopDefaultMode", 21 }
};

static void *g_dummy_cfstring_ptrs[10] = {
    &g_dummy_cfstrings[0],
    &g_dummy_cfstrings[1],
    &g_dummy_cfstrings[2],
    &g_dummy_cfstrings[3],
    &g_dummy_cfstrings[4],
    &g_dummy_cfstrings[5],
    &g_dummy_cfstrings[6],
    &g_dummy_cfstrings[7],
    &g_dummy_cfstrings[8],
    &g_dummy_cfstrings[9]
};

static uintptr_t g_stack_canary = 0xDEADBEEFCAFEBABEULL;
static FILE *g_darwin_stdin = nullptr;
static FILE *g_darwin_stdout = nullptr;
static FILE *g_darwin_stderr = nullptr;

static uintptr_t g_mod_init_va = 0x10022C958ULL;
static size_t g_mod_init_count = 47;

static void *g_libcpp_handle = nullptr;

extern "C" void dyld_stub_binder_trap(void) {
    void *lr = __builtin_return_address(0);
    LOGE("FATAL: unpatched lazy stub reached dyld_stub_binder from %p!", lr);
    abort();
}

// ============================================================================
// Global Symbol Hook Registry
// ============================================================================

static std::unordered_map<std::string, void*> g_symbol_map;

static void *darwin_malloc_diag(size_t size) {
    if (size > 0x40000000ULL) {
        LOGE("ANOMALOUS _malloc(%zu = 0x%zx) caller=%p", size, size, __builtin_return_address(0));
        return nullptr;
    }
    return malloc(size);
}

static void register_core_symbols() {
    if (!g_symbol_map.empty()) return;

    g_libcpp_handle = dlopen("libc++_shared.so", RTLD_NOW | RTLD_GLOBAL);
    g_darwin_stdin = stdin;
    g_darwin_stdout = stdout;
    g_darwin_stderr = stderr;

    // 1. Darwin / Mach ABI Hooks
    g_symbol_map["dyld_stub_binder"] = (void *)dyld_stub_binder_trap;
    g_symbol_map["___error"] = (void *)___error;
    g_symbol_map["___sincosf_stret"] = (void *)__sincosf_stret;
    g_symbol_map["___exp10"] = (void *)___exp10;
    g_symbol_map["_OSMemoryBarrier"] = (void *)_OSMemoryBarrier;
    g_symbol_map["_mach_absolute_time"] = (void *)_mach_absolute_time;
    g_symbol_map["_mach_timebase_info"] = (void *)_mach_timebase_info;
    g_symbol_map["_mach_wait_until"] = (void *)_mach_wait_until;
    g_symbol_map["_semaphore_create"] = (void *)_semaphore_create;
    g_symbol_map["_semaphore_destroy"] = (void *)_semaphore_destroy;
    g_symbol_map["_semaphore_signal"] = (void *)_semaphore_signal;
    g_symbol_map["_semaphore_wait"] = (void *)_semaphore_wait;
    g_symbol_map["_semaphore_timedwait"] = (void *)_semaphore_timedwait;
    g_symbol_map["_mach_host_self"] = (void *)_mach_host_self;
    g_symbol_map["_mach_task_self_"] = (void *)&darwin_mach_task_self;
    g_symbol_map["_host_page_size"] = (void *)_host_page_size;
    g_symbol_map["_host_statistics"] = (void *)_host_statistics;
    g_symbol_map["_task_info"] = (void *)_task_info;
    g_symbol_map["_sysctlbyname"] = (void *)_sysctlbyname;
    g_symbol_map["__dyld_register_func_for_add_image"] = (void *)__dyld_register_func_for_add_image;
    g_symbol_map["__DefaultRuneLocale"] = (void *)&g_darwin_default_rune_locale;
    g_symbol_map["___assert_rtn"] = (void *)darwin_assert_rtn;
    g_symbol_map["___tolower"] = (void *)darwin_tolower;
    g_symbol_map["___toupper"] = (void *)darwin_toupper;
    g_symbol_map["_bzero"] = (void *)darwin_bzero;
    g_symbol_map["_memset_pattern16"] = (void *)darwin_memset_pattern16;
    g_symbol_map["_hash_create"] = (void *)darwin_hash_create;
    g_symbol_map["_hash_search"] = (void *)darwin_hash_search;
    g_symbol_map["___stdinp"] = (void *)&g_darwin_stdin;
    g_symbol_map["___stdoutp"] = (void *)&g_darwin_stdout;
    g_symbol_map["___stderrp"] = (void *)&g_darwin_stderr;

    // 2. Darwin Filesystem, POSIX & pthread ABI overrides
    g_symbol_map["_malloc"] = (void *)darwin_malloc_diag;
    g_symbol_map["_stat"] = (void *)darwin_stat;
    g_symbol_map["_fstat"] = (void *)darwin_fstat;
    g_symbol_map["_open"] = (void *)darwin_open;
    g_symbol_map["_fcntl"] = (void *)darwin_fcntl;
    g_symbol_map["_fopen"] = (void *)darwin_fopen;
    g_symbol_map["_read"] = (void *)darwin_read;
    g_symbol_map["_pread"] = (void *)darwin_pread;
    g_symbol_map["_fread"] = (void *)darwin_fread;
    g_symbol_map["_inflate"] = (void *)darwin_inflate;
    g_symbol_map["_access"] = (void *)darwin_access;
    g_symbol_map["_mkdir"] = (void *)darwin_mkdir;
    g_symbol_map["_unlink"] = (void *)darwin_unlink;
    g_symbol_map["_rename"] = (void *)darwin_rename;
    g_symbol_map["_opendir"] = (void *)darwin_opendir;
    g_symbol_map["_readdir"] = (void *)darwin_readdir;
    g_symbol_map["_closedir"] = (void *)darwin_closedir;
    g_symbol_map["_dlsym"] = (void *)darwin_dlsym;
    g_symbol_map["_sysconf"] = (void *)darwin_sysconf;
    g_symbol_map["_setjmp"] = (void *)darwin_setjmp;
    g_symbol_map["_longjmp"] = (void *)darwin_longjmp;
    g_symbol_map["_pthread_once"] = (void *)darwin_pthread_once;
    g_symbol_map["_pthread_setname_np"] = (void *)darwin_pthread_setname_np;
    g_symbol_map["_pthread_key_create"] = (void *)darwin_pthread_key_create;
    g_symbol_map["_pthread_mutexattr_settype"] = (void *)darwin_pthread_mutexattr_settype;
    g_symbol_map["_pthread_mutex_init"] = (void *)darwin_pthread_mutex_init;
    g_symbol_map["_pthread_mutex_destroy"] = (void *)darwin_pthread_mutex_destroy;
    g_symbol_map["_pthread_mutex_lock"] = (void *)darwin_pthread_mutex_lock;
    g_symbol_map["_pthread_mutex_trylock"] = (void *)darwin_pthread_mutex_trylock;
    g_symbol_map["_pthread_mutex_unlock"] = (void *)darwin_pthread_mutex_unlock;
    g_symbol_map["_pthread_cond_init"] = (void *)darwin_pthread_cond_init;
    g_symbol_map["_pthread_cond_destroy"] = (void *)darwin_pthread_cond_destroy;
    g_symbol_map["_pthread_cond_signal"] = (void *)darwin_pthread_cond_signal;
    g_symbol_map["_pthread_cond_broadcast"] = (void *)darwin_pthread_cond_broadcast;
    g_symbol_map["_pthread_cond_wait"] = (void *)darwin_pthread_cond_wait;
    g_symbol_map["_pthread_cond_timedwait"] = (void *)darwin_pthread_cond_timedwait;
    g_symbol_map["___sprintf_chk"] = (void *)darwin_sprintf_chk;
    g_symbol_map["__sprintf_chk"] = (void *)darwin_sprintf_chk;
    g_symbol_map["_asprintf"] = (void *)darwin_asprintf;
    g_symbol_map["asprintf"] = (void *)darwin_asprintf;
    g_symbol_map["__ZNKSt3__16locale9use_facetERNS0_2idE"] = (void *)apple_locale_use_facet;
    g_symbol_map["_ZNKSt3__16locale9use_facetERNS0_2idE"] = (void *)apple_locale_use_facet;


    // 3. AudioToolbox RemoteIO
    g_symbol_map["_AudioComponentFindNext"] = (void *)_AudioComponentFindNext;
    g_symbol_map["_AudioComponentInstanceNew"] = (void *)_AudioComponentInstanceNew;
    g_symbol_map["_AudioComponentInstanceDispose"] = (void *)_AudioComponentInstanceDispose;
    g_symbol_map["_AudioUnitInitialize"] = (void *)_AudioUnitInitialize;
    g_symbol_map["_AudioUnitUninitialize"] = (void *)_AudioUnitUninitialize;
    g_symbol_map["_AudioOutputUnitStart"] = (void *)_AudioOutputUnitStart;
    g_symbol_map["_AudioOutputUnitStop"] = (void *)_AudioOutputUnitStop;
    g_symbol_map["_AudioUnitSetProperty"] = (void *)_AudioUnitSetProperty;

    // 4. CoreFoundation & Foundation
    g_symbol_map["___CFConstantStringClassReference"] = (void *)___CFConstantStringClassReference;
    g_symbol_map["_kCFAllocatorNull"] = (void *)&kCFAllocatorNull;
    g_symbol_map["_kCFCoreFoundationVersionNumber"] = (void *)&kCFCoreFoundationVersionNumber;
    g_symbol_map["_CFStringCreateWithCStringNoCopy"] = (void *)CFStringCreateWithCStringNoCopy;
    g_symbol_map["_CFStringGetCStringPtr"] = (void *)CFStringGetCStringPtr;
    g_symbol_map["_CFStringGetCString"] = (void *)CFStringGetCString;
    g_symbol_map["_CFRelease"] = (void *)CFRelease;
    g_symbol_map["_CFBundleGetMainBundle"] = (void *)CFBundleGetMainBundle;
    g_symbol_map["_CFBundleGetInfoDictionary"] = (void *)CFBundleGetInfoDictionary;
    g_symbol_map["_CFBundleCopyLocalizedString"] = (void *)CFBundleCopyLocalizedString;
    g_symbol_map["_NSSearchPathForDirectoriesInDomains"] = (void *)NSSearchPathForDirectoriesInDomains;
    g_symbol_map["_NSClassFromString"] = (void *)NSClassFromString;
    g_symbol_map["__NSConcreteGlobalBlock"] = (void *)_NSConcreteGlobalBlock;
    g_symbol_map["__NSConcreteStackBlock"] = (void *)_NSConcreteStackBlock;

    // 5. Objective-C Runtime
    g_symbol_map["__objc_empty_cache"] = (void *)_objc_empty_cache;
    g_symbol_map["__objc_empty_vtable"] = (void *)_objc_empty_vtable;
    g_symbol_map["_objc_msgSend"] = (void *)objc_msgSend;
    g_symbol_map["_objc_msgSendSuper2"] = (void *)objc_msgSendSuper2;
    g_symbol_map["_objc_alloc"] = (void *)objc_alloc;
    g_symbol_map["_objc_retain"] = (void *)objc_retain;
    g_symbol_map["_objc_release"] = (void *)objc_release;
    g_symbol_map["_objc_autorelease"] = (void *)objc_autorelease;
    g_symbol_map["_objc_getClass"] = (void *)objc_getClass;
    g_symbol_map["_objc_getMetaClass"] = (void *)objc_getMetaClass;
    g_symbol_map["_objc_getRequiredClass"] = (void *)objc_getRequiredClass;
    g_symbol_map["_objc_lookUpClass"] = (void *)objc_lookUpClass;
    g_symbol_map["_objc_allocateClassPair"] = (void *)objc_allocateClassPair;
    g_symbol_map["_objc_registerClassPair"] = (void *)objc_registerClassPair;
    g_symbol_map["_objc_initializeClassPair"] = (void *)objc_initializeClassPair;
    g_symbol_map["_objc_readClassPair"] = (void *)objc_readClassPair;
    g_symbol_map["_objc_copyClassNamesForImage"] = (void *)objc_copyClassNamesForImage;
    g_symbol_map["_objc_getProtocol"] = (void *)objc_getProtocol;
    g_symbol_map["_objc_constructInstance"] = (void *)objc_constructInstance;
    g_symbol_map["_objc_setProperty_nonatomic"] = (void *)objc_setProperty_nonatomic;
    g_symbol_map["_class_getName"] = (void *)class_getName;
    g_symbol_map["_class_getSuperclass"] = (void *)class_getSuperclass;
    g_symbol_map["_class_isMetaClass"] = (void *)class_isMetaClass;
    g_symbol_map["_class_addMethod"] = (void *)class_addMethod;
    g_symbol_map["_class_replaceMethod"] = (void *)class_replaceMethod;
    g_symbol_map["_class_getInstanceMethod"] = (void *)class_getInstanceMethod;
    g_symbol_map["_class_getInstanceVariable"] = (void *)class_getInstanceVariable;
    g_symbol_map["_class_addProperty"] = (void *)class_addProperty;
    g_symbol_map["_class_addProtocol"] = (void *)class_addProtocol;
    g_symbol_map["_method_setImplementation"] = (void *)method_setImplementation;
    g_symbol_map["_object_getClass"] = (void *)object_getClass;
    g_symbol_map["_object_getIvar"] = (void *)object_getIvar;
    g_symbol_map["_object_getIndexedIvars"] = (void *)object_getIndexedIvars;
    g_symbol_map["_property_copyAttributeList"] = (void *)property_copyAttributeList;
    g_symbol_map["_protocol_getName"] = (void *)protocol_getName;
    g_symbol_map["_protocol_getMethodDescription"] = (void *)protocol_getMethodDescription;
    g_symbol_map["_sel_registerName"] = (void *)sel_registerName;
    g_symbol_map["_sel_getName"] = (void *)sel_getName;
    g_symbol_map["_sel_getUid"] = (void *)sel_getUid;
    g_symbol_map["_UIApplicationMain"] = (void *)_UIApplicationMain;

    // 6. Zlib & Apple libc++ basic_string ABI shim
    register_apple_string_symbols(g_symbol_map);
    g_symbol_map["_inflate"] = (void *)inflate;
    g_symbol_map["_inflateInit2_"] = (void *)inflateInit2_;
    g_symbol_map["_inflateReset"] = (void *)inflateReset;
    g_symbol_map["_inflateEnd"] = (void *)inflateEnd;

    // 7. Framework Constant Objects & GOT Notification Symbols
    g_symbol_map["_kEAGLColorFormatRGBA8"] = (void *)&kEAGLColorFormatRGBA8;
    g_symbol_map["_kEAGLDrawablePropertyColorFormat"] = (void *)&kEAGLDrawablePropertyColorFormat;
    g_symbol_map["_kEAGLDrawablePropertyRetainedBacking"] = (void *)&kEAGLDrawablePropertyRetainedBacking;
    g_symbol_map["_AVAudioSessionCategoryAmbient"] = (void *)&AVAudioSessionCategoryAmbient;
    g_symbol_map["_AVAudioSessionCategoryPlayback"] = (void *)&AVAudioSessionCategoryPlayback;

    g_symbol_map["_AVAudioSessionInterruptionNotification"] = (void *)&g_dummy_cfstring_ptrs[0];
    g_symbol_map["_AVAudioSessionInterruptionTypeKey"] = (void *)&g_dummy_cfstring_ptrs[1];
    g_symbol_map["_AVAudioSessionMediaServicesWereResetNotification"] = (void *)&g_dummy_cfstring_ptrs[2];
    g_symbol_map["_AVAudioSessionPortHeadphones"] = (void *)&g_dummy_cfstring_ptrs[3];
    g_symbol_map["_AVAudioSessionRouteChangeNotification"] = (void *)&g_dummy_cfstring_ptrs[4];
    g_symbol_map["_AVAudioSessionRouteChangePreviousRouteKey"] = (void *)&g_dummy_cfstring_ptrs[5];
    g_symbol_map["_GCControllerDidConnectNotification"] = (void *)&g_dummy_cfstring_ptrs[6];
    g_symbol_map["_GCControllerDidDisconnectNotification"] = (void *)&g_dummy_cfstring_ptrs[7];
    g_symbol_map["_GKPlayerAuthenticationDidChangeNotificationName"] = (void *)&g_dummy_cfstring_ptrs[8];
    g_symbol_map["_NSDefaultRunLoopMode"] = (void *)&g_dummy_cfstring_ptrs[9];
    g_symbol_map["___stack_chk_guard"] = (void *)&g_stack_canary;
}

static void *lookup_dlsym(const char *sym_name) {
    if (!sym_name) return nullptr;
    void *sym = dlsym(RTLD_DEFAULT, sym_name);
    if (!sym && g_libcpp_handle) {
        sym = dlsym(g_libcpp_handle, sym_name);
    }
    return sym;
}

void *macho_resolve_symbol(const char *name) {
    if (!name) return nullptr;
    register_core_symbols();

    auto it = g_symbol_map.find(name);
    if (it != g_symbol_map.end()) {
        return it->second;
    }

    // Objective-C class & metaclass references
    if (strncmp(name, "_OBJC_CLASS_$_", 14) == 0) {
        void *cls = (void *)objc_getClass(name + 14);
        if (cls) {
            g_symbol_map[name] = cls;
            return cls;
        }
    } else if (strncmp(name, "_OBJC_METACLASS_$_", 18) == 0) {
        void *mcls = (void *)objc_getMetaClass(name + 18);
        if (mcls) {
            g_symbol_map[name] = mcls;
            return mcls;
        }
    }

    // Try dlsym without leading underscore for POSIX and standard library / C++ symbols
    const char *dlsym_name = name;
    if (name[0] == '_') dlsym_name = name + 1;

    void *sym = lookup_dlsym(dlsym_name);
    if (sym) {
        g_symbol_map[name] = sym;
        return sym;
    }

    // Translate Darwin libc++ ABI namespace std::__1 (St3__1) -> Android NDK std::__ndk1 (St6__ndk1)
    if (strncmp(dlsym_name, "_Z", 2) == 0 && strstr(dlsym_name, "St3__1") != nullptr) {
        std::string ndk_name(dlsym_name);
        size_t pos = 0;
        while ((pos = ndk_name.find("St3__1", pos)) != std::string::npos) {
            ndk_name.replace(pos, 6, "St6__ndk1");
            pos += 9;
        }
        sym = lookup_dlsym(ndk_name.c_str());
        if (sym) {
            g_symbol_map[name] = sym;
            return sym;
        }
    }

    // Try exact name as fallback
    sym = lookup_dlsym(name);
    if (sym) {
        g_symbol_map[name] = sym;
        return sym;
    }

    LOGE("[macho_resolve_symbol] Unresolved symbol: %s", name);
    return nullptr;
}

// ============================================================================
// Dyld Bind Opcode Decoder
// ============================================================================

static uint64_t read_uleb128(const uint8_t *&p, const uint8_t *end) {
    uint64_t result = 0;
    int bit = 0;
    while (p < end) {
        uint8_t byte = *p++;
        result |= (uint64_t)(byte & 0x7F) << bit;
        if ((byte & 0x80) == 0) break;
        bit += 7;
    }
    return result;
}

static int64_t read_sleb128(const uint8_t *&p, const uint8_t *end) {
    int64_t result = 0;
    int bit = 0;
    uint8_t byte = 0;
    while (p < end) {
        byte = *p++;
        result |= (int64_t)(byte & 0x7F) << bit;
        bit += 7;
        if ((byte & 0x80) == 0) break;
    }
    if (bit < 64 && (byte & 0x40)) {
        result |= (~0ULL << bit);
    }
    return result;
}

static size_t apply_dyld_bind_stream(
    const uint8_t *stream,
    size_t stream_size,
    const std::vector<uintptr_t> &seg_vmaddrs,
    bool is_lazy,
    const char *label,
    std::unordered_set<uintptr_t> &bound_addrs
) {
    if (!stream || stream_size == 0) return 0;

    const uint8_t *p = stream;
    const uint8_t *end = stream + stream_size;

    uint8_t type = 1; // BIND_TYPE_POINTER
    uint8_t seg_idx = 0;
    uint64_t seg_off = 0;
    int64_t addend = 0;
    const char *sym_name = nullptr;
    size_t bound = 0;
    size_t unresolved = 0;

    auto do_bind_at = [&]() {
        if (seg_idx >= seg_vmaddrs.size() || !sym_name) {
            LOGE("[%s] Invalid bind target seg=%u sym=%s", label, seg_idx, sym_name ? sym_name : "<null>");
            return;
        }
        uintptr_t addr = seg_vmaddrs[seg_idx] + (uintptr_t)seg_off;
        void *resolved = macho_resolve_symbol(sym_name);
        if (resolved) {
            uint64_t val = (uint64_t)((int64_t)(uintptr_t)resolved + addend);
            if (type == 1) {
                *(uint64_t *)addr = val;
                bound_addrs.insert(addr);
                bound++;
            } else {
                LOGE("[%s] Unsupported bind type %u for %s at 0x%lX", label, type, sym_name, addr);
            }
        } else {
            LOGE("[%s] Failed to resolve %s (target 0x%lX, add=%lld)", label, sym_name, addr, (long long)addend);
            unresolved++;
        }
    };

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t opcode = byte & BIND_OPCODE_MASK;
        uint8_t imm = byte & BIND_IMMEDIATE_MASK;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                if (!is_lazy) return bound;
                type = 1;
                addend = 0;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                (void)read_uleb128(p, end);
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                sym_name = (const char *)p;
                while (p < end && *p != '\0') p++;
                if (p < end) p++;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = imm;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(p, end);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm;
                seg_off = read_uleb128(p, end);
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                seg_off += read_uleb128(p, end);
                break;
            case BIND_OPCODE_DO_BIND:
                do_bind_at();
                seg_off += sizeof(uintptr_t);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                do_bind_at();
                seg_off += sizeof(uintptr_t) + read_uleb128(p, end);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                do_bind_at();
                seg_off += sizeof(uintptr_t) + (uint64_t)imm * sizeof(uintptr_t);
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                uint64_t count = read_uleb128(p, end);
                uint64_t skip = read_uleb128(p, end);
                for (uint64_t k = 0; k < count; k++) {
                    do_bind_at();
                    seg_off += sizeof(uintptr_t) + skip;
                }
                break;
            }
            default:
                LOGE("[%s] Unknown bind opcode 0x%02X", label, byte);
                return bound;
        }
    }

    if (unresolved > 0) {
        LOGE("[%s] %zu binds unresolved out of %zu total", label, unresolved, bound + unresolved);
    }
    return bound;
}

// ============================================================================
// Mach-O ARM64 In-Memory Loader
// ============================================================================

int macho_loader_init(const char *macho_path, MachOLoadInfo *out_info) {
    LOGI("Loading ARM64 Mach-O from: %s", macho_path);
    FILE *f = fopen(macho_path, "rb");
    if (!f) {
        LOGE("Failed to open binary file: %s", macho_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data || fread(file_data, 1, file_size, f) != file_size) {
        LOGE("Failed to read file data");
        free(file_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    // Locate ARM64 slice if universal fat binary
    uint32_t magic = *(uint32_t *)file_data;
    uint32_t slice_offset = 0;
    size_t slice_size = file_size;

    if (magic == 0xBEBAFECA || magic == FAT_MAGIC) {
        uint32_t nfat = __builtin_bswap32(*(uint32_t *)(file_data + 4));
        for (uint32_t i = 0; i < nfat; i++) {
            uint32_t *arch = (uint32_t *)(file_data + 8 + i * 20);
            uint32_t cputype = __builtin_bswap32(arch[0]);
            uint32_t off = __builtin_bswap32(arch[2]);
            uint32_t sz = __builtin_bswap32(arch[3]);
            if (cputype == CPU_TYPE_ARM64) {
                slice_offset = off;
                slice_size = sz;
                LOGI("Found ARM64 slice in Fat binary at offset 0x%X (size: %zu bytes)", off, (size_t)sz);
                break;
            }
        }
    }

    uint8_t *slice_data = file_data + slice_offset;
    uint32_t *hdr = (uint32_t *)slice_data;
    if (hdr[0] != MH_MAGIC_64) {
        LOGE("Invalid Mach-O 64-bit magic: 0x%08X", hdr[0]);
        free(file_data);
        return -1;
    }

    uint32_t ncmds = hdr[4];
    uint8_t *cmd_ptr = slice_data + 32;

    uintptr_t text_vmaddr = 0x100000000ULL;
    size_t text_vmsize = 0;
    uintptr_t data_vmaddr = 0x10022C000ULL;
    size_t data_vmsize = 0;
    uintptr_t entry_point = 0;

    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint32_t indirectsymoff = 0, nindirectsyms = 0;
    uint32_t bind_off = 0, bind_size = 0;
    uint32_t weak_bind_off = 0, weak_bind_size = 0;
    uint32_t lazy_bind_off = 0, lazy_bind_size = 0;

    struct SegmentRef {
        char name[17];
        uint64_t vmaddr;
        uint64_t vmsize;
        uint64_t fileoff;
        uint64_t filesize;
    };
    struct SectionRef {
        char name[34];
        uintptr_t addr;
        size_t size;
        uint32_t offset;
        uint32_t reserved1;
    };

    std::vector<SegmentRef> segments;
    std::vector<uintptr_t> seg_vmaddrs;
    std::vector<SectionRef> sections;

    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = *(uint32_t *)cmd_ptr;
        uint32_t cmdsize = *(uint32_t *)(cmd_ptr + 4);

        if (cmd == LC_SEGMENT_64) {
            SegmentRef seg = {};
            memcpy(seg.name, cmd_ptr + 8, 16);
            seg.vmaddr = *(uint64_t *)(cmd_ptr + 24);
            seg.vmsize = *(uint64_t *)(cmd_ptr + 32);
            seg.fileoff = *(uint64_t *)(cmd_ptr + 40);
            seg.filesize = *(uint64_t *)(cmd_ptr + 48);
            uint32_t nsects = *(uint32_t *)(cmd_ptr + 64);

            segments.push_back(seg);
            seg_vmaddrs.push_back((uintptr_t)seg.vmaddr);

            if (strcmp(seg.name, "__TEXT") == 0) {
                text_vmaddr = seg.vmaddr;
                text_vmsize = seg.vmsize;
            } else if (strcmp(seg.name, "__DATA") == 0) {
                data_vmaddr = seg.vmaddr;
                data_vmsize = seg.vmsize;
            }

            uint8_t *sect_ptr = cmd_ptr + 72;
            for (uint32_t s = 0; s < nsects; s++) {
                SectionRef sec;
                char s_name[17] = {0};
                memcpy(s_name, sect_ptr, 16);
                snprintf(sec.name, sizeof(sec.name), "%s,%s", seg.name, s_name);
                sec.addr = *(uint64_t *)(sect_ptr + 32);
                sec.size = *(uint64_t *)(sect_ptr + 40);
                sec.offset = *(uint32_t *)(sect_ptr + 48);
                sec.reserved1 = *(uint32_t *)(sect_ptr + 68);
                sections.push_back(sec);
                sect_ptr += 80;
            }
        } else if (cmd == LC_SYMTAB) {
            symoff = *(uint32_t *)(cmd_ptr + 8);
            nsyms = *(uint32_t *)(cmd_ptr + 12);
            stroff = *(uint32_t *)(cmd_ptr + 16);
            strsize = *(uint32_t *)(cmd_ptr + 20);
        } else if (cmd == LC_DYSYMTAB) {
            indirectsymoff = *(uint32_t *)(cmd_ptr + 56);
            nindirectsyms = *(uint32_t *)(cmd_ptr + 60);
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            bind_off = *(uint32_t *)(cmd_ptr + 16);
            bind_size = *(uint32_t *)(cmd_ptr + 20);
            weak_bind_off = *(uint32_t *)(cmd_ptr + 24);
            weak_bind_size = *(uint32_t *)(cmd_ptr + 28);
            lazy_bind_off = *(uint32_t *)(cmd_ptr + 32);
            lazy_bind_size = *(uint32_t *)(cmd_ptr + 36);
        } else if (cmd == LC_MAIN) {
            uint64_t entryoff = *(uint64_t *)(cmd_ptr + 8);
            entry_point = text_vmaddr + entryoff;
        }

        cmd_ptr += cmdsize;
    }

    LOGI("Mach-O Segments: TEXT @ 0x%lX (size: 0x%zX), DATA @ 0x%lX (size: 0x%zX)", text_vmaddr, text_vmsize, data_vmaddr, data_vmsize);

    // Reserve virtual memory map at fixed address 0x100000000
    size_t total_vmsize = (data_vmaddr + data_vmsize) - text_vmaddr;
    void *mapped_base = mmap((void *)text_vmaddr, total_vmsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped_base == MAP_FAILED) {
        LOGE("Failed to mmap fixed address at 0x%lX. Error: %s", text_vmaddr, strerror(errno));
        free(file_data);
        return -1;
    }

    // Copy full loadable segments (__TEXT including Mach-O header, and __DATA)
    for (const auto &seg : segments) {
        if (strcmp(seg.name, "__TEXT") == 0 || strcmp(seg.name, "__DATA") == 0) {
            if (seg.filesize > 0 && (seg.fileoff + seg.filesize) <= slice_size &&
                seg.vmaddr >= text_vmaddr && (seg.vmaddr + seg.filesize) <= (text_vmaddr + total_vmsize)) {
                memcpy((void *)seg.vmaddr, slice_data + seg.fileoff, seg.filesize);
            }
        }
    }

    // 1. Apply LC_DYLD_INFO_ONLY binds (non-lazy, weak, lazy)
    size_t dyld_bound = 0;
    std::unordered_set<uintptr_t> dyld_bound_addrs;
    if (bind_size > 0 && (bind_off + bind_size) <= slice_size) {
        dyld_bound += apply_dyld_bind_stream(slice_data + bind_off, bind_size, seg_vmaddrs, false, "DYLD_BIND", dyld_bound_addrs);
    }
    if (weak_bind_size > 0 && (weak_bind_off + weak_bind_size) <= slice_size) {
        dyld_bound += apply_dyld_bind_stream(slice_data + weak_bind_off, weak_bind_size, seg_vmaddrs, false, "DYLD_WEAK_BIND", dyld_bound_addrs);
    }
    if (lazy_bind_size > 0 && (lazy_bind_off + lazy_bind_size) <= slice_size) {
        dyld_bound += apply_dyld_bind_stream(slice_data + lazy_bind_off, lazy_bind_size, seg_vmaddrs, true, "DYLD_LAZY_BIND", dyld_bound_addrs);
    }

    // 2. Also walk indirect symbol tables (__la_symbol_ptr, __got) as backstop
    const char *strtab = (const char *)(slice_data + stroff);
    uint32_t *indirect_syms = (uint32_t *)(slice_data + indirectsymoff);
    size_t indirect_bound = 0;

    for (const auto &sec : sections) {
        if (strstr(sec.name, "la_symbol_ptr") != nullptr || strstr(sec.name, "got") != nullptr) {
            uint32_t start_idx = sec.reserved1;
            size_t count = sec.size / sizeof(uintptr_t);
            uintptr_t *ptr_table = (uintptr_t *)sec.addr;

            for (size_t i = 0; i < count; i++) {
                if ((start_idx + i) >= nindirectsyms) break;
                if (dyld_bound_addrs.count((uintptr_t)&ptr_table[i]) != 0) continue;
                uint32_t sym_idx = indirect_syms[start_idx + i];
                if (sym_idx < nsyms) {
                    uint8_t *nlist = slice_data + symoff + sym_idx * 16;
                    uint32_t n_strx = *(uint32_t *)nlist;
                    if (n_strx < strsize) {
                        const char *sym_name = strtab + n_strx;
                        void *resolved = macho_resolve_symbol(sym_name);
                        if (resolved) {
                            ptr_table[i] = (uintptr_t)resolved;
                            indirect_bound++;
                        }
                    }
                }
            }
        } else if (strcmp(sec.name, "__DATA,__objc_classlist") == 0 && sec.size >= sizeof(uintptr_t)) {
            objc_register_macho_classes(sec.addr, sec.size / sizeof(uintptr_t));
        } else if (strcmp(sec.name, "__DATA,__mod_init_func") == 0 && sec.size >= sizeof(uintptr_t)) {
            g_mod_init_va = sec.addr;
            g_mod_init_count = sec.size / sizeof(uintptr_t);
        }
    }

    size_t total_bound = dyld_bound + indirect_bound;
    LOGI("Bound %zu dyld binds + %zu indirect symbols in Mach-O.", dyld_bound, indirect_bound);

    // Apply memory protections (TEXT -> r-x) and flush icache
    mprotect((void *)text_vmaddr, text_vmsize, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)text_vmaddr, (char *)(text_vmaddr + text_vmsize));

    if (out_info) {
        out_info->base_address = text_vmaddr;
        out_info->text_vmaddr = text_vmaddr;
        out_info->text_vmsize = text_vmsize;
        out_info->data_vmaddr = data_vmaddr;
        out_info->data_vmsize = data_vmsize;
        out_info->entry_point = entry_point;
        out_info->num_binds_resolved = total_bound;
    }

    free(file_data);
    LOGI("Mach-O in-memory loader successfully initialized! Entry point at 0x%lX", entry_point);
    return 0;
}

int macho_loader_execute_mod_inits(const MachOLoadInfo *info) {
    if (!info) return -1;
    uintptr_t *inits = (uintptr_t *)g_mod_init_va;
    size_t count = g_mod_init_count;

    LOGI("Executing %zu static initializers from __mod_init_func (0x%lX)...", count, g_mod_init_va);
    for (size_t i = 0; i < count; i++) {
        if (inits[i] != 0) {
            LOGI("  mod_init[%zu] -> 0x%lX", i, inits[i]);
            void (*init_func)() = (void (*)())inits[i];
            init_func();
        }
    }
    LOGI("All %zu static initializers executed successfully.", count);
    return 0;
}

int macho_loader_start_engine(const MachOLoadInfo *info, int argc, char **argv) {
    if (!info || info->entry_point == 0) return -1;

    // Execute static initializers first
    macho_loader_execute_mod_inits(info);

    LOGI("Invoking Mach-O entry point main() at 0x%lX...", info->entry_point);
    int (*entry_func)(int, char **) = (int (*)(int, char **))info->entry_point;
    return entry_func(argc, argv);
}
