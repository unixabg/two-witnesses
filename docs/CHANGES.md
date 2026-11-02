# What changed, version by version

Newest first. Each entry keeps the reasoning and the desk tests behind the change;
the README has only what you need to run the kit.

## What changed in v5.4 (2026-11-02): transmit power, relaying, the CSI view (one reflash for every board)

- **Dashboard: the CSI itself (`c`).** The chosen link's amplitudes across the
  channel, one row a second, newest at the top, as "." (weakest) and 1..9.
  Press once for the shape (a column of dots that stays is a notch, as a
  strong reflection makes); again for the change from the last minute ("."
  unchanged, 9 = 3 dB or more: movement lights it up); a third time to close.
  In the shape, each side of the `|` (L-LTF, HT-LTF) has its own scale: the
  L-LTF part comes in much weaker and was squashed into dots. The footer says
  whether anything is being recorded (live.sh: nothing is). Everything else on the dashboard is computed from these numbers.

- **Firmware tw_node 2.0: relaying.** In a run at 11 dBm, when a car parked
  between boards 1 and 2, board 1 heard only about a quarter of board 2's
  frames. That is sensing (it shows as path loss), but board 2's own
  measurements travel home in those same frames, so 56% of them were lost
  too, while board 3 still heard board 2 well. Now every frame says how well
  its sender hears each board, and when the cabled board hears one under 70%,
  the boards that hear both (80%+) carry its records home in spare room of
  their own frames (details and simulator results: docs/REFERENCE.md). On by
  default; `tw_setid.py PORT relay off|on` is a network setting like the
  power. The frame header grew to 40 bytes (version 2): 1.x and 2.0 boards do
  not hear each other, so every board is reflashed.
- **tw_setid.py tries up to three times** and says "done" only when the board
  reports the new number or setting: the board's USB line sometimes dropped a
  command or its answer when busy. The firmware now waits briefly to send its
  answers instead of dropping them.
- **The standing scene can live in `private/scene-now.txt`** (used before
  `~/tw-data/scene-now.txt`), like the board layout.
- `summary.txt` counts the records that came home through a relay, per link,
  and lists each board's relay setting; the dashboard shows `heard N%` for a
  board the cabled one hears badly, and `relay OFF`.

- **Firmware tw_node 1.1: transmit power, set from the computer.**
  `tw_setid.py PORT power 11` (2-20 dBm, or `default` = 20) tells the board on
  the cable; it puts the setting into the two spare bytes of every frame's
  header (a change number and the power in quarter dBm), and every board that
  hears a newer change number takes it, applies it
  (`esp_wifi_set_max_tx_power`) and keeps it in flash. A board that was off
  takes it when it comes back; two settings with the same change number go to
  the lower-numbered board's. Boards on 1.0 send zeros there, which 1.1
  ignores, so mixed boards still work (the 1.0 ones stay at full power). The
  channel stays fixed at 11. Why: at full power boards a few metres
  apart hear each other strongly, much of it by reflection off metal; a
  lower power may make each link depend more on its own path
  (to be tested: one night at 20 dBm against one at 10-11 dBm).
  Simulator (`sim --setpower T@DBM[@BOARD]`): every board had it within
  0.014-0.034 s in each case; a board switched off during the change took it
  on return; two conflicting settings ended on the lower board's; delivery
  unchanged (99.7%).
  The dashboard shows each board's power; `summary.txt` lists it per board
  (and when it changed).

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
- **Fix: stale lines at the start of a run.** The board's USB buffer can still
  hold lines from the last time something read it (the previous run's last
  moment, hours old); they came out first and one 10 s window counted 1.4
  million frames, so `summary.txt` showed about 53% "delivery" loss on every
  link (2026-10-31; the views and across.txt were right). `listen.py` now
  throws away the first second after opening the port, and `split_links.py`
  caps any window that counts far more than a typical one.

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

