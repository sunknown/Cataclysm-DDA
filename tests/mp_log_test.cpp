#include "cata_catch.h"

#if defined(MP_SERVER)

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "color.h"
#include "cursesdef.h"
#include "fake_messages.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_session_ui.h"
#include "mp/mp_transport.h"
#include "cursesport.h"
#include "log_snapshot.h"
#include "messages.h"
#include "mp/mp_log.h"
#include "output.h"
#include "panels.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"

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
    // The UI flag is per seat now. Drive it the way draw_panels and the join
    // menu do: through this session's own state.
    mp_session seat;
    mp_native_log_clear_rect();
    CHECK_FALSE( mp_session_ui_active( seat ) );

    mp_scene_rect rect;
    CHECK_FALSE( mp_native_log_source_rect( rect ) );

    mp_session_ui_enter( seat );
    CHECK( mp_session_ui_active( seat ) );

    mp_native_log_note_rect( 134, 2, 26, 33 );
    REQUIRE( mp_native_log_source_rect( rect ) );
    CHECK( rect.x == 134 );
    CHECK( rect.y == 2 );
    CHECK( rect.w == 26 );
    CHECK( rect.h == 33 );

    // A zero area is "no source panel", not a rect at the origin.
    mp_native_log_note_rect( 1, 2, 0, 5 );
    CHECK_FALSE( mp_native_log_source_rect( rect ) );

    // Clearing the rect is not leaving the game: the seat keeps its flag.
    mp_native_log_note_rect( 134, 2, 26, 33 );
    mp_native_log_clear_rect();
    CHECK( mp_session_ui_active( seat ) );
    CHECK_FALSE( mp_native_log_source_rect( rect ) );

    mp_session_ui_leave( seat );
    CHECK_FALSE( mp_session_ui_active( seat ) );
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

namespace
{

// The active-context pointer must not dangle between test cases.
player_context log_test_ctx;

struct wire_frame {
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
};

void reset_log_test_world()
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_activate_player( 0 );

    mp_set_active_context( &log_test_ctx );
    log_test_ctx.slot_index = 0;
    log_test_ctx.input_queue.clear();
    log_test_ctx.w_terrain = catacurses::newwin( 24, 80, point( 0, 0 ) );

    mp_set_active_sched( nullptr );
    mp_sessions_reset_for_test();
    mp_server_reset_for_test();
    // The real server builds the sidebar layouts at game start; without them
    // panel_manager::get_current_layout() recurses on its own fallback.
    panel_manager::get_manager().init();
    mp_native_log_clear_rect();
}

std::shared_ptr<mp_transport> install_log_session()
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    auto session = std::make_unique<mp_session>();
    session->transport = pair.a;
    session->context_index = 0;
    session->grid.resize( 80, 24 );
    mp_sessions().push_back( std::move( session ) );
    return pair.b;
}

std::vector<wire_frame> drain_log_frames( mp_transport &transport )
{
    std::vector<wire_frame> frames;
    while( const std::optional<std::vector<uint8_t>> payload = transport.recv( 0 ) ) {
        wire_frame frame;
        REQUIRE( mp_unwrap( *payload, frame.kind, frame.body ) );
        frames.push_back( std::move( frame ) );
    }
    return frames;
}

std::vector<mp_control> log_controls( const std::vector<wire_frame> &frames )
{
    std::vector<mp_control> controls;
    for( const wire_frame &frame : frames ) {
        if( frame.kind != mp_msg_kind::control ) {
            continue;
        }
        mp_control control;
        if( mp_control_from_json( std::string( frame.body.begin(), frame.body.end() ),
                                  control ) &&
            control.kind == mp_control_kind::log ) {
            controls.push_back( control );
        }
    }
    return controls;
}

} // namespace

TEST_CASE( "mp_log_flush", "[mp]" )
{
    reset_log_test_world();
    const std::shared_ptr<mp_transport> client = install_log_session();
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->caps = 2;

    Messages::log_message line;
    line.runs.push_back( { "hello", c_white } );
    set_fake_sidebar_snapshot( { line } );
    mp_session_ui_enter( *session );
    mp_native_log_note_rect( 134, 2, 26, 33 );

    mp_server_flush_screen();
    std::vector<mp_control> logs = log_controls( drain_log_frames( *client ) );
    REQUIRE( logs.size() == 1 );
    CHECK( logs[0].log.seq == 1 );
    CHECK( logs[0].log.ui );
    CHECK( logs[0].log.x == 134 );
    CHECK( logs[0].log.y == 2 );
    CHECK( logs[0].log.w == 26 );
    CHECK( logs[0].log.h == 33 );
    REQUIRE( logs[0].log.lines.size() == 1 );
    CHECK( logs[0].log.lines[0].runs[0].text == "hello" );

    // The same log on the next flush sends nothing.
    mp_server_flush_screen();
    CHECK( log_controls( drain_log_frames( *client ) ).empty() );

    // A changed message advances the sequence.
    line.runs[0].text = "changed";
    set_fake_sidebar_snapshot( { line } );
    mp_server_flush_screen();
    logs = log_controls( drain_log_frames( *client ) );
    REQUIRE( logs.size() == 1 );
    CHECK( logs[0].log.seq == 2 );

    // A text client never gets one.
    session->caps = 0;
    mp_server_flush_screen();
    CHECK( log_controls( drain_log_frames( *client ) ).empty() );

    // ui=false is the join/creation state: no rect, nothing to place.
    session->caps = 2;
    mp_session_ui_leave( *session );
    mp_native_log_clear_rect();
    mp_server_flush_screen();
    logs = log_controls( drain_log_frames( *client ) );
    REQUIRE( logs.size() == 1 );
    CHECK_FALSE( logs[0].log.ui );
    CHECK( logs[0].log.w == 0 );
}

