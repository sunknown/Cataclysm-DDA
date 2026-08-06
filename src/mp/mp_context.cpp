#include "mp/mp_context.h"

#include <cassert>

#include "cata_utility.h"
#include "game.h"
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
