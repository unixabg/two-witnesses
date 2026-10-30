#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_map.py - draw the boards and the lines between them, to scale, in text.

Used by dash.py (the map view, key g) and links_report.py (a small map for each
moment in across.txt). Run it alone to check a boards.csv:

    ~/scratch-esp/bin/python tw_map.py ~/tw-data/boards.csv

Where: the kit's private/ folder (private/boards.csv, kept out of git), or
~/tw-data/boards.csv. Each run copies the one it used into its folder.

boards.csv: one row per board, and optionally landmarks:
    id,x_ft,y_ft,facing,label
    1,0,-15,N,cushion
    2,0,15,S,metal shelf
    3,-15,-2,E,metal cabinet
    C,-8,15,,metal cabinet
x grows to the east, y to the north (any unit, as long as it is the same for
both). facing: N, S, E, W (or blank). A row whose id is not a number is a
landmark, drawn with the first letter of its id.

Each line between two boards is drawn with a character for how much it is
stirring now: "." quiet, "-" a little, "=" moving, "#" strongly, "?" no data.
"""
import csv
import math
import os
import sys

LEVEL_CHARS = ".-=#?"           # 0 quiet .. 3 strongly; 4 = no data on that line
FACING = {"N": "^", "S": "v", "E": ">", "W": "<"}


def find_boards(*folders):
    """the first boards.csv found in these folders (a run keeps a copy of its layout),
    then in the kit's private/ folder (ignored by git: the owner's own layout), then
    in TW_DATA (~/tw-data)"""
    data = os.environ.get("TW_DATA", os.path.expanduser("~/tw-data"))
    private = os.path.join(os.path.dirname(os.path.abspath(__file__)), "private")
    for d in [f for f in folders if f] + [private, data]:
        p = os.path.join(d, "boards.csv")
        if os.path.exists(p):
            return p
    return None


def load_boards(path):
    """-> (boards {id: (x, y, facing, label)}, landmarks [(letter, x, y, label)])"""
    boards, marks = {}, []
    if not path or not os.path.exists(path):
        return boards, marks
    with open(path, newline="") as fh:
        rows = [r for r in csv.reader(fh) if r and not r[0].strip().startswith("#")]
    for r in rows:
        r = [c.strip() for c in r] + ["", "", "", "", ""]
        if r[0].lower() == "id":
            continue
        try:
            x, y = float(r[1]), float(r[2])
        except ValueError:
            continue
        if r[0].isdigit():
            boards[int(r[0])] = (x, y, r[3].upper()[:1], r[4])
        elif r[0]:
            marks.append((r[0][0].upper(), x, y, r[4] or r[0]))
    return boards, marks


def pair(a, b):
    return (min(a, b), max(a, b))


def render(boards, levels, marks=(), width=60, height=20):
    """Text map. levels: {(a, b) with a < b: 0..3}; pairs not given are drawn quiet.
    Returns a list of strings (height rows), north at the top. A terminal cell is about
    twice as tall as it is wide, so one row stands for two columns' worth of distance."""
    if not boards:
        return []
    pts = [(x, y) for x, y, _f, _l in boards.values()] + [(x, y) for _c, x, y, _l in marks]
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
    dx, dy = max(x1 - x0, 1e-6), max(y1 - y0, 1e-6)
    inner_w, inner_h = width - 5, height - 2               # room for labels at the edges
    scale = min(inner_w / dx, 2 * inner_h / dy)            # columns per unit
    ox = 2                                                 # left-aligned
    oy = 1 + (inner_h - dy * scale / 2) / 2

    def cell(x, y):
        return (int(round(oy + (y1 - y) * scale / 2)), int(round(ox + (x - x0) * scale)))

    grid = [[" "] * width for _ in range(height)]
    lvl = [[-1] * width for _ in range(height)]
    ids = sorted(boards)
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            L = levels.get(pair(a, b), 0)
            (r0, c0), (r1, c1) = cell(*boards[a][:2]), cell(*boards[b][:2])
            n = max(abs(r1 - r0), abs(c1 - c0), 1)
            for k in range(1, n):
                r = int(round(r0 + (r1 - r0) * k / n))
                c = int(round(c0 + (c1 - c0) * k / n))
                if 0 <= r < height and 0 <= c < width and L >= lvl[r][c]:
                    grid[r][c] = LEVEL_CHARS[L]
                    lvl[r][c] = L
    for ch, x, y, _lab in marks:
        r, c = cell(x, y)
        if 0 <= r < height and 0 <= c < width:
            grid[r][c] = ch
    for bid, (x, y, facing, _lab) in boards.items():
        r, c = cell(x, y)
        txt = str(bid) + FACING.get(facing, "")
        for k, ch in enumerate(txt):
            if 0 <= r < height and 0 <= c + k < width:
                grid[r][c + k] = ch
    return ["".join(row).rstrip() for row in grid]


def legend(boards, marks):
    parts = [f"{b}{FACING.get(f, '')} {lab}".strip() for b, (_x, _y, f, lab) in sorted(boards.items())]
    parts += [f"{c} {lab}" for c, _x, _y, lab in marks]
    return ("lines: . quiet  - a little  = moving  # strongly  ? no data     "
            + "   ".join(parts) + "   (north up)")


def distances(boards):
    ids = sorted(boards)
    out = []
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            (xa, ya), (xb, yb) = boards[a][:2], boards[b][:2]
            out.append((a, b, math.hypot(xa - xb, ya - yb)))
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else find_boards()
    boards, marks = load_boards(path)
    if not boards:
        sys.exit(f"no boards in {path or 'boards.csv (none found)'}")
    print(f"{path}: {len(boards)} boards, {len(marks)} landmarks")
    for line in render(boards, {}, marks, 60, 22):
        print("  " + line)
    print("  " + legend(boards, marks))
    for a, b, d in distances(boards):
        print(f"  {a}-{b}: {d:.1f}")


if __name__ == "__main__":
    main()
