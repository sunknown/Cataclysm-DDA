#include "do_turn.h"

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ratio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "action.h"
#include "activity_type.h"
#include "avatar.h"
#include "bionics.h"
#include "cached_options.h"
#include "calendar.h"
#ifdef TILES
#include "cata_imgui.h"
#endif
#include "cata_variant.h"
#include "clzones.h"
#include "coordinates.h"
#include "debug.h"
#include "debug_capture.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "explosion.h"
#include "field.h"
#include "game.h"
#include "game_constants.h"
#include "gamemode.h"
#include "help.h"
#include "input.h"
#include "input_context.h"
#include "item_wakeup.h"
#include "magic_enchantment.h"
#include "map.h"
#include "map_iterator.h"
#include "map_scale_constants.h"
#include "mapbuffer.h"
#include "mapdata.h"
#include "memorial_logger.h"
#include "messages.h"
#include "mission.h"
#include "monster.h"
#include "mp/mp_context.h"
#include "mp/mp_sched.h"
#include "mp/mp_tether.h"
#include "mp/mp_turn.h"
#include "mtype.h"
#include "music.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "player_activity.h"
#include "point.h"
#include "popup.h"
#include "rng.h"
#include "scent_map.h"
#include "sdlsound.h"
#include "simple_pathfinding.h"
#include "sounds.h"
#include "stats_tracker.h"
#include "string_formatter.h"
#include "timed_event.h"
#include "translations.h"
#include "type_id.h"
#include "uilist.h"
#include "ui_manager.h"
#include "units.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "worldfactory.h"

static const activity_id ACT_AUTODRIVE( "ACT_AUTODRIVE" );
static const activity_id ACT_FIRSTAID( "ACT_FIRSTAID" );
static const activity_id ACT_MIGRATION_CANCEL( "ACT_MIGRATION_CANCEL" );
static const activity_id ACT_OPERATION( "ACT_OPERATION" );

static const bionic_id bio_alarm( "bio_alarm" );

static const efftype_id effect_controlled( "controlled" );
static const efftype_id effect_npc_suspend( "npc_suspend" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_sleep( "sleep" );

static const event_statistic_id event_statistic_last_words( "last_words" );

static const json_character_flag json_flag_NO_SCENT( "NO_SCENT" );

static const trait_id trait_HAS_NEMESIS( "HAS_NEMESIS" );

#if defined(__ANDROID__)
extern std::map<std::string, std::list<input_event>> quick_shortcuts_map;
extern bool add_best_key_for_action_to_quick_shortcuts( action_id action,
        const std::string &category, bool back );
#endif

#define dbg(x) DebugLog((x),D_GAME) << __FILE__ << ":" << __LINE__ << ": "

namespace turn_handler
{
bool cleanup_at_end()
{
    avatar &u = get_avatar();
    if( g->uquit == QUIT_DIED || g->uquit == QUIT_SUICIDE ) {
        // Put (non-hallucinations) into the overmap so they are not lost.
        for( monster &critter : g->all_monsters() ) {
            g->despawn_monster( critter );
        }
        // if player has "hunted" trait, remove their nemesis monster on death
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.remove_nemesis();
        }
        // Reset NPC factions and disposition
        g->reset_npc_dispositions();
        // Save the factions', missions and set the NPC's overmap coordinates
        // Npcs are saved in the overmap.
        g->save_factions_missions_npcs(); //missions need to be saved as they are global for all saves.

        // and the overmap, and the local map.
        g->save_maps(); //Omap also contains the npcs who need to be saved.

        //save achievements entry
        g->save_achievements();

        g->death_screen();
        std::chrono::seconds time_since_load =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - g->time_of_last_load );
        std::chrono::seconds total_time_played = g->time_played_at_last_load + time_since_load;
        get_event_bus().send<event_type::game_over>( total_time_played );
        // Struck the save_player_data here to forestall Weirdness
        g->move_save_to_graveyard();
        g->write_memorial_file( g->stats().value_of( event_statistic_last_words )
                                .get<cata_variant_type::string>() );
        get_memorial().clear();
        std::vector<std::string> characters = g->list_active_saves();
        // remove current player from the active characters list, as they are dead
        std::vector<std::string>::iterator curchar = std::find( characters.begin(),
                characters.end(), u.get_save_id() );
        if( curchar != characters.end() ) {
            characters.erase( curchar );
        }

        if( characters.empty() ) {
            bool queryDelete = false;
            bool queryReset = false;

            if( get_option<std::string>( "WORLD_END" ) == "query" ) {
                bool decided = false;
                std::string buffer = _( "Warning: NPC interactions and some other global flags "
                                        "will not all reset when starting a new character in an "
                                        "already-played world.  This can lead to some strange "
                                        "behavior.\n\n"
                                        "Are you sure you wish to keep this world?"
                                      );

                while( !decided ) {
                    uilist smenu;
                    smenu.allow_cancel = false;
                    smenu.addentry( 0, true, 'r', "%s", _( "Reset world" ) );
                    smenu.addentry( 1, true, 'd', "%s", _( "Delete world" ) );
                    smenu.addentry( 2, true, 'k', "%s", _( "Keep world" ) );
                    smenu.query();

                    switch( smenu.ret ) {
                        case 0:
                            queryReset = true;
                            decided = true;
                            break;
                        case 1:
                            queryDelete = true;
                            decided = true;
                            break;
                        case 2:
                            decided = query_yn( buffer );
                            break;
                    }
                }
            }

            if( queryDelete || get_option<std::string>( "WORLD_END" ) == "delete" ) {
                world_generator->delete_world( world_generator->active_world->world_name, true );

            } else if( queryReset || get_option<std::string>( "WORLD_END" ) == "reset" ) {
                world_generator->delete_world( world_generator->active_world->world_name, false );
            }
        } else if( get_option<std::string>( "WORLD_END" ) != "keep" ) {
            std::string tmpmessage;
            for( auto &character : characters ) {
                tmpmessage += "\n  ";
                tmpmessage += character;
            }
            popup( _( "World retained.  Characters remaining:%s" ), tmpmessage );
        }
        if( g->gamemode ) {
            g->gamemode = std::make_unique<special_game>(); // null gamemode or something..
        }
    }

    //Reset any offset due to driving
    g->set_driving_view_offset( point_rel_ms::zero );

    //clear all sound channels
    sfx::fade_audio_channel( sfx::channel::any, 300 );
    sfx::fade_audio_group( sfx::group::weather, 300 );
    sfx::fade_audio_group( sfx::group::time_of_day, 300 );
    sfx::fade_audio_group( sfx::group::context_themes, 300 );
    sfx::fade_audio_group( sfx::group::low_stamina, 300 );

    zone_manager::get_manager().clear();

    MAPBUFFER.clear();
    overmap_buffer.clear();

#if defined(__ANDROID__)
    quick_shortcuts_map.clear();
#endif
    return true;
}

} // namespace turn_handler

