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


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
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
