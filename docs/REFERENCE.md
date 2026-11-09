# Reference: every tool, and how the analysis judges

The README covers everyday use. This page has the details: the full list of
tools, how `view.py` decides what is an event, low data and a rhythm, the
esp-radar replay, and the desk tests.

## The tools

| File | What it does |
|---|---|
| `run.sh` | a long run: the routine above |
| `dash.sh`, `dash.py` | the live dashboard of whatever is running |
| `tw_node-s3-ch11.bin` | the firmware for every board; see FLASH-README.md; source and simulator in `firmware/` |
| `tw_setid.py` | give a tw_node board its number (`board N`; or show which it is and every board's settings); `power DBM` / `power default` sets every board's transmit power, `relay off` / `relay on` relaying (network settings; tries up to 3 times until the board reports it); `check` (30 s: two boards with one number, firmware, settings, weak links); `survey`, `channel N`, `group N`; its head lists every command |
| `flash_board.sh`, `flash_board.py` | set up one board on the cable, step by step: flash it (whole image or program part), its group and number, its group's settings (the channel for a new group's first board), its LED, then `tw_setid.py PORT check`; asks before each change |
| `split_links.py` | a many-board recording into one folder per link, for view.py / radar_replay.py; `--view`, `--view-only` |
| `links_report.py` | the links of a many-board run side by side: `links/across.txt` |
| `replay.py` | a page that replays a many-board run on the board layout: each link both ways, second by second (change, loss, shape; a link's quiet level learned again after its lasting changes), a timeline, events, lasting changes (shape or RSSI steps), your own notes; on watch runs the camera's events, labels and frames (linked; a camera panel follows the cursor); long events as stretches with the peaks inside them; "About this recording" (firmware, power and its changes, relaying, bandwidth, thresholds); rings around a board when everything lit is near it, hollow when lit one way only; drawn as waves (each board a beacon, a fan per path) or lines (`--view waves|lines`, a switch on the page); `--settle MIN` (when a new state counts as usual; 15), `--title`, `--from`, `--to`, `--note`, `--out` |
| `after_run.sh` | the views of a finished run folder, one link or many (run.sh and watch.sh call it) |
| `live.sh` | the dashboard with nothing saved |
| `version.sh`, `VERSION` | which kit this is, and whether every file is intact |
| `tools/sums.sh` | refresh SHA256SUMS after a change (before each commit) |
| `tw_layout.py` | the board layout you keep: show it, `confirm` (is it still right?), `edit` (value by value), `backups` (put an earlier version back), `moves SURVEY_FOLDER` (write a survey's moved boards into the layout), `check RUN_FOLDER` (the layout against the radio: each link's signal against its length, swapped numbers, a board heard much worse one way; with each board's transmit power as reported), `keep`, `path`; every version kept in `layouts/` beside it; `TW_DATA` other than ~/tw-data: that site's own |
| `tw_map.py`, `boards-example.csv` | the board layout drawn to scale (dashboard map, maps in across.txt) |
| `tw_lib.sh` | shared by the .sh scripts (sessions that close themselves) |
| `ROADMAP.md` | what is planned and what is being looked into |
| `survey.sh` | placement test: first the layout (still right? edit, earlier versions), the scene; 2 quiet minutes to check the layout against the radio (layout-check.txt); then the dashboard; at each mark, which board was moved there and to which x y (moves.csv, on the map at once); `survey.sh stop` offers the moves for the layout in placement view, marks to placement.csv; the folder keeps the layout and scene it started with |
| `watch.sh`, `watch.py` | detection mode: live radio + camera detector, kept frames, events to label |
| `listen.py` | the listener itself: hourly gzip, flushed each minute (a power cut loses at most a minute), reconnects if the board is unplugged, refuses to write into RAM |
| `radar_replay.py` | Espressif's esp-radar detector replayed on a recorded run, compared with watch events and labels (`radar_emu.py`, `radar_c3.elf`, `radar_c3_harness.c`, `LICENSE-esp-radar.txt` go with it) |
| `view.py` | the view of a run folder; `--usual FILE`, `--quiet HH:MM-HH:MM`, `--k 8`, `--min-data 0.5` |
| `short_run.sh` | a short test: the routine above |
| `record_csi.py` | plain recorder (as before) |
| `analyze_csi.py` | blind reader (as before); now also reads `.csv.gz`, e.g. one hour of a night |
| `seal.py` | seal / open an expectation (as before) |
| `fake_board.py` | a pretend receiver with planted signals, for testing at the desk (see below) |
| `setup_once.sh` | the one-time setup |

## Relaying, in detail (tw_node 2.0)

- **Hearing reports.** Every frame says how well its sender hears each board:
  the share of that board's frames it received, smoothed over about the last
  second (its frame numbers tell how many were missed). The dashboard shows
  `heard N%` for a board the cabled one hears under 90%.
- **Stranded.** A board is stranded when the exit (the lowest-numbered board
  with a computer) hears it under 70%.
- **Relays.** Boards that hear the stranded board at 80% or more, are heard by
  the exit at 80% or more, and are not exits themselves. The lowest-numbered
  of them, up to 8, share the work: relay *i* of *k* carries the records whose
  number leaves *i* when divided by *k*. Every board works this out from the
  same reports, so no extra messages are needed; if two boards briefly
  disagree, a record is carried twice and the copy is dropped.
- **Room.** A relay sends its own new records first, then relayed ones (once
  each), then second copies of its own. Relaying never adds frames or slots;
  it fills room in frames already sent.
- **Records** carried by a relay have hop = 1 in the TWR line; `summary.txt`
  counts them per link ("came home through a relay").

Simulator (`firmware/sim`, 60 s, board 1 missing 75% of board 2's frames for
40 s, 5% random loss everywhere): board 2's reports reaching the computer,
relaying off / on:

| boards | off | on | airtime off / on |
|---|---|---|---|
| 3 | 61% | 97.6% | 8.0% / 8.8% |
| 6 | 53% | 94-95% | 25.9% / 25.9% |
| 16 | 46% | 91-95% | 25.2% / 26.1% |
| 16, two boards stranded | 46% | 74-91% | 26.7% |

Board 1 not hearing board 2 at all: 93% got home (the relay sends each once).
The relay switched off for 15 s mid-run: relaying stopped and resumed within a
second. Path loss on the blocked direction is unchanged: it still shows.

**Six boards, more cases (simulator, 2026-11-04;** 120 s, 3% random loss
everywhere, seed 7): each board's reports that reached the computer.

| case | reports home | airtime |
|---|---|---|
| 3 boards, healthy | 99.9% each | 9% |
| 6 boards, healthy | 97.6-97.8% each | 26% |
| 6: board 1 misses 75% of board 2 for 80 s, relay off / on | board 2: 51.5% / 96.2% (49% via relays) | 26% |
| 6: board 1 hears nothing of board 2 for 80 s, relay on | board 2: 93.7% (65% via relays) | 25% |
| 6: boards 2 and 3 both 75% shadowed from board 1, relay off | 51% / 52% | 26% |
| same, relay on | board 2: 83%, board 3: 53% | 26% |
| 6: board 1 misses 25% of every board | 73-75% for every board | 24% |
| 6: board 1 misses 40% of every board | 59-64% | 26% |
| same 40%, board 4 cabled instead | 97.4-97.7% everywhere | 26% |

Why: a frame carries at most 1400 bytes, which is 6 full records (28-byte
header + 192 amplitudes). With 6 boards each frame brings 5 new records of its
own, so one place is left for second copies and relayed records. Hence 97.6%
rather than 99.9% when healthy (most second copies no longer fit), one
stranded board recovered (96%) but two at once only partly (83% and 53%: the
relays have one place each per frame), and a cabled board that hears everyone
somewhat badly (above the 70% that counts as stranded) costs every board what
it misses, with nothing to carry it. The fix that needs no code: cable the
board that hears all the others best (the survey's layout check and the
dashboard's `heard N%` show which). Room for more: tw_node 2.1 sends a second
frame in the slot when records wait (docs/SECOND-FRAME.md: the same cases
with it: 6 boards healthy 99.9%, two stranded boards 98.6% each).

## How the view judges

- **Movement**: each 10 s, how much the CSI amplitude wobbles (sd / mean per
  live subcarrier, then the median). An **event** is a run above
  usual + 8 robust spreads. "Usual" is the night itself, or a quiet night's
  `usual.json`.
- **Rhythms**: the analyze_csi.py recipe on each 10-minute piece. A rate that
  stands out in at least half the pieces is a **habit** (the fridge's
  0.23 Hz, say). Each piece also shows its strongest peak that is *not* a
  habit, marked `NEW, notable` at x20 or more. That is where breathing would
  show.
- **Gaps**: 5 s or more with nothing received. **RSSI shifts**: a 5-minute
  mean that moved 0.5 dB or more. **Low data**: 10 s windows with under half
  the usual packets; listed, drawn in amber, never events.
- Clock times come from the computer's clock (keep chrony running).
  It prints times and numbers only: no labels, no guesses.

## Espressif's own detector beside ours: radar_replay.py

```
~/scratch-esp/bin/python ~/tw-kit/radar_replay.py ~/tw-data/watch/2026-10-27_0607 --train 08:00-09:00
```
Replays a recorded run through **esp-radar**, Espressif's presence and
movement detector for these boards (esp-radar 0.3.4, as its `console_test`
example runs it on an ESP32-S3 with default settings), and sets its calls
beside the watch events and your labels. No reflashing: it works on any run
already recorded.

- `--train HH:MM-HH:MM`: calibrate on a stretch you know was empty, as the
  board's `radar --train_start` / `--train_stop` does. Without it,
  Espressif's untrained defaults are used (movement only, no presence).
- Writes `radar.txt` (summary, and "caught N of M" for each watch kind and
  each label), `radar_events.csv`, `radar.csv` (every window), `radar.png`.
- Fill in `detect/events.csv` labels first, then run it: the comparison by
  label is the useful part.
- About 1-2 minutes per recorded hour on the Chromebook.

How it is faithful: Espressif ships the windowing and training as source
(Apache-2.0) and the core math only as a compiled library. The windowing,
training and the console's decision rule are copied line for line from the
source. The core math was worked out from the library's symbols and debug
info (covariance of the window, top eigenvector by the power method;
jitter = largest 1-|correlation| with the last 3 windows; wander = smallest
1-|correlation| with the trained shapes) and **checked against Espressif's
compiled code itself** running under an emulator (`radar_c3.elf`, the
library's ESP32-C3 build): over 3,000 windows the two agree to 3e-7.
`--check 3000` repeats that check on your own data; `--emulate` runs the
whole replay on the original (about 100x slower).

What differs from a board running esp-radar live: our receiver turns on
the chip's channel filter and LTF merge (esp-radar leaves them off), so the
CSI it sees is slightly smoothed. A live check would be a spare board
flashed with Espressif's `console_test`, which already listens for
our sender's address.

Desk test (fake board, one hour, trained 22:01-22:10): the planted walk
found exactly (22:41:24-22:42:54), nothing during the planted 10 minutes of
70% loss, the same result from the real 384-byte line layout. Untrained,
Espressif's default line sits below the fake board's noise and fires a lot:
train it.

## Tested at the desk (2026-10-24) with fake_board.py

- Listener: hourly files, the 7 s planted gap logged at the right time, loss
  counted right; killed hard partway through a file, and the file still read
  up to the break (229 of 230 s).
- RAM check: refuses tmpfs and a live overlay whose writable layer is in
  RAM; accepts an overlay whose writable layer is on disk (persistence).
- analyze_csi.py: output identical to the old version on the same file.
- Morning view, **blind**: a random plan was hidden in a file; the reading
  was sealed before opening it. Planted vs found:
  rhythm 0.419 Hz 01:37:57–02:06:32 vs 0.418 Hz 01:38:00–02:06:30;
  gap 01:54:41–01:55:06 vs the same; walk 04:08:15–04:10:02 vs
  04:08:10–04:10:10. No false events.
- Evening run: camera and recorder together, clock burned into the video,
  both clocks agree, seal written.

Not tested here (no hardware in the cloud): the real board, the real
camera, chrony, systemd-inhibit, apt on the USB. The fake board is cleaner
than the garage, so its ratios run higher than real ones; set thresholds
from a real quiet night.

Try the tools without hardware:
```
python3 fake_board.py run /tmp/fn --hours 3              # a pretend night
python3 view.py /tmp/fn                                  # read it, then
cat /tmp/fn/PLANTED.txt                                  # see what was planted
```
