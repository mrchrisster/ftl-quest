// quest_xr.cpp — immersive OpenXR presentation for FTL on Quest 3.
//
// Runs entirely on FTL's GL thread inside EAGL presentRenderbuffer: the game's
// EGL context is handed to xrCreateSession, and each frame the five diorama
// plane FBOs (darwin_abi.cpp) are blitted straight into five OpenXR quad
// swapchains stacked in depth (2.5D diorama, ported from the GBA recomp
// host_window_quest.cpp: kLayeredPlaneCount=5, flat / 6cm / 11cm depth).
//
// Controls (Touch controllers):
//   trigger (either hand)        laser pointer click -> quest_inject_touch (drag while held)
//   L+R thumbstick click chord   cycle depth: 2D FLAT -> 3D 6CM -> 3D DEEP 11CM
//   right thumbstick click       recenter screen in front of the head
//   right thumbstick up/down     power +/- on the system under the laser
//   A = pause, Y = door controls, left thumbstick click = pause
//   grip (either hand)           grab the screen and move it around the room (stick Y = push/pull)
//   X / Y / B, left stick tilts  forwarded to the existing shortcut handlers

#if defined(FTL_ENABLE_OPENXR)

#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_PLATFORM_ANDROID
#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <string>

#include "darwin_abi.h"
#include "uikit_shim.h"

#define TAG "FTL_XR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" EGLConfig quest_egl_get_config(void);

namespace {

constexpr unsigned kPlanes = 5;
constexpr float kTrigOn = 0.55f, kTrigOff = 0.35f;
constexpr int kCursorSize = 64;
// FTL's touch space = UIScreen bounds = the 1280x800 frame (confirmed in the headset: a 1152x720
// "correction" derived from .tap_cmd probes on the language screen put the laser off target).
constexpr float kTouchW = 1280.0f, kTouchH = 800.0f;
constexpr float kFrameToTouch = 1.0f;
constexpr int kLaserPointerId = 42;
constexpr int kButtonPointerId = 43;
// Fixed HUD buttons, in 1280x800 frame pixels (measured from frame dumps, Kestrel layout).
constexpr float kPauseX = 1075.0f, kPauseY = 752.0f;  // PAUSE button, bottom right
constexpr float kDoorsX = 946.0f, kDoorsY = 750.0f;   // door-control subsystem icon, bottom right

XrVector3f rot(const XrQuaternionf &q, float vx, float vy, float vz) {
    const float tx = 2.0f * (q.y * vz - q.z * vy);
    const float ty = 2.0f * (q.z * vx - q.x * vz);
    const float tz = 2.0f * (q.x * vy - q.y * vx);
    return {vx + q.w * tx + (q.y * tz - q.z * ty), vy + q.w * ty + (q.z * tx - q.x * tz),
            vz + q.w * tz + (q.x * ty - q.y * tx)};
}
XrVector3f add(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f scl(XrVector3f v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f cross(XrVector3f a, XrVector3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(XrVector3f v) { return std::sqrt(dot(v, v)); }
XrVector3f norm(XrVector3f v, XrVector3f fb = {0, 0, -1}) { float l = len(v); return l > 1e-5f ? scl(v, 1.0f / l) : fb; }

XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf qconj(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }

XrQuaternionf quat_from_basis(XrVector3f x, XrVector3f y, XrVector3f z) {
    const float m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    const float tr = m00 + m11 + m22;
    XrQuaternionf q{};
    if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s}; }
    else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1 + m00 - m11 - m22) * 2; q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s}; }
    else if (m11 > m22) { float s = std::sqrt(1 + m11 - m00 - m22) * 2; q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s}; }
    else { float s = std::sqrt(1 + m22 - m00 - m11) * 2; q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s}; }
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-6f ? XrQuaternionf{q.x / n, q.y / n, q.z / n, q.w / n} : XrQuaternionf{0, 0, 0, 1};
}

struct Swap {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> imgs;
    int w = 0, h = 0;
};

struct XrState {
    JavaVM *vm = nullptr;
    jobject activity = nullptr;
    bool init_tried = false, ok = false;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;

    Swap planes[kPlanes];
    Swap cursor, beam;
    GLuint dst_fbo = 0;

    int depth_mode = 1; // 0 flat, 1 = 6cm, 2 = 11cm
    XrPosef screen = {{0, 0, 0, 1}, {0, 0, -1.3f}};
    float distance = 1.3f, width_m = 1.7f;
    bool pose_init = false;
    int recenter_frames = 6;
    XrTime recenter_after = 0;

    XrActionSet set = XR_NULL_HANDLE;
    XrPath hands[2] = {};
    XrAction aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE,
             squeeze = XR_NULL_HANDLE, a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE,
             y = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    XrSpace aim_space[2] = {};

