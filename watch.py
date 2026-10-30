#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""watch.py - detection mode. Listens to the live copy of the CSI lines that
listen.py sends (--tap), decides once a second whether the radio
picture is moving, keeps the camera running into a short rolling buffer, and
when the radio or the camera fires, keeps the frames from a few seconds
before to a few seconds after. Use it through watch.sh.

    python3 watch.py OUTDIR --tap 47000 --until 07:00 [--k 10] [--nocam]

Two witnesses, judged separately:
  radio   "motion" = how much the CSI picture changes from one second to the
          next (median over subcarriers of |log change|), divided by the
          scene's usual (rolling median of the last 10 quiet minutes).
          With several boards (tw_node) every link is judged on its own and
          the radio fires when any link does; seconds.csv and events.csv
          name the link that moved most ("2-1": board 1 hearing board 2).
          Fires at x10, or x6 two seconds running (--k, --k2).
          Seconds with under half the usual packets are marked LOW DATA and
          never fire: heavy loss makes the measure lie (learned 2026-10-26).
          Nothing can fire in the first ~80 s (20 s to find the live
          subcarriers, 60 quiet seconds to learn "usual"); watch.log says
          "radio armed" when it can.
  camera  how much the picture changes from one frame (1 a second) to the
          next, divided by its usual. Fires at x4 (--ck).

Every event is one of:  radio+camera | radio only | camera only
Writes into OUTDIR:
  events.csv    one line per event, with empty "label" and "note" columns
                for you to fill in (person, car, door, animal, light,
                nothing, dark, other)
  events/       the kept frames, one folder per event
  seconds.csv   every second: packets, radio motion, camera motion, flags
                (so thresholds can be changed afterwards and re-scored)
  sheet-NN.png  contact sheets: one row per event
  watch.log
