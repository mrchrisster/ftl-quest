#include "uikit_shim.h"
#include "darwin_abi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <mutex>
#include <android/log.h>

#define TAG "FTL_UIKIT_SHIM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static std::map<int, UITouch_t> g_active_touches;
static std::mutex g_touch_mutex;
static id g_root_view = nullptr;
static bool g_doors_opened = false;

// iPad native layout coordinates (1024x768 normalized to native resolution)
static const float UI_PAUSE_X = 40.0f;
static const float UI_PAUSE_Y = 35.0f;
static const float UI_DOORS_OPEN_X = 980.0f;
static const float UI_DOORS_OPEN_Y = 165.0f;
static const float UI_DOORS_CLOSE_X = 980.0f;
static const float UI_DOORS_CLOSE_Y = 220.0f;
static const float UI_WEAPON_Y = 715.0f;
static const float UI_WEAPON_X[4] = { 150.0f, 250.0f, 350.0f, 450.0f };

extern "C" {

void quest_set_root_view(id view) {
    g_root_view = view;
    LOGI("Root view registered for touch dispatch: %p", view);
}

id quest_get_root_view(void) {
    return g_root_view;
}

// Emulated NSSet wrapper for passing touches to touchesBegan/Moved/Ended
struct TouchSet_impl {
    id isa;
    std::vector<UITouch_t*> touches;
};

struct TouchEnumerator_impl {
    id isa;
    std::vector<UITouch_t*> touches;
    size_t index;
};

id quest_touchset_enumerator(id touch_set) {
    if (!touch_set) return nil;
    TouchSet_impl *set = (TouchSet_impl *)touch_set;
    TouchEnumerator_impl *en = new TouchEnumerator_impl();
    en->isa = objc_getClass("NSEnumerator");
    en->touches = set->touches;
    en->index = 0;
    return (id)en;
}

id quest_enumerator_next_object(id enumerator) {
    if (!enumerator) return nil;
    TouchEnumerator_impl *en = (TouchEnumerator_impl *)enumerator;
    if (en->index < en->touches.size()) {
        return (id)en->touches[en->index++];
    }
    return nil;
}

void quest_inject_touch(int pointer_id, float x, float y, UITouchPhase phase) {
    static int s_touch_logs = 0;
    if (s_touch_logs < 24) {
        LOGI("quest_inject_touch(id=%d, x=%.1f, y=%.1f, phase=%d, root=%p)",
             pointer_id, x, y, (int)phase, g_root_view);
        s_touch_logs++;
    }
    double now_sec = (double)_mach_absolute_time() / 1000000000.0;
    UITouch_t *t = nullptr;
    id root_view = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_touch_mutex);
        auto it = g_active_touches.find(pointer_id);
        if (it == g_active_touches.end()) {
            UITouch_t touch;
            touch.isa = objc_getClass("UITouch");
            touch.touch_id = pointer_id;
            touch.location.x = x;
            touch.location.y = y;
            touch.previous_location = touch.location;
            touch.phase = phase;
            touch.timestamp = now_sec;
            touch.view = g_root_view;
            touch.window = nullptr;
            auto inserted = g_active_touches.emplace(pointer_id, touch);
            t = &inserted.first->second;
        } else {
            it->second.previous_location = it->second.location;
            it->second.location.x = x;
            it->second.location.y = y;
            it->second.phase = phase;
            it->second.timestamp = now_sec;
            t = &it->second;
        }
        root_view = g_root_view;
    }

    if (root_view && t) {
        SEL selector = nullptr;
        switch (phase) {
            case UITouchPhaseBegan:
                selector = sel_registerName("touchesBegan:withEvent:");
                break;
            case UITouchPhaseMoved:
                selector = sel_registerName("touchesMoved:withEvent:");
                break;
            case UITouchPhaseEnded:
                selector = sel_registerName("touchesEnded:withEvent:");
                break;
            case UITouchPhaseCancelled:
                selector = sel_registerName("touchesCancelled:withEvent:");
                break;
            default:
                break;
        }

        if (selector) {
            TouchSet_impl set;
            set.isa = objc_getClass("NSSet");
            set.touches.push_back(t);
            objc_msgSend(root_view, selector, (id)&set, nil);
        }
    }

    if (phase == UITouchPhaseEnded || phase == UITouchPhaseCancelled) {
        std::lock_guard<std::mutex> lock(g_touch_mutex);
        g_active_touches.erase(pointer_id);
    }
}

