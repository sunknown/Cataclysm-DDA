#pragma once
#ifndef CATA_SRC_MP_MP_MODE_H
#define CATA_SRC_MP_MP_MODE_H

// Whether the current session has more than one player character. False in
// every single player session, including in a build that has multiplayer
// compiled in.
//
// Derived from the number of player slots rather than stored in a flag of its
// own: a session is co-op exactly when more than one avatar exists. Two
// independent pieces of state could (and did) disagree in both directions -- a
// save that restored a second slot came back with the mode off, leaving slot 1
// simulated but unplayable, and disabling the mode left the slot behind so
// "single player" still ran two avatars. Deriving it makes both states
// unrepresentable. Turning co-op off therefore means destroying the extra
// slots; see game::remove_extra_player_slots().
bool mp_coop_active();

#endif // CATA_SRC_MP_MP_MODE_H
