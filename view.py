#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""view.py - a whole run (a night, a day) at a glance.

Usage:
    ~/scratch-esp/bin/python view.py RUN_FOLDER [--usual usual.json] [--k 8]
                                               [--quiet HH:MM-HH:MM] [--min-data 0.5]
    e.g. ~/scratch-esp/bin/python view.py ~/tw-data/runs/2026-10-26_2200

Reads every csi-*.csv.gz in the folder (and night-*.csv.gz from kits before
v4.6; one hour at a time, so it runs in little memory; a file cut off by a
power cut is read up to the break) and writes into the same folder:
    view.png    four strips against clock time:
                     1 movement: how much the CSI wobbles, each 10 s
                     2 rhythms: each 10 minutes, which rates stand out (0.03-1 Hz)
                     3 RSSI, each minute
                     4 packets per second and percent lost, each minute
    view.txt    the same in words: events and gaps by clock time, RSSI
                   shifts, the strongest rhythms of each hour and each 10 min
    usual.json     this run's "usual" levels; give a quiet run's file to
                   --usual on later runs so they are judged against it
                   (--quiet HH:MM-HH:MM takes "usual" from that stretch of
                   this run only, e.g. the hours you know were empty)

If the folder holds a scene.txt (run.sh asks for one: which cars, doors,
people), its text is shown at the top of view.txt.

Clock times come from the Linux stamp on each line (keep chrony running).
It prints times and numbers only: no labels, no guesses.

LOW DATA: a 10 s window with under half the usual packets (--min-data) is
marked low data and never called an event, nor used for "usual": under heavy
loss the movement measure lies (afternoon of 2026-10-26: 65-75% lost, a
3,830 s false event). A 10-minute rhythm piece with that little data is
skipped too. Low-data stretches are listed and drawn in amber.