void handle_key_blocking_activity()
{
    if( test_mode ) {
        return;
    }
    avatar &u = get_avatar();
    const bool has_unfinished_activity = u.activity && (
            u.activity.id()->based_on() == based_on_type::NEITHER
            || u.activity.moves_left > 0 );
    if( has_unfinished_activity || u.has_destination() ) {
        input_context ctxt = get_default_mode_input_context();
        const std::string action = ctxt.handle_input( 0 );
        bool refresh = true;
        if( action == "pause" ) {
            if( u.activity.is_interruptible_with_kb() ) {
                g->cancel_activity_query( _( "Confirm:" ) );
            }
        } else if( action == "zoom_in" ) {
            g->zoom_in();
            g->mark_main_ui_adaptor_resize();
        } else if( action == "zoom_out" ) {
            g->zoom_out();
            g->mark_main_ui_adaptor_resize();
        } else if( action == "player_data" ) {
            u.disp_info( true );
        } else if( action == "messages" ) {
            Messages::display_messages();
        } else if( action == "help" ) {
            get_help().display_help();
        } else if( action != "HELP_KEYBINDINGS" ) {
            refresh = false;
        }
        if( refresh ) {
            ui_manager::redraw();
            refresh_display();
        }
    } else {
        refresh_display();
        inp_mngr.pump_events();
    }
}

