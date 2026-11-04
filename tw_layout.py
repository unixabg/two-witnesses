#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_layout.py - the board layout (boards.csv): show it, check it is still right,
edit it, and keep every version.

    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py            show it
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py confirm    show it and ask if it is still
                                                               right (survey.sh does this)
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py edit       change it, value by value
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py backups    list the earlier versions, put one back
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py keep       keep a copy if it changed (run.sh,
                                                               survey.sh, watch.sh do this at the start)
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py path       which file it is
    ~/scratch-esp/bin/python ~/tw-kit/tw_layout.py check RUN_FOLDER [--minutes 2] [--at HH:MM]
                         the layout against the radio, from a quiet stretch of a recording
                         (survey.sh does this after its start; any run folder will do)

The one file you keep up to date: the kit's private/boards.csv (git ignores
private/). For a second site, run the kit with TW_DATA set to that site's
folder (TW_DATA=~/site-b ~/tw-kit/run.sh 07:00): its layout is then
~/site-b/boards.csv. ~/tw-data/boards.csv, the old place, still counts when
private/boards.csv does not exist. Each run and survey copies the layout it
started with into its folder; replay.py and the views draw that copy, so an
old run keeps the layout it had.

Every version is kept, in a folder layouts/ beside the file
(private/layouts/boards-2026-11-03_181905.csv): before and after each edit here,
and at the start of each run or survey when the file changed since the last
copy (an edit with a text editor). Nothing is ever overwritten.

check: with nobody moving, a link's signal falls with its length, so each link's
median RSSI is set against its length in the layout (the level from the links
themselves, falling 25 dB per tenfold distance, as indoors). It flags a link
8 dB or more off what its length suggests (a board in the wrong place in the
file, or something solid in the path), a better fit with two board numbers
swapped, a board the others hear much better (or worse) than it hears them on
all its paths (its receiving or sending side), loss at rest over 20%, and
whether anything moved during the check (the numbers may then be off). It
cannot prove a layout right: it catches the common mistakes before a night is
recorded on a wrong map. With three boards the fit is rough (about 4 dB).

The format (also tw_map.py): one row per board, and any landmarks:
    id,x_ft,y_ft,facing,label
    1,0,-15,N,by the door
    T,6,4,,workbench
