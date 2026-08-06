#pragma once

#include <cstddef>
#include <deque>

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
