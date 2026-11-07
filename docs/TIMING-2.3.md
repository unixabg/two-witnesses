# Frame timing (tw_node 2.3?): simulator results, design, no firmware yet

Status: 2026-11-07. Built as tw_node 2.3 (docs/TW_NODE-2.3.md), the "hwrx" clock
learning the radio's offset from the CSI callbacks; this note is how it was
chosen. The
question was whether the delays the 2.2 boards measured (docs/TW_NODE-2.2.md,
section 3) will hurt once there are 6 boards or more, and what fixing them
would buy.

## What the simulator now does

`sim --late measured` replaces the ideal radio with the delays the boards
measured (TWF, the cabled board): a board builds its frame and stamps its time
at its slot time; hands it to ESP-NOW 270-430 us later; the radio puts it on
the air later still, and its send-done comes 50-150 us after the air.
Handed-over to send-done, less the air, is 650 us for a normal frame and 290
us for a second one; from send-done to handing the second frame over, 170-330
us. One frame in 500 is 1-5 ms late. A receiver's CSI callback (the time on
its record) comes 50-150 us after the air, and its ESP-NOW receive callback
(the time its clock uses) 450-550 us after: fitted to `lag_check.py` on the
real boards (below). A radio that finds the channel busy waits (as real Wi-Fi
does) instead of colliding. `sim --fix ...` adds the changes below;
`firmware/sim/timing_cases.py` runs every case (120 s, 3% loss, seed 7).

How it compares with the real boards (board 1 cabled and timekeeper):

| | real (2.2) | simulator |
|---|---|---|
| normal frame handed over (target 400), `second force` | 705-743 / 781-832 us | 750 / 830 us |
| ... to its send-done (air 887 us) | 1534 / 4220 us | 1544 / 6858 us |
| send-done to second frame handed over | 250 / 335 us | 250 / 330 us |
| ... to its send-done | 1049 us (air 761) | 1048 us (air 751) |
| second frame's end, own clock | about 3540 us | 3584 us |
| followers' lag (`lag_check.py`, A / B / C) | board 2: 1069-1236, board 3: 1272-1475 us | 1231-1246 us |
| board 2's frames arrive, by the timekeeper's clock | 3214-3308 us into its slot | 3233 us |

(mean / worst of 10-s means.) What the match says:

- The second frame's length is decided right at the send-done, so most of
  the 250 us "wake" is the build of the frame itself. The 340 us before a
  normal frame is probably the same (the board waits for its slot busily,
  then builds): to be checked.
- On the real boards the three estimates of `lag_check.py` agree (within 10
  us on one run). In the simulator they only agree when a record's time and
  the time the clock uses come from callbacks that are equally late after
  the air. They are not: the ESP-NOW receive callback, which sets the clock,
  comes about 400 us after the CSI callback (300-550 us: board 3 more). So
  the lag is the timekeeper's stamp-to-air delay plus that late callback.

## What it shows

1. **The followers' clocks are 1.1-1.5 ms behind the timekeeper's** (real
   boards, `lag_check.py`; simulator 1.24 ms). 2.2 counted each frame's own
   air time, which removed up to 700 us; the rest is the stamp being taken
   when a frame is built, about 1 ms before the air, and the receive
   callback coming about 0.5 ms after it.
2. **Up to 6 boards it costs nothing.** 3 boards, 6 boards, 6 forced, 6 with
   two boards shadowed from the cabled one: reports home within 0.4 points
   of the ideal radio (6 shadowed: 97.7% worst board, ideal 97.9%).
   Followers' normal frames go on the air about 2540 us into their slots
   (target 400), and board 2's really arrive after its own slot has ended;
   their second frames end about 4500-4600 us in. The next board is just as
   late, so they do not meet: 100-150 waits for the channel in 120 s, 0-3
   collisions.
3. **Above 6 boards it costs, even in normal running.** The period is then
   3 ms per board with nothing spare, and the last board's late frames run
   into the timekeeper's slot, which has no lag. 8 boards, normal settings:
   150 collisions, 3,300 waits and 1,800 skipped second frames in 120 s;
   worst board 94.8% home against 96.9% with an ideal radio. 7-8 boards with
   `second force`: 9,400-9,500 waits, 117-196 collisions, 6,600-7,200
   skipped second frames.

## The fix (each part simulated)

| part | what it does |
|---|---|
| ahead | build the frame ahead, hand it over at its time (0-40 us late) |
| clock | each frame says its sender's smallest recent stamp-to-send-done delay (one header byte, 20 us steps); receivers add it to the clock |
| hwrx | as clock, and receivers time the frame by the radio's own receive time (rx_ctrl timestamp: the frame's start) plus its air time, not by the callback, less 100 us for the sender's send-done delay |
| cb | the second frame sent from the send-done itself, its length cut for 310 us of access instead of 150 |
| queue (instead of cb) | the second frame built at once and handed over behind the first, cut for the first's worst start |

| case | 2.2 as measured | ahead + clock + cb | ahead + hwrx + cb | ahead + hwrx + queue |
|---|---|---|---|---|
| followers' lag | 1234-1321 us | 386-398 us | 3-14 us | 1-12 us |
| 6 forced: followers on air; 2nd frames over 3200 / 3400 | 2543 us; 32,919 / 32,000 | 1370 us; 27,162 / 16,666 | 986 us; 1,257 / 96 | 986 us; 233 / 218 |
| 8 healthy: worst board home; collisions | 94.8%; 150 | 96.4%; 0 | 96.2%; 0 | 96.4%; 0 |
| 8 forced: waits, collisions, skipped | 9,500, 196, 7,206 | 219, 1, 92 | 188, 1, 88 | 387, 7, 30 |

- "clock" alone leaves about 390 us: it covers the sending side, not the
  late receive callback. "hwrx" covers both. It needs the radio's receive
  timestamp to run on the same clock as the board's own timer: the cabled
  board's records say (`lag_check.py`, "CSI callback after the radio's
  receive time"): if that comes out as the frame's air time plus a little,
  it does.
- Frames cannot get closer than about 985 us into their slots: the rest is
  the radio's own time before the air, which the board does not control.
- A clock correction without "cb" (or "queue") makes followers' late second
  frames run into the next board more, since the lag that kept them apart
  is gone.
- "cb" or "queue": queue skips almost nothing but collides a little more;
  cb depends on ESP-NOW accepting a send from its own send-done callback,
  queue on it accepting a second send before the first is done. Either needs
  a desk test.
- Up to 6 boards, reports home unchanged with any of them; above 6, back to
  about what an ideal radio gives.

## Plan

1. `lag_check.py` on a real run: the receive timestamp's clock (above).
2. tw_node 2.3, one commit per change: the two measurements (where each
   board's frames arrive by the timekeeper's clock; the time the build
   takes); build ahead; the second frame from the send-done ("cb"); the clock
   from the radio's receive time with the sender's delay in the header
   ("hwrx"); version and image.
3. A desk test that ESP-NOW sends from its send-done callback; the 10-minute
   `second force` run with `second_check.py` and `lag_check.py` (target: lag
   under 50 us, second frames ending under 3200 us); then a normal night
   compared with one before.
