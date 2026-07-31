#!/usr/bin/env python3
"""Compare two determinism probe logs and report the first divergence.

Usage: compare_logs.py run_a.csv run_b.csv

Exit code 0 means the two runs matched on every turn, 1 means they diverged,
2 means the inputs could not be read or were not probe logs.

The report names the first divergent turn and which component moved first,
which is what tells you where to look: an rng_calls delta means one run
consumed a different number of random values, while a creature_hash delta at
an identical rng_calls means the same draws produced a different arrangement,
pointing at iteration order rather than at the RNG.
"""
import csv
import sys

HEADER = ["turn", "rng_calls", "rng_state", "creature_hash",
          "player_hash", "combined"]
FIELDS = HEADER[1:]


def load(path):
    """Read a probe log, or raise ValueError if it is not one."""
    with open(path, newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise ValueError("%s is empty" % path)
    missing = [name for name in HEADER if name not in rows[0]]
    if missing:
        raise ValueError("%s is missing column(s): %s"
                         % (path, ", ".join(missing)))
    return rows


def report_divergence(index, row_a, row_b):
    """Print the first divergent sample and a reading of what it means."""
    print("DIVERGED at turn %s (sample index %d)" % (row_a["turn"], index))
    for field in HEADER:
        mark = "  " if row_a[field] == row_b[field] else "->"
        print("%s %s: %s vs %s" % (mark, field, row_a[field], row_b[field]))

    if row_a["turn"] != row_b["turn"]:
        print("\nThe turn numbers themselves differ, so one run advanced the")
        print("clock differently. Check that both runs started from the same")
        print("state - a probe log is only comparable with one recorded from")
        print("an identical starting point.")
    elif row_a["rng_calls"] != row_b["rng_calls"]:
        delta = int(row_b["rng_calls"]) - int(row_a["rng_calls"])
        print("\nRNG call count differs by %d. One run drew a" % delta)
        print("different number of random values. Look for a conditional")
        print("RNG call whose")
        print("condition depends on iteration order or on floating point.")
    else:
        print("\nRNG call count is identical, so both runs drew the same")
        print("values in the same quantity but arranged the world")
        print("differently. This is the signature of nondeterministic")
        print("iteration order - the prime suspect is monsters_by_location")
        print("in src/creature_tracker.h:150.")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    try:
        a = load(sys.argv[1])
        b = load(sys.argv[2])
    except (OSError, ValueError) as err:
        print("ERROR: %s" % err)
        return 2

    if len(a) != len(b):
        print("DIVERGED: length %d vs %d" % (len(a), len(b)))
        return 1

    for i, (row_a, row_b) in enumerate(zip(a, b)):
        if all(row_a[name] == row_b[name] for name in HEADER):
            continue
        report_divergence(i, row_a, row_b)
        return 1

    print("IDENTICAL across %d turns (%d field comparisons)"
          % (len(a), len(a) * len(HEADER)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
