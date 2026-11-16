#include "objc_shim.h"
#include "darwin_abi.h"
#include "eagl_shim.h"
#include "foundation_shim.h"
#include "uikit_shim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>

#define TAG "FTL_OBJC_SHIM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

void *_objc_empty_cache[2] = { nullptr, nullptr };
void *_objc_empty_vtable[2] = { nullptr, nullptr };

// Framework string constant instances (CFString_MachO layout)
struct StaticCFString {
    void *isa;
    uintptr_t flags;
    const char *chars;
    size_t length;
};

static StaticCFString g_str_kEAGLColorFormatRGBA8 = { ___CFConstantStringClassReference, 0x7C8, "kEAGLColorFormatRGBA8", 21 };
static StaticCFString g_str_kEAGLDrawablePropertyColorFormat = { ___CFConstantStringClassReference, 0x7C8, "kEAGLDrawablePropertyColorFormat", 32 };
static StaticCFString g_str_kEAGLDrawablePropertyRetainedBacking = { ___CFConstantStringClassReference, 0x7C8, "kEAGLDrawablePropertyRetainedBacking", 36 };
static StaticCFString g_str_AVAudioSessionCategoryAmbient = { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionCategoryAmbient", 29 };
static StaticCFString g_str_AVAudioSessionCategoryPlayback = { ___CFConstantStringClassReference, 0x7C8, "AVAudioSessionCategoryPlayback", 30 };

void *kEAGLColorFormatRGBA8 = &g_str_kEAGLColorFormatRGBA8;
void *kEAGLDrawablePropertyColorFormat = &g_str_kEAGLDrawablePropertyColorFormat;
void *kEAGLDrawablePropertyRetainedBacking = &g_str_kEAGLDrawablePropertyRetainedBacking;
void *AVAudioSessionCategoryAmbient = &g_str_AVAudioSessionCategoryAmbient;
void *AVAudioSessionCategoryPlayback = &g_str_AVAudioSessionCategoryPlayback;

struct ClassRuntimeInfo {
    std::string name;
    bool is_metaclass = false;
    size_t instance_size = 64;
    std::unordered_map<std::string, IMP> instance_methods;
    std::unordered_map<std::string, IMP> class_methods;
};

static std::recursive_mutex g_objc_mutex;
static std::unordered_map<std::string, const char *> g_selector_table;
static std::unordered_map<std::string, Class> g_class_table;
static std::unordered_map<std::string, Class> g_metaclass_table;
static std::unordered_map<Class, ClassRuntimeInfo *> g_class_info;
static std::unordered_set<void *> g_known_classes;
static std::unordered_map<id, int> g_refcounts;
static std::unordered_map<id, void *> g_extra_data;

// Global UI singletons
static id g_app_delegate = nullptr;
static id g_ui_application = nullptr;
static id g_ui_window = nullptr;
static id g_ui_screen = nullptr;
static id g_ui_device = nullptr;
static __thread id g_tls_eagl_current_obj = nullptr;
static id g_notification_center = nullptr;
static id g_run_loop = nullptr;
static id g_audio_session = nullptr;
static id g_local_player = nullptr;

static bool g_builtins_initialized = false;
static void init_builtin_classes();

static Class create_shim_class(const char *name, Class superclass, size_t instance_size = 64) {
    auto it = g_class_table.find(name);
    if (it != g_class_table.end()) {
        if (superclass && !it->second->superclass) {
            it->second->superclass = superclass;
        }
        return it->second;
    }

    auto *ro = (objc_class_ro *)calloc(1, sizeof(objc_class_ro));
    ro->instanceSize = (uint32_t)instance_size;
    ro->name = strdup(name);

    auto *meta_ro = (objc_class_ro *)calloc(1, sizeof(objc_class_ro));
    meta_ro->instanceSize = sizeof(objc_class);
    meta_ro->name = ro->name;

    Class metacls = (Class)calloc(1, sizeof(objc_class));
    metacls->superclass = superclass ? superclass->isa : nullptr;
    metacls->cache = _objc_empty_cache;
    metacls->vtable = _objc_empty_vtable;
    metacls->ro = meta_ro;

    Class cls = (Class)calloc(1, sizeof(objc_class));
    cls->isa = metacls;
    cls->superclass = superclass;
    cls->cache = _objc_empty_cache;
    cls->vtable = _objc_empty_vtable;
    cls->ro = ro;

    auto *info = new ClassRuntimeInfo();
    info->name = name;
    info->is_metaclass = false;
    info->instance_size = instance_size;

    auto *metainfo = new ClassRuntimeInfo();
    metainfo->name = name;
    metainfo->is_metaclass = true;
    metainfo->instance_size = sizeof(objc_class);

    g_class_table[name] = cls;
    g_metaclass_table[name] = metacls;
    g_class_info[cls] = info;
    g_class_info[metacls] = metainfo;
    g_known_classes.insert(cls);
    g_known_classes.insert(metacls);
    return cls;
}

static void add_class_method(Class cls, const char *sel_name, IMP imp) {
    if (!cls || !sel_name || !imp) return;
    auto it = g_class_info.find(cls);
    if (it != g_class_info.end()) {
        it->second->class_methods[sel_name] = imp;
    }
    if (cls->isa) {
        auto mit = g_class_info.find(cls->isa);
        if (mit != g_class_info.end()) {
            mit->second->instance_methods[sel_name] = imp;
        }
    }
}

static void add_instance_method(Class cls, const char *sel_name, IMP imp) {
    if (!cls || !sel_name || !imp) return;
    auto it = g_class_info.find(cls);
    if (it != g_class_info.end()) {
        it->second->instance_methods[sel_name] = imp;
    }
}

// ============================================================================
// Typed C++ Implementations for Foundation / UIKit / EAGL / AVAudioSession
// ============================================================================

static id shim_NSObject_alloc(id self, SEL _cmd) {
    (void)_cmd;
    return objc_alloc((Class)self);
}

static id shim_NSObject_allocWithZone(id self, SEL _cmd, void *zone) {
    (void)_cmd; (void)zone;
    return objc_alloc((Class)self);
}

static id shim_NSObject_new(id self, SEL _cmd) {
    (void)_cmd;
    id obj = objc_alloc((Class)self);
    return objc_msgSend(obj, sel_registerName("init"));
}

static id shim_NSObject_init(id self, SEL _cmd) {
    (void)_cmd;
    return self;
}

static id shim_NSObject_retain(id self, SEL _cmd) {
    (void)_cmd;
    return objc_retain(self);
}

static id shim_NSObject_release(id self, SEL _cmd) {
    (void)_cmd;
    objc_release(self);
    return nil;
}

static id shim_NSObject_autorelease(id self, SEL _cmd) {
    (void)_cmd;
    return objc_autorelease(self);
}

static id shim_NSObject_class(id self, SEL _cmd) {
    (void)_cmd;
    return (id)object_getClass(self);
}

static id shim_NSObject_self(id self, SEL _cmd) {
    (void)_cmd;
    return self;
}

static BOOL shim_NSObject_respondsToSelector(id self, SEL _cmd, SEL aSel) {
    (void)_cmd;
    return objc_lookup_imp_for_send(self, aSel) != nullptr ? YES : NO;
}

static BOOL shim_NSObject_isKindOfClass(id self, SEL _cmd, Class target) {
    (void)_cmd;
    Class cls = object_getClass(self);
    while (cls) {
        if (cls == target) return YES;
        cls = cls->superclass;
    }
    return NO;
}

// NSString
static id shim_NSString_stringWithUTF8String(id self, SEL _cmd, const char *cstr) {
    (void)self; (void)_cmd;
    return nsstring_create_with_utf8(cstr ? cstr : "");
}

static id shim_NSString_initWithUTF8String(id self, SEL _cmd, const char *cstr) {
    (void)self; (void)_cmd;
    return nsstring_create_with_utf8(cstr ? cstr : "");
}

static id shim_NSString_initWithBytes(id self, SEL _cmd, const void *bytes, size_t len, unsigned long encoding) {
    (void)self; (void)_cmd; (void)encoding;
    std::string s(bytes ? (const char *)bytes : "", len);
    return nsstring_create_with_utf8(s.c_str());
}

static const char *shim_NSString_UTF8String(id self, SEL _cmd) {
    (void)_cmd;
    return nsstring_get_utf8(self);
}

static size_t shim_NSString_length(id self, SEL _cmd) {
    (void)_cmd;
    const char *s = nsstring_get_utf8(self);
    return s ? strlen(s) : 0;
}

static BOOL shim_NSString_getCString(id self, SEL _cmd, char *buf, size_t maxLen, unsigned long enc) {
    (void)_cmd; (void)enc;
    const char *s = nsstring_get_utf8(self);
    if (!buf || maxLen == 0) return NO;
    strncpy(buf, s ? s : "", maxLen - 1);
    buf[maxLen - 1] = '\0';
    return YES;
}

static long shim_NSString_compare(id self, SEL _cmd, id other, ...) {
    (void)_cmd;
    const char *a = nsstring_get_utf8(self);
    const char *b = nsstring_get_utf8(other);
    int c = strcmp(a ? a : "", b ? b : "");
    if (c < 0) return -1;
    if (c > 0) return 1;
    return 0;
}

// NSBundle
static id shim_NSBundle_mainBundle(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return nsbundle_main_bundle();
}

static id shim_NSBundle_resourcePath(id self, SEL _cmd) {
    (void)_cmd;
    return nsbundle_resource_path(self);
}

// NSUserDefaults
static id shim_NSUserDefaults_standardUserDefaults(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return nsuserdefaults_standard();
}

static id shim_NSUserDefaults_synchronize(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return (id)YES;
}

static id shim_NSUserDefaults_objectForKey(id self, SEL _cmd, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    const char *v = nsuserdefaults_get_string(self, k);
    if (!v || v[0] == '\0') return nil;
    return nsstring_create_with_utf8(v);
}

static BOOL shim_NSUserDefaults_boolForKey(id self, SEL _cmd, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    return nsuserdefaults_get_int(self, k) != 0 ? YES : NO;
}

static void shim_NSUserDefaults_setObjectForKey(id self, SEL _cmd, id obj, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    const char *v = nsstring_get_utf8(obj);
    nsuserdefaults_set_string(self, k, v);
}

static long shim_NSUserDefaults_integerForKey(id self, SEL _cmd, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    return (long)nsuserdefaults_get_int(self, k);
}

static id shim_NSUserDefaults_stringForKey(id self, SEL _cmd, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    const char *v = nsuserdefaults_get_string(self, k);
    if (!v || v[0] == '\0') return nil;
    return nsstring_create_with_utf8(v);
}

static void shim_NSUserDefaults_setBoolForKey(id self, SEL _cmd, BOOL val, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    nsuserdefaults_set_int(self, k, val ? 1 : 0);
}

static void shim_NSUserDefaults_setIntegerForKey(id self, SEL _cmd, long val, id key) {
    (void)_cmd;
    const char *k = nsstring_get_utf8(key);
    nsuserdefaults_set_int(self, k, (int)val);
}

// NSArray / NSSet / NSEnumerator
static size_t shim_NSArray_count(id self, SEL _cmd) {
    (void)_cmd;
    if (!self) return 0;
    return nsarray_count(self);
}

static id shim_NSArray_objectAtIndex(id self, SEL _cmd, size_t idx) {
    (void)_cmd;
    return nsarray_object_at_index(self, idx);
}

static id shim_NSSet_objectEnumerator(id self, SEL _cmd) {
    (void)_cmd;
    return quest_touchset_enumerator(self);
}

static id shim_NSEnumerator_nextObject(id self, SEL _cmd) {
    (void)_cmd;
    return quest_enumerator_next_object(self);
}

// NSDictionary / NSMutableDictionary
struct NSDictionary_impl {
    Class isa;
    std::unordered_map<std::string, id> kv;
};

static id shim_NSDictionary_init(id self, SEL _cmd) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    if (g_extra_data.find(self) == g_extra_data.end()) {
        g_extra_data[self] = new NSDictionary_impl();
    }
    return self;
}