"""
import argparse
import collections
import csv
import datetime as dt
import glob
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

import numpy as np

STOP = [False]
LOG = None


def log(msg):
    line = f"{dt.datetime.now():%Y-%m-%d %H:%M:%S}  {msg}"
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")
        LOG.flush()


def stop_time(until, hours):
    now = dt.datetime.now()
    ends = []
    if until:
        h, m = (int(x) for x in until.split(":"))
        e = now.replace(hour=h, minute=m, second=0, microsecond=0)
        if e <= now:
            e += dt.timedelta(days=1)
        ends.append(e)
    if hours:
        ends.append(now + dt.timedelta(hours=hours))
    if not ends:
        sys.exit("give --until HH:MM or --hours N")
    return min(ends).timestamp()


SEEN = set()
SEEN_ORDER = collections.deque()


def parse_twr(line):
    """a tw_node record: (host seconds, link, rssi, amplitudes), None if unreadable or a duplicate."""
    f = line.strip().split(",")
    if len(f) != 19:
        return None
    n, step = int(f[16]), int(f[17])
    if len(f[18]) != 2 * n:
        return None
    key = (int(f[2]), int(f[7]), int(f[6]))      # measuring board, boot, record number
    if key in SEEN:
        return None                             # the second copy of a record
    SEEN.add(key)
    SEEN_ORDER.append(key)
    if len(SEEN_ORDER) > 200000:
        SEEN.discard(SEEN_ORDER.popleft())
    amp = np.frombuffer(bytes.fromhex(f[18]), dtype=np.uint8).astype(float) * (step / 4)
    return int(f[0]) / 1e9, f"{f[3]}-{f[2]}", int(f[10]), amp


def parse(datagram):
    """(host seconds, link, rssi, amplitudes) or None."""
    try:
        line = datagram.decode("ascii", errors="replace")
        if line[:30].find(",TWR,") >= 0:
            return parse_twr(line)
        q = line.find('"')
        f = line[:q].split(",")
        if len(f) != 26 or line.count("CSI_DATA") != 1:
            return None
        v = np.fromstring(line[q + 2:line.rfind("]")], sep=",", dtype=float)
        if v.size != int(f[23]) or v.size % 2:
            return None
        return int(f[0]) / 1e9, "", int(f[4]), np.hypot(v[0::2], v[1::2])
    except (ValueError, IndexError):
        return None


class Camera:
    """ffmpeg writes one JPEG a second into ring/, named by its clock time."""

    def __init__(self, folder, cam_input):
        self.ring = os.path.join(folder, "ring")
        os.makedirs(self.ring, exist_ok=True)
        import shlex
        cmd = (["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"] + shlex.split(cam_input) +
               ["-vf", "fps=1,drawtext=text='%{localtime}':x=8:y=8:fontsize=20:fontcolor=white:box=1:boxcolor=black",
                "-q:v", "6", "-f", "image2", "-strftime", "1",
                os.path.join(self.ring, "%Y%m%d-%H%M%S.jpg")])
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(os.path.join(folder, "camera.log"), "a"))
        self.prev = None
        self.prev_name = None

    def alive(self):
        return self.p.poll() is None

    def frames(self):
        return sorted(glob.glob(os.path.join(self.ring, "*.jpg")))

    @staticmethod
    def t_of(path):
        return dt.datetime.strptime(os.path.basename(path)[:15], "%Y%m%d-%H%M%S").timestamp()

    def new_frames(self):
        """[(frame time, change vs previous frame, brightness)] for every complete
        frame not yet seen (the newest file may still be being written)."""
        from PIL import Image
        out = []
        for name in self.frames()[:-1]:
            if self.prev_name is not None and name <= self.prev_name:
                continue
            try:
                im = np.asarray(Image.open(name).convert("L").resize((80, 60)), dtype=float)
            except Exception:
                continue
            if self.prev is not None:
                out.append((self.t_of(name), float(np.abs(im - self.prev).mean()), float(im.mean())))
            self.prev, self.prev_name = im, name
        return out

    def keep(self, t0, t1, dest, max_frames=40):
        os.makedirs(dest, exist_ok=True)
        sel = [f for f in self.frames() if t0 <= self.t_of(f) <= t1]
        if len(sel) > max_frames:            # long event: keep an even spread
            idx = np.linspace(0, len(sel) - 1, max_frames).astype(int)
            sel = [sel[i] for i in idx]
        for f in sel:
            shutil.copy2(f, dest)
        return len(sel)

    def tidy(self, keep_after):
        for f in self.frames()[:-3]:
            if self.t_of(f) < keep_after:
                try:
                    os.remove(f)
                except OSError:
                    pass

    def close(self):
        if self.alive():
            self.p.terminate()
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--tap", type=int, default=47000)
    ap.add_argument("--until")
    ap.add_argument("--hours", type=float)
    ap.add_argument("--k", type=float, default=10.0, help="radio fires at this x usual")
    ap.add_argument("--k2", type=float, default=6.0, help="...or at this, two seconds running")
    ap.add_argument("--ck", type=float, default=4.0, help="camera fires at this x usual")
    ap.add_argument("--pre", type=int, default=5, help="seconds of frames kept before an event")
    ap.add_argument("--post", type=int, default=10, help="...and after")
    ap.add_argument("--nocam", action="store_true")
    ap.add_argument("--cam-input", default=os.environ.get(
        "CAM_INPUT", "-f v4l2 -input_format mjpeg -framerate 15 -video_size 640x480 -i /dev/video0"))
    ap.add_argument("--cam-input-file", help="read --cam-input from this file")
    a = ap.parse_args()
    if a.cam_input_file:
        a.cam_input = open(a.cam_input_file).read().strip()

    end = stop_time(a.until, a.hours)
    os.makedirs(a.outdir, exist_ok=True)
    LOG = open(os.path.join(a.outdir, "watch.log"), "a")
    signal.signal(signal.SIGTERM, lambda *_: STOP.__setitem__(0, True))
    signal.signal(signal.SIGINT, lambda *_: STOP.__setitem__(0, True))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
    sock.bind(("127.0.0.1", a.tap))
    sock.settimeout(0.2)
    cam = None if a.nocam else Camera(a.outdir, a.cam_input)
    log(f"watching: tap {a.tap}, camera {'off' if a.nocam else 'on'}, radio fires at x{a.k:g} "
        f"(or x{a.k2:g} twice), camera at x{a.ck:g}; stop {dt.datetime.fromtimestamp(end):%H:%M}")

    sec_f = open(os.path.join(a.outdir, "seconds.csv"), "a", newline="")
    sec_w = csv.writer(sec_f)
    if sec_f.tell() == 0:
        sec_w.writerow(["time", "clock", "packets", "rssi", "radio_motion", "radio_x", "low_data",
                        "cam_motion", "cam_x", "brightness", "link"])
    ev_path = os.path.join(a.outdir, "events.csv")
    events = []
    if os.path.exists(ev_path):
        events = list(csv.DictReader(open(ev_path)))

    LAG = 4                       # decide each second this long after it, once both witnesses reported
    cquiet = collections.deque(maxlen=600)
    radio = {}                                  # second -> {link: (packets, rssi, motion, x, low)}
    camres = {}                                 # second -> (change, brightness)
    state = dict(last_x=0.0, ev=None, next_t=None)
    started = time.time()
    last_cam_check = 0

    class RadioLink:
        """one link's per-second picture, its usual, and its motion"""

        def __init__(self, name):
            self.name = name
            self.cur, self.sums, self.cnt, self.rs = None, None, 0, []
            self.prev_pic = self.live = None
            self.first_pics = []
            self.quiet = collections.deque(maxlen=600)      # last 10 quiet minutes of radio motion
            self.pk_hist = collections.deque(maxlen=600)

        def add(self, s_, rssi, amp):
            if self.cur is not None and s_ != self.cur:
                self.finish()
            if self.cur is None:
                self.cur, self.sums, self.cnt, self.rs = s_, np.zeros_like(amp), 0, []
            if amp.shape == self.sums.shape:
                self.sums += amp
                self.cnt += 1
                self.rs.append(rssi)

        def finish(self):
            t, cnt, rs = self.cur, self.cnt, self.rs
            self.cur = None
            if t is None or not cnt:
                return
            pic = self.sums / cnt
            tag = f" (link {self.name})" if self.name else ""
            if self.live is None:
                self.first_pics.append(pic)
                if len(self.first_pics) >= 20:
                    self.live = np.median(self.first_pics, axis=0) > 1.0
                    log(f"{int(self.live.sum())} live subcarriers of {self.live.size}{tag}")
                self.prev_pic = pic
                return
            motion = None
            if self.prev_pic is not None and self.prev_pic.shape == pic.shape:
                a1, a0 = pic[self.live], self.prev_pic[self.live]
                ok = (a1 > 0) & (a0 > 0)
                motion = float(np.median(np.abs(np.log(a1[ok]) - np.log(a0[ok])))) if ok.any() else None
            self.prev_pic = pic
            self.pk_hist.append(cnt)
            low = len(self.pk_hist) > 30 and cnt < 0.5 * np.median(self.pk_hist)
            x = motion / np.median(self.quiet) if (motion is not None and len(self.quiet) >= 60) else 0.0
            if motion is not None and not low and x < a.k2:
                self.quiet.append(motion)
                if len(self.quiet) == 60:
                    log(f"radio armed{tag}: usual motion {np.median(self.quiet):.5f} from the first 60 quiet seconds")
            radio.setdefault(t, {})[self.name] = (cnt, float(np.mean(rs)) if rs else None, motion, x, low)

    links = {}

    def combine(per_link):
        """the second's radio verdict: the link that moved most, among links with enough data"""
        if not per_link:
            return 0, None, None, 0.0, True, ""
        ok = {k: v for k, v in per_link.items() if not v[4]}
        pool = ok or per_link
        best = max(pool, key=lambda k: pool[k][3])
        cnt = sum(v[0] for v in per_link.values())
        _c, rssi, motion, x, _low = pool[best]
        return cnt, rssi, motion, x, not ok, best

    def decide(t):
        cnt, rssi, motion, x, low, best = combine(radio.pop(t, {}))
        cm, br = camres.pop(t, (None, None))
        radio_fire = (not low) and (x >= a.k or (x >= a.k2 and state["last_x"] >= a.k2))
        state["last_x"] = 0.0 if low else x
        cx = cm / max(np.median(cquiet), 0.3) if (cm is not None and len(cquiet) >= 30) else 0.0
        cam_fire = cx >= a.ck
        if cm is not None and cx < a.ck:
            cquiet.append(cm)
        sec_w.writerow([f"{t:.0f}", dt.datetime.fromtimestamp(t).strftime("%H:%M:%S"), cnt,
                        f"{rssi:.1f}" if rssi is not None else "", f"{motion:.5f}" if motion is not None else "",
                        f"{x:.1f}", int(low), f"{cm:.2f}" if cm is not None else "",
                        f"{cx:.1f}", f"{br:.0f}" if br is not None else "", best])
        ev = state["ev"]
        if radio_fire or cam_fire:
            if ev is None:
                ev = state["ev"] = dict(start=t, last=t, radio=False, cam=False, rx=0.0, cx=0.0, dark=[],
                                        link="")
                log(f"event starts {dt.datetime.fromtimestamp(t):%H:%M:%S} "
                    f"({'+'.join(w for w, f in (('radio', radio_fire), ('camera', cam_fire)) if f)})")
            ev["last"] = t
            ev["radio"] |= radio_fire
            ev["cam"] |= cam_fire
            if radio_fire and x > ev["rx"]:
                ev["rx"] = x
                ev["link"] = best
            if cam_fire:
                ev["cx"] = max(ev["cx"], cx)
        if ev is not None:
            if br is not None:
                ev["dark"].append(br)
            if t - ev["last"] > a.post:
                close_event(ev)
                state["ev"] = None

    def close_event(e):
        kind = "radio+camera" if e["radio"] and e["cam"] else ("radio only" if e["radio"] else "camera only")
        eid = dt.datetime.fromtimestamp(e["start"]).strftime("%Y%m%d-%H%M%S")
        n = 0
        if cam:
            n = cam.keep(e["start"] - a.pre, e["last"] + a.post, os.path.join(a.outdir, "events", eid))
        dark = bool(e["dark"]) and float(np.median(e["dark"])) < 20
        row = dict(event=eid, start=dt.datetime.fromtimestamp(e["start"]).strftime("%H:%M:%S"),
                   end=dt.datetime.fromtimestamp(e["last"]).strftime("%H:%M:%S"),
                   seconds=f"{e['last'] - e['start'] + 1:.0f}", kind=kind,
                   radio_peak=f"{e['rx']:.1f}", camera_peak=f"{e['cx']:.1f}",
                   frames=n, hint="dark" if dark else "", label="", note="", radio_link=e.get("link", ""))
        events.append(row)
        write_events()
        log(f"event {eid}: {row['start']}-{row['end']} {kind}, radio x{row['radio_peak']}, "
            f"camera x{row['camera_peak']}, {n} frames{' (dark)' if dark else ''}")

    def write_events():
        cols = ["event", "start", "end", "seconds", "kind", "radio_peak", "camera_peak",
                "frames", "hint", "label", "note", "radio_link"]
        with open(ev_path, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=cols)
            w.writeheader()
            for r in events:
                w.writerow({c: r.get(c, "") for c in cols})

    n_dgrams = 0
    try:
        while not STOP[0] and time.time() < end:
            try:
                d = sock.recv(65536)
                n_dgrams += 1
            except socket.timeout:
                d = None
            now = time.time()
            if cam and now - last_cam_check >= 0.25:
                last_cam_check = now
                for ft, cm, br in cam.new_frames():
                    camres[int(ft)] = (cm, br)
                if not cam.alive() and now - started > 5:
                    log("camera stopped (see camera.log); carrying on with radio only")
                    cam = None
                elif cam:
                    ev = state["ev"]
                    cam.tidy((ev["start"] if ev else now - LAG) - a.pre - 30)
            # decide every second that is now LAG seconds old
            if state["next_t"] is None and (radio or camres):
                state["next_t"] = int(now) - LAG
            while state["next_t"] is not None and state["next_t"] <= now - LAG:
                decide(state["next_t"])
                state["next_t"] += 1
            for old in [k for k in list(radio) + list(camres) if state["next_t"] and k < state["next_t"] - 60]:
                radio.pop(old, None)
                camres.pop(old, None)
            for lk in links.values():                             # no data: close the second anyway
                if lk.cur is not None and now - lk.cur > 2:
                    lk.finish()
            if d is None:
                if n_dgrams == 0 and now - started > 30 and int(now) % 60 == 0:
                    log("nothing on the tap yet: is listen.py running with --tap?")
                continue
            p = parse(d)
            if p is None:
                continue
            ht, name, rssi, amp = p
            lk = links.get(name) or links.setdefault(name, RadioLink(name))
            lk.add(int(ht), rssi, amp)
    finally:
        if state["ev"] is not None:
            close_event(state["ev"])
        sec_f.close()
        if cam:
            cam.close()
        log(f"stop: {len(events)} events in {a.outdir}")
        try:
            make_sheets(a.outdir, events)
        except Exception as e:                 # never lose the events over a picture
            log(f"contact sheet failed: {e}")
        LOG.close()


