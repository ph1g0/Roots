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

// bpm x10 between this week's mean and the long-term mean before the night
// heart rate detail says anything about accumulated load. Under it, silence.
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

// Same title, but on an opaque band. A detail view scrolls its content under
// the header, and a transparent title let rows slide through the letters.
static void title_bar(GContext *ctx, GRect b, const char *t) {
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, TOP_H - 6), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, C_TRACK);
  graphics_draw_line(ctx, GPoint(0, TOP_H - 7), GPoint(b.size.w, TOP_H - 7));
  title(ctx, b, t);
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
//
// min_span is the smallest difference, in the series' own units, that is
// allowed to fill the chart. Without it the rule was "fit whatever is here",
// which is right for heart rate and badly wrong for weight: 1.2 kg of normal
// water movement was drawn as the full height of the card, so a chart of
// nothing looked like a chart of something. fixed_lo / fixed_hi pin an axis
// end where the scale has a real meaning (a score out of ten); -1 is auto.
static int vbars(GContext *ctx, int y, int w, const char *heading, const int *v, int n,
                 int ref, GColor col, int height, int min_span, int fixed_lo, int fixed_hi) {
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
  int span = hi - lo;
  if (span < min_span) {                 // widen around the middle, not upward
    int grow = min_span - span;
    lo -= grow / 2;
    hi += grow - grow / 2;
    span = min_span;
  }
  int pad = span / 6; if (pad < 1) pad = 1;
  int floor_v = fixed_lo >= 0 ? fixed_lo : lo - pad;
  if (floor_v < 0) floor_v = 0;
  int top_v   = fixed_hi >= 0 ? fixed_hi : hi + pad;
  int range = top_v - floor_v; if (range < 1) range = 1;
  int slot = (x1 - x0) / n, bw = slot - 2; if (bw < 2) bw = 2;
  graphics_context_set_fill_color(ctx, col);
  for (int i = 0; i < n; i++) {
    if (v[i] <= 0) continue;
    int bh = ((v[i] - floor_v) * (bottom - top)) / range;
    if (bh < 2) bh = 2;
    if (bh > bottom - top) bh = bottom - top;
    graphics_fill_rect(ctx, GRect(x0 + i * slot + 1, bottom - bh, bw, bh), 1, GCornersTop);
  }
  if (ref > 0) {
    int ry = bottom - ((ref - floor_v) * (bottom - top)) / range;
    if (ry >= top && ry <= bottom) {
      graphics_context_set_stroke_color(ctx, C_INK);
      for (int x = x0; x < x1; x += 6) graphics_draw_line(ctx, GPoint(x, ry), GPoint(x + 3, ry));
    }
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
typedef enum { H_RHR, H_SLEEP, H_SCORE, H_HRV } HistField;
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
    case CARD_NIGHT_HR: return "NIGHT HEART RATE";
    case CARD_SLEEP:    return "SLEEP";
    case CARD_HRV:      return "HRV";
    case CARD_STEPS:    return "STEPS";
    case CARD_METRIC:   return metric_name();
    default:            return "";
  }
}

// TODAY: the ring, the number, the band in a pill, one sentence.
//
// Every card below follows the same shape now: figure, pill, one line. No
// history on the front. Three bar rows was the most a front could hold, which
// meant the card showed you the least interesting window on the data and cost
// two thirds of the screen doing it. SELECT has room for thirty days.
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
    // Drawn from the tenths, so the ring moves on days the digit does not.
    graphics_draw_arc(ctx, ring, GOvalScaleModeFitCircle, 0,
                      (TRIG_MAX_ANGLE * h->score_x10) / SCORE_FULL_X10);
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
    if (hh - BOT_H - y >= LINE_H) line(ctx, y, w, (hh - BOT_H - y) / LINE_H, "Press SELECT for why.");
  }
}

