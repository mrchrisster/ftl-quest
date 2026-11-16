#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include "eagl_shim.h"
#include "darwin_abi.h"
#include "uikit_shim.h"
#include "macho_loader.h"

#define TAG "FTL_QUEST_ENTRY"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static MachOLoadInfo g_macho_info = {};
static bool g_engine_booted = false;

static void *engine_bootstrap_thread(void *arg) {
    LOGI("Engine bootstrap thread started.");
    char *argv[] = { (char *)"FTL", nullptr };
    int ret = macho_loader_start_engine(&g_macho_info, 1, argv);
    LOGI("Mach-O engine exited with code: %d", ret);
    return nullptr;
}

extern "C" {

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeInit(
    JNIEnv *env,
    jobject thiz,
    jobject surface,
    jstring internalPath,
    jstring externalPath
) {
    const char *internal_str = env->GetStringUTFChars(internalPath, nullptr);
    const char *external_str = env->GetStringUTFChars(externalPath, nullptr);

    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    int width = window ? ANativeWindow_getWidth(window) : 1280;
    int height = window ? ANativeWindow_getHeight(window) : 800;

    if (g_engine_booted) {
        if (window) {
            LOGI("nativeInit: engine already running; refreshing EGL surface (%dx%d).", width, height);
            quest_egl_update_surface(window, width, height);
        }
        env->ReleaseStringUTFChars(internalPath, internal_str);
        env->ReleaseStringUTFChars(externalPath, external_str);
        return;
    }

    LOGI("=================================================");
    LOGI(" Bootstrapping FTL for Meta Quest 3 (Horizon OS) ");
    LOGI("=================================================");
    LOGI(" Internal Storage: %s", internal_str);
    LOGI(" External Storage: %s", external_str);

    darwin_vfs_init(internal_str, external_str);

    if (quest_egl_init(window, width, height) != 0) {
        LOGE("Failed to initialize Quest EGL display/surface pipeline!");
        env->ReleaseStringUTFChars(internalPath, internal_str);
        env->ReleaseStringUTFChars(externalPath, external_str);
        return;
    }
    LOGI("Quest EGL pipeline active (%dx%d).", width, height);

    char macho_path[1024];
    snprintf(macho_path, sizeof(macho_path), "%s/FTL", external_str);
    if (access(macho_path, F_OK) != 0) {
        snprintf(macho_path, sizeof(macho_path), "%s/FTL", internal_str);
    }

    LOGI("Target Mach-O binary path: %s", macho_path);

    if (macho_loader_init(macho_path, &g_macho_info) == 0) {
        LOGI("Mach-O binary loaded and bound. Spawning engine thread...");
        g_engine_booted = true;
        pthread_t tid;
        pthread_create(&tid, nullptr, engine_bootstrap_thread, nullptr);
    } else {
        LOGE("Failed to load Mach-O binary from: %s", macho_path);
    }

    env->ReleaseStringUTFChars(internalPath, internal_str);
    env->ReleaseStringUTFChars(externalPath, external_str);
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeSetActivity(JNIEnv *env, jclass clazz, jobject activity) {
    (void)clazz;
#if defined(FTL_ENABLE_OPENXR)
    static bool s_done = false;
    if (s_done || !activity) return;
    s_done = true;
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);
    quest_xr_set_java(vm, env->NewGlobalRef(activity));
#else
    (void)env; (void)activity;
#endif
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeSurfaceDestroyed(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    quest_egl_surface_destroyed();
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeRenderFrame(JNIEnv *env, jobject thiz) {
    // Rendering and eglSwapBuffers are driven by FTL's GL thread via EAGLContext presentRenderbuffer:
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeTouch(
    JNIEnv *env,
    jobject thiz,
    jint pointer_id,
    jfloat x,
    jfloat y,
    jint phase
) {
    quest_inject_touch(pointer_id, x, y, (UITouchPhase)phase);
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeGamepadButton(
    JNIEnv *env,
    jobject thiz,
    jint button_code,
    jboolean pressed
) {
    quest_inject_button((QuestControllerButton)button_code, (bool)pressed);
}

JNIEXPORT void JNICALL
Java_io_github_mrchrisster_ftlquest_MainActivity_nativeGamepadAxis(
    JNIEnv *env,
    jobject thiz,
    jint stick_id,
    jfloat axis_x,
    jfloat axis_y
) {
    quest_inject_thumbstick(stick_id, axis_x, axis_y);
}

}
