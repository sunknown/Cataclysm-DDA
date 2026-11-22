#include "cata_catch.h"

#if defined(MP_SERVER)

#include <string>
#include <vector>

#include "color.h"
#include "cursesdef.h"
#include "fake_messages.h"
#include "mp/mp_protocol.h"
#include "cursesport.h"
#include "log_snapshot.h"
#include "messages.h"
#include "mp/mp_log.h"
#include "output.h"

// Phase 4b-1: the wire and the glyph path must resolve inline colour tags with
// one implementation. split_by_color() cuts at each tag, so a tag segment also
// carries the text that follows it; these tests pin the extracted runs,
// including the trailing-tag empty run that carries the final colour.

TEST_CASE( "mp_log_split_colored_text", "[mp]" )
{
    SECTION( "plain text is one run in the initial colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "hello", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "hello" );
        CHECK( runs[0].color == c_green );
    }

    SECTION( "an empty string produces no runs" ) {
        CHECK( split_colored_text( "", c_green, c_white, report_color_error::no ).empty() );
    }

    SECTION( "an inline colour colours the text after its tag" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<color_red>b</color>c", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 3 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text == "b" );
        CHECK( runs[1].color == c_red );
        CHECK( runs[2].text == "c" );
        CHECK( runs[2].color == c_green );
    }

    SECTION( "an unmatched close tag falls back to the base colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a</color>b", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 2 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text == "b" );
        CHECK( runs[1].color == c_white );
    }

    SECTION( "a trailing tag keeps an empty run with the final colour" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<color_red>", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 2 );
        CHECK( runs[0].text == "a" );
        CHECK( runs[0].color == c_green );
        CHECK( runs[1].text.empty() );
        CHECK( runs[1].color == c_red );
    }

    SECTION( "a non-tag angle bracket stays literal text" ) {
        // split_by_color only cuts at <color_*> and </color>; any other tag
        // syntax is plain text and must survive verbatim.
        const std::vector<colored_text_run> runs = split_colored_text(
                    "a<no_such_tag>b", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "a<no_such_tag>b" );
        CHECK( runs[0].color == c_green );
    }

    SECTION( "an invalid colour name stays literal text" ) {
        const std::vector<colored_text_run> runs = split_colored_text(
                    "<color_not_a_colour>x", c_green, c_white, report_color_error::no );
        REQUIRE( runs.size() == 1 );
        CHECK( runs[0].text == "<color_not_a_colour>x" );
        CHECK( runs[0].color == c_green );
    }
}

// The test binary links tests/fake_messages.cpp instead of src/messages.cpp, so
// the message deque cannot be driven from here. The TTL predicate and the
// selection rules are split out for that reason; the real snapshot's adapter is
// four field reads and is covered by the live pass.

TEST_CASE( "mp_log_ttl_predicate", "[mp]" )
{
    CHECK( Messages::log_exceeds_ttl( 0, 5, 5 ) );
    CHECK_FALSE( Messages::log_exceeds_ttl( 0, 5, 4 ) );
    CHECK_FALSE( Messages::log_exceeds_ttl( 0, 0, 100 ) );
    // A timestamp from the future is not "old"; the subtraction is ordered.
    CHECK_FALSE( Messages::log_exceeds_ttl( 10, 5, 5 ) );
}

