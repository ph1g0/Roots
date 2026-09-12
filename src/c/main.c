#include <pebble.h>
#include "common.h"
#include "headroom.h"
#include "cards.h"
#include "hrv.h"
#include "metric.h"
#include "settings.h"
#include "theme.h"
#include "touch.h"

static Window   *s_main, *s_detail;
static Layer    *s_main_layer, *s_detail_layer;
static Headroom  s_h;
static int       s_card    = CARD_HEADROOM;

// The visible cards, in enum order. Rebuilt whenever the phone sends new
// settings, so hiding a card takes it out of the UP/DOWN loop immediately
// rather than at the next launch.
static uint8_t   s_order[CARD_COUNT];
static int       s_visible = 0;

static void rebuild_order(void) {
  int keep = s_card;
  s_visible = 0;
  for (int c = 0; c < CARD_COUNT; c++)
    if (settings_card_visible(c)) s_order[s_visible++] = (uint8_t)c;
  // Stay where you were if that card is still shown; otherwise go home.
  s_card = CARD_HEADROOM;
  for (int i = 0; i < s_visible; i++) if (s_order[i] == keep) { s_card = keep; break; }
}

static int card_index(void) {
  for (int i = 0; i < s_visible; i++) if (s_order[i] == s_card) return i;
  return 0;
}
static void card_step(int by) {
  if (s_visible <= 0) return;
  int i = (card_index() + by + s_visible) % s_visible;
  s_card = s_order[i];
  if (s_main_layer) layer_mark_dirty(s_main_layer);
}
static int       s_scroll  = 0;
static int       s_content = 0;   // set by the last detail draw

static void redraw(void) {
  if (s_main_layer)   layer_mark_dirty(s_main_layer);
  if (s_detail_layer) layer_mark_dirty(s_detail_layer);
}

// ---------------------------------------------------------------- layers
static void main_update(Layer *l, GContext *ctx) {
  cards_draw_main(ctx, layer_get_bounds(l), s_card, &s_h);
}

static void detail_update(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  s_content = cards_draw_detail(ctx, b, s_card, &s_h, s_scroll);
}

// ---------------------------------------------------------------- detail window
static void detail_scroll(int dy) {
  int visible = s_detail_layer ? layer_get_bounds(s_detail_layer).size.h : 0;
  int max = s_content - visible;
  if (max < 0) max = 0;
  s_scroll += dy;
  if (s_scroll < 0)   s_scroll = 0;
  if (s_scroll > max) s_scroll = max;
  layer_mark_dirty(s_detail_layer);
}
static void detail_up(ClickRecognizerRef r, void *c)   { detail_scroll(-30); }
static void detail_down(ClickRecognizerRef r, void *c) { detail_scroll(30); }
static void detail_clicks(void *c) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, detail_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, detail_down);
}
static void detail_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_detail_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_detail_layer, detail_update);
  layer_add_child(root, s_detail_layer);
}
static void detail_unload(Window *w) {
  layer_destroy(s_detail_layer);
  s_detail_layer = NULL;
}

// ---------------------------------------------------------------- main window
static void push_detail(void) {
  if (!s_detail) {
    s_detail = window_create();
    window_set_background_color(s_detail, C_BG);
    window_set_click_config_provider(s_detail, detail_clicks);
    window_set_window_handlers(s_detail, (WindowHandlers){ .load = detail_load, .unload = detail_unload });
  }
  s_scroll = 0;
  window_stack_push(s_detail, true);
}
// SELECT opens the detail everywhere. HRV is the one exception: a test is the
// point of that card, so SELECT starts one and the graph is on a long press.
//
// Metric used to work the same way, and it was wrong. Logging a weight is a
// thing you do once a week; looking at the trend is what you open the card
// for, and it was the one action buried behind a long press. Swapped.
static void metric_saved(void) { redraw(); }

