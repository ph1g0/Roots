#include "headroom.h"
#include "common.h"
#include "history.h"
#include "settings.h"

// ------------------------------------------------------------------
// Tunables. All in one place.
// ------------------------------------------------------------------
#define LOOKBACK_MIN         1080  // 18 h — the largest range we hold at once
#define CHUNK_MINUTES        120   // minute-history fetch size

// The night is looked for in a range anchored to the calendar, not to "now",
// so the answer does not change depending on when you open the app.
#define NIGHT_START_BEFORE_H 6     // start 18:00 yesterday
#define NIGHT_SPAN_H         18    // ...through 12:00 today

#define MIN_NIGHT_MINUTES    180   // total asleep below this is not a night
#define MAX_NIGHT_MINUTES    840   // 14 h; past this it is not a night either
#define SESSION_JOIN_MIN     90    // gap that still counts as the same night
#define MAX_SESSIONS         12
#define QUIET_STEP_MAX       15    // steps that can still be a toilet trip
#define QUIET_BRIDGE_MIN     8     // how many such minutes may bridge a night

#define HR_MIN_PLAUSIBLE     35
#define HR_MAX_NIGHT         110
#define HR_MAX_DELTA         15    // bpm between genuinely adjacent readings
#define HR_ADJACENT_MIN      2     // only apply the jump rule within this gap
#define TROUGH_PCT           30    // RHR = mean of the lowest X% of readings
#define MIN_CLEAN_MINUTES    20    // below this: no number at all

// VMC has no documented unit on this hardware and we have already been burned
// once by guessing units (see README, accelerometer). The gate ships off, and
// NightResult keeps the VMC we actually saw so it can be set from real data.
#define STILL_VMC_MAX        0     // 0 = rule off

#define BASELINE_EMA_ALPHA   15    // percent, ~14-night effective window
// For the first nights the EMA is a plain running mean (alpha = 1/n), so night
// two is weighted 50%, night three 33%, and the baseline is usable from day
// two instead of being pinned to whatever the first night happened to be.
#define BASELINE_MEAN_NIGHTS 7

// ---- The score ---------------------------------------------------------
// Ten points, minus what you owe. Nothing adds.
//
// v0.8 anchored a normal night at 88/100 and clipped at 100 after 1.5 bpm, so
// everything from your recovered floor down to your best night ever collapsed
// onto the same number, and a 3 bpm overnight rise moved the score by four
// points out of a hundred. The clip is gone: there is no reward region left to
// clip, because being further below the floor earns nothing at all.
#define RHR_COST_PER_BPM_X10 10    // tenths of a point per bpm over the floor
#define SLEEP_COST_PER_H_X10 20    // tenths of a point per hour short
#define SLEEP_COST_MAX_X10   50    // sleep alone never takes more than half
// Sleep detection on this hardware is good to roughly a quarter hour, so a few
// minutes under your usual is measurement noise, not a short night. This is an
// instrument tolerance, not a grace period: it is set to what the sensor can
// actually resolve and nothing more.
#define SLEEP_TOLERANCE_MIN  10

// The recovered floor: the mean of the lowest RECOVERED_PCT of readable nights
// over RECOVERED_DAYS. Long enough that a training block cannot drag it up
// with itself, short enough that real fitness gains move it within a couple of
// months — which is the answer to "my score must not climb forever".
#define RECOVERED_PCT        30
#define RECOVERED_DAYS       60
#define RECOVERED_MIN_NIGHTS 2

// ---- Waking rest -------------------------------------------------------
// A sleeping trough is not a resting heart rate. The same person who troughs
// at 54 asleep sits at 66-72 at a desk. v0.6 put the "elevated" line at the
// trough plus a fixed 25 bpm, which lands ~8 bpm above sitting and calls most
// of the waking day elevated: 433 minutes on a normal Tuesday, which is a
// constant, not a signal. So the waking figure is measured rather than
// assumed. Since v1.0 none of this reaches the score — it places the zone
// thresholds the Steps detail draws, and nothing else.
#define REST_PCTILE          20    // percentile of still daytime minutes
#define REST_HR_MAX          140   // above this it is not a resting minute
#define REST_MIN_SAMPLES     12    // fewer readings than this: fall back
#define REST_FALLBACK_ADD    12    // bpm over the sleeping baseline, last resort
#define REST_BASELINE_DAYS   14    // days of stored rest_bpm averaged together

// ---- Zones -------------------------------------------------------------
// Karvonen, on the reserve between waking rest and the personal ceiling.
// Counting starts at the zone 2 ceiling, so a commute, a flight of stairs and
// eight hours at a desk count nothing at all — which is the promise in §13.
// HRR_Z3 is the dial: lower it toward 60 if heavy lifting, where the peaks
// are brief, reads as zero too often. Display only; a wrong value here is
// cosmetic now rather than load-bearing.
#define HRR_Z3               70    // % of heart-rate reserve
#define HRR_Z4               80
#define HRR_Z5               90
#define W_Z3                 1     // weighted minutes per real minute
#define W_Z4                 2
#define W_Z5                 4
#define MIN_RESERVE_BPM      40    // below this the zones are nonsense; show none

