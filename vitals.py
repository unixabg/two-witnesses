#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
#
# The method (motion and breathing in dB above the receiver's own noise floor, from the
# sub-carrier covariance; the breathing rate by harmonic summation) is that of wifisense-pi,
# Copyright The-Masked-Bear, Apache-2.0: https://github.com/The-Masked-Bear/wifisense-pi
# (pi/wifisense/dsp/motion.py, breathing.py, csi.py). Ported here to the kit's records and to
# numpy alone; the numbers (bands, windows, thresholds) are theirs.
"""vitals.py - does a link see a person who is still? An offline test on a run folder.

    ~/scratch-esp/bin/python vitals.py RUN_FOLDER [--from HH:MM] [--to HH:MM] [--link 2-1]
                                                  [--csv FILE] [--step 10]

For each link (both directions of a pair together: "1-2" is 1->2 and 2->1), every --step
seconds, from the kit's records (TWR) and the boards' own clock:

  motion   dB above the noise floor, 1.5-9 Hz, the last 6 s: movement (walking, a hand)
  vitals   dB above the noise floor, 0.13-0.7 Hz, the last 30 s: what a still body still
           does (the chest, breathing)
  breath   the breathing rate (breaths a minute) and how sure (0-1), the last 45 s, when the
           link is not swamped by movement
  state    empty / still (vitals at presence two steps in a row, no movement) / moving

The reference is the receiver's own noise (the sub-carriers' independent part), not a learned
quiet level: an empty room is 0 dB, and someone who stays there is not learned away. What
counts as a person: wifisense-pi's thresholds (motion on at 6 dB, presence at 4.5 dB).

Prints a line a minute for each link (the most it saw that minute), and with --csv every
step. Nothing here changes the boards or the run.
"""
import argparse
import collections
import csv
import datetime as dt
import glob
import gzip
import os
import re
import sys
import zlib

import numpy as np

ANALYSIS_HZ = 20.0
MOTION_BAND, VITAL_BAND = (1.5, 9.0), (0.13, 0.70)
MOTION_WINDOW_S, VITAL_WINDOW_S, BREATH_WINDOW_S = 6.0, 30.0, 45.0
BREATH_MIN_S = 20.0
MOTION_ON_DB, PRESENCE_ON_DB = 6.0, 4.5
BREATH_LOW_HZ, BREATH_HIGH_HZ, HARMONICS = 0.13, 0.60, 7
BREATH_VALID = 0.42                 # their confidence to report a rate
KEEP_S = BREATH_WINDOW_S + 5


def read_lines(path):
    if not path.endswith(".gz"):
        yield from open(path, errors="replace")
        return
    try:
        with gzip.open(path, "rt", errors="replace") as f:
            yield from f
    except (EOFError, gzip.BadGzipFile, zlib.error):
        print(f"note: {path} ends early (cut off); read up to the break", file=sys.stderr)


def lltf(amp, n, phy):
    """the 52 L-LTF sub-carriers of a record: a compact record has only those; a full one has
    the whole buffer (esp-radar's table, as tw_lltf_ranges in the firmware)"""
    if n == 52:
        return amp
    if n < 59:
        return None
    if (phy >> 5) & 3:                  # a secondary channel (above or below)
        return np.concatenate([amp[6:32], amp[33:59]])
    return np.concatenate([amp[38:64], amp[1:27]])


# ---------------- the estimator (wifisense-pi's, in numpy) ----------------

def _bh(n):
    """Blackman-Harris window (as theirs, for its -92 dB sidelobes)"""
    k = np.arange(n) / max(n - 1, 1)
    return 0.35875 - 0.48829 * np.cos(2 * np.pi * k) + 0.14128 * np.cos(4 * np.pi * k) \
        - 0.01168 * np.cos(6 * np.pi * k)


