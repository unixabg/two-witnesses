#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""timing_cases.py - the simulator with the delays the real boards measured
(tw_node 2.2), against each part of the timing fix proposed for 2.3
(docs/TIMING-2.3.md). A few minutes.

    cc -O2 -o sim sim.c ../tw_node/main/tw_proto.c -lm
    python3 timing_cases.py ./sim [CASE ...]     (CASE: part of a case name, e.g. "8 forced")

Per case and model: the measuring boards' reports that reached the computer
(worst board / mean), collisions, times a radio waited for the channel, the
followers' clocks against the timekeeper's (mean, us; negative = behind),
where followers' normal frames were on the air in their slots (mean / max,
us after the slot start by the timekeeper's clock; the target is 400), where
second frames ended (the limit is 3200; the next board's frame is due from
3400), and second frames sent / skipped for lack of time. 120 s, 3% random
loss, seed 7.
"""
import collections
import json
import subprocess
import sys

CASES = [("3 healthy", "--nodes 3"), ("6 healthy", "--nodes 6"), ("6 forced", "--nodes 6 --set 0@second=force"),
         ("6, 2+3 shadowed", "--nodes 6 --block 2-1@0-120@0.75,3-1@0-120@0.75"),
         ("7 forced", "--nodes 7 --set 0@second=force"),
         ("8 healthy", "--nodes 8"), ("8 forced", "--nodes 8 --set 0@second=force")]
MODELS = [("ideal radio (sim before)", []), ("2.2 as measured", ["--late", "measured"]),
          ("2.2, receive callback +150", ["--late", "rxcb=650"]), ("2.2, radio late 5x as often", ["--late", "stall=0.01"]),
          ("2.3 ahead", ["--fix", "ahead"]), ("2.3 ahead+clock", ["--fix", "ahead,clock"]),
          ("2.3 ahead+clock+cb", ["--fix", "ahead,clock,cb"]), ("2.3 ahead+clock+queue", ["--fix", "ahead,clock,queue"]),
          ("2.3 ahead+hwrx", ["--fix", "ahead,hwrx"]), ("2.3 ahead+hwrx+cb", ["--fix", "ahead,hwrx,cb"]),
          ("2.3 ahead+hwrx+queue", ["--fix", "ahead,hwrx,queue"])]


def run(sim, case, model):
    r = subprocess.run([sim] + case.split() + ["--seconds", "120", "--loss", "0.03"] + model,
                       capture_output=True, text=True, check=True)
    tr = json.loads(r.stderr)
    got = collections.Counter()
    for line in r.stdout.splitlines():
        f = line.split(",")[1:]
        if f and f[0] == "TWR" and len(f) == 18:
            got[f"{f[2]}-{f[1]}"] += 1
    made_by, got_by = collections.Counter(), collections.Counter()
    for link, made in tr["made"].items():
        rx = link.split("-")[1]
        made_by[rx] += made
        got_by[rx] += got.get(link, 0)
    share = [100 * got_by[x] / made_by[x] for x in made_by]
    sent = sum(v[0] for v in tr["extra"].values())
    skip = sum(v[1] for v in tr["extra"].values())
    row = [f"{min(share):5.1f} / {sum(share) / len(share):5.1f}%", f"{tr['collisions']:4d}"]
    if "late" in tr:
        lt = tr["late"]
        fol = [v for k, v in lt.items() if k != "1" and v["n"]]
        n = sum(v["n"] for v in fol)
        nx = sum(v["n_x"] for v in lt.values())
        row += [f"{tr['deferrals']:5d}", f"{tr['clock_err_us'][0]:5.0f}",
                f"{sum(v['start'][0] * v['n'] for v in fol) / n:5.0f} / {max(v['start'][1] for v in fol):5.0f}",
                f"{sum(v['xend'][0] * v['n_x'] for v in lt.values()) / nx:5.0f} / "
                f"{max(v['xend'][1] for v in lt.values()):5.0f}" if nx else "-",
                f"{sum(v['x_over'] for v in lt.values()):6d}", f"{sum(v['x_into'] for v in lt.values()):6d}"]
    else:
        row += ["-", f"{tr['clock_err_us'][0]:5.0f}", "-", "-", "-", "-"]
    return row + [f"{sent:6d} / {skip:4d}"]


def main():
    sim, only = sys.argv[1], sys.argv[2:]
    head = ["home worst/mean", "coll", "waits", "clock", "normal on air", "2nd frame ends", ">3200", ">3400",
            "2nd sent/skip"]
    w = [16, 4, 5, 5, 13, 13, 6, 6, 13]
    for name, case in CASES:
        if only and not any(o in name for o in only):
            continue
        print(f"== {name} ({case})")
        print("   " + f"{'':28s}" + "  ".join(f"{h:>{k}s}" for h, k in zip(head, w)))
        for label, model in MODELS:
            print("   " + f"{label:28s}" + "  ".join(f"{c:>{k}s}" for c, k in zip(run(sim, case, model), w)))


if __name__ == "__main__":
    main()
