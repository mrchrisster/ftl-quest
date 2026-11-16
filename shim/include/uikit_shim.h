#pragma once

#include <cstdint>
#include <stdbool.h>
#include "objc_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double x;
    double y;
} CGPoint;

typedef struct {
    double width;
    double height;
} CGSize;

typedef struct {
    CGPoint origin;
    CGSize size;
} CGRect;

typedef enum {
    UITouchPhaseBegan = 0,
    UITouchPhaseMoved = 1,
    UITouchPhaseStationary = 2,
    UITouchPhaseEnded = 3,
    UITouchPhaseCancelled = 4
} UITouchPhase;

typedef enum {
    QUEST_BTN_A = 0,
    QUEST_BTN_B,
    QUEST_BTN_X,
    QUEST_BTN_Y,
    QUEST_BTN_GRIP_L,
    QUEST_BTN_GRIP_R,
    QUEST_BTN_TRIGGER_L,
    QUEST_BTN_TRIGGER_R,
    QUEST_BTN_THUMB_L,
    QUEST_BTN_THUMB_R,
    QUEST_BTN_MENU
} QuestControllerButton;

struct UITouch_t {
    id isa;
    int touch_id;
    CGPoint location;
    CGPoint previous_location;
    UITouchPhase phase;
    double timestamp;
    id view;
    id window;
};

// Quest 3 Touch & Laser Pointer Injection
void quest_set_root_view(id view);
id quest_get_root_view(void);
void quest_inject_touch(int pointer_id, float x, float y, UITouchPhase phase);
void quest_dispatch_touches(void);
id quest_touchset_enumerator(id touch_set);
id quest_enumerator_next_object(id enumerator);

// Quest 3 Controller Physical Buttons & Shortcut System
void quest_inject_button(QuestControllerButton btn, bool pressed);
void quest_inject_thumbstick(int stick_id, float axis_x, float axis_y);

// Tactical Shortcut Trigger Helpers
void quest_trigger_pause(void);
void quest_trigger_doors_open(void);
void quest_trigger_doors_close(void);
void quest_trigger_select_weapon(int slot_index);

#ifdef __cplusplus
}
#endif
