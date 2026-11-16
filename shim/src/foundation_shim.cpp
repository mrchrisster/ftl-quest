#include "foundation_shim.h"
#include "darwin_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <android/log.h>

#define TAG "FTL_FOUNDATION_SHIM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct NSString_impl {
    id isa;
    std::string text;
};

struct NSBundle_impl {
    id isa;
    std::string bundle_path;
};

struct NSUserDefaults_impl {
    id isa;
    std::unordered_map<std::string, std::string> kv_store;
};

static NSBundle_impl g_main_bundle = { nullptr, "/sdcard/Android/data/com.subsetgames.ftl/files" };
static NSUserDefaults_impl g_defaults = { nullptr, {} };

extern "C" {

id nsstring_create_with_utf8(const char *utf8_str) {
    if (!utf8_str) return nil;
    NSString_impl *s = new NSString_impl();
    s->isa = objc_getClass("NSString");
    s->text = utf8_str;
    return (id)s;
}

const char *nsstring_get_utf8(id ns_str) {
    if (!ns_str) return "";
    void *isa = *(void **)ns_str;
    if (isa == ___CFConstantStringClassReference || isa == nullptr) {
        struct CFStr { void *isa; uintptr_t flags; const char *chars; size_t length; };
        CFStr *cf = (CFStr *)ns_str;
        return cf->chars ? cf->chars : "";
    }
    NSString_impl *s = (NSString_impl *)ns_str;
    return s->text.c_str();
}

id nsbundle_main_bundle(void) {
    g_main_bundle.isa = objc_getClass("NSBundle");
    return (id)&g_main_bundle;
}

id nsbundle_resource_path(id bundle) {
    NSBundle_impl *b = (NSBundle_impl *)bundle;
    return nsstring_create_with_utf8(b ? b->bundle_path.c_str() : "/sdcard/Android/data/com.subsetgames.ftl/files");
}

id nsbundle_path_for_resource(id bundle, id name, id ext) {
    const char *name_str = nsstring_get_utf8(name);
    const char *ext_str = ext ? nsstring_get_utf8(ext) : "";

    char full_path[1024];
    if (ext_str && strlen(ext_str) > 0) {
        snprintf(full_path, sizeof(full_path), "/sdcard/Android/data/com.subsetgames.ftl/files/%s.%s", name_str, ext_str);
    } else {
        snprintf(full_path, sizeof(full_path), "/sdcard/Android/data/com.subsetgames.ftl/files/%s", name_str);
    }
    LOGI("[NSBundle pathForResource] Requested '%s.%s' -> '%s'", name_str, ext_str, full_path);
    return nsstring_create_with_utf8(full_path);
}

id nsuserdefaults_standard(void) {
    g_defaults.isa = objc_getClass("NSUserDefaults");
    return (id)&g_defaults;
}

int nsuserdefaults_get_int(id defaults, const char *key) {
    if (!key) return 0;
    auto it = g_defaults.kv_store.find(key);
    if (it != g_defaults.kv_store.end()) {
        return atoi(it->second.c_str());
    }
    return 0;
}

void nsuserdefaults_set_int(id defaults, const char *key, int value) {
    if (!key) return;
    g_defaults.kv_store[key] = std::to_string(value);
}

const char *nsuserdefaults_get_string(id defaults, const char *key) {
    if (!key) return "";
    auto it = g_defaults.kv_store.find(key);
    if (it != g_defaults.kv_store.end()) {
        return it->second.c_str();
    }
    return "";
}

void nsuserdefaults_set_string(id defaults, const char *key, const char *val) {
    if (!key || !val) return;
    g_defaults.kv_store[key] = val;
}

struct NSArray_impl {
    id isa;
    std::vector<id> items;
};

static NSArray_impl g_search_paths = { nullptr, {} };

size_t nsarray_count(id arr) {
    if (!arr) return 0;
    return ((NSArray_impl *)arr)->items.size();
}

id nsarray_object_at_index(id arr, size_t idx) {
    if (!arr) return nil;
    NSArray_impl *a = (NSArray_impl *)arr;
    if (idx >= a->items.size()) return nil;
    return a->items[idx];
}

// Emulation of Apple's NSSearchPathForDirectoriesInDomains
void *NSSearchPathForDirectoriesInDomains(unsigned long directory, unsigned long domainMask, signed char expandTilde) {
    (void)domainMask; (void)expandTilde;
    g_search_paths.isa = objc_getClass("NSArray");
    g_search_paths.items.clear();
    if (directory == NSDocumentDirectory) {
        g_search_paths.items.push_back(nsstring_create_with_utf8("/sdcard/Android/data/com.subsetgames.ftl/files/save"));
    } else {
        g_search_paths.items.push_back(nsstring_create_with_utf8("/sdcard/Android/data/com.subsetgames.ftl/files"));
    }
    return (void *)&g_search_paths;
}

}
