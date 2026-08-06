# Фаза 2: контекст игрока — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** у каждого игрока появляется явная структура `player_context` (окна, вью-стейт, ввод, стек UI, главный адаптор), атомарно перенацеливаемые глобальные алиасы при активации, при этом в обычной игре своп тождественен и ничего не ломается.

**Architecture:** контекст живёт в обеих сборках (один контекст в обычной игре — ровно как слот нулевой). `mp_activate_player(idx)` — единственный писатель глобальных алиасов: своп аватара через существующий `swap_slot_into_u` (приватный), ре-пойнт `stdscr`/окон, пересчёт метрик из `viewport_params`, переключение активного стека `ui_manager` и активного контекста для вью-стейта и ввода. `ui_manager` держит указатель на активный стек (никогда не `nullptr`). Вью-стейт читается через активный контекст, в `game` не копируется. Метрики вычисляются из одного входа, а не синхронизируются.

**Tech Stack:** C++17, собственные `player_context` (`src/mp/mp_context.h/.cpp`), существующие `swap_slot_into_u`, `ui_manager`, `cata_cursesport`, Catch2 (`tests/cata_test`), Makefile (авторитетный путь) + CMake.

## Global Constraints

- **Спеки:** `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md` (§5.3, §5.4, §10, §11 Ф2) и `docs/superpowers/specs/2026-08-06-mp-phase2-context-design.md`.
- **Скоуп Ф2:** один контекст в обеих сборках, своп тождественный. Второй игрок, своп на эстафете, разрез `do_turn` на `world_turn`/`avatar_turn`, `mp_activate_world`, `mp_for_each_context` — **Ф3**, в этом плане не реализуются.
- **Сетевой протокол не меняется.** `status`-сообщение — долг Ф4.
- **Сайдбар:** Ф2 `panel_manager` не трогает; сайдбар в серверной сборке рисуется общим менеджером, как сейчас.
- **Копи-синк туда-обратно запрещён.** У каждого поля один источник: ре-пойнт (объекты) или чтение через активный контекст (вью-стейт, ввод, `ui_stack`, главный адаптор).
- **Сборка тестов (обычная):** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`. Запуск: `tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`.
- **Сборка тестов (серверная):** `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`. Запуск: `tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`.
- **Регресс:** весь тестовый набор обычной сборки зелёный (259 файлов `tests/*.cpp` на момент плана; спека называет «266» — считает тест-кейсы) — главный критерий безвредности рефактора на каждом шаге.
- **Стиль:** `make astyle`. Комментарии по-английски (кроме документов плана).
- **Коммиты:** после каждой задачи, префиксы `feat(mp):`, `refactor(mp):`, `test(mp):`, `build:`, `docs:`.
- **Заглушки запрещены.** Каждый символ появляется с окончательным поведением.
- **Отступление от спека §7.2, зафиксированное явно:** `mp_context_swap_test` гейтится `#if defined(MP_SERVER)`, а не «обычная сборка». Причина: самосравненец рисует клетки в catacurses-окна, а in-memory curses (`cursesport`) компилируется только в серверной сборке — в обычной (ncurses) сборке окна headless не создаются (прецедент: `mp_server_backend_test`). Механизм контекста при этом по-прежнему прогоняется в обычной сборке (`mp_context_test` + 266 тестов); ворота «не зависят от MULTIPLAYER-сборки» сохранены.
- **Значения, разрешённые при планировании** — в конце документа.

---

## Обзор файлов

**Создаются:**

| Файл | Ответственность |
|---|---|
| `src/mp/mp_context.h`, `.cpp` | `viewport_params`, `player_context`, активный контекст, метрики, активация, создание окон |
| `tests/mp_context_test.cpp` | Юнит контекста и свопа (обычная сборка) |
| `tests/mp_context_swap_test.cpp` | Самосравненец A→B→A по содержимому ячеек (только `MP_SERVER`) |
| `tests/game_test_helper.h` | Общий тест-хелпер: сброс слотов (`game_test_helper`, переезжает из `hotseat_test.cpp`) + RAII-восстановитель активного контекста/стека (`restore_active_state`) |

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/game.h` | `#include "mp/mp_context.h"`, член `std::deque<player_context> contexts` (стабильные адреса), `friend void mp_activate_player(size_t)`; удалить `ter_view_p` (:1272), `driving_view_offset` (:1286), `main_ui_adaptor` (:1380); `swap_slot_into_u` → private |
| `src/game.cpp` | `create_or_get_main_ui_adaptor` → контекст; resize-колбэк → `mp_resize_active_context`; `invalidate_main_ui_adaptor`/`mark_main_ui_adaptor_resize` → широковещательные; `draw_pixel_minimap` (:3500) → активный контекст; запись `TERMINAL_X/Y` (:550-552) закрыта в серверной сборке |
| `src/ui_manager.h` | Тип `ui_stack_t` переезжает сюда; `ui_manager::set_active_stack`; член `ui_stack_t *owner` в `ui_adaptor` |
| `src/ui_manager.cpp` | `static ui_stack_t *active_stack` + `world_stack`; ~40 использований; регистрация/дерегистрация в `*active_stack`; дебаг-ассерт владения |
| `src/mp/mp_hotseat_loop.cpp` | Строки 34 и 75 переходят на `mp_activate_player` |
| `src/mp/mp_server_input.cpp` | Очередь нажатий через активный контекст |
| `src/mp/mp_curses_server.cpp` | Таймаут ввода через активный контекст |
| `src/overmap_ui.cpp` | `:2104` создаёт `w_overmap` в активном контексте |
| `src/action.cpp`, `src/animation.cpp`, `src/construction.cpp`, `src/editmap.cpp`, `src/handle_action.cpp`, `src/ranged.cpp`, `src/do_turn.cpp`, `src/savegame.cpp`, `src/cata_tiles.cpp`, `src/sdltiles.cpp` | Сайты `ter_view_p`/`driving_view_offset` → `mp_active_context()`, `show_adm` закрыт в серверной сборке |
| `tests/hotseat_test.cpp` | `:184` переходит на `mp_activate_player(1)`; локальный `game_test_helper` переезжает в `tests/game_test_helper.h` |
| `tests/mp_server_io_test.cpp` | Настройка активного контекста перед вызовами ввода |
| `Makefile`, `CMakeLists.txt` | `src/mp/mp_context.cpp` в обычную сборку; исправление mp-глоба в CMake |

**Почему так.** `player_context` — единственный новый модуль в `src/mp/`, собирается всегда. Всё, что можно проверить без игры (метрики, активный контекст, стек), проверяется в обычной сборке; то, что требует окон (рисование ячеек) — в серверной сборке, где in-memory curses. Изменения существующих файлов — это перенос владения (окна, адаптор), перенацеливание алиасов и 30 механических правок вью-стейта.

---

### Task 1: `mp_context` — структуры, метрики, активный контекст

**Files:**
- Create: `src/mp/mp_context.h`, `src/mp/mp_context.cpp`
- Create: `tests/mp_context_test.cpp` (первая часть)
- Modify: `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `catacurses::window` (`cursesdef.h`), `ui_stack_t` (переносится в `ui_manager.h` в шаге 1 этой задачи), `input_event` (`input_enums.h`), `tripoint_bub_ms`/`point_rel_ms` (`coordinates.h`), глобалы `TERMX`/`TERMY`/`TERRAIN_WINDOW_*`/`POSX`/`POSY` (`output.h`).
- Produces (полные сигнатуры, на них опираются задачи 3–7):

```cpp
// Всё, что нужно знать серверу о дисплее игрока. Прислано клиентом; в Ф2
// term_cols/term_rows зеркалят TERMX/TERMY, которые пишет game_ui::init_ui()
// (в серверной сборке это mp_server_grid). font_units/tile_units равны 1 и в
// Ф2 не потребляются (формула w * font_units / tile_units оживает в tiles-фазе).
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

// Писатель производных метрик экрана (TERRAIN_WINDOW_*, POSX, POSY) и зеркала
// TERMX/TERMY из контекста. Базовые TERMX/TERMY пишет game_ui::init_ui()
// (game.cpp:514) до любых колбэков; контекст заполняется из них (задачи 3),
// поэтому запись TERMX/TERMY здесь тождественна — она нужна для Ф3, когда
// размер терминала будет следовать за активным игроком.
// OVERMAP_* и FULL_SCREEN_* вычисляются своими существующими писателями как
// чистые функции от TERMX/TERMY — консистентность по построению.
void mp_apply_viewport_params( const player_context &ctx );
```

**Примечание по `ui_stack_t`:** поле `player_context::ui_stack` требует полный тип `ui_stack_t`. Тип переезжает в `ui_manager.h` в задаче 2; в задаче 1 объявить `ui_stack_t` в `ui_manager.h` сразу (это тривиальный перенос одной строки `using ui_stack_t = std::vector<std::reference_wrapper<ui_adaptor>>;` из `ui_manager.cpp:24`) — иначе `mp_context.h` не скомпилируется. Задача 2 продолжает рефактор `ui_manager`.

- [ ] **Step 1: Перенести `ui_stack_t` в `ui_manager.h`**

В `src/ui_manager.h`, до `class ui_adaptor`, добавить:
```cpp
using ui_stack_t = std::vector<std::reference_wrapper<ui_adaptor>>;
```
В `src/ui_manager.cpp:24` удалить строку-алиас (само `static ui_stack_t ui_stack;` остаётся, трогается в задаче 2).

- [ ] **Step 2: Создать `src/mp/mp_context.h`**

```cpp
#pragma once

#include <cstddef>
#include <deque>

#include "coordinates.h"    // tripoint_bub_ms, point_rel_ms
#include "cursesdef.h"      // catacurses::window
#include "input_enums.h"    // input_event
#include "ui_manager.h"     // ui_stack_t, ui_adaptor
#include "memory.h"         // weak_ptr_fast

struct viewport_params { /* как в Interfaces */ };
struct player_context { /* как в Interfaces */ };

