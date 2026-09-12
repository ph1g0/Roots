#pragma once
#include <pebble.h>
#include <stdbool.h>

// Headroom: how much of your top end is likely available today.
// Never a verdict, never a rest instruction — see the concept doc, §3.
//
// v0.9 replaced the 0..100 score with a 0..10 one, and changed what the
// number means. It is no longer a position on a scale with a midpoint. It is
// a full battery with deductions taken out of it:
//
//     headroom = 10 − (night heart rate above your recovered floor)
//                   − (sleep short of your usual)
//
// Sleeping longer than usual and troughing lower than your floor both earn
// nothing. There is no upside term, so the number cannot drift upward with
// fitness — improvement moves the floor instead, and 10 keeps meaning
// "recovered" rather than slowly coming to mean "recovered for last winter".

typedef enum { CONF_NONE = 0, CONF_LOW, CONF_MEDIUM, CONF_HIGH } Confidence;

typedef enum {
  BAND_NONE = 0,
  BAND_EASY,        // easy movement
  BAND_AEROBIC,
  BAND_MODERATE,
  BAND_STRONG,
  BAND_FULL,        // full send
} Band;

// Where last night's window came from. Shown to the user, because a window we
// guessed from the clock deserves less trust than one Pebble Health reported.
typedef enum {
  WIN_NONE = 0,
  WIN_SLEEP_API,    // HealthService sleep session
  WIN_QUIET,        // longest still stretch found in the minute history
  WIN_CLOCK,        // fixed overnight window, last resort
} WindowSource;

#define CURVE_N 48        // points in a stored heart-rate curve

// The score and every deduction are carried in tenths of a point: the whole
// range is 0..100 here and 0..10 on screen. Tenths exist so a 0.6 bpm rise is
// not rounded out of existence before the deductions are added up.
#define SCORE_FULL_X10   100

// One night. Computed once per calendar day and cached.
typedef struct {
  uint8_t  schema;
  uint8_t  confidence;         // Confidence
  uint8_t  window_source;      // WindowSource
  uint8_t  sleep_sessions;     // how many HealthActivitySleep sessions we saw
  uint8_t  nights_learned;
  bool     scored;             // floor learned and the night was readable
  bool     sleep_known;        // a real sleep figure, not a stillness guess

  uint16_t night_rhr_x10;      // bpm * 10
  uint16_t floor_rhr_x10;      // your recovered level: what 10 is measured from
  uint8_t  floor_nights;       // readable nights the floor was built from
  uint16_t sleep_minutes;      // sum of the night's sleep sessions, 0 = unknown
  uint16_t sleep_span_min;     // first asleep to last awake, awake gaps included
  uint16_t baseline_sleep_min; // your usual: what costing nothing means

  uint16_t win_start_min;      // minute of day
  uint16_t win_end_min;
  uint16_t win_minutes;

  // Data quality — every rejected minute is attributed to a rule, so a zero
  // score can be traced to the rule that caused it.
  uint16_t hr_used;
  uint16_t rej_no_hr;          // no reading in that minute at all
  uint16_t rej_range;          // outside the plausible asleep band
  uint16_t rej_motion;         // VMC gate (off by default)
  uint16_t rej_jump;           // optical artefact
  uint16_t vmc_lo, vmc_avg, vmc_hi;  // VMC across the minutes we kept

  // The two deductions, in tenths of a point, and what is left of the ten.
  uint8_t  rhr_cost_x10;       // night heart rate above the floor
  uint8_t  sleep_cost_x10;     // sleep short of your usual
  uint8_t  morning_score_x10;  // 0..100 tenths == 0.0..10.0

  // Heart rate across the window, CURVE_N buckets, 0 = no reading in bucket.
  uint8_t  curve[CURVE_N];
} NightResult;

// Today, recomputed on every open. Not persisted.
//
// Nothing in here reaches the score any more. Daytime effort is measured and
// shown, but it is not subtracted: what a session cost is a question only
// tonight's reading can answer, and guessing at it during the day was the app
// estimating the input again — the thing §2 exists to stop doing.
typedef struct {
  int32_t  steps;              // Pebble Health's own count, unfiltered
  int32_t  steps_typical;      // Pebble Health's average across all days
  int32_t  steps_d1, steps_d2; // yesterday, the day before
  uint8_t  wday;               // today's tm_wday
  uint16_t hr_samples;         // readings since waking
  uint16_t last_hr;            // most recent reading today, 0 if none
  uint16_t max_hr;             // highest reading since waking

  // Zones, placed on the reserve between measured waking rest and the highest
  // heart rate this person has actually reached. All 0 when there is not
  // enough to place them, and nothing is counted in that case.
  uint16_t rest_bpm;           // waking rest in use today, not the sleeping trough
  uint16_t hrmax_bpm;          // personal ceiling the zones are scaled against
  uint16_t z3_bpm, z4_bpm, z5_bpm;   // floor of each zone
  uint16_t min_z3, min_z4, min_z5;   // minutes spent in each
  uint16_t hard_minutes;       // z3 + z4 + z5, unweighted
  uint16_t work_minutes;       // weighted: z3 x1, z4 x2, z5 x4

  bool     hr_available;

  uint8_t  curve[CURVE_N];     // HR since waking, 0 = no reading
  uint16_t curve_start_min;    // minute of day of curve[0]
  uint16_t curve_step_min;     // minutes per bucket
} DayResult;

typedef struct {
  NightResult night;
  DayResult   day;
  bool    valid;               // there is a real number to show
  bool    provisional;         // fewer than SETTLED_BASELINE_NIGHTS
  uint8_t score_x10;           // 0..100 tenths, what the ring is drawn from
  uint8_t score;               // 0..10, what is printed
  uint8_t band;                // Band
} Headroom;

// Night (cached per day) + today. Call on launch and on significant updates.
void headroom_compute(Headroom *out);

// Today only, reusing the cached night. Call once a minute while open.
void headroom_refresh_day(Headroom *out);

// Cheapest refresh: today's step total. Call on movement events.
void headroom_refresh_steps(Headroom *out);

const char *band_name(uint8_t band);
const char *band_line(uint8_t band);          // one plain sentence, no verdict
const char *headroom_status(const Headroom *h); // shown when there is no score
const char *confidence_label(uint8_t c);
const char *window_source_label(uint8_t s);

// "6.9" into a caller's buffer. One place, so the score is never formatted two
// different ways on two different cards.
void headroom_fmt_x10(char *buf, int n, int x10);
