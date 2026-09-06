#pragma once
#include <pebble.h>
#include <stdbool.h>

// "How do you feel?" — one tap, once a day, asked before the score is shown.
// Skippable with BACK; a skipped day is not asked again.

typedef void (*FeelAnswerCb)(uint8_t feel);   // 1 rough .. 5 great

bool feel_should_ask(void);
void feel_show(FeelAnswerCb cb);
