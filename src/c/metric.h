#pragma once
#include <pebble.h>
#include <stdbool.h>

// One user-tracked number, kept apart from DayRecord on purpose. DayRecord is
// sized to exactly 252 bytes a chunk and every byte added to it costs baseline
// days; an optional feature should not shrink the long baseline for people who
// never turn it on. So tracked values get their own store: 4 bytes a day, 63
// to a chunk, two chunks, 126 days.
//
// Values are stored canonically — weight is always kg x10 — and converted at
// the edges. Changing units on the phone must not rewrite history.

typedef struct {
  uint16_t day;        // history_day_key()
  uint16_t value;      // canonical units x10
} MetricPoint;

#define METRIC_PER_CHUNK 63
#define METRIC_DAYS      (METRIC_PER_CHUNK * METRIC_CHUNKS)

void     metric_load(void);
bool     metric_get(int days_ago, uint16_t *value);   // canonical x10
void     metric_set_today(uint16_t value);            // canonical x10
bool     metric_last(uint16_t *value, int *days_ago); // most recent entry
// Mean over days_ago in [from, to]. false when there is nothing in range.
bool     metric_avg(int from_days_ago, int to_days_ago, uint16_t *out);
// Newest last, one slot per day, 0 = no entry that day. Returns the count.
int      metric_series(int *out, int days);

// The most recent `max` entries, newest first, skipping the empty days
// between them. `days_ago[i]` is how far back entry i was logged. Weight is
// logged weekly at best, so "the last seven days" is usually one bar and "the
// last seven entries" is the thing worth drawing. Returns how many were found.
int      metric_recent(uint16_t *values, int *days_ago, int max, int within_days);

// Display helpers. These respect the units setting; the store never does.
const char *metric_name(void);         // "WEIGHT"
const char *metric_unit(void);         // "kg" / "lb"
uint16_t    metric_to_display(uint16_t canonical_x10);
uint16_t    metric_from_display(uint16_t display_x10);
uint16_t    metric_step_x10(void);     // one button press, in display units
uint16_t    metric_min_x10(void);
uint16_t    metric_max_x10(void);

// The entry screen: UP/DOWN adjusts, long-press repeats, SELECT saves.
typedef void (*MetricEntryCb)(void);
void metric_entry_show(MetricEntryCb done);
