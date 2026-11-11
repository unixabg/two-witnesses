#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""flash_board.py - walk through setting up one board on the cable (flash_board.sh starts it).

    flash_board.sh            (PORT, VENV from the environment, as the other scripts)

Step by step, asking before each change:
  1. the kit: the firmware image's fingerprint, esptool, the port
  2. what is on the cable: a board already running tw_node (its number,
     group, firmware), or a new one
  3. flashing: a new board gets the whole image; a tw_node board, the program
     part only (keeps its number and settings), the whole image (a fresh
     start), or nothing
  4. its group (0 = the default) and its board number. Numbers are per group
     and must not repeat: write down which board got which, and label it
  5. its settings: it takes its group's within a few seconds (looking for the
     group if it is on another channel); the first board of a new group
     chooses the group's channel instead
  6. its LED (optional), a test blink so you can see which board it is
  7. tw_setid.py PORT check: two boards with one number, firmware, settings
Nothing is changed without a yes. Ctrl-C stops at any step.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

KIT = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(KIT, "tw_node-s3-ch11.bin")
APP_OFFSET = 0x20000                    # the program part (FLASH-README): the first 128 KB stay


def say(text=""):
    print(text, flush=True)


def ask(question, default=None, choices=None):
    """a question; Enter takes the default"""
    hint = f" [{default}]" if default not in (None, "") else ""
    while True:
        try:
            got = input(f"{question}{hint}: ").strip()
        except EOFError:
            sys.exit("\nstopped (no more answers)")
        got = got or ("" if default is None else str(default))
        if not choices or got.lower() in choices:
            return got.lower() if choices else got
        say(f"  please answer one of: {', '.join(choices)}")


def yes(question, default="y"):
    return ask(question + " (y/n)", default, ("y", "n", "yes", "no")).startswith("y")


