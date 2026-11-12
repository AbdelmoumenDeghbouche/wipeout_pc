#!/bin/bash
# Records a pad route from boot for the Deck benchmark (tools/deck/bench-run.sh).
#   route-record.sh start <build dir> [name]  launches the build, focuses it and presses F9 until
#                                      the recording runs; then play the route on the Deck.
#   route-record.sh stop               presses F9 again and writes the route timed from "gamepad
#                                      connected", the moment a GOW3_PAD_REPLAY replay starts:
#                                      ~/diag/routes/<name>.pad when started with a name (then
#                                      `bench-run.sh N A B <name>` replays it), else
#                                      ~/diag/route-shifted.pad.
#   route-record.sh list               the saved routes: length and whether the marks are written.
# A route whose first input comes earlier than 18 s after the start is moved later as a whole on "stop"
# (ROUTE_MIN_FIRST_MS, 0 to keep it as recorded): earlier inputs are not taken reliably on a replay.
set -u
DIAG=$HOME/diag; ROUTES=$DIAG/routes
export DISPLAY=:0 XAUTHORITY=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
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
    BUILD=${2:?build dir}; NAME=${3:-}
    if [ -n "$NAME" ]; then
        [[ $NAME =~ ^[A-Za-z0-9._-]+$ ]] || { echo "a route name is letters, digits, '.', '_' or '-'" >&2; exit 2; }
        [ -e "$ROUTES/$NAME.pad" ] && { echo "$ROUTES/$NAME.pad exists: pick another name or remove it" >&2; exit 1; }
    fi
    pgrep -x gow3-probe >/dev/null && { echo "the game is running" >&2; exit 1; }
    rm -f "$DIAG/route.pad"; printf '%s\n' "$NAME" > "$DIAG/route.name"
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
    NAME=$(cat "$DIAG/route.name" 2>/dev/null)
    OUT=$DIAG/route-shifted.pad
    [ -n "$NAME" ] && { mkdir -p "$ROUTES"; OUT=$ROUTES/$NAME.pad; }
    awk -v s="$shift_ms" '{$1 = $1 + s; print}' "$DIAG/route.pad" > "$OUT"
    echo "shift $shift_ms ms: $OUT ($(wc -l < "$OUT") samples)"
    # A replay does not take inputs made in the first seconds after the start reliably (two routes whose
    # first press came 7-10 s in stayed in the menus; the routes that replay press first at 18 s or later).
    # A route that starts earlier is moved later as a whole; ROUTE_MIN_FIRST_MS=0 leaves it as recorded.
    min_first=${ROUTE_MIN_FIRST_MS:-18000}
    # The first input: a button, a trigger, or a stick pushed well off centre (not stick drift).
    first=$(awk 'function off(v) { return v < 88 || v > 168 }
                 $2 != 0 || off($3) || off($4) || off($5) || off($6) || $7 > 40 || $8 > 40 {print $1; exit}' "$OUT")
    if [ -n "$first" ] && [ "$first" -lt "$min_first" ]; then
        add=$((min_first - first))
        awk -v a="$add" -v f="$first" '$1 >= f {$1 = $1 + a} {print}' "$OUT" > "$OUT.tmp" && mv "$OUT.tmp" "$OUT"
        echo "first input was $first ms after the start: the route is moved $add ms later (first input now at $min_first ms)"
    fi
    [ -n "$NAME" ] && echo "write $OUT.marks (countdown_ms, photo_ms), then: bench-run.sh N A B $NAME" ;;
list)
    for f in "$ROUTES"/*.pad; do
        [ -e "$f" ] || { echo "no routes in $ROUTES"; break; }
        end=$(tail -1 "$f" | cut -d' ' -f1)
        marks=$([ -e "$f.marks" ] && echo "marks written" || echo "NO MARKS")
        printf '%-24s %5.1f s  %s\n' "$(basename "$f" .pad)" "$(awk -v m="$end" 'BEGIN {print m / 1000}')" "$marks"
    done ;;
*) echo "usage: route-record.sh start <build dir> [name] | stop | list" >&2; exit 2 ;;
esac
