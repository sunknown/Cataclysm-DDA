#pragma once
#ifndef CATA_SRC_MP_MP_LOG_H
#define CATA_SRC_MP_MP_LOG_H

#include <cstdint>
#include <string>

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

// Drops the rect: join and waiting screens, character creation, and a replaced
// seat all leave no source panel for the log to point at. Does not touch the
// seat's UI state; that is mp_session_ui's to own.
void mp_native_log_clear_rect();

// The current sidebar layout has a Log panel that is toggled on and renders.
// False when the layout has no Log panel.
bool mp_native_log_panel_enabled();

// Builds the log control body (JSON, the control kind included) from the newest
// Messages::sidebar_snapshot(200): palette bytes, the rect/UI/flow fields, and
// the wire limits. Never empty; a client that did not ask for the log never
// calls this.
std::string mp_log_body( bool from_top, bool ui, const mp_scene_rect &rect, uint32_t seq );

// FNV-1a 64 over the body with the digits after "seq": replaced by nothing, so
// an unchanged log hashes equal across sequence numbers.
uint64_t mp_log_hash( const std::string &body );

#endif // CATA_SRC_MP_MP_LOG_H
