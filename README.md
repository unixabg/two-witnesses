# two-witnesses

**Seeing movement with Wi-Fi.** A few cheap ESP32-S3 boards send radio frames
to each other; each board measures how every frame arrived (its channel state
information, CSI: the strength on each of the channel's subcarriers). A
person, a car, a door or a running motor changes those measurements. These
tools record them for hours, show them live in a terminal dashboard, and
read whole runs afterwards: events, rhythms, which pair of boards saw what,
and a map of the boards with the lines that moved.

It grew in a garage, one careful step at a time: planted test signals,
predictions sealed before reading, one change per run, and every habit of the
instrument named before anything is believed. The method matters as much as
the code; the "What changed" sections below keep the reasoning.

- **Hardware**: 2-16 ESP32-S3 boards (the tested ones have the built-in
  antenna), USB power for each; one is cabled to a Linux computer.
- **Firmware**: `tw_node-s3-ch11.bin`, one image for every board: all boards
  send and listen, roles move by themselves. Start with **FLASH-README.md**.
  (The earlier two-board images, one sender and one receiver, are in release
  v5.3.)
- **Version**: the file `VERSION` (`~/tw-kit/version.sh` prints it); releases
  are tagged `vX.Y`, and `next` carries `X.Y-next` between them.
- **License**: Apache-2.0 (LICENSE, NOTICE); third-party parts in
  THIRD-PARTY-NOTICES.md.
- Written by Richard Nelson with Claude (Anthropic) as co-author.

## Once, on the persistent USB

```
tar xzf two-witnesses-kit-v5.3.tar.gz
cd two-witnesses-kit
./setup_once.sh
```
It installs the packages, sets the clock (chrony, America/Chicago), makes
the venv `~/scratch-esp`, copies the kit to `~/tw-kit`, and checks that
`~/tw-data` is really on the USB and not in RAM. Then log out and in once
(serial ports without sudo).

## A long run: a night, a day (one command)

```
~/tw-kit/run.sh 07:00        # before bed;   ~/tw-kit/run.sh 17:00  in the morning
```
- Listens until that clock time (the next time it comes round), one gzip
  file per clock hour in its own folder `~/tw-data/runs/<date>_<start time>/`
  (e.g. `2026-10-26_2210`), about 45 MB an hour. A day run and a night run
  never share a folder.
- Asks for the scene (which cars, doors, people) and keeps it in
  `scene.txt`, shown at the top of `view.txt`. `SCENE="..."` skips the question.
- Blocks sleep; runs in tmux, so the terminal can be closed. **Use mains
  power:** on battery the Chromebook lasted under 5 hours (about 19% an hour).
- When the run ends, sleep is allowed again by itself (setup_once.sh
  allows that one `systemctl` command without a password). If that is not
  set up, it says so and you run `~/tw-kit/run.sh unmask`.
- At the end it makes the **view** by itself: `view.png` (the run at a
  glance) and `view.txt` (the same in words: events and gaps by clock time,
  RSSI shifts, rhythms per hour and per 10 min).
- Watch: `~/tw-kit/dash.sh`, the live dashboard (or `tmux attach -t run`; leave: Ctrl-b then d).
  Stop early: `~/tw-kit/run.sh stop` (the view is still made).
- After a quiet run, keep its `usual.json` and judge later runs by it:
  `USUAL=~/tw-data/runs/<quiet run>/usual.json ~/tw-kit/run.sh 07:00`
  Or, if only part of a run was quiet (say 01:00-05:00), take "usual" from
  that stretch: `QUIET=01:00-05:00 ~/tw-kit/run.sh 07:00`, or afterwards
  `view.py ~/tw-data/runs/<run> --quiet 01:00-05:00`.
- **Low data:** a 10 s window with under half the usual packets is marked
  low data (amber in the picture, listed in the text) and is never called
  an event, nor used for "usual". Under heavy loss the movement measure
  lies: on the afternoon of 2026-10-26 (65-75% lost) it made a 3,830 s
  false event, and even the fake board's random loss doubles it.

## Detection mode: radio + camera (one command)

```
~/tw-kit/watch.sh 07:00            # or: ~/tw-kit/watch.sh 17:00 --nocam
```
Records exactly as `run.sh` does, and alongside it:
- decides once a second whether the radio picture is moving (x10 its usual,
  or x6 two seconds running), and separately whether the camera picture is
  changing (x4 its usual);
- keeps the camera running at 1 frame a second in a short rolling buffer, so
  when either fires it saves frames from **5 s before** to 10 s after;
- never fires on seconds with under half the usual packets (marked low data).

Each event is **radio+camera**, **radio only** (a habit, or something hidden
in plain sight) or **camera only** (a radio miss, or a light change).
Everything is in `~/tw-data/watch/<date>_<start time>/` (the recording and
its view at the top, as in a run); look at `detect/sheet-01.png`
(one row per event) and fill in `detect/events.csv`:

| label | when |
|---|---|
| person | someone is in the frames |
| car | a car moves |
| door | the garage door moves, nobody visible |
| animal | an animal |
| light | lights on or off, sunlight, headlights |
| nothing | frames look the same throughout |
| dark | too dark to tell |
| other | anything else (say what in `note`) |

Then send me `events.csv`, `seconds.csv` and the sheets. The detector's calls
are made before anyone looks, so they stay blind; your labels are the truth.
Stop early: `~/tw-kit/watch.sh stop`. Look in: `tmux attach -t watch`.
Notes: the built-in camera has no infrared, so in the dark the frames are
black (the event still counts; it gets the hint "dark"). Frames stay on the
Chromebook.

## A short test (one command)

```
~/tw-kit/short_run.sh cars2-laps-03 600
```
Checks the clock, the receiver and the camera; offers to seal your
expectation; counts down 10 s; runs camera and recorder together; stops on
time; reports rate, loss, and whether the board's and the computer's clocks
agree. Everything lands in `~/tw-data/short/<name>/`. It refuses a name
that already exists. Options: `--nocam`, `--delay S`. Set `COPY_TO=user@host:dir/`
to copy the folder off as well.

Send `<name>.csv` for the blind reading; send the seal only after the reading
is posted.

## Many boards (v5.0): tw_node

Read **FLASH-README.md** first: flashing, board numbers, the first bench
test, and how to go back to the two-board images.

- Boards are numbered 1-16: give each board its number once with
  `tw_setid.py PORT N`. Whichever board is on the computer prints
  everything; the lowest-numbered board heard keeps time. Both roles move by
  themselves if a board goes quiet. A second board on a second computer
  records the same data, as a hot standby.
- `run.sh`, `watch.sh`, `survey.sh`, `dash.sh` work unchanged (same port).
  The dashboard shows the boards and one row per link (`2→1` = board 1
  hearing board 2); `[` `]` choose the link shown in detail and in the
  placement view.
- `watch.py` judges every link and fires when any one moves; `seconds.csv`
  and `events.csv` name the link (`radio_link`, "2-1").
- Afterwards (v5.1: `run.sh` and `watch.sh` do this by themselves at the
  end; by hand: `after_run.sh RUN_FOLDER`, or `split_links.py RUN_FOLDER --view`)
  `links/<from>-<to>/` folders in the usual format, so `view.py`,
  `analyze_csi.py` and `radar_replay.py` work per link; `links/summary.txt`
  lists every link, and **`links/across.txt` compares them** (see v5.1).
- The protocol was desk-tested in a simulator running the same code
  (`firmware/sim/`); the radio part is untested until your first flash.

## The live dashboard (v4.8): dash.sh

```
~/tw-kit/dash.sh
```
Over SSH, while `run.sh`, `watch.sh` or `survey.sh` is going: a live screen,
refreshed each second, with packets/s and loss, RSSI (now, 10 s average,
min/max, steadiness), the noise floor and how far the signal is above it,
radio motion (as watch.py measures it), sparklines of the last 1, 5 or 30
minutes, the latest watch events, the newest log lines, and the computer's
health (disk, mains, sleep block, clock sync). It only listens: closing it
never touches the recording.

Keys: `m` mark this spot (saves the last 30 s to placement.csv), `p`
placement view, `g` map view (v5.3), `r` reset min/max, `+`/`-` sparkline span, `q` close it
(`dash.sh` opens a fresh one). Ctrl-b then d leaves it running in tmux.

## Placement test: survey.sh

```
~/tw-kit/survey.sh            # two hours, or: ~/tw-kit/survey.sh 11:30
```
Records (into `~/tw-data/survey/<date>_<time>/`) and opens the dashboard in
its placement view: a big RSSI bar with GOOD / FAIR / POOR (solid: above
-70 dBm and under 10% lost). Put the board somewhere, wait 30 s without
moving, press `m` and type where it is ("sender 6 m, chest height"). Walk,
mark, walk, mark: `placement.csv` is your table of real distances. Stop with
`~/tw-kit/survey.sh stop`. The board on the Chromebook's cable stays put;
move the others.

## A look without recording: live.sh (v5.1)

```
~/tw-kit/live.sh              # up to 4 hours, or: ~/tw-kit/live.sh 1
```
The same dashboard, with nothing saved (only `~/tw-data/live/listen.log`),
for a quick check of the boards. `q` in the dashboard stops it; Ctrl-b then
d leaves it running; `~/tw-kit/live.sh stop` from another terminal. Sleep is
not blocked. It holds the board's port, so it cannot run beside run.sh,
watch.sh or survey.sh (they say so).

## What changed in v5.4-next (work in progress, on the next branch)

- **One firmware image** (`tw_node-s3-ch11.bin`); the two-board images and
  their guide are in release v5.3. `FLASH-README.md` is the one flashing guide.
- **`VERSION`** is the one place for the version; `tools/sums.sh` refreshes
  SHA256SUMS after a change.
- **Install from a git clone:** `git clone https://github.com/unixabg/two-witnesses`,
  then `./setup_once.sh` inside it makes `~/tw-kit` a link to the clone, so
  `git pull` there updates the installed kit (a copied `~/tw-kit` from before
  is moved aside to `~/tw-kit.before-link-<date>`, not deleted). From a
  downloaded copy (no `.git`), setup copies the files as before, without
  `.git` and `private/`.
- **Your own files in `private/`** (ignored by git, never committed or pushed):
  the kit reads the board layout from `private/boards.csv` first, then
  `~/tw-data/boards.csv`. Setup puts the example layout there if you have
  none; `version.sh` says which layout is in use.

## What changed in v5.3 (2026-10-30): map, fast readout, standing scene (no reflashing)

- **`version.sh`**: `~/tw-kit/version.sh` prints the kit version (from
  `VERSION`), checks every file against SHA256SUMS, and shows the board
  layout and the standing scene in use.
- **The board layout, `~/tw-data/boards.csv`** (from v5.4: `private/boards.csv` first; setup_once.sh copies
  `boards-example.csv` there if there is none): one row per board (number,
  x and y in feet, facing N/S/E/W, a note) and landmarks such as a big metal cabinet.
  Edit it when a board moves; each run copies it into its folder, so old runs
  keep the layout they had. `tw_map.py ~/tw-data/boards.csv` draws it and
  lists the distances.
- **Dashboard map view (key `g`)**: the boards to scale, north up, each line
  drawn by how much it stirs right now: `.` quiet, `-` a little, `=` moving,
  `#` strongly, `?` no data. Motion and the last 2 s of loss decide; the
  stronger direction of each pair counts.
- **Dashboard fast readout**: a `2 s` loss column beside the 10 s one, so a
  person crossing a line shows within a second or two.
- **A small map for each moment in `links/across.txt`**, lines lit by what
  each link showed then (events and path loss; delivery losses are left
  dark: they are plumbing, not sensing).
- **The standing scene, `~/tw-data/scene-now.txt`**: what is true in the
  garage now ("car 1 in, board 2: new adapter + cable"). At the keyboard,
  run.sh and watch.sh show it: Enter keeps it, anything typed replaces it.
  Cron runs, with nobody to ask, use it as it is. Each run's scene.txt says
  where its scene came from. `SCENE="..."` still gives a one-run scene.
- `tests/gen_run.py` makes a synthetic 3-board recording with
  planted features, for testing without real data.
- Every night: `0 0 * * * $HOME/tw-kit/run.sh 07:00 >> $HOME/tw-data/cron.log 2>&1`
  (write your home folder out in full in the crontab)
  (`crontab -e`; `0 0 * * *` = midnight every day).
- Desk test: run.sh through the simulator with nobody at the keyboard (the
  standing scene and the layout landed in the run folder; the dashboard map
  drew the garage; the automatic views drew the planted walk on the 1-2 line
  in across.txt); the scene prompt at a terminal (typed text became the
  standing scene, Enter kept it).

## What changed in v5.2 (2026-10-29): the two losses told apart (no reflashing)

A frame on a many-board link can go missing on two trips:

- **path**: the measuring board never heard it. That is sensing: a person or
  a car between the two boards shows here, so a rise in path loss between a
  pair is a detection of its own.
- **delivery**: the board heard it, but its report never reached the
  computer (reports ride in the board's own frames, over its path to the
  cabled board). That is plumbing.

Every board numbers its reports one after another, and every frame carries
the sender's frame number, so the two can be told apart per 10 s:
delivered / (fraction of the board's reports that came) = frames heard;
sent - heard = path lost. (The overnight of 10-29: 2-3 lost 17%, but its
path, like its mirror 3-2, loses about 3%: the rest was delivery.)

- `split_links.py` writes `links/<link>/loss.csv` (per 10 s: sent,
  delivered, heard, path %, delivery %) and `summary.txt` shows
  missing / path / delivery per link. `--again` re-splits a run split by an
  older kit (needed once, to get the two losses):
  `split_links.py ~/tw-data/runs/2026-10-29_0000 --again --view`
- `view.py`: the packets line gives both; every LOW DATA stretch says
  "mostly PATH" or "mostly DELIVERY"; a new list **PATH LOSS RISES**:
  10 s windows where the pair stopped hearing each other (usual + k x spread,
  at least 15 points above usual), drawn purple; strip 4 has the path loss.
- `links/across.txt`: path and delivery columns; path loss rises are
  moments like events (on several links, or ONE LINK ONLY); low data on
  every link is WHOLE CHANNEL when it was path loss, ALL REPORTS MISSING
  when it was delivery (the cabled board or the computer).
- Dashboard: each board shows "reports home NN%" (last 10 s).
- Desk test: the synthetic recording with board 3's reports 15% lost all the
  time, board 2's 65% lost for 200 s, and a 60 s path loss rise on 2-3 alone:
  path and delivery per link within 0.1 point of the truth; the 200 s
  stretch called DELIVERY on 1-2 and 3-2; the 2-3 rise and the 1-3 stretch
  called PATH, one link only; the all-link dropout WHOLE CHANNEL. Without the
  planted features: nothing reported.

## What changed in v5.1 (2026-10-29): computer side only, no reflashing

- **Loss on relayed links is right.** `view.py` counted each record that
  reached the computer a frame late (relayed, after a newer one) as two
  losses: the overnight of 10-29 showed 9.8% on 1-2 (true 3.0%) and 31% on
  2-3 (true 17%). It now counts in measurement order. Links measured by
  the cabled board were always right (2-1: 4.5%).
- **The end of a run makes the right views by itself**: `after_run.sh`
  (called by run.sh and watch.sh) sees a many-board recording and runs
  `split_links.py --view`; a two-board one gets `view.py` as before.
- **`links/across.txt`** (`links_report.py`), the links side by side:
  - MOMENTS ON TWO OR MORE LINKS: events, low data, RSSI shifts within a
    minute of each other, which links showed them and which stayed quiet
    (a car leaving: 1-2 and 2-1 yes, 2-3 quiet). Low data on every link at
    once is marked WHOLE CHANNEL, with each board's record rate and noise
    floor then, and whether the timekeeper changed.
  - ON ONE LINK ONLY.
  - RHYTHMS THAT COME BACK: rates found among the top peaks of the
    10-minute pieces more often than chance allows, marked ONE LINK (a habit
    of that board or path) or SEVERAL LINKS, with multiples grouped
    (1.1733 / 2.3467 Hz). Rates under 0.15 Hz are listed apart: a test with
    pure noise showed 0.03-0.13 Hz on every link, the method's own edge.
- **Faster**: `split_links.py` about 4x (same output, checked byte for
  byte); views made several at once (one per processor core).
  `--view-only` remakes the views and the report of a run split before, e.g.
  last night's: `split_links.py ~/tw-data/runs/2026-10-29_0000 --view-only`.
- **Finished sessions close themselves**: when a run, watch or survey is
  done (views and sheets made), its window stays 30 minutes, then the tmux
  session closes. A finished session still open is closed by the next start,
  so it can no longer block a cron start. A session that is still busy
  (making its views) is never touched.
- **`live.sh`**: the dashboard with nothing saved (`listen.py --no-save`).
- Desk tests: a synthetic 75-minute, 3-board recording with planted
  features (whole-channel dropout, one-link loss stretch, an event and
  RSSI step on 1-2/2-1, 1.1733 Hz with its double on every link, 4.3067 Hz
  on 1-2 only): every one found and placed right, nothing else reported; the
  same recording without the features: nothing reported. run.sh through the
  simulator on a pty: recording, automatic split and views, the session
  closing, the next start not blocked.

## What changed in v5.0 (2026-10-28)

- `tw_node-s3-ch11.bin`: one firmware for every board (source in
  `firmware/tw_node/`), built from Espressif's csi_send/csi_recv at the same
  commit, ESP-IDF v5.5.6. Fixed 3 ms slot per board number; timekeeper and
  exit roles that move; each record sent twice while 6 boards or fewer;
  compact records above that; boot number + record number for
  de-duplication; hop count kept for multi-hop later.
- `firmware/sim/`: the same protocol code in a pretend garage. It found two
  real faults before any board was flashed: boards the timekeeper had not yet
  heard all shared one spare slot and collided forever (8 boards: some links
  0%), and after a reboot a board's records were thrown away as duplicates
  for 15 s. Both fixed (slots by board number; boot numbers).
- `tw_setid.py` (board numbers), `split_links.py` (per-link folders).
- `listen.py` records tw_node lines; `dash.py` board table and one row per
  link; `watch.py` every link; `radar_replay.py` compact records and the
  recording's own packet rate (`--interval-ms`; 10 ms as before for the
  single-link recordings, so their results are unchanged).
- Desk tests: simulator scenarios above; simulated boards -> pty ->
  listen.py -> dashboard (6 links, board death shown as NO DATA); split and
  view per link (planted walk on link 2-1); watch.py on three simulated
  boards caught the planted walk at the right seconds and named link 1-2,
  no false events; watch.py on the single-board fake stream unchanged (walk
  x52.7, 40 s of 70% loss marked LOW DATA, no false events).

## What changed in v4.8 (2026-10-28)

- `dash.py` / `dash.sh`: the live dashboard above; `run.sh` and `watch.sh`
  open it in a tmux window of their own (`run:1`, `watch:2`).
- `survey.sh`: placement test with marks.
- `listen.py`: `--tap` may be given more than once (watch.py on 47000, the
  dashboard on 47001); each copy still never holds up the recording.
- `run.sh stop` now targets the listener window explicitly (with a second
  window open, a bare Ctrl-C could have gone to the wrong one).
- American spellings in the code (neighbor, analyze, normalized, gray).
- Desk test (fake board): the planted 7 s gap, walk (motion x37-51) and 40 s
  of 70% loss (LOW DATA, 71% lost) all showed live; marks saved; run.sh,
  watch.sh (detector and dashboard together) and survey.sh ran through and
  ended cleanly; the dashboard used about 1% of a core here.

## v4.7.1 (2026-10-28): garbled lines no longer stop radar_replay.py or analyze_csi.py

On the real 10-27 recording, radar_replay.py stopped at the 07:00 file: a
line damaged on the serial link (bytes lost, e.g. two commas in a row) makes
numpy raise an error instead of returning a short row. Both tools now skip
such lines and count them, as view.py and watch.py already did. Desk test:
an hour with about 266 lines damaged in five different ways; every tool ran
through, and the planted walk was found as before.

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

## What changed in v4.6 (2026-10-26, later): names for any time of day

The tools were named for nights ("night.sh", "morning view") but the kit
runs by day too, and two runs on one date needed the `TW_DATA=.../day`
trick. Everything is now named for a *run*, and each run has its own
folder named by date and start time. Old data folders still read:
`view.py` also accepts `night-*.csv.gz`.

| was | is now |
|---|---|
| `night.sh 07:00` | `run.sh 07:00` (UNTIL is required: a run is not always a night) |
| `listen_night.py PORT OUTDIR` → `OUTDIR/<date>/` | `listen.py PORT RUN_FOLDER` → straight into the folder |
| `morning_view.py` → `morning.png/.txt` | `view.py` → `view.png/.txt` |
| `evening_run.sh` → `~/tw-data/evenings/` | `short_run.sh` → `~/tw-data/short/` |
| `~/tw-data/nights/<date>/` | `~/tw-data/runs/<date>_<HHMM>/` |
| `~/tw-data/watch/<date_time>/<date>/...` | `~/tw-data/watch/<date>_<HHMM>/...` (flat, plus `detect/`) |
| `night-YYYYMMDD-HH.csv.gz` | `csi-YYYYMMDD-HH.csv.gz` |
| `night.log` | `listen.log` |
| the chain script `run.sh` in the folder | `commands.sh` (`commands-recorder.sh`, `commands-detector.sh` in watch) |
| tmux session `night` | `run` |
| `fake_board.py night OUTDIR` | `fake_board.py run RUN_FOLDER` |

## What changed in v4.5 (2026-10-26, evening)

The three things owed from the nights before, and what a read-through of the code found.

- `view.py`: **low-data windows** (under half the usual packets,
  `--min-data`) are marked, listed, drawn in amber, and never called events;
  a 10-min rhythm piece with that little data is skipped. `--quiet
  HH:MM-HH:MM` takes "usual" from a chosen stretch of the night and still
  writes `usual.json`. `scene.txt` in the folder is shown at the top.
  Files are put in clock order whatever their names. Desk test: a planted
  10 min of 70% loss came out as `LOW DATA 22:24:00 - 22:34:00, 25
  packets/s, 72% lost` and no event; the planted walk, gap and RSSI step
  were found as before.
- `analyze_csi.py`: **damaged receiver timestamps** (a garbled header value
  such as -156: one sample whose clock jumps wildly out and back) are
  skipped and counted. The bracket is now parsed by numpy in one go: an
  hour file takes 6 s instead of minutes; output on the same file is
  byte-identical to v4.4.
- `run.sh` / `watch.sh`: **a bug in the "did it start" check.** `pgrep -f
  "listen.py ..."` also matched the tmux shell that runs the chain,
  so a listener that refused at once (RAM folder, say) was still reported
  as "listening until 07:00". The check is now anchored to the python
  process itself. The chain each window runs is written to `run.sh` in the
  night folder (no quoting inside tmux, and a record of what ran).
  `run.sh stop`, `run.sh unmask`, the scene question, `QUIET=`.
- Sleep block: `setup_once.sh` writes `/etc/sudoers.d/tw-sleep` (validated
  with `visudo -c`) allowing exactly `systemctl mask|unmask` of the four
  sleep units without a password, so the night unblocks sleep by itself
  and asks for no password at the start. Without the rule everything works
  as before (one password at the start, unmask by hand).
- `listen.py`: a restart within the same hour writes
  `night-...-HH.part2.csv.gz` instead of appending a second gzip member,
  which could never be read after a file cut short by a hard kill. A crash
  (disk full, say) is named in `listen.log` before the listener falls.
- `fake_board.py`: plants a **loss** stretch (10 min of 70% in `night`, 40 s
  in `live`) so the low-data handling can be desk-tested.
- "COM port" wording fixed in FLASH-README.md and short_run.sh: the
  receiver is read on the board's native USB port.

## What changed in v4.3 (2026-10-26)

- Detection mode (`watch.sh`, `watch.py`); `listen.py --tap PORT`
  copies each line to the detector without ever waiting for it.
- Desk-tested with the fake board and a fake camera: a planted 7 s gap was
  marked low data and did not fire; a camera-only box was caught appearing
  and vanishing (camera only); a planted walk with the box came out as one
  radio+camera event, radio x51. The first version lost camera results when
  two frames landed in one second; fixed (each frame keyed by its own time,
  each second decided 4 s later).
- `short_run.sh`: the clock check skips damaged timestamps (the false
  "clocks disagree" of 2026-10-25).
- **The receiver is on the board's native USB port** (lsusb: 303a:1001
  Espressif USB JTAG/serial), not the COM port. It works (4% loss); keep it
  on that port so runs stay comparable.

