#pragma once
#ifndef CATA_SRC_MP_MP_CELL_H
#define CATA_SRC_MP_MP_CELL_H

#include <cstdint>
#include <string>
#include <vector>

// One character cell of a player's screen.
//
// `ch` is the UTF-8 glyph, mirroring cata_cursesport::cursecell: an empty
// string marks the second column of a double-width character.
//
// `fg` and `bg` are indices into the 256 colour space. Values 0-15 come from
// cursesport, which folds bold and blink into the base colour by adding 8
// (src/cursesport.cpp:489-506); 237 is dark_gray; values above 15 come from
// imgui by way of imtui, which works in ansi256. The client maps them with
// its own palette.
struct mp_cell {
    std::string ch = " ";
    uint8_t fg = 7;
    uint8_t bg = 0;

    bool operator==( const mp_cell &rhs ) const {
        return ch == rhs.ch && fg == rhs.fg && bg == rhs.bg;
    }
    bool operator!=( const mp_cell &rhs ) const {
        return !operator==( rhs );
    }
};

// A screen of cells with per-row change tracking. Row granularity mirrors
// cata_cursesport::curseline::touched, so the server can copy the game's own
// notion of "what changed" straight through.
class mp_screen_grid
{
    public:
        mp_screen_grid( int width, int height );

        // Discards all content and marks the whole screen dirty.
        void resize( int width, int height );

        int width() const {
            return width_;
        }
        int height() const {
            return height_;
        }

        const mp_cell &at( int x, int y ) const;
        // Out of range writes are ignored, matching curses semantics.
        void set( int x, int y, const mp_cell &c );

        void mark_all_dirty();
        void clear_dirty();
        bool any_dirty() const;
        std::vector<int> dirty_rows() const;

    private:
        int width_ = 0;
        int height_ = 0;
        std::vector<mp_cell> cells_;
        std::vector<bool> dirty_;
};

#endif // CATA_SRC_MP_MP_CELL_H
