#include "mp/mp_join_menu.h"

#include <algorithm>

#include "avatar.h"
#include "color.h"
#include "coordinates.h"
#include "cursesdef.h"
#include "enums.h"
#include "filesystem.h"
#include "help.h"
#include "localized_comparator.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "point.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "uilist.h"

// Names and save ids taken by players of the current join phase. Only touched
// by whoever holds the baton, so it needs no lock of its own.
static std::vector<std::string> g_claimed;

void mp_join_reset_claims()
{
    g_claimed.clear();
}

static bool claimed( const std::string &name )
{
    return std::find( g_claimed.begin(), g_claimed.end(), name ) != g_claimed.end();
}

std::optional<save_t> mp_continue_target( const std::vector<save_t> &world_saves,
        const std::string &card_save_id )
{
    if( card_save_id.empty() ) {
        return std::nullopt;
    }
    for( const save_t &save : world_saves ) {
        if( save.decoded_name() == card_save_id ) {
            return save;
        }
    }
    return std::nullopt;
}

// The templates in the server's template folder, as the main menu lists them.
static bool pick_template( std::string &out )
{
    std::vector<std::string> templates;
    for( std::string path : get_files_from_path( ".template", PATH_INFO::templatedir(), false,
            true ) ) {
        path.erase( path.find( ".template" ), std::string::npos );
        path.erase( 0, path.find_last_of( "\\/" ) + 1 );
        templates.push_back( path );
    }
    if( templates.empty() ) {
        popup( _( "No templates found." ) );
        return false;
    }
    std::sort( templates.begin(), templates.end(), localized_compare );
    uilist menu( _( "Preset character" ), templates );
    menu.query();
    if( menu.ret < 0 || static_cast<size_t>( menu.ret ) >= templates.size() ) {
        return false;
    }
    out = templates[menu.ret];
    return true;
}

// Whether `name` can be given to a new character in this session.
static bool name_is_free( const std::string &name )
{
    return !name.empty() && !claimed( name ) &&
           !world_generator->active_world->save_exists( save_t::from_save_id( name ) );
}

// Settles the created character's name: kept when free, otherwise the player
// is asked for another until it is free or they back out.
static bool settle_name( avatar &ava )
{
    while( !name_is_free( ava.name ) ) {
        const std::string why = claimed( ava.name ) ?
                                _( "Another player in this session has just taken that name." ) :
                                _( "A survivor with that name already lives in this world." );
        string_input_popup prompt;
        prompt.title( why + " " + _( "Choose another name:" ) ).width( 30 ).text( ava.name );
        const std::string fresh = prompt.query_string();
        if( prompt.canceled() ) {
            return false;
        }
        ava.name = fresh;
    }
    g_claimed.push_back( ava.name );
    return true;
}

mp_join_choice mp_run_join_menu( const std::string &card_save_id )
{
    avatar &ava = get_avatar();
    // A replacement seat enters this menu standing where its character fell.
    // Every reset below builds a blank avatar, whose Creature constructor puts
    // it at the invalid sentinel; left there, a living unplaced seat poisons
    // mp_bubble_anchor() -- and so the map centre and z -- and any other
    // seat's replacement anchor. Keep the position across the resets. The join
    // phase starts at the sentinel with nothing legal to keep, and no world is
    // ticking there to consume it.
    const tripoint_abs_ms last_pos = ava.pos_abs();
    enum { NEW_CUSTOM, NEW_PRESET, NEW_RANDOM, CONTINUE, SETTINGS, HELP, QUIT };

    while( true ) {
        // Rebuilt every time round: another player may have taken this card's
        // character while this one was in a submenu.
        const std::vector<save_t> &saves = world_generator->active_world->world_saves;
        std::optional<save_t> resume = mp_continue_target( saves, card_save_id );
        if( resume && claimed( resume->decoded_name() ) ) {
            resume.reset();
        }

        uilist menu;
        menu.text = string_format( _( "Join the game in \"%s\"" ),
                                   world_generator->active_world->world_name );
        if( resume ) {
            const std::string who = resume->decoded_name();
            menu.entries.emplace_back( CONTINUE, true, 'o',
                                       string_format( _( "Continue as %s" ), who ) );
        }
        menu.entries.emplace_back( NEW_CUSTOM, true, 'c', _( "New character: custom" ) );
        menu.entries.emplace_back( NEW_PRESET, true, 'p', _( "New character: preset" ) );
        menu.entries.emplace_back( NEW_RANDOM, true, 'r', _( "New character: random" ) );
        // Server-side options, and anyone may change them: co-op is a trusted
        // circle, and the one dangerous shared setting (sidebar width) is
        // already closed on the server.
        menu.entries.emplace_back( SETTINGS, true, 's', _( "Settings" ) );
        menu.entries.emplace_back( HELP, true, '?', _( "Help" ) );
        menu.entries.emplace_back( QUIT, true, 'q', _( "Quit" ) );
        menu.query();

        mp_join_choice choice;
        switch( menu.ret ) {
            case NEW_CUSTOM:
            case NEW_PRESET:
            case NEW_RANDOM: {
                const character_type type = menu.ret == NEW_CUSTOM ? character_type::CUSTOM
                                            : menu.ret == NEW_RANDOM ? character_type::RANDOM
                                            : character_type::TEMPLATE;
                std::string tmpl;
                if( type == character_type::TEMPLATE && !pick_template( tmpl ) ) {
                    continue;
                }
                // Into a blank avatar, so nothing of an earlier attempt or of
                // the empty slot survives into the character.
                ava = avatar();
                ava.set_pos_abs_only( last_pos );
                if( !ava.create( type, tmpl ) || !settle_name( ava ) ) {
                    // Backed out. Back to the menu, not into the session with a
                    // half-built character.
                    ava = avatar();
                    ava.set_pos_abs_only( last_pos );
                    continue;
                }
                choice.decision = mp_join_choice::kind::created;
                return choice;
            }
            case CONTINUE:
                // Re-checked after the menu released the baton: the entry was
                // built from the saves as they were before this player's own
                // input wait, and a partner may have claimed the same character
                // in the meantime. Without this, two clients sharing a card can
                // both continue one save.
                if( !resume || claimed( resume->decoded_name() ) ) {
                    continue;
                }
                g_claimed.push_back( resume->decoded_name() );
                choice.decision = mp_join_choice::kind::continued;
                choice.save = resume;
                return choice;
            case SETTINGS:
                get_options().show( false );
                continue;
            case HELP:
                get_help().display_help();
                continue;
            case QUIT:
            default:
                // Escape, a dropped connection and the session ending all land
                // here: a menu's cancel is the only answer any of them gives.
                choice.decision = mp_join_choice::kind::quit;
                return choice;
        }
    }
}

void mp_draw_waiting_screen( const std::vector<std::string> &still_choosing )
{
    catacurses::window w = catacurses::stdscr;
    werase( w );
    std::string text = _( "Waiting for the others to choose their survivors:" );
    for( const std::string &name : still_choosing ) {
        text += "\n  " + name;
    }
    fold_and_print( w, point( 2, 2 ), getmaxx( w ) - 4, c_white, text );
    wnoutrefresh( w );
    catacurses::doupdate();
}