// The ceiling is what this person has actually reached. An age from the
// config page only ever sets the fallback and the floor: if you have hit 189,
// the app uses 189, not what a formula predicts for a 40-year-old. Measured
// beats assumed, which is the whole of §2.
#define HRMAX_DAYS           90
#define HRMAX_FLOOR          160
#define HRMAX_FALLBACK       185

// Pebble Health samples heart rate every ~10 minutes at rest and more often
// during activity. A reading therefore stands for the minutes after it, until
// the next one, up to this cap. v0.5 counted only the minutes that held a
// reading, which is why the Drain card never showed anything.
#define HR_HOLD_MIN          10

// ------------------------------------------------------------------
// Minute buffer. One flat array, loaded over whichever range the caller needs,
// so window detection, RHR extraction and the daytime scan all read the same
// thing. hr == 0 means "no usable reading in this minute" (missing, or flagged
// invalid) and everything downstream treats those the same way.
// ------------------------------------------------------------------
typedef struct {
  uint8_t  hr;
  uint8_t  steps;
  uint16_t vmc;
} MinRec;

static MinRec   s_min[LOOKBACK_MIN];
static time_t   s_min_t0;
static uint16_t s_min_n;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int iabs(int v) { return v < 0 ? -v : v; }

static void load_minutes(time_t from, time_t to) {
  static HealthMinuteData buf[CHUNK_MINUTES];

  memset(s_min, 0, sizeof(s_min));
  s_min_t0 = 0; s_min_n = 0;
  if (to <= from) return;

  int span = (int)((to - from) / SECONDS_PER_MINUTE);
  if (span > LOOKBACK_MIN) { from = to - (time_t)LOOKBACK_MIN * SECONDS_PER_MINUTE; span = LOOKBACK_MIN; }
  s_min_t0 = from - (from % SECONDS_PER_MINUTE);
  s_min_n  = (uint16_t)span;

  time_t t = s_min_t0;
  while (t < to) {
    time_t rs = t, re = to;
    uint32_t got = health_service_get_minute_history(buf, CHUNK_MINUTES, &rs, &re);
    if (got == 0) {
      // A gap, not the end. v0.4's reader stopped here and lost the rest of
      // the night; step over it instead.
      t += CHUNK_MINUTES * SECONDS_PER_MINUTE;
      continue;
    }
    for (uint32_t i = 0; i < got; i++) {
      if (buf[i].is_invalid) continue;
      int idx = (int)((rs - s_min_t0) / SECONDS_PER_MINUTE) + (int)i;
      if (idx < 0 || idx >= (int)s_min_n) continue;
      s_min[idx].hr    = buf[i].heart_rate_bpm;
      s_min[idx].steps = buf[i].steps;
      s_min[idx].vmc   = buf[i].vmc;
    }
    t = (re > t) ? re : t + CHUNK_MINUTES * SECONDS_PER_MINUTE;  // always advance
  }
}

static int idx_of(time_t t) { return (int)((t - s_min_t0) / SECONDS_PER_MINUTE); }
static time_t time_of(int idx) { return s_min_t0 + (time_t)idx * SECONDS_PER_MINUTE; }

static uint16_t minute_of_day(time_t t) {
  struct tm *lt = localtime(&t);
  return (uint16_t)(lt->tm_hour * 60 + lt->tm_min);
}

// Average heart rate per bucket across s_min[a..b), CURVE_N buckets. A bucket
// with no readings stays 0 and the graph leaves a gap rather than inventing a
// line through it. Returns the bucket width in minutes.
static uint16_t fill_curve(uint8_t *curve, int a, int b, int hr_lo, int hr_hi) {
  memset(curve, 0, CURVE_N);
  int span = b - a;
  if (span <= 0) return 1;
  int step = (span + CURVE_N - 1) / CURVE_N;
  if (step < 1) step = 1;
  for (int k = 0; k < CURVE_N; k++) {
    int s = a + k * step, e = s + step;
    if (s >= b) break;
    if (e > b) e = b;
    uint32_t sum = 0, n = 0;
    for (int i = s; i < e; i++) {
      int hr = s_min[i].hr;
      if (hr < hr_lo || hr > hr_hi) continue;
      sum += hr; n++;
    }
    if (n) curve[k] = (uint8_t)(sum / n);
  }
  return (uint16_t)step;
}

// 18:00 the evening before `day0` through 12:00 on it, never past now.
static void night_range(time_t day0, time_t *from, time_t *to) {
  time_t now  = time(NULL);
  *from = day0 - NIGHT_START_BEFORE_H * SECONDS_PER_HOUR;
  *to   = *from + NIGHT_SPAN_H * SECONDS_PER_HOUR;
  if (*to > now) *to = now;
}

// ------------------------------------------------------------------
// Finding last night. Three sources, best first, and we say which one we used.
// ------------------------------------------------------------------
typedef struct { time_t s, e; } Session;
typedef struct { Session v[MAX_SESSIONS]; uint8_t n; } SessionList;

static bool session_iter(HealthActivity a, time_t start, time_t end, void *ctx) {
  SessionList *L = ctx;
  if (a == HealthActivitySleep && L->n < MAX_SESSIONS) {
    L->v[L->n].s = start;
    L->v[L->n].e = end;
    L->n++;
  }
  return L->n < MAX_SESSIONS;
}

