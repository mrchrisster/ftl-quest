#include <cstdio>
#include <cstdlib>
#include <android/log.h>

#define TAG "FTL_OPENAL_SHIM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

extern "C" {

// OpenAL device & context lifecycle stubs / OpenAL-Soft bridge hooks
void *quest_alc_open_device(const char *devicename) {
    LOGI("alcOpenDevice: %s", devicename ? devicename : "(default)");
    return (void *)0x1000;
}

void *quest_alc_create_context(void *device, const int *attrlist) {
    LOGI("alcCreateContext");
    return (void *)0x2000;
}

int quest_alc_make_context_current(void *context) {
    return 1;
}

void quest_alc_destroy_context(void *context) {
}

int quest_alc_close_device(void *device) {
    return 1;
}

}
