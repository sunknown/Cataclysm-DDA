#pragma once
#ifndef CATA_SRC_MP_MP_HOTSEAT_LOOP_H
#define CATA_SRC_MP_MP_HOTSEAT_LOOP_H

// Advances the game by one hotseat turn. Every player slot acts in index
// order (section 5.5 of the design spec), then the world advances with the
// single-player turn machinery, exactly as in a solo game.
// Returns true if the game is over.
bool mp_hotseat_do_turn();

#endif // CATA_SRC_MP_MP_HOTSEAT_LOOP_H