def setid(port, *args, quiet=False):
    """run tw_setid.py PORT ARGS (its own checks and confirmations); -> (exit code, its output)"""
    cmd = [sys.executable, os.path.join(KIT, "tw_setid.py"), port, *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if not quiet and out:
        say("  " + out.replace("\n", "\n  "))
    return r.returncode, out


def hello(port, seconds=14):
    """the board's hello (TWH, every 10 s): dict, or None if it does not say one"""
    sys.path.insert(0, KIT)
    import tw_setid as T
    try:
        p = T.Port(port)
    except SystemExit:
        return None
    for line in p.lines(seconds):
        if line.startswith("TWH"):
            f = line.split(",")
            p.s.close()
            return dict(board=int(f[1]), fw=f[3], group=int(f[9]) if len(f) >= 10 and f[9].strip().isdigit() else 0,
                        line=line)
    p.s.close()
    return None


def image_ok():
    """the image's fingerprint against SHA256SUMS (the kit's)"""
    if not os.path.exists(IMAGE):
        sys.exit(f"no firmware image in the kit ({IMAGE})")
    want = None
    try:
        for line in open(os.path.join(KIT, "SHA256SUMS")):
            parts = line.split()
            if len(parts) == 2 and parts[1].lstrip("*") == os.path.basename(IMAGE):
                want = parts[0]
    except OSError:
        pass
    got = hashlib.sha256(open(IMAGE, "rb").read()).hexdigest()
    if want and got != want:
        sys.exit("the firmware image does not match SHA256SUMS: update the kit (git pull) and try again")
    return "matches SHA256SUMS" if want else "(not in SHA256SUMS: not checked)"


def esptool():
    """the esptool command: TW_ESPTOOL (tests), then the kit's Python environment, then the PATH"""
    if os.environ.get("TW_ESPTOOL"):
        return os.environ["TW_ESPTOOL"].split()
    venv = os.environ.get("VENV", os.path.expanduser("~/scratch-esp"))
    for c in (os.path.join(venv, "bin", "esptool"), os.path.join(venv, "bin", "esptool.py"), shutil.which("esptool"),
              shutil.which("esptool.py")):
        if c and os.path.exists(c):
            return [c]
    return [sys.executable, "-m", "esptool"]


def flash(port, whole):
    """write the whole image (0x0) or the program part (0x20000); retries on a yes"""
    path, offset, tmp = IMAGE, "0x0", None
    if not whole:
        tmp = tempfile.NamedTemporaryFile(prefix="tw_node-app-", suffix=".bin", delete=False)
        tmp.write(open(IMAGE, "rb").read()[APP_OFFSET:])
        tmp.close()
        path, offset = tmp.name, hex(APP_OFFSET)
    try:
        while True:
            say("")
            say("Put the board in download mode: hold BOOT, tap RST, let go of BOOT.")
            ask("Press Enter when done", "")
            cmd = esptool() + ["--chip", "esp32s3", "--port", port, "write-flash", offset, path]
            say(f"  writing {'the whole image' if whole else 'the program part'} ...")
            r = subprocess.run(cmd, capture_output=True, text=True)
            tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-3:])
            if r.returncode == 0:
                say("  written: " + (tail.splitlines()[-1] if tail else "ok"))
                say("Now tap RST (the board starts the new firmware).")
                ask("Press Enter when done", "")
                return True
            say("  esptool did not write it:\n    " + tail.replace("\n", "\n    "))
            say("  (usually: not in download mode, the wrong port, or another program using it)")
            if not yes("Try again?"):
                return False
    finally:
        if tmp:
            os.unlink(tmp.name)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PORT", "/dev/ttyACM0")
    if not sys.stdin.isatty():
        say("(answers are read from the input, not a terminal)")
    say(__doc__.split("\n\n")[0].split(" - ")[1].split(" (")[0].capitalize() + f": the board on {port}.")
    say("")
    # 1. the kit
    say(f"1. The kit: firmware image {image_ok()}; esptool: {' '.join(esptool())}.")
    if not os.path.exists(port):
        sys.exit(f"   no {port}: plug the board's native USB port in (or set PORT=...)")
    # 2. what is there
    say(f"2. Listening for a tw_node board on {port} (up to 14 s) ...")
    h = hello(port)
    if h:
        say(f"   found: board {h['board']}{' (no number yet)' if h['board'] == 16 else ''}, group {h['group']}, "
            f"firmware {h['fw']}")
        say("3. Flashing: [p] the program part only (keeps its number and settings),")
        say("              [w] the whole image (a fresh start: number and settings cleared), [n] no flashing")
        how = ask("   Which", "p", ("p", "w", "n"))
    else:
        say("   no tw_node answered: a new board (or other firmware). It gets the whole image.")
        how = "w" if yes("3. Write the whole image?") else "n"
    if how != "n":
        if not flash(port, how == "w"):
            sys.exit("not flashed; nothing else changed")
        say("   waiting for its hello ...")
        h = hello(port, 20)
        if not h:
            sys.exit("no hello after flashing: tap RST once more, then run flash_board.sh again (choose n)")
        say(f"   it runs {h['fw']}: board {h['board']}, group {h['group']}")
    if not h:
        sys.exit("no tw_node board to set up")
    # 4. group, then number (a group change restarts it, and so does a number)
    say("4. Its group: 0 is the default, and every board so far. Another group only for a second array nearby.")
    g = ask("   Group", h["group"])
    if not g.isdigit() or not 0 <= int(g) <= 255:
        sys.exit("a group is 0..255")
    if int(g) != h["group"]:
        code, _ = setid(port, "group", g)
        if code:
            sys.exit("the group was not set; see above")
    say("   Its board number, 1-15 (16 means none). Numbers must not repeat within a group: write down which")
    say("   board gets which, and put a label on it.")
    cur = h["board"]
    n = ask("   Board number", cur if cur != 16 else None)
    if not n.isdigit() or not 1 <= int(n) <= 15:
        sys.exit("a board number is 1..15")
    if int(n) != cur:
        code, _ = setid(port, "board", n)
        if code:
            sys.exit("the number was not set; see above")
    # 5. settings
    say("5. Its settings: a board takes its group's within a few seconds (if the group is on another channel,")
    say("   it looks for it: up to about a minute). Listening ...")
    others = False
    for _ in range(4):
        code, out = setid(port, quiet=True)
        others = any(line.startswith("  board ") and "(on the cable)" not in line for line in out.splitlines())
        if others:
            break
        time.sleep(2)
    say("  " + "\n  ".join(line for line in out.splitlines() if line.startswith("  board") or "every board" in line))
    if not others:
        say("   no other board of its group heard.")
        if yes("   Is this the first board of a new group (choose the group's channel)?", "n"):
            say("   A survey first shows which channels are busy (about a minute): tw_setid.py PORT survey.")
            if yes("   Run the survey now?", "y"):
                setid(port, "survey")
            ch = ask("   The group's channel (1-11)", 11)
            setid(port, "channel", ch)
        else:
            say("   Then the others are off, out of reach, or on another channel: it keeps looking for them by")
            say("   itself. Check later with: tw_setid.py PORT check")
    # 6. LED
    say("6. Its LED shows movement it measures (off when quiet; tw_node 3.4: a white flash is a detection).")
    if yes("   Turn its LED on (GPIO 48, the usual on these boards)?", "y"):
        setid(port, "led", "on", "48")
        setid(port, "led", "test")
        say("   it blinks for 7 s: this is the board you just set up")
    # 7. check
    say("7. The health check (30 s):")
    code, _ = setid(port, "check")
    say("")
    say(f"Done: board {n}, group {g}, firmware {h['fw']}. Unplug it, place it, and write its place in your layout")
    say("file (and on its label). Run tw_setid.py PORT check again once every board is in place.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit("\nstopped")
