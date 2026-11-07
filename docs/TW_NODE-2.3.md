# tw_node 2.3: frame timing

Status: built 2026-11-07; simulator checked; desk test and real runs to come.
Why and how it was chosen: docs/TIMING-2.3.md.

## What changes

1. **Two more timing figures** (TWF): `build=` how long building the normal
   frame takes; `rxcb=` how late the ESP-NOW receive callback comes after a
   frame's end, by the radio's own receive time; `rx_k=` the offset from the
   radio's receive time to the board's clock.
2. **The normal frame built ahead**: 800 us before its slot time, stamped
   for that time, handed over at the time. `tx_at` should be close to 400
   (2.2: about 740).
3. **The second frame sent from the send-done**: built while the normal one
   is on the air, cut for a send-done at the board's running mean + 2
   spreads and 310 us to get on the air; ESP-NOW's send-done callback sends
   it if it still fits, else it is dropped and its records go back in the
   queue. If the callback had not got it, or ESP-NOW refuses a send there,
   the work task sends it as 2.2 did. TWF: `x_cb`, `x_task`, `x_refused`,
   `extra_late`, `ovh`.
4. **The clock from the radio's receive time**: each frame carries its
   sender's delay (time stamp to send-done less the air, the least of its
   last 16; in the sender's own place in the hearing list, flag 0x20, which
   2.2 boards ignore). A receiver times the timekeeper's frame by the
   radio's receive time (the frame's start) instead of the ESP-NOW callback.
   Followers should be within about 65 us of the timekeeper (2.2: 1.1-1.5
   ms).

The frame format and protocol version are unchanged; 2.2 and 2.3 boards hear
each other, but a 2.2 follower keeps its 1.1-1.5 ms lag: **upgrade every
board**. Program-only upgrade (FLASH-README): the bootloader and partition
table are the same as 2.2's.

## Simulator (firmware/sim, the same tw_proto.c; `timing_cases.py`)

With the delays the 2.2 boards measured (120 s, 3% loss, seed 7):

| case | 2.2 | 2.3 |
|---|---|---|
| followers' lag | 1234-1321 us | 50-65 us |
| followers' normal frames on the air | about 2540 us into their slots | about 1035 us |
| 6 boards, `second force`: second frames past 3200 us | 32,919 of 35,913 | 1,002 of 34,042 (1,937 dropped as too late) |
| 6 boards, two shadowed: worst board home | 97.7% | 97.9% |
| 8 boards, normal: worst board home; collisions | 94.8%; 150 | 96.9%; 1 |
| 8 boards, `second force`: waits, collisions | 9,500; 196 | 208; 3 |

Reports home: unchanged up to 6 boards; above 6, as with an ideal radio.
A 150 us slower receive callback, or the radio late five times as often:
the same within 0.6 points.

## Tests

1. **Desk, 2 boards, `second force`** (10 minutes; then `second on`): does
   ESP-NOW send from its send-done callback (`x_cb` grows, `x_refused` 0)?
   `second_check.py`: `tx_at` close to 400, second frames ending under 3200
   us; `build` under 800 us.
2. **Three boards, `second force`, 10 minutes**: `second_check.py` as above,
   and `lag_check.py`: lag A/B/C under 100 us (2.2: 1.1-1.5 ms).
3. **A normal night**, compared with one before (`links/summary.txt`):
   nothing should change but the timing.
