# Modules: two-witnesses as a carrier platform

Status: on the next branch (v5.7-next). Stages 1-3 below are done (stage 3
waits for its night on real boards); nothing here is in a released image yet.
Each stage says what it changes and how it is checked.

## The idea

Today the boards are one program: a mesh (slots, a timekeeper, relaying,
settings, updates over the air) and one use of it (CSI sensing: measurements,
movement scores, detections). The mesh is useful by itself. Split it:

- **the core**: the mesh, the radio, and CSI *receiving* for the health of
  the links (always built);
- **modules**: what a board does with the mesh (CSI sensing is the first;
  a sound level from an ICS-43434 is the second; anyone can add one).

A build chooses its modules in a menu, as the Linux kernel's does: ESP-IDF
already uses the kernel's Kconfig language (`idf.py menuconfig`), so each
module brings a `Kconfig` file and appears in the menu by itself.

```
make csi_defconfig     # today's board: the core + CSI sensing
make menuconfig        # choose modules and their options
make bin               # build the image
```

## What stays in the core, and why CSI receiving does

The core is everything a board needs to be part of a group, whatever it
measures:

- slots, the timekeeper, hearing, relaying, de-duplication, the second frame;
- settings and actions (keys), updates over the air (signed, with rollback);
- the radio: channel (1-11), transmit power (2-20 dBm), the regulatory
  country (US, 4.4). **Modules never touch the radio**: every build keeps
  these limits;
- the LED: its pin and brightness, the test sequence and colors to try (a
  module gives the color it shows the rest of the time);
- the channel survey (it needs promiscuous mode);
- **CSI receiving, for link health.** CSI is measured on frames a board
  receives anyway: it sends nothing, so it costs no airtime and changes
  nothing about emissions. The core needs it:
  - the clock: the offset between the radio's receive stamp and the board's
    clock is learned from CSI callbacks (2.3, `rxk_note`); it keeps every
    slot tight;
  - each link's RSSI (`TWN` takes it from the CSI path today), noise floor,
    gain (AGC, FFT: a saturated link at 20 dBm, or one near its noise), and
    the radio's error flags (`rx_state`, first word invalid).

  Every build measures CSI for this; a menu option to turn it off (the clock
  learning its offset from the ESP-NOW callback instead, about 0.4 ms later
  and less steady) would come only with a timing test that shows it holds.

What a board sends of its CSI is the CSI sensing module's: the amplitude
records (`TWR`), which fill most of a frame, and everything worked out from
them. A build without the module measures CSI for health and sends none of it.

## The CSI sensing module (module 1)

Owns, from today's firmware:

- the measurement records (kinds 0 and 1, `TWR`), full or compact;
- the movement score, detections (3.4), steadier than quiet (3.5), relearning
  quiet, and the LED's white detection flash and green steadier mark;
- the keys `detect` (6), `relearn` (7) and `detect low` (8): they keep their
  numbers, now owned by module 1 in the table below.

