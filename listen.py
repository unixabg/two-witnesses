#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""listen.py - the long-run listener. Records the receiver board for hours
(a night, a day, a week) into one gzip file per clock hour, written straight
to disk (the USB), never to RAM.

Usage:
    ~/scratch-esp/bin/python listen.py PORT RUN_FOLDER [--until HH:MM]
                                                       [--hours N] [--baud B]
    e.g. ~/scratch-esp/bin/python listen.py /dev/ttyACM0 ~/tw-data/runs/2026-10-26_2200 --until 07:00

Writes into RUN_FOLDER (made if missing; run.sh names it by date and start time):
    csi-YYYYMMDD-HH.csv.gz     one per clock hour; lines as record_csi.py:
                               host_ns,<the board's CSI_DATA line>
                               (or, from tw_node boards, host_ns,TWR/TWN/TWS/... lines:
                               split_links.py turns those into one folder per link)
                               (a restart within the hour starts a new file,
                               csi-YYYYMMDD-HH.part2.csv.gz: never an append)
    listen.log                 a line every 10 minutes: rate, lost, gaps
It stops at --until (next time that clock time comes round) or after
--hours, whichever comes first; Ctrl-C also stops it cleanly.

Safety habits:
  - Refuses to start if OUTDIR is on a RAM disk (tmpfs, or a live system's
    overlay whose writable layer is in RAM). --force overrides.
  - Warns if less than 2 GB is free (an hour is about 45 MB, a night 400 MB).
  - Flushes the gzip stream every minute, so a power cut loses at most a
    minute and the file still opens (analyze_csi.py and view.py read
    up to the break).
  - If the board is unplugged or resets, it waits and reconnects, and
    notes the gap in listen.log.
  - --no-save keeps only listen.log (live.sh: the dashboard without a recording).
  - --tap PORT also copies each line to a UDP port on this computer, for the
    live detector (watch.py) and the dashboard (dash.py); give --tap more than
    once for several. Copies that nobody takes are simply dropped.
