#if defined(MP_SERVER)

#include "mp/mp_imgui_server.h"

#include <cstdint>
#include <string>
#include <vector>

#include "catacharset.h"
#include "mp/mp_cell.h"
#include "mp/mp_server_screen.h"
#include <imtui/imtui.h>

namespace
{

// The imtui text backend renders into the process-global TScreen owned by the
// ImTui::ImplImtui_Data that ImTui_ImplNcurses_Init would otherwise allocate.
// The server forks that Init away (there is no ncurses terminal), so the
// blitter allocates the shared Imtui data itself when nothing else has. When
// no ImGui context exists (the mp backend tests call the hook directly) the
// data lives in a static fallback instead, which is fine: the hook and the
// blitter both reach it through this function.
ImTui::ImplImtui_Data &ensure_imtui_data()
{
    if( ImTui::ImplImtui_Data *data = ImTui::ImTui_Impl_GetBackendData() ) {
        return *data;
    }
    if( ImGui::GetCurrentContext() != nullptr ) {
        ImTui::ImplImtui_Data *data = new ImTui::ImplImtui_Data();
        ImGui::GetIO().BackendPlatformUserData = data;
        return *data;
    }
    static ImTui::ImplImtui_Data fallback;
    return fallback;
}

} // namespace

void mp_test_fill_imtui_screen( const int w, const int h, const char ch,
                                const uint8_t fg, const uint8_t bg )
{
    // Pure test hook: no game state is touched. Filling every cell with the
    // same glyph and colour lets the blitter be exercised end to end without
    // booting the whole game.
    ImTui::TScreen &screen = ensure_imtui_data().Screen;
    screen.resize( w, h );
    for( int i = 0; i < screen.size(); ++i ) {
        ImTui::TCell &cell = screen.data[i];
        cell.fg = fg;
        cell.bg = bg;
        cell.ch = static_cast<uint32_t>( ch );
        cell.chwidth = 1;
    }
}

void mp_imgui_draw_screen()
{
    ImTui::TScreen &screen = ensure_imtui_data().Screen;
    const int grid_width = mp_server_grid().width();
    const int grid_height = mp_server_grid().height();

    std::vector<mp_cell> row;
    row.reserve( grid_width );
    for( int y = 0; y < screen.ny && y < grid_height; ++y ) {
        row.clear();
        for( int x = 0; x < screen.nx && x < grid_width; ++x ) {
            const ImTui::TCell &cell = screen.data[y * screen.nx + x];
            if( cell.ch == 0 ) {
                // The text renderer writes only the cells an imgui window
                // touched; everything else stays at zero from the per-frame
                // clear. Carrying the current grid cell through leaves the
                // curses content underneath intact, the same layering the TUI
                // backend gets by drawing its imgui windows on top of the
                // game's curses windows. Writing the identical value is also
                // what keeps an untouched row from being marked dirty.
                row.push_back( mp_server_grid().at( x, y ) );
                continue;
            }
            mp_cell out;
            out.ch = utf32_to_utf8( cell.ch );
            out.fg = cell.fg;
            out.bg = cell.bg;
            row.push_back( out );
            if( cell.chwidth > 1 ) {
                // A double-width glyph claims the next column as an empty
                // continuation cell, exactly how cursesport lays them out. The
                // text renderer leaves the TScreen's own continuation columns
                // unfilled, so skip them after writing ours.
                if( x + 1 < grid_width ) {
                    mp_cell continuation;
                    continuation.ch.clear(); // empty ch marks the second column
                    continuation.fg = cell.fg;
                    continuation.bg = cell.bg;
                    row.push_back( continuation );
                }
                x += cell.chwidth - 1;
            }
        }
        mp_server_blit_row( 0, y, row );
    }
}

#endif // MP_SERVER