// NIGHT HEART RATE: how far above your recovered floor last night sat, which
// is the whole of the heart-rate deduction. Below the floor reads as zero,
// because below the floor costs nothing.
static void draw_night_hr(GContext *ctx, GRect b, const Headroom *h) {
  const NightResult *n = &h->night;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[48], p2[48] = "", t[16];

  if (n->night_rhr_x10 == 0 || n->floor_rhr_x10 == 0) {
    y = figure(ctx, y, w, "--", "bpm");
    y = pill(ctx, y, w, "NO NIGHT READ", "");
    line(ctx, y, w, 3, "Press SELECT: the counts there name the rule that dropped it.");
    return;
  }
  int d = (int)n->night_rhr_x10 - (int)n->floor_rhr_x10;
  snprintf(big, sizeof big, "%s", d > 0 ? "+" : "");
  fmt_x10(big + strlen(big), sizeof big - strlen(big), d);
  y = figure(ctx, y, w, big, "bpm vs floor");

  fmt_x10(t, sizeof t, n->floor_rhr_x10);
  snprintf(p1, sizeof p1, "YOUR FLOOR %s", t);
  if (n->floor_nights) snprintf(p2, sizeof p2, "LOWEST NIGHTS OF %u", n->floor_nights);
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  char c[12];
  if (d <= 0) {
    line(ctx, y, w, 3, "At or under your floor. Nothing owed on heart rate today.");
  } else {
    headroom_fmt_x10(c, sizeof c, n->rhr_cost_x10);
    snprintf(p1, sizeof p1, "%s off today's ten.", c);
    line(ctx, y, w, 3, p1);
  }
}

// SLEEP: hours against your usual. Longer than usual is not worth anything,
// so the card says so rather than implying a reward.
static void draw_sleep(GContext *ctx, GRect b, const Headroom *h) {
  const NightResult *n = &h->night;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[48], p2[48] = "", t[16];

  if (!n->sleep_known) {
    y = figure(ctx, y, w, "--", "asleep");
    y = pill(ctx, y, w, "NOT MEASURED", "");
    line(ctx, y, w, 4, "No sleep session last night, so sleep costs nothing today.");
    return;
  }
  fmt_hm(big, sizeof big, n->sleep_minutes);
  y = figure(ctx, y, w, big, "");

  int usual = n->baseline_sleep_min;
  if (usual) { fmt_hm(t, sizeof t, usual); snprintf(p1, sizeof p1, "YOUR USUAL %s", t); }
  else snprintf(p1, sizeof p1, "LEARNING YOUR USUAL");
  { char st[8], e[8]; fmt_clock(st, sizeof st, n->win_start_min); fmt_clock(e, sizeof e, n->win_end_min);
    snprintf(p2, sizeof p2, "%s-%s", st, e); }
  y = pill(ctx, y, w, p1, tall(b) ? p2 : "");

  if (n->sleep_cost_x10) {
    char cst[12]; fmt_hm(t, sizeof t, (uint16_t)(usual - n->sleep_minutes));
    headroom_fmt_x10(cst, sizeof cst, n->sleep_cost_x10);
    snprintf(p1, sizeof p1, "%s short. %s off today's ten.", t, cst);
    line(ctx, y, w, 3, p1);
  } else if (usual && (int)n->sleep_minutes >= usual) {
    fmt_hm(t, sizeof t, (uint16_t)(n->sleep_minutes - usual));
    snprintf(p1, sizeof p1, "%s over your usual. Costs nothing.", t);
    line(ctx, y, w, 3, p1);
  } else {
    line(ctx, y, w, 3, "About your usual. Costs nothing.");
  }
}

// HRV: last test and the recent average. On-demand, never overnight.
static void draw_hrv(GContext *ctx, GRect b, const Headroom *h) {
  HrvResult r;
  (void)h;
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
  line(ctx, y, w, 3, "Not part of the score. Compare tests with each other only.");
}