TEST_CASE( "mp_log_snapshot_build", "[mp]" )
{
    SECTION( "newest first, and the cap keeps the newest" ) {
        const std::vector<log_source_entry> entries = {
            { "newest", c_white, true },
            { "middle", c_white, true },
            { "oldest", c_white, true },
        };
        const std::vector<Messages::log_message> snap =
            log_snapshot_build( entries, 2, false );
        REQUIRE( snap.size() == 2 );
        CHECK( snap[0].runs[0].text == "newest" );
        CHECK( snap[1].runs[0].text == "middle" );
    }

    SECTION( "an old entry loses its tags and takes its colour" ) {
        const std::vector<log_source_entry> entries = {
            { "You hit the <color_red>zombie</color> for 5 damage.", c_dark_gray, false },
        };
        const std::vector<Messages::log_message> snap =
            log_snapshot_build( entries, 200, false );
        REQUIRE( snap.size() == 1 );
        REQUIRE( snap[0].runs.size() == 1 );
        CHECK( snap[0].runs[0].text == "You hit the zombie for 5 damage." );
        CHECK( snap[0].runs[0].color == c_dark_gray );
    }

    SECTION( "a recent entry keeps its inline runs" ) {
        const std::vector<log_source_entry> entries = {
            { "You hit the <color_red>zombie</color> for 5 damage.", c_white, true },
        };
        const std::vector<Messages::log_message> snap =
            log_snapshot_build( entries, 200, false );
        REQUIRE( snap.size() == 1 );
        REQUIRE( snap[0].runs.size() == 3 );
        CHECK( snap[0].runs[0].text == "You hit the " );
        CHECK( snap[0].runs[0].color == c_white );
        CHECK( snap[0].runs[1].text == "zombie" );
        CHECK( snap[0].runs[1].color == c_red );
        CHECK( snap[0].runs[2].text == " for 5 damage." );
    }

    SECTION( "cooldown-hidden is dropped only for new_bottom" ) {
        const std::vector<log_source_entry> entries = {
            { "newest", c_white, true, false, false },
            { "hidden", c_white, true, true, false },
            { "older", c_white, true, false, false },
        };

        const std::vector<Messages::log_message> bottom =
            log_snapshot_build( entries, 200, false );
        REQUIRE( bottom.size() == 2 );
        CHECK( bottom[0].runs[0].text == "newest" );
        CHECK( bottom[1].runs[0].text == "older" );

        const std::vector<Messages::log_message> top =
            log_snapshot_build( entries, 200, true );
        REQUIRE( top.size() == 3 );
        CHECK( top[1].runs[0].text == "hidden" );
    }

    SECTION( "the first expired entry stops the scan" ) {
        const std::vector<log_source_entry> entries = {
            { "newest", c_white, true, false, false },
            { "expired", c_white, true, false, true },
            { "older", c_white, true, false, false },
        };
        const std::vector<Messages::log_message> snap =
            log_snapshot_build( entries, 200, false );
        REQUIRE( snap.size() == 1 );
        CHECK( snap[0].runs[0].text == "newest" );
    }

    SECTION( "a tag-only entry produces no line" ) {
        const std::vector<log_source_entry> entries = {
            { "<color_red>", c_white, true },
            { "visible", c_white, true },
        };
        const std::vector<Messages::log_message> snap =
            log_snapshot_build( entries, 200, false );
        REQUIRE( snap.size() == 1 );
        CHECK( snap[0].runs[0].text == "visible" );
    }

    SECTION( "a zero cap returns nothing" ) {
        const std::vector<log_source_entry> entries = { { "x", c_white, true } };
        CHECK( log_snapshot_build( entries, 0, false ).empty() );
    }
}

TEST_CASE( "mp_log_palette_index", "[mp]" )
{
    // A spare pair: other tests own the low indices.
    const cata_cursesport::pairs saved = cata_cursesport::colorpairs[99];
    cata_cursesport::colorpairs[99].FG = catacurses::cyan;

    const nc_color color = nc_color::from_color_pair_index( 99 );
    CHECK( nc_color_foreground_index( color ) ==
           static_cast<uint8_t>( catacurses::cyan ) );
    CHECK( nc_color_foreground_index( color.bold() ) ==
           static_cast<uint8_t>( catacurses::cyan ) + 8 );

    cata_cursesport::colorpairs[99] = saved;
}

TEST_CASE( "mp_log_source_rect", "[mp]" )
{
    mp_native_log_clear_rect();
    CHECK_FALSE( mp_native_log_ui_active() );

    mp_scene_rect rect;
    CHECK_FALSE( mp_native_log_source_rect( rect ) );

    mp_native_log_note_ui();
    CHECK( mp_native_log_ui_active() );

    mp_native_log_note_rect( 134, 2, 26, 33 );
    REQUIRE( mp_native_log_source_rect( rect ) );
    CHECK( rect.x == 134 );
    CHECK( rect.y == 2 );
    CHECK( rect.w == 26 );
    CHECK( rect.h == 33 );

    // A zero area is "no source panel", not a rect at the origin.
    mp_native_log_note_rect( 1, 2, 0, 5 );
    CHECK_FALSE( mp_native_log_source_rect( rect ) );

    mp_native_log_clear_rect();
    CHECK_FALSE( mp_native_log_ui_active() );
    CHECK_FALSE( mp_native_log_source_rect( rect ) );
}

