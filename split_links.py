#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""split_links.py - turn a many-board (tw_node) recording into one folder per
link, in the same line format as the single-link recordings, so view.py,
analyze_csi.py and radar_replay.py work on each link unchanged.

    ~/scratch-esp/bin/python split_links.py RUN_FOLDER [--view]
    e.g. ~/scratch-esp/bin/python split_links.py ~/tw-data/runs/2026-11-02_0700 --view
         ~/scratch-esp/bin/python split_links.py ~/tw-data/runs/2026-11-02_0700 --view-only

Writes RUN_FOLDER/links/<from>-<to>/csi-YYYYMMDD-HH.csv.gz for every link
("2-1" = board 1 measuring board 2's frames), and links/summary.txt (records,
rate, loss and RSSI per link, and the boards seen) and links/channel.json.
With --view it also runs view.py on each link folder (several at once), then
links_report.py, which compares the links: links/across.txt.
--view-only remakes the views and the report of a run split before, without
splitting again; --again removes links/ and splits afresh (needed once for runs
split before v5.2, to get the path / delivery losses).

Each record is counted once, even if it reached the computer twice (each
record is sent in two frames): records are told apart by (measuring board,
boot number, record number). Relayed records can arrive slightly out of
order (a lost first copy is made up by the second, a frame later), so each
link is put back in measurement order before it is written. The amplitudes
become the "I" values of the usual CSI_DATA line (with Q = 0), so every tool
sees the same amplitudes.
"""
import argparse
import collections
import datetime as dt
import glob
import gzip
import heapq
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_csi as A  # noqa: E402  (read_lines: reads gzip cut short by a power cut)

REORDER = 256           # records held back per link to restore measurement order


def parse_twr(f):
    """fields after host_ns: TWR,rx,tx,txseq,hop,recseq,boot,rx_us,net_us,rssi,noise,agc,fft,phy,kind,n,step,hex"""
    if len(f) != 18:
        return None
    try:
        n, step = int(f[15]), int(f[16])
        h = f[17]
        if len(h) != 2 * n:
            return None
        q = bytes.fromhex(h)
        return dict(rx=int(f[1]), tx=int(f[2]), txseq=int(f[3]), hop=int(f[4]), recseq=int(f[5]),
                    boot=int(f[6]), rx_us=int(f[7]), net_us=int(f[8]), rssi=int(f[9]), noise=int(f[10]),
                    agc=int(f[11]), fft=int(f[12]), phy=int(f[13]), kind=int(f[14]), n=n,
                    q=q, step=step)
    except ValueError:
        return None


_AMP_TEXT = {}


def amp_text(step):
    """the 256 possible "I,Q" texts for one quantizing step (amplitude = q x step / 4)"""
    t = _AMP_TEXT.get(step)
    if t is None:
        t = _AMP_TEXT[step] = [f"{int(round(v * step / 4))},0" for v in range(256)]
    return t


def csi_line(host_ns, r):
    phy = r["phy"]
    cwb, stbc, sig, sec = phy & 3, (phy >> 2) & 1, (phy >> 3) & 3, (phy >> 5) & 3
    ts = r["rx_us"] - (1 << 32) if r["rx_us"] >= (1 << 31) else r["rx_us"]      # signed, as the board prints
    data = ",".join(map(amp_text(r["step"]).__getitem__, r["q"]))
    fwi = 1 if r["kind"] & 0x80 else 0
    return (f"{host_ns},CSI_DATA,{r['txseq']},1a:00:00:00:00:{r['tx']:02x},{r['rssi']},11,{sig},0,{cwb},0,0,0,"
            f"{stbc},0,0,{r['noise']},0,11,{sec},{ts},0,0,0,{2 * r['n']},{fwi},\"[{data}]\"\n")


class Splitter:
    def __init__(self, folder):
        self.folder = folder
        self.outdir = os.path.join(folder, "links")
        os.makedirs(self.outdir, exist_ok=True)
        self.outs = {}                                  # (link, hour) -> gzip file
        self.pending = collections.defaultdict(list)    # link -> heap
        self.boot_index = {}                            # (rx, boot) -> order the boots were seen
        self.seen = set()
        self.boards = {}
        self.dups = self.bad = 0
        self.hour_span = (0, 0, "")
        self.win = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))  # 10 s -> rx -> [n, noise sum]
        self.tws = []                                   # the exit's timekeeping, where it changed
        # for telling the two losses apart, per 10 s window (see loss_windows)
        self.lw = collections.Counter()                 # (window, link) -> records delivered
        self.txw = {}                                   # (window, tx) -> [lowest, highest frame number seen]
        self.rxw = {}                                   # (window, rx, boot) -> [records, lowest, highest record number]
        self.stats = collections.defaultdict(lambda: dict(n=0, first=None, last=None, lost=0, last_seq=None,
                                                          last_boot=None, rssi=[], boots=set()))

    def add(self, host_ns, r):
        key = (r["rx"], r["boot"], r["recseq"])
        if key in self.seen:
            self.dups += 1
            return
        self.seen.add(key)
        w10 = host_ns // 10**10 * 10
        w = self.win[w10][r["rx"]]
        w[0] += 1
        w[1] += r["noise"]
        link = f"{r['tx']}-{r['rx']}"
        self.lw[(w10, link)] += 1
        t = self.txw.get((w10, r["tx"]))
        if t is None:
            self.txw[(w10, r["tx"])] = [r["txseq"], r["txseq"]]
        else:
            t[0], t[1] = min(t[0], r["txseq"]), max(t[1], r["txseq"])
        q = self.rxw.get((w10, r["rx"], r["boot"]))
        if q is None:
            self.rxw[(w10, r["rx"], r["boot"])] = [1, r["recseq"], r["recseq"]]
        else:
            q[0] += 1
            q[1], q[2] = min(q[1], r["recseq"]), max(q[2], r["recseq"])
        bi = self.boot_index.setdefault((r["rx"], r["boot"]), len(self.boot_index))
        heap = self.pending[link]
        heapq.heappush(heap, (bi, r["recseq"], host_ns, len(self.seen), r))
        if len(heap) > REORDER:
            self.write(link, *heapq.heappop(heap))

    def write(self, link, _bi, _recseq, host_ns, _k, r):
        lo, hi, hour = self.hour_span
        if not lo <= host_ns < hi:
            t = dt.datetime.fromtimestamp(host_ns / 1e9).replace(minute=0, second=0, microsecond=0)
            lo = int(t.timestamp() * 1e9)
            hour = t.strftime("%Y%m%d-%H")
            self.hour_span = (lo, lo + 3600 * 10**9, hour)
        fh = self.outs.get((link, hour))
        if fh is None:
            d = os.path.join(self.outdir, link)
            os.makedirs(d, exist_ok=True)
            fh = self.outs[(link, hour)] = gzip.open(os.path.join(d, f"csi-{hour}.csv.gz"), "at", compresslevel=3)
            fh.write(f"# name=csi-{hour} link={link} (board {r['rx']} measuring board {r['tx']}) "
                     f"split from {os.path.abspath(self.folder)}\n")
        fh.write(csi_line(host_ns, r))
        s = self.stats[link]
        s["n"] += 1
        s["first"] = host_ns if s["first"] is None else s["first"]
        s["last"] = host_ns
        s["boots"].add((r["rx"], r["boot"]))
        if len(s["rssi"]) < 200000:
            s["rssi"].append(r["rssi"])
        # frames of the sender this board did not measure (a reboot of either restarts the count)
        if s["last_seq"] is not None and s["last_boot"] == r["boot"] and 0 < r["txseq"] - s["last_seq"] < 1000:
            s["lost"] += r["txseq"] - s["last_seq"] - 1
        s["last_seq"], s["last_boot"] = r["txseq"], r["boot"]

    def finish(self):
        for link, heap in self.pending.items():
            while heap:
                self.write(link, *heapq.heappop(heap))
        for fh in self.outs.values():
            fh.close()

    @staticmethod
    def _made(series):
        """{window: [lowest, highest]} of a counter that goes up by one per frame (or record)
        -> {window: how many were made in that window}: the rise of the highest number since
        the window before, or (first window, after a gap or a restart) highest - lowest + 1"""
        out, prev_w, prev_hi = {}, None, None
        for w in sorted(series):
            lo, hi = series[w]
            if prev_w == w - 10 and 0 <= hi - prev_hi <= max(4 * (hi - lo + 1), 50):
                out[w] = hi - prev_hi
            else:
                out[w] = hi - lo + 1
            prev_w, prev_hi = w, hi
        return out

    def loss_windows(self):
        """The two losses of each link, per 10 s window.

        A link's frames can go missing on two trips. PATH: the measuring board never
        heard the frame (that is sensing: something in the way shows here). DELIVERY:
        the board measured it, but its report did not reach the computer (it travels
        in the board's own frames, over its path to the cabled board; plumbing).
        Every board numbers its reports one after another (record number), so a gap
        there is a delivery loss, and it hits all of that board's links alike (their
        reports ride in the same frames). Frame numbers count what the sender sent.
        So, per window: sent = frames the sender sent; delivered = reports that came;
        delivery fraction f = reports that came / reports the board made;
        heard = delivered / f; path lost = sent - heard.
        Returns {link: [(window, sent, delivered, heard, path %, delivery %), ...]}"""
        tx_made = {}
        for tx in {k[1] for k in self.txw}:
            tx_made[tx] = self._made({w: v for (w, t), v in self.txw.items() if t == tx})
        rx_frac = collections.defaultdict(lambda: [0, 0])          # (window, rx) -> [came, made]
        for rb in {(k[1], k[2]) for k in self.rxw}:
            rows = {w: v[1:] for (w, rx, b), v in self.rxw.items() if (rx, b) == rb}
            made = self._made(rows)
            for w, m in made.items():
                c = rx_frac[(w, rb[0])]
                c[0] += self.rxw[(w, rb[0], rb[1])][0]
                c[1] += max(m, self.rxw[(w, rb[0], rb[1])][0])
        self.rx_frac = rx_frac
        out = collections.defaultdict(list)
        for (w, link), got in sorted(self.lw.items()):
            tx, rx = (int(x) for x in link.split("-"))
            sent = tx_made.get(tx, {}).get(w)
            came, made = rx_frac.get((w, rx), (0, 0))
            if not sent or not made:
                continue
            f = came / made
            heard = min(sent, got / f) if f > 0 else float(got)
            out[link].append((w, sent, got, heard, 100 * (1 - heard / sent), 100 * (1 - f)))
        return out

    def save_losses(self):
        """links/<link>/loss.csv (view.py reads it) and the totals for summary.txt"""
        self.loss_tot = {}
        for link, rows in self.loss_windows().items():
            d = os.path.join(self.outdir, link)
            if not os.path.isdir(d):
                continue
            with open(os.path.join(d, "loss.csv"), "w") as fh:
                fh.write("# per 10 s: frames sent, reports delivered, frames heard (estimated), "
                         "path lost %, delivery lost % (see split_links.py loss_windows)\n")
                fh.write("t,sent,delivered,heard,path_lost_pct,delivery_lost_pct\n")
                for w, sent, got, heard, pl, dl in rows:
                    fh.write(f"{w},{sent},{got},{heard:.1f},{pl:.2f},{dl:.2f}\n")
            sent = sum(r[1] for r in rows)
            got = sum(r[2] for r in rows)
            heard = sum(r[3] for r in rows)
            self.loss_tot[link] = (100 * (1 - heard / sent) if sent else 0.0,
                                   100 * (1 - got / heard) if heard else 0.0)

    def note_tws(self, host_ns, f):
        """TWS,id,ver,leader,flags,active,period,slot,offset,...,leader_changes,queue_used"""
        try:
            row = [host_ns / 1e9, int(f[1]), int(f[3]), int(f[15])]
        except (ValueError, IndexError):
            return
        if not self.tws or self.tws[-1][1:] != row[1:]:
            self.tws.append(row)
        self.tws_last = row

    def save_channel(self):
        """links/channel.json: records and mean noise floor per measuring board each 10 s,
        and the exit's timekeeper and leader-change count wherever they changed"""
        last = getattr(self, "tws_last", None)
        tws = self.tws + ([last] if last is not None and last is not self.tws[-1] else [])
        frac = getattr(self, "rx_frac", {})
        rows = [[t, {str(rx): [n, round(ns / n, 2),
                               round(100 * (1 - frac[(t, rx)][0] / frac[(t, rx)][1]), 1)
                               if frac.get((t, rx)) and frac[(t, rx)][1] else None]
                     for rx, (n, ns) in sorted(v.items())}]
                for t, v in sorted(self.win.items())]
        json.dump(dict(win_s=10, windows=rows, tws=tws), open(os.path.join(self.outdir, "channel.json"), "w"))

    def report(self):
        L = [f"run       {os.path.abspath(self.folder)}",
             f"records   {len(self.seen):,} (duplicates skipped {self.dups:,}, unreadable lines {self.bad})", ""]
        L += sorted(self.boards.values()) + ([""] if self.boards else [])
        L.append("link   (from-to)   records    span h   rate/s   missing    path  delivery   median RSSI   boots")
        scene = os.path.join(self.folder, "scene.txt")
        for link in sorted(self.stats, key=lambda k: [int(x) for x in k.split("-")]):
            s = self.stats[link]
            span = (s["last"] - s["first"]) / 1e9 if s["n"] > 1 else 0
            rs = sorted(s["rssi"])
            lost = 100 * s["lost"] / max(s["n"] + s["lost"], 1)
            pl, dl = getattr(self, "loss_tot", {}).get(link, (float("nan"), float("nan")))
            L.append(f"  {link:>6}            {s['n']:9,d}  {span / 3600:7.2f}  {s['n'] / max(span, 1):7.1f}  "
                     f"{lost:6.1f}%   {pl:5.1f}%   {dl:5.1f}%      {rs[len(rs) // 2] if rs else 0:6d}       "
                     f"{len(s['boots'])}")
            dst = os.path.join(self.outdir, link, "scene.txt")
            if os.path.exists(scene) and not os.path.exists(dst):
                open(dst, "w").write(open(scene).read())
        L += ["", "missing = frames of the sender with no report here. path = the measuring board did not",
              "hear them (sensing: something in the way shows here); delivery = it heard them, but its",
              "report never reached the computer (plumbing: the measuring board's path to the cabled one).",
              "The two are estimated per 10 s from the frame and record numbers (links/<link>/loss.csv)."]
        text = "\n".join(L) + "\n"
        open(os.path.join(self.outdir, "summary.txt"), "w").write(text)
        return text


