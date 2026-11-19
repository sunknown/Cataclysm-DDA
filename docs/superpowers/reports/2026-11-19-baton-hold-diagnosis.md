# Baton-hold diagnosis: `participant world has held the baton for 30 s`

- Date: 2026-11-19
- Branch: `fix/mp-watchdog` (from `mp` @ af943744bb)
- Code under study: `mp_sched_start_watchdog()` (`src/mp/mp_sched.cpp`), `do_turn()`'s
  barrier loop (`src/do_turn.cpp`), trace added in 2de091677e.

## Symptom

`.data/mp-srv/config/debug.log` from the manual session of 2026-11-19 carries repeated lines

```
15:10:17.278 WARNING : mp sched: participant world has held the baton for 30017 ms
15:11:22.416 WARNING : mp sched: participant world has held the baton for 30316 ms
15:12:19.148 WARNING : mp sched: participant world has held the baton for 30316 ms
15:14:09.114 WARNING : mp sched: participant world has held the baton for 30017 ms
15:15:11.650 WARNING : mp sched: participant world has held the baton for 30316 ms
```

five times in five minutes, in a session that remained fully playable. The original report
(13:10:16, `30017 ms`) came 25 s after a partner disconnected and the other player kept playing.

## Reproduction

Two clients on the `coop-manual` world, both continuing their saved characters; Bo's terminal is
closed (a real transport close, the session continues), Ada keeps acting. With the trace build
and the trace's DebugLog mirror serialised (so lines do not interleave), the watchdog fired again:

```
15:41:39.179 WARNING : mp sched: participant world has held the baton for 30119 ms
```

The server log for the same run:

```
15:41:09 mp: game started in world "coop-manual"
15:41:15 mp: "Greta Sauer" disconnected after 29s; the session continues
```

## Trace

Records between game start and the report (`15:41:09..15:41:40`), format
`<generation> <holder> <reason>`:

```
15:41:09.8 INFO : mp sched trace: 765 world turn-start
15:41:09.8 INFO : mp sched trace: 765 world poll-begin
15:41:09.8 INFO : mp sched trace: 766 1 yield_to
...
15:41:39.1 INFO : mp sched trace: 1378 world park
15:41:39.1 INFO : mp sched trace: 1378 world turn-start
```

Aggregates over that 31 s window:

| Measure | Value |
|---|---|
| well-formed trace records | 4294 |
| baton handoffs (generation delta) | 615 (≈19.9/s) |
| watchdog 100 ms ticks replaying the old rule | 311 |
| ticks whose sampled holder was `world` | 305 |
| ticks whose sampled holder was a player | 6 |
| longest stretch sampled as `world` | **30.3 s** |
| reports at the end of that stretch | 1 (`30119 ms`) |

The phase mix is `poll-begin` 1063, `poll-end` 1063, `turn-start` 776, `park` 774,
`release_to` 308, `yield_to` 308: the world was running its barrier loop throughout, and the
batons were moving.

## Verdict: A — measurement artifact

The world genuinely was the holder at almost every instant, but it was not stalled: it handed the
baton to the dropped seat and got it back ~20 times a second, and the held-player slices were
shorter than the watchdog's 100 ms poll. The watchdog reset its clock only when a poll *observed*
a different holder (`mp_sched.cpp`, `if( now_holder != last )`). 305 of 311 polls saw `world`, so
the timer accumulated to 30.3 s even though 615 handoffs happened inside it. This is exactly the
sampling failure hypothesis A predicted.

There is a second hole the reproduction exposes: `world_waiting` is set **after**
`wait_for_request()` returns (`do_turn.cpp`), so the 50 ms wait windows themselves are not
declared; and the `moved` path clears the flag every iteration, so a world that yields to a
dropped seat and immediately continues is "not waiting" for minutes at a time. A suppression that
is only set after the wait cannot cover the wait.

What this is not: a real 30 s hold. No phase in the trace sits unchanged for 30 s; the trace keeps
producing records, and the session kept answering Ada's keys.

## Consequences for the fix

- The watchdog must follow **handoffs**, not the sampled holder: reset on any observed change of
  the new `holder_generation()` (and still on a holder change).
- The declared wait must start **before** `wait_for_request()` and end after it returns, so the
  wait itself is covered, not just the return.
- Threshold and report-once behaviour stay as they are; genuine stalls must still report.

Out of scope, recorded for the human: with a dropped seat the world spins yielding to it at ~20
handoffs/s even when the remaining player is idle at the map. That spin is the trigger for the
false report and a CPU cost of its own; the watchdog fix makes the report honest, it does not
stop the spin.

## Manual rerun with the fix

Same script, same world, same disconnect, ~2 minutes of Ada acting with the fixed server
(`a4b0a04d0a`):

| Measure | Before fix | After fix |
|---|---|---|
| watchdog reports | 1 (`30119 ms`) | **0** |
| trace records after the disconnect | — | 19111 |
| baton handoffs after the disconnect | 615 in 31 s | 3564 in ~120 s |
| holder mix (records) | ~99% world | 15746 world / 1782 player |
| generation | 765 → 1380 | 806 → 4370 |

The condition is unchanged — the baton still moves thousands of times while the world is the
sampled holder — and the report is gone, because the watchdog now resets on the generation
instead of on the sampled holder.
