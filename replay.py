#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""replay.py - a page that replays a many-board run on the board layout: every
link, both directions, second by second, with a timeline, the events and the
lasting changes.

    ~/scratch-esp/bin/python ~/tw-kit/replay.py RUN_FOLDER [--title TITLE]
            [--from HH:MM] [--to HH:MM] [--note "HH:MM[:SS] text"]... [--view waves|lines]
            [--settle MIN] [--out FILE]
    e.g. ~/scratch-esp/bin/python ~/tw-kit/replay.py ~/tw-data/runs/2026-11-03_0000 \\
            --title "Garage replay" --from 05:00 --note "07:15 door opened"

Writes RUN_FOLDER/replay.html (or --out): one file, no outside links, opens in
any browser, offline. Without --title it asks at a keyboard (Enter keeps
"Link replay") and uses that name when nobody is there (cron). --from/--to
keep part of the run (clock times, as listen.py --until: the next time that
clock time comes round after the run started). --note puts your own marks on
the timeline (any number of them). A survey folder's own marks (the m key:
placement.csv) and boards moved (moves.csv) are shown as notes by themselves;
the boards are drawn where they were when it started.

Two ways to draw the links (a switch on the page flips between them; --view
says which the page opens with):
  waves   (the default) each board is a beacon with one fan of arcs per path,
          pointing to the other board, for its own signal reaching it; up to
          3 arcs lit for how much that direction changed, and echoes running
          out along the path while it plays. Both ends of a path lit:
          something real; one end only: a board's own quirk.
  lines   two lines per link, one per direction, the arrow from the sender to
          the board that measures.

The layout is the run folder's boards.csv: the one the run started with
(run.sh and survey.sh keep it there), boards and landmarks alike; edit that
copy to add a landmark to an old run. A folder without one is drawn with the
layout you keep up to date (tw_layout.py), and the page and the terminal say
so. Format: tw_map.py. The page holds the layout and the times of the run:
keep it as private as the run itself.

