#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *id;
typedef const struct objc_selector *SEL;
typedef struct objc_class *Class;
typedef struct objc_method *Method;
typedef void *Ivar;
typedef signed char BOOL;

#define YES ((BOOL)1)
#define NO  ((BOOL)0)
#define nil ((id)0)
#define Nil ((Class)0)

typedef id (*IMP)(id, SEL, ...);

struct objc_class_ro {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    uint32_t reserved;
    uint64_t ivarLayout;
    const char *name;
    const void *baseMethods;
    const void *baseProtocols;
    const void *ivars;
    const void *weakIvarLayout;
    const void *baseProperties;
};

struct objc_class {
    Class isa;
    Class superclass;
    void *cache;
    void *vtable;
    struct objc_class_ro *ro;
};

struct objc_super2 {
    id receiver;
    Class current_class;
};

extern void *_objc_empty_cache[2];
extern void *_objc_empty_vtable[2];

// Objective-C runtime core functions
SEL sel_registerName(const char *name);
SEL sel_getUid(const char *name);
const char *sel_getName(SEL sel);
Class objc_getClass(const char *name);
Class objc_getMetaClass(const char *name);
Class objc_getRequiredClass(const char *name);
Class objc_lookUpClass(const char *name);
Class objc_allocateClassPair(Class superclass, const char *name, size_t extraBytes);
void objc_registerClassPair(Class cls);
void objc_initializeClassPair(Class superclass, const char *name, Class cls, Class metacls);
Class objc_readClassPair(Class cls, const void *info);
const char **objc_copyClassNamesForImage(const void *image, unsigned int *outCount);
void *objc_getProtocol(const char *name);
void *objc_constructInstance(Class cls, void *bytes);
void objc_setProperty_nonatomic(id self, SEL _cmd, id newValue, ptrdiff_t offset);

const char *class_getName(Class cls);
Class class_getSuperclass(Class cls);
BOOL class_isMetaClass(Class cls);
BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types);
IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types);
Method class_getInstanceMethod(Class cls, SEL name);
Ivar class_getInstanceVariable(Class cls, const char *name);
BOOL class_addProperty(Class cls, const char *name, const void *attributes, unsigned int attributeCount);
BOOL class_addProtocol(Class cls, void *protocol);
IMP method_setImplementation(Method m, IMP imp);

Class object_getClass(id obj);
void *object_getIvar(id obj, Ivar ivar);
void *object_getIndexedIvars(id obj);
void *property_copyAttributeList(void *property, unsigned int *outCount);
const char *protocol_getName(void *p);
struct objc_method_description_t { SEL name; char *types; };
struct objc_method_description_t protocol_getMethodDescription(void *p, SEL aSel, BOOL isRequiredMethod, BOOL isInstanceMethod);

// Objective-C Message Dispatcher (ARM64 assembly trampoline + C++ lookup)
id objc_msgSend(id self, SEL op, ...);
id objc_msgSendSuper2(struct objc_super2 *super_struct, SEL op, ...);
IMP objc_lookup_imp_for_send(id self, SEL op, void *caller_lr = nullptr);
IMP objc_lookup_imp_for_super2(struct objc_super2 *super_struct, SEL op);

// Object creation and memory management
id objc_alloc(Class cls);
id objc_retain(id obj);
void objc_release(id obj);
id objc_autorelease(id obj);

// Foundation / UIKit bridge helpers
void *NSClassFromString(void *aClassName);
int _UIApplicationMain(int argc, char *argv[], void *principalClassName, void *delegateClassName);

// Register Mach-O __DATA,__objc_classlist classes and methods into the runtime
void objc_register_macho_classes(uintptr_t classlist_va, size_t count);

// Apple framework constant objects
extern void *kEAGLColorFormatRGBA8;
extern void *kEAGLDrawablePropertyColorFormat;
extern void *kEAGLDrawablePropertyRetainedBacking;
extern void *AVAudioSessionCategoryAmbient;
extern void *AVAudioSessionCategoryPlayback;

#ifdef __cplusplus
}
#endif
