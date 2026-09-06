#pragma once
#include <pebble.h>
#include <stdbool.h>

// Headroom: how much of your top end is likely available today.
// Never a verdict, never a rest instruction — see the concept doc, §3.

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

// One night. Computed once per calendar day and cached.
typedef struct {
  uint8_t  schema;
  uint8_t  confidence;         // Confidence
  uint8_t  window_source;      // WindowSource
  uint8_t  sleep_sessions;     // how many HealthActivitySleep sessions we saw
  uint8_t  nights_learned;
  bool     scored;             // baseline learned and the night was readable

  uint16_t night_rhr_x10;      // bpm * 10
  uint16_t baseline_rhr_x10;   // baseline used for today's score
  uint16_t sleep_minutes;      // sum of the night's sleep sessions
  uint16_t sleep_span_min;     // first asleep to last awake, awake gaps included
  uint16_t baseline_sleep_min;

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

  uint8_t  load_part;          // 0..100 from overnight RHR
  uint8_t  sleep_part;         // 0..100 from sleep duration
  uint8_t  morning_score;      // 0..100 before any intraday drain or calibration

  // Heart rate across the window, CURVE_N buckets, 0 = no reading in bucket.
  uint8_t  curve[CURVE_N];
} NightResult;

// Today, recomputed on every open. Not persisted.
typedef struct {
  int32_t  steps;              // Pebble Health's own count, unfiltered
  int32_t  steps_typical;      // Pebble Health's average for this weekday
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

  uint8_t  drain;              // points removed from the morning score
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
  int8_t  feel_bias;           // calibration offset from "how do you feel"
  uint8_t feel_today;          // 0 = not answered, 1..5
  uint8_t score;               // 0..100
  uint8_t band;                // Band
} Headroom;

// Night (cached per day) + today. Call on launch and on significant updates.
void headroom_compute(Headroom *out);

// Today only, reusing the cached night. Call once a minute while open.
void headroom_refresh_day(Headroom *out);

// Cheapest refresh: today's step total. Call on movement events.
void headroom_refresh_steps(Headroom *out);

// "How do you feel?" answer for today, 1 (rough) .. 5 (great). Updates the
// calibration offset and today's history record, then re-finalises.
void headroom_record_feel(Headroom *h, uint8_t feel);

const char *band_name(uint8_t band);
const char *band_line(uint8_t band);          // one plain sentence, no verdict
const char *headroom_status(const Headroom *h); // shown when there is no score
const char *confidence_label(uint8_t c);
const char *window_source_label(uint8_t s);
const char *feel_label(uint8_t feel);