// STEPS: today against your own average day.
static void draw_steps(GContext *ctx, GRect b, const Headroom *h) {
  const DayResult *d = &h->day;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[40], p2[32];

  fmt_thousands(big, sizeof big, d->steps);
  y = figure(ctx, y, w, big, "");
  // A goal if you set one on the phone, your own average if you did not.
  // No default goal: 10,000 is a 1960s pedometer advertisement, and inventing
  // a target the user never chose is the kind of gate §11 rules out.
  int32_t mark = settings()->step_goal ? settings()->step_goal : d->steps_typical;
  snprintf(p1, sizeof p1, settings()->step_goal ? "YOUR GOAL" : "YOUR AVERAGE DAY");
  fmt_thousands(p2, sizeof p2, mark);
  y = pill(ctx, y, w, p1, mark ? p2 : "");

  if (mark) {
    int32_t dd = d->steps - mark;
    char num[16]; fmt_thousands(num, sizeof num, dd < 0 ? -dd : dd);
    snprintf(p1, sizeof p1, "%s %s.", num, dd < 0 ? "to go" : "over");
    line(ctx, y, w, 2, p1);
  }
}

// METRIC: whatever you told the phone you are watching right now. Same
// grammar as every other card — figure, pill, line — so it does not read as a
// bolted-on extra.
static void draw_metric(GContext *ctx, GRect b, const Headroom *h) {
  (void)h;
  int w = b.size.w;
  int y = TOP_H;
  char big[16], p1[32], p2[32] = "";

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
  line(ctx, y, w, 2, "Not part of the score.");
}

