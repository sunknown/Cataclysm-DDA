#pragma once
#ifndef CATA_SRC_MP_MP_SESSION_UI_H
#define CATA_SRC_MP_MP_SESSION_UI_H

#if defined(MP_SERVER)

struct mp_session;

// Phase 4b-2: the per-seat in-game UI state. The process-wide flag this
// replaces was true for whichever seat last drew, which is the wrong answer
// for a creation or replacement seat sharing the process with a playing one.
// These are the only writers of mp_session::ui_active.

// draw_panels ran for this seat: an in-game screen is up.
void mp_session_ui_enter( mp_session &s );

// The seat is blank, in the join menu, or in character creation: no in-game
// UI, no hover request to keep, and no interval carried into the next entry.
void mp_session_ui_leave( mp_session &s );

bool mp_session_ui_active( const mp_session &s );

// The hover describe feed: the client asked for it and the seat is in game.
bool mp_session_can_describe( const mp_session &s );

#endif // MP_SERVER

#endif // CATA_SRC_MP_MP_SESSION_UI_H
