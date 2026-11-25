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
#include "mp/mp_mode.h"
#include "mp/mp_plan.h"
#include "mp/mp_resolve.h"
#include "mp/mp_scene.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_tether.h"
#include "mp/mp_transport.h"
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
        std::chrono::seconds total_time_played =
            mp_active_context().time_played_at_last_load + time_since_load;
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
#if defined( MULTIPLAYER )
    // An away player whose activity drained their moves reaches this poll from
    // player_act() instead of the normal input wait. They still own their key:
    // a read that only redraws would leave them away for ever. Remember the
    // seat so ':' and a costly action can have their usual away-state effect.
    const std::optional<size_t> away_slot =
        mp_coop_active() && mp_player_is_away( g->active_slot() )
        ? std::optional<size_t>( g->active_slot() ) : std::nullopt;
#endif
    const bool has_unfinished_activity = u.activity && (
            u.activity.id()->based_on() == based_on_type::NEITHER
            || u.activity.moves_left > 0 );
#if defined( MULTIPLAYER )
    if( has_unfinished_activity || u.has_destination() || away_slot ) {
#else
    if( has_unfinished_activity || u.has_destination() ) {
#endif
        input_context ctxt = get_default_mode_input_context();
        const std::string action = ctxt.handle_input( 0 );
        bool refresh = true;
#if defined( MULTIPLAYER )
        if( away_slot ) {
            const action_id act = look_up_action( action );
            if( act == ACTION_COOP_AWAY ) {
                mp_return_from_away( *away_slot );
                refresh = false;
            } else if( act != ACTION_NULL && act != ACTION_TIMEOUT &&
                       !mp_action_is_free( act ) ) {
                mp_return_from_away( *away_slot );
                refresh = false;
            }
        }
#endif
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
// One monster's step, shared by the single-player while-loop below and the
// round step: form a plan, take one move, then let the tile the critter ended
// up in act on it. Returns false when the critter is controlled and therefore
// makes no plan of its own.
bool mon_step( monster &critter, map &m )
{
    critter.made_footstep = false;
    // Controlled critters don't make their own plans
    if( critter.has_effect( effect_controlled ) ) {
        return false;
    }
    // Formulate a path to follow
    critter.plan();
    critter.move(); // Move one square, possibly hit u
    critter.process_triggers();
    m.creature_in_field( critter );
    return true;
}

// The motion alarm a monster's presence raises for the world's viewpoint. Runs
// once per monster per monster pass: after its whole move loop in single
// player, and after its single step in a co-op round.
void mon_motion_alarm( const monster &critter )
{
    avatar &u = get_avatar();
    if( critter.is_dead() || critter.is_hallucination() ||
        rl_dist( u.pos_abs(), critter.pos_abs() ) >= u.enchantment_cache->modify_value(
            enchant_vals::mod::MOTION_ALARM, 0 ) ) {
        return;
    }
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

// The conditions under which the NPC move loop takes an iteration. Shared so
// the single-player loop and the round step agree on who may move.
bool npc_can_move( const npc &guy )
{
    return !guy.is_dead() &&
           ( !guy.in_sleep_state() ||
             guy.activity.id() == ACT_OPERATION || guy.activity.id() == ACT_MIGRATION_CANCEL ) &&
           guy.get_moves() > 0;
}

// One iteration of an NPC's move loop, shared by the single-player while-loop
// below and the round step. The result carries what the single-player loop's
// no-progress guard needs; the round step runs one iteration and has no loop
// to guard.
struct npc_step_result {
    // True when npc::move() spent any moves.
    bool spent_moves = false;
    // True when the iteration changed the NPC's destination activity.
    bool changed_destination = false;
};

npc_step_result npc_step( npc &guy )
{
    const int moves = guy.get_moves();
    const bool has_destination = guy.has_destination_activity();
    guy.move();
    return { moves != guy.get_moves(), has_destination != guy.has_destination_activity() };
}

// The work that has to happen before a monster's first step of the tick.
// Returns false for a critter outside the reality bubble: it gets no turn and
// no alarm, exactly as the old monmove() loop skipped it.
bool mon_preamble( monster &critter )
{
    map &m = get_map();
    if( !m.inbounds( critter.pos_abs() ) ) {
        return false;
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
    return true;
}

// The per-NPC work that precedes that NPC's move loop for this tick.
void npc_preamble( npc &guy )
{
    map &m = get_map();
    if( guy.is_mounted() ) {
        guy.check_mount_is_spooked();
    }
    m.creature_in_field( guy );
    if( !guy.has_effect( effect_npc_suspend ) ) {
        guy.process_turn();
    }
}

void monmove()
{
    map &m = get_map();

    for( monster &critter : g->all_monsters() ) {
        if( !mon_preamble( critter ) ) {
            continue;
        }
        while( critter.get_moves() > 0 && !critter.is_dead() &&
               !critter.has_effect( effect_ridden ) ) {
            if( !mon_step( critter, m ) ) {
                critter.set_moves( 0 );
                break;
            }
        }
        mon_motion_alarm( critter );
    }

    g->cleanup_dead();

    // The remaining monsters are all alive, but may be outside of the reality bubble.
    // If so, despawn them. This is not the same as dying, they will be stored for later and the
    // monster::die function is not called.
    g->despawn_nonlocal_monsters();

    // Now, do active NPCs.
    for( npc &guy : g->all_npcs() ) {
        npc_preamble( guy );
        int turns = 0;
        int real_count = 0;
        const int count_limit = std::max( 10, guy.get_moves() / 64 );
        while( npc_can_move( guy ) && turns < 10 ) {
            const npc_step_result step = npc_step( guy );
            if( !step.spent_moves ) {
                // Count every time we exit npc::move() without spending any moves.
                real_count++;
                if( !step.changed_destination || real_count > count_limit ) {
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

// The world's own work at the top of a tick: everything that has to happen
// before any creature is given its turn. Ends by removing the dead, which is
// where monmove() always started its creature pass.
void world_tick_block()
{
    map &m = get_map();
    scent_map &scent = get_scent();
    // No-scent debug mutation has to be processed here or else it takes time to start working.
    // Every player leaves a scent trail, not just whoever holds the baton.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        avatar &ava = g->player_at( i );
        if( !ava.has_flag( json_flag_NO_SCENT ) ) {
            scent.set( ava.pos_bub(), ava.scent, ava.get_type_of_scent() );
            overmap_buffer.set_scent( ava.pos_abs_omt(), ava.scent );
        }
    }
    scent.update( g->player_at( 0 ).pos_bub(), m );

    // We need floor cache before checking falling 'n stuff
    m.build_floor_caches();

    m.process_falling();
    m.vehmove();
    m.process_fields();
    m.process_items();
    explosion_handler::process_explosions();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        avatar &ava = g->player_at( i );
        m.creature_in_field( ava );
    }

    // Apply sounds from previous turn to monster and NPC AI.
    sounds::process_sounds();
    const int levz = m.get_abs_sub().z();
    // Update vision caches for monsters. If this turns out to be expensive,
    // consider a stripped down cache just for monsters.
    m.build_map_cache( levz, true );

    g->cleanup_dead();
}

// The world work after the creatures have moved. Single player runs it right
// after monmove(); a session runs it from world_turn_post_tail() at tick
// close. It never touches the dead -- both callers own that pass.
void world_turn_rest()
{
    map &m = get_map();
    if( calendar::once_every( time_between_npc_OM_moves ) ) {
        overmap_npc_move();
    }
    // NPC morale is world work, not per-avatar: an NPC's morale doesn't belong
    // to any one player slot, so it updates once per turn here rather than once
    // per avatar_turn_post() call.
    if( calendar::once_every( 1_minutes ) ) {
        for( npc &guy : g->all_npcs() ) {
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
    g->mon_info_update();

    // handle_weather_effects() never reads the avatar it's passed: it decays
    // fields/scent, fills water collectors, wets every player slot, applies
    // glare to every slot and clears lightning_active, all by looping
    // g->get_players() internally (see weather.cpp:494 and :114). It runs once
    // per turn from the world half, not once per avatar. The guard reads
    // "weather processes unless every player is underwater" rather than a
    // single avatar's state: weather is world state, so one player diving must
    // not suspend rain for everybody. At N=1 this is identical to the original
    // `!ava.is_underwater()` check by construction.
    const int levz = m.get_abs_sub().z();
    bool anyone_not_underwater = false;
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        anyone_not_underwater = anyone_not_underwater || !g->player_at( i ).is_underwater();
    }
    if( levz >= 0 && anyone_not_underwater ) {
        handle_weather_effects( get_weather().weather_id );
    }

    // The visibility cache is shared map state, not tied to a particular
    // avatar. The progress UI used to be called here too, but it reads and
    // opens UI for a particular avatar (get_avatar()) and now runs from
    // avatar_turn_post() instead, in the per-player tail loop -- see the
    // comment there.
    m.invalidate_visibility_cache();
}

} // namespace

void mon_tick_pre()
{
    world_tick_block();

    // A session has no whole-tick monmove(): every creature's preamble runs
    // once here, and its single step waits for a round. Single player never
    // calls this; monmove() interleaves the same preambles with its loops.
    for( monster &critter : g->all_monsters() ) {
        mon_preamble( critter );
    }
    for( npc &guy : g->all_npcs() ) {
        npc_preamble( guy );
    }
}

#if defined( MULTIPLAYER )
bool mon_round_step()
{
    map &m = get_map();
    // Whether anything moved this round. A round in which no creature can
    // spend anything is the end of the tick's creature work: the loop uses
    // this together with mp_resolve_any_creature_can_act() to decide whether
    // another monster-only round follows, so a stuck critter (a controlled
    // monster whose step zeroes its moves, an NPC that will not spend) closes
    // the tick instead of running it again for ever.
    bool progress = false;

    // Monsters have no stable id to order by, so the round orders them by
    // where they stand, then by name. stable_sort keeps the tracker's order
    // for exact ties, which makes the pass reproducible.
    std::vector<monster *> order;
    for( monster &critter : g->all_monsters() ) {
        if( m.inbounds( critter.pos_abs() ) ) {
            order.push_back( &critter );
        }
    }
    std::stable_sort( order.begin(), order.end(), []( const monster * l, const monster * r ) {
        const tripoint_abs_ms lp = l->pos_abs();
        const tripoint_abs_ms rp = r->pos_abs();
        if( lp.x() != rp.x() ) {
            return lp.x() < rp.x();
        }
        if( lp.y() != rp.y() ) {
            return lp.y() < rp.y();
        }
        if( lp.z() != rp.z() ) {
            return lp.z() < rp.z();
        }
        return l->get_name() < r->get_name();
    } );

    // One step per critter: a monster that could take several this tick takes
    // one in this round and another in the next.
    for( monster *const critter : order ) {
        if( critter->is_dead() || critter->has_effect( effect_ridden ) ||
            critter->get_moves() <= 0 ) {
            continue;
        }
        if( !mon_step( *critter, m ) ) {
            // Controlled: no plan of its own this round, exactly as in the
            // single-player loop.
            critter->set_moves( 0 );
            continue;
        }
        mon_motion_alarm( *critter );
        progress = true;
    }
    g->cleanup_dead();

    // One iteration of each eligible NPC's move loop. The no-progress guard
    // belongs to the single-player loop, which runs many iterations; a round
    // runs exactly one, so there is no loop for it to stop.
    for( npc &guy : g->all_npcs() ) {
        if( npc_can_move( guy ) && npc_step( guy ).spent_moves ) {
            progress = true;
        }
    }
    g->cleanup_dead();
    return progress;
}

bool mp_resolve_any_creature_can_act()
{
    map &m = get_map();
    for( monster &critter : g->all_monsters() ) {
        if( !critter.is_dead() && !critter.has_effect( effect_ridden ) &&
            m.inbounds( critter.pos_abs() ) && critter.get_moves() > 0 ) {
            return true;
        }
    }
    for( const npc &guy : g->all_npcs() ) {
        if( npc_can_move( guy ) ) {
            return true;
        }
    }
    return false;
}
#endif // MULTIPLAYER

void world_turn_post_tail()
{
    // The co-op tick close. No monmove() runs in a session, so the pieces it
    // owns for single player happen here, once per tick: the despawn, the NPC
    // body updates, and the final removal of the dead.
    g->despawn_nonlocal_monsters();
    for( npc &guy : g->all_npcs() ) {
        if( !guy.is_dead() ) {
            guy.npc_update_body();
        }
    }
    world_turn_rest();
    g->cleanup_dead();
}

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
            // An activity drains a player's moves before this branch is
            // reached, so the normal input wait never runs for them. An away
            // player still has a key of their own to read. A blocking read
            // parks the player thread on its wire, and the world can never
            // hand it the baton to run avatar_turn_pre() -- the only place an
            // activity advances in a session. So poll instead whenever the
            // partner is playing, and keep the blocking read only while the
            // all-away hold keeps the turn open: with no further baton
            // handoffs, that is the only wait that can still pick up ':'.
#if defined( MULTIPLAYER )
            if( mp_player_is_away( g->active_slot() ) ) {
                if( mp_turn_all_away() ) {
                    handle_action();
                } else {
                    handle_key_blocking_activity();
                }
            } else
#endif
            {
                // Rate limit key polling to 10 times a second. The timestamp
                // lives in this player's context (last_key_poll) rather than
                // a function-local static: a shared static was wrong once
                // every player got their own context.
                auto &start = mp_active_context().last_key_poll;
                const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() );
                if( ( now - start ).count() > 100 ) {
                    handle_key_blocking_activity();
                    start = now;
                }
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

// A session ends for everybody the moment anyone leaves it -- quits, dies or
// drops (the roster is fixed). None of single-player's ending applies: the
// death screens, the deathcam and the world-end menu all ask the one player,
// and moving the save to the graveyard would take everyone else's characters
// with it. The world is saved as it stands instead, a dead character with it,
// whose death is then dealt with when the world is next loaded alone.
static bool end_session( mp_sched &sched )
{
    sched.set_world_waiting( false );
    mp_activate_world();
    if( g->uquit != QUIT_SAVED && !g->save() ) {
        debugmsg( "the session ended but the world could not be saved" );
    }
    g->uquit = QUIT_SAVED;
    return turn_handler::cleanup_at_end();
}

bool game::do_turn()
{
    if( mp_sched *const sched = mp_active_sched() ) {
        // With more than one player the turn is resumable: world_turn_pre opens
        // it once, and while the barrier is unsatisfied do_turn returns without
        // closing it -- the calendar stays frozen until every living player has
        // acted or been dropped by the hurry countdown.
        mp_activate_world();
        mp_sched_trace_phase( "turn-start" );
#if defined(MP_SERVER)
        // A client that dropped may come back: its seat is parked, and the
        // listener is polled by the world once per turn. Non-blocking, and a
        // no-op when no listener is bound (the in-process harness).
        mp_server_poll_new_clients();
#endif
        // Single-player's own check, is_game_over(), reads only the avatar in
        // u and goes on to ask that player questions; a session looks at every
        // slot and asks nobody.
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            if( player_at( i ).is_dead_state() ) {
#if defined(MP_SERVER)
                // A death is a replacement, not the end of everybody's game:
                // place the corpse once and let the seat's own thread offer a
                // new character. The loop below hands that thread the baton.
                mp_server_notify_death( i );
#else
                mp_request_session_end( player_at( i ).get_name() + " died" );
#endif
            }
        }
        if( mp_session_end_requested() ) {
            // A player quit, died or dropped. A player thread had nowhere to
            // return player_act's `true` to, so it left it here.
            return end_session( *sched );
        }
#if defined(MP_SERVER) && defined(MULTIPLAYER)
        // One tick, one or more rounds. The tick opens once and stays open
        // while any seat or creature can still act; every poll collects that
        // seat's plan, hands the baton to a seat the session owes one, and
        // resolves the round as soon as every seat that can act has committed
        // or passed. A round that leaves somebody able to act starts the next
        // round in the same tick; otherwise the tick closes here.
        const bool opened_tick = !mp_turn_is_open();
        if( opened_tick ) {
            mp_turn_open_tick();
            // The client's own view of the new tick: the phase is planning and
            // each seat's AP is back, before the pacing wait below -- so it is
            // there before any key can arrive, not after the first event.
            mp_server_broadcast_party();
            mp_server_broadcast_phase();
            mp_server_flush_scenes_all();
        }

        // A lost wire is a server-side fact. A seat parked in a menu notices it
        // on its own, but a seat with no input never touches its wire, and a
        // disconnected seat with moves would hold the round for ever.
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            mp_session *const seat = mp_session_of_slot( i );
            if( seat != nullptr && seat->transport && !seat->transport->connected() &&
                !mp_player_is_disconnected( i ) ) {
                mp_server_mark_disconnected( i );
            }
        }

        // Pacing: one tick's time per tick, and a key never pays a poll
        // interval. A queued key -- or local control work the seat owns, a
        // resize to apply or a hover answer that is due -- means grant it now;
        // a round somebody is still planning in sleeps until a plan, a key or a
        // dead wire arrives; a tick that opens with nobody planning in it still
        // takes a tick (the single-player busy poll); and a continuation round
        // -- the tick is already open, the round is settled, nobody holds a key
        // -- runs back to back, which is what gives a 200-AP monster its later
        // rounds.
        bool any_seat_has_input = false;
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            any_seat_has_input = any_seat_has_input || mp_server_seat_has_input( i ) ||
                                 mp_server_seat_needs_local_service( i );
        }
        if( !any_seat_has_input && ( !mp_turn_round_complete() || opened_tick ) ) {
            mp_sched_trace_phase( "poll-begin" );
            // The world holds the baton on purpose across this wait: without
            // saying so, a round open on a thinking player for longer than the
            // watchdog's threshold logs as a stalled participant.
            sched->set_world_waiting( true );
            mp_server_wait_for_events( MP_POLL_MS );
            sched->set_world_waiting( false );
        }

        // Round controls become plan state. The drain runs to the end on every
        // poll: the event wait above is edge-triggered, so a single take per
        // poll would strand the rest until the next event. Every one of them --
        // accepted or refused -- changes the party snapshot, so one broadcast
        // follows the whole drain rather than one per message.
        size_t controls_handled = 0;
        size_t control_slot = 0;
        mp_control control;
        while( mp_server_take_control( control_slot, control ) ) {
            ++controls_handled;
            switch( control.kind ) {
                case mp_control_kind::plan: {
                    if( control.plan.kind == mp_plan_kind::pass ) {
                        // A pass on the wire is the pass control: the resolver's
                        // empty round does not spend anything, so a merely ready
                        // pass would leave the seat eligible and the tick could
                        // never close.
                        mp_plan_pass( control_slot );
                        break;
                    }
                    mp_plan plan;
                    plan.kind = control.plan.kind;
                    plan.cost = control.plan.cost;
                    plan.seq = control.plan.seq;
                    plan.label = control.plan.label;
                    if( plan.kind != mp_plan_kind::pass ) {
                        // A client has no map coordinates: its cells are the
                        // seat's own scene rect cells, 0-based inside the frame,
                        // so the world places them through that seat's origin.
                        // The sidebar's position moves the terrain window but
                        // not the frame the client indexed. A pass names no
                        // tile and skips this.
                        std::optional<tripoint_bub_ms> target;
                        mp_activate_player( control_slot );
                        mp_scene_rect rect;
                        if( mp_scene_mask_map_rect( rect ) ) {
                            target = mp_scene_rect_tile_at( rect, mp_active_context().ter_view_p,
                                                            control.plan.x, control.plan.y );
                        }
                        mp_activate_world();
                        if( !target ) {
                            mp_plan_set_error( control_slot, "target is off the map" );
                            break;
                        }
                        plan.target = *target;
                    }
                    std::string error;
                    if( !mp_plan_submit( control_slot, plan, error ) ) {
                        mp_plan_set_error( control_slot, error );
                    }
                    break;
                }
                case mp_control_kind::plan_ready: {
                    std::string error;
                    if( !mp_plan_ready( control_slot, error ) ) {
                        mp_plan_set_error( control_slot, error );
                    }
                    break;
                }
                case mp_control_kind::plan_cancel:
                    mp_plan_cancel( control_slot );
                    // An intent withdrawn leaves the round waiting for the seat
                    // again, which is the planning phase it reports.
                    if( !mp_plan_is_ready( control_slot ) &&
                        !mp_plan_is_passed( control_slot ) ) {
                        mp_plan_set_phase( mp_round_phase::planning );
                    }
                    break;
                case mp_control_kind::plan_pass:
                    mp_plan_pass( control_slot );
                    break;
                default:
                    break;
            }
        }
        // A seat that opened or closed a menu while it held the baton changed
        // the `menu` flag its client gates plan keys on. The seat's input wait
        // woke this loop for exactly that; one party body puts the client on
        // the state the seat is actually in, without waiting for another seat's
        // control.
        const bool menu_changed = mp_server_take_menu_change();
        if( controls_handled > 0 ) {
            // The snapshot carries the new plans and any refusal text the loop
            // just stored. The scenes keep their own dirty gate inside the
            // flush and are not sent here: a plan changes the party panel,
            // not the map.
            mp_server_broadcast_party();
            mp_server_broadcast_phase();
        } else if( menu_changed ) {
            // Only the menu flag moved. This body must not activate a seat: a
            // seat reported the change from inside a menu, and the avatar swap
            // an activation performs would expire the item locations that menu
            // is holding.
            mp_server_broadcast_party_ui();
        }

        // A parked seat is owed the baton when it has an event to deliver, a
        // replacement to choose, or a quit to unwind. Those are the three
        // things this thread can be parked for and nothing else: granting a
        // seat that owes none of them would run its 125 ms input poll at the
        // world's pace -- the spin the old barrier had. One seat at a time;
        // yield_to returns once this thread holds the baton again.
        //
        // The event half covers the key a seat took out of its inbox before it
        // parked (mp_server_seat_has_input), and the key that arrived while it
        // was parked here (the inbox half of the same call).
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            if( !sched->waiting_for_baton( i ) ) {
                continue;
            }
            // A scene client that has just left the join phase has no terrain
            // window yet: its first in-game screen is built when its own
            // thread runs the turn loop, and the world's all-seat flush cannot
            // build a frame without that window. A client that sends nothing
            // and waits for its first scene would then be parked for ever on
            // the join screen. This grant is that first draw; the seat's input
            // wait flushes the screen and scene for it. Text clients get their
            // screen from the grant their first key earns, and an away seat is
            // not drawing anything, so neither is woken for this.
            mp_session *const seat = mp_session_of_slot( i );
            const bool needs_first_scene =
                seat != nullptr && ( seat->caps & MP_CAP_SCENE ) != 0 &&
                !mp_player_is_away( i ) && !mp_player_is_disconnected( i ) &&
                ( i >= g->contexts.size() || !g->contexts[i].w_terrain );
            if( mp_server_seat_has_input( i ) || mp_player_needs_replacement( i ) ||
                sched->quit_requested( i ) || mp_server_seat_needs_local_service( i ) ||
                needs_first_scene ) {
                sched->yield_to( mp_sched::world, i );
            }
        }

        if( mp_turn_round_complete() ) {
            mp_sched_trace_phase( "resolve" );
            const bool creature_progress = mp_resolve_round( player_slot_count() );
            // The resolver leaves the settled round for this caller to forget;
            // readiness does not survive into the next round.
            mp_plan_reset_round();
            bool anyone_can_act = false;
            for( size_t i = 0; i < player_slot_count(); ++i ) {
                anyone_can_act = anyone_can_act || mp_turn_player_can_act( i );
            }
            // Another round in this tick while a seat can still act, or while
            // the creatures both can and did: the progress half is what ends a
            // monster-only chain, because a creature that cannot spend (or that
            // zeroed its own moves) makes no progress and the next round would
            // be empty.
            if( anyone_can_act ||
                ( creature_progress && mp_resolve_any_creature_can_act() ) ) {
                mp_plan_set_phase( mp_round_phase::planning );
            } else {
                mp_sched_trace_phase( "tail-begin" );
                mp_turn_close_tick();
                mp_sched_trace_phase( "tail-end" );
            }
            // The round as it now stands, with the phase either back to
            // planning or still the resolver's `done`, and each seat's map
            // after the actions. This one site covers every round of a chain,
            // a monster-only round included, and a close in the same poll --
            // the state the client gets is the final one, with the moves the
            // close handed back.
            mp_server_broadcast_party();
            mp_server_broadcast_phase();
            mp_server_flush_scenes_all();
        }
        return false;
#else
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
        // mp_turn_paced: a turn nobody is playing in still lasts a countdown's
        // length, or a hurry that dropped everyone would let the world run free.
        // mp_turn_all_away: with every living player away there is nobody to
        // wait for and nobody to pace, and the world must not move either.
        while( !mp_barrier_satisfied() || mp_turn_paced() || mp_turn_all_away() ) {
            mp_sched_trace_phase( "poll-begin" );
            mp_tick_hurry();
            bool moved = false;
            std::vector<bool> owes_turn( player_slot_count(), false );
            for( size_t i = 0; i < player_slot_count(); ++i ) {
                const mp_player_state state = mp_player_state_of( i );
                // waiting_for_baton alone is not enough: a player who already
                // acted this turn releases and immediately re-parks on
                // wait_for_turn for the next one, so they report it too -- and
                // yielding to them again would spin the world. Hand the baton
                // only to someone who still owes the turn: acting (not yet
                // released this turn) or holding (returning from an input wait)
                // -- or to a player the hurry left behind, coming back from
                // their input wait with a key. The world is not waiting on them,
                // but they are still playing, and nobody else reads their keys.
                // An away player at an input wait is the same case: the turn
                // may be held open for them by mp_turn_all_away(), and their
                // key is already read off the wire, so the game has to get it.
                owes_turn[i] = state == mp_player_state::acting ||
                               state == mp_player_state::holding ||
                               mp_player_left_behind( i ) ||
                               mp_player_is_away_at_input_wait( i );
#if defined(MP_SERVER)
                // A dead seat with its player still connected owes the session
                // a replacement. It never runs player_act() -- the single
                // global quit is latched there -- but its thread has a branch
                // waiting for exactly this grant.
                owes_turn[i] = owes_turn[i] ||
                               ( mp_player_needs_replacement( i ) &&
                                 sched->waiting_for_baton( i ) );
#endif
                if( owes_turn[i] && sched->waiting_for_baton( i ) ) {
                    sched->yield_to( mp_sched::world, i );
                    moved = true;
                }
            }
            mp_sched_trace_phase( "poll-end" );
            if( !moved ) {
                // Waiting turn: the loop above -- the sole authority on who owes
                // the turn -- found nobody both parked and actionable. Sleep out
                // one poll interval, or less if one of the players who still owe
                // the turn comes back from their input wait, and return; the
                // turn stays open. The wait is on those players only: a player
                // who already acted re-parks on wait_for_turn straight away, and
                // waking on them made this a 100% CPU spin for as long as a
                // partner read a menu. The declared wait tells the watchdog that
                // the world is holding the baton on purpose, so a partner
                // reading a menu does not log as a stalled participant.
                // The declared wait spans the wait itself, not just its
                // return: a poll that lands inside wait_for_request() must
                // already see "the world is waiting on purpose". Clearing it
                // afterwards keeps the flag honest for the polling that
                // follows -- a world that keeps running is accountable.
                mp_sched_trace_phase( "park" );
                sched->set_world_waiting( true );
                sched->wait_for_request( MP_POLL_MS, owes_turn );
                sched->set_world_waiting( false );
                return false;
            }
            if( mp_session_end_requested() ) {
                // Somebody quit from inside their own turn: stop polling for a
                // barrier nobody is going to satisfy.
                return end_session( *sched );
            }
        }
        sched->set_world_waiting( false );
        mp_activate_world();
        mp_sched_trace_phase( "tail-begin" );
        world_turn_post();

        // Standing still is what being dropped means, and it has to be charged
        // before moves are handed back: they accumulate, and a dropped player
        // spends none of them.
        mp_forfeit_dropped_moves();

        // Moves come back after monmove, exactly as in single player. This runs
        // on the world thread under each player's context in turn -- the world
        // keeps the baton -- so anything here that asks for input gets the
        // world's answer (mp_input_must_not_wait).
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
        mp_sched_trace_phase( "tail-end" );
#if defined(EMSCRIPTEN)
        // This will cause a prompt to be shown if the window is closed, until the
        // game is saved.
        EM_ASM( window.game_unsaved = true; );
#endif
        debug_menu::debug_capture::tick_if_active();
        return false;
#endif // defined(MP_SERVER) && defined(MULTIPLAYER)
    }

    if( is_game_over() ) {
        return turn_handler::cleanup_at_end();
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
    // Single player: the whole-tick pass, in the order it has always had.
    // world_tick_block() ends with the cleanup monmove() used to open with, so
    // the block -> monmove -> rest sequence is the original interleaving.
    // A session instead runs mon_tick_pre() at tick open, mon_round_step() per
    // round, and world_turn_post_tail() at tick close.
    world_tick_block();
    monmove();
    world_turn_rest();
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