How "movement" is measured: CSI amplitude averaged into 10 Hz bins; in each
10 s window, each live subcarrier's spread (sd / mean); the median over the
subcarriers. An event is a run of windows above usual + k x spread
(median + k x 1.4826 x MAD; k = 8 by default), merged if under 30 s apart.
How rhythms are measured: the analyze_csi.py recipe (60 s detrend, Hann,
ratio to neighbors +-20 bins) on each 10-minute piece; each hour also gets
the average of its pieces. A rate notable in at least half the pieces is
called a habit (the fridge, say); each piece also reports its strongest
peak that is not a habit, which is where something new would show.
"""
import argparse
import datetime as dt
import glob
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (same recipe as the short runs)

BIN_HZ = A.BIN_HZ
WIN_S = 10            # movement window
SEG_S = 600           # rhythm piece
CHUNK = 5000          # lines parsed at a time
SPEC_FMAX = 1.0       # rhythm strip upper rate (Hz); peaks are listed up to 4.9
HABIT_TOL = 2.5 / SEG_S   # a peak within 2.5 resolution steps of a habit is that habit


def fmt(t):
    return dt.datetime.fromtimestamp(t).strftime("%H:%M:%S")


class Grow:
    """A growable float array (rows = 10 Hz bins)."""

    def __init__(self, width):
        self.a = np.zeros((36000 + 600, width))

    def ensure(self, n):
        if n > len(self.a):
            b = np.zeros((max(n, 2 * len(self.a)), self.a.shape[1]))
            b[:len(self.a)] = self.a
            self.a = b


def read_hour(path, csi_len):
    """Stream one file. Returns dict of binned arrays, or None if empty."""
    t_origin = None
    sums = cnt = rs = None
    pk_t, pk_id = [], []
    skipped = 0
    buf = []

    def flush(buf):
        nonlocal t_origin, sums, cnt, rs, csi_len
        heads, datas = [], []
        for line in buf:
            q = line.find('"')
            if q < 0:
                continue
            f = line[:q].split(",")
            try:
                if len(f) != 26:
                    raise ValueError
                h_ns, pid, rssi = int(f[0]), int(f[2]), int(f[4])
                ln, fw = int(f[23]), int(f[24])
            except ValueError:
                continue
            d = line[q + 2:line.rfind("]")]
            if csi_len[0] is None:
                csi_len[0] = ln
            if ln != csi_len[0] or d.count(",") != ln - 1:
                continue
            heads.append((h_ns, pid, rssi, fw))
            datas.append(d)
        if not heads:
            return len(buf)
        try:
            vals = np.fromstring(",".join(datas), sep=",", dtype=np.int16)
            if vals.size != len(datas) * csi_len[0]:
                raise ValueError
        except ValueError:
            # a garbled line somewhere in this chunk: parse line by line, skip the bad ones
            keep_h, keep_v = [], []
            for hd, d in zip(heads, datas):
                try:
                    v = np.fromstring(d, sep=",", dtype=np.int16)
                except ValueError:
                    continue
                if v.size == csi_len[0]:
                    keep_h.append(hd)
                    keep_v.append(v)
            if not keep_h:
                return len(buf)
            heads, vals = keep_h, np.concatenate(keep_v)
        h = np.array(heads, dtype=np.int64)
        iq = vals.reshape(len(h), -1, 2).astype(float)
        bad_fw = h[:, 3] == 1
        iq[bad_fw, :2, :] = 0
        amp = np.hypot(iq[..., 0], iq[..., 1])
        t = h[:, 0] / 1e9
        if t_origin is None:
            t_origin = np.floor(t[0] / 60) * 60
            sums, cnt, rs = Grow(amp.shape[1]), Grow(1), Grow(1)
        idx = ((t - t_origin) * BIN_HZ).astype(int)
        ok = idx >= 0
        idx, amp, t, h = idx[ok], amp[ok], t[ok], h[ok]
        n = idx.max() + 1
        for g in (sums, cnt, rs):
            g.ensure(n)
        np.add.at(sums.a, idx, amp)
        np.add.at(cnt.a[:, 0], idx, 1)
        np.add.at(rs.a[:, 0], idx, h[:, 2].astype(float))
        pk_t.append(t)
        pk_id.append(h[:, 1])
        return len(buf) - len(h)

    for line in A.read_lines(path):
        if "CSI_DATA" not in line:
            continue
        buf.append(line)
        if len(buf) >= CHUNK:
            skipped += flush(buf)
            buf = []
    if buf:
        skipped += flush(buf)
    if t_origin is None:
        return None
    n = int(np.flatnonzero(cnt.a[:, 0])[-1]) + 1
    return dict(t0=t_origin, sums=sums.a[:n], cnt=cnt.a[:n, 0], rssi=rs.a[:n, 0],
                pk_t=np.concatenate(pk_t), pk_id=np.concatenate(pk_id), skipped=skipped)


def fill(x, empty):
    good = np.flatnonzero(~empty)
    if good.size == 0:
        return x
    for k in range(x.shape[1]):
        x[empty, k] = np.interp(np.flatnonzero(empty), good, x[good, k])
    return x


def analyze_hour(r, live=None):
    cnt = r["cnt"]
    empty = cnt == 0
    with np.errstate(invalid="ignore", divide="ignore"):
        amp = r["sums"] / cnt[:, None]
        rssi = r["rssi"] / cnt
    if live is None:
        live = np.nanmedian(amp[~empty], axis=0) > 1.0
    amp = amp[:, live]
    t0 = r["t0"]
    out = dict(move=[], minute=[], seg=[], gaps=[])

    # per packet: how many sender ids went missing just before it (loss). Counted in the
    # file's own order, which is measurement order: a board's own recording is written as
    # it arrives, and split_links.py writes each link in measurement order. Relayed records
    # can reach the computer a frame late, after a newer one, so sorting by arrival time
    # first would count each late record as two losses (kit v5.0: 9.8% shown, 3.0% true).
    d = np.diff(r["pk_id"])
    lost_here = np.r_[0, np.where((d > 0) & (d < 1000), d - 1, 0)]
    order = np.argsort(r["pk_t"], kind="stable")
    pt, lost_here = r["pk_t"][order], lost_here[order]

    # movement, per 10 s, with the window's packets and loss (for the low-data mark)
    w = int(WIN_S * BIN_HZ)
    nwin = len(cnt) // w
    wi = ((pt - t0) // WIN_S).astype(int)
    okw = (wi >= 0) & (wi < nwin)
    lost_w = np.bincount(wi[okw], weights=lost_here[okw], minlength=nwin)
    for k in range(nwin):
        sl = slice(k * w, (k + 1) * w)
        ok = ~empty[sl]
        got = int(cnt[sl].sum())
        if ok.sum() < w // 2:
            if got:                 # too little even to measure: still a (very) low-data window
                out["move"].append((t0 + k * WIN_S, np.nan, got, lost_w[k]))
            continue
        a = amp[sl][ok]
        m = a.mean(axis=0)
        rel = a.std(axis=0) / np.where(m > 0, m, np.nan)
        out["move"].append((t0 + k * WIN_S, float(np.nanmedian(rel)), got, lost_w[k]))

    # gaps of >= 5 s with nothing received
    e = np.r_[False, empty, False].astype(int)
    starts, ends = np.flatnonzero(np.diff(e) == 1), np.flatnonzero(np.diff(e) == -1)
    for a_, b_ in zip(starts, ends):
        if a_ == 0:
            continue                # before the file's first packet: not a gap
        if (b_ - a_) / BIN_HZ >= 5:
            out["gaps"].append((t0 + a_ / BIN_HZ, t0 + b_ / BIN_HZ))

    # per minute: packets, lost, rssi
    mins = ((pt - t0) // 60).astype(int)
    nmin = mins.max() + 1
    got = np.bincount(mins, minlength=nmin)
    lost = np.bincount(mins, weights=lost_here, minlength=nmin)
    bmin = (np.arange(len(cnt)) // (60 * BIN_HZ)).astype(int)
    rsum = np.bincount(bmin, weights=np.nan_to_num(rssi) * cnt, minlength=nmin)[:nmin]
    rcnt = np.bincount(bmin, weights=cnt, minlength=nmin)[:nmin]
    for m in range(nmin):
        if got[m] == 0:
            continue
        span = 60.0
        if m == 0 or m == nmin - 1:     # the file's first and last minute may be partial
            inm = pt[mins == m]
            lo = inm[0] if m == 0 else t0 + 60 * m
            hi = inm[-1] if m == nmin - 1 else t0 + 60 * (m + 1)
            span = max(hi - lo, 1.0)
        out["minute"].append((t0 + 60 * m, got[m] / span,
                              100 * lost[m] / max(got[m] + lost[m], 1),
                              rsum[m] / rcnt[m] if rcnt[m] else np.nan, got[m]))

    # rhythms per 10-minute piece, aligned to the clock
    amp_f = fill(amp.copy(), empty)
    seg = int(SEG_S * BIN_HZ)
    first = int(((-(t0 % SEG_S)) % SEG_S) * BIN_HZ)
    for s in range(first, len(cnt) - seg + 1, seg):
        sl = slice(s, s + seg)
        if empty[sl].mean() > 0.10:
            continue
        f, p = A.spectrum(A.detrend(amp_f[sl]))
        out["seg"].append((t0 + s / BIN_HZ, f, p, int(cnt[sl].sum())))
    return out, live


def robust(x):
    x = np.asarray(x)
    med = float(np.median(x))
    mad = float(np.median(np.abs(x - med))) * 1.4826
    return med, mad


def merge_runs(times, flags, gap_s=30):
    ev = []
    for t, fl in zip(times, flags):
        if not fl:
            continue
        if ev and t - ev[-1][1] <= gap_s:
            ev[-1][1] = t + WIN_S
        else:
            ev.append([t, t + WIN_S])
    return ev


def clock_stretch(spec, t_ref):
    """'HH:MM-HH:MM' -> (start, end) unix seconds, on the day that holds t_ref
    (a stretch that passes midnight is fine)."""
    try:
        a_, b_ = spec.split("-")
        ha, ma = (int(x) for x in a_.split(":"))
        hb, mb = (int(x) for x in b_.split(":"))
    except ValueError:
        sys.exit(f"--quiet wants HH:MM-HH:MM, not {spec!r}")
    day = dt.datetime.fromtimestamp(t_ref).replace(hour=0, minute=0, second=0, microsecond=0)
    start = day + dt.timedelta(hours=ha, minutes=ma)
    if start.timestamp() + 12 * 3600 < t_ref:      # the run began the day before
        start += dt.timedelta(days=1)
    elif start.timestamp() > t_ref + 12 * 3600:
        start -= dt.timedelta(days=1)
    end = start.replace(hour=hb, minute=mb)
    if end <= start:
        end += dt.timedelta(days=1)
    return start.timestamp(), end.timestamp()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--usual", help="usual.json from a quiet run, to judge this run against")
    ap.add_argument("--k", type=float, default=8.0, help="event threshold, in robust spreads above usual")
    ap.add_argument("--notable", type=float, default=A.NOTABLE, help="rhythm ratio worth a mark")
    ap.add_argument("--quiet", metavar="HH:MM-HH:MM",
                    help="take 'usual' from this stretch of the run only (hours known to be empty)")
    ap.add_argument("--min-data", type=float, default=0.5,
                    help="a 10 s window with under this fraction of the usual packets is LOW DATA")
    a = ap.parse_args()
    if a.usual and a.quiet:
        sys.exit("give --usual or --quiet, not both")

    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz"))
                   + glob.glob(os.path.join(a.folder, "night-*.csv.gz")))   # kits before v4.6
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    move, minute, segs, gaps, hours = [], [], [], [], []
    csi_len, live, skipped = [None], None, 0
    pk_first, pk_last = np.inf, -np.inf
    for path in files:
        print(f"reading {os.path.basename(path)}", flush=True)
        r = read_hour(path, csi_len)
        if r is None:
            continue
        skipped += r["skipped"]
        pk_first, pk_last = min(pk_first, float(r["pk_t"].min())), max(pk_last, float(r["pk_t"].max()))
        o, live = analyze_hour(r, live)
        move += o["move"]
        minute += o["minute"]
        segs += o["seg"]
        gaps += o["gaps"]
        if o["seg"]:
            hours.append((r["t0"], o["seg"]))
    if not move:
        sys.exit("nothing readable")
    # files are read in name order; put everything in clock order regardless
    for lst in (move, minute, segs, gaps):
        lst.sort(key=lambda x: x[0])
    # the 10 s windows holding the recording's first and last packet are only partly
    # covered: their few packets are not low data (they made a false "low data" at the start)
    move = [m for m in move if m[0] >= pk_first - 0.5 and m[0] + WIN_S <= pk_last + 0.5] or move

    mt = np.array([m[0] for m in move])
    mv = np.array([m[1] for m in move])
    mpk = np.array([m[2] for m in move], dtype=float)      # packets in the window
    mlost = np.array([m[3] for m in move], dtype=float)    # sender ids missing in the window
    u = {}
    if a.usual:
        u = json.load(open(a.usual))
        usual_from = os.path.basename(a.usual)
    pk_usual = u.get("packets_per_10s_median") or float(np.median(mpk))
    low = (mpk < a.min_data * pk_usual) | np.isnan(mv)
    quiet_sel = ~low
    quiet_from = None
    if a.quiet:
        qa, qb = clock_stretch(a.quiet, mt[0])
        quiet_sel &= (mt >= qa) & (mt < qb)
        quiet_from = (qa, qb)
        if quiet_sel.sum() < 30:
            sys.exit(f"--quiet {a.quiet}: only {int(quiet_sel.sum())} usable 10 s windows there")
    if a.usual and "move_median" in u:
        med, mad = u["move_median"], u["move_spread"]
    else:
        med, mad = robust(mv[quiet_sel]) if quiet_sel.any() else robust(mv[~np.isnan(mv)])
        usual_from = (f"this run {fmt(quiet_from[0])[:5]}-{fmt(quiet_from[1])[:5]}" if quiet_from
                      else "this run")
    thr = med + a.k * max(mad, 1e-6)
    with np.errstate(invalid="ignore"):
        events = merge_runs(mt, (mv > thr) & ~low)
    low_runs = merge_runs(mt, low)
    # rhythm pieces with too little data are skipped as well
    seg_min = a.min_data * pk_usual * (SEG_S / WIN_S)
    n_seg_all = len(segs)
    segs = [sg for sg in segs if sg[3] >= seg_min]
    hours = [(t, [sg for sg in hs if sg[3] >= seg_min]) for t, hs in hours]
    hours = [(t, hs[0][1], np.mean([p for _t, _f, p, _n in hs], axis=0), len(hs)) for t, hs in hours if hs]

    mn = np.array(minute, dtype=float)
    # RSSI shifts: 5-minute means that differ from the previous 5 minutes by >= 0.5 dB
    shifts = []
    if len(mn) >= 10:
        r5 = [(mn[i, 0], np.nanmean(mn[i:i + 5, 3])) for i in range(0, len(mn) - 4, 5)]
        for (ta, ra), (tb, rb) in zip(r5, r5[1:]):
            if abs(rb - ra) >= 0.5:
                shifts.append((tb, rb - ra))

    # the two losses of a many-board link (split_links.py writes loss.csv): PATH = the
    # measuring board did not hear the frame (sensing), DELIVERY = its report never came
    # (plumbing). A rise in path loss between a pair is a detection of its own.
    loss = None
    loss_path = os.path.join(a.folder, "loss.csv")
    if os.path.exists(loss_path):
        try:
            loss = np.loadtxt(loss_path, delimiter=",", skiprows=2, ndmin=2)
            if loss.size == 0:
                loss = None
        except ValueError:
            loss = None
    path_rises, pl_med, pl_line = [], None, None
    if loss is not None:
        lt, lsent, lgot, lheard, lpl, ldl = loss.T
        ok = (lsent >= 20) & (ldl <= 70) & (lgot >= 5)       # estimate trustworthy there
        if ok.sum() >= 30:
            pl_med, pl_mad = robust(lpl[ok])
            pl_line = max(pl_med + a.k * max(pl_mad, 0.5), pl_med + 15)
            for s_, e_ in merge_runs(lt, ok & (lpl > pl_line)):
                sel = (lt >= s_) & (lt < e_) & ok
                path_rises.append((s_, e_, float(lpl[sel].max())))

    def losses_in(s_, e_):
        """(path %, delivery %) over [s_, e_), or None without loss.csv"""
        if loss is None:
            return None
        sel = (loss[:, 0] >= s_ - 0.5) & (loss[:, 0] < e_ - 0.5)
        sent, got, heard = loss[sel, 1].sum(), loss[sel, 2].sum(), loss[sel, 3].sum()
        if sent <= 0:
            return None
        return 100 * (1 - heard / sent), 100 * (1 - got / heard) if heard > 0 else 100.0

    t_first, t_last = mt[0], mt[-1] + WIN_S
    n_got = int(mn[:, 4].sum())
    lost_pct = np.average(mn[:, 2], weights=mn[:, 1])
    scene = ""
    scene_path = os.path.join(a.folder, "scene.txt")
    if os.path.exists(scene_path):
        scene = " / ".join(x.strip() for x in open(scene_path, errors="replace") if x.strip())

    # ---- words ----
    L = []
    L.append(f"run       {a.folder}")
    if scene:
        L.append(f"scene     {scene}")
    L.append(f"files     {len(files)} ({os.path.basename(files[0])} .. {os.path.basename(files[-1])})")
    L.append(f"span      {dt.datetime.fromtimestamp(t_first):%Y-%m-%d %H:%M} - "
             f"{dt.datetime.fromtimestamp(t_last):%H:%M} ({(t_last - t_first) / 3600:.1f} h)")
    both = losses_in(t_first, t_last)
    L.append(f"packets   {n_got:,} received, {lost_pct:.1f}% lost"
             + (f" (path {both[0]:.1f}%, delivery {both[1]:.1f}%)" if both else "")
             + f"; odd lines skipped {skipped}")
    L.append(f"CSI       {csi_len[0]} bytes/packet, {int(live.sum())} live subcarriers")
    L.append(f"RSSI      median {np.nanmedian(mn[:, 3]):.2f} dBm (per-minute range "
             f"{np.nanmin(mn[:, 3]):.2f} .. {np.nanmax(mn[:, 3]):.2f})")
    L.append(f"movement  usual {med:.5f}, spread {mad:.5f} (from {usual_from}); "
             f"event line {thr:.5f} (k={a.k:g})")
    L.append(f"low data  under {a.min_data * pk_usual / WIN_S:.0f} packets/s (usual {pk_usual / WIN_S:.0f}/s): "
             f"{int(low.sum())} of {len(low)} windows; {n_seg_all - len(segs)} of {n_seg_all} rhythm pieces skipped")
    L.append("")
    L.append(f"EVENTS (movement above the line, not in low data): {len(events)}")
    for s, e in events:
        sel = (mt >= s) & (mt < e) & ~low
        L.append(f"  {fmt(s)} - {fmt(e)}  {e - s:5.0f} s   peak x{np.nanmax(mv[sel]) / med:.1f} usual")
    L.append(f"LOW DATA (under half the usual packets; movement not judged there): {len(low_runs)}")
    for s, e in low_runs:
        sel = (mt >= s) & (mt < e)
        got, lst = mpk[sel].sum(), mlost[sel].sum()
        lo2 = losses_in(s, e)
        cause = ""
        if lo2:
            cause = (f"   path {lo2[0]:.0f}%, delivery {lo2[1]:.0f}%: "
                     + ("mostly PATH (the boards did not hear each other)" if lo2[0] >= lo2[1]
                        else "mostly DELIVERY (the reports did not get home)"))
        L.append(f"  {fmt(s)} - {fmt(e)}  {e - s:5.0f} s   {got / (e - s):4.0f} packets/s, "
                 f"{100 * lst / max(got + lst, 1):.0f}% lost{cause}")
    if loss is not None:
        L.append("PATH LOSS RISES (frames the measuring board did not hear, delivery losses taken out"
                 + (f"; usual {pl_med:.1f}%, line {pl_line:.1f}%" if pl_med is not None else "; too little data")
                 + "): " + str(len(path_rises)))
        for s_, e_, pk in path_rises:
            L.append(f"  {fmt(s_)} - {fmt(e_)}  {e_ - s_:5.0f} s   path loss up to {pk:.0f}%")
    L.append(f"GAPS (nothing received for 5 s or more): {len(gaps)}")
    for s, e in gaps:
        L.append(f"  {fmt(s)} - {fmt(e)}  {e - s:5.0f} s")
    L.append(f"RSSI SHIFTS (5-min mean moved >= 0.5 dB): {len(shifts)}")
    for t, d in shifts:
        L.append(f"  {fmt(t)}  {d:+.2f} dB")
    L.append("")
    L.append(f"RHYTHMS each hour (average of its 10-min pieces), top 4; notable >= x{a.notable:g}")
    L.append("  hour   pieces   ratio      Hz   period s  per min")
    for t, f, p, n in hours:
        for i, (ratio, fr) in enumerate(A.peaks(f, p, top=4)):
            mark = "  <- notable" if ratio >= a.notable else ""
            lab = f"  {dt.datetime.fromtimestamp(t):%H}:00   {n:4d}  " if i == 0 else " " * 17
            L.append(f"{lab}{ratio:6.1f}  {fr:7.4f}  {1 / fr:8.2f}  {fr * 60:7.2f}{mark}")
    # habits: rates notable in at least half of the 10-min pieces
    seg_peaks = [A.peaks(f, p, top=6) for _t, f, p, _n in segs]
    if a.usual and "habits_hz" in u:
        habits = list(u["habits_hz"])
    else:
        votes = {}
        for pk in seg_peaks:
            for fb in {round(fr * SEG_S) for ratio, fr in pk if ratio >= a.notable}:
                votes[fb] = votes.get(fb, 0) + 1
        habits = sorted(fb / SEG_S for fb, v in votes.items()
                        if v + votes.get(fb - 1, 0) + votes.get(fb + 1, 0) >= max(2, len(segs) / 2)
                        and v >= max(votes.get(fb - 1, 0), votes.get(fb + 1, 0)))

    def is_habit(fr):
        return any(abs(fr - h) <= HABIT_TOL for h in habits)

    L.append("")
    L.append("HABITS (rates notable in at least half the 10-min pieces"
             + (f", from {a.usual})" if a.usual and "habits_hz" in u else ")") + ": "
             + (", ".join(f"{h:.4f} Hz ({h * 60:.2f}/min)" for h in habits) or "none"))
    L.append("")
    L.append("RHYTHMS each 10 min: strongest peak; then strongest that is not a habit")
    seg_max, seg_new = [], []
    for (t, f, p, _n), pk in zip(segs, seg_peaks):
        ratio, fr = pk[0]
        seg_max.append(ratio)
        new = [(r_, f_) for r_, f_ in pk if not is_habit(f_)]
        nr, nf = new[0] if new else (0.0, 0.0)
        seg_new.append(nr)
        mark = "  <- NEW, notable" if nr >= a.notable else ""
        L.append(f"  {fmt(t)[:5]}  x{ratio:6.1f} {fr:7.4f} Hz {'(habit)' if is_habit(fr) else '       '}"
                 f"  | x{nr:6.1f} {nf:7.4f} Hz ({nf * 60:6.2f}/min){mark}")
    text = "\n".join(L) + "\n"
    out_txt = os.path.join(a.folder, "view.txt")
    open(out_txt, "w").write(text)
    print(text)

    usual = dict(made_from=os.path.abspath(a.folder), usual_from=usual_from,
                 move_median=med, move_spread=mad, habits_hz=habits,
                 packets_per_10s_median=float(np.median(mpk)), min_data=a.min_data,
                 seg_peak_ratio_median=float(np.median(seg_max)) if seg_max else None,
                 seg_new_ratio_median=float(np.median(seg_new)) if seg_new else None,
                 seg_new_ratio_max=float(np.max(seg_new)) if seg_new else None,
                 rssi_median=float(np.nanmedian(mn[:, 3])), lost_pct=float(lost_pct))
    if not a.usual:
        json.dump(usual, open(os.path.join(a.folder, "usual.json"), "w"), indent=1)

    # the same findings for programs (links_report.py compares the links of a many-board run)
    def peak_x(s, e):
        sel = (mt >= s) & (mt < e) & ~low
        return float(np.nanmax(mv[sel]) / med) if sel.any() else None

    def low_row(s, e):
        sel = (mt >= s) & (mt < e)
        got, lst = mpk[sel].sum(), mlost[sel].sum()
        lo2 = losses_in(s, e)
        return [s, e, float(got / (e - s)), float(100 * lst / max(got + lst, 1))] + (list(lo2) if lo2 else [None, None])

    found = dict(folder=os.path.abspath(a.folder), span=[float(t_first), float(t_last)],
                 packets=n_got, lost_pct=float(lost_pct), rssi_median=float(np.nanmedian(mn[:, 3])),
                 packets_per_s_usual=float(pk_usual / WIN_S), move_usual=med, event_line=thr,
                 events=[[s, e, peak_x(s, e)] for s, e in events],
                 low_data=[low_row(s, e) for s, e in low_runs],
                 path_lost_pct=both[0] if both else None, delivery_lost_pct=both[1] if both else None,
                 path_usual=pl_med, path_line=pl_line,
                 path_rises=[[float(s_), float(e_), pk] for s_, e_, pk in path_rises],
                 gaps=[[float(s), float(e)] for s, e in gaps],
                 rssi_shifts=[[float(t), float(d)] for t, d in shifts],
                 habits_hz=habits, seg_s=SEG_S,
                 pieces=[[float(t), [[float(r_), float(f_)] for r_, f_ in pk]]
                         for (t, _f, _p, _n), pk in zip(segs, seg_peaks)])
    json.dump(found, open(os.path.join(a.folder, "view.json"), "w"))

    # ---- picture ----
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    def D(t):
        return mdates.date2num([dt.datetime.fromtimestamp(x) for x in np.atleast_1d(t)])

    fig, ax = plt.subplots(4, 1, figsize=(13, 10), sharex=True,
                           gridspec_kw=dict(height_ratios=[1.2, 1.4, 0.8, 0.8]))
    ax[0].semilogy(D(mt), mv, lw=0.6, color="#3b6ea5")
    ax[0].axhline(thr, color="#c0392b", lw=0.8, ls="--")
    ax[0].axhline(med, color="0.5", lw=0.6, ls=":")
    ax[0].set_ylabel("movement\n(sd/mean, 10 s)")
    for s, e in events:
        for x in ax:
            x.axvspan(D(s)[0], D(e)[0], color="#c0392b", alpha=0.15, lw=0)
    for s, e in gaps:
        for x in ax:
            x.axvspan(D(s)[0], D(e)[0], color="0.3", alpha=0.25, lw=0)
    for s, e in low_runs:
        for x in ax:
            x.axvspan(D(s)[0], D(e)[0], color="#f39c12", alpha=0.25, lw=0)
    if quiet_from:
        ax[0].axvspan(D(quiet_from[0])[0], D(quiet_from[1])[0], ymin=0, ymax=0.04, color="#2ecc71", lw=0)

    if segs:
        f = segs[0][1]
        band = (f >= A.F_MIN) & (f <= SPEC_FMAX)
        img = np.array([np.log10(p[band] / np.median(p[band])) for _t, _f, p, _n in segs]).T
        ts = np.array([t for t, _f, _p, _n in segs])
        fb = f[band]
        fedge = np.r_[fb[0] - (fb[1] - fb[0]) / 2, (fb[:-1] + fb[1:]) / 2, fb[-1] + (fb[1] - fb[0]) / 2]
        # pieces need not be next to each other (gaps); draw each as its own column
        for i, t in enumerate(ts):
            ax[1].pcolormesh(D([t, t + SEG_S]), fedge, img[:, i:i + 1], shading="flat",
                             vmin=-0.5, vmax=2.0, cmap="magma")
        ax[1].axhspan(0.1, 0.5, xmin=0, xmax=0.008, color="#2ecc71")
        ax[1].set_ylabel("rate (Hz)\nrhythms, 10 min")
        ax[1].set_ylim(A.F_MIN, SPEC_FMAX)

    ax[2].plot(D(mn[:, 0]), mn[:, 3], lw=0.8, color="#d35400")
    ax[2].set_ylabel("RSSI dBm\n(per min)")
    ax[3].plot(D(mn[:, 0]), mn[:, 1], lw=0.8, color="#16a085", label="packets/s")
    ax[3].set_ylabel("packets/s")
    a3 = ax[3].twinx()
    a3.plot(D(mn[:, 0]), mn[:, 2], lw=0.8, color="0.4", label="% lost")
    if loss is not None:
        a3.plot(D(loss[:, 0]), loss[:, 4], lw=0.6, color="#8e44ad", label="% path lost (10 s)")
        a3.legend(loc="upper right", fontsize=7)
        for s_, e_, _pk in path_rises:
            for x in ax:
                x.axvspan(D(s_)[0], D(e_)[0], color="#8e44ad", alpha=0.15, lw=0)
    a3.set_ylabel("% lost")
    span_h = (t_last - t_first) / 3600
    ax[3].xaxis.set_major_locator(mdates.MinuteLocator(byminute=[0] if span_h > 6 else [0, 30]))
    ax[3].xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax[3].set_xlabel("clock time")
    ax[0].set_title(f"{os.path.basename(os.path.abspath(a.folder))}"
                    + (f"  [{scene[:60]}]" if scene else "")
                    + f": red = events, amber = low data, gray = gaps, purple = path loss rises, "
                      f"dashed = event line ({usual_from})"
                    + ("; green bar = quiet stretch" if quiet_from else "")
                    + "; green tick on strip 2 = 0.1-0.5 Hz (breathing band)", fontsize=8)
    fig.tight_layout()
    out_png = os.path.join(a.folder, "view.png")
    fig.savefig(out_png, dpi=110)
    print(f"saved {out_txt}\nsaved {out_png}" + ("" if a.usual else f"\nsaved {os.path.join(a.folder, 'usual.json')}"))


if __name__ == "__main__":
    main()
