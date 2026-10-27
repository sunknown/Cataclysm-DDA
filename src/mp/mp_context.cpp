#include "mp/mp_context.h"

#include <cassert>

#include "game.h"
#include "game_constants.h"
#include "game_ui.h"
#include "mp/mp_sched.h"
#include "output.h"
#include "panels.h"
#include "popup.h" // static_popup's complete definition, for wait_popup

// Out-of-line so the compiler-generated bodies (including the exception-
// unwind logic the constructor may need) are emitted only here, where
// static_popup is complete -- see the comment on the declarations.
player_context::player_context() = default;
player_context::~player_context() = default;

static player_context *active_context = nullptr;

// True while the world phase is active. The active context pointer is preserved
// during this phase for code that reads it (serialization, debug UI), so this
// flag is the way to distinguish "a player acts" from "the world runs".
static bool g_world_phase = true; // start in world phase before any player activates

bool mp_in_world_phase()
{
    return g_world_phase;
}

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
    cata_assert( idx < g->contexts.size() );
    g_world_phase = false;
    player_context &ctx = g->contexts[idx];
    ctx.slot_index = idx;

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

void mp_sync_context_count()
{
    const size_t want = g->player_slot_count();
    cata_assert( want >= 1 );
    // A shrink can free the element the active pointer names, so drop the
    // pointer first and re-point below.
    const size_t active = g->contexts.empty() ? 0 : g->active_slot();
    while( g->contexts.size() < want ) {
        g->contexts.emplace_back();
        player_context &fresh = g->contexts.back();
        fresh.slot_index = g->contexts.size() - 1;
        // Until this player's client reports its own terminal, borrow slot 0's:
        // a context with a zero-sized viewport would compute degenerate metrics.
        fresh.viewport = g->contexts[0].viewport;
        // Not a copy of the root window: the same window. Every context draws
        // into one stdscr, and that is safe as long as two things hold.
        //  - Only the baton holder (or, at teardown, the unwind-right holder)
        //    draws, so the window's cells and its per-line `touched` flags have
        //    one writer at a time.
        //  - A draw reaches a grid at refresh time, not at flush time:
        //    curses_drawwindow() copies the touched lines into the grid of the
        //    session active at that moment -- the drawing player's own -- and
        //    clears the flags. The UI redraws and refreshes before it blocks
        //    for input, which is where the baton leaves, so no touched line of
        //    one player's is left for the next player's refresh to carry into
        //    their grid. Each grid then keeps its own copy of whatever was last
        //    refreshed into it, which is all a client ever sees.
        // Checked by two_players_sit_in_different_heavy_menus: six full-screen
        // menus drawn into this one window, pairwise, and neither player's grid
        // carries the other's menu. What it does NOT cover is clients of
        // different sizes: one root sized for one terminal is wrong for the
        // other, so per-client terminal sizes need a root per context.
        fresh.stdscr = g->contexts[0].stdscr;
    }
    while( g->contexts.size() > want ) {
        // A context whose ui_stack is non-empty still has live ui_adaptors
        // whose owner points at it. Popping it out from under them (reachable
        // from toggle_coop_slot(), debug_menu.cpp) would leave owner dangling;
        // state the invariant here, loudly and locally, rather than let it
        // manifest later as a dangling-pointer read inside ~ui_adaptor().
        cata_assert( g->contexts.back().ui_stack.empty() );
        g->contexts.pop_back();
    }
    mp_activate_player( active < want ? active : 0 );
}

void mp_activate_world()
{
    g_world_phase = true;
    // Bring slot 0's avatar back into u before leaving the player phase. This
    // mirrors step 1 of mp_activate_player( 0 ) -- the avatar swap only, not
    // the screen/window/ui-stack steps, which must stay pointed at the world
    // stack rather than a player's. Without this, if the player loop's last
    // iteration left u on a different slot (do_turn()'s second call site does,
    // at N > 1), the world half would see two different players: g->u (last
    // slot) and mp_active_context() (forced to slot 0 below). mon_info_update()
    // is the proven case -- it reads get_avatar() to rebuild the shared
    // monster-visibility cache -- and roughly two dozen other call sites read
    // one alias or the other without knowing which phase they are in.
    const size_t current = g->active_slot();
    if( current != 0 ) {
        g->swap_slot_into_u( current );
    }
    g->set_active_slot( 0 );
    cata_assert( g->player_slots[g->active_slot()] == &g->u );

    // No player is acting. In a session -- player threads, or more than one
    // slot -- the ui stack becomes the world stack, so UI raised here lands
    // somewhere real instead of in a player's window set while that player may
    // be mid-menu on their own thread.
    //
    // Single-player keeps its one player's stack, as the game did before the
    // turn was split: there is one screen and nobody else to protect it from.
    // The world half draws there (shots and explosions over the map, an NPC's
    // dialogue) and NPC activity code resets that player's progress popup from
    // here, which must happen under the stack the popup registered in.
    //
    // The active context is NOT cleared, for the same reason the avatar aliases
    // are not: world code that still reads them must see live state rather than
    // a dangling one. Serialization is the proven case -- the autosave inside
    // world_turn_pre() writes mp_active_context().driving_view_offset -- and
    // roughly two dozen other call sites read the context without knowing which
    // phase they are in. "World phase" means nobody is acting, not that the
    // per-player state ceases to exist, so it stays on slot 0.
    if( mp_active_sched() != nullptr || g->player_slot_count() > 1 ) {
        ui_manager::activate_world_stack();
    } else {
        ui_manager::set_active_stack( &g->contexts[0].ui_stack );
    }
    mp_set_active_context( &g->contexts[0] );
}

void mp_for_each_context( const std::function<void( player_context & )> &fn )
{
    // Capture the actual phase (which ui stack, and which player if any),
    // not just the active slot index: the world phase also rests on slot 0
    // (mp_activate_world() above), so the slot index alone cannot tell a
    // world-phase caller apart from a caller that had activated player 0.
    // Restoring by slot only would activate a player after a world-phase
    // call, silently leaving the world phase.
    ui_stack_t *const restore_stack = ui_manager::get_active_stack();
    bool restore_is_world = true;
    size_t restore_slot = 0;
    for( size_t i = 0; i < g->contexts.size(); ++i ) {
        if( restore_stack == &g->contexts[i].ui_stack ) {
            restore_is_world = false;
            restore_slot = i;
            break;
        }
    }

    for( size_t i = 0; i < g->contexts.size(); ++i ) {
        mp_activate_player( i );
        fn( g->contexts[i] );
    }

    if( restore_is_world ) {
        mp_activate_world();
    } else {
        mp_activate_player( restore_slot );
    }
}
