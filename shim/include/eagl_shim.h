#pragma once

#include <cstdint>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kEAGLRenderingAPIOpenGLES1 = 1,
    kEAGLRenderingAPIOpenGLES2 = 2,
    kEAGLRenderingAPIOpenGLES3 = 3
} EAGLRenderingAPI;

struct EAGLContext_t {
    EAGLRenderingAPI api;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    GLuint defaultFBO;
    GLuint colorRenderbuffer;
    GLuint depthRenderbuffer;
    int width;
    int height;
};

typedef struct EAGLContext_t *EAGLContextRef;

// EAGL Context API Shims
EAGLContextRef eagl_create_context(EAGLRenderingAPI api);
int eagl_set_current_context(EAGLContextRef ctx);
EAGLContextRef eagl_get_current_context(void);
int eagl_present_renderbuffer(EAGLContextRef ctx, GLenum target);
int eagl_renderbuffer_storage_from_drawable(EAGLContextRef ctx, GLenum target, void *drawable);

// Global EGL lifecycle management for Quest 3
int quest_egl_init(void *native_window, int width, int height);
int quest_egl_update_surface(void *native_window, int width, int height);
void quest_egl_surface_destroyed(void);
void quest_egl_swap_buffers(void);
void quest_egl_destroy(void);
void eagl_get_surface_size(int *width, int *height);
EGLConfig quest_egl_get_config(void);
void quest_xr_set_java(void *vm, void *activity);

#ifdef __cplusplus
}
#endif
