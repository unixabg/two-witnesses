#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
"""tw_update.py - updates over the air (tw_node 4.0), for tw_setid.py:

    tw_setid.py PORT update-key [show|none]     your update key: made once (private/update-key.pem),
                                                its public half given to the board on the cable
    tw_setid.py PORT update [board N|all] [FILE]   sign the program (the kit's image unless FILE) and
                                                have the board on the cable send it to its group

The key: a P-256 key pair kept in the kit's private/ folder (git ignores it). Each board learns
the public half over its cable (update-key, or flash_board.sh) and takes only programs signed with
the private half: a board with no key takes no update. Keep a copy of private/update-key.pem
somewhere safe: without it, the boards that know its public half take no update until each is
given a new key over its cable.

The update: the program (the part of the image from 0x20000, or a program file) is signed here,
given to the board on the cable over USB (into its PSRAM), checked there, and sent by radio to its
group (docs/PROTOCOL-3.md, "Updates over the air"); each board checks it again, writes it to its
other program slot and restarts into it, the board on the cable last. A new program is kept once
it has run a minute and hears its group; otherwise the board goes back to the old one by itself
within 10 minutes. Boards on older firmware take no part: give them tw_node 4.0 once by cable
(flash_board.sh).
"""
import base64
import hashlib
import os
import re
import sys
import time

KIT = os.path.dirname(os.path.abspath(__file__))
KEY = os.path.join(KIT, "private", "update-key.pem")
IMAGE = os.path.join(KIT, "tw_node-s3-ch11.bin")
APP_OFFSET = 0x20000
CHUNK = 1280                        # tw_proto.h TW_UP_CHUNK
SLOT = 0x180000                     # TW_UP_MAX
STATES = {0: "-", 1: "receiving", 2: "checking", 3: "writing", 4: "restarting into it", 5: "sending",
          6: "checked", 10: "no memory (PSRAM)", 11: "no update key: give it one over its cable (update-key)",
          12: "the program arrived damaged", 13: "not signed with the key it knows", 14: "not a tw_node program",
          15: "writing it to flash failed", 16: "took no part"}


def crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        sys.exit("updates need the Python package cryptography: ~/scratch-esp/bin/pip install cryptography")
    return hashes, serialization, ec, utils


