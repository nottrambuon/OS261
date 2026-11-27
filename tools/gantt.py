#!/usr/bin/env python3
"""Draw a text Gantt chart from the output of ./os

Usage:
    ./os sched > out.txt
    python3 tools/gantt.py out.txt

Each row is one CPU, each column is one time slot, each cell is the PID
running on that CPU in that slot ('.' = idle).
"""
import re
import sys


def parse(lines):
    slot = 0
    running = {}            # cpu -> pid currently on that cpu
    events = []             # (slot, cpu, pid or None)
    for line in lines:
        m = re.match(r"Time slot\s+(\d+)", line)
        if m:
            slot = int(m.group(1))
            continue
        m = re.search(r"CPU (\d+): Dispatched process\s+(\d+)", line)
        if m:
            events.append((slot, int(m.group(1)), int(m.group(2))))
            continue
        m = re.search(r"CPU (\d+): (Put process|Processed)\s+(\d+)", line)
        if m:
            events.append((slot, int(m.group(1)), None))
    end = slot
    cpus = sorted({c for _, c, _ in events})
    grid = {c: ["."] * end for c in cpus}
    for s, c, pid in sorted(events, key=lambda e: e[0]):
        running[c] = pid
        for t in range(s, end):
            grid[c][t] = "." if pid is None else str(pid)
    return end, grid


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    with open(sys.argv[1]) as f:
        end, grid = parse(f.readlines())
    width = len(str(end)) + 1
    print("slot  " + "".join(str(t).rjust(width) for t in range(end)))
    for cpu, cells in grid.items():
        print(f"CPU{cpu}  " + "".join(c.rjust(width) for c in cells))


if __name__ == "__main__":
    main()