player_context &mp_active_context();       // ассертит ненулевой активный контекст
player_context *mp_active_context_ptr();   // сырой указатель, может быть nullptr
void mp_set_active_context( player_context *ctx ); // nullptr = сброс, чтение ассертит
void mp_apply_viewport_params( const player_context &ctx );
```

- [ ] **Step 3: Написать падающий тест — метрики**

Создать `tests/mp_context_test.cpp`:

```cpp
#include "cata_catch.h"

#include <memory>

#include "mp/mp_context.h"
#include "output.h"

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
```
Заголовок `#include "cata_scope_helpers.h"` в тест.

Заголовки: `panel_manager` даёт `src/panels.h`. Добавить `#include "panels.h"` в тест.

- [ ] **Step 4: Написать падающие тесты — активный контекст и изоляция вью-стейта**

Тесты ставят глобальный активный контекст на стековые локали — восстановление обязательно, иначе следующий тест пишет в освобождённую память. RAII-хелпер (в `tests/mp_context_test.cpp`):

```cpp
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
```

- [ ] **Step 5: Запустить тесты — убедиться, что падают**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: FAIL — `mp_context.h` не существует, `mp_apply_viewport_params`/`mp_set_active_context` не объявлены.

- [ ] **Step 6: Реализовать `src/mp/mp_context.cpp`**

```cpp
#include "mp/mp_context.h"

#include <cassert>

#include "cata_utility.h"   // weak_ptr_fast
#include "game.h"
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
```

`to_map_font_dimension` объявлен в `src/game_ui.h` — добавить `#include "game_ui.h"`. `get_width_left/right` — `src/panels.h`. `weak_ptr_fast` живёт в `cata_utility.h` (проверить при компиляции, при необходимости взять из `memory.h`).

- [ ] **Step 7: Добавить `src/mp/mp_context.cpp` в сборку**

В `Makefile` перед блоком `ifeq ($(MULTIPLAYER),1)` (строка ~1115):
```make
# The player context ships in every build; the rest of src/mp/ is MULTIPLAYER-only.
SOURCES += $(SRC_DIR)/mp/mp_context.cpp
```
CMake — **в `src/CMakeLists.txt`, не в корневом.** Корневой `CMakeLists.txt:483-487` аппендит в `CATACLYSM_DDA_SOURCES`, но `src/CMakeLists.txt:16` делает `file(GLOB CATACLYSM_DDA_SOURCES ...)` в дочернем скоупе и затирает значение родителя — поэтому и путь, и сам файл кладутся в `src/CMakeLists.txt`:

```cmake
# src/CMakeLists.txt, после GLOB-блока CATACLYSM_DDA_SOURCES:
# The player context ships in every build; the rest of src/mp/ is MULTIPLAYER-only.
list(APPEND CATACLYSM_DDA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mp/mp_context.cpp)

if (MULTIPLAYER)
    file(GLOB MULTIPLAYER_MP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mp/*.cpp)
    list(FILTER MULTIPLAYER_MP_SOURCES EXCLUDE REGEX ".*mp_context\\.cpp$")
    list(APPEND CATACLYSM_DDA_SOURCES ${MULTIPLAYER_MP_SOURCES})
endif ()
```
В корневом `CMakeLists.txt` блок `if (MULTIPLAYER)` оставить только с `add_definitions(-DMULTIPLAYER)`, сломанный глоб `${CMAKE_CURRENT_SOURCE_DIR}/mp/*.cpp` удалить (он указывал на несуществующий `<root>/mp/` и молча не собирал ничего).

- [ ] **Step 8: Запустить тесты — зелёные**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: PASS. Также полный прогон обычной сборки зелёный (поведение не менялось — `mp_apply_viewport_params` пока не зовётся игрой).

- [ ] **Step 9: Коммит**

```bash
git add src/mp/mp_context.h src/mp/mp_context.cpp tests/mp_context_test.cpp \
        src/ui_manager.h src/ui_manager.cpp Makefile CMakeLists.txt
git commit -m "feat(mp): add the player context structures, viewport metrics, and active-context accessors"
```

---

### Task 2: `ui_manager` — активный стек и инвариант владения

**Files:**
- Modify: `src/ui_manager.h`, `src/ui_manager.cpp`
- Modify: `tests/mp_context_test.cpp` (тест стека)

**Interfaces:**
- Consumes: `ui_stack_t` (уже в `ui_manager.h`, задача 1).
- Produces:
  - `void ui_manager::set_active_stack( ui_stack_t *stack );` — единственный сеттер, `cata_assert(stack != nullptr)`.
  - `active_stack` никогда не `nullptr`; по умолчанию указывает на file-static `world_stack` (заглушка фазы мира для аварийных UI, реально включается в Ф3).
  - `ui_adaptor` при конструировании запоминает `ui_stack_t *owner = active_stack`; деструктор в дебаг-сборке делает `cata_assert( owner == active_stack )`.
  - `ui_adaptor::ui_stack_size()` возвращает размер активного стека.

- [ ] **Step 1: Создать `tests/game_test_helper.h` — общий хелпер слотов и RAII-восстановитель**

`game_test_helper` сегодня определён локально в `tests/hotseat_test.cpp:7` (друг `game`, `game.h:197`). Он нужен и `mp_context_test` (задача 5) — вынести в общий заголовок. Туда же — RAII-восстановитель активного состояния: тесты задач 1, 2, 7 ставят активный стек/контекст на стековые локали, и без восстановления следующий тест пишет в освобождённую память.

`tests/game_test_helper.h`:
```cpp
#pragma once

#include "game.h"
#include "mp/mp_context.h"
#include "ui_manager.h"

// Test helper to reset player slots between tests (friend of game).
class game_test_helper
{
    public:
        static void reset_player_slots()
        {
            // Тело — как сейчас в tests/hotseat_test.cpp:13-27.
            avatar &base = g->player_at( 0 );
            g->owned_extra_players.clear();
            g->player_slots.clear();
            g->player_slots.push_back( &base );
            g->active_slot_index = 0;
        }
};

// Restores the active context and the active ui stack on scope exit. Any test
// that points the globals at stack locals must own one of these.
class restore_active_state
{
        player_context *const ctx_;
        ui_stack_t *const stack_;
    public:
        restore_active_state() :
            ctx_( mp_active_context_ptr() ), stack_( ui_manager::get_active_stack() ) {}
        ~restore_active_state()
        {
            mp_set_active_context( ctx_ );
            ui_manager::set_active_stack( stack_ );
        }
};
```
`tests/hotseat_test.cpp`: удалить локальный `class game_test_helper`, добавить `#include "game_test_helper.h"`.

- [ ] **Step 2: Написать падающий тест — переключение активного стека**

Добавить в `tests/mp_context_test.cpp`:

```cpp
TEST_CASE( "ui_adaptors_register_into_the_active_stack", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t stack_a;
    ui_stack_t stack_b;

    ui_manager::set_active_stack( &stack_a );
    {
        ui_adaptor ui;
        CHECK( ui_adaptor::ui_stack_size() == 1 );
    }
    // Деструктор выполнился под тем же активным стеком — инвариант владения цел.
    CHECK( ui_adaptor::ui_stack_size() == 0 );

    ui_manager::set_active_stack( &stack_b );
    {
        ui_adaptor ui;
        CHECK( ui_adaptor::ui_stack_size() == 1 );
        CHECK( stack_a.size() == 0 );
        CHECK( stack_b.size() == 1 );
    }
    CHECK( ui_adaptor::ui_stack_size() == 0 );
    // restore_active_state в деструкторе вернёт предыдущий активный стек.
}
```