static id shim_NSDictionary_withObjectsForKeysCount(id self, SEL _cmd, id *objs, id *keys, size_t count) {
    (void)self; (void)_cmd;
    id dict = objc_alloc(objc_getClass("NSMutableDictionary"));
    auto *d = new NSDictionary_impl();
    for (size_t i = 0; i < count; i++) {
        const char *k = keys && keys[i] ? nsstring_get_utf8(keys[i]) : "";
        if (k) d->kv[k] = objs ? objs[i] : nil;
    }
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    g_extra_data[dict] = d;
    return dict;
}

static id shim_NSDictionary_objectForKey(id self, SEL _cmd, id key) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_extra_data.find(self);
    if (it == g_extra_data.end() || !it->second) return nil;
    auto *d = (NSDictionary_impl *)it->second;
    const char *k = nsstring_get_utf8(key);
    if (!k) return nil;
    auto kit = d->kv.find(k);
    return (kit != d->kv.end()) ? kit->second : nil;
}

static void shim_NSDictionary_setObjectForKey(id self, SEL _cmd, id obj, id key) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto *d = (NSDictionary_impl *)g_extra_data[self];
    if (!d) {
        d = new NSDictionary_impl();
        g_extra_data[self] = d;
    }
    const char *k = nsstring_get_utf8(key);
    if (k) d->kv[k] = obj;
}

