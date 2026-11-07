# Frame timing (tw_node 2.3?): simulator results, design, no firmware yet

Status: 2026-11-07, simulator only. The question was whether the delays the
2.2 boards measured (docs/TW_NODE-2.2.md, section 3) will hurt once there are
6 boards, and what fixing them would buy.

## What the simulator now does

`sim --late measured` replaces the ideal radio with the delays the boards
measured (TWF, the cabled board, desk and garage runs): a board builds its
frame and stamps its time at its slot time; hands it to ESP-NOW 270-430 us
later; the radio puts it on the air later still, and its send-done comes
50-150 us after the air (a receiver's receive callback as late). Handed-over
to send-done, less the air, is 650 us for a normal frame and 290 us for a
second one; from send-done to handing the second frame over, 170-330 us. One
frame in 500 is 1-5 ms late. A radio that finds the channel busy waits (as
real Wi-Fi does) instead of colliding. `sim --fix ...` adds the changes below;
`firmware/sim/timing_cases.py` runs every case (120 s, 3% loss, seed 7).

How it compares with the real cabled board (`second force`, 3 boards):

| | real (2.2) | simulator |
|---|---|---|
| normal frame handed over (target 400) | 705-743 / 781-832 us | 750 / 830 us |
| ... to its send-done (air 887 us) | 1534 / 4220 us | 1543 / 6488 us |
| send-done to second frame handed over | 250 / 335 us | 251 / 330 us |
| ... to its send-done | 1049 us (air 761) | 1049 us (air 752) |
| second frame's end, own clock | about 3540 us | 3586 us |

(mean / worst of 10-s means.) One thing the match says: the second frame's
length is decided right at the send-done, so most of the 250 us "wake" is the
build of the frame itself. The 340 us before a normal frame is probably the
same (the board waits for its slot time busily, then builds): to be checked.

## What it shows

1. **The followers' clocks are still about 850 us behind the timekeeper's**
   (simulator; 790-850 us whatever the split of the radio's 650 us between
   before and after the air, since the lag is their sum). A frame's time is
   stamped when it is built, and it reaches the air about 1 ms later. 2.2
   counted each frame's own air time, which removed up to 700 us; the
   simulator then had an ideal radio, hence its "within 6 us". Not yet
   measured on the boards.
2. **Up to 6 boards it costs nothing.** 3 boards, 6 boards, 6 forced, 6 with
   two boards shadowed from the cabled one: reports home within 0.3 points
   of the ideal radio (6 shadowed: 98.0% worst board, ideal 97.9%). Followers'
   normal frames go on the air about 2150 us into their slots (target 400),
   and their second frames end about 4130-4210 us in, 1.2 ms into the next
   board's slot. But the next board is just as late, so they do not meet:
   90-160 waits for the channel in 120 s (540-830 if the radio is late five
   times as often), 0-3 collisions.
3. **It costs when the period has no room to spare and second frames are
   many.** 7 or 8 boards (period 3 ms per board) with `second force`: 6,200-
   7,400 waits, 31-75 collisions, 2,400-3,700 second frames skipped, reports
   home 0.3-0.6 points lower (96.9-97.1% against 97.3-97.5%). The last
   board's late frames run into the timekeeper's slot, which has no lag. In
   normal running above 6 boards records are compact and sent once, and
   second frames are rare (8 in 120 s at 8 boards): no effect there.

## The fix, if wanted (each part simulated)

| part | what it does | 6 forced: followers on air, 2nd frames over 3200 / 3400 | 8 forced: waits, collisions, skipped |
|---|---|---|---|
| none (2.2) | | 2153 us; 32,918 / 32,039 | 6,218, 57, 2,801 |
| ahead | build the frame ahead, hand it over at its time | 1516 us; 31,708 / 28,393 | 1,237, 2, 82 |
| + clock | each frame says its sender's smallest recent stamp-to-send-done delay (one header byte, 20 us steps); receivers add it to the clock | 976 us; 25,252 / 5,996 | 233, 2, 83 |
| + cb | the second frame sent from the send-done itself, its length cut for 310 us of access instead of 150 | 977 us; 1,164 / 93 | 215, 6, 83 |
| + queue (instead of cb) | the second frame built at once and handed over behind the first, cut for the first's worst start | 975 us; 236 / 220 | 260, 9, 6 |

- Frames cannot get closer than about 975 us into their slots: the rest is
  the radio's own time before the air, which the board does not control.
- "clock" alone removes the lag that keeps a follower's late second frame
  apart from the next board, so it must come with "cb" (or "queue").
- "cb" or "queue": queue skips almost nothing but collides a little more;
  cb depends on ESP-NOW accepting a send from its own send-done callback,
  queue on it accepting a second send before the first is done. Either
  needs a desk test.
- Reports home: unchanged up to 6 boards with any of them; at 7-8 forced,
  97.2-97.5%, as with the ideal radio.

## Recommendation

Not needed for boards 4-6. Worth doing before going past 6 boards with second
frames in use, or if anything comes to depend on the boards' clocks agreeing
(today nothing does). Two cheap measurements first, the next time the
firmware changes anyway:

1. The cabled board, when it is the timekeeper, reports where each board's
   normal frame arrives in its slot: on the real boards, are followers about
   850 us behind as the simulator says?
2. Stamp before and after building the normal frame: is the 340 us the
   build (then "ahead" removes it), or a late wake?

Then decide on "ahead + clock + cb", with a desk test of ESP-NOW sending from
its callback, and the 10-minute `second force` run with `second_check.py`.
