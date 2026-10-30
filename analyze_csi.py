#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""analyze_csi.py - read a record_csi.py recording blind and list its rhythms.

Usage:
    python3 analyze_csi.py NAME.csv [--from S] [--to S] [--png]
    (NAME.csv.gz works too, e.g. one hour of a long run)

    --from/--to  seconds from the start of the file to keep (default: skip
                 the first 10 s, while the receiver settles its gain)
    --png        also save NAME.spectrum.png (needs matplotlib)

What it does (same recipe as the Chromebook RSSI files):
  1. time from the receiver's own microsecond clock (not the PC's)
  2. CSI amplitude per subcarrier; empty (guard) subcarriers dropped
  3. average into 10 Hz bins; fill the few empty bins by straight lines
  4. remove slow drift: subtract a 60 s moving average
  5. Hann periodogram per subcarrier, each scaled to equal total power,
     then averaged over subcarriers (and, separately, RSSI alone)
  6. each peak is judged against its neighboring rates: ratio = its power
     divided by the median of the rates around it. Starting thresholds:
     CSI x20, RSSI x40. Reset them from the empty run: its biggest peak is
     what chance plus the room's habits give (--notable, --notable-rssi).

It prints rates only: no labels, no guesses. Compare with the seal after.
"""
import argparse
import gzip
import sys
import warnings
import zlib

import numpy as np

BIN_HZ = 10.0
DETREND_S = 60.0
F_MIN, F_MAX = 0.03, 4.9
NEIGH = 20      # bins each side used as "neighbors"
GUARD = 2       # bins each side of a peak left out of its neighbors
NOTABLE = 20.0      # CSI (averaged over many subcarriers)
NOTABLE_RSSI = 40.0 # one series is rougher: empty fakes reach x21 by chance


def read_lines(path):
    """Lines of a .csv or .csv.gz; a gzip cut short (power off) is read up to
    the break."""
    if not path.endswith(".gz"):
        yield from open(path, errors="replace")
        return
    try:
        with gzip.open(path, "rt", errors="replace") as f:
            yield from f
    except (EOFError, gzip.BadGzipFile, zlib.error):
        print(f"note: {path} ends early (cut off); read up to the break", file=sys.stderr)


def parse(path):
    ids, ts, rssi, rows = [], [], [], []
    bad = 0
    for line in read_lines(path):
        if line.startswith("#") or "CSI_DATA" not in line:
            continue
        q = line.find('"')
        q2 = line.find('"', q + 1)
        if q < 0 or q2 < 0:
            bad += 1
            continue
        f = line[:q].split(",")
        try:
            if len(f) != 26:
                raise ValueError
            pkt, rs, ts_us, fw, ln = int(f[2]), int(f[4]), int(f[19]), int(f[24]), int(f[23])
        except (IndexError, ValueError):
            bad += 1                 # garbled line (serial hiccup)
            continue
        # numpy parses the bracket in one go (the old int() loop took minutes on an hour file);
        # a garbled bracket parses short and is caught by the length check
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                vals = np.fromstring(line[q + 1:q2].strip("[]"), sep=",", dtype=np.int16)
        except ValueError:
            bad += 1                 # garbled bracket (bytes lost on the serial link)
            continue
        if ln != len(vals):
            bad += 1
            continue
        if fw == 1:                  # first_word_invalid: first 4 bytes bad
            vals[:4] = 0
        ids.append(pkt)
        rssi.append(rs)
        ts.append(ts_us)
        rows.append(vals)
    if not rows:
        sys.exit("no CSI_DATA lines found")
    # keep the most common CSI length only
    lens = np.array([len(r) for r in rows])
    common = np.bincount(lens).argmax()
    keep = lens == common
    ids = np.array(ids)
    ts, rssi = (np.array(a, dtype=float) for a in (ts, rssi))
    # damaged receiver timestamps (a garbled header value such as -156): one sample whose
    # clock jumps wildly in and back out again, while its neighbors agree with each other
    WILD = 10e6                      # 10 s between two received packets is never the clock
    step = np.diff(ts) % 2.0 ** 32
    damaged = np.zeros(len(ts), dtype=bool)
    if len(ts) >= 3:
        around = (ts[2:] - ts[:-2]) % 2.0 ** 32
        damaged[1:-1] = (step[:-1] > WILD) & (step[1:] > WILD) & (around < WILD)
    keep &= ~damaged
    rows = [r for r, k in zip(rows, keep) if k]
    ids, ts, rssi = ids[keep], ts[keep], rssi[keep]
    iq = np.vstack(rows).astype(float).reshape(len(rows), -1, 2)
    amp = np.hypot(iq[..., 0], iq[..., 1])
    ts = np.unwrap(ts, period=2.0 ** 32)   # 32-bit microsecond clock
    t = (ts - ts[0]) / 1e6
    skipped = int((~keep).sum()) + bad
    return t, ids, rssi, amp, skipped, int(damaged.sum()), common


def bin_series(t, x, t0, t1):
    n = int((t1 - t0) * BIN_HZ)
    idx = ((t - t0) * BIN_HZ).astype(int)
    ok = (idx >= 0) & (idx < n)
    idx, x = idx[ok], x[ok]
    cnt = np.bincount(idx, minlength=n).astype(float)
    if x.ndim == 1:
        x = x[:, None]
    out = np.empty((n, x.shape[1]))
    for k in range(x.shape[1]):
        s = np.bincount(idx, weights=x[:, k], minlength=n)
        with np.errstate(invalid="ignore", divide="ignore"):
            out[:, k] = s / cnt
    empty = cnt == 0
    good = np.flatnonzero(~empty)
    for k in range(out.shape[1]):
        out[empty, k] = np.interp(np.flatnonzero(empty), good, out[good, k])
    return out, empty.mean()


def detrend(x):
    w = int(DETREND_S * BIN_HZ)
    kernel = np.ones(w)
    num = np.apply_along_axis(lambda c: np.convolve(c, kernel, "same"), 0, x)
    den = np.convolve(np.ones(len(x)), kernel, "same")[:, None]
    return x - num / den


def spectrum(x):
    win = np.hanning(len(x))[:, None]
    p = np.abs(np.fft.rfft(x * win, axis=0)) ** 2
    p /= p.sum(axis=0, keepdims=True) + 1e-30
    f = np.fft.rfftfreq(len(x), 1.0 / BIN_HZ)
    return f, p.mean(axis=1)


def peaks(f, p, top=8):
    band = np.flatnonzero((f >= F_MIN) & (f <= F_MAX))
    found = []
    for i in band:
        if not (p[i] > p[i - 1] and p[i] >= p[i + 1]):
            continue
        lo, hi = max(1, i - NEIGH), min(len(p), i + NEIGH + 1)
        neigh = np.r_[p[lo:max(lo, i - GUARD)], p[min(hi, i + GUARD + 1):hi]]
        if len(neigh) < 5:
            continue
        found.append((p[i] / np.median(neigh), f[i]))
    found.sort(reverse=True)
    return found[:top]


def report(title, f, p, notable):
    print(f"\n{title}")
    print("   ratio      Hz    period s   per min")
    for ratio, fr in peaks(f, p):
        mark = "  <- notable" if ratio >= notable else ""
        print(f"  {ratio:6.1f}  {fr:7.4f}  {1 / fr:9.2f}  {fr * 60:8.2f}{mark}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--from", dest="t0", type=float, default=10.0)
    ap.add_argument("--to", dest="t1", type=float, default=None)
    ap.add_argument("--png", action="store_true")
    ap.add_argument("--notable", type=float, default=NOTABLE,
                    help="CSI threshold; reset it from the empty run's biggest peak")
    ap.add_argument("--notable-rssi", type=float, default=NOTABLE_RSSI)
    a = ap.parse_args()

    t, ids, rssi, amp, dropped_len, damaged, csi_len = parse(a.csv)
    t1 = t[-1] if a.t1 is None else min(a.t1, t[-1])
    t0 = a.t0
    if t1 - t0 < 3 * DETREND_S:
        sys.exit(f"only {t1 - t0:.0f} s to analyze; need at least {3 * DETREND_S:.0f} s")

    live = np.median(amp, axis=0) > 1.0          # drop empty guard subcarriers
    a_bins, empty = bin_series(t, amp[:, live], t0, t1)
    r_bins, _ = bin_series(t, rssi, t0, t1)

    span_ids = ids.max() - ids.min() + 1
    print(f"file      {a.csv}")
    print(f"packets   {len(t)} (lost {span_ids - len(t)} of {span_ids} sent, "
          f"{100 * (span_ids - len(t)) / span_ids:.1f}%); garbled/odd lines skipped: {dropped_len}"
          + (f" (of which {damaged} with a damaged receiver timestamp)" if damaged else ""))
    print(f"rate      {len(t) / t[-1]:.1f}/s received over {t[-1]:.0f} s (receiver clock)")
    print(f"window    {t0:.0f}-{t1:.0f} s, {BIN_HZ:.0f} Hz bins, {100 * empty:.1f}% bins empty")
    print(f"CSI       {csi_len} bytes/packet, {live.sum()} live subcarriers of {amp.shape[1]}")
    print(f"RSSI      mean {rssi.mean():.1f} dBm, sd {rssi.std():.2f} dB")
    print(f"resolution {BIN_HZ / len(a_bins):.4f} Hz; neighbors +-{NEIGH} bins")

    f, p_csi = spectrum(detrend(a_bins))
    _, p_rssi = spectrum(detrend(r_bins))
    report(f"CSI amplitude (all live subcarriers), notable >= x{a.notable:.0f}", f, p_csi, a.notable)
    report(f"RSSI alone, notable >= x{a.notable_rssi:.0f}", f, p_rssi, a.notable_rssi)

    if a.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        band = (f >= F_MIN) & (f <= F_MAX)
        fig, ax = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
        ax[0].semilogy(f[band], p_csi[band], lw=0.8)
        ax[0].set_ylabel("CSI power")
        ax[1].semilogy(f[band], p_rssi[band], lw=0.8, color="tab:orange")
        ax[1].set_ylabel("RSSI power")
        ax[1].set_xlabel("rate (Hz)")
        ax[0].set_title(a.csv)
        out = a.csv.removesuffix(".gz").rsplit(".", 1)[0] + ".spectrum.png"
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        print(f"\nsaved {out}")


if __name__ == "__main__":
    main()