static void shim_NSUserDefaults_registerDefaults(id self, SEL _cmd, id dict) {
    (void)_cmd;
    if (!dict) return;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_extra_data.find(dict);
    if (it == g_extra_data.end() || !it->second) return;
    auto *d = (NSDictionary_impl *)it->second;
    Class numCls = objc_getClass("NSNumber");
    for (const auto &entry : d->kv) {
        const char *existing = nsuserdefaults_get_string(self, entry.first.c_str());
        if (existing && existing[0] != '\0') continue;
        id val = entry.second;
        if (!val) continue;
        if (numCls && *(Class *)val == numCls) {
            auto nit = g_extra_data.find(val);
            int iv = (nit != g_extra_data.end()) ? (int)(intptr_t)nit->second : 0;
            nsuserdefaults_set_int(self, entry.first.c_str(), iv);
        } else {
            const char *sv = nsstring_get_utf8(val);
            if (sv) nsuserdefaults_set_string(self, entry.first.c_str(), sv);
        }
    }
}

// NSNumber
static id shim_NSNumber_numberWithInt(id self, SEL _cmd, int val) {
    (void)self; (void)_cmd;
    id num = objc_alloc(objc_getClass("NSNumber"));
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    g_extra_data[num] = (void *)(intptr_t)val;
    return num;
}

static id shim_NSNumber_numberWithBool(id self, SEL _cmd, BOOL val) {
    return shim_NSNumber_numberWithInt(self, _cmd, val ? 1 : 0);
}

static id shim_NSNumber_numberWithInteger(id self, SEL _cmd, long val) {
    return shim_NSNumber_numberWithInt(self, _cmd, (int)val);
}

static int shim_NSNumber_intValue(id self, SEL _cmd) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_extra_data.find(self);
    return (it != g_extra_data.end()) ? (int)(intptr_t)it->second : 0;
}

static BOOL shim_NSNumber_boolValue(id self, SEL _cmd) {
    return shim_NSNumber_intValue(self, _cmd) != 0 ? YES : NO;
}

static long shim_NSNumber_integerValue(id self, SEL _cmd) {
    return (long)shim_NSNumber_intValue(self, _cmd);
}

// NSNotificationCenter & NSRunLoop
static id shim_NSNotificationCenter_defaultCenter(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_notification_center) g_notification_center = objc_alloc(objc_getClass("NSNotificationCenter"));
    return g_notification_center;
}

static void shim_NSNotificationCenter_addObserver(id self, SEL _cmd, id observer, SEL aSelector, id aName, id anObject) {
    (void)self; (void)_cmd; (void)observer; (void)aSelector; (void)aName; (void)anObject;
}

static void shim_NSNotificationCenter_removeObserver(id self, SEL _cmd, id observer, ...) {
    (void)self; (void)_cmd; (void)observer;
}

static void shim_NSNotificationCenter_postNotification(id self, SEL _cmd, id aName, id anObject, ...) {
    (void)self; (void)_cmd; (void)aName; (void)anObject;
}

static id shim_NSRunLoop_currentRunLoop(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_run_loop) g_run_loop = objc_alloc(objc_getClass("NSRunLoop"));
    return g_run_loop;
}

// UIDevice
static id shim_UIDevice_currentDevice(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_ui_device) g_ui_device = objc_alloc(objc_getClass("UIDevice"));
    return g_ui_device;
}

static id shim_UIDevice_systemVersion(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return nsstring_create_with_utf8("10.0");
}

static long shim_UIDevice_userInterfaceIdiom(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 1; // UIUserInterfaceIdiomPad
}

// UIScreen
static id shim_UIScreen_mainScreen(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_ui_screen) g_ui_screen = objc_alloc(objc_getClass("UIScreen"));
    return g_ui_screen;
}

static CGRect shim_UIScreen_bounds(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    int w = 1280, h = 800;
    eagl_get_surface_size(&w, &h);
    return CGRect{{0.0, 0.0}, {(double)w, (double)h}};
}

static double shim_UIScreen_scale(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 1.0;
}

static long shim_UIScreen_maximumFramesPerSecond(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 90;
}

// UIApplication
static id shim_UIApplication_sharedApplication(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_ui_application) g_ui_application = objc_alloc(objc_getClass("UIApplication"));
    return g_ui_application;
}

static void shim_noop_void(id self, SEL _cmd, ...) {
    (void)self; (void)_cmd;
}

static id shim_UIApplication_delegate(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return g_app_delegate;
}

// UIView / UIWindow / UIViewController
static id shim_UIView_initWithFrame(id self, SEL _cmd, CGRect frame) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    CGRect *stored = (CGRect *)malloc(sizeof(CGRect));
    *stored = frame;
    g_extra_data[self] = stored;

    const char *cname = class_getName(object_getClass(self));
    if (cname && strcmp(cname, "SILView") == 0) {
        quest_set_root_view(self);
    }
    return self;
}

static CGRect shim_UIView_bounds(id self, SEL _cmd) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_extra_data.find(self);
    if (it != g_extra_data.end() && it->second) {
        CGRect r = *(CGRect *)it->second;
        if (r.size.width > 0 && r.size.height > 0) {
            r.origin.x = 0.0;
            r.origin.y = 0.0;
            return r;
        }
    }
    return shim_UIScreen_bounds(self, _cmd);
}

static double shim_UIView_contentScaleFactor(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 1.0;
}

static id shim_UIView_layer(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return objc_alloc(objc_getClass("CAEAGLLayer"));
}

static id shim_UIView_layerClass(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return (id)objc_getClass("CAEAGLLayer");
}

static id shim_UIWindow_initWithFrame(id self, SEL _cmd, CGRect frame) {
    g_ui_window = self;
    return shim_UIView_initWithFrame(self, _cmd, frame);
}

static id shim_UIViewController_initWithNibName(id self, SEL _cmd, id nib, id bundle) {
    (void)_cmd; (void)nib; (void)bundle;
    return self;
}

// UITouch
static double shim_UITouch_timestamp(id self, SEL _cmd) {
    (void)_cmd;
    if (!self) return 0.0;
    UITouch_t *t = (UITouch_t *)self;
    return t->timestamp;
}

static CGPoint shim_UITouch_locationInView(id self, SEL _cmd, id view) {
    (void)_cmd; (void)view;
    if (!self) return CGPoint{0.0, 0.0};
    UITouch_t *t = (UITouch_t *)self;
    return t->location;
}

