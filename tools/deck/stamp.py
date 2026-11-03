"""Prefix every line of stdin with the local wall clock (HH:MM:SS.mmm, the clock the game's
screenshot names use) and append it to the file given; also echo to stdout.
usage: python3 -I -u stamp.py <log file>"""
import sys, time
out = open(sys.argv[1], "a", buffering=1)
for raw in sys.stdin.buffer:
    t = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d " % int((t % 1) * 1000)
    line = stamp + raw.decode("utf-8", "replace")
    out.write(line)
    sys.stdout.write(line)
