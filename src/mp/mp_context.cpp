#include "mp/mp_context.h"

#include <cassert>

#include "cata_utility.h"
#include "game.h"
#include "game_ui.h"
#include "output.h"
#include "panels.h"

static player_context *active_context = nullptr;

player_context &mp_active_context()
{
    cata_assert( active_context != nullptr );
    return *active_context;
}

player_context *mp_active_context_ptr()
{
    return active_context;
}

void mp_set_active_context( player_context *ctx )
{
    // nullptr — легальный сброс (нужен тестам и, в Ф3, выходу из игрока);
    // чтение через mp_active_context() ассертит ненулевой указатель.
    active_context = ctx;
}

void mp_apply_viewport_params( const player_context &ctx )
{
    // Базовые TERMX/TERMY уже установлены game_ui::init_ui() до любого
    // колбэка; контекст заполняется из них (задачи 3), поэтому эти записи
    // тождественны в Ф2 и станут реальными в Ф3, когда размер терминала
    // будет следовать за активным игроком.
    TERMX = ctx.viewport.term_cols;
    TERMY = ctx.viewport.term_rows;

    const int sidebar_left = panel_manager::get_manager().get_width_left();
    const int sidebar_right = panel_manager::get_manager().get_width_right();

    TERRAIN_WINDOW_HEIGHT = TERMY;
    TERRAIN_WINDOW_WIDTH = TERMX - ( sidebar_left + sidebar_right );
    TERRAIN_WINDOW_TERM_WIDTH = TERRAIN_WINDOW_WIDTH;
    TERRAIN_WINDOW_TERM_HEIGHT = TERRAIN_WINDOW_HEIGHT;

    // Террейн-окно считается в тайлах карты; шрифты терминала и карты могут
    // отличаться. В серверной сборке пересчёта нет: единицы уже учтены тем,
    // кто заполнил viewport_params (клиентом), а to_map_font_dimension читал бы
    // font-глобалы, которых на сервере нет.
#if !defined(MP_SERVER)
    to_map_font_dimension( TERRAIN_WINDOW_WIDTH, TERRAIN_WINDOW_HEIGHT );
#endif

    POSX = TERRAIN_WINDOW_WIDTH / 2;
    POSY = TERRAIN_WINDOW_HEIGHT / 2;
}