    int laser_hand = 1;
    bool trig_down[2] = {false, false};
    bool touching = false;
    bool hit_valid = false;
    int hit_plane = 2;
    XrVector3f hit_pos{}, ray_origin{};
    float hit_u = 0, hit_v = 0;
    bool prev_click[2] = {false, false}, prev_both = false, chord_used = false;
    bool prev_btn[6] = {};
    bool prev_grip[2] = {};
    // Grip grab: the screen follows the gripping controller rigidly while held.
    int grab_hand = -1;
    XrVector3f grab_rel_pos{};
    XrQuaternionf grab_rel_rot{0, 0, 0, 1};
};

XrState g;

// Timed synthetic touch gestures (one step per presented frame, with waits).
struct TouchStep { float x, y; UITouchPhase phase; int wait; };
std::vector<TouchStep> g_gesture;
void queue_tap_frame(float fx, float fy) {
    float tx = fx * kFrameToTouch, ty = fy * kFrameToTouch;
    g_gesture.push_back({tx, ty, UITouchPhaseBegan, 3});
    g_gesture.push_back({tx, ty, UITouchPhaseEnded, 2});
}
void queue_tap_touch(float tx, float ty) {
    g_gesture.push_back({tx, ty, UITouchPhaseBegan, 3});
    g_gesture.push_back({tx, ty, UITouchPhaseEnded, 2});
}
// FTL mobile: swipe down on a system removes power, swipe up adds it (a plain tap does not).
void queue_swipe_touch(float tx, float ty, float dir /* +1 down, -1 up */) {
    g_gesture.push_back({tx, ty, UITouchPhaseBegan, 2});
    for (int i = 1; i <= 6; i++) g_gesture.push_back({tx, ty + dir * i * 10.0f, UITouchPhaseMoved, 1});
    g_gesture.push_back({tx, ty + dir * 60.0f, UITouchPhaseEnded, 2});
}
void run_gestures() {
    static int s_wait = 0;
    if (s_wait > 0) { s_wait--; return; }
    if (g_gesture.empty()) return;
    TouchStep st = g_gesture.front();
    g_gesture.erase(g_gesture.begin());
    quest_inject_touch(kButtonPointerId, st.x, st.y, st.phase);
    s_wait = st.wait;
}

float depth_step(int m) { return m == 1 ? 0.06f : (m == 2 ? 0.11f : 0.0f); }

void haptic(int hand, float amp, XrDuration ns) {
    if (!g.haptic) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amp; v.duration = ns; v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = g.haptic; hi.subactionPath = g.hands[hand];
    xrApplyHapticFeedback(g.session, &hi, (const XrHapticBaseHeader *)&v);
}

bool make_swap(Swap &s, int w, int h) {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g.session, n, &n, fmts.data());
    // FTL's pixels are gamma-encoded (sRGB) bytes. Use an sRGB swapchain so the compositor decodes
    // them correctly, and copy the bytes raw (sRGB write encoding disabled in blit_planes): encoding
    // again made everything too bright and broke premultiplied alpha (rgb > a looked translucent).
    int64_t fmt = GL_SRGB8_ALPHA8;
    if (std::find(fmts.begin(), fmts.end(), fmt) == fmts.end()) {
        fmt = GL_RGBA8;
        if (std::find(fmts.begin(), fmts.end(), fmt) == fmts.end() && !fmts.empty()) fmt = fmts[0];
    }
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        std::string list;
        for (auto f : fmts) { char b[16]; snprintf(b, sizeof(b), "0x%llx ", (long long)f); list += b; }
        LOGI("swapchain formats: %s-> chose 0x%llx", list.c_str(), (long long)fmt);
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = (uint32_t)w; ci.height = (uint32_t)h;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g.session, &ci, &s.sc);
    if (XR_FAILED(r)) { LOGE("xrCreateSwapchain %dx%d failed %d", w, h, r); return false; }
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(s.sc, 0, &ic, nullptr);
    s.imgs.assign(ic, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(s.sc, ic, &ic, (XrSwapchainImageBaseHeader *)s.imgs.data());
    s.w = w; s.h = h;
    return true;
}

GLuint acquire(Swap &s) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.sc, &ai, &idx))) return 0;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(s.sc, &wi))) return 0;
    return s.imgs[idx].image;
}
void release(Swap &s) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.sc, &ri);
}

// Upload a premultiplied RGBA image once into a static swapchain (cursor / beam).
void fill_static(Swap &s, const std::vector<uint8_t> &rgba) {
    GLuint tex = acquire(s);
    if (tex) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        GLint ua = 4, rl = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &ua);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rl);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.w, s.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        glPixelStorei(GL_UNPACK_ALIGNMENT, ua);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rl);
    }
    release(s);
}

