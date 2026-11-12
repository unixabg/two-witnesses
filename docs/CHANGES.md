# What changed, version by version

Newest first. Each entry keeps the reasoning and the desk tests behind the change;
the README has only what you need to run the kit.

## What changed in v5.6-next (work in progress, on the next branch)

- **README: what a board's LED shows.** A table of every LED state. It was
  scattered over the detection and steadier rows, and FLASH-README's LED row
  still said "movement only":
  - off: quiet;
  - blue, pale to deep: movement;
  - a short white blink: a detection, once when it begins and once each
    second while it lasts;
  - steady green: steadier than quiet, and what ends it.

  It also gives the order when several apply (white over blue over green)
  and the blank first minute after a start. FLASH-README's LED row now says
  the same in short.

- **tw_node 4.3: version and image.** The two fixes below, in
  `tw_node-s3-ch11.bin`: the busy-start check by its own level, always on,
  and a hold that lasts only as long as the quiet before it. The first
  128 KB are 4.0's, so 4.x boards take it over the air.

- **Firmware (4.3): a hold lasts only as long as the quiet before it.** In
  4.2, a busy period longer than about 3 minutes at a board's start (boards
  switched on in an occupied room) still blinded a link. The movement
  scored near 1x against the busy level long enough to arm the link, and
  when the room emptied the link was marked steadier and held. At the end
  of the hold, the quiet level then drifted down at 0.2% a second: a level
  10x too high took about 19 minutes. In the simulator, someone about for
  260 s left link 1-2 marked for 20+ minutes.
  - **The new rule:** a hold lasts at most as many seconds as the link
    scored near quiet since its quiet level started, and never more than
    30 minutes.
  - **At the end of a hold,** the link learns its quiet level again, from
    the stillness, instead of drifting.
  - **Simulator, on the cases the review asked for:**
    - Someone about for 260 s: the link was marked for about 200 s, then
      relearned, and was normal (1.0x) about 4.5 minutes after the room
      emptied.
    - 150 s of quiet, then still for good: held for 150 s, then a relearn,
      then 1.0x.
    - 34 minutes of quiet, then a 35-minute stop: held for 1,798 s, then a
      relearn.
    - Someone walking across the link through the relearn after a hold:
      the relearn learned the busy level, then the busy-start check
      relearned once more, and the link was back at 1.0x.
    - A shorter walk inside a relearn is ignored by the median start
      without any help.
    - 4.2's busy-start cure, with the low threshold on or off: no change.
    - Normal runs: 4.2's records and scores. The timing and update cases
      are unchanged.
  - **Trade-offs:**
    - Each hold that runs out now costs that link 60 s without scores. The
      drift it replaces kept scoring, only less sensitively. For a wrong
      level that is a clear gain. After a real stillness it means one
      blind minute each time.
    - Right after that relearn the stillness is the quiet level, so a fan
      starting again reads as movement at its full ratio until the level
      climbs. In the simulator that was about 55 s of detection (the
      review expected 40-50 s). 4.2 had the same, only later.
    - "Became its quiet" (TWS low_accepted) now counts every hold that ran
      its course, short ones included.
  - **The remaining limit:** a room that is busy for an hour or more when a
    board starts still means up to 30 minutes of a link marked steadier
    after it empties. On one link, an hour of a person and an hour of a fan
    give the same signal. The replay, which takes its quiet level from the
    whole night, does not have this limit.
  - The simulator gains `--walk A-B@X-Y` (a walk across link X-Y).
  - From a second review.

- **Firmware (4.3): the busy-start check no longer depends on the low
  threshold.** In 4.2 the check ran inside the steadier-than-quiet logic
  and used its threshold (key 8). Two problems came from that, both found
  in a second review:
  - With the low threshold off, a busy start was never caught. In the
    simulator a link was still at 0.5x 13 minutes after one.
  - A high threshold (0.8, 0.9) could take an ordinary quiet link that
    hasn't armed yet for a busy start, again and again.

  Now the check has its own fixed level, 0.5x for 10 s before the link is
  armed, and it is always on. As a consequence, with key 8 below 0.5x, an
  unarmed link at 0.4x relearns although the user's threshold would not
  have marked it. That is intended: it is about whether the start was
  wrong, not about what the user calls still.

  In the simulator, a 90 s busy start is cured with the low threshold on or
  off (1 relearn, then 1.0x). Thresholds of 0.7, 0.8 and 0.9 on quiet
  boards gave no relearns over four seeds each.

