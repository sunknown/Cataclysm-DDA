#include "cata_catch.h"

#include <memory>

#include "cata_scope_helpers.h"
#include "mp/mp_context.h"
#include "output.h"
#include "panels.h"

TEST_CASE( "mp_apply_viewport_params_computes_metrics_from_the_context", "[mp]" )
{
    // Метрики — глобалы; записать и восстановить все, чтобы не засорять соседние тесты.
    restore_on_out_of_scope restore_termx( TERMX );
    restore_on_out_of_scope restore_termy( TERMY );
    restore_on_out_of_scope restore_tww( TERRAIN_WINDOW_WIDTH );
    restore_on_out_of_scope restore_twh( TERRAIN_WINDOW_HEIGHT );
    restore_on_out_of_scope restore_twtw( TERRAIN_WINDOW_TERM_WIDTH );
    restore_on_out_of_scope restore_twth( TERRAIN_WINDOW_TERM_HEIGHT );
    restore_on_out_of_scope restore_posx( POSX );
    restore_on_out_of_scope restore_posy( POSY );

    // Терминал 120x40, боковые панели по умолчанию. Проверяем, что метрики —
    // чистая функция от viewport, без скрытых источников.
    player_context ctx;
    ctx.viewport.term_cols = 120;
    ctx.viewport.term_rows = 40;

    mp_apply_viewport_params( ctx );

    CHECK( TERMX == 120 );
    CHECK( TERMY == 40 );
    // Террейн-окно: вся ширина за вычетом боковых панелей.
    const int sidebar = panel_manager::get_manager().get_width_left() +
                        panel_manager::get_manager().get_width_right();
    CHECK( TERRAIN_WINDOW_WIDTH + sidebar == 120 );
    CHECK( TERRAIN_WINDOW_HEIGHT == 40 );
    CHECK( POSX == TERRAIN_WINDOW_WIDTH / 2 );
    CHECK( POSY == TERRAIN_WINDOW_HEIGHT / 2 );
}

namespace {

// Восстанавливает активный контекст на предыдущий (может быть nullptr).
class restore_active_context
{
        player_context *const saved_;
    public:
        restore_active_context() : saved_( mp_active_context_ptr() ) {}
        ~restore_active_context() { mp_set_active_context( saved_ ); }
};

} // namespace

TEST_CASE( "mp_active_context_follows_the_last_set_context", "[mp]" )
{
    restore_active_context restore;
    player_context a;
    player_context b;

    mp_set_active_context( &a );
    CHECK( &mp_active_context() == &a );
    mp_set_active_context( &b );
    CHECK( &mp_active_context() == &b );
    mp_set_active_context( &a );
    CHECK( &mp_active_context() == &a );
}

TEST_CASE( "view_state_written_under_one_context_is_invisible_to_another", "[mp]" )
{
    restore_active_context restore;
    player_context a;
    player_context b;

    mp_set_active_context( &a );
    mp_active_context().ter_view_p = tripoint_bub_ms( 5, 6, 0 );
    mp_active_context().driving_view_offset = point_rel_ms( 1, 2 );

    mp_set_active_context( &b );
    // Вью-стейт контекста B не тронут активацией A.
    CHECK( mp_active_context().ter_view_p == tripoint_bub_ms() );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms() );

    mp_set_active_context( &a );
    CHECK( mp_active_context().ter_view_p == tripoint_bub_ms( 5, 6, 0 ) );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms( 1, 2 ) );
}