// Pebble Health reports a broken night as several sessions and shows you the
// sum. v0.5 took the longest single session, which is short by however long
// you were awake in the middle. Collect the whole cluster instead: total
// asleep is the sum of the sessions, and the window we read heart rate from
// runs from the first start to the last end.
static bool window_from_sleep(int *ws, int *we, uint8_t *sessions, uint16_t *asleep_min) {
  SessionList L = { .n = 0 };
  health_service_activities_iterate(HealthActivitySleep, s_min_t0,
                                    time_of(s_min_n), HealthIterationDirectionPast,
                                    session_iter, &L);
  *sessions = L.n;
  if (L.n == 0) return false;

  for (int i = 1; i < (int)L.n; i++) {          // ascending by start
    Session t = L.v[i]; int j = i - 1;
    while (j >= 0 && L.v[j].s > t.s) { L.v[j + 1] = L.v[j]; j--; }
    L.v[j + 1] = t;
  }

  int best = 0;
  for (int i = 1; i < (int)L.n; i++)
    if ((L.v[i].e - L.v[i].s) > (L.v[best].e - L.v[best].s)) best = i;

  int lo = best, hi = best;
  while (lo > 0 && (L.v[lo].s - L.v[lo - 1].e) <= SESSION_JOIN_MIN * SECONDS_PER_MINUTE) lo--;
  while (hi < (int)L.n - 1 && (L.v[hi + 1].s - L.v[hi].e) <= SESSION_JOIN_MIN * SECONDS_PER_MINUTE) hi++;

  uint32_t asleep = 0;
  for (int i = lo; i <= hi; i++) asleep += (uint32_t)(L.v[i].e - L.v[i].s);
  if (asleep < (uint32_t)MIN_NIGHT_MINUTES * SECONDS_PER_MINUTE) return false;

  int a = clampi(idx_of(L.v[lo].s), 0, s_min_n);
  int b = clampi(idx_of(L.v[hi].e), 0, s_min_n);
  if (b - a < MIN_NIGHT_MINUTES) return false;

  *asleep_min = (uint16_t)(asleep / SECONDS_PER_MINUTE);
  *ws = a; *we = b;
  return true;
}

// No sleep session? Find the longest stretch that cannot have been an awake
// day: no steps, tolerating short trips. Missing minutes count as quiet — the
// watch being off your wrist is not evidence of walking.
static bool window_from_quiet(int *ws, int *we) {
  int best_s = -1, best_len = 0;
  int cur = -1, gap = 0;

  for (int i = 0; i <= (int)s_min_n; i++) {
    bool end_of_data = (i == (int)s_min_n);
    bool quiet = !end_of_data && s_min[i].steps == 0;
    bool small = !end_of_data && s_min[i].steps <= QUIET_STEP_MAX;

    if (quiet) {
      if (cur < 0) cur = i;
      gap = 0;
    } else if (cur >= 0 && small && gap < QUIET_BRIDGE_MIN) {
      gap++;
    } else {
      if (cur >= 0) {
        int len = i - gap - cur;
        if (len > best_len) { best_len = len; best_s = cur; }
      }
      cur = -1; gap = 0;
    }
  }
  if (best_len < MIN_NIGHT_MINUTES || best_s < 0) return false;
  *ws = best_s; *we = best_s + best_len;
  return true;
}

// Last resort: 23:00 yesterday to the end of the night range.
static bool window_from_clock(time_t day0, int *ws, int *we) {
  time_t s = day0 - SECONDS_PER_HOUR;
  time_t e = time_of(s_min_n);
  if (s < s_min_t0) s = s_min_t0;
  int a = clampi(idx_of(s), 0, s_min_n);
  int b = clampi(idx_of(e), 0, s_min_n);
  if (b - a < MIN_NIGHT_MINUTES) return false;
  *ws = a; *we = b;
  return true;
}

