#if defined( MP_SERVER )

#include "cata_catch.h"

#include "cata_imgui.h"
#include "color.h"
#include "cursesdef.h"

TEST_CASE( "the_in_memory_curses_port_feeds_the_imgui_colour_table", "[mp]" )
{
    // The palette arrays are loaded by the imgui client in the game; a unit
    // test has no client, so load them explicitly before asking for a colour.
    cataimgui::load_colors();
    catacurses::init_pair( 90, catacurses::white, catacurses::black );
    const nc_color colour = nc_color::from_color_pair_index( 90 );
    // The imgui conversion reads the pair index off the colour; the in-memory
    // curses port must carry it, not just the attribute bits.
    CHECK( colour.get_index() == 90 );
    const ImVec4 rgba = cataimgui::imvec4_from_color( colour );
    CHECK( rgba.x > 0.5f );
    CHECK( rgba.y > 0.5f );
    CHECK( rgba.z > 0.5f );
    // Bold and blink rebuild the attribute; they must carry the pair index too,
    // or a highlighted row loses its colour again.
    CHECK( colour.bold().get_index() == 90 );
    CHECK( colour.blink().get_index() == 90 );
    CHECK( cataimgui::imvec4_from_color( colour.bold() ).x > 0.5f );
}

#endif // MP_SERVER