Заголовки: `#include "ui_manager.h"`, `#include "game_test_helper.h"`. Тест не трогает окна и работает в обычной сборке.

- [ ] **Step 3: Запустить — убедиться, что падает**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: FAIL — `set_active_stack` не объявлен.

- [ ] **Step 4: Реализовать в `src/ui_manager.cpp`**

Заменить (около строки 36):
```cpp
static ui_stack_t world_stack;
static ui_stack_t *active_stack = &world_stack;
```
(`static ui_stack_t ui_stack;` удалить.)

Добавить сеттер и геттер:
```cpp
void ui_manager::set_active_stack( ui_stack_t *stack )
{
    cata_assert( stack != nullptr );
    active_stack = stack;
}

ui_stack_t *ui_manager::get_active_stack()
{
    return active_stack;
}
```
Объявить в `ui_manager.h` в namespace `ui_manager` (рядом с `invalidate_all_ui_adaptors`):
```cpp
/**
 * Set the stack that new and existing `ui_adaptor` objects register into and
 * that the redraw paths iterate. Never null: the world-phase placeholder is
 * used while no player context is active. Called from mp_activate_player /
 * mp_activate_world (Ф3).
 */
void set_active_stack( ui_stack_t *stack );
/** Get the active stack. Never null. Used by tests to restore it on scope exit. */
ui_stack_t *get_active_stack();
```

**Тип `ui_stack_t` в `ui_manager.h`.** Алиас стоит до определения `ui_adaptor` в этом же файле, поэтому нужны форвард-декларация и заголовок `<vector>`. В `src/ui_manager.h`, перед `using ui_stack_t`:
```cpp
#include <vector>   // в верхней части, рядом с <functional>/<memory>

class ui_adaptor;   // форвард — сам класс определён ниже в этом файле
using ui_stack_t = std::vector<std::reference_wrapper<ui_adaptor>>;
```
(`<functional>` для `reference_wrapper` уже включён; добавить при необходимости `<functional>` тоже.)

Заменить **все** использования `ui_stack` на `*active_stack` (следующие строки — полный список; на момент написания плана ~40 обращений в `ui_manager.cpp`):
- `ui_stack.emplace_back( *this );` (строки 42, 49, 86) → `active_stack->emplace_back( *this );` и добавить `owner = active_stack;`
- деструктор (109-117): цикл по `active_stack->rbegin()`, `active_stack->erase(...)`.
- `:252` `return active_stack->size();`
- `:269-276, 305-315, 350, 362, 372-373, 380, 430-446, 453-457, 459, 475, 483-491, 548, 592, 599`: заменить `ui_stack` на `*active_stack`, `&ui_stack` на `active_stack`.

Проверить спец-случаи:
- строка ~442: `ui_stack_t *ui_stack_orig = &ui_stack;` → `ui_stack_t *ui_stack_orig = active_stack;`
- строка ~380: `if( test_mode || active_stack->empty() )`
- строка ~440: `std::unique_ptr<ui_stack_t> ui_stack_copy;` (локальная копия — имя не менять, это не глобал).

- [ ] **Step 5: Добавить `owner` и ассерт в `ui_adaptor`**

В `src/ui_manager.h`, в `private:` секцию `ui_adaptor`:
```cpp
// Стек, в который этот адаптор зарегистрировался при конструировании.
// Деструктор обязан видеть его активным (инвариант владения, §5.2 спека).
ui_stack_t *owner = nullptr;
```
В каждом из трёх конструкторов (`ui_adaptor()`, `ui_adaptor(disable_uis_below)`, `ui_adaptor(debug_message_ui)`) после `active_stack->emplace_back( *this );`:
```cpp
owner = active_stack;
```
В деструкторе, перед циклом удаления:
```cpp
cata_assert( owner == active_stack );
```

- [ ] **Step 6: Запустить тесты — зелёные + регресс**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: PASS. Затем полный прогон: `make check` (или `tests/cata_test` без фильтра) — все тесты зелёные. Нарушение ассерта где-либо — признак адаптора, живущего через переключение стека; в Ф2 такого быть не должно (активен один стек).

- [ ] **Step 7: Коммит**

```bash
git add src/ui_manager.h src/ui_manager.cpp tests/mp_context_test.cpp \
        tests/game_test_helper.h tests/hotseat_test.cpp
git commit -m "refactor(mp): make ui_manager operate on an active stack pointer with an ownership assert"
```

---

### Task 3: Окна и главный `ui_adaptor` — владение переезжает в контекст

**Files:**
- Modify: `src/game.h`, `src/game.cpp`, `src/overmap_ui.cpp`
- Modify: `src/mp/mp_context.h`, `src/mp/mp_context.cpp`
- Modify: `tests/mp_context_test.cpp` (проверка алиасов)

**Interfaces:**
- Consumes: `player_context` (задача 1), `mp_active_context()` (задача 1), `mp_activate_player` (задача 5 — ensure зовёт его, поэтому в задаче 3 ensure активирует вручную, а вызов добавляется в задаче 5).
- Produces:
  - `void mp_ensure_player_context();` — создаёт `game::contexts[0]`, заполняет `viewport` из `TERMX/TERMY` (базовые размеры пишет `game_ui::init_ui()`, game.cpp:514 — единственный их писатель), `stdscr = catacurses::stdscr`, вызывает `mp_apply_viewport_params`, ставит активным. **Всегда** выставляет активный контекст — даже если `contexts` уже был непуст (иначе после `contexts.clear()` активный указатель висит).
  - `void mp_resize_active_context();` — перечитывает `viewport` из `TERMX/TERMY`, применяет метрики, создаёт окна контекста, ре-пойнтит алиасы `game` на окна активного контекста, зовёт `position_from_window`.
  - `game::contexts` — `std::deque<player_context>`, публичный член. **Deque, а не `vector`:** элементы не переезжают при `emplace_back`, поэтому указатели `mp_set_active_context(&contexts[i])` и `set_active_stack(&contexts[i].ui_stack)` не инвалидируются в Ф3, когда появится второй контекст.
  - `mp_ensure_player_context()` вызывается из `game::setup()` (game.cpp:678) — общий хук новой игры и загрузки (`main_menu.cpp:991` и `game_io.cpp:315`), поэтому загрузка сейва (`savegame.cpp:318`) читает вью-стейт через уже выставленный активный контекст, без ассерта.
  - `invalidate_main_ui_adaptor()`/`mark_main_ui_adaptor_resize()` — широковещательные по `contexts`.

- [ ] **Step 1: Изменить `src/game.h`**

- Добавить `#include "mp/mp_context.h"` (рядом с прочими includes).
- Удалить член `main_ui_adaptor` (строка ~1380).
- В **публичную** секцию с методами слотов (рядом с `player_slot_count`, строка ~441; `player_slots` на 1220 — приватный DATA-блок) добавить:
```cpp
// Player contexts, parallel to player_slots. Deque keeps element addresses
// stable across emplace_back, so active-context pointers never dangle. Ф2:
// size is always one; a second context arrives with the scheduler in Ф3.
std::deque<player_context> contexts; // NOLINT(cata-serialize)
```

- [ ] **Step 2: Реализовать `mp_ensure_player_context` и `mp_resize_active_context` в `src/mp/mp_context.cpp`**

