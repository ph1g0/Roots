# Roots — one honest battery for the whole body

A deliberately small health app for Pebble Time 2 (emery), also builds for
Pebble 2 (diorite) and Pebble Time (basalt). Seven cards, one number, and a
detail view behind each card for anyone who goes looking.

## Build

```sh
pebble build
pebble install --emulator emery     # or --phone <ip>
```

Needs **SDK 4.33** (12 Aug 2026) for the HRV calls. On an older SDK build with
`CFLAGS=-DROOTS_NO_HRV`: the test window still runs but records nothing.

No background worker. If `worker_src/` is still in your tree, delete it.
Health data only means anything on real hardware; the emulator can inject
steps and sleep since 4.33 but not heart rate.

## Version notes

- **v0.9 (current)** — the first card is called **Headroom**, because
  "Today" named the screen instead of the number. Settings moved to the phone
  (Clay config page): age and sex for a better starting ceiling, units, step
  goal, zone floor, accent colour, and which cards are shown at all. No Setup
  card — a settings menu driven by four buttons would have added screens to a
  loop walked every morning, which is the thing the app is against. Optional
  **Metric** card for one number you choose to track, weight first. Steps now
  compares against your average day rather than your average Friday.
- **v0.8** — the drain measures intensity instead of time. The
  "elevated" line was the sleeping trough plus a fixed 25 bpm, which for a
  54.3 trough sits at 79 — about eight beats above sitting at a desk. It
  reported 433 elevated minutes on an ordinary day and, worse, saturated
  `DRAIN_MAX` every day, so the intraday figure carried no information at
  all. Now: waking rest is *measured* (20th percentile of still daytime
  minutes), zones are placed on heart-rate reserve against the highest rate
  you have actually reached, counting starts at the zone 2 ceiling, and
  minutes are weighted by zone. Trend card rebuilt around drift and nothing
  else; every other metric's history moved to its own detail. `DayRecord`
  grew to 14 bytes — existing history is migrated, not discarded.
- **v0.7.3** — backfill actually works: v0.7.2 gated each past
  night on `health_service_metric_accessible` for heart rate, which is not a
  summable metric, so every past night was skipped and a blank record was
  written that blocked the retry. Gate removed, blank records retried up to
  three times, and `BACKFILL_VER` clears the blanks already on watches. One
  colour for every graphic: cyan.
- **v0.7.2** — backfill: the previous six nights are read from
  Pebble Health's minute buffer on first launch, so bars, trends and a
  (provisional) score exist on day one. Contrast pass: all figures white,
  card colours restricted to the bright end of the palette and used only
  in graphics.
- **v0.7** — visual redesign in Pebble Health's idiom: big
  figure in the card colour, yellow pill for the one comparison, bars for
  the last days with a tick at your usual, vertical bars for trends.
  Detail views cut to one graphic and a few rows. Touch navigation.
- **v0.6.1** — readability pass: dark theme by default, nothing
  under 18 px, white on black, shorter copy, backlight held while open,
  long-press UP toggles light/dark.
- **v0.6** — usable from day two, not week three. Score from the
  second night, labelled provisional until the seventh; first morning shows
  the raw readings instead of a blank. "How do you feel?" prompt before the
  number, which calibrates the score to you. Daily summary record in
  persistent storage (105 days then, 90 now), which makes the Trend card, the long
  baseline and drift detection possible. On-demand HRV test with guided
  breathing. Heart-rate curves for last night and today. **Drain fixed**: it
  now actually counts (see below).
- **v0.5.1 (current)** — sleep total now matches Pebble Health: all the
  sessions in a night are summed instead of taking the longest one, and the
  night is searched in a range anchored to the calendar rather than to the
  moment you opened the app. Recharge shows time in bed and time awake
  alongside time asleep.
- **v0.5** — cards instead of one screen plus a stack of numbers.
  Step filter removed. Overnight RHR reworked: three ways to find the night,
  no unit-guessing motion gate, per-rule rejection counts on the Data card.
  Scores are now headroom bands, not a recovery verdict.
- v0.4 — worker removed; minute-level step filter on the firmware counter.
- v0.3 — established that the accelerometer SDK path is unusable (below).
- v0.2 — ring UI, detail cards.
- v0.1 — initial score + cadence-locked raw accelerometer filter.

