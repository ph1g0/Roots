#include "metric.h"
#include "common.h"
#include "history.h"
#include "settings.h"
#include "theme.h"

static MetricPoint s_p[METRIC_DAYS];
static int         s_n;
static bool        s_loaded;

// ---------------------------------------------------------------- store
void metric_load(void) {
  if (s_loaded) return;
  s_loaded = true;
  s_n = 0;
  for (int c = 0; c < METRIC_CHUNKS && s_n < METRIC_DAYS; c++) {
    uint32_t key = KEY_METRIC_CHUNK0 + c;
    if (!persist_exists(key)) break;
    int recs = persist_get_size(key) / (int)sizeof(MetricPoint);
    if (recs <= 0) break;
    if (recs > METRIC_DAYS - s_n) recs = METRIC_DAYS - s_n;
    persist_read_data(key, &s_p[s_n], recs * sizeof(MetricPoint));
    s_n += recs;
  }
  int w = 0;                                   // ascending, no blanks
  for (int i = 0; i < s_n; i++) {
    if (s_p[i].day == 0 || s_p[i].value == 0) continue;
    if (w > 0 && s_p[i].day <= s_p[w - 1].day) continue;
    s_p[w++] = s_p[i];
  }
  s_n = w;
}

static void save(void) {
  int done = 0;
  for (int c = 0; c < METRIC_CHUNKS; c++) {
    uint32_t key = KEY_METRIC_CHUNK0 + c;
    int recs = s_n - done;
    if (recs > METRIC_PER_CHUNK) recs = METRIC_PER_CHUNK;
    if (recs <= 0) { if (persist_exists(key)) persist_delete(key); continue; }
    persist_write_data(key, &s_p[done], recs * sizeof(MetricPoint));
    done += recs;
  }
}

void metric_set_today(uint16_t value) {
  metric_load();
  uint16_t day = history_day_key();
  int i = s_n;
  while (i > 0 && s_p[i - 1].day > day) i--;
  if (i > 0 && s_p[i - 1].day == day) {        // re-weighed: last one wins
    s_p[i - 1].value = value;
    save();
    return;
  }
  if (s_n == METRIC_DAYS) {
    if (i == 0) return;
    memmove(&s_p[0], &s_p[1], (METRIC_DAYS - 1) * sizeof(MetricPoint));
    s_n--; i--;
  }
  memmove(&s_p[i + 1], &s_p[i], (s_n - i) * sizeof(MetricPoint));
  s_p[i].day   = day;
  s_p[i].value = value;
  s_n++;
  save();
}

bool metric_get(int days_ago, uint16_t *value) {
  metric_load();
  uint16_t want = history_day_key() - (uint16_t)days_ago;
  for (int i = s_n - 1; i >= 0; i--) {
    if (s_p[i].day == want) { *value = s_p[i].value; return true; }
    if (s_p[i].day < want) break;
  }
  return false;
}

bool metric_last(uint16_t *value, int *days_ago) {
  metric_load();
  if (s_n == 0) return false;
  *value    = s_p[s_n - 1].value;
  *days_ago = (int)history_day_key() - (int)s_p[s_n - 1].day;
  return true;
}

bool metric_avg(int from_days_ago, int to_days_ago, uint16_t *out) {
  metric_load();
  uint16_t today = history_day_key();
  uint32_t sum = 0; int cnt = 0;
  for (int i = 0; i < s_n; i++) {
    int ago = (int)today - (int)s_p[i].day;
    if (ago < from_days_ago || ago > to_days_ago) continue;
    sum += s_p[i].value; cnt++;
  }
  if (!cnt) return false;
  *out = (uint16_t)(sum / cnt);
  return true;
}

int metric_series(int *out, int days) {
  metric_load();
  if (days > METRIC_DAYS) days = METRIC_DAYS;
  uint16_t today = history_day_key();
  for (int i = 0; i < days; i++) out[i] = 0;
  for (int i = 0; i < s_n; i++) {
    int ago = (int)today - (int)s_p[i].day;
    if (ago < 0 || ago >= days) continue;
    out[days - 1 - ago] = metric_to_display(s_p[i].value);
  }
  return days;
}