namespace
{
void monmove()
{
    g->cleanup_dead();
    map &m = get_map();
    avatar &u = get_avatar();

    for( monster &critter : g->all_monsters() ) {
        if( !m.inbounds( critter.pos_abs() ) ) {
            continue;
        }
        const tripoint_bub_ms critter_pos = critter.pos_bub( m );

        // Critters in impassable tiles get pushed away, unless it's not impassable for them
        if( !critter.is_dead() && ( m.impassable( critter_pos ) &&
                                    !m.get_impassable_field_at( critter_pos ).has_value() ) &&
            !critter.can_move_to( critter_pos ) ) {
            dbg( D_ERROR ) << "game:monmove: " << critter.name()
                           << " can't move to its location!  (" << critter_pos.x()
                           << ":" << critter_pos.y() << ":" << critter_pos.z() << "), "
                           << m.tername( critter_pos );
            add_msg_debug( debugmode::DF_MONSTER, "%s can't move to its location!  (%d,%d,%d), %s",
                           critter.name(),
                           critter_pos.x(), critter_pos.y(), critter_pos.z(), m.tername( critter_pos ) );
            bool okay = false;
            for( const tripoint_bub_ms &dest : m.points_in_radius( critter_pos, 3 ) ) {
                if( critter.can_move_to( dest ) && g->is_empty( dest ) ) {
                    critter.setpos( m, dest );
                    okay = true;
                    break;
                }
            }
            if( !okay ) {
                // die of "natural" cause (overpopulation is natural)
                critter.die( &m, nullptr );
            }
        }

        if( !critter.is_dead() ) {
            critter.process_turn();
        }

        m.creature_in_field( critter );
        if( calendar::once_every( 1_days ) ) {
            if( critter.has_flag( mon_flag_MILKABLE ) ) {
                critter.refill_udders();
            }
            critter.try_biosignature();
            critter.try_reproduce();
        }
        while( critter.get_moves() > 0 && !critter.is_dead() && !critter.has_effect( effect_ridden ) ) {
            critter.made_footstep = false;
            // Controlled critters don't make their own plans
            if( !critter.has_effect( effect_controlled ) ) {
                // Formulate a path to follow
                critter.plan();
            } else {
                critter.set_moves( 0 );
                break;
            }
            critter.move(); // Move one square, possibly hit u
            critter.process_triggers();
            m.creature_in_field( critter );
        }

        if( !critter.is_dead() && !critter.is_hallucination() &&
            rl_dist( u.pos_abs(), critter.pos_abs() ) < u.enchantment_cache->modify_value(
                enchant_vals::mod::MOTION_ALARM, 0 ) ) {
            if( u.has_active_bionic( bio_alarm ) ) {
                u.mod_power_level( -bio_alarm->power_trigger );
                add_msg( m_warning, _( "Your motion alarm goes off!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your motion alarm goes off!" ) );
            } else {
                add_msg( m_warning, _( "You suddenly feel alerted!" ) );
                g->cancel_activity_or_ignore_query( distraction_type::motion_alarm,
                                                    _( "Your instincts warn you for danger!" ) );
            }
            if( u.has_effect( effect_sleep ) ) {
                u.wake_up();
            }
        }
    }

    g->cleanup_dead();

    // The remaining monsters are all alive, but may be outside of the reality bubble.
    // If so, despawn them. This is not the same as dying, they will be stored for later and the
    // monster::die function is not called.
    g->despawn_nonlocal_monsters();

    // Now, do active NPCs.
    for( npc &guy : g->all_npcs() ) {
        int turns = 0;
        int real_count = 0;
        const int count_limit = std::max( 10, guy.get_moves() / 64 );
        if( guy.is_mounted() ) {
            guy.check_mount_is_spooked();
        }
        m.creature_in_field( guy );
        if( !guy.has_effect( effect_npc_suspend ) ) {
            guy.process_turn();
        }
        while( !guy.is_dead() && ( !guy.in_sleep_state() ||
                                   guy.activity.id() == ACT_OPERATION || guy.activity.id() == ACT_MIGRATION_CANCEL ) &&
               guy.get_moves() > 0 && turns < 10 ) {
            const int moves = guy.get_moves();
            const bool has_destination = guy.has_destination_activity();
            guy.move();
            if( moves == guy.get_moves() ) {
                // Count every time we exit npc::move() without spending any moves.
                real_count++;
                if( has_destination == guy.has_destination_activity() || real_count > count_limit ) {
                    turns++;
                }
            }
            // Turn on debug mode when in infinite loop
            // It has to be done before the last turn, otherwise
            // there will be no meaningful debug output.
            if( turns == 9 ) {
                debugmsg( "NPC '%s' entered infinite loop, npc activity id: '%s'",
                          guy.get_name(), guy.activity.id().str() );
            }
        }

        // If we spun too long trying to decide what to do (without spending moves),
        // Invoke cognitive suspension to prevent an infinite loop.
        if( turns == 10 ) {
            add_msg( _( "%s faints!" ), guy.get_name() );
            guy.reboot();
        }

        if( !guy.is_dead() ) {
            guy.npc_update_body();
        }
    }
    g->cleanup_dead();
}

void overmap_npc_move()
{
    avatar &u = get_avatar();
    std::vector<npc *> travelling_npcs;
    static constexpr int move_search_radius = 600;
    for( auto &elem : overmap_buffer.get_npcs_near_player( move_search_radius ) ) {
        if( !elem ) {
            continue;
        }
        npc *npc_to_add = elem.get();
        if( ( !npc_to_add->is_active() || rl_dist( u.pos_bub(), npc_to_add->pos_bub() ) > SEEX * 2 ) &&
            npc_to_add->mission == NPC_MISSION_TRAVELLING ) {
            travelling_npcs.push_back( npc_to_add );
        }
    }
    bool npcs_need_reload = false;
    for( npc *&elem : travelling_npcs ) {
        if( elem->has_omt_destination() ) {
            if( !elem->omt_path.empty() ) {
                if( rl_dist( elem->omt_path.back(), elem->pos_abs_omt() ) > 2 ) {
                    // recalculate path, we got distracted doing something else probably
                    elem->omt_path.clear();
                } else if( elem->omt_path.back() == elem->pos_abs_omt() ) {
                    elem->omt_path.pop_back();
                }
            }
            if( elem->omt_path.empty() ) {
                elem->omt_path = overmap_buffer.get_travel_path( elem->pos_abs_omt(), elem->goal,
                                 overmap_path_params::for_npc() ).points;
                if( elem->omt_path.empty() ) { // goal is unreachable, or already reached goal, reset it
                    elem->goal = npc::no_goal_point;
                }
            } else {
                elem->travel_overmap( elem->omt_path.back() );
                npcs_need_reload = true;
            }
        }
        if( !elem->has_omt_destination() && calendar::once_every( 1_hours ) && one_in( 3 ) ) {
            // travelling destination is reached/not set, try different one
            elem->set_omt_destination();
        }
    }
    if( npcs_need_reload ) {
        g->reload_npcs();
    }
}

} // namespace