Positions in feet (or any unit, the same for both) from one reference point
you choose: x to the east, y to the north. facing: N, S, E, W or blank. A row
whose id is a number is a board; any other id is a landmark.
"""
import datetime as dt
import filecmp
import glob
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tw_map  # noqa: E402

HEADER = "id,x_ft,y_ft,facing,label"


def read(path):
    """-> (comment lines at the top, rows [dict id, x, y, facing, label]) in file order"""
    comments, rows = [], []
    if not path or not os.path.exists(path):
        return comments, rows
    with open(path, errors="replace") as f:
        for line in f:
            s = line.rstrip("\n")
            if s.strip().startswith("#"):
                if not rows:
                    comments.append(s)
                continue
            p = [c.strip() for c in s.split(",", 4)] + ["", "", "", "", ""]
            if not p[0] or p[0].lower() == "id":
                continue
            try:
                x, y = float(p[1]), float(p[2])
            except ValueError:
                continue
            rows.append(dict(id=p[0], x=x, y=y, facing=p[3].upper()[:1], label=p[4]))
    return comments, rows


def num(v):
    return f"{v:g}"


def write(path, comments, rows):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".new"
    with open(tmp, "w") as f:
        for c in comments:
            f.write(c + "\n")
        f.write(HEADER + "\n")
        for r in sorted(rows, key=lambda r: (not r["id"].isdigit(), int(r["id"]) if r["id"].isdigit() else 0, r["id"])):
            f.write(f"{r['id']},{num(r['x'])},{num(r['y'])},{r['facing']},{r['label']}\n")
    os.replace(tmp, path)


def backup_dir(path):
    return os.path.join(os.path.dirname(path), "layouts")


def _order(name):
    """boards-2026-11-03_182300.csv, boards-2026-11-03_182300-2.csv: by time, then counter"""
    m = re.fullmatch(r"boards-(.+?)(?:-(\d+))?\.csv", os.path.basename(name))
    return (m[1], int(m[2] or 1)) if m else (os.path.basename(name), 0)


def backups(path):
    return sorted(glob.glob(os.path.join(backup_dir(path), "boards-*.csv")), key=_order)


def keep(path, quiet=False):
    """copy the layout into layouts/ if it differs from the newest copy there; -> the copy or None"""
    if not os.path.exists(path):
        return None
    old = backups(path)
    if old and filecmp.cmp(old[-1], path, shallow=False):
        return None
    os.makedirs(backup_dir(path), exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y-%m-%d_%H%M%S")
    dest = os.path.join(backup_dir(path), f"boards-{stamp}.csv")
    k = 2
    while os.path.exists(dest):
        dest = os.path.join(backup_dir(path), f"boards-{stamp}-{k}.csv")
        k += 1
    shutil.copy2(path, dest)
    if not quiet:
        print(f"layout kept as {dest}")
    return dest


def changes(before, after):
    """a short account of what differs between two lists of rows"""
    a = {r["id"]: r for r in before}
    b = {r["id"]: r for r in after}
    out = []
    for i in sorted(set(a) | set(b), key=lambda s: (not s.isdigit(), int(s) if s.isdigit() else 0, s)):
        kind = "board" if i.isdigit() else "landmark"
        if i not in b:
            out.append(f"{kind} {i} removed")
        elif i not in a:
            out.append(f"{kind} {i} added")
        else:
            r0, r1 = a[i], b[i]
            if (r0["x"], r0["y"]) != (r1["x"], r1["y"]):
                out.append(f"{kind} {i} moved {num(r0['x'])},{num(r0['y'])} -> {num(r1['x'])},{num(r1['y'])}")
            if r0["facing"] != r1["facing"]:
                out.append(f"{kind} {i} faces {r1['facing'] or '-'} (was {r0['facing'] or '-'})")
            if r0["label"] != r1["label"]:
                out.append(f"{kind} {i} label '{r1['label']}' (was '{r0['label']}')")
    return out


def draw(rows, width=60, height=18):
    boards = {int(r["id"]): (r["x"], r["y"], r["facing"], r["label"]) for r in rows if r["id"].isdigit()}
    marks = [(r["id"][0].upper(), r["x"], r["y"], r["label"] or r["id"]) for r in rows if not r["id"].isdigit()]
    lines = tw_map.render(boards, {}, marks, width=width, height=height)
    return lines


def show(path, rows=None):
    if rows is None:
        _, rows = read(path)
    print(f"layout: {path}" + ("" if os.path.exists(path) else "  (not there yet)"))
    if not rows:
        print("  no boards in it yet")
        return
    for line in draw(rows):
        print("  " + line)
    for r in rows:
        kind = "board   " if r["id"].isdigit() else "landmark"
        print(f"  {kind} {r['id']:>3}  x {num(r['x']):>6}  y {num(r['y']):>6}  {r['facing'] or '-':1}  {r['label']}")


def ask(prompt, default=""):
    try:
        v = input(f"{prompt} [{default}]: " if default != "" else f"{prompt}: ").strip()
    except EOFError:
        raise SystemExit("\nstopped (nothing changed)")
    return v if v else str(default)


def ask_num(prompt, default):
    while True:
        v = ask(prompt, num(default) if default is not None else "")
        if v == "-":
            return "-"
        try:
            return float(v)
        except ValueError:
            print("    a number, please (or - to remove)")


def edit_row(r, is_board):
    name = f"board {r['id']}" if is_board else f"{r['id']}"
    x = ask_num(f"  {name}: x (ft east; - removes it)", r["x"])
    if x == "-":
        return None
    y = ask_num(f"  {name}: y (ft north)", r["y"])
    if y == "-":
        return None
    facing = r["facing"]
    if is_board:
        while True:
            facing = ask(f"  {name}: facing N/S/E/W (blank: -)", r["facing"] or "-").upper()
            facing = "" if facing == "-" else facing[:1]
            if facing in ("", "N", "S", "E", "W"):
                break
            print("    N, S, E, W or -")
    label = ask(f"  {name}: label", r["label"] or "-")
    return dict(id=r["id"], x=x, y=y, facing=facing, label="" if label == "-" else label.replace(",", " "))


def edit(path):
    comments, rows = read(path)
    print("Positions in feet from one reference point you choose (a corner, a door):")
    print("x to the east, y to the north. Enter keeps the value in [ ]; '-' as x removes the row.\n")
    new = []
    print("Boards:")
    for r in [r for r in rows if r["id"].isdigit()]:
        e = edit_row(r, True)
        if e:
            new.append(e)
    while True:
        i = ask("  add a board: its number (Enter: no more)", "")
        if not i:
            break
        if not i.isdigit() or any(r["id"] == i for r in new):
            print("    a number not used yet")
            continue
        e = edit_row(dict(id=i, x=None, y=None, facing="", label=""), True)
        if e:
            new.append(e)
    print("Landmarks (cars, doors, anything worth drawing):")
    for r in [r for r in rows if not r["id"].isdigit()]:
        e = edit_row(r, False)
        if e:
            new.append(e)
    while True:
        i = ask("  add a landmark: a short id such as C2 or G (Enter: no more)", "").replace(",", "")
        if not i:
            break
        if i.isdigit() or any(r["id"] == i for r in new):
            print("    an id that is not a number and not used yet")
            continue
        e = edit_row(dict(id=i, x=None, y=None, facing="", label=""), False)
        if e:
            new.append(e)
    ch = changes(rows, new)
    print()
    show(path, new)
    if not ch:
        print("\nno changes")
        return False
    print("\nchanges:\n  " + "\n  ".join(ch))
    if ask("save? Enter = yes, n = no", "").lower().startswith("n"):
        print("not saved")
        return False
    keep(path, quiet=True)                         # the version before
    stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    comments = [c for c in comments if not c.startswith("# edited ")] + [f"# edited {stamp} with tw_layout.py"]
    write(path, comments, new)
    keep(path, quiet=True)                         # and this one
    print(f"saved {path} (earlier versions in {backup_dir(path)})")
    return True


def restore(path):
    old = backups(path)
    if not old:
        print("no earlier versions yet")
        return False
    _, cur = read(path)
    print("earlier versions, newest first:")
    lst = list(reversed(old))
    for k, b in enumerate(lst, 1):
        _, rows = read(b)
        nb = sum(r["id"].isdigit() for r in rows)
        ch = changes(cur, rows)
        nl = len(rows) - nb
        print(f"  {k:2d}  {os.path.basename(b)}  {nb} board{'s' * (nb != 1)}, {nl} landmark{'s' * (nl != 1)}   "
              + ("same as now" if not ch else "differs: " + "; ".join(ch[:3]) + (" ..." if len(ch) > 3 else "")))
    v = ask("put one back: its number (Enter: none)", "")
    if not v.isdigit() or not 1 <= int(v) <= len(lst):
        print("nothing changed")
        return False
    keep(path, quiet=True)
    shutil.copy2(lst[int(v) - 1], path)
    keep(path, quiet=True)
    print(f"{path} is now {os.path.basename(lst[int(v) - 1])}")
    return True


def confirm(path):
    show(path)
    if not sys.stdin.isatty():
        return
    while True:
        print()
        v = ask("Is this layout still right? Enter = yes, e = edit, b = earlier versions", "").lower()
        if not v:
            return
        if v.startswith("e"):
            edit(path)
        elif v.startswith("b"):
            restore(path)
        else:
            continue
        print()
        show(path)


N_PATH = 2.5             # how fast the signal falls with distance indoors: -10 x n x log10(d)
OFF_DB = 8               # a link this far from what its length suggests is flagged
ONEWAY_DB = 6            # a link this much stronger one way than the other is flagged
LOSS_PCT = 20            # at rest, more loss than this is flagged


def read_quiet(folder, minutes, at=None):
    """per direction (tx, rx): rssi values, frame numbers, and the RSSI spread, from the
    last `minutes` of the recording (or from clock time `at`, HH:MM)"""
    import gzip
    import zlib
    files = sorted(glob.glob(os.path.join(folder, "csi-*.csv.gz")))
    if not files:
        sys.exit(f"no recording (csi-*.csv.gz) in {folder}")

    def lines(path):
        try:
            with gzip.open(path, "rt", errors="replace") as f:
                yield from f
        except (EOFError, gzip.BadGzipFile, zlib.error):
            pass                 # still being written, or cut short: read up to there
    t_from = None
    if at:
        h, m = (int(x) for x in at.split(":")[:2])
        files_at = [f for f in files if re.search(rf"-{h:02d}(\.part\d+)?\.csv\.gz$", f)]
        files = files_at or files
    else:
        files = files[-2:]        # the last minutes are in the newest file (or the one before)
    rec = {}
    for path in files:
        for line in lines(path):
            p = line.split(",", 12)
            if len(p) < 12 or p[1] != "TWR":
                continue
            try:
                t = int(p[0]) / 1e9
                rx, tx, txseq, rssi = int(p[2]), int(p[3]), int(p[4]), int(p[10])
            except ValueError:
                continue
            if at and t_from is None:
                d = dt.datetime.fromtimestamp(t).replace(hour=h, minute=m, second=0, microsecond=0)
                t_from = d.timestamp()
            rec.setdefault((tx, rx), []).append((t, txseq, rssi))
    if not rec:
        sys.exit(f"no tw_node records in {folder}")
    t_end = max(v[-1][0] for v in rec.values())
    lo, hi = (t_from, t_from + minutes * 60) if t_from else (t_end - minutes * 60, t_end)
    out = {}
    for d, v in rec.items():
        v = [x for x in v if lo <= x[0] < hi]
        if len(v) < 100:
            continue
        rs = sorted(x[2] for x in v)
        seqs = sorted({x[1] for x in v})
        span = seqs[-1] - seqs[0] + 1
        q1, q3 = rs[len(rs) // 4], rs[3 * len(rs) // 4]
        win = {}                  # loss per 10 s: steady at rest, jumps when something moves
        for x in v:
            win.setdefault(int((x[0] - lo) // 10), []).append(x[1])
        wl = [100 * (1 - len(set(s)) / (max(s) - min(s) + 1)) for s in win.values() if len(s) > 20]
        out[d] = dict(rssi=rs[len(rs) // 2], spread=q3 - q1, n=len(v), jumpy=(max(wl) - min(wl)) if wl else 0,
                      loss=100 * (1 - len(seqs) / span) if span > 0 else 0.0,
                      secs=v[-1][0] - v[0][0])
    return out, lo, hi


def fit(links, pos):
    """expected link RSSI from length: A - 10 n log10(d), A from the links themselves
    (median, so one odd link does not pull it); -> {pair: (length, expected)}"""
    import math
    L = {}
    for (a, b), r in links.items():
        if a in pos and b in pos:
            d = max(math.hypot(pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]), 1.0)
            L[(a, b)] = (d, r + 10 * N_PATH * math.log10(d))
    if not L:
        return {}
    A = sorted(v[1] for v in L.values())[len(L) // 2]
    return {k: (d, A - 10 * N_PATH * math.log10(d)) for k, (d, _) in L.items()}


def rms(links, exp):
    e = [links[k] - exp[k][1] for k in exp]
    return (sum(x * x for x in e) / len(e)) ** 0.5 if e else 0.0


def check(folder, minutes=2.0, at=None):
    """the layout against the radio, from a quiet stretch of a recording; -> the report lines"""
    import itertools
    folder = os.path.abspath(os.path.expanduser(folder))
    lay = os.path.join(folder, "boards.csv")
    if not os.path.exists(lay):
        lay = tw_map.layout_file()
    _, rows = read(lay)
    pos = {int(r["id"]): (r["x"], r["y"]) for r in rows if r["id"].isdigit()}
    dirs, lo, hi = read_quiet(folder, minutes, at)
    out = [f"Layout check: {lay}",
           f"recording {dt.datetime.fromtimestamp(lo):%H:%M:%S}-{dt.datetime.fromtimestamp(hi):%H:%M:%S} in {folder}",
           f"(each link's signal against its length in the layout; expected = the links' own level, "
           f"falling {10 * N_PATH:g} dB per tenfold distance)", ""]
    moved = [f"{a}->{b}" for (a, b), v in sorted(dirs.items()) if v["spread"] >= 3 or v["jumpy"] >= 15]
    pairs = sorted({tuple(sorted(d)) for d in dirs})
    links = {}
    for a, b in pairs:
        r = [dirs[d]["rssi"] for d in ((a, b), (b, a)) if d in dirs]
        links[(a, b)] = sum(r) / len(r)
    exp = fit(links, pos)
    out.append("link    length  RSSI a->b  b->a   expected  off by   loss a->b  b->a")
    flags = []
    for a, b in pairs:
        ab, ba = dirs.get((a, b)), dirs.get((b, a))
        fmt = lambda v, k: f"{v[k]:5.0f}" if v else "    -"
        if (a, b) in exp:
            d, e = exp[(a, b)]
            off = links[(a, b)] - e
            out.append(f"{a}-{b}   {d:6.1f} ft  {fmt(ab, 'rssi')}  {fmt(ba, 'rssi')}   {e:6.0f}   {off:+5.0f} dB"
                       f"   {fmt(ab, 'loss')}% {fmt(ba, 'loss')}%")
            if abs(off) >= OFF_DB:
                flags.append(f"link {a}-{b} is {abs(off):.0f} dB {'stronger' if off > 0 else 'weaker'} than its "
                             f"length ({d:.0f} ft) suggests: check where boards {a} and {b} are in the layout "
                             f"(or something solid stands in this path)")
        else:
            out.append(f"{a}-{b}   (a board missing from the layout)  {fmt(ab, 'rssi')}  {fmt(ba, 'rssi')}")
            flags.append(f"boards {a} and {b} talk, but {'they are' if a not in pos and b not in pos else 'one is'} "
                         f"not in the layout")
        for d, v in ((a, b), ab), ((b, a), ba):
            if v and v["loss"] >= LOSS_PCT and not moved:
                flags.append(f"{d[0]}->{d[1]} loses {v['loss']:.0f}% of frames at rest")
    # one way stronger than the other: said once per board when all its paths agree
    diff = {}                     # (a, b): how much better a is heard by b than a hears b
    for a, b in pairs:
        if (a, b) in dirs and (b, a) in dirs:
            diff[(a, b)] = dirs[(a, b)]["rssi"] - dirs[(b, a)]["rssi"]
            diff[(b, a)] = -diff[(a, b)]
    told = set()
    for x in sorted({b for d in diff for b in d}):
        mine = [diff[d] for d in diff if d[0] == x]
        if len(mine) >= 2 and (all(m >= ONEWAY_DB * .75 for m in mine) or all(m <= -ONEWAY_DB * .75 for m in mine)):
            m = sum(mine) / len(mine)
            side = ("its receiving side: its antenna, or noise from a computer or charger near it"
                    if m > 0 else "its sending side: its antenna, power setting or mounting")
            flags.append(f"board {x}: on all its {len(mine)} paths the others hear it about {abs(m):.0f} dB "
                         f"{'better' if m > 0 else 'worse'} than it hears them: {side}")
            told |= {tuple(sorted(d)) for d in diff if x in d}
    for a, b in pairs:
        if (a, b) in diff and (a, b) not in told and abs(diff[(a, b)]) >= ONEWAY_DB:
            weak = (a, b) if diff[(a, b)] < 0 else (b, a)
            flags.append(f"link {a}-{b}: {weak[0]}->{weak[1]} is {abs(diff[(a, b)]):.0f} dB weaker than "
                         f"{weak[1]}->{weak[0]} (one board's antenna, power or mounting)")
    # a board number swapped in the file: does another assignment fit much better?
    ids = sorted(pos)
    if 3 <= len(ids) <= 7 and len(exp) >= 3:
        base = rms(links, exp)
        best = None
        for perm in itertools.permutations(ids):
            if perm == tuple(ids):
                continue
            p2 = {i: pos[j] for i, j in zip(ids, perm)}
            r = rms(links, fit(links, p2))
            if best is None or r < best[0]:
                best = (r, perm)
        if best and base >= 5 and best[0] < 0.6 * base and base - best[0] >= 3:
            sw = [f"{i} where the layout has {j}" for i, j in zip(ids, best[1]) if i != j]
            flags.append(f"the signals fit much better ({best[0]:.0f} dB off instead of {base:.0f}) with board "
                         + ", ".join(sw) + ": are those numbers swapped in the layout?")
    out.append("")
    if moved:
        out.append("note: the signal moved during the check on " + ", ".join(moved)
                   + " (someone or something moving?); the numbers above may be off")
    out += (["looks right: every link's signal fits its length"] if not flags else ["to check:"] + ["  " + f for f in flags])
    return out


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    if cmd == "check":
        import argparse
        ap = argparse.ArgumentParser(prog="tw_layout.py check")
        ap.add_argument("cmd"); ap.add_argument("folder")
        ap.add_argument("--minutes", type=float, default=2.0, help="how long a quiet stretch (default 2)")
        ap.add_argument("--at", help="HH:MM: from this clock time (default: the last minutes recorded)")
        a = ap.parse_args()
        print("\n".join(check(a.folder, a.minutes, a.at)))
        return
    path = tw_map.layout_file()
    if cmd == "path":
        print(path)
    elif cmd == "show":
        show(path)
    elif cmd == "confirm":
        confirm(path)
        keep(path, quiet=True)
    elif cmd == "edit":
        edit(path)
    elif cmd == "backups":
        restore(path)
    elif cmd == "keep":
        keep(path)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
