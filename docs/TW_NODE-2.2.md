# tw_node 2.2: design

Status: design, 2026-11-07, agreed: these six, in this order, flash staying
4 MB. Each commit that follows builds one of them.

Six changes, one commit each, in this order, then the version and image.
The frame and record formats stay those of 2.1 (protocol version 2): 2.1 and
2.2 boards hear each other. Upgrade: the program part only, as from 2.0 to 2.1
(FLASH-README); the bootloader and partition table are unchanged (checked:
only build stamps differ). Flash stays 4 MB: 16 MB would change the bootloader
header and the partition table, so a full image, and nothing needs the room yet.

## 1. Every board says which firmware it runs

Every 5 s each board queues a small record of a new kind, `TW_KIND_INFO` (2),
with its firmware and build ("tw_node 2.2|Nov  7 2026 10:00:00", at most 64
bytes, no factory address: not needed, and kept off the air). It travels like
any record: second copy, relayed if needed. Its record number has the top bit
set and a counter of its own, so the computer's delivery counts (which follow
the measurement record numbers) do not see a gap.

The cabled board prints it as `TWV,board,boot,firmware,build` (its own too).
`tw_setid.py PORT` shows each board's firmware; `split_links.py` lists it per
board in `links/summary.txt`; `replay.py` in "About this recording". A board
that has not said (2.1 or older) shows "firmware not reported".

## 2. The followers' clock lag

A follower sets its clock from the timekeeper's frames: "the frame says the
timekeeper's clock read T when it was built; I received it at my time t; add
150 us for the air". A full frame is on the air for up to 890 us, and t is
taken when it has been received, at its end, so every follower ran up to about
700 us behind (docs/SECOND-FRAME.md). Now the candidate adds the frame's own
air time, `tw_air_us(len)`, from its length. What remains is the time before a
frame gets on the air (queueing, channel access) and the receiver's driver;
keeping the largest candidate, as now, keeps the smallest of those.

Side effect to know: the lag gave the timekeeper's late second frames room
before the next board's slot (its slot is first). Without it the next board
starts up to 700 us earlier, closer to them. At 3 boards second frames hardly
ever go (17 in a night), so it matters only in `second force` tests, until 3
has shown where their time goes and the timing is fixed.

## 3. Where a second frame's time goes

The 2.1 tests: second frames end about 450 us past their limit, and the gap
after the normal frame is 2-3 ms instead of about 1.1 ms. The timestamps came
from callbacks, so real delay and late callbacks could not be told apart. The
cabled board now times every frame of its own (in `second force` it sends
second frames too), in four parts:
- `tx_at`: where in its slot it handed the normal frame to ESP-NOW (us after
  the slot start; the target is 400);
- `tx_radio`: from that hand-off to ESP-NOW's send-done for it (air time,
  channel access, driver);
- `x_wake`: from that send-done to handing the second frame over (our task:
  the callback, the queue, building the frame);
- `x_radio`: from that hand-off to the second frame's send-done.
Each as mean/max over 10 s in the TWF line, with the frames' mean lengths
(`tx_len`, `x_len`) so the air time is known (`tw_air_us`). `second_check.py`
prints them against the air time: what is left over is driver and channel,
or our task.

Also: a board stops repeating where its last second frame ended once it has
sent none for a second (header byte 23 back to 0), so the end shown is never
stale (the 2.1 report needed a computer-side guard for that).

## 4. How busy each core is, and memory

FreeRTOS counts how long each core is idle (run-time statistics, on
`esp_timer`: a timer read per task switch, well under 1% of the CPU). Every
10 s the TWF line adds `core0=` and `core1=` (percent busy over those 10 s;
Wi-Fi runs on core 0, our work task on core 1), and `heap_min=` (the lowest
free internal memory since start, bytes). `tw_setid.py` and `second_check.py`
leave them alone; the dashboard's health line shows them.

## 5. PSRAM

The boards are ESP32-S3-WROOM-1 N16R8: 8 MB of octal PSRAM. It is turned on,
with "start without it if it is not there" (boards 4-6 may be another
variant). Nothing uses it yet: Wi-Fi, our queues and buffers stay in internal
memory as now. The hello line (TWH) adds `psram=` (bytes, 0 if none) and the
TWF line `psram_free=`. What to use it for is its own design (buffering a
board cut off for a while is first in line).

## 6. A movement score made on the board

Each board computes, for each board it hears, the same "change" the computer
computes (replay.py, dash.py): per record the amplitudes in dB, in bands of 4
subcarriers (the bins live in more than half of the link's first 50 records),
each record's own level taken out; per second, each band's spread; divided by
the band's quiet level, averaged over the bands. The quiet level is the
band's running median of those per-second spreads (a slow step towards each
new second: 2% of the level), started from the first 30 seconds. Only normal
frames count (not second frames).

Once a second each board queues a record of kind `TW_KIND_SCORE` (3): 16
bytes, the score of each board it heard that second (x10, 1..255; 0 = none),
plus its uptime and the timekeeper's clock. The cabled board prints
`TWM,board,boot,uptime_s,net_us,s1,...,s16`. `split_links.py` writes them to
`links/board_score.csv` (one row a second per link) and `score_check.py RUN`
(new) compares them with the computer's change for the same seconds: how
closely they follow each other, and whether the loud seconds are the same.
Nothing uses the score yet: it is there to be checked against the computer,
before anything depends on it.

Cost: about 600 bytes of internal memory per board heard, one record a second
(44 bytes), and the arithmetic of the dB conversion (a few us a record).

## Tests (real boards)

1. Upgrade all three (program part only); `tw_setid.py PORT`: every board
   says tw_node 2.2 and its build, number and 11 dBm kept.
2. 10 minutes `second force` (measure-second off): `second_check.py` shows
   where the cabled board's frame time goes. Then `second on`.
3. A normal night: `core0`/`core1`/`heap_min`/`psram_free` in TWF (the
   dashboard shows them), `score_check.py` on the night.