void game::handle_progress_ui()
{
    avatar &u = get_avatar();
    // The popup and its "first redraw" flag are this player's own
    // (player_context::wait_popup): the caller (avatar_turn_post(), below)
    // has already activated this player, so mp_active_context() is `u`'s own
    // context, and the popup's ui_adaptor registers into `u`'s own ui stack.
    player_context &ctx = mp_active_context();

    // handle activity/progress/waiting UI
    const bool player_is_sleeping = u.has_effect( effect_sleep );
    bool wait_redraw = false;
    std::string wait_message;
    time_duration wait_refresh_rate;
    if( player_is_sleeping ) {
        wait_redraw = true;
        wait_message = _( "Wait till you wake up…" );
        wait_refresh_rate = 30_minutes;
    } else if( const std::optional<std::string> progress = u.activity.get_progress_message( u ) ) {
        wait_redraw = true;
        wait_message = *progress;
        if( u.activity.is_interruptible() && u.activity.interruptable_with_kb ) {
            wait_message += string_format( _( "\n%s to interrupt" ), press_x( ACTION_PAUSE ) );
        }
        if( u.activity.id() == ACT_AUTODRIVE ) {
            wait_refresh_rate = 1_turns;
        } else if( u.activity.id() == ACT_FIRSTAID ) {
            wait_refresh_rate = 5_turns;
        } else {
            wait_refresh_rate = 5_minutes;
        }
    }
    if( wait_redraw ) {
        if( ctx.first_redraw_since_waiting_started ||
            calendar::once_every( std::min( 1_minutes, wait_refresh_rate ) ) ) {
            if( ctx.first_redraw_since_waiting_started || calendar::once_every( wait_refresh_rate ) ) {
                ui_manager::redraw();
            }

            // Avoid redrawing the main UI every time due to invalidation
#ifdef TILES
            // If an ImGui window just closed and cleared the buffer, do a full
            // redraw now before blocking UIs below.
            if( cataimgui::clear_pending() ) {
                ui_manager::redraw();
            }
#endif
            ui_adaptor dummy( ui_adaptor::disable_uis_below {} );
            if( !ctx.wait_popup ) {
                ctx.wait_popup = std::make_unique<static_popup>();
            }
            ctx.wait_popup->on_top( true ).wait_message( "%s", wait_message );
            ui_manager::redraw();
            refresh_display();
            ctx.first_redraw_since_waiting_started = false;
        }
    } else {
        // Nothing to wait for now
        wait_popup_reset();
        ctx.first_redraw_since_waiting_started = true;
    }
}

