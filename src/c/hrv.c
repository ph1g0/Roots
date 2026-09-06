#include "hrv.h"
#include "common.h"
#include "history.h"
#include "theme.h"

// ------------------------------------------------------------------
// Protocol. Consistency is what makes a personal HRV baseline usable, so the
// test is the same length, with the same breathing pattern, every time.
// ------------------------------------------------------------------
#define HRV_SETTLE_S        30    // sit still, sensor warms up, nothing recorded
#define HRV_MEASURE_S       120   // intervals recorded
#define HRV_PACED           1     // 1 = guided breathing during the measure phase
#define HRV_BREATH_IN_MS    5000  // 5 in + 5 out = 6 breaths/min
#define HRV_BREATH_OUT_MS   5000
#define HRV_TICK_MS         100

#define PPI_MIN_MS          300   // 200 bpm
#define PPI_MAX_MS          2000  // 30 bpm
#define PPI_REL_REJECT_PCT  25    // vs the running median: PPG artefact gate
#define PPI_MAX             400
#define MIN_BEATS           40    // fewer than this and the number is not shown

typedef enum { PH_SETTLE, PH_MEASURE, PH_DONE } Phase;

static Window   *s_win;
static Layer    *s_layer;
static AppTimer *s_timer;
static Phase     s_phase;
static uint32_t  s_elapsed_ms;      // in the current phase
static uint16_t  s_ppi[PPI_MAX];
static uint16_t  s_n, s_rej;
static int32_t   s_last_raw = -1;
static uint16_t  s_live_hr;
static HrvResult s_res;
static bool      s_open;

// ---------------------------------------------------------------- maths
static uint32_t isqrt32(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
    bit >>= 2;
  }
  return r;
}

static uint16_t median5(const uint16_t *a, int n) {   // median of the last min(n,5)
  uint16_t t[5]; int k = n < 5 ? n : 5;
  for (int i = 0; i < k; i++) t[i] = a[n - k + i];
  for (int i = 1; i < k; i++) { uint16_t v = t[i]; int j = i - 1; while (j >= 0 && t[j] > v) { t[j+1] = t[j]; j--; } t[j+1] = v; }
  return t[k / 2];
}

static void accept_interval(int32_t ms) {
  if (ms < PPI_MIN_MS || ms > PPI_MAX_MS) { s_rej++; return; }
  if (ms == s_last_raw) return;             // same beat reported twice; not a beat
  s_last_raw = ms;
  if (s_n >= 3) {
    int med = median5(s_ppi, s_n);
    int d = (int)ms - med; if (d < 0) d = -d;
    if (d * 100 > med * PPI_REL_REJECT_PCT) { s_rej++; return; }
  }
  if (s_n < PPI_MAX) s_ppi[s_n++] = (uint16_t)ms;
}

static void compute(void) {
  memset(&s_res, 0, sizeof s_res);
  s_res.schema = HRV_SCHEMA;
  s_res.beats = s_n; s_res.rejected = s_rej; s_res.paced = HRV_PACED;
  s_res.day = history_day_key();
  time_t now = time(NULL); struct tm *lt = localtime(&now);
  s_res.minute_of_day = (uint16_t)(lt->tm_hour * 60 + lt->tm_min);
  if (s_n < MIN_BEATS) return;

  uint32_t sum = 0;
  for (int i = 0; i < s_n; i++) sum += s_ppi[i];
  uint32_t mean = sum / s_n;
  s_res.mean_hr = (uint8_t)(60000 / (mean ? mean : 1));

  uint32_t sq = 0;
  for (int i = 1; i < s_n; i++) { int d = (int)s_ppi[i] - (int)s_ppi[i-1]; sq += (uint32_t)(d * d); }
  uint32_t rmssd = isqrt32(sq / (s_n - 1));
  s_res.rmssd_ms = (uint8_t)(rmssd > 255 ? 255 : rmssd);

  uint32_t var = 0;
  for (int i = 0; i < s_n; i++) { int d = (int)s_ppi[i] - (int)mean; var += (uint32_t)(d * d); }
  uint32_t sdnn = isqrt32(var / s_n);
  s_res.sdnn_ms = (uint8_t)(sdnn > 255 ? 255 : sdnn);

  persist_write_data(KEY_HRV_LAST, &s_res, sizeof s_res);
  DayRecord *rec = history_today();
  rec->hrv_rmssd = s_res.rmssd_ms;
  rec->hrv_sdnn  = s_res.sdnn_ms;
  rec->hrv_hr    = s_res.mean_hr;
  history_save();
}

bool hrv_last(HrvResult *out) {
  if (!persist_exists(KEY_HRV_LAST) || persist_get_size(KEY_HRV_LAST) != (int)sizeof(HrvResult)) return false;
  persist_read_data(KEY_HRV_LAST, out, sizeof(HrvResult));
  return out->schema == HRV_SCHEMA;
}

