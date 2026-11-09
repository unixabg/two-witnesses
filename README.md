# two-witnesses

**Seeing movement with Wi-Fi.** A few cheap ESP32-S3 boards send radio frames
to each other; each board measures how every frame arrived (its channel state
information, CSI: the strength on each of the channel's subcarriers). A
person, a car, a door or a running motor changes those measurements. These
tools record them for hours, show them live in a terminal dashboard, and read
whole runs afterwards: events, rhythms, which pair of boards saw what, and a
map of the boards with the lines that moved.

- **Hardware**: 2-16 ESP32-S3 boards (the tested ones have the built-in
  antenna), USB power for each; one is cabled to a Linux computer.
- **Firmware**: `tw_node-s3-ch11.bin`, one image for every board. Flashing:
  **[FLASH-README.md](FLASH-README.md)**.
- **More**: [docs/REFERENCE.md](docs/REFERENCE.md) (every tool, how the
  analysis judges), [docs/CHANGES.md](docs/CHANGES.md) (what changed and why).
- **License**: Apache-2.0 ([LICENSE](LICENSE), [NOTICE](NOTICE));
  third-party parts in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- Written by Richard Nelson with Claude (Anthropic) as co-author.

> **Privacy.** The radio does not stop at walls. In testing, boards in a
> garage picked up people moving in the house next door and a truck in the
> driveway. Sense only places where you have the right to, and tell the
> people who live or work there.

## Install (once)

```
git clone https://github.com/unixabg/two-witnesses
cd two-witnesses
./setup_once.sh
```
It installs the packages (Debian/Ubuntu), sets the clock (chrony), makes the
Python environment `~/scratch-esp`, makes **`~/tw-kit` a link to this clone**
(so `git pull` updates the installed kit), and checks that `~/tw-data` (the
recordings) is on real storage. Log out and in once afterwards (serial ports
without sudo). Then flash the boards: [FLASH-README.md](FLASH-README.md).

## Your own files (never committed)

| file | what |
|---|---|
| `private/boards.csv` | where your boards are (feet from a reference point, which way each faces) and landmarks; drawn on the dashboard map, in each report and replay. Show it: `~/tw-kit/tw_layout.py`; change it value by value: `tw_layout.py edit` (or any text editor); earlier versions: `tw_layout.py backups` (every version is kept in `private/layouts/`). Each run keeps a copy of the one it started with. A second site: run the kit with `TW_DATA=~/site-b`; its layout is `~/site-b/boards.csv`. Format: `boards-example.csv` |
| `private/scene-now.txt` | what is true right now ("car 1 in, door shut"); every run records it. Update it whenever something changes (`~/tw-data/scene-now.txt` works too; `private/` is used first) |
| `private/` | anything else of yours: git ignores the whole folder |

## Everyday use

| to... | run |
|---|---|
| record a night or a day | `~/tw-kit/run.sh 07:00` (until that clock time) |
| record with the camera too | `~/tw-kit/watch.sh 07:00` (or `--nocam`) |
| look at the boards without recording | `~/tw-kit/live.sh` (`q` quits) |
| watch a run that is going | `~/tw-kit/dash.sh` (`g` map, `q` closes the view, the run goes on) |
| test where to put a board | `~/tw-kit/survey.sh` (it first shows the layout and asks if it is still right, then checks it against the radio in 2 quiet minutes; then walk a board around, `m` marks a spot and asks if a board was moved there and to which x y; `survey.sh stop` offers to write the moves into the layout) |
| stop a run early | `~/tw-kit/run.sh stop` (or `watch.sh stop`, `survey.sh stop`) |
| is everything right? two boards with one number, firmware, settings, weak links (30 s) | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 check` |
| the group of the board on the cable (tw_node 3.3; 0 = the default; it restarts) | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 group 2` (over its own cable, one board at a time) |
| the radio channel of every board: show it, or move them all (tw_node 3.2, every board; 1-11) | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 channel` or `... channel 6` (not during a run) |
| who else is on the air, per channel and board (tw_node 3.1; the network pauses about 12 s) | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 survey` (saved in `~/tw-data/surveys/`; not during a run) |
| set every board's transmit power | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 power 11` (`default` = 20 dBm; nothing running on the port) |
| relaying off / on (every board) | `~/scratch-esp/bin/python ~/tw-kit/tw_setid.py /dev/ttyACM0 relay off` (`relay on` is the default) |
| does the boards' own movement score follow the computer's? (tw_node 2.2) | `~/scratch-esp/bin/python ~/tw-kit/score_check.py ~/tw-data/runs/<run>` |
| how far behind the timekeeper do the boards' clocks run? (any run; 2.3 should be under 100 us) | `~/scratch-esp/bin/python ~/tw-kit/lag_check.py ~/tw-data/runs/<run>` |
| do the network settings agree both ways? (tw_node 2.6) | `~/scratch-esp/bin/python ~/tw-kit/net_check.py ~/tw-data/runs/<run>` |
| the second frame: tests (tw_node 2.1) | `tw_setid.py PORT second off\|on\|force`, `measure-second on\|off`, `cap N\|off`; after a test put back `second on`, `measure-second off`, `cap off`; read a test run with `~/scratch-esp/bin/python ~/tw-kit/second_check.py ~/tw-data/runs/<run>` (docs/SECOND-FRAME.md) |
| check the installed kit | `~/tw-kit/version.sh` |

- Runs live in tmux, so you can close the terminal or the SSH session; they
  block sleep while they record (use mains power). Only one recorder can hold
  the board at a time; the scripts tell you if one is already going.
- **Every night**, with cron (`crontab -e`, your home folder written out):
  `0 0 * * * /home/YOU/tw-kit/run.sh 07:00 >> /home/YOU/tw-data/cron.log 2>&1`
- The dashboard: a row per link (`2→1` = board 1 hearing board 2) with
  packets/s, loss (10 s and 2 s), signal strength, how far it is above the
  noise, and motion (x its usual); each board's "reports home" percentage;
  the map (`g`) draws each line by how much it stirs right now; `c` shows the
  chosen link's CSI itself, one row a second: its shape across the channel
  (press again: what changed in the last minute). With tw_node 2.1 a board's
  line also says `2nd N/s` while it sends second frames, and any test setting
  left on, loudly (`2nd FORCED`, `2nd OFF`, `MEASURING 2nd`, `CAP N`); the
  health line counts `2nd sent` and `rx_err` (measurements the radio flagged)
  since the dashboard opened.

## Boards and relaying

- **How many:** up to **16** boards. Up to 6 work best: each measurement
  keeps all 192 amplitudes and goes out twice. From 7 on, measurements shrink
  to 52 amplitudes (as esp-radar uses) and go out once, and each board sends
  a little less often (every 48 ms with 16). Each board needs its own number.
- **Relaying (on by default):** only the cabled board talks to the computer,
  so a board it cannot hear well (a car parked between them) would lose its
  reports. With relaying, the boards that hear both carry them home, in room
  left in their own frames (and, from tw_node 2.1, in a second frame in their
  own slot when the first is full). What that board
  measured still arrives, and the blocked line still shows as path loss on the
  map: relaying saves reports, it does not hide what is in the way. Turn it
  off only for a comparison run (`tw_setid.py ... relay off`, then `relay on`).
- **Placing:** the board with the clearest view of all the others is the best
  one to cable to the computer.
- **The second frame (tw_node 2.1, on by default):** when a board's frame is
  full (6 boards: its own 5 new measurements leave one place), it sends a
  second one right after, in its own slot, for the records still waiting
  (second copies, relayed ones). It carries reports only: nothing is measured
  from it, so every link is still measured once per period, as before. With 3
  boards everything fits in one frame, so it hardly ever goes. Turn it off
  for a comparison run: `tw_setid.py ... second off` (then `second on`).

## After a run

Everything is in `~/tw-data/runs/<date>_<start time>/` (or `watch/`,
`survey/`). The views are made by themselves when the run ends:

| file | read it for |
|---|---|
| `links/across.txt` | **start here**: all links side by side; moments seen on several links (with a small map each), things on one link only, rhythms that keep coming back |
| `links/<from>-<to>/view.txt` / `view.png` | one link in detail: events, low data, path loss rises, signal shifts, rhythms |
| `links/summary.txt` | records, loss and signal per link for the whole run |
| `detect/events.csv`, `detect/sheet-*.png` | watch runs only: each event with camera frames; fill in the `label` column |

Two kinds of loss are told apart: **path** (the measuring board did not hear
the frame: something was in the way, which is sensing) and **delivery** (it
heard it, but its report never reached the computer: plumbing).

At the end of every many-board run the checks are made too (second_check,
lag_check, net_check, score_check: each `<check>.txt` in the run's folder;
one that does not apply to the run's firmware says so in `checks.out`).
Remake the views and checks of a run (for example after updating the kit):
`~/tw-kit/after_run.sh ~/tw-data/runs/<run>`, or the views only, for a many-board run
`~/scratch-esp/bin/python ~/tw-kit/split_links.py ~/tw-data/runs/<run> --again --view`.

Replay a many-board run on the board layout (every link, second by second,
with a timeline, the events and the lasting changes), as one page to open in
a browser: `~/scratch-esp/bin/python ~/tw-kit/replay.py ~/tw-data/runs/<run>`
writes `replay.html` into the run folder. It asks for a title (or give
`--title "..."`); `--from 05:00 --to 07:00` keeps part of the run, and
`--note "07:15 door opened"` adds your own marks (on a watch run the camera's
events and frames show too: see `replay.py --help` for sharing it through a
small web server), and `--view lines` opens it with
one line per direction instead of the beacons (a switch on the page flips
between them). The page holds the board layout:
keep it as private as the run.

## Reading a run honestly

What makes the results believable is how they are read:
1. **One change per run** (a board moved, an adapter swapped): otherwise
   nothing can be told apart.
2. **Seal what you know before anyone reads the data**, then compare:
   `python3 ~/tw-kit/seal.py seal <name> "15:33 car in; 15:39 person at freezer"`
   prints a fingerprint to share; after the reading,
   `python3 ~/tw-kit/seal.py open <name>` shows the notes and proves they
   were not changed.
3. **Report times and sizes first, causes second.** Small events nobody saw
   are often real (through a wall, outside), not noise.
4. **Name the instrument's habits** (a steady rhythm that is a motor, a weak
   pair of boards, a bad USB adapter) before believing anything new.

## Updating

```
cd ~/two-witnesses && git pull && ~/tw-kit/version.sh
```
`main` holds releases (tagged `vX.Y`); `next` is where work happens
(`VERSION` says `X.Y-next` there). `version.sh` shows the version, checks every
file against `SHA256SUMS`, and shows the layout and scene in use. Don't
switch branches while a run is going. Contributors: after changing files,
`tools/sums.sh` refreshes `SHA256SUMS`.
