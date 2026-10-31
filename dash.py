#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""dash.py - a live dashboard in the terminal (works over SSH, inside tmux).

It reads the live copy of the CSI lines that listen.py sends (--tap) and
shows, once a second, for each radio link (one board's frames as measured by
another; with tw_node boards, every pair in both directions):
  - packets per second and percent lost
  - RSSI now, its 10 s average, min/max and steadiness; the noise floor and
    how far the signal is above it
  - radio "motion" (the same measure as watch.py: how much the CSI picture
    changes from one second to the next, x its usual)
  - scrolling sparklines of motion, RSSI and packets
and below: the latest events (watch.py's events.csv), the newest lines of
listen.log, and the computer's health (disk, mains, sleep block, clock).

It only listens: closing it (q) never touches the recording.

Usage (run.sh, watch.sh and survey.sh start it for you, in a tmux window):
    python3 dash.py --tap 47001 [--folder RUN_FOLDER] [--until HH:MM] [--survey]
Keys:
    m   mark this spot: type where the board is (e.g. "sender 6 m, chest
        height"); the last 30 s are averaged and saved to placement.csv
    p   placement view on/off: big RSSI and link-quality readouts, for
        walking a board around (survey.sh starts in it)
    g   map view on/off (many boards): the boards to scale from boards.csv
        (the run folder's, else ~/tw-data/boards.csv), each line drawn by how
        much it stirs right now (motion and the last 2 s of loss)
    [ ] choose the link shown in detail (with several boards)
    r   reset min/max
    +/- sparkline span (1, 5, 30 minutes)
    q   quit the dashboard (the recording carries on)
"""
import argparse
import collections
import csv
import curses
import datetime as dt
import glob
import locale
import os
import shutil
import socket
import subprocess
import sys
import time
import warnings

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tw_map  # noqa: E402

SPANS = [60, 300, 1800]
HIST_S = 3600
QUIET_S = 600
MARK_S = 30
SPARK_U = "▁▂▃▄▅▆▇█"
SPARK_A = " .:-=+*#"


def parse(dgram):
    """(host_s, mac, seq, rssi, noise, amplitudes) or None."""
    try:
        line = dgram.decode("ascii", errors="replace")
        q = line.find('"')
        q2 = line.rfind("]")
        f = line[:q].split(",")
        if q < 0 or q2 < 0 or len(f) != 26 or line.count("CSI_DATA") != 1:
            return None
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            v = np.fromstring(line[q + 2:q2], sep=",", dtype=float)
        if v.size != int(f[23]) or v.size % 2:
            return None
        return (int(f[0]) / 1e9, f[3], int(f[2]), int(f[4]), int(f[15]), np.hypot(v[0::2], v[1::2]))
    except (ValueError, IndexError):
        return None


ARROW = "\u2192"


def parse_tw(dgram):
    """tw_node lines (host_ns,TWx,...): ("rec", t, link, seq, rssi, noise, amp, key) for a record,
    (kind, t, fields) for the others, None if unreadable."""
    try:
        line = dgram.decode("ascii", errors="replace").strip()
        f = line.split(",")
        if len(f) < 3 or not f[1].startswith("TW"):
            return None
        t = int(f[0]) / 1e9
        kind = f[1]
        if kind == "TWR":
            if len(f) != 19:
                return None
            n, step = int(f[16]), int(f[17])
            if len(f[18]) != 2 * n:
                return None
            amp = np.frombuffer(bytes.fromhex(f[18]), dtype=np.uint8).astype(float) * (step / 4)
            rx, tx = int(f[2]), int(f[3])
            return ("rec", t, f"{tx}{ARROW}{rx}", int(f[4]), int(f[10]), int(f[11]), amp,
                    (rx, int(f[7]), int(f[6])))
        return (kind, t, f[2:])
    except (ValueError, IndexError):
        return None


class Delivery:
    """How many of each board's reports reach the computer (last 10 s). Every board numbers
    its reports one after another, so the numbers missing are reports lost on the way
    (plumbing), not frames the board failed to hear (sensing)."""

    def __init__(self):
        self.recent = collections.defaultdict(collections.deque)    # rx -> (time, boot, number)

    def add(self, rx, boot, num):
        q = self.recent[rx]
        now = time.time()
        if q and q[-1][1] != boot:
            q.clear()                       # the board restarted: its numbers start again
        q.append((now, boot, num))
        while q and q[0][0] < now - 10:
            q.popleft()

    def home_pct(self, rx):
        q = self.recent.get(rx)
        if not q or len(q) < 20:
            return None
        nums = [n for _t, _b, n in q]
        made = max(nums) - min(nums) + 1
        return 100.0 * min(len(nums), made) / made


class Link:
    """Per-second statistics for one link (one sender as heard by one receiver)."""

    def __init__(self, key):
        self.key = key
        self.cur = None             # second being filled
        self.sums = None
        self.n = 0
        self.rssi, self.noise = [], []
        self.lost = 0
        self.last_seq = None
        self.first_pics, self.live, self.prev_pic = [], None, None
        self.hist = collections.deque(maxlen=HIST_S)     # (t, pk, lost, rssi, noise, motion, x, low)
        self.quiet = collections.deque(maxlen=QUIET_S)
        self.pk_hist = collections.deque(maxlen=300)
        self.rmin = self.rmax = None
        self.peaks = collections.deque(maxlen=50)        # (t, x) where x >= 10
        self.last_rx = 0.0

    def add(self, t, seq, rssi, noise, amp):
        s = int(t)
        self.last_rx = time.time()
        if self.cur is not None and s != self.cur:
            self.finish()
        if self.cur is None:
            self.cur, self.sums, self.n, self.rssi, self.noise, self.lost = s, np.zeros_like(amp), 0, [], [], 0
        d = None if self.last_seq is None else seq - self.last_seq
        if d is not None and 0 < d < 1000:
            self.lost += d - 1
            self.last_seq = seq
        elif d is not None and -1000 < d < 0:
            self.lost = max(0, self.lost - 1)       # a relayed record arriving late fills its gap
        else:
            self.last_seq = seq
        if amp.shape == self.sums.shape:
            self.sums += amp
            self.n += 1
        self.rssi.append(rssi)
        self.noise.append(noise)

    def finish(self):
        if self.cur is None or self.n == 0:
            self.cur = None
            return
        pic = self.sums / self.n
        motion = None
        if self.live is None:
            self.first_pics.append(pic)
            if len(self.first_pics) >= 20:
                self.live = np.median(self.first_pics, axis=0) > 1.0
        elif self.prev_pic is not None and self.prev_pic.shape == pic.shape:
            a1, a0 = pic[self.live], self.prev_pic[self.live]
            ok = (a1 > 0) & (a0 > 0)
            if ok.any():
                motion = float(np.median(np.abs(np.log(a1[ok]) - np.log(a0[ok]))))
        self.prev_pic = pic
        self.pk_hist.append(self.n)
        low = len(self.pk_hist) > 30 and self.n < 0.5 * np.median(self.pk_hist)
        x = motion / np.median(self.quiet) if (motion is not None and len(self.quiet) >= 60) else None
        if motion is not None and not low and (x is None or x < 6):
            self.quiet.append(motion)
        r = float(np.mean(self.rssi))
        self.rmin = r if self.rmin is None else min(self.rmin, r)
        self.rmax = r if self.rmax is None else max(self.rmax, r)
        if x is not None and x >= 10 and not low:
            self.peaks.append((self.cur, x))
        self.hist.append((self.cur, self.n, self.lost, r, float(np.mean(self.noise)), motion, x, low))
        self.cur = None

    def window(self, seconds):
        now = time.time()
        return [h for h in self.hist if h[0] >= now - seconds - 1]

    def stats(self, seconds):
        w = self.window(seconds)
        if not w:
            return None
        pk = sum(h[1] for h in w)
        lost = sum(h[2] for h in w)
        rs = np.array([h[3] for h in w])
        xs = [h[6] for h in w if h[6] is not None]
        return dict(n=len(w), pps=pk / len(w), lost=100 * lost / max(pk + lost, 1),
                    rssi=float(rs.mean()), rssi_sd=float(rs.std()), noise=float(np.mean([h[4] for h in w])),
                    xmed=float(np.median(xs)) if xs else None, xmax=max(xs) if xs else None)


# ---------------- computer health ----------------
def health(folder):
    out = []
    try:
        free = shutil.disk_usage(folder or os.path.expanduser("~")).free / 1e9
        out.append(f"disk {free:.0f} GB free")
    except OSError:
        pass
    ac = glob.glob("/sys/class/power_supply/A*/online")
    if ac:
        try:
            out.append("mains on" if open(ac[0]).read().strip() == "1" else "ON BATTERY")
        except OSError:
            pass
    for b in glob.glob("/sys/class/power_supply/BAT*/capacity")[:1]:
        try:
            out.append(f"battery {open(b).read().strip()}%")
        except OSError:
            pass
    try:
        st = subprocess.run(["systemctl", "is-enabled", "sleep.target"], capture_output=True, text=True,
                            timeout=2).stdout.strip()
        out.append("sleep blocked" if st == "masked" else "sleep ALLOWED")
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        tr = subprocess.run(["chronyc", "tracking"], capture_output=True, text=True, timeout=2).stdout
        out.append("clock synced" if "Leap status     : Normal" in tr else "clock NOT synced")
    except (OSError, subprocess.SubprocessError):
        pass
    temps = []
    for z in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
        try:
            temps.append(int(open(z).read()) / 1000)
        except (OSError, ValueError):
            pass
    if temps:
        out.append(f"{max(temps):.0f} C")
    try:
        out.append(f"load {os.getloadavg()[0]:.2f}")
    except OSError:
        pass
    return out


def tail(path, n):
    try:
        with open(path, "rb") as fh:
            fh.seek(0, 2)
            size = fh.tell()
            fh.seek(max(0, size - 4096))
            return fh.read().decode(errors="replace").splitlines()[-n:]
    except OSError:
        return []


def events(folder, n):
    path = os.path.join(folder, "detect", "events.csv") if folder else ""
    try:
        rows = list(csv.DictReader(open(path)))
    except OSError:
        return None
    return rows[-n:]


# ---------------- drawing ----------------
def spark(vals, width, lo, hi, chars):
    vals = list(vals)[-width:]
    out = []
    for v in vals:
        if v is None or not np.isfinite(v):
            out.append(" ")
            continue
        f = (v - lo) / (hi - lo) if hi > lo else 0
        out.append(chars[int(min(max(f, 0), 0.999) * len(chars))])
    return "".join(out).rjust(width)


def resample(hist, span, width):
    """Bucket the last `span` seconds into `width` columns (max per column)."""
    now = int(time.time())
    if not hist:
        return [None] * width, [None] * width, [None] * width
    step = span / width
    cols_x, cols_r, cols_p = [None] * width, [None] * width, [None] * width
    for t, pk, _l, r, _n, _m, x, low in hist:
        age = now - t
        if age < 0 or age >= span:
            continue
        c = width - 1 - int(age / step)
        xv = np.log10(max(x, 0.5)) if x is not None else None
        if xv is not None and (cols_x[c] is None or xv > cols_x[c]):
            cols_x[c] = xv
        cols_r[c] = r if cols_r[c] is None else max(cols_r[c], r)
        cols_p[c] = pk if cols_p[c] is None else min(cols_p[c], pk)
    return cols_x, cols_r, cols_p


class Screen:
    def __init__(self, scr, a):
        self.scr, self.a = scr, a
        self.utf = "utf" in (locale.getpreferredencoding() or "").lower()
        self.chars = SPARK_U if self.utf else SPARK_A
        self.span_i = 0
        self.placement = a.survey
        self.msg = ""
        self.msg_t = 0
        if curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            for i, c in enumerate((curses.COLOR_GREEN, curses.COLOR_YELLOW, curses.COLOR_RED, curses.COLOR_CYAN), 1):
                curses.init_pair(i, c, -1)

    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and x < w:
            try:
                self.scr.addnstr(y, x, text, max(0, w - x - 1), attr)
            except curses.error:
                pass

    def color(self, i):
        return curses.color_pair(i) if curses.has_colors() else 0

    def draw(self, links, started, health_items, boards=None):
        if boards is not None and any(ARROW in k for k in links):
            return self.draw_multi(links, started, health_items, boards)
        a = self.a
        scr = self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        now = dt.datetime.now()
        title = (f" two-witnesses  {'survey' if a.survey else 'live'}  "
                 f"{os.path.basename(os.path.abspath(a.folder)) if a.folder else ''}")
        right = f"{now:%H:%M:%S}  up {str(dt.timedelta(seconds=int(time.time() - started)))}" + (
            f"  stops {a.until}" if a.until else "")
        self.put(0, 0, title.ljust(w), curses.A_REVERSE)
        self.put(0, max(0, w - len(right) - 2), right, curses.A_REVERSE)
        y = 2
        if not links:
            self.put(y, 1, f"waiting for data on UDP 127.0.0.1:{a.tap} ...", self.color(2))
            self.put(y + 1, 1, f"(is listen.py running with --tap {a.tap}?)")
            y += 3
        span = SPANS[self.span_i]
        for link in links.values():
            s10, s600 = link.stats(10), link.stats(600)
            stale = time.time() - link.last_rx > 5
            name = f"link from {link.key}"
            self.put(y, 1, name, curses.A_BOLD)
            if stale:
                self.put(y, len(name) + 3, f"NO DATA for {time.time() - link.last_rx:.0f} s", self.color(3) | curses.A_BOLD)
            y += 1
            if s10 is None:
                self.put(y, 3, "collecting ...")
                y += 2
                continue
            lost_c = self.color(1 if s10["lost"] < 10 else 2 if s10["lost"] < 30 else 3)
            self.put(y, 3, f"packets {s10['pps']:5.1f}/s   lost")
            self.put(y, 28, f"{s10['lost']:5.1f}% (10 s)", lost_c)
            if s600:
                self.put(y, 44, f"{s600['lost']:5.1f}% (10 min)")
            y += 1
            snr = s10["rssi"] - s10["noise"]
            self.put(y, 3, f"RSSI   {link.hist[-1][3]:6.1f} dBm now  {s10['rssi']:6.1f} avg 10 s  "
                           f"min {link.rmin:.1f} max {link.rmax:.1f}  sd {s10['rssi_sd']:.2f}")
            y += 1
            self.put(y, 3, f"noise  {s10['noise']:6.1f} dBm      signal above noise {snr:5.1f} dB")
            y += 1
            lastx = link.hist[-1][6]
            if lastx is None:
                mtxt = "motion  learning the usual (first ~80 s)"
            else:
                mtxt = f"motion  x{lastx:5.1f} now" + (f"   max x{s600['xmax']:.1f} (10 min)" if s600 and s600['xmax'] else "")
            low = link.hist[-1][7]
            self.put(y, 3, mtxt, self.color(3 if (lastx or 0) >= 10 else 2 if (lastx or 0) >= 6 else 0))
            if low:
                self.put(y, 60, "LOW DATA", self.color(2) | curses.A_BOLD)
            y += 1
            sw = max(10, min(w - 16, span))          # never more columns than seconds
            cx, cr, cp = resample(link.hist, span, sw)
            lbl = {60: "1 min", 300: "5 min", 1800: "30 min"}[span]
            self.put(y, 3, "motion  " + spark(cx, sw, 0, 2, self.chars))
            self.put(y + 1, 3, "RSSI    " + spark(cr, sw, (link.rmin or -90) - 0.5, (link.rmax or -30) + 0.5, self.chars))
            pmax = max([p for p in cp if p] or [100])
            self.put(y + 2, 3, "packets " + spark(cp, sw, 0, pmax, self.chars))
            self.put(y + 3, 11, f"<- {lbl} ago".ljust(sw - 4) + "now", curses.A_DIM)
            y += 5
            if self.placement:
                y = self.draw_placement(link, s10, y, w)
            if link.peaks:
                pk = list(link.peaks)[-3:]
                self.put(y, 3, "radio moments x>=10: " + "   ".join(
                    f"{dt.datetime.fromtimestamp(t):%H:%M:%S} x{x:.0f}" for t, x in pk))
                y += 1
            y += 1
        ev = events(a.folder, 4)
        if ev is not None:
            self.put(y, 1, "EVENTS (watch.py)", curses.A_BOLD)
            y += 1
            if not ev:
                self.put(y, 3, "none yet")
                y += 1
            for e in ev:
                self.put(y, 3, f"{e.get('start', '')}-{e.get('end', '')}  {e.get('kind', ''):13s} "
                               f"radio x{e.get('radio_peak', '')}  camera x{e.get('camera_peak', '')}  "
                               f"{e.get('frames', '')} frames {e.get('hint', '')}")
                y += 1
            y += 1
        marks = self.read_marks(3)
        if marks:
            self.put(y, 1, "MARKS (placement.csv)", curses.A_BOLD)
            y += 1
            for m in marks:
                self.put(y, 3, f"{m['clock']}  {m['rssi']:>6s} dBm  above noise {m['snr']:>5s} dB  "
                               f"{m['pps']:>5s}/s  lost {m['lost']:>5s}%  {m['label']}")
                y += 1
            y += 1
        if a.folder and y < h - 6:
            self.put(y, 1, "LOG", curses.A_BOLD)
            y += 1
            for line in tail(os.path.join(a.folder, "listen.log"), max(1, min(4, h - y - 4))):
                self.put(y, 3, line[11:] if len(line) > 11 else line, curses.A_DIM)
                y += 1
        self.put(h - 3, 1, "  ".join(health_items))
        if self.msg and time.time() - self.msg_t < 8:
            self.put(h - 2, 1, self.msg, self.color(4))
        self.put(h - 1, 0, " m mark  p placement  r reset  +/- span  q quit (recording carries on) ".ljust(w),
                 curses.A_REVERSE)
        scr.refresh()

    def draw_multi(self, links, started, health_items, boards):
        a, scr = self.a, self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        now = time.time()
        title = (f" two-witnesses  {'survey' if a.survey else 'live'}  "
                 f"{os.path.basename(os.path.abspath(a.folder)) if a.folder else ''}")
        right = f"{dt.datetime.now():%H:%M:%S}  up {str(dt.timedelta(seconds=int(now - started)))}" + (
            f"  stops {a.until}" if a.until else "")
        self.put(0, 0, title.ljust(w), curses.A_REVERSE)
        self.put(0, max(0, w - len(right) - 2), right, curses.A_REVERSE)
        y = 2
        st = boards.get("status")
        if st:
            self.put(y, 1, f"exit board {st['id']}   timekeeper board {st['leader']}   period {st['period']} ms   "
                           f"timekeeper changes {st['leader_changes']}", curses.A_BOLD)
            y += 1
        x = 1
        for bid in sorted(k for k in boards if isinstance(k, int)):
            b = boards[bid]
            age = now - b["t"]
            tag = (" exit" if b.get("exit") else "") + (" time" if b.get("leader") else "")
            home = boards["delivery"].home_pct(bid) if "delivery" in boards else None
            txt = (f"board {bid}{tag}: " + (f"{age:.0f} s ago" if age > 2 else "ok")
                   + (f" {b['rssi']} dBm" if b.get("rssi") is not None else "")
                   + (f", reports home {home:.0f}%" if home is not None else "")
                   + (f", heard {b['heard']}%" if b.get("heard") is not None and b["heard"] < 90 else "")
                   + (f", tx {b['power_q'] / 4:g} dBm" if b.get("power_q") else "")
                   + (", relay OFF" if b.get("relay") == 0 else ""))
            if x + len(txt) > w - 2:
                y += 1
                x = 1
            self.put(y, x, txt, self.color(3) if age > 5 else 0)
            x += len(txt) + 3
        y += 2
        keys = sorted(links, key=lambda k: [int(v) for v in k.split(ARROW)])
        self.sel = min(getattr(self, "sel", 0), max(len(keys) - 1, 0))
        span = SPANS[self.span_i]
        lbl = {60: "1 min", 300: "5 min", 1800: "30 min"}[span]
        sw = max(8, min(w - 58, span))
        self.put(y, 1, f"{'link':7s} {'pkts/s':>7s} {'lost':>6s} {'2 s':>6s} {'RSSI':>7s} {'>noise':>7s} "
                       f"{'motion':>7s}  motion, last {lbl}", curses.A_BOLD)
        y += 1
        for i, k in enumerate(keys):
            link = links[k]
            s10 = link.stats(10)
            mark = ">" if i == self.sel else " "
            if s10 is None or not link.hist:
                self.put(y, 0, f"{mark}{k:7s}  collecting ...")
                y += 1
                continue
            lastx = link.hist[-1][6]
            xtxt = "learn" if lastx is None else f"x{lastx:.1f}"
            lost_c = self.color(1 if s10["lost"] < 10 else 2 if s10["lost"] < 30 else 3)
            self.put(y, 0, f"{mark}{k:7s} {s10['pps']:7.1f}", curses.A_BOLD if i == self.sel else 0)
            self.put(y, 16, f"{s10['lost']:6.1f}%", lost_c)
            s2 = link.stats(2) or s10
            self.put(y, 23, f"{s2['lost']:6.1f}%", self.color(1 if s2["lost"] < 10 else 2 if s2["lost"] < 30 else 3))
            self.put(y, 31, f"{s10['rssi']:7.1f} {s10['rssi'] - s10['noise']:7.1f}")
            self.put(y, 47, f"{xtxt:>7s}", self.color(3 if (lastx or 0) >= 10 else 2 if (lastx or 0) >= 6 else 0))
            cx, _cr, _cp = resample(link.hist, span, sw)
            if now - link.last_rx > 5:
                self.put(y, 56, f"NO DATA for {now - link.last_rx:.0f} s", self.color(3))
            else:
                self.put(y, 56, spark(cx, sw, 0, 2, self.chars))
            y += 1
        y += 1
        if getattr(self, "map_on", False):
            y = self.draw_map(links, y, h, w)
        elif keys:
            k = keys[self.sel]
            link = links[k]
            s10 = link.stats(10)
            if s10 and link.hist:
                tx, rx = k.split(ARROW)
                self.put(y, 1, f"link {k}: board {rx} hearing board {tx}   ([ ] another link)", curses.A_BOLD)
                y += 1
                self.put(y, 3, f"RSSI {link.hist[-1][3]:6.1f} dBm now  {s10['rssi']:6.1f} avg 10 s  "
                               f"min {link.rmin:.1f} max {link.rmax:.1f}  sd {s10['rssi_sd']:.2f}")
                y += 1
                sw2 = max(10, min(w - 16, span))
                cx, cr, cp = resample(link.hist, span, sw2)
                self.put(y, 3, "RSSI    " + spark(cr, sw2, (link.rmin or -90) - 0.5, (link.rmax or -30) + 0.5,
                                                  self.chars))
                pmax = max([p for p in cp if p] or [100])
                self.put(y + 1, 3, "packets " + spark(cp, sw2, 0, pmax, self.chars))
                y += 3
                if self.placement:
                    y = self.draw_placement(link, s10, y, w)
        ev = events(a.folder, 3)
        if ev and y < h - 6:
            self.put(y, 1, "EVENTS (watch.py)", curses.A_BOLD)
            y += 1
            for e in ev:
                self.put(y, 3, f"{e.get('start', '')}-{e.get('end', '')}  {e.get('kind', ''):13s} "
                               f"radio x{e.get('radio_peak', '')}  camera x{e.get('camera_peak', '')}")
                y += 1
            y += 1
        marks = self.read_marks(2)
        if marks and y < h - 5:
            self.put(y, 1, "MARKS (placement.csv)", curses.A_BOLD)
            y += 1
            for m in marks:
                self.put(y, 3, f"{m['clock']} {m['link']:6s} {m['rssi']:>6s} dBm  {m['lost']:>5s}% lost  {m['label']}")
                y += 1
        line = "  ".join(health_items)
        hb = boards.get("health")
        if hb:
            line += "   board " + " ".join(x for x in hb if x.startswith(("usb_drops", "queue_full", "frames")))
        self.put(h - 3, 1, line)
        if self.msg and time.time() - self.msg_t < 8:
            self.put(h - 2, 1, self.msg, self.color(4))
        self.put(h - 1, 0, " m mark  p placement  g map  [ ] link  r reset  +/- span  q quit (recording carries on) ".ljust(w),
                 curses.A_REVERSE)
        scr.refresh()

    def map_levels(self, links):
        """per board pair, 0..3: the stronger of its two directions, from the motion now
        (x its usual) and the loss over the last 2 s"""
        levels = {}
        for k, link in links.items():
            try:
                a, b = (int(v) for v in k.split(ARROW))
            except ValueError:
                continue
            s2 = link.stats(2)
            if not link.hist or s2 is None:
                continue
            x = link.hist[-1][6] or 0
            lv = 3 if x >= 10 else 2 if x >= 6 else 1 if x >= 3 else 0
            lost = s2["lost"]
            lv = max(lv, 3 if lost >= 60 else 2 if lost >= 30 else 0)
            if time.time() - link.last_rx > 5:
                lv = 4                              # no data: drawn "?"
            p = tw_map.pair(a, b)
            levels[p] = max(levels.get(p, 0), lv)
        return levels

    def draw_map(self, links, y, h, w):
        now = time.time()
        if now - getattr(self, "boards_t", 0) > 30:
            self.boards_xy = tw_map.load_boards(tw_map.find_boards(self.a.folder))
            self.boards_t = now
        bxy, marks = self.boards_xy
        if not bxy:
            self.put(y, 1, "MAP: no boards.csv (in the run folder or ~/tw-data); see tw_map.py for the format",
                     self.color(2))
            return y + 2
        mh = max(8, min(24, h - y - 6))
        mw = max(30, min(w - 4, 2 * mh * 3))
        rows = tw_map.render(bxy, self.map_levels(links), marks, mw, mh)
        colors = {"#": 3, "=": 2, "-": 4, "?": 3}
        self.put(y, 1, "MAP (g: back to the link detail)", curses.A_BOLD)
        y += 1
        for row in rows:
            for c, ch in enumerate(row):
                if ch != " ":
                    self.put(y, 3 + c, ch, self.color(colors[ch]) if ch in colors else
                             (curses.A_BOLD if ch.isalnum() else 0))
            y += 1
        self.put(y, 3, tw_map.legend(bxy, marks)[:w - 5])
        return y + 2

    def draw_placement(self, link, s10, y, w):
        r = s10["rssi"]
        bar_w = max(10, min(50, w - 40))
        f = min(max((r + 90) / 60, 0), 1)       # -90 .. -30 dBm
        bar = ("█" if self.utf else "#") * int(f * bar_w)
        quality = ("good" if r > -70 and s10["lost"] < 10 else "fair" if r > -80 and s10["lost"] < 30 else "poor")
        qc = self.color({"good": 1, "fair": 2, "poor": 3}[quality])
        self.put(y, 3, "PLACEMENT", curses.A_BOLD)
        self.put(y + 1, 5, f"RSSI {r:6.1f} dBm  ", curses.A_BOLD)
        self.put(y + 1, 25, bar.ljust(bar_w) + "|", qc)
        self.put(y + 1, 27 + bar_w, quality.upper(), qc | curses.A_BOLD)
        self.put(y + 2, 5, f"-90 dBm{'':{max(0, bar_w - 7)}}-30 dBm", curses.A_DIM)
        s30 = link.stats(MARK_S)
        if s30:
            self.put(y + 3, 5, f"last {MARK_S} s: RSSI {s30['rssi']:.1f} +-{s30['rssi_sd']:.2f} dB, "
                               f"above noise {s30['rssi'] - s30['noise']:.1f} dB, {s30['pps']:.1f}/s, lost {s30['lost']:.1f}%")
        self.put(y + 4, 5, "solid link: above -70 dBm, under 10% lost.  Hold still 30 s, then m",
                 curses.A_DIM)
        return y + 6

    # ---- marks ----
    def marks_path(self):
        return os.path.join(self.a.folder or os.path.expanduser("~/tw-data"), "placement.csv")

    def read_marks(self, n):
        try:
            return list(csv.DictReader(open(self.marks_path())))[-n:]
        except OSError:
            return []

    def mark(self, links):
        h, w = self.scr.getmaxyx()
        curses.echo()
        curses.curs_set(1)
        self.scr.nodelay(False)
        self.put(h - 2, 1, " " * (w - 2))
        self.put(h - 2, 1, "where is the board? (Enter to save, empty to cancel): ", self.color(4))
        self.scr.refresh()
        try:
            label = self.scr.getstr(h - 2, 56, 120).decode(errors="replace").strip()
        except curses.error:
            label = ""
        curses.noecho()
        curses.curs_set(0)
        self.scr.nodelay(True)
        if not label:
            self.msg, self.msg_t = "mark cancelled", time.time()
            return
        path = self.marks_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        new = not os.path.exists(path)
        with open(path, "a", newline="") as fh:
            wr = csv.writer(fh)
            if new:
                wr.writerow(["time", "clock", "link", "label", "seconds", "rssi", "rssi_sd", "noise", "snr",
                             "pps", "lost", "motion_x_median"])
            saved = 0
            for link in links.values():
                s = link.stats(MARK_S)
                if not s:
                    continue
                wr.writerow([f"{time.time():.0f}", f"{dt.datetime.now():%H:%M:%S}", link.key.replace(ARROW, "-"),
                             label, s["n"],
                             f"{s['rssi']:.2f}", f"{s['rssi_sd']:.2f}", f"{s['noise']:.1f}",
                             f"{s['rssi'] - s['noise']:.1f}", f"{s['pps']:.1f}", f"{s['lost']:.1f}",
                             f"{s['xmed']:.2f}" if s["xmed"] is not None else ""])
                saved += 1
        self.msg = f"saved '{label}' ({saved} link{'s' if saved != 1 else ''}, last {MARK_S} s) to placement.csv"
        self.msg_t = time.time()


def run(scr, a):
    curses.curs_set(0)
    scr.nodelay(True)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    try:
        sock.bind(("127.0.0.1", a.tap))
    except OSError as e:
        raise SystemExit(f"cannot listen on UDP {a.tap}: {e} (is another dashboard open?)")
    sock.setblocking(False)
    ui = Screen(scr, a)
    links = {}
    boards = {}                         # board number -> what we last heard of it; "status", "health"
    deliv = boards["delivery"] = Delivery()
    seen = set()
    seen_order = collections.deque()
    started = time.time()
    next_draw = 0.0
    hl, hl_t = [], 0.0
    while True:
        # drain the socket
        for _ in range(2000):
            try:
                d = sock.recv(65536)
            except BlockingIOError:
                break
            if d[:30].find(b",TW") >= 0:
                p = parse_tw(d)
                if p is None:
                    continue
                if p[0] == "rec":
                    _k, t, key, seq, rssi, noise, amp, dk = p
                    if dk in seen:
                        continue            # the second copy of a record already counted
                    seen.add(dk)
                    deliv.add(*dk)
                    seen_order.append(dk)
                    if len(seen_order) > 200000:
                        seen.discard(seen_order.popleft())
                    link = links.get(key) or links.setdefault(key, Link(key))
                    link.add(t, seq, rssi, noise, amp)
                    continue
                kind, t, f = p
                try:
                    if kind == "TWN":           # src,seq,leader,flags,active,period,uptime,rssi
                        b = boards.setdefault(int(f[0]), {})
                        b.update(t=time.time(), rssi=int(f[7]), exit=bool(int(f[3]) & 2),
                                 leader=int(f[2]) == int(f[0]))
                        if len(f) >= 10:            # tw_node 1.1+: ,set_seq,set_power (quarter dBm)
                            b["power_q"] = int(f[9])
                        if len(f) >= 12:            # tw_node 2.0+: ,relay on/off,% the exit hears it
                            b["relay"], b["heard"] = int(f[10]), int(f[11])
                    elif kind == "TWS":         # id,ver,leader,flags,active,period,slot,offset,...
                        boards["status"] = dict(id=int(f[0]), leader=int(f[2]), period=int(f[5]),
                                                leader_changes=int(f[14]))
                        b = boards.setdefault(int(f[0]), {})
                        b.update(t=time.time(), rssi=None, exit=True, leader=int(f[2]) == int(f[0]))
                        if len(f) >= 19:            # tw_node 1.1+: ...,set_seq,set_power,radio's power
                            b["power_q"] = int(f[18]) or int(f[17])
                        if len(f) >= 20:            # tw_node 2.0+: ...,relay on/off,relayed,no room
                            b["relay"] = int(f[19])
                    elif kind == "TWF":
                        boards["health"] = f[1:]
                except (ValueError, IndexError):
                    pass
                continue
            p = parse(d)
            if p is None:
                continue
            t, mac, seq, rssi, noise, amp = p
            link = links.get(mac) or links.setdefault(mac, Link(mac))
            link.add(t, seq, rssi, noise, amp)
        now = time.time()
        for link in links.values():           # close the second even when data stops
            if link.cur is not None and now - link.cur > 2:
                link.finish()
        k = scr.getch()
        if k in (ord("q"), ord("Q")):
            return
        if k == ord("m"):
            ui.mark(links)
        elif k == ord("p"):
            ui.placement = not ui.placement
        elif k == ord("g"):
            ui.map_on = not getattr(ui, "map_on", False)
        elif k == ord("r"):
            for link in links.values():
                link.rmin = link.rmax = None
            ui.msg, ui.msg_t = "min/max reset", now
        elif k in (ord("]"), 9):
            ui.sel = (getattr(ui, "sel", 0) + 1) % max(len(links), 1)
        elif k == ord("["):
            ui.sel = max(getattr(ui, "sel", 0) - 1, 0)
        elif k in (ord("+"), ord("=")):
            ui.span_i = min(ui.span_i + 1, len(SPANS) - 1)
        elif k == ord("-"):
            ui.span_i = max(ui.span_i - 1, 0)
        if now - hl_t > 10:
            hl, hl_t = health(a.folder), now
        if now >= next_draw or k != -1:
            ui.draw(links, started, hl, boards)
            next_draw = now + 1.0
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tap", type=int, default=47001)
    ap.add_argument("--folder", help="the run folder (for listen.log, events.csv, placement.csv)")
    ap.add_argument("--until", help="shown in the title only")
    ap.add_argument("--survey", action="store_true", help="start in the placement view")
    a = ap.parse_args()
    locale.setlocale(locale.LC_ALL, "")
    os.environ.setdefault("ESCDELAY", "25")
    try:
        curses.wrapper(run, a)
    except KeyboardInterrupt:
        pass
    print("dashboard closed; the recording (if any) carries on.")


if __name__ == "__main__":
    main()