// ---------------------------------------------------------------- sensor
#ifndef ROOTS_NO_HRV
static void sensor_on(void)  { health_service_set_hrv_sample_period(1); health_service_set_heart_rate_sample_period(1); }
static void sensor_off(void) { health_service_set_hrv_sample_period(0); health_service_set_heart_rate_sample_period(0); }
static int32_t peek_ppi(void) { return (int32_t)health_service_peek_hrv_ppi_ms(); }
#else
static void sensor_on(void)  { health_service_set_heart_rate_sample_period(1); }
static void sensor_off(void) { health_service_set_heart_rate_sample_period(0); }
static int32_t peek_ppi(void) { return 0; }
#endif

void hrv_health_event(HealthEventType e) {
  if (!s_open) return;
#ifndef ROOTS_NO_HRV
  if (e == HealthEventHRVUpdate && s_phase == PH_MEASURE) accept_interval(peek_ppi());
#endif
  if (e == HealthEventHeartRateUpdate) {
    HealthValue v = health_service_peek_current_value(HealthMetricHeartRateRawBPM);
    if (v > 0) { s_live_hr = (uint16_t)v; if (s_layer) layer_mark_dirty(s_layer); }
  }
}

bool hrv_is_open(void) { return s_open; }

// ---------------------------------------------------------------- timing
static void tick(void *c) {
  s_timer = NULL;
  s_elapsed_ms += HRV_TICK_MS;
  if (s_phase == PH_SETTLE && s_elapsed_ms >= HRV_SETTLE_S * 1000) {
    s_phase = PH_MEASURE; s_elapsed_ms = 0; s_n = 0; s_rej = 0; s_last_raw = -1;
    vibes_short_pulse();
  } else if (s_phase == PH_MEASURE && s_elapsed_ms >= HRV_MEASURE_S * 1000) {
    s_phase = PH_DONE;
    sensor_off();
    compute();
    vibes_double_pulse();
  }
  if (s_layer) layer_mark_dirty(s_layer);
  if (s_phase != PH_DONE) s_timer = app_timer_register(HRV_TICK_MS, tick, NULL);
}

// ---------------------------------------------------------------- drawing
static void txt(GContext *ctx, const char *s, const char *font, GRect r, GTextAlignment a) {
  graphics_draw_text(ctx, s, fonts_get_system_font(font), r, GTextOverflowModeTrailingEllipsis, a, NULL);
}

static void draw_running(GContext *ctx, GRect b) {
  int w = b.size.w, h = b.size.h;
  GColor ink  = C_INK;
  GColor mute = C_MUTE;
  GColor ring = C_ACCENT2;
  char v[32];

  bool settle = s_phase == PH_SETTLE;
  int total_s = settle ? HRV_SETTLE_S : HRV_MEASURE_S;
  int left_s  = total_s - (int)(s_elapsed_ms / 1000);
  if (left_s < 0) left_s = 0;

  graphics_context_set_text_color(ctx, ink);
  txt(ctx, settle ? "Settling" : "Measuring", F_TITLE, GRect(4, 2, w - 8, 30), GTextAlignmentCenter);

  // Breathing circle. During settle it just sits there.
  int rmin = w >= 180 ? 22 : 16, rmax = w >= 180 ? 56 : 40;
  int r = rmin;
  const char *cue = "Sit still";
  if (!settle && HRV_PACED) {
    uint32_t cyc = s_elapsed_ms % (HRV_BREATH_IN_MS + HRV_BREATH_OUT_MS);
    if (cyc < HRV_BREATH_IN_MS) { r = rmin + (int)((rmax - rmin) * cyc / HRV_BREATH_IN_MS); cue = "Breathe in"; }
    else { uint32_t o = cyc - HRV_BREATH_IN_MS; r = rmax - (int)((rmax - rmin) * o / HRV_BREATH_OUT_MS); cue = "Breathe out"; }
  } else if (!settle) {
    cue = "Breathe normally";
  }
  GPoint c = GPoint(w / 2, h / 2 - 6);
  graphics_context_set_stroke_color(ctx, C_TRACK);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, c, rmax);
  graphics_context_set_fill_color(ctx, ring);
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_stroke_width(ctx, 1);

  // Countdown sits in the circle; black on the bright fill, ink when the
  // circle is small enough that the text would spill onto the background.
  snprintf(v, sizeof v, "%d", left_s);
  graphics_context_set_text_color(ctx, r >= rmin + 10 ? GColorBlack : ink);
  txt(ctx, v, FONT_KEY_GOTHIC_28_BOLD, GRect(c.x - 34, c.y - 18, 68, 34), GTextAlignmentCenter);

  graphics_context_set_text_color(ctx, ink);
  txt(ctx, cue, F_TITLE, GRect(4, c.y + rmax + 4, w - 8, 30), GTextAlignmentCenter);

  graphics_context_set_text_color(ctx, mute);
  if (s_live_hr) snprintf(v, sizeof v, "%u bpm  ·  %u beats", s_live_hr, s_n);
  else           snprintf(v, sizeof v, "no sensor yet  ·  %u beats", s_n);
  txt(ctx, v, F_BODY, GRect(4, h - 28, w - 8, LINE_H), GTextAlignmentCenter);
}

