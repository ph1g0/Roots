#pragma once

// ---- Persistent storage keys ----
// 1, 2 and 8 were the worker's step keys (v0.1-v0.3). 7 was the v0.4
// RecoveryResult blob. None of them are reused: a stale value read back at the
// wrong size is exactly how you get a confident-looking zero on screen.
#define KEY_BASELINE_RHR_X10   3    // int, bpm * 10   (kept from v0.4)
#define KEY_BASELINE_NIGHTS    4    // int             (kept from v0.4)
#define KEY_BASELINE_SLEEP_MIN 5    // int, minutes    (kept from v0.4)
#define KEY_LAST_NIGHT_DAY     9    // int, day key of the cached night
#define KEY_NIGHT_RESULT       10   // NightResult blob

// v0.6
// 11, 12 and 13 were the "how do you feel" prompt's day marker, calibration
// offset and answer count (v0.6-v0.8). The prompt is gone and none of the
// three are reused: a stale bias read back under a new meaning is exactly the
// silent wrongness the rest of this list exists to avoid.
#define KEY_HRV_LAST           14   // HrvResult blob, most recent test
#define KEY_THEME_DARK         15   // bool, default true
#define KEY_BACKFILL_VER       16   // int; bump BACKFILL_VER to make old blank records retry
#define BACKFILL_VER           2
#define KEY_HIST_SCHEMA        17   // int, layout version of the chunks below
#define KEY_HIST_SCRUB_VER     19   // int; bump HIST_SCRUB_VER to re-run the pass
// A record already written cannot be re-read once it falls out of Pebble
// Health's seven-day minute buffer, so fixing a *writer* bug does nothing for
// the rows it already wrote. This runs one pass over stored history and drops
// values the new rules would never have produced. Bump it when a rule changes.
//   1: sleep_min above MAX_NIGHT_MINUTES — the watch-in-its-box night that
//      window_from_quiet filed as a 15-hour sleep (v0.8 and earlier).
#define HIST_SCRUB_VER         1
#define KEY_SETTINGS           18   // Settings blob, sent from the phone
#define KEY_HIST_CHUNK0        20   // 20..24: DayRecord history, 18 records each
#define HIST_CHUNKS            5
#define KEY_METRIC_CHUNK0      30   // 30..31: tracked metric history, 63 each
#define METRIC_CHUNKS          2

// Bump when NightResult changes shape. A cached blob with a different schema
// is thrown away rather than reinterpreted.
#define NIGHT_SCHEMA           5

// Bump when DayRecord changes shape. Unlike NightResult, history is not
// thrown away on a mismatch — three months of nights is the whole point of
// keeping it — so every bump needs a migration in history_load().
//   1: 12 bytes, 21 per chunk, 105 days   (v0.6-v0.7.3)
//   2: 14 bytes, 18 per chunk,  90 days   (v0.8: + rest_bpm, hr_max)
#define HIST_SCHEMA            2

// Nights before a number appears at all. Two is the honest minimum: the first
// night *is* the baseline, so there is nothing to compare it to.
#define MIN_BASELINE_NIGHTS    2
// Nights before the number stops being labelled provisional.
#define SETTLED_BASELINE_NIGHTS 7
