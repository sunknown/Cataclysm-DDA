#if defined(MP_SERVER)

#include "cata_catch.h"

#include <cstdint>
#include <vector>

#include "cursesdef.h"
#include "mp/mp_context.h"
#include "mp/mp_scene.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_server_screen.h"
#include "point.h"

// mp_server_reset_for_test() clears the active context's input queue, so every
// test installs one first. Static, not a stack local, so the active pointer does
// not dangle between tests.
static player_context test_mask_ctx;

static void reset_mask_test()
{
    mp_set_active_context( &test_mask_ctx );
    test_mask_ctx.input_queue.clear();
    test_mask_ctx.w_terrain = catacurses::window();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 80, 24 );
}

// The map the mask grid covers: 40x10 cells at (10, 0), the same shape the
// server gives the terrain window.
static catacurses::window make_map_window()
{
    catacurses::window win = catacurses::newwin( 10, 40, point( 10, 0 ) );
    test_mask_ctx.w_terrain = win;
    return win;
}

static void blit( const catacurses::window &win, const char *text = "." )
{
    catacurses::mvwprintw( win, point::zero, text );
    catacurses::wrefresh( win );
}

TEST_CASE( "mp_scene_mask_clears_when_terrain_blits", "[mp]" )
{
    reset_mask_test();
    catacurses::window map = make_map_window();
    blit( map );

    mp_scene_mask_note( 12, 2, 3, 2, 0 );
    REQUIRE( mp_scene_mask_rects().size() == 1 );

    blit( map );

    CHECK( mp_scene_mask_rects().empty() );
}

TEST_CASE( "mp_scene_mask_tracks_windows_over_the_map", "[mp]" )
{
    reset_mask_test();
    catacurses::window map = make_map_window();
    blit( map );

    // (5, 2) 10x4 crosses the map's left edge at x = 10, so the mask is the
    // clipped intersection, not the whole window.
    catacurses::window overlay = catacurses::newwin( 4, 10, point( 5, 2 ) );
    blit( overlay, "overlay" );

    const std::vector<mp_scene_rect> rects = mp_scene_mask_rects();
    REQUIRE( rects.size() == 1 );
    CHECK( rects[0].x == 10 );
    CHECK( rects[0].y == 2 );
    CHECK( rects[0].w == 5 );
    CHECK( rects[0].h == 4 );
    CHECK( rects[0].role == 0 );
}

TEST_CASE( "mp_scene_mask_rects_merge_and_keep_roles", "[mp]" )
{
    reset_mask_test();
    catacurses::window map = make_map_window();
    blit( map );

    mp_scene_mask_note( 10, 1, 1, 1, 0 );
    mp_scene_mask_note( 11, 1, 1, 1, 0 );

    catacurses::window log = catacurses::newwin( 1, 2, point( 20, 3 ) );
    mp_scene_mask_register_role( log, 1 );
    blit( log, "lo" );

    const std::vector<mp_scene_rect> rects = mp_scene_mask_rects();
    REQUIRE( rects.size() == 2 );
    // The two adjacent text cells collapse into one run.
    CHECK( rects[0].x == 10 );
    CHECK( rects[0].y == 1 );
    CHECK( rects[0].w == 2 );
    CHECK( rects[0].h == 1 );
    CHECK( rects[0].role == 0 );
    // The log keeps its own role instead of merging with the text.
    CHECK( rects[1].x == 20 );
    CHECK( rects[1].y == 3 );
    CHECK( rects[1].w == 2 );
    CHECK( rects[1].h == 1 );
    CHECK( rects[1].role == 1 );
}

TEST_CASE( "mp_scene_mask_ignores_windows_outside_the_map", "[mp]" )
{
    reset_mask_test();
    catacurses::window map = make_map_window();
    blit( map );

    catacurses::window outside = catacurses::newwin( 3, 5, point( 0, 15 ) );
    blit( outside, "far" );

    CHECK( mp_scene_mask_rects().empty() );
}

TEST_CASE( "mp_scene_mask_hash_changes_with_the_rect_list", "[mp]" )
{
    reset_mask_test();
    catacurses::window map = make_map_window();
    blit( map );

    mp_scene_mask_note( 12, 2, 1, 1, 0 );
    const uint64_t first = mp_scene_mask_hash();
    CHECK( mp_scene_mask_hash() == first );

    // The same list produced again hashes the same.
    blit( map );
    mp_scene_mask_note( 12, 2, 1, 1, 0 );
    CHECK( mp_scene_mask_hash() == first );

    // One more masked cell changes the compressed list, so the hash changes.
    mp_scene_mask_note( 13, 2, 1, 1, 0 );
    CHECK( mp_scene_mask_hash() != first );
}

#endif // MP_SERVER
