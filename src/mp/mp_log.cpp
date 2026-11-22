#include "mp/mp_log.h"

#include <algorithm>
#include <cstdint>

#include "panels.h"

namespace
{

mp_scene_rect source_rect;
bool source_rect_valid = false;
bool ui_active = false;

} // namespace

void mp_native_log_note_rect( const int x, const int y, const int w, const int h )
{
    if( w <= 0 || h <= 0 ) {
        // A panel with no area is no panel. Keep the origin out of the wire.
        source_rect = {};
        source_rect_valid = false;
        return;
    }
    source_rect.x = static_cast<uint16_t>( std::max( 0, x ) );
    source_rect.y = static_cast<uint16_t>( std::max( 0, y ) );
    source_rect.w = static_cast<uint16_t>( w );
    source_rect.h = static_cast<uint16_t>( h );
    source_rect_valid = true;
}

bool mp_native_log_source_rect( mp_scene_rect &out )
{
    if( !source_rect_valid ) {
        return false;
    }
    out = source_rect;
    return true;
}

void mp_native_log_clear_rect()
{
    source_rect = {};
    source_rect_valid = false;
    ui_active = false;
}

bool mp_native_log_panel_enabled()
{
    for( const window_panel &panel :
         panel_manager::get_manager().get_current_layout().panels() ) {
        if( panel.get_id() == "Log" ) {
            return panel.toggle && panel.render();
        }
    }
    return false;
}

void mp_native_log_note_ui()
{
    ui_active = true;
}

bool mp_native_log_ui_active()
{
    return ui_active;
}
