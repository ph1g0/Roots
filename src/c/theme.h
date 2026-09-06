#pragma once
#include <pebble.h>
#include <stdbool.h>

// One palette, read by every draw call. Dark by default: a small screen with
// a short backlight window needs bright text on a black field, not grey on
// white. Light stays available (long-press UP on any card) because a
// transflective display reads better in direct sun with a white ground.
typedef struct {
  GColor bg;       // page
  GColor ink;      // all primary text
  GColor mute;     // secondary text — still clearly readable, never faint
  GColor track;    // unfilled ring, chart frame, rules
  GColor accent;   // ring fill, chart line. One colour, no traffic light.
  GColor accent2;  // breathing circle, dashed baseline
  GColor pill;     // the highlighted comparison box
  GColor pill_ink; // text inside it
  // Card identity colours. They say which card you are on, never how good
  // the number is — the same card is the same colour on a bad day.
  GColor c_drain, c_sleep, c_steps, c_hrv;
} Theme;

const Theme *theme(void);
bool  theme_is_dark(void);
void  theme_toggle(void);
void  theme_reload(void);   // after the phone sends new settings

#define C_BG      (theme()->bg)
#define C_INK     (theme()->ink)
#define C_MUTE    (theme()->mute)
#define C_TRACK   (theme()->track)
#define C_ACCENT  (theme()->accent)
#define C_ACCENT2 (theme()->accent2)
#define C_PILL    (theme()->pill)
#define C_PILL_INK (theme()->pill_ink)
#define C_DRAIN   (theme()->c_drain)
#define C_SLEEP   (theme()->c_sleep)
#define C_STEPS   (theme()->c_steps)
#define C_HRV     (theme()->c_hrv)

// Readability floor. Nothing on screen is set smaller than this.
#define F_BODY      FONT_KEY_GOTHIC_18
#define F_BODY_B    FONT_KEY_GOTHIC_18_BOLD
#define F_TITLE     FONT_KEY_GOTHIC_24_BOLD
#define F_HUGE      FONT_KEY_GOTHIC_28_BOLD
#define F_BIG       FONT_KEY_BITHAM_42_BOLD     // big figures, full character set
#define F_BIG_S     FONT_KEY_BITHAM_30_BLACK    // same on 144 px screens
#define LINE_H      22            // Gothic 18 line height
