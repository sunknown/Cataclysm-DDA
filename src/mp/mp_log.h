#pragma once
#ifndef CATA_SRC_MP_MP_LOG_H
#define CATA_SRC_MP_MP_LOG_H

#include "mp/mp_scene.h"

// Phase 4b-1: the native log's source panel and UI state. The server records
// what it drew; the client decides where to place its own log. The rect is a
// hint, not a contract: a zero area means "no source panel".

// The absolute cell rect of the window draw_messages just painted. The server
// keeps drawing its text; the client suppresses these cells once it has a
// snapshot.
void mp_native_log_note_rect( int x, int y, int w, int h );

// The last recorded rect; false when there is none. Does not consult the panel
// layout: mp_server_flush_screen combines this with mp_native_log_panel_enabled
// so the zero-rect decision has one owner.
bool mp_native_log_source_rect( mp_scene_rect &out );

// Drops the rect and marks the UI inactive: join and waiting screens, character
// creation, and a replaced seat all leave no game UI the log could belong to.
void mp_native_log_clear_rect();

// The current sidebar layout has a Log panel that is toggled on and renders.
// False when the layout has no Log panel.
bool mp_native_log_panel_enabled();

// The game's main UI was drawn since the last clear. draw_panels calls this
// every frame, so a toggled-off Log panel is still an in-game UI.
void mp_native_log_note_ui();
bool mp_native_log_ui_active();

#endif // CATA_SRC_MP_MP_LOG_H
