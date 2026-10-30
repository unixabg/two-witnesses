#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""check_sim.py - read what a simulated exit printed, compare with what really
happened (the simulator's JSON on stderr), and say plainly whether the
protocol did its job.

    sim --nodes 3 --seconds 60 > out.txt 2> truth.json
    python3 check_sim.py out.txt truth.json [--expect-leaders 1,2]
"""
import argparse
import collections
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("truth")
    ap.add_argument("--min-delivery", type=float, default=0.85,
                    help="each link must deliver at least this share of the measurements made")
    a = ap.parse_args()
    truth = json.load(open(a.truth))
    got = collections.Counter()
    keys = collections.Counter()
    bad = 0
    leaders, periods = [], []
    for line in open(a.out, errors="replace"):
        line = line.strip()
        if "," in line and line.split(",", 1)[0].isdigit():
            line = line.split(",", 1)[1]
        f = line.split(",")
        if f[0] == "TWR":
            if len(f) != 18 or len(f[17]) != 2 * int(f[15]):
                bad += 1
                continue
            rx, tx, recseq, boot = int(f[1]), int(f[2]), int(f[5]), int(f[6])
            keys[(rx, boot, recseq)] += 1
            got[f"{tx}-{rx}"] += 1
        elif f[0] == "TWS":
            leaders.append(int(f[3]))
            periods.append(int(f[6]))
    dups = sum(c - 1 for c in keys.values() if c > 1)
    print(f"nodes {truth['nodes']}, {truth['seconds']:.0f} s, random loss {100 * truth['loss']:.0f}%: "
          f"{truth['frames']} frames, {truth['collisions']} collisions, airtime {truth['airtime_pct']:.1f}%")
    worst = 1.0
    for link, made in sorted(truth["made"].items(), key=lambda kv: [int(x) for x in kv[0].split("-")]):
        share = got.get(link, 0) / made
        worst = min(worst, share)
        print(f"  link {link:>5}: made {made:6d}, reached the computer {got.get(link, 0):6d} ({100 * share:5.1f}%)")
    print(f"  duplicates printed: {dups};  malformed lines: {bad}")
    if leaders:
        changes = sum(1 for x, y in zip(leaders, leaders[1:]) if x != y)
        print(f"  leader as seen by the exit: {sorted(set(leaders))}, changes {changes}; "
              f"period {min(periods)}-{max(periods)} ms")
    print(f"  dropped from queues (never sent): {truth['dropped']}")
    ok = worst >= a.min_delivery and dups == 0 and bad == 0
    print("RESULT", "PASS" if ok else "FAIL", f"(worst link {100 * worst:.1f}%, needed {100 * a.min_delivery:.0f}%)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
