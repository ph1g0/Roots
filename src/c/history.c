#include "history.h"
#include "common.h"

static DayRecord s_rec[HIST_DAYS];
static int       s_n      = 0;
static bool      s_loaded = false;

uint16_t history_day_key(void) {
  // time_start_of_today() is local midnight as a UTC instant. Noon of that
  // day always lands inside the same UTC day for any zone within +/-12 h, so
  // consecutive local days map to consecutive keys regardless of timezone.
  return (uint16_t)((time_start_of_today() + 12 * SECONDS_PER_HOUR) / SECONDS_PER_DAY);
}

// ---- Schema 1 -> 2 -------------------------------------------------------
// The v1 record was 12 bytes, 21 to a chunk, 105 days. v2 adds rest_bpm and
// hr_max and holds 18 to a chunk. Reinterpreting the old bytes at the new
// stride would shear every field, so the old layout is read explicitly and
// the newest HIST_DAYS records are carried across. The two new fields start
// at zero and fill in from the next few days of use.
typedef struct {
  uint16_t day, rhr_x10, sleep_min;
  uint8_t  score, feel, conf, hrv_rmssd, hrv_hr, hrv_sdnn;
} DayRecordV1;

#define V1_PER_CHUNK 21

static void migrate_v1(void) {
  DayRecordV1 old[V1_PER_CHUNK];      // one chunk at a time: 252 bytes of stack
  s_n = 0;
  for (int c = 0; c < HIST_CHUNKS; c++) {
    uint32_t key = KEY_HIST_CHUNK0 + c;
    if (!persist_exists(key)) break;
    int size = persist_get_size(key);
    int recs = size / (int)sizeof(DayRecordV1);
    if (recs <= 0) break;
    if (recs > V1_PER_CHUNK) recs = V1_PER_CHUNK;
    persist_read_data(key, old, recs * sizeof(DayRecordV1));

    for (int i = 0; i < recs; i++) {
      if (old[i].day == 0) continue;
      if (s_n > 0 && old[i].day <= s_rec[s_n - 1].day) continue;   // keep ascending
      if (s_n == HIST_DAYS) {            // 105 days in, 90 out: drop the oldest
        memmove(&s_rec[0], &s_rec[1], (HIST_DAYS - 1) * sizeof(DayRecord));
        s_n--;
      }
      DayRecord *r = &s_rec[s_n++];
      memset(r, 0, sizeof(*r));
      r->day       = old[i].day;
      r->rhr_x10   = old[i].rhr_x10;
      r->sleep_min = old[i].sleep_min;
      r->score     = old[i].score;
      r->feel      = old[i].feel;
      r->conf      = old[i].conf;
      r->hrv_rmssd = old[i].hrv_rmssd;
      r->hrv_hr    = old[i].hrv_hr;
      r->hrv_sdnn  = old[i].hrv_sdnn;
    }
  }
  history_save();
}

void history_load(void) {
  if (s_loaded) return;
  s_loaded = true;
  s_n = 0;

  int schema = persist_exists(KEY_HIST_SCHEMA) ? persist_read_int(KEY_HIST_SCHEMA) : 1;
  if (schema != HIST_SCHEMA) {
    if (schema == 1 && persist_exists(KEY_HIST_CHUNK0)) migrate_v1();
    else for (int c = 0; c < HIST_CHUNKS; c++)
           if (persist_exists(KEY_HIST_CHUNK0 + c)) persist_delete(KEY_HIST_CHUNK0 + c);
    persist_write_int(KEY_HIST_SCHEMA, HIST_SCHEMA);
    return;                      // s_rec is already what migrate_v1 left
  }

  for (int c = 0; c < HIST_CHUNKS && s_n < HIST_DAYS; c++) {
    uint32_t key = KEY_HIST_CHUNK0 + c;
    if (!persist_exists(key)) break;
    int size = persist_get_size(key);
    int recs = size / (int)sizeof(DayRecord);
    if (recs <= 0) break;
    if (recs > HIST_DAYS - s_n) recs = HIST_DAYS - s_n;
    persist_read_data(key, &s_rec[s_n], recs * sizeof(DayRecord));
    s_n += recs;
  }
  // Defensive: keep ascending, drop anything with a zero day key.
  int w = 0;
  for (int i = 0; i < s_n; i++) {
    if (s_rec[i].day == 0) continue;
    if (w > 0 && s_rec[i].day <= s_rec[w - 1].day) continue;
    s_rec[w++] = s_rec[i];
  }
  s_n = w;
}

