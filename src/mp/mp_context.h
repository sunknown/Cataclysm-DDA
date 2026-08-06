#pragma once

#include <cstddef>
#include <deque>

#include "coordinates.h"    // tripoint_bub_ms, point_rel_ms
#include "cursesdef.h"      // catacurses::window
#include "input_enums.h"    // input_event
#include "ui_manager.h"     // ui_stack_t, ui_adaptor
#include "memory_fast.h"    // weak_ptr_fast

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
};

player_context &mp_active_context();       // ассертит ненулевой активный контекст
player_context *mp_active_context_ptr();   // сырой указатель, может быть nullptr (для сброса/тестов)
void mp_set_active_context( player_context *ctx ); // nullptr = сброс; чтение ассертит

void mp_apply_viewport_params( const player_context &ctx );