// EAGLContext
static id shim_EAGLContext_initWithAPI(id self, SEL _cmd, int api) {
    (void)_cmd;
    EAGLContextRef ctx = eagl_create_context((EAGLRenderingAPI)api);
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    g_extra_data[self] = ctx;
    return self;
}

static id shim_EAGLContext_initWithAPISharegroup(id self, SEL _cmd, int api, void *sharegroup) {
    (void)sharegroup;
    return shim_EAGLContext_initWithAPI(self, _cmd, api);
}

static int shim_EAGLContext_API(id self, SEL _cmd) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_extra_data.find(self);
    if (it != g_extra_data.end() && it->second) {
        return (int)((EAGLContextRef)it->second)->api;
    }
    return kEAGLRenderingAPIOpenGLES3;
}

static id shim_EAGLContext_sharegroup(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return (id)0x1;
}

static BOOL shim_EAGLContext_setCurrentContext(id self, SEL _cmd, id ctxObj) {
    (void)self; (void)_cmd;
    g_tls_eagl_current_obj = ctxObj;
    if (!ctxObj) {
        return eagl_set_current_context(nullptr) ? YES : NO;
    }
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    EAGLContextRef ctx = (EAGLContextRef)g_extra_data[ctxObj];
    return eagl_set_current_context(ctx) ? YES : NO;
}

static id shim_EAGLContext_currentContext(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return g_tls_eagl_current_obj;
}

static BOOL shim_EAGLContext_presentRenderbuffer(id self, SEL _cmd, unsigned int target) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    EAGLContextRef ctx = (EAGLContextRef)g_extra_data[self];
    return eagl_present_renderbuffer(ctx, target) ? YES : NO;
}

static BOOL shim_EAGLContext_renderbufferStorageFromDrawable(id self, SEL _cmd, unsigned int target, id drawable) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    EAGLContextRef ctx = (EAGLContextRef)g_extra_data[self];
    return eagl_renderbuffer_storage_from_drawable(ctx, target, drawable) ? YES : NO;
}

// CADisplayLink
struct DisplayLinkState {
    id target = nil;
    SEL sel = nullptr;
    int fps = 90;
    volatile bool active = false;
    pthread_t thread = 0;
};

static void *display_link_thread(void *arg) {
    DisplayLinkState *st = (DisplayLinkState *)arg;
    LOGI("CADisplayLink thread started (target=%p sel=%s fps=%d)",
         st->target, st->sel ? (const char *)st->sel : "(null)", st->fps);
    while (st->active) {
        if (st->target && st->sel) {
            objc_msgSend(st->target, st->sel);
        }
        int us = (st->fps > 0) ? (1000000 / st->fps) : 11111;
        if (us < 1000) us = 11111;
        usleep((useconds_t)us);
    }
    return nullptr;
}

static id shim_CADisplayLink_displayLinkWithTarget(id self, SEL _cmd, id target, SEL sel) {
    (void)self; (void)_cmd;
    id link = objc_alloc(objc_getClass("CADisplayLink"));
    auto *st = new DisplayLinkState();
    st->target = target;
    st->sel = sel;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    g_extra_data[link] = st;
    return link;
}

static void shim_CADisplayLink_addToRunLoop(id self, SEL _cmd, id runloop, id mode) {
    (void)_cmd; (void)runloop; (void)mode;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto *st = (DisplayLinkState *)g_extra_data[self];
    if (st && !st->active) {
        st->active = true;
        pthread_create(&st->thread, nullptr, display_link_thread, st);
        pthread_detach(st->thread);
    }
}

static void shim_CADisplayLink_setPreferredFPS(id self, SEL _cmd, long fps) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto *st = (DisplayLinkState *)g_extra_data[self];
    if (st && fps > 0) st->fps = (int)fps;
}

static void shim_CADisplayLink_invalidate(id self, SEL _cmd) {
    (void)_cmd;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto *st = (DisplayLinkState *)g_extra_data[self];
    if (st) st->active = false;
}

// AVAudioSession
static id shim_AVAudioSession_sharedInstance(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_audio_session) g_audio_session = objc_alloc(objc_getClass("AVAudioSession"));
    return g_audio_session;
}

static double shim_AVAudioSession_sampleRate(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 44100.0;
}

static double shim_AVAudioSession_IOBufferDuration(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 512.0 / 44100.0;
}

static double shim_AVAudioSession_outputLatency(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return 0.0;
}

static BOOL shim_AVAudioSession_retYES(id self, SEL _cmd, ...) {
    (void)self; (void)_cmd;
    return YES;
}

// GCController / GKLocalPlayer
static id shim_GCController_controllers(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    static id empty_arr = nil;
    if (!empty_arr) empty_arr = objc_alloc(objc_getClass("NSArray"));
    return empty_arr;
}

static id shim_GKLocalPlayer_localPlayer(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    if (!g_local_player) g_local_player = objc_alloc(objc_getClass("GKLocalPlayer"));
    return g_local_player;
}

static BOOL shim_GKLocalPlayer_isAuthenticated(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return NO;
}

static id shim_GKLocalPlayer_playerID(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return nsstring_create_with_utf8("");
}

struct AppleBlockLiteral {
    void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *block, ...);
    void *descriptor;
};

static void shim_GKLocalPlayer_authenticateWithCompletionHandler(id self, SEL _cmd, id handler) {
    (void)self; (void)_cmd;
    LOGI("GKLocalPlayer authenticateWithCompletionHandler: handler=%p", handler);
    if (handler) {
        auto *blk = (AppleBlockLiteral *)handler;
        if (blk->invoke) {
            id dummy_err = objc_alloc(objc_getClass("NSObject"));
            LOGI("GKLocalPlayer invoking completion block=%p invoke=%p (err=%p)", blk, blk->invoke, dummy_err);
            ((void (*)(void *, id))blk->invoke)(blk, dummy_err);
        }
    }
}

static void shim_GKLocalPlayer_setAuthenticateHandler(id self, SEL _cmd, id handler) {
    (void)self; (void)_cmd;
    LOGI("GKLocalPlayer setAuthenticateHandler: handler=%p", handler);
    if (handler) {
        auto *blk = (AppleBlockLiteral *)handler;
        if (blk->invoke) {
            id dummy_err = objc_alloc(objc_getClass("NSObject"));
            ((void (*)(void *, id, id))blk->invoke)(blk, nil, dummy_err);
        }
    }
}

