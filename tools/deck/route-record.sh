#!/bin/bash
# Records a pad route from boot for the Deck benchmark (tools/deck/bench-run.sh).
#   route-record.sh start <build dir>  launches the build, focuses it and presses F9 until the
#                                      recording runs; then play the route on the Deck.
#   route-record.sh stop               presses F9 again and writes ~/diag/route-shifted.pad:
#                                      the route timed from "gamepad connected", the moment a
#                                      GOW3_PAD_REPLAY replay starts.
set -u
DIAG=$HOME/diag
export DISPLAY=:0 XAUTHORITY=$(ls /run/user/1000/xauth_* | head -1)
game_window() {
    local pid; pid=$(pgrep -x gow3-probe | head -1)
    [ -n "$pid" ] || return 1
    xdotool search --pid "$pid" 2>/dev/null | while read -r w; do
        [ -n "$(xdotool getwindowname "$w" 2>/dev/null)" ] && echo "$w"
    done | head -1
}
press_f9() {
    local w; w=$(game_window) || return 1
    xdotool windowactivate --sync "$w" 2>/dev/null
    xdotool keydown F9; sleep 0.4; xdotool keyup F9
}
case "${1:-}" in
start)
    BUILD=${2:?build dir}
    pgrep -x gow3-probe >/dev/null && { echo "the game is running" >&2; exit 1; }
    rm -f "$DIAG/route.pad"
    RUN=$DIAG/runs/route-$(date +%Y%m%d-%H%M%S); mkdir -p "$RUN"; echo "$RUN" > "$DIAG/route.run"
    systemd-run --user --collect --unit wipeout-route -E DISPLAY -E XAUTHORITY \
        -E GOW3_PAD_RECORD="$DIAG/route.pad" bash -c \
        "cd '$BUILD' && PYTHONUNBUFFERED=1 ./AppRun --play 2>&1 | python3 -I -u '$DIAG/stamp.py' '$RUN/game.log' > /dev/null"
    for _ in $(seq 1 60); do
        sleep 2
        press_f9 && sleep 1.5
        [ -e "$DIAG/route.pad" ] && { echo "recording ($RUN)"; exit 0; }
    done
    echo "the recording did not start" >&2; exit 1 ;;
stop)
    RUN=$(cat "$DIAG/route.run")
    press_f9 || { echo "no game window" >&2; exit 1; }
    for _ in $(seq 1 20); do grep -q "pad recording stopped" "$RUN/game.log" && break; sleep 0.5; done
    ms() { awk -F'[:. ]' '{print (($1*60+$2)*60+$3)*1000+$4}' <<<"$1"; }
    connected=$(grep -m1 "gamepad connected" "$RUN/game.log" | cut -d' ' -f1)
    started=$(grep -m1 "pad recording started" "$RUN/game.log" | cut -d' ' -f1)
    [ -n "$connected" ] && [ -n "$started" ] || { echo "no connect/start lines in $RUN/game.log" >&2; exit 1; }
    shift_ms=$(( $(ms "$started") - $(ms "$connected") ))
    awk -v s="$shift_ms" '{$1 = $1 + s; print}' "$DIAG/route.pad" > "$DIAG/route-shifted.pad"
    echo "shift $shift_ms ms: $DIAG/route-shifted.pad ($(wc -l < "$DIAG/route-shifted.pad") samples)" ;;
*) echo "usage: route-record.sh start <build dir> | stop" >&2; exit 2 ;;
esac
