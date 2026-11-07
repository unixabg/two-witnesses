# What changed, version by version

Newest first. Each entry keeps the reasoning and the desk tests behind the change;
the README has only what you need to run the kit.

## What changed in v5.5-next (work in progress, on the next branch)

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

