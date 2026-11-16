#include "eagl_shim.h"
#include "uikit_shim.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <android/log.h>
#include <android/native_window.h>

// 5-plane diorama hooks (darwin_abi.cpp)
extern "C" void darwin_set_main_fbo(uint32_t fbo, int w, int h);
extern "C" void darwin_planes_request(bool enabled);
extern "C" void darwin_planes_frame_boundary();
extern "C" bool darwin_planes_enabled();
extern "C" void darwin_dump_planes(const char *dir, int frame);
extern "C" void darwin_trace_request();
#if defined(FTL_ENABLE_OPENXR)
extern "C" int quest_xr_present(void);
#endif

#define TAG "FTL_EAGL_SHIM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static struct EAGLContext_t g_current_eagl_context = {};
static __thread EAGLContextRef g_tls_eagl_context = nullptr;

static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLConfig g_egl_config = nullptr;
static ANativeWindow *g_active_window = nullptr;
static ANativeWindow *g_pending_window = nullptr;
static std::mutex g_surface_mutex;
static std::atomic<bool> g_has_pending_window{false};
static std::atomic<bool> g_surface_destroyed{false};

extern "C" {

EGLConfig quest_egl_get_config(void) { return g_egl_config; }

int quest_egl_init(void *native_window, int width, int height) {
#if defined(FTL_ENABLE_OPENXR)
    // Immersive: the Android surface is never shown; keep FTL at its tuned 1280x800 layout.
    width = 1280; height = 800;
#endif
    LOGI("Initializing EGL for Quest 3 / Android (target: %dx%d)...", width, height);
    
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        LOGE("Failed to initialize EGL");
        return -1;
    }
    LOGI("EGL initialized: version %d.%d", major, minor);

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(g_egl_display, attribs, &config, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("Failed to choose EGL config");
        return -1;
    }
    g_egl_config = config;

    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, (const EGLint[]){
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    });
    if (g_egl_context == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL GLES3 context");
        return -1;
    }

    g_active_window = (ANativeWindow *)native_window;
    if (native_window) {
        g_egl_surface = eglCreateWindowSurface(g_egl_display, config, (EGLNativeWindowType)native_window, nullptr);
        g_surface_destroyed.store(false);
    } else {
        const EGLint pbuf_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        g_egl_surface = eglCreatePbufferSurface(g_egl_display, config, pbuf_attribs);
        g_surface_destroyed.store(true);
    }
    if (g_egl_surface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL surface");
        return -1;
    }

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        LOGE("Failed to make EGL context current");
        return -1;
    }

    g_current_eagl_context.api = kEAGLRenderingAPIOpenGLES3;
    g_current_eagl_context.display = g_egl_display;
    g_current_eagl_context.context = g_egl_context;
    g_current_eagl_context.surface = g_egl_surface;
    g_current_eagl_context.width = width;
    g_current_eagl_context.height = height;
    g_current_eagl_context.defaultFBO = 0; // Android default surface FBO is 0

    // Release context from the Java UI thread so FTL's engine/render thread can bind it
    g_tls_eagl_context = nullptr;
    g_surface_destroyed.store(false);
    g_has_pending_window.store(false);
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    LOGI("EGL initialization complete.");
    return 0;
}

int quest_egl_update_surface(void *native_window, int width, int height) {
#if defined(FTL_ENABLE_OPENXR)
    width = 1280; height = 800;
#endif
    LOGI("Updating EGL window surface (%dx%d, win=%p)", width, height, native_window);
    std::lock_guard<std::mutex> lock(g_surface_mutex);
    if (g_pending_window && g_pending_window != (ANativeWindow *)native_window) {
        ANativeWindow_release(g_pending_window);
    }
    g_pending_window = (ANativeWindow *)native_window;
    g_has_pending_window.store(true);
    g_surface_destroyed.store(false);
    return 0;
}

void quest_egl_surface_destroyed(void) {
    LOGI("EGL surface destroyed notification received.");
    g_surface_destroyed.store(true);
}

void quest_egl_swap_buffers(void) {
    if (g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE) {
        eglSwapBuffers(g_egl_display, g_egl_surface);
    }
}

void quest_egl_destroy(void) {
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_context != EGL_NO_CONTEXT) eglDestroyContext(g_egl_display, g_egl_context);
        if (g_egl_surface != EGL_NO_SURFACE) eglDestroySurface(g_egl_display, g_egl_surface);
        eglTerminate(g_egl_display);
    }
    g_egl_display = EGL_NO_DISPLAY;
    g_egl_context = EGL_NO_CONTEXT;
    g_egl_surface = EGL_NO_SURFACE;
}