```cpp
void mp_ensure_player_context()
{
    if( g->contexts.empty() ) {
        g->contexts.emplace_back();
    }
    player_context &ctx = g->contexts[0];
    // TERMX/TERMY установлены game_ui::init_ui() до любых колбэков; контекст
    // зеркалит их (источник — init_ui, не get_terminal_width, который в TUI
    // читает опцию TERMINAL_X, а не реальный размер терминала).
    ctx.viewport.term_cols = TERMX;
    ctx.viewport.term_rows = TERMY;
    mp_apply_viewport_params( ctx );
    ctx.stdscr = catacurses::stdscr;
    mp_set_active_context( &ctx );
    // В задаче 5 сюда добавится mp_activate_player( 0 ) — см. задачу 5, шаг 6.
}

void mp_resize_active_context()
{
    player_context &ctx = mp_active_context();

    ctx.viewport.term_cols = TERMX;
    ctx.viewport.term_rows = TERMY;
    mp_apply_viewport_params( ctx );

    const int sidebar_left = panel_manager::get_manager().get_width_left();
    const int sidebar_right = panel_manager::get_manager().get_width_right();

    ctx.w_terrain = ctx.w_terrain_ptr = catacurses::newwin(
        TERRAIN_WINDOW_HEIGHT, TERRAIN_WINDOW_WIDTH, point( sidebar_left, 0 ) );
    ctx.w_minimap = ctx.w_minimap_ptr = catacurses::newwin(
        MINIMAP_HEIGHT, MINIMAP_WIDTH, point::zero );
    ctx.w_pixel_minimap = catacurses::newwin( 1, 1, point::zero );

    // Ре-пойнт алиасов game на окна активного контекста.
    catacurses::stdscr = ctx.stdscr;
    g->w_terrain = ctx.w_terrain;
    g->w_overmap = ctx.w_overmap;
    g->w_omlegend = ctx.w_omlegend;
    g->w_minimap = ctx.w_minimap;
    g->w_pixel_minimap = ctx.w_pixel_minimap;
    g->w_terrain_ptr = ctx.w_terrain_ptr;
    g->w_minimap_ptr = ctx.w_minimap_ptr;

    if( const shared_ptr_fast<ui_adaptor> ui = ctx.main_ui_adaptor.lock() ) {
        ui->position_from_window( ctx.stdscr );
    }
}
```
Заголовки: `catacurses::newwin`, `stdscr` — `cursesdef.h`; `MINIMAP_WIDTH/HEIGHT` — `game_constants.h` (добавить include); `point` — `coordinate_point.h`/`enums.h`; `TERMX/TERMY` — `output.h`.

- [ ] **Step 3: Добавить `mp_ensure_player_context()` в `game::setup()`**

В начало `game::setup()` (game.cpp:678), до MULTIPLAYER-сброса слотов (703-715):
```cpp
void game::setup()
{
    // Контекст должен существовать до загрузки сейва: savegame.cpp:318 читает
    // вью-стейт через активный контекст, а setup() вызывается из main_menu.cpp:991
    // (новая игра) и game_io.cpp:315 (загрузка) — оба до create_or_get_main_ui_adaptor.
    mp_ensure_player_context();
    ...
}
```

- [ ] **Step 4: Переписать `create_or_get_main_ui_adaptor` в `src/game.cpp` (3173-3234)**

```cpp
shared_ptr_fast<ui_adaptor> game::create_or_get_main_ui_adaptor()
{
    mp_ensure_player_context();
    player_context &ctx = mp_active_context();
    shared_ptr_fast<ui_adaptor> ui = ctx.main_ui_adaptor.lock();
    if( !ui ) {
        ctx.main_ui_adaptor = ui = make_shared_fast<ui_adaptor>();
        ui->on_redraw( []( ui_adaptor &u ) {
            g->draw( u );
        } );
        ui->on_screen_resize( []( ui_adaptor & ) {
            mp_resize_active_context();
        } );
        ui->mark_resize();
    }
    return ui;
}
```
Тело старого resize-колбэка (строки 3181-3230) удаляется — оно переехало в `mp_resize_active_context` (шаг 2). Строки создания `w_terrain`/`w_minimap`/`w_pixel_minimap` больше не в `game.cpp`.

- [ ] **Step 5: Сделать `invalidate_main_ui_adaptor`/`mark_main_ui_adaptor_resize` широковещательными (game.cpp:3236-3250)**

```cpp
void game::invalidate_main_ui_adaptor() const
{
    for( const player_context &ctx : contexts ) {
        if( const shared_ptr_fast<ui_adaptor> ui = ctx.main_ui_adaptor.lock() ) {
            ui->invalidate_ui();
        }
    }
}

void game::mark_main_ui_adaptor_resize() const
{
    for( const player_context &ctx : contexts ) {
        if( const shared_ptr_fast<ui_adaptor> ui = ctx.main_ui_adaptor.lock() ) {
            ui->mark_resize();
        }
    }
}
```

- [ ] **Step 6: Окна, создаваемые вне главного resize-колбэка, писать в контекст**

`src/overmap_ui.cpp:2104`:
```cpp
// Было: g->w_overmap = catacurses::newwin( OVERMAP_WINDOW_HEIGHT, OVERMAP_WINDOW_WIDTH, point::zero );
g->w_overmap = mp_active_context().w_overmap = catacurses::newwin(
    OVERMAP_WINDOW_HEIGHT, OVERMAP_WINDOW_WIDTH, point::zero );
```
Добавить `#include "mp/mp_context.h"` в `overmap_ui.cpp`.

`src/game.cpp` `draw_pixel_minimap` (около 3500) — внутри метода `game`, поэтому писать в оба места через член (не `g->`):
```cpp
void game::draw_pixel_minimap( const catacurses::window &w )
{
    w_pixel_minimap = mp_active_context().w_pixel_minimap = w;
}
```
(`game.cpp` уже включает `mp/mp_context.h` через `game.h`.)

**Порядок resize-до-активации.** `mp_activate_player` (задача 5) безусловно ре-пойнтит `g->w_*` из контекста. Гарантия порядка: создание окон (`mp_resize_active_context`, первый redraw) предшествует любой активации в игровом пути (`mp_ensure_player_context` в `setup()`, hotseat-цикл уже после первого resize). В тестах, где resize не происходил, окна контекста пусты — и пусты алиасы, базовая линия совпадает.

- [ ] **Step 7: Проверка алиасов в тесте**

Добавить в `tests/mp_context_test.cpp`:
```cpp
TEST_CASE( "ensure_and_resize_repopulate_the_active_context", "[mp]" )
{
    mp_ensure_player_context();
    player_context &ctx = mp_active_context();
    CHECK( ctx.viewport.term_cols == TERMX );
    CHECK( ctx.viewport.term_rows == TERMY );

    mp_resize_active_context();
    // Алиасы game указывают на окна активного контекста.
    CHECK( g->w_terrain == ctx.w_terrain );
    CHECK( g->w_terrain_ptr == ctx.w_terrain_ptr );
    CHECK( g->w_minimap == ctx.w_minimap );
    CHECK( g->w_pixel_minimap == ctx.w_pixel_minimap );
    CHECK( catacurses::stdscr == ctx.stdscr );
}
```
**Важно:** этот тест вызывает `mp_resize_active_context` → `catacurses::newwin`. В обычной сборке (ncurses) headless-окна не создаются. Поэтому **этот тест-кейс гейтить** `#if defined(MP_SERVER)` (он переедет в `mp_context_swap_test` в задаче 7 — там же и останется жить; здесь не оставлять). На момент задачи 3 в обычной сборке проверяется только `mp_ensure_player_context` + метрики; создание окон проверяется серверной сборкой и вручную.

Уточнение для шага: в обычной сборке тест-кейс `ensure_and_resize_repopulate_the_active_context` заменить на проверку только `mp_ensure_player_context` (без `newwin`):
```cpp
TEST_CASE( "ensure_creates_the_single_context_and_activates_it", "[mp]" )
{
    mp_ensure_player_context();
    CHECK( g->contexts.size() == 1 );
    CHECK( &mp_active_context() == &g->contexts[0] );
    CHECK( g->contexts[0].viewport.term_cols == TERMX );
}
```
Оконная часть уходит в `mp_context_swap_test` (задача 7).

