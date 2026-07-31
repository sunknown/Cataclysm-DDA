#include "determinism_probe.h"

#include <fstream>
#include <locale>
#include <sstream>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "game.h"
#include "monster.h"
#include "npc.h"
#include "rng.h"

static constexpr uint64_t fnv_offset_basis = 1469598103934665603ULL;
static constexpr uint64_t fnv_prime = 1099511628211ULL;

uint64_t fnv1a_u64( uint64_t hash, uint64_t value )
{
    for( int i = 0; i < 8; ++i ) {
        hash ^= ( value >> ( i * 8 ) ) & 0xFFULL;
        hash *= fnv_prime;
    }
    return hash;
}

// Signed values are folded through a two's-complement-stable conversion so the
// hash does not depend on implementation-defined signed shifts.
static uint64_t as_u64( int v )
{
    return static_cast<uint64_t>( static_cast<int64_t>( v ) );
}

static uint64_t hash_creature( uint64_t h, const Creature &c )
{
    const tripoint_abs_ms p = c.pos_abs();
    h = fnv1a_u64( h, as_u64( p.x() ) );
    h = fnv1a_u64( h, as_u64( p.y() ) );
    h = fnv1a_u64( h, as_u64( p.z() ) );
    h = fnv1a_u64( h, as_u64( c.get_hp() ) );
    h = fnv1a_u64( h, as_u64( c.get_moves() ) );
    return h;
}

// std::minstd_rand0 streams its entire state as a single decimal integer.
// Uses rng_peek_engine() rather than rng_get_engine(): the latter counts the
// access, so sampling would advance the counter this very sample reports.
//
// The stream is imbued with the classic locale before use. An engine streams
// through the stream's locale, and CDDA installs a user locale globally
// (src/main.cpp, src/options.cpp); under a locale with digit grouping, state
// 1234567 would stream as "1,234,567" and the parse below would stop at the
// comma and yield 1. That is a silent loss of sensitivity in a divergence
// detector - the one direction of failure a probe must not have.
static uint64_t current_rng_state()
{
    std::ostringstream ss;
    ss.imbue( std::locale::classic() );
    ss << rng_peek_engine();
    try {
        return std::stoull( ss.str() );
    } catch( const std::exception & ) {
        // Cannot happen for minstd_rand0 (state fits in 31 bits), but a silent
        // wrong number here would be indistinguishable from a real divergence.
        return 0;
    }
}

determinism_sample sample_world_state()
{
    determinism_sample s;
    s.turn = to_turn<int>( calendar::turn );
    s.rng_calls = rng_call_count();

    // current_rng_state() must be read after rng_calls, because streaming the
    // engine does not touch it but reading the counter first keeps the two
    // fields describing the same instant regardless of future changes.
    s.rng_state = current_rng_state();

    uint64_t ch = fnv_offset_basis;
    uint64_t creature_count = 0;
    for( monster &m : g->all_monsters() ) {
        ch = hash_creature( ch, m );
        // AI state, not just the body. A divergence in target selection or in
        // anger accumulation exists a turn or more before it moves anybody, so
        // hashing position alone reports it late or not at all.
        ch = fnv1a_u64( ch, as_u64( m.anger ) );
        ch = fnv1a_u64( ch, as_u64( m.morale ) );
        ++creature_count;
    }
    for( npc &guy : g->all_npcs() ) {
        ch = hash_creature( ch, guy );
        ++creature_count;
    }
    // Folded in so that a world which lost creatures cannot hash like a world
    // that kept them: an emptied creature list would otherwise collapse to the
    // bare offset basis and compare equal across two equally dead runs.
    ch = fnv1a_u64( ch, creature_count );
    s.creature_hash = ch;

    uint64_t ph = fnv_offset_basis;
    ph = hash_creature( ph, get_avatar() );
    ph = fnv1a_u64( ph, as_u64( get_avatar().get_stamina() ) );
    // Spec 7.2 asks for stamina and sleepiness both. In a scenario whose whole
    // premise is a sleeping avatar, sleepiness is the avatar-side quantity that
    // actually evolves.
    ph = fnv1a_u64( ph, as_u64( get_avatar().get_sleepiness() ) );
    s.player_hash = ph;

    uint64_t combined = fnv_offset_basis;
    combined = fnv1a_u64( combined, as_u64( s.turn ) );
    combined = fnv1a_u64( combined, s.rng_calls );
    combined = fnv1a_u64( combined, s.rng_state );
    combined = fnv1a_u64( combined, s.creature_hash );
    combined = fnv1a_u64( combined, s.player_hash );
    s.combined = combined;

    return s;
}

bool write_sample_log( const std::string &path, const std::vector<determinism_sample> &samples )
{
    std::ofstream out( path, std::ios::trunc );
    // Classic locale for the same reason current_rng_state() uses it: a grouped
    // thousands separator would corrupt the CSV the comparison tool parses.
    out.imbue( std::locale::classic() );
    out << "turn,rng_calls,rng_state,creature_hash,player_hash,combined\n";
    for( const determinism_sample &s : samples ) {
        out << s.turn << ',' << s.rng_calls << ',' << s.rng_state << ','
            << s.creature_hash << ',' << s.player_hash << ',' << s.combined << '\n';
    }
    out.close();
    return out.good();
}
