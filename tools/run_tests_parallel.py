#!/usr/bin/env python3
"""Run the Cataclysm-DDA Catch2 suite sharded across cores.

The vendored Catch2 is v2.13.10, which has no --shard-count, so sharding is
done by test name: the suite is enumerated with --list-test-names-only, split
into balanced shards, and each shard is handed back to the binary via -f.

Balancing uses a duration cache written by previous runs (Catch2's -d yes).
The first run is balanced by name only and will be lumpy; every run after that
uses measured times, so the shards converge on equal wall time.

Every shard gets its own --user-dir, which is mandatory rather than tidy: a
reused user-dir accumulates past_games_info and breaks stats_tracker.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

# Failures known to be independent of any change under test: they pass when run
# alone, and never call do_turn. Reported apart from real failures so a genuine
# regression cannot hide among them.
KNOWN_FLAKES = {
    "default_overmap_generation_has_non_mandatory_specials_at_origin",
    "highway_neighbor_missing_connections_no_crash",
    "weary_recovery_mutations",
}

# Tests that hang -- spin at full CPU and never finish -- in the sharded layout,
# though they pass when run alone. They are left out of every run unless a spec
# names them exactly, because one of them costs a whole run: the shard waits on
# it for as long as anyone lets it. Each entry says what it blocks and who fixes
# it, like KNOWN_FLAKES above.
#  - overmap_terrain_coverage: alone on a shard with seed 1 it spun for 41 and
#    33 minutes in two runs; standalone it passes in ~295 s. Blocks only a full
#    run finishing; unrelated to the multiplayer work, nobody owns a fix.
KNOWN_HANGS = {
    "overmap_terrain_coverage",
}

# The safety net for a hang nobody has listed yet: a shard still running after
# this long is killed and reported with the tests it never finished. A healthy
# full run's longest shard takes a few minutes, so this only ever fires on a
# hang.
DEFAULT_SHARD_TIMEOUT = 900

# Catch2's console separators are exactly 79 characters wide. The width is the
# whole test: the game's own startup banner prints a 41-dash rule, and matching
# that as a block header swallows the rest of the log.
DASHES = re.compile(r"^-{79}$")
EQUALS = re.compile(r"^={79}$")
DURATION = re.compile(r"^\s*([0-9]+\.[0-9]+) s: (.+?)\s*$")
SUMMARY_CASES = re.compile(r"^test cases:\s*(\d+)\s*\|.*?(?:(\d+)\s+failed)?$")
ALL_PASSED = re.compile(r"^All tests passed \((\d+) assertion")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run the cata_test suite in parallel shards.",
        epilog="Example: cata-test-par.py -j 10 -- --rng-seed 1234",
    )
    p.add_argument("spec", nargs="*",
                   help="Catch2 test specs/tags to run (default: the whole suite)")
    p.add_argument("-b", "--binary", default="tests/cata_test",
                   help="test binary (default: tests/cata_test)")
    p.add_argument("-j", "--jobs", type=int, default=0,
                   help="number of shards (default: number of cores)")
    p.add_argument("-o", "--outdir", default="",
                   help="artifact directory (default: /tmp/cata-par-<binary name>)")
    p.add_argument("--cache", default="",
                   help="duration cache file (default: "
                        "~/.cache/cata-test-par/<binary name>.tsv)")
    p.add_argument("--keep", action="store_true",
                   help="keep the per-shard user-dirs after the run")
    p.add_argument("--allow-known-flakes", action="store_true",
                   help="exit 0 when the only failures are the known flakes")
    p.add_argument("--shard-timeout", type=float, default=DEFAULT_SHARD_TIMEOUT,
                   help="kill a shard still running after this many seconds and "
                        f"report it as timed out (default: {DEFAULT_SHARD_TIMEOUT}; "
                        "0 disables)")
    p.add_argument("--include-hangs", action="store_true",
                   help="also run the tests in KNOWN_HANGS")
    p.add_argument("--slowest", type=int, default=10,
                   help="how many slowest tests to print (default: 10)")
    p.add_argument("--plan", default="",
                   help="shard assignment file (default: "
                        "~/.cache/cata-test-par/<binary name>.plan). Reused when it "
                        "still matches the test set, so runs stay comparable")
    p.add_argument("--replan", action="store_true",
                   help="recompute the shard assignment from current durations")
    p.add_argument("--seed", default="1",
                   help="RNG seed passed to every shard (default: 1; "
                        "'time' reproduces the unseeded, non-reproducible behaviour)")

    # Everything after a bare -- is forwarded to every shard verbatim. argparse
    # cannot hold two variadic positionals, so the split happens here.
    argv = sys.argv[1:]
    extra: list[str] = []
    if "--" in argv:
        cut = argv.index("--")
        argv, extra = argv[:cut], argv[cut + 1:]
    args = p.parse_args(argv)
    args.extra = extra
    return args


def enumerate_tests(binary: str, specs: list[str], workdir: Path) -> list[str]:
    listing_dir = workdir / "list-user-dir"
    shutil.rmtree(listing_dir, ignore_errors=True)
    listing_dir.mkdir(parents=True, exist_ok=True)
    cmd = [binary, "--list-test-names-only", f"--user-dir={listing_dir}/"] + specs
    proc = subprocess.run(cmd, capture_output=True, text=True)
    # Catch2 v2 returns the matched count (255-capped) from list mode, so a
    # non-zero exit here is normal. Empty output is the real failure.
    names = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip()]
    shutil.rmtree(listing_dir, ignore_errors=True)
    if not names:
        sys.exit(f"no tests matched {specs or ['(whole suite)']}\n"
                 f"{proc.stderr.strip()[:2000]}")
    return names


def load_cache(path: Path) -> dict[str, float]:
    if not path.exists():
        return {}
    out: dict[str, float] = {}
    for line in path.read_text().splitlines():
        name, _, secs = line.rpartition("\t")
        if name:
            try:
                out[name] = float(secs)
            except ValueError:
                pass
    return out


def save_cache(path: Path, durations: dict[str, float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{n}\t{s:.4f}\n" for n, s in sorted(durations.items())))


def balance(names: list[str], jobs: int, known: dict[str, float]) -> list[list[str]]:
    """Longest-processing-time-first bin packing.

    Tests with no measured duration get the median of the ones that have it, so
    a first run spreads unknowns evenly instead of piling them into one shard.
    """
    measured = [known[n] for n in names if n in known]
    default = statistics.median(measured) if measured else 1.0
    weighted = sorted(((known.get(n, default), n) for n in names), reverse=True)

    shards: list[list[str]] = [[] for _ in range(jobs)]
    loads = [0.0] * jobs
    for secs, name in weighted:
        i = loads.index(min(loads))
        shards[i].append(name)
        loads[i] += secs
    return [s for s in shards if s]


def escape_spec(name: str) -> str:
    """Catch2 treats , [ ] * \\ as spec syntax; a literal name must escape them."""
    out = []
    for ch in name:
        if ch in "\\,[]*":
            out.append("\\")
        out.append(ch)
    return "".join(out)


def parse_log(text: str) -> tuple[set[str], int | None, int | None, dict[str, float]]:
    """Return (failed test names, cases run, cases failed, durations).

    Catch2's console reporter opens a dashed block for every test case that has
    something to say -- a failure OR a warning -- so a block alone does not mean
    a failure; the block body has to contain a FAILED: line.
    """
    failed: set[str] = set()
    durations: dict[str, float] = {}
    cases = failures = None

    lines = text.splitlines()
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]

        m = DURATION.match(line)
        if m:
            # -d yes reports every SECTION, not just the test case, and a test
            # case with sections is re-entered once per leaf -- so its own name
            # appears several times, each covering one pass. Summing gives the
            # real cost of the test case; taking the last value would understate
            # a heavily sectioned test by a large factor and unbalance the
            # shards. Section names accumulate too and are simply never looked
            # up, since balancing only ever asks about test case names.
            durations[m.group(2)] = durations.get(m.group(2), 0.0) + float(m.group(1))
            i += 1
            continue

        if line.startswith("test cases:"):
            nums = re.findall(r"(\d+)\s+(passed|failed)", line)
            total = re.match(r"test cases:\s*(\d+)", line)
            cases = int(total.group(1)) if total else None
            failures = next((int(v) for v, kind in nums if kind == "failed"), 0)
            i += 1
            continue

        if ALL_PASSED.match(line):
            m2 = re.search(r"in (\d+) test case", line)
            cases = int(m2.group(1)) if m2 else cases
            failures = 0
            i += 1
            continue

        if DASHES.match(line):
            # header: dashes, name (may wrap), dashes, location, dots
            i += 1
            name_lines = []
            # A wrapped name is at most a couple of lines; the bound keeps a
            # malformed block from consuming the rest of the log.
            while i < n and not DASHES.match(lines[i]) and len(name_lines) < 6:
                name_lines.append(lines[i])
                i += 1
            if i < n and DASHES.match(lines[i]):
                i += 1  # closing dashes
            # The header is the test case name, then one indented line per
            # SECTION the failure sits in. The name alone is what KNOWN_FLAKES
            # lists; with the sections glued on, a flake failing inside one
            # read as a real failure.
            case_lines = [x.strip() for x in name_lines if x and not x[0].isspace()]
            name = " ".join(x for x in case_lines if x) or " ".join(
                x.strip() for x in name_lines if x.strip())
            body = []
            while i < n and not DASHES.match(lines[i]) and not EQUALS.match(lines[i]):
                body.append(lines[i])
                i += 1
            if any("FAILED:" in b for b in body) and name:
                failed.add(name)
            continue

        i += 1

    return failed, cases, failures, durations


def main() -> int:
    args = parse_args()
    binary = args.binary
    if not os.access(binary, os.X_OK):
        sys.exit(f"{binary} is not executable -- build it first, or pass -b")

    jobs = args.jobs or os.cpu_count() or 4
    name = Path(binary).name
    outdir = Path(args.outdir or f"/tmp/cata-par-{name}")
    cache_path = Path(args.cache or
                      Path.home() / ".cache" / "cata-test-par" / f"{name}.tsv")

    shutil.rmtree(outdir, ignore_errors=True)
    outdir.mkdir(parents=True, exist_ok=True)

    names = enumerate_tests(binary, args.spec, outdir)
    # A spec that names a hang exactly is a request to run it; anything else
    # (the whole suite, a tag) leaves it out.
    skipped_hangs = [] if args.include_hangs else sorted(
        x for x in names if x in KNOWN_HANGS and x not in args.spec)
    names = [x for x in names if x not in skipped_hangs]
    if not names:
        sys.exit("only known hangs matched -- name one exactly or pass --include-hangs")
    known = load_cache(cache_path)
    jobs = min(jobs, len(names))

    # Freezing the assignment is what makes two runs comparable. The suite has
    # order-dependent tests -- ones that pass alone and in the full sequential
    # run but fail inside a subset, reproducibly, seed or no seed -- so a shard
    # layout that drifts between runs surfaces a different pre-existing failure
    # each time and no run can be diffed against another. The plan is dropped
    # and recomputed whenever the test set itself changes.
    # Keyed by spec as well as binary, so alternating a tag run with a full run
    # does not make each one throw the other's layout away.
    spec_key = "all" if not args.spec else hashlib.sha1(
        "\0".join(args.spec).encode()).hexdigest()[:8]
    plan_path = Path(args.plan or Path.home() / ".cache" / "cata-test-par" /
                     f"{name}.{spec_key}.plan")
    shards = None
    if not args.replan and plan_path.exists():
        loaded: dict[int, list[str]] = {}
        for line in plan_path.read_text().splitlines():
            idx, _, test = line.partition("\t")
            if test:
                loaded.setdefault(int(idx), []).append(test)
        planned = [t for s in loaded.values() for t in s]
        if set(planned) == set(names):
            shards = [loaded[k] for k in sorted(loaded)]
        else:
            print(f"test set changed ({len(names)} now, {len(planned)} in the "
                  f"plan) -- recomputing the shard layout", flush=True)
    if shards is None:
        shards = balance(names, jobs, known)
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text("".join(f"{i}\t{t}\n"
                                     for i, s in enumerate(shards) for t in s))

    covered = sum(1 for x in names if x in known)
    seed_note = "clock (NOT reproducible)" if args.seed == "time" else args.seed
    print(f"{len(names)} test cases -> {len(shards)} shards "
          f"({covered}/{len(names)} with measured durations from {cache_path}), "
          f"rng seed {seed_note}", flush=True)
    if skipped_hangs:
        print(f"skipped known hang(s): {', '.join(skipped_hangs)} "
              f"(--include-hangs runs them)", flush=True)

    procs = []
    started = time.monotonic()
    for i, shard in enumerate(shards):
        shard_file = outdir / f"shard-{i:02d}.txt"
        shard_file.write_text("".join(escape_spec(x) + "\n" for x in shard))
        user_dir = outdir / f"user-dir-{i:02d}"
        user_dir.mkdir(parents=True, exist_ok=True)
        log_path = outdir / f"shard-{i:02d}.log"
        cmd = [binary, "-f", str(shard_file), "-d", "yes", "--order", "decl",
               f"--user-dir={user_dir}/"]
        # Without --rng-seed the suite seeds from the clock (tests/test_main.cpp
        # :459-462 leaves the engine seed alone when rngSeed() is 0), so every
        # run draws a different sequence and a handful of statistical tests fail
        # at random -- which run they hit says nothing about the code. The same
        # code path reseeds before each test case, so a pinned seed also makes a
        # test's RNG independent of what ran before it, which is what lets these
        # shards stand in for a sequential run.
        if args.seed != "time":
            cmd += ["--rng-seed", str(args.seed)]
        cmd += args.extra
        log = open(log_path, "w")
        procs.append({
            "i": i, "cmd": cmd, "log": log, "log_path": log_path,
            "count": len(shard), "tests": shard, "started": time.monotonic(),
            "timed_out": False,
            "proc": subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT),
        })

    # Polled rather than waited on in order, so a shard is reported when it
    # ends rather than when every shard before it has, and so a hung one can be
    # killed on time.
    done = 0
    running = list(procs)
    while running:
        for p in list(running):
            if p["proc"].poll() is None:
                if (args.shard_timeout > 0 and
                        time.monotonic() - p["started"] > args.shard_timeout):
                    # By PID, through the Popen handle: never a pattern match.
                    p["proc"].kill()
                    p["proc"].wait()
                    p["timed_out"] = True
                else:
                    continue
            p["elapsed"] = time.monotonic() - p["started"]
            p["log"].close()
            running.remove(p)
            done += 1
            note = " TIMED OUT" if p["timed_out"] else ""
            print(f"  shard {p['i']:02d}: {p['count']:4d} cases, "
                  f"{p['elapsed']:6.1f}s, exit {p['proc'].returncode}{note} "
                  f"({done}/{len(procs)} done)", flush=True)
        if running:
            time.sleep(0.5)
    wall = time.monotonic() - started

    all_failed: set[str] = set()
    all_durations: dict[str, float] = {}
    total_cases = 0
    crashed = []
    timed_out = []
    for p in procs:
        failed, cases, _failures, durations = parse_log(
            p["log_path"].read_text(errors="replace"))
        all_failed |= failed
        all_durations.update(durations)
        total_cases += cases or 0
        rc = p["proc"].returncode
        # Catch2 exits with the failed-test count; a signal (negative rc) or a
        # missing summary means the shard died rather than reported.
        if p["timed_out"]:
            # -d yes prints a test's duration when it ends, so the tests of
            # this shard with no duration are the hung one and those queued
            # behind it.
            unfinished = [t for t in p["tests"] if t not in durations]
            timed_out.append((p["i"], p["log_path"], unfinished))
        elif rc < 0 or cases is None:
            crashed.append((p["i"], rc, p["log_path"]))
        p["failed"] = failed

    if all_durations:
        known.update(all_durations)
        save_cache(cache_path, known)

    real = sorted(all_failed - KNOWN_FLAKES)
    flakes = sorted(all_failed & KNOWN_FLAKES)

    print(f"\nwall {wall:.1f}s | {total_cases} cases reported | "
          f"{len(real)} failed | {len(flakes)} known flake(s)")
    if procs:
        times = [p["elapsed"] for p in procs]
        print(f"shard spread: {min(times):.1f}s .. {max(times):.1f}s "
              f"(perfect balance would be ~{sum(times) / len(times):.1f}s)")

    if timed_out:
        print(f"\nTIMED OUT SHARDS (killed after {args.shard_timeout:.0f}s):")
        for i, path, unfinished in timed_out:
            print(f"  shard {i:02d}: {path}")
            if not unfinished:
                print("    every test finished; the process hung on the way out")
                continue
            print(f"    never finished: {', '.join(unfinished[:5])}"
                  + (f" (+{len(unfinished) - 5} more)" if len(unfinished) > 5 else ""))
        print("  one of them hung (shards run in declaration order, not this one); run\n"
              "  them alone to find it -- one that hangs reproducibly goes in KNOWN_HANGS")

    if crashed:
        print("\nCRASHED OR TRUNCATED SHARDS:")
        for i, rc, path in crashed:
            print(f"  shard {i:02d}: exit {rc} -- {path}")

    if flakes:
        print("\nknown pre-existing flakes (pass standalone):")
        for f in flakes:
            print(f"  {f}")

    if real:
        print("\nFAILED:")
        for f in real:
            where = next((str(p["log_path"]) for p in procs if f in p["failed"]), "?")
            print(f"  {f}\n    {where}")

    if args.slowest and all_durations:
        print(f"\nslowest {args.slowest}:")
        for secs, tname in sorted(
                ((s, n) for n, s in all_durations.items()), reverse=True)[:args.slowest]:
            print(f"  {secs:7.1f}s  {tname}")

    if not args.keep:
        for p in procs:
            shutil.rmtree(outdir / f"user-dir-{p['i']:02d}", ignore_errors=True)

    if crashed or timed_out or real:
        return 1
    if flakes and not args.allow_known_flakes:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
