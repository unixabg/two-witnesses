# Third-party parts of two-witnesses, and their licenses

Checked 2026-10-30 against the actual downloaded packages. Everything not
listed here was written for this project (the kit's Python and shell tools,
`firmware/tw_node/main/tw_proto.c`, `tw_proto.h`, the simulator in
`firmware/sim/`, `radar_c3_harness.c`, `tests/gen_run.py`) and is under the
project's own license, Apache-2.0 (LICENSE, NOTICE).

| Part | Where | From | License |
|---|---|---|---|
| esp_radar_motion_dec 0.1.1, compiled library (ESP32-C3 build) | inside `radar_c3.elf` | Espressif Systems, components.espressif.com (header: `SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD`, `SPDX-License-Identifier: Apache-2.0`) | Apache-2.0 (`LICENSE-esp-radar.txt`, identical to the package's LICENSE) |
| zig compiler_rt soft-float routines | inside `radar_c3.elf` (linked by `zig cc`) | the Zig project | MIT |
| radar_c3.elf as a whole | kit root | radar_c3_harness.c (this project) + the two rows above | the harness under the project license; Espressif's part Apache-2.0; compiler_rt MIT |
| ESP-IDF v5.5.6, including Espressif's precompiled Wi-Fi/PHY libraries | inside every `.bin` firmware image | Espressif Systems | Apache-2.0 |
| esp_csi_gain_ctrl 0.1.5 | inside `tw_node-s3-ch11.bin` | Espressif Systems | Apache-2.0 |
| esp-csi examples csi_send / csi_recv (commit 8633d67) | `firmware/tw_node/main/main.c` is built on them (the two-board images built from them are in release v5.3) | Espressif Systems | example code: public domain / CC0 (their header), within the Apache-2.0 esp-csi repository |
| cmake_utilities | build tool only (fetched at build time, not shipped) | Espressif Systems | not distributed |

The esp-radar algorithm that `radar_replay.py` reproduces in Python was worked
out by running Espressif's compiled library on test inputs (and checked
against it to within 3e-7); no Espressif source code is copied.

## Used at run time, not shipped here

Installed by `setup_once.sh` from the usual package registries, not included
in this repository: numpy (BSD-3-Clause), matplotlib (matplotlib license,
BSD-style), pyserial (BSD-3-Clause), esptool (GPL-2.0-or-later; a separate
program, only run), unicorn (GPL-2.0; imported by `radar_emu.py`, only for
the optional esp-radar replay). Whoever bundles these together with the
kit must follow their licenses; the repository itself does not.

## What Apache-2.0 asks of whoever redistributes the Espressif parts

- include a copy of the Apache License 2.0 (`LICENSE-esp-radar.txt`);
- keep Espressif's copyright notices (in the headers and binaries as shipped);
- there are no NOTICE files in these packages to carry along;
- if any Espressif file is modified, say so in that file.