- [ ] **Step 8: Собрать и прогнать регресс**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-test/`
Expected: весь тестовый набор обычной сборки зелёный. Локальный запуск игры (`./cataclysm-tiles` или curses-сборка) — старт партии, перемещение, инвентарь не падают.

- [ ] **Step 9: Коммит**

```bash
git add src/game.h src/game.cpp src/overmap_ui.cpp src/mp/mp_context.h src/mp/mp_context.cpp tests/mp_context_test.cpp
git commit -m "refactor(mp): move window ownership and the main ui_adaptor into the player context"
```

---

### Task 4: Вью-стейт — миграция сайтов на активный контекст

**Files:**
- Modify: `src/game.h`
- Modify: `src/action.cpp`, `src/animation.cpp`, `src/cata_tiles.cpp`, `src/construction.cpp`, `src/do_turn.cpp`, `src/editmap.cpp`, `src/game.cpp`, `src/handle_action.cpp`, `src/ranged.cpp`, `src/savegame.cpp`, `src/sdltiles.cpp`

**Interfaces:**
- Consumes: `mp_active_context()` (задача 1), `player_context::ter_view_p`/`driving_view_offset`.
- Produces: в `game` больше нет членов `ter_view_p`/`driving_view_offset`; все чтения/записи идут через `mp_active_context()`.

- [ ] **Step 1: Удалить члены из `src/game.h`**

Удалить `tripoint_bub_ms ter_view_p;` (строка ~1272) и `point_rel_ms driving_view_offset;` (строка ~1286).

- [ ] **Step 2: Пройти по всем сайтам и заменить на `mp_active_context()`**

Полный список (по состоянию на момент написания плана; каждый сайт — механическая правка):

`ter_view_p`:
| Файл:строка | Было | Стало |
|---|---|---|
| `action.cpp:1232` | `g->ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `animation.cpp:153` | `g.ter_view_p` | `mp_active_context().ter_view_p` |
| `cata_tiles.cpp:1533` | `g->ter_view_p.xy()` | `mp_active_context().ter_view_p.xy()` |
| `construction.cpp:1396` | `g->ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `editmap.cpp:255` | `g->ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `game.cpp:2470` | `ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `game.cpp:3388` | `ter_view_p.z()` | `mp_active_context().ter_view_p.z()` |
| `game.cpp:3389` | `here.build_map_cache( ter_view_p.z() )` | `here.build_map_cache( mp_active_context().ter_view_p.z() )` |
| `game.cpp:3390` | `here.update_visibility_cache( ter_view_p.z() )` | `here.update_visibility_cache( mp_active_context().ter_view_p.z() )` |
| `game.cpp:3570` | `ter_view_p = center;` | `mp_active_context().ter_view_p = center;` |
| `game.cpp:6445` | `ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `handle_action.cpp:3403` | `ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |
| `sdltiles.cpp:3671` | `g->ter_view_p` | `mp_active_context().ter_view_p` |
| `sdltiles.cpp:3782` | `g->ter_view_p.z()` | `mp_active_context().ter_view_p.z()` |
| `ranged.cpp:3237` | `g->ter_view_p.raw().xy()` | `mp_active_context().ter_view_p.raw().xy()` |

`driving_view_offset`:
| Файл:строка | Было | Стало |
|---|---|---|
| `do_turn.cpp:211` | `g->set_driving_view_offset( point_rel_ms::zero )` | без изменений (сеттер ниже) |
| `do_turn.cpp:706` | `driving_view_offset.x() != 0` | `mp_active_context().driving_view_offset.x() != 0` |
| `game.cpp:1383` | `set_driving_view_offset( point_rel_ms::zero )` | без изменений |
| `game.cpp:1459` | `-driving_view_offset + point(...)` | `-mp_active_context().driving_view_offset + point(...)` |
| `game.cpp:1466` | `( driving_view_offset.x() + ... )` | `( mp_active_context().driving_view_offset.x() + ... )` |
| `game.cpp:1468` | `( driving_view_offset.y() + ... )` | `( mp_active_context().driving_view_offset.y() + ... )` |
| `game.cpp:1470` | `set_driving_view_offset( {...} )` | без изменений |
| `game.cpp:1473-1481` (сеттер) | `driving_view_offset` (3 раза) | `mp_active_context().driving_view_offset` |
| `handle_action.cpp:2183-2184` | `g->driving_view_offset.x()/y()` | `mp_active_context().driving_view_offset.x()/y()` |
| `savegame.cpp:138` | `json.member( "driving_view_offset", driving_view_offset )` | `json.member( "driving_view_offset", mp_active_context().driving_view_offset )` |
| `savegame.cpp:318` | `data.read( "driving_view_offset", driving_view_offset )` | `data.read( "driving_view_offset", mp_active_context().driving_view_offset )` |

В каждом из этих файлов добавить `#include "mp/mp_context.h"`, если его ещё нет (для `game.cpp` он есть через `game.h`; для `action.cpp`, `animation.cpp`, `cata_tiles.cpp`, `construction.cpp`, `do_turn.cpp`, `editmap.cpp`, `handle_action.cpp`, `ranged.cpp`, `savegame.cpp`, `sdltiles.cpp` — добавить).

`set_driving_view_offset` (game.cpp:1473) — оставить методом `game`, но тело читает/пишет активный контекст:
```cpp
void game::set_driving_view_offset( const point_rel_ms &p )
{
    player_context &ctx = mp_active_context();
    avatar &you = get_avatar();
    you.view_offset.raw() -= ctx.driving_view_offset.raw();
    ctx.driving_view_offset = p;
    you.view_offset.raw() += ctx.driving_view_offset.raw();
}
```

- [ ] **Step 3: Собрать и прогнать регресс**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-test/`
Expected: все тесты зелёные. Отсутствие `mp_active_context()` до `mp_ensure_player_context()` невозможно в игровом пути (обеспечение вызывается из `create_or_get_main_ui_adaptor` до любого отрисовочного цикла); в тестах, которые читают вью-стейт, контекст выставляется явно.

- [ ] **Step 4: Коммит**

```bash
git add src/game.h src/action.cpp src/animation.cpp src/cata_tiles.cpp \
        src/construction.cpp src/do_turn.cpp src/editmap.cpp src/game.cpp \
        src/handle_action.cpp src/ranged.cpp src/savegame.cpp src/sdltiles.cpp
git commit -m "refactor(mp): read view state through the active player context"
```

---

### Task 5: `mp_activate_player` — точка свопа, hotseat на новом пути

**Files:**
- Modify: `src/game.h`, `src/game.cpp` (только если сеттер менялся в задаче 4 — уже сделан)
- Modify: `src/mp/mp_context.h`, `src/mp/mp_context.cpp`
- Modify: `src/mp/mp_hotseat_loop.cpp`
- Modify: `tests/hotseat_test.cpp`, `tests/mp_context_test.cpp`

**Interfaces:**
- Consumes: `game::swap_slot_into_u` (станет private, вызывается из `mp_activate_player` по дружбе), `ui_manager::set_active_stack` (задача 2), `mp_apply_viewport_params` (задача 1).
- Produces:
  - `void mp_activate_player( size_t idx );` — единственный писатель глобальных алиасов:
    1. своп аватара: `if( current != 0 ) swap_slot_into_u( current ); if( idx != 0 ) swap_slot_into_u( idx );` затем `set_active_slot( idx )`;
    2. `catacurses::stdscr = ctx->stdscr`;
    3. ре-пойнт 7 окон;
    4. `mp_apply_viewport_params( *ctx )`;
    5. `ui_manager::set_active_stack( &ctx->ui_stack )`;
    6. `mp_set_active_context( ctx )`.
  - Инвариант `player_slots[active] == &u`, проверяется `cata_assert` в дебаг-сборке.
  - `game::swap_slot_into_u` → private; `friend void mp_activate_player( size_t idx );` в `game`.

- [ ] **Step 1: Изменить `src/game.h` — приватный `swap_slot_into_u` + дружба**

- Перенести `swap_slot_into_u` в приватную секцию (добавить в private-блок возле `w_terrain_ptr`).
- В приватную секцию добавить дружбу:
```cpp
friend void mp_activate_player( size_t idx );
```
(Объявление функции уже видно через `#include "mp/mp_context.h"`.)

- [ ] **Step 2: Написать падающий тест — A→B→A на слотах**

В `tests/mp_context_test.cpp` (слоты приватные у `game` — сброс через общего хелпера, `#include "game_test_helper.h"`):
```cpp
TEST_CASE( "activating_slots_in_sequence_preserves_each_slots_content", "[mp]" )
{
    restore_active_state restore;
    game_test_helper::reset_player_slots();
    // Не очищать g->contexts: активный указатель указывал бы на освобождённый
    // элемент, и restore_active_state вернул бы его же. ensure переиспользует
    // существующий contexts[0] или создаёт новый и в любом случае ставит активным.
    mp_ensure_player_context();

    g->player_at( 0 ).name = "SlotZero";
    avatar &second = g->add_player_slot();
    second.name = "SlotOne";
    avatar &third = g->add_player_slot();
    third.name = "SlotTwo";

    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOne" );
    get_avatar().name = "SlotOneEdited";

    mp_activate_player( 2 );
    CHECK( get_avatar().name == "SlotTwo" );

    // Возврат в слот 1: контент сохранён (непарный swap сломал бы это).
    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOneEdited" );

    mp_activate_player( 0 );
    CHECK( get_avatar().name == "SlotZero" );
}
```
(`avatar` — `src/avatar.h`, `game.h` уже есть.)

- [ ] **Step 3: Написать падающий тест — `mp_activate_player(0)` тождественен**

Спека §7.1 п.1: своп на слоте 0 ничего не меняет — `stdscr`, `u`, окна, метрики. В обычной тест-сборке окна не созданы (ncurses headless), поэтому проверка честно фиксирует «не изменилось» по значениям алиасов и метрик:

