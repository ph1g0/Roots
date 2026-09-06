#include "touch.h"

void touch_init(void) {
#ifndef ROOTS_NO_TOUCH
  app_touch_navigation_enable(true);
#endif
}

#ifdef ROOTS_TOUCH_GESTURES
// Best reading of the 4.33 changelog: recognizers are created with a
// callback, attached to a window, and report started/updated/completed/
// cancelled. Adjust names to the header; the logic below is the whole of it.
static TouchCb s_cb;

static void on_swipe(Recognizer *r, RecognizerEvent e, void *ctx) {
  if (e != RecognizerEventCompleted || !s_cb) return;
  SwipeDirection dir = swipe_recognizer_get_direction(r);
  if (dir == SwipeDirectionUp)        s_cb(TOUCH_NEXT);   // finger up = next card, like a list
  else if (dir == SwipeDirectionDown) s_cb(TOUCH_PREV);
}
static void on_tap(Recognizer *r, RecognizerEvent e, void *ctx) {
  if (e == RecognizerEventCompleted && s_cb) s_cb(TOUCH_SELECT);
}

void touch_attach(Window *w, TouchCb cb) {
  s_cb = cb;
  window_set_touch_bridge_disabled(w, true);   // we handle it; don't also get button events
  window_attach_recognizer(w, swipe_recognizer_create(on_swipe, NULL));
  window_attach_recognizer(w, tap_recognizer_create(on_tap, NULL));
}
#else
void touch_attach(Window *w, TouchCb cb) { (void)w; (void)cb; }
#endif