TEST_CASE( "mp_log_ui_per_seat", "[mp]" )
{
    // The 4b-1 regression: the flag used to be one process-wide static, so a
    // playing seat's draw_panels made a creation seat's snapshot claim an
    // in-game UI. Each seat owns its flag now.
    reset_log_test_world();
    const std::shared_ptr<mp_transport> client = install_log_session();
    mp_session *const creation = mp_session_of_slot( 0 );
    REQUIRE( creation != nullptr );
    creation->caps = 2;

    // A second seat in the same process is playing.
    auto other = std::make_unique<mp_session>();
    other->context_index = 1;
    mp_session *const playing = other.get();
    mp_sessions().push_back( std::move( other ) );

    Messages::log_message line;
    line.runs.push_back( { "hello", c_white } );
    set_fake_sidebar_snapshot( { line } );
    mp_session_ui_enter( *playing );
    mp_session_ui_leave( *creation );
    mp_native_log_note_rect( 134, 2, 26, 33 );

    // The flush runs for the active seat, which is the creation seat.
    mp_server_flush_screen();
    const std::vector<mp_control> logs = log_controls( drain_log_frames( *client ) );
    REQUIRE( logs.size() == 1 );
    CHECK_FALSE( logs[0].log.ui );
}

TEST_CASE( "mp_log_panel_lookup_before_init", "[mp]" )
{
    // A fresh manager has no layouts at all. Before a game starts, a client
    // flush during a load-time prompt can reach the log panel lookup; it must
    // report "no Log panel", not recurse through its own classic fallback.
    panel_manager fresh;
    CHECK( fresh.get_current_layout().panels().empty() );
}

TEST_CASE( "resolve_actor_text", "[mp]" )
{
    // The actor keeps the player form even after a rename: stable id matches.
    CHECK( resolve_actor_text( "You drop your rock.",
                               "<npcname> drops their rock.",
                               "Katelynn Burdette", "Kate", "Katelynn Burdette" )
           == "You drop your rock." );

    // Another seat reads the frozen display name, all placeholders replaced.
    CHECK( resolve_actor_text( "You see the rock.",
                               "<npcname> sees the rock; <npcname> smiles.",
                               "Katelynn Burdette", "Kate", "Emory Mullen" )
           == "Kate sees the rock; Kate smiles." );

    // No third-person form, no actor, or no display name: the primary stays.
    CHECK( resolve_actor_text( "You hear thunder.", "",
                               "Katelynn Burdette", "Kate", "Emory Mullen" )
           == "You hear thunder." );
    CHECK( resolve_actor_text( "You hear thunder.", "<npcname> hears thunder.",
                               "", "", "Emory Mullen" )
           == "You hear thunder." );
    CHECK( resolve_actor_text( "You drop your rock.", "<npcname> drops their rock.",
                               "Katelynn Burdette", "", "Emory Mullen" )
           == "You drop your rock." );

    // A name with punctuation is substituted literally.
    CHECK( resolve_actor_text( "You drop.", "<npcname> drops.",
                               "Rose 'Ragtag' Williamson", "Rose 'Ragtag' Williamson",
                               "Emory Mullen" )
           == "Rose 'Ragtag' Williamson drops." );
}

TEST_CASE( "character_pair_records_actor", "[mp]" )
{
    // A local avatar: the hook only reads the names, and mutating the global
    // one leaks the name into later [mp] cases that run in suite order.
    avatar you;
    you.name = "Katelynn Burdette";
    you.play_name = "Kate";
    clear_fake_message_pairs();

    you.add_msg_player_or_npc( "You drop your rock.",
                               "<npcname> drops their rock." );

    REQUIRE( fake_message_pairs().size() == 1 );
    const FakeMessagePair &pair = fake_message_pairs().front();
    CHECK( pair.player == "You drop your rock." );
    CHECK( pair.npc == "<npcname> drops their rock." );
    CHECK( pair.actor_id == "Katelynn Burdette" );
    CHECK( pair.actor_display == "Kate" );
}

#endif // MP_SERVER
