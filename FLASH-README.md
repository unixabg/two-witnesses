# Flashing tw_node: one firmware for every board

All boards are equal and run the same image, `tw_node-s3-ch11.bin`. Every board
both sends and listens; each frame it sends is the sounding packet the others
measure, and it carries the board's own measurements of the others. The board
cabled to a computer prints everything (its own measurements and those relayed
to it), so the other boards need only a power bank.

```
7ccbc4e86519cb0867a32712635bd1db6874567bc5e492dbe3134424429cfa53  tw_node-s3-ch11.bin
```
Firmware tw_node 3.1: the channel survey (`tw_setid.py PORT survey`: who else
is on the air, per channel and board). 3.0 and 3.1 boards work together (the
same protocol), but only 3.1 boards survey: update them all, program part
only, one at a time, as below.
3.0: protocol 3 (docs/PROTOCOL-3.md): a leaner frame header,
and the network settings (power, relay, second frame, ...) travel only as
settings by record, which every board says every 5 s. **3.0 boards do not hear
2.x boards: flash every board to 3.0 together** (below; back to 2.7 the same
way). 2.7: each board's second-frame end travels in its own record
too (`net_check.py RUN` compares). 2.6: the network settings (power, relay,
second frame, ...) travel both ways, in the header as before and on protocol 3's transport, and
the boards count any disagreement (`net_check.py RUN`). 2.5.1: settings and
commands by record (protocol 3's transport, docs/PROTOCOL-3.md), so a board's LED can be set, tested or tried
over the air, for one board or all (below; 2.5: settings by record). 2.4: a board's RGB LED can show its own movement score in the
replay's colors (off unless set: below; 2.4.1: its blues pale to deep, as the
replay's light theme; 2.4.2: led try; 2.4.3: movement only, no orange). tw_node 2.3: frames go out on time (built ahead), the second frame
goes from the send-done itself, and the followers' clocks are set from the
radio's own receive time, so they no longer run 1.1-1.5 ms behind the
timekeeper's (below; docs/TW_NODE-2.3.md); every board reports its firmware,
timing, load and memory, PSRAM is on, each board makes a movement score
(2.2); the second frame and the radio's error flag (2.1), relaying (2.0) and
the transmit power setting (1.1). 2.0 to 2.7 boards hear each other (the
frame format is the same), but **run every board on the same firmware**. 1.x boards do not
hear 2.x boards, and 2.x boards do not hear 3.x boards. Built from Espressif's esp-csi examples (csi_send / csi_recv at commit
8633d67, the same as the earlier images) with ESP-IDF v5.5.6: same channel
(11, 40 MHz, secondary below), same ESP-NOW rate (HT40 MCS0), same CSI
settings and gain compensation as the v2 receiver, so its measurements stay
comparable with the recordings so far. Source: `firmware/tw_node/`.

## What it does