void quest_dispatch_touches(void) {
}

// ============================================================================
// Tactical Controller Button Shortcut System
// ============================================================================

static void inject_synthetic_tap(float x, float y) {
    int synth_id = 99; // Dedicated ID for synthetic tactical taps
    quest_inject_touch(synth_id, x, y, UITouchPhaseBegan);
    quest_inject_touch(synth_id, x, y, UITouchPhaseEnded);
}

void quest_trigger_pause(void) {
    LOGI("[Controller Shortcut] Toggling Spacebar / Pause...");
    // 1. First attempt: tap the pause button directly
    inject_synthetic_tap(UI_PAUSE_X, UI_PAUSE_Y);

    // 2. Also simulate the iPad 2-finger tap gesture
    quest_inject_touch(90, 400.0f, 384.0f, UITouchPhaseBegan);
    quest_inject_touch(91, 600.0f, 384.0f, UITouchPhaseBegan);
    quest_inject_touch(90, 400.0f, 384.0f, UITouchPhaseEnded);
    quest_inject_touch(91, 600.0f, 384.0f, UITouchPhaseEnded);
}

void quest_trigger_doors_open(void) {
    LOGI("[Controller Shortcut] Opening all interior doors...");
    inject_synthetic_tap(UI_DOORS_OPEN_X, UI_DOORS_OPEN_Y);
}

void quest_trigger_doors_close(void) {
    LOGI("[Controller Shortcut] Closing all interior doors...");
    inject_synthetic_tap(UI_DOORS_CLOSE_X, UI_DOORS_CLOSE_Y);
}

void quest_trigger_select_weapon(int slot_index) {
    if (slot_index >= 0 && slot_index < 4) {
        LOGI("[Controller Shortcut] Selecting Weapon Slot %d...", slot_index + 1);
        inject_synthetic_tap(UI_WEAPON_X[slot_index], UI_WEAPON_Y);
    }
}

void quest_inject_button(QuestControllerButton btn, bool pressed) {
    if (!pressed) return; // Trigger on press down

    switch (btn) {
        case QUEST_BTN_GRIP_L:
        case QUEST_BTN_GRIP_R:
        case QUEST_BTN_THUMB_L:
            // Squeezing the Grip Trigger or clicking Left Stick = Instant Tactical Pause
            quest_trigger_pause();
            break;

        case QUEST_BTN_Y:
            // Y Button = Toggle All Interior Doors
            if (g_doors_opened) {
                quest_trigger_doors_close();
                g_doors_opened = false;
            } else {
                quest_trigger_doors_open();
                g_doors_opened = true;
            }
            break;

        case QUEST_BTN_X:
            // X Button = Autofire Toggle (coordinates roughly near weapon UI bottom-left)
            inject_synthetic_tap(80.0f, UI_WEAPON_Y);
            break;

        case QUEST_BTN_B:
            // B Button = Cancel / Deselect
            inject_synthetic_tap(UI_PAUSE_X, UI_PAUSE_Y);
            break;

        default:
            break;
    }
}

void quest_inject_thumbstick(int stick_id, float axis_x, float axis_y) {
    // Left stick quick weapon selection via 4-way tilt:
    // Up: Weapon 1, Right: Weapon 2, Down: Weapon 3, Left: Weapon 4
    const float kThreshold = 0.7f;
    static bool stick_centered = true;

    if (stick_id == 0) { // Left stick
        if (stick_centered) {
            if (axis_y < -kThreshold) {
                quest_trigger_select_weapon(0); // Up -> Weapon 1
                stick_centered = false;
            } else if (axis_x > kThreshold) {
                quest_trigger_select_weapon(1); // Right -> Weapon 2
                stick_centered = false;
            } else if (axis_y > kThreshold) {
                quest_trigger_select_weapon(2); // Down -> Weapon 3
                stick_centered = false;
            } else if (axis_x < -kThreshold) {
                quest_trigger_select_weapon(3); // Left -> Weapon 4
                stick_centered = false;
            }
        } else {
            if (fabs(axis_x) < 0.3f && fabs(axis_y) < 0.3f) {
                stick_centered = true;
            }
        }
    }
}

}