def _welch(x, fs, nperseg):
    """mean periodogram of half-overlapping, linearly detrended, windowed segments (as
    scipy.signal.welch with window="blackmanharris", detrend="linear"); x is (T,) or (T, C)"""
    x = x if x.ndim == 2 else x[:, None]
    step = max(1, nperseg // 2)
    w = _bh(nperseg)
    tt = np.arange(nperseg) - (nperseg - 1) / 2
    acc, cnt = 0, 0
    for s in range(0, x.shape[0] - nperseg + 1, step):
        seg = x[s:s + nperseg]
        seg = seg - seg.mean(axis=0) - np.outer(tt, (tt @ seg) / (tt @ tt))
        acc = acc + np.abs(np.fft.rfft(seg * w[:, None], axis=0)) ** 2
        cnt += 1
    freqs = np.fft.rfftfreq(nperseg, 1 / fs)
    return freqs, (acc / max(cnt, 1)) / (fs * (w ** 2).sum())


def _raw_band_snr(centred, band):
    try:
        u, sv, _ = np.linalg.svd(centred, full_matrices=False)
    except np.linalg.LinAlgError:
        return 0.0, 0.0
    if sv.size < 8 or sv[0] <= 0:
        return 0.0, 0.0
    comps = u * sv
    nperseg = min(comps.shape[0], max(64, comps.shape[0] // 2))
    freqs, sig = _welch(comps[:, 0], ANALYSIS_HZ, nperseg)
    lo = max(4, comps.shape[1] // 2)
    tail = comps[:, lo:lo + 12]
    if tail.shape[1] == 0:
        return 0.0, 0.0
    _, tpsd = _welch(tail, ANALYSIS_HZ, nperseg)
    noise = float(np.median(tpsd))
    if noise <= 0 or not np.isfinite(noise):
        return 0.0, 0.0
    sel = (freqs >= band[0]) & (freqs <= min(band[1], ANALYSIS_HZ / 2 * 0.95))
    if not sel.any():
        return 0.0, 0.0
    return 10 * np.log10(max(float(np.mean(sig[sel, 0])) / noise, 1e-12)), noise


_bias = {}


def _noise_bias_db(T, K, band):
    """what the estimator says of pure white noise of the same shape (theirs: about 7 dB),
    measured once per shape and taken off, so an empty room reads 0 dB"""
    key = (T, K, band)
    if key not in _bias:
        rng = np.random.default_rng(0xC51)
        trials = []
        for _ in range(7):
            x = rng.standard_normal((T, K))
            db, lvl = _raw_band_snr(x - x.mean(axis=0), band)
            if lvl > 0:
                trials.append(db)
        _bias[key] = float(np.median(trials)) if trials else 0.0
    return _bias[key]


def uniform(t, rows, window_s, now):
    """the last window_s seconds of (t, rows) on a 20 Hz grid (linear); None if under 80% covered"""
    sel = t >= now - window_s
    if sel.sum() < 32:
        return None
    tw, rw = t[sel], rows[sel]
    gaps = np.diff(tw)
    if tw[-1] - tw[0] < 0.8 * window_s or (gaps.size and gaps.max() > 1.0):
        return None
    grid = np.arange(tw[0], tw[-1], 1 / ANALYSIS_HZ)
    out = np.empty((grid.size, rw.shape[1]))
    for k in range(rw.shape[1]):
        out[:, k] = np.interp(grid, tw, rw[:, k])
    return out


def band_db(t, rows, window_s, band, now):
    u = uniform(t, rows, window_s, now)
    if u is None or u.shape[0] < 64 or u.shape[1] < 8:
        return None
    c = u - u.mean(axis=0)
    raw, noise = _raw_band_snr(c, band)
    if noise <= 0:
        return None
    return float(np.clip(raw - _noise_bias_db(c.shape[0], c.shape[1], band), -20, 60))


def breath_rate(t, rows, now):
    """(breaths a minute, confidence 0-1) by harmonic summation, or None"""
    u = uniform(t, rows, BREATH_WINDOW_S, now)
    if u is None or u.shape[0] < int(BREATH_MIN_S * ANALYSIS_HZ):
        return None
    c = u - u.mean(axis=0)
    sens = c.std(axis=0)                                  # the sub-carriers that respond
    keep = sens > np.quantile(sens, 0.5)
    if keep.sum() >= 4:
        c = c[:, keep]
    try:
        uu, sv, _ = np.linalg.svd(c, full_matrices=False)
    except np.linalg.LinAlgError:
        return None
    x = uu[:, 0] * sv[0]
    n = x.size
    nfft = int(2 ** np.ceil(np.log2(n * 4)))
    psd = np.abs(np.fft.rfft((x - x.mean()) * np.hanning(n), n=nfft)) ** 2
    freqs = np.fft.rfftfreq(nfft, 1 / ANALYSIS_HZ)
    band = (freqs >= BREATH_LOW_HZ) & (freqs <= BREATH_HIGH_HZ)
    if not band.any():
        return None
    bpsd, bfreq = psd[band], freqs[band]
    score = np.zeros_like(bpsd)
    for i, f0 in enumerate(bfreq):
        tot = 0.0
        for h in range(1, HARMONICS + 1):
            if f0 * h > ANALYSIS_HZ / 2 * 0.95:
                break
            tot += psd[int(np.argmin(np.abs(freqs - f0 * h)))] / h
        score[i] = tot
    pk = int(np.argmax(score))
    floor = float(np.median(bpsd)) or 1e-12
    conf = float(np.clip(np.log10(max(bpsd[pk] / floor, 1.0)) / 1.7, 0, 1))
    share = float(bpsd[np.abs(bfreq - bfreq[pk]) < 0.04].sum() / (bpsd.sum() or 1e-12))
    conf *= float(np.clip(share * 3.0, 0.3, 1.0))
    return float(bfreq[pk] * 60), conf


# ---------------- the run: records into links, links into steps ----------------

class Link:
    """one direction's records: (board clock seconds, 52 gain-normalised amplitudes)"""

    def __init__(self):
        self.above = 0                                    # steps in a row with vitals at presence
        self.t = collections.deque()
        self.a = collections.deque()
        self.wrap = 0
        self.last = None

    def add(self, net_us, amp):
        if self.last is not None and net_us < self.last - 2 ** 31:
            self.wrap += 2 ** 32                          # the 32-bit microsecond clock wrapped
        self.last = net_us
        ts = (net_us + self.wrap) / 1e6
        if self.t and abs(ts - self.t[-1]) > 30:          # the clock jumped (a restart): start over
            self.t.clear()
            self.a.clear()
        nrm = np.linalg.norm(amp)
        if nrm <= 0:
            return
        self.t.append(ts)
        self.a.append(amp / nrm)                          # their AGC fix: keep the shape only
        while self.t and self.t[0] < ts - KEEP_S:
            self.t.popleft()
            self.a.popleft()

    def arrays(self):
        t = np.fromiter(self.t, float, len(self.t))
        o = np.argsort(t, kind="stable")                  # relayed records arrive a little late
        return t[o], np.array(self.a)[o]


def step(link):
    t, rows = link.arrays()
    if t.size < 64:
        return None
    now = t[-1]
    m = band_db(t, rows, MOTION_WINDOW_S, MOTION_BAND, now)
    v = band_db(t, rows, VITAL_WINDOW_S, VITAL_BAND, now)
    if m is None:
        return None
    br = None
    if v is not None and max(m, v) >= PRESENCE_ON_DB and not (m > 18 or (m >= MOTION_ON_DB and m > v - 6)):
        br = breath_rate(t, rows, now)
    return m, v, br


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--from", dest="t_from", help="HH:MM (computer time) to start")
    ap.add_argument("--to", dest="t_to", help="HH:MM to stop")
    ap.add_argument("--link", help="only this pair, e.g. 2-1 (both directions)")
    ap.add_argument("--csv", help="every step to this file")
    ap.add_argument("--step", type=float, default=10.0, help="seconds between steps (10)")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")) + glob.glob(os.path.join(a.folder, "csi-*.csv")))
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    want = None
    if a.link:
        x, y = (int(v) for v in a.link.split("-"))
        want = {(x, y), (y, x)}

    def file_start(path):
        """when a file begins, from its name (listen.py: csi-YYYYMMDD-HH[MM][.partN].csv.gz)"""
        m = re.match(r"csi-(\d{8})-(\d{2})(\d{2})?", os.path.basename(path))
        if not m:
            return None
        return dt.datetime.strptime(m.group(1) + m.group(2) + (m.group(3) or "00"), "%Y%m%d%H%M")

    def after(hhmm, start):
        """the first HH:MM at or after start (a run from 23:00 and --from 04:00: the next morning)"""
        try:
            h, m = (int(v) for v in hhmm.split(":"))
            t = start.replace(hour=h, minute=m, second=0, microsecond=0)
        except ValueError:
            sys.exit(f"time HH:MM, not {hhmm}")
        return t if t >= start.replace(second=0, microsecond=0) else t + dt.timedelta(days=1)

    links, due, rows_out, minute = {}, {}, [], collections.defaultdict(dict)
    t_from = t_to = None
    starts = [file_start(f) for f in files]
    if a.t_from or a.t_to:
        run_start = next((s0 for s0 in starts if s0), None)
        if run_start is None:                             # names not as listen.py writes: the first line
            for line in read_lines(files[0]):
                if line[:1].isdigit():
                    run_start = dt.datetime.fromtimestamp(int(line.split(",", 1)[0]) / 1e9)
                    break
        if run_start is None:
            sys.exit("could not tell when the run began")
        t_from = after(a.t_from, run_start) if a.t_from else run_start
        t_to = after(a.t_to, t_from) if a.t_to else None
        # skip whole files outside the time: a file ends where the next begins
        if all(starts):
            keep = []
            for i, f in enumerate(files):
                end = starts[i + 1] if i + 1 < len(files) and starts[i + 1] > starts[i] else None
                if (end is None or end > t_from - dt.timedelta(seconds=KEEP_S)) and (t_to is None or starts[i] <= t_to):
                    keep.append(f)
            files = keep
        print(f"from {t_from:%Y-%m-%d %H:%M}" + (f" to {t_to:%Y-%m-%d %H:%M}" if t_to else "")
              + f": {len(files)} file{'s' if len(files) != 1 else ''}", file=sys.stderr)
    seen = set()
    done = False
    for path in files:
        for line in read_lines(path):
            f = line.rstrip("\n").split(",")
            if len(f) != 19 or f[1] != "TWR":
                continue
            try:
                host = int(f[0]) / 1e9
                rx, tx, kind, n, stp = int(f[2]), int(f[3]), int(f[15]), int(f[16]), int(f[17])
                if (kind & 0x0F) > 1 or (kind & 0x20) or len(f[18]) != 2 * n:
                    continue                              # not a measurement, or a second frame's
                key = (rx, int(f[7]), int(f[6]))
                if key in seen:
                    continue
                seen.add(key)
                if len(seen) > 2_000_000:
                    seen.clear()
                if want and (tx, rx) not in want:
                    continue
                when = dt.datetime.fromtimestamp(host)
                if t_from and when < t_from - dt.timedelta(seconds=KEEP_S):
                    continue                              # (the last KEEP_S before it: to warm up)
                if t_to and when > t_to:
                    done = True
                    break
                amp = np.frombuffer(bytes.fromhex(f[18]), dtype=np.uint8).astype(float) * (stp / 4)
                amp = lltf(amp, n, int(f[14]))
                if amp is None:
                    continue
                pair = f"{min(rx, tx)}-{max(rx, tx)}"
                lk = links.setdefault((tx, rx), Link())
                lk.add(int(f[9]), amp)
                if t_from and when < t_from:
                    continue                              # warming up: measured, not yet shown
                if host >= due.get((tx, rx), 0):
                    due[(tx, rx)] = host + a.step
                    r = step(lk)
                    if r is None:
                        continue
                    m, v, br = r
                    moving = m >= MOTION_ON_DB
                    # still: vitals at presence for two steps in a row (as their presence gate
                    # holds 2 s before it believes, so one noisy window is not a person)
                    lk.above = lk.above + 1 if v is not None and v >= PRESENCE_ON_DB else 0
                    present = moving or lk.above >= 2
                    state = "moving" if moving else "still" if present else "empty"
                    bpm, conf = br if br and state == "still" else (None, 0.0)   # a rate only for someone still
                    rows_out.append([when.strftime("%H:%M:%S"), f"{tx}->{rx}", f"{m:.1f}",
                                     "" if v is None else f"{v:.1f}", state,
                                     "" if bpm is None or conf < BREATH_VALID else f"{bpm:.1f}", f"{conf:.2f}"])
                    mk = when.strftime("%H:%M")
                    cur = minute[mk].get(pair)
                    if cur is None:
                        cur = minute[mk][pair] = dict(m=m, v=v if v is not None else -99, st=set(), bpm=[])
                    cur["m"] = max(cur["m"], m)
                    cur["v"] = max(cur["v"], v if v is not None else -99)
                    cur["st"].add(state)
                    if bpm is not None and conf >= BREATH_VALID:
                        cur["bpm"].append(bpm)
            except (ValueError, IndexError):
                continue                                  # a garbled line (bytes lost on the cable)
        if done:
            break
    if not minute:
        sys.exit("no link had enough records (at least 6 s of measurements) in that time")
    pairs = sorted({p for mm in minute.values() for p in mm}, key=lambda p: tuple(int(v) for v in p.split("-")))
    print("minute by minute, each pair's most that minute: motion dB / vitals dB, state, breaths a minute")
    print("  (dB above the receiver's own noise; a person: motion 6+ moving, else vitals 4.5+ still)")
    for mk in sorted(minute):
        cells = []
        for p in pairs:
            c = minute[mk].get(p)
            if not c:
                cells.append(f"{p}: -")
                continue
            st = "moving" if "moving" in c["st"] else "still" if "still" in c["st"] else "empty"
            br = f" {np.median(c['bpm']):.0f}/min" if c["bpm"] else ""
            cells.append(f"{p}: {c['m']:.0f}/{c['v']:.0f} {st}{br}")
        print(f"{mk}  " + "   ".join(cells))
    if a.csv:
        with open(a.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["time", "link", "motion_db", "vitals_db", "state", "breaths_min", "confidence"])
            w.writerows(rows_out)
        print(f"every step: {a.csv} ({len(rows_out)} rows)")


if __name__ == "__main__":
    main()