static void init_builtin_classes() {
    if (g_builtins_initialized) return;
    g_builtins_initialized = true;

    Class nsObject = create_shim_class("NSObject", nullptr, 8);
    add_class_method(nsObject, "alloc", (IMP)shim_NSObject_alloc);
    add_class_method(nsObject, "allocWithZone:", (IMP)shim_NSObject_allocWithZone);
    add_class_method(nsObject, "new", (IMP)shim_NSObject_new);
    add_class_method(nsObject, "class", (IMP)shim_NSObject_self);
    add_instance_method(nsObject, "init", (IMP)shim_NSObject_init);
    add_instance_method(nsObject, "retain", (IMP)shim_NSObject_retain);
    add_instance_method(nsObject, "release", (IMP)shim_NSObject_release);
    add_instance_method(nsObject, "autorelease", (IMP)shim_NSObject_autorelease);
    add_instance_method(nsObject, "class", (IMP)shim_NSObject_class);
    add_instance_method(nsObject, "self", (IMP)shim_NSObject_self);
    add_instance_method(nsObject, "respondsToSelector:", (IMP)shim_NSObject_respondsToSelector);
    add_instance_method(nsObject, "isKindOfClass:", (IMP)shim_NSObject_isKindOfClass);
    add_instance_method(nsObject, "dealloc", (IMP)shim_NSObject_release);

    Class nsString = create_shim_class("NSString", nsObject);
    add_class_method(nsString, "stringWithUTF8String:", (IMP)shim_NSString_stringWithUTF8String);
    add_instance_method(nsString, "initWithUTF8String:", (IMP)shim_NSString_initWithUTF8String);
    add_instance_method(nsString, "initWithBytes:length:encoding:", (IMP)shim_NSString_initWithBytes);
    add_instance_method(nsString, "UTF8String", (IMP)shim_NSString_UTF8String);
    add_instance_method(nsString, "length", (IMP)shim_NSString_length);
    add_instance_method(nsString, "lengthOfBytesUsingEncoding:", (IMP)shim_NSString_length);
    add_instance_method(nsString, "getCString:maxLength:encoding:", (IMP)shim_NSString_getCString);
    add_instance_method(nsString, "compare:", (IMP)shim_NSString_compare);
    add_instance_method(nsString, "compare:options:", (IMP)shim_NSString_compare);

    Class nsBundle = create_shim_class("NSBundle", nsObject);
    add_class_method(nsBundle, "mainBundle", (IMP)shim_NSBundle_mainBundle);
    add_instance_method(nsBundle, "resourcePath", (IMP)shim_NSBundle_resourcePath);

    Class nsDefaults = create_shim_class("NSUserDefaults", nsObject);
    add_class_method(nsDefaults, "standardUserDefaults", (IMP)shim_NSUserDefaults_standardUserDefaults);
    add_instance_method(nsDefaults, "synchronize", (IMP)shim_NSUserDefaults_synchronize);
    add_instance_method(nsDefaults, "objectForKey:", (IMP)shim_NSUserDefaults_objectForKey);
    add_instance_method(nsDefaults, "boolForKey:", (IMP)shim_NSUserDefaults_boolForKey);
    add_instance_method(nsDefaults, "integerForKey:", (IMP)shim_NSUserDefaults_integerForKey);
    add_instance_method(nsDefaults, "stringForKey:", (IMP)shim_NSUserDefaults_stringForKey);
    add_instance_method(nsDefaults, "setObject:forKey:", (IMP)shim_NSUserDefaults_setObjectForKey);
    add_instance_method(nsDefaults, "setBool:forKey:", (IMP)shim_NSUserDefaults_setBoolForKey);
    add_instance_method(nsDefaults, "setInteger:forKey:", (IMP)shim_NSUserDefaults_setIntegerForKey);
    add_instance_method(nsDefaults, "registerDefaults:", (IMP)shim_NSUserDefaults_registerDefaults);

    Class nsArray = create_shim_class("NSArray", nsObject);
    add_instance_method(nsArray, "count", (IMP)shim_NSArray_count);
    add_instance_method(nsArray, "objectAtIndex:", (IMP)shim_NSArray_objectAtIndex);

    Class nsSet = create_shim_class("NSSet", nsObject);
    add_instance_method(nsSet, "objectEnumerator", (IMP)shim_NSSet_objectEnumerator);

    Class nsEnum = create_shim_class("NSEnumerator", nsObject);
    add_instance_method(nsEnum, "nextObject", (IMP)shim_NSEnumerator_nextObject);

    Class nsDict = create_shim_class("NSDictionary", nsObject);
    add_class_method(nsDict, "dictionaryWithObjects:forKeys:count:", (IMP)shim_NSDictionary_withObjectsForKeysCount);
    add_instance_method(nsDict, "init", (IMP)shim_NSDictionary_init);
    add_instance_method(nsDict, "objectForKey:", (IMP)shim_NSDictionary_objectForKey);
    add_instance_method(nsDict, "valueForKey:", (IMP)shim_NSDictionary_objectForKey);
    add_instance_method(nsDict, "objectForKeyedSubscript:", (IMP)shim_NSDictionary_objectForKey);

    Class nsMutDict = create_shim_class("NSMutableDictionary", nsDict);
    add_instance_method(nsMutDict, "setObject:forKey:", (IMP)shim_NSDictionary_setObjectForKey);
    add_instance_method(nsMutDict, "setObject:forKeyedSubscript:", (IMP)shim_NSDictionary_setObjectForKey);

    Class nsNumber = create_shim_class("NSNumber", nsObject);
    add_class_method(nsNumber, "numberWithInt:", (IMP)shim_NSNumber_numberWithInt);
    add_class_method(nsNumber, "numberWithBool:", (IMP)shim_NSNumber_numberWithBool);
    add_class_method(nsNumber, "numberWithInteger:", (IMP)shim_NSNumber_numberWithInteger);
    add_instance_method(nsNumber, "intValue", (IMP)shim_NSNumber_intValue);
    add_instance_method(nsNumber, "boolValue", (IMP)shim_NSNumber_boolValue);
    add_instance_method(nsNumber, "integerValue", (IMP)shim_NSNumber_integerValue);

    Class nsNotif = create_shim_class("NSNotificationCenter", nsObject);
    add_class_method(nsNotif, "defaultCenter", (IMP)shim_NSNotificationCenter_defaultCenter);
    add_instance_method(nsNotif, "addObserver:selector:name:object:", (IMP)shim_NSNotificationCenter_addObserver);
    add_instance_method(nsNotif, "removeObserver:", (IMP)shim_NSNotificationCenter_removeObserver);
    add_instance_method(nsNotif, "removeObserver:name:object:", (IMP)shim_NSNotificationCenter_removeObserver);
    add_instance_method(nsNotif, "postNotificationName:object:", (IMP)shim_NSNotificationCenter_postNotification);
    add_instance_method(nsNotif, "postNotificationName:object:userInfo:", (IMP)shim_NSNotificationCenter_postNotification);

    Class nsRunLoop = create_shim_class("NSRunLoop", nsObject);
    add_class_method(nsRunLoop, "currentRunLoop", (IMP)shim_NSRunLoop_currentRunLoop);

    create_shim_class("NSAutoreleasePool", nsObject);
    create_shim_class("NSURL", nsObject);

    Class uiDevice = create_shim_class("UIDevice", nsObject);
    add_class_method(uiDevice, "currentDevice", (IMP)shim_UIDevice_currentDevice);
    add_instance_method(uiDevice, "systemVersion", (IMP)shim_UIDevice_systemVersion);
    add_instance_method(uiDevice, "userInterfaceIdiom", (IMP)shim_UIDevice_userInterfaceIdiom);

    Class uiScreen = create_shim_class("UIScreen", nsObject);
    add_class_method(uiScreen, "mainScreen", (IMP)shim_UIScreen_mainScreen);
    add_instance_method(uiScreen, "bounds", (IMP)shim_UIScreen_bounds);
    add_instance_method(uiScreen, "scale", (IMP)shim_UIScreen_scale);
    add_instance_method(uiScreen, "nativeScale", (IMP)shim_UIScreen_scale);
    add_instance_method(uiScreen, "maximumFramesPerSecond", (IMP)shim_UIScreen_maximumFramesPerSecond);

    Class uiApp = create_shim_class("UIApplication", nsObject);
    add_class_method(uiApp, "sharedApplication", (IMP)shim_UIApplication_sharedApplication);
    add_instance_method(uiApp, "delegate", (IMP)shim_UIApplication_delegate);
    add_instance_method(uiApp, "setIdleTimerDisabled:", (IMP)shim_noop_void);

    Class uiView = create_shim_class("UIView", nsObject);
    add_class_method(uiView, "layerClass", (IMP)shim_UIView_layerClass);
    add_instance_method(uiView, "initWithFrame:", (IMP)shim_UIView_initWithFrame);
    add_instance_method(uiView, "bounds", (IMP)shim_UIView_bounds);
    add_instance_method(uiView, "contentScaleFactor", (IMP)shim_UIView_contentScaleFactor);
    add_instance_method(uiView, "setContentScaleFactor:", (IMP)shim_noop_void);
    add_instance_method(uiView, "setMultipleTouchEnabled:", (IMP)shim_noop_void);
    add_instance_method(uiView, "layer", (IMP)shim_UIView_layer);

    Class uiWindow = create_shim_class("UIWindow", uiView);
    add_instance_method(uiWindow, "initWithFrame:", (IMP)shim_UIWindow_initWithFrame);
    add_instance_method(uiWindow, "setRootViewController:", (IMP)shim_noop_void);
    add_instance_method(uiWindow, "makeKeyAndVisible", (IMP)shim_noop_void);

    Class uiVC = create_shim_class("UIViewController", nsObject);
    add_instance_method(uiVC, "initWithNibName:bundle:", (IMP)shim_UIViewController_initWithNibName);

    Class uiTouch = create_shim_class("UITouch", nsObject);
    add_instance_method(uiTouch, "timestamp", (IMP)shim_UITouch_timestamp);
    add_instance_method(uiTouch, "locationInView:", (IMP)shim_UITouch_locationInView);

    Class eaglLayer = create_shim_class("CAEAGLLayer", nsObject);
    add_instance_method(eaglLayer, "setOpaque:", (IMP)shim_noop_void);
    add_instance_method(eaglLayer, "setDrawableProperties:", (IMP)shim_noop_void);
    create_shim_class("UIColor", nsObject);
    create_shim_class("UILabel", uiView);
    create_shim_class("UITextField", uiView);
    create_shim_class("UIAlertView", nsObject);

    Class eaglCtx = create_shim_class("EAGLContext", nsObject);
    add_class_method(eaglCtx, "setCurrentContext:", (IMP)shim_EAGLContext_setCurrentContext);
    add_class_method(eaglCtx, "currentContext", (IMP)shim_EAGLContext_currentContext);
    add_instance_method(eaglCtx, "initWithAPI:", (IMP)shim_EAGLContext_initWithAPI);
    add_instance_method(eaglCtx, "initWithAPI:sharegroup:", (IMP)shim_EAGLContext_initWithAPISharegroup);
    add_instance_method(eaglCtx, "API", (IMP)shim_EAGLContext_API);
    add_instance_method(eaglCtx, "sharegroup", (IMP)shim_EAGLContext_sharegroup);
    add_instance_method(eaglCtx, "presentRenderbuffer:", (IMP)shim_EAGLContext_presentRenderbuffer);
    add_instance_method(eaglCtx, "renderbufferStorage:fromDrawable:", (IMP)shim_EAGLContext_renderbufferStorageFromDrawable);

    Class dispLink = create_shim_class("CADisplayLink", nsObject);
    add_class_method(dispLink, "displayLinkWithTarget:selector:", (IMP)shim_CADisplayLink_displayLinkWithTarget);
    add_instance_method(dispLink, "addToRunLoop:forMode:", (IMP)shim_CADisplayLink_addToRunLoop);
    add_instance_method(dispLink, "setPreferredFramesPerSecond:", (IMP)shim_CADisplayLink_setPreferredFPS);
    add_instance_method(dispLink, "invalidate", (IMP)shim_CADisplayLink_invalidate);

    Class audioSess = create_shim_class("AVAudioSession", nsObject);
    add_class_method(audioSess, "sharedInstance", (IMP)shim_AVAudioSession_sharedInstance);
    add_instance_method(audioSess, "sampleRate", (IMP)shim_AVAudioSession_sampleRate);
    add_instance_method(audioSess, "IOBufferDuration", (IMP)shim_AVAudioSession_IOBufferDuration);
    add_instance_method(audioSess, "outputLatency", (IMP)shim_AVAudioSession_outputLatency);
    add_instance_method(audioSess, "setActive:error:", (IMP)shim_AVAudioSession_retYES);
    add_instance_method(audioSess, "setPreferredIOBufferDuration:error:", (IMP)shim_AVAudioSession_retYES);
    add_instance_method(audioSess, "setCategory:withOptions:error:", (IMP)shim_AVAudioSession_retYES);

    Class gcCtrl = create_shim_class("GCController", nsObject);
    add_class_method(gcCtrl, "controllers", (IMP)shim_GCController_controllers);

    Class gkPlayer = create_shim_class("GKLocalPlayer", nsObject);
    add_class_method(gkPlayer, "localPlayer", (IMP)shim_GKLocalPlayer_localPlayer);
    add_instance_method(gkPlayer, "isAuthenticated", (IMP)shim_GKLocalPlayer_isAuthenticated);
    add_instance_method(gkPlayer, "playerID", (IMP)shim_GKLocalPlayer_playerID);
    add_instance_method(gkPlayer, "authenticateWithCompletionHandler:", (IMP)shim_GKLocalPlayer_authenticateWithCompletionHandler);
    add_instance_method(gkPlayer, "setAuthenticateHandler:", (IMP)shim_GKLocalPlayer_setAuthenticateHandler);

    create_shim_class("GKAchievement", nsObject);
}