static void select_action(void) {
  if (s_card == CARD_HRV) hrv_show();
  else                    push_detail();
}
static void open_detail(ClickRecognizerRef r, void *c) { select_action(); }
static void long_select(ClickRecognizerRef r, void *c) {
  if (s_card == CARD_HRV)         push_detail();
  else if (s_card == CARD_METRIC) metric_entry_show(metric_saved);
}
// Long-press UP flips dark/light. Dark reads better under a two-second
// backlight; light reads better in direct sun on this display.
static void long_up(ClickRecognizerRef r, void *c) {
  theme_toggle();
  window_set_background_color(s_main, C_BG);
  if (s_detail) window_set_background_color(s_detail, C_BG);
  redraw();
}
static void touch_action(TouchAction a) {
  if (a == TOUCH_PREV)        card_step(-1);
  else if (a == TOUCH_NEXT)   card_step(1);
  else if (a == TOUCH_SELECT) select_action();
}
static void card_prev(ClickRecognizerRef r, void *c) { card_step(-1); }
static void card_next(ClickRecognizerRef r, void *c) { card_step(1); }
static void main_clicks(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, card_prev);
  window_single_click_subscribe(BUTTON_ID_DOWN, card_next);
  window_single_click_subscribe(BUTTON_ID_SELECT, open_detail);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, long_select, NULL);
  window_long_click_subscribe(BUTTON_ID_UP, 700, long_up, NULL);
}
static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_main_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_main_layer, main_update);
  layer_add_child(root, s_main_layer);
}
static void main_unload(Window *w) {
  layer_destroy(s_main_layer);
  s_main_layer = NULL;
}

// ---------------------------------------------------------------- refresh
// A new minute record has landed: recompute today's part. The night is cached
// and only re-read once per calendar day.
static void tick(struct tm *t, TimeUnits changed) {
  headroom_refresh_day(&s_h);
  redraw();
}

static void health_event(HealthEventType e, void *c) {
  if (hrv_is_open()) { hrv_health_event(e); return; }   // the test owns the sensor
  if (e == HealthEventMovementUpdate) {
    headroom_refresh_steps(&s_h);   // cheap: today's step total only
    redraw();
  } else if (e == HealthEventSleepUpdate || e == HealthEventSignificantUpdate) {
    headroom_compute(&s_h);
    redraw();
  }
}

// ---------------------------------------------------------------- lifecycle
// The phone sent new settings: colours, which cards are shown, the zone
// floor. Rebuild everything that depends on them and redraw in place.
static void settings_changed(void) {
  theme_reload();
  rebuild_order();
  if (s_main)   window_set_background_color(s_main, C_BG);
  if (s_detail) window_set_background_color(s_detail, C_BG);
  headroom_compute(&s_h);
  redraw();
}

static void init(void) {
  settings_init();
  settings_subscribe(settings_changed);
  rebuild_order();
  headroom_compute(&s_h);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);
  health_service_events_subscribe(health_event, NULL);

  // The system backlight times out after a couple of seconds, which is not
  // long enough to read a card. Hold it on while Roots is in front; the app
  // is open for seconds at a time, so the cost is nil on a 30-day battery.
#ifndef ROOTS_NO_LIGHT
  light_enable(true);
#endif

  touch_init();
  s_main = window_create();
  window_set_background_color(s_main, C_BG);
  touch_attach(s_main, touch_action);
  window_set_click_config_provider(s_main, main_clicks);
  window_set_window_handlers(s_main, (WindowHandlers){ .load = main_load, .unload = main_unload });
  window_stack_push(s_main, false);

  // v0.8 asked "how do you feel?" here, before the number was visible, and
  // fed the answer back as a calibration offset. It is gone. Self-report is
  // not a second opinion the model can be graded against: people are poor
  // judges of their own state, the answer is trivially gameable in either
  // direction, and the mapping it used made an honest "Good" read as a
  // complaint that the score was too high. The app measures or it says
  // nothing.
}

static void deinit(void) {
#ifndef ROOTS_NO_LIGHT
  light_enable(false);
#endif
  health_service_events_unsubscribe();
  tick_timer_service_unsubscribe();
  if (s_detail) window_destroy(s_detail);
  window_destroy(s_main);
}

int main(void) { init(); app_event_loop(); deinit(); }