void game::world_turn_pre()
{
    drain_renderer_recovery();

    weather_manager &weather = get_weather();

    // Increment game turn
    if( new_game ) {
        new_game = false;
        weather.on_game_start();
    } else {
        gamemode->per_turn();
        calendar::turn += 1_turns;
    }
    //used for dimension swapping
    if( swapping_dimensions ) {
        swapping_dimensions = false;
    }
    play_music( music::get_music_id_string() );

    // starting a new turn, clear out temperature cache
    weather.temperature_cache.clear();

    if( npcs_dirty ) {
        load_npcs();
    }

    timed_event_manager &timed_events = get_timed_events();
    timed_events.process();
    get_item_wakeups().process( calendar::turn );
    mission::process_all();
    avatar &u = get_avatar();
    map &m = get_map();
    if( calendar::once_every( 1_days ) ) {
        overmap_buffer.process_mongroups();
    }

    // Move hordes every turn, move_hordes has its own rate limiting
    overmap_buffer.move_hordes();
    if( calendar::once_every( time_duration::from_minutes( 2.5 ) ) ) {
        if( u.has_trait( trait_HAS_NEMESIS ) ) {
            overmap_buffer.move_nemesis();
        }
    }

    debug_hour_timer.print_time();

    // Auto-save if autosave is enabled. With more than one player the guard is
    // "nobody is dead": a save taken while one of them is dying would restore
    // into a half-finished death.
    bool anyone_dead = false;
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        anyone_dead = anyone_dead || player_at( i ).is_dead_state();
    }
    if( get_option<bool>( "AUTOSAVE" ) &&
        calendar::once_every( 1_turns * get_option<int>( "AUTOSAVE_TURNS" ) ) &&
        !anyone_dead ) {
        autosave();
    }

    weather.update_weather();

    reset_light_level();
    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
        m.set_lightmap_cache_dirty( z );
    }

    perhaps_add_random_npc( /* ignore_spawn_timers_and_rates = */ false );
}

bool game::player_act( avatar &ava )
{
    map &m = get_map();

    // avatar processes human input through handle_action()
    if( !ava.has_effect( effect_sleep ) || uquit == QUIT_WATCH ) {
        if( ava.get_moves() > 0 || uquit == QUIT_WATCH ) {
            while( ( ava.get_moves() > 0 || uquit == QUIT_WATCH ) && !mp_sched_stopping() ) {

                // handle_action() may cause map updates, creatures to die
                m.process_falling();
                cleanup_dead();

                mon_info_update();
                // Process any new sounds the player caused during their turn.
                for( npc &guy : all_npcs() ) {
                    if( rl_dist( guy.pos_bub(), ava.pos_bub() ) < MAX_VIEW_DISTANCE ) {
                        sounds::process_sound_markers( &guy );
                    }
                }
                explosion_handler::process_explosions();
                sounds::process_sound_markers( &ava );
                if( !ava.activity && uquit != QUIT_WATCH
                    && ( !ava.has_distant_destination() || calendar::once_every( 10_seconds ) ) ) {
                    wait_popup_reset();
                    ui_manager::redraw();
                }

                if( queue_screenshot ) {
                    take_screenshot();
                    queue_screenshot = false;
                }

                if( handle_action() ) {
                    ++moves_since_last_save;
                    ava.action_taken();
                }

                if( is_game_over() ) {
                    return true;
                }

                if( uquit == QUIT_WATCH ) {
                    break;
                }

                // avatar processes moves for activities started by handle_action()
                while( ava.get_moves() > 0 && ava.activity ) {
                    ava.activity.do_turn( ava );
                }
            }
            // Reset displayed sound markers now that the turn is over.
            // We only want this to happen if the player had a chance to examine the sounds.
            sounds::reset_markers();
        } else {
            // Rate limit key polling to 10 times a second. The timestamp lives in
            // this player's context (player_context::last_key_poll) rather than a
            // function-local static: see that field's comment for why a shared
            // static was wrong once every player got their own context.
            auto &start = mp_active_context().last_key_poll;
            const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() );
            if( ( now - start ).count() > 100 ) {
                handle_key_blocking_activity();
                start = now;
            }

            mon_info_update();

            // If player is performing a task, a monster is dangerously close,
            // and monster can reach to the player or it has some sort of a ranged attack,
            // warn them regardless of previous safemode warnings
            if( ava.activity ) {
                for( std::pair<const distraction_type, std::string> &dist :
                     ava.activity.get_distractions() ) {
                    if( cancel_activity_or_ignore_query( dist.first, dist.second ) ) {
                        break;
                    }
                }
            }
        }
    }

    // Deviation from the brief's literal Step 4: this block used to sit in
    // do_turn() between the action loop and world_turn_post(), reading only
    // slot 0. It reads mp_active_context().driving_view_offset -- per-player
    // context state -- so it has to run under its owner's context, which is
    // exactly what player_act() guarantees (the orchestrator activates the
    // player before calling this). Folding it into avatar_turn_post() would
    // put it in the upkeep tail instead of the acting half it conceptually
    // belongs to.
    if( mp_active_context().driving_view_offset.x() != 0 ||
        mp_active_context().driving_view_offset.y() != 0 ) {
        // Still have a view offset, but might not be driving anymore,
        // or the option has been deactivated,
        // might also happen when someone dives from a moving car.
        // or when using the handbrake.
        vehicle *veh = veh_pointer_or_null( m.veh_at( ava.pos_bub() ) );
        calc_driving_offset( veh );
    }

    return false;
}

