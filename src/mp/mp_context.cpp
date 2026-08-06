#include "mp/mp_context.h"

#include <cassert>

#include "game.h"
#include "game_constants.h"
#include "game_ui.h"
#include "output.h"
#include "panels.h"

static player_context *active_context = nullptr;

player_context &mp_active_context()
{
    cata_assert( active_context != nullptr );
    return *active_context;
}

player_context *mp_active_context_ptr()
{
    return active_context;
}

void mp_set_active_context( player_context *ctx )
{
    // nullptr is a legal reset (needed by tests and, in Phase 3, by leaving a player);
    // reading through mp_active_context() asserts a non-null pointer.
    active_context = ctx;
}

void mp_apply_viewport_params( const player_context &ctx )
{
    // The base TERMX/TERMY are already set by game_ui::init_ui() before any
    // callback; the context is filled from them (task 3), so these writes
    // are identical in Phase 2 and will become real in Phase 3, when the terminal
    // size will follow the active player.
    TERMX = ctx.viewport.term_cols;
    TERMY = ctx.viewport.term_rows;

    const int sidebar_left = panel_manager::get_manager().get_width_left();
    const int sidebar_right = panel_manager::get_manager().get_width_right();

    TERRAIN_WINDOW_HEIGHT = TERMY;
    TERRAIN_WINDOW_WIDTH = TERMX - ( sidebar_left + sidebar_right );
    TERRAIN_WINDOW_TERM_WIDTH = TERRAIN_WINDOW_WIDTH;
    TERRAIN_WINDOW_TERM_HEIGHT = TERRAIN_WINDOW_HEIGHT;

    // The terrain window is measured in map tiles; the terminal and map fonts may
    // differ. In the server build there is no recalculation: the units are already
    // accounted for by whoever filled viewport_params (the client), and
    // to_map_font_dimension would read font globals, which the server does not have.
#if !defined(MP_SERVER)
    to_map_font_dimension( TERRAIN_WINDOW_WIDTH, TERRAIN_WINDOW_HEIGHT );
#endif

    POSX = TERRAIN_WINDOW_WIDTH / 2;
    POSY = TERRAIN_WINDOW_HEIGHT / 2;
}

void mp_ensure_player_context()
{
    if( g->contexts.empty() ) {
        g->contexts.emplace_back();
    }
    player_context &ctx = g->contexts[0];
    // TERMX/TERMY are set by game_ui::init_ui() before any callbacks; the context
    // mirrors them (the source is init_ui, not get_terminal_width, which in the TUI
    // reads the TERMINAL_X option rather than the real terminal size).
    ctx.viewport.term_cols = TERMX;
    ctx.viewport.term_rows = TERMY;
    mp_apply_viewport_params( ctx );
    ctx.stdscr = catacurses::stdscr;
    mp_set_active_context( &ctx );
    // Activating player 0 here makes the context mechanism run through the
    // ordinary single-player build: the main ui_adaptor registers into the
    // context's stack instead of world_stack, and every alias points at the
    // context. The first redraw then runs mp_resize_active_context, which
    // creates the windows and re-points the aliases again (resize-before-
    // activation ordering, task 3).
    mp_activate_player( 0 );
}

void mp_activate_player( size_t idx )
{
    // No ensure here: mp_ensure_player_context() itself ends with
    // mp_activate_player( 0 ), otherwise this would recurse forever. Callers
    // must have already called ensure (setup() / create_or_get_main_ui_adaptor
    // / tests).
    cata_assert( !g->contexts.empty() );
    // In Phase 2 there is a single context (hotseat shares it between slots);
    // per-player contexts (contexts[idx]) arrive with the scheduler in Phase 3.
    player_context &ctx = g->contexts[0];

    // 1. Avatar: take the current slot out of u, put the new one in.
    // swap_slot_into_u is self-inverse and only valid as a swap-in/swap-out
    // pair, so it is private and is called only from here.
    const size_t current = g->active_slot();
    if( current != 0 ) {
        g->swap_slot_into_u( current );
    }
    if( idx != 0 ) {
        g->swap_slot_into_u( idx );
    }
    g->set_active_slot( idx );
    cata_assert( g->player_slots[g->active_slot()] == &g->u );

    // 2. Screen.
    catacurses::stdscr = ctx.stdscr;
    // 3. Windows.
    g->w_terrain = ctx.w_terrain;
    g->w_overmap = ctx.w_overmap;
    g->w_omlegend = ctx.w_omlegend;
    g->w_minimap = ctx.w_minimap;
    g->w_pixel_minimap = ctx.w_pixel_minimap;
    g->w_terrain_ptr = ctx.w_terrain_ptr;
    g->w_minimap_ptr = ctx.w_minimap_ptr;
    // 4. Metrics.
    mp_apply_viewport_params( ctx );
    // 5. UI stack.
    ui_manager::set_active_stack( &ctx.ui_stack );
    // 6. View state and input.
    mp_set_active_context( &ctx );
}

void mp_resize_active_context()
{
    player_context &ctx = mp_active_context();

    // Re-capture the backend's fresh root: backends that replace stdscr on
    // resize (server, SDL) leave the context's stored window at the old size.
    ctx.stdscr = catacurses::stdscr;

    ctx.viewport.term_cols = TERMX;
    ctx.viewport.term_rows = TERMY;
    mp_apply_viewport_params( ctx );

    const int sidebar_left = panel_manager::get_manager().get_width_left();

    ctx.w_terrain = ctx.w_terrain_ptr = catacurses::newwin(
                       TERRAIN_WINDOW_HEIGHT, TERRAIN_WINDOW_WIDTH, point( sidebar_left, 0 ) );
    ctx.w_minimap = ctx.w_minimap_ptr = catacurses::newwin(
                        MINIMAP_HEIGHT, MINIMAP_WIDTH, point::zero );
    ctx.w_pixel_minimap = catacurses::newwin( 1, 1, point::zero );

    // Re-point game's window aliases to the active context's windows.
    catacurses::stdscr = ctx.stdscr;
    g->w_terrain = ctx.w_terrain;
    g->w_overmap = ctx.w_overmap;
    g->w_omlegend = ctx.w_omlegend;
    g->w_minimap = ctx.w_minimap;
    g->w_pixel_minimap = ctx.w_pixel_minimap;
    g->w_terrain_ptr = ctx.w_terrain_ptr;
    g->w_minimap_ptr = ctx.w_minimap_ptr;

    if( const shared_ptr_fast<ui_adaptor> ui = ctx.main_ui_adaptor.lock() ) {
        ui->position_from_window( ctx.stdscr );
    }
}
