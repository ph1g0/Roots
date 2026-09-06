#include "theme.h"
#include "common.h"
#include "settings.h"

static bool  s_loaded, s_dark = true;
static Theme s_t;

// Accent palette, picked on the phone. Order is fixed: the config page sends
// an index, so inserting a colour in the middle would repaint every watch.
// On a 1-bit display all of these collapse to white, which is correct — the
// choice is decoration, never meaning (see the Theme comment on c_drain).
static GColor accent_at(uint8_t i) {
#ifdef PBL_COLOR
  switch (i) {
    case 1:  return GColorGreen;
    case 2:  return GColorChromeYellow;
    case 3:  return GColorOrange;
    case 4:  return GColorMagenta;
    case 5:  return GColorPictonBlue;
    default: return GColorCyan;
  }
#else
  (void)i; return GColorWhite;
#endif
}

static void build(void) {
  if (s_dark) {
    s_t.bg      = GColorBlack;
    s_t.ink     = GColorWhite;
    s_t.mute    = PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
    s_t.track   = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorLightGray);   // dithered on 1-bit
    s_t.accent  = accent_at(settings()->accent);
    s_t.accent2 = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);  // #FFAA00
    s_t.pill    = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite);
    s_t.pill_ink= GColorBlack;
    // One colour for every graphic. Cyan on black is the most legible pair
    // this panel has; per-card colours were tried (v0.7.2) and none of the
    // others came close. The fields stay so a future theme can differ.
    s_t.c_drain = s_t.c_sleep = s_t.c_steps = s_t.c_hrv = s_t.accent;
  } else {
    s_t.bg      = GColorWhite;
    s_t.ink     = GColorBlack;
    s_t.mute    = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
    s_t.track   = GColorLightGray;
    // Light mode needs a dark accent for contrast on white, so the palette
    // choice only applies to the dark theme, which is the default.
    s_t.accent  = PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack);
    s_t.accent2 = PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack);
    s_t.pill    = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    s_t.pill_ink= PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite);
    s_t.c_drain = s_t.c_sleep = s_t.c_steps = s_t.c_hrv = s_t.accent;
  }
}

const Theme *theme(void) {
  if (!s_loaded) {
    s_loaded = true;
    s_dark = settings()->dark;
    build();
  }
  return &s_t;
}

// The phone changed something: re-read and rebuild.
void theme_reload(void) {
  s_loaded = true;
  s_dark = settings()->dark;
  build();
}

bool theme_is_dark(void) { theme(); return s_dark; }

void theme_toggle(void) {
  theme();
  s_dark = !s_dark;
  settings_set_dark(s_dark);
  build();
}