// ------------------------------------------------------------------
// Overnight resting heart rate.
//
//  1. a minute needs a reading, inside the plausible asleep band
//  2. optional motion gate (off until VMC is calibrated)
//  3. a jump larger than HR_MAX_DELTA between two genuinely adjacent minutes
//     is held and only accepted if the next reading confirms the new level
//  4. RHR = mean of the lowest TROUGH_PCT of survivors — a trough, not a
//     minimum, so one lucky low reading cannot define the night
//
// The jump rule only applies when the previous accepted reading was within
// HR_ADJACENT_MIN minutes. Pebble Health does not sample every minute; across
// a ten-minute gap a 20 bpm change is normal physiology, not an artefact.
//
// Reading across the whole span rather than each session separately is
// deliberate: a trough is robust to the few awake minutes in the middle.
// ------------------------------------------------------------------
static bool extract_rhr(NightResult *r, int ws, int we) {
  static uint8_t clean[LOOKBACK_MIN];
  uint16_t n = 0;
  uint32_t vmc_sum = 0, vmc_n = 0;
  uint16_t vmc_lo = 0xFFFF, vmc_hi = 0;

  int prev = -1, prev_i = -100, pending = -1;

  for (int i = ws; i < we && n < LOOKBACK_MIN; i++) {
    const MinRec *m = &s_min[i];
    if (m->hr == 0)                                       { r->rej_no_hr++;  continue; }
    if (m->hr < HR_MIN_PLAUSIBLE || m->hr > HR_MAX_NIGHT) { r->rej_range++;  continue; }
    if (STILL_VMC_MAX > 0 && m->vmc > STILL_VMC_MAX)      { r->rej_motion++; continue; }

    int hr = m->hr;
    if (prev >= 0 && (i - prev_i) <= HR_ADJACENT_MIN && iabs(hr - prev) > HR_MAX_DELTA) {
      if (pending >= 0 && iabs(hr - pending) <= 5) {
        if (n < LOOKBACK_MIN) clean[n++] = (uint8_t)pending;
        if (n < LOOKBACK_MIN) clean[n++] = (uint8_t)hr;
        prev = hr; prev_i = i; pending = -1;
      } else {
        if (pending >= 0) r->rej_jump++;
        pending = hr;
      }
      continue;
    }
    if (pending >= 0) { r->rej_jump++; pending = -1; }

    clean[n++] = (uint8_t)hr;
    prev = hr; prev_i = i;

    if (m->vmc < vmc_lo) vmc_lo = m->vmc;
    if (m->vmc > vmc_hi) vmc_hi = m->vmc;
    vmc_sum += m->vmc; vmc_n++;
  }
  if (pending >= 0) r->rej_jump++;

  r->hr_used = n;
  r->vmc_lo  = vmc_n ? vmc_lo : 0;
  r->vmc_hi  = vmc_hi;
  r->vmc_avg = vmc_n ? (uint16_t)(vmc_sum / vmc_n) : 0;
  if (n < MIN_CLEAN_MINUTES) return false;

  for (uint16_t i = 1; i < n; i++) {          // insertion sort, n is small
    uint8_t v = clean[i]; int j = (int)i - 1;
    while (j >= 0 && clean[j] > v) { clean[j + 1] = clean[j]; j--; }
    clean[j + 1] = v;
  }
  uint16_t k = (uint16_t)((n * TROUGH_PCT) / 100);
  if (k < 5) k = 5;
  if (k > n) k = n;
  uint32_t sum = 0;
  for (uint16_t i = 0; i < k; i++) sum += clean[i];
  r->night_rhr_x10 = (uint16_t)((sum * 10) / k);
  return true;
}

// ------------------------------------------------------------------
// Baselines and scoring
// ------------------------------------------------------------------
static int day_key(void) {
  time_t now = time(NULL);
  struct tm *lt = localtime(&now);
  return lt->tm_yday + lt->tm_year * 400;
}

// nights = how many nights are already in the baseline before this one.
static int ema(int old, int fresh, int nights) {
  if (nights <= 0) return fresh;
  int alpha = BASELINE_EMA_ALPHA;
  if (nights < BASELINE_MEAN_NIGHTS) alpha = 100 / (nights + 1);   // running mean
  if (alpha < BASELINE_EMA_ALPHA) alpha = BASELINE_EMA_ALPHA;
  return old + ((fresh - old) * alpha) / 100;
}

// Provisional confidence: a baseline built on a handful of nights should not
// look like a settled one, whatever the sensor coverage was.
static uint8_t cap_conf(uint8_t c, int nights) {
  if (nights < SETTLED_BASELINE_NIGHTS && c > CONF_LOW) return CONF_LOW;
  return c;
}

// Ten points, minus what you owe. The one place the score is computed, so a
// night in the history chart and the night on the front of the app can never
// disagree about what the same two numbers are worth.
//
//   sleep_min == 0 means the night's sleep was not measured, not that you did
//   not sleep. An unmeasured night costs nothing: the app does not know, and
//   charging you for what it does not know is exactly the dishonesty §8 is
//   about. It shows up as reduced confidence instead.
static void score_night(int rhr_x10, int sleep_min, int floor_x10, int base_sleep,
                        uint8_t *rhr_cost, uint8_t *sleep_cost, uint8_t *score_x10) {
  int over = rhr_x10 - floor_x10;                    // bpm * 10
  if (over < 0) over = 0;                            // below the floor earns nothing
  int rc = (over * RHR_COST_PER_BPM_X10) / 10;
  rc = clampi(rc, 0, SCORE_FULL_X10);

  int sc = 0;
  if (sleep_min > 0 && base_sleep > 0) {
    int short_min = base_sleep - SLEEP_TOLERANCE_MIN - sleep_min;
    if (short_min > 0) sc = clampi((short_min * SLEEP_COST_PER_H_X10) / 60,
                                   0, SLEEP_COST_MAX_X10);
  }

  *rhr_cost   = (uint8_t)rc;
  *sleep_cost = (uint8_t)sc;
  *score_x10  = (uint8_t)clampi(SCORE_FULL_X10 - rc - sc, 0, SCORE_FULL_X10);
}

// Every stored night is rescored against the floor as it stands today, so the
// history chart is one consistent scale rather than a record of what each
// morning happened to believe at the time. It is 90 records and no sensor
// work, and it is what makes a chart drawn after the floor moves still mean
// something.
static void rescore_history(int floor_x10, int base_sleep) {
  if (floor_x10 <= 0) return;
  int cnt = 0;
  DayRecord *recs = (DayRecord *)history_records(&cnt);
  bool dirty = false;
  for (int i = 0; i < cnt; i++) {
    uint8_t rc, sc, s;
    if (recs[i].rhr_x10 == 0) { if (recs[i].score) { recs[i].score = 0; dirty = true; } continue; }
    score_night(recs[i].rhr_x10, recs[i].sleep_min, floor_x10, base_sleep, &rc, &sc, &s);
    if (recs[i].score != s) { recs[i].score = s; dirty = true; }
  }
  if (dirty) history_save();
}

