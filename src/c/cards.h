#pragma once
#include <pebble.h>
#include "headroom.h"

// Top-level cards. UP/DOWN moves between the *visible* ones (main.c builds
// the order from settings), SELECT opens the detail for whichever one you are
// on — except HRV, where SELECT starts a test, and Metric, where it opens the
// entry screen. Both put their detail behind a long press.
typedef enum {
  CARD_HEADROOM = 0,   // the one number
  CARD_DRAIN,          // what used the battery
  CARD_RECHARGE,       // what refilled it
  CARD_TRENDS,         // drift: the short baseline against the long one
  CARD_HRV,            // on-demand test
  CARD_STEPS,
  CARD_METRIC,         // one thing you chose to track; off unless you pick one
  CARD_DATA,           // how much to trust today's number
  CARD_COUNT
} Card;

const char *card_title(int card);

void cards_draw_main(GContext *ctx, GRect bounds, int card, const Headroom *h);

// Draws the detail for a card, shifted up by `scroll` px. Returns the total
// content height so the caller can clamp scrolling.
int cards_draw_detail(GContext *ctx, GRect bounds, int card, const Headroom *h, int scroll);