TEST_CASE( "mp_log_body", "[mp]" )
{
    const cata_cursesport::pairs saved = cata_cursesport::colorpairs[98];
    cata_cursesport::colorpairs[98].FG = catacurses::magenta;

    Messages::log_message first;
    first.runs.push_back( { "alpha ", nc_color::from_color_pair_index( 98 ) } );
    first.runs.push_back( { "beta", nc_color::from_color_pair_index( 98 ).bold() } );
    Messages::log_message second;
    second.runs.push_back( { "gamma", c_white } );
    set_fake_sidebar_snapshot( { first, second } );

    const mp_scene_rect rect{ 134, 2, 26, 33 };
    const std::string body = mp_log_body( false, true, rect, 7 );

    mp_control got;
    REQUIRE( mp_control_from_json( body, got ) );
    CHECK( got.kind == mp_control_kind::log );
    CHECK( got.log.seq == 7 );
    CHECK( got.log.ui );
    CHECK( got.log.x == 134 );
    CHECK( got.log.y == 2 );
    CHECK( got.log.w == 26 );
    CHECK( got.log.h == 33 );
    CHECK_FALSE( got.log.top );
    REQUIRE( got.log.lines.size() == 2 );
    REQUIRE( got.log.lines[0].runs.size() == 2 );
    CHECK( got.log.lines[0].runs[0].text == "alpha " );
    CHECK( got.log.lines[0].runs[0].color ==
           static_cast<uint8_t>( catacurses::magenta ) );
    CHECK( got.log.lines[0].runs[1].color ==
           static_cast<uint8_t>( catacurses::magenta ) + 8 );

    cata_cursesport::colorpairs[98] = saved;
}

TEST_CASE( "mp_log_hash", "[mp]" )
{
    Messages::log_message line;
    line.runs.push_back( { "same", c_white } );
    set_fake_sidebar_snapshot( { line } );

    const mp_scene_rect rect{ 0, 0, 0, 0 };
    const std::string first = mp_log_body( false, true, rect, 1 );
    const std::string second = mp_log_body( false, true, rect, 2 );
    CHECK( first != second );
    CHECK( mp_log_hash( first ) == mp_log_hash( second ) );

    line.runs[0].text = "changed";
    set_fake_sidebar_snapshot( { line } );
    CHECK( mp_log_hash( first ) != mp_log_hash( mp_log_body( false, true, rect, 1 ) ) );
}

TEST_CASE( "mp_log_body_caps", "[mp]" )
{
    const mp_scene_rect rect{ 0, 0, 0, 0 };

    SECTION( "at most 200 lines, newest kept" ) {
        std::vector<Messages::log_message> lines;
        for( int i = 0; i < 201; i++ ) {
            Messages::log_message line;
            line.runs.push_back( { std::to_string( i ), c_white } );
            lines.push_back( line );
        }
        set_fake_sidebar_snapshot( std::move( lines ) );

        mp_control got;
        REQUIRE( mp_control_from_json( mp_log_body( false, true, rect, 1 ), got ) );
        CHECK( got.log.lines.size() == 200 );
        CHECK( got.log.lines[0].runs[0].text == "0" );
    }

    SECTION( "runs past 256 fold into the last kept run" ) {
        Messages::log_message line;
        for( int i = 0; i < 257; i++ ) {
            line.runs.push_back( { std::to_string( i ), c_white } );
        }
        set_fake_sidebar_snapshot( { line } );

        mp_control got;
        REQUIRE( mp_control_from_json( mp_log_body( false, true, rect, 1 ), got ) );
        REQUIRE( got.log.lines.size() == 1 );
        REQUIRE( got.log.lines[0].runs.size() == 256 );
        CHECK( got.log.lines[0].runs.back().text == "255256" );
    }

    SECTION( "64 KiB truncates at a UTF-8 boundary" ) {
        Messages::log_message line;
        std::string text( 65535, 'x' );
        text += "\xC3\xA9";  // e-acute; a raw cut at 65536 would split it
        line.runs.push_back( { text, c_white } );
        set_fake_sidebar_snapshot( { line } );

        mp_control got;
        REQUIRE( mp_control_from_json( mp_log_body( false, true, rect, 1 ), got ) );
        REQUIRE( got.log.lines.size() == 1 );
        REQUIRE( got.log.lines[0].runs.size() == 1 );
        CHECK( got.log.lines[0].runs[0].text.size() == 65535 );
    }
}

#endif // MP_SERVER