static void night_body(NightResult *r, time_t day0, uint8_t *raw_conf_out) {
  time_t from, to;
  night_range(day0, &from, &to);
  load_minutes(from, to);

  int base_rhr   = persist_exists(KEY_BASELINE_RHR_X10)   ? persist_read_int(KEY_BASELINE_RHR_X10)   : 0;
  int base_sleep = persist_exists(KEY_BASELINE_SLEEP_MIN) ? persist_read_int(KEY_BASELINE_SLEEP_MIN) : 450;
  int nights     = persist_exists(KEY_BASELINE_NIGHTS)    ? persist_read_int(KEY_BASELINE_NIGHTS)    : 0;

  // The floor is read from the stored history, from yesterday backwards, so
  // tonight is never part of the thing tonight is measured against. Until
  // there are enough nights for a percentile to mean anything, the old EMA
  // stands in — and the number is flagged provisional the whole time.
  int nfloor = 0;
  int floor_rhr = history_low_rhr_x10(1, RECOVERED_DAYS, RECOVERED_PCT, &nfloor);
  if (nfloor < RECOVERED_MIN_NIGHTS || floor_rhr == 0) { floor_rhr = base_rhr; nfloor = 0; }

  r->floor_rhr_x10      = (uint16_t)floor_rhr;
  r->floor_nights       = (uint8_t)(nfloor > 255 ? 255 : nfloor);
  r->baseline_sleep_min = (uint16_t)base_sleep;
  r->nights_learned     = (uint8_t)nights;

  if (s_min_n == 0) { r->window_source = WIN_NONE; r->confidence = CONF_NONE; return; }

  int ws = 0, we = 0;
  uint8_t sessions = 0;
  uint16_t asleep = 0;

  if (window_from_sleep(&ws, &we, &sessions, &asleep)) {
    r->window_source = WIN_SLEEP_API;
  } else if (window_from_quiet(&ws, &we)) {
    r->window_source = WIN_QUIET;
    asleep = (uint16_t)(we - ws);        // time still, which is time in bed at best
  } else if (window_from_clock(day0, &ws, &we)) {
    r->window_source = WIN_CLOCK;
    asleep = (uint16_t)(we - ws);
  } else {
    r->window_source = WIN_NONE;
    r->confidence = CONF_NONE;
    r->sleep_sessions = sessions;
    return;
  }

  // A stillness window is good enough to read a heart-rate trough from, but it
  // is not sleep and must never be filed as sleep. A watch in its box is
  // perfectly still, every one of its minutes is "quiet", and window_from_quiet
  // counts missing minutes as quiet on purpose — so v0.8 recorded the entire
  // 18-hour search range as one night's sleep. That is the doubled first bar
  // on anyone's chart who did not wear the watch on day one.
  //
  // Only HealthActivitySleep counts, and only inside a plausible length.
  r->sleep_known = (r->window_source == WIN_SLEEP_API) &&
                   asleep >= MIN_NIGHT_MINUTES && asleep <= MAX_NIGHT_MINUTES;

  r->sleep_sessions = sessions;
  r->win_minutes    = (uint16_t)(we - ws);
  r->sleep_span_min = r->sleep_known ? (uint16_t)(we - ws) : 0;
  r->sleep_minutes  = r->sleep_known ? asleep : 0;
  r->win_start_min  = minute_of_day(time_of(ws));
  r->win_end_min    = minute_of_day(time_of(we));

  fill_curve(r->curve, ws, we, HR_MIN_PLAUSIBLE, HR_MAX_NIGHT);

  if (!extract_rhr(r, ws, we)) { r->confidence = CONF_NONE; return; }

  r->confidence = r->hr_used >= 120 ? CONF_HIGH :
                  r->hr_used >= 40  ? CONF_MEDIUM : CONF_LOW;
  // A window we inferred from the clock is a guess. Don't dress it up.
  if (r->window_source == WIN_CLOCK && r->confidence > CONF_MEDIUM) r->confidence = CONF_MEDIUM;
  uint8_t raw_conf = r->confidence;      // what the baseline update looks at
  *raw_conf_out = raw_conf;

  // `nights` counts the nights already in the baseline; tonight makes one
  // more. With MIN_BASELINE_NIGHTS = 2 the first number appears on morning two.
  if (nights + 1 >= MIN_BASELINE_NIGHTS && floor_rhr > 0) {
    score_night(r->night_rhr_x10, r->sleep_known ? r->sleep_minutes : 0,
                floor_rhr, base_sleep,
                &r->rhr_cost_x10, &r->sleep_cost_x10, &r->morning_score_x10);
    r->scored = true;
  }

  // A bad night must not poison the baseline. While the baseline is still
  // young, though, a low-confidence night is better than no night: the
  // alternative is a blank screen for another day.
  bool feeds_baseline = raw_conf >= CONF_MEDIUM ||
                        (raw_conf == CONF_LOW && nights < SETTLED_BASELINE_NIGHTS);
  if (feeds_baseline) {
    base_rhr = ema(base_rhr, r->night_rhr_x10, nights);
    // Only a measured night moves your usual sleep. Feeding an unknown night
    // in as a zero is how a sleep baseline walks to nothing.
    if (r->sleep_known) {
      base_sleep = ema(base_sleep, r->sleep_minutes, nights);
      persist_write_int(KEY_BASELINE_SLEEP_MIN, base_sleep);
    }
    if (nights < 250) nights++;
    persist_write_int(KEY_BASELINE_RHR_X10, base_rhr);
    persist_write_int(KEY_BASELINE_NIGHTS, nights);
    r->nights_learned = (uint8_t)nights;
  }
  r->confidence = cap_conf(r->confidence, nights);
}