- **tw_node 4.2: version and image.** The four fixes below, in
  `tw_node-s3-ch11.bin`: a busy start no longer blinds a link, a board gone
  quiet ends its link's mark, a second without a score no longer breaks a
  detection, and a relearn starts from a clean second. The first 128 KB are
  4.0's, so 4.x boards take it over the air (`tw_setid.py PORT update board
  all`).

- **`tw_setid.py PORT` names busy starts.** The cabled board's status line
  adds "N links learned quiet again after a busy start" when there were any
  (TWS field 44, tw_node 4.2).

- **Firmware (4.2): a busy start no longer blinds a link.** This one was
  serious, and it was in 3.5's design.
  - **The problem:** someone moving through a link during a board's first
    minute leaves that link's quiet level too high, so afterwards quiet
    scores low. 3.5 took that for "steadier than quiet" at once, then held
    the quiet level where it was (the hold meant to keep a real still
    stretch visible). The wrong level stayed, and the link was blind to
    movement for many minutes. In the simulator, a walk through the start
    left link 1-2 marked for about 15 minutes, quiet scoring 0.1x, and a
    later walk was missed.
  - **The fix:** a link is "armed" for steadier than quiet only once it has
    scored near quiet (0.8-1.2x) for 120 seconds, not in a row, since its
    quiet level started. A link that drops to the low threshold before
    that had a busy start: that link alone learns its quiet level again,
    counted in TWS (field 44), and is not marked.
  - **Tested in the simulator:** after the busy start the link learns its
    quiet level again about 10 s after the walk ends and scores 1.0x. The
    3.5 still cases are marked as before, and the 35-minute stillness is
    held and accepted.
  - **The trade-off:** something usually moving that stops within about 2
    minutes of a board starting gives a relearn, not a mark.
  - Found in a review by another model. Its own fix (armed after 10 s near
    quiet) did not cure it: a walk going on past the start scores near 1x
    against the busy level and armed the link at once.

- **Firmware (4.2): a board that goes quiet ends its link's "steadier" mark.**
  A second without a score left a steadier-than-quiet stretch as it was, so
  when the other board was switched off or went out of reach, the mark (and
  the green LED) stayed for as long as it was gone. Now 5 seconds in a row
  without a score end it. Found in a review by another model. In the
  simulator, with link 1-3 still and board 3 off for 300 s, 4.1 kept the
  mark through those 300 s; now it ends 5 s after board 3 goes, and the
  stretch begins again when board 3 is back (the link is still still).

- **Firmware (4.2): a second without a score no longer breaks a detection.** A
  second with fewer than 5 records of a link has no score (frames lost:
  someone in the way costs some). That reset the detection, so one walk could
  count as several. Now such a second neither counts nor breaks it; two in a
  row do. Found in a review by another model. In the simulator, a walk with
  85% of one direction's frames blocked counted 3 detections on 4.1 and
  counts 1 now. With fewer frames blocked nothing changes.

- **Firmware (4.2): a relearn starts from a clean second.** A relearn (on
  command, or when a board changes channel) started the quiet level again,
  but kept the sums of the second it fell in. After a channel change those
  mixed two channels and became the new quiet level's first value. Now the
  relearn clears them too. Found in a review by another model; in the
  simulator the channel-change case now differs from 4.1 by that second, and
  every other case is the same.

- **tw_node 4.1: version and image.** The fixes since 4.0, in
  `tw_node-s3-ch11.bin`: the board on the cable keeps an upload until it is
  checked, and the program description names its version. The first 128 KB
  are the same as 4.0's, so 4.0 boards take it over the air
  (`tw_setid.py PORT update board all`). A 3.x board needs the one cable
  flash, as for 4.0.

- **The program's description names its version.** The board's "TWUP
  verified" line read "tw_node 1": ESP-IDF's program description takes its
  version from `firmware/tw_node/version.txt` (or git), and there was none,
  so it said 1. Now `version.txt` holds the firmware's version, kept the same
  as FW_VERSION in main.c, and the line reads "tw_node 4.1".

- **`tw_setid.py PORT update`: the manifest and the check go together.** So
  the board takes both in one go, before 4.0's once-a-second tick can drop
  the update in between (see below). If it is lost anyway, the upload runs
  once more. Boards on 4.0 as flashed take updates this way, with no reflash.

- **Firmware (tw_node, next version): the board on the cable no longer drops
  an update before checking it.** On 4.0, `TWUP check` answered "not all
  there (0 of 655 chunks, no manifest)" right after a complete upload. When
  the computer gave the cabled board a program, the board started the
  update's idle clock (2 minutes without data drops an unfinished update)
  from a stale time, not from that moment. So once the manifest arrived, the
  next once-a-second tick took the update as long idle and dropped it before
  the check came. The clock now starts when the upload begins and again at
  the manifest. Receiving boards were not affected; they start the clock
  from the first frame. The simulator now runs a tick between the manifest
  and the check, which fails on 4.0's code and passes now, and the update
  cases give the same results. It goes out with the next firmware version,
  over the air.

- **`tw_setid.py PORT update`: two chunks at a time over USB, not eight.**
  On the first real board the upload stopped at chunk 2 ("no answer after 5
  tries"). The board's USB driver keeps at most 4 KB it has not read yet, and
  drops what comes beyond it without a word (esp_driver_usb_serial_jtag: the
  packet is lost when its ring buffer is full). Eight lines of about 1.72 KB
  each overflowed it. The stand-in board used in the desk test read as fast
  as they came, so it never showed this. Now two lines are in flight (3.4
  KB), and one after any retry. If a chunk is still not taken after 5 tries,
  the error gives the board's last answer. No firmware change: boards on 4.0
  as flashed take it.

- **tw_node 4.0: version and image.** Updates over the air (below) in
  `tw_node-s3-ch11.bin`. This time the first 128 KB change: a bootloader with
  rollback, the two-slot partition table, and an empty program-slot record at
  0x11000. So each board needs one flash of all but its settings
  (`flash_board.sh`, p), not the program part alone. The settings area is
  left as it was, and the boards keep their numbers and settings.

- **`flash_board.sh`: "all but its settings", and the update key.** The
  flashing choice that kept a board's number and settings wrote only the
  program (from 0x20000). Now it writes everything except the settings area
  (0xb000-0x11000): the bootloader and partition table, the program-slot
  record and the program. That is the one cable flash that moves a 3.x board
  to tw_node 4.0's two program slots, and later it puts a board back on
  slot 0 with the program just written. A new step 7 gives the board your
  update key (making it the first time) when it runs 4.0. Checked with a
  stand-in esptool: three pieces, at 0x0, 0x11000 and 0x20000, the settings
  left alone.

- **Computer side: `tw_setid.py PORT update-key [show|none]` and `update
  [board N|all] [FILE]`** (tw_node 4.0; the work is in the new
  `tw_update.py`).
  - **`update-key`** makes your key pair once (P-256,
    `private/update-key.pem`, readable by you only) and gives its public half
    to the board on the cable. `show` compares fingerprints; `none` removes
    the key from the board.
  - **`update`** takes the kit's image, or a program or image FILE, and
    finds its version and build in it. It signs the program and gives it to
    the board on the cable, several chunks in flight at once (about a second
    on a stand-in; 10-30 s expected on a board). The board checks it and
    sends it to board N, every board of its group, or (without `board`)
    takes it itself.
  - **Following it:** it shows each board's part every 5 s, and ends when
    each board runs the new program or has said why not: no key, another
    key's signature, damaged, no PSRAM, older firmware (named before
    anything is sent), or not heard.
  - Checked against a stand-in 4.0 board in every case above. Its signatures
    were cross-checked with ESP-IDF's mbedtls, called as the board calls it.

- **Firmware (tw_node 4.0): updates over the air.** docs/TW_NODE-4.0.md has
  the whole design and the steps for the first time on the boards; the frames
  are in docs/PROTOCOL-3.md.
  - **Two program slots and rollback.** The flash gets two 1.5 MB program
    slots and rollback in the bootloader. The settings area stays where it
    was.
  - **The update key.** Each board learns an update key's public half over
    its cable (`TWSET upkey`). A board with none takes no update.
  - **Sending.** The board on the cable receives a signed program over USB
    (`TWUP ...`) into its PSRAM, checks it, and sends it to its group in short
    bursts of its own `TU` frames. The group keeps quiet for 60 ms after each
    one. Each board reports which chunks it has, and the next bursts fill the
    gaps.
  - **Taking it.** Each board checks the SHA-256, your signature and that it
    is a tw_node for the ESP32-S3. It writes the program to its other slot
    4 KB at a time, says READY and restarts into it. The board on the cable
    goes last.
  - **Proving it.** A new program is kept once it has run a minute and hears
    its group (or a computer reads it). Otherwise the bootloader returns to
    the old one within 10 minutes, or at once after a crash.
  - **New lines.** TWP for each board's part; TWS, TWH and the replies gain
    their update fields.
  - **The simulator** gains `--update T@TARGET:SIZE`. An 820 KB program
    reached 3 boards in 6-8 s, at 20% loss in 10-13 s, and 6 boards in 7-9 s.
    1.5 MB reached 8 boards in 13-15 s. One board only, the cabled board
    alone, a board off mid-update (the sender stops waiting), a board
    restarting mid-update (it gets every chunk again) and a second group
    (untouched) all worked, measuring went on throughout (PASS), and with no
    update the records are byte-for-byte 3.5's.
  - **Not yet tried on a board:** the flash writes, the signature check and
    the rollback.

- **`tw_setid.py PORT survey` no longer calls 3.4 and 3.5 boards older
  firmware.** It recognised 3.1 to 3.3 (and 4.x) by name, so a 3.4 or 3.5
  board was named as "older firmware" that "does not survey", though it does.
  It now compares version numbers, as the other commands do.

- **tw_node 3.5: version and image.** Steadier than quiet (below) in
  `tw_node-s3-ch11.bin`. Program-only upgrade (its first 128 KB are 3.4's);
  every board watches at 0.5x until given another (`detect low off` turns it
  off).

- **The replay: steadier than quiet, the boards' and its own.** On runs
  with tw_node 3.5 boards, the seconds a board marked a link steadier than
  quiet are a green line along the bottom of that direction's row. The page
  also finds its own: a direction under `--below X` times its quiet level
  (0.5 unless given; 0 turns it off) for 10 s or more. These are drawn as a
  green outline on the row and listed with the lasting changes. The page's
  quiet level is the median of the whole scene, so unlike a board's it does
  not follow the stillness: a still stretch shows for as long as it lasts.
  "This second" names both, and a link whose two directions are both under
  it reads "steadier than quiet" instead of "quiet". About lists each board's
  low threshold, the boards' marks per direction, and the page's stretches;
  the printed summary says them too. The boards' detections are now **red**
  along the top of the row, so green means steadier on the page and on the
  LED. Checked on a simulated half hour with a walk and 200 s of stillness on
  link 1-3: the walk is one event, tagged "boards"; the stillness is marked
  by both on 1→3 and 3→1 only (0.34x). Checked at 1280 px light and 390 px
  dark: no script errors and no sideways scroll. On an older run it says "not in this
  recording".

- **Computer side: `tw_setid.py PORT detect low X|off [board N|all]`**
  (tw_node 3.5). X is 0.1 to 0.9 times the quiet level, given and confirmed
  as `detect`. The status shows each board's low threshold ("low 0.5x") and,
  for the board on the cable, how many steadier stretches began and how many
  became its quiet. A board on older firmware is now named only when the
  command was for it (`detect 4 board 2` no longer mentions an old board 3).
  Checked against a stand-in board with one older board among them, including
  bad values.

- **Firmware (tw_node 3.5): steadier than quiet.** A link only gets stiller
  than its quiet level when something that is part of that quiet stops: a
  fan or the furnace, a compressor, a radio nearby, someone who was sitting
  still and leaves. A link at or below its board's low threshold for 10 s in
  a row is now marked. The threshold is key 8 (a board key, x10 of the quiet
  level, 1-9, 0 = off; 0.5x until given; kept in the board's flash). The mark
  goes in the TWM record (a second set of 16 bits), and the cabled board's
  TWS counts it (threshold, stretches begun, stretches that became the quiet:
  fields 36-38). The LED shows green while it lasts; movement (blue) and the
  white flash come first. A second without a score, from lost frames, neither
  counts nor breaks a stretch, so losing frames cannot look like stillness. While a link is marked, its quiet level is
  held: it does not step down, which would otherwise take the stillness in
  as quiet within minutes and end the mark. After 30 minutes the stillness
  becomes the new quiet: that is counted, the mark ends, and the quiet level
  follows again. `relearn` clears it. The simulator has `--still A-B`: a
  "fan" on link 1-3 that stops between A and B s. Results:
  - 120 s still: marked 310-419 s on 1→3 and 3→1 only (0.3x). When the fan
    started again the link read 1.0x, not a false movement detection.
  - 35 minutes still: marked for 1,800 s, then accepted (counted once).
  - 40% frame loss with nothing still: no marks.
  - Low threshold turned off by record: no marks.
  - The standard cases give the same verdicts as 3.4. A few timing numbers
    move a little, because each score record is 2 bytes longer on the air.

- **tw_node 3.4: version and image.** Detections, each board's threshold
  and relearn (below) in `tw_node-s3-ch11.bin`. Program-only upgrade (its
  first 128 KB are 3.3's); every board starts at 3x until given another.

- **The replay shows the boards' own detections, and takes `--detect X`.**
  On a run with tw_node 3.4 boards, each second a board marked a detection
  on a link is a green line over that direction's row of the timeline; the
  events they fall in are tagged "boards"; "This second" says "the boards
  detected: 1→2, 2→1"; "About this recording" lists each board's threshold
  (as the boards reported it) and the detections per direction, and the
  summary printed at the end counts them and how many events have one. They
  are the boards' measure, not the page's: the two mostly agree, not to the
  second. `--detect X` sets one fixed event threshold for every direction (X
  times its quiet level) instead of each direction's own from its quietest
  hour, to compare nights by one rule or to see what a board's threshold
  would have caught. Older runs say "not in this recording". Checked on a
  simulated 25 minutes with a walk (the walk's event tagged, both
  directions marked, no others; no script errors) and on an older watch run.

- **Computer side: `tw_setid.py PORT detect X [board N|all]` and
  `relearn [board N|all]`** (tw_node 3.4). `detect` sets the threshold (1.5
  to 10 times the quiet level, in steps of 0.1) and `relearn` has the board(s)
  learn their quiet level again; without `board ...` it is the board on the
  cable, over its cable. With `board N|all` they go as the LED settings do
  (given by the cabled board, passed on for 60 s, confirmed when each named
  board reports the new generation), and a board on older firmware is named
  and left out. Wrong values are answered before the port is opened.
  `tw_setid.py PORT` shows each board's threshold ("detect 3.0x quiet") and,
  for the board on the cable, its detections since it started. Desk-tested
  against a stand-in board (one board, every board, a 3.3 board among them,
  bad values).

- **Firmware (tw_node 3.4): each board detects, and holds its own
  threshold.** Until now each board scored every link it measures once a
  second (TWM) and nothing decided anything from it. Now a link at or above
  its board's threshold for 2 s in a row is a *detection*: marked in that
  second's TWM record (a bit per link, after the second frame's end), counted
  in the cabled board's TWS (its threshold x10 and the detections since it
  started, fields 34 and 35), and shown by the LED as a short white flash on
  top of its blue (it then goes back to the blue). Two new keys (docs/PROTOCOL-3.md):
  6 *detect*, a board key (x10 of the quiet level, 15-100; 30 until given;
  kept in the board's flash, so each board holds its own), and 7 *relearn*,
  an action: the board learns every link's quiet level again from the next
  60 s. A board relearns by itself when it moves to another channel (by
  command, or finding its group by scanning): the old quiet level belongs to
  the old channel.
  The quiet level itself now starts from the first 60 s, as a stepped median
  (each second moves it a shrinking step toward that second, up or down), not
  the mean of the first 30 s. The mean took everything in: someone walking
  past in the first half minute left the floor high for hours (it then
  corrects by only 2x every ~6 minutes), so later movement scored low. Desk
  tests (Python, then the simulator): 10 busy seconds of 60 at 4x leave the
  floor within 7% (the mean: 54% high); 20 busy seconds at 10x, 1.27x (the
  mean: 4.11x); in the simulator, a walk during the start left 3.3's floor
  3.3x high (quiet scored 0.3x, the walk only 2.3x), 3.4's 1.4x (0.7x).
  Simulator: a walk across link 1-2 at 100-110 s is detected 101-110 s on
  1→2 and 2→1 only, the other links never; threshold 9.9x trims it; relearn
  at 90 s pauses the scores for 61 s and the walk inside is not scored. The
  standard and timing cases give the same verdicts as 3.3 (a few timing
  numbers move a little: each score record is 2 bytes longer on the air).

- **`replay.py --serve` ends cleanly.** After Ctrl-C (or `--hours`) it
  printed "stopped serving" and then a Python error (`NameError: total`): the
  run's summary (records, thresholds, events, lasting changes) had landed
  inside the serving code when `--serve` was added. The summary is back where
  it was, printed before serving starts.

- **The replay's camera no longer flashes while playing.** The camera keeps
  about one frame a second, with gaps; a second without its own frame (or one
  before it) hid the picture and said "no frame kept", so playback flickered.
  Now a second shows the latest frame of the last 6 s (saying "frame from
  HH:MM:SS" when it is not its own), and "no frame kept" only where there is
  none; the next frames load ahead while playing. Checked with a third of an
  event's frames missing: the picture stays up through the event.

- **`replay.py RUN --serve-only`** serves the page already made (after a
  Ctrl-C, say) without reading the run again: seconds instead of minutes, a
  new link each time; with no page yet it says how to make one.

- **The replay page, rearranged for phones and big screens.** The title,
  controls and timeline stay at the top while the page scrolls. Below them the
  map and the camera sit side by side (on a phone too; tap the frame to make
  it big, and again to put it back), with a new **This second** box: the event
  under way (its number, start, length and what it was), the watch's event,
  lasting changes and notes within half a minute, and for each link what its
  two directions did, as an observation ("changed both ways", "changed one way
  only: 2→1", "quiet", "no data"), with the numbers; each direction's change,
  loss and shape fold out beneath. The events and the lasting changes follow,
  side by side on a wide screen; the map's legend folds out ("How to read the
  map"). On a phone the event buttons are arrows and the clock shares their
  row (at 360 px Play is its icon); on a big screen the map and the frame keep
  to the window's height. A missing frame no longer shows a broken image.
  Checked at 360, 390, 820 and 1280 px, with and without a camera: no script
  errors, no sideways scroll.

- **`replay.py --serve` shows each request**, one line as it comes: when,
  from where (through a Chromebook's port forwarding, ChromeOS's inner
  address, not the device's), what (the link's random part as `<link>`) and
  the answer. A browser that first tries an encrypted connection (Chrome's
  "Always use secure connections") is named as such, with what to do, instead
  of a page of Python errors; a dropped connection is one line. (It was
  silent, which left a phone that could not open the page unexplained.)

- **`replay.py --serve`**: after writing the page, it serves it to a phone
  or another computer at home, with a watch run's camera frames, and nothing
  else of the run folder (not the recordings, not the layout file), under an
  address with a random part (only someone given the link opens it), for 2
  hours (`--hours`) or until Ctrl-C, on port 8000 (`--port`). It prints the
  link and how to let a Chromebook forward the port. Checked: the page and a
  frame are served; the recording, the layout file, the page without the
  random part and two ways of climbing out of the folder are not found. At
  phone width the page needs nothing more: one column, no sideways scroll.
  The old advice (`python3 -m http.server` in the folder) served the whole
  folder to anyone at home; it is gone from the help.

## What changed in v5.5 (2026-11-10): protocol 3, the clock, the channel and groups (one reflash for every board)

- **`flash_board.sh`: a board set up step by step.** With the board on the
  cable it checks the kit (the image against SHA256SUMS, esptool, the port),
  listens for a tw_node board, and flashes: a new board the whole image, a
  tw_node board the program part (or the whole image, or nothing), with the
  download-mode steps on screen and a retry. Then its group (0 the default),
  its board number (with the reminder to write it down and label the board:
  numbers must not repeat in a group), its group's settings (shown once taken;
  for the first board of a new group, a survey and the group's channel), its
  LED with a test blink, and the health check. It asks before every change.
  Tried with stand-in boards and esptool: a new board joining boards 1 and 2,
  and a tw_node board moved to a new group on channel 6.
- **`tw_setid.py PORT check`**: 30 s of listening, then problems and notes.
  Two boards with one number (each board says a start tag every 5 s, kept
  until it restarts: two tags taking turns, each heard again, are two boards;
  a restart changes it once and is only noted), a board without a number
  (16), firmware or settings or channels that differ, a board looking for its
  group, a board heard under 90% of the time, another group nearby. It ends
  "all healthy" or the count of problems (and exits 1). With no third board
  to relay, two boards sharing the cabled board's own number may not show:
  the radio may drop frames from its own address.

- **`tw_setid.py PORT board N`** sets the board number, in the same form as
  the other settings (`power 11`, `channel 6`, `group 2`). The older
  `tw_setid.py PORT N`, an unknown word, or `board` without a number shows
  the help (which has the new form) before the port is opened; `board 17`
  says the number must be 1..16. FLASH-README uses the new form.

- **tw_node 3.3: version and image.** Groups (below) in `tw_node-s3-ch11.bin`.
  Program-only upgrade (its first 128 KB are 3.2's); every board stays in
  group 0 until given another over its cable.

- **Computer side: `tw_setid.py PORT group N`** (tw_node 3.3), over the
  board's own cable: it restarts in group N and says so (TWH). The status
  shows the cabled board's group and the frames of other groups it has heard;
  the run summary shows each hello's group.

- **Firmware (tw_node 3.3): groups.** Header byte 19 (reserved since 3.0)
  says the sender's group; a board takes only its own group's frames, and
  counts the others' (TWS: its group, other groups' frames, the last such
  group). The group is in the board's flash ("grp"), set over its cable
  (`TWSET group N`, 0-255; the board restarts), default 0 (TW_GROUP_DEFAULT:
  the group of every board before 3.3, which send 0 there). It is also in the
  board's radio address (byte 4; group 0: as before), so the radio measures
  only its own group's boards, and the survey counts another group as other
  traffic. A board moved to another group forgets its network settings'
  generations, so it takes its new group's settings; a board that finds its
  group by looking, with no channel ever given, takes the channel it found
  (without it, a moved board would go back to its old channel every 20 s; the
  simulator found that). Each group has its own board numbers. Two groups on
  one channel share its air: give each its own channel. Simulator: two groups
  of three on one channel stay apart (no records across, a timekeeper each,
  each counting the other's frames); on two channels they do not hear each
  other; a group-2 board back as a new board on the other group's channel
  finds its group; with one group, the records are the same as 3.2's.

- **tw_node 3.2: version and image.** The channel (below) in
  `tw_node-s3-ch11.bin` (the name stays: 11 is the channel of a board with
  nothing in its flash). Program-only upgrade (its first 128 KB are 3.1's);
  every board on 3.2 before a channel change.

- **Computer side: `tw_setid.py PORT channel [N]`** (tw_node 3.2). Without N
  it shows every board's channel; with N (1-11, from one table of channels
  by band) it refuses unless every board says it runs 3.2, gives it, waits
  for the move, and names any board not heard on the new channel. The status
  shows each board's channel, and "moving to" or "LOOKING FOR THE OTHERS"
  when so. The run summary (`links/summary.txt`) shows each board's channel
  with its power, and when it changed.

- **Firmware (tw_node 3.2): the channel.** A new network key (21): the radio
  channel, 1-11 (2.4 GHz, US; 40 MHz with its partner 4 above for 1-4, 4
  below for 5-11, as 11's 7 so far), kept in flash ("chan", "ngh") and said
  every 5 s like the others. A board that takes a new one moves 20 s later,
  by which time every board has it, so they move together. A board that hears
  no other for 30 s looks for them: 0.3 s on each other channel, then 10 s
  back home, and again; where it hears one it stays and takes the network's
  settings (a board off during a change, a new board on the default 11). The
  channel survey comes back to the board's channel. TWS ends with the channel
  it works on, the network's, 1 while looking, switches and scans; TWN with
  each board's channel key. 3.1 and 3.2 boards work together, but only 3.2
  boards follow a change. Simulator: 3 and 6 boards move together with a dip
  of half a second; boards that missed the change (off and back as new
  boards, deaf for its time, out of the cabled board's reach, a new board 60 s
  later, the far end of the scan) are back within about a second of hearing
  again; two changes 5 s apart end on the second; channel 13 is refused; and
  with no change, 3.2 is 3.1 (the timing cases differ by 4 us in the second
  frame's end, from the longer settings record). It caught a fault before the
  boards did: a frame noted a moment after "now" looked 71 minutes old, and
  every board went looking (fixed: a signed comparison).

- **tw_node 3.1: version and image.** The channel survey (below) in
  `tw_node-s3-ch11.bin`. Program-only upgrade (its first 128 KB are 3.0's),
  numbers and settings kept; 3.0 and 3.1 boards work together, so one at a
  time is fine.

- **Computer side: `tw_setid.py PORT survey`** (tw_node 3.1). Gives the
  survey to every board, waits up to 40 s for each board's result, and prints
  a table: per channel and board, busy % and foreign frames, the loudest
  foreign signal and the noise floor; then the seven 40 MHz pairs in 1-11
  ranked by their worst board (a pair's busy: the busiest channel overlapping
  its 40 MHz, by how much of it overlaps; a tie keeps the pair in use). Saved
  in `$TW_DATA/surveys/`. Boards on older firmware are named and skipped.

- **Firmware (tw_node 3.1): the channel survey.** A new action (key 5, for
  all boards): 5 s after taking it, every board at once leaves its channel and
  listens 1 s on each of channels 1-11 (20 MHz, every frame the radio
  decodes), then comes back: about 12 s. It counts the other traffic only (our
  boards are silent then, and any that is not is left out by its address):
  frames, their loudest and mean RSSI, the noise floor, and busy, the share of
  the time their frames took (from each frame's length and rate). The result
  is a new record (kind 6, printed as TWU), sent every 5 s for a minute.
  Bluetooth or a microwave oven are not decoded: only the noise floor shows
  them. Protocol 3 as before: 3.0 and 3.1 boards work together (a 3.0 board
  just does not survey). Simulator: 3 and 6 boards, the network paused
  11.6 s and back at full rate within a second; every board's result arrives
  within a second of its return, with 15% loss, a board the cable board
  cannot hear, and two surveys in a row; a board that was off when it was
  given and back within the minute surveys on its own later (harmless).

- **`after_run.sh` makes the checks too.** After a many-board run's views
  it runs `second_check`, `lag_check`, `net_check` and `score_check`, each
  into its own `.txt` in the run's folder (their output in `checks.out`); one
  that does not apply to the run says "nothing for this run" and the rest go
  on, and the checks run even if the views failed. `run.sh` and `watch.sh`
  call it at the end, so a night's checks are ready in the morning.

- **Scripts: the shared settings in one place.** `TW_DATA`, `PORT`, `VENV`
  (and the Python it gives), `DASH_TAP` and the sleep settings are set in
  `tw_lib.sh` instead of in each of `run.sh`, `watch.sh`, `survey.sh` and
  `live.sh`; each still takes them from the environment. Checked: every
  script ends with the same values as before, by default and when given.

- **tw_node 3.0: version and image.** Protocol 3 (below) in
  `tw_node-s3-ch11.bin`. Program-only upgrade (its first 128 KB are 2.7's),
  numbers and settings kept, but **every board together**: 3.0 and 2.x boards
  ignore each other. Back to 2.7 the same way.

- **Computer side (tw_node 3.0):** `tw_setid.py PORT` listens 11 s for every
  board (two report periods, and time for a relayed one: 6 s missed some),
  reads 3.0's standing entries, says "transport agrees" only for 2.6-2.7
  boards (3.0 has one way), and never chooses generation 0. `net_check.py
  RUN` on a 3.0 run compares each board's settings with the cabled board's.

- **Firmware (tw_node 3.0): protocol 3, the lean header** (docs/PROTOCOL-3.md,
  step 3). Protocol version 3: 2.x and 3.0 boards ignore each other's frames,
  so every board is flashed together. The header drops the uptime, the network
  settings and the second frame's end (bytes 18-23); the sender's delay (2.3's,
  in its own place of the hearing list until now) gets byte 18, and 19-23 are
  reserved (sent 0, ignored); flag 0x20 is free. The network settings travel
  only on the transport: a newer network key taken sets the power and flags
  the board uses (applied and kept in flash as before); a setting given at the
  cabled board is its keys' next generations. The cabled board fills TWN's
  uptime, settings and second-frame end from each board's records (its TWM,
  its standing keys, now flagged 0x02) instead of its header: 0 until said.
  Generation 0 is "never given" (a new board with an empty flash can never
  undo the network's settings); the setting number is the sum of the network
  keys' generations; two givers at once: the larger value wins. 2.5's settings
  record (kind 4), TWF `net_diff` and TWV's LED fields are gone. Simulator
  (same seed, 2.7 against 3.0): delivery and timing the same in every case
  (`timing_cases.py`: identical but the ideal radio's forced cases, by 0.1%);
  a power change reaches 3 boards in 0.47 s (2.7's header: 0.014 s), two
  changes 0.5 s apart both arrive, two boards not heard by the giver 0.62 s,
  a board off for an hour takes the changes made meanwhile 0.43 s after it is
  back; two exits giving different powers at once end the same on every
  board; LED keys and actions as in 2.7.

- **tw_node 2.7: version and image.** Reports both ways (above) in
  `tw_node-s3-ch11.bin`; includes 2.5.1 and 2.6. Program-only upgrade.

- **Computer side (tw_node 2.7): `net_check.py` compares the reports both
  ways too** (uptime and the second frame's end, by record against header),
  and `second_check.py` takes each board's own end from its record when it has
  one.

- **Firmware (tw_node 2.7): reports both ways (protocol 3, step 2).** Each
  board's once-a-second record (TWM) adds where its last second frame ended
  (a 17th value, us), as header byte 23 still says; its uptime has been in
  TWM since 2.2. A board with no scores but a recent second frame sends TWM
  for it. In 3.0 the header's copies go.

- **tw_node 2.6: version and image.** Network settings both ways (above) in
  `tw_node-s3-ch11.bin`; includes 2.5.1. Program-only upgrade, numbers and
  settings kept; the network keys start from each board's header settings.

- **Computer side: `net_check.py RUN`** (tw_node 2.6): per board, its network
  settings on the transport against its frames' header, at each of its
  announcements, and the cabled board's own count of seconds they disagreed.
  `tw_setid.py PORT` says per board whether the transport agrees. `listen.py`
  and the run tools already keep and count TWX lines.

- **Firmware (tw_node 2.6): network settings both ways (protocol 3, step 1).**
  Power, relay, second frame, measure-second and cap are also network keys
  (16-20) on the transport: `tw_set_power`/`tw_set_flags` give the same change
  there, and every board says them every 5 s (standing; newest generation
  wins; within a second after it takes a newer one). The header still spreads
  and decides them; each board compares the two each second and TWF counts
  the seconds they disagree (`net_diff`, `net_diff_now`). The keys'
  generations are kept in flash. Simulator (`--no-hdr-set` turns the header's
  spreading off): the transport alone reached 6 boards in 0.6 s, two boards
  that cannot hear the giver in 0.6 s, and a board off for 290 s caught up
  1.4-4.4 s after coming back; both ways together: 0 s of disagreement;
  reports home as in 2.5.1.

- **tw_node 2.5.1: version and image.** The transport (above) in
  `tw_node-s3-ch11.bin`. Program-only upgrade, numbers and settings kept.
  Flash every board: 2.5 boards pass the new records on but do not take them.

- **Computer side: `tw_setid.py` reads the boards' reports (TWX, tw_node
  2.5.1)** to show every board's LED and to confirm a change; `led test` and
  `led try R G B` take `board N|all` too. Boards on 2.5 still answer through
  TWV.

- **Firmware (tw_node 2.5.1): protocol 3's transport, step 0**
  (docs/PROTOCOL-3.md). A new record kind (`TW_KIND_CMD`, 5; printed as TWX)
  with variable-length entries: target, key, flags, generation, seconds left,
  value. Key classes: board settings (LED pin and brightness) and actions,
  run once per new generation and not kept (LED test; LED try R G B); a board
  skips keys it does not know by their length. Every 5 s each board sends
  report entries (its keys' values and generations) for the computer to
  confirm a change. 2.5's records are still read. Simulator: a board that
  cannot hear the giver took a setting in 0.6 s and test/try to all within
  0.6 s; a stale copy was ignored; reports home as in 2.5.

- **Design: protocol 3 (docs/PROTOCOL-3.md), no firmware.** One transport for
  settings and commands, built on 2.5's settings record: variable-length
  entries, three classes of key (board, standing network, action), reports for
  confirmation; the header keeps the per-frame fields, the network settings,
  uptime and the second-frame diagnostic move out, the clock delay gets its
  own byte, five bytes stay reserved. Four steps, each checked before the
  next.

- **tw_node 2.5: version and image.** Settings by record (above) in
  `tw_node-s3-ch11.bin`; otherwise as 2.4.3. Program-only upgrade, numbers and
  settings kept. A board on older firmware passes the records on but does not
  take them: flash every board.

- **Computer side: `tw_setid.py PORT led on|off|bright ... board N|all`**
  (tw_node 2.5): reads every board's LED setting and generation from their
  reports, gives the next generation, and waits until the board(s) report it
  (three tries); names any board that did not. `tw_setid.py PORT` shows every
  board's LED. `listen.py` keeps the TWX lines; `split_links.py` and `dash.py`
  count them as delivered records (like TWV and TWM), so they do not show as
  losses.

- **Firmware (tw_node 2.5): settings by record, for one board or all.** A new
  record kind (`TW_KIND_SET`, printed as TWX) carries entries "for board N (or
  all): key = value, generation G". The board on the computer gives them
  (`TWSET led on|off|bright ... board N gen G`); every board passes on what it
  has heard once a second for 60 s, so a setting reaches boards that do not
  hear the giver; a board takes an entry for it when its generation is newer
  than its own (8 bits, wrapping), so a late stale copy changes nothing. The
  first keys: the LED's pin and brightness. Each board's report (TWV) adds
  its LED pin, brightness and their generations, for the computer to choose
  the next generation and see the setting taken. The frame header and the
  protocol version are unchanged. Simulator (`--give`): a board that cannot
  hear the giver took it in 0.6 s through the others; a board off for 40 s
  took it on return (off for 80 s: missed, as designed).

- **tw_node 2.4.3: version and image.** Movement only (above) in
  `tw_node-s3-ch11.bin`; otherwise as 2.4.2. Program-only upgrade, the LED
  setting kept.

- **Firmware (tw_node 2.4.3): the LED shows movement only; orange is gone.**
  2.4's orange counted a minute of movement as a lasting change, which is not
  what the replay's orange means (a link's shape standing apart from its usual,
  with nothing moving): a different measurement, not made on the board. For
  now the LED is off when quiet and on the blue ramp when something moves;
  `led test` is red, green, blue, three blues, off (7 s).

- **tw_node 2.4.2: version and image.** The orange and `led try` (above) in
  `tw_node-s3-ch11.bin`; otherwise as 2.4.1. Program-only upgrade.

- **Computer side: `tw_setid.py PORT led try R G B`** (tw_node 2.4.2).

- **Firmware (tw_node 2.4.2): `TWSET led try R G B`** shows exactly that color
  (0..255 each, not scaled by the brightness) for 10 seconds, then back to
  what the LED shows: to choose colors by eye. Nothing is kept.

- **Firmware (tw_node 2.4.2): the LED's orange, amber and blinking.** The
  replay's orange, made linear for the LED, kept a seventh of red as green and
  read as red, next to the boards' red power light. Now the LED's own amber
  (255, 102, 0, scaled by the brightness: 32, 13, 0 at 32), blinking one
  second on, one off, from the once-a-second update.

- **tw_node 2.4.1: version and image.** The blues (above) in
  `tw_node-s3-ch11.bin`; otherwise as 2.4. Program-only upgrade, numbers and
  settings (the LED's too) kept.

- **Firmware (tw_node 2.4.1): the LED's blues in the replay's usual order.**
  2.4 took the dark theme's ramp, so a loud change showed near-white. Now the
  light theme's, as the replay is usually seen: faint pale blue for a small
  change to deep blue for a loud one (the quiet grey left out: quiet is off).
  On screen the navy is dark; on an LED dark means dim, so the strength grows
  with the change instead: a quarter of the brightness at quiet, all of it at
  6 x. Orange the same color, at full brightness.

- **tw_node 2.4: version and image.** The LED (above) in
  `tw_node-s3-ch11.bin`; otherwise as 2.3. Upgrade from 2.0-2.3: the program
  part only (FLASH-README), numbers and settings kept; the LED stays off
  until set. Not yet tested on a real board.

- **Computer side: `tw_setid.py PORT led on [GPIO] | off | bright N | test`**
  for the board on the cable (tw_node 2.4), and `tw_setid.py PORT` shows its
  LED setting.

- **Firmware (tw_node 2.4): an RGB LED shows the board's movement score.**
  Off unless set (per board, kept in its flash; not a network setting). It
  shows the loudest link the board measures, in the replay's colors (dark
  theme): off when quiet (under 1.15 x), the blue ramp up to 6 x, orange for a
  lasting change (a link above quiet for 60 s, until the board's quiet level
  has relearned it; a fresh change elsewhere shows over it). A WS2812-type LED
  on any free pin (not 19, 20, 26-37), driven by the RMT peripheral: 3 bytes a
  second from the status block. `tw_led.c` decides the color (plain C,
  checked against replay.py's colorFor: within 1 of 255 at every score). The
  hello line (TWH) adds the LED's pin and brightness. USB commands `TWSET led
  on [GPIO] | off | bright N | test`.

- **tw_node 2.3: version and image.** The changes above in
  `tw_node-s3-ch11.bin`, built with ESP-IDF v5.5.6; docs/TW_NODE-2.3.md (what
  changes, the simulator, the tests). Upgrade from 2.0-2.2: the program part
  only (FLASH-README), numbers and settings kept. Not yet tested on real
  radios: first a desk test that ESP-NOW sends from its send-done callback.

- **Firmware (tw_node 2.3): after one refusal, the callback leaves second
  frames to the work task.** If ESP-NOW refuses a send from its send-done
  callback, the callback stops trying (until the board restarts) and the
  work task sends them, as in 2.2; TWF `x_refused` says it happened.

- **Computer side: `second_check.py` reads 2.3's timing**: the build time, the
  receive callback after a frame's end, what second frames are cut for, and
  second frames sent from the send-done, by the task, refused there, dropped
  as too late.

- **Firmware (tw_node 2.3): the clock from the radio's own receive time.**
  Each frame says its sender's delay: the least, over its last 16 frames, of
  time stamp to send-done less the air (20 us steps), in the sender's own
  place in the hearing list (always 100%, never read; flag `TW_F_DELAY`,
  0x20; 2.2 boards ignore it). A receiver times the timekeeper's frame by the
  radio's receive time (the frame's start, on its clock through the offset
  learned from the CSI callbacks) instead of the ESP-NOW callback, which
  comes about 0.4 ms after; the sender's delay less 100 us (`TW_POST_EST_US`,
  its send-done after the air) is how long after its stamp it started. The
  protocol version stays 2. Simulator: followers 50-60 us behind the
  timekeeper (2.2: about 1240 us), the same with a 150 us slower callback.

- **Firmware (tw_node 2.3): the second frame sent from the send-done.** The
  work task builds it while the normal frame is on the air, cut for a
  send-done late in the board's usual spread (running mean + 2 spreads) and
  310 us to get on the air (2.2: 150; the boards measured 290). ESP-NOW's
  send-done callback (the Wi-Fi task, other core) sends it at once if it
  still fits, else it is dropped and its records go back in the queue as
  they were (`tw_unsend_extra`). If the callback had not got it yet, or
  ESP-NOW refuses a send there, the work task sends it as in 2.2. TWF:
  `extra_late` (dropped), `x_cb` (from the callback), `x_task` (by the task),
  `x_refused`, `ovh` (the mean / spread used). Simulator, `second force`, 6
  boards: about 5% dropped, 0.9% of those sent end past 3200 us (2.2: 92%);
  reports home unchanged.

- **Firmware (tw_node 2.3): the normal frame built ahead.** A board builds its
  frame 800 us before its slot time, stamped for that time, and hands it to
  ESP-NOW at the time: in 2.2 the build took the first 300-400 us of the slot
  (frames handed over at about 740 us, target 400). Records that arrive in
  those 800 us go in the next frame. TWF `tx_at` should now be close to 400.

- **Firmware (tw_node 2.3): two more timing figures in TWF.** `build=` how long
  building the normal frame takes (mean/max, us: is the 340 us before a frame
  goes out the build?); `rxcb=` how late the ESP-NOW receive callback comes
  after a frame's end, by the radio's own receive time; `rx_k=` the offset
  from the radio's receive time to the board's clock, learned from the CSI
  callbacks (the smallest callback - stamp - air over 8 s; 0 = not learned).
  Measurements only: nothing behaves differently.

- **Design update: frame timing (docs/TIMING-2.3.md).** The real boards'
  lag is 1.1-1.5 ms, not the 850 us first simulated; most of the difference
  is the ESP-NOW receive callback (the clock's time) coming about 400 us
  after the CSI callback. With the simulator fitted to that: still no cost up
  to 6 boards; above 6, a cost even in normal running (8 boards: worst board
  94.8% home, 150 collisions in 120 s). The header correction alone leaves
  about 390 us; timing frames by the radio's receive time as well ("hwrx")
  brings followers within 15 us and 8 boards to 0 collisions. Plan: check
  that timestamp's clock with `lag_check.py`, then build 2.3.

- **Simulator: fitted to the real boards' lag.** A receiver's CSI callback
  (its records' time) and its ESP-NOW receive callback (its clock) are now
  apart: 50-150 us and 450-550 us after the air, which reproduces
  `lag_check.py` on the boards (lag about 1.24 ms; A, B and C agree). New
  `--fix hwrx`: the clock from the radio's receive time and the sender's
  delay; `--late rxcb=`/`csicb=` set the callbacks. `timing_cases.py` runs the
  hwrx variants. Without `--late` the output is unchanged.

- **Computer side: `lag_check.py` also says how late the cabled board's CSI
  callback runs after the radio's own receive time** (each record's rx_us,
  the rx_ctrl timestamp), per sender: whether that timestamp is on the
  board's own clock and could time frames for the clock instead of a
  callback (docs/TIMING-2.3.md, "hwrx").

- **Computer side: `lag_check.py RUN`**, how far behind the timekeeper's
  clock the other boards run, from a run's own files (every record carries
  the measuring board's clock when it heard the frame). Three estimates per
  board; one needs nothing newer than 2.0. Simulator check: 849-851 us found
  where the truth was 847, and 0 +- 5 with the timing fix.

- **Design: frame timing for tw_node 2.3 (docs/TIMING-2.3.md), no firmware.**
  With the measured delays the simulator says: up to 6 boards (also with two
  boards shadowed) reports home are within 0.3 points of an ideal radio,
  because every follower is equally late; followers' clocks are still about
  850 us behind the timekeeper's; at 7-8 boards with `second force`, 6,200-
  7,400 waits for the channel, 31-75 collisions and 2,400-3,700 skipped second
  frames in 120 s. Building ahead, a clock correction in the header and the
  second frame sent from the send-done bring that to about 200 waits and 0-6
  collisions. Not needed for boards 4-6; two measurements on the real boards
  come first.

- **Simulator: the delays the real boards measured.** `sim --late measured`
  gives each board the delays tw_node 2.2 reported (frame built and stamped at
  its slot time, handed to ESP-NOW about 340 us later, about 650 us more to its
  send-done, 250 us from there to the second frame, now and then 1-5 ms
  late), and radios that wait for a busy channel instead of colliding. It
  reproduces the cabled board's TWF figures within about 50 us. `--fix
  ahead,clock,cb|queue` simulates the parts of a timing fix;
  `firmware/sim/timing_cases.py` runs the cases; `check_sim.py` prints where
  each board's frames were on the air in its slot. Without these options the
  simulator gives exactly what it gave before.

- **Computer side: a TWR line that is not a measurement is never taken for
  one.** During an upgrade, a 2.1 board on the cable prints 2.2's firmware and
  score records as ordinary TWR lines (kinds 2 and 3). `split_links.py` counts
  them as delivered and nothing more; the dashboard and `second_check.py` skip
  them.

- **tw_node 2.2: version and image.** The six changes above (and the queue
  slot fix) in `tw_node-s3-ch11.bin`, built with ESP-IDF v5.5.6. Upgrade from
  2.0 or 2.1: the program part only (FLASH-README), numbers and settings kept.

- **Firmware (tw_node 2.2): a movement score made on the board.** Each board
  computes, for each board it hears, the computer's "change": per record the
  amplitudes in dB in bands of 4 live bins, the record's own level out; per
  second each band's spread against its quiet level (a running median that
  settles after a lasting change in 10-15 minutes), averaged over the bands.
  Normal frames only. Once a second it queues a record of a new kind
  (`TW_KIND_SCORE`, 3) with the 16 scores; the cabled board prints
  `TWM,board,boot,record number,uptime,timekeeper's clock,s1..s16` (x10).
  `listen.py` keeps TWM lines; `split_links.py` counts them as delivered and
  writes `links/board_score.csv`; `score_check.py RUN` (new) compares them with
  the computer's change for the same seconds (rank correlation, loud seconds
  for both / either). Nothing uses the score yet: it is to be checked first.
  Simulator, a 30-s walk across link 1-2: the score about 10 on 1-2 both ways,
  1.0 elsewhere; 31 loud seconds for both on each direction, none for only one.
  Memory: about 800 bytes per board heard.