The once-a-second core record (kind 3, `TWM`) carries both core fields (the
board's uptime, where its last second frame ended) and the module's (scores,
detections, steadier marks). It keeps its layout on the air: the core builds
it, and the CSI module fills its fields through a hook; a build without the
module sends zeros there (score 0 already means "none"). No protocol change.

## A module, in one folder

```
modules/csi/
  module.yml        what it is (below): checked at build time
  tw_mod_csi/       an ESP-IDF component (its folder name is its component name,
                    so each module's must differ: tw_mod_<name>)
    Kconfig         its menu: "config TW_MOD_CSI" and its options
    CMakeLists.txt  compiles its sources only when CONFIG_TW_MOD_CSI=y
    tw_mod_csi.c    the board side (ESP-IDF glue)
    csi_sense.c     the portable part (no ESP-IDF): also built into the desk
                    simulator, as tw_proto.c is
  host/             a Python plug-in for the computer
    decode.py       its lines -> values
    dash_view.py    optional: its own dashboard view (CSI: the c view, the
                    map's motion lines)
    replay.py       optional: its own replay pages
  README.md         what it measures, its wiring, its privacy note
```

The firmware build finds modules by itself
(`EXTRA_COMPONENT_DIRS` = every `modules/*/tw_mod_*`). A contributor adds one
folder and never edits the core.

### module.yml

```
id: 1
name: csi
version: 1
title: CSI sensing (movement scores, detections, CSI records)
senses_people: yes
kinds: 0, 1            # record kinds it owns (only the CSI module has its own;
                       # every other module sends kind 8, below)
keys: 6, 7, 8          # keys it owns (the CSI module's from 3.4/3.5; others use key 9)
budget: 1200           # most bytes a frame it may fill, of TW_MAX_PAYLOAD 1400
pins:                  # GPIOs it uses (none)
```

The build refuses a configuration where two enabled modules share an id, a
kind or a key, use the same pin, or whose budgets add to more than a frame
holds (the core keeps 120 bytes for its own records).

### Module ids

| id | module | |
|---|---|---|
| 0 | the core | |
| 1 | csi | CSI sensing |
| 2 | sound_level | the ICS-43434's level, dB once a second, never audio |
| 3-199 | given out here, one per module merged into the kit | |
| 200-254 | free for experiments: anyone, no need to ask | |
| 255 | reserved | |

## On the board: the interface

`firmware/tw_node/main/tw_module.h` (stage 3):

```c
typedef struct tw_module {
    uint8_t     id, version;            /* module.yml */
    const char *name;                   /* "csi" */
    uint8_t     senses_people;
    const tw_sense_t *sense;            /* the CSI sensing module only: its portable part */
    void (*init)(tw_node_t *n);                          /* its settings, from flash */
    void (*keys_changed)(tw_node_t *n, uint32_t changed);    /* settings taken: keep its own */
    int  (*command)(tw_node_t *n, char *cmd, char *reply, int max, int *after);  /* TWSET ... */
    int  (*led)(const tw_node_t *n, uint8_t bright, uint8_t c[3]);   /* its color; 1: flash */
} tw_module_t;
```

- Every hook is optional, and runs in the work task (the one that owns the
  protocol state), so a module needs no locks of its own.
- Which modules a build has: a table, `tw_modules.c`, written into the build
  folder by `main/CMakeLists.txt` from the enabled modules
  (`CONFIG_TW_MOD_*`). The core walks the table; it has no `#ifdef` for any
  module. (Every module found is a requirement of the core's component, and
  one turned off builds nothing: ESP-IDF settles requirements before it reads
  the configuration, so only a module's sources may depend on it.)
- A cable command no part of the build knows gets `TWERR not in this build:
  ...` (before tw_node 4.5 a board said nothing).
- `tw_sense_t` (`tw_proto.h`) is how the portable core calls CSI sensing
  (`modules/csi/tw_mod_csi/csi_sense.c`): a measurement into a record, the
  second's scores and detections into the `TWM` record, relearning. The
  desk simulator gives it to its boards the same way (`--no-csi`: without).
  CSI sensing's state and constants are still in `tw_proto.h`'s
  `tw_node_t`; they move into the module in a later step.

To come with the module record (stage 4), for every other module:

```c
    void (*tick)(tw_mod_ctx_t *, uint32_t now);              /* once a second */
    void (*on_csi)(tw_mod_ctx_t *, const tw_csi_t *);         /* a CSI measurement (the core's) */
    void (*on_record)(tw_mod_ctx_t *, const tw_rec_t *);      /* its records, from other boards */
    int  (*on_key)(tw_mod_ctx_t *, uint8_t key, const uint8_t *v, int len);   /* key 9 */
    const char *(*line)(tw_mod_ctx_t *, const tw_rec_t *, char *out, int max);
```

- A module sends with `tw_mod_send(ctx, subtype, bytes, len)`. The core puts
  it in a module record (kind 8, below), queues it like any record (two
  copies, relaying, de-duplication) and keeps the module within its budget.
- `tw_mod_key_get/set` keep a module's settings in flash under its own name.

### The module record (kind 8)

A record of kind 8 is a module's: `q[0]` its module id, `q[1]` the module's
own subtype, `q[2..]` its data (up to 190 bytes); `tx` is the sending board
itself, `txseq` is free for the module (a counter, a time).

At the exit it is printed as

```
TWZ,board,boot,record number,module,subtype,txseq,timekeeper clock,hex data
```

unless the module prints its own line (`line`). Records of a module a board
was not built with are still relayed and printed as `TWZ`: boards with
different modules share a group.

**Boards on 4.x** (no modules) parse kind 8, relay it, and print it as a
`TWR` line of kind 8; the computer side skips a `TWR` whose kind is above 1
since kit v5.5 (`0cb2b32`). So a 5.x board can join a 4.x group, and an
upgrade can go one board at a time, as 4.x's did.

### Module settings (key 9)

Key 9 is a module's setting or action: its value is the module id, the
module's own key, then up to 6 bytes. A board without that module answers
`TWERR no module <name> in this build` over its cable, and over the air
reports it does not have it, so `tw_setid.py` can say which boards ignored
it. (Boards skip keys they do not know by their length, so 4.x boards skip
key 9.)

### What a board says it has

- `TWV` (every 5 s, every board): `tw_node 5.0 +csi/1 +sound_level/1`.
- `TWH` (the cable's hello): the same list, and which of them sense people.

## On the computer

- **`tw_setid.py`**: each board's modules in the status; `detect`,
  `relearn` and the other module commands check the target boards have the
  module, as `board_key()` checks a firmware version today; `module NAME KEY
  VALUE [board N|all]` for any module's setting.
- **`check`**: a warning when boards in one group have different modules (as
  for different firmware), and a note for every board whose modules sense
  people.
- **Plug-ins**: `listen.py` keeps every line as now. `dash.py`, `replay.py`,
  `view.py` load `modules/*/host/` for the modules the boards say they have.
  A module without a plug-in shows as "board 3: module 7, no plug-in"; its
  lines are kept.
- **Updates over the air**: `tw_update.py` reads the new image's module list
  and asks before an update adds or removes a module on a board. Adding a
  microphone is never a side effect.

### The dashboard

Three layers:

1. **The core dashboard** (every build): boards, their firmware and modules,
   the hearing matrix, delivery and loss, relaying, settings, link health
   (RSSI, noise, gain, errors), the log, the computer's health, and the map
   (boards and hearing).
2. **Panels by declaration** (most modules): `decode.py` declares what it
   gives, for example

   ```python
   SERIES = [dict(name="sound_db", per="board", unit="dB", lo=0, hi=100, view="spark")]
   EVENTS = [dict(name="loud", per="board")]
   def decode(line): ...   # a TWZ line (or the module's own) -> {"board": 3, "sound_db": 41.5}
   ```

   and the dashboard draws a table, sparklines and events from that. No
   curses code in a module.
3. **Views of their own** (rich modules): a module can bring a view and a
   key; the CSI module's `c` view and the map's motion lines move there
   unchanged. The core keeps `q m p [ ] + - r g` and refuses a module key
   that takes one of them.

`replay.py` and `view.py` follow the same three layers.

## Building

At the top of the kit:

| | |
|---|---|
| `make csi_defconfig` | the configuration in `configs/csi_defconfig` (today's board) |
| `make NAME_defconfig` | any other in `configs/` |
| `make menuconfig` | the menu: modules, their options |
| `make savedefconfig NAME=mine` | keep the current choices as `configs/mine_defconfig` |
| `make bin` | build, and merge the image: `out/tw_node-s3-<config>-ch11.bin` |
| `make release-bin` | the same, and copy it to `tw_node-s3-ch11.bin` (csi only: the image the kit ships) |
| `make modules` | the modules found, and which this configuration has |
| `make sim` / `make sim-check` | the desk simulator, and its 3-board check |
| `make clean` | the build folders |

Configurations: `csi` (today's board) and `mesh` (the core alone, the mesh as
transport only; not yet tested on boards). A configuration in `configs/` holds the kit's own choices only (`CONFIG_TW_*`:
modules and their options); the rest of the build settings stay in
`firmware/tw_node/sdkconfig.defaults`, under every configuration. After
`make menuconfig`, the image is named `<config>-custom` until it is saved.
`tw_node-s3-ch11.bin` (what `flash_board.sh`, `FLASH-README.md` and
`version.sh` use) changes only with `make release-bin`, at a version bump.
ESP-IDF v5.5.6 (`IDF_EXPORT=~/esp-idf/export.sh` by default).

## Privacy

A module that senses people says so (`senses_people: yes`), and every tool
that shows a board shows it. A module should send what it needs and no
more: the sound-level module sends one dB value a second and never audio
(the mesh could not carry audio anyway: 16 kHz of 16-bit sound is 32 KB/s a
board, many times a board's share of the air).

## Stages, each leaving next working

1. **This document.**
2. **Building** (done): the `Makefile`, `configs/`, and the module checks
   (no firmware change). Gate, passed: `make csi_defconfig && make bin` gave
   4.4's image but for the build dates and the hashes that carry them (the
   check used for 4.4).
3. **The CSI module** (done; its night to come): the interface, the
   generated table, and CSI sensing moved behind it, the core keeping CSI
   receiving. `configs/mesh_defconfig` builds the core alone. Gates: the
   desk simulator gave the same output, byte for byte, before and after the
   move (3 boards; 6 with someone walking; a board off for 40 s with 10%
   loss; receive errors with measure-second on), and runs with `--no-csi`;
   then a night on real boards against a 4.4 night in the same layout and
   power (moving code changes the image, so no byte check on the image).
4. **Module records**: kind 8, `TWZ`, key 9, the module lists in `TWV` and
   `TWH`, and `tw_setid.py` / `check` reading them. Gate: a mixed group (4.4
   and 5.0 boards) for a night; the simulator with an unknown module.
5. **Plug-ins** for `dash.py`, `replay.py`, `view.py`; the CSI views move
   to the CSI plug-in. Gate: the dashboard and a replay of a 4.4 run look
   as they did.
6. **sound_level**, the second module. Gate: a night with it on one board,
   and a build with CSI sensing off (the mesh as transport only).
7. **Release**: tw_node 5.0, two-witnesses v6 on main.
