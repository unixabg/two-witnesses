#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_modules.py - the modules in modules/ (docs/MODULES.md): list them, check them,
and keep a configuration's choices.

    python3 tools/tw_modules.py list [SDKCONFIG]          the modules, and which SDKCONFIG has
    python3 tools/tw_modules.py check [SDKCONFIG]         refuse clashes (ids, names, kinds, keys:
                                                          every module; pins and the airtime
                                                          budget: the ones SDKCONFIG has)
    python3 tools/tw_modules.py savedefconfig SDKCONFIG OUT   SDKCONFIG's CONFIG_TW_* lines -> OUT

Used by the Makefile; standard library only. module.yml is a small subset of YAML: one
"key: value" a line, lists as "a, b, c", "#" starts a comment.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAYLOAD = 1400          # TW_MAX_PAYLOAD (firmware/tw_node/main/tw_proto.h)
CORE_KEEPS = 120        # bytes a frame the core keeps for its own records (docs/MODULES.md)
CORE_KINDS = {2, 3, 4, 5, 6, 7, 8}      # the core's record kinds (4: gone, never reused; 8: modules)
CORE_KEYS = set(range(1, 6)) | {9} | set(range(16, 22))   # LED, survey, module key, network keys
REQUIRED = ("id", "name", "version", "title", "senses_people", "budget")
INT_LIST = ("kinds", "keys", "pins")


def parse(path):
    m = {}
    with open(path) as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            if ":" not in line:
                sys.exit(f"{path}:{n}: not 'key: value': {raw.rstrip()}")
            k, v = (x.strip() for x in line.split(":", 1))
            m[k] = v
    for k in REQUIRED:
        if k not in m or m[k] == "":
            sys.exit(f"{path}: no {k}")
    try:
        m["id"], m["version"], m["budget"] = int(m["id"]), int(m["version"]), int(m["budget"])
        for k in INT_LIST:
            m[k] = [int(x) for x in m.get(k, "").replace(",", " ").split()]
    except ValueError as e:
        sys.exit(f"{path}: {e}")
    if m["senses_people"] not in ("yes", "no"):
        sys.exit(f"{path}: senses_people is yes or no")
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,15}", m["name"]):
        sys.exit(f"{path}: name: lower case letters, digits, _ (16 at most), a letter first")
    m["dir"] = os.path.dirname(path)
    m["component"] = os.path.join(m["dir"], "tw_mod_" + m["name"])
    return m


def modules():
    return sorted((parse(p) for p in glob.glob(os.path.join(ROOT, "modules", "*", "module.yml"))),
                  key=lambda m: m["id"])


def enabled(sdkconfig):
    on = set()
    if sdkconfig and os.path.exists(sdkconfig):
        with open(sdkconfig) as f:
            for line in f:
                mm = re.match(r"CONFIG_TW_MOD_([A-Z0-9_]+)=y$", line.strip())
                if mm:
                    on.add(mm.group(1).lower())
    return on


def check(mods, on):
    problems = []
    seen = {}
    for m in mods:
        where = os.path.relpath(m["dir"], ROOT)
        if os.path.basename(m["dir"]) != m["name"]:
            problems.append(f"{where}: its folder must be named as the module: modules/{m['name']}/")
        if not os.path.isdir(m["component"]):
            problems.append(f"{where}: no folder tw_mod_{m['name']} (its ESP-IDF component)")
        if not 1 <= m["id"] <= 254:
            problems.append(f"{where}: id {m['id']}: 1-254 (0 is the core, 255 reserved)")
        for what, vals, core in (("id", [m["id"]], set()), ("name", [m["name"]], set()),
                                 ("kind", m["kinds"], CORE_KINDS), ("key", m["keys"], CORE_KEYS)):
            for v in vals:
                if v in core:
                    problems.append(f"{where}: {what} {v} is the core's")
                elif (what, v) in seen:
                    problems.append(f"{where}: {what} {v} is also {seen[(what, v)]}'s")
                else:
                    seen[(what, v)] = where
        if m["id"] != 1 and (m["kinds"] or m["keys"]):
            problems.append(f"{where}: kinds and keys of its own: only the CSI module's (from 3.4/3.5); "
                            "a module sends kind 8 and takes key 9 (docs/MODULES.md)")
        for k in m["kinds"]:
            if not 0 <= k <= 15:
                problems.append(f"{where}: kind {k}: 0-15")
        for k in m["keys"]:
            if not 1 <= k <= 31:
                problems.append(f"{where}: key {k}: 1-31")
    unknown = on - {m["name"] for m in mods}
    for u in sorted(unknown):
        problems.append(f"CONFIG_TW_MOD_{u.upper()}=y but no modules/*/module.yml named {u}")
    used = [m for m in mods if m["name"] in on]
    pins = {}
    for m in used:
        for p in m["pins"]:
            if p in pins:
                problems.append(f"GPIO {p}: both {pins[p]} and {m['name']}")
            pins[p] = m["name"]
    budget = sum(m["budget"] for m in used)
    if budget > PAYLOAD - CORE_KEEPS:
        problems.append(f"airtime: the modules want {budget} bytes a frame, "
                        f"{PAYLOAD - CORE_KEEPS} are free ({PAYLOAD}, the core keeps {CORE_KEEPS})")
    return problems


def main(a):
    if not a or a[0] not in ("list", "check", "savedefconfig"):
        sys.exit(__doc__)
    if a[0] == "savedefconfig":
        if len(a) != 3:
            sys.exit("savedefconfig SDKCONFIG OUT")
        keep = []
        with open(a[1]) as f:
            for line in f:
                line = line.strip()
                if line.startswith("CONFIG_TW_"):
                    keep.append(line)
                else:
                    mm = re.match(r"# (CONFIG_TW_[A-Z0-9_]+) is not set$", line)
                    if mm:
                        keep.append(mm.group(1) + "=n")
        with open(a[2], "w") as f:
            f.write(f"# {os.path.basename(a[2])}: the kit's choices (CONFIG_TW_*), saved by make savedefconfig.\n"
                    "# The other build settings: firmware/tw_node/sdkconfig.defaults, under every configuration.\n")
            f.write("".join(x + "\n" for x in keep))
        print(f"{a[2]}: {len(keep)} setting{'s' if len(keep) != 1 else ''}")
        return 0
    sdk = a[1] if len(a) > 1 else None
    mods, on = modules(), enabled(sdk)
    if a[0] == "list":
        if not mods:
            print("no modules yet (modules/*/module.yml): the core alone")
        for m in mods:
            mark = "on " if m["name"] in on else "off" if sdk else "   "
            people = ", senses people" if m["senses_people"] == "yes" else ""
            print(f"  {mark} {m['id']:3d} {m['name']:<16} v{m['version']}  {m['title']}{people}")
        used = [m for m in mods if m["name"] in on]
        if sdk:
            print(f"airtime: {sum(m['budget'] for m in used)} of {PAYLOAD - CORE_KEEPS} bytes a frame "
                  "for modules")
        return 0
    problems = check(mods, on)
    for p in problems:
        print("PROBLEM:", p)
    if problems:
        return 1
    print(f"modules: {len(mods)} found, {len([m for m in mods if m['name'] in on])} in this configuration, "
          "no clashes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