Per second and per direction of every link (from the records' amplitudes, in
bands of 4 subcarriers, after taking out each record's overall level):
  change  how much the link's shape fluctuated within the second, in times its
          quiet level (each band's median over the run); 1 = quiet
  loss    share of the sender's frames that left no record that second
  shape   how far the link's average shape that second is from its usual
          lately (the median of the 30 minutes before, ending a minute
          earlier), dB. Something parked, moved or opened shows for about 15
          minutes, then counts as the usual and the mark clears (--settle MIN
          sets the minutes); the lasting changes listed below stay.
Events: seconds where a direction passes its threshold (the 99.9th percentile
of its quietest hour, but no more than its median + 20 x its usual spread,
and at least 1.3), merged across gaps of up to 15 s, kept
when 2 s or longer or one second at 1.5x the threshold.
Watch runs (watch.sh): the camera's events (detect/events.csv, with the label
and note you give them) are marked on the timeline and listed with the lasting
changes, events the camera kept frames for are tagged "camera", and a camera
panel shows the frame of the second under the cursor (the camera keeps one a
second, around its events). The frames are linked, not copied: the page shows
them while it sits in its watch folder. To look at it from another computer,
serve the folder:
    cd ~/tw-data/watch/<run> && python3 -m http.server 8000
and open http://localhost:8000/replay.html on the Chromebook; for other
computers at home add --bind 0.0.0.0, let ChromeOS forward the port (Settings,
Linux development environment, Port forwarding: 8000), and open
http://<the Chromebook's address>:8000/replay.html. Anyone on the network can
then read the whole folder (recordings and frames) until Ctrl-C stops it.
Stretches: an event of 2 minutes or more (a link that stays restless, as after
a car has left) is listed as a stretch, with the peaks inside it as events of
their own (E48a, E48b, ...): moments when at least two directions rise to 1.8x
their level of the 5 minutes before, or the loss jumps by 20 points; a peak
with only one direction's loss behind it is marked weak. A long stretch no
longer hides what happens within it.
About this recording: the instrument as the boards reported it during the run
(firmware, each board's transmit power and relaying, any change of power, which
is also put on the timeline, bandwidth, layout, thresholds, settle time, kit
version); the title line says the power ("all boards at 11 dBm"). Runs at
different powers compare only by shape and RSSI steps, not by event sizes.
Lasting changes: a link whose shape steps by 1 dB or more in both directions
and stays (the 5 minutes before against the 5 minutes after); listed with the
change in RSSI. Something parked, moved or opened, not someone moving.

Two marks for where something is (a direction counts as lit past its event
threshold, held for 3 seconds; each mark lasts 3 seconds or more):
  near a board   everything lit touches one board, on at least two of its
                 paths, at least one of them lit both ways, and the path away
                 from it is quiet: something near that board, on any side
                 (behind it too). Drawn as rings around it; events mostly near
                 one board are tagged "near N".
  one-sided      a direction lit while the other way of the same path is
                 clearly quiet: more likely a board's own quirk than the room.
                 Drawn hollow; "one-sided" in the table.

What it cannot show exactly where a person is. A lit link means something moved in
or near its path, or (with no shape change) somewhere its signal reflects
through. Three boards give three lines; more boards give more crossings.

Time and memory: about a minute per recorded hour on a laptop for 3 boards
(longer on a Chromebook), and a few MB of memory per hour and link direction.
"""
import argparse
import datetime as dt
import glob
import html
import json
import os
import re
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines: reads gzip cut short by a power cut)
import tw_map  # noqa: E402  (find_boards, load_boards)

BAND = 4                # subcarriers per band
MIN_REC = 30            # records in a second to use it (at 50 frames/s)
BATCH = 200000


def parse_clock(s):
    m = re.fullmatch(r"(\d{1,2}):(\d{2})(?::(\d{2}))?", s.strip())
    if not m:
        raise ValueError(s)
    return int(m[1]), int(m[2]), int(m[3] or 0)


def next_at(start, hms):
    """epoch of the next time this clock time comes round at or after start (local time)"""
    d = dt.datetime.fromtimestamp(start).replace(hour=hms[0], minute=hms[1], second=hms[2], microsecond=0)
    if d.timestamp() < start:
        d += dt.timedelta(days=1)
    return int(d.timestamp())


class Acc:
    """per direction: running sums of the band shape for each second of each hour"""
    def __init__(self):
        self.valid = {}             # dir -> bool mask over bins (fixed at its first batch)
        self.nbins = {}             # dir -> record length it uses
        self.hours = {}             # (dir, hour epoch) -> dict of arrays
        self.done = {}              # (dir, hour epoch) -> finished float16 arrays

    def bands(self, d):
        idx = np.flatnonzero(self.valid[d])
        n = len(idx) // BAND
        return idx[: n * BAND].reshape(n, BAND)

    def add(self, d, secs, step, q, rssi):
        a = q.astype(np.float32) * (step[:, None] / 4.0)
        if d not in self.valid:
            self.valid[d] = (q > 0).mean(axis=0) > 0.5
        bd = self.bands(d)
        if len(bd) == 0:
            return
        db = 20 * np.log10(a + 0.5)
        shp = db[:, bd].mean(axis=2)                  # records x bands
        shp -= shp.mean(axis=1, keepdims=True)        # the record's own level out
        hours = secs - secs % 3600
        for h in np.unique(hours):
            k = (d, int(h))
            if k in self.done:          # a stray second for an hour already finished
                continue
            if k not in self.hours:
                nb = shp.shape[1]
                self.hours[k] = dict(s1=np.zeros((3600, nb)), s2=np.zeros((3600, nb)),
                                     n=np.zeros(3600), r=np.zeros(3600))
            H = self.hours[k]
            m = hours == h
            i = (secs[m] - h).astype(int)
            np.add.at(H["s1"], i, shp[m]); np.add.at(H["s2"], i, shp[m] ** 2)
            np.add.at(H["n"], i, 1); np.add.at(H["r"], i, rssi[m])

    def finish(self, before=None):
        for k in [k for k in self.hours if before is None or k[1] < before]:
            H = self.hours.pop(k)
            n = np.maximum(H["n"], 1)[:, None]
            mean = H["s1"] / n
            std = np.sqrt(np.maximum(H["s2"] / n - mean * mean, 0))
            self.done[k] = dict(mean=mean.astype(np.float16), std=std.astype(np.float16),
                                n=H["n"].astype(np.int32), r=(H["r"] / n[:, 0]).astype(np.float32))


def instrument(inst, p, t_from, t_to):
    """the boards' own reports: transmit power and relaying (TWN: each board heard, once a
    second; TWS: the cabled board), firmware (TWH: hello; its board address is not kept)"""
    try:
        sec = int(p[0]) // 1_000_000_000
        if (t_from and sec < t_from) or (t_to and sec >= t_to):
            return
        f = p[2:]
        if p[1] == "TWN" and len(f) >= 10:
            inst["power"].append((sec, int(f[0]), int(f[9])))
            if len(f) >= 11:
                inst["relay"].append((sec, int(f[0]), int(f[10])))
        elif p[1] == "TWS" and len(f) >= 19:
            inst["power"].append((sec, int(f[0]), int(f[18]) or int(f[17])))
            if len(f) >= 20:
                inst["relay"].append((sec, int(f[0]), int(f[19])))
        elif p[1] == "TWH" and len(f) >= 4:
            inst["fw"].add(f"{f[2]} (built {f[3]})")
    except ValueError:
        pass


def describe(inst, epoch, settle, thr, names, layout, own):
    """-> (one line for the title, rows for "About this recording", notes for power changes)"""
    dbm = lambda q: q / 4 if q else 20.0           # 0 = never set: full power
    per, changes = {}, []
    last = {}
    for sec, b, q in sorted(inst["power"]):
        per.setdefault(b, {}).setdefault(q, 0)
        per[b][q] += 1
        if b in last and last[b] != q:
            changes.append((sec, b, dbm(last[b]), dbm(q)))
        last[b] = q
    common = {b: dbm(max(c, key=c.get)) for b, c in sorted(per.items())}
    if not common:
        power = "transmit power not reported"
    elif len(set(common.values())) == 1:
        power = f"all boards at {next(iter(common.values())):g} dBm"
    else:
        power = "boards at " + ", ".join(f"{b}: {v:g}" for b, v in common.items()) + " dBm (not all the same)"
    if changes:
        power += f" for most of the run (changed {len(changes)} time{'s' * (len(changes) != 1)}: see About)"
    rel = {}
    for sec, b, v in inst["relay"]:
        rel[b] = v
    phy = max(inst["phy"], key=inst["phy"].get) if inst["phy"] else None
    bw = ""
    if phy is not None:
        ph = int(phy)
        bw = {0: "20 MHz", 1: "40 MHz"}.get(ph & 3, f"bandwidth code {ph & 3}")
        bw += {1: ", secondary channel above", 2: ", secondary channel below"}.get((ph >> 5) & 3, "")
    kit = "?"
    try:
        kit = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "VERSION")).read().strip()
    except OSError:
        pass
    rows = [
        ("firmware", ", ".join(sorted(inst["fw"])) or "not reported in this part"),
        ("transmit power", ", ".join(f"board {b} {v:g} dBm" for b, v in common.items()) or "not reported"),
        ("power changes", "; ".join(f"{dt.datetime.fromtimestamp(s):%H:%M:%S} board {b} {a:g} -> {c:g} dBm"
                                    for s, b, a, c in changes) or "none"),
        ("relaying", ", ".join(f"board {b} {'on' if v else 'off'}" for b, v in sorted(rel.items())) or "not reported"),
        ("channel", bw or "not reported"),
        ("layout", os.path.basename(layout) + (" kept in the run folder" if own else " as it is now (none in the run folder)")),
        ("thresholds", ", ".join(f"{n} {x:.2f}" for n, x in zip(names, thr))),
        ("shape settles after", f"{settle:g} min"),
        ("made by", f"replay.py, kit {kit}, {dt.datetime.now():%Y-%m-%d %H:%M}"),
    ]
    notes = [(s, f"board {b} power {a:g} -> {c:g} dBm") for s, b, a, c in changes]
    return power, rows, notes


def read_run(folder, t_from, t_to):
    files = sorted(glob.glob(os.path.join(folder, "csi-*.csv.gz")) + glob.glob(os.path.join(folder, "csi-*.csv")))
    if not files:
        sys.exit(f"no recording (csi-*.csv.gz) in {folder}")
    acc = Acc()
    acc.inst = dict(power=[], relay=[], fw=set(), phy={})   # the instrument, as the boards report it
    recent = {}                 # (rx, boot) -> set of record numbers seen lately (records come twice)
    total = dup = 0
    t_first = None
    for path in files:
        t0 = time.time()
        buf = {}
        nrec = 0

        def flush():
            for d, (ss, st, qq, rr) in buf.items():
                acc.add(d, np.array(ss, np.int64), np.array(st, np.float32),
                        np.frombuffer(b"".join(qq), np.uint8).reshape(len(qq), -1), np.array(rr, np.float32))
            buf.clear()
        for line in A.read_lines(path):
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split(",")
            if len(p) > 3 and p[1] in ("TWN", "TWS", "TWH"):
                instrument(acc.inst, p, t_from, t_to)
                continue
            if len(p) != 19 or p[1] != "TWR":
                continue
            try:
                sec = int(p[0]) // 1_000_000_000
                rx, tx, recseq, boot, n, step = int(p[2]), int(p[3]), int(p[6]), int(p[7]), int(p[16]), int(p[17])
                rssi = int(p[10])
            except ValueError:
                continue
            if t_first is None:
                t_first = sec
            if (t_from and sec < t_from) or (t_to and sec >= t_to):
                continue
            seen = recent.setdefault((rx, boot), set())
            if recseq in seen:
                dup += 1
                continue
            seen.add(recseq)
            if len(seen) > 20000:
                recent[(rx, boot)] = {x for x in seen if x > recseq - 10000}
            d = (tx, rx)
            if acc.nbins.setdefault(d, n) != n or len(p[18]) != 2 * n:
                continue
            acc.inst["phy"][p[14]] = acc.inst["phy"].get(p[14], 0) + 1
            b = buf.setdefault(d, ([], [], [], []))
            b[0].append(sec); b[1].append(step); b[2].append(bytes.fromhex(p[18])); b[3].append(rssi)
            nrec += 1
            if nrec % BATCH == 0:
                flush()
        flush()
        total += nrec
        if acc.hours:       # hours wholly before this file's last hour are complete
            last = max(h for _, h in acc.hours)
            acc.finish(before=last)
        print(f"  {os.path.basename(path)}: {nrec:,} records ({time.time() - t0:.0f} s)", file=sys.stderr)
    acc.finish()
    if not acc.done:
        sys.exit("no tw_node records (TWR lines) in that part of the run")
    return acc, total, dup, t_first


def assemble(acc):
    """-> seconds index (epoch per index), segments, and per direction arrays over it"""
    dirs = sorted({d for d, _ in acc.done})
    hours = sorted({h for _, h in acc.done})
    n_all = np.zeros((len(hours), 3600))
    for (d, h), D in acc.done.items():
        n_all[hours.index(h)] += D["n"]
    # keep from the first to the last second with records, hour by hour
    keep = []
    for hi, h in enumerate(hours):
        have = np.flatnonzero(n_all[hi] > 0)
        keep.append((h, int(have[0]), int(have[-1]) + 1) if len(have) else None)
    keep = [k for k in keep if k]
    epoch = np.concatenate([np.arange(h + a, h + b) for h, a, b in keep])
    nb = {d: acc.done[next(k for k in acc.done if k[0] == d)]["std"].shape[1] for d in dirs}
    out = {}
    for d in dirs:
        parts = {f: [] for f in ("mean", "std", "n", "r")}
        for h, a, b in keep:
            D = acc.done.get((d, h))
            for f in parts:
                if D is None:
                    shape = (b - a, nb[d]) if f in ("mean", "std") else (b - a,)
                    parts[f].append(np.zeros(shape, np.float32))
                else:
                    parts[f].append(D[f][a:b].astype(np.float32))
        out[d] = {f: np.concatenate(v) for f, v in parts.items()}
    # segments: runs of consecutive seconds
    br = np.flatnonzero(np.diff(epoch) != 1) + 1
    starts = np.concatenate([[0], br]); ends = np.concatenate([br, [len(epoch)]])
    segs = [(int(a), int(b)) for a, b in zip(starts, ends)]
    return dirs, epoch, segs, out


def recent_usual(mean, ok, epoch, settle):
    """per second, the link's usual shape lately: the median of the 2 x `settle` seconds
    before it, ending a minute earlier (so a change does not count against itself). A
    median turns once the new state fills half its window, so a change counts as usual
    `settle` (+1 minute) after it happened. Worked out every 30 s; where there is too
    little before (the start of the run, after a gap), the median of what comes after
    stands in."""
    base = np.full(mean.shape, np.nan, np.float32)
    lag, every = 60, 30
    for g in range(0, len(epoch), every):
        t = epoch[g]
        a, b = np.searchsorted(epoch, t - lag - 2 * settle), np.searchsorted(epoch, t - lag)
        w = np.flatnonzero(ok[a:b]) + a
        if len(w) < 60:
            a, b = g, np.searchsorted(epoch, t + settle)
            w = np.flatnonzero(ok[a:b]) + a
        if len(w) >= 10:
            base[g:g + every] = np.median(mean[w], axis=0)
    return base


def measures(dirs, out, epoch, settle):
    size, loss, shape, rssi = {}, {}, {}, {}
    for d in dirs:
        o = out[d]
        ok = o["n"] >= MIN_REC
        if ok.sum() < 60:
            ok = o["n"] >= max(5, np.percentile(o["n"], 50) * 0.6)
        ref = np.median(o["std"][ok], axis=0) if ok.any() else np.ones(o["std"].shape[1])
        s = (o["std"] / np.maximum(ref, 1e-3)).mean(axis=1)
        base = recent_usual(o["mean"], ok, epoch, settle)
        sh = np.sqrt(((o["mean"] - base) ** 2).mean(axis=1))
        expect = max(np.percentile(o["n"], 95), 1)
        size[d] = np.where(ok, s, np.nan)
        shape[d] = np.where(ok, sh, np.nan)
        loss[d] = np.clip(1 - o["n"] / expect, 0, 1) * 100
        rssi[d] = np.where(o["n"] > 0, o["r"], np.nan)
    return size, loss, shape, rssi


def find_events(dirs, epoch, size):
    S = np.array([np.nan_to_num(size[d], nan=1.0) for d in dirs])
    hours = epoch - epoch % 3600
    thr = []
    for k, d in enumerate(dirs):
        best = None
        for h in np.unique(hours):
            x = S[k][(hours == h) & ~np.isnan(size[d])]
            if len(x) < 1200:
                continue
            p90 = np.percentile(x, 90)
            if best is None or p90 < best[0]:
                med = np.median(x)
                mad = 1.4826 * np.median(np.abs(x - med))
                # the rare top of a quiet hour, but never above its usual spread x20:
                # a long event in a short run must not raise its own threshold
                best = (p90, min(np.percentile(x, 99.9), med + 20 * mad))
        thr.append(max(best[1] if best else 1.5, 1.3))
    thr = np.array(thr)
    R = S / thr[:, None]
    hit = (R > 1).any(axis=0)
    ev = []
    for i in np.flatnonzero(hit):
        if ev and epoch[i] - epoch[ev[-1][1]] <= 15:
            ev[-1][1] = i
        else:
            ev.append([i, i])
    keep = []
    for a, b in ev:
        if hit[a:b + 1].sum() >= 2 or R[:, a:b + 1].max() >= 1.5:
            pk = np.nanmax(S[:, a:b + 1], axis=1)
            keep.append(dict(i=int(a), j=int(b), pk=[round(float(x), 1) for x in pk],
                             lit=[bool(x > t) for x, t in zip(pk, thr)]))
    return keep, thr


def board_marks(dirs, size, thr):
    """per second: the board everything lit is near (0: none), and the directions lit one
    way only. Lit = past the direction's event threshold within the last 3 seconds.
    Near board X: everything lit touches X, on at least two of its paths, at least one of
    them lit both ways, and a path away from X is seen and quiet. One-sided: a direction
    lit while the other way of the same path is seen and clearly quiet (under a third of
    the way from quiet to its own threshold: thresholds differ, and a real change that
    just misses the other way's is not one-sided). Both hold for 3 seconds or more."""
    S = np.array([np.nan_to_num(size[d], nan=0.0) for d in dirs])
    seen = np.array([~np.isnan(size[d]) for d in dirs])
    thr = np.asarray(thr)[:, None]
    lit = S > thr
    lit3 = lit.copy()
    lit3[:, 1:] |= lit[:, :-1]
    lit3[:, 2:] |= lit[:, :-2]
    part = (S - 1) / np.maximum(thr - 1, 0.05)        # 0 quiet .. 1 at the threshold
    part3 = part.copy()
    part3[:, 1:] = np.maximum(part3[:, 1:], part[:, :-1])
    part3[:, 2:] = np.maximum(part3[:, 2:], part[:, :-2])

    def lasting(m, n=3):                # keep only runs of n seconds or more
        out = np.zeros_like(m)
        i = 0
        while i < len(m):
            if m[i]:
                j = i
                while j < len(m) and m[j] == m[i]:
                    j += 1
                if j - i >= n:
                    out[i:j] = m[i:j]
                i = j
            else:
                i += 1
        return out
    k = {d: i for i, d in enumerate(dirs)}
    one = np.zeros_like(lit3)
    for d, i in k.items():
        j = k.get((d[1], d[0]))
        if j is not None:
            one[i] = lasting(lit3[i] & (part3[j] < 1 / 3) & seen[j])
    near = np.zeros(S.shape[1], int)
    for x in sorted({b for d in dirs for b in d}):
        away = [k[d] for d in dirs if x not in d]
        partners = sorted({b for d in dirs for b in d if x in d and b != x})
        if not away or len(partners) < 2:
            continue
        quiet_away = ~lit3[away].any(axis=0) & seen[away].any(axis=0)
        n_lit = sum((lit3[k[(x, p)]] if (x, p) in k else 0) | (lit3[k[(p, x)]] if (p, x) in k else 0)
                    for p in partners)
        both = np.zeros(S.shape[1], bool)
        for p in partners:
            if (x, p) in k and (p, x) in k:
                both |= lit3[k[(x, p)]] & lit3[k[(p, x)]]
        near[quiet_away & (n_lit >= 2) & both] = x
    return lasting(near), one


def find_peaks(dirs, epoch, size, loss, events, near):
    """inside an event of 2 minutes or more (a stretch: a link that stays restless), the
    moments that rise well above the stretch's own recent level: at least two directions
    at 1.8 x their level of the 5 minutes before (ending 30 s earlier), or a jump in loss
    of 20 points. A peak with only one direction's loss behind it is marked weak."""
    S = np.array([size[d] for d in dirs])
    L = np.array([loss[d] for d in dirs])
    for e in events:
        i0, j0 = e["i"], e["j"]
        if epoch[j0] - epoch[i0] < 120:
            continue
        e["stretch"] = True
        med = np.nanmedian(S[:, i0:j0 + 1], axis=1)
        k = int(np.nanargmax(med))
        e["level"] = [k, round(float(med[k]), 1)]
        hits = []
        for i in range(i0, j0 + 1):
            a = int(np.searchsorted(epoch, epoch[i] - 300)); b = int(np.searchsorted(epoch, epoch[i] - 30))
            if b <= a:
                continue
            with np.errstate(all="ignore"):
                lev = np.nanmedian(S[:, a:b], axis=1)
                levl = np.nanmedian(L[:, a:b], axis=1)
            up = {d for d in range(len(dirs)) if S[d, i] > 1.3 and S[d, i] >= 1.8 * lev[d]}
            lj = {d for d in range(len(dirs)) if L[d, i] - levl[d] >= 20}
            if len(up) >= 2 or lj:
                hits.append((i, up, lj))
        subs = []
        for i, up, lj in hits:
            if subs and epoch[i] - epoch[subs[-1]["j"]] <= 15:
                s = subs[-1]; s["j"] = i; s["up"] |= up; s["lj"] |= lj
            else:
                subs.append(dict(i=i, j=i, up=set(up), lj=set(lj)))
        e["subs"] = []
        for s in subs:
            if epoch[s["j"]] - epoch[s["i"]] < 2:
                continue
            seg = near[s["i"]:s["j"] + 1]
            vals, cnt = np.unique(seg[seg > 0], return_counts=True)
            e["subs"].append(dict(i=s["i"], j=s["j"], up=sorted(s["up"]), lj=sorted(s["lj"]),
                                  pk=[round(float(np.nanmax(S[d, s["i"]:s["j"] + 1])), 1) for d in range(len(dirs))],
                                  weak=not s["up"] and len(s["lj"]) == 1,
                                  near=int(vals[cnt.argmax()]) if len(vals) and cnt.max() >= 2 else 0))


def read_camera(folder, epoch):
    """a watch run's camera (watch.sh): its events (detect/events.csv, with your label and
    note) and the frames kept around them (detect/events/<event>/*.jpg, one a second, named
    by the time); the frames are linked from the page, not copied into it"""
    import csv
    det = os.path.join(folder, "detect")
    events, frames = [], []
    try:
        rows = list(csv.DictReader(open(os.path.join(det, "events.csv"))))
    except OSError:
        return events, frames

    def index(t):
        i = int(np.searchsorted(epoch, t))
        for k in (i, i - 1):
            if 0 <= k < len(epoch) and abs(int(epoch[k]) - t) <= 1:
                return k
        return None
    for r in rows:
        eid = (r.get("event") or "").strip()
        try:
            t0 = int(dt.datetime.strptime(eid, "%Y%m%d-%H%M%S").timestamp())
            h, m, s = (int(x) for x in r.get("end", "").split(":"))
            end = dt.datetime.fromtimestamp(t0).replace(hour=h, minute=m, second=s)
            t1 = int(end.timestamp()) + (86400 if end.timestamp() < t0 else 0)
        except ValueError:
            continue
        i = int(np.searchsorted(epoch, t0)); j = int(np.searchsorted(epoch, t1))
        if i >= len(epoch) or j <= 0:
            continue
        events.append(dict(i=i, j=max(i, min(j, len(epoch) - 1)), event=eid, kind=r.get("kind", ""),
                           label=r.get("label", "").strip(), note=r.get("note", "").strip()))
        for f in sorted(glob.glob(os.path.join(det, "events", eid, "*.jpg"))):
            try:
                tf = int(dt.datetime.strptime(os.path.basename(f)[:15], "%Y%m%d-%H%M%S").timestamp())
            except ValueError:
                continue
            k = index(tf)
            if k is not None:
                frames.append((k, os.path.relpath(f, folder)))
    frames = sorted(set(frames))
    return events, [dict(i=k, src=src) for k, src in frames]


def find_steps(dirs, epoch, out, rssi):
    """links whose average shape steps by >= 1 dB, or whose RSSI steps by >= 3 dB, in both
    directions, and stays"""
    steps = []
    pairs = sorted({tuple(sorted(d)) for d in dirs})
    W, G, C = 300, 30, 10           # window, guard, grid (s)
    for a, b in pairs:
        ds = [d for d in ((a, b), (b, a)) if d in out]
        mean = {d: out[d]["mean"] for d in ds}
        ok = {d: out[d]["n"] >= MIN_REC for d in ds}
        grid = np.arange(W + G, len(epoch) - W - G, C)
        score = np.zeros(len(grid)); rscore = np.zeros(len(grid))
        for gi, i in enumerate(grid):
            if epoch[i + W + G] - epoch[i - W - G] != 2 * (W + G):
                continue                        # a gap in the recording: no step across it
            sc, rs = [], []
            for d in ds:
                pre = ok[d][i - W - G:i - G]; post = ok[d][i + G:i + W + G]
                if pre.sum() < 60 or post.sum() < 60:
                    sc = []
                    break
                m0 = np.median(mean[d][i - W - G:i - G][pre], axis=0)
                m1 = np.median(mean[d][i + G:i + W + G][post], axis=0)
                sc.append(float(np.sqrt(((m1 - m0) ** 2).mean())))
                rs.append(float(np.median(rssi[d][i + G:i + W + G][post]) - np.median(rssi[d][i - W - G:i - G][pre])))
            if sc:
                score[gi] = min(sc)
                rscore[gi] = min(abs(x) for x in rs) if all(x > 0 for x in rs) or all(x < 0 for x in rs) else 0
        both = np.maximum(score / 1.0, rscore / 3.0)      # >= 1: a lasting change
        for gi in np.flatnonzero(both >= 1.0):
            lo, hi = max(0, gi - W // C), min(len(both), gi + W // C + 1)
            if both[gi] < both[lo:hi].max():
                continue
            i = int(grid[gi])
            # the second it happened: where the distance from "before" first passes halfway
            d0 = ds[0]
            pre = ok[d0][i - W - G:i - G]
            if score[gi] >= 1.0:
                m0 = np.median(mean[d0][i - W - G:i - G][pre], axis=0)
                dist = np.sqrt(((mean[d0][i - G:i + G + W // 2] - m0) ** 2).mean(axis=1))
                half = score[gi] / 2
            else:
                dist = np.abs(rssi[d0][i - G:i + G + W // 2] - np.median(rssi[d0][i - W - G:i - G][pre]))
                half = rscore[gi] / 2
            dist[~ok[d0][i - G:i + G + W // 2]] = np.nan
            past = np.nan_to_num(dist) >= half
            cross = np.flatnonzero(past[:-2] & past[1:-1] & past[2:])
            at = i - G + int(cross[0]) if len(cross) else i
            dr = [np.nanmean(rssi[d][i + G:i + W + G]) - np.nanmean(rssi[d][i - W - G:i - G]) for d in ds]
            steps.append(dict(i=at, link=f"{a}-{b}", db=round(score[gi], 1), rssi=round(float(np.nanmean(dr)), 1)))
    steps.sort(key=lambda s: s["i"])
    merged = []                     # one step per link within the window (a plateau ties)
    for s in steps:
        m = next((m for m in merged if m["link"] == s["link"] and s["i"] - m["i"] <= W), None)
        if m is None:
            merged.append(s)
        elif (s["db"], abs(s["rssi"])) > (m["db"], abs(m["rssi"])):
            m.update(db=s["db"], rssi=s["rssi"])
    return merged


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--title")
    ap.add_argument("--from", dest="t_from", help="HH:MM: start here")
    ap.add_argument("--to", dest="t_to", help="HH:MM: stop here")
    ap.add_argument("--note", action="append", default=[], help='"HH:MM[:SS] text": a mark of your own')
    ap.add_argument("--out", help="the page to write (default RUN_FOLDER/replay.html)")
    ap.add_argument("--settle", type=float, default=15, metavar="MIN",
                    help="minutes after which a new state counts as usual (the orange mark clears); default 15")
    ap.add_argument("--view", choices=("waves", "lines"), default="waves",
                    help="how the page opens: waves (each board a beacon, a fan per path) or lines (one line per direction)")
    a = ap.parse_args()
    folder = os.path.abspath(os.path.expanduser(a.folder))
    title = a.title
    if title is None:
        if sys.stdin.isatty():
            try:
                title = input("Title for the page [Link replay]: ").strip()
            except EOFError:
                title = ""
        title = title or "Link replay"

    layout = tw_map.find_boards(folder)
    boards, marks = tw_map.load_boards(layout)
    if not boards:
        sys.exit("no boards.csv (in the run folder, or the one you keep: tw_layout.py path); see tw_map.py for the format")
    own = os.path.dirname(os.path.abspath(layout)) == folder
    if not own:
        print(f"note: no layout kept in the run folder; drawn with {layout} as it is now", file=sys.stderr)

    # the run's first second, to place --from/--to/--note
    first = None
    files = sorted(glob.glob(os.path.join(folder, "csi-*.csv*")))
    for line in A.read_lines(files[0]) if files else []:
        m = re.match(r"(\d{19}),TW", line)
        if m:
            first = int(m[1]) // 1_000_000_000
            break
    if first is None:
        sys.exit(f"no tw_node records in {folder}")
    t_from = next_at(first, parse_clock(a.t_from)) if a.t_from else None
    t_to = next_at(t_from or first, parse_clock(a.t_to)) if a.t_to else None

    print(f"reading {folder}" + (f" from {a.t_from}" if a.t_from else "") + (f" to {a.t_to}" if a.t_to else ""), file=sys.stderr)
    acc, total, dup, _ = read_run(folder, t_from, t_to)
    dirs, epoch, segs, out = assemble(acc)
    size, loss, shape, rssi = measures(dirs, out, epoch, a.settle * 60)
    events, thr = find_events(dirs, epoch, size)
    steps = find_steps(dirs, epoch, out, rssi)
    near, one = board_marks(dirs, size, thr)
    for e in events:                    # an event mostly near one board says so
        seg = near[e["i"]:e["j"] + 1]
        vals, cnt = np.unique(seg[seg > 0], return_counts=True)
        if len(vals) and cnt.max() >= max(2, 0.25 * len(seg)):
            e["near"] = int(vals[cnt.argmax()])
    find_peaks(dirs, epoch, size, loss, events, near)
    cam_events, cam_frames = read_camera(folder, epoch)
    names = [f"{tx}-{rx}" for tx, rx in dirs]
    power, about, power_notes = describe(acc.inst, epoch, a.settle, thr, names, layout, own)

    notes = []
    for s in a.note:
        m = re.match(r"\s*(\d{1,2}:\d{2}(?::\d{2})?)\s*(.*)", s)
        if not m:
            print(f"note skipped (start it with HH:MM): {s}", file=sys.stderr)
            continue
        t = next_at(int(epoch[0]) - 3600, parse_clock(m[1]))
        i = int(np.searchsorted(epoch, t))
        if i < len(epoch) and abs(int(epoch[i]) - t) <= 60:
            notes.append(dict(i=i, label=m[2] or m[1]))
        else:
            print(f"note outside the recorded part, skipped: {s}", file=sys.stderr)
    # a survey's own marks (m: placement.csv, one row per link) and boards moved (moves.csv)
    import csv
    for name, text in (("placement.csv", lambda r: r.get("label", "")),
                       ("moves.csv", lambda r: f"board {r['board']} moved to {r['x_ft']},{r['y_ft']}")):
        seen = set()
        try:
            for r in csv.DictReader(open(os.path.join(folder, name))):
                try:
                    t = int(float(r["time"]))
                    label = text(r)
                except (KeyError, ValueError):
                    continue
                if (t, label) in seen or not label:
                    continue
                seen.add((t, label))
                i = int(np.searchsorted(epoch, t))
                if i < len(epoch) and abs(int(epoch[i]) - t) <= 60:
                    notes.append(dict(i=i, label=label))
        except OSError:
            pass
    for t_, label in power_notes:       # the instrument changed during the run
        i = int(np.searchsorted(epoch, t_))
        if i < len(epoch):
            notes.append(dict(i=i, label=label))
    notes.sort(key=lambda n: n["i"])

    scene = ""
    sp = os.path.join(folder, "scene.txt")
    if os.path.exists(sp):
        lines = open(sp, errors="replace").read().strip().splitlines()
        scene = lines[0][:160] if lines else ""

    def q(x, k, mx):
        return np.where(np.isnan(x), -1, np.clip(np.round(x * k), 0, mx)).astype(int).tolist()
    loc = lambda t: dt.datetime.fromtimestamp(int(t))
    span = f"{loc(epoch[0]):%Y-%m-%d %H:%M} to {loc(epoch[-1]):%H:%M}"
    data = dict(
        title=title, view=a.view,
        about=about, camera=dict(events=cam_events, frames=cam_frames),
        sub=" · ".join(x for x in [os.path.basename(folder), span, f"{len(boards)} boards", power, scene,
                                    "" if own else "layout as it is now (none kept in the run folder)"] if x),
        segs=[dict(a=a_, n=b_ - a_, t=int(epoch[a_]),
                   off=int(loc(epoch[a_]).astimezone().utcoffset().total_seconds())) for a_, b_ in segs],
        dirs=names,
        size=[q(size[d], 20, 400) for d in dirs],
        loss=[q(loss[d], 1, 100) for d in dirs],
        shape=[q(shape[d], 10, 300) for d in dirs],
        events=[dict(n=k + 1, **e) for k, e in enumerate(events)],
        near=near.tolist(), one=["".join("1" if x else "0" for x in row) for row in one],
        steps=steps, notes=notes,
        boards=[dict(id=str(k), x=v[0], y=v[1], label=v[3]) for k, v in sorted(boards.items())],
        marks=[dict(x=m[1], y=m[2], label=m[3]) for m in marks],
    )
    page = PAGE.replace("__TITLE__", html.escape(title)).replace("__SETTLE__", f"{a.settle:g}").replace(
        "__DATA__", json.dumps(data, separators=(",", ":")).replace("</", "<\\/"))
    dest = a.out or os.path.join(folder, "replay.html")
    with open(dest, "w") as f:
        f.write(page)

    print(f"\n{dest}  ({os.path.getsize(dest) // 1024} KB)  layout: {layout}")
    print(f"{total:,} records ({dup:,} second copies skipped); {len(names)} directions; "
          f"{len(epoch) // 3600} h {len(epoch) % 3600 // 60} min in {len(segs)} part(s)")
    print("thresholds: " + "  ".join(f"{n} {t:.2f}" for n, t in zip(names, thr)))
    nb = {x: int((near == x).sum()) for x in np.unique(near[near > 0])}
    print(power)
    if cam_events:
        print(f"camera: {len(cam_events)} watch events, {len(cam_frames)} frames linked "
              f"(the page shows them while it sits in this folder)")
    for e in events:
        if e.get("stretch"):
            print(f"stretch {loc(epoch[e['i']]):%H:%M:%S}, {int(epoch[e['j']] - epoch[e['i']]) + 1} s: "
                  f"{names[e['level'][0]]} restless ({e['level'][1]}x quiet); peaks inside: "
                  + (", ".join(f"{loc(epoch[s['i']]):%H:%M:%S}" + (" (weak)" if s["weak"] else "") for s in e["subs"]) or "none"))
    print(f"{len(events)} events" + "".join(f"; near board {x}: {n} s" for x, n in nb.items())
          + "; lasting changes:" + ("" if steps else " none"))
    for s in steps:
        print(f"  {loc(epoch[s['i']]):%H:%M:%S}  link {s['link']}: shape {s['db']} dB, RSSI {s['rssi']:+.1f} dB")


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
:root {
  --page: #f9f9f7; --surface: #fcfcfb; --ink: #0b0b0b; --ink2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --axis: #c3c2b7; --quiet: #d6d5ce;
  --s1: #cde2fb; --s2: #86b6ef; --s3: #3987e5; --s4: #1c5cab; --s5: #0d366b;
  --shape: #eb6834; --note: #0b0b0b; --sel: #fde7c2; --cam: #6250d6;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --page: #0d0d0d; --surface: #1a1a19; --ink: #f0efec; --ink2: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --axis: #383835; --quiet: #3a3a37;
    --s1: #184f95; --s2: #2a78d6; --s3: #5598e7; --s4: #9ec5f4; --s5: #e3eefc;
    --shape: #d95926; --note: #f0efec; --sel: #4a3410; --cam: #9085e9;
  }
}
:root[data-theme="dark"] {
  --page: #0d0d0d; --surface: #1a1a19; --ink: #f0efec; --ink2: #c3c2b7; --muted: #898781;
  --grid: #2c2c2a; --axis: #383835; --quiet: #3a3a37;
  --s1: #184f95; --s2: #2a78d6; --s3: #5598e7; --s4: #9ec5f4; --s5: #e3eefc;
  --shape: #d95926; --note: #f0efec; --sel: #4a3410; --cam: #9085e9;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--page); color: var(--ink);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
header { padding: 14px 16px 6px; }
h1 { font-size: 18px; margin: 0 0 2px; font-weight: 650; }
.sub { color: var(--ink2); font-size: 13px; }
.wrap { display: grid; grid-template-columns: minmax(0, 1.25fr) minmax(280px, 1fr); gap: 12px; padding: 8px 16px; }
@media (max-width: 820px) { .wrap { grid-template-columns: 1fr; } }
.card { background: var(--surface); border: 1px solid var(--grid); border-radius: 10px; padding: 10px 12px; min-width: 0; }
.card h2 { font-size: 13px; font-weight: 600; margin: 0 0 6px; color: var(--ink2); text-transform: uppercase; letter-spacing: .04em; }
svg text { fill: var(--ink); }
#map { width: 100%; height: auto; display: block; }
.controls { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; padding: 4px 16px 0; }
.controls button, .controls select { font: inherit; color: var(--ink); background: var(--surface);
  border: 1px solid var(--axis); border-radius: 7px; padding: 5px 10px; cursor: pointer; }
.controls button:hover { border-color: var(--ink2); }
#clock { font: 600 22px/1 ui-monospace, SFMono-Regular, Menlo, monospace; min-width: 112px; }
#nowev { color: var(--ink2); font-size: 13px; }
.tlwrap { padding: 8px 16px 4px; }
#tl { width: 100%; display: block; background: var(--surface); border: 1px solid var(--grid);
  border-radius: 10px; cursor: crosshair; touch-action: none; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
th, td { text-align: right; padding: 3px 6px; border-bottom: 1px solid var(--grid); }
th:first-child, td:first-child { text-align: left; }
th { color: var(--muted); font-weight: 500; font-size: 12px; }
.sw { display: inline-block; width: 14px; height: 6px; border-radius: 3px; vertical-align: middle; margin-right: 6px; }
#evlist { max-height: 300px; overflow: auto; margin-top: 4px; }
.ev { display: grid; grid-template-columns: 46px 66px 50px 1fr; gap: 6px; padding: 3px 4px; border-radius: 6px;
  cursor: pointer; font-variant-numeric: tabular-nums; font-size: 13px; }
.ev:hover { background: var(--grid); }
.ev.on { background: var(--sel); }
.ev.st span:last-child { color: var(--ink2); }
.ev.sub { padding-left: 18px; grid-template-columns: 46px 66px 50px 1fr; }
details.about { margin-top: 8px; }
details.about summary { cursor: pointer; color: var(--ink); }
details.about table { width: auto; margin-top: 6px; font-size: 12px; }
details.about th { text-align: left; color: var(--muted); font-weight: 500; padding-right: 14px; }
details.about td { text-align: left; }
.ev .m { color: var(--muted); }
.legend { display: flex; flex-wrap: wrap; gap: 14px; color: var(--ink2); font-size: 12px; margin-top: 6px; align-items: center; }
.ramp { display: inline-block; width: 110px; height: 8px; border-radius: 4px; vertical-align: middle;
  background: linear-gradient(90deg, var(--quiet), var(--s1), var(--s2), var(--s3), var(--s4), var(--s5)); }
#tip { position: fixed; pointer-events: none; background: var(--surface); color: var(--ink); border: 1px solid var(--axis);
  border-radius: 8px; padding: 6px 9px; font-size: 12px; box-shadow: 0 4px 14px rgba(0,0,0,.15); display: none; z-index: 9;
  font-variant-numeric: tabular-nums; max-width: 280px; }
.notes { color: var(--ink2); font-size: 13px; padding: 2px 16px 16px; }
.notes p { margin: 4px 0; }
#camcard img { width: 100%; display: block; border-radius: 8px; background: var(--grid); aspect-ratio: 4 / 3; object-fit: contain; }
#camnote { color: var(--ink2); font-size: 12px; margin: 4px 0 10px; }
#camnote button { font: inherit; font-size: 12px; color: var(--ink); background: var(--surface); border: 1px solid var(--axis); border-radius: 6px; padding: 1px 8px; cursor: pointer; margin-left: 6px; }
.cam { color: var(--cam); font-weight: 600; font-size: 11px; margin-left: 4px; }
.os { color: var(--muted); font-size: 11px; margin-left: 6px; }
.nr { font-weight: 600; color: var(--ink); }
.maphead { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 6px; }
.maphead h2 { margin: 0; }
.seg { display: inline-flex; border: 1px solid var(--axis); border-radius: 7px; overflow: hidden; }
.seg button { font: inherit; font-size: 12px; color: var(--ink2); background: var(--surface); border: 0; padding: 3px 10px; cursor: pointer; }
.seg button.on { background: var(--ink); color: var(--surface); }
body.v-waves .lg-lines, body.v-lines .lg-waves { display: none; }
kbd { font: 11px ui-monospace, monospace; border: 1px solid var(--axis); border-radius: 4px; padding: 0 4px; }
</style>
</head>
<body>
<header>
  <h1 id="title"></h1>
  <div class="sub" id="sub"></div>
</header>
<div class="controls">
  <button id="play" aria-label="Play">&#9654; Play</button>
  <select id="speed" aria-label="Speed">
    <option value="1">1&times;</option><option value="10">10&times;</option>
    <option value="60" selected>60&times;</option><option value="300">300&times;</option>
  </select>
  <button id="prev" title="Previous event (p)">&#8592; event</button>
  <button id="next" title="Next event (n)">event &#8594;</button>
  <span id="clock">00:00:00</span>
  <span id="nowev"></span>
</div>
<div class="tlwrap"><canvas id="tl" aria-label="Timeline: change per link direction across the run"></canvas></div>
<div class="wrap">
  <div class="card">
    <div class="maphead"><h2>Map</h2>
      <div class="seg" role="group" aria-label="Map view"><button id="vWaves" type="button">Waves</button><button id="vLines" type="button">Lines</button></div></div>
    <svg id="map" role="img" aria-label="The boards and their links, to scale"></svg>
    <div class="legend">
      <span><span class="ramp"></span>&nbsp; change: quiet &rarr; 6&times; its quiet level or more</span>
      <span><svg width="26" height="8"><line x1="1" y1="4" x2="25" y2="4" stroke="var(--shape)" stroke-width="3" stroke-dasharray="4 3"/></svg> shape moved &ge;1.5 dB from its recent usual (something standing or parked differently; clears once it has stayed __SETTLE__ min)</span>
      <span><svg width="26" height="8"><line x1="1" y1="4" x2="25" y2="4" stroke="var(--muted)" stroke-width="3" stroke-dasharray="1 3" stroke-linecap="round"/></svg> no data that second (frames lost)</span>
      <span><svg width="26" height="14"><circle cx="13" cy="7" r="5.5" fill="none" stroke="var(--s3)" stroke-width="2"/><circle cx="13" cy="7" r="2" fill="var(--ink)"/></svg> rings around a board: everything lit touches that board and the path away from it is quiet: something near it, on any side (behind it too)</span>
      <span><svg width="26" height="8"><line x1="2" y1="4" x2="24" y2="4" stroke="var(--s3)" stroke-width="5" stroke-linecap="round"/><line x1="2" y1="4" x2="24" y2="4" stroke="var(--surface)" stroke-width="2" stroke-linecap="round"/></svg> hollow: lit one way only, the other way quiet: more likely a board&rsquo;s own quirk than the room</span>
      <span class="lg-lines">two lines per link: one per direction, arrow from the sender to the board that measures</span>
      <span class="lg-waves">each board is a beacon: one fan per path, toward the other board, for its own signal reaching it; up to 3 arcs lit = how much it changed; echoes along the path while playing. Both ends of a path lit: something real; one end only: a board&rsquo;s own quirk</span>
    </div>
  </div>
  <div class="card">
    <div id="camcard" hidden><h2>Camera</h2><img id="camimg" alt="the camera's frame at this second"><div id="camnote"></div></div>
    <h2>Now</h2>
    <table id="now"><thead><tr><th>direction</th><th>change &times;</th><th>loss %</th><th>shape dB</th></tr></thead><tbody></tbody></table>
    <h2 style="margin-top:12px">Lasting changes and notes</h2>
    <div id="marklist"></div>
    <h2 style="margin-top:12px">Events</h2>
    <div id="evlist"></div>
  </div>
</div>
<div class="notes">
  <p><b>What this shows:</b> which links were disturbed, when, and how much. It cannot place a person: a lit link means something moved in or near that path, or (with no shape change) somewhere its signal reflects through. Orange marks on the timeline are lasting changes (a link&rsquo;s shape stepped and stayed: something parked, moved or opened); black marks are your notes; violet marks are the camera&rsquo;s events (watch runs), with your label.</p>
  <details class="about"><summary><b>About this recording</b>: the instrument as the boards reported it</summary><table id="abouttbl"></table></details>
  <p><kbd>space</kbd> play/pause &middot; <kbd>&larr;</kbd><kbd>&rarr;</kbd> &plusmn;1 s (shift &plusmn;1 min) &middot; <kbd>n</kbd><kbd>p</kbd> next/previous event &middot; click or drag the timeline to jump.</p>
</div>
<div id="tip"></div>
<script>
const D = __DATA__;
document.getElementById("title").textContent = D.title;
document.getElementById("sub").textContent = D.sub;
const NAMES = D.dirs, ND = NAMES.length;
const N = D.size[0].length;
const pad = n => String(n).padStart(2, "0");
const segOf = i => { let k = 0; while (k + 1 < D.segs.length && i >= D.segs[k + 1].a) k++; return D.segs[k]; };
const tOf = i => { const s = segOf(i); return s.t + (i - s.a); };
function clk(i) { const s = segOf(i); const d = new Date((s.t + (i - s.a) + s.off) * 1000);
  return pad(d.getUTCHours()) + ":" + pad(d.getUTCMinutes()) + ":" + pad(d.getUTCSeconds()); }
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function hex(c) { const v = parseInt(c.slice(1), 16); return [v >> 16, (v >> 8) & 255, v & 255]; }
let RAMP;
function readRamp() { RAMP = ["--quiet", "--s1", "--s2", "--s3", "--s4", "--s5"].map(n => hex(css(n))); }
function colorFor(v) {
  if (v < 0) return null;
  let f = v < 1.15 ? 0 : Math.log(v) / Math.log(6);
  f = Math.min(Math.max(f, 0), 1) * (RAMP.length - 1);
  const a = Math.floor(f), b = Math.min(a + 1, RAMP.length - 1), w = f - a;
  const c = RAMP[a].map((x, k) => Math.round(x + (RAMP[b][k] - x) * w));
  return `rgb(${c[0]},${c[1]},${c[2]})`;
}
const raw = (arr, d, i) => arr[d][i];
const sizeAt = (d, i) => { const x = D.size[d][i]; return x < 0 ? -1 : x / 20; };

/* ---------- map ---------- */
const svg = document.getElementById("map"), NS = "http://www.w3.org/2000/svg";
function el(tag, attrs, parent) { const e = document.createElementNS(NS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); (parent || svg).appendChild(e); return e; }
const B = {}; D.boards.forEach(b => B[b.id] = b);
const pts = D.boards.concat(D.marks);
const xs = pts.map(p => p.x), ys = pts.map(p => p.y);
const span = Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...ys) - Math.min(...ys), 1);
const P = span * 0.16 + 2, FS = span / 26;
const x0 = Math.min(...xs) - P - span * 0.12, x1 = Math.max(...xs) + P + span * 0.12, y0 = Math.min(...ys) - P, y1 = Math.max(...ys) + P;
svg.setAttribute("viewBox", `${x0} ${-y1} ${x1 - x0} ${y1 - y0}`);
const step = span <= 60 ? 5 : span <= 150 ? 10 : 50;
const g0 = el("g", {});
for (let x = Math.ceil(x0 / step) * step; x <= x1; x += step) el("line", { x1: x, y1: -y1, x2: x, y2: -y0, stroke: "var(--grid)", "stroke-width": span / 500 }, g0);
for (let y = Math.ceil(y0 / step) * step; y <= y1; y += step) el("line", { x1: x0, y1: -y, x2: x1, y2: -y, stroke: "var(--grid)", "stroke-width": span / 500 }, g0);
el("text", { x: x1 - FS * .4, y: -y1 + FS * 1.3, "font-size": FS * .85, "text-anchor": "end", fill: "var(--muted)" }, g0).textContent = `N ↑  grid ${step}`;
const defs = el("defs", {});
const mk = el("marker", { id: "arr", viewBox: "0 0 10 10", refX: 5, refY: 5, markerWidth: FS * 1.2, markerHeight: FS * 1.2, markerUnits: "userSpaceOnUse", orient: "auto" }, defs);
el("path", { d: "M0,1 L9,5 L0,9 z", fill: "context-stroke" }, mk);
const OFF = span / 55, W0 = span / 85;
const gLines = el("g", {}), gWaves = el("g", {});
/* waves: each board a beacon, one fan of 3 arcs per path, toward the other board */
const FAN = { r: [1.55, 2.3, 3.05].map(k => k * FS), half: 32 * Math.PI / 180, w: FS * .28 };
function arcPath(cxp, cyp, R, th, half) {
  const a = th - half, b = th + half;
  return `M${cxp + R * Math.cos(a)},${cyp + R * Math.sin(a)} A${R},${R} 0 0 1 ${cxp + R * Math.cos(b)},${cyp + R * Math.sin(b)}`;
}
[...new Set(NAMES.map(nm => nm.split("-").sort().join("-")))].forEach(pr => {     // a faint path under each pair
  const [s, r] = pr.split("-"); if (!B[s] || !B[r]) return;
  el("line", { x1: B[s].x, y1: -B[s].y, x2: B[r].x, y2: -B[r].y, stroke: "var(--axis)", "stroke-width": W0 * .45, "stroke-dasharray": `${W0} ${W0 * 1.2}` }, gWaves);
});
const angTo = (a, b) => Math.atan2(-(b.y - a.y), b.x - a.x);
const fanEls = NAMES.map((nm, d) => {
  const [s, r] = nm.split("-"); const a = B[s], b = B[r];
  if (!a || !b) return null;
  const ax = a.x, ay = -a.y, L = Math.hypot(b.x - a.x, b.y - a.y) || 1;
  const th = angTo(a, b);
  // narrow the fan to fit beside this board's other fans
  let gap = Math.PI;
  NAMES.forEach(o => { const [s2, r2] = o.split("-"); if (s2 !== s || r2 === r || !B[r2]) return;
    let dA = Math.abs(angTo(a, B[r2]) - th); if (dA > Math.PI) dA = 2 * Math.PI - dA; gap = Math.min(gap, dA); });
  const half = Math.max(Math.min(FAN.half, gap * 0.42), 8 * Math.PI / 180);
  const g = el("g", {}, gWaves);
  const shape = el("path", { d: arcPath(ax, ay, FAN.r[2] + FS * .7, th, half), fill: "none", stroke: "var(--shape)", "stroke-width": FAN.w * 1.1, "stroke-dasharray": `${FAN.w * 1.6} ${FAN.w}`, opacity: 0 }, g);
  const arcs = FAN.r.map(R => el("path", { d: arcPath(ax, ay, R, th, half), fill: "none", stroke: "var(--quiet)", "stroke-width": FAN.w, "stroke-linecap": "round" }, g));
  const hollow = FAN.r.map(R => el("path", { d: arcPath(ax, ay, R, th, half), fill: "none", stroke: "var(--surface)", "stroke-width": FAN.w * .42, "stroke-linecap": "round", opacity: 0 }, g));
  const echo = el("path", { d: "", fill: "none", stroke: "var(--quiet)", "stroke-width": FAN.w, "stroke-linecap": "round", opacity: 0 }, g);
  const R = FAN.r[2] + FS * .9;          // hover: the wedge the fan sits in
  const hit = el("path", { d: `M${ax},${ay} L${ax + R * Math.cos(th - half)},${ay + R * Math.sin(th - half)} A${R},${R} 0 0 1 ${ax + R * Math.cos(th + half)},${ay + R * Math.sin(th + half)} Z`, fill: "transparent" }, g);
  hit.addEventListener("mousemove", e => showTip(e, dirTip(d, cur)));
  hit.addEventListener("mouseleave", hideTip);
  return { g, arcs, hollow, echo, shape, ax, ay, th, half, end: Math.max(L / 2 - FS * .3, FAN.r[2] + FS), ph: Math.random(), lvl: 0, f: 0, c: "" };
});
const linkEls = NAMES.map((nm, d) => {
  const [s, r] = nm.split("-"); const a = B[s], b = B[r];
  if (!a || !b) return null;                         // a board missing from boards.csv
  const dx = b.x - a.x, dy = b.y - a.y, L = Math.hypot(dx, dy) || 1;
  const ox = dy / L * OFF, oy = -dx / L * OFF;
  const p = (x, y) => `${x + ox},${-(y + oy)}`;
  const pl = `${p(a.x, a.y)} ${p((a.x + b.x) / 2, (a.y + b.y) / 2)} ${p(b.x, b.y)}`;
  const halo = el("polyline", { points: pl, fill: "none", stroke: "var(--shape)", "stroke-width": W0 * 2.2, "stroke-dasharray": `${W0 * 2} ${W0 * 1.4}`, opacity: 0 }, gLines);
  const line = el("polyline", { points: pl, fill: "none", stroke: "var(--quiet)", "stroke-width": W0, "stroke-linecap": "round", "marker-mid": "url(#arr)" }, gLines);
  const hollow = el("polyline", { points: pl, fill: "none", stroke: "var(--surface)", "stroke-width": W0 * .45, "stroke-linecap": "round", opacity: 0 }, gLines);
  const hit = el("polyline", { points: pl, fill: "none", stroke: "transparent", "stroke-width": W0 * 4 }, gLines);
  hit.addEventListener("mousemove", e => showTip(e, dirTip(d, cur)));
  hit.addEventListener("mouseleave", hideTip);
  return { line, halo, hollow };
});
/* rings: everything lit is near one board (on any side of it, behind it too) */
const gRings = el("g", {});
const ringEls = {};
D.boards.forEach(b => {
  const g = el("g", { style: "display:none" }, gRings);
  const rings = FAN.r.map(R => el("circle", { cx: b.x, cy: -b.y, r: R, fill: "none", stroke: "var(--quiet)", "stroke-width": FAN.w }, g));
  const echo = el("circle", { cx: b.x, cy: -b.y, r: FAN.r[2], fill: "none", stroke: "var(--quiet)", "stroke-width": FAN.w, opacity: 0 }, g);
  el("text", { x: b.x, y: -b.y - FAN.r[2] - FS * .55, "font-size": FS * .8, "text-anchor": "middle", fill: "var(--ink2)", "font-weight": 600 }, g).textContent = `near ${b.id}`;
  ringEls[b.id] = { g, rings, echo, ph: 0, c: "", f: 0, lvl: 0 };
});
const isOne = (d, i) => D.one[d][i] === "1";
const cxm = (Math.min(...xs) + Math.max(...xs)) / 2;
D.marks.forEach(m => {
  const g = el("g", {});
  el("title", {}, g).textContent = m.label;
  el("rect", { x: m.x - FS * .4, y: -m.y - FS * .4, width: FS * .8, height: FS * .8, rx: FS * .15, fill: "var(--muted)" }, g);
  const short = m.label.length > 18 ? m.label.slice(0, 17).replace(/[\s+(,]+$/, "") + "…" : m.label;
  el("text", { x: m.x + FS * .75, y: -m.y + FS * .35, "font-size": FS * .85, fill: "var(--ink2)" }, g).textContent = short;
});
D.boards.forEach(b => {
  el("circle", { cx: b.x, cy: -b.y, r: FS, fill: "var(--surface)", stroke: "var(--ink)", "stroke-width": FS * .17 });
  el("text", { x: b.x, y: -b.y + FS * .36, "font-size": FS, "text-anchor": "middle", "font-weight": 700 }).textContent = b.id;
  const right = b.x >= cxm;
  if (b.label) el("text", { x: b.x + (right ? 1.45 : -1.45) * FS, y: -b.y + FS * .32, "font-size": FS * .85, fill: "var(--ink2)", "text-anchor": right ? "start" : "end" }).textContent = b.label;
});

/* ---------- tooltip ---------- */
const tip = document.getElementById("tip");
function showTip(e, h) { tip.innerHTML = h; tip.style.display = "block";
  tip.style.left = Math.min(e.clientX + 14, innerWidth - tip.offsetWidth - 8) + "px";
  tip.style.top = Math.min(e.clientY + 14, innerHeight - tip.offsetHeight - 8) + "px"; }
function hideTip() { tip.style.display = "none"; }
function dirTip(d, i) {
  const [s, r] = NAMES[d].split("-"); const v = sizeAt(d, i);
  return `<b>${s} → ${r}</b> &nbsp;<span style="color:var(--muted)">board ${r} hearing board ${s}</span><br>` +
    (v < 0 ? "no data this second" : `change ${v.toFixed(2)}× quiet<br>loss ${raw(D.loss, d, i)}%<br>shape moved ${(raw(D.shape, d, i) / 10).toFixed(1)} dB` +
      (isOne(d, i) ? `<br><b>one-sided:</b> ${r} \u2192 ${s} is quiet` : ""));
}

/* ---------- now table and events ---------- */
const tbody = document.querySelector("#now tbody");
const rows = NAMES.map(nm => { const tr = document.createElement("tr"); tr.innerHTML = `<td><span class="sw"></span>${nm.replace("-", " → ")}<span class="os"></span></td><td></td><td></td><td></td>`; tbody.appendChild(tr); return tr; });
const evlist = document.getElementById("evlist");
const ITEMS = [];                       // events, and inside a stretch its peaks, in time order
function evRow(name, i, j, text, cls) {
  const div = document.createElement("div"); div.className = "ev" + (cls ? " " + cls : "");
  div.innerHTML = `<span class="m">${name}</span><span>${clk(i)}</span><span class="m">${tOf(j) - tOf(i) + 1} s</span><span></span>`;
  div.lastChild.innerHTML = text;
  div.addEventListener("click", () => seek(Math.max(0, i - 5)));
  evlist.appendChild(div);
  ITEMS.push({ i, j, name, el: div });
}
const arrow = d => NAMES[d].replace("-", "→");
D.events.forEach(e => {
  const top = e.pk.indexOf(Math.max(...e.pk)), nr = e.near ? ` <b class="nr">near ${e.near}</b>` : "";
  if (e.stretch) {
    evRow(`E${e.n}`, e.i, e.j, `stretch: ${arrow(e.level[0])} restless (${e.level[1]}× quiet), peak ${Math.max(...e.pk).toFixed(1)}${nr}`, "st");
    e.subs.forEach((s, k) => {
      const big = s.pk.indexOf(Math.max(...s.pk));
      const what = s.up.length ? `peak ${s.pk[big].toFixed(1)} on ${arrow(big)}` : `loss jump on ${s.lj.map(arrow).join(", ")}`;
      evRow(`E${e.n}${String.fromCharCode(97 + (k % 26))}`, s.i, s.j,
            what + (s.up.length && s.lj.length ? ", loss jump" : "") + (s.weak ? ` <span class="m">(weak)</span>` : "")
            + (s.near ? ` <b class="nr">near ${s.near}</b>` : ""), "sub");
    });
  } else {
    evRow(`E${e.n}`, e.i, e.j, `peak ${Math.max(...e.pk).toFixed(1)} on ${arrow(top)}${nr}`);
  }
});
if (!D.events.length) evlist.textContent = "none";
const abt = document.getElementById("abouttbl");
(D.about || []).forEach(([k, v]) => { const tr = document.createElement("tr"); tr.innerHTML = "<th></th><td></td>";
  tr.children[0].textContent = k; tr.children[1].textContent = v; abt.appendChild(tr); });
function stepText(s, short) {
  const sh = s.db >= 1 ? `shape ${s.db} dB` : "", rs = Math.abs(s.rssi) >= (short ? 3 : 1) ? `RSSI ${s.rssi > 0 ? "+" : ""}${s.rssi} dB` : "";
  return `${s.link} ` + [sh, rs].filter(Boolean).join(", ");
}
const CAM = D.camera || { events: [], frames: [] };
const frameAt = new Map(CAM.frames.map(f => [f.i, f.src]));
ITEMS.forEach(e => {                     // events the camera kept frames for
  if (CAM.frames.some(f => f.i >= e.i - 2 && f.i <= e.j + 2)) e.el.lastChild.insertAdjacentHTML("beforeend", ` <span class="cam">camera</span>`);
});
const marklist = document.getElementById("marklist");
const mk2 = D.steps.map(s => ({ i: s.i, kind: "change", text: "link " + stepText(s, false) }))
  .concat(D.notes.map(n => ({ i: n.i, kind: "note", text: n.label })))
  .concat(CAM.events.map(c => ({ i: c.i, kind: "cam", text: "camera: " + (c.label || c.kind) + (c.note ? " (" + c.note + ")" : "") })))
  .sort((a, b) => a.i - b.i);
mk2.forEach(m => {
  const div = document.createElement("div"); div.className = "ev";
  div.innerHTML = `<span class="m" style="color:var(${m.kind === "change" ? "--shape" : m.kind === "cam" ? "--cam" : "--note"})">${m.kind === "change" ? "▮" : m.kind === "cam" ? "◉" : "▸"}</span><span>${clk(m.i)}</span><span></span><span></span>`;
  div.lastChild.textContent = m.text;
  div.addEventListener("click", () => seek(Math.max(0, m.i - 20)));
  marklist.appendChild(div);
});
if (!mk2.length) marklist.textContent = "none";

/* ---------- timeline ---------- */
const cv = document.getElementById("tl"), cx = cv.getContext("2d");
const ROWH = Math.max(5, Math.min(15, Math.floor(110 / ND)));
const TL = { left: 54, right: 10, top: 34, gap: 22 };
cv.style.height = (TL.top + ND * ROWH + 22) + "px";
let tlW = 0;
function plotW() { return tlW - TL.left - TL.right - TL.gap * (D.segs.length - 1); }
function xOfI(i) { const s = D.segs.indexOf(segOf(i)); return TL.left + i * plotW() / N + s * TL.gap; }
function iOfX(x) {
  const w = plotW();
  for (let k = D.segs.length - 1; k >= 0; k--) {
    const sx = TL.left + D.segs[k].a * w / N + k * TL.gap;
    if (x >= sx || k === 0) { const i = D.segs[k].a + Math.round((x - sx) * N / w); return Math.max(D.segs[k].a, Math.min(D.segs[k].a + D.segs[k].n - 1, i)); }
  }
  return 0;
}
let strips = null;
function layoutTL() {
  const r = cv.getBoundingClientRect(), dpr = devicePixelRatio || 1;
  cv.width = Math.round(r.width * dpr); cv.height = Math.round(r.height * dpr);
  cx.setTransform(dpr, 0, 0, dpr, 0, 0); tlW = r.width;
  // the strips change only with the size or the theme: draw them once, off screen
  strips = document.createElement("canvas"); strips.width = cv.width; strips.height = cv.height;
  const sx = strips.getContext("2d"); sx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const w = plotW();
  NAMES.forEach((nm, d) => {
    const y = TL.top + d * ROWH;
    D.segs.forEach((s, k) => {
      const px0 = TL.left + s.a * w / N + k * TL.gap, pxN = s.n * w / N;
      for (let px = 0; px < pxN; px++) {
        const a = s.a + Math.floor(px * N / w), b = Math.min(s.a + s.n, Math.max(a + 1, s.a + Math.floor((px + 1) * N / w)));
        let m = -1; for (let i = a; i < b; i++) { const v = D.size[d][i]; if (v > m) m = v; }
        const c = colorFor(m < 0 ? -1 : m / 20); if (!c) continue;
        sx.fillStyle = c; sx.fillRect(px0 + px, y + 1, 1.05, ROWH - 2);
      }
    });
  });
  drawTL();
}
function drawTL() {
  const H = cv.getBoundingClientRect().height, w = plotW();
  cx.clearRect(0, 0, tlW, H);
  cx.save(); cx.setTransform(1, 0, 0, 1, 0, 0); cx.drawImage(strips, 0, 0); cx.restore();
  cx.font = "11px system-ui, sans-serif"; cx.textBaseline = "middle";
  const bottom = TL.top + ND * ROWH;
  NAMES.forEach((nm, d) => {
    cx.fillStyle = css("--ink2"); cx.textAlign = "right";
    if (ROWH >= 9) cx.fillText(nm.replace("-", "→"), TL.left - 8, TL.top + d * ROWH + ROWH / 2);
  });
  // gaps between parts of the recording
  cx.fillStyle = css("--muted"); cx.textAlign = "center";
  for (let k = 1; k < D.segs.length; k++) {
    const gx = TL.left + D.segs[k].a * w / N + (k - 1) * TL.gap;
    cx.save(); cx.translate(gx + TL.gap / 2, TL.top + ND * ROWH / 2); cx.rotate(-Math.PI / 2);
    cx.fillText("not recorded", 0, 0); cx.restore();
  }
  // time ticks: about every 80 px, on round minutes
  const perPx = N / w, choices = [60, 300, 600, 1800, 3600, 7200, 10800];
  const stepS = choices.find(c => c / perPx >= 80) || 21600;
  cx.textAlign = "center";
  D.segs.forEach(s => { for (let i = s.a; i < s.a + s.n; i++) { if ((tOf(i) + s.off) % stepS === 0) { const x = xOfI(i);
    cx.fillRect(x, bottom + 2, 1, 4); cx.fillText(clk(i).slice(0, 5), x, bottom + 13); } } });
  // lasting changes (orange, labels above) and notes (ink, labels below them)
  const taken = { 9: [], 22: [] };
  function label(x, y, text) {               // skip a label that would run into another
    const w = cx.measureText(text).width, right = x > tlW * .7;
    const a = right ? x - 5 - w : x + 5, b = a + w;
    if (taken[y].some(([p, q]) => a < q + 6 && b > p - 6)) return;
    taken[y].push([a, b]); cx.textAlign = "left"; cx.fillText(text, a, y);
  }
  D.steps.forEach(s => { const x = xOfI(s.i); cx.fillStyle = css("--shape"); cx.fillRect(x - .5, TL.top - 4, 2, ND * ROWH + 4); label(x, 9, stepText(s, true)); });
  CAM.events.forEach(c => { const x = xOfI(c.i); cx.fillStyle = css("--cam"); cx.fillRect(x - .5, TL.top - 4, 2, ND * ROWH + 4); label(x, 22, c.label || c.kind); });
  D.notes.forEach(n => { const x = xOfI(n.i); cx.fillStyle = css("--note"); cx.fillRect(x - .5, TL.top - 4, 2, ND * ROWH + 4); label(x, 22, n.label); });
  cx.fillStyle = css("--ink2");
  D.events.forEach(e => {
    cx.fillRect(xOfI(e.i), TL.top - 6, 1, 3);
    if (e.stretch) {                    // a stretch: a bar along its length, its peaks ticked
      cx.globalAlpha = .45; cx.fillRect(xOfI(e.i), TL.top - 9, Math.max(1, xOfI(e.j) - xOfI(e.i)), 2); cx.globalAlpha = 1;
      e.subs.forEach(s => cx.fillRect(xOfI(s.i), TL.top - 8, 1, 5));
    }
  });
  cx.fillStyle = css("--ink"); cx.fillRect(xOfI(cur) - 1, TL.top - 8, 2, ND * ROWH + 10);
}

/* ---------- playback ---------- */
let cur = 0, playing = false, last = 0, acc = 0;
const speedSel = document.getElementById("speed");
function windowMax(d, i) {
  const k = Math.max(1, Math.round(Number(speedSel.value) / 12));
  let m = -1; for (let j = Math.max(0, i - k + 1); j <= i; j++) { const v = D.size[d][j]; if (v > m) m = v; }
  return m < 0 ? -1 : m / 20;
}
function render() {
  document.getElementById("clock").textContent = clk(cur);
  NAMES.forEach((nm, d) => {
    const v = playing ? windowMax(d, cur) : sizeAt(d, cur);
    const c = colorFor(v) || css("--muted");
    const f = v < 1.15 ? 0 : Math.min(Math.log(v) / Math.log(6), 1);
    const L = linkEls[d];
    if (L) {
      L.line.setAttribute("stroke", c);
      L.line.setAttribute("stroke-width", (W0 * (1 + f * 1.6)).toFixed(3));
      L.line.setAttribute("stroke-dasharray", v < 0 ? `${W0 * .5} ${W0 * 1.3}` : "none");
      const sh = raw(D.shape, d, cur) / 10;
      L.halo.setAttribute("opacity", sh >= 1.5 ? Math.min(0.4 + (sh - 1.5) / 4, 0.9).toFixed(2) : 0);
      L.hollow.setAttribute("opacity", v >= 1.15 && isOne(d, cur) ? 1 : 0);
      L.hollow.setAttribute("stroke-width", (W0 * (1 + f * 1.6) * .45).toFixed(3));
    }
    const F = fanEls[d];
    if (F) {
      F.lvl = v < 1.15 ? 0 : f < 0.34 ? 1 : f < 0.67 ? 2 : 3; F.f = f; F.c = c;
      const quiet = css("--quiet");
      F.arcs.forEach((a, k) => {
        a.setAttribute("stroke", v < 0 ? css("--muted") : k < F.lvl ? c : quiet);
        a.setAttribute("stroke-dasharray", v < 0 ? `${FAN.w * .3} ${FAN.w * 1.2}` : "none");
        F.hollow[k].setAttribute("opacity", v >= 0 && k < F.lvl && isOne(d, cur) ? 1 : 0);
      });
      const sh = raw(D.shape, d, cur) / 10;
      F.shape.setAttribute("opacity", sh >= 1.5 ? Math.min(0.45 + (sh - 1.5) / 4, 0.95).toFixed(2) : 0);
      if (!playing || v < 0 || F.lvl === 0) F.echo.setAttribute("opacity", 0);
    }
    const tds = rows[d].children, sz = sizeAt(d, cur);
    tds[0].firstChild.style.background = c;
    tds[0].lastChild.textContent = sz >= 1.15 && isOne(d, cur) ? "one-sided" : "";
    tds[1].textContent = sz < 0 ? "–" : sz.toFixed(2);
    tds[2].textContent = raw(D.loss, d, cur) < 0 ? "–" : raw(D.loss, d, cur);
    tds[3].textContent = raw(D.shape, d, cur) < 0 ? "–" : (raw(D.shape, d, cur) / 10).toFixed(1);
  });
  showFrame();
  const X = String(D.near[cur] || "");
  for (const id in ringEls) {
    const R = ringEls[id], show = id === X;
    R.g.style.display = show ? "" : "none";
    if (!show) continue;
    let best = -1;                     // the strongest lit direction of that board
    NAMES.forEach((nm, d) => { if (nm.split("-").includes(id)) { const v = playing ? windowMax(d, cur) : sizeAt(d, cur); if (v > best) best = v; } });
    R.c = colorFor(best) || css("--muted"); R.f = best < 1.15 ? 0 : Math.min(Math.log(best) / Math.log(6), 1);
    R.lvl = R.f < 0.34 ? 1 : R.f < 0.67 ? 2 : 3;
    R.rings.forEach((r, k) => r.setAttribute("stroke", k < R.lvl ? R.c : css("--quiet")));
    if (!playing) R.echo.setAttribute("opacity", 0);
  }
  fanEls.forEach((F, d) => { if (F) F.g.style.display = X && NAMES[d].split("-")[0] === X ? "none" : ""; });   // the rings stand in for its fans
  let on = -1;
  ITEMS.forEach((e, k) => { const a = e.i <= cur && cur <= e.j; e.el.classList.toggle("on", a); if (a) on = k; });
  document.getElementById("nowev").textContent = on >= 0 ? `in ${ITEMS[on].name}` : "";
  const nearK = on >= 0 ? on : ITEMS.findIndex(e => e.i > cur);
  if (nearK >= 0) { const r = ITEMS[nearK].el, top = r.offsetTop - evlist.offsetTop;
    if (top < evlist.scrollTop || top > evlist.scrollTop + evlist.clientHeight - r.offsetHeight) evlist.scrollTop = top - evlist.clientHeight / 3; }
  drawTL();
}
function seek(i) { cur = Math.max(0, Math.min(N - 1, i)); render(); }
const camcard = document.getElementById("camcard"), camimg = document.getElementById("camimg"), camnote = document.getElementById("camnote");
if (CAM.frames.length) camcard.hidden = false;
camimg.addEventListener("error", () => { camimg.hidden = true;
  camnote.textContent = "frame not found: open this page in its watch folder, or through a web server started there (see replay.py)"; });
function showFrame() {
  if (!CAM.frames.length) return;
  const src = frameAt.get(cur) || frameAt.get(cur - 1);
  const ev = CAM.events.find(c => c.i - 6 <= cur && cur <= c.j + 11);
  if (src) {
    camimg.hidden = false;
    if (camimg.getAttribute("src") !== src) camimg.setAttribute("src", src);
    camnote.textContent = (ev ? `watch event ${ev.event}: ${ev.label || ev.kind}${ev.note ? " (" + ev.note + ")" : ""}` : "");
  } else {
    camimg.hidden = true;
    const nx = CAM.frames.find(f => f.i > cur) || CAM.frames[0];
    camnote.innerHTML = "no frame kept for this moment (the camera keeps frames around the watch's events)";
    const b = document.createElement("button"); b.textContent = `${clk(nx.i)} →`;
    b.addEventListener("click", () => seek(nx.i)); camnote.appendChild(b);
  }
}
const still = matchMedia("(prefers-reduced-motion: reduce)");
function echoes(dt) {                    // waves: rings travel out along each active path
  if (mode !== "waves" || still.matches) return;
  fanEls.forEach(F => {
    if (!F) return;
    if (F.lvl === 0) { F.echo.setAttribute("opacity", 0); return; }
    F.ph = (F.ph + dt * (0.5 + 1.3 * F.f)) % 1;
    const R = FAN.r[2] + F.ph * (F.end - FAN.r[2]);
    F.echo.setAttribute("d", arcPath(F.ax, F.ay, R, F.th, F.half));
    F.echo.setAttribute("stroke", F.c);
    F.echo.setAttribute("opacity", ((1 - F.ph) * (0.35 + 0.2 * F.lvl)).toFixed(2));
  });
  const R = ringEls[String(D.near[cur] || "")];
  if (R) {
    R.ph = (R.ph + dt * (0.5 + 1.3 * R.f)) % 1;
    R.echo.setAttribute("r", FAN.r[2] + R.ph * FS * 7);
    R.echo.setAttribute("stroke", R.c);
    R.echo.setAttribute("opacity", ((1 - R.ph) * 0.75).toFixed(2));
  }
}
function tick(ts) {
  if (!playing) return;
  if (last) { const dt = (ts - last) / 1000; acc += dt * Number(speedSel.value); const s = Math.floor(acc);
    if (s) { acc -= s; cur += s; if (cur >= N - 1) { cur = N - 1; setPlay(false); } render(); }
    if (playing) echoes(Math.min(dt, 0.1)); }
  last = ts; requestAnimationFrame(tick);
}
let mode = "";
function setMode(m) {
  mode = m === "lines" ? "lines" : "waves";
  gLines.style.display = mode === "lines" ? "" : "none";
  gWaves.style.display = mode === "waves" ? "" : "none";
  document.body.classList.toggle("v-lines", mode === "lines"); document.body.classList.toggle("v-waves", mode === "waves");
  document.getElementById("vWaves").classList.toggle("on", mode === "waves");
  document.getElementById("vLines").classList.toggle("on", mode === "lines");
  render();
}
document.getElementById("vWaves").addEventListener("click", () => setMode("waves"));
document.getElementById("vLines").addEventListener("click", () => setMode("lines"));
const playBtn = document.getElementById("play");
function setPlay(p) { playing = p; last = 0; acc = 0; playBtn.innerHTML = p ? "&#10073;&#10073; Pause" : "&#9654; Play"; if (p) requestAnimationFrame(tick); render(); }
playBtn.addEventListener("click", () => setPlay(!playing));
speedSel.addEventListener("change", render);
function jumpEv(dir) {
  const E = ITEMS; const e = dir > 0 ? E.find(e => e.i > cur + 5) : [...E].reverse().find(e => e.i < cur - 5);
  if (e) seek(Math.max(0, e.i - 5));
}
document.getElementById("next").addEventListener("click", () => jumpEv(1));
document.getElementById("prev").addEventListener("click", () => jumpEv(-1));
document.addEventListener("keydown", e => {
  if (e.target.tagName === "SELECT") return;
  if (e.key === " ") { e.preventDefault(); setPlay(!playing); }
  else if (e.key === "ArrowRight") seek(cur + (e.shiftKey ? 60 : 1));
  else if (e.key === "ArrowLeft") seek(cur - (e.shiftKey ? 60 : 1));
  else if (e.key === "n") jumpEv(1); else if (e.key === "p") jumpEv(-1);
});
let drag = false;
cv.addEventListener("pointerdown", e => { drag = true; cv.setPointerCapture(e.pointerId); seek(iOfX(e.offsetX)); });
cv.addEventListener("pointermove", e => { const i = iOfX(e.offsetX); if (drag) seek(i);
  showTip(e, `<b>${clk(i)}</b><br>` + NAMES.map((nm, d) => `${nm.replace("-", "→")} ${sizeAt(d, i) < 0 ? "–" : sizeAt(d, i).toFixed(2)}`).join("<br>")); });
cv.addEventListener("pointerup", () => drag = false);
cv.addEventListener("pointerleave", hideTip);
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => { readRamp(); layoutTL(); render(); });
addEventListener("resize", layoutTL);
readRamp(); layoutTL(); cur = 0; setMode(D.view);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