| | |
|---|---|
| board number | 1-16, set once per board with `tw_setid.py` (kept in the board's flash); until then a board says it is board 16 |
| address on the air | `1a:00:00:00:00:NN` (NN = board number), so every measurement says who sent the frame |
| time slots | board N sends in its own 3 ms slot, (N-1) x 3 ms into each period; the period is 20 ms (50 frames/s per board) up to board 6, then 3 ms per board (48 ms with 16) |
| timekeeper | the lowest-numbered board heard in the last 2 s; if it goes quiet, the next takes over within about 2 s |
| transmit power | full (20 dBm) until set; `tw_setid.py PORT power 11` sets every board at once (2-20 dBm) through the board on the cable; each board keeps it in flash, and one that was off picks it up when it comes back |
| relaying | on by default: when the cabled board hears a board badly (a car between them), the boards that hear both carry its records too, in room left in their own frames; `tw_setid.py PORT relay off` / `relay on` for every board |
| second frame (2.1) | on by default: when a board's frame is full and records still wait (second copies, relayed records), it sends a second frame right after, in its own slot, ending before the next board's slot; same frame number, records only: nothing is measured from it, so each link is still measured once per period. The cabled board sends none. With 3 boards everything fits, so it hardly ever goes. `tw_setid.py PORT second off` / `second on`; tests: `second force`, `measure-second on`, `cap N` (docs/SECOND-FRAME.md) |
| radio's error flag (2.1) | a measurement of a frame the radio says it received with an error (`rx_state`) is kept and marked; `split_links.py` counts them per link |
| firmware report (2.2) | every 5 s each board sends its firmware and build (TWV); `tw_setid.py PORT` shows it for every board |
| load, memory, PSRAM (2.2) | the cabled board's TWF line says how busy each core was (`core0=`, `core1=` %), the lowest free memory (`heap_min=`) and free PSRAM (`psram_free=`); its hello says the PSRAM found (8 MB on the N16R8) |
| second frame timing (2.2) | the cabled board times its own frames (`tx_at`, `tx_radio`, `x_wake`, `x_radio` in TWF); `second_check.py` explains them (run with `second force`) |
| network settings both ways (2.6) | nothing to do: `power`, `relay`, `second`, `measure-second`, `cap` work as before and also travel on the transport, where every board says them every 5 s. `tw_setid.py PORT` says per board "transport agrees"; `net_check.py RUN` compares them over a run; TWF counts seconds they disagreed (`net_diff`) |
| settings by record (2.5, 2.5.1) | `tw_setid.py PORT led on [GPIO] board N` (or `board all`; also `led off`, `led bright N`, and 2.5.1 `led test`, `led try R G B`): the board on the cable gives it, every board passes it on for 60 s, the named board(s) take it and report it (2.5.1: within a second or two; through relays a few more); tw_setid waits for that and names any board that did not (off, out of reach). `tw_setid.py PORT` shows every board's LED. The run's TWX lines are these records |
| RGB LED (2.4) | per board, kept in its flash; over the cable: `tw_setid.py PORT led on` (GPIO 48; `led on N` for another pin), `led off` (the default), `led bright 1-255` (32), `led test` (7 s: red, green, blue, three blues, off), `led try R G B` (that color for 10 s, to choose by eye). Shows the board's own movement score on the loudest link it measures: off when quiet, faint pale blue to full deep blue as the change grows (the replay's light theme). Movement only: the replay's orange (a link's shape standing apart from its usual) is not made on the board On boards like this setup's, an "RGB" solder pad by the LED may need bridging: `led test` shows it |
| frame timing (2.3) | a board builds its frame 800 us ahead and hands it over at its slot time; its second frame is built while the first is on the air and sent from the send-done (dropped, records kept, if it would end late); followers time the timekeeper's frames by the radio's receive time. TWF adds `build`, `rxcb`, `rx_k`, `x_cb`, `x_task`, `x_refused`, `extra_late`, `ovh`; `second_check.py` and `lag_check.py` explain them |
| movement score (2.2) | once a second each board scores how much each link it measures moved, as the computer's "change" (TWM); `score_check.py` compares the two. Nothing uses it yet |
| boards | up to 16; up to 6 every measurement keeps all 192 amplitudes and goes out twice |
| exit | any board with a computer reading its native USB port; plug a second board into a second computer for a hot standby |
| records | each measurement goes out in two frames (redundancy) while there are 6 boards or fewer; above that, one copy, and records shrink to the 52 L-LTF subcarriers (as esp-radar uses) to keep the air under ~25% |
| de-duplication | records carry (measuring board, boot number, record number) and a hop count (0 today; ready for multi-hop outdoors) |

Desk-tested in a simulator running the same protocol code (`firmware/sim/`):
3 boards 99.5-100% of measurements reach the computer at 5% random loss;
the timekeeper dying for 20 s and coming back 99.6%; 8 boards 94%; 16 boards
93.5% at 25% airtime; 30% loss 89%; a 25-minute run across the 71-minute clock
wrap 99.8%; boards switched on late are heard and given slots within about a
second; with a second listener on another board the two computers together had
100%. 2.1, the same simulator: 6 boards 97.6% -> 99.9%, two boards stranded at
once 83% / 53% -> 98.6% each, airtime 26% -> 40%, every second frame within its
slot; 3 boards unchanged (docs/SECOND-FRAME.md); 2.1 has run on the real boards
since 2026-11-06, its second frames heard and measured as well as the first
but ending about 450 us late. 2.2, the same simulator: the followers' clocks
within 6 us of the timekeeper's (were 360-740 us behind), every published
case as before, the movement score about 10 during a walk across a link and
1.0 elsewhere; 2.2 has run on the real boards since 2026-11-07. 2.3, the same
simulator with the delays the 2.2 boards measured: followers within 50-65 us
of the timekeeper (2.2: 1.1-1.5 ms on the boards), second frames past their limit 3% of the
time with `second force` (2.2: 92%), 8 boards 1 collision in 120 s (2.2: 150).
2.3 has run on all three boards since 2026-11-07 (followers 14-76 us from the
timekeeper). 2.4's LED ran on board 1 on 2026-11-08 (on, test and detection
worked; 2.4.1 turns its blue ramp the replay's usual way round).

## Flash (each board, once)

One image for every board. (For the record: the earlier two-board firmware (one sender, one receiver) is kept in release
v5.3: download it from the v5.3 tag, or in a clone
`git checkout v5.3 -- csi_send-s3-ch11.bin csi_recv-s3-ch11-v2.bin`; its guide:
`git show v5.3:FLASH-README.md`.)

