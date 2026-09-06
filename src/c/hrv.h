#pragma once
#include <pebble.h>
#include <stdbool.h>

// On-demand HRV test. The sensor is only asked for beat intervals while this
// window is open; the moment it closes, both sample-period requests go back
// to automatic. Nothing runs in the background.

typedef struct {
  uint8_t  schema;
  uint8_t  rmssd_ms;       // 0 = none, 255 = clipped
  uint8_t  sdnn_ms;
  uint8_t  mean_hr;
  uint16_t beats;          // intervals kept
  uint16_t rejected;       // intervals dropped as artefacts
  uint16_t day;            // history day key
  uint16_t minute_of_day;  // when the test finished
  bool     paced;          // guided breathing was on
} HrvResult;

#define HRV_SCHEMA 1

void hrv_show(void);                              // push the test window
bool hrv_is_open(void);
void hrv_health_event(HealthEventType e);         // forwarded from main's handler
bool hrv_last(HrvResult *out);                    // most recent finished test