bool init_actions() {
    xrStringToPath(g.instance, "/user/hand/left", &g.hands[0]);
    xrStringToPath(g.instance, "/user/hand/right", &g.hands[1]);
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof(si.actionSetName), "ftl");
    snprintf(si.localizedActionSetName, sizeof(si.localizedActionSetName), "FTL Controls");
    if (XR_FAILED(xrCreateActionSet(g.instance, &si, &g.set))) return false;
    auto mk = [&](const char *n, XrActionType t, bool per_hand, XrAction &out) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        snprintf(ai.actionName, sizeof(ai.actionName), "%s", n);
        snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", n);
        ai.actionType = t;
        ai.countSubactionPaths = per_hand ? 2 : 0;
        ai.subactionPaths = per_hand ? g.hands : nullptr;
        return XR_SUCCEEDED(xrCreateAction(g.set, &ai, &out));
    };
    if (!mk("aim", XR_ACTION_TYPE_POSE_INPUT, true, g.aim) || !mk("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true, g.trigger) ||
        !mk("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true, g.stick) ||
        !mk("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g.stick_click) ||
        !mk("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true, g.squeeze) || !mk("btn_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g.a) ||
        !mk("btn_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g.b) || !mk("btn_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g.x) ||
        !mk("btn_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g.y) || !mk("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false, g.menu) ||
        !mk("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true, g.haptic)) {
        LOGE("xrCreateAction failed");
        return false;
    }
    auto P = [&](const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(g.instance, s, &p); return p; };
    std::vector<XrActionSuggestedBinding> b = {
        {g.aim, P("/user/hand/left/input/aim/pose")}, {g.aim, P("/user/hand/right/input/aim/pose")},
        {g.trigger, P("/user/hand/left/input/trigger/value")}, {g.trigger, P("/user/hand/right/input/trigger/value")},
        {g.stick, P("/user/hand/left/input/thumbstick")}, {g.stick, P("/user/hand/right/input/thumbstick")},
        {g.stick_click, P("/user/hand/left/input/thumbstick/click")}, {g.stick_click, P("/user/hand/right/input/thumbstick/click")},
        {g.squeeze, P("/user/hand/left/input/squeeze/value")}, {g.squeeze, P("/user/hand/right/input/squeeze/value")},
        {g.a, P("/user/hand/right/input/a/click")}, {g.b, P("/user/hand/right/input/b/click")},
        {g.x, P("/user/hand/left/input/x/click")}, {g.y, P("/user/hand/left/input/y/click")},
        {g.menu, P("/user/hand/left/input/menu/click")},
        {g.haptic, P("/user/hand/left/output/haptic")}, {g.haptic, P("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding ip{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    ip.interactionProfile = P("/interaction_profiles/oculus/touch_controller");
    ip.countSuggestedBindings = (uint32_t)b.size();
    ip.suggestedBindings = b.data();
    if (XR_FAILED(xrSuggestInteractionProfileBindings(g.instance, &ip))) { LOGE("bindings failed"); return false; }
    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.action = g.aim; sc.subactionPath = g.hands[i]; sc.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        if (XR_FAILED(xrCreateActionSpace(g.session, &sc, &g.aim_space[i]))) return false;
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1; at.actionSets = &g.set;
    return XR_SUCCEEDED(xrAttachSessionActionSets(g.session, &at));
}

bool g_has_refresh = false;

// Loader + instance + system: must run on a normal JNI thread (the loader calls into Java).
bool init_instance(JNIEnv *env) {
    if (!g.vm || !g.activity) { LOGE("No JavaVM/activity; OpenXR disabled"); return false; }
    (void)env;
    PFN_xrInitializeLoaderKHR init_loader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader)) && init_loader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = g.vm; li.applicationContext = g.activity;
        XrResult lr = init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
        if (XR_FAILED(lr)) { LOGE("xrInitializeLoaderKHR failed %d", lr); return false; }
    }
    uint32_t ext_n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_n, nullptr);
    std::vector<XrExtensionProperties> props(ext_n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_n, &ext_n, props.data());
    std::vector<const char *> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    for (auto &p : props)
        if (strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) { exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME); g_has_refresh = true; }
    XrInstanceCreateInfoAndroidKHR ia{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ia.applicationVM = g.vm; ia.applicationActivity = g.activity;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.next = &ia;
    snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "FTL Quest");
    snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "ftl-quest");
    ici.applicationInfo.applicationVersion = 1; ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 26);
    ici.enabledExtensionCount = (uint32_t)exts.size(); ici.enabledExtensionNames = exts.data();
    XrResult r = xrCreateInstance(&ici, &g.instance);
    if (XR_FAILED(r)) { LOGE("xrCreateInstance failed %d", r); return false; }
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(g.instance, &sg, &g.system))) { LOGE("xrGetSystem failed"); return false; }
    LOGI("OpenXR instance + system ready");
    return true;
}

