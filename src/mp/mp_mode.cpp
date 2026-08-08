#include "mp/mp_mode.h"

#include "game.h"

bool mp_coop_active()
{
    // Derived, not stored -- see the header for why. `g` is null very early in
    // startup and in unit tests that run before the game object exists, which
    // is single player by definition.
    return g && g->player_slot_count() > 1;
}
