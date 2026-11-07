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
    if "extra" in truth:            # tw_node 2.1: a second frame in the slot
        ex = truth["extra"]
        sent = sum(v[0] for v in ex.values())
        print(f"  extra frames: {sent} sent, {sum(v[1] for v in ex.values())} skipped (no time), "
              f"{truth.get('collisions_extra', 0)} collisions involving one; latest end "
              f"{max((v[2] for v in ex.values() if v[0]), default=0)} us into the slot "
              f"(may end by {truth.get('slot_end_us', 3200)}), late {sum(v[3] for v in ex.values())}"
              + (f"; measured {truth['extra_measured']}" if truth.get("extra_measured") else ""))
    if "clock_err_us" in truth:
        ce = truth["clock_err_us"]
        print(f"  followers' clocks against the timekeeper's: {ce[0]:+.0f} us on average, at worst {ce[1]:.0f} us off")
    if "late" in truth:             # --late: the delays the real boards measured (tw_node 2.2)
        print(f"  measured delays: {truth['deferrals']} times a radio waited for the channel; per board "
              "(after the first 10 s; us in its slot by the timekeeper's clock, mean / max):")
        print("    board  normal frame on air at   second frame ends     over 3200  over 3400   waited (n, mean / max)")
        for b, v in sorted(truth["late"].items(), key=lambda kv: int(kv[0])):
            xe = f"{v['xend'][0]:6.0f} / {v['xend'][1]:5.0f}" if v["n_x"] else "      -       "
            print(f"    {b:>5}  {v['start'][0]:6.0f} / {v['start'][1]:5.0f}        {xe}     {v['x_over']:6d}     "
                  f"{v['x_into']:6d}   {v['defer'][0]:6d}, {v['defer'][1]:4.0f} / {v['defer'][2]:5.0f}")
        ex = truth.get("exit_board")
        for b, v in sorted(truth["late"].items(), key=lambda kv: int(kv[0])):
            if ex is not None and int(b) != ex:
                continue
            print(f"  board {b} as its TWF would say: tx_at {v['tx_at'][0]:.0f} / {v['tx_at'][1]:.0f}, "
                  f"tx_radio {v['tx_radio'][0]:.0f} / {v['tx_radio'][1]:.0f} (air {(v['tx_len'] + 30) * 16 / 27 + 40:.0f}), "
                  f"x_wake {v['x_wake'][0]:.0f} / {v['x_wake'][1]:.0f}, x_radio {v['x_radio'][0]:.0f} / "
                  f"{v['x_radio'][1]:.0f} (air {(v['x_len'] + 30) * 16 / 27 + 40 if v['x_len'] else 0:.0f}), "
                  f"own end {v['x_end_own'][0]:.0f} / {v['x_end_own'][1]:.0f}")
    made_by = collections.Counter()
    got_by = collections.Counter()
    for link, made in truth["made"].items():
        rx = link.split("-")[1]
        made_by[rx] += made
        got_by[rx] += got.get(link, 0)
    print("  reports home per measuring board: " +
          ", ".join(f"{rx}: {100 * got_by[rx] / made_by[rx]:.1f}%" for rx in sorted(made_by, key=int)))
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
