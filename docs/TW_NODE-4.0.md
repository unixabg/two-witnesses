# tw_node 4.0: updates over the air

Status: built 2026-11-11. The transfer has been checked in the simulator.
The signing has been cross-checked: the kit's Python signs, and ESP-IDF's
mbedtls verifies with the board's own calls. The computer side has been
checked against a stand-in board. Real boards are next (the steps are below).

## What changes

1. **Two program slots.** The flash layout is `firmware/tw_node/partitions.csv`.
   - The settings area stays at 0xb000 (24 KB), so the board's number,
     group, settings, LED and thresholds survive.
   - A small "which slot" record (otadata, 0x11000) replaces the unused
     phy_init area, which moves to 0x13000.
   - Two 1.5 MB program slots sit at 0x20000 and 0x1a0000. The program is
     about 0.8 MB, and the build's 4 MB flash leaves room (the N16R8 has 16 MB).
   - The bootloader's rollback is on.
2. **One last flash by cable**, done by `flash_board.sh`. Its "all but the
   settings" option writes the bootloader, partition table and empty slot
   record, plus the program, around the settings. A 3.x board moves to the
   new layout and keeps everything it had. (The whole image is still a fresh
   start.)
3. **An update key on each board,** given over its cable:
   - `tw_setid.py PORT update-key` (or `flash_board.sh`) makes a P-256 key
     pair once, in the kit's `private/update-key.pem`. Each board learns only
     the public half, kept in its flash.
   - A board takes only programs signed with the matching private half.
   - A board with no key takes no update.
   - Each person running the kit has their own key, so nothing about it is in
     the published firmware.
4. **The update** (`tw_setid.py PORT update [board N|all] [FILE]`):
   - The program is the kit's image from 0x20000, or FILE. It is signed on the
     computer. The manifest is the program's SHA-256 and the signature over it.
   - It goes over USB into the cabled board's PSRAM, about 1.7 KB a line,
     two lines in flight at once (one after any retry: the board's USB driver
     drops what comes beyond the 4 KB it has not read). The cabled board
     checks it with its own key.
   - The cabled board sends it by radio to its group in short bursts:
     - **Frames:** the update frames are its own frames, marked `TU`. Older
       firmware and other groups drop them.
     - **Bursts:** at most 0.6 s each, a frame every 2.2 ms, the manifest
       first, with 1.6 s of normal running between bursts.
     - **Quiet while they come:** every board of the group keeps quiet for
       60 ms after each update frame.
     - **Reports and repair:** each board says once a second which chunks it
       has (a TWP record). The next bursts send what any of them still lacks.
   - Each board checks the whole program: its SHA-256, the signature with its
     own key, and that it is a tw_node for this chip. It then writes it to its
     other slot 4 KB at a time, says READY for 3 s and restarts into it. The
     cabled board goes last (if it was asked for).
5. **Proving the new program.** A board started from a new slot keeps the
   program once it has run a minute and hears a board of its group (or a
   computer reads it). Otherwise the bootloader goes back to the program
   before, after 10 minutes, or at once if it crashes or restarts first.
6. **No second time.** A board that restarted into an update says READY when
   it hears that update again, and takes it no more. A board that refused one
   (bad signature, no key) says so for a minute and takes no more of that
   update.

The protocol doesn't change for measuring: 4.0 boards and 3.x boards share a
network as before (protocol 3). A 3.x board takes no part in updates and
doesn't keep quiet for them, so its frames may collide with a burst.
docs/PROTOCOL-3.md has the frames and records.

## Lines for the computer