def run_views(outdir, links, workers):
    """view.py on each link, a few at a time (one per processor core)"""
    here = os.path.dirname(os.path.abspath(__file__))
    todo = list(links)
    running = []
    while todo or running:
        while todo and len(running) < workers:
            link = todo.pop(0)
            d = os.path.join(outdir, link)
            fh = open(os.path.join(d, "view.out"), "w")
            print(f"view of link {link} ...", flush=True)
            running.append((link, fh, subprocess.Popen([sys.executable, os.path.join(here, "view.py"), d],
                                                       stdout=fh, stderr=subprocess.STDOUT)))
        link, fh, p = running.pop(0)
        p.wait()
        fh.close()
        ok = os.path.exists(os.path.join(outdir, link, "view.json"))
        print(f"  {os.path.join(outdir, link, 'view.txt')}" if ok and p.returncode == 0
              else f"  link {link}: view.py failed, see {os.path.join(outdir, link, 'view.out')}", flush=True)


def link_names(outdir):
    names = [os.path.basename(d) for d in glob.glob(os.path.join(outdir, "*-*"))
             if glob.glob(os.path.join(d, "csi-*.csv.gz"))]
    return sorted(names, key=lambda k: [int(x) for x in k.split("-")])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--view", action="store_true",
                    help="also run view.py on each link, then links_report.py across them")
    ap.add_argument("--view-only", action="store_true",
                    help="the run is split already: only (re)make the views and the report")
    ap.add_argument("--again", action="store_true",
                    help="remove the run's links/ folder and split again (e.g. a run split by an older kit)")
    ap.add_argument("--workers", type=int, default=max(1, min(4, os.cpu_count() or 1)),
                    help="views made at the same time (default: processor cores, at most 4)")
    a = ap.parse_args()
    outdir = os.path.join(a.folder, "links")
    if a.again and os.path.isdir(outdir):
        import shutil
        shutil.rmtree(outdir)
        print(f"removed {outdir}; splitting again")
    if a.view_only:
        links = link_names(outdir)
        if not links:
            sys.exit(f"no split links in {outdir}; run without --view-only first")
    else:
        files = sorted(glob.glob(os.path.join(a.folder, "csi-*.csv.gz")))
        if not files:
            sys.exit(f"no csi-*.csv.gz in {a.folder}")
        if link_names(outdir):
            sys.exit(f"{outdir} already has split files; remove that folder to split again "
                     f"(or use --view-only to remake the views)")
        sp = Splitter(a.folder)
        for path in files:
            print(f"reading {os.path.basename(path)}", flush=True)
            for line in A.read_lines(path):
                f = line.rstrip("\n").split(",")
                if len(f) < 2:
                    continue
                if f[1] == "TWH" and len(f) >= 5:
                    sp.boards[f[2]] = f"board {f[2]}: factory address {f[3]}, firmware {f[4]}"
                    continue
                if f[1] == "TWS":
                    try:
                        sp.note_tws(int(f[0]), f[1:])
                    except ValueError:
                        pass
                    continue
                if f[1] != "TWR":
                    continue
                r = parse_twr(f[1:])
                try:
                    host_ns = int(f[0])
                except ValueError:
                    r = None
                if r is None:
                    sp.bad += 1
                    continue
                sp.add(host_ns, r)
        sp.finish()
        sp.save_losses()
        sp.save_channel()
        print(sp.report())
        print(f"saved {outdir}/<link>/ and {os.path.join(outdir, 'summary.txt')}")
        links = sorted(sp.stats, key=lambda k: [int(x) for x in k.split("-")])
    if a.view or a.view_only:
        run_views(outdir, links, a.workers)
        import links_report
        text = links_report.report(outdir)
        print(text)
        print(f"saved {os.path.join(outdir, 'across.txt')}")


if __name__ == "__main__":
    main()