bool game::do_turn()
{
    if( is_game_over() ) {
        return turn_handler::cleanup_at_end();
    }

    if( mp_sched *const sched = mp_active_sched() ) {
        // With more than one player the turn is resumable: world_turn_pre opens
        // it once, and while the barrier is unsatisfied do_turn returns without
        // closing it -- the calendar stays frozen until every living player has
        // acted or been dropped by the hurry countdown.
        mp_activate_world();
        if( mp_session_end_requested() ) {
            // A player quit or died out of the game. Their thread had nowhere
            // to return player_act's `true` to, so it left it here.
            return turn_handler::cleanup_at_end();
        }
        if( !mp_turn_is_open() ) {
            mp_turn_reset(); // once per opened turn, not once per poll
            world_turn_pre();
            mp_set_turn_open( true );
            // Release every player once for this opened turn. avatar_turn_pre
            // (which drains a running activity) must run for each of them even
            // if the barrier could already close: a player carrying an activity
            // reads as 'acted' at the start of a turn and would otherwise never
            // be handed the baton, so their activity would never advance.
            for( size_t i = 0; i < player_slot_count(); ++i ) {
                if( sched->waiting_for_baton( i ) ) {
                    sched->yield_to( mp_sched::world, i );
                }
            }
        }
        while( !mp_barrier_satisfied() ) {
            mp_tick_hurry();
            bool moved = false;
            for( size_t i = 0; i < player_slot_count(); ++i ) {
                const mp_player_state state = mp_player_state_of( i );
                // waiting_for_baton alone is not enough: a player who already
                // acted this turn releases and immediately re-parks on
                // wait_for_turn for the next one, so they report it too -- and
                // yielding to them again would spin the world. Hand the baton
                // only to someone who still owes the turn: acting (not yet
                // released this turn) or holding (returning from an input wait).
                if( sched->waiting_for_baton( i ) &&
                    ( state == mp_player_state::acting || state == mp_player_state::holding ) ) {
                    sched->yield_to( mp_sched::world, i );
                    moved = true;
                }
            }
            if( moved ) {
                // The world ran somebody, so it is accountable again: any stall
                // from here on is a real one and the watchdog should say so.
                sched->set_world_waiting( false );
            }
            if( !moved && !sched->wait_for_request( MP_POLL_MS ) ) {
                // Waiting turn: nobody owes the turn and nobody became ready.
                // The world does not tick and do_turn returns; the turn stays
                // open. The declared wait tells the watchdog that the world is
                // holding the baton on purpose, so a partner reading a menu
                // does not log as a stalled participant.
                sched->set_world_waiting( true );
                return false;
            }
            if( mp_session_end_requested() ) {
                // Somebody quit from inside their own turn: stop polling for a
                // barrier nobody is going to satisfy.
                sched->set_world_waiting( false );
                return turn_handler::cleanup_at_end();
            }
        }
        sched->set_world_waiting( false );
        mp_activate_world();
        world_turn_post();

        // Standing still is what being dropped means, and it has to be charged
        // before moves are handed back: they accumulate, and a dropped player
        // spends none of them.
        mp_forfeit_dropped_moves();

        // Moves come back after monmove, exactly as in single player. The baton
        // goes back to each player for this because the morale paths can draw.
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            mp_activate_player( i );
            avatar_turn_post( player_at( i ) );
        }
        mp_activate_player( 0 );
        if( player_slot_count() > 1 ) {
            const tripoint_abs_ms anchor = mp_bubble_anchor();
            shift_map_center( anchor );
            if( anchor.z() != get_map().get_abs_sub().z() ) {
                shift_map_z( anchor.z() );
            }
        }
        mp_set_turn_open( false );
#if defined(EMSCRIPTEN)
        // This will cause a prompt to be shown if the window is closed, until the
        // game is saved.
        EM_ASM( window.game_unsaved = true; );
#endif
        debug_menu::debug_capture::tick_if_active();
        return false;
    }

    // Single player: no scheduler and no thread -- the orchestrator runs the
    // loop inline, which is what makes the ordinary single-player build walk
    // this same code and every test exercise it.
    mp_activate_world();
    world_turn_pre();
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        mp_activate_player( i );
        avatar &ava = player_at( i );
        avatar_turn_pre( ava );
        if( player_act( ava ) ) {
            // Deviation from the brief's literal Step 4: player_act() returning
            // true only means "the session must end" -- it never calls
            // cleanup_at_end() itself, so that global teardown (death screen,
            // graveyard save, achievements, MAPBUFFER/overmap_buffer clear,
            // audio fade) still has to run somewhere. do_turn() is that place,
            // mirroring its own top-of-function is_game_over() check: this
            // runs on the orchestrating thread, never inside one player's
            // worker while others may still be live (task 6+).
            //
            // This path is deliberately untested: cleanup_at_end() performs
            // global teardown (graveyard save, MAPBUFFER/overmap_buffer clear,
            // audio fade) that a unit test cannot exercise honestly. Coverage
            // here is by review, not by test -- do not assume a test backs it.
            return turn_handler::cleanup_at_end();
        }
    }
    mp_activate_world();
    world_turn_post();
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        mp_activate_player( i );
        avatar_turn_post( player_at( i ) );
    }
    mp_activate_player( 0 );
    if( player_slot_count() > 1 ) {
        const tripoint_abs_ms anchor = mp_bubble_anchor();
        shift_map_center( anchor );
        if( anchor.z() != get_map().get_abs_sub().z() ) {
            shift_map_z( anchor.z() );
        }
    }
