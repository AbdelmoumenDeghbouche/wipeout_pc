#!/bin/bash
# CPU benchmark on the Steam Deck: N launches of build A and build B, alternating, each replaying
# a pad route from boot into a Vineta K race and photo mode with GOW3_FRAME_LOG on.
#   bench-run.sh <runs per build> <A spec> <B spec> [route]
#   spec: <extracted AppImage dir>[:GOW3_X=y[:...]]   route: default ~/diag/routes/vinetak-photo.pad
# Summarise with: python3 -I ~/diag/bench-summary.py <printed directory>
set -u
N=${1:?runs per build}; SPEC_A=${2:?A spec}; SPEC_B=${3:?B spec}
ROUTE=${4:-$HOME/diag/routes/vinetak-photo.pad}
DIAG=$HOME/diag; SHOTS=$HOME/.local/share/gow3/user/screenshots
OUT=$DIAG/bench/$(date +%Y%m%d-%H%M%S); mkdir -p "$OUT"
cp "$ROUTE" "$ROUTE.marks" "$OUT/" || exit 1
export DISPLAY=:0 XAUTHORITY=$(ls /run/user/1000/xauth_* | head -1)
pgrep -x gow3-probe >/dev/null && { echo "the game is running" >&2; exit 1; }
run_one() { # <run dir> <spec>
    local dir=$1 build=${2%%:*} envs=() e
    [ "$2" != "$build" ] && IFS=: read -ra envs <<<"${2#*:}"
    mkdir -p "$dir"
    echo replay > "$DIAG/pad.ctl"
    local args=(-E DISPLAY -E XAUTHORITY -E GOW3_FRAME_STATS=1 -E GOW3_FRAME_LOG="$dir/frames.log"
                -E GOW3_PAD_FILE="$DIAG/pad.ctl" -E GOW3_PAD_REPLAY="$ROUTE" -E GOW3_STALL_MS=25)
    for e in "${envs[@]}"; do args+=(-E "$e"); done
    printf '%s\n' "build=$build" "${envs[@]}" > "$dir/env.txt"
    systemd-run --user --collect --unit wipeout-bench "${args[@]}" bash -c \
        "cd '$build' && PYTHONUNBUFFERED=1 ./AppRun --play 2>&1 | python3 -I -u '$DIAG/stamp.py' '$dir/game.log' > /dev/null"
    local t=0 status=ok
    until grep -q "pad replay finished" "$dir/game.log" 2>/dev/null; do
        sleep 2; t=$((t + 2))
        [ $t -ge 240 ] && { status=timeout; break; }
    done
    if [ $status = ok ]; then
        sleep 25
        local before; before=$(ls "$SHOTS" 2>/dev/null | tail -1)
        echo shot > "$DIAG/pad.ctl"; sleep 3
        local after; after=$(ls "$SHOTS" 2>/dev/null | tail -1)
        [ -n "$after" ] && [ "$after" != "$before" ] && cp "$SHOTS/$after" "$dir/"
    fi
    echo "$status" > "$dir/status"
    pkill -x gow3-probe
    while pgrep -x gow3-probe >/dev/null; do sleep 1; done
    while systemctl --user is-active --quiet wipeout-bench; do sleep 1; done # the unit name is reused
    sleep 10
}
for i in $(seq 1 "$N"); do
    run_one "$OUT/$(printf %02d $((2 * i - 1)))-A" "$SPEC_A"
    run_one "$OUT/$(printf %02d $((2 * i)))-B" "$SPEC_B"
done
echo "$OUT"
