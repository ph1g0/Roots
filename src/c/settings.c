#include "settings.h"
#include "common.h"
#include "cards.h"

static Settings          s_s;
static bool              s_loaded;
static SettingsChangedCb s_cb;

// Every card except Headroom can be turned off. Headroom is the app; if you
// do not want it you want a different app. Metric is off until you pick
// something to track, so the card count only grows for people who asked.
#define CARDS_DEFAULT  ((uint16_t)(~(1u << CARD_METRIC)))

static int32_t clampi_s(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void defaults(void) {
  memset(&s_s, 0, sizeof s_s);
  s_s.schema    = SETTINGS_SCHEMA;
  s_s.accent    = 0;              // cyan
  s_s.dark      = true;
  s_s.units     = UNITS_METRIC;
  s_s.age       = 0;
  s_s.sex       = SEX_UNSPECIFIED;
  s_s.hrr_z3    = 0;              // headroom.c falls back to HRR_Z3
  s_s.metric    = METRIC_NONE;
  s_s.step_goal = 0;
  s_s.cards     = CARDS_DEFAULT;
}

static void load(void) {
  if (s_loaded) return;
  s_loaded = true;
  defaults();
  if (persist_exists(KEY_SETTINGS) && persist_get_size(KEY_SETTINGS) == (int)sizeof(Settings)) {
    Settings t;
    persist_read_data(KEY_SETTINGS, &t, sizeof t);
    if (t.schema == SETTINGS_SCHEMA) s_s = t;
  }
  // v0.6-v0.8 kept the theme in its own key. Carry it over once so the watch
  // does not flip to dark on update for anyone who chose light.
  if (persist_exists(KEY_THEME_DARK)) {
    s_s.dark = persist_read_bool(KEY_THEME_DARK);
    persist_delete(KEY_THEME_DARK);
    persist_write_data(KEY_SETTINGS, &s_s, sizeof s_s);
  }
}

const Settings *settings(void) { load(); return &s_s; }

void settings_set_dark(bool dark) {
  load();
  s_s.dark = dark;
  persist_write_data(KEY_SETTINGS, &s_s, sizeof s_s);
}

bool settings_card_visible(int card) {
  load();
  if (card == CARD_HEADROOM) return true;
  if (card == CARD_METRIC)   return s_s.metric != METRIC_NONE;
  if (card < 0 || card >= CARD_COUNT) return false;
  return (s_s.cards >> card) & 1u;
}

// ---------------------------------------------------------------- inbox
// Clay handles showConfiguration and webviewclosed itself and sends one key
// per config item, so there is no hand-written JS to get wrong. The cost is
// that the six card toggles arrive separately and are folded into the bitmask
// here rather than on the phone.
//
// Clay sends a number field as an int, but an *empty* number field can arrive
// as a string, so both shapes are accepted. Reading int32 off a cstring tuple
// would be silent garbage — and a garbage age quietly moves everyone's zones.
static bool opt_int(DictionaryIterator *it, uint32_t key, int32_t *out) {
  Tuple *t = dict_find(it, key);
  if (!t) return false;
  switch (t->type) {
    case TUPLE_INT:
    case TUPLE_UINT:
      *out = (int32_t)t->value->int32;
      return true;
    case TUPLE_CSTRING:
      // cstring is declared char[0], so it can be passed as a pointer but not
      // compared to NULL (-Werror=address) or subscripted (-Wzero-length-
      // bounds). Neither is needed: length 0 is an empty field, and atoi of an
      // empty string is 0 anyway. An empty field means "not given", which the
      // caller treats as a real answer rather than an age of zero.
      *out = (t->length == 0) ? 0 : (int32_t)atoi(t->value->cstring);
      return true;
    default:
      return false;
  }
}

static uint8_t opt_u8(DictionaryIterator *it, uint32_t key, uint8_t cur, int lo, int hi) {
  int32_t v;
  if (!opt_int(it, key, &v)) return cur;
  return (uint8_t)clampi_s(v, lo, hi);
}

// A card toggle that is absent leaves its bit alone, so a partial message
// cannot silently hide half the app.
static uint16_t card_bit(DictionaryIterator *it, uint32_t key, uint16_t cards, int bit) {
  int32_t v;
  if (!opt_int(it, key, &v)) return cards;
  return v ? (uint16_t)(cards | (1u << bit)) : (uint16_t)(cards & ~(1u << bit));
}

static void inbox(DictionaryIterator *it, void *ctx) {
  load();
  int32_t v;

  s_s.accent = opt_u8(it, MESSAGE_KEY_accent, s_s.accent, 0, ACCENT_COUNT - 1);
  s_s.units  = opt_u8(it, MESSAGE_KEY_units,  s_s.units,  0, UNITS_IMPERIAL);
  s_s.sex    = opt_u8(it, MESSAGE_KEY_sex,    s_s.sex,    0, SEX_MALE);
  s_s.metric = opt_u8(it, MESSAGE_KEY_metric, s_s.metric, 0, METRIC_KIND_COUNT - 1);
  if (opt_int(it, MESSAGE_KEY_dark, &v))     s_s.dark = v != 0;
  if (opt_int(it, MESSAGE_KEY_stepGoal, &v)) s_s.step_goal = (uint16_t)clampi_s(v, 0, 60000);

  // Age 0 means "not given", which is different from a bad value. Anything
  // outside a plausible range becomes not-given rather than being clamped to
  // the nearest end: a wrong age is worse than no age, because the app has a
  // measured ceiling to fall back on and no way to notice a bad guess.
  if (opt_int(it, MESSAGE_KEY_age, &v))
    s_s.age = (v >= 10 && v <= 100) ? (uint8_t)v : 0;

  // The zone floor is on the config page because §8 is honest that heavy,
  // low-rep work can sit under it. Out of range means the built-in default.
  if (opt_int(it, MESSAGE_KEY_hrrZ3, &v))
    s_s.hrr_z3 = (v >= 50 && v <= 85) ? (uint8_t)v : 0;

  uint16_t cards = s_s.cards;
  cards = card_bit(it, MESSAGE_KEY_cardNightHr,  cards, CARD_NIGHT_HR);
  cards = card_bit(it, MESSAGE_KEY_cardSleep,    cards, CARD_SLEEP);
  cards = card_bit(it, MESSAGE_KEY_cardHrv,      cards, CARD_HRV);
  cards = card_bit(it, MESSAGE_KEY_cardSteps,    cards, CARD_STEPS);
  s_s.cards = (uint16_t)(cards | (1u << CARD_HEADROOM));

  persist_write_data(KEY_SETTINGS, &s_s, sizeof s_s);
  if (s_cb) s_cb();
}

void settings_subscribe(SettingsChangedCb cb) { s_cb = cb; }

void settings_init(void) {
  load();
  app_message_register_inbox_received(inbox);
  // Small inbox: the config page sends ten integers. The outbox is unused —
  // the watch never sends anything back, so there is nothing to size for.
  app_message_open(256, 32);
}
