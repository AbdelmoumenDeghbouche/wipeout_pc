#!/bin/bash
# Experiment run on the Steam Deck: frame stats every 5 s (WIPEOUT_FRAME_STATS), GPU busy and the
# per-thread CPU every 6 s, live switch files (WIPEOUT_TOGGLE_FILE, WIPEOUT_PRESET_FILE,
# WIPEOUT_SCENE_DEBUG), plus launch-time variables given as arguments:
#   ~/diag/exp-run.sh WIPEOUT_PREP_WORKERS=0      draw-preparation workers off
#   ~/diag/exp-run.sh WIPEOUT_UPSCALER=off        any other launch variable
# Start it as a user service so it outlives the ssh session:
#   systemd-run --user --collect --unit wipeout-diag -E DISPLAY=:0 \
#       -E XAUTHORITY=$(ls /run/user/1000/xauth_* | head -1) \
#       bash -c "$HOME/diag/exp-run.sh WIPEOUT_PREP_WORKERS=0 > $HOME/diag/exp.out 2>&1"
# Output: ~/diag/runs/<stamp>/{game.log,threads.txt,env.txt}. BUILD is the extracted AppImage.
set -u
BUILD="${BUILD:-$HOME/Games/Wipeout Omega/deckperf/squashfs-root}"
DATA="$HOME/.local/share/wipeout"
RUN="$HOME/diag/runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
export WIPEOUT_FRAME_STATS=1 WIPEOUT_GPU_LOG=info WIPEOUT_STALL_MS=25 WIPEOUT_SCREENSHOT_EVERY=5
export WIPEOUT_TOGGLE_FILE="$HOME/diag/toggles.txt" WIPEOUT_PRESET_FILE="$HOME/diag/preset.txt"
export WIPEOUT_SCENE_DEBUG="$HOME/diag/scene.trigger"
for arg in "$@"; do
    case "$arg" in
        WIPEOUT_*=*) export "$arg" ;;
        *) echo "unknown argument: $arg (expected WIPEOUT_NAME=value)" >&2; exit 2 ;;
    esac
done
echo 0 > "$WIPEOUT_TOGGLE_FILE"
rm -f "$WIPEOUT_PRESET_FILE" "$WIPEOUT_SCENE_DEBUG"
env | grep '^WIPEOUT_' | sort > "$RUN/env.txt"
(
  sleep 15
  while true; do
    PID=$(pgrep -x wipeout-probe | head -1)
    if [ -n "$PID" ]; then
      echo "=== $(date +%H:%M:%S) load $(cut -d' ' -f1 /proc/loadavg) gpu_busy $(cat /sys/class/drm/card*/device/gpu_busy_percent 2>/dev/null | head -1)% sclk $(grep '\*' /sys/class/drm/card*/device/pp_dpm_sclk | head -1 | awk '{print $2}') toggles $(cat "$WIPEOUT_TOGGLE_FILE") preset $(cat "$WIPEOUT_PRESET_FILE" 2>/dev/null)"
      top -b -d 3 -n 2 -H -p "$PID" 2>/dev/null | awk '/^top/{n++} n==2 && NR>7 && $9+0 > 2.0 {printf "%6s %5s %s\n", $1, $9, $NF}' | sort -k2 -rn | head -16
    fi
    sleep 3
  done
) > "$RUN/threads.txt" 2>&1 &
SAMPLER=$!
trap 'kill $SAMPLER 2>/dev/null' EXIT
echo "run: $RUN"
cd "$BUILD" || exit 1
PYTHONUNBUFFERED=1 ./AppRun --play 2>&1 | python3 -I -u "$HOME/diag/stamp.py" "$RUN/game.log" > /dev/null
cp "$DATA/user/last_run.log" "$RUN/last_run.log" 2>/dev/null
echo "done: $RUN"