- **Firmware (tw_node 2.2): PSRAM on.** The boards (ESP32-S3-WROOM-1 N16R8)
  have 8 MB of octal PSRAM; the firmware now starts it, and starts without it
  on a module that has none (boards 4-6 may be another variant). It is used
  only when asked for: everything else, Wi-Fi included, stays in internal
  memory as before (with the default setting our 67 KB event queue would have
  moved there). Nothing asks for it yet. The hello line (TWH) adds the PSRAM
  found (bytes), shown by `tw_setid.py` and in `links/summary.txt` ("PSRAM 8
  MB"); TWF adds `psram_free=`, shown on the dashboard. The bootloader and
  partition table are unchanged (only build stamps differ): still a
  program-only upgrade. Flash stays 4 MB.

- **Firmware (tw_node 2.2): how busy each core is, and memory.** FreeRTOS now
  counts each core's idle time (run-time statistics on `esp_timer`: a timer
  read per task switch). The TWF line adds `core0=` and `core1=` (percent busy
  over its 10 s; Wi-Fi runs on core 0, our work task on core 1) and `heap_min=`
  (the lowest free internal memory since start). The dashboard's health line
  shows them (`core0=23% core1=6% heap_min=131k`); its `usb_drops` item is
  shorter.

- **Firmware (tw_node 2.2): where a second frame's time goes.** In the 2.1
  tests second frames ended about 450 us past their limit, and the
  timestamps (callbacks) could not tell real delay from late callbacks. The
  cabled board now times each frame of its own, in four parts, mean/max over
  10 s in its TWF line: `tx_at` (where in its slot it handed the normal frame
  to ESP-NOW), `tx_radio` (from there to its send-done), `x_wake` (from that
  send-done to handing the second frame over: our own task), `x_radio` (to the
  second frame's send-done), and `tx_len`/`x_len`. `second_check.py` prints them
  against the frames' air time: what is left is channel access and driver, or
  our task. Run it with `second force` (an exit sends second frames only then).
  Also: a board stops repeating where its last second frame ended a second
  after its last one (header byte 23 back to 0). Simulator: forced for 60 s
  then normal, ends reported only in the forced minute.

- **Firmware (tw_node 2.2): the followers' clocks no longer lag the
  timekeeper's.** A follower set its clock from the timekeeper's frames adding
  a fixed 150 us for the air, but it notes a frame when it has been received,
  at its end, and a long frame is on the air up to 890 us: every follower ran
  behind, more the fuller the timekeeper's frames. Now each frame's own air
  time counts (from its length, `tw_air_us`). Simulator (it now reports the
  followers' clock error): 3 boards -360 us -> 0 (at worst 2 us), 3 boards
  forced and 6 boards -713..-737 -> 0, 16 boards -555 -> 0 (6 us); delivery
  unchanged in every case. Real boards keep a small remainder (driver,
  channel access). The lag had left the timekeeper's late second frames room
  before the next board's slot; that room is gone, which matters in `second
  force` tests until the second frame's timing is fixed.

- **Firmware (tw_node 2.2): every board says which firmware it runs.** Until
  now only the cabled board's hello (TWH) said, over its USB. Every 5 s each
  board queues a record of a new kind (`TW_KIND_INFO`, 2) with its firmware and
  build (no factory address), carried home like any record; the cabled board
  prints it as `TWV,board,boot,record number,firmware,build`. It takes the
  board's ordinary record numbers, so delivery counts stay right (a separate
  numbering would have upset the de-duplication window). `tw_setid.py PORT`
  shows each board's firmware ("not reported (2.1 or older)" for a board that
  does not say); `links/summary.txt` and the replay's "About this recording"
  list it per board; the dashboard says `FIRMWARE ...` on a board that runs
  something other than the cabled one. `listen.py` keeps TWV lines (it keeps
  only the line kinds it knows). Simulator: 12 a board in 60 s, delivery
  unchanged (0.0-0.1%).

- **Firmware (tw_node 2.2): a reused queue slot forgets that it held a
  relayed record.** When a slot that had carried a relayed record was reused
  for one of the board's own measurements, its "relayed" mark stayed: the
  record was sent once only (no second copy) and counted as relayed. Only
  boards that relay were affected. Simulator, 6 boards, two stranded at once:
  with the second frame (the default) unchanged within 0.2 points; without it
  the relays' room is shared differently (boards 2 / 3: 82.8% / 53.5% ->
  71.0% / 64.5%), since the relays' own records keep their second copies.

- **`dash.py` shows the second frame and the test settings.** A board's line
  says `2nd N/s` while it sends second frames (normally nothing: with 3
  boards they hardly ever go), and any 2.1 setting that is not the default,
  loudly: `2nd FORCED`, `2nd OFF`, `MEASURING 2nd`, `CAP N`, so a test left on
  shows before a run. The health line adds `2nd sent +N` and `rx_err +N`
  (measurements the radio flagged) since the dashboard opened, as
  `usb_drops`. Records measured from second frames (`measure-second on`) count
  as delivered but no longer go into the link rows, as in the link files: a
  link's packets/s, motion and CSI view stay one measurement per period.
  Desk test: the dashboard fed live from the simulator, forced / normal /
  forced and measured.

- **`second_check.py`: a board's reported end only counts while its second
  frames are heard.** Each board keeps saying where its last second frame
  ended, also after it has stopped sending them, so a normal run straight
  after a forced test showed "own end 3560 us, 602 s over 3200" with no second
  frame sent (2026-11-06). Now it shows "-" there. (The firmware will stop
  repeating the old value in its next release.)

- **Computer side for the second frame (tw_node 2.1).** `tw_setid.py PORT
  second on|off|force`, `measure-second on|off`, `cap N|off`, each confirmed
  by the board on the cable like the power; `tw_setid.py PORT` now shows these
  settings for every board, the test ones loudly (they survive restarts: put
  them back after a test). `second_check.py RUN` (new) reads a run's own
  files: per board, second frames announced / heard / with CSI, the gap after
  the normal frame, where the sender says its second frame ended in its slot
  (limit 3200 us), rx_state-flagged measurements; the cabled board's own
  second frames; and with `measure-second on`, the second frame's CSI against
  the normal frame 1 ms before (RSSI, shape dB) next to two normal frames one
  period apart. `split_links.py` keeps records measured from second frames out
  of the link files (each link stays one measurement per period) but counts
  them as delivered, so they do not show as lost reports; `summary.txt` lists
  them. Desk test (simulator, 2 boards forced and measured, 3% loss): 97.3% of
  announced second frames heard, all with CSI, gap 1085 us, own end 2460 /
  2660 us; delivery 0.0-0.1% lost, as without.

- **Firmware (tw_node 2.1): a second frame in a board's own slot.** At 6 boards
  a frame's 6 places are mostly taken by the sender's own 5 new records, so
  second copies and relayed records stopped fitting (simulator: 97.6% home,
  two stranded boards 83% / 53%). Now, when records are still waiting after a
  board's frame has gone out (ESP-NOW's send-done), it sends an extra frame
  right after, in the same slot, cut to end within 200 us of the next board's
  400 us guard. It has the same frame number and a flag, carries records as
  usual, and counts for nothing else (hearing, clock, settings), and no
  measurement is made from it by default, so each link is still measured once
  per period. Exits send none. Simulator, same cases: 6 boards 99.9%, two
  stranded boards 98.6% each, airtime 26% -> 40%, every extra frame ended by
  2834 us into its slot (limit 3200); 3 boards unchanged (all fits: no extra
  frames). Test settings, spread like the power: `second on|off|force` (force:
  every board, full-length frames), `measure-second on|off`, `cap N|off`
  (records per frame; `cap 2` crowds 3 boards like 6). The cabled board
  reports per board (TWN): extra frames announced, heard, with CSI, the gap
  after the normal frame, and where the sender says its extra frame ended
  (header byte 23, by its own clock). Design, results and the tests to run:
  docs/SECOND-FRAME.md. Found on the way: followers' clocks lag the
  timekeeper by a few hundred us (long frames; ROADMAP).

- **Firmware (tw_node 2.1, with the second frame below): the radio's error flag
  is kept.** A CSI measurement whose receive info says `rx_state != 0` (the
  radio reports the frame as received with an error; Espressif does not
  document the numbers) was recorded like any other. Now its record carries a
  flag (`kind` bit 0x40; the record format is unchanged and older tools ignore
  the bit), nothing is dropped. `split_links.py` writes it into the CSI_DATA
  line's `rx_state` field (0 or 1) and lists flagged records per link in
  `links/summary.txt`; the cabled board's TWF line counts them (`rx_err=`).
  Idea from ESPectre, which drops such frames. Whether to leave them out of the
  measures is for later, once a night shows what they line up with (the
  one-direction blips on link 1-3? board 1's weaker receiving side?).
  Simulator: `--rxerr P` flags a share of measurements; desk test: 327 flagged
  in 60 s, all 327 counted on their links.

- **`replay.py`: a link's quiet level is learned again after a lasting change.**
  A link's change (how much its shape fluctuates within a second) was measured
  against its quiet level over the whole run. A parked car can leave a link
  more restless (or steadier) for good: on a real run, two directions sat at
  3-4x their threshold from the moment a car parked to the end of the run, one
  dark band and one long stretch, and no new event could start on them. Their
  thresholds also came from a mix of both scenes, so they fit neither. Now the
  run is split into scenes at each link's lasting changes: a link keeps the
  quiet level of the scene before for `--settle` minutes (+1) after the change,
  so the change shows as long as the orange mark does, then its quiet level is
  the new scene's own median. Only a lasting change does this: someone working
  in the garage for half an hour stays loud. A scene under a minute long after
  settling keeps the one before. "About this recording" lists when each link's
  quiet level was learned again, and the terminal prints it under the lasting
  changes. A run without lasting changes comes out as before (desk test: page
  data identical but for "About").
  Desk test (synthetic, `tests/gen_run.py` with a car on link 2-3 at 08:30 and
  someone walking through at 08:55): before, 2->3 read 0.65x quiet before the
  car and 1.1x after (the median sat between the scenes), its threshold was
  6.8, and the walk came out as 20 of its 30 s; now 1.00x before, 1.9x for the
  16 minutes after the car, relearned at 08:46:00, 1.00x after, the walk 30 s
  in full.

- **`replay.py`: the watch's events say "watch:", not "camera:".** A watch
  run started without the camera (`--nocam`) still has events (radio only),
  and they showed as "camera: radio only". They now read "watch: person",
  "watch: radio only", accurate with or without a camera.

- **`replay.py` on watch runs: the camera beside the radio.** A watch run's
  camera events (detect/events.csv, with your label and note) are marked on
  the timeline in their own color and listed with the lasting changes; events
  the camera kept frames for are tagged "camera"; and a camera panel shows the
  frame of the second under the cursor (one a second, around the watch's
  events; between them it says so and offers the next one). The frames are
  linked, not copied into the page, so it stays small and shows them while it
  sits in its watch folder; `replay.py`'s help says how to share it through a
  small web server (`python3 -m http.server` in the folder). Moved away from
  its folder, the page says the frames were not found and why.


  An event of 2 minutes or more is now a *stretch* (a link that stays
  restless, as after a car has left), and the moments inside it that rise
  well above the stretch's own recent level are listed under it as events of
  their own (E48a, E48b, ...): at least two directions at 1.8x their level of
  the 5 minutes before, or a 20-point jump in loss; a peak with only one
  direction's loss behind it is marked weak. Before, a stretch merged
  everything into one long event: on a real morning a car leaving was
  followed by half an hour of restlessness, and two walks in the house near
  the garage, noted by the owner, were inside it unseen; now one comes out as
  a peak on three directions and the other as a weak one. The timeline draws
  a bar along each stretch with its peaks ticked.
  The page also says what instrument made the recording, from the boards' own
  reports: the title line gives the transmit power ("all boards at 11 dBm"),
  and an "About this recording" section lists the firmware and its build,
  each board's power and relaying, any power change (also marked on the
  timeline), the bandwidth, the layout used, the thresholds, the settle time
  and the kit version. Runs at different powers compare by shape and RSSI
  steps, not by event sizes.

 Read from the
  boards' own reports during the checked stretch (the TWN and TWS lines) and
  printed under the time: "transmit power: 11 dBm on every board (1, 2, 3)",
  or each board's when they differ (then links are not compared fairly), and
  whether it changed during the check. Comparing two nights' checks is only
  fair at the same power: a night at 8 dBm and one at 11 dBm differ by 3 dB on
  every link before anything in the room changes.

 After `m` (the spot's
  label), the survey dashboard asks if a board was moved there: its number
  and new x y in feet, as in boards.csv (Enter: none). The move goes to
  moves.csv in the survey folder and the map shows the board there at once.
  `survey.sh stop` then offers to write the moves into the layout (the last
  position typed for each board), with the usual backups; a survey that ended
  by itself is offered at the next `survey.sh` (or `tw_layout.py confirm`),
  and the answer is noted in the folder (moves-done.txt) so it is asked once.
  `tw_layout.py moves SURVEY_FOLDER` does it by hand. `replay.py` shows a
  survey's marks and moves as notes on its timeline.

 With nobody
  moving, a link's signal falls with its length, so each link's median RSSI
  is set against its length in the layout. It flags a link 8 dB or more off
  (a board in the wrong place in the file, or something solid in the path), a
  much better fit with two board numbers swapped, a board the others hear
  much better (or worse) than it hears them on all its paths, loss at rest,
  and anything moving during the check. `survey.sh` runs it on 2 quiet
  minutes once it records (layout-check.txt in the folder); it works on any
  run folder (`--at HH:MM --minutes 10`).
  Desk tests on a real night (3 boards): on a quiet stretch every link fits
  its length within 6 dB, and 2 minutes say what 10 do; with two boards'
  positions swapped in a copy of the layout it names the swap; during
  movement it says so instead of reporting loss at rest. It found one thing
  on the real layout: the cabled board hears the other two about 6 dB worse
  than they hear it, on both its paths: its receiving side (possibly noise
  from the computer on its cable), where a 31 s rhythm on that board's
  receiving side had been seen before.

 The layout is
  what every map, report and replay is drawn on, so it now has a tool of its
  own: `tw_layout.py` shows it (to scale, with a table), `edit` changes it
  value by value (Enter keeps each value; feet from one reference point you
  choose, x east, y north; boards and landmarks, added or removed), `backups`
  lists the earlier versions with what differs and puts one back. Every
  version is kept in `layouts/` beside the file, with the time in its name:
  before and after each edit, and at the start of each run or survey when the
  file changed since the last kept one (an edit with a text editor). Nothing
  is overwritten.
  **One file:** `private/boards.csv`. A second site runs with `TW_DATA` set
  to its own folder and keeps `boards.csv` there. `~/tw-data/boards.csv` still
  counts when `private/boards.csv` does not exist.
  **survey.sh** now starts with the layout: it shows it and asks if it is
  still right (Enter = yes, e = edit, b = an earlier version), then the scene
  as run.sh asks it; the survey folder keeps both, as a run folder does.
  **replay.py** draws the run folder's copy, boards and landmarks alike (edit
  that copy to add a landmark to an old run); a folder without one is drawn
  with the layout as it is now, and the page and the terminal say so.

- **`replay.py`: near a board, and one-sided.** Two marks for where
  something is. *Near a board*: everything lit touches one board, on at least
  two of its paths, at least one of them lit both ways, while the path away
  from it stays quiet; it is drawn as rings all the way round that board
  (something near it on any side, behind it too, where no path runs), and
  events mostly near one board are tagged "near N". *One-sided*: a direction
  lit while the other way of the same path is clearly quiet (under a third of
  the way to its own threshold, since thresholds differ); a real change shows
  both ways, so this is more likely a board's own quirk; it is drawn hollow,
  with "one-sided" in the table. Lit means past the direction's event
  threshold, held 3 seconds; each mark has to last 3 seconds. On a real night
  the rings came up only in the half hour of activity, around the board a car
  was parked beside, while someone was at the car and while it pulled out;
  one-sided came up as short blips at night, most often on one board's
  receiving side, where a 31 s rhythm of that board had been found before.

- **`replay.py`: the orange mark clears.** A link's shape is now compared with
  its usual *lately* (the median of the 30 minutes before, ending a minute
  earlier) instead of over the whole run, so something parked, moved or opened
  is marked for about 15 minutes and then counts as the usual: after a car
  left, the mark used to stay on its links until the run ended; now it clears
  about 16 minutes later. If it comes back, that is a change again and is
  marked.
  `--settle MIN` sets the minutes. The timeline ticks and the list of lasting
  changes stay as they were.

- **`replay.py`: waves.** The page now opens with each board drawn as a
  beacon: one fan of arcs per path, pointing to the other board, for the
  board's own signal reaching it, in the same colors as the lines; up to 3
  arcs lit for how much that direction changed (still, so a paused screenshot
  is exact), and while it plays, echoes running out along the path; a faint
  path stays under each pair so a lit fan reads as "this path", not "at this
  board". Both ends of a path lit: something real; one end only: a board's
  own quirk. Fans narrow to fit beside each other. `--view lines` opens with
  the lines instead; a switch on the page flips between them. Echoes are off
  for viewers who ask their computer for reduced motion.

- **`replay.py`: a run replayed on the board layout.** One page (no outside
  links, opens offline) with every link drawn twice, one line per direction,
  colored by how much it changed each second (its fluctuation in times its
  quiet level), orange dashes where its shape stays off its usual (something
  standing or parked), dots where its frames were lost; a timeline of every
  direction for the whole run; events; lasting changes found by themselves (a
  link whose shape steps by 1 dB, or its RSSI by 3 dB, in both directions and
  stays); notes of your own (`--note "07:15 door opened"`). It reads the run
  folder's boards.csv first, then private/, then ~/tw-data, and asks for a
  title at a keyboard (`--title`). The measures work on bands of 4
  subcarriers after taking out each record's overall level, so a change of
  power or gain is not taken for movement. Thresholds come from each
  direction's quietest hour (99.9th percentile, capped at the median + 20x its
  spread, so a long event in a short run cannot raise its own threshold).
  Desk tests: on tests/gen_run.py the planted movement (60 s on 1-2) is the
  one event and the planted RSSI step (+6 dB) is found at the second it was
  planted; the whole-channel and one-link losses show as gaps. On a real
  night (4 hours, 3 boards, 1.5 minutes to read) a car leaving shows as a
  lasting change on the links next to its bay (shape 4.4 dB, RSSI +4.8 dB),
  four minutes after the owner noted getting in. It cannot place a person:
  with three boards it shows which paths were disturbed.

- **Dashboard: `usb_drops` counted from when the dashboard opened.** The
  board counts every line it could not hand over since it booted, and most of
  those are from the hours when nothing was reading the port (the buffer
  fills and stays full), so a large number said nothing about the run under
  way. The health line now shows `usb_drops=+N since start (M since boot)`:
  N is what matters while watching. Counting on the board only while a
  computer reads stays on the roadmap (firmware).

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