## Design (v0.7)

The cards borrow Pebble Health's vocabulary, because that vocabulary is
right for this screen: a graphic with a number in it, not text with a
colour behind it.

```
        ^                       chevron: there is a card above
   NIGHT HEART RATE             which card, small caps, muted
   +2.4  bpm vs usual           the figure, Bitham 42 bold, card colour
  [ YOUR BASELINE 52.1     ]    the pill: one comparison, black on yellow
  [ 34 MIN ELEVATED TODAY  ]
   Last night          54.5     bars: last three days, white tick = usual
   ██████████|████
   Yesterday           51.2
   ████████████|██
        v
```

- **All text is white** (labels light grey). Colour never carries a word:
  the figure, the values beside the bars, the rows in the detail are all
  white on black, because that is the most contrast the panel has.
- **One colour: cyan** (#00FFFF) for every ring, bar and chart, on black,
  with the yellow pill. Per-card colours were tried in v0.7.2 and nothing
  else on the 64-colour panel is as legible; the card name at the top does
  the job colour was doing. Colour never says whether the news is good.
- Bars are the comparison. Longer is more: more steps, more sleep, more HRV,
  and on the heart-rate card more *reserve* (a lower night is a longer bar).
  The white tick is your usual.
- Trends is fourteen vertical bars with your baseline dashed across them.
- Nothing under Gothic 18. Titles are 18 bold caps, muted. The pill text is
  18 bold caps, black on yellow. Chevrons instead of dots.
- SELECT gives a little more, not a lot: one graphic (a heart-rate area for
  the night or the day, thirty days of bars for trends) and three or four
  rows. Data is the one card that is a list, because diagnostics are a list.
- Dark by default, light on long-press UP. Backlight held while open.

## Touch

Pebble Time 2 has a touchscreen; SDK 4.33 added the APIs. `touch.c` does two
things:

1. `app_touch_navigation_enable()` on launch — the system turns swipes into
   UP/DOWN and taps into SELECT, which is all the cards need. Compiled in by
   default; `-DROOTS_NO_TOUCH` removes it. The changelog names the function
   but not its arguments, so if it does not compile, look at
   `Foundation/Event_Service/TouchService` in the SDK header and fix that one
   line.
2. Gesture recognizers, behind `-DROOTS_TOUCH_GESTURES`, for the case where
   the bridge does not cover a custom-drawn Layer window. The code is written
   against the names in the changelog; the signatures are a guess and the
   comment says so. Swipe up = next card, swipe down = previous, tap = select.

## Layout

| File | Purpose |
|---|---|
| `src/c/main.c` | Lifecycle, card navigation, detail window, feel prompt on launch |
| `src/c/cards.c` | All drawing: seven main cards, detail views, the chart helper |
| `src/c/headroom.c` | Night window, overnight RHR, baselines, drain, score, calibration |
| `src/c/history.c` | One record per day in persistent storage; long baseline source |
| `src/c/feel.c` | "How do you feel?" window |
| `src/c/hrv.c` | On-demand HRV test window and RMSSD/SDNN maths |
| `src/c/theme.c` | Palette, per-card colours, dark/light, persisted |
| `src/c/touch.c` | Touch navigation opt-in, optional gesture recognizers |
| `src/c/common.h` | Persist keys and the cache schema number |

`recovery.c`, `recovery.h`, `steps.c` and `steps.h` are gone. Delete them.

## Navigation

```
UP / DOWN        move between the cards you have left switched on
SELECT           open the detail for the card you are on
SELECT (HRV)     start a test; long-press SELECT for the HRV graph instead
SELECT (Metric)  log today's value; long-press SELECT for the graph instead
long-press UP    dark / light theme
BACK             leave the detail (or the app, from a card)
UP / DOWN        scroll, inside a detail view
```

Cards other than Headroom can be turned off on the phone, and Metric is off
until you pick something to track, so most people see fewer than this.

| Card | Figure | Pill | Bars |
|---|---|---|---|
| **Headroom** | headroom 0–100 in a ring | the band | — (one sentence) |
| **Night heart rate** | ± bpm vs usual | your baseline, minutes hard today | last 3 nights, tick = baseline |
| **Sleep** | hours asleep | your usual, window | last 3 nights, tick = usual |
| **Trend** | 7-day minus long-term | both averages, drift verdict | 21 nights, dashed long-term |
| **HRV** | last test, ms | recent average | last 3 tests |
| **Steps** | steps today | your goal, or your average day | today, yesterday, day before |
| **Metric** | last value | change over four weeks | 28 days |
| **Data** | confidence, 3 blocks | clean minutes, window source | — |

On launch, if you have not been asked today, a one-screen prompt comes first:
**How do you feel?** UP/DOWN across five states, SELECT to answer, BACK to
skip. It is asked before the number is visible so the number cannot anchor
the answer, and a skipped day is never asked again.

Every card uses the same shape: figure, pill, bars. Nothing anywhere is
smaller than 18 px.

The Pebble Time 2 has a touchscreen and PebbleOS gained a Touch Screen API in
the July 2026 SDK update, but this build is buttons only. Card navigation is a
single index (`s_card` in `main.c`) with three verbs — previous, next, open —
so swipes map onto it directly once you have the API in front of you.

## Bands

The score maps to intensity language, never to a gate. There is no band that
says stop and none that says rest day.

| Score | Band | Line |
|---|---|---|
| 90+ | Full send | top end is there |
| 72–89 | Strong | hard work is fine |
| 55–71 | Moderate | keep volume, back off the top end |
| 40–54 | Aerobic | easy pace, technique, mobility |
| < 40 | Easy movement | a walk, whatever feels good |

A normal night on baseline lands at 88, i.e. Strong. Full send needs a night
measurably better than your usual, which is the point.

## Steps (v0.5: no filter)

Steps come straight from `health_service_sum_today(HealthMetricStepCount)`.

The v0.4 minute filter is deleted. It could only ever drop two things: minutes
with one or two steps, and minutes inside a sleep session the HealthService
reported. On this watch it removed nothing measurable, which is consistent
with the same sleep-session lookup failing in the score path. There is no
point shipping a filter whose output is provably identical to its input.

If phantom steps show up later, the place to put it back is a rule that works
on data whose units we understand. `HealthMinuteData.vmc` is not that yet —
see the Data card.

## The headroom score

Computed once per morning and cached for the calendar day; the daytime part is
recomputed every minute the app is open.

**1. Find last night.** Three sources, best first, and the Recharge and Data
cards both say which one was used:

| Source | What it is |
|---|---|
| `sleep sessions` | every `HealthActivitySleep` in the night, joined across gaps of ≤ 90 min |
| `still stretch` | longest run of zero-step minutes, bridging trips of ≤ 15 steps |
| `clock guess` | 23:00 onwards. Confidence capped at medium. |

The search range is 18:00 yesterday to 12:00 today, anchored to the calendar
rather than to `now`, so the answer does not depend on what time you open the
app. v0.5 looked back 18 h from the current moment, which quietly truncated
last night if you opened it in the evening.

**Sleep total vs window.** Pebble Health reports a broken night as several
sessions and shows you their sum. `sleep_minutes` is that sum, so the two
agree. `sleep_span_min` is first-asleep to last-awake, and the difference
between them is the time you were awake in the middle; both are on the
Recharge card. Heart rate is read across the whole span, not per session — a
trough is robust to a few awake minutes, and cutting the span up would throw
away readings for no gain.

v0.4 had only the sleep-session source and took the longest single session, so
one failing lookup produced no number at all and a broken night read short.
Missing minutes count as still: the watch being off your wrist is not evidence
that you were walking.

**2. Overnight RHR** from the minute history inside that window:

- keep a minute that has a reading in the 35–110 bpm band
- a jump over 15 bpm is held and only accepted if the next reading confirms
  it — **but only when the two readings are within 2 minutes of each other.**
  Pebble Health does not sample every minute; across a ten-minute gap a 20 bpm
  change is normal physiology, not an optical glitch.
- RHR = mean of the lowest 30 % of survivors (a trough, not a minimum)
- fewer than 20 clean minutes → no number, confidence none

**3. Baseline** = EMA of nightly RHR and sleep minutes. For the first seven
nights the weight is 1/n (a plain running mean), after that α = 0.15
(≈ 14 nights). Medium-confidence nights or better update it; while the
baseline is under a week old, low-confidence nights do too, because a blank
screen is worse than a rough number. The baseline keys are unchanged from
v0.4, so an existing baseline survives the upgrade.

**Backfill.** Pebble Health keeps seven days of minute history, so on a
fresh install (or a reinstall that lost persist storage) `backfill()` reads
the previous six nights, oldest first, before today's. Each becomes a
`DayRecord` and feeds the baseline, so on the first launch you already have
a week of bars on the heart-rate and sleep cards, seven bars on Trends, and
a provisional score for today. A night that cannot be read is retried on the
next three launches and then left alone; the Data detail shows "History
readable N of M" so you can see it working. Nights before the seven-day
buffer cannot be recovered; after that, history accumulates as before.

**Cold start.** Morning one shows the raw night — RHR and hours asleep — with
no number, because the first night *is* the baseline. Morning two shows a
number. Until morning seven the Today card carries "Provisional: night N of
7", confidence is capped at Low, and the detail view says why. Nothing is
hidden; it is just labelled.

**4. Morning score** = 0.7 · heart-rate part + 0.3 · sleep part, each anchored
at 88 for a night on baseline.

**5. Calibration.** Your morning answer maps to a target (Rough 35, Low 50,
Okay 64, Good 80, Great 93 — band midpoints). The difference between target
and morning score feeds an EMA (first answers weighted 1/n, then 0.25),
clamped to ±15, and that offset is added to every score. Shown as "Your
calibration" on the Today detail. If you keep saying Good on Moderate
mornings, the number drifts up to meet you; the user is the ground truth.

**6. Intraday drain.** Minutes since waking, weighted by zone. Zone 3 counts
once, zone 4 twice, zone 5 four times; below zone 3 nothing counts at all.
Ten weighted minutes are free, then one point per six, capped at 20. A
commute, the stairs and a full day at a desk score zero. It is silent — no
notification ever — and tomorrow's overnight reading overrules it entirely.

The zones sit on heart-rate reserve, Karvonen style, between two figures the
app measures rather than assumes:

- **Waking rest** is the 20th percentile of daytime minutes with a reading
  and no steps, averaged over the last fortnight of stored values. Not the
  sleeping trough — see below.
- **The ceiling** is the highest heart rate seen in the last 90 days, floored
  at 160. No age is asked for, because there is nowhere honest to ask it.

For a 68 bpm waking rest and a 185 bpm ceiling that puts zone 3 at 150, zone
4 at 162 and zone 5 at 173. Both figures, and the resulting zone floors, are
on the Night heart rate detail so the threshold can be checked against
whatever else you use.

**Why Drain read 433 minutes before v0.8.** The threshold was the sleeping
baseline plus a fixed 25 bpm. But `baseline_rhr_x10` is a *trough* — the mean
of the lowest 30% of readings inside the sleep window — and a person who
troughs at 54.3 asleep sits at 66–72 at a desk. The line landed at 79, which
standing up clears. Combined with `HR_HOLD_MIN` (each reading stands for the
ten minutes after it, so one sample above the line claims eleven minutes),
coverage approached the whole waking day. The visible symptom was a silly
number; the real damage was that `over / 8` blew past `DRAIN_MAX` at 190
minutes, so every day produced an identical −20 and the intraday figure was a
constant rather than a measurement.

**Why Drain always said 0 min before v0.6.** Pebble Health samples heart rate
roughly every ten minutes at rest, more often when you are active. The
minute records therefore have a reading in perhaps one minute in ten, and
v0.5 only counted minutes *with* a reading. An hour at 140 bpm produced six
"elevated minutes", never reaching the free ones. Each reading now stands
for the minutes after it, until the next reading or ten minutes, whichever
comes first.

Nothing you did yesterday is subtracted. Only the measured overnight response
moves the morning number.

## The motion gate is off

`STILL_VMC_MAX` is **0**, i.e. disabled. v0.4 shipped it at 60, chosen without
hardware data. If overnight VMC on this watch is routinely above 60, that rule
alone rejects the whole night and the Night card shows 0.0 bpm — which is
exactly the symptom v0.4 had.

The Data card now reports `VMC lo/avg/hi` across the minutes that survived, so
the threshold can be set from real numbers instead of guessed. Set it a little
above the high value, not below the average.

We have guessed units on this hardware once already. Not again.

## Reading the Data card

If **clean readings** is zero, the largest of the four counters below it is the
rule that caused it:

| Counter | Means | If it dominates |
|---|---|---|
| No reading | the minute had no heart rate at all | Pebble Health may not be sampling HR overnight — check `HR metric`, and the health settings on the watch |
| Out of range | reading outside 35–110 bpm | widen `HR_MAX_NIGHT`, or the sensor is losing contact |
| Motion gate | VMC above the threshold | only possible if you turned it on; lower it |
| Jump rule | consecutive readings disagreeing by > 15 bpm | raise `HR_MAX_DELTA`, or the strap is loose |

`Window from` tells you whether the sleep-session lookup worked. If it says
`still stretch` or `clock guess` every morning, `health_service_activities_iterate`
is returning nothing on this firmware and that is worth reporting upstream —
it is the same lookup the old step filter depended on.

## Why we don't count steps ourselves (v0.3 finding, unchanged)

On Pebble Time 2 firmware as of August 2026, `AccelRawData` arrives as integer
m/s², not the documented milli-g. At rest |a| reads 6–10 with a resolution of
1, i.e. one LSB ≈ 100 mg and ±200 mg of quantisation noise on a signal that
should read a steady ~9.8. Both the app-side and worker-side services report
the same. Any peak detector either counts nothing or counts sitting still.

The firmware's own counter clearly has finer data internally, so this is an
SDK export bug in the driver for the new IMU. Reported upstream. If it is
fixed, the v0.3 `step_filter.c` in git history is the starting point, at the
v0.1 thresholds (`LOCK_STEPS 6`, `PEAK_THRESH_MG 180`, `CADENCE_TOL_PCT 35`).

## History and Trends

Pebble Health keeps seven days of minute data; anything longer has to be
ours. `history.c` keeps one 14-byte `DayRecord` per day — night RHR, sleep,
morning score, confidence, feel, HRV, waking rest, peak HR — in five 252-byte
persist chunks, 90 days in all, oldest dropped first. The night is written
when it is analysed; feel, HRV, waking rest and peak are filled in during the
day. Waking rest and peak are only written when they change, because
`day_analyse` runs once a minute while the app is open.

`HIST_SCHEMA` guards the layout. Bumping it does **not** wipe the history —
three months of nights is the entire reason the file exists — so every bump
needs a migration in `history_load`. The 1 → 2 migration reads the old
12-byte records at the old 21-per-chunk stride, keeps the newest 90 and
leaves the two new fields at zero.

**Where each series lives.** One rule, after v0.8: a metric's history is in
that metric's own detail, never collected into a gallery elsewhere.

| Series | Card |
|---|---|
| Morning headroom, how you felt | Today, behind SELECT |
| Night heart rate, 60 days | Trend, behind SELECT |
| Sleep, 30 days | Sleep, behind SELECT |
| RMSSD, 30 days | HRV, behind SELECT |

Gaps are nights Roots could not read; a gap is drawn as a gap, never bridged.

**The Trend card is not a trend gallery.** It shows drift and nothing else:
the difference between the short and long baselines as the figure, both
averages in the pill, 21 nights of bars behind them. The dashed line is the
**long-term mean**, not `night.baseline_rhr_x10` — that baseline is a
~14-night EMA, so it climbs with a training block and would sit level under
the bars exactly when drift is happening, hiding the one thing the card
exists to show. This is fixed in v0.8; before it, the reference line was the
EMA.

**Drift.** Once there are three readable nights in the last week and fourteen
in the 90 days before that, the card compares the two averages. A 7-day
average 1.5 bpm or more above the long-term one is reported in plain words.
It is the one thing the daily number cannot show, and the only place Roots
volunteers an interpretation.

## HRV test

Opt-in, foreground only. SELECT on the HRV card opens a 2½-minute window:
30 s settling, then 120 s of guided breathing at six a minute (5 s in, 5 s
out, the circle grows and shrinks). Only while that window is open does the
app request `health_service_set_hrv_sample_period(1)` and a 1 s heart-rate
period; both go back to automatic in the window's unload handler, so an
abandoned test costs nothing either.

Each `HealthEventHRVUpdate` is read with `health_service_peek_hrv_ppi_ms()`.
Intervals outside 300–2000 ms are dropped, an interval identical to the
previous one is treated as the same beat re-reported, and anything more than
25 % away from the median of the last five accepted is dropped as a PPG
artefact. Fewer than 40 clean beats → no number, and the screen says what to
fix. RMSSD (successive differences) and SDNN are both computed and stored;
mean HR and the beat/dropped counts are shown.

**Open question, still open.** Whether the API delivers every interval or
one snapshot per sample period is not documented. At 1 s and ~60 bpm those
are nearly the same thing; at 45 bpm some beats will be reported twice (the
duplicate rule handles that) and at 80 bpm some will be missed, which makes
RMSSD an approximation. Log a few tests with `pebble logs` and compare the
beat count against 120 s × HR/60. If it is consistently short, SDNN is the
number to trust and RMSSD should be relabelled.

The HRV result is **not** part of the headroom score. Paced breathing
maximises respiratory sinus arrhythmia, so this reads higher than resting
HRV and is only comparable with itself. Same time of day, same posture, same
protocol, every time — the value is in the trend, not the number.

## Tunables

All at the top of `headroom.c`. Most likely to need attention after a week on
real hardware: `MIN_CLEAN_MINUTES`, `HR_MAX_DELTA`, `HRR_Z3`, `REST_PCTILE`,
`HRMAX_FLOOR`, `HR_HOLD_MIN`, `STILL_VMC_MAX`. `HRR_Z3` is the one that
decides what counts as work: 70 is the honest zone 2 ceiling, drop it toward
60 if heavy lifting — where the peaks are brief — reads as zero too often. HRV protocol constants (`HRV_SETTLE_S`,
`HRV_MEASURE_S`, `HRV_PACED`, breath timing, artefact gate) are at the top of
`hrv.c`; `HRV_PACED 0` gives a spontaneous-breathing test instead.
`MIN_BASELINE_NIGHTS` and `SETTLED_BASELINE_NIGHTS` are in `common.h`.

## Settings

There is no settings card. Everything configurable lives on the phone, in a
Clay config page, and reaches the watch as a single AppMessage that
`settings.c` writes to one persist blob. The reasoning is in the header
comment of `settings.h`: a config menu driven by four buttons would have cost
two or three screens in a loop the user walks every morning, and a watch app
that grows a settings menu is most of the way to being the dashboard §13 rules
out.

Every field is optional and every default keeps the app working with the phone
disconnected forever. Two are worth calling out:

- **Age and sex** only set the *starting* ceiling for the zones (Tanaka, or
  Gulati for women). The moment Roots has seen a higher heart rate than the
  formula predicts, the measured one wins. They fill the first few weeks;
  they never override what happened.
- **Work starts at** exposes `HRR_Z3`. It is on the page because §8 is honest
  that heavy, low-rep work can sit under the zone 2 ceiling, and the fix for
  that is personal rather than a constant someone else picked.

`METRIC_CHUNKS` and `HIST_CHUNKS` together use about 1.8 KB of the 4 KB persist
budget. That is why the tracked metric is one at a time.

## Known gaps / next steps

- **Verify the HRV API shape on hardware** — see the open question above.
- **Feel vs score agreement** — both series are on the Today detail now; the
  next step is a single agreement figure on the Data card, which is the
  product's actual quality metric.
- **Zone floors on real data** — `HRR_Z3` at 70% of reserve is a reasonable
  first guess, not a measured one. The Night heart rate detail shows the
  waking rest, the ceiling and the resulting floors precisely so they can be
  checked against a chest strap before the constant is trusted.
- **Touch** — see above.
- **Wake-up** — the score is computed on open. A `wakeup` at your usual rise
  time could pre-compute it and drop a timeline pin.
- **HRV** — deliberately absent. It cannot be collected passively and it is
  the input contributing least.
- **Detail views on basalt (168 px)** — the Data view is eleven rows; it
  scrolls with UP/DOWN rather than being cut off.