extern "C" {

SEL sel_registerName(const char *name) {
    if (!name) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_selector_table.find(name);
    if (it != g_selector_table.end()) {
        return reinterpret_cast<SEL>(it->second);
    }
    char *copy = strdup(name);
    g_selector_table[name] = copy;
    return reinterpret_cast<SEL>(copy);
}

SEL sel_getUid(const char *name) {
    return sel_registerName(name);
}

const char *sel_getName(SEL sel) {
    return sel ? reinterpret_cast<const char *>(sel) : "";
}

Class objc_getClass(const char *name) {
    if (!name) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();
    auto it = g_class_table.find(name);
    if (it != g_class_table.end()) {
        return it->second;
    }
    return create_shim_class(name, g_class_table["NSObject"]);
}

Class objc_getMetaClass(const char *name) {
    if (!name) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();
    Class cls = objc_getClass(name);
    return cls ? cls->isa : nullptr;
}

Class objc_getRequiredClass(const char *name) {
    return objc_getClass(name);
}

Class objc_lookUpClass(const char *name) {
    if (!name) return nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();
    auto it = g_class_table.find(name);
    return (it != g_class_table.end()) ? it->second : nullptr;
}

Class objc_allocateClassPair(Class superclass, const char *name, size_t extraBytes) {
    (void)extraBytes;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();
    return create_shim_class(name ? name : "UnnamedClass", superclass);
}

void objc_registerClassPair(Class cls) {
    (void)cls;
}

void objc_initializeClassPair(Class superclass, const char *name, Class cls, Class metacls) {
    (void)superclass; (void)name; (void)cls; (void)metacls;
}

Class objc_readClassPair(Class cls, const void *info) {
    (void)info;
    return cls;
}

const char **objc_copyClassNamesForImage(const void *image, unsigned int *outCount) {
    (void)image;
    if (outCount) *outCount = 0;
    return nullptr;
}

void *objc_getProtocol(const char *name) {
    (void)name;
    return nullptr;
}

void *objc_constructInstance(Class cls, void *bytes) {
    if (!cls || !bytes) return nullptr;
    size_t sz = (cls->ro && cls->ro->instanceSize > 8) ? cls->ro->instanceSize : 64;
    memset(bytes, 0, sz);
    *(Class *)bytes = cls;
    return bytes;
}

void objc_setProperty_nonatomic(id self, SEL _cmd, id newValue, ptrdiff_t offset) {
    (void)_cmd;
    if (!self) return;
    id *slot = (id *)((uint8_t *)self + offset);
    *slot = newValue;
}

const char *class_getName(Class cls) {
    if (!cls) return "nil";
    if (cls->ro && cls->ro->name) return cls->ro->name;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_class_info.find(cls);
    return (it != g_class_info.end()) ? it->second->name.c_str() : "Unknown";
}

Class class_getSuperclass(Class cls) {
    return cls ? cls->superclass : nullptr;
}

BOOL class_isMetaClass(Class cls) {
    if (!cls) return NO;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_class_info.find(cls);
    return (it != g_class_info.end() && it->second->is_metaclass) ? YES : NO;
}

BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types) {
    (void)types;
    if (!cls || !name || !imp) return NO;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    add_instance_method(cls, sel_getName(name), imp);
    return YES;
}

IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types) {
    class_addMethod(cls, name, imp, types);
    return imp;
}

Method class_getInstanceMethod(Class cls, SEL name) {
    (void)cls; (void)name;
    return nullptr;
}

Ivar class_getInstanceVariable(Class cls, const char *name) {
    (void)cls; (void)name;
    return nullptr;
}

BOOL class_addProperty(Class cls, const char *name, const void *attributes, unsigned int attributeCount) {
    (void)cls; (void)name; (void)attributes; (void)attributeCount;
    return YES;
}

BOOL class_addProtocol(Class cls, void *protocol) {
    (void)cls; (void)protocol;
    return YES;
}

IMP method_setImplementation(Method m, IMP imp) {
    (void)m;
    return imp;
}

Class object_getClass(id obj) {
    if (!obj) return Nil;
    return *(Class *)obj;
}

void *object_getIvar(id obj, Ivar ivar) {
    (void)obj; (void)ivar;
    return nullptr;
}

void *object_getIndexedIvars(id obj) {
    return obj ? (void *)((uint8_t *)obj + sizeof(Class)) : nullptr;
}

void *property_copyAttributeList(void *property, unsigned int *outCount) {
    (void)property;
    if (outCount) *outCount = 0;
    return nullptr;
}

const char *protocol_getName(void *p) {
    (void)p;
    return "";
}

struct objc_method_description_t protocol_getMethodDescription(void *p, SEL aSel, BOOL isRequiredMethod, BOOL isInstanceMethod) {
    (void)p; (void)aSel; (void)isRequiredMethod; (void)isInstanceMethod;
    return { nullptr, nullptr };
}

id objc_alloc(Class cls) {
    if (!cls) return nil;
    size_t sz = 64;
    if (cls->ro && cls->ro->instanceSize > sz) {
        sz = cls->ro->instanceSize;
    }
    void *obj = calloc(1, sz);
    *(Class *)obj = cls;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    g_refcounts[(id)obj] = 1;
    return (id)obj;
}

id objc_retain(id obj) {
    if (!obj) return nil;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_refcounts.find(obj);
    if (it != g_refcounts.end()) it->second++;
    return obj;
}

void objc_release(id obj) {
    if (!obj) return;
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    auto it = g_refcounts.find(obj);
    if (it != g_refcounts.end() && --(it->second) <= 0) {
        g_refcounts.erase(it);
        g_extra_data.erase(obj);
        free(obj);
    }
}

id objc_autorelease(id obj) {
    return obj;
}

// ============================================================================
// Method Lookup (called by objc_msgSend / objc_msgSendSuper2 ASM trampolines)
// ============================================================================

static IMP lookup_in_hierarchy(Class cls, const char *sel_str, bool class_method) {
    while (cls) {
        auto it = g_class_info.find(cls);
        if (it != g_class_info.end()) {
            auto &tbl = class_method ? it->second->class_methods : it->second->instance_methods;
            auto mit = tbl.find(sel_str);
            if (mit != tbl.end()) {
                return mit->second;
            }
        }
        cls = cls->superclass;
    }
    return nullptr;
}

static std::unordered_set<std::string> g_logged_unhandled;