## What changed in v4.1 / v4.2, after the first real night (2026-10-24)

- **Serial reading, the big one.** `listen.py` and `record_csi.py`
  read the port line by line with `readline()`, which fetches one byte at a
  time: 47% of a fast server core at 91 lines/s. The Chromebook could not
  keep up, the USB chip dropped chunks of 5-9 lines, and the night lost 21%,
  then 45%, with 6-10% of lines garbled where chunks went missing. Of the
  missing packets, 76-82% went in those dropped chunks; between clean lines
  the loss was only 7-8% (the radio, as in earlier garage runs). Both now
  read in blocks: 2% CPU including gzip. Replaying 16,380 real lines at
  91/s: every one arrived, byte for byte.
- `listen.log` now shows, each 10 minutes, CPU use and "run-together lines"
  (two lines joined where bytes were lost). Both should stay near zero.
- `view.py` skips garbled lines instead of crashing.
- `run.sh`: systemd-inhibit asked for a password inside tmux and hung;
  it now masks sleep in the open instead. It refuses to start if the
  port needs sudo, and warns when on battery.

## The tools

| File | What it does |
|---|---|
| `run.sh` | a long run: the routine above |
| `dash.sh`, `dash.py` | the live dashboard of whatever is running |
| `tw_node-s3-ch11.bin` | the firmware for every board; see FLASH-README.md; source and simulator in `firmware/` |
| `tw_setid.py` | give a tw_node board its number (or show which it is) |
| `split_links.py` | a many-board recording into one folder per link, for view.py / radar_replay.py; `--view`, `--view-only` |
| `links_report.py` | the links of a many-board run side by side: `links/across.txt` |
| `after_run.sh` | the views of a finished run folder, one link or many (run.sh and watch.sh call it) |
| `live.sh` | the dashboard with nothing saved |
| `version.sh`, `VERSION` | which kit this is, and whether every file is intact |
| `tools/sums.sh` | refresh SHA256SUMS after a change (before each commit) |
| `tw_map.py`, `boards-example.csv` | the board layout drawn to scale (dashboard map, maps in across.txt) |
| `tw_lib.sh` | shared by the .sh scripts (sessions that close themselves) |
| `ROADMAP.md` | what is planned and what is being looked into |
| `survey.sh` | placement test: dashboard in placement view, marks to placement.csv |
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