void history_save(void) {
  int done = 0;
  for (int c = 0; c < HIST_CHUNKS; c++) {
    uint32_t key = KEY_HIST_CHUNK0 + c;
    int recs = s_n - done;
    if (recs > HIST_PER_CHUNK) recs = HIST_PER_CHUNK;
    if (recs <= 0) { if (persist_exists(key)) persist_delete(key); continue; }
    persist_write_data(key, &s_rec[done], recs * sizeof(DayRecord));
    done += recs;
  }
}

DayRecord *history_upsert(uint16_t day) {
  history_load();
  // Find the insertion point from the end; records are ascending.
  int i = s_n;
  while (i > 0 && s_rec[i - 1].day > day) i--;
  if (i > 0 && s_rec[i - 1].day == day) return &s_rec[i - 1];
  if (s_n == HIST_DAYS) {
    if (i == 0) return &s_rec[0];               // older than everything we keep
    memmove(&s_rec[0], &s_rec[1], (HIST_DAYS - 1) * sizeof(DayRecord));
    s_n--; i--;
  }
  memmove(&s_rec[i + 1], &s_rec[i], (s_n - i) * sizeof(DayRecord));
  memset(&s_rec[i], 0, sizeof(DayRecord));
  s_rec[i].day = day;
  s_n++;
  return &s_rec[i];
}

DayRecord *history_today(void) { return history_upsert(history_day_key()); }

const DayRecord *history_records(int *count) {
  history_load();
  *count = s_n;
  return s_rec;
}

const DayRecord *history_get(int days_ago) {
  history_load();
  uint16_t want = history_day_key() - (uint16_t)days_ago;
  for (int i = s_n - 1; i >= 0; i--) {
    if (s_rec[i].day == want) return &s_rec[i];
    if (s_rec[i].day < want) break;
  }
  return NULL;
}

uint16_t history_avg_rhr_x10(int from_days_ago, int to_days_ago, int *n) {
  history_load();
  uint16_t today = history_day_key();
  uint32_t sum = 0; int cnt = 0;
  for (int i = 0; i < s_n; i++) {
    int ago = (int)today - (int)s_rec[i].day;
    if (ago < from_days_ago || ago > to_days_ago) continue;
    if (s_rec[i].rhr_x10 == 0 || REC_CONF(&s_rec[i]) < 2) continue;   // CONF_MEDIUM
    sum += s_rec[i].rhr_x10; cnt++;
  }
  *n = cnt;
  return cnt ? (uint16_t)(sum / cnt) : 0;
}

uint8_t history_avg_rest_bpm(int from_days_ago, int to_days_ago, int *n) {
  history_load();
  uint16_t today = history_day_key();
  uint32_t sum = 0; int cnt = 0;
  for (int i = 0; i < s_n; i++) {
    int ago = (int)today - (int)s_rec[i].day;
    if (ago < from_days_ago || ago > to_days_ago) continue;
    if (s_rec[i].rest_bpm == 0) continue;
    sum += s_rec[i].rest_bpm; cnt++;
  }
  *n = cnt;
  return cnt ? (uint8_t)(sum / cnt) : 0;
}

uint8_t history_max_hr(int days) {
  history_load();
  uint16_t today = history_day_key();
  uint8_t hi = 0;
  for (int i = 0; i < s_n; i++) {
    int ago = (int)today - (int)s_rec[i].day;
    if (ago < 0 || ago > days) continue;
    if (s_rec[i].hr_max > hi) hi = s_rec[i].hr_max;
  }
  return hi;
}