IMP objc_lookup_imp_for_send(id self, SEL op, void *caller_lr) {
    if (!self || !op) return nullptr;
    const char *sel_str = sel_getName(op);
    if ((uintptr_t)self < 0x100000ULL) {
        LOGE("BAD ObjC self=%p sel='%s' caller=%p", self, sel_str ? sel_str : "?", caller_lr);
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();

    bool is_cls = (g_known_classes.find(self) != g_known_classes.end());
    if (is_cls) {
        IMP imp = lookup_in_hierarchy((Class)self, sel_str, true);
        if (imp) return imp;
        imp = lookup_in_hierarchy(g_class_table["NSObject"], sel_str, true);
        if (!imp) {
            auto *rc = (Class)self;
            std::string key = std::string("+[") + (rc->ro && rc->ro->name ? rc->ro->name : "?") + " " + (sel_str ? sel_str : "?") + "]";
            if (g_logged_unhandled.insert(key).second) {
                LOGE("UNHANDLED ObjC class msg: %s (caller=%p)", key.c_str(), caller_lr);
            }
        }
        return imp;
    }

    Class cls = *(Class *)self;
    if ((uintptr_t)cls < 0x100000ULL && cls != nullptr) {
        LOGE("CORRUPT ObjC isa=%p on self=%p sel='%s' caller=%p", cls, self, sel_str ? sel_str : "?", caller_lr);
        return nullptr;
    }
    if ((void *)cls == (void *)___CFConstantStringClassReference || cls == nullptr) {
        cls = g_class_table["NSString"];
    }
    IMP imp = lookup_in_hierarchy(cls, sel_str, false);
    if (!imp) {
        std::string key = std::string("-[") + (cls && cls->ro && cls->ro->name ? cls->ro->name : "?") + " " + (sel_str ? sel_str : "?") + "]";
        if (g_logged_unhandled.insert(key).second) {
            LOGE("UNHANDLED ObjC inst msg: %s (caller=%p)", key.c_str(), caller_lr);
        }
    }
    return imp;
}

IMP objc_lookup_imp_for_super2(struct objc_super2 *super_struct, SEL op) {
    if (!super_struct || !super_struct->receiver || !op) return nullptr;
    const char *sel_str = sel_getName(op);
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();

    Class start_cls = super_struct->current_class ? super_struct->current_class->superclass : nullptr;
    bool is_cls = (g_known_classes.find(super_struct->receiver) != g_known_classes.end());
    IMP imp = lookup_in_hierarchy(start_cls, sel_str, is_cls);
    if (!imp) {
        std::string key = std::string("super[") + (start_cls && start_cls->ro && start_cls->ro->name ? start_cls->ro->name : "?") + " " + (sel_str ? sel_str : "?") + "]";
        if (g_logged_unhandled.insert(key).second) {
            LOGE("UNHANDLED ObjC super msg: %s", key.c_str());
        }
    }
    return imp;
}

void objc_register_macho_classes(uintptr_t classlist_va, size_t count) {
    std::lock_guard<std::recursive_mutex> lock(g_objc_mutex);
    init_builtin_classes();

    struct macho_method_list_t {
        uint32_t entsize;
        uint32_t count;
    };
    struct macho_method_t {
        const char *name;
        const char *types;
        IMP imp;
    };

    Class *classes = (Class *)classlist_va;
    for (size_t i = 0; i < count; i++) {
        Class cls = classes[i];
        if (!cls || !cls->ro || !cls->ro->name) continue;
        const char *cname = cls->ro->name;

        auto *info = new ClassRuntimeInfo();
        info->name = cname;
        info->is_metaclass = false;
        info->instance_size = cls->ro->instanceSize > 0 ? cls->ro->instanceSize : 8;

        if (cls->ro->baseMethods) {
            const auto *ml = (const macho_method_list_t *)cls->ro->baseMethods;
            const auto *entries = (const macho_method_t *)((const uint8_t *)ml + sizeof(macho_method_list_t));
            for (uint32_t m = 0; m < ml->count; m++) {
                if (entries[m].name && entries[m].imp) {
                    info->instance_methods[entries[m].name] = entries[m].imp;
                }
            }
        }

        g_class_table[cname] = cls;
        g_class_info[cls] = info;
        g_known_classes.insert(cls);

        if (cls->isa) {
            auto *metainfo = new ClassRuntimeInfo();
            metainfo->name = cname;
            metainfo->is_metaclass = true;
            metainfo->instance_size = sizeof(objc_class);
            if (cls->isa->ro && cls->isa->ro->baseMethods) {
                const auto *ml = (const macho_method_list_t *)cls->isa->ro->baseMethods;
                const auto *entries = (const macho_method_t *)((const uint8_t *)ml + sizeof(macho_method_list_t));
                for (uint32_t m = 0; m < ml->count; m++) {
                    if (entries[m].name && entries[m].imp) {
                        metainfo->instance_methods[entries[m].name] = entries[m].imp;
                        info->class_methods[entries[m].name] = entries[m].imp;
                    }
                }
            }
            g_metaclass_table[cname] = cls->isa;
            g_class_info[cls->isa] = metainfo;
            g_known_classes.insert(cls->isa);
        }

        LOGI("Registered Mach-O ObjC class '%s' @ %p (%zu methods, super=%p)",
             cname, cls, info->instance_methods.size(), cls->superclass);
    }
}

// UIApplicationMain entry point
int _UIApplicationMain(int argc, char *argv[], void *principalClassName, void *delegateClassName) {
    (void)argc; (void)argv; (void)principalClassName;
    LOGI("UIApplicationMain called. Initializing FTL SIL runtime...");

    const char *del_name = delegateClassName ? nsstring_get_utf8((id)delegateClassName) : "SILAppDelegate";
    if (!del_name || del_name[0] == '\0') del_name = "SILAppDelegate";

    Class appDelCls = objc_getClass(del_name);
    g_app_delegate = objc_alloc(appDelCls);
    objc_msgSend(g_app_delegate, sel_registerName("init"));

    id app = shim_UIApplication_sharedApplication(nil, nullptr);
    SEL launchSel = sel_registerName("application:didFinishLaunchingWithOptions:");
    LOGI("Invoking -[%s application:didFinishLaunchingWithOptions:] on %p...", del_name, g_app_delegate);
    objc_msgSend(g_app_delegate, launchSel, app, nil);

    LOGI("SILAppDelegate launch sequence completed. Entering run loop wait...");
    while (true) {
        sleep(1);
    }
    return 0;
}

void *NSClassFromString(void *aClassName) {
    if (!aClassName) return nullptr;
    const char *name = nsstring_get_utf8((id)aClassName);
    return (void *)objc_getClass(name);
}

}