static void draw_done(GContext *ctx, GRect b) {
  int w = b.size.w, h = b.size.h;
  GColor ink  = C_INK;
  GColor mute = C_MUTE;
  char v[40];
  int y = 2;

  graphics_context_set_text_color(ctx, ink);
  txt(ctx, "HRV test", F_TITLE, GRect(4, y, w - 8, 30), GTextAlignmentCenter);
  y += 36;

  if (s_res.rmssd_ms == 0) {
    txt(ctx, "--", FONT_KEY_BITHAM_42_LIGHT, GRect(0, y, w, 48), GTextAlignmentCenter);
    y += 52;
    snprintf(v, sizeof v, "%u clean beats, need %d.", s_n, MIN_BEATS);
    txt(ctx, v, F_BODY_B, GRect(4, y, w - 8, LINE_H), GTextAlignmentCenter);
    y += LINE_H + 4;
    graphics_draw_text(ctx, "Tighten the strap a notch and keep the wrist still.",
                       fonts_get_system_font(F_BODY), GRect(8, y, w - 16, 3 * LINE_H),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }
  snprintf(v, sizeof v, "%u", s_res.rmssd_ms);
  txt(ctx, v, w >= 180 ? FONT_KEY_LECO_42_NUMBERS : FONT_KEY_LECO_36_BOLD_NUMBERS, GRect(0, y, w, 48), GTextAlignmentCenter);
  y += 50;
  graphics_context_set_text_color(ctx, mute);
  txt(ctx, "ms RMSSD", F_BODY, GRect(4, y, w - 8, LINE_H), GTextAlignmentCenter);
  y += LINE_H + 6;

  // Against your recent tests, if there are any.
  int cnt = 0; uint32_t sum = 0;
  for (int ago = 1; ago <= 30 && cnt < 7; ago++) {
    const DayRecord *r = history_get(ago);
    if (r && r->hrv_rmssd) { sum += r->hrv_rmssd; cnt++; }
  }
  graphics_context_set_text_color(ctx, ink);
  if (cnt >= 2) {
    int d = (int)s_res.rmssd_ms - (int)(sum / cnt);
    snprintf(v, sizeof v, "%+d ms vs your last %d", d, cnt);
  } else {
    snprintf(v, sizeof v, "First tests set the baseline");
  }
  txt(ctx, v, F_BODY_B, GRect(4, y, w - 8, LINE_H), GTextAlignmentCenter);
  y += LINE_H + 4;

  graphics_context_set_text_color(ctx, mute);
  snprintf(v, sizeof v, "%u bpm  ·  %u beats", s_res.mean_hr, s_res.beats);
  txt(ctx, v, F_BODY, GRect(4, y, w - 8, LINE_H), GTextAlignmentCenter);
  y += LINE_H;
  snprintf(v, sizeof v, "SDNN %u  ·  %u dropped", s_res.sdnn_ms, s_res.rejected);
  txt(ctx, v, F_BODY, GRect(4, y, w - 8, LINE_H), GTextAlignmentCenter);
  txt(ctx, "Back to close", F_BODY, GRect(4, h - 28, w - 8, LINE_H), GTextAlignmentCenter);
}

static void draw(Layer *l, GContext *ctx) {
  GRect b = layer_get_bounds(l);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  if (s_phase == PH_DONE) draw_done(ctx, b); else draw_running(ctx, b);
}

// ---------------------------------------------------------------- window
static void load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, draw);
  layer_add_child(root, s_layer);

  s_phase = PH_SETTLE; s_elapsed_ms = 0; s_n = 0; s_rej = 0; s_last_raw = -1; s_live_hr = 0;
  s_open = true;
  sensor_on();
  s_timer = app_timer_register(HRV_TICK_MS, tick, NULL);
}
static void unload(Window *w) {
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  sensor_off();                      // always, even on an abandoned test
  s_open = false;
  layer_destroy(s_layer); s_layer = NULL;
  window_destroy(s_win);  s_win = NULL;
}

void hrv_show(void) {
  if (s_win) return;
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_window_handlers(s_win, (WindowHandlers){ .load = load, .unload = unload });
  window_stack_push(s_win, true);
}
