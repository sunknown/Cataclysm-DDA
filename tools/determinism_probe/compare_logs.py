#!/usr/bin/env python3
"""Compare two determinism probe logs and report the first divergence.

Usage: compare_logs.py run_a.csv run_b.csv

Exit code 0 means the two runs matched on every turn, 1 means they diverged.
The report names the first divergent turn and which component moved first,
which is what tells you where to look: an rng_calls delta means one run
consumed a different number of random values, while a creature_hash delta at
an identical rng_calls means the same draws produced a different arrangement,
pointing at iteration order rather than at the RNG.
"""
import csv
import sys

FIELDS = ["rng_calls", "rng_state", "creature_hash", "player_hash", "combined"]


def load(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    a = load(sys.argv[1])
    b = load(sys.argv[2])

    if len(a) != len(b):
        print(f"DIVERGED: length {len(a)} vs {len(b)}")
        return 1

    for i, (ra, rb) in enumerate(zip(a, b)):
        if ra["combined"] == rb["combined"]:
            continue

        print(f"DIVERGED at turn {ra['turn']} (sample index {i})")
        for field in FIELDS:
            mark = "  " if ra[field] == rb[field] else "->"
            print(f"{mark} {field}: {ra[field]} vs {rb[field]}")

        if ra["rng_calls"] != rb["rng_calls"]:
            delta = int(rb["rng_calls"]) - int(ra["rng_calls"])
            print(f"\nRNG call count differs by {delta}. One run drew a different")
            print("number of random values. Look for a conditional RNG call whose")
            print("condition depends on iteration order or on floating point.")
        else:
            print("\nRNG call count is identical, so both runs drew the same values")
            print("in the same quantity but arranged the world differently. This is")
            print("the signature of nondeterministic iteration order — the prime")
            print("suspect is monsters_by_location in src/creature_tracker.h:150.")
        return 1

    print(f"IDENTICAL across {len(a)} turns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
