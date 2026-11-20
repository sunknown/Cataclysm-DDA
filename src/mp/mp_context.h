#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>

#include "coordinates.h"    // tripoint_bub_ms, point_rel_ms
#include "cursesdef.h"      // catacurses::window
#include "input_enums.h"    // input_event
#include "ui_manager.h"     // ui_stack_t, ui_adaptor
#include "memory_fast.h"    // weak_ptr_fast

class live_view;
class memorial_logger;
class safemode;
class static_popup;
class zone_manager;
struct character_creator_uistate;
namespace auto_pickup
{
class player_settings;
}
namespace auto_notes
{
class auto_note_settings;
}

struct viewport_params {
    int term_cols = 0, term_rows = 0;
    int font_units = 1, tile_units = 1;
    int font_units_v = 1, tile_units_v = 1;
};

struct player_context {
    size_t slot_index = 0;
    viewport_params viewport;
    catacurses::window stdscr;
    catacurses::window w_terrain, w_overmap, w_omlegend, w_minimap, w_pixel_minimap;
    catacurses::window w_terrain_ptr, w_minimap_ptr;
    tripoint_bub_ms ter_view_p;
    point_rel_ms driving_view_offset;
    std::deque<input_event> input_queue;
    int input_timeout_ms = -1;
    ui_stack_t ui_stack;
    weak_ptr_fast<ui_adaptor> main_ui_adaptor;
    // Timestamp of this player's last key-poll rate-limit check (player_act()).
    // Lives per player rather than as a function-local static: a shared static
    // let one idle player's poll reset the timer for another (slot 0 could
    // starve slot 1's handle_key_blocking_activity() for up to 100 ms), and it
    // would race once player_act() runs on per-player threads.
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds> last_key_poll =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );
    // This player's progress/wait popup (crafting, sleeping...) and the flag
    // that decides whether the next handle_progress_ui() forces a full
    // redraw. Both used to live on `game` -- a single instance shared by every
    // player -- which is wrong on two counts: the popup's ui_adaptor has to be
    // created and destroyed under the same ui stack (this player's own,
    // enforced by ui_adaptor's owner == active_stack assert), and a shared
    // "first redraw" flag would let one player's redraw suppress another's.
    std::unique_ptr<static_popup> wait_popup; // NOLINT(cata-serialize)
    bool first_redraw_since_waiting_started = true; // NOLINT(cata-serialize)
    // This player's mouse-view hint. The ui_adaptor it raises has to be created
    // and destroyed under this player's own ui stack (~ui_adaptor asserts
    // owner == active_stack), which one `game`-wide live_view could not honour:
    // one player's key destroyed the hint another player had opened.
    std::unique_ptr<live_view> liveview; // NOLINT(cata-serialize)
    // This player's retained overmap ui_adaptor: fast travel keeps the map up
    // across turns. Per player for the same reason as liveview above --
    // ~ui_adaptor asserts it dies under the stack it registered in -- and
    // because one game-wide shared_ptr let the first seat to close its map
    // null the pointer the other seat's retained redraw asserts on. Access
    // through mp_active_overmap_ui() below.
    std::shared_ptr<ui_adaptor> overmap_ui; // NOLINT(cata-serialize)

    // This player's character-creator menu state (newcharacter.cpp's
    // cc_uistate). It is per player for the same reason everything else here
    // is: two clients can be building a character at the same time, each on its
    // own thread, so one process-wide instance meant one player's tab switch,
    // selection or "finished" flag was another's -- a confirmer could watch
    // their summary tab turn into the scenario tab and never finish. A pointer
    // rather than a value so this header keeps its forward declarations; built
    // on first use by creator_state() below.
    std::unique_ptr<character_creator_uistate> creator_state_ptr; // NOLINT(cata-serialize)

    // This seat's per-character state. All five used to be process-wide
    // singletons, which is wrong for a session with several characters: every
    // character load overwrote the one before it (memorial().load() and
    // auto_notes load(true) clear, zone_manager::deserialize/load_zones replace,
    // the pickup and safemode rules are reassigned), so a character played with
    // someone else's zones, pickup rules, notes, safemode list and memorial log
    // -- and the per-slot save then wrote the last-loaded character's state into
    // every character's files. They are per seat now, and the accessors
    // (get_memorial, get_auto_pickup, get_auto_notes_settings, get_safemode,
    // zone_manager::get_manager) return the active seat's.
    //
    // Pointers rather than values so this header keeps its forward declarations,
    // and created on first use so a seat that never reaches the game pays for
    // nothing. Declared before the constructors below for the same reason
    // wait_popup is: the complete definitions live in mp_context.cpp.
    std::unique_ptr<memorial_logger> memorial;                        // NOLINT(cata-serialize)
    std::unique_ptr<auto_pickup::player_settings> pickup_rules;       // NOLINT(cata-serialize)
    std::unique_ptr<auto_notes::auto_note_settings> auto_notes;       // NOLINT(cata-serialize)
    std::unique_ptr<safemode> safemode_rules;                         // NOLINT(cata-serialize)
    std::unique_ptr<zone_manager> zones;                              // NOLINT(cata-serialize)

    // This character's playtime before the session started, as its own `.pt`
    // file recorded it. Per seat because each character has its own total: one
    // shared baseline wrote the first-loaded character's hours into every
    // character's file.
    std::chrono::seconds time_played_at_last_load{};

    // Declared (not defaulted inline) because wait_popup needs the complete
    // definition of static_popup to construct/destroy, and this header only
    // forward-declares it -- exactly the reason `game` itself declares
    // game()/~game() instead of defaulting them (game.h/game.cpp). Defined in
    // mp_context.cpp, which includes popup.h. The same holds for
    // creator_state_ptr's character_creator_uistate.
    player_context();
    ~player_context();

    // The creator state of this player, created on first use.
    character_creator_uistate &creator_state();

    // Drops this seat's mouse-view hint under the stack it registered in, then
    // puts the caller's stack back. The seat handover paths call this: the hint
    // belonged to the terminal that left.
    void drop_live_view();

    // This seat's per-character state, each created on first use. The memorial
    // also subscribes itself to the game's event bus on creation: a log that is
    // not subscribed never records anything, and one shared subscriber would
    // put every player's deeds in one file.
    memorial_logger &memorial_state();
    // This seat's memorial if it has already been created, nullptr otherwise.
    // Read by memorial_logger::notify(), which must not create one: doing so
    // would mutate the event bus's subscriber list mid-broadcast.
    memorial_logger *memorial_ptr() const;
    auto_pickup::player_settings &pickup_rules_state();
    auto_notes::auto_note_settings &auto_notes_state();
    safemode &safemode_state();
    zone_manager &zones_state();
};