// One night, ending on the morning of `day0`, recorded under history day
// `hist_day`. The record is written whatever happened, so a night that could
// not be read is remembered as unreadable rather than retried forever.
static void night_analyse_for(NightResult *r, time_t day0, uint16_t hist_day) {
  memset(r, 0, sizeof(*r));
  r->schema = NIGHT_SCHEMA;
  uint8_t raw_conf = CONF_NONE;
  night_body(r, day0, &raw_conf);

  // The daily summary record. HRV is filled in later in the day.
  DayRecord *rec = history_upsert(hist_day);
  rec->rhr_x10   = r->night_rhr_x10;
  rec->sleep_min = r->sleep_known ? r->sleep_minutes : 0;
  rec->score     = r->morning_score_x10;     // tenths, same scale as the card
  rec->feel      = 0;                        // the daily question is gone; see v0.9
  REC_SET(rec, raw_conf, REC_TRIES(rec) + 1);
  history_save();
}

static void night_analyse(NightResult *r) {
  night_analyse_for(r, time_start_of_today(), history_day_key());
}

// Pebble Health keeps seven days of minute history, so on a fresh install
// the previous six nights are already on the wrist. Read them, oldest first,
// so the baseline and the bars exist on day one instead of day seven.
//
// A night is (re)read when it has no record, or an empty one that has been
// tried fewer than three times. No accessibility pre-check: the minute
// history is the only thing that knows whether a night is readable, and
// v0.7.2's pre-check said "no" for heart rate on every past day and then
// wrote a blank record that blocked the retry. BACKFILL_VER clears those.
#define BACKFILL_DAYS  6
#define BACKFILL_TRIES 3

static void backfill(void) {
  uint16_t today = history_day_key();
  time_t day0 = time_start_of_today();

  // Repair before reading. The sleep-window fix only changed what gets
  // *written*, so the 15-hour night an older build already recorded would have
  // sat in the chart until it aged out of the 90-day window.
  history_scrub(MAX_NIGHT_MINUTES);

  int ver = persist_exists(KEY_BACKFILL_VER) ? persist_read_int(KEY_BACKFILL_VER) : 0;
  if (ver != BACKFILL_VER) {
    for (int k = 1; k <= BACKFILL_DAYS; k++) {
      DayRecord *r = (DayRecord *)history_get(k);
      if (r && r->rhr_x10 == 0) REC_SET(r, 0, 0);
    }
    history_save();
    persist_write_int(KEY_BACKFILL_VER, BACKFILL_VER);
  }

  for (int k = BACKFILL_DAYS; k >= 1; k--) {
    const DayRecord *r = history_get(k);
    if (r && (r->rhr_x10 || REC_TRIES(r) >= BACKFILL_TRIES)) continue;
    NightResult tmp;
    night_analyse_for(&tmp, day0 - (time_t)k * SECONDS_PER_DAY, (uint16_t)(today - k));
  }

  // Backfill runs oldest first, so the earliest nights were scored against a
  // floor that did not yet include the later ones. One pass over the finished
  // set fixes that, and costs nothing.
  int nf = 0;
  int fl = history_low_rhr_x10(1, RECOVERED_DAYS, RECOVERED_PCT, &nf);
  if (nf >= RECOVERED_MIN_NIGHTS)
    rescore_history(fl, persist_exists(KEY_BASELINE_SLEEP_MIN)
                          ? persist_read_int(KEY_BASELINE_SLEEP_MIN) : 450);
}

static void night_cached(NightResult *r) {
  backfill();
  int today = day_key();
  if (persist_exists(KEY_LAST_NIGHT_DAY) && persist_read_int(KEY_LAST_NIGHT_DAY) == today &&
      persist_exists(KEY_NIGHT_RESULT) &&
      persist_get_size(KEY_NIGHT_RESULT) == (int)sizeof(NightResult)) {
    persist_read_data(KEY_NIGHT_RESULT, r, sizeof(NightResult));
    if (r->schema == NIGHT_SCHEMA) return;
  }
  night_analyse(r);
  // Only a night we could actually read gets cached; a failed read is retried
  // next time the app opens, by which point more data may have landed.
  if (r->confidence != CONF_NONE) {
    persist_write_int(KEY_LAST_NIGHT_DAY, today);
    persist_write_data(KEY_NIGHT_RESULT, r, sizeof(NightResult));
  }
}

