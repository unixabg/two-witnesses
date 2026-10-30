#!/bin/bash
# SPDX-FileCopyrightText: 2026 Richard Nelson
# SPDX-License-Identifier: Apache-2.0
# setup_once.sh - run once on the persistent USB (Debian). Safe to run again.
#
#   tar xzf two-witnesses-kit-v4.tar.gz && cd two-witnesses-kit && ./setup_once.sh
#
# It: installs chrony ffmpeg v4l-utils tmux rsync python3-venv; sets the
# timezone (America/Chicago unless TZ_WANT says otherwise) and starts chrony;
# makes the venv ~/scratch-esp with esptool 5.6.0, numpy, matplotlib; adds
# you to the dialout group (serial ports without sudo, after logging in
# again); lets run.sh block and unblock sleep without a password (one exact
# systemctl command, in /etc/sudoers.d/tw-sleep, so the block undoes itself
# when the run ends); copies the kit to ~/tw-kit after checking its
# fingerprints; makes ~/tw-data and checks that it really lands on the USB,
# not in RAM.
set -u
TZ_WANT=${TZ_WANT:-America/Chicago}
SRC=$(cd "$(dirname "$0")" && pwd)
KIT=$HOME/tw-kit; DATA=$HOME/tw-data; VENV=$HOME/scratch-esp
ok() { echo "  ok: $*"; }
bad() { echo "  PROBLEM: $*"; FAIL=1; }
FAIL=0

echo "1. fingerprints of the kit"
(cd "$SRC" && sha256sum -c --quiet SHA256SUMS) && ok "all files match SHA256SUMS" || bad "a file does not match SHA256SUMS"

echo "2. packages"
sudo apt-get update -qq && sudo apt-get install -y -qq chrony ffmpeg v4l-utils tmux rsync python3-venv >/dev/null \
  && ok "chrony ffmpeg v4l-utils tmux rsync python3-venv" || bad "apt-get install failed"

echo "3. clock"
sudo timedatectl set-timezone "$TZ_WANT" 2>/dev/null || sudo ln -sf "/usr/share/zoneinfo/$TZ_WANT" /etc/localtime
sudo systemctl enable --now chrony >/dev/null 2>&1
sleep 5; sudo chronyc makestep >/dev/null 2>&1
if chronyc tracking 2>/dev/null | grep -q "Leap status *: Normal"; then ok "chrony synchronised, $(date '+%Z %H:%M:%S')"
else echo "  not yet synchronised (normal for the first minute); check later: chronyc tracking"; fi

echo "4. python venv $VENV"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install -q "esptool==5.6.0" numpy matplotlib pillow && \
  "$VENV/bin/python" -c "import serial, numpy, matplotlib, PIL, esptool" && ok "esptool 5.6.0, numpy, matplotlib, pillow, pyserial" \
  || bad "venv install failed"
# for radar_replay.py --emulate / --check only (Espressif's compiled library under emulation)
"$VENV/bin/pip" install -q unicorn pyelftools && ok "unicorn, pyelftools (radar_replay --check)" \
  || echo "  note: unicorn not installed; radar_replay.py still works, only --emulate/--check need it"

echo "5. serial port permission"
if id -nG | grep -qw dialout; then ok "already in dialout"
else sudo usermod -aG dialout "$USER" && ok "added to dialout: log out and in (or reboot) for it to count"; fi

echo "6. sleep block without a password (so run.sh can undo it when the run ends)"
SYSTEMCTL=$(command -v systemctl || echo /usr/bin/systemctl)
UNITS="sleep.target suspend.target hibernate.target hybrid-sleep.target"
RULE=$(mktemp)
{
  echo "# written by two-witnesses setup_once.sh: run.sh may block and unblock sleep, nothing else"
  for c in "$SYSTEMCTL" /usr/bin/systemctl /bin/systemctl; do
    echo "$USER ALL=(root) NOPASSWD: $c mask $UNITS, $c unmask $UNITS"
  done | sort -u
} > "$RULE"
if sudo visudo -cf "$RULE" >/dev/null 2>&1 && sudo install -m 0440 -o root -g root "$RULE" /etc/sudoers.d/tw-sleep; then
  if sudo -n "$SYSTEMCTL" unmask $UNITS 2>/dev/null; then ok "sudo rule in place (/etc/sudoers.d/tw-sleep)"
  else bad "the sudo rule was written but does not work; run.sh will ask for a password instead"; fi
else
  bad "could not write the sudo rule; run.sh will ask for a password and ask you to unmask at the end"
fi
rm -f "$RULE"

echo "7. kit -> $KIT"
mkdir -p "$KIT"
if [ "$SRC" != "$KIT" ]; then cp -a "$SRC"/. "$KIT"/; fi
chmod +x "$KIT"/*.sh "$KIT"/*.py
ok "$(ls "$KIT" | wc -l) files"

echo "8. data folder $DATA"
mkdir -p "$DATA/runs" "$DATA/short" "$DATA/watch"
if [ ! -e "$DATA/boards.csv" ] && [ -r "$KIT/boards-example.csv" ]; then
  cp "$KIT/boards-example.csv" "$DATA/boards.csv"; ok "board layout: $DATA/boards.csv (edit it when a board moves)"
fi
"$VENV/bin/python" -c "import sys; sys.path.insert(0,'$KIT'); import listen as L; r,w=L.ram_check('$DATA'); print('  ' + w); sys.exit(1 if r else 0)" \
  && ok "on real storage" || bad "$DATA is in RAM: persistence is not on, files would vanish at shutdown"
df -h --output=avail "$DATA" | tail -1 | sed 's/^ */  free space: /'

echo "9. devices now"
S=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null); echo "  serial: ${S:-none plugged in}" | tr '\n' ' '; echo
C=$(ls /dev/video* 2>/dev/null | head -2); echo "  camera: ${C:-none}" | tr '\n' ' '; echo

echo
[ $FAIL = 0 ] && echo "SETUP DONE." || echo "SETUP FINISHED WITH PROBLEMS (see above)."
echo "A long run:  $KIT/run.sh 07:00        A short test:  $KIT/short_run.sh NAME SECONDS"