// ---------------------------------------------------------------- units
// 1 kg = 2.2046 lb. Kept in integers; the display rounds to 0.1 either way,
// so a value entered in pounds and read back in pounds is unchanged.
const char *metric_name(void) {
  return settings()->metric == METRIC_WEIGHT ? "WEIGHT" : "";
}
const char *metric_unit(void) {
  if (settings()->metric != METRIC_WEIGHT) return "";
  return settings()->units == UNITS_IMPERIAL ? "lb" : "kg";
}
uint16_t metric_to_display(uint16_t c) {
  if (settings()->units != UNITS_IMPERIAL) return c;
  return (uint16_t)(((uint32_t)c * 22046 + 5000) / 10000);
}
uint16_t metric_from_display(uint16_t d) {
  if (settings()->units != UNITS_IMPERIAL) return d;
  return (uint16_t)(((uint32_t)d * 10000 + 11023) / 22046);
}
uint16_t metric_step_x10(void) { return 1; }                                   // 0.1
uint16_t metric_min_x10(void)  { return settings()->units == UNITS_IMPERIAL ?  660 :  300; }
uint16_t metric_max_x10(void)  { return settings()->units == UNITS_IMPERIAL ? 6600 : 3000; }

// ---------------------------------------------------------------- entry
static Window        *s_win;
static Layer         *s_layer;
static uint16_t       s_val;          // display units x10
static MetricEntryCb  s_done;

static void draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  int w = b.size.w, h = b.size.h;
  char v[24];

  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  graphics_context_set_text_color(ctx, C_MUTE);
  graphics_draw_text(ctx, metric_name(), fonts_get_system_font(F_BODY_B),
                     GRect(0, 9, w, LINE_H), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  snprintf(v, sizeof v, "%u.%u", s_val / 10, s_val % 10);
  graphics_context_set_text_color(ctx, C_ACCENT);
  graphics_draw_text(ctx, v, fonts_get_system_font(w >= 180 ? F_BIG : F_BIG_S),
                     GRect(0, h / 2 - 44, w, 56), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, C_MUTE);
  graphics_draw_text(ctx, metric_unit(), fonts_get_system_font(F_BODY),
                     GRect(0, h / 2 + 12, w, LINE_H), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);

  graphics_context_set_fill_color(ctx, C_PILL);
  graphics_fill_rect(ctx, GRect(8, h - 14 - LINE_H - 6, w - 16, LINE_H + 6), 4, GCornersAll);
  graphics_context_set_text_color(ctx, C_PILL_INK);
  graphics_draw_text(ctx, "SELECT \xc2\xb7 BACK TO SKIP", fonts_get_system_font(F_BODY_B),
                     GRect(8, h - 14 - LINE_H - 5, w - 16, LINE_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void bump(int by) {
  int v = (int)s_val + by;
  if (v < (int)metric_min_x10()) v = metric_min_x10();
  if (v > (int)metric_max_x10()) v = metric_max_x10();
  s_val = (uint16_t)v;
  layer_mark_dirty(s_layer);
}
static void up(ClickRecognizerRef r, void *c)   { bump( (int)metric_step_x10()); }
static void down(ClickRecognizerRef r, void *c) { bump(-(int)metric_step_x10()); }
static void save_click(ClickRecognizerRef r, void *c) {
  metric_set_today(metric_from_display(s_val));
  window_stack_pop(true);
  if (s_done) s_done();
}
static void clicks(void *c) {
  // Held: 0.1 a press for the first second, then it runs. A kilo is ten
  // presses either way, which is about as much as four buttons can do well.
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down);
  window_single_click_subscribe(BUTTON_ID_SELECT, save_click);
}
static void load_win(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);
}
static void unload_win(Window *w) {
  layer_destroy(s_layer); s_layer = NULL;
  window_destroy(s_win);  s_win = NULL;
}

void metric_entry_show(MetricEntryCb done) {
  if (s_win) return;
  s_done = done;

  // Start from the last value you entered, not from a round number: the
  // change since yesterday is usually a few presses, and starting at 70.0
  // every time would make it twenty.
  uint16_t last; int ago;
  s_val = metric_last(&last, &ago) ? metric_to_display(last)
                                   : (settings()->units == UNITS_IMPERIAL ? 1650 : 750);

  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_click_config_provider(s_win, clicks);
  window_set_window_handlers(s_win, (WindowHandlers){ .load = load_win, .unload = unload_win });
  window_stack_push(s_win, true);
}