// ------------------------------------------------------------------
// Today
// ------------------------------------------------------------------
// The REST_PCTILE-th percentile of today's still minutes: a reading, no
// steps, from waking to now. Counting sort rather than a comparison sort —
// this runs once a minute while the app is open and the range is 100 values.
// Returns 0 when there are too few readings to mean anything.
static uint8_t measure_waking_rest(void) {
  uint16_t hist[REST_HR_MAX + 1];
  memset(hist, 0, sizeof hist);
  int n = 0;
  for (int i = 0; i < (int)s_min_n; i++) {
    if (s_min[i].steps != 0) continue;
    int hr = s_min[i].hr;
    if (hr < HR_MIN_PLAUSIBLE || hr > REST_HR_MAX) continue;
    hist[hr]++; n++;
  }
  if (n < REST_MIN_SAMPLES) return 0;
  int want = (n * REST_PCTILE) / 100, acc = 0;
  for (int hr = HR_MIN_PLAUSIBLE; hr <= REST_HR_MAX; hr++) {
    acc += hist[hr];
    if (acc > want) return (uint8_t)hr;
  }
  return 0;
}

// Age-predicted maximum, 0 when no age was given. Tanaka for everyone,
// Gulati for women — both fit their populations better than 220 minus age,
// which was never more than a napkin figure.
static int hrmax_predicted(void) {
  const Settings *st = settings();
  if (!st->age) return 0;
  return st->sex == SEX_FEMALE ? 206 - (88 * st->age) / 100
                               : 208 - (70 * st->age) / 100;
}

static void day_analyse(DayResult *d, const NightResult *n) {
  memset(d, 0, sizeof(*d));

  time_t now  = time(NULL);
  time_t day0 = time_start_of_today();
  d->hr_available = health_service_metric_accessible(HealthMetricHeartRateBPM, day0, now)
                    == HealthServiceAccessibilityMaskAvailable;

  time_t from = day0;
  if (n->window_source != WIN_NONE) {
    time_t wake = day0 + (time_t)n->win_end_min * SECONDS_PER_MINUTE;
    if (wake > from && wake < now) from = wake;
  }
  load_minutes(from, now);
  if (s_min_n == 0) return;

  // Today's peak, over every reading rather than only the held ones: it feeds
  // the personal ceiling, so a single hard minute should count.
  for (int i = 0; i < (int)s_min_n; i++)
    if (s_min[i].hr >= HR_MIN_PLAUSIBLE && s_min[i].hr > d->max_hr) d->max_hr = s_min[i].hr;

  // Both new figures go into today's record, but only when they have actually
  // moved: this function runs once a minute while the app is open and
  // history_save() writes five chunks.
  uint8_t rest_today = measure_waking_rest();
  DayRecord *rec = history_today();
  bool dirty = false;
  if (d->max_hr && d->max_hr > rec->hr_max) {
    rec->hr_max = (uint8_t)(d->max_hr > 255 ? 255 : d->max_hr);
    dirty = true;
  }
  if (rest_today && rest_today != rec->rest_bpm) { rec->rest_bpm = rest_today; dirty = true; }
  if (dirty) history_save();

  // Waking rest: the fortnight's mean where there is one, today's measurement
  // on a fresh install, the sleeping baseline plus a constant only as a last
  // resort — and that last case is visible on the card as a low confidence.
  int nrest = 0;
  int rest = history_avg_rest_bpm(0, REST_BASELINE_DAYS - 1, &nrest);
  if (!rest) rest = rest_today;
  if (!rest && n->floor_rhr_x10) rest = n->floor_rhr_x10 / 10 + REST_FALLBACK_ADD;
  d->rest_bpm = (uint16_t)rest;

  int predicted = hrmax_predicted();
  int floor_bpm = predicted ? (predicted * 90) / 100 : HRMAX_FLOOR;
  int ceiling   = history_max_hr(HRMAX_DAYS);
  if ((int)d->max_hr > ceiling)   ceiling = d->max_hr;
  if (ceiling == 0)               ceiling = predicted ? predicted : HRMAX_FALLBACK;
  else if (ceiling < floor_bpm)   ceiling = floor_bpm;
  d->hrmax_bpm = (uint16_t)ceiling;

  // The zone floor is on the config page because §8 says heavy, low-rep work
  // can sit under it. Blank there means the built-in default.
  int z3pct = settings()->hrr_z3 ? settings()->hrr_z3 : HRR_Z3;
  int z4pct = z3pct + (HRR_Z4 - HRR_Z3);
  int z5pct = z3pct + (HRR_Z5 - HRR_Z3);

  int reserve = ceiling - rest;
  if (rest && reserve >= MIN_RESERVE_BPM) {
    d->z3_bpm = (uint16_t)(rest + (reserve * z3pct) / 100);
    d->z4_bpm = (uint16_t)(rest + (reserve * z4pct) / 100);
    d->z5_bpm = (uint16_t)(rest + (reserve * z5pct) / 100);
  }

  // Each reading is held for the minutes that follow it, up to HR_HOLD_MIN.
  int cur = 0, held = 0;
  for (int i = 0; i < (int)s_min_n; i++) {
    int hr = s_min[i].hr;
    if (hr >= HR_MIN_PLAUSIBLE) {
      cur = hr; held = 0;
      d->hr_samples++;
      d->last_hr = (uint16_t)hr;
    } else if (cur && held < HR_HOLD_MIN) {
      held++;
    } else {
      cur = 0;
    }
    if (!cur || !d->z3_bpm) continue;
    if      (cur >= d->z5_bpm) d->min_z5++;
    else if (cur >= d->z4_bpm) d->min_z4++;
    else if (cur >= d->z3_bpm) d->min_z3++;
  }
  d->hard_minutes = d->min_z3 + d->min_z4 + d->min_z5;
  d->work_minutes = (uint16_t)(d->min_z3 * W_Z3 + d->min_z4 * W_Z4 + d->min_z5 * W_Z5);

  d->curve_start_min = minute_of_day(s_min_t0);
  d->curve_step_min  = fill_curve(d->curve, 0, s_min_n, HR_MIN_PLAUSIBLE, 220);
}