player_context &mp_active_context();       // asserts a non-null active context
player_context *mp_active_context_ptr();   // raw pointer, may be nullptr (for reset/tests)
// The active player's mouse-view hint, created on first use (like wait_popup).
// Being per player is the point: its ui_adaptor registers in the active
// context's ui stack and is destroyed there, so no other player's key can
// close it.
live_view &mp_active_live_view();
// The active player's retained overmap ui_adaptor. Falls back to the game's
// overmap_data.ui when no context is installed: single-player installs
// contexts[0] and uses the context path, so the fallback serves callers that
// run before or without one (the main menu, tests that never install one).
std::shared_ptr<ui_adaptor> &mp_active_overmap_ui();
void mp_set_active_context( player_context *ctx ); // nullptr = reset; reading asserts

void mp_apply_viewport_params( const player_context &ctx );

// Creates game::contexts[0] if needed, fills its viewport from TERMX/TERMY,
// mirrors catacurses::stdscr, applies the viewport params and makes the context
// active. Always sets an active context, even when contexts was already non-empty
// (otherwise after contexts.clear() the active pointer would dangle).
void mp_ensure_player_context();
// The single writer of the global aliases: swaps the current slot out of u and
// slot idx in, then re-points stdscr, the seven game windows, the TERMX/TERMY
// metrics, the active ui stack and the active player context from contexts[0].
// Callers must have called mp_ensure_player_context() first (which itself ends
// with mp_activate_player( 0 )).
void mp_activate_player( size_t idx );
// Re-reads the viewport from TERMX/TERMY, applies the metrics, creates the
// context's windows, re-points game's window aliases to them and repositions
// the main ui_adaptor from the context's stdscr.
void mp_resize_active_context();

// Grows or shrinks game::contexts to match game::player_slot_count(). New
// contexts inherit the terminal size of context 0, which is what a freshly
// seeded slot wants until its client says otherwise. Callers that add or
// remove a slot must call this; the active context is re-pointed afterwards
// because a shrink can invalidate it.
void mp_sync_context_count();

// Enters the world phase: nobody is acting. The ui stack becomes the world
// stack (a real object, not nullptr -- it catches the emergency UI that
// debugmsg raises).
//
// The avatar aliases and the active context both stay on slot 0, so any code
// that still reads them sees live state rather than a dangling or null one.
// The world half genuinely reaches such code -- the autosave in
// world_turn_pre() serializes mp_active_context().driving_view_offset -- and
// the phase is about who may act, not about which state exists.
void mp_activate_world();

// True while the world phase is active (no player is acting). The active context
// pointer is preserved during this phase for code that reads it, so this flag is
// the reliable way to distinguish "a player owns the turn" from "the world runs".
bool mp_in_world_phase();

// Runs `fn` once per player context, activating each in turn and restoring the
// previously active one afterwards. Used by the broadcast paths: world-side
// code that has to draw into every player's screen (explosions, animations).
void mp_for_each_context( const std::function<void( player_context & )> &fn );