- **TWP** (each board's part, relayed like any record):
  `TWP,board,boot,record,update id (hex),state,chunks it has,of how many,manifest (1/0)`.
  The states are in tw_proto.h (TW_UP_ST_*):
  - 1 receiving
  - 2 complete (being checked)
  - 3 writing
  - 4 READY (restarting into it)
  - 5 sending (the cabled board)
  - 6 checked (the cabled board, not yet sent)
  - 10 no PSRAM
  - 11 no key
  - 12 damaged
  - 13 another key's signature
  - 14 not a tw_node program
  - 15 the flash write failed
- **TWS** gains, at the end: the update state, 1 while sending, update frames
  sent, bursts, and chunks still lacking somewhere.
- **TWH** (hello) gains, at the end: the program slot it runs (`ota_0` /
  `ota_1`), 1 while that program has still to prove itself, and the update
  key's fingerprint (the first 8 hex digits of its SHA-256; `none`).
- Answers to the computer: `TWOK upkey FP`, and `TWUP ready|ok|verified|sending|err ...`.

## Desk tests

**Simulator** (`sim --update T@TARGET:SIZE`). Each board with every chunk
checks them against the program sent, says READY and restarts 3 s later
(off for 4 s).

| case | the boards had it after | update frames | measuring |
|---|---|---|---|
| 3 boards, 820 KB | 6-8 s, the cabled board last | 708 (641 chunks) | 99.3% PASS |
| 20% loss | 10-13 s | 1,112 | 95.7% PASS |
| 6 boards, 5% loss | 7-9 s | 832 | 99.1% PASS |
| 8 boards, 1.5 MB | 13-15 s | 1,549 | 92.7% PASS |
| board 2 only | 6 s; the others untouched | 683 | 99.8% PASS |
| the cabled board only | at once (no radio) | 0 | |
| board 3 off from 3 s in, for minutes | the others in 9 s; the sender stopped waiting for 3 when it went quiet | 948 | 99.5% PASS |
| board 3 off for 4 s mid-update (restarts, empty) | the others 6 s; board 3 got it all again, 15 s | 1,405 | 96.1% PASS |
| a second group, its own channel or the same | only its own group took it | 850 | 99.2% PASS |

Without an update the simulator's records are byte-for-byte those of 3.5,
and the timing cases are unchanged.

**Signing:** keys and signatures made by `tw_update.py` (Python's
cryptography) were verified with ESP-IDF's mbedtls, built for the computer
and called exactly as the board calls it. Three signatures with the key
verified. Another key's signature was refused, and one changed byte was
refused as damaged.

**Computer side** against a stand-in 4.0 board: `update-key` (made, given,
shown), and `update` to every board, one board, and the cabled board alone.
Also checked: an older board in the group (named, left out), a board with
another key (named, the others updated), and a cabled board with no key
(refused before sending). The USB upload took about 1 s over the stand-in's
link; on a real board expect 10-30 s.

## On the boards: the first time

1. **Move every board to 4.0 by cable,** one at a time:
   `~/tw-kit/flash_board.sh`, answer **p** ("all but its settings"), and yes
   to "Give it your update key". The first board makes the key:
   **copy `~/tw-kit/private/update-key.pem` somewhere safe.**
   `tw_setid.py /dev/ttyACM0` should say, for the cabled board, `program slot
   ota_0, update key xxxxxxxx`.
2. **Check:** `tw_setid.py /dev/ttyACM0 check`. Every board should show
   tw_node 4.0 and the same settings as before.
3. **A first update to one board:** with a second 4.0 build (a test program
   file; the same version with another build time):
   `tw_setid.py /dev/ttyACM0 update board 2 FILE`. It should say board 2
   "runs" the test build. A minute later `tw_setid.py /dev/ttyACM0` shows it
   for board 2.
4. **To every board:** `tw_setid.py /dev/ttyACM0 update board all FILE`. Board
   1 (on the cable) goes last, and its port is gone for a few seconds while it
   restarts. All should say they run the test build. Look at a short run's
   replay to see that measuring went on.
5. **Rollback** (the safety net):
   - Update board 2 back to the cable-flashed build:
     `update board 2 tw_node-s3-ch11.bin`.
   - The moment it says board 2 is "restarting into it", switch boards 1 and
     3 off (unplug board 1's cable last, after it says so).
   - Board 2 then hears no one. Leave it 12 minutes, then switch the others
     on.
   - `tw_setid.py /dev/ttyACM0` should show board 2 back on the test build
     (it went back by itself), in the other slot.
6. **Refusals:**
   - Give board 3 another key over its cable (`update-key none`, which leaves
     it with none), then `update board 3 FILE`. It should say "no update key"
     and keep running.
   - Give it your key again with `update-key` on its cable.
