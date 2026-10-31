# Flashing tw_node: one firmware for every board

All boards are equal and run the same image, `tw_node-s3-ch11.bin`. Every board
both sends and listens; each frame it sends is the sounding packet the others
measure, and it carries the board's own measurements of the others. The board
cabled to a computer prints everything (its own measurements and those relayed
to it), so the other boards need only a power bank.

```
76265f8a9d8f8e486e42c53a2e3850b406d4669874fea4c9e2a04780c4f011a1  tw_node-s3-ch11.bin
```
Firmware tw_node 2.0: relaying (below) and the transmit power setting (1.1).
**Every board must run 2.0**: the frame format changed, and 2.0 boards and
1.x boards do not hear each other. Built from Espressif's esp-csi examples (csi_send / csi_recv at commit
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
| relaying | on by default: when the cabled board hears a board badly (a car between them), the boards that hear both carry its records too, in room left in the frames they send anyway; `tw_setid.py PORT relay off` / `relay on` for every board |
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
100%. **Not tested: real radios.** The first flash is the first real test.

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
# -> board 16, factory address 30:30:f9:xx:xx:xx, firmware tw_node 2.0
```
A new board says it is board 16 and needs a number:
`~/scratch-esp/bin/python tw_setid.py /dev/ttyACM0 1` (then 2, 3, ... for the
others, one at a time on the cable). Every board needs its own.

Flashing as above (erase-flash, or writing the whole image) clears what the
board kept in flash: its number and its settings. Set the number again after
every reflash; the settings come back by themselves from the other boards if they
still have them (or set them again). `tw_setid.py` tries up to three times and
says "done" only when the board reports the new number.

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
The setting travels in every frame; each board takes it within a second and
keeps it across restarts. If two computers set different powers at the same
moment, the lower-numbered board's setting wins. The dashboard shows each
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
