#pragma once
#include <pebble.h>
#include <stdbool.h>

// Everything configurable lives on the phone.
//
// A Setup card would have cost two or three more screens in a loop you walk
// every morning, and each one would have been a number picker driven by four
// buttons. That is the opposite of §11's calm interface, and a watch app that
// grows a settings menu is most of the way to being the dashboard §13 rules
// out. So: the config page is where you set things once, the watch is where
// you read one number.
//
// Nothing here is required. Every field has a default that keeps the app
// working exactly as it does with the phone disconnected forever.

typedef enum { SEX_UNSPECIFIED = 0, SEX_FEMALE, SEX_MALE } Sex;
typedef enum { UNITS_METRIC = 0, UNITS_IMPERIAL } Units;

// Tracked metric on the optional Metric card. One at a time: this is a thing
// you are watching right now, not a second dashboard.
typedef enum { METRIC_NONE = 0, METRIC_WEIGHT, METRIC_KIND_COUNT } MetricKind;

#define ACCENT_COUNT 6

typedef struct {
  uint8_t  schema;
  uint8_t  accent;      // index into the palette in theme.c
  bool     dark;        // long-press UP on the watch still flips this
  uint8_t  units;       // Units
  uint8_t  age;         // years, 0 = not given
  uint8_t  sex;         // Sex
  uint8_t  hrr_z3;      // % of reserve where work starts counting, 0 = default
  uint8_t  metric;      // MetricKind shown on the Metric card
  uint16_t step_goal;   // 0 = compare against your average instead
  uint16_t cards;       // bitmask of visible cards, bit n = Card n
} Settings;

#define SETTINGS_SCHEMA 1

const Settings *settings(void);

// Load from persist, apply defaults, open AppMessage. Call before any window.
void settings_init(void);

// Called after the phone sends new settings, so the caller can rebuild the
// card order and redraw.
typedef void (*SettingsChangedCb)(void);
void settings_subscribe(SettingsChangedCb cb);

void settings_set_dark(bool dark);
bool settings_card_visible(int card);