```cpp
TEST_CASE( "mp_activate_player_zero_is_the_identity_swap", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    const catacurses::window stdscr_before = catacurses::stdscr;
    const catacurses::window terrain_before = g->w_terrain;
    const int termx_before = TERMX;
    const int termy_before = TERMY;

    mp_activate_player( 0 );

    CHECK( catacurses::stdscr == stdscr_before );
    CHECK( g->w_terrain == terrain_before );
    CHECK( TERMX == termx_before );
    CHECK( TERMY == termy_before );
    CHECK( &get_avatar() == &g->player_at( 0 ) );
}
```
(В серверной сборке ту же тождественность по живым окнам покрывает `mp_context_swap_test`, задача 7.)

- [ ] **Step 4: Запустить — убедиться, что падает**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: FAIL — `mp_activate_player` не объявлен.

- [ ] **Step 5: Реализовать `mp_activate_player` в `src/mp/mp_context.cpp`**

```cpp
void mp_activate_player( size_t idx )
{
    // Без ensure: mp_ensure_player_context() сам вызывает mp_activate_player(0)
    // (шаг 6) — иначе бесконечная рекурсия. Вызывающие обязаны сначала вызвать
    // ensure (setup()/create_or_get_main_ui_adaptor/тесты).
    cata_assert( !g->contexts.empty() );
    // В Ф2 контекст один (hotseat делит его между слотами); per-player
    // контексты (contexts[idx]) приходят с планировщиком в Ф3.
    player_context &ctx = g->contexts[0];

    // 1. Аватар: снять текущий, поставить новый. swap_slot_into_u self-inverse,
    // корректна только парой вызовов — поэтому она приватна и зовётся здесь.
    const size_t current = g->active_slot();
    if( current != 0 ) {
        g->swap_slot_into_u( current );
    }
    if( idx != 0 ) {
        g->swap_slot_into_u( idx );
    }
    g->set_active_slot( idx );
    cata_assert( g->player_slots[g->active_slot()] == &g->u );

    // 2. Экран.
    catacurses::stdscr = ctx.stdscr;
    // 3. Окна.
    g->w_terrain = ctx.w_terrain;
    g->w_overmap = ctx.w_overmap;
    g->w_omlegend = ctx.w_omlegend;
    g->w_minimap = ctx.w_minimap;
    g->w_pixel_minimap = ctx.w_pixel_minimap;
    g->w_terrain_ptr = ctx.w_terrain_ptr;
    g->w_minimap_ptr = ctx.w_minimap_ptr;
    // 4. Метрики.
    mp_apply_viewport_params( ctx );
    // 5. Стек UI.
    ui_manager::set_active_stack( &ctx.ui_stack );
    // 6. Вью-стейт и ввод.
    mp_set_active_context( &ctx );
}
```

- [ ] **Step 6: Вызвать `mp_activate_player(0)` из конца `mp_ensure_player_context`**

Спека §6.1: ensure создаёт контекст **и** активирует игрока 0. Без этого в одиночной игре активный стек остаётся `world_stack` (главный адаптор регистрируется не туда) и механизм контекста не прогоняется обычной сборкой. В конец `mp_ensure_player_context()` (mp_context.cpp, задача 3 шаг 2) добавить:
```cpp
    mp_set_active_context( &ctx );
    mp_activate_player( 0 );
```
Порядок безопасен: активация ре-пойнтит алиасы на (пока пустые) окна контекста, а первый redraw тут же запускает `mp_resize_active_context`, который создаёт окна и ре-пойнтит заново — см. «Порядок resize-до-активации» в задаче 3.

- [ ] **Step 7: Перевести hotseat-цикл на `mp_activate_player`**

`src/mp/mp_hotseat_loop.cpp`, строки 24-78. Заменить:
```cpp
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->set_active_slot( i );
        g->swap_slot_into_u( i );              // ← удалить (было :34)
        avatar &who = g->player_at( i );
        ...
        g->swap_slot_into_u( i );              // ← удалить (было :75)
    }
    g->set_active_slot( 0 );                   // ← удалить (было :77)
```
на:
```cpp
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        mp_activate_player( i );
        avatar &who = g->player_at( i );
        ...
        // нет: mp_activate_player следующей итерации снимет этот слот
    }
    mp_activate_player( 0 );
```
Тело цикла между активацией и концом (drain activity, `handle_action`, тезер) не меняется. Обновить комментарий перед циклом (он ссылается на `swap_slot_into_u` — переписать на `mp_activate_player`). Добавить `#include "mp/mp_context.h"`.

- [ ] **Step 8: Обновить `tests/hotseat_test.cpp`**

Строка ~184: `g->set_active_slot( 1 ); g->swap_slot_into_u( 1 );` → `mp_ensure_player_context(); mp_activate_player( 1 );` (комментарий про «состояние середины хода» оставить, дополнить, что активация — через точку свопа; ensure обязателен — `mp_activate_player` ассертит непустой `contexts`). Добавить `#include "mp/mp_context.h"` (`game_test_helper.h` уже подключён с задачи 2).

- [ ] **Step 9: Прогнать тесты**

Run (обычная): `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-test/`
Run (multiplatform/hotseat): `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1 && tests/cata_test "[hotseat],[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: обычная сборка зелёная; `[hotseat]` и `[mp]` зелёные (A→B→A, hotseat-цикл через новую точку).

- [ ] **Step 10: Коммит**

```bash
git add src/game.h src/mp/mp_context.h src/mp/mp_context.cpp \
        src/mp/mp_hotseat_loop.cpp tests/hotseat_test.cpp tests/mp_context_test.cpp
git commit -m "feat(mp): mp_activate_player as the single alias-swap point; hotseat uses it"
```

---

### Task 6: Ввод сервера через активный контекст + серверные гварды

**Files:**
- Modify: `src/mp/mp_server_input.cpp`, `src/mp/mp_curses_server.cpp`
- Modify: `src/game.cpp` (запись `TERMINAL_X/Y`), `src/handle_action.cpp` (`show_adm`)
- Modify: `tests/mp_server_io_test.cpp`

**Interfaces:**
- Consumes: `mp_active_context()` (задача 1), `player_context::input_queue`/`input_timeout_ms`.
- Produces: серверный бэкенд читает/пишет очередь и таймаут активного контекста; `set_timeout` в серверной сборке пишет в активный контекст; запись `TERMINAL_X/Y` закрыта в `MP_SERVER`; `show_adm` недоступен в `MP_SERVER`.

- [ ] **Step 1: Очередь ввода сервера — через активный контекст**

В `src/mp/mp_server_input.cpp`:
- Удалить `std::deque<input_event> g_input_queue;` (строка ~29).
- В `dispatch()` (58-89) при `input` (:62): `mp_active_context().input_queue.push_back( msg.event );`
- **`take_queued()` (:111-117)** — реальный читатель очереди (его зовут `wait_for_input` :218 и `pump` :254). Вместо `g_input_queue.empty()/front()/pop_front()`:
```cpp
bool take_queued( input_event &out )
{
    std::deque<input_event> &q = mp_active_context().input_queue;
    if( q.empty() ) {
        return false;
    }
    out = std::move( q.front() );
    q.pop_front();
    return true;
}
```
- `mp_server_wait_for_input` (210-268) и `mp_server_pump` (270-279) — без изменений сверх этого: они уже идут через `take_queued`.
- **`mp_server_input_reset_for_test()` (:204-207)** — раньше чистил `g_input_queue`; теперь обязан сбросить очередь активного контекста, иначе сброс между тестами молча перестаёт работать:
```cpp
void mp_server_input_reset_for_test()
{
    mp_active_context().input_queue.clear();
    ...
}
```

Добавить `#include "mp/mp_context.h"`. В начале `mp_server_wait_for_input`/`pump` активный контекст уже выставлен (сервер вызывает `mp_ensure_player_context` при старте партии через `create_or_get_main_ui_adaptor`; `mp_server_input_reset_for_test` в тестах — после выставления контекста).

- [ ] **Step 2: Таймаут сервера — через активный контекст**

В `src/mp/mp_curses_server.cpp`:
- `get_input_event` (135-170): `mp_server_wait_for_input( mp_active_context().input_timeout_ms )` вместо локального `input_timeout_ms`.
- Удалить локальный `int input_timeout_ms = -1;` (строка ~36) и его использования в `set_timeout`/`get_input_event`.
- `set_timeout` (177-182): `mp_active_context().input_timeout_ms = delay;` + `input_timeout = delay;` (общий keymap-таймаут `input_manager::input_timeout` оставить).

Добавить `#include "mp/mp_context.h"`.

- [ ] **Step 3: Обновить `tests/mp_server_io_test.cpp`**

Ввод теперь живёт в активном контексте, поэтому и `mp_server_wait_for_input`/`mp_server_pump`, и **`mp_server_reset_for_test()`** (зовётся из `mp_server_screen::mp_server_reset_for_test`, mp_server_screen.cpp:104-114, а та чистит `mp_active_context().input_queue`) требуют выставленного контекста. Хелпер в начале файла:

