# MP world free-run after a hurry drop and a disconnect — verdict

Branch: `fix/mp-world-free-run` (from `mp` at `72f859f14c`). Plan:
`docs/superpowers/plans/2026-11-21-mp-world-free-run.md`. Specs: scheduler §7.3
(the pace floor), lifecycle §5.1 (a disconnect does not end the session).

## Symptom

Live run C on 2026-11-20: immediately after
`"Genie Roe" disconnected; the session continues`, the calendar went
8:00:27 → 8:06:43 in about 19 s (~20 turns/s) with an autosave, and the scheduler
trace repeated `turn-start → poll-begin → poll-end → park` every 50 ms with no
baton handoffs. The next keypress stopped it. Run D — a fresh disconnect with no
preceding hurry — did not free-run.

## Reproduction

`tests/mp_session_test.cpp`:
`a_left_behind_player_paces_the_world_after_the_partner_disconnects`.

The harness shortens both countdowns to 1000 ms, seat 0 opens the inventory and is
dropped by seat 1's hurry, seat 1 takes the turn it hurried through, then seat 1's
client is dropped. Two turns are timed from that point.

RED at the current code (`--rng-seed 1`): **63 ms for two turns**, against a
1000 ms floor — a turn every ~31 ms, the live free-run's rate. The floor would
have made the same window ~2000 ms, so the pin (`elapsed >= 1500 ms`) fails
before the fix and passes after it.

A probe at the pace decision printed, once per turn:

```
PACEPROBE keyboard slot=1 left_behind=1 age_ms=123 s0_lb=1 s0_state=5
  s1_state=1 s1_act=0 s1_dist=0 s1_away=0 s1_disc=1 slots=2 dead0=0 dead1=0
```

## Mechanism

The hurry drop is not per-turn: `mp_turn_reset()` keeps
`g_dropped_by_countdown[slot]` while the player stays at the same input wait, so a
reader is not waited on for as long as they read (§7.3). With the partner gone,
the barrier is satisfied and the world is free to move; the only thing that must
pace it is the floor in `mp_turn_paced()`.

| slot | `mp_player_state_of` | left behind | away | disconnected | activity / destination | `mp_turn_paced()` |
|---|---|---|---|---|---|---|
| 0 | `not_waited` | yes | no | no | none | arms the floor |
| 1 | `disconnected` | no | no | **yes** | none | **returns false** |

`mp_turn_paced()` skips a dead seat, an `away` seat and a left-behind seat. A
disconnected seat falls through all three and then satisfies
`!activity && !has_distant_destination()`, so the function concludes that somebody
is playing by hand and returns false before the floor is reached. The barrier is
satisfied, the floor is off, and the turn loop closes turns as fast as the CPU
runs them.

Run D was clean because no hurry had dropped anyone: the surviving seat still held
the barrier, the turn could not close at all, and the pace floor was never asked to
do anything.

## Why the plan's first sequence did not go red

The countdown drops every holder, not only the reader: in the harness seat 1 was
sitting at its input wait too, so `mp_player_left_behind(1)` was true and armed the
floor by itself. That is why the original sequence passed (2020 ms for two turns)
on the unfixed code. The live sequence differs: the hurrying partner takes its
turn, which releases its own drop at the turn boundary, and only then loses its
client. The test now presses `.` for seat 1 after the countdown expires, asserts
`!mp_player_left_behind(1)`, and then disconnects — at which point the drop belongs
to seat 0 only and the defect shows.

## Fix (Task 2)

`mp_turn_paced()` treats a disconnected seat like an away one: nobody is at that
keyboard, so it cannot be the player whose actions pace the world. The header
contract is updated with the same sentence. Barrier semantics, countdown lengths
and the away rule are unchanged.

One test detail follows from the fix: a paced turn stays open, and `do_turn()`
returns after each `MP_POLL_MS` (50 ms) poll, so `run_until`'s "turns" are polls.
A 1000 ms floor costs more than 20 polls per turn, which is why the measurement
budget is 100 polls rather than the plan's 20. The unfixed code closes turns
without polling, so the RED still fails.
