#pragma once
#include <pebble.h>
#include <stdbool.h>

// One compact record per day, written by Roots itself. Pebble Health keeps
// only seven days of minute history; everything longer-horizon — the long
// baseline, drift detection, every trend graph — is built from these.
typedef struct {
  uint16_t day;          // history_day_key() of the morning the night ended
  uint16_t rhr_x10;      // overnight RHR, 0 = not read
  uint16_t sleep_min;    // 0 = not read
  uint8_t  score;        // morning score before drain/calibration, 0 = none
  uint8_t  feel;         // 0 = not asked/skipped, 1..5
  uint8_t  conf;         // low nibble: Confidence. High nibble: read attempts (backfill)
  uint8_t  hrv_rmssd;    // ms from the on-demand test, 0 = none (255 = clipped)
  uint8_t  hrv_hr;       // mean HR during that test
  uint8_t  hrv_sdnn;     // ms
  uint8_t  rest_bpm;     // waking resting HR measured that day, 0 = not read
  uint8_t  hr_max;       // highest reading seen that day, 0 = none
} DayRecord;             // 14 bytes; 18 per 252-byte persist chunk

// Whole bpm for both new fields on purpose: they feed a threshold placed at
// 70% of heart-rate reserve, where half a beat moves the line by 0.15 bpm.
// Keeping them one byte each is what holds the record at 14 and the history
// at 90 days, which is the length the long baseline actually needs.
#define HIST_PER_CHUNK 18
#define HIST_DAYS      (HIST_PER_CHUNK * 5)   // 90 days: the long baseline

uint16_t history_day_key(void);              // today, in the local calendar

void history_load(void);                     // idempotent; called by the others
void history_save(void);

// Today's record, created if absent. Caller fills fields, then history_save().
DayRecord *history_today(void);
// Any day's record, created (in order) if absent. Used by the backfill.
DayRecord *history_upsert(uint16_t day);

// Records in ascending day order. count may be 0.
const DayRecord *history_records(int *count);

// Record for a day exactly `days_ago` days back (0 = today), or NULL.
const DayRecord *history_get(int days_ago);

// Mean overnight RHR (x10) over days_ago in [from, to], readable nights only.
// Returns 0 and n = 0 when there is nothing.
uint16_t history_avg_rhr_x10(int from_days_ago, int to_days_ago, int *n);

// Your recovered level: the mean of the lowest `pct` percent of readable
// nights in [from, to]. `n` receives how many nights were in range.
//
// This is deliberately not the mean. A mean sits above your recovered heart
// rate by construction, because every drained night is in it — which put the
// entire informative region (your true floor up to a few bpm over it) below
// the line, and made a hard session read as "below baseline, so fine". The
// lowest slice is what you look like with nothing owed, and that is the only
// thing a deduction can sensibly be measured from.
uint16_t history_low_rhr_x10(int from_days_ago, int to_days_ago, int pct, int *n);

// Mean waking resting HR (bpm) over days_ago in [from, to]. 0 when empty.
uint8_t history_avg_rest_bpm(int from_days_ago, int to_days_ago, int *n);

// Highest daily peak heart rate in the last `days` days. 0 when empty. This
// is the personal ceiling the zone boundaries are placed against.
uint8_t history_max_hr(int days);

#define REC_CONF(r)     ((r)->conf & 0x0F)
#define REC_TRIES(r)    ((r)->conf >> 4)
#define REC_SET(r, c, t) ((r)->conf = (uint8_t)(((c) & 0x0F) | (((t) & 0x0F) << 4)))