#if defined(EMSCRIPTEN)
    // This will cause a prompt to be shown if the window is closed, until the
    // game is saved.
    EM_ASM( window.game_unsaved = true; );
#endif
    debug_menu::debug_capture::tick_if_active();
    return false;
}

void game::world_turn_post()
{
    map &m = get_map();
    scent_map &scent = get_scent();
    // No-scent debug mutation has to be processed here or else it takes time to start working.
    // Every player leaves a scent trail, not just whoever holds the baton.
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        avatar &ava = player_at( i );
        if( !ava.has_flag( json_flag_NO_SCENT ) ) {
            scent.set( ava.pos_bub(), ava.scent, ava.get_type_of_scent() );
            overmap_buffer.set_scent( ava.pos_abs_omt(), ava.scent );
        }
    }
    scent.update( player_at( 0 ).pos_bub(), m );

    // We need floor cache before checking falling 'n stuff
    m.build_floor_caches();

    m.process_falling();
    m.vehmove();
    m.process_fields();
    m.process_items();
    explosion_handler::process_explosions();
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        avatar &ava = player_at( i );
        m.creature_in_field( ava );
    }

    // Apply sounds from previous turn to monster and NPC AI.
    sounds::process_sounds();
    const int levz = m.get_abs_sub().z();
    // Update vision caches for monsters. If this turns out to be expensive,
    // consider a stripped down cache just for monsters.
    m.build_map_cache( levz, true );

    // process monster and npc turn
    monmove();

    if( calendar::once_every( time_between_npc_OM_moves ) ) {
        overmap_npc_move();
    }
    // NPC morale is world work, not per-avatar: an NPC's morale doesn't belong
    // to any one player slot, so it updates once per turn here rather than once
    // per avatar_turn_post() call.
    if( calendar::once_every( 1_minutes ) ) {
        for( npc &guy : all_npcs() ) {
            guy.update_morale();
            guy.check_and_recover_morale();
        }
    }
    m.furniture_terrain_emit_fields();
    // required after monsters move and fields emit
    // This runs once for the world half, not once per slot: mon_info_update()
    // reads game::u, and mp_activate_world() (top of the world half) swaps u
    // back to slot 0's avatar before switching the ui stack, so this reads a
    // single, well-defined viewpoint instead of whichever slot the player loop
    // happened to leave active. A per-slot pass also needs mon_info_update()
    // to take the avatar as a parameter instead.
    mon_info_update();

    // handle_weather_effects() never reads the avatar it's passed: it decays
    // fields/scent, fills water collectors, wets every player slot, applies
    // glare to every slot and clears lightning_active, all by looping
    // g->get_players() internally (see weather.cpp:494 and :114). It runs once
    // per turn from the world half, not once per avatar. The guard reads
    // "weather processes unless every player is underwater" rather than a
    // single avatar's state: weather is world state, so one player diving must
    // not suspend rain for everybody. At N=1 this is identical to the original
    // `!ava.is_underwater()` check by construction.
    bool anyone_not_underwater = false;
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        anyone_not_underwater = anyone_not_underwater || !player_at( i ).is_underwater();
    }
    if( levz >= 0 && anyone_not_underwater ) {
        handle_weather_effects( get_weather().weather_id );
    }

    // The visibility cache is shared map state, not tied to a particular
    // avatar. The progress UI used to be called here too, but it reads and
    // opens UI for a particular avatar (get_avatar()) and now runs from
    // avatar_turn_post() instead, in the per-player tail loop below -- see
    // the comment there.
    m.invalidate_visibility_cache();
}