```cpp
#include "mp/mp_context.h"

// Контекст ввода тестов — статический (не стековая локаль), чтобы активный
// указатель не висел между тестами.
static player_context test_input_ctx;

// В каждом тесте (или в фикстуре), до любых вызовов ввода/сброса:
mp_set_active_context( &test_input_ctx );
test_input_ctx.input_queue.clear();
```
Первым делом в тесте, который не выставлял контекст сам, вызвать `mp_set_active_context( &test_input_ctx )` — иначе `mp_server_input_reset_for_test` упадёт на ассерте `mp_active_context()`.

- [ ] **Step 4: Закрыть запись `TERMINAL_X/Y` в серверной сборке**

`src/game.cpp:545-552`. Текущий guard `#if !defined(TUI)` в серверной сборке (где `TUI` не определён) выполняет ветку с записью опций. Заменить:
```cpp
#if !defined(TUI)
    TERMX = get_terminal_width();
    TERMY = get_terminal_height();

    get_options().get_option( "TERMINAL_X" ).setValue( TERMX * get_scaling_factor() );
    get_options().get_option( "TERMINAL_Y" ).setValue( TERMY * get_scaling_factor() );
    get_options().save();
#else
```
на три ветки (простой `#if !defined(TUI) && !defined(MP_SERVER)` увёл бы серверную сборку в `#else`, а тот зовёт `ensure_term_size()`, определённую только под `#if !(defined(TILES) || defined(MP_SERVER))` — линк-ошибка):
```cpp
#if defined(MP_SERVER)
    // Размер терминала — размер клиента (композитный буфер сервера).
    // Опции не трогаем: запись TERMINAL_X/Y затирала бы конфиг сервера (§3.1).
    // ensure_term_size() не вызываем — она не скомпилирована в серверной сборке.
    TERMX = get_terminal_width();
    TERMY = get_terminal_height();
#elif !defined(TUI)
    TERMX = get_terminal_width();
    TERMY = get_terminal_height();

    get_options().get_option( "TERMINAL_X" ).setValue( TERMX * get_scaling_factor() );
    get_options().get_option( "TERMINAL_Y" ).setValue( TERMY * get_scaling_factor() );
    get_options().save();
#else
    TERMY = getmaxy( catacurses::stdscr );
    TERMX = getmaxx( catacurses::stdscr );
    ensure_term_size();
    // FULL_SCREEN_HEIGHT-паритет (game.cpp:559-563) остаётся на месте.
#endif
```
В серверной сборке `get_terminal_width()` — это `mp_server_grid()` (mp_curses_server.cpp:184), то есть размер клиента. Ветка `#else` (TUI) не затронута — паритет `FULL_SCREEN_HEIGHT` сохранён.

- [ ] **Step 5: Закрыть `show_adm` в серверной сборке**

`src/handle_action.cpp`, case `ACTION_TOGGLE_PANEL_ADM` (3156-3158):
```cpp
        case ACTION_TOGGLE_PANEL_ADM:
#if defined(MP_SERVER)
            // На общем менеджере сайдбара настройка двигает вьюпорт всем
            // посреди чужого хода — в серверной сборке недоступна (§6.5 спека).
            add_msg( m_info, _( "Sidebar layout is shared on the server." ) );
#else
            panel_manager::get_manager().show_adm();
#endif
            break;
```
(`add_msg`, `_` — уже доступны в `handle_action.cpp`.)

- [ ] **Step 6: Собрать и прогнать серверную сборку**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: серверные тесты зелёные. Вручную: сервер + клиент на 127.0.0.1, старт партии, движение — ввод и экран работают.

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_server_input.cpp src/mp/mp_curses_server.cpp \
        src/game.cpp src/handle_action.cpp tests/mp_server_io_test.cpp
git commit -m "refactor(mp): server input reads the active context; close TERMINAL_X/Y and sidebar admin on the server"
```

---

### Task 7: `mp_context_swap_test` — самосравненец по содержимому ячеек (MP_SERVER)

**Files:**
- Create: `tests/mp_context_swap_test.cpp`

**Interfaces:**
- Consumes: `player_context`, `mp_activate_player`, `mp_resize_active_context`, `ui_manager::set_active_stack`, инвариант владения (задачи 1–5).
- Produces: главный ворота Ф2 — доказывает, что возврат в контекст идеально восстанавливает экран, и ловит протёкший глобал.

- [ ] **Step 1: Написать тест**

```cpp
#if defined(MP_SERVER)

#include "cata_catch.h"

#include "cached_options.h"      // test_mode
#include "cata_scope_helpers.h"  // restore_on_out_of_scope
#include "cursesport.h"          // cata_cursesport::WINDOW
#include "game_test_helper.h"    // restore_active_state
#include "mp/mp_context.h"
#include "ui_manager.h"

