# Protocol 3: a lean frame header, and one transport for settings and commands

Status: design, 2026-11-08. No firmware yet; each step below is built and
checked (simulator, then a real night) before the next.

## Why

The frame header (40 bytes, tw_proto.h) is full. Some of it does not need to
travel in every frame: the network settings (they change rarely; repeating
them in every frame is just how they spread), the sender's uptime, and a
diagnostic for the second frame. Since tw_node 2.5 records can carry settings
(TW_KIND_SET, docs/CHANGES.md): the board on the computer gives an entry
"for board N or all: key = value, generation G", every board passes it on,
and the named boards take it. Protocol 3 makes that the one way settings and
commands travel, and keeps in the header only what each frame has to carry.

Rule from here on: **a new setting or command is a key on this transport**,
not a new header byte or a new mechanism.

## The header in protocol 3

| bytes | 2.x | 3.0 |
|---|---|---|
| 0-1 | magic "TW" | the same |
| 2 | protocol version 2 | 3 |
| 3 | sender | the same |
| 4-7 | frame number | the same |
| 8-11 | time stamp | the same |
| 12 | timekeeper | the same |
| 13 | flags | the same (TW_F_DELAY retired: the delay has its own byte) |
| 14-15 | boards heard | the same |
| 16 | period, ms | the same |
| 17 | records in the frame | the same |
| 18-19 | the sender's uptime, s | **18: the sender's delay** (time stamp to send-done less the air, 20 us steps; 2.3's, now in its own place); **19: reserved** |
| 20-22 | network settings: setting number, power, flags | **reserved** |
| 23 | where its last second frame ended | **reserved** |
| 24-39 | how well it hears boards 1-16 (its own place: the delay, 2.3+) | how well it hears boards 1-16 (its own place: 100 again) |

Reserved bytes are sent as 0 and **ignored by a 3.0 receiver**, so a later
use must be harmless where they read 0. They cost about 0.6 us of air each
per frame (3 us for the five): kept as room for the unknown.

Where the moved fields go:
- network settings: standing settings on the transport (below);
- uptime: the board's report (TWV), every 5 s;
- the second frame's end: the movement-score record (TWM), once a second, as
  one more field.

## The transport

### A record of entries

A new record kind, `TW_KIND_CMD` (5), printed as `TWX`; 2.5's `TW_KIND_SET`
(4) stays readable during the steps and is dropped in 3.0. Its payload is a
list of entries:

| byte | |
|---|---|
| 0 | target: a board 1-16, or 0 = all |
| 1 | key |
| 2 | flags: 0x01 report (this is the sender's own state, not an instruction) |
| 3 | generation (8 bits, wrapping: newer = (int8)(new - old) > 0) |
| 4 | seconds left to pass it on |
| 5 | value length L (0-8) |
| 6.. | value, L bytes, little end first |

Variable length lets a key carry what it needs (a color is 3 bytes, a power
1); a board skips an entry whose key it does not know, by its length, so new
keys never break older boards on the same protocol.

### Three classes of key

| class | what | kept | passed on | examples |
|---|---|---|---|---|
| **board** | a setting of one board (or all), its own | in the board's flash | for 60 s after it is given | LED pin, LED brightness |
| **network** | one value for every board | in every board's flash | **standing**: every board says its current value every 5 s, for ever (newest generation wins) | power, relay, second frame, measure-second, cap |
| **action** | do something once | not kept | for 60 s after it is given | LED test, LED try R G B |

- A board takes an instruction entry for it (target = its number or 0) when
  the generation is newer than the one it holds for that key, and passes it
  on as the class says.
- Standing settings replace the header's spreading: a board that was off for
  hours catches up within about 5 s of coming back (the header: one frame),
  and nothing expires.
- An action runs once per new generation. Its generation is kept in memory
  only: a board that restarts within the minute may run it once more
  (harmless for the LED; any future action that must not repeat says so).

### Reports and confirmation

Every 5 s each board sends report entries (flag 0x01, target = itself) with
its value and generation of every board and network key, and the generation
of the last action it ran. The computer reads them (TWX lines):
`tw_setid.py PORT` shows every board's settings; a command waits until the
named boards report the new generation, and names those that did not. 2.5's
LED fields in TWV are dropped in 3.0 (kept until then).

### Keys (first list)

| key | class | value | command (`tw_setid.py PORT ...`) |
|---|---|---|---|
| 1 | board | LED pin, 1 byte (0 = none) | `led on [GPIO]`, `led off` [`board N\|all`] |
| 2 | board | LED brightness, 1 byte | `led bright N` [`board N\|all`] |
| 3 | action | LED test, 0 bytes | `led test` [`board N\|all`] |
| 4 | action | LED try, 3 bytes R G B | `led try R G B` [`board N\|all`] |
| 16 | network | power, 1 byte (quarter dBm) | `power DBM`, `power default` |
| 17 | network | relay, 1 byte (on/off) | `relay on\|off` |
| 18 | network | second frame, 1 byte (on/off/force) | `second on\|off\|force` |
| 19 | network | measure-second, 1 byte | `measure-second on\|off` |
| 20 | network | cap, 1 byte (0 = off) | `cap N\|off` |

Without `board ...`, a board key or action is for the board on the cable (as
now); network keys are always for every board. A future feature adds a key
and a command here.

## Steps

Each step: the simulator first (the same tw_proto.c), then a real run and a
night, then the next.

0. **2.5.1: the new record, and the LED's actions over the air.** TW_KIND_CMD
   with variable-length entries and report entries; keys 1-4. `led test` and
   `led try` gain `board N|all`. 2.5's TW_KIND_SET still read. Check:
   simulator `--give` with every key; on the boards, `led test board all`.
1. **2.6: network settings both ways.** The header carries them as now, and
   every board also says them as standing settings (keys 16-20). Each board
   compares the two and counts any disagreement (TWF). Commands still go the
   header's way, and give the same setting on the transport. Check: the
   simulator with the header's spreading switched off converges on its own
   (a board off for an hour, stranded boards, two changes in a row); on the
   boards a night with no disagreement, and a power change both ways.
2. **2.7: reports both ways.** Uptime in TWV and the second frame's end in
   TWM as well as in the header; the computer tools read the new places
   (falling back to the old). Check: `second_check.py` the same both ways.
3. **3.0: the header changes.** Protocol version 3; the moved fields go, the
   delay gets byte 18, reserved bytes are 0 and ignored; TW_KIND_SET and
   TWV's LED fields go. 2.x and 3.0 boards ignore each other's frames: every
   board is flashed together (best when boards 4-6 arrive, as they are
   flashed then anyway). The computer tools read 2.x and 3.0 runs alike.
   Check: the simulator's whole suite, then the boards: a short run with
   `second force` (second_check, lag_check), then a night against a 2.x one.

Until 3.0, each moved field still works by its old way if the new one
misbehaves.

## What does not change

Slots, the clock, relaying, the second frame, measurements and their records,
the movement score, the computer side's link files and replay. Runs from
before stay readable.
