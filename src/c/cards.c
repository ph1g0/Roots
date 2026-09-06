#include "cards.h"
#include "common.h"
#include "history.h"
#include "hrv.h"
#include "metric.h"
#include "settings.h"
#include "theme.h"

// The visual vocabulary, borrowed from Pebble Health and used on every card:
//
//   ^  chevron              you can go up
//   TITLE                   which card, small caps, muted
//   4579  bpm               the figure, big and bold, in the card's colour
//   [ YOUR AVERAGE DAY 5748 ] the pill: one comparison, black on yellow
//   Today      ████████|    bars: the last few days, white tick = your usual
//   Wed 54     ██████  |
//   v  chevron              you can go down
//
// Nothing is smaller than Gothic 18. All text is white (labels light grey);
// colour is only ever in bars, rings and the pill. Text explains the
// graphic; it never replaces it. The detail behind SELECT is one graphic and three or four
// rows, not a log.

#define TOP_H   34    // chevron + title
#define BOT_H   14    // chevron
#define BAR_ROW 26    // label line + bar

// bpm x10 between the short and long baselines before the Trend card calls it
// anything. Under this it says you are on your long-term average.
#define DRIFT_X10 15

// ---------------------------------------------------------------- helpers
static void txt(GContext *ctx, const char *s, const char *font, GRect r,
                GTextAlignment a, GTextOverflowMode o) {
  graphics_draw_text(ctx, s, fonts_get_system_font(font), r, o, a, NULL);
}

static bool tall(GRect b)       { return b.size.h >= 200; }
static const char *f_big(int w) { return w >= 180 ? F_BIG : F_BIG_S; }
static int big_h(int w)         { return w >= 180 ? 48 : 36; }