namespace {

// Возвращает содержимое окна как последовательность глифов построчно.
std::string dump_cells( const catacurses::window &win )
{
    cata_cursesport::WINDOW *w = win.get<cata_cursesport::WINDOW>();
    REQUIRE( w != nullptr );
    std::string out;
    for( const cata_cursesport::curseline &line : w->line ) {
        for( const cata_cursesport::cursecell &cell : line.chars ) {
            out += cell.ch;
        }
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE( "context_swap_restores_each_screens_cells", "[mp]" )
{
    // Тест ставит активный контекст/стек на стековые локали a/b — RAII
    // вернёт предыдущее состояние, иначе следующий тест пишет в освобождённую память.
    restore_active_state restore;

    // Два контекста с собственными корневыми окнами.
    player_context a;
    player_context b;
    const int cols = 30;
    const int rows = 10;
    a.stdscr = catacurses::newwin( rows, cols, point::zero );
    b.stdscr = catacurses::newwin( rows, cols, point::zero );

    // Сценарии: скриптовые redraw-колбэки рисуют в активный stdscr.
    const std::string scenario_a = "SCENARIO_A";
    const std::string scenario_b = "SCENARIO_B";
    const auto paint = []( const std::string &text ) {
        werase( catacurses::stdscr );
        mvwprintw( catacurses::stdscr, point( 1, 2 ), text );
        wnoutrefresh( catacurses::stdscr );
    };

    // Активировать A, нарисовать A, снять содержимое.
    mp_set_active_context( &a );
    ui_manager::set_active_stack( &a.ui_stack );
    catacurses::stdscr = a.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_a ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_a = dump_cells( a.stdscr );

    // Активировать B, нарисовать B, снять.
    mp_set_active_context( &b );
    ui_manager::set_active_stack( &b.ui_stack );
    catacurses::stdscr = b.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_b ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_b = dump_cells( b.stdscr );
    // Сценарий B реально попал в окно B.
    CHECK( cells_b.find( scenario_b ) != std::string::npos );
    // Окно A не загрязнено сценарием B.
    CHECK( cells_a.find( scenario_b ) == std::string::npos );

    // Вернуться в A, повторить сценарий A, снять A'.
    mp_set_active_context( &a );
    ui_manager::set_active_stack( &a.ui_stack );
    catacurses::stdscr = a.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_a ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_a2 = dump_cells( a.stdscr );

    // Ключевой ассерт: A == A' по содержимому ячеек.
    CHECK( cells_a == cells_a2 );
}
#endif // defined(MP_SERVER)
```

Пояснения к механике:
- In-memory curses (`cursesport`) доступен только в `MP_SERVER`-сборке — отсюда гейт (см. «Отступление» в Global Constraints).
- `test_mode` в `cached_options.h`; `ui_manager::redraw_invalidated()` в `test_mode` рано выходит — поэтому временно `test_mode = false` через `restore_on_out_of_scope`.
- Адапторы конструируются под активным стеком и умирают под ним же — инвариант владения (§5.2) проверяется ассертом.
- Протёкший глобал (общий `stdscr`, не тот стек, окно вне контекста) меняет `cells_a`/`cells_a2` и ловит тест сразу.

- [ ] **Step 2: Запустить — убедиться, что проходит**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: PASS (если тест упал на ассерте владения или несовпадении ячеек — чинить механизм, а не тест).

- [ ] **Step 3: Коммит**

```bash
git add tests/mp_context_swap_test.cpp
git commit -m "test(mp): self-compare proves a context switch restores the screen cells exactly"
```

---

### Task 8: Полный регресс и ворота Ф2

**Files:** нет (верификация).

- [ ] **Step 1: Обычная сборка — все тесты**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-test/`
Expected: весь тестовый набор обычной сборки зелёный (включая `mp_context_test`).

- [ ] **Step 2: Серверная сборка — mp-тесты**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`
Expected: зелёные (включая `mp_context_swap_test`, серверные mp-тесты, hotseat).

- [ ] **Step 3: Живой сервер — smoke**

Поднять `cataclysm-server`, подключиться клиентом `--connect 127.0.0.1:PORT`, создать персонажа, пройти несколько ходов, открыть инвентарь/крафт. Убедиться, что экран и ввод работают (регресс задач 3, 4, 6 на реальном пути).

- [ ] **Step 4: CI-прогон**

Проверить, что `mp-server`-джоба (matrix.yml) гоняет `mp_context_swap_test` (тег `[mp]` покрывает). Если требуется, запустить локально те же команды, что в CI.

- [ ] **Step 5: Коммит (если есть хвосты)**

```bash
git add -A
git commit -m "docs(mp): phase 2 context — implementation complete"
```

---

## Самопроверка плана

**Покрытие спека.**

- §2 (решения): ре-пойнт + чтение через активный контекст — задачи 1, 3, 4, 5; `ui_stack` никогда не `nullptr` — задача 2; hotseat через `mp_activate_player` — задача 5; ворота в обычной сборке — задачи 1–5, 8.
- §3 (структура контекста, метрики не хранятся): задача 1 (`mp_apply_viewport_params` — единственный писатель террейн-метрик; `TERMINAL_X/Y` в серверной сборке закрыта — задача 6).
- §4 (своп, `swap_slot_into_u` приватна, инвариант `player_slots[active]==&u`): задача 5.
- §5 (ui_manager, инвариант владения, ввод): задачи 2, 6.
- §6 (интеграция: владение, создание окон, главный адаптор, широковещательная инвалидация, фаза мира — только зафиксирована, сайдбар): задачи 3, 4, 5, 6.
- §7 (тесты: `mp_context_test`, `mp_context_swap_test`, регресс обычной сборки): задачи 1, 3, 5, 7, 8.
- §8 (риски): гниение пути — контекст в обеих сборках (задача 1); переезд окон — тесты обычной сборки + swap-тест (задачи 3, 7); инвариант стека — ассерт (задача 2).
- §9 (границы): второй игрок, разрез `do_turn`, `mp_server_screen` на сессию, `status` — вне плана, записано в Global Constraints.
- §10 (правки родителя): только к родительскому спеку, в план не входят.

**Отступления от спека, зафиксированные явно.**

1. **`mp_context_swap_test` под `MP_SERVER`**, а не «обычная сборка». Обоснование в Global Constraints: обычная сборка (ncurses) не создаёт окна headless; in-memory curses — только в серверной. Механизм контекста при этом в обычной сборке (задача 1–5 тесты), ворота «не зависят от MULTIPLAYER-сборки» сохранены.
2. **`TERMX/TERMY` пишет `game_ui::init_ui()`, а не `mp_apply_viewport_params`.** Спек §3.1 называет `mp_apply_viewport_params` «единственным писателем», но в TUI-сборке реальный размер терминала — `getmaxx(stdscr)` из `init_ui` (game.cpp:554-557), а `get_terminal_width()` читает опцию `TERMINAL_X` (output.cpp:3289) — другой источник. План: `init_ui` остаётся писателем базовых размеров, контекст зеркалит `TERMX/TERMY` (задачи 3), `mp_apply_viewport_params` пишет их тождественно и вычисляет производные (`TERRAIN_WINDOW_*`, `POSX/POSY`). Запись из контекста станет реальной в Ф3, когда размер терминала пойдёт за активным игроком.
3. **`OVERMAP_*` и `FULL_SCREEN_*` не пишутся из `mp_apply_viewport_params`.** Они остаются у своих существующих писателей (`overmap_ui.cpp:2096-2100`, `game_ui::init_ui`), которые — чистые функции от `TERMX`/`TERMY`. Консистентность по построению. Запись их из контекста потребовала бы трогать писателей — против §12.6 родителя.
4. **`game::contexts` — `std::deque`, а не `std::vector`.** Вектор реаллоцируется при `emplace_back`, и указатели `mp_set_active_context(&contexts[i])`/`set_active_stack(&contexts[i].ui_stack)` повисли бы в Ф3 со вторым контекстом. Deque хранит адреса элементов стабильными.
5. **`mp_ensure_player_context` вызывается из `game::setup()`**, а не только из `create_or_get_main_ui_adaptor`. Загрузка сейва (`savegame.cpp:318`) читает вью-стейт внутри `menu.opening_screen()` до `main.cpp:689`; `setup()` — общий хук new-игры (`main_menu.cpp:991`) и загрузки (`game_io.cpp:315`).
6. **`mp_activate_player` не зовёт `mp_ensure_player_context`** (иначе рекурсия: ensure → activate → ensure): ассертит непустой `contexts`, вызывающие сначала зовут ensure. ensure сам активирует игрока 0 — иначе одиночная игра не прогоняла бы механизм контекста.
7. **`font_units`/`tile_units` в Ф2 не потребляются.** Метрики используют существующую `to_map_font_dimension` (no-op в curses/сервере, реальная в TILES). Формула `w * font_units / tile_units` из §3.1 оживает в tiles-фазе, когда серверу придёт клиент с реальным отношением шрифтов. Поля в структуре есть, потребление — tiles-фаза.

**Почему вью-стейт — 30 правок в 10 файлах, а не копи-синк.** Спек §2 явно отверг копи-синк туда-обратно: забытое поле в одной из сторон гниёт молча. Чтение через активный контекст — один источник правды. Сайтов всего 30 (15+15), один прямой писатель (`game.cpp:3570`); механическая замена с полным списком в задаче 4. Список существующих файлов шире, чем «пять мест» из §12.6 родителя, — это плата решения, принятого в спеке; зафиксировано в Global Constraints.

**Согласованность имён.** `mp_active_context()`/`mp_active_context_ptr()`/`mp_set_active_context` — задачи 1, 3, 4, 5, 6, 7. `mp_apply_viewport_params` — 1, 3, 5. `mp_ensure_player_context` — 3, 5, 8. `mp_resize_active_context` — 3. `mp_activate_player` — 5, 7. `ui_manager::set_active_stack`/`get_active_stack` — 2, 5, 7. `player_context::input_queue`/`input_timeout_ms` — 1, 6. `game::contexts` (deque) — 1, 3, 5, 8. `game_test_helper`/`restore_active_state` — 2, 5, 7.

**Почему нет заглушек.** `mp_active_context()` ассертит ненулевой указатель; `world_stack` в `ui_manager` — реальный объект, включается в Ф3. `mp_activate_world`/`mp_for_each_context` сознательно не объявлены: они не нужны Ф2 и появятся с планировщиком.

---

## Значения, разрешённые при планировании

| Что | Значение |
|---|---|
| Тестовый харнесс | Catch2, `#include "cata_catch.h"`; тег `[mp]`; `tests/cata_test` (обычная) / `tests/mpserver-cata_test` (серверная) |
| Точка входа контекста | `game::setup()` (`game.cpp:678`, хук new+load) и `create_or_get_main_ui_adaptor()` (`game.cpp:3173`) → `mp_ensure_player_context()` |
| Базовые TERMX/TERMY | `game_ui::init_ui()` (`game.cpp:514`): TUI — `getmaxx(stdscr)`+`ensure_term_size`; не-TUI — `get_terminal_width()`; `MP_SERVER` — `mp_server_grid()`. Контекст зеркалит их |
| Сброс слотов в тестах | `game_test_helper::reset_player_slots()` (`tests/game_test_helper.h`; `add_player_slot()` — публичный) |
| RAII теста | `restore_active_state` (`tests/game_test_helper.h`); `restore_on_out_of_scope` (`cata_scope_helpers.h:32`) |
| Масштаб шрифта | `to_map_font_dimension` (`game_ui.h`; no-op в curses/сервере, `sdltiles.cpp:6756` в TILES) |
| Минимальный терминал | `EVEN_MINIMUM_TERM_WIDTH = 80`, `EVEN_MINIMUM_TERM_HEIGHT = 24` (`game_constants.h`) |
| Сайдбар | `panel_manager::get_manager().get_width_left()/get_width_right()` (`panels.h:137-138`) |
| Окна | `catacurses::newwin`, `MINIMAP_WIDTH/HEIGHT` (`game_constants.h`) |
| Режим теста | `extern bool test_mode` (`cached_options.h:44`); `restore_on_out_of_scope` (`cata_scope_helpers.h:32`) |
| Чтение ячеек | `win.get<cata_cursesport::WINDOW>()->line[y].chars[x].ch` (`cursesport.h:44-60`) |
