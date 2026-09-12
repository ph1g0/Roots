#pragma once
#include <pebble.h>
#include "headroom.h"

// Top-level cards. UP/DOWN moves between the *visible* ones (main.c builds
// the order from settings), SELECT opens the detail for whichever one you are
// on — except HRV, where SELECT starts a test, and Metric, where it opens the
// entry screen. Both put their detail behind a long press.
//
// v0.9 removed two cards. Trend was a whole card for one comparison that
// belongs next to the night heart rate it is a comparison of; it now lives in
// that card's detail. Data was diagnostics presented as a destination — a card
// you walk past every morning to read a number you cannot act on. Its
// confidence line moved onto Headroom's detail, where it qualifies the number
// it is about, and the rejection counts moved to the bottom of the night heart
// rate detail, where they explain the reading they came from.
typedef enum {
  CARD_HEADROOM = 0,   // the one number
  CARD_NIGHT_HR,       // last night against your recovered floor
  CARD_SLEEP,          // what refilled it
  CARD_HRV,            // on-demand test
  CARD_STEPS,
  CARD_METRIC,         // one thing you chose to track; off unless you pick one
  CARD_COUNT
} Card;

const char *card_title(int card);

void cards_draw_main(GContext *ctx, GRect bounds, int card, const Headroom *h);

// Draws the detail for a card, shifted up by `scroll` px. Returns the total
// content height so the caller can clamp scrolling.
int cards_draw_detail(GContext *ctx, GRect bounds, int card, const Headroom *h, int scroll);