void cards_draw_main(GContext *ctx, GRect b, int card, const Headroom *h) {
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_INK);
  switch (card) {
    case CARD_HEADROOM: draw_headroom(ctx, b, h); break;
    case CARD_NIGHT_HR: draw_night_hr(ctx, b, h); break;
    case CARD_SLEEP:    draw_sleep(ctx, b, h);    break;
    case CARD_HRV:      draw_hrv(ctx, b, h);      break;
    case CARD_METRIC:   draw_metric(ctx, b, h);   break;
    default:            draw_steps(ctx, b, h);    break;
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
// "A little more info": the graphic that did not fit on the front, then the
// rows behind it. Scrolls if it must.
int cards_draw_detail(GContext *ctx, GRect b, int card, const Headroom *h, int scroll) {
  const NightResult *n = &h->night;
  const DayResult   *d = &h->day;
  int w = b.size.w;
  int y = TOP_H - scroll;
  char v[48];
  int floor_bpm = n->floor_rhr_x10 / 10;
  static int s1[64];

  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_INK);

  if (card == CARD_HEADROOM) {
    if (!h->valid) {
      y = line(ctx, y, w, 4, "A number appears on the second readable night and is provisional until the seventh.");
      y = row_kv(ctx, y, w, "Confidence", confidence_label(n->confidence));
      snprintf(v, sizeof v, "%u", n->hr_used);
      y = row_kv(ctx, y, w, "Clean readings", v);
      y = row_kv(ctx, y, w, "Window from", window_source_label(n->window_source));
    } else {
      // The arithmetic, in the order it happens, then the readings it came
      // from. v1.0 shipped with only the deductions, labelled "Night heart
      // rate 0.0" — which reads as "your heart rate is zero" rather than "your
      // heart rate cost you nothing". Owing nothing is the good case and it
      // has to look like it.
      y = row_kv(ctx, y, w, "Start of day", "10");
      headroom_fmt_x10(v, sizeof v, n->rhr_cost_x10);
      strncat(v, " off", sizeof v - strlen(v) - 1);
      y = bar_row(ctx, y, w, "Heart rate", v, n->rhr_cost_x10, -1, C_DRAIN);
      if (n->sleep_known) {
        headroom_fmt_x10(v, sizeof v, n->sleep_cost_x10);
        strncat(v, " off", sizeof v - strlen(v) - 1);
        y = bar_row(ctx, y, w, "Sleep", v, n->sleep_cost_x10, -1, C_SLEEP);
      } else {
        y = row_kv(ctx, y, w, "Sleep", "not measured");
      }
      headroom_fmt_x10(v, sizeof v, h->score_x10);
      y = pill(ctx, y + 2, w, "HEADROOM", v);

      // The four numbers the two deductions were computed from. Without these
      // the card asserts an answer instead of showing its working.
      if (n->night_rhr_x10) { fmt_x10(v, sizeof v, n->night_rhr_x10); strncat(v, " bpm", sizeof v - strlen(v) - 1);
                              y = row_kv(ctx, y, w, "Last night", v); }
      if (n->floor_rhr_x10) { fmt_x10(v, sizeof v, n->floor_rhr_x10); strncat(v, " bpm", sizeof v - strlen(v) - 1);
                              y = row_kv(ctx, y, w, "Your floor", v); }
      if (n->sleep_known)   { fmt_hm(v, sizeof v, n->sleep_minutes);  y = row_kv(ctx, y, w, "Slept", v); }
      if (n->baseline_sleep_min) { fmt_hm(v, sizeof v, n->baseline_sleep_min); y = row_kv(ctx, y, w, "Your usual", v); }

      if (n->floor_nights) snprintf(v, sizeof v, "%s \xc2\xb7 floor from %u nights",
                                    confidence_label(n->confidence), n->floor_nights);
      else                 snprintf(v, sizeof v, "%s \xc2\xb7 floor still learning",
                                    confidence_label(n->confidence));
      y = row_kv(ctx, y, w, "Confidence", v);
      y = line(ctx, y + 4, w, 4, "Ten, less what you owe. At or under your floor owes nothing, which is why a good night shows 0.0 off.");

      int cn = hist_series(s1, 30, H_SCORE);
      y = vbars(ctx, y + 6, w, "Headroom, 30 days", s1, cn, 0, C_ACCENT, 56, 0, 0, SCORE_FULL_X10);
    }

  } else if (card == CARD_NIGHT_HR) {
    // The day-by-day bars the card fronts used to carry. They were never the
    // problem — showing only three of them on the front was. Seven here, then
    // sixty in the chart below.
    int base = n->floor_rhr_x10 ? n->floor_rhr_x10 : 550;
    for (int ago = 0; ago < 7; ago++) {
      const DayRecord *r = history_get(ago);
      int val = (ago == 0 && n->night_rhr_x10) ? n->night_rhr_x10 : (r ? r->rhr_x10 : 0);
      if (val) fmt_x10(v, sizeof v, val); else snprintf(v, sizeof v, "--");
      y = bar_row(ctx, y, w, ago == 0 ? "Last night" : ago_label(ago), v,
                  val ? frac_pct((base + 80) - val, 0, 160) : 0, 50, C_DRAIN);
    }
    y = line(ctx, y + 2, w, 2, "Longer bar = lower night. Tick = your floor.");

    fmt_x10(v, sizeof v, n->floor_rhr_x10); strncat(v, " bpm", sizeof v - strlen(v) - 1);
    y = row_kv(ctx, y + 2, w, "Your floor", v);

    // What the Trend card used to be. It is one comparison about night heart
    // rate, so it lives with night heart rate.
    int n7 = 0, nl = 0;
    int s7 = history_avg_rhr_x10(0, 6, &n7), sl = history_avg_rhr_x10(7, 60, &nl);
    if (n7) { fmt_x10(v, sizeof v, s7); strncat(v, " bpm", sizeof v - strlen(v) - 1); } else snprintf(v, sizeof v, "--");
    y = row_kv(ctx, y, w, "This week", v);
    if (nl) { fmt_x10(v, sizeof v, sl); strncat(v, " bpm", sizeof v - strlen(v) - 1); } else snprintf(v, sizeof v, "--");
    y = row_kv(ctx, y, w, "Long-term", v);
    if (n7 >= 3 && nl >= 14) {
      int dd = s7 - sl;
      snprintf(v, sizeof v, "%s", dd > 0 ? "+" : "");
      fmt_x10(v + strlen(v), sizeof v - strlen(v), dd);
      strncat(v, " bpm", sizeof v - strlen(v) - 1);
      y = row_kv(ctx, y, w, "Difference", v);
      if (dd >= DRIFT_X10)
        y = line(ctx, y + 4, w, 4, "This week sits above your long-term average. That is what accumulated load looks like from the wrist.");
    }

    // The night read itself. Diagnostics belong to the reading they explain,
    // not to a card of their own that you walk past every morning.
    snprintf(v, sizeof v, "%u", n->hr_used);            y = row_kv(ctx, y + 4, w, "Clean readings", v);
    y = row_kv(ctx, y, w, "Window from", window_source_label(n->window_source));
    snprintf(v, sizeof v, "%u / %u / %u", n->rej_no_hr, n->rej_range, n->rej_jump);
    y = row_kv(ctx, y, w, "No HR/range/jump", v);
    y = line(ctx, y + 4, w, 3, "If clean readings is low, the largest count above is the rule that dropped them.");

    int cn = hist_series(s1, 60, H_RHR);
    y = vbars(ctx, y + 6, w, "Night heart rate, 60 days", s1, cn, n->floor_rhr_x10, C_DRAIN, 70, 60, -1, -1);
    y = line(ctx, y + 2, w, 2, "Dashed line: your recovered floor.");

  } else if (card == CARD_SLEEP) {
    int usual = n->baseline_sleep_min;
    int hi = usual ? (usual * 5) / 4 : 600;
    for (int ago = 0; ago < 7; ago++) {
      const DayRecord *r = history_get(ago);
      int val = (ago == 0 && n->sleep_known) ? n->sleep_minutes : (r ? r->sleep_min : 0);
      if (val) fmt_hm(v, sizeof v, (uint16_t)val); else snprintf(v, sizeof v, "--");
      y = bar_row(ctx, y, w, ago == 0 ? "Last night" : ago_label(ago), v,
                  val ? frac_pct(val, 0, hi) : 0, usual ? frac_pct(usual, 0, hi) : -1, C_SLEEP);
    }
    y = line(ctx, y + 2, w, 2, "Tick = your usual.");

    if (n->sleep_known) {
      fmt_hm(v, sizeof v, n->sleep_span_min);                      y = row_kv(ctx, y + 2, w, "In bed", v);
      { int awake = (int)n->sleep_span_min - (int)n->sleep_minutes;
        snprintf(v, sizeof v, "%d min", awake < 0 ? 0 : awake); }
      y = row_kv(ctx, y, w, "Awake", v);
      snprintf(v, sizeof v, "%u", n->sleep_sessions);              y = row_kv(ctx, y, w, "Sessions", v);
    } else {
      y = line(ctx, y + 2, w, 4, "Pebble Health reported no sleep session. A still stretch is enough to read heart rate from, but it is not sleep, so none was recorded.");
    }
    y = area(ctx, y + 4, w, "Through the night", n->curve, floor_bpm, C_SLEEP, 64);
    int cn = hist_series(s1, 30, H_SLEEP);
    y = vbars(ctx, y + 4, w, "Sleep, 30 days", s1, cn, n->baseline_sleep_min, C_SLEEP, 56, 180, -1, -1);
    y = line(ctx, y + 2, w, 2, "Dashed line: your usual.");

  } else if (card == CARD_HRV) {
    int cn = hist_series(s1, 30, H_HRV);
    y = vbars(ctx, y, w, "RMSSD, 30 days", s1, cn, 0, C_HRV, 70, 20, -1, -1);
    HrvResult r;
    if (hrv_last(&r) && r.rmssd_ms) {
      snprintf(v, sizeof v, "%u ms", r.sdnn_ms);              y = row_kv(ctx, y, w, "SDNN", v);
      snprintf(v, sizeof v, "%u bpm", r.mean_hr);             y = row_kv(ctx, y, w, "Heart rate", v);
      snprintf(v, sizeof v, "%u / %u", r.beats, r.rejected);  y = row_kv(ctx, y, w, "Beats / dropped", v);
    }
    y = line(ctx, y + 4, w, 4, "Paced breathing reads high. Compare tests with each other, not with other apps.");

  } else if (card == CARD_METRIC) {
    int cn = metric_series(s1, 60);
    // 4.0 kg is the smallest spread allowed to fill the chart: below that you
    // are looking at hydration, not at a trend.
    y = vbars(ctx, y, w, "60 days", s1, cn, 0, C_ACCENT, 70, 40, -1, -1);
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

  } else {  // CARD_STEPS — seven days, and nothing that is not steps
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
    if (mark) {
      fmt_thousands(v, sizeof v, mark);
      y = row_kv(ctx, y + 4, w, settings()->step_goal ? "Your goal" : "Your average day", v);
    }
  }

  int content = y + scroll + 10;
  title_bar(ctx, b, card_title(card));
  return content;
}