bool g_instance_ok = false;

bool init_xr() {
    g.init_tried = true;
    if (!g_instance_ok) return false;
    XrResult r;
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req_fn = nullptr;
    xrGetInstanceProcAddr(g.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn);
    if (req_fn) { XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR}; req_fn(g.instance, g.system, &req); }

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = eglGetCurrentDisplay();
    gb.config = quest_egl_get_config();
    gb.context = eglGetCurrentContext();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb; sci.systemId = g.system;
    r = xrCreateSession(g.instance, &sci, &g.session);
    if (XR_FAILED(r)) { LOGE("xrCreateSession failed %d", r); return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xrCreateReferenceSpace(g.session, &rs, &g.local);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(g.session, &rs, &g.view);

    if (g_has_refresh) {
        PFN_xrRequestDisplayRefreshRateFB req_rate = nullptr;
        xrGetInstanceProcAddr(g.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&req_rate);
        // FTL renders at 60 fps: 60 Hz if offered, else 120 Hz (2:1 cadence) instead of judder at 72.
        if (req_rate) {
            XrResult rr = req_rate(g.session, 60.0f);
            if (XR_FAILED(rr)) rr = req_rate(g.session, 120.0f);
            LOGI("Display refresh request (60, else 120 Hz): %d", (int)rr);
        }
    }
    int pw = 0, ph = 0;
    darwin_get_scene_size(&pw, &ph);
    if (pw <= 0 || ph <= 0) { pw = 1228; ph = 768; }
    for (unsigned p = 0; p < kPlanes; p++) if (!make_swap(g.planes[p], pw, ph)) return false;

    // Cursor: soft ring + bright dot, premultiplied.
    if (make_swap(g.cursor, kCursorSize, kCursorSize)) {
        std::vector<uint8_t> px((size_t)kCursorSize * kCursorSize * 4, 0);
        for (int yy = 0; yy < kCursorSize; yy++) for (int xx = 0; xx < kCursorSize; xx++) {
            float dx = (xx + 0.5f) / kCursorSize * 2 - 1, dy = (yy + 0.5f) / kCursorSize * 2 - 1, d = std::sqrt(dx * dx + dy * dy);
            float a = 0;
            if (d < 0.32f) a = 1.0f;
            else if (d > 0.62f && d < 0.86f) a = 0.85f;
            else if (d < 1.0f) a = std::max(0.0f, 0.25f * (1.0f - d));
            uint8_t *q = &px[((size_t)yy * kCursorSize + xx) * 4];
            q[0] = (uint8_t)(255 * a * 0.55f); q[1] = (uint8_t)(255 * a * 1.0f); q[2] = (uint8_t)(255 * a * 0.75f); q[3] = (uint8_t)(255 * a);
        }
        fill_static(g.cursor, px);
    }
    if (make_swap(g.beam, 8, 64)) {
        std::vector<uint8_t> px((size_t)8 * 64 * 4, 0);
        for (int yy = 0; yy < 64; yy++) for (int xx = 0; xx < 8; xx++) {
            float t = std::fabs((xx + 0.5f) / 8 * 2 - 1); // across the beam
            float a = 0.55f * (1.0f - t * t);
            uint8_t *q = &px[((size_t)yy * 8 + xx) * 4];
            q[0] = (uint8_t)(255 * a * 0.5f); q[1] = (uint8_t)(255 * a); q[2] = (uint8_t)(255 * a * 0.8f); q[3] = (uint8_t)(255 * a);
        }
        fill_static(g.beam, px);
    }
    glGenFramebuffers(1, &g.dst_fbo);
    if (!init_actions()) { LOGE("OpenXR actions failed"); return false; }
    darwin_planes_request(true); // planes feed the quad stack
    LOGI("OpenXR ready: %d planes %dx%d", kPlanes, pw, ph);
    return true;
}

void recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (t <= 0 || XR_FAILED(xrLocateSpace(g.view, g.local, t, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    XrVector3f fwd = norm(rot(loc.pose.orientation, 0, 0, -1));
    fwd.y *= 0.35f; // mostly level, slight tilt toward gaze
    fwd = norm(fwd);
    g.screen.position = add(loc.pose.position, scl(fwd, g.distance));
    g.screen.position.y -= 0.08f;
    XrVector3f z = scl(fwd, -1.0f), up = {0, 1, 0};
    XrVector3f xcol = norm(cross(up, z), {1, 0, 0});
    XrVector3f ycol = norm(cross(z, xcol), {0, 1, 0});
    g.screen.orientation = quat_from_basis(xcol, ycol, z);
    g.pose_init = true;
}

void handle_events() {
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult r = xrPollEvent(g.instance, &ev);
        if (r != XR_SUCCESS) break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *s = (XrEventDataSessionStateChanged *)&ev;
            g.state = s->state;
            LOGI("session state -> %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g.running = XR_SUCCEEDED(xrBeginSession(g.session, &bi));
                g.pose_init = false; g.recenter_frames = 6; g.recenter_after = 0;
                LOGI("xrBeginSession -> %d", g.running ? 1 : 0);
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g.session);
                g.running = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                g.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            auto *rc = (XrEventDataReferenceSpaceChangePending *)&ev;
            g.pose_init = false; g.recenter_after = rc->changeTime; g.recenter_frames = 6;
        }
    }
}

// Quad of plane p in the current depth mode (perspective-matched so planes align from the head).
void plane_quad(unsigned p, XrPosef &pose, XrExtent2Df &size) {
    float aspect = (float)g.planes[0].h / (float)std::max(1, g.planes[0].w);
    float step = depth_step(g.depth_mode);
    float z = ((float)p - 2.0f) * step;
    float d_ref = std::max(0.5f, g.distance);
    float s = std::max(0.25f, d_ref - z) / d_ref;
    XrVector3f off = rot(g.screen.orientation, 0, 0, z);
    pose.orientation = g.screen.orientation;
    pose.position = add(g.screen.position, off);
    size = {g.width_m * s, g.width_m * aspect * s};
}

// Intersect a ray with a quad; returns uv in [0,1] (v down) if hit.
bool ray_quad(XrVector3f o, XrVector3f d, const XrPosef &pose, XrExtent2Df size, float &u, float &v, XrVector3f &hit) {
    XrVector3f n = rot(pose.orientation, 0, 0, 1);
    float den = dot(d, n);
    if (std::fabs(den) < 1e-5f) return false;
    float t = dot(sub(pose.position, o), n) / den;
    if (t <= 0) return false;
    hit = add(o, scl(d, t));
    XrVector3f rel = sub(hit, pose.position);
    float lx = dot(rel, rot(pose.orientation, 1, 0, 0)), ly = dot(rel, rot(pose.orientation, 0, 1, 0));
    u = lx / size.width + 0.5f;
    v = 0.5f - ly / size.height;
    return u >= -0.02f && u <= 1.02f && v >= -0.02f && v <= 1.02f;
}