def make_sheets(folder, events, per_page=10):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from PIL import Image
    if not events:
        return
    secs = list(csv.DictReader(open(os.path.join(folder, "seconds.csv"))))
    st = np.array([float(r["time"]) for r in secs])
    rx = np.array([float(r["radio_x"] or 0) for r in secs])
    cx = np.array([float(r["cam_x"] or 0) for r in secs])
    for page in range(0, len(events), per_page):
        chunk = events[page:page + per_page]
        fig, axes = plt.subplots(len(chunk), 5, figsize=(15, 2.3 * len(chunk)), squeeze=False,
                                 gridspec_kw=dict(width_ratios=[1.6, 1, 1, 1, 1]))
        for row, e in zip(axes, chunk):
            t0 = dt.datetime.strptime(e["event"], "%Y%m%d-%H%M%S").timestamp()
            dur = float(e["seconds"])
            sel = (st >= t0 - 20) & (st <= t0 + dur + 20)
            row[0].plot(st[sel] - t0, rx[sel], lw=0.8, label="radio x", color="#3b6ea5")
            row[0].plot(st[sel] - t0, cx[sel], lw=0.8, label="camera x", color="#e67e22")
            row[0].set_yscale("symlog", linthresh=1)
            row[0].set_title(f"{e['start']}-{e['end']}  {e['kind']}  "
                             f"r x{e['radio_peak']} c x{e['camera_peak']} {e['hint']}", fontsize=8)
            row[0].tick_params(labelsize=7)
            fr = sorted(glob.glob(os.path.join(folder, "events", e["event"], "*.jpg")))
            picks = [fr[int(i)] for i in np.linspace(0, len(fr) - 1, 4)] if len(fr) >= 4 else fr
            for ax in row[1:]:
                ax.axis("off")
            for ax, f in zip(row[1:], picks):
                ax.imshow(Image.open(f))
                ax.set_title(os.path.basename(f)[9:15], fontsize=7)
        axes[0][0].legend(fontsize=6, loc="upper right")
        fig.tight_layout()
        out = os.path.join(folder, f"sheet-{page // per_page + 1:02d}.png")
        fig.savefig(out, dpi=80)
        plt.close(fig)
        log(f"saved {out}")


if __name__ == "__main__":
    main()
