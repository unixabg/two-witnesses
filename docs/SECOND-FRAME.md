# A second frame in a board's own slot (tw_node 2.1): design

Status: built 2026-11-06 (firmware, simulator, computer side); simulator
results below; the desk test and the night tests on real boards still to run.

## Why

A frame carries at most 1400 bytes: 6 full records (28-byte header + 192
amplitudes). With 6 boards every frame brings 5 new records of its own, so
one place is left for second copies and relayed records. The simulator
(docs/REFERENCE.md): 97.6% of reports home when healthy instead of 99.9%,
one stranded board recovered (96%) but two at once only partly (83% and 53%).
A slot is 3 ms and a full frame takes under 1 ms on the air (HT40 MCS0, 13.5
Mbit/s), so there is room in the slot for a second frame.

## What a board does

1. **When.** Right after its normal frame has gone out (ESP-NOW's send-done
   callback), a board sends a second frame ("extra frame") in the same slot if
   all of these hold:
   - records are still waiting that did not fit: its own new ones, relayed
     ones, or second copies of records sent in an *earlier* slot (not copies
     of what just went out: a copy 1 ms later protects less than one a frame
     later);
   - it is in step with the timekeeper and its slot fits in the period (not in
     the random sends of a board that has just started);
   - it is not an exit (a board with a computer prints its records itself;
     carrying them on would only use air), unless the test setting forces it;
   - the second frame setting is not off (it is on by default).
2. **How long.** The extra frame must end before the next board's slot starts,
   keeping 200 us of the next board's 400 us guard for clock error: deadline =
   own slot start + 3000 + 200 us, on the timekeeper's clock. Its length is cut
   to what fits by then, counting 40 us of preamble, the MAC header, 13.5
   Mbit/s and 150 us of channel access. If not even one waiting record fits,
   it is not sent (counted as "skipped"). At 6 boards a full first frame ends
   about 1.6 ms into the slot, which leaves room for a full second one.
3. **What it says.** The same header as the normal frame, with a new flag
   (`TW_F_EXTRA`, 0x08) and **the same frame number**. The normal frame gets a
   flag of its own when an extra one should follow (`TW_F_EXTRA_NEXT`, 0x10),
   so a receiver can count extra frames it missed.
4. **Receivers.**
   - The records in an extra frame are handled exactly as in a normal one:
     the exit prints them (de-duplicated as always), relays carry them on.
   - The header changes nothing else: an extra frame does not count toward how
     well the sender is heard, the clock, or settings. Hearing (and with it
     the relay choices) stays as it is now.
   - No measurement is made from it, by default: each link is still measured
     once per period, so recordings stay comparable with every night so far,
     and the extra frame adds no records to carry. (Test setting below.)
5. **Mixing with 2.0 boards.** The protocol version stays 2. A 2.0 board takes
   an extra frame as a normal one: its records are used (good); hearing and
   the clock are unaffected (same frame number: the gap is 0, which both
   ignore); but it makes a measurement from it, so for a while its links have
   two records with the same frame number. Better to reflash all boards
   together, which is the plan anyway.

## The radio's error flag (rx_state)

In the same release: a CSI measurement whose receive info says `rx_state != 0`
(the radio reports the frame as received with an error; the numbers are not
documented) gets a record flag (`TW_KIND_RXERR`, 0x40). Nothing is dropped.
The record format does not change (the flag uses a free bit of `kind`, which
the computer side only reads for 0x80). `split_links.py` writes it into the
`rx_state` field of the CSI_DATA line (0 or 1) and counts it per link in
`summary.txt`. Deciding whether to leave such records out comes later, from
what they line up with.

## Test settings

Network settings, spread to every board like the transmit power and kept in
flash (they ride in the `set_flags` byte, so 2.0 boards carry them on
unchanged):

| `tw_setid.py PORT ...` | bit | what it does |
|---|---|---|
| `second off` / `second on` | 0x02 set / clear | no extra frames / extra frames when records wait (default) |
| `second force` | 0x04 | every board (exits too) sends an extra frame in every slot, and both its frames are padded to the full length: 6 boards' airtime with 3 boards |
| `measure-second on` / `off` | 0x08 | receivers also measure extra frames; those records have flag 0x20 (`TW_KIND_EXTRA`) and are kept out of the link files (counted in `summary.txt`), for `second_check.py` |
| `cap N` / `cap off` | 0xF0 (N = 1..15) | at most N records per frame: 3 boards crowded like 6 (N = 2) |

`tw_setid.py PORT` shows them for every board. A run's TWS/TWN lines carry
them, so the run records what it used. **Put everything back** (`second on`,
`measure-second off`, `cap off`) after a test: they survive restarts.

## What the boards report

- **The cabled board, about each board it hears** (TWN, once a second, new
  fields at the end): frames heard, extra frames announced (flag 0x10), extra
  frames heard, CSI measurements on extra frames (the radio gave CSI for both
  frames 1 ms apart?), the latest end of an extra frame in that second (us
  after the sender's slot start, by the cabled board's clock), and its own
  measurements of that board flagged rx_state. The setting byte is added too.
