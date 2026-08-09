#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>

#include "coordinates.h"    // tripoint_bub_ms, point_rel_ms
#include "cursesdef.h"      // catacurses::window
#include "input_enums.h"    // input_event
#include "ui_manager.h"     // ui_stack_t, ui_adaptor
#include "memory_fast.h"    // weak_ptr_fast

struct viewport_params {
    int term_cols = 0, term_rows = 0;
    int font_units = 1, tile_units = 1;
    int font_units_v = 1, tile_units_v = 1;
};

struct player_context {
    size_t slot_index = 0;
    viewport_params viewport;
    catacurses::window stdscr;
    catacurses::window w_terrain, w_overmap, w_omlegend, w_minimap, w_pixel_minimap;
    catacurses::window w_terrain_ptr, w_minimap_ptr;
    tripoint_bub_ms ter_view_p;
    point_rel_ms driving_view_offset;
    std::deque<input_event> input_queue;
    int input_timeout_ms = -1;
    ui_stack_t ui_stack;
    weak_ptr_fast<ui_adaptor> main_ui_adaptor;
    // Timestamp of this player's last key-poll rate-limit check (player_act()).
    // Lives per player rather than as a function-local static: a shared static
    // let one idle player's poll reset the timer for another (slot 0 could
    // starve slot 1's handle_key_blocking_activity() for up to 100 ms), and it
    // would race once player_act() runs on per-player threads.
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds> last_key_poll =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );
};

player_context &mp_active_context();       // asserts a non-null active context
player_context *mp_active_context_ptr();   // raw pointer, may be nullptr (for reset/tests)
void mp_set_active_context( player_context *ctx ); // nullptr = reset; reading asserts

void mp_apply_viewport_params( const player_context &ctx );

// Creates game::contexts[0] if needed, fills its viewport from TERMX/TERMY,
// mirrors catacurses::stdscr, applies the viewport params and makes the context
// active. Always sets an active context, even when contexts was already non-empty
// (otherwise after contexts.clear() the active pointer would dangle).
void mp_ensure_player_context();
// The single writer of the global aliases: swaps the current slot out of u and
// slot idx in, then re-points stdscr, the seven game windows, the TERMX/TERMY
// metrics, the active ui stack and the active player context from contexts[0].
// Callers must have called mp_ensure_player_context() first (which itself ends
// with mp_activate_player( 0 )).
void mp_activate_player( size_t idx );
// Re-reads the viewport from TERMX/TERMY, applies the metrics, creates the
// context's windows, re-points game's window aliases to them and repositions
// the main ui_adaptor from the context's stdscr.
void mp_resize_active_context();

// Grows or shrinks game::contexts to match game::player_slot_count(). New
// contexts inherit the terminal size of context 0, which is what a freshly
// seeded slot wants until its client says otherwise. Callers that add or
// remove a slot must call this; the active context is re-pointed afterwards
// because a shrink can invalidate it.
void mp_sync_context_count();

// Enters the world phase: nobody is acting. The ui stack becomes the world
// stack (a real object, not nullptr -- it catches the emergency UI that
// debugmsg raises).
//
// The avatar aliases and the active context both stay on slot 0, so any code
// that still reads them sees live state rather than a dangling or null one.
// The world half genuinely reaches such code -- the autosave in
// world_turn_pre() serializes mp_active_context().driving_view_offset -- and
// the phase is about who may act, not about which state exists.
void mp_activate_world();

// Runs `fn` once per player context, activating each in turn and restoring the
// previously active one afterwards. Used by the broadcast paths: world-side
// code that has to draw into every player's screen (explosions, animations).
void mp_for_each_context( const std::function<void( player_context & )> &fn );