void game::avatar_turn_pre( avatar &ava )
{
    map &m = get_map();

    // If controlling a vehicle that is owned by someone else
    if( ava.in_vehicle && ava.controlling_vehicle ) {
        vehicle *veh = veh_pointer_or_null( m.veh_at( ava.pos_bub() ) );
        if( veh && !veh->handle_potential_theft( ava, true ) ) {
            veh->handle_potential_theft( ava, false, false );
        }
    }

    // If you're inside a wall or something and haven't been telefragged, let's get you out.
    if( ( m.impassable( ava.pos_bub() ) && !m.impassable_field_at( ava.pos_bub() ) ) &&
        !m.has_flag( ter_furn_flag::TFLAG_CLIMBABLE, ava.pos_bub() ) ) {
        ava.stagger();
    }

    // If riding a horse - chance to spook
    if( ava.is_mounted() ) {
        ava.check_mount_is_spooked();
    }

    ava.update_body();

    // process avatar activities (ignoring user input)
    while( ava.get_moves() > 0 && ava.activity ) {
        ava.activity.do_turn( ava );
    }

    // Process NPC sound events before they move or they hear themselves talking
    for( npc &guy : all_npcs() ) {
        if( rl_dist( guy.pos_bub(), ava.pos_bub() ) < MAX_VIEW_DISTANCE ) {
            sounds::process_sound_markers( &guy );
        }
    }

    music::deactivate_music_id( music::music_id::sound );

    // Process sound events into sound markers for display to the player.
    sounds::process_sound_markers( &ava );

    if( ava.is_deaf() ) {
        sfx::do_hearing_loss();
    }
}

void game::avatar_turn_post( avatar &ava )
{
    weather_manager &weather = get_weather();

    // Runs first, before anything below: this is the single-player order
    // world_turn_post()/avatar_turn_post( u ) always had, since
    // handle_progress_ui() used to be the last statement of world_turn_post()
    // and avatar_turn_post( u ) ran immediately after it. handle_progress_ui()
    // opens a UI and reads get_avatar(), so it has to run in this player's
    // own half: do_turn()'s tail loop calls mp_activate_player( i ) right
    // before this, so get_avatar() reads `ava` itself, and the popup it may
    // build registers into `ava`'s own ui stack, not the world stack.
    handle_progress_ui();

    // replenish avatar moves
    ava.process_turn();

    if( ava.get_moves() < 0 && get_option<bool>( "FORCE_REDRAW" ) ) {
        ui_manager::redraw();
        refresh_display();
    }

    ava.update_bodytemp();
    ava.update_body_wetness( *weather.weather_precise );
    ava.apply_wetness_morale( weather.temperature );

    if( calendar::once_every( 1_minutes ) ) {
        ava.update_morale();
    }

    if( calendar::once_every( 9_turns ) ) {
        ava.check_and_recover_morale();
    }

    if( !ava.is_deaf() ) {
        sfx::remove_hearing_loss();
    }
    sfx::do_danger_music();
    sfx::do_vehicle_engine_sfx();
    sfx::do_vehicle_exterior_engine_sfx();
    sfx::do_low_stamina_sfx();

    // reset player noise
    ava.volume = 0;

    // Calculate bionic power balance
    ava.power_balance = ava.get_power_level() - ava.power_prev_turn;
    ava.power_prev_turn = ava.get_power_level();
}