```
cd ~/tw-kit
sha256sum -c --ignore-missing SHA256SUMS          # tw_node-s3-ch11.bin: OK
```
Board on the native USB port, in download mode (hold BOOT, tap RST, release BOOT):
```
~/scratch-esp/bin/esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
~/scratch-esp/bin/esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x0 tw_node-s3-ch11.bin
```
Tap RST. Then check who it is (it says hello every 10 s):
```
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0
# -> board 16, factory address 30:30:f9:xx:xx:xx, firmware tw_node 3.1, PSRAM 8 MB, LED off
```
A new board says it is board 16 and needs a number:
`~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 1` (then 2, 3, ... for the
others, one at a time on the cable). Every board needs its own.

Flashing as above (erase-flash, or writing the whole image) clears what the
board kept in flash: its number and its settings. Set the number again after
every reflash; the settings come back by themselves from the other boards if they
still have them (or set them again).

**Upgrading a board that already runs tw_node 2.0 or later, keeping its number
and settings:** write only the program part of the image (from 0x20000; the
bootloader and partition table are the same as 2.0's to 2.7's):
```
dd if=tw_node-s3-ch11.bin of=/tmp/tw_node-app.bin bs=4096 skip=32
~/scratch-esp/bin/esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0x20000 /tmp/tw_node-app.bin
```
(no erase-flash). Tap RST; `tw_setid.py /dev/ttyACM0` should say
`firmware tw_node 3.1` with the board's old number (and, once all are done,
every board's firmware). If it says board 16, write
the whole image as above and set the number again. `tw_setid.py` tries up to three times and
says "done" only when the board reports the new number.

**2.x to 3.0: every board, one after the other, before a run.** A 3.0 board
and a 2.x board ignore each other's frames, so until the last one is done the
boards are two groups (each with its own timekeeper; the cabled board hears
only its own group). Each keeps its number, power, relay and second-frame
setting and LED through the program-only upgrade. When all are on 3.0,
`tw_setid.py /dev/ttyACM0` should list every board with the same setting.
Boards 4-6, new, with an empty flash: the whole image, a number, and they take
the network's settings from the others within a few seconds (a new board
never undoes them). **Back to 2.7** the same way, program part only, every
board, with 2.7's image (`git show 2afee59:tw_node-s3-ch11.bin > tw_node-2.7.bin`
in the kit's folder, then the `dd` above on that file); 2.7 reads the
settings 3.0 kept.

## Transmit power and relaying

With every board running 1.1 and the cabled one on the computer (stop
`live.sh` or a run first: only one program can use the port):
```
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 power 11        # 11 dBm, every board
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 power default   # back to 20 dBm
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 relay off       # relaying off, every board
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 relay on        # back on (the default)
~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0                 # who is it; every board's settings
```
The setting travels by record (3.0; before, in every frame); each board takes
it within a second or two and keeps it across restarts, and a board that was
off takes it within a few seconds of coming back. If two computers set
different powers at the same moment, the higher power wins on every board (2.x:
the lower-numbered board's setting). The dashboard shows each
board's power (`tx 11 dBm`), `relay OFF` when relaying is off, and `heard 25%`
when the cabled board hears a board badly; each run's `links/summary.txt`
records the settings, and how many records came home through a relay. Change
them between runs, not during one.

## First test, on the bench (10 minutes)

1. Board 1 on the computer's native USB port. Boards 2 and 3 on
   power banks (or chargers), a metre or two away.
2. `~/tw-kit/survey.sh` - the dashboard should show, within a few seconds:
   `exit board 1   timekeeper board 1   period 20 ms`, boards 2 and 3 `ok`,
   and six links (`1→2 1→3 2→1 2→3 3→1 3→2`) at about 45-50 packets/s each.
3. The health line at the bottom shows the board's `frames=` count. It must
   grow. If `csi=` grows but `frames=` stays 0, measurements are made but the
   relayed ones do not arrive (only links ending at board 1 would show): tell
   me, the fix is known.
4. Unplug board 2's power bank: its four links say NO DATA within 5 s; plug it
   back: they return. Unplug board 1 from the computer and plug board 3 in
   instead: board 3 becomes the exit and prints everything.

Things I could not check here and you will see at once: whether the power
banks switch themselves off (an ESP32-S3 draws about 100 mA; some banks cut
out below that), and the real loss and RSSI between the boards.

## Then

- Place the boards (walk them with `survey.sh` and the `m` key), then a quiet
  run and a watch day as before. `watch.py` judges every link and names the
  one that moved; `view.py` and `radar_replay.py` work per link after
  `split_links.py RUN_FOLDER --view`.
- To go back to two boards (one sender, one receiver): the earlier two-board firmware (one sender, one receiver) is kept in release
  v5.3: download it from the v5.3 tag, or in a clone
  `git checkout v5.3 -- csi_send-s3-ch11.bin csi_recv-s3-ch11-v2.bin`; its guide:
  `git show v5.3:FLASH-README.md`.