// Bands on the printed number, not on the tenths, so the word under the score
// can never contradict the digit above it.
static uint8_t band_for(uint8_t score) {
  if (score >= 10) return BAND_FULL;
  if (score >= 8)  return BAND_STRONG;
  if (score >= 6)  return BAND_MODERATE;
  if (score >= 4)  return BAND_AEROBIC;
  return BAND_EASY;
}

static void finalise(Headroom *h) {
  h->valid       = h->night.scored;
  h->provisional = h->night.nights_learned < SETTLED_BASELINE_NIGHTS;
  if (!h->valid) { h->score_x10 = 0; h->score = 0; h->band = BAND_NONE; return; }
  // The morning number is the whole number. Nothing done since waking is
  // subtracted from it, so it does not quietly fall through the afternoon.
  h->score_x10 = h->night.morning_score_x10;
  h->score     = (uint8_t)clampi((h->score_x10 + 5) / 10, 0, 10);
  h->band      = band_for(h->score);
}

void headroom_compute(Headroom *out) {
  memset(out, 0, sizeof(*out));
  night_cached(&out->night);
  rescore_history(out->night.floor_rhr_x10, out->night.baseline_sleep_min);
  day_analyse(&out->day, &out->night);
  headroom_refresh_steps(out);
  finalise(out);
}

// "6.9", or "10" with no decimal because a tenth of a point on a full battery
// is a distinction without a difference.
void headroom_fmt_x10(char *buf, int n, int x10) {
  if (x10 >= SCORE_FULL_X10) { snprintf(buf, n, "10"); return; }
  int a = x10 < 0 ? -x10 : x10;
  snprintf(buf, n, "%s%d.%d", x10 < 0 ? "-" : "", a / 10, a % 10);
}

void headroom_refresh_day(Headroom *out) {
  day_analyse(&out->day, &out->night);
  headroom_refresh_steps(out);
  finalise(out);
}

void headroom_refresh_steps(Headroom *out) {
  time_t day0 = time_start_of_today();
  struct tm *lt = localtime(&day0);
  out->day.wday  = (uint8_t)lt->tm_wday;
  out->day.steps = health_service_sum_today(HealthMetricStepCount);
  // Your average day. TimeScopeWeekly gives the average of this *weekday*
  // only — "typical Friday" — which is a smaller, noisier sample and answers
  // a question nobody asked. TimeScopeDaily averages every day there is.
  out->day.steps_typical = health_service_sum_averaged(HealthMetricStepCount, day0,
                               day0 + SECONDS_PER_DAY, HealthServiceTimeScopeDaily);
  out->day.steps_d1 = health_service_sum(HealthMetricStepCount, day0 - SECONDS_PER_DAY, day0);
  out->day.steps_d2 = health_service_sum(HealthMetricStepCount, day0 - 2 * SECONDS_PER_DAY, day0 - SECONDS_PER_DAY);
}

// ------------------------------------------------------------------
// Language. Ceilings, never gates: no band tells you to stop.
// ------------------------------------------------------------------
const char *band_name(uint8_t band) {
  switch (band) {
    case BAND_FULL:     return "Full send";
    case BAND_STRONG:   return "Strong";
    case BAND_MODERATE: return "Moderate";
    case BAND_AEROBIC:  return "Aerobic";
    case BAND_EASY:     return "Easy movement";
    default:            return "--";
  }
}

const char *band_line(uint8_t band) {
  switch (band) {
    case BAND_FULL:     return "Top end is there. PRs, intervals, heavy singles.";
    case BAND_STRONG:   return "Hard work is fine. The top set may feel heavy.";
    case BAND_MODERATE: return "Keep the volume, back off the top end.";
    case BAND_AEROBIC:  return "Easy pace, technique, mobility.";
    case BAND_EASY:     return "A walk, light cycling, whatever feels good.";
    default:            return "";
  }
}

const char *headroom_status(const Headroom *h) {
  if (h->night.confidence == CONF_NONE) return "No readable night yet. Press SELECT for why.";
  if (h->night.nights_learned < MIN_BASELINE_NIGHTS) return "First night logged. Your first number arrives tomorrow.";
  return "No number today.";
}

const char *confidence_label(uint8_t c) {
  switch (c) {
    case CONF_HIGH:   return "High";
    case CONF_MEDIUM: return "Medium";
    case CONF_LOW:    return "Low";
    default:          return "None";
  }
}

const char *window_source_label(uint8_t s) {
  switch (s) {
    case WIN_SLEEP_API: return "sleep sessions";
    case WIN_QUIET:     return "still stretch";
    case WIN_CLOCK:     return "clock guess";
    default:            return "none";
  }
}
