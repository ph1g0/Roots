#include "feel.h"
#include "common.h"
#include "history.h"
#include "headroom.h"
#include "theme.h"

static Window      *s_win;
static Layer       *s_layer;
static int          s_sel = 3;          // start on "Okay": neutral, not leading
static FeelAnswerCb s_cb;

bool feel_should_ask(void) {
  int today = history_day_key();
  return !(persist_exists(KEY_FEEL_DAY) && persist_read_int(KEY_FEEL_DAY) == today);
}

static void mark_asked(void) { persist_write_int(KEY_FEEL_DAY, history_day_key()); }

static void draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  int w = b.size.w, h = b.size.h;

  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, C_MUTE);
  graphics_draw_text(ctx, "HOW DO YOU FEEL?", fonts_get_system_font(F_BODY_B),
                     GRect(0, 9, w, LINE_H), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // The answer, big, in the accent colour.
  graphics_context_set_text_color(ctx, C_ACCENT);
  graphics_draw_text(ctx, feel_label((uint8_t)s_sel), fonts_get_system_font(w >= 180 ? F_BIG : F_BIG_S),
                     GRect(0, h / 2 - 44, w, 56), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Five blocks, lit up to the selection: a bar you fill, not a slider.
  int gap = 4, bw = (w - 16 - 4 * gap) / 5, by = h / 2 + 16;
  for (int i = 1; i <= 5; i++) {
    graphics_context_set_fill_color(ctx, i <= s_sel ? C_ACCENT : C_TRACK);
    graphics_fill_rect(ctx, GRect(8 + (i - 1) * (bw + gap), by, bw, 12), 2, GCornersAll);
  }

  graphics_context_set_fill_color(ctx, C_PILL);
  graphics_fill_rect(ctx, GRect(8, h - 14 - LINE_H - 6, w - 16, LINE_H + 6), 4, GCornersAll);
  graphics_context_set_text_color(ctx, C_PILL_INK);
  graphics_draw_text(ctx, "SELECT · BACK TO SKIP", fonts_get_system_font(F_BODY_B),
                     GRect(8, h - 14 - LINE_H - 5, w - 16, LINE_H), GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void up(ClickRecognizerRef r, void *c)   { if (s_sel < 5) s_sel++; layer_mark_dirty(s_layer); }
static void down(ClickRecognizerRef r, void *c) { if (s_sel > 1) s_sel--; layer_mark_dirty(s_layer); }
static void select(ClickRecognizerRef r, void *c) {
  mark_asked();
  uint8_t f = (uint8_t)s_sel;
  window_stack_pop(true);
  if (s_cb) s_cb(f);
}
static void back(ClickRecognizerRef r, void *c) {
  mark_asked();                      // skipped: never nagged about
  window_stack_pop(true);
}
static void clicks(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, up);
  window_single_click_subscribe(BUTTON_ID_DOWN, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, select);
  window_single_click_subscribe(BUTTON_ID_BACK, back);
}
static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
}
static void unload(Window *w) {
  layer_destroy(s_layer); s_layer = NULL;
  window_destroy(s_win);  s_win = NULL;
}

void feel_show(FeelAnswerCb cb) {
  s_cb  = cb;
  s_sel = 3;
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers){ .load = load, .unload = unload });
  window_stack_push(s_win, true);
}