void poll_input(XrTime t) {
    if (g.state != XR_SESSION_STATE_FOCUSED) {
        if (g.touching) { quest_inject_touch(kLaserPointerId, g.hit_u * kTouchW, g.hit_v * kTouchH, UITouchPhaseCancelled); g.touching = false; }
        g.hit_valid = false;
        return;
    }
    XrActiveActionSet as{g.set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(g.session, &si))) return;
    auto getf = [&](XrAction a, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = g.hands[h];
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        return (XR_SUCCEEDED(xrGetActionStateFloat(g.session, &gi, &st)) && st.isActive) ? st.currentState : 0.0f;
    };
    auto getb = [&](XrAction a, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = h >= 0 ? g.hands[h] : XR_NULL_PATH;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(g.session, &gi, &st)) && st.isActive && st.currentState;
    };
    auto getv = [&](XrAction a, int h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = g.hands[h];
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        return (XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &gi, &st)) && st.isActive) ? st.currentState : XrVector2f{0, 0};
    };

    // Trigger hysteresis; the hand that last pressed its trigger owns the laser.
    bool pressed_edge[2] = {false, false};
    for (int h = 0; h < 2; h++) {
        float tv = getf(g.trigger, h);
        bool down = g.trig_down[h] ? tv > kTrigOff : tv > kTrigOn;
        if (down && !g.trig_down[h]) { pressed_edge[h] = true; if (!g.touching) g.laser_hand = h; }
        g.trig_down[h] = down;
    }

    // Laser ray vs. the plane stack. Hit-test the middle (rooms) plane in layered modes,
    // so clicks on rooms/crew line up; UI targets are large enough to tolerate parallax.
    g.hit_valid = false;
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    if (g.aim_space[g.laser_hand] && XR_SUCCEEDED(xrLocateSpace(g.aim_space[g.laser_hand], g.local, t, &hl)) &&
        (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        XrVector3f o = hl.pose.position, d = norm(rot(hl.pose.orientation, 0, 0, -1));
        // Pick the layer the user actually sees under the ray: test planes front (UI) to back
        // and take the first whose pixel at the hit point is visible (alpha read from the
        // plane FBO). Each plane is hit at its own depth, so parallax between the controller
        // and the head no longer shifts clicks off UI buttons.
        const bool layered = g.depth_mode != 0 && darwin_planes_enabled();
        int pw = 0, ph = 0;
        darwin_get_plane_size(&pw, &ph);
        GLint prev_read = 0;
        if (layered) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
        for (int p = layered ? 4 : 2; p >= (layered ? 0 : 2); p--) {
            XrPosef qp; XrExtent2Df qs;
            plane_quad((unsigned)p, qp, qs);
            float u, v; XrVector3f hit;
            if (!ray_quad(o, d, qp, qs, u, v, hit)) continue;
            u = std::clamp(u, 0.0f, 1.0f); v = std::clamp(v, 0.0f, 1.0f);
            bool visible = true;
            if (layered && p > 0 && pw > 0 && ph > 0) {
                // Sample a small window, not one pixel: UI icons are punched out (holes in the
                // round system buttons), and the ray must not fall through them. UI gets a
                // wider pick radius than world layers.
                const int r = (p == 4) ? 9 : 3, n = 2 * r + 1;
                uint8_t px[19 * 19 * 4];
                int cx = std::clamp((int)(u * pw), r, pw - 1 - r), cy = std::clamp((int)((1.0f - v) * ph), r, ph - 1 - r);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, darwin_get_plane_fbo(p));
                glReadPixels(cx - r, cy - r, n, n, GL_RGBA, GL_UNSIGNED_BYTE, px);
                visible = false;
                for (int i = 0; i < n * n && !visible; i++) visible = px[i * 4 + 3] > 24;
            }
            if (!visible) continue;
            g.hit_valid = true;
            g.hit_u = u; g.hit_v = v;
            g.hit_pos = hit; g.ray_origin = o;
            g.hit_plane = p;
            break;
        }
        if (layered) glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    }
    const float tx = g.hit_u * kTouchW, ty = g.hit_v * kTouchH;
    const int lh = g.laser_hand;
    if (!g.touching && pressed_edge[lh] && g.hit_valid) {
        quest_inject_touch(kLaserPointerId, tx, ty, UITouchPhaseBegan);
        g.touching = true;
        haptic(lh, 0.35f, 12000000LL);
    } else if (g.touching && g.trig_down[lh]) {
        if (g.hit_valid) quest_inject_touch(kLaserPointerId, tx, ty, UITouchPhaseMoved);
    } else if (g.touching) {
        quest_inject_touch(kLaserPointerId, tx, ty, UITouchPhaseEnded);
        g.touching = false;
    }

    // Stick clicks: L+R chord cycles depth, R alone recenters, L alone = pause (existing shortcut).
    bool lc = getb(g.stick_click, 0), rc = getb(g.stick_click, 1);
    bool both = lc && rc;
    if (both && !g.prev_both) {
        g.depth_mode = (g.depth_mode + 1) % 3;
        g.chord_used = true;
        haptic(0, 0.45f, 25000000LL); haptic(1, 0.45f, 25000000LL);
        LOGI("Depth mode -> %d (%.2fm)", g.depth_mode, (double)depth_step(g.depth_mode));
    }
    g.prev_both = both;
    if (!lc && !rc) {
        if (!g.chord_used) {
            if (g.prev_click[1]) { recenter(t); haptic(1, 0.3f, 15000000LL); }
            else if (g.prev_click[0]) quest_inject_button(QUEST_BTN_THUMB_L, true);
        }
        g.chord_used = false;
    }
    g.prev_click[0] = lc; g.prev_click[1] = rc;

    // Grip (either hand) = grab the screen; it follows that controller while held.
    // Right stick Y while grabbing pushes/pulls it. Otherwise right stick up/down = power +/-
    // on the system under the laser (up = tap to add, down = swipe down to remove).
    XrVector2f rs = getv(g.stick, 1), ls = getv(g.stick, 0);
    for (int h = 0; h < 2; h++) {
        bool gr = getf(g.squeeze, h) > 0.6f;
        if (gr && !g.prev_grip[h] && g.grab_hand < 0) {
            XrSpaceLocation cl{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(g.aim_space[h], g.local, t, &cl)) &&
                (cl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                XrQuaternionf ci = qconj(cl.pose.orientation);
                XrVector3f d = sub(g.screen.position, cl.pose.position);
                g.grab_rel_pos = rot(ci, d.x, d.y, d.z);
                g.grab_rel_rot = qmul(ci, g.screen.orientation);
                g.grab_hand = h;
                haptic(h, 0.3f, 15000000LL);
            }
        }
        if (!gr && g.prev_grip[h] && g.grab_hand == h) {
            g.grab_hand = -1;
            XrSpaceLocation hv{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(g.view, g.local, t, &hv)))
                g.distance = std::clamp(len(sub(g.screen.position, hv.pose.position)), 0.5f, 4.0f);
        }
        g.prev_grip[h] = gr;
    }
    if (g.grab_hand >= 0) {
        XrSpaceLocation cl{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g.aim_space[g.grab_hand], g.local, t, &cl)) &&
            (cl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            XrVector2f st = getv(g.stick, g.grab_hand);
            if (std::fabs(st.y) > 0.3f) {
                float L = len(g.grab_rel_pos);
                float nl = std::clamp(L * (1.0f + st.y * 0.015f), 0.3f, 4.0f);
                g.grab_rel_pos = scl(norm(g.grab_rel_pos), nl);
            }
            XrVector3f off = rot(cl.pose.orientation, g.grab_rel_pos.x, g.grab_rel_pos.y, g.grab_rel_pos.z);
            g.screen.position = add(cl.pose.position, off);
            g.screen.orientation = qmul(cl.pose.orientation, g.grab_rel_rot);
            XrSpaceLocation hv{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(g.view, g.local, t, &hv)))
                g.distance = std::clamp(len(sub(g.screen.position, hv.pose.position)), 0.5f, 4.0f);
            g.pose_init = true;
            g.recenter_frames = 0;
        }
    } else {
        static int s_power_dir = 0; // edge-triggered stick flicks
        int dir = rs.y > 0.7f ? 1 : (rs.y < -0.7f ? -1 : 0);
        if (std::fabs(rs.y) < 0.3f) s_power_dir = 0;
        if (dir != 0 && s_power_dir == 0 && g.hit_valid && !g.touching && g_gesture.empty()) {
            s_power_dir = dir;
            queue_swipe_touch(tx, ty, dir > 0 ? -1.0f : 1.0f);
            haptic(1, 0.25f, 10000000LL);
            LOGI("Power %s at touch (%.0f,%.0f)", dir > 0 ? "+" : "-", tx, ty);
        }
    }
    if (g.grab_hand != 0) quest_inject_thumbstick(0, ls.x, -ls.y);

    // Dedicated buttons: A = pause, Y = door controls. X/B/menu are left free for now.
    const bool btn_a = getb(g.a, -1), btn_y = getb(g.y, -1);
    if (btn_a && !g.prev_btn[0] && g_gesture.empty()) { queue_tap_frame(kPauseX, kPauseY); haptic(1, 0.3f, 12000000LL); }
    if (btn_y && !g.prev_btn[1] && g_gesture.empty()) { queue_tap_frame(kDoorsX, kDoorsY); haptic(0, 0.3f, 12000000LL); }
    g.prev_btn[0] = btn_a; g.prev_btn[1] = btn_y;
}