EAGLContextRef eagl_create_context(EAGLRenderingAPI api) {
    LOGI("eagl_create_context(api=%d)", (int)api);
    EAGLContextRef ctx = (EAGLContextRef)calloc(1, sizeof(struct EAGLContext_t));
    ctx->api = api;
    ctx->display = g_egl_display;
    ctx->context = g_egl_context;
    ctx->surface = g_egl_surface;
    ctx->width = g_current_eagl_context.width;
    ctx->height = g_current_eagl_context.height;
    ctx->defaultFBO = 0;
    return ctx;
}

int eagl_set_current_context(EAGLContextRef ctx) {
    g_tls_eagl_context = ctx;
    if (ctx && ctx->display != EGL_NO_DISPLAY && ctx->context != EGL_NO_CONTEXT) {
        EGLBoolean ok = eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context);
        LOGI("eagl_set_current_context(%p) -> %d (err=0x%x)", ctx, (int)ok, ok ? 0 : eglGetError());
        return ok ? 1 : 0;
    }
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    return 1;
}

EAGLContextRef eagl_get_current_context(void) {
    return g_tls_eagl_context;
}

void eagl_get_surface_size(int *width, int *height) {
    if (width) *width = (g_current_eagl_context.width > 0) ? g_current_eagl_context.width : 1280;
    if (height) *height = (g_current_eagl_context.height > 0) ? g_current_eagl_context.height : 800;
}

static int g_swap_count = 0;

int eagl_present_renderbuffer(EAGLContextRef ctx, GLenum target) {
    (void)target;
    if (!ctx) ctx = g_tls_eagl_context;

    if (g_has_pending_window.load()) {
        std::lock_guard<std::mutex> lock(g_surface_mutex);
        if (g_has_pending_window.exchange(false) && g_pending_window && g_egl_display != EGL_NO_DISPLAY && g_egl_context != EGL_NO_CONTEXT) {
            eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (g_egl_surface != EGL_NO_SURFACE) {
                eglDestroySurface(g_egl_display, g_egl_surface);
                g_egl_surface = EGL_NO_SURFACE;
            }
            if (g_active_window) {
                ANativeWindow_release(g_active_window);
                g_active_window = nullptr;
            }
            g_active_window = g_pending_window;
            g_pending_window = nullptr;
            g_egl_surface = eglCreateWindowSurface(g_egl_display, g_egl_config, (EGLNativeWindowType)g_active_window, nullptr);
            if (g_egl_surface != EGL_NO_SURFACE) {
                eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
                g_current_eagl_context.surface = g_egl_surface;
                if (ctx) ctx->surface = g_egl_surface;
                LOGI("Swapped in new EGL window surface %p on GL thread.", g_egl_surface);
            } else {
                LOGE("Failed to recreate EGLSurface for new window %p", g_active_window);
            }
        }
    }

    static int s_pending_tap_up = 0;
    static float s_tap_x = 0.0f, s_tap_y = 0.0f;
    if (s_pending_tap_up > 0) {
        s_pending_tap_up--;
        if (s_pending_tap_up == 0) {
            LOGI("Injecting synthetic touch UP at (%.1f, %.1f)", s_tap_x, s_tap_y);
            quest_inject_touch(0, s_tap_x, s_tap_y, UITouchPhaseEnded);
        }
    } else if ((g_swap_count % 10) == 0) {
        const char *tap_path = "/sdcard/Android/data/com.subsetgames.ftl/files/.tap_cmd";
        if (access(tap_path, F_OK) == 0) {
            FILE *tf = fopen(tap_path, "r");
            if (tf) {
                if (fscanf(tf, "%f %f", &s_tap_x, &s_tap_y) == 2) {
                    LOGI("Injecting synthetic touch DOWN at (%.1f, %.1f)", s_tap_x, s_tap_y);
                    quest_inject_touch(0, s_tap_x, s_tap_y, UITouchPhaseBegan);
                    s_pending_tap_up = 4;
                }
                fclose(tf);
            }
            unlink(tap_path);
        }
    }

    GLenum err_blit = GL_NO_ERROR;
    uint8_t px_center[4] = {0, 0, 0, 0};
    int non_black = 0;
    const char *trigger_path = "/sdcard/Android/data/com.subsetgames.ftl/files/.dump_frame";
    bool dump_now = (access(trigger_path, F_OK) == 0);
    bool sample = (g_swap_count < 3 || g_swap_count == 60 || dump_now);

    if (ctx && ctx->defaultFBO != 0) {
        int w = (ctx->width > 0) ? ctx->width : g_current_eagl_context.width;
        int h = (ctx->height > 0) ? ctx->height : g_current_eagl_context.height;
        if (w <= 0) w = 1280;
        if (h <= 0) h = 800;

        GLint prev_draw_fbo = 0;
        GLint prev_read_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->defaultFBO);
        if (sample) {
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px_center);
            uint8_t row[1280 * 4];
            if (w <= 1280) {
                glReadPixels(0, h / 2, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
                for (int i = 0; i < w * 4; i += 4) {
                    if (row[i] || row[i + 1] || row[i + 2]) non_black++;
                }
            }
            if (dump_now) {
                unlink(trigger_path);
                uint8_t *img = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
                if (img) {
                    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    const char *out_path = "/sdcard/Android/data/com.subsetgames.ftl/files/latest.ppm";
                    FILE *fp = fopen(out_path, "wb");
                    if (fp) {
                        fprintf(fp, "P6\n%d %d\n255\n", w, h);
                        for (int y = h - 1; y >= 0; y--) {
                            for (int x = 0; x < w; x++) {
                                fwrite(&img[(y * w + x) * 4], 1, 3, fp);
                            }
                        }
                        fclose(fp);
                        LOGI("Dumped on-demand screenshot %s (frame=%d %dx%d)", out_path, g_swap_count, w, h);
                    }
                    free(img);
                }
                if (darwin_planes_enabled()) {
                    darwin_dump_planes("/sdcard/Android/data/com.subsetgames.ftl/files", g_swap_count);
                }
            }
        }
#if defined(FTL_ENABLE_OPENXR)
        if (quest_xr_present()) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
            if ((g_swap_count % 30) == 0) {
                const char *trace_path = "/sdcard/Android/data/com.subsetgames.ftl/files/.trace_frame";
                if (access(trace_path, F_OK) == 0) { unlink(trace_path); darwin_trace_request(); }
            }
            darwin_planes_frame_boundary();
            g_swap_count++;
            return 1;
        }