def load_key(make=False):
    """the private key (made and saved if make and there is none); None if none"""
    hashes, serialization, ec, utils = crypto()
    if os.path.exists(KEY):
        with open(KEY, "rb") as f:
            return serialization.load_pem_private_key(f.read(), password=None)
    if not make:
        return None
    key = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(KEY), exist_ok=True)
    fd = os.open(KEY, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    print(f"made a new update key: {KEY}\n  keep a copy somewhere safe: boards that know it take updates "
          "signed with it only")
    return key


def public_bytes(key):
    """the public half as the boards take it: 65 bytes, 04 x y"""
    hashes, serialization, ec, utils = crypto()
    return key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def fingerprint(pub):
    return hashlib.sha256(pub).hexdigest()[:8]


def sign(key, data):
    """the manifest: SHA-256 of data, then the signature over it (r, s: 32 bytes each)"""
    hashes, serialization, ec, utils = crypto()
    d = hashlib.sha256(data).digest()
    r, s = utils.decode_dss_signature(key.sign(d, ec.ECDSA(utils.Prehashed(hashes.SHA256()))))
    return d + r.to_bytes(32, "big") + s.to_bytes(32, "big")


def program(path):
    """the program to send, from the kit's image (the part from 0x20000) or a program file;
    -> (bytes, 'tw_node 4.0 (built ...)' or None)"""
    data = open(path, "rb").read()
    if len(data) > APP_OFFSET and data[0] == 0xE9 and data[APP_OFFSET] == 0xE9:
        data = data[APP_OFFSET:]             # a whole image: its program part
    if not data or data[0] != 0xE9:
        sys.exit(f"{path}: not an ESP32 program or a tw_node image")
    if len(data) > SLOT:
        sys.exit(f"{path}: {len(data):,} bytes; a program slot holds {SLOT:,}")
    desc = data[32:32 + 256]                 # esp_app_desc_t after the image and segment headers
    if desc[:4] != (0xABCD5432).to_bytes(4, "little") or desc[48:56] != b"tw_node\0":
        sys.exit(f"{path}: not a tw_node program")
    m = re.search(rb"(tw_node \d+\.\d+(?:\.\d+)?)\|([A-Z][a-z]{2} [ \d]\d \d{4} \d\d:\d\d:\d\d)", data)
    return data, (f"{m[1].decode()} (built {m[2].decode()})" if m else None)


def answer(p, cmd, want, seconds=3, tries=4):
    """send cmd until a line starting with want (or TWUP err / TWERR) comes; -> that line or None"""
    for _ in range(tries):
        p.send(cmd)
        for line in p.lines(seconds):
            if line.startswith(want) or line.startswith(("TWUP err", "TWERR")):
                return line
    return None


def key_command(p, arg):
    """update-key [show|none]"""
    if arg == "none":
        line = answer(p, "TWSET upkey none", "TWOK upkey")
        if not line:
            sys.exit("no answer: is it running tw_node 4.0?")
        print("the board on the cable has no update key now: it takes no update")
        return
    key = load_key(make=arg is None)
    line = answer(p, "TWSET upkey", "TWOK upkey")
    if not line:
        sys.exit("no answer: updates need tw_node 4.0 on the board on the cable (flash_board.sh)")
    board_fp = line.split()[-1]
    mine = fingerprint(public_bytes(key)) if key else None
    if arg == "show":
        print(f"your update key: {mine or 'none yet (tw_setid.py PORT update-key makes one)'}")
        print(f"the board on the cable: {board_fp}" + (" (yours)" if board_fp == mine else
                                                        "" if board_fp == "none" else " (not yours)"))
        return
    if board_fp == mine:
        print(f"the board on the cable already has your update key ({mine})")
        return
    line = answer(p, f"TWSET upkey {public_bytes(key).hex()}", "TWOK upkey")
    if not line or not line.startswith("TWOK"):
        sys.exit(f"not taken: {line or 'no answer'}")
    got = line.split()[-1]
    if got != mine:
        sys.exit(f"the board says {got}, your key is {mine}: try again")
    print(f"done: the board on the cable takes updates signed with your key ({mine})"
          + (f"; it had {board_fp} before" if board_fp != "none" else ""))


def upload(p, data, man):
    """the program and its manifest to the board on the cable; -> its 'TWUP verified' line"""
    line = answer(p, f"TWUP begin {len(data)}", "TWUP ready", tries=2)
    if not line:
        sys.exit("no answer: updates need tw_node 4.0 on the board on the cable (flash_board.sh)")
    if not line.startswith("TWUP ready"):
        sys.exit(f"the board on the cable: {line}")
    n = (len(data) + CHUNK - 1) // CHUNK
    t0, shown, nxt, done = time.time(), 0, 0, 0
    pending, tries, buf = {}, {}, b""        # a few chunks on the way at once; each answered by number

    def put(i):
        p.send(f"TWUP d {i} {base64.b64encode(data[i * CHUNK:(i + 1) * CHUNK]).decode()}")
        pending[i] = time.time()
        tries[i] = tries.get(i, 0) + 1
        if tries[i] > 5:
            sys.exit(f"chunk {i} of {n}: no answer after 5 tries")
    while done < n:
        while nxt < n and len(pending) < 8:
            put(nxt)
            nxt += 1
        buf += p.s.read(p.s.in_waiting or 1)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            m = re.match(r"TWUP (ok|err chunk) (\d+)$", line)
            if m and int(m[2]) in pending:
                i = int(m[2])
                del pending[i]
                if m[1] == "ok":
                    done += 1
                else:
                    put(i)
            elif line.startswith("TWUP err"):
                sys.exit(f"the board on the cable: {line}")
        for i, t in list(pending.items()):
            if time.time() - t > 3:
                put(i)
        pct = done * 100 // n
        if pct >= shown + 20:
            shown = pct - pct % 20
            print(f"  to the board on the cable: {pct}% ({time.time() - t0:.0f} s)")
    line = answer(p, f"TWUP man {man.hex()}", "TWUP ok man")
    if not line or not line.startswith("TWUP ok"):
        sys.exit(f"the manifest: {line or 'no answer'}")
    line = answer(p, "TWUP check", "TWUP verified", seconds=6, tries=2)
    if not line or not line.startswith("TWUP verified"):
        sys.exit(f"the board on the cable did not accept it: {line or 'no answer'}")
    return line


def update(p, status, target, path):
    """sign, give to the board on the cable, send to target (None: itself; 0: every board of its
    group; N: board N), and follow it until each target runs the new program or says why not"""
    key = load_key()
    if not key:
        sys.exit("no update key yet: tw_setid.py PORT update-key (makes one and gives it to the board on the "
                 "cable; each other board learns it over its own cable once, or with flash_board.sh)")
    data, new_fw = program(path or IMAGE)
    man = sign(key, data)
    uid = man[:4].hex()
    print(f"program: {os.path.basename(path or IMAGE)}, {len(data):,} bytes, {new_fw or 'tw_node (version not found)'}, "
          f"update {uid}, signed with key {fingerprint(public_bytes(key))}")
    print("listening for the boards (11 s) ...")
    st = status(p, 11)
    cable = next((b for b, v in st.items() if v.get("cable")), None)
    if cable is None:
        sys.exit("no board on the cable answered: is tw_node running (and nothing else using the port)?")
    targets = [cable] if target is None else sorted(st) if target == 0 else [target]
    if target and target not in st:
        print(f"  board {target} is not heard now: it gets it only if it comes back while it is sent")

    def vers(b):
        fw = st.get(b, {}).get("fw") or ""
        m = re.match(r"tw_node (\d+)\.(\d+)", fw)
        return (int(m[1]), int(m[2])) if m else None
    old = [b for b in targets if b in st and (vers(b) or (0, 0)) < (4, 0)]
    if cable in old:
        sys.exit(f"the board on the cable runs {st[cable].get('fw')}: updates need tw_node 4.0 on it "
                 "(flash_board.sh, once)")
    if old:
        print(f"  board{'s' if len(old) > 1 else ''} {', '.join(map(str, old))} run{'' if len(old) > 1 else 's'} older firmware: "
              "no part in updates until given tw_node 4.0 once by cable (flash_board.sh)")
    same = [b for b in targets if b in st and new_fw and st[b].get("fw") == new_fw]
    if same and len(same) == len(targets):
        print(f"every board asked for runs {new_fw} already: nothing to do")
        return 0
    print(f"giving it to the board on the cable (board {cable}) ...")
    line = upload(p, data, man)
    print(f"  {line}")
    who = "every board of its group" if target == 0 else f"board {target}" if target else "itself"
    line = answer(p, f"TWUP send {0 if target == 0 else target or cable}", "TWUP sending", tries=2)
    if not line or not line.startswith("TWUP sending"):
        sys.exit(f"not sent: {line or 'no answer'}")
    print(f"board {cable} sends it to {who}: a few seconds to a minute; then each checks it, writes it and "
          "restarts (the board on the cable last)")
    return follow(p, uid, targets, cable, new_fw, old)


def follow(p, uid, targets, cable, new_fw, old):
    """watch TWP (each board's part), TWS (the sender), TWV (what each runs) until done"""
    rep, runs, ready_at = {}, {}, {}
    sending, t0, last_show = None, time.time(), 0
    end = t0 + 600
    restarted = False
    while time.time() < end:
        for line in p.lines(1):
            f = line.split(",")
            try:
                if f[0] == "TWP" and len(f) >= 9 and f[4] == uid:
                    b, s = int(f[1]), int(f[5])
                    rep[b] = (s, int(f[6]), int(f[7]))
                    if s == 4 and b not in ready_at:
                        ready_at[b] = time.time()
                elif f[0] == "TWS" and len(f) >= 44 and int(f[1]) == cable:
                    sending = int(f[40])
                    if int(f[39]) == 4 and cable not in ready_at:
                        ready_at[cable] = time.time()
                elif f[0] == "TWV" and len(f) >= 6:
                    runs[int(f[1])] = f"{f[4]} (built {f[5]})"
                elif line.startswith("TWUP restarting"):
                    restarted = True
                    ready_at.setdefault(cable, time.time())
            except ValueError:
                continue
        now = time.time()
        if now - last_show >= 5:
            last_show = now
            parts = []
            for b in targets:
                s = rep.get(b)
                if new_fw and runs.get(b) == new_fw and b in ready_at:
                    parts.append(f"{b} runs it")
                elif s:
                    parts.append(f"{b} {STATES.get(s[0], s[0])}" + (f" {s[1]}/{s[2]}" if s[0] == 1 else ""))
                elif b in old:
                    parts.append(f"{b} older firmware")
                else:
                    parts.append(f"{b} -")
            print(f"  {now - t0:3.0f} s: " + ("sending; " if sending else "") + ", ".join(parts))
        done = True
        for b in targets:
            s = (rep.get(b) or (0,))[0]
            if b in old or s >= 10:
                continue
            if new_fw and runs.get(b) == new_fw and b in ready_at:
                continue
            if b == cable and not restarted and s != 4 and not sending and sending is not None and \
                    (cable not in ready_at) and now - t0 > 60 and not rep.get(cable):
                continue
            done = False
        if done and now - t0 > 15:
            break
    print()
    bad = 0
    for b in targets:
        s = (rep.get(b) or (0,))[0]
        if new_fw and runs.get(b) == new_fw and b in ready_at:
            print(f"  board {b}: runs {new_fw}; it keeps it once it has run a minute and hears its group "
                  "(otherwise it goes back by itself within 10 minutes)")
        elif b in old:
            print(f"  board {b}: older firmware, no part: give it tw_node 4.0 once by cable (flash_board.sh)")
        elif s >= 10:
            bad += 1
            print(f"  board {b}: {STATES.get(s, s)}")
        elif b in ready_at:
            bad += 1
            print(f"  board {b}: wrote it and restarted, but has not said it runs it yet: check with tw_setid.py "
                  "PORT in a minute")
        else:
            bad += 1
            print(f"  board {b}: did not get it ({STATES.get(s, 'not heard')}): off or out of reach? Run the update "
                  "again when it is back (boards that have it say so and take it no more)")
    return 1 if bad else 0
