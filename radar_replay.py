#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""radar_replay.py - Espressif's esp-radar motion/presence detector, replayed on a
recorded run, so its calls can be set beside ours (watch.py) and your labels.

Usage:
    ~/scratch-esp/bin/python radar_replay.py RUN_FOLDER [--train HH:MM-HH:MM]
                                             [--emulate] [--check N]
    e.g. ~/scratch-esp/bin/python radar_replay.py ~/tw-data/watch/2026-10-27_0607 --train 08:00-09:00

What it reproduces (esp-radar 0.3.4 + esp_radar_motion_dec 0.1.1 + the
console_test example, as on an ESP32-S3 at Espressif's defaults):
  - per packet: the L-LTF part of the CSI, every 4th subcarrier (13 values),
    amplitude = sqrt(I^2 + Q^2)
  - windows: about 20 packets (200 ms rules, cleared after a 100 ms gap in the
    receiver's clock), each window overlapping the last by half
  - per window: the window's main "shape" (top eigenvector of its 13-subcarrier
    picture, power method, as in Espressif's compiled library); jitter = the
    largest 1-|correlation| between this shape and the previous 3 (movement);
    wander = the smallest 1-|correlation| to the shapes learned in training
    (presence; only after --train)
  - decision (console_test): "move" when 2 of the last 5 windows have jitter
    above 5 x threshold (0.0003 untrained, or the training's quiet maximum) or
    above 5 x the median of the last 25 and over 0.0002; "someone" when the
    lower-half mean of the last 25 wanders is above threshold / 0.15
  - --train: calibrates on that stretch as `radar --train_start/--train_stop`
    would (keep the stretch empty), then replays the whole run with the result

Espressif ships the core math only as a compiled library. This tool carries a
numpy copy of it, worked out from the library's symbols, debug info and
probing, and checked against the original running under an emulator
(--emulate uses that original itself: needs `pip install unicorn pyelftools`
and radar_c3.elf beside this file; about 100x slower). --check N runs both on
the first N windows and prints the largest difference.

As on a board just switched on, the first second or two can call "move"
(its 25-window buffers start at zero).

Known differences from a board running esp-radar live: our receiver firmware
(csi_recv) turns on channel_filter_en and ltf_merge_en (esp-radar leaves them
off) and applies its own gain compensation, so the CSI is slightly smoothed;
the board's float rounding is not copied bit for bit.

Writes into RUN_FOLDER:
    radar.csv          every window: time, packets, jitter, wander, move, someone
    radar_events.csv   runs of "move" (merged when under 3 s apart, as the
                       board's 3 s LED hold), with the watch events they overlap
    radar.txt          settings, training result, and the comparison with
                       detect/events.csv (by kind, and by label once you fill it)
    radar.png          jitter through the run, move marks, watch events
"""
import argparse
import csv
import datetime as dt
import glob
import os
import sys
import warnings

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines: reads gzip cut short by a power cut)

# ---------------- esp-radar defaults (ESP_RADAR_*_CONFIG_DEFAULT, console_test) ----------------
CSI_RECV_INTERVAL_MS = 10            # console_test: 1000 / 100 Hz
CSI_HANDLE_TIME_MS = 200
DEC_WINDOW_SIZE = 4
SUB_CARRIER_STEP = 4
HANDLE_WINDOW = (CSI_HANDLE_TIME_MS // CSI_RECV_INTERVAL_MS) * 2        # 40 frames
BUFF_SIZE = HANDLE_WINDOW + 20                                           # 60
CSI_WANDER_NUM = 10
CSI_WANDER_THRESHOLD = 0.002
RADAR_OUTLIERS_THRESHOLD = 0.005
ITRS_MAX, DELTA_MIN = 30, 1e-4       # power method, from the library's constants
RADAR_BUFF_MAX_LEN = 25              # console_test
PREDICT_BUFF_SIZE = 5
PREDICT_OUTLIERS_NUMBER = 2
SOMEONE_SENSITIVITY = 0.15
MOVE_SENSITIVITY = 0.20
MOVE_THRESHOLD_DEFAULT = 0.0003
EVENT_MERGE_S = 3.0

# L-LTF byte ranges in the CSI buffer (esp-radar csi_sub_carrier_table.c, ESP32/S2/S3/C3)
# key: (secondary channel 0 none / 1 above / 2 below, signal mode 0 non-HT / 1 HT, cwb, stbc, length)
_LLTF_NONE = ((76, 128), (2, 54))
_LLTF_SEC = ((12, 64), (66, 118))
LLTF_TABLE = {
    (0, 0, 0, 0, 128): _LLTF_NONE, (0, 1, 0, 0, 256): _LLTF_NONE, (0, 1, 0, 1, 384): _LLTF_NONE,
}
for _sec in (1, 2):
    LLTF_TABLE.update({(_sec, 0, 0, 0, 128): _LLTF_SEC, (_sec, 1, 0, 0, 256): _LLTF_SEC,
                       (_sec, 1, 0, 1, 380): _LLTF_SEC, (_sec, 1, 1, 0, 384): _LLTF_SEC,
                       (_sec, 1, 1, 1, 612): _LLTF_SEC})


def set_interval(ms):
    """esp-radar sizes its windows from the expected packet interval (console_test:
    --send_data_interval); set it as the board would be configured for this recording's rate."""
    global CSI_RECV_INTERVAL_MS, HANDLE_WINDOW, BUFF_SIZE
    CSI_RECV_INTERVAL_MS = ms
    HANDLE_WINDOW = (CSI_HANDLE_TIME_MS // CSI_RECV_INTERVAL_MS) * 2
    BUFF_SIZE = HANDLE_WINDOW + 20


def lltf_index(key):
    rng = LLTF_TABLE.get(key)
    if rng is None and key[4] == 128:          # e.g. the fake board's lines
        rng = _LLTF_NONE if key[0] == 0 else _LLTF_SEC
    if rng is None and key[4] == 2 * 52:       # tw_node compact record: the 52 L-LTF subcarriers only
        rng = ((0, 104),)
    if rng is None:
        return None
    idx = np.concatenate([np.arange(a, b) for a, b in rng])          # bytes, I/Q interleaved
    n_sc = len(idx) // 2 // SUB_CARRIER_STEP
    first = idx[np.arange(n_sc) * SUB_CARRIER_STEP * 2]
    return first, idx[np.arange(n_sc) * SUB_CARRIER_STEP * 2 + 1]


# ---------------- the math (numpy copy of esp_radar_motion_dec) ----------------
def dec_compute(X):
    """Top eigenvector (power method, max-normalized) of the window's row Gram
    matrix, projected back onto the subcarriers, divided by the row count."""
    X = np.asarray(X, dtype=np.float64)
    rows = X.shape[0]
    G = X @ X.T / X.shape[1]
    v = np.ones(rows)
    lam_last = 0.0
    for _ in range(ITRS_MAX):
        w = G @ v
        lam = np.max(np.abs(w))
        if lam == 0:
            break
        v = w / lam
        if abs(lam - lam_last) < DELTA_MIN:
            break
        lam_last = lam
    return (X.T @ v) / rows


def dec_wander(a, b):
    da = a - a.mean()
    db = b - b.mean()
    den = np.sqrt((da * da).sum() * (db * db).sum())
    return 0.0 if den == 0 else float(1.0 - abs((da * db).sum() / den))


def dec_jitter(current, history, buff_num):
    win = len(history)
    if buff_num < win:
        return 0.0
    own = (buff_num - 1) % win
    return max(dec_wander(current, history[i]) for i in range(win) if i != own)


class EmulatedDec:
    """Espressif's compiled library (ESP32-C3 build) under the unicorn emulator."""

    def __init__(self):
        here = os.path.dirname(os.path.abspath(__file__))
        sys.path.insert(0, here)
        from radar_emu import MotionDec
        self.m = MotionDec(os.path.join(here, "radar_c3.elf"))

    def compute(self, X):
        rc, out = self.m.compute(np.asarray(X, np.float32))
        return out.astype(np.float64)

    def jitter(self, current, history, buff_num):
        return float(self.m.jitter(current, history, buff_num))

    def wander(self, a, b):
        return float(self.m.wander(a, b))


# ---------------- reading the run ----------------
def packets(paths):
    """Yield (host_s, rx_ms, amplitudes[13]) per usable packet, in file order."""
    cache = {}
    for path in paths:
        print(f"reading {os.path.basename(path)}", flush=True)
        for line in A.read_lines(path):
            if "CSI_DATA" not in line or line.startswith("#"):
                continue
            q = line.find('"')
            q2 = line.rfind("]")
            if q < 0 or q2 < 0:
                continue
            f = line[:q].split(",")
            if len(f) != 26 or line.count("CSI_DATA") != 1:
                continue
            try:
                host_ns, sig, cwb, stbc = int(f[0]), int(f[6]), int(f[8]), int(f[12])
                sec, ts_us, ln = int(f[18]), int(f[19]), int(f[23])
            except ValueError:
                continue
            key = (sec, sig, cwb, stbc, ln)
            if key not in cache:
                cache[key] = lltf_index(key)
            ix = cache[key]
            if ix is None:
                continue
            try:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore")
                    v = np.fromstring(line[q + 2:q2], sep=",", dtype=np.float64)
            except ValueError:
                continue                # a garbled line (bytes lost on the serial link)
            if v.size != ln:
                continue
            amp = np.hypot(v[ix[0]], v[ix[1]])
            yield host_ns / 1e9, (ts_us & 0xFFFFFFFF) // 1000, amp


# ---------------- esp-radar's windowing and training, mirrored ----------------
class Radar:
    def __init__(self, dec):
        self.dec = dec
        self.ring = None
        self.ts = np.zeros(BUFF_SIZE, dtype=np.int64)
        self.host = np.zeros(BUFF_SIZE)
        self.next_seq = 0
        self.window_start = 0
        self.last_ts = 0
        self.relaxed = False
        self.motion = [None] * DEC_WINDOW_SIZE
        self.motion_num = 0
        # training state
        self.training = False
        self.records = []                 # up to CSI_WANDER_NUM shapes of the empty room
        self.cal_buff = [0.0] * 3
        self.cal_buff_size = 0
        self.none_sum = 0.0
        self.none_count = 0.0
        self.static_wander = 0.0
        self.wander_last = 1.0
        self.data_num = 0

    def _window_update(self):
        """csi_window_update(): returns (begin, window) when a window is ready."""
        n = self.next_seq
        begin = self.window_start % BUFF_SIZE
        end = n % BUFF_SIZE
        window = n - self.window_start
        spent = int(self.ts[end] - self.ts[begin])
        delta = int(self.ts[end] - self.last_ts)
        strict = CSI_HANDLE_TIME_MS // 2
        limit = CSI_HANDLE_TIME_MS if self.relaxed else strict
        ready = None
        if self.relaxed and 0 <= delta <= strict:
            self.relaxed = False
        if delta < 0 or delta > limit:
            if delta > strict:
                self.relaxed = True
            self.window_start = n
        elif spent >= CSI_HANDLE_TIME_MS * 2 or window >= HANDLE_WINDOW // 2:
            if window < HANDLE_WINDOW // 3:
                self.window_start = n
            else:
                ready = (begin, window)
                self.window_start += window // 2
        self.last_ts = int(self.ts[end])
        self.next_seq += 1
        return ready

    def push(self, host_s, rx_ms, amp):
        if self.ring is None:
            self.ring = np.zeros((BUFF_SIZE, len(amp)))
        if len(amp) != self.ring.shape[1]:
            return None
        i = self.next_seq % BUFF_SIZE
        self.ring[i] = amp
        self.ts[i] = rx_ms & 0xFFFFFFFF
        self.host[i] = host_s
        ready = self._window_update()
        if ready is None:
            return None
        begin, window = ready
        idx = (begin + np.arange(window)) % BUFF_SIZE
        return self._detect(self.ring[idx], self.host[idx[-1]], window)

    def _detect(self, X, host_s, rows):
        cur = self.dec.compute(X)
        self.motion[self.motion_num % DEC_WINDOW_SIZE] = cur
        self.motion_num += 1
        if self.motion_num >= DEC_WINDOW_SIZE:
            jitter = self.dec.jitter(cur, self.motion, self.motion_num)
        else:
            jitter = 0.0
        wander = 0.0
        if self.records:
            wander = min(self.dec.wander(r, cur) for r in self.records[:CSI_WANDER_NUM])
        if self.training and self.motion_num >= 2:
            wander = self._train_sample(jitter, wander)
        return host_s, rows, jitter, wander

    def _train_sample(self, jitter, wander):
        """csi_training_collect_sample()"""
        n = self.cal_buff_size
        i1, i2, i3 = (n + 3 - 2) % 3, (n + 3 - 1) % 3, n % 3
        self.cal_buff[i3] = jitter
        self.cal_buff_size += 1
        if self.cal_buff_size < 3:
            return wander
        first, second, third = self.cal_buff[i1], self.cal_buff[i2], self.cal_buff[i3]
        if first - second > RADAR_OUTLIERS_THRESHOLD and third - second > RADAR_OUTLIERS_THRESHOLD:
            return wander
        if self.static_wander < jitter:
            self.static_wander = second
        if self.wander_last > CSI_WANDER_THRESHOLD:
            prev = self.motion[(self.motion_num + DEC_WINDOW_SIZE - 2) % DEC_WINDOW_SIZE]
            if prev is not None:
                k = self.data_num % CSI_WANDER_NUM
                if k < len(self.records):
                    self.records[k] = prev.copy()
                else:
                    self.records.append(prev.copy())
                self.data_num += 1
                wander = 0.0
        else:
            if self.wander_last > 0.00001:
                self.none_sum += self.wander_last
                self.none_count += 1.0
        self.wander_last = wander
        return wander

    def train_start(self):
        self.training = True
        self.records, self.data_num = [], 0
        self.cal_buff, self.cal_buff_size = [0.0] * 3, 0
        self.none_sum = self.none_count = self.static_wander = 0.0
        self.wander_last = 1.0
        self.motion_num = 0

    def train_stop(self):
        self.training = False
        if not self.data_num or self.none_count == 0:
            return None
        return self.none_sum / self.none_count, self.static_wander


class Console:
    """console_test's wifi_radar_cb decision rule (float32 like the board)."""

    def __init__(self, someone_thr=0.0, move_thr=MOVE_THRESHOLD_DEFAULT):
        self.w = np.zeros(RADAR_BUFF_MAX_LEN, np.float32)
        self.j = np.zeros(RADAR_BUFF_MAX_LEN, np.float32)
        self.count = 0
        self.someone_thr = np.float32(someone_thr)
        self.move_thr = np.float32(move_thr)

    def step(self, wander, jitter):
        self.w[self.count % RADAR_BUFF_MAX_LEN] = wander
        self.j[self.count % RADAR_BUFF_MAX_LEN] = jitter
        self.count += 1
        if self.count < PREDICT_BUFF_SIZE:
            return None
        srt = np.sort(self.w)
        wander_avg = float(srt[:int(RADAR_BUFF_MAX_LEN * 0.5 + 0.5)].mean())
        jitter_med = float(np.sort(self.j)[RADAR_BUFF_MAX_LEN // 2])
        someone = wander_avg * SOMEONE_SENSITIVITY > self.someone_thr
        moves = 0
        for i in range(PREDICT_BUFF_SIZE):
            jj = self.j[(self.count - 1 - i) % RADAR_BUFF_MAX_LEN]
            if jj * MOVE_SENSITIVITY > self.move_thr or (jj * MOVE_SENSITIVITY > jitter_med and jj > 0.0002):
                moves += 1
        return wander_avg, jitter_med, bool(someone), moves >= PREDICT_OUTLIERS_NUMBER


# ---------------- helpers ----------------
def clock_stretch(spec, t_ref):
    a_, b_ = spec.split("-")
    ha, ma = (int(x) for x in a_.split(":"))
    hb, mb = (int(x) for x in b_.split(":"))
    day = dt.datetime.fromtimestamp(t_ref).replace(hour=0, minute=0, second=0, microsecond=0)
    start = day + dt.timedelta(hours=ha, minutes=ma)
    if start.timestamp() + 12 * 3600 < t_ref:
        start += dt.timedelta(days=1)
    end = start.replace(hour=hb, minute=mb)
    if end <= start:
        end += dt.timedelta(days=1)
    return start.timestamp(), end.timestamp()


def hms(t):
    return dt.datetime.fromtimestamp(t).strftime("%H:%M:%S")


def run_files(folder):
    if os.path.isfile(folder):
        return [folder]
    return sorted(glob.glob(os.path.join(folder, "csi-*.csv.gz")) + glob.glob(os.path.join(folder, "night-*.csv.gz")))


def watch_events(folder):
    """detect/events.csv of a watch run, with unix start/end times."""
    path = os.path.join(folder, "detect", "events.csv")
    if not os.path.exists(path):
        return []
    out = []
    for r in csv.DictReader(open(path)):
        try:
            t0 = dt.datetime.strptime(r["event"], "%Y%m%d-%H%M%S").timestamp()
        except (KeyError, ValueError):
            continue
        r["t0"], r["t1"] = t0, t0 + float(r.get("seconds") or 1) - 1
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", help="a run folder (or one csi-*.csv.gz file)")
    ap.add_argument("--train", metavar="HH:MM-HH:MM", help="calibrate on this stretch (keep it empty)")
    ap.add_argument("--emulate", action="store_true", help="use Espressif's compiled library under emulation (slow)")
    ap.add_argument("--check", type=int, default=0, metavar="N",
                    help="compare the numpy copy with the emulated original on the first N windows, then stop")
    ap.add_argument("--slack", type=float, default=5.0, help="seconds of slack when matching events (default 5)")
    ap.add_argument("--interval-ms", type=int, default=0,
                    help="expected packet interval as configured on the board (default: from the recording; "
                         "esp-radar's own default is 10 ms, 100 packets/s)")
    a = ap.parse_args()

    files = run_files(a.folder)
    if not files:
        sys.exit(f"no csi-*.csv.gz in {a.folder}")
    folder = a.folder if os.path.isdir(a.folder) else os.path.dirname(os.path.abspath(a.folder))

    class Fast:
        compute = staticmethod(dec_compute)
        jitter = staticmethod(dec_jitter)
        wander = staticmethod(dec_wander)

    if a.check:
        emu = EmulatedDec()
        rf, re_ = Radar(Fast()), Radar(emu)
        worst = dict(jitter=0.0, shape=0.0)
        n = 0
        for p in packets(files):
            of = rf.push(*p)
            oe = re_.push(*p)
            if of is None:
                continue
            cf = rf.motion[(rf.motion_num - 1) % DEC_WINDOW_SIZE]
            ce = re_.motion[(re_.motion_num - 1) % DEC_WINDOW_SIZE]
            worst["shape"] = max(worst["shape"], float(np.max(np.abs(cf - ce) / np.maximum(np.abs(ce), 1e-9))))
            worst["jitter"] = max(worst["jitter"], abs(of[2] - oe[2]))
            n += 1
            if n >= a.check:
                break
        print(f"checked {n} windows: largest relative difference in the window shape {worst['shape']:.2e}, "
              f"largest difference in jitter {worst['jitter']:.2e}")
        return

    dec = EmulatedDec() if a.emulate else Fast()
    trained = None
    t_first = None
    if a.interval_ms:
        set_interval(a.interval_ms)
    else:                                   # from the recording: median gap between packets
        gaps, last = [], None
        for _h, rx_ms, _amp in packets(files[:1]):
            if last is not None and 0 < rx_ms - last < 1000:
                gaps.append(rx_ms - last)
            last = rx_ms
            if len(gaps) >= 3000:
                break
        if gaps:
            med = float(np.median(gaps))
            ms = min((10, 20, 25, 40, 50, 100), key=lambda v: abs(v - med))
            set_interval(ms)
    print(f"packet interval {CSI_RECV_INTERVAL_MS} ms: windows of {HANDLE_WINDOW // 2}-{HANDLE_WINDOW} packets", flush=True)
    if a.train:
        r = Radar(dec)
        for host_s, rx_ms, amp in packets(files):
            if t_first is None:
                t_first = host_s
                ta, tb = clock_stretch(a.train, t_first)
            if host_s < ta:
                continue
            if host_s >= tb:
                break
            if not r.training and r.data_num == 0 and not r.records:
                r.train_start()
            r.push(host_s, rx_ms, amp)
        res = r.train_stop()
        if res is None:
            sys.exit(f"--train {a.train}: no training result (too little data in that stretch?)")
        trained = dict(records=[x.copy() for x in r.records], data_num=r.data_num,
                       wander_thr=res[0], jitter_thr=res[1], stretch=(ta, tb))
        print(f"trained on {hms(ta)}-{hms(tb)}: {r.data_num} shapes kept, "
              f"wander threshold {res[0]:.6f}, jitter threshold {res[1]:.6f}")

    radar = Radar(dec)
    if trained:
        radar.records = trained["records"]
        radar.data_num = trained["data_num"]
        console = Console(trained["wander_thr"], trained["jitter_thr"])
    else:
        console = Console()
    rows_out = []
    for host_s, rx_ms, amp in packets(files):
        o = radar.push(host_s, rx_ms, amp)
        if o is None:
            continue
        t, n_rows, jit, wan = o
        d = console.step(wan, jit)
        if d is None:
            continue
        rows_out.append((t, n_rows, jit, wan, d[0], d[1], int(d[2]), int(d[3])))
    if not rows_out:
        sys.exit("no windows: is this a CSI recording?")
    R = np.array(rows_out)
    with open(os.path.join(folder, "radar.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["time", "clock", "rows", "jitter", "wander", "wander_avg", "jitter_median", "someone", "move"])
        for t, n_rows, jit, wan, wa, jm, so, mv in rows_out:
            w.writerow([f"{t:.3f}", hms(t), int(n_rows), f"{jit:.6f}", f"{wan:.6f}", f"{wa:.6f}",
                        f"{jm:.6f}", so, mv])

    # events: runs of move, merged under EVENT_MERGE_S apart
    ev = []
    for t, _r, jit, _w, _wa, _jm, _so, mv in rows_out:
        if not mv:
            continue
        if ev and t - ev[-1][1] <= EVENT_MERGE_S:
            ev[-1][1] = t
            ev[-1][2] = max(ev[-1][2], jit)
        else:
            ev.append([t, t, jit])
    wev = watch_events(folder)
    sl = a.slack

    def overlaps(s, e, ws, we):
        return s - sl <= we and ws - sl <= e

    with open(os.path.join(folder, "radar_events.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["start", "end", "seconds", "peak_jitter", "watch_events", "watch_kinds", "watch_labels"])
        for s, e, pk in ev:
            m = [x for x in wev if overlaps(s, e, x["t0"], x["t1"])]
            w.writerow([hms(s), hms(e), f"{e - s + 0.2:.1f}", f"{pk:.4f}",
                        " ".join(x["event"] for x in m), " | ".join(x["kind"] for x in m),
                        " | ".join(x.get("label", "") for x in m)])

    # ---- words ----
    span = R[-1, 0] - R[0, 0]
    L = [f"run       {folder}",
         f"span      {hms(R[0, 0])} - {hms(R[-1, 0])} ({span / 3600:.1f} h), {len(R):,} windows "
         f"({len(R) / max(span, 1):.1f}/s), {int(np.median(R[:, 1]))} packets per window (median)",
         f"engine    {'Espressif compiled library, emulated' if a.emulate else 'numpy copy of Espressif library'}"]
    if trained:
        L.append(f"trained   {hms(trained['stretch'][0])}-{hms(trained['stretch'][1])}: {trained['data_num']} shapes, "
                 f"wander thr {trained['wander_thr']:.6f}, jitter thr {trained['jitter_thr']:.6f}")
        L.append(f"move when jitter > {trained['jitter_thr'] / MOVE_SENSITIVITY:.5f} (or > 5x recent median); "
                 f"someone when wander avg > {trained['wander_thr'] / SOMEONE_SENSITIVITY:.5f}")
    else:
        L.append(f"untrained (Espressif defaults): move when jitter > {MOVE_THRESHOLD_DEFAULT / MOVE_SENSITIVITY:.4f} "
                 f"or > 5x recent median; no presence (needs --train)")
    L.append(f"jitter    median {np.median(R[:, 2]):.6f}, 99th pct {np.percentile(R[:, 2], 99):.6f}, max {R[:, 2].max():.4f}")
    L.append(f"move      {100 * R[:, 7].mean():.1f}% of windows" + (
        f";  someone {100 * R[:, 6].mean():.1f}% of windows" if trained else ""))
    L.append("")
    L.append(f"ESP-RADAR EVENTS (move, merged under {EVENT_MERGE_S:g} s apart): {len(ev)}")
    for s, e, pk in ev[:400]:
        m = [x for x in wev if overlaps(s, e, x["t0"], x["t1"])]
        tag = ("  = watch " + ", ".join(f"{x['start']} {x['kind']}" + (f" [{x['label']}]" if x.get("label") else "")
                                        for x in m)) if m else ""
        L.append(f"  {hms(s)} - {hms(e)}  {e - s + 0.2:6.1f} s  peak jitter {pk:.4f}{tag}")
    if len(ev) > 400:
        L.append(f"  ... {len(ev) - 400} more in radar_events.csv")
    if wev:
        L.append("")
        L.append(f"WATCH EVENTS (detect/events.csv) and whether esp-radar also called move (+-{sl:g} s):")
        groups = {}
        for x in wev:
            hit = any(overlaps(s, e, x["t0"], x["t1"]) for s, e, _ in ev)
            for key in [("kind", x["kind"])] + ([("label", x["label"])] if x.get("label") else []):
                g = groups.setdefault(key, [0, 0])
                g[0] += 1
                g[1] += int(hit)
        for (what, name), (n, h) in sorted(groups.items()):
            L.append(f"  {what:5s} {name:14s} {h:3d} of {n:3d} caught by esp-radar")
        n_alone = sum(1 for s, e, _ in ev if not any(overlaps(s, e, x["t0"], x["t1"]) for x in wev))
        L.append(f"  esp-radar events with no watch event nearby: {n_alone} of {len(ev)}")
        if not any(x.get("label") for x in wev):
            L.append("  (fill in the label column of detect/events.csv and run again to see it by label)")
    text = "\n".join(L) + "\n"
    open(os.path.join(folder, "radar.txt"), "w").write(text)
    print(text)

    # ---- picture ----
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    def D(t):
        return mdates.date2num([dt.datetime.fromtimestamp(x) for x in np.atleast_1d(t)])

    fig, ax = plt.subplots(2, 1, figsize=(13, 6), sharex=True, gridspec_kw=dict(height_ratios=[2, 0.6]))
    # per-second max jitter keeps the picture light
    sec = np.floor(R[:, 0]).astype(np.int64)
    us, inv = np.unique(sec, return_inverse=True)
    jmax = np.zeros(len(us))
    np.maximum.at(jmax, inv, R[:, 2])
    ax[0].semilogy(D(us), np.maximum(jmax, 1e-6), lw=0.5, color="#3b6ea5", label="jitter (max per second)")
    thr = (trained["jitter_thr"] if trained else MOVE_THRESHOLD_DEFAULT) / MOVE_SENSITIVITY
    ax[0].axhline(thr, color="#c0392b", lw=0.8, ls="--", label="move line")
    for s, e, _ in ev:
        ax[0].axvspan(D(s)[0], D(e + 0.2)[0], color="#c0392b", alpha=0.25, lw=0)
    ax[0].set_ylabel("esp-radar jitter")
    ax[0].legend(fontsize=7, loc="upper right")
    colors = {"radio+camera": "#8e44ad", "radio only": "#3b6ea5", "camera only": "#e67e22"}
    for x in wev:
        y = {"radio+camera": 2, "radio only": 1, "camera only": 0}.get(x["kind"], 0)
        ax[1].plot(D([x["t0"], x["t1"] + 1]), [y, y], lw=6, color=colors.get(x["kind"], "0.5"), solid_capstyle="butt")
    ax[1].set_yticks([0, 1, 2], ["camera only", "radio only", "radio+camera"], fontsize=7)
    ax[1].set_ylim(-0.7, 2.7)
    ax[1].set_ylabel("watch events", fontsize=8)
    ax[1].xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax[0].set_title(f"{os.path.basename(os.path.abspath(folder))}: esp-radar replay "
                    f"({'trained ' + a.train if trained else 'untrained'}); red = esp-radar move events", fontsize=9)
    fig.tight_layout()
    out_png = os.path.join(folder, "radar.png")
    fig.savefig(out_png, dpi=110)
    print(f"saved {os.path.join(folder, 'radar.txt')}, radar.csv, radar_events.csv, radar.png")


if __name__ == "__main__":
    main()