static void fmt_thousands(char *b, size_t n, int32_t v) {
  if (v >= 1000) snprintf(b, n, "%ld,%03ld", (long)(v / 1000), (long)(v % 1000));
  else           snprintf(b, n, "%ld", (long)v);
}
static void fmt_x10(char *b, size_t n, int x10) {
  int a = x10 < 0 ? -x10 : x10;
  snprintf(b, n, "%s%d.%d", x10 < 0 ? "-" : "", a / 10, a % 10);
}
static void fmt_hm(char *b, size_t n, uint16_t minutes) {
  snprintf(b, n, "%uh %02um", minutes / 60, minutes % 60);
}
static void fmt_clock(char *b, size_t n, uint16_t minute_of_day) {
  snprintf(b, n, "%02u:%02u", minute_of_day / 60, minute_of_day % 60);
}
static const char *wday_name(int wday) {
  static const char *n[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  return n[((wday % 7) + 7) % 7];
}

static int today_wday(void) {
  time_t t = time_start_of_today();
  return localtime(&t)->tm_wday;
}
static int frac_pct(int v, int lo, int hi) {   // 0..100 of v within [lo, hi]
  if (hi <= lo) return 0;
  int p = ((v - lo) * 100) / (hi - lo);
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

// ---------------------------------------------------------------- vocabulary
static void chevron(GContext *ctx, GRect b, bool up) {
  int cx = b.size.w / 2, y = up ? 5 : b.size.h - 6;
  graphics_context_set_stroke_color(ctx, C_MUTE);
  for (int i = 0; i < 5; i++) {
    int yy = up ? y + i : y - i;
    graphics_draw_line(ctx, GPoint(cx - i - 1, yy), GPoint(cx + i + 1, yy));
  }
}

static void title(GContext *ctx, GRect b, const char *t) {
  graphics_context_set_text_color(ctx, C_MUTE);
  txt(ctx, t, F_BODY_B, GRect(0, 9, b.size.w, LINE_H), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
}

// The big figure with a unit hanging off its baseline. Always white: text
// carries the meaning, so it gets the most contrast the display has. The
// card's colour lives in the bars and rings under it, never in the words.
static int figure(GContext *ctx, int y, int w, const char *big, const char *unit) {
  int h = big_h(w);
  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, big, f_big(w), GRect(8, y - 6, w - 16, h + 8), GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  if (unit && unit[0]) {
    GSize sz = graphics_text_layout_get_content_size(big, fonts_get_system_font(f_big(w)),
                  GRect(0, 0, w, h + 8), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
    graphics_context_set_text_color(ctx, C_MUTE);
    txt(ctx, unit, F_BODY, GRect(8 + sz.w + 6, y + h - LINE_H - 4, w - sz.w - 20, LINE_H),
        GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  }
  return y + h - 4;
}

// One comparison in a filled box. Two short lines at most.
static int pill(GContext *ctx, int y, int w, const char *l1, const char *l2) {
  int h = l2 && l2[0] ? 2 * LINE_H + 6 : LINE_H + 6;
  graphics_context_set_fill_color(ctx, C_PILL);
  graphics_fill_rect(ctx, GRect(8, y, w - 16, h), 4, GCornersAll);
  graphics_context_set_text_color(ctx, C_PILL_INK);
  txt(ctx, l1, F_BODY_B, GRect(8, y + 1, w - 16, LINE_H), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
  if (l2 && l2[0])
    txt(ctx, l2, F_BODY_B, GRect(8, y + 1 + LINE_H, w - 16, LINE_H), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
  return y + h + 6;
}

// A day row: label and value on one line, a bar under it. pct is the fill,
// tick_pct is where the white "usual" mark goes (-1 for none).
static int bar_row(GContext *ctx, int y, int w, const char *label, const char *value,
                   int pct, int tick_pct, GColor col) {
  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, label, F_BODY, GRect(8, y - 2, w / 2, LINE_H), GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  txt(ctx, value, F_BODY_B, GRect(w / 2, y - 2, w / 2 - 8, LINE_H), GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  int by = y + LINE_H - 4, bh = 8, x0 = 8, bw = w - 16;
  graphics_context_set_fill_color(ctx, C_TRACK);
  graphics_fill_rect(ctx, GRect(x0, by, bw, bh), 2, GCornersAll);
  if (pct > 0) {
    graphics_context_set_fill_color(ctx, col);
    graphics_fill_rect(ctx, GRect(x0, by, (bw * pct) / 100, bh), 2, GCornersAll);
  }
  if (tick_pct >= 0) {
    int tx = x0 + (bw * tick_pct) / 100;
    graphics_context_set_fill_color(ctx, C_INK);
    graphics_fill_rect(ctx, GRect(tx - 1, by - 2, 3, bh + 4), 0, GCornerNone);
  }
  return y + BAR_ROW;
}

// Vertical bars, oldest left. v[i] <= 0 draws nothing. ref draws a dashed
// line. Used for anything with more than a handful of days.
static int vbars(GContext *ctx, int y, int w, const char *heading, const int *v, int n,
                 int ref, GColor col, int height) {
  if (heading && heading[0]) {
    graphics_context_set_text_color(ctx, C_INK);
    txt(ctx, heading, F_BODY_B, GRect(8, y, w - 16, LINE_H), GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
    y += LINE_H + 2;
  }
  int lo = 0x7fffffff, hi = 0, present = 0;
  for (int i = 0; i < n; i++) if (v[i] > 0) { present++; if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
  if (ref > 0) { if (ref < lo) lo = ref; if (ref > hi) hi = ref; }
  int x0 = 8, x1 = w - 8, top = y, bottom = y + height;
  graphics_context_set_stroke_color(ctx, C_TRACK);
  graphics_draw_line(ctx, GPoint(x0, bottom), GPoint(x1, bottom));
  if (present == 0) {
    graphics_context_set_text_color(ctx, C_MUTE);
    txt(ctx, "no data yet", F_BODY, GRect(x0, y + height / 2 - 11, x1 - x0, LINE_H), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
    return bottom + 8;
  }
  // Bars start a little below the lowest value so differences are visible,
  // but never from zero for a quantity that is never zero (heart rate).
  int span = hi - lo; if (span < 4) span = 4;
  int floor_v = lo - span / 2; if (floor_v < 0) floor_v = 0;
  int range = hi - floor_v; if (range < 1) range = 1;
  int slot = (x1 - x0) / n, bw = slot - 2; if (bw < 2) bw = 2;
  graphics_context_set_fill_color(ctx, col);
  for (int i = 0; i < n; i++) {
    if (v[i] <= 0) continue;
    int bh = ((v[i] - floor_v) * (bottom - top)) / range; if (bh < 2) bh = 2;
    graphics_fill_rect(ctx, GRect(x0 + i * slot + 1, bottom - bh, bw, bh), 1, GCornersTop);
  }
  if (ref > 0) {
    int ry = bottom - ((ref - floor_v) * (bottom - top)) / range;
    graphics_context_set_stroke_color(ctx, C_INK);
    for (int x = x0; x < x1; x += 6) graphics_draw_line(ctx, GPoint(x, ry), GPoint(x + 3, ry));
  }
  return bottom + 8;
}

// A heart-rate curve drawn as a filled area under a line. v[i] <= 0 = gap.
static int area(GContext *ctx, int y, int w, const char *heading, const uint8_t *curve,
                int ref, GColor col, int height) {
  if (heading && heading[0]) {
    graphics_context_set_text_color(ctx, C_INK);
    txt(ctx, heading, F_BODY_B, GRect(8, y, w - 16, LINE_H), GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
    y += LINE_H + 2;
  }
  int lo = 255, hi = 0, present = 0;
  for (int i = 0; i < CURVE_N; i++) if (curve[i]) { present++; if (curve[i] < lo) lo = curve[i]; if (curve[i] > hi) hi = curve[i]; }
  if (ref > 0) { if (ref < lo) lo = ref; if (ref > hi) hi = ref; }
  int x0 = 40, x1 = w - 8, top = y + 2, bottom = y + height;
  graphics_context_set_stroke_color(ctx, C_TRACK);
  graphics_draw_line(ctx, GPoint(x0, bottom), GPoint(x1, bottom));
  if (present == 0) {
    graphics_context_set_text_color(ctx, C_MUTE);
    txt(ctx, "no readings", F_BODY, GRect(x0, y + height / 2 - 11, x1 - x0, LINE_H), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);
    return bottom + 8;
  }
  lo -= 4; hi += 4; if (lo < 0) lo = 0;
  int range = hi - lo; if (range < 1) range = 1;
  char lab[8];
  graphics_context_set_text_color(ctx, C_MUTE);
  snprintf(lab, sizeof lab, "%d", hi); txt(ctx, lab, F_BODY, GRect(0, top - 10, x0 - 4, LINE_H), GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  snprintf(lab, sizeof lab, "%d", lo); txt(ctx, lab, F_BODY, GRect(0, bottom - 12, x0 - 4, LINE_H), GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  int slot = (x1 - x0) / CURVE_N; if (slot < 1) slot = 1;
  graphics_context_set_fill_color(ctx, col);
  for (int i = 0; i < CURVE_N; i++) {
    if (!curve[i]) continue;
    int h = ((curve[i] - lo) * (bottom - top)) / range; if (h < 1) h = 1;
    graphics_fill_rect(ctx, GRect(x0 + i * slot, bottom - h, slot, h), 0, GCornerNone);
  }
  if (ref > 0) {
    int ry = bottom - ((ref - lo) * (bottom - top)) / range;
    graphics_context_set_stroke_color(ctx, C_INK);
    for (int x = x0; x < x1; x += 6) graphics_draw_line(ctx, GPoint(x, ry), GPoint(x + 3, ry));
  }
  return bottom + 8;
}

static int row_kv(GContext *ctx, int y, int w, const char *label, const char *value) {
  graphics_context_set_text_color(ctx, C_MUTE);
  txt(ctx, label, F_BODY, GRect(8, y, (w * 3) / 5 - 8, LINE_H), GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, value, F_BODY_B, GRect((w * 3) / 5 - 8, y, (w * 2) / 5, LINE_H), GTextAlignmentRight, GTextOverflowModeTrailingEllipsis);
  return y + 26;
}

static int line(GContext *ctx, int y, int w, int lines, const char *s) {
  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, s, F_BODY, GRect(8, y, w - 16, lines * LINE_H), GTextAlignmentLeft, GTextOverflowModeWordWrap);
  return y + lines * LINE_H;
}

// History series helpers -----------------------------------------------
typedef enum { H_RHR, H_SLEEP, H_SCORE, H_HRV, H_FEEL } HistField;
static int hist_series(int *out, int days, HistField f) {
  int cnt; const DayRecord *recs = history_records(&cnt);
  int first_ago = cnt > 0 ? (int)history_day_key() - (int)recs[0].day : 0;
  int n = first_ago + 1; if (n > days) n = days; if (n < 7) n = 7;
  for (int i = 0; i < n; i++) {
    const DayRecord *r = history_get(n - 1 - i);
    int v = 0;
    if (r) switch (f) {
      case H_RHR:   v = r->rhr_x10;   break;
      case H_SLEEP: v = r->sleep_min; break;
      case H_SCORE: v = r->score;     break;
      case H_HRV:   v = r->hrv_rmssd; break;
      case H_FEEL:  v = r->feel * 20; break;
    }
    out[i] = v;
  }
  return n;
}

// Label for "days_ago" as Pebble Health writes it: Today, Yesterday, Wed.
static const char *ago_label(int ago) {
  if (ago == 0) return "Today";
  if (ago == 1) return "Yesterday";
  return wday_name(today_wday() - ago);
}

// ---------------------------------------------------------------- main cards
const char *card_title(int card) {
  switch (card) {
    case CARD_HEADROOM: return "HEADROOM";
    case CARD_DRAIN:    return "NIGHT HEART RATE";
    case CARD_RECHARGE: return "SLEEP";
    case CARD_TRENDS:   return "TREND";
    case CARD_HRV:      return "HRV";
    case CARD_STEPS:    return "STEPS";
    case CARD_METRIC:   return metric_name();
    case CARD_DATA:     return "DATA";
    default:            return "";
  }
}

// TODAY: the ring, the number, the band in a pill, one sentence.
static void draw_headroom(GContext *ctx, GRect b, const Headroom *h) {
  int w = b.size.w, hh = b.size.h;
  int r = tall(b) ? 46 : 34;
  GPoint c = GPoint(w / 2, TOP_H + r + 2);

  GRect ring = GRect(c.x - r, c.y - r, 2 * r, 2 * r);
  graphics_context_set_stroke_width(ctx, tall(b) ? 12 : 9);
  graphics_context_set_stroke_color(ctx, C_TRACK);
  graphics_draw_arc(ctx, ring, GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);
  if (h->valid) {
    graphics_context_set_stroke_color(ctx, C_ACCENT);
    graphics_draw_arc(ctx, ring, GOvalScaleModeFitCircle, 0, (TRIG_MAX_ANGLE * h->score) / 100);
  }
  graphics_context_set_stroke_width(ctx, 1);

  char num[8];
  if (h->valid) snprintf(num, sizeof num, "%u", h->score);
  else          strncpy(num, "--", sizeof num);
  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, num, w >= 180 ? FONT_KEY_LECO_42_NUMBERS : FONT_KEY_LECO_36_BOLD_NUMBERS,
      GRect(c.x - r, c.y - (w >= 180 ? 28 : 24), 2 * r, 48), GTextAlignmentCenter, GTextOverflowModeTrailingEllipsis);

  int y = c.y + r + 8;
  if (h->valid) {
    char caps[24]; const char *bn = band_name(h->band);
    for (int i = 0; bn[i] && i < 23; i++) caps[i] = (bn[i] >= 'a' && bn[i] <= 'z') ? bn[i] - 32 : bn[i], caps[i + 1] = 0;
    y = pill(ctx, y, w, caps, "");
    if (hh - BOT_H - y >= LINE_H) line(ctx, y, w, (hh - BOT_H - y) / LINE_H, band_line(h->band));
  } else if (h->night.night_rhr_x10) {
    char v[24];
    snprintf(v, sizeof v, "NIGHT %u BPM", (h->night.night_rhr_x10 + 5) / 10);
    y = pill(ctx, y, w, "FIRST NIGHT", v);
    if (hh - BOT_H - y >= LINE_H) line(ctx, y, w, (hh - BOT_H - y) / LINE_H, "Your number arrives tomorrow.");
  } else {
    y = pill(ctx, y, w, "NO NIGHT READ", "");
    if (hh - BOT_H - y >= LINE_H) line(ctx, y, w, (hh - BOT_H - y) / LINE_H, "The Data card says why.");
  }
}

// NIGHT HEART RATE: +/- vs baseline, then the last three nights as bars.
// Longer bar = more reserve (a lower night), tick = your baseline.
static void draw_drain(GContext *ctx, GRect b, const Headroom *h) {
  const NightResult *n = &h->night;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[48], p2[48] = "", t[16];

  if (n->night_rhr_x10 == 0 || n->baseline_rhr_x10 == 0) {
    y = figure(ctx, y, w, "--", "bpm");
    y = pill(ctx, y, w, "NO NIGHT READ", "");
    line(ctx, y, w, 2, "The Data card says which rule dropped it.");
    return;
  }
  int d = (int)n->night_rhr_x10 - (int)n->baseline_rhr_x10;
  snprintf(big, sizeof big, "%s", d > 0 ? "+" : "");
  fmt_x10(big + strlen(big), sizeof big - strlen(big), d);
  y = figure(ctx, y, w, big, "bpm vs usual");

  fmt_x10(t, sizeof t, n->baseline_rhr_x10);
  snprintf(p1, sizeof p1, "YOUR BASELINE %s", t);
  if (h->day.hard_minutes) snprintf(p2, sizeof p2, "%u MIN HARD TODAY", h->day.hard_minutes);
  else p2[0] = 0;
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  int base = n->baseline_rhr_x10;   // x10. Reversed: a lower night is a longer bar.
  int rows = tall(b) ? 3 : 2;
  for (int ago = 0; ago < rows; ago++) {
    const DayRecord *r = history_get(ago);
    int v = ago == 0 ? n->night_rhr_x10 : (r ? r->rhr_x10 : 0);
    char val[16];
    if (v) fmt_x10(val, sizeof val, v); else snprintf(val, sizeof val, "--");
    y = bar_row(ctx, y, w, ago == 0 ? "Last night" : ago_label(ago), val,
                v ? frac_pct((base + 80) - v, 0, 160) : 0, 50, C_DRAIN);
  }
}

// SLEEP: hours, typical, last three nights vs usual.
static void draw_recharge(GContext *ctx, GRect b, const Headroom *h) {
  const NightResult *n = &h->night;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[48], p2[48], t[16];

  if (n->sleep_minutes == 0) {
    y = figure(ctx, y, w, "--", "asleep");
    y = pill(ctx, y, w, "NO SLEEP FOUND", "");
    line(ctx, y, w, 2, "No sleep window for last night.");
    return;
  }
  fmt_hm(big, sizeof big, n->sleep_minutes);
  y = figure(ctx, y, w, big, "");

  int usual = n->baseline_sleep_min;
  if (usual) { fmt_hm(t, sizeof t, usual); snprintf(p1, sizeof p1, "YOUR USUAL %s", t); }
  else snprintf(p1, sizeof p1, "LEARNING YOUR USUAL");
  { char s[8], e[8]; fmt_clock(s, sizeof s, n->win_start_min); fmt_clock(e, sizeof e, n->win_end_min);
    int awake = (int)n->sleep_span_min - (int)n->sleep_minutes;
    if (awake >= 5) snprintf(p2, sizeof p2, "%s-%s, %d MIN AWAKE", s, e, awake);
    else            snprintf(p2, sizeof p2, "%s-%s", s, e); }
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  int hi = usual ? (usual * 5) / 4 : 600;
  int rows = tall(b) ? 3 : 2;
  for (int ago = 0; ago < rows; ago++) {
    const DayRecord *r = history_get(ago);
    int v = ago == 0 ? n->sleep_minutes : (r ? r->sleep_min : 0);
    char val[16];
    if (v) fmt_hm(val, sizeof val, v); else snprintf(val, sizeof val, "--");
    y = bar_row(ctx, y, w, ago == 0 ? "Last night" : ago_label(ago), val,
                v ? frac_pct(v, 0, hi) : 0, usual ? frac_pct(usual, 0, hi) : -1, C_SLEEP);
  }
}

// TREND: the one thing a daily card structurally cannot show — a short
// baseline sitting above a long one (§5). Every other metric's history now
// lives in that metric's own detail; this card is the drift signal and
// nothing else, which is why the figure is the difference and not a smoothed
// copy of the Night heart rate card.
static void draw_trends(GContext *ctx, GRect b, const Headroom *h) {
  int w = b.size.w;
  static int v[32];
  int n = hist_series(v, 21, H_RHR);
  int y = TOP_H;

  int n7 = 0, nl = 0;
  int s7 = history_avg_rhr_x10(0, 6, &n7);
  int sl = history_avg_rhr_x10(7, 90, &nl);
  int cnt; history_records(&cnt);
  char p1[40], p2[32] = "", big[16], t1[12], t2[12];

  if (n7 >= 3 && nl >= 14) {
    int dd = s7 - sl;
    snprintf(big, sizeof big, "%s", dd > 0 ? "+" : "");
    fmt_x10(big + strlen(big), sizeof big - strlen(big), dd);
    y = figure(ctx, y, w, big, "vs long-term");
    fmt_x10(t1, sizeof t1, s7);
    fmt_x10(t2, sizeof t2, sl);
    snprintf(p1, sizeof p1, "7D %s \xc2\xb7 90D %s", t1, t2);
    if      (dd >=  DRIFT_X10) snprintf(p2, sizeof p2, "ACCUMULATED LOAD");
    else if (dd <= -DRIFT_X10) snprintf(p2, sizeof p2, "BELOW YOUR USUAL");
    else                       snprintf(p2, sizeof p2, "ON YOUR LONG-TERM");
  } else {
    y = figure(ctx, y, w, "--", "vs long-term");
    snprintf(p1, sizeof p1, "%d OF 21 NIGHTS", cnt > 21 ? 21 : cnt);
    snprintf(p2, sizeof p2, "FOR THE LONG VIEW");
  }
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  // The dashed line is the long-term mean, not night.baseline_rhr_x10. That
  // baseline is a ~14-night EMA, so it climbs with the block and would sit
  // level under the bars exactly when drift is happening — hiding the thing
  // this card exists to show.
  int bottom = b.size.h - BOT_H - 4;
  vbars(ctx, y, w, "", v, n, nl ? sl : 0, C_ACCENT, bottom - y);
}

// HRV: last test, recent average, last three tests as bars.
static void draw_hrv(GContext *ctx, GRect b, const Headroom *h) {
  HrvResult r;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[32], p2[32] = "";
  bool have = hrv_last(&r) && r.rmssd_ms;

  int cnt = 0; uint32_t sum = 0;
  for (int a = 1; a <= 30 && cnt < 7; a++) { const DayRecord *d = history_get(a); if (d && d->hrv_rmssd) { sum += d->hrv_rmssd; cnt++; } }
  int avg = cnt ? (int)(sum / cnt) : 0;

  if (!have) {
    y = figure(ctx, y, w, "--", "ms");
    y = pill(ctx, y, w, "SELECT TO TEST", tall(b) ? "2\xc2\xbd MIN, SEATED" : "");
    line(ctx, y, w, 3, "Best first thing in the morning, the same way every time.");
    return;
  }
  snprintf(big, sizeof big, "%u", r.rmssd_ms);
  y = figure(ctx, y, w, big, "ms RMSSD");
  if (avg) snprintf(p1, sizeof p1, "RECENT AVERAGE %d", avg);
  else     snprintf(p1, sizeof p1, "FIRST TEST");
  snprintf(p2, sizeof p2, "SELECT TO TEST AGAIN");
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  int hi = avg ? (avg * 3) / 2 : r.rmssd_ms * 3 / 2;
  int rows = tall(b) ? 3 : 2, shown = 0;
  for (int ago = 0; ago <= 30 && shown < rows; ago++) {
    const DayRecord *d = history_get(ago);
    int v = ago == 0 ? r.rmssd_ms : (d ? d->hrv_rmssd : 0);
    if (!v) continue;
    char val[16]; snprintf(val, sizeof val, "%d ms", v);
    y = bar_row(ctx, y, w, ago_label(ago), val, frac_pct(v, 0, hi), avg ? frac_pct(avg, 0, hi) : -1, C_HRV);
    shown++;
  }
}

// STEPS: exactly the Pebble Health card, because that card is right.
static void draw_steps(GContext *ctx, GRect b, const Headroom *h) {
  const DayResult *d = &h->day;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[32], p2[32];

  fmt_thousands(big, sizeof big, d->steps);
  y = figure(ctx, y, w, big, "");
  // A goal if you set one on the phone, your own average if you did not.
  // No default goal: 10,000 is a 1960s pedometer advertisement, and inventing
  // a target the user never chose is the kind of gate §11 rules out.
  int32_t mark = settings()->step_goal ? settings()->step_goal : d->steps_typical;
  snprintf(p1, sizeof p1, settings()->step_goal ? "YOUR GOAL" : "YOUR AVERAGE DAY");
  fmt_thousands(p2, sizeof p2, mark);
  y = pill(ctx, y, w, p1, mark ? p2 : "");

  int32_t vals[3] = { d->steps, d->steps_d1, d->steps_d2 };
  int32_t hi = mark ? (mark * 13) / 10 : 1;
  for (int i = 0; i < 3; i++) if (vals[i] > hi) hi = vals[i];
  int rows = tall(b) ? 3 : 2;
  for (int i = 0; i < rows; i++) {
    char val[16]; fmt_thousands(val, sizeof val, vals[i]);
    y = bar_row(ctx, y, w, ago_label(i), val, frac_pct((int)vals[i], 0, (int)hi),
                mark ? frac_pct((int)mark, 0, (int)hi) : -1, C_STEPS);
  }
}

// DATA: a three-block confidence meter and the one thing to know.
static void draw_data(GContext *ctx, GRect b, const Headroom *h) {
  const NightResult *n = &h->night;
  int w = b.size.w;
  int y = TOP_H;
  char p1[32], p2[32] = "";

  graphics_context_set_text_color(ctx, C_INK);
  txt(ctx, confidence_label(n->confidence), f_big(w), GRect(8, y - 6, w - 16, big_h(w) + 8),
      GTextAlignmentLeft, GTextOverflowModeTrailingEllipsis);
  y += big_h(w) + 2;
  // three blocks: none / low / medium / high
  int bw = (w - 16 - 8) / 3;
  for (int i = 0; i < 3; i++) {
    graphics_context_set_fill_color(ctx, (int)n->confidence > i ? C_ACCENT : C_TRACK);
    graphics_fill_rect(ctx, GRect(8 + i * (bw + 4), y, bw, 10), 2, GCornersAll);
  }
  y += 18;

  if (n->confidence == CONF_NONE) {
    snprintf(p1, sizeof p1, "NO CLEAN MINUTES");
    snprintf(p2, sizeof p2, "SELECT FOR THE RULE");
  } else {
    snprintf(p1, sizeof p1, "%u CLEAN MINUTES", n->hr_used);
    const char *src = window_source_label(n->window_source);
    for (int i = 0; src[i] && i < 31; i++) p2[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i], p2[i + 1] = 0;
  }
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  char v[40];
  if (n->nights_learned < SETTLED_BASELINE_NIGHTS)
    snprintf(v, sizeof v, "Baseline: night %u of %d, provisional.", n->nights_learned, SETTLED_BASELINE_NIGHTS);
  else
    snprintf(v, sizeof v, "Baseline built on %u nights.", n->nights_learned);
  line(ctx, y, w, 2, v);
}

// METRIC: whatever you told the phone you are watching right now. Same
// grammar as every other card — figure, pill, bars — so it does not read as a
// bolted-on extra.
static void draw_metric(GContext *ctx, GRect b, const Headroom *h) {
  (void)h;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[32], p2[32] = "";
  static int v[32];

  uint16_t val; int ago = 0;
  bool have = metric_last(&val, &ago);

  if (have) {
    uint16_t disp = metric_to_display(val);
    snprintf(big, sizeof big, "%u.%u", disp / 10, disp % 10);
    y = figure(ctx, y, w, big, metric_unit());

    // Against the four weeks before last week, not against yesterday: day to
    // day this number is mostly water, and a card that shouts about noise is
    // a card you learn to distrust.
    uint16_t recent, older;
    if (metric_avg(0, 6, &recent) && metric_avg(7, 27, &older)) {
      int dd = (int)metric_to_display(recent) - (int)metric_to_display(older);
      int ad = dd < 0 ? -dd : dd;
      snprintf(p1, sizeof p1, "%s%d.%d %s IN 4 WEEKS", dd < 0 ? "-" : "+",
               ad / 10, ad % 10, metric_unit());
    } else if (ago == 0) {
      snprintf(p1, sizeof p1, "LOGGED TODAY");
    } else {
      snprintf(p1, sizeof p1, "%d DAYS AGO", ago);
    }
    if (ago > 0) snprintf(p2, sizeof p2, "SELECT TO ADD TODAY");
  } else {
    y = figure(ctx, y, w, "--", metric_unit());
    snprintf(p1, sizeof p1, "SELECT TO ADD");
  }
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  int n = metric_series(v, 28);
  int bottom = b.size.h - BOT_H - 4;
  vbars(ctx, y, w, "", v, n, 0, C_ACCENT, bottom - y);
}

void cards_draw_main(GContext *ctx, GRect b, int card, const Headroom *h) {
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_INK);
  switch (card) {
    case CARD_HEADROOM: draw_headroom(ctx, b, h); break;
    case CARD_DRAIN:    draw_drain(ctx, b, h);    break;
    case CARD_RECHARGE: draw_recharge(ctx, b, h); break;
    case CARD_TRENDS:   draw_trends(ctx, b, h);   break;
    case CARD_HRV:      draw_hrv(ctx, b, h);      break;
    case CARD_STEPS:    draw_steps(ctx, b, h);    break;
    case CARD_METRIC:   draw_metric(ctx, b, h);   break;
    default:            draw_data(ctx, b, h);     break;
  }
  if (card == CARD_HEADROOM && h->provisional && h->valid) {
    char t[32];
    snprintf(t, sizeof t, "TODAY  \xc2\xb7  NIGHT %u/%d", h->night.nights_learned, SETTLED_BASELINE_NIGHTS);
    title(ctx, b, t);
  } else {
    title(ctx, b, card_title(card));
  }
  chevron(ctx, b, true);
  chevron(ctx, b, false);
}

// ---------------------------------------------------------------- detail
// "A little more info": one graphic and a few rows. Scrolls if it must.
int cards_draw_detail(GContext *ctx, GRect b, int card, const Headroom *h, int scroll) {
  const NightResult *n = &h->night;
  const DayResult   *d = &h->day;
  int w = b.size.w;
  int y = TOP_H - scroll;
  char v[40];
  int base_bpm = n->baseline_rhr_x10 / 10;

  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_INK);

  if (card == CARD_HEADROOM) {
    if (!h->valid) {
      y = line(ctx, y, w, 4, "A number appears on the second readable night and is provisional until the seventh.");
    } else {
      snprintf(v, sizeof v, "%u", n->load_part);
      y = bar_row(ctx, y, w, "Night heart rate", v, n->load_part, 88, C_DRAIN);
      snprintf(v, sizeof v, "%u", n->sleep_part);
      y = bar_row(ctx, y, w, "Sleep", v, n->sleep_part, 88, C_SLEEP);
      y = pill(ctx, y + 2, w, "70% HEART  30% SLEEP", "");
      snprintf(v, sizeof v, "%u", n->morning_score);           y = row_kv(ctx, y, w, "This morning", v);
      snprintf(v, sizeof v, "%+d", h->feel_bias);              y = row_kv(ctx, y, w, "Your calibration", v);
      snprintf(v, sizeof v, "-%u", d->drain);                  y = row_kv(ctx, y, w, "Used since waking", v);
      if (h->feel_today) y = row_kv(ctx, y, w, "You said", feel_label(h->feel_today));
      y = line(ctx, y + 4, w, 3, "Only last night moves the morning number. Nothing you did yesterday is subtracted.");

      // The score's own history belongs with the score, not on the Trend card.
      static int s1[32];
      int cn = hist_series(s1, 30, H_SCORE);
      y = vbars(ctx, y + 6, w, "Headroom, 30 days", s1, cn, 0, C_ACCENT, 56);
      cn = hist_series(s1, 30, H_FEEL);
      y = vbars(ctx, y, w, "How you felt", s1, cn, 0, C_PILL, 40);
    }

  } else if (card == CARD_DRAIN) {
    y = area(ctx, y, w, "Since waking", d->curve, base_bpm, C_DRAIN, 70);
    if (d->z3_bpm) {
      snprintf(v, sizeof v, "%u min", d->hard_minutes);          y = row_kv(ctx, y, w, "Above zone 2", v);
      snprintf(v, sizeof v, "%u/%u/%u", d->min_z3, d->min_z4, d->min_z5);
      y = row_kv(ctx, y, w, "Z3 / Z4 / Z5", v);
      snprintf(v, sizeof v, "%u+ bpm", d->z3_bpm);               y = row_kv(ctx, y, w, "Counting starts", v);
      snprintf(v, sizeof v, "%u bpm", d->rest_bpm);              y = row_kv(ctx, y, w, "Your waking rest", v);
      snprintf(v, sizeof v, "%u bpm", d->hrmax_bpm);             y = row_kv(ctx, y, w, "Highest seen", v);
    } else {
      y = line(ctx, y, w, 3, "Not enough waking readings yet to place your zones.");
    }
    if (d->max_hr) { snprintf(v, sizeof v, "%u bpm", d->max_hr); y = row_kv(ctx, y, w, "Peak today", v); }
    snprintf(v, sizeof v, "-%u", d->drain);                      y = row_kv(ctx, y, w, "Cost today", v);
    y = line(ctx, y + 4, w, 4, "Minutes are weighted: zone 4 counts double, zone 5 four times. Below zone 3, nothing counts.");
    y = line(ctx, y, w, 2, "A running estimate. Tonight's reading overrules it.");

  } else if (card == CARD_RECHARGE) {
    y = area(ctx, y, w, "Through the night", n->curve, base_bpm, C_SLEEP, 70);
    fmt_hm(v, sizeof v, n->sleep_span_min);                        y = row_kv(ctx, y, w, "In bed", v);
    { int awake = (int)n->sleep_span_min - (int)n->sleep_minutes; snprintf(v, sizeof v, "%d min", awake < 0 ? 0 : awake); }
    y = row_kv(ctx, y, w, "Awake", v);
    snprintf(v, sizeof v, "%u", n->sleep_sessions);                y = row_kv(ctx, y, w, "Sessions", v);
    y = row_kv(ctx, y, w, "Found by", window_source_label(n->window_source));
    {
      static int s1[32];
      int cn = hist_series(s1, 30, H_SLEEP);
      y = vbars(ctx, y + 6, w, "Sleep, 30 days", s1, cn, n->baseline_sleep_min, C_SLEEP, 56);
      y = line(ctx, y + 2, w, 2, "Dashed line: your usual.");
    }

  } else if (card == CARD_TRENDS) {
    static int s1[64];
    int n7 = 0, nlong = 0;
    int s7 = history_avg_rhr_x10(0, 6, &n7), sl = history_avg_rhr_x10(7, 90, &nlong);
    int cn = hist_series(s1, 60, H_RHR);
    y = vbars(ctx, y, w, "Night heart rate, 60 days", s1, cn, nlong ? sl : 0, C_DRAIN, 70);

    if (n7) fmt_x10(v, sizeof v, s7); else snprintf(v, sizeof v, "--");
    strncat(v, " bpm", sizeof v - strlen(v) - 1);              y = row_kv(ctx, y, w, "This week", v);
    if (nlong) fmt_x10(v, sizeof v, sl); else snprintf(v, sizeof v, "--");
    strncat(v, " bpm", sizeof v - strlen(v) - 1);              y = row_kv(ctx, y, w, "Long-term", v);
    if (n7 && nlong) {
      int dd = s7 - sl;
      snprintf(v, sizeof v, "%s", dd > 0 ? "+" : "");
      fmt_x10(v + strlen(v), sizeof v - strlen(v), dd);
      strncat(v, " bpm", sizeof v - strlen(v) - 1);
    } else snprintf(v, sizeof v, "--");
    y = row_kv(ctx, y, w, "Difference", v);
    snprintf(v, sizeof v, "%d nights", nlong);                 y = row_kv(ctx, y, w, "In the long view", v);

    y = line(ctx, y + 4, w, 5, "Recent nights sitting above your long-term average is what accumulated load looks like from the wrist. The daily number cannot show it.");
    y = line(ctx, y, w, 2, "Dashed line: your long-term average.");

  } else if (card == CARD_HRV) {
    static int s1[32];
    int cn = hist_series(s1, 30, H_HRV);
    y = vbars(ctx, y, w, "RMSSD, 30 days", s1, cn, 0, C_HRV, 70);
    HrvResult r;
    if (hrv_last(&r) && r.rmssd_ms) {
      snprintf(v, sizeof v, "%u ms", r.sdnn_ms);              y = row_kv(ctx, y, w, "SDNN", v);
      snprintf(v, sizeof v, "%u bpm", r.mean_hr);             y = row_kv(ctx, y, w, "Heart rate", v);
      snprintf(v, sizeof v, "%u / %u", r.beats, r.rejected);  y = row_kv(ctx, y, w, "Beats / dropped", v);
    }
    y = line(ctx, y + 4, w, 4, "Paced breathing reads high. Compare tests with each other, not with other apps.");

  } else if (card == CARD_METRIC) {
    static int s1[64];
    int cn = metric_series(s1, 60);
    y = vbars(ctx, y, w, "60 days", s1, cn, 0, C_ACCENT, 70);
    uint16_t a;
    if (metric_avg(0, 6, &a))   { uint16_t x = metric_to_display(a);
      snprintf(v, sizeof v, "%u.%u %s", x / 10, x % 10, metric_unit()); }
    else snprintf(v, sizeof v, "--");
    y = row_kv(ctx, y, w, "This week", v);
    if (metric_avg(7, 27, &a))  { uint16_t x = metric_to_display(a);
      snprintf(v, sizeof v, "%u.%u %s", x / 10, x % 10, metric_unit()); }
    else snprintf(v, sizeof v, "--");
    y = row_kv(ctx, y, w, "Four weeks ago", v);
    { int cnt = 0; for (int i = 0; i < cn; i++) if (s1[i]) cnt++;
      snprintf(v, sizeof v, "%d in 60 days", cnt); }
    y = row_kv(ctx, y, w, "Entries", v);
    y = line(ctx, y + 4, w, 4, "Weekly averages, not day to day: most of the daily movement is water.");
    y = line(ctx, y, w, 2, "SELECT on the card to add today.");

  } else if (card == CARD_STEPS) {
    // Seven days against the typical, most recent first.
    time_t day0 = time_start_of_today();
    int32_t mark = settings()->step_goal ? settings()->step_goal : d->steps_typical;
    int32_t hi = mark ? (mark * 13) / 10 : 1;
    int32_t vals[7];
    for (int i = 0; i < 7; i++) {
      vals[i] = i == 0 ? d->steps : health_service_sum(HealthMetricStepCount, day0 - i * SECONDS_PER_DAY, day0 - (i - 1) * SECONDS_PER_DAY);
      if (vals[i] > hi) hi = vals[i];
    }
    for (int i = 0; i < 7; i++) {
      fmt_thousands(v, sizeof v, vals[i]);
      y = bar_row(ctx, y, w, ago_label(i), v, frac_pct((int)vals[i], 0, (int)hi),
                  mark ? frac_pct((int)mark, 0, (int)hi) : -1, C_STEPS);
    }

  } else {  // CARD_DATA — the diagnostics; this is the one place a list belongs
    y = row_kv(ctx, y, w, "Window from", window_source_label(n->window_source));
    fmt_hm(v, sizeof v, n->win_minutes);                y = row_kv(ctx, y, w, "Window", v);
    snprintf(v, sizeof v, "%u", n->hr_used);            y = row_kv(ctx, y, w, "Clean readings", v);
    snprintf(v, sizeof v, "%u", n->rej_no_hr);          y = row_kv(ctx, y, w, "No reading", v);
    snprintf(v, sizeof v, "%u", n->rej_range);          y = row_kv(ctx, y, w, "Out of range", v);
    snprintf(v, sizeof v, "%u", n->rej_motion);         y = row_kv(ctx, y, w, "Motion gate", v);
    snprintf(v, sizeof v, "%u", n->rej_jump);           y = row_kv(ctx, y, w, "Jump rule", v);
    snprintf(v, sizeof v, "%u/%u/%u", n->vmc_lo, n->vmc_avg, n->vmc_hi); y = row_kv(ctx, y, w, "VMC lo/avg/hi", v);
    y = row_kv(ctx, y, w, "HR metric", d->hr_available ? "available" : "blocked");
    { int cnt, readable = 0; const DayRecord *recs = history_records(&cnt);
      for (int i = 0; i < cnt; i++) if (recs[i].rhr_x10) readable++;
      snprintf(v, sizeof v, "%d of %d", readable, cnt); }
    y = row_kv(ctx, y, w, "History readable", v);
    y = row_kv(ctx, y, w, "Theme", theme_is_dark() ? "dark" : "light");
    y = line(ctx, y + 4, w, 3, "If clean readings is zero, the largest count above is the rule that dropped them.");
    y = line(ctx, y, w, 2, "Long-press UP on a card to switch theme.");
  }

  int content = y + scroll + 10;
  title(ctx, b, card_title(card));
  return content;
}