#endif
        if (!g_surface_destroyed.load() && g_egl_surface != EGL_NO_SURFACE) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            err_blit = glGetError();
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
    }

    if (g_surface_destroyed.load() || g_egl_surface == EGL_NO_SURFACE) {
        g_swap_count++;
        usleep(16000);
        return 1;
    }
    if (sample) {
        LOGI("eagl_present_renderbuffer frame %d (fbo=%u %dx%d blitErr=0x%x centerRGBA=%u,%u,%u,%u midRowNonBlack=%d)",
             g_swap_count, ctx ? ctx->defaultFBO : 0,
             ctx ? ctx->width : 0, ctx ? ctx->height : 0, (unsigned)err_blit,
             px_center[0], px_center[1], px_center[2], px_center[3], non_black);
    }
    if ((g_swap_count % 30) == 0) {
        darwin_planes_request(access("/sdcard/Android/data/com.subsetgames.ftl/files/.planes", F_OK) == 0);
        const char *trace_path = "/sdcard/Android/data/com.subsetgames.ftl/files/.trace_frame";
        if (access(trace_path, F_OK) == 0) { unlink(trace_path); darwin_trace_request(); }
    }
    darwin_planes_frame_boundary();
    g_swap_count++;
    quest_egl_swap_buffers();
    return 1;
}

int eagl_renderbuffer_storage_from_drawable(EAGLContextRef ctx, GLenum target, void *drawable) {
    (void)drawable;
    int w = (ctx && ctx->width > 0) ? ctx->width : g_current_eagl_context.width;
    int h = (ctx && ctx->height > 0) ? ctx->height : g_current_eagl_context.height;
    if (w <= 0) w = 1280;
    if (h <= 0) h = 800;

    GLint bound_fbo = 0;
    GLint bound_rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound_rb);
    if (ctx) {
        ctx->defaultFBO = (GLuint)bound_fbo;
        ctx->colorRenderbuffer = (GLuint)bound_rb;
        ctx->width = w;
        ctx->height = h;
    }

    glRenderbufferStorage(target, GL_RGBA8, w, h);
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) darwin_set_main_fbo((uint32_t)bound_fbo, w, h);
    LOGI("eagl_renderbuffer_storage_from_drawable(target=0x%x, %dx%d, fbo=%d, rb=%d) err=0x%x",
         (unsigned)target, w, h, bound_fbo, bound_rb, (unsigned)err);
    return (err == GL_NO_ERROR) ? 1 : 0;
}

}