"""
import argparse
import datetime as dt
import gzip
import os
import shutil
import signal
import sys
import time
import zlib

RAM_FS = {"tmpfs", "ramfs", "devtmpfs"}
# lines from tw_node, the many-board firmware: records, boards heard, status, hello, health
TW_KINDS = ("TWR,", "TWN,", "TWS,", "TWH,", "TWF,", "TWOK")


def mount_of(path):
    """(mount point, fstype, super options) of the mount holding path."""
    path = os.path.realpath(path)
    best = ("/", "?", "")
    for line in open("/proc/self/mountinfo"):
        left, right = line.split(" - ", 1)
        mnt = left.split()[4].replace("\\040", " ")
        fstype, _src, sopts = (right.split() + ["", ""])[:3]
        if (path == mnt or path.startswith(mnt.rstrip("/") + "/")) and len(mnt) >= len(best[0]):
            best = (mnt, fstype, sopts)
    return best


def ram_check(path):
    """Return (is_ram, plain explanation)."""
    mnt, fstype, sopts = mount_of(path)
    if fstype in RAM_FS:
        return True, f"{path} is on {mnt} ({fstype}): that is RAM"
    if fstype == "overlay":
        upper = [o.split("=", 1)[1] for o in sopts.split(",") if o.startswith("upperdir=")]
        if not upper:
            return True, f"{path} is on an overlay ({mnt}) whose writable layer could not be found"
        umnt, ufs, _ = mount_of(upper[0])
        if ufs in RAM_FS:
            return True, (f"{path} is on the live system's overlay; its writable layer "
                          f"{upper[0]} is in RAM ({ufs}). Is persistence on?")
        return False, f"{path}: overlay, writable layer {upper[0]} on {umnt} ({ufs})"
    return False, f"{path}: {mnt} ({fstype})"


def stop_time(until, hours):
    now = dt.datetime.now()
    ends = []
    if until:
        h, m = (int(x) for x in until.split(":"))
        end = now.replace(hour=h, minute=m, second=0, microsecond=0)
        if end <= now:
            end += dt.timedelta(days=1)
        ends.append(end)
    if hours:
        ends.append(now + dt.timedelta(hours=hours))
    if not ends:
        sys.exit("give --until HH:MM or --hours N")
    return min(ends)


class Hourly:
    """One gzip file per period (normally a clock hour)."""

    def __init__(self, folder, rotate_min, meta):
        self.folder, self.rotate_min, self.meta = folder, rotate_min, meta
        self.f = self.key = None
        self.last_flush = time.time()

    def _key(self, now):
        m = (now.minute // self.rotate_min) * self.rotate_min if self.rotate_min < 60 else 0
        return now.replace(minute=m, second=0, microsecond=0)

    def write(self, text):
        now = dt.datetime.now()
        key = self._key(now)
        if key != self.key:
            self.close()
            self.key = key
            stamp = key.strftime("%Y%m%d-%H") + ("" if self.rotate_min >= 60 else key.strftime("%M"))
            path = os.path.join(self.folder, f"csi-{stamp}.csv.gz")
            part = 2
            while os.path.exists(path):
                # a restart within the same hour: a new file, never an append. A gzip
                # member appended after one that was cut short (hard kill, power cut)
                # can never be read back; a separate file always can.
                path = os.path.join(self.folder, f"csi-{stamp}.part{part}.csv.gz")
                part += 1
            self.raw = open(path, "xb")
            self.f = gzip.GzipFile(fileobj=self.raw, mode="wb", compresslevel=6)
            self.f.write(f"# name=csi-{stamp} {self.meta} start_unix={time.time():.3f}\n".encode())
            log(f"file {path}")
        self.f.write(text.encode())
        if time.time() - self.last_flush >= 60:
            self.f.flush(zlib.Z_SYNC_FLUSH)     # gzip sync point: readable after a power cut
            self.raw.flush()
            os.fsync(self.raw.fileno())
            self.last_flush = time.time()

    def close(self):
        if self.f:
            self.f.close()
            self.raw.close()
            self.f = None


class NoSave:
    """--no-save: the lines go to the taps only"""

    def write(self, text):
        pass

    def close(self):
        pass


LOG = None


def log(msg):
    line = f"{dt.datetime.now():%Y-%m-%d %H:%M:%S}  {msg}"
    print(line, flush=True)
    if LOG:
        LOG.write(line + "\n")
        LOG.flush()


def open_port(port, baud):
    import serial
    tries = 0
    while True:
        try:
            s = serial.Serial(port, baud, timeout=1)
            s.reset_input_buffer()
            if tries:
                log(f"port {port} open again after {10 * tries} s")
            return s
        except (OSError, serial.SerialException) as e:
            if tries % 30 == 0:         # once, then every 5 minutes: not a line every 10 s for hours
                log(f"cannot open {port} ({e.__class__.__name__}); retrying every 10 s")
            tries += 1
            time.sleep(10)
            if STOP[0]:
                return None


STOP = [False]


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("outdir", help="the run folder; files go straight into it")
    ap.add_argument("--until", help="stop at this clock time, HH:MM (e.g. 07:00)")
    ap.add_argument("--hours", type=float, help="stop after this many hours")
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--force", action="store_true", help="write even if OUTDIR looks like RAM")
    ap.add_argument("--tap", type=int, action="append", default=[],
                    help="also send each line to this UDP port on this computer (watch.py, dash.py); "
                         "may be given more than once; the recording never waits for it")
    ap.add_argument("--no-save", action="store_true",
                    help="keep nothing but listen.log: only feed the taps (live.sh: a look without recording)")
    ap.add_argument("--rotate-minutes", type=int, default=60, help=argparse.SUPPRESS)  # for tests
    a = ap.parse_args()

    end = stop_time(a.until, a.hours)
    os.makedirs(a.outdir, exist_ok=True)
    is_ram, why = ram_check(a.outdir)
    print(why if not a.no_save else "not saving the lines (--no-save): only listen.log is written")
    if is_ram and not a.force and not a.no_save:
        sys.exit("REFUSED: that would fill RAM within hours. Point the run folder at the USB "
                 "(or use --force if you know better).")
    free_gb = shutil.disk_usage(a.outdir).free / 1e9
    print(f"free space {free_gb:.1f} GB" + ("   WARNING: under 2 GB" if free_gb < 2 else ""))

    run = a.outdir
    LOG = open(os.path.join(run, "listen.log"), "a")
    log(f"start: port={a.port} baud={a.baud} stop at {end:%Y-%m-%d %H:%M} folder={run}")

    signal.signal(signal.SIGTERM, lambda *_: STOP.__setitem__(0, True))
    signal.signal(signal.SIGINT, lambda *_: STOP.__setitem__(0, True))

    import serial
    out = NoSave() if a.no_save else Hourly(run, a.rotate_minutes, f"port={a.port} baud={a.baud}")
    ser = open_port(a.port, a.baud)
    n_ok = n_other = 0
    first_id = last_id = None
    lost = 0
    last_rx = time.time()
    gap_noted = False
    period_start, period_ok, period_lost = time.time(), 0, 0

    tap = None
    taps = [("127.0.0.1", p) for p in a.tap if p]
    if taps:
        import socket
        tap = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tap.setblocking(False)
        log("tap: copying lines to UDP " + ", ".join(f"127.0.0.1:{p}" for _h, p in taps))
    buf = b""
    n_merged = 0
    n_tw = period_tw = 0
    cpu0 = time.process_time()
    try:
        while not STOP[0] and dt.datetime.now() < end and ser is not None:
            try:
                # read whatever has arrived, in one go: reading byte by byte (readline)
                # cost too much CPU on the Chromebook, and the USB chip then dropped
                # whole chunks of lines (night of 2026-10-24: 45% lost)
                chunk = ser.read(max(1, min(ser.in_waiting, 1 << 16)))
            except (OSError, serial.SerialException) as e:
                log(f"port lost ({e.__class__.__name__}); waiting for the board")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = open_port(a.port, a.baud)
                last_id = None          # board may have restarted; don't count a gap as loss
                buf = b""
                continue
            now = time.time()
            if not chunk:
                if now - last_rx > 5 and not gap_noted:
                    log("no data for 5 s (sender off? receiver stuck?)")
                    gap_noted = True
                continue
            if gap_noted:
                log(f"data back after {now - last_rx:.0f} s")
                gap_noted = False
            last_rx = now
            stamp = time.time_ns()
            buf += chunk
            if b"\n" not in chunk:
                if len(buf) > 1 << 20:      # no line end in 1 MB: wrong baud? start afresh
                    buf = b""
                continue
            *raws, buf = buf.split(b"\n")
            for raw in raws:
                line = raw.decode("ascii", errors="replace").strip()
                tw = line.startswith(TW_KINDS)
                if not (tw or line.startswith("CSI_DATA,")):
                    if line:
                        n_other += 1
                    continue
                if line.count("CSI_DATA") + sum(line.count(k) for k in TW_KINDS) > 1:
                    n_merged += 1           # bytes were dropped on the way: two lines ran together
                out.write(f"{stamp},{line}\n")
                if tap is not None:
                    dgram = f"{stamp},{line}".encode()
                    for dest in taps:
                        try:
                            tap.sendto(dgram, dest)
                        except OSError:
                            pass            # nobody listening, or busy: never hold up the recording
                if tw:                      # tw_node (many boards): loss is counted per link by dash/split_links
                    n_tw += 1
                    period_tw += line.startswith("TWR,")
                    continue
                n_ok += 1
                period_ok += 1
                try:
                    pid = int(line.split(",", 2)[1])
                    if last_id is not None and 0 < pid - last_id < 100000:
                        lost += pid - last_id - 1
                        period_lost += pid - last_id - 1
                    first_id = pid if first_id is None else first_id
                    last_id = pid
                except ValueError:
                    pass
            if now - period_start >= 600:
                sent = period_ok + period_lost
                cpu = 100 * (time.process_time() - cpu0) / (now - period_start)
                if period_tw:
                    log(f"10 min: many-board records {period_tw / (now - period_start):.1f}/s, "
                        f"run-together lines {n_merged}, other lines {n_other}, CPU {cpu:.0f}%")
                else:
                    log(f"10 min: {period_ok / (now - period_start):.1f}/s, lost {period_lost} of {sent} "
                        f"({100 * period_lost / max(sent, 1):.1f}%), run-together lines {n_merged}, "
                        f"other lines {n_other}, CPU {cpu:.0f}%")
                period_start, period_ok, period_lost, n_merged, period_tw = now, 0, 0, 0, 0
                cpu0 = time.process_time()
    except Exception as e:                  # disk full, say: name it in listen.log, then fall
        log(f"CRASH: {e!r}")
        raise
    finally:
        out.close()
        sent = n_ok + lost
        log(f"stop: {n_ok} CSI lines, lost {lost} of {sent} ({100 * lost / max(sent, 1):.1f}%)"
            + (f"; {n_tw} many-board lines (tw_node)" if n_tw else ""))
        LOG.close()


if __name__ == "__main__":
    main()
