#!/bin/bash
# Call-graph profile of both draw pipeline threads at the race start: the GPU command processor
# (shadPS4:GpuComm, stage A) and the draw recorder (wipeout:DrawRec, stage B), in one recording.
#   ~/diag/perf-start.sh [seconds]   run at the grid (countdown screen): records both threads for
#                                    40 s (or the given seconds); sample times are wall-clock.
#   ~/diag/perf-start.sh report [HH:MM:SS[.mmm]]
#       top symbols per thread over the 20 s from that wall-clock time. With no time: from the
#       pack start in the newest ~/diag/runs/*/game.log, i.e. the first Frame stats window with
#       >= 3000 draws/frame, which starts 5 s before its stamp.
# Test-only overrides (env): PERF_TID = comma list of threads to record, GAME_LOG = game.log to read.
set -u
DATA=$HOME/diag/perf-start.data
COMMS="shadPS4:GpuComm wipeout:DrawRec"   # stage A, stage B

if [ "${1:-}" != report ]; then
  SECS=${1:-40}
  TIDS=${PERF_TID:-}
  if [ -z "$TIDS" ]; then
    PID=$(pgrep -x wipeout-probe | head -1)
    [ -n "$PID" ] || { echo "wipeout-probe is not running" >&2; exit 1; }
    THREADS=$(ps -L -o tid=,comm= -p "$PID")
    for c in $COMMS; do
      T=$(awk -v c="$c" '$2 == c {print $1}' <<<"$THREADS" | head -1)
      [ -n "$T" ] || { echo "thread $c not found in wipeout-probe ($PID)" >&2; exit 1; }
      TIDS=${TIDS:+$TIDS,}$T
    done
  fi
  echo "recording threads $TIDS for $SECS s from $(date +%T.%3N)"
  perf record -k CLOCK_REALTIME -e cpu-clock:u -F 1999 -g -t "$TIDS" -o "$DATA" -- sleep "$SECS" || { echo "perf record failed" >&2; exit 1; }
  echo "done at $(date +%T.%3N); after the race: ~/diag/perf-start.sh report"
  exit 0
fi

START=${2:-}
BACK=0
if [ -z "$START" ]; then
  LOG=${GAME_LOG:-$(ls -t "$HOME"/diag/runs/*/game.log 2>/dev/null | head -1)}
  [ -f "$LOG" ] || { echo "no game.log found" >&2; exit 1; }
  read -r START DRAWS < <(sed -nE 's/^([0-9:.]+) Frame stats.* ([0-9]+) draws\/frame.*/\1 \2/p' "$LOG" | awk '$2 >= 3000 {print; exit}')
  [ -n "$START" ] || { echo "no Frame stats window with >= 3000 draws/frame in $LOG" >&2; exit 1; }
  echo "pack start: $LOG: window stamped $START, $DRAWS draws/frame; window starts 5 s earlier"
  BACK=5
fi
[ -f "$DATA" ] || { echo "no recording at $DATA" >&2; exit 1; }
HDR=$(perf report -i "$DATA" --header-only 2>/dev/null)
FIRST=$(awk '/time of first sample/ {print $NF}' <<<"$HDR")
LAST=$(awk '/time of last sample/ {print $NF}' <<<"$HDR")
[ -n "$FIRST" ] && [ -n "$LAST" ] || { echo "no samples in $DATA" >&2; exit 1; }
T=$(date -d "$(date -d "@$FIRST" +%F) $START" +%s.%6N) || exit 1
read -r A B < <(awk -v t="$T" -v k="$BACK" 'BEGIN {printf "%.6f %.6f\n", t - k, t - k + 20}')
echo "recording $(date -d "@$FIRST" +%T.%3N) - $(date -d "@$LAST" +%T.%3N)," \
     "window $(date -d "@$A" +%T.%3N) - $(date -d "@$B" +%T.%3N): --time $A,$B"
awk -v a="$A" -v b="$B" -v f="$FIRST" -v l="$LAST" \
  'BEGIN {if (a < f || b > l) print "WARNING: window not fully inside the recording"}'
for c in $COMMS; do
  echo "== $c"
  perf report -i "$DATA" --time "$A,$B" --comm "$c" --stdio --no-children -g none --percentage relative --sort symbol --percent-limit 1.5 2>/dev/null | grep -E '^# Samples|^[^#]' | sed -E 's/ +/ /g' | head -26
done
