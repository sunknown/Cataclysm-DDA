#pragma once
#ifndef CATA_SRC_MP_MP_MODE_H
#define CATA_SRC_MP_MP_MODE_H

// Whether the current session is a co-op session. False in every single
// player session, including in a build that has multiplayer compiled in.
bool mp_mode_active();
void mp_mode_set_active( bool active );

#endif // CATA_SRC_MP_MP_MODE_H