void blit_planes(bool flat, GLuint scene_fbo, int sw, int sh) {
    GLint prev_draw = 0, prev_read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(0x8DB9 /* GL_FRAMEBUFFER_SRGB_EXT: raw byte copy into the sRGB swapchain */);
    int pw = 0, ph = 0;
    darwin_get_plane_size(&pw, &ph);
    const unsigned n = flat ? 1 : kPlanes;
    for (unsigned p = 0; p < n; p++) {
        GLuint src = flat ? scene_fbo : darwin_get_plane_fbo((int)p);
        int w = flat ? sw : pw, h = flat ? sh : ph;
        Swap &s = g.planes[p];
        GLuint tex = acquire(s);
        if (tex && src && w > 0 && h > 0) {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.dst_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
            glBlitFramebuffer(0, 0, w, h, 0, 0, s.w, s.h, GL_COLOR_BUFFER_BIT, (w == s.w && h == s.h) ? GL_NEAREST : GL_LINEAR);
        }
        release(s);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.dst_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glEnable(0x8DB9);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
}

} // namespace

extern "C" {

void quest_xr_set_java(void *vm, void *activity) {
    g.vm = (JavaVM *)vm;
    g.activity = (jobject)activity;
    JNIEnv *env = nullptr;
    g.vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    g_instance_ok = init_instance(env);
}

// Called from eagl_present_renderbuffer on FTL's GL thread after the frame is complete.
// Returns 1 if the frame was presented through OpenXR (skip the window blit/swap).
int quest_xr_present(void) {
    if (!g.init_tried) {
        // Wait (briefly) for the scene FBO so swapchains match it 1:1.
        static int s_wait = 0;
        if (!darwin_scene_known() && s_wait++ < 180) return 0;
        g.ok = init_xr();
    }
    if (!g.ok) return 0;
    handle_events();
    if (!g.running) { usleep(10000); return 1; }

    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g.session, &wi, &fs))) return 1;
    if (g.state == XR_SESSION_STATE_FOCUSED && (!g.pose_init || g.recenter_frames > 0) && fs.predictedDisplayTime >= g.recenter_after) {
        recenter(fs.predictedDisplayTime);
        if (g.pose_init && g.recenter_frames > 0) g.recenter_frames--;
    }
    // adb test hooks: files/.xr_depth ("0"|"1"|"2"), files/.xr_recenter
    static int s_poll = 0;
    if ((s_poll++ % 30) == 0) {
        const char *dp = "/sdcard/Android/data/com.subsetgames.ftl/files/.xr_depth";
        if (FILE *f = fopen(dp, "r")) {
            int m = -1;
            if (fscanf(f, "%d", &m) == 1 && m >= 0 && m <= 2) { g.depth_mode = m; LOGI("Depth mode -> %d (file)", m); }
            fclose(f);
            unlink(dp);
        }
        const char *rp = "/sdcard/Android/data/com.subsetgames.ftl/files/.xr_recenter";
        if (access(rp, F_OK) == 0) { unlink(rp); g.pose_init = false; g.recenter_frames = 6; g.recenter_after = 0; }
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g.session, &bi))) return 1;
    poll_input(fs.predictedDisplayTime);
    run_gestures();

    std::vector<const XrCompositionLayerBaseHeader *> layers;
    XrCompositionLayerQuad quads[kPlanes + 2];
    for (auto &q : quads) { q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD}; }
    if (fs.shouldRender && g.pose_init) {
        int sw = 0, sh = 0;
        GLuint scene = darwin_get_scene_fbo(&sw, &sh);
        const bool flat = g.depth_mode == 0 || !darwin_planes_enabled();
        blit_planes(flat, scene, sw, sh);
        const unsigned n = flat ? 1 : kPlanes;
        for (unsigned p = 0; p < n; p++) {
            XrCompositionLayerQuad &q = quads[p];
            q.layerFlags = p == 0 ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // planes 1-4 premultiplied
            q.space = g.local; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = g.planes[p].sc;
            q.subImage.imageRect = {{0, 0}, {g.planes[p].w, g.planes[p].h}};
            plane_quad(flat ? 2 : p, q.pose, q.size);
            layers.push_back((const XrCompositionLayerBaseHeader *)&q);
        }
        if (g.hit_valid && g.cursor.sc) {
            // Beam: thin quad from controller to hit, its face turned toward the head.
            XrSpaceLocation hv{XR_TYPE_SPACE_LOCATION};
            XrVector3f head = g.screen.position;
            if (XR_SUCCEEDED(xrLocateSpace(g.view, g.local, fs.predictedDisplayTime, &hv))) head = hv.pose.position;
            XrVector3f axis = sub(g.hit_pos, g.ray_origin);
            float L = len(axis);
            if (L > 0.05f && g.beam.sc) {
                XrVector3f ydir = scl(axis, 1.0f / L);
                XrVector3f mid = add(g.ray_origin, scl(axis, 0.5f));
                XrVector3f toh = sub(head, mid);
                XrVector3f zdir = norm(sub(toh, scl(ydir, dot(toh, ydir))), {0, 0, 1});
                XrVector3f xdir = norm(cross(ydir, zdir), {1, 0, 0});
                XrCompositionLayerQuad &q = quads[kPlanes];
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                q.space = g.local; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = g.beam.sc;
                q.subImage.imageRect = {{0, 0}, {g.beam.w, g.beam.h}};
                q.pose.orientation = quat_from_basis(xdir, ydir, zdir);
                q.pose.position = mid;
                q.size = {0.004f, L};
                layers.push_back((const XrCompositionLayerBaseHeader *)&q);
            }
            XrCompositionLayerQuad &c = quads[kPlanes + 1];
            c.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            c.space = g.local; c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            c.subImage.swapchain = g.cursor.sc;
            c.subImage.imageRect = {{0, 0}, {g.cursor.w, g.cursor.h}};
            c.pose.orientation = g.screen.orientation;
            c.pose.position = add(g.hit_pos, rot(g.screen.orientation, 0, 0, 0.004f));
            float cs = g.touching ? 0.018f : 0.024f;
            c.size = {cs, cs};
            layers.push_back((const XrCompositionLayerBaseHeader *)&c);
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (uint32_t)layers.size();
    ei.layers = layers.empty() ? nullptr : layers.data();
    XrResult r = xrEndFrame(g.session, &ei);
    static int s_err = 0;
    if (XR_FAILED(r) && s_err++ < 5) LOGW("xrEndFrame failed %d (layers=%u)", r, ei.layerCount);
    return 1;
}

int quest_xr_active(void) { return g.ok ? 1 : 0; }

} // extern "C"

#endif // FTL_ENABLE_OPENXR
