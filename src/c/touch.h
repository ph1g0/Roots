#pragma once
#include <pebble.h>

// Touch on Pebble Time 2. Two layers:
//  1. app_touch_navigation_enable(true): the system turns swipes into UP/DOWN and
//     taps into SELECT, which is exactly how the cards already work. On by
//     default; -DROOTS_NO_TOUCH removes it.
//  2. Gesture recognizers, for when the bridge does not cover a plain Layer
//     window. Off by default (-DROOTS_TOUCH_GESTURES) because the changelog
//     names the functions but not their signatures — check
//     User_Interface/Gesture_Recognizers in the SDK header first.
typedef enum { TOUCH_PREV, TOUCH_NEXT, TOUCH_SELECT } TouchAction;
typedef void (*TouchCb)(TouchAction a);

void touch_init(void);
void touch_attach(Window *w, TouchCb cb);