- **The cabled board itself** (TWF, every 10 s, new `key=value` fields): extra
  frames sent, skipped for lack of time, send errors, the latest end of its
  own extra frames (from ESP-NOW's send-done), and rx_state-flagged
  measurements in total. TWS adds the setting flags and the extra frames
  sent and skipped.
- `second_check.py RUN_FOLDER` (new) reads a run's raw lines and says, per
  board: extra frames announced / heard / with CSI, the latest end against
  the deadline, and (with `measure-second on`) how the CSI of extra frames
  compares with that of the normal frame 1 ms earlier: RSSI and shape, per
  link.

## Tests

1. **Desk, 2 boards** (board 1 cabled, board 2 at 1-2 m), `second force` and
   `measure-second on`, 10 minutes. Answers: does the cabled board hear
   board 2's extra frames, does the radio give CSI for both frames 1 ms apart,
   is it as good, and where in the slot do they end. Then the same with board
   1 forced too (`second_check.py` reports its own sending).
2. **Three boards, crowded**: `cap 2`, one night with `second off` and one
   with `second on`, everything else the same. That is the six-board question
   with three boards; compare delivery per link (`links/summary.txt`).
3. **Three boards, normal** (`second on`, no cap): should be exactly as now
   (no extra frames: everything fits), apart from the rx_state counts.

## Simulator (firmware/sim, the same tw_proto.c)

120 s, 3% random loss everywhere, seed 7, as the table in docs/REFERENCE.md
(the "off" column reproduces it). Reports of each measuring board that
reached the computer; extra frames go on the air 50 us after the frame
before ends, plus up to 150 us of channel access, and collide like any frame.

| case | second off | second on | airtime off / on |
|---|---|---|---|
| 3 boards, healthy | 99.9% | 99.9% (no extra frames: all fits) | 8.8% / 8.8% |
| 6 boards, healthy | 97.5-97.8% | 99.9% | 26% / 40% |
| 6: board 1 misses 75% of board 2, relay off | board 2: 52.6% | 62.6% | 26% / 40% |
| same, relay on | board 2: 96.1% | 98.5% | 26% / 41% |
| 6: board 1 hears nothing of board 2, relay on | board 2: 92.9% | 96.3% | 25% / 41% |
| 6: boards 2 and 3 both 75% shadowed from board 1, relay on | 82.8% / 53.5% | 98.6% / 98.6% | 26% / 43% |
| 6: board 1 misses 25% of every board | 73-75% | 76-84% | 24% / 28% |
| 6: board 1 misses 40% of every board | 59-64% | 69-82% | 26% / 37% |
| same 40%, board 4 cabled instead | 97.4-97.7% | 99.6-99.9% | 26% / 36% |
| 16 boards (compact records, one copy) | 96.4-97.3% | 96.5-97.5% (11 extra frames) | 26% / 26% |
| 3 boards, `cap 2` | 97.1-97.3% | 99.8-99.9% | 5% / 8% |
| 3 boards, `cap 3` | 98.6-98.7% | 99.8-99.9% | 7% / 9% |
| 3 boards, `second force` | | 99.9% | 27% |
| 6 boards, `second force` | | 99.8-99.9% | 53% |

- No extra frame ended later than 2834 us into its slot (by its sender's
  clock; the limit is 3200) in any case; none was late. At 6 boards healthy:
  2695 us. Collisions: 0-2 per 120 s involving an extra frame (forced, 6
  boards: 2 of 72 000 frames).
- Two stranded boards at once, the case relaying could not carry (83% / 53%),
  now both get 98.6% home: the relays have a second frame's room.
- A cabled board that misses some of everyone gains less (25%: 74% -> 80%):
  the extra frame carries second copies, but a copy is lost as often as the
  first. Cabling the board that hears best is still the fix (99.6-99.9%).
- 16 boards: records are compact and sent once above 6 boards, so they fit
  and almost no extra frames go: unchanged, as it should be.
- `cap 2` crowds 3 boards the way 6 are crowded (97.1% without the second
  frame, as 6 boards' 97.6%): that is the setting for the night test.

**Seen on the way (not changed here):** a follower sets its clock from the
timekeeper's frames assuming 150 us on the air (`AIRTIME_US`), but a long
frame takes up to 890 us, so every follower runs a fixed few hundred us
behind the timekeeper (about 700 us when its frames are full length). All
followers lag alike, so neighbouring slots stay apart, which is why nothing
collides; it eats into the gap only where the period is tight (above 6
boards, last board before the timekeeper's slot). Counting each frame's own
length into the clock estimate would remove it: a separate change, after
these tests. For the same reason the cabled board's measure of where another
board's extra frame ended would include that lag, so the boards report two
other things instead: each sender says where its last extra frame ended by
its own clock (header byte 23, which 2.0 boards send as 0 and ignore), and
the cabled board measures the gap from a board's normal frame to its extra
frame, which needs no clock.
