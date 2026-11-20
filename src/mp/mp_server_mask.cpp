#include "mp/mp_server_mask.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "cursesdef.h"
#include "cursesport.h"
#include "mp/mp_context.h"
#include "mp/mp_scene.h"

namespace
{

// A role is empty only where nothing overlays the map. Roles themselves use
// the whole uint8_t range, so -1 marks a cell the UI does not cover.
constexpr int16_t EMPTY_CELL = -1;

struct role_entry {
    std::weak_ptr<void> owner;
    uint8_t role = 0;
};

// Roles are keyed by native pointer but own nothing: once the window behind the
// pointer dies the weak reference expires and the entry reads as role 0.
std::map<const void *, role_entry> &role_registry()
{
    static std::map<const void *, role_entry> registry;
    return registry;
}

// One cell of the map window, in map-local coordinates. Geometry is rebuilt
// whenever the active terrain window changes (resize, slot switch, a test
// assigning its own window).
struct mask_grid {
    bool active = false;
    const void *window_key = nullptr;
    uint16_t ox = 0;
    uint16_t oy = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    std::vector<int16_t> roles;
};

mask_grid &grid()
{
    static mask_grid state;
    return state;
}

// Re-reads the map window's rect and resets the cells when it changed. Returns
// false when there is no map window to cover, in which case the mask API is a
// silent no-op.
bool sync_geometry()
{
    player_context *const ctx = mp_active_context_ptr();
    if( ctx == nullptr || !ctx->w_terrain ) {
        return false;
    }
#if defined(TUI)
    // The mask hook lives in the MP server's curses backend; the terminal
    // backends have no in-memory window rect to read.
    return false;
#else
    cata_cursesport::WINDOW *const win =
        ctx->w_terrain.get<cata_cursesport::WINDOW>();
    if( win == nullptr || win->width <= 0 || win->height <= 0 ) {
        return false;
    }
    mask_grid &g = grid();
    const void *const key = ctx->w_terrain.get<void>();
    const uint16_t ox = static_cast<uint16_t>( win->pos.x );
    const uint16_t oy = static_cast<uint16_t>( win->pos.y );
    const uint16_t w = static_cast<uint16_t>( win->width );
    const uint16_t h = static_cast<uint16_t>( win->height );
    if( !g.active || g.window_key != key || g.ox != ox || g.oy != oy ||
        g.w != w || g.h != h ) {
        g.active = true;
        g.window_key = key;
        g.ox = ox;
        g.oy = oy;
        g.w = w;
        g.h = h;
        g.roles.assign( static_cast<size_t>( w ) * h, EMPTY_CELL );
    }
    return true;
#endif
}

} // namespace

void mp_scene_mask_clear()
{
    if( !sync_geometry() ) {
        return;
    }
    std::fill( grid().roles.begin(), grid().roles.end(), EMPTY_CELL );
}

void mp_scene_mask_note( const uint16_t x, const uint16_t y, const uint16_t w,
                         const uint16_t h, const uint8_t role )
{
    if( !sync_geometry() ) {
        return;
    }
    mask_grid &g = grid();
    const int left = std::max<int>( x, g.ox );
    const int top = std::max<int>( y, g.oy );
    const int right = std::min<int>( static_cast<int>( x ) + w,
                                     static_cast<int>( g.ox ) + g.w );
    const int bottom = std::min<int>( static_cast<int>( y ) + h,
                                      static_cast<int>( g.oy ) + g.h );
    for( int row = top; row < bottom; ++row ) {
        const size_t base = static_cast<size_t>( row - g.oy ) * g.w;
        for( int col = left; col < right; ++col ) {
            g.roles[base + static_cast<size_t>( col - g.ox )] = role;
        }
    }
}

void mp_scene_mask_register_role( const catacurses::window &win, const uint8_t role )
{
    const void *const key = win.get<void>();
    if( key == nullptr ) {
        return;
    }
    if( role == 0 ) {
        // Role 0 is the default for unregistered windows; keeping an entry
        // would only shadow that.
        role_registry().erase( key );
        return;
    }
    // Panel windows are recreated every draw, so a long session would
    // otherwise pile up one dead entry per frame: drop the expired ones now.
    std::map<const void *, role_entry> &registry = role_registry();
    for( auto it = registry.begin(); it != registry.end(); ) {
        if( it->second.owner.expired() ) {
            it = registry.erase( it );
        } else {
            ++it;
        }
    }
    registry[key] = role_entry{ win.weak_ptr(), role };
}

uint8_t mp_scene_mask_role_for( const catacurses::window &win )
{
    const void *const key = win.get<void>();
    if( key == nullptr ) {
        return 0;
    }
    std::map<const void *, role_entry> &registry = role_registry();
    const auto it = registry.find( key );
    if( it == registry.end() ) {
        return 0;
    }
    if( it->second.owner.expired() ) {
        registry.erase( it );
        return 0;
    }
    return it->second.role;
}

std::vector<mp_scene_rect> mp_scene_mask_rects()
{
    std::vector<mp_scene_rect> rects;
    if( !sync_geometry() ) {
        return rects;
    }
    const mask_grid &g = grid();
    // Runs still open in the previous row, keyed by (start, width, role), so
    // vertically equal spans extend the rect instead of starting a new one.
    std::map<std::tuple<int, int, int>, size_t> open;
    for( int row = 0; row < g.h; ++row ) {
        std::map<std::tuple<int, int, int>, size_t> next;
        const size_t base = static_cast<size_t>( row ) * g.w;
        int col = 0;
        while( col < g.w ) {
            const int role = g.roles[base + static_cast<size_t>( col )];
            if( role == EMPTY_CELL ) {
                ++col;
                continue;
            }
            int end = col + 1;
            while( end < g.w &&
                   g.roles[base + static_cast<size_t>( end )] == role ) {
                ++end;
            }
            const std::tuple<int, int, int> key( col, end - col, role );
            const auto open_it = open.find( key );
            if( open_it != open.end() ) {
                ++rects[open_it->second].h;
                next.emplace( key, open_it->second );
            } else {
                rects.push_back( mp_scene_rect{
                        static_cast<uint16_t>( g.ox + col ),
                        static_cast<uint16_t>( g.oy + row ),
                        static_cast<uint16_t>( end - col ), 1,
                        static_cast<uint8_t>( role ) } );
                next.emplace( key, rects.size() - 1 );
            }
            col = end;
        }
        open = std::move( next );
    }
    return rects;
}

uint64_t mp_scene_mask_hash()
{
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash]( const uint8_t byte ) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for( const mp_scene_rect &rect : mp_scene_mask_rects() ) {
        mix( static_cast<uint8_t>( rect.x >> 8 ) );
        mix( static_cast<uint8_t>( rect.x & 0xff ) );
        mix( static_cast<uint8_t>( rect.y >> 8 ) );
        mix( static_cast<uint8_t>( rect.y & 0xff ) );
        mix( static_cast<uint8_t>( rect.w >> 8 ) );
        mix( static_cast<uint8_t>( rect.w & 0xff ) );
        mix( static_cast<uint8_t>( rect.h >> 8 ) );
        mix( static_cast<uint8_t>( rect.h & 0xff ) );
        mix( rect.role );
    }
    return hash;
}
