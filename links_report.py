#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""links_report.py - compare the links of a many-board run.

    ~/scratch-esp/bin/python links_report.py RUN_FOLDER/links
    (split_links.py --view runs it for you)

Reads each link's view.json (written by view.py) and links/channel.json
(written by split_links.py; optional) and writes links/across.txt:

  LINKS       one line per link: packets, lost, RSSI, how many events,
              low-data stretches and RSSI shifts
  MOMENTS     the times when something showed on two or more links at once
              (events, low data, RSSI shifts within a minute of each other),
              which links showed it and which stayed quiet. Low data on
              every link at once is marked WHOLE CHANNEL: no single board or
              path explains it (another device on the channel, or our own
              timekeeping - the timekeeper changes are shown beside it).
  ONE LINK    things that showed on one link only
  RHYTHMS     rates that keep coming back in the 10-minute pieces, more
              often than chance allows, and whether they come back on one
              link only (a habit of that board or that path) or on several
              (more likely something in the room, if it also starts and
              stops). Multiples of a rate are grouped with it.

Times and numbers only: no labels, no guesses.
"""
import collections
import datetime as dt
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tw_map  # noqa: E402

NEAR_S = 60          # things on different links this close are one moment
TOL_BINS = 2         # rhythm rates this many steps apart (1 step = 1/600 Hz) are the same rate
F_MIN, F_MAX = 0.03, 4.9
P_CHANCE = 0.001     # a rate is reported if chance would give its count less often than this
SLOW_HZ = 0.15       # below this the method itself makes peaks (the 60 s detrend's edge): pure
                     # noise shows 0.03-0.13 Hz on every link, so those are listed apart, not judged


def fmt(t):
    return dt.datetime.fromtimestamp(t).strftime("%H:%M:%S")


def link_key(k):
    return [int(x) for x in k.split("-")]


def load(outdir):
    views = {}
    for path in glob.glob(os.path.join(outdir, "*-*", "view.json")):
        link = os.path.basename(os.path.dirname(path))
        try:
            views[link] = json.load(open(path))
        except (ValueError, OSError):
            pass
    channel = None
    try:
        channel = json.load(open(os.path.join(outdir, "channel.json")))
    except (ValueError, OSError):
        pass
    return dict(sorted(views.items(), key=lambda kv: link_key(kv[0]))), channel


def poisson_tail(k, lam):
    """P(X >= k) for X ~ Poisson(lam)"""
    if k <= 0:
        return 1.0
    term = math.exp(-lam)
    cdf = term
    for i in range(1, k):
        term *= lam / i
        cdf += term
    return max(0.0, 1.0 - cdf)


def needed(lam, tests):
    """smallest count that chance reaches less often than P_CHANCE, over this many rates"""
    k = 1
    while poisson_tail(k, lam) * tests >= P_CHANCE:
        k += 1
    return k


def moments(views):
    items = []      # (start, end, link, text, cause: "path" / "delivery" / None)
    for link, v in views.items():
        for s, e, x in v["events"]:
            items.append((s, e, link, f"event x{x:.1f}" if x else "event", None))
        for row in v["low_data"]:
            s, e, _pps, lost = row[:4]
            pl, dl = (row[4], row[5]) if len(row) >= 6 else (None, None)
            if pl is None:
                items.append((s, e, link, f"low data {lost:.0f}% lost", None))
            else:
                cause = "path" if pl >= dl else "delivery"
                items.append((s, e, link, f"low data {lost:.0f}% lost (path {pl:.0f}%, delivery {dl:.0f}%)", cause))
        for s, e, pk in v.get("path_rises", []):
            items.append((s, e, link, f"PATH LOSS up to {pk:.0f}% (usual {v.get('path_usual') or 0:.0f}%)", "path"))
        for s, e in v["gaps"]:
            items.append((s, e, link, "gap", None))
        for t, d in v["rssi_shifts"]:
            items.append((t, t, link, f"RSSI {d:+.1f} dB", None))
    items.sort()
    groups = []
    for it in items:
        if groups and it[0] <= groups[-1]["end"] + NEAR_S:
            g = groups[-1]
            g["end"] = max(g["end"], it[1])
            g["items"].append(it)
        else:
            groups.append(dict(start=it[0], end=it[1], items=[it]))
    return groups


def channel_lines(channel, s, e, views):
    """records and noise floor per measuring board in [s, e], against the run's usual"""
    if not channel:
        return []
    rows = channel["windows"]
    win = channel.get("win_s", 10)
    per_rx_all = collections.defaultdict(list)
    for t, d in rows:
        for rx, vals in d.items():
            per_rx_all[rx].append((t, vals[0], vals[1], vals[2] if len(vals) > 2 else None))
    out = []
    parts = []
    for rx in sorted(per_rx_all, key=int):
        allr = per_rx_all[rx]
        ns = sorted(n for _t, n, _z, _d in allr)
        zs = sorted(z for _t, _n, z, _d in allr)
        usual_n, usual_z = ns[len(ns) // 2], zs[len(zs) // 2]
        inside = [(n, z, dl) for t, n, z, dl in allr if s - win < t <= e]
        if not inside:
            parts.append(f"board {rx}: nothing")
            continue
        n_in = sum(n for n, _z, _d in inside) / (len(inside) * win)
        z_in = sum(z * n for n, z, _d in inside) / max(sum(n for n, _z, _d in inside), 1)
        dls = [d_ for _n, _z, d_ in inside if d_ is not None]
        parts.append(f"board {rx}: {n_in:.0f} records/s (usual {usual_n / win:.0f}), "
                     f"noise floor {z_in:.1f} dBm (usual {usual_z:.1f})"
                     + (f", its reports {max(dls):.0f}% undelivered" if dls else ""))
    out.append("      measured: " + "; ".join(parts))
    tws = channel.get("tws") or []
    before = [r for r in tws if r[0] <= s - 30]
    during = [r for r in tws if s - 30 < r[0] <= e + 30]
    if before or during:
        ref = (before[-1] if before else during[0])
        last = during[-1] if during else ref
        out.append(f"      timekeeping (exit board {ref[1]}): timekeeper {ref[2]}"
                   + (f" -> {last[2]}" if last[2] != ref[2] else " throughout")
                   + f", leader changes {ref[3]}" + (f" -> {last[3]}" if last[3] != ref[3] else " (none)"))
    return out


def rhythms(views):
    """rates that recur in the 10-min pieces more often than chance"""
    lo, hi = int(F_MIN * 600), int(F_MAX * 600)
    nbins = hi - lo + 1
    hits = collections.defaultdict(lambda: collections.defaultdict(set))   # bin -> link -> piece times
    exact = collections.defaultdict(lambda: collections.defaultdict(list))   # bin -> link -> (rate, ratio)
    pieces = {}
    top = 0
    for link, v in views.items():
        pieces[link] = [t for t, _pk in v["pieces"]]
        for t, pk in v["pieces"]:
            top = max(top, len(pk))
            for ratio, fr in pk:
                b0 = int(round(fr * 600))
                for b in range(b0 - TOL_BINS, b0 + TOL_BINS + 1):
                    hits[b][link].add(t)
                    exact[b][link].append((fr, ratio))
    if not top:
        return [], {}
    width = 2 * TOL_BINS + 1
    tests = nbins / width
    n_all = sum(len(p) for p in pieces.values())
    lam_all = n_all * top * width / nbins
    k_all = needed(lam_all, tests)
    k_link = {link: needed(len(p) * top * width / nbins, tests * len(views)) for link, p in pieces.items()}
    found = []
    for b, by_link in hits.items():
        total = sum(len(ts) for ts in by_link.values())
        strong = [lk for lk, ts in by_link.items() if len(ts) >= k_link[lk]]
        links_on = [lk for lk, ts in by_link.items() if len(ts) >= 2]
        if strong or (total >= k_all and len(links_on) >= 2):
            found.append((total, b, {lk: sorted(ts) for lk, ts in by_link.items()}, strong, exact[b]))
    found.sort(key=lambda x: (-x[0], x[1]))
    kept = []
    for f in found:
        if all(abs(f[1] - k[1]) > 2 * TOL_BINS for k in kept):
            kept.append(f)
    kept.sort(key=lambda x: x[1])
    # multiples: a rate within tolerance of 2-6 x a lower kept rate joins that rate's family
    family = {}
    for i, f in enumerate(kept):
        for g in kept[:i]:
            base = family.get(g[1], g[1])
            m = f[1] / base
            if 2 <= round(m) <= 6 and abs(f[1] - round(m) * base) <= TOL_BINS + round(m) // 2:
                family[f[1]] = base
                break
    info = dict(k_all=k_all, lam_all=lam_all, k_link=k_link, pieces=pieces, top=top)
    return kept, dict(info, family=family)


def hour_counts(ts):
    c = collections.Counter(dt.datetime.fromtimestamp(t).strftime("%H") for t in ts)
    return " ".join(f"{h}:{c[h]}" for h in sorted(c))


def moment_levels(items):
    """map levels (0..4) per board pair for one moment's items"""
    levels = {}
    for _s, _e, link, text, cause in items:
        if cause == "delivery":
            continue                            # reports that did not get home: not sensing
        try:
            a, b = (int(v) for v in link.split("-"))
        except ValueError:
            continue
        lv = 1                                  # RSSI shift, gap or low data: something, unsized
        if text.startswith("event x"):
            try:
                x = float(text.split()[1][1:])
            except (ValueError, IndexError):
                x = 3
            lv = 3 if x >= 6 else 2
        elif cause == "path" and text.startswith("low data"):
            lv = 2
        elif text.startswith("PATH LOSS"):
            try:
                pk = float(text.split("up to ")[1].split("%")[0])
            except (ValueError, IndexError):
                pk = 30
            lv = 3 if pk >= 50 else 2
        p = tw_map.pair(a, b)
        levels[p] = max(levels.get(p, 0), lv)
    return levels


def report(outdir):
    views, channel = load(outdir)
    bxy, marks = tw_map.load_boards(tw_map.find_boards(os.path.dirname(os.path.abspath(outdir)), outdir))
    if not views:
        sys.exit(f"no view.json under {outdir} (run view.py on the links first, kit v5.1 or later)")
    L = [f"links     {os.path.abspath(outdir)}  ({len(views)} links with a view)", ""]
    L.append("LINKS (from-to = the 'to' board measuring the 'from' board's frames)")
    L.append("  link    packets     lost    path  delivery   RSSI   events  low-data  path rises  RSSI shifts")

    def pct(x):
        return f"{x:6.1f}%" if x is not None else "     - "
    for link, v in views.items():
        L.append(f"  {link:>5} {v['packets']:10,d}  {v['lost_pct']:6.1f}%  {pct(v.get('path_lost_pct'))}  "
                 f"{pct(v.get('delivery_lost_pct'))}  {v['rssi_median']:6.1f}  "
                 f"{len(v['events']):6d}  {len(v['low_data']):8d}  {len(v.get('path_rises', [])):9d}  "
                 f"{len(v['rssi_shifts']):6d}")
    L.append("  (path = frames the measuring board did not hear: sensing; delivery = its reports that "
             "never reached the computer: plumbing)")
    L.append("")

    groups = moments(views)
    multi = [g for g in groups if len({it[2] for it in g["items"]}) >= 2]
    single = [g for g in groups if len({it[2] for it in g["items"]}) == 1]
    L.append(f"MOMENTS ON TWO OR MORE LINKS (within {NEAR_S} s of each other): {len(multi)}")
    for g in multi:
        s, e = g["start"], g["end"]
        on = sorted({it[2] for it in g["items"]}, key=link_key)
        live = [lk for lk, v in views.items() if v["span"][0] <= e and v["span"][1] >= s]
        quiet = [lk for lk in live if lk not in on]
        low_on = {it[2] for it in g["items"] if it[3].startswith(("low data", "gap"))}
        whole = len(low_on) == len(live) and len(live) >= 2
        tag = ""
        if whole:
            causes = [it[4] for it in g["items"] if it[3].startswith("low data") and it[4]]
            if causes and causes.count("delivery") > len(causes) / 2:
                tag = "   ALL REPORTS MISSING (low data on every link, mostly delivery: the cabled board or the computer)"
            elif causes and causes.count("path") > len(causes) / 2:
                tag = "   WHOLE CHANNEL (low data on every link, the boards did not hear each other)"
            else:
                tag = "   WHOLE CHANNEL (low data on every link)"
        L.append(f"  {fmt(s)} - {fmt(e)}  on {len(on)} of {len(live)} links{tag}")
        for lk in on:
            what = ", ".join(f"{it[3]} {fmt(it[0])}" + (f"-{fmt(it[1])}" if it[1] > it[0] else "")
                             for it in g["items"] if it[2] == lk)
            L.append(f"      {lk:>5}: {what}")
        if quiet:
            L.append(f"      quiet: {', '.join(quiet)}")
        if whole or low_on:
            L += channel_lines(channel, s, e, views)
        if bxy:
            for row in tw_map.render(bxy, moment_levels(g["items"]), marks, 41, 12):
                if row.strip():
                    L.append("        " + row)
    if bxy and multi:
        L.append("    (maps: " + tw_map.legend(bxy, marks) + ")")
    L.append("")
    L.append(f"ON ONE LINK ONLY: {len(single)}")
    for g in single:
        it0 = g["items"][0]
        what = ", ".join(f"{it[3]} {fmt(it[0])}" + (f"-{fmt(it[1])}" if it[1] > it[0] else "") for it in g["items"])
        L.append(f"  {it0[2]:>5}: {what}")
    L.append("")

    kept, info = rhythms(views)
    if info:
        L.append("RHYTHMS THAT COME BACK (rates among the top peaks of the 10-min pieces more often than "
                 f"chance; chance: {info['lam_all']:.1f} pieces for any rate over all links, "
                 f"reported from {info['k_all']})")
        fam = info["family"]
        slow = [k for k in kept if k[1] / 600 < SLOW_HZ]
        kept = [k for k in kept if k[1] / 600 >= SLOW_HZ]
        if not kept:
            L.append("  none")

        def rate_of(ex):
            allr = sorted(fr for lk in ex for fr, _r in ex[lk])
            return allr[len(allr) // 2]
        exact_of = {k[1]: rate_of(k[4]) for k in kept}
        for total, b, by_link, strong, ex in kept:
            allr = sorted(fr for lk in ex for fr, _r in ex[lk])
            fr = allr[len(allr) // 2]
            base = fam.get(b)
            links_on = [lk for lk in sorted(by_link, key=link_key) if len(by_link[lk]) >= 2]
            if len(strong) == 1 and sum(len(by_link[lk]) for lk in strong) >= 0.7 * total:
                where = f"ONE LINK: {strong[0]}"
            elif len(links_on) >= 2:
                where = f"SEVERAL LINKS: {', '.join(links_on)}"
            else:
                where = "scattered"
            L.append(f"  {fr:7.4f} Hz ({fr * 60:6.2f}/min)  in {total} pieces  {where}"
                     + (f"   (x{round(b / base)} of {exact_of.get(base, base / 600):.4f} Hz)" if base else ""))
            for lk in sorted(by_link, key=link_key):
                ts = by_link[lk]
                if len(ts) < 2 and lk not in strong:
                    continue
                rs = sorted(r_ for _f, r_ in ex[lk])
                L.append(f"      {lk:>5}: {len(ts):3d} of {len(info['pieces'][lk])} pieces, typically x{rs[len(rs) // 2]:.0f}"
                         + ("  (more than chance on this link alone)" if lk in strong else "")
                         + f"   by hour {hour_counts(ts)}")
        if slow:
            L.append(f"  under {SLOW_HZ} Hz (the method's own edge; pure noise shows these too, not judged): "
                     + ", ".join(f"{k[1] / 600:.3f} Hz" for k in slow))
    text = "\n".join(L) + "\n"
    open(os.path.join(outdir, "across.txt"), "w").write(text)
    return text


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        sys.exit(__doc__)
    d = sys.argv[1]
    if os.path.isdir(os.path.join(d, "links")):
        d = os.path.join(d, "links")
    print(report(d))
    print(f"saved {os.path.join(d, 'across.txt')}")


if __name__ == "__main__":
    main()
