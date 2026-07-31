#pragma once
#ifndef CATA_TESTS_DETERMINISM_PROBE_H
#define CATA_TESTS_DETERMINISM_PROBE_H

#include <cstdint>
#include <string>
#include <vector>

// One per-turn observation of world state, as defined by section 7.2 of
// docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md
struct determinism_sample {
    int turn = 0;
    uint64_t rng_calls = 0;
    uint64_t rng_state = 0;
    uint64_t creature_hash = 0;
    uint64_t player_hash = 0;
    uint64_t combined = 0;
};

// FNV-1a over the eight bytes of value. Order sensitive by construction.
uint64_t fnv1a_u64( uint64_t hash, uint64_t value );

// Snapshot the current world. Creatures are visited via game::all_monsters(),
// which iterates a std::vector and is therefore reproducible; the unordered
// monsters_by_location map is deliberately not used.
determinism_sample sample_world_state();

// Write samples as CSV with a header line, for tools/determinism_probe/compare_logs.py
// Returns false if the file could not be written. Callers must check: a probe
// run that silently produced no log leaves the comparison step reading a stale
// file from a previous run and calling it a clean result.
bool write_sample_log( const std::string &path, const std::vector<determinism_sample> &samples );

#endif // CATA_TESTS_DETERMINISM_PROBE_H
