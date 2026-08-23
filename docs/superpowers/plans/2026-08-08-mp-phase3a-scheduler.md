# Фаза 3a: планировщик с эстафетой — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** два игрока играют одновременно в одном мире по TCP — каждый в своём меню, каждый со своим экраном, — а мир тикает, когда все отходили.

**Architecture:** цикл hotseat удаляется, многоаватарный фундамент под ним остаётся. `game::do_turn()` режется на `world_turn_pre` / `avatar_turn_pre` / `player_act` / `world_turn_post` / `avatar_turn_post` и становится оркестратором; одиночная игра ходит по тому же разрезу при N=1 без потоков. `mp_sched` даёт эстафету: поток на игрока, главный поток — мир, единственная точка передачи — `get_input_event()` серверного бэкенда. `mp_turn` выводит состояния игроков из состояния и держит барьер с «тороплю». `mp_session` заменяет процессные модули экрана и ввода объектом на клиента.

**Tech Stack:** C++17, `std::thread`/`std::condition_variable`, существующие `player_context` (Ф2), `mp_transport`/`mp_loopback_transport`/`mp_tcp_transport` (Ф0–Ф1), Catch2 (`tests/cata_test`, `tests/mpserver-cata_test`), Makefile (авторитетный путь) + CMake, ThreadSanitizer.

## Global Constraints

- **Спеки:** `docs/superpowers/specs/2026-08-08-mp-phase3a-scheduler-design.md` (основной) и `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md` (родитель). Предшественник: `docs/superpowers/specs/2026-08-06-mp-phase2-context-design.md`.
- **Скоуп Ф3a.** Меню входа, создание персонажа в контексте, загрузка мира сервером из меню, один сейв на персонажа — **Ф3b**, в этом плане не реализуются. Залипающее «ходите без меня», разгон долгих активностей, настройка длительностей — **Ф4**. Вход и выход посреди партии, возврат после обрыва, раскрутка стека через `ESC`, перевод в follower-npc, наблюдатель после смерти, конфиг-файл сервера — **Ф5**. Тезер под N > 2 — **Ф6**.
- **Состав сессии фиксирован на старте.** Сервер ждёт N клиентов; обрыв любого клиента или смерть любого игрока заканчивают сессию. Ни парковки контекста, ни льготных интервалов.
- **Один путь для обоих билдов.** Разрез `do_turn` и оркестратор собираются всегда; одиночная игра при N=1 ходит по ним без потоков. Под `MULTIPLAYER` — только `mp_sched`, `mp_turn`, `mp_session` и серверный бэкенд.
- **Правило потоков:** всё, что может открыть UI, исполняется в потоке своего игрока под его контекстом. `world_turn_pre`/`world_turn_post` — на главном потоке, активного игрока нет.
- **Порядок одиночной игры сохраняется точно.** `avatar_turn_post` (восстановление moves) идёт строго после `world_turn_post` (`monmove`).
- **Заглушки запрещены.** Каждый символ появляется с окончательным поведением.
- **Сборка тестов (обычная):** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`. Запуск: `tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`.
- **Сборка тестов (серверная):** `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`. Запуск: `tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-test/`.
- **Полный набор — только на свежей user-dir.** Переиспользованная `--user-dir` копит `past_games_info` и роняет `stats_tracker`.
- **Регресс:** весь тестовый набор обычной сборки зелёный — главный критерий безвредности разреза на каждом шаге.
- **Стиль:** `make astyle`. Комментарии по-английски (кроме документов плана). Лимит 100 колонок.
- **Коммиты:** после каждой задачи, префиксы `feat(mp):`, `refactor(mp):`, `test(mp):`, `build:`, `docs:`.
- **Значения, разрешённые при планировании** — в конце документа.

---

## Обзор файлов

**Создаются:**

| Файл | Ответственность |
|---|---|
| `src/mp/mp_sched.h`, `.cpp` | Эстафета: владелец, `yield_to`/`wait_for_turn`, потоки игроков, ассерты, сторожевой таймер |
| `src/mp/mp_turn.h`, `.cpp` | Состояния игрока, правило барьера, отсчёт «тороплю», автоторопление, белый список свободных действий |
| `src/mp/mp_session.h`, `.cpp` | Объект на клиента: транспорт, сетка ячеек, курсор, бэклог, буфер ввода, резайз, имя, индекс контекста |
| `tests/mp_turn_test.cpp` | Правило барьера, состояния, отсчёт, белый список (обычная сборка) |
| `tests/mp_sched_test.cpp` | Строгость эстафеты, ассерты, сторожевой таймер (серверная сборка) |
| `tests/mp_session_harness.h`, `.cpp` | Сессионный харнесс: сервер и N клиентов на loopback с задержкой |
| `tests/mp_session_test.cpp` | Два игрока в разных меню, сверка экранов, жизнь сессии (серверная сборка) |
| `tests/coop_turn_test.cpp` | Порядок разреза `do_turn` (обычная сборка) |
| `data/json/ui/coop.json` | Виджет `coop_player_desc` (переезд из `hotseat.json`) |

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/do_turn.cpp`, `src/do_turn.h` | Разрез `game::do_turn()` на пять функций плюс оркестратор |
| `src/game.h` | Объявления пяти новых членов; удаление `friend bool mp_hotseat_do_turn()` (:201); комментарии |
| `src/main.cpp` | Удаление `#include "mp/mp_hotseat_loop.h"` (:50) и развилки hotseat (:712-717) |
| `src/mp/mp_context.h`, `.cpp` | `mp_activate_player` берёт `contexts[idx]`; `mp_activate_world()`; `mp_for_each_context()`; `mp_sync_context_count()` |
| `src/ui_manager.h`, `.cpp` | `ui_manager::activate_world_stack()` — единственный способ поставить стек мира |
| `src/mp/mp_mode.h`, `.cpp` | `mp_mode_active()` → `mp_coop_active()` |
| `src/mp/mp_curses_server.cpp` | `get_input_event` отдаёт и забирает эстафету; экран и ввод через активную сессию |
| `src/mp/mp_server_screen.h`, `.cpp` | Процессное состояние переезжает в `mp_session`; модуль становится фасадом над активной сессией |
| `src/mp/mp_server_input.h`, `.cpp` | То же для ввода и резайза |
| `src/mp/mp_server_main.cpp` | Accept до N, `--players`, раздача слотов по `mp_hello.name`, подъём потоков |
| `src/mp/mp_hotseat_loop.h`, `.cpp` | **Удаляются** |
| `src/debug.cpp` | Немодальный `debugmsg` в серверной сборке |
| `src/debug_menu.h`, `.cpp` | `toggle_hotseat` → `toggle_coop_slot`; пункт меню переименован |
| `src/display.h`, `.cpp` | `hotseat_player_text` → `coop_player_text` |
| `src/widget.h`, `.cpp` | `widget_var::hotseat_player_text` → `coop_player_text` |
| `data/json/ui/sidebar.json`, `sidebar-legacy-labels.json` | Ссылка на `coop_player_desc` |
| `src/map.cpp`, `creature.cpp`, `weather.cpp`, `npc.cpp`, `character.cpp`, `game.cpp`, `overmapbuffer.cpp`, `cata_tiles.cpp` | ~35 комментариев «hotseat slot» → «co-op player slot» |
| `tests/hotseat_test.cpp` → `tests/coop_slots_test.cpp` | Переименование файла и тега |
| `tests/hotseat_sim_test.cpp` → `tests/coop_sim_test.cpp` | Переименование; два теста цикла удаляются и возвращаются в задаче 4 |
| `Makefile`, `CMakeLists.txt` | `src/mp/mp_sched.cpp`, `mp_turn.cpp` в обычную сборку; `--players` не требует правок сборки |

**Почему так.** `mp_sched` и `mp_turn` — чистая механика без сети, собираются всегда и проверяются обычной сборкой. `mp_session` — единственный модуль, знающий про транспорт, живёт под `MP_SERVER`. Разрез `do_turn` остаётся в `do_turn.cpp`: это игровая логика, а не мультиплеерный слой, и одиночная игра ходит по нему.

---

### Task 1: Снос hotseat

**Files:**
- Delete: `src/mp/mp_hotseat_loop.h`, `src/mp/mp_hotseat_loop.cpp`
- Rename: `tests/hotseat_test.cpp` → `tests/coop_slots_test.cpp`, `tests/hotseat_sim_test.cpp` → `tests/coop_sim_test.cpp`, `data/json/ui/hotseat.json` → `data/json/ui/coop.json`
- Modify: `src/main.cpp:50,712-717`, `src/game.h:201,455-459,744`, `src/mp/mp_mode.h`, `src/mp/mp_mode.cpp`, `src/display.h:210-212`, `src/display.cpp:1342`, `src/widget.h:78`, `src/widget.cpp:187-188,1165,1299-1300`, `src/debug_menu.h:74-79`, `src/debug_menu.cpp:636,1967,2024,4340`, `data/json/ui/sidebar.json:38`, `data/json/ui/sidebar-legacy-labels.json:227`, `src/map.cpp`, `src/creature.cpp`, `src/weather.cpp`, `src/npc.cpp`, `src/character.cpp`, `src/game.cpp`, `src/overmapbuffer.cpp`, `src/cata_tiles.cpp`

**Interfaces:**
- Consumes: ничего нового.
- Produces: `bool mp_coop_active();` (`src/mp/mp_mode.h`) — предикат «в сессии больше одного аватара», выводится из `g->player_slot_count() > 1`, как и раньше. `void debug_menu::toggle_coop_slot( const std::string &template_name );` (`src/debug_menu.h`) — та же семантика, что у `toggle_hotseat`. `std::string display::coop_player_text( const Character &u );` (`src/display.h`). `widget_var::coop_player_text` (`src/widget.h`).

**Порядок неслучаен:** снос идёт до разреза `do_turn`, чтобы разрез не пришлось делать совместимым с умирающим циклом.

- [ ] **Step 1: Переименовать тестовые файлы и теги — тесты перестают компилироваться**

```bash
git mv tests/hotseat_test.cpp tests/coop_slots_test.cpp
git mv tests/hotseat_sim_test.cpp tests/coop_sim_test.cpp
sed -i 's/"\[hotseat\]"/"[coop]"/g' tests/coop_slots_test.cpp tests/coop_sim_test.cpp
sed -i 's/mp_mode_active/mp_coop_active/g' tests/coop_slots_test.cpp
sed -i 's/debug_menu::toggle_hotseat/debug_menu::toggle_coop_slot/g' tests/coop_slots_test.cpp
sed -i 's/hotseat_debug_toggle_creates_slot_and_enables_mode/coop_debug_toggle_creates_slot_and_enables_mode/' tests/coop_slots_test.cpp
sed -i 's/mp_mode_is_derived_from_the_slot_count/coop_mode_is_derived_from_the_slot_count/' tests/coop_slots_test.cpp
```

- [ ] **Step 2: Удалить два теста цикла из `tests/coop_sim_test.cpp`**

Удалить целиком `TEST_CASE( "hotseat_turn_advances_calendar_once", ... )` и
`TEST_CASE( "hotseat_turn_drains_moves_from_every_slot", ... )` (сейчас строки 124-200),
а также `#include "mp/mp_hotseat_loop.h"` (строка 16).

Они проверяли поведение удаляемого цикла. Эквивалентные тесты нового оркестратора
возвращаются в задаче 4 (`calendar_advances_once_per_turn`,
`every_slot_gets_its_moves_back`). Оставшиеся пять тестов файла (`get_players_returns_every_slot`,
`nearest_player_picks_the_closer_slot`, `anchor_is_integer_midpoint`,
`tether_thresholds_match_the_spec`, `monster_targets_the_closer_player`) не трогаются:
они про слоты, якорь и тезер, которые остаются.

- [ ] **Step 3: Проверить, что сборка тестов падает**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | tail -20`
Expected: FAIL — `'mp_coop_active' was not declared in this scope` в `tests/coop_slots_test.cpp`.

- [ ] **Step 4: Переименовать `mp_mode_active` → `mp_coop_active`**

В `src/mp/mp_mode.h` и `src/mp/mp_mode.cpp` переименовать функцию. Комментарий в
заголовке переписать так, чтобы он не обещал hotseat:

```cpp
// Whether the current session has more than one player character. False in
// every single player session, including in a build that has multiplayer
// compiled in.
//
// Derived from the number of player slots rather than stored in a flag of its
// own: a session is co-op exactly when more than one avatar exists. Two
// independent pieces of state could (and did) disagree in both directions -- a
// save that restored a second slot came back with the mode off, leaving slot 1
// simulated but unplayable, and disabling the mode left the slot behind so
// "single player" still ran two avatars. Deriving it makes both states
// unrepresentable. Turning co-op off therefore means destroying the extra
// slots; see game::remove_extra_player_slots().
bool mp_coop_active();
```

Обновить все вызовы:

```bash
grep -rl "mp_mode_active" src/ | xargs sed -i 's/mp_mode_active/mp_coop_active/g'
```

- [ ] **Step 5: Удалить цикл hotseat и его вызов**

```bash
git rm src/mp/mp_hotseat_loop.h src/mp/mp_hotseat_loop.cpp
```

В `src/main.cpp` удалить строку 50 (`#include "mp/mp_hotseat_loop.h"`) и свести
развилку (строки 712-717) к одной строке:

```cpp
        while( true ) {
            if( g->do_turn() ) {
                break;
            }
        }
```

В `src/game.h:201` удалить `friend bool mp_hotseat_do_turn();`.

- [ ] **Step 6: Переименовать виджет и его источник**

`src/display.h:210-212`:
```cpp
// Which co-op slot currently holds the keyboard, e.g. "P2: Wilburn Longoria".
// Empty in single player.
std::string coop_player_text( const Character &u );
```

`src/display.cpp:1342` — переименовать определение, внутри `mp_coop_active()`.

`src/widget.h:78`:
```cpp
    coop_player_text, // Which co-op slot currently holds the keyboard; empty in single player
```

`src/widget.cpp` — строки 187-188 (`return "coop_player_text";`), 1165, 1299-1300
(`desc.first = display::coop_player_text( ava );`).

```bash
git mv data/json/ui/hotseat.json data/json/ui/coop.json
sed -i 's/hotseat_player_desc/coop_player_desc/; s/hotseat_player_text/coop_player_text/' data/json/ui/coop.json
sed -i 's/hotseat_player_desc/coop_player_desc/' data/json/ui/sidebar.json data/json/ui/sidebar-legacy-labels.json
```

- [ ] **Step 7: Переименовать дебаг-тоггл**

`src/debug_menu.h:74-79`:
```cpp
// Creates the second co-op player slot (once per session, from a saved
// character template when `template_name` is non-empty), which is what
// enables co-op mode. If co-op mode is already active, disables it instead by
// destroying every slot above index 0.
//
// This is the only way to seed a second avatar into a world until Phase 3b
// gives each client a join menu of its own.
void toggle_coop_slot( const std::string &template_name );
```

`src/debug_menu.cpp` — переименовать `toggle_hotseat` (:1967), `toggle_hotseat_menu`
(:2024) → `toggle_coop_slot_menu`, обновить тексты пунктов:

- `:636` — `_( "Add/remove a co-op player slot" )`;
- `:2028` — `_( "Remove the other co-op character?  This permanently deletes them." )`;
- `:4340` — `translate_marker( "Add/remove a co-op player slot" )`, ключ поиска
  `"coop multiplayer slot toggle"`.

Значение `debug_menu_index::TOGGLE_HOTSEAT` переименовать в `TOGGLE_COOP_SLOT`.

- [ ] **Step 8: Пройти по комментариям**

```bash
grep -rn "hotseat" src/ | grep -v "^src/mp/"
```

Ожидается ~35 строк в `map.cpp`, `creature.cpp`, `weather.cpp`, `npc.cpp`,
`character.cpp`, `game.cpp`, `overmapbuffer.cpp`, `cata_tiles.cpp`, `game.h`.
Все — комментарии, описывающие многоаватарную модель верно. Заменить «hotseat slot»
на «co-op player slot», «In hotseat,» на «With more than one player,». Ссылку
`mp_hotseat_loop.cpp sets active_slot()` в `character.cpp:1774` заменить на
`mp_activate_player() sets active_slot()`.

Отдельно `src/game.h:744` — комментарий про использование хотсит-циклом переписать
на «used by the co-op turn loop».

После правок `grep -rn "hotseat" src/ data/ tests/` должен молчать.

- [ ] **Step 9: Собрать и прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-mp-t1/
```
Expected: PASS. `mp_hotseat_do_turn` больше не существует; двухслотовая игра временно
не имеет цикла хода для слота 1 — это восстанавливается оркестратором в задаче 4.

- [ ] **Step 10: Проверить JSON**

Run: `make -j$(nproc) CCACHE=1 TILES=0 tools/format/json_formatter.cgi && tests/cata_test "[widget]" --user-dir=/tmp/cata-mp-t1b/`
(The Makefile has no `json_formatter` target; the formatter binary is `tools/format/json_formatter.cgi`.)
Expected: PASS — виджет `coop_player_desc` резолвится в обоих сайдбарах.

- [ ] **Step 11: Коммит**

```bash
make astyle
git add -A
git commit -m "refactor(mp): remove the hotseat turn loop, keep the multi-avatar foundation

The 94-line sequential loop is replaced by the scheduler in this phase, so
it goes first: the do_turn cut that follows would otherwise have to stay
compatible with a loop that is about to die. Everything else that carried the
hotseat name is the multi-avatar foundation the design reuses whole, so it is
renamed rather than deleted -- slots, tether, bubble anchor, the sim migration
to nearest/any player, and the debug toggle that seeds a second avatar until
Phase 3b gives clients a join menu.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Разрез — `world_turn_pre` и `world_turn_post`

**Files:**
- Modify: `src/do_turn.cpp:523-807`, `src/game.h`
- Create: `tests/coop_turn_test.cpp`

**Interfaces:**
- Consumes: `game::do_turn()` в текущем монолитном виде.
- Produces (объявляются в `src/game.h` рядом с `do_turn`, определяются в `src/do_turn.cpp`):

```cpp
        /**
         * The half of a turn that belongs to the world and reads no particular
         * avatar: calendar, weather, hordes, timed events, missions, autosave,
         * NPC spawning, light. Runs once per turn on the orchestrating thread
         * with no active player (see mp_activate_world()).
         */
        void world_turn_pre();
        /**
         * The half of a turn that moves the world after the players have acted:
         * falling, vehicles, fields, items, explosions, sounds, the map cache,
         * monsters and overmap NPCs. Runs once per turn with no active player.
         *
         * Deliberately does not replenish anybody's moves: in single player the
         * avatar's process_turn() runs after monmove, and the co-op barrier
         * depends on that ordering staying true (see avatar_turn_post).
         */
        void world_turn_post();
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/coop_turn_test.cpp`:

```cpp
#include "cata_catch.h"

#include "avatar.h"
#include "calendar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "mp/mp_context.h"
#include "player_activity.h"

// The world halves of a turn are what the orchestrator runs with no active
// player. These tests pin the two properties the co-op barrier depends on:
// the calendar advances exactly once in the pre half, and the post half never
// hands anybody their moves back.

TEST_CASE( "world_turn_pre_advances_the_calendar_once", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // The first call in a process is the one-time new_game pass
    // (weather.on_game_start), not a calendar advance.
    g->world_turn_pre();

    const int before = to_turn<int>( calendar::turn );
    g->world_turn_pre();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "world_turn_post_does_not_replenish_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    g->world_turn_post();

    // Moves come back in avatar_turn_post, which the orchestrator runs after
    // this. Replenishing here would let a player act again before the monsters
    // had moved, and would make the barrier meaningless.
    CHECK( u.get_moves() <= 0 );
}
```

- [ ] **Step 2: Прогнать тест — он не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "world_turn"`
Expected: FAIL — `'class game' has no member named 'world_turn_pre'`.

- [ ] **Step 3: Объявить обе функции в `src/game.h`**

Вставить объявления из блока **Interfaces** непосредственно перед `bool do_turn();`.

- [ ] **Step 4: Вынести тела**

В `src/do_turn.cpp` создать `void game::world_turn_pre()` и перенести в неё тело
`do_turn()` со строки 529 (`drain_renderer_recovery();`) по строку 608
(`perhaps_add_random_npc( false );`) **за вычетом** пяти персональных кусков, которые
остаются на месте в `do_turn()` и уедут в `avatar_turn_pre` в задаче 3:

- `:561-566` — проверка угона транспорта;
- `:569-572` — `u.stagger()` из непроходимой клетки;
- `:575-577` — `u.check_mount_is_spooked()`;
- `:592` — `u.update_body()`;
- `:595-599` — автосейв (читает `u.is_dead_state()`; остаётся мировым, но проверка
  смерти заменяется на проход по слотам — см. шаг 5).

Создать `void game::world_turn_post()` и перенести туда тело со строки 725
(`m.build_floor_caches();`) по строку 748 (`mon_info_update();`) **за вычетом**
персональных кусков:

- `:720-723` — установка запаха от `u` (обязательно до `scent.update()`);
- `:731` — `m.creature_in_field( u )`.

Эти два и завершающий `mon_info_update()` становятся проходами по слотам прямо внутри
`world_turn_post()` — они мировые по смыслу, но их вход персональный:

```cpp
    scent_map &scent = get_scent();
    // Every player leaves a scent trail, not just whoever holds the baton.
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        avatar &ava = player_at( i );
        if( !ava.has_flag( json_flag_NO_SCENT ) ) {
            scent.set( ava.pos_bub(), ava.scent, ava.get_type_of_scent() );
            overmap_buffer.set_scent( ava.pos_abs_omt(), ava.scent );
        }
    }
    scent.update( player_at( 0 ).pos_bub(), m );
```

и симметрично для `creature_in_field` и `mon_info_update`.

В `do_turn()` на местах вырезанных блоков поставить вызовы `world_turn_pre();`
и `world_turn_post();`. Поведение обязано остаться идентичным.

- [ ] **Step 5: Заменить проверку смерти в автосейве на проход по слотам**

```cpp
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
```

- [ ] **Step 6: Прогнать тест**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[coop]" --user-dir=/tmp/cata-mp-t2/`
Expected: PASS.

- [ ] **Step 7: Прогнать полный регресс**

Run: `tests/cata_test --user-dir=/tmp/cata-mp-t2-full/`
Expected: PASS — весь набор. Это главный критерий, что вынос ничего не сдвинул.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/game.h src/do_turn.cpp tests/coop_turn_test.cpp
git commit -m "refactor(mp): split the world halves out of do_turn

world_turn_pre and world_turn_post are the parts of a turn that belong to
nobody in particular, so they are the parts the orchestrator will run with no
active player. Three steps inside the post half take a per-avatar input
(scent, creature_in_field, mon_info_update) and become passes over the slots;
the autosave guard becomes 'nobody is dead' for the same reason. do_turn calls
both in place, so behaviour is unchanged.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Разрез — `avatar_turn_pre` и `avatar_turn_post`

**Files:**
- Modify: `src/do_turn.cpp`, `src/game.h`, `tests/coop_turn_test.cpp`

**Interfaces:**
- Consumes: `game::world_turn_pre()`, `game::world_turn_post()` (задача 2).
- Produces (объявляются в `src/game.h` рядом с задачей 2):

```cpp
        /**
         * The per-avatar head of a turn: the checks and upkeep that read one
         * character, plus draining whatever activity they already had running.
         *
         * Runs in that player's own thread under that player's context, because
         * an activity can open a window (crafting asks for confirmation) and
         * windows only exist inside their owner's context.
         */
        void avatar_turn_pre( avatar &ava );
        /**
         * The per-avatar tail of a turn: move replenishment, weather effects,
         * body temperature, wetness, morale, sound effects, bionic power.
         *
         * Runs after world_turn_post(), never before: process_turn() hands
         * moves back, and in single player that happens after monmove.
         */
        void avatar_turn_post( avatar &ava );
```

- [ ] **Step 1: Дописать падающие тесты в `tests/coop_turn_test.cpp`**

```cpp
TEST_CASE( "avatar_turn_post_replenishes_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    g->avatar_turn_post( u );

    CHECK( u.get_moves() > 0 );
}

TEST_CASE( "avatar_turn_pre_drains_a_running_activity", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->avatar_turn_pre( u );

    // The activity consumed the turn's moves without the player pressing
    // anything: this is what makes an occupied player count as "has acted"
    // at the barrier (mp_turn, task 7).
    CHECK( u.get_moves() <= 0 );
}

TEST_CASE( "avatar_turn_post_runs_for_every_slot_independently", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    a.set_moves( 0 );
    b.set_moves( 0 );

    g->avatar_turn_post( a );
    g->avatar_turn_post( b );

    // The defect the old hotseat loop had: slot 1 only got process_turn(), so
    // its body temperature, wetness and morale never updated. Both slots now
    // go through the same function.
    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );

    game_test_helper::reset_player_slots();
}
```

Дописать `#include "game_test_helper.h"` и `#include "activity_type.h"`, если их нет.

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "avatar_turn"`
Expected: FAIL — `'class game' has no member named 'avatar_turn_post'`.

- [ ] **Step 3: Объявить обе функции в `src/game.h`**

- [ ] **Step 4: Вынести тела**

`void game::avatar_turn_pre( avatar &ava )` собирает пять кусков, оставленных задачей 2,
плюс дожатие активности и звуки — всё с заменой `u` на `ava`:

```cpp
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
```

`void game::avatar_turn_post( avatar &ava )` берёт хвост `do_turn()` со строки 750
(`u.process_turn();`) по строку 800 (`u.power_prev_turn = u.get_power_level();`)
с той же заменой. `handle_progress_ui()` и `m.invalidate_visibility_cache()` внутри
этого диапазона **не персональные** — оставить их в `world_turn_post()`.

В `do_turn()` на местах вырезанного поставить `avatar_turn_pre( u );` и
`avatar_turn_post( u );` так, чтобы порядок не изменился.

- [ ] **Step 5: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-mp-t3/
tests/cata_test --user-dir=/tmp/cata-mp-t3-full/
```
Expected: PASS оба.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add src/game.h src/do_turn.cpp tests/coop_turn_test.cpp
git commit -m "refactor(mp): split the per-avatar halves out of do_turn

avatar_turn_pre carries the checks that read one character plus draining a
running activity; avatar_turn_post carries move replenishment and the body
upkeep. Both take the avatar explicitly instead of reading u, which is what
lets the orchestrator run them once per player. This also closes the hotseat
defect the design named: slots above zero used to get process_turn() alone and
never updated temperature, wetness or morale.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Разрез — `player_act` и оркестратор `do_turn`

**Files:**
- Modify: `src/do_turn.cpp`, `src/game.h`, `tests/coop_turn_test.cpp`

**Interfaces:**
- Consumes: все четыре функции задач 2-3, `mp_bubble_anchor()` (`src/mp/mp_tether.h`), `mp_activate_player`/`mp_ensure_player_context` (`src/mp/mp_context.h`).
- Produces:

```cpp
        /**
         * One player's action loop: keeps asking for actions while they have
         * moves. Every wait for input inside here is where the baton changes
         * hands (task 6), so this always runs in that player's own thread under
         * that player's context.
         *
         * Returns true when the session must end (the game is over, or the
         * player quit) -- do_turn propagates that to main().
         */
        bool player_act( avatar &ava );
```

`game::do_turn()` сохраняет сигнатуру `bool do_turn()`: `src/main.cpp` его не меняет.

- [ ] **Step 1: Дописать падающие тесты — порядок и оба слота**

```cpp
TEST_CASE( "calendar_advances_once_per_turn", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // With no moves and no activity a slot's action loop is skipped, which is
    // what makes this headless: handle_action() would read real input.
    auto idle_all_slots = [&]() {
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            avatar &idle = g->player_at( i );
            idle.set_moves( 0 );
            idle.activity = player_activity();
        }
    };

    idle_all_slots();
    g->do_turn();          // one-time new_game pass

    idle_all_slots();
    const int before = to_turn<int>( calendar::turn );
    g->do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "every_slot_gets_its_moves_back", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // Both slots spend their turn on an activity: the action loop reads real
    // input and cannot run headless, the activity branch can.
    a.set_moves( 100 );
    b.set_moves( 100 );
    a.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    b.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->do_turn();

    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "moves_come_back_only_after_the_world_moved", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    // The single-player ordering the whole barrier rests on: world_turn_post
    // (monmove) happens while the player is still out of moves.
    g->world_turn_pre();
    CHECK( u.get_moves() <= 0 );
    g->world_turn_post();
    CHECK( u.get_moves() <= 0 );
    g->avatar_turn_post( u );
    CHECK( u.get_moves() > 0 );
}
```

`mp_sync_context_count()` появляется в задаче 5; до неё в этих двух тестах его нет.
Дописать вызовы в задаче 5 вместе с самой функцией — в этой задаче тесты пишутся
без него.

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "player_act\|do_turn"`
Expected: FAIL — тест `every_slot_gets_its_moves_back` не проходит: слот 1 не ходит.

- [ ] **Step 3: Вынести `player_act`**

Перенести в `bool game::player_act( avatar &ava )` тело со строки 632
(`if( !u.has_effect( effect_sleep ) ...`) по строку 705 (закрывающая скобка
внешнего `if`), заменив `u` на `ava`. Ранний выход из середины цикла
(`return turn_handler::cleanup_at_end();`) превращается в `return true;`, конец
функции — `return false;`.

Условие цикла действий остаётся прежним (`while( ava.get_moves() > 0 || uquit ==
QUIT_WATCH )`). Слагаемое завершения сессии (`!mp_sched_stopping()`) добавится в
задаче 10, где в `do_turn` появляется ветка планировщика: `mp_sched` тогда уже
существует, а до неё в `do_turn.cpp` ему неоткуда взяться.

- [ ] **Step 4: Собрать оркестратор**

```cpp
bool game::do_turn()
{
    if( is_game_over() ) {
        return turn_handler::cleanup_at_end();
    }

    // The world half: no player is active, so nothing here may open a window.
    mp_activate_world();
    world_turn_pre();

    // The player half. With one player there is no scheduler and no thread:
    // the orchestrator runs the loop inline, which is what makes the ordinary
    // single-player build walk this same code and every test exercise it.
    // mp_sched replaces this branch for more than one player (task 6).
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        mp_activate_player( i );
        avatar &ava = player_at( i );
        avatar_turn_pre( ava );
        if( player_act( ava ) ) {
            return true;
        }
    }

    mp_activate_world();
    world_turn_post();

    // Moves come back here, after monmove, exactly as in single player. The
    // baton goes back to each player for this because handle_progress_ui and
    // the morale paths can draw.
    for( size_t i = 0; i < player_slot_count(); ++i ) {
        mp_activate_player( i );
        avatar_turn_post( player_at( i ) );
    }

    mp_activate_player( 0 );

    // Keep the reality bubble centred between the players. A single player is
    // his own midpoint, so this is identical to the single-player behaviour.
    if( player_slot_count() > 1 ) {
        const tripoint_abs_ms anchor = mp_bubble_anchor();
        shift_map_center( anchor );
        if( anchor.z() != get_map().get_abs_sub().z() ) {
            shift_map_z( anchor.z() );
        }
    }

    debug_menu::debug_capture::tick_if_active();
    return false;
}
```

`mp_activate_world()` появляется в задаче 5. До неё поставить `mp_activate_player( 0 )`
в обоих местах и пометить комментарием `// task 5 replaces this with mp_activate_world()`.

- [ ] **Step 5: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-mp-t4/
tests/cata_test --user-dir=/tmp/cata-mp-t4-full/
```
Expected: PASS оба.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add src/game.h src/do_turn.cpp tests/coop_turn_test.cpp
git commit -m "feat(mp): do_turn becomes the orchestrator of the split turn

The action loop moves into player_act, and do_turn becomes the sequence the
design draws: world_turn_pre, every player acting, world_turn_post, then every
player's tail. Single player walks it at N=1 with no scheduler and no thread,
so all of it is covered by the ordinary test suite. This also restores the turn
for slots above zero, which the hotseat loop used to provide.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Контекст на игрока

**Files:**
- Modify: `src/mp/mp_context.h`, `src/mp/mp_context.cpp`, `src/ui_manager.h`, `src/ui_manager.cpp`, `src/do_turn.cpp`, `tests/mp_context_test.cpp`, `tests/coop_turn_test.cpp`

**Interfaces:**
- Consumes: `game::contexts` (`src/game.h:448`, `std::deque<player_context>`), `ui_manager::set_active_stack`/`get_active_stack`.
- Produces:

```cpp
// Grows or shrinks game::contexts to match game::player_slot_count(). New
// contexts inherit the terminal size of context 0, which is what a freshly
// seeded slot wants until its client says otherwise. Callers that add or
// remove a slot must call this; the active context is re-pointed afterwards
// because a shrink can invalidate it.
void mp_sync_context_count();

// Enters the world phase: no player is active. The ui stack becomes the
// world stack (a real object, not nullptr -- it catches the emergency UI that
// debugmsg raises), and the avatar aliases are left pointing at slot 0 so any
// code that still reads them sees a live character rather than a dangling one.
void mp_activate_world();

// Runs `fn` once per player context, activating each in turn and restoring the
// previously active one afterwards. Used by the broadcast paths: world-side
// code that has to draw into every player's screen (explosions, animations).
void mp_for_each_context( const std::function<void( player_context & )> &fn );
```

```cpp
namespace ui_manager
{
// Points the active stack at the world stack. The only way to reach it from
// outside ui_manager.cpp: the world stack is not a player's stack and must
// never be handed out as one.
void activate_world_stack();
} // namespace ui_manager
```

- [ ] **Step 1: Написать падающие тесты**

Дописать в `tests/mp_context_test.cpp`:

```cpp
TEST_CASE( "contexts_track_the_slot_count", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 1 );

    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    CHECK( g->contexts.size() == 2 );
    // A fresh context inherits the terminal size of context 0 until its client
    // reports its own.
    CHECK( g->contexts[1].viewport.term_cols == g->contexts[0].viewport.term_cols );
    CHECK( g->contexts[1].viewport.term_rows == g->contexts[0].viewport.term_rows );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    CHECK( g->contexts.size() == 1 );
}

TEST_CASE( "activating_a_player_selects_that_players_context", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    mp_activate_player( 0 );
    CHECK( mp_active_context_ptr() == &g->contexts[0] );
    CHECK( ui_manager::get_active_stack() == &g->contexts[0].ui_stack );

    mp_activate_player( 1 );
    CHECK( mp_active_context_ptr() == &g->contexts[1] );
    CHECK( ui_manager::get_active_stack() == &g->contexts[1].ui_stack );

    // View state is per-context, not shared: writing through one player's
    // context must not be visible through the other's.
    mp_active_context().driving_view_offset = point_rel_ms( 7, 9 );
    mp_activate_player( 0 );
    CHECK( mp_active_context().driving_view_offset != point_rel_ms( 7, 9 ) );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_world_phase_has_no_active_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_activate_player( 0 );

    mp_activate_world();

    // The world stack is a real object: debugmsg can raise a UI during the
    // world phase, and it must land somewhere rather than dereference null.
    CHECK( ui_manager::get_active_stack() != nullptr );
    CHECK( ui_manager::get_active_stack() != &g->contexts[0].ui_stack );

    mp_activate_player( 0 );
}

TEST_CASE( "for_each_context_visits_every_player_and_restores", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_activate_player( 1 );

    std::vector<player_context *> seen;
    mp_for_each_context( [&seen]( player_context & ctx ) {
        seen.push_back( &ctx );
    } );

    CHECK( seen.size() == 2 );
    CHECK( seen[0] == &g->contexts[0] );
    CHECK( seen[1] == &g->contexts[1] );
    // The caller's context is restored: broadcast drawing must not leave the
    // baton holder pointing at somebody else's windows.
    CHECK( mp_active_context_ptr() == &g->contexts[1] );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_sync_context_count\|mp_activate_world"`
Expected: FAIL — `'mp_sync_context_count' was not declared in this scope`.

- [ ] **Step 3: Добавить `ui_manager::activate_world_stack()`**

`src/ui_manager.h`, рядом с `set_active_stack` (:315):

```cpp
// Points the active stack at the world stack -- the stack that owns the UI
// raised while no player is active (debugmsg, and nothing else by design).
// It is a separate entry point rather than a getter because the world stack
// must never be handed out as if it were a player's.
void activate_world_stack();
```

`src/ui_manager.cpp`, рядом с `set_active_stack` (:600):

```cpp
void activate_world_stack()
{
    active_stack = &world_stack;
}
```

- [ ] **Step 4: Реализовать три функции в `mp_context`**

В `src/mp/mp_context.h` добавить `#include <functional>` и объявления из
блока **Interfaces**.

В `src/mp/mp_context.cpp`:

```cpp
void mp_sync_context_count()
{
    const size_t want = g->player_slot_count();
    cata_assert( want >= 1 );
    // A shrink can free the element the active pointer names, so drop the
    // pointer first and re-point below.
    const size_t active = g->contexts.empty() ? 0 : g->active_slot();
    while( g->contexts.size() < want ) {
        g->contexts.emplace_back();
        player_context &fresh = g->contexts.back();
        fresh.slot_index = g->contexts.size() - 1;
        // Until this player's client reports its own terminal, borrow slot 0's:
        // a context with a zero-sized viewport would compute degenerate metrics.
        fresh.viewport = g->contexts[0].viewport;
        fresh.stdscr = g->contexts[0].stdscr;
    }
    while( g->contexts.size() > want ) {
        g->contexts.pop_back();
    }
    mp_activate_player( active < want ? active : 0 );
}

void mp_activate_world()
{
    // No player is active. The avatar aliases keep pointing at slot 0 rather
    // than being cleared: world code that still reads get_avatar() sees a live
    // character, and slot 0 is always the one resident in u between swaps.
    ui_manager::activate_world_stack();
    mp_set_active_context( nullptr );
}

void mp_for_each_context( const std::function<void( player_context & )> &fn )
{
    const size_t restore = g->active_slot();
    for( size_t i = 0; i < g->contexts.size(); ++i ) {
        mp_activate_player( i );
        fn( g->contexts[i] );
    }
    mp_activate_player( restore );
}
```

- [ ] **Step 5: `mp_activate_player` берёт свой контекст**

В `src/mp/mp_context.cpp:90-92` заменить

```cpp
    // In Phase 2 there is a single context (hotseat shares it between slots);
    // per-player contexts (contexts[idx]) arrive with the scheduler in Phase 3.
    player_context &ctx = g->contexts[0];
```

на

```cpp
    cata_assert( idx < g->contexts.size() );
    player_context &ctx = g->contexts[idx];
    ctx.slot_index = idx;
```

- [ ] **Step 6: Подключить в оркестратор и в дебаг-тоггл**

В `src/do_turn.cpp` заменить два помеченных `// task 5` вызова
`mp_activate_player( 0 )` на `mp_activate_world()`.

В `src/debug_menu.cpp`, в `toggle_coop_slot`, после `add_player_slot()` и после
`remove_extra_player_slots()` добавить `mp_sync_context_count();` — слот без контекста
уронил бы `mp_activate_player` на ассерте.

Дописать `mp_sync_context_count()` в тесты задачи 4 (`every_slot_gets_its_moves_back`),
как отмечено там же.

- [ ] **Step 7: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp],[coop]" --user-dir=/tmp/cata-mp-t5/
tests/cata_test --user-dir=/tmp/cata-mp-t5-full/
```
Expected: PASS оба.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_context.h src/mp/mp_context.cpp src/ui_manager.h src/ui_manager.cpp \
        src/do_turn.cpp src/debug_menu.cpp tests/mp_context_test.cpp tests/coop_turn_test.cpp
git commit -m "feat(mp): one context per player, and a world phase with none

mp_activate_player finally reaches contexts[idx] instead of always contexts[0],
which is what makes two players able to sit in two menus. mp_activate_world
names the other half of the design: during the world phase nobody is active and
the ui stack is the world stack, so a debugmsg raised there lands somewhere real
instead of dereferencing null.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: `mp_sched` — эстафета

**Files:**
- Create: `src/mp/mp_sched.h`, `src/mp/mp_sched.cpp`, `tests/mp_sched_test.cpp`
- Modify: `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `mp_activate_player`, `mp_activate_world` (задача 5).
- Produces:

```cpp
// The baton. Exactly one participant runs at a time; the right to run is passed
// explicitly. There is no real parallelism, which is what keeps world mutation
// as sequential as it is in single player -- and what makes a ThreadSanitizer
// report a genuine missed handoff rather than noise.
//
// Participants are identified by index: 0..n-1 are the players, and
// mp_sched::world is the orchestrating thread.
class mp_sched
{
    public:
        static constexpr size_t world = static_cast<size_t>( -1 );

        // Creates the scheduler with `players` player participants. The calling
        // thread becomes the world participant and starts out holding the baton.
        explicit mp_sched( size_t players );
        ~mp_sched();

        mp_sched( const mp_sched & ) = delete;
        mp_sched &operator=( const mp_sched & ) = delete;

        // Hands the baton to `to` and blocks until this participant holds it
        // again. The two halves are one call because every yield in the game is
        // a yield-and-resume: the code that gave up the baton always wants to
        // continue where it left off.
        void yield_to( size_t from, size_t to );

        // Blocks until `who` holds the baton. Used by a freshly started player
        // thread, which has nothing to hand over yet.
        void wait_for_turn( size_t who );

        // Hands the baton to `to` without waiting to get it back. Used by a
        // player thread that is finishing for the turn.
        void release_to( size_t from, size_t to );

        // Who holds the baton right now.
        size_t holder() const;

        // Whether the calling thread is the current holder. The debug assert
        // "only the holder runs" is written in terms of this.
        bool holds( size_t who ) const;

        // Ends every player participant: each is woken in turn and told to
        // return from its body. Called by the destructor and by session
        // teardown.
        void shut_down();

        // Whether shut_down() has been called. A player thread body checks it
        // after every resume and returns when it is set.
        bool stopping() const;

        // Whether `who` is currently parked on wait_for_turn (or on the resume
        // half of yield_to), i.e. ready to be handed the baton. The world polls
        // this to find who wants to run next. A slot blocked on the wire inside
        // an input wait is NOT waiting for the baton and reports false.
        bool waiting_for_baton( size_t who ) const;

        // Blocks the world participant until any player becomes ready to run, or
        // until `timeout_ms` passes. Returns true when a player is ready. This
        // is what replaces a spin: the world sleeps instead of burning a core
        // while a partner reads a menu.
        bool wait_for_request( int timeout_ms );

        // Declares that the world is holding the baton on purpose: the barrier
        // is unsatisfied and it is waiting for somebody to come back from an
        // input wait. Set by the orchestrator when it closes out a waiting turn
        // and cleared as soon as the barrier is satisfied.
        //
        // Only the watchdog reads it. Without it, "a partner read a menu for
        // half a minute" -- the design working exactly as specified -- logs as
        // a stalled participant, and the phase gate's log fills with false
        // alarms until nobody reads it.
        void set_world_waiting( bool waiting );
        bool world_waiting() const;

    private:
        // Asserts that `to` names a real participant. Handing the baton to an
        // index nobody waits on parks it forever.
        void check_target( size_t to ) const;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        size_t players_ = 0;
        size_t holder_ = world;
        bool stopping_ = false;
        bool world_waiting_ = false;
        // Per-participant "parked on wait_for_turn / the resume half of
        // yield_to" flag, indexed by player slot (0..players_-1). The world
        // participant has no entry: it never waits for the baton, it waits for
        // somebody to ask for it (wait_for_request).
        std::vector<char> waiting_for_baton_;
};

// The world participant's polling cadence while the barrier is held: it sleeps
// up to this long for a player to become ready before deciding the tick must
// wait. A few round-trips -- the loopback test transport runs at ~5 ms latency,
// so a keypress lands within one or two polls. Too small spins the world, too
// large makes waiting turns sluggish.
constexpr int MP_POLL_MS = 50;

// The scheduler of the running session, or nullptr in a single-player session
// (where there is no baton because there is nothing to pass it to).
mp_sched *mp_active_sched();
void mp_set_active_sched( mp_sched *sched );

// Debug-build assert that the calling thread is the baton holder for `who`.
// A no-op in release builds. Catches "running without holding", which the
// watchdog cannot see -- the watchdog only catches "holding too long".
void mp_assert_holds( size_t who );

// Whether the session is shutting down; false when there is no scheduler at
// all. Read by player_act's loop, which must unwind when the session ends:
// get_input_event hands back a timeout event without the baton, and a loop that
// kept going would call get_input_event again and trip mp_assert_holds.
bool mp_sched_stopping();

// Starts the watchdog thread. It logs the current holder whenever the baton has
// not moved for longer than `threshold_ms`, then keeps watching. Stopped by
// mp_sched::shut_down().
void mp_sched_start_watchdog( mp_sched &sched, int threshold_ms );
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_sched_test.cpp`:

```cpp
#include "cata_catch.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "mp/mp_sched.h"

// The baton's whole promise is that exactly one participant runs at a time.
// These tests state that promise in a way that fails loudly if it breaks.

TEST_CASE( "the_baton_starts_with_the_world", "[mp]" )
{
    mp_sched sched( 2 );
    CHECK( sched.holder() == mp_sched::world );
}

TEST_CASE( "only_one_participant_runs_at_a_time", "[mp]" )
{
    constexpr size_t players = 3;
    constexpr int rounds = 200;

    mp_sched sched( players );
    // Incremented on entry and decremented on exit by every participant. If two
    // ever ran at once this would exceed 1, which is exactly the bug the baton
    // exists to prevent.
    std::atomic<int> inside{ 0 };
    std::atomic<int> max_inside{ 0 };
    std::atomic<bool> failed{ false };

    std::vector<std::thread> threads;
    for( size_t i = 0; i < players; ++i ) {
        threads.emplace_back( [&sched, &inside, &max_inside, &failed, i]() {
            for( int r = 0; r < rounds; ++r ) {
                sched.wait_for_turn( i );
                if( sched.stopping() ) {
                    return;
                }
                const int now = ++inside;
                if( now > 1 ) {
                    failed = true;
                }
                int prev = max_inside.load();
                while( now > prev && !max_inside.compare_exchange_weak( prev, now ) ) {}
                --inside;
                sched.release_to( i, ( i + 1 ) % players );
            }
        } );
    }

    // Kick the round robin off, then take the baton back at the end.
    sched.yield_to( mp_sched::world, 0 );
    sched.shut_down();
    for( std::thread &t : threads ) {
        t.join();
    }

    CHECK_FALSE( failed.load() );
    CHECK( max_inside.load() == 1 );
}

TEST_CASE( "yield_to_returns_only_when_the_baton_comes_back", "[mp]" )
{
    mp_sched sched( 1 );
    std::atomic<bool> player_ran{ false };

    std::thread player( [&sched, &player_ran]() {
        sched.wait_for_turn( 0 );
        player_ran = true;
        sched.release_to( 0, mp_sched::world );
    } );

    sched.yield_to( mp_sched::world, 0 );
    // The world only gets here after the player handed the baton back, so the
    // player's work is guaranteed visible -- no polling, no sleeping.
    CHECK( player_ran.load() );
    CHECK( sched.holder() == mp_sched::world );

    sched.shut_down();
    player.join();
}

TEST_CASE( "shut_down_wakes_every_waiting_participant", "[mp]" )
{
    mp_sched sched( 2 );
    std::atomic<int> returned{ 0 };

    std::vector<std::thread> threads;
    for( size_t i = 0; i < 2; ++i ) {
        threads.emplace_back( [&sched, &returned, i]() {
            sched.wait_for_turn( i );
            ++returned;
        } );
    }

    // Nobody was ever handed the baton: without shut_down both threads would
    // block forever, which is the failure mode this guards.
    sched.shut_down();
    for( std::thread &t : threads ) {
        t.join();
    }
    CHECK( returned.load() == 2 );
}

TEST_CASE( "a_parked_player_reports_waiting_for_the_baton", "[mp]" )
{
    mp_sched sched( 1 );

    std::thread player( [&sched]() {
        sched.wait_for_turn( 0 );
    } );

    // wait_for_turn sets the flag before parking, so this poll terminates.
    while( !sched.waiting_for_baton( 0 ) ) {
        std::this_thread::yield();
    }
    CHECK( sched.waiting_for_baton( 0 ) );
    // A requester is already parked: wait_for_request sees it immediately.
    CHECK( sched.wait_for_request( 100 ) );

    sched.shut_down();
    player.join();
    CHECK_FALSE( sched.waiting_for_baton( 0 ) );
}

TEST_CASE( "wait_for_request_times_out_when_nobody_is_ready", "[mp]" )
{
    mp_sched sched( 1 );

    // Nobody is parked and nobody becomes ready within the timeout: the world
    // must not spin, it must return false so do_turn can close out a waiting
    // turn.
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE( sched.wait_for_request( 50 ) );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Actually waited out the timeout rather than returning on a spurious wake.
    CHECK( elapsed >= std::chrono::milliseconds( 45 ) );
}

TEST_CASE( "a_declared_wait_is_off_by_default", "[mp]" )
{
    mp_sched sched( 1 );

    // Default false, so a scheduler nobody told about waiting turns still gets
    // its stalls reported. The flag suppresses the watchdog, and a suppression
    // that defaults to on would hide the one bug the watchdog exists to find.
    CHECK_FALSE( sched.world_waiting() );
    sched.set_world_waiting( true );
    CHECK( sched.world_waiting() );
    sched.set_world_waiting( false );
    CHECK_FALSE( sched.world_waiting() );
}

TEST_CASE( "mp_sched_stopping_is_false_without_a_scheduler", "[mp]" )
{
    // player_act's loop condition calls this on every iteration in single
    // player, where there is no scheduler at all.
    mp_set_active_sched( nullptr );
    CHECK_FALSE( mp_sched_stopping() );

    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    CHECK_FALSE( mp_sched_stopping() );
    sched.shut_down();
    CHECK( mp_sched_stopping() );
    mp_set_active_sched( nullptr );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_sched"`
Expected: FAIL — `mp/mp_sched.h: No such file or directory`.

- [ ] **Step 3: Создать `src/mp/mp_sched.h`**

Содержимое — блок **Interfaces** целиком, обёрнутый в `#pragma once` /
`#ifndef CATA_SRC_MP_MP_SCHED_H`, с `#include <condition_variable>`, `<cstddef>`,
`<mutex>`, `<vector>`.

- [ ] **Step 4: Создать `src/mp/mp_sched.cpp`**

```cpp
#include "mp/mp_sched.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "cata_assert.h"
#include "debug.h"

namespace
{
mp_sched *g_active_sched = nullptr;
std::thread g_watchdog;
std::atomic<bool> g_watchdog_stop{ false };
} // namespace

mp_sched::mp_sched( const size_t players ) : players_( players ),
    waiting_for_baton_( players_, 0 )
{
}

// Handing the baton to a participant that does not exist would park it forever:
// nobody is waiting on that index, so nobody ever hands it back. Caught here
// rather than diagnosed later from a hang.
void mp_sched::check_target( const size_t to ) const
{
    cata_assert( to == world || to < players_ );
}

mp_sched::~mp_sched()
{
    shut_down();
}

void mp_sched::yield_to( const size_t from, const size_t to )
{
    check_target( to );
    std::unique_lock<std::mutex> lock( mutex_ );
    cata_assert( holder_ == from );
    holder_ = to;
    if( from != world ) {
        // The caller is now parked waiting for the baton back. Flag first,
        // notify second, exactly as in wait_for_turn: the two orders are
        // equivalent only because the mutex is held across both, and writing
        // them the same way everywhere keeps that from being load-bearing.
        waiting_for_baton_[from] = 1;
    }
    cv_.notify_all();
    cv_.wait( lock, [this, from] {
        return holder_ == from || stopping_;
    } );
    if( from != world ) {
        waiting_for_baton_[from] = 0;
    }
}

void mp_sched::wait_for_turn( const size_t who )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    // Flag first, notify second: a world parked in wait_for_request must wake
    // and see that a requester appeared, or it would sleep out its whole poll.
    waiting_for_baton_[who] = 1;
    cv_.notify_all();
    cv_.wait( lock, [this, who] {
        return holder_ == who || stopping_;
    } );
    waiting_for_baton_[who] = 0;
}

void mp_sched::release_to( const size_t from, const size_t to )
{
    check_target( to );
    std::lock_guard<std::mutex> lock( mutex_ );
    cata_assert( holder_ == from );
    holder_ = to;
    cv_.notify_all();
}

bool mp_sched::waiting_for_baton( const size_t who ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return who < waiting_for_baton_.size() && waiting_for_baton_[who] != 0;
}

bool mp_sched::wait_for_request( const int timeout_ms )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    return cv_.wait_for( lock, std::chrono::milliseconds( timeout_ms ), [this] {
        for( const char flag : waiting_for_baton_ ) {
            if( flag != 0 ) {
                return true;
            }
        }
        return false;
    } );
}

void mp_sched::set_world_waiting( const bool waiting )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    world_waiting_ = waiting;
}

bool mp_sched::world_waiting() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return world_waiting_;
}

size_t mp_sched::holder() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return holder_;
}

bool mp_sched::holds( const size_t who ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return holder_ == who;
}

void mp_sched::shut_down()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        stopping_ = true;
    }
    cv_.notify_all();
    g_watchdog_stop = true;
    if( g_watchdog.joinable() ) {
        g_watchdog.join();
    }
}

bool mp_sched::stopping() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return stopping_;
}

mp_sched *mp_active_sched()
{
    return g_active_sched;
}

void mp_set_active_sched( mp_sched *const sched )
{
    g_active_sched = sched;
}

bool mp_sched_stopping()
{
    const mp_sched *const sched = mp_active_sched();
    return sched && sched->stopping();
}

void mp_assert_holds( const size_t who )
{
#if !defined(NDEBUG)
    if( mp_sched *const sched = mp_active_sched() ) {
        // "Running without holding the baton" is the failure the watchdog
        // cannot see: it only measures how long the baton sat still. Catching
        // it here names the offending participant at the moment it runs.
        cata_assert( sched->holds( who ) );
    }
#else
    ( void )who;
#endif
}

void mp_sched_start_watchdog( mp_sched &sched, const int threshold_ms )
{
    g_watchdog_stop = false;
    g_watchdog = std::thread( [&sched, threshold_ms]() {
        size_t last = sched.holder();
        auto since = std::chrono::steady_clock::now();
        bool reported = false;
        while( !g_watchdog_stop ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            const size_t now_holder = sched.holder();
            if( now_holder != last ) {
                last = now_holder;
                since = std::chrono::steady_clock::now();
                reported = false;
                continue;
            }
            const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - since ).count();
            if( held > threshold_ms && !reported ) {
                if( last == mp_sched::world && sched.world_waiting() ) {
                    // Not a stall. The world holds the baton for as long as a
                    // partner reads a menu, and that is the design working.
                    // Reporting it would make the one message that means "a
                    // handoff was missed" indistinguishable from normal play.
                    continue;
                }
                // Report once per stall, not once per poll: a genuinely stuck
                // participant would otherwise fill the log faster than anyone
                // could read it.
                DebugLog( D_WARNING, D_MAIN ) << "mp sched: participant "
                                              << ( last == mp_sched::world ? std::string( "world" ) :
                                                   std::to_string( last ) )
                                              << " has held the baton for " << held << " ms";
                reported = true;
            }
        }
    } );
}
```

- [ ] **Step 5: Добавить в сборку**

`Makefile:1116` — рядом с `mp_context.cpp`:

```make
# The player context and the scheduler ship in every build; the rest of src/mp/
# is MULTIPLAYER-only.
SOURCES += $(SRC_DIR)/mp/mp_context.cpp
SOURCES += $(SRC_DIR)/mp/mp_sched.cpp
```

В `CMakeLists.txt` — добавить `src/mp/mp_sched.cpp` туда же, где `src/mp/mp_context.cpp`.

- [ ] **Step 6: Прогнать тесты**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-t6/
```
Expected: PASS.

- [ ] **Step 7: Прогнать под ThreadSanitizer**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 TSAN=1 BUILD_PREFIX=tsan-
tests/tsan-cata_test "[mp]" --user-dir=/tmp/cata-mp-t6-tsan/
```
Expected: PASS, ноль отчётов TSAN. Отчёт здесь означал бы настоящую гонку в самой
эстафете, а не в игре.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_sched.h src/mp/mp_sched.cpp tests/mp_sched_test.cpp Makefile CMakeLists.txt
git commit -m "feat(mp): the baton -- one participant runs at a time

Threads with a strict baton, as the design chose over fibers: portable C++,
a real stack per player, and a failure mode (a hang) that is loud rather than
silent state corruption. The watchdog catches 'holds and will not let go'; the
debug assert catches 'runs without holding'. Ships in every build so the
ordinary test suite exercises it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: `mp_turn` — состояния игрока и барьер

**Files:**
- Create: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`, `tests/mp_turn_test.cpp`
- Modify: `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `game::player_at`, `game::player_slot_count`, `avatar::get_moves`, `avatar::activity`, `Character::has_distant_destination`.
- Produces:

```cpp
// What a player is doing from the turn loop's point of view. Derived from game
// state on every query rather than stored: two pieces of state that could
// disagree is exactly the bug the co-op mode fixed once already.
enum class mp_player_state {
    // Has moves and is currently executing.
    acting,
    // Out of moves, or occupied by something that will spend the turn without
    // asking: a running activity, or a travel destination already set. This is
    // the cheap form of "planned turns" -- a player who has told the game what
    // to do does not have to be at the keyboard for the world to move on.
    acted,
    // Has moves and is sitting at an input wait. The only state that holds the
    // barrier, and therefore the only state a countdown is aimed at.
    holding,
    // Dropped from the barrier for this turn by a hurry countdown.
    not_waited,
};

mp_player_state mp_player_state_of( size_t slot );

// True while `slot` is one of the players still holding the barrier. Read by
// the sidebar widget so everyone can see who is being waited on.
bool mp_player_holds_barrier( size_t slot );

// True when the world may tick: every living player has either acted or been
// dropped from the barrier. The single rule the whole turn model rests on.
bool mp_barrier_satisfied();

// Marks `slot` as sitting at an input wait, or no longer sitting at one. Called
// by the server backend around the point where it blocks on the wire, because
// that is the only place the game can tell "thinking" from "working".
void mp_set_waiting_for_input( size_t slot, bool waiting );

// Clears the per-turn barrier bookkeeping that belongs to a fresh turn: the
// hurry countdown, and the countdown's drops -- but only for players who have
// since left their input wait.
//
// Two deliberate exceptions, both of which were freezes when they were not
// exceptions:
//
// The input-wait flags are never cleared here. They belong to the input backend
// and live as long as the player is actually parked on a wait. Clearing them
// would turn a parked player into "acting", which mp_tick_hurry() cannot drop.
//
// A drop survives while its player has not moved from the same input wait. The
// alternative is one keypress of "hurry up" buying exactly one tick of world:
// a partner reading a recipe list would have to be hurried again every turn,
// which is the phase gate ("an evening of play") made unplayable by its own
// safety valve. Once they press a key and act, the input-wait flag clears and
// they rejoin the barrier on their own.
//
// Called once per opened turn by the orchestrator.
void mp_turn_reset();

// Clears everything the module holds, including the input-wait flags, the turn
// flag and the end-of-session request. For test setup and teardown and for
// session start: those are the moments when the state of a previous session is
// not conservative but wrong.
void mp_turn_reset_all();

// Whether a turn is currently open: world_turn_pre has run and the barrier has
// not been satisfied yet. While open, do_turn skips world_turn_pre and only
// polls the barrier, so a waiting turn never re-runs the world half.
bool mp_turn_is_open();

// Opens or closes the turn. Set true right after world_turn_pre runs, set false
// once the barrier is satisfied and world_turn_post has run.
void mp_set_turn_open( bool open );

// The session must end: a player quit or died out of the game. Player threads
// call the setter because player_act's return value has nowhere else to go --
// they run inside a thread body, not inside do_turn -- and the orchestrator
// turns it into do_turn()'s `true` for main().
void mp_request_session_end();
bool mp_session_end_requested();
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_turn_test.cpp`:

```cpp
#include "cata_catch.h"

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "mp/mp_context.h"
#include "mp/mp_turn.h"
#include "player_activity.h"

// The barrier rule is the whole turn model. These tests pin the four ways a
// player can stop holding it -- and the one way they keep holding it.

TEST_CASE( "a_player_out_of_moves_has_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "a_player_at_an_input_wait_with_moves_holds_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    mp_set_waiting_for_input( 0, true );

    // This is the case the whole design is about: a turn is typically one
    // action, and browsing a menu costs no time, so a player reading a recipe
    // list would freeze their partner for as long as they read.
    CHECK( mp_player_state_of( 0 ) == mp_player_state::holding );
    CHECK_FALSE( mp_barrier_satisfied() );

    mp_set_waiting_for_input( 0, false );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acting );
}

TEST_CASE( "a_running_activity_counts_as_having_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    mp_set_waiting_for_input( 0, true );

    // Occupied, not thinking: the activity will spend the turn whether or not
    // the player is looking at the screen, so the barrier must not wait.
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "a_travel_destination_counts_as_having_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    u.set_destination( { u.pos_bub() + tripoint::east } );
    mp_set_waiting_for_input( 0, true );

    // This is the cheap form of planned turns: the player set a route, walked
    // away into a menu, and their character keeps moving.
    REQUIRE( u.has_distant_destination() );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );

    u.clear_destination();
}

TEST_CASE( "the_barrier_waits_for_the_slowest_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.set_moves( 0 );
    a.activity = player_activity();
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );

    CHECK_FALSE( mp_barrier_satisfied() );

    b.set_moves( 0 );
    CHECK( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_turn"`
Expected: FAIL — `mp/mp_turn.h: No such file or directory`.

- [ ] **Step 3: Создать `src/mp/mp_turn.h`**

Блок **Interfaces** целиком, с `#include <cstddef>` и guard
`CATA_SRC_MP_MP_TURN_H`.

- [ ] **Step 4: Создать `src/mp/mp_turn.cpp`**

```cpp
#include "mp/mp_turn.h"

#include <vector>

#include "avatar.h"
#include "game.h"
#include "player_activity.h"

namespace
{
// Barrier bookkeeping, indexed by slot. Not part of player_context: it is state
// about the turn, not about the player's display. What belongs to one turn and
// what outlives it is spelled out at mp_turn_reset in the header.
std::vector<bool> g_waiting_for_input;
std::vector<bool> g_dropped_by_countdown;
// Whether a turn is currently open (see mp_turn_is_open in the header).
bool g_turn_open = false;
// Whether a player asked for the session to end (see mp_request_session_end).
bool g_session_end = false;

void ensure_sized()
{
    const size_t n = g->player_slot_count();
    g_waiting_for_input.resize( n, false );
    g_dropped_by_countdown.resize( n, false );
}
} // namespace

mp_player_state mp_player_state_of( const size_t slot )
{
    ensure_sized();
    if( slot < g_dropped_by_countdown.size() && g_dropped_by_countdown[slot] ) {
        return mp_player_state::not_waited;
    }
    avatar &ava = g->player_at( slot );
    // "Occupied" outranks "has moves": an activity or a set destination will
    // spend this turn without the player touching a key, so making the barrier
    // wait on them would freeze everybody for no reason.
    if( ava.activity || ava.has_distant_destination() ) {
        return mp_player_state::acted;
    }
    if( ava.get_moves() <= 0 ) {
        return mp_player_state::acted;
    }
    if( slot < g_waiting_for_input.size() && g_waiting_for_input[slot] ) {
        return mp_player_state::holding;
    }
    return mp_player_state::acting;
}

bool mp_barrier_satisfied()
{
    ensure_sized();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( g->player_at( i ).is_dead_state() ) {
            continue;
        }
        const mp_player_state state = mp_player_state_of( i );
        if( state != mp_player_state::acted && state != mp_player_state::not_waited ) {
            return false;
        }
    }
    return true;
}

void mp_set_waiting_for_input( const size_t slot, const bool waiting )
{
    ensure_sized();
    if( slot < g_waiting_for_input.size() ) {
        g_waiting_for_input[slot] = waiting;
    }
}

void mp_turn_reset()
{
    ensure_sized();
    for( size_t i = 0; i < g_dropped_by_countdown.size(); ++i ) {
        // A drop is released only by its own player moving on. Somebody who is
        // still sitting at the same input wait stays dropped, or one press of
        // "hurry up" would buy exactly one tick and the next turn would stop on
        // them again. The input-wait flags themselves are never cleared here
        // (see the header). The hurry countdown is reset in task 8.
        if( i >= g_waiting_for_input.size() || !g_waiting_for_input[i] ) {
            g_dropped_by_countdown[i] = false;
        }
    }
}

void mp_turn_reset_all()
{
    g_waiting_for_input.assign( g->player_slot_count(), false );
    g_dropped_by_countdown.assign( g->player_slot_count(), false );
    g_turn_open = false;
    g_session_end = false;
}

bool mp_turn_is_open()
{
    return g_turn_open;
}

void mp_set_turn_open( const bool open )
{
    g_turn_open = open;
}

void mp_request_session_end()
{
    g_session_end = true;
}

bool mp_session_end_requested()
{
    return g_session_end;
}
```

- [ ] **Step 5: Добавить в сборку**

`Makefile` — `SOURCES += $(SRC_DIR)/mp/mp_turn.cpp` рядом с `mp_sched.cpp`;
то же в `CMakeLists.txt`.

- [ ] **Step 6: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-t7/
tests/cata_test --user-dir=/tmp/cata-mp-t7-full/
```
Expected: PASS оба.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp tests/mp_turn_test.cpp Makefile CMakeLists.txt
git commit -m "feat(mp): the barrier rule and the four player states

States are derived from game state on every query rather than stored: two
pieces of state that can disagree is a bug this mode already paid for once.
The rule that matters is that an activity or a set travel destination counts as
having acted -- without it, a turn being one action and menus costing no time
means any partner's open menu is your pause.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: `mp_turn` — «тороплю», автоторопление, белый список простоя

**Files:**
- Modify: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`, `tests/mp_turn_test.cpp`, `src/handle_action.cpp`, `src/action.h`, `src/action.cpp`, `data/raw/keybindings.json`

**Interfaces:**
- Consumes: `mp_player_state_of`, `mp_barrier_satisfied` (задача 7), `action_id` (`src/action.h:22`), `Creature::sees`, `game::get_creatures_if`.
- Produces:

```cpp
// Starts the hurry countdown, if one is not already running. Any player who has
// already acted may call it. The countdown is visible to everyone; when it
// expires the server stops waiting for whoever is still holding -- it does not
// reach into their UI or close anything. Their character stands still and skips
// the turn; their menu stays open and they confirm when they are ready.
void mp_request_hurry();

// Milliseconds left on the countdown, or -1 when none is running. Read by the
// sidebar widget and by the tests.
int mp_hurry_remaining_ms();

// Advances the countdown and, when it expires, drops every holding player from
// the barrier for this turn. Called once per orchestrator poll.
//
// Also starts the countdown by itself, on the shorter combat length, while a
// hostile creature in the bubble can see any player: nobody spends a keystroke
// on "hurry up" when something is running at them.
void mp_tick_hurry();

// Spends the turn's moves of every player the world stopped waiting for.
// Called by the orchestrator once per closed turn, immediately before
// avatar_turn_post hands moves back.
//
// Not an optimisation and not cosmetic: moves accumulate (Character::
// process_turn does `moves += get_speed()`), and a dropped player never spends
// theirs, because they are reading a menu. Without this, a partner who read a
// recipe list for a minute comes back holding a minute's worth of moves and
// takes sixty actions inside one tick, which is both an exploit and exactly
// what "their character stands still and skips the turn" says does not happen.
void mp_forfeit_dropped_moves();

// Whether `act` may be taken by a player who has already acted this turn.
//
// The default is "no": the list below is a whitelist, not a classification of
// all 153 action ids. Classifying every one of them would be both expensive and
// dangerous -- a mistake in the "free" direction lets a player act outside
// their turn. Inverted, a mistake merely refuses something harmless, which is
// annoying and fixed by one line.
bool mp_action_is_free( action_id act );

// Moves the countdown's start point back by `ms`, so a test can expire a
// fifteen-second countdown without sleeping for fifteen seconds.
void mp_advance_hurry_for_test( int ms );
```

Длительности отсчёта — константы в `mp_turn.h`; настройка через конфиг сервера — Ф4:

```cpp
// Long enough to finish reading a screen, short enough that a partner does not
// go and make tea. Tuned against the Phase 3a gate; configurable in Phase 4.
constexpr int MP_HURRY_MS = 15000;
// A fight is not the moment to read anything.
constexpr int MP_HURRY_COMBAT_MS = 4000;
```

- [ ] **Step 1: Написать падающие тесты**

Дописать в `tests/mp_turn_test.cpp`:

```cpp
TEST_CASE( "a_hurry_countdown_drops_the_holder_from_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.set_moves( 0 );
    a.activity = player_activity();
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    REQUIRE_FALSE( mp_barrier_satisfied() );

    mp_request_hurry();
    CHECK( mp_hurry_remaining_ms() > 0 );

    // Expire it by ticking past the countdown.
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();

    // The holder is dropped, not interrupted: still at their input wait, still
    // with moves, but no longer keeping the world from moving.
    CHECK( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    CHECK( b.get_moves() > 0 );
    CHECK( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "the_countdown_clears_with_the_turn", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_request_hurry();
    REQUIRE( mp_hurry_remaining_ms() > 0 );

    mp_turn_reset();
    CHECK( mp_hurry_remaining_ms() == -1 );
}

TEST_CASE( "a_drop_lasts_while_the_player_stays_at_the_same_input_wait", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    g->player_at( 0 ).set_moves( 0 );
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();
    REQUIRE( mp_player_state_of( 1 ) == mp_player_state::not_waited );

    // Next turn opens. They are still in the same menu, so they stay dropped:
    // one press of "hurry up" that bought exactly one tick would mean hurrying
    // a partner again every turn for as long as they read, which is the phase
    // gate made unplayable by its own safety valve.
    mp_turn_reset();
    CHECK( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    CHECK( mp_barrier_satisfied() );

    // They press a key and act: the backend clears the wait, and the next turn
    // waits for them again like anybody else.
    mp_set_waiting_for_input( 1, false );
    mp_turn_reset();
    CHECK( mp_player_state_of( 1 ) == mp_player_state::acting );
    CHECK_FALSE( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "a_dropped_player_does_not_bank_the_moves_they_never_spent", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    g->player_at( 0 ).set_moves( 0 );
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();
    REQUIRE( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    // The drop itself takes nothing away: the menu is still open and the
    // character is untouched, exactly as the design promises.
    REQUIRE( b.get_moves() > 0 );

    // Closing the turn is where the standing still is paid for. Moves
    // accumulate, so without this the reader banks a turn's worth per tick.
    mp_forfeit_dropped_moves();
    CHECK( b.get_moves() == 0 );
    // Whoever is not dropped keeps whatever they have.
    CHECK( g->player_at( 0 ).get_moves() == 0 );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "menu_actions_are_free_while_waiting_and_costly_ones_are_not", "[mp]" )
{
    // The whitelist is inverted on purpose: anything not named here is refused
    // for a player who already acted, so a gap in the list is an annoyance
    // rather than a free action outside the turn.
    CHECK( mp_action_is_free( ACTION_INVENTORY ) );
    CHECK( mp_action_is_free( ACTION_PL_INFO ) );
    CHECK( mp_action_is_free( ACTION_MAP ) );
    CHECK( mp_action_is_free( ACTION_LOOK ) );
    CHECK( mp_action_is_free( ACTION_DISPLAY_MESSAGES ) );
    CHECK( mp_action_is_free( ACTION_OPTIONS ) );
    CHECK( mp_action_is_free( ACTION_HELP ) );
    CHECK( mp_action_is_free( ACTION_ZOOM_IN ) );
    CHECK( mp_action_is_free( ACTION_ZOOM_OUT ) );
    CHECK( mp_action_is_free( ACTION_COOP_HURRY ) );

    CHECK_FALSE( mp_action_is_free( ACTION_MOVE_FORTH ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PICKUP ) );
    CHECK_FALSE( mp_action_is_free( ACTION_FIRE ) );
    CHECK_FALSE( mp_action_is_free( ACTION_CRAFT ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PAUSE ) );
}
```

Плюс тест на видимость состояния — без него игроки не знают, кого ждут:

```cpp
TEST_CASE( "the_sidebar_says_who_is_being_waited_on", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    a.set_moves( 0 );
    a.activity = player_activity();
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );

    REQUIRE( mp_player_holds_barrier( 1 ) );
    // Ada is waiting on Bo, and has to be told so -- otherwise the only signal
    // that the game has not frozen is that it has not frozen.
    const std::string line = display::coop_player_text( a );
    CHECK( line.find( "Bo" ) != std::string::npos );

    mp_request_hurry();
    const std::string counting = display::coop_player_text( a );
    CHECK( counting.find( "Bo" ) != std::string::npos );
    // A countdown that is running but invisible is worse than none: the player
    // cannot tell whether pressing hurry did anything.
    CHECK( counting != line );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_request_hurry\|ACTION_COOP_HURRY"`
Expected: FAIL — `'mp_request_hurry' was not declared in this scope`.

- [ ] **Step 3: Добавить действие `ACTION_COOP_HURRY`**

В `src/action.h`, в `enum action_id`, непосредственно перед `NUM_ACTIONS`:

```cpp
    /** Ask everyone still thinking to hurry up: starts a visible countdown,
     *  after which the world moves on without them. Co-op only. */
    ACTION_COOP_HURRY,
```

В `src/action.cpp` — строка в `action_ident()` (`"coop_hurry"`) и в
`action_name()` (`_( "Hurry the others" )`), по образцу соседних значений.

В `data/raw/keybindings.json` — привязка по умолчанию к `'` в контексте `DEFAULTMODE`,
по образцу соседних записей.

- [ ] **Step 4: Реализовать отсчёт и белый список**

Дописать в `src/mp/mp_turn.cpp`:

```cpp
namespace
{
// When the countdown started, or nullopt when none is running. A time point
// rather than a remaining count: the orchestrator polls at whatever rate the
// session runs at, and a decrementing counter would drift with it.
std::optional<std::chrono::steady_clock::time_point> g_hurry_started;
int g_hurry_length_ms = MP_HURRY_MS;

// True while a hostile creature inside the bubble can see any player.
bool anyone_is_seen_by_a_hostile()
{
    for( Creature &critter : g->all_creatures() ) {
        monster *const mon = dynamic_cast<monster *>( &critter );
        if( mon == nullptr || mon->friendly != 0 ) {
            continue;
        }
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            if( mon->sees( get_map(), g->player_at( i ) ) ) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

void mp_request_hurry()
{
    if( g_hurry_started ) {
        return;
    }
    g_hurry_started = std::chrono::steady_clock::now();
    g_hurry_length_ms = MP_HURRY_MS;
}

int mp_hurry_remaining_ms()
{
    if( !g_hurry_started ) {
        return -1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - *g_hurry_started ).count();
    const int left = g_hurry_length_ms - static_cast<int>( elapsed );
    return left > 0 ? left : 0;
}

void mp_advance_hurry_for_test( const int ms )
{
    if( g_hurry_started ) {
        *g_hurry_started -= std::chrono::milliseconds( ms );
    }
}

void mp_tick_hurry()
{
    ensure_sized();
    // Nobody spends a keystroke on "hurry up" when something is running at
    // them, so combat starts its own countdown, and a shorter one.
    if( !g_hurry_started && anyone_is_seen_by_a_hostile() ) {
        g_hurry_started = std::chrono::steady_clock::now();
        g_hurry_length_ms = MP_HURRY_COMBAT_MS;
    }
    if( !g_hurry_started || mp_hurry_remaining_ms() > 0 ) {
        return;
    }
    // Expired. Drop everyone still holding -- for this turn only. Their UI is
    // untouched: their character stands still and skips the turn, their menu
    // stays open, and they confirm their choice when they are ready.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( mp_player_state_of( i ) == mp_player_state::holding ) {
            g_dropped_by_countdown[i] = true;
        }
    }
    g_hurry_started.reset();
}

void mp_forfeit_dropped_moves()
{
    ensure_sized();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( mp_player_state_of( i ) == mp_player_state::not_waited ) {
            // Standing still is what the drop means. Taken at the end of the
            // turn rather than at the moment of the drop, because a drop lasts
            // as long as the player sits in the same menu while every closed
            // turn hands out another turn's worth of moves.
            g->player_at( i ).set_moves( 0 );
        }
    }
}

bool mp_action_is_free( const action_id act )
{
    switch( act ) {
        // Reading and looking: what a player actually wants to do while their
        // partner thinks.
        case ACTION_INVENTORY:
        case ACTION_PL_INFO:
        case ACTION_MAP:
        case ACTION_SKY:
        case ACTION_LOOK:
        case ACTION_DISPLAY_MESSAGES:
        case ACTION_MISSIONS:
        case ACTION_FACTIONS:
        case ACTION_MORALE:
        case ACTION_MEDICAL:
        // Client-side and bookkeeping.
        case ACTION_OPTIONS:
        case ACTION_HELP:
        case ACTION_KEYBINDINGS:
        case ACTION_ZOOM_IN:
        case ACTION_ZOOM_OUT:
        case ACTION_TOGGLE_PANEL_ADM:
        case ACTION_TOGGLE_SAFEMODE:
        case ACTION_TOGGLE_AUTOSAFE:
        // The escape hatch itself, and leaving.
        case ACTION_COOP_HURRY:
        case ACTION_QUIT:
        case ACTION_SAVE:
            return true;
        default:
            // Everything else costs time, or might. Refusing is the safe
            // direction: a gap here is an annoyance, a gap the other way is a
            // free action outside the turn.
            return false;
    }
}
```

Дописать `mp_turn_reset()` сбросом отсчёта: `g_hurry_started.reset();
g_hurry_length_ms = MP_HURRY_MS;`. То же — в `mp_turn_reset_all()`.

`mp_player_holds_barrier` — тонкая обёртка, нужная виджету, чтобы не тащить
`mp_player_state` в `display.cpp`:

```cpp
bool mp_player_holds_barrier( const size_t slot )
{
    return mp_player_state_of( slot ) == mp_player_state::holding;
}
```

- [ ] **Step 5: Расширить сайдбарный виджет до строки состояния**

`src/display.cpp`, `display::coop_player_text` — сейчас показывает только активный
слот. Дописать, кого ждут и идёт ли отсчёт:

```cpp
std::string display::coop_player_text( const Character &u )
{
    if( !mp_coop_active() ) {
        return std::string();
    }
    // Who is being waited on, and for how much longer. Without this the only
    // signal that the session has not frozen is that it has not frozen.
    std::string waiting;
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( mp_player_holds_barrier( i ) ) {
            waiting += ( waiting.empty() ? std::string() : ", " ) + g->player_at( i ).name;
        }
    }
    if( waiting.empty() ) {
        return string_format( _( "P%d: %s" ), g->active_slot() + 1, u.name );
    }
    const int left = mp_hurry_remaining_ms();
    if( left < 0 ) {
        return string_format( _( "waiting: %s" ), waiting );
    }
    return string_format( _( "waiting: %s (%d)" ), waiting, ( left + 999 ) / 1000 );
}
```

- [ ] **Step 6: Подключить к `handle_action`**

В `src/handle_action.cpp`, сразу после того как действие определено и до его
исполнения:

```cpp
#if defined(MULTIPLAYER)
    if( mp_coop_active() && !mp_action_is_free( act ) &&
        mp_player_state_of( g->active_slot() ) == mp_player_state::acted ) {
        add_msg( m_info, _( "You have already acted this turn." ) );
        return false;
    }
    if( act == ACTION_COOP_HURRY ) {
        mp_request_hurry();
        return false;
    }
#endif
```

- [ ] **Step 7: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-t8/
tests/cata_test --user-dir=/tmp/cata-mp-t8-full/
```
Expected: PASS оба.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp src/action.h src/action.cpp \
        src/handle_action.cpp src/display.cpp data/raw/keybindings.json tests/mp_turn_test.cpp
git commit -m "feat(mp): the hurry countdown and the idle whitelist

The escape hatch the barrier needs to be survivable: a player who has acted can
start a visible countdown, after which the server stops waiting for whoever is
still thinking. It never reaches into their UI -- their menu stays open, their
character skips the turn. Combat starts the countdown by itself, and shorter.
The idle whitelist is inverted on purpose: the default is 'costs time', so a
missing entry refuses something harmless instead of granting a free action.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 9: `mp_session` — объект на клиента

**Files:**
- Create: `src/mp/mp_session.h`, `src/mp/mp_session.cpp`
- Modify: `src/mp/mp_server_screen.h`, `src/mp/mp_server_screen.cpp`, `src/mp/mp_server_input.h`, `src/mp/mp_server_input.cpp`, `tests/mp_server_io_test.cpp`

**Interfaces:**
- Consumes: `mp_screen_grid` (`src/mp/mp_cell.h`), `mp_transport`, `player_context`.
- Produces:

```cpp
// Everything the server holds for one connected client. This used to be
// process-wide state in mp_server_screen.cpp and mp_server_input.cpp, which was
// only ever right because there was exactly one client.
struct mp_session {
    // The wire. Owned here; the session outlives every use of it.
    std::shared_ptr<mp_transport> transport;
    // What this client's screen looks like, with per-row dirty tracking.
    mp_screen_grid grid{ 80, 24 };
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = false;
    bool cursor_changed = false;
    // A resize this client asked for that the curses backend has not applied.
    std::optional<mp_resize> pending_resize;
    // The character name from the hello, used to pick this client's slot.
    std::string name;
    // Index into game::contexts and game::player_slots. The two are parallel,
    // and the session is addressed through this index rather than holding a
    // player_context pointer: player_context ships in every build and must not
    // learn about networking types.
    size_t context_index = 0;
};

// The sessions of the running server, indexed the same way as game::contexts.
std::vector<std::unique_ptr<mp_session>> &mp_sessions();

// The session of the player whose context is active, or nullptr during the
// world phase (when no player is active) and in tests without a session.
mp_session *mp_active_session();

// Drops every session. Tests only.
void mp_sessions_reset_for_test();
```

`mp_server_handshake` меняет сигнатуру — размер терминала и имя теперь принадлежат
сессии, а не процессу:

```cpp
// Was: bool mp_server_handshake( mp_transport &, std::string &reject_reason );
//
// Fills `session.name` and sizes `session.grid` from the accepted hello instead
// of writing the process-wide grid. The transport parameter stays separate from
// the session because the handshake runs before the session is worth keeping:
// a refused peer never becomes one.
bool mp_server_handshake( mp_transport &transport, mp_session &session,
                          std::string &reject_reason );
```

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_server_io_test.cpp`:

```cpp
TEST_CASE( "each_session_keeps_its_own_screen", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_sessions_reset_for_test();

    mp_loopback_pair wire_a = mp_make_loopback_pair();
    mp_loopback_pair wire_b = mp_make_loopback_pair();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->transport = wire_a.a;
    mp_sessions()[0]->context_index = 0;
    mp_sessions()[0]->grid.resize( 80, 24 );
    mp_sessions()[1]->transport = wire_b.a;
    mp_sessions()[1]->context_index = 1;
    mp_sessions()[1]->grid.resize( 100, 30 );

    mp_activate_player( 0 );
    REQUIRE( mp_active_session() == mp_sessions()[0].get() );
    mp_server_blit_row( 0, 0, { mp_cell{ "A", 1, 0 } } );

    mp_activate_player( 1 );
    REQUIRE( mp_active_session() == mp_sessions()[1].get() );
    mp_server_blit_row( 0, 0, { mp_cell{ "B", 2, 0 } } );

    // The leak this guards is the one the design calls the main class of bug:
    // player 1 seeing player 2's screen.
    CHECK( mp_sessions()[0]->grid.at( 0, 0 ).ch == "A" );
    CHECK( mp_sessions()[1]->grid.at( 0, 0 ).ch == "B" );
    CHECK( mp_sessions()[0]->grid.width() == 80 );
    CHECK( mp_sessions()[1]->grid.width() == 100 );

    mp_activate_player( 0 );
    mp_sessions_reset_for_test();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_world_phase_has_no_active_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->context_index = 0;

    mp_activate_player( 0 );
    REQUIRE( mp_active_session() != nullptr );

    mp_activate_world();
    // Nothing the world phase draws belongs to one client, so there is nothing
    // to address. Broadcast drawing goes through mp_for_each_context.
    CHECK( mp_active_session() == nullptr );

    mp_activate_player( 0 );
    mp_sessions_reset_for_test();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- 2>&1 | grep -m3 "mp_session"`
Expected: FAIL — `mp/mp_session.h: No such file or directory`.

- [ ] **Step 3: Создать `src/mp/mp_session.h` и `.cpp`**

Заголовок — блок **Interfaces**, обёрнутый в `#if defined(MP_SERVER)` и guard
`CATA_SRC_MP_MP_SESSION_H`.

`src/mp/mp_session.cpp`:

```cpp
#if defined(MP_SERVER)

#include "mp/mp_session.h"

#include "mp/mp_context.h"

namespace
{
std::vector<std::unique_ptr<mp_session>> g_sessions;
} // namespace

std::vector<std::unique_ptr<mp_session>> &mp_sessions()
{
    return g_sessions;
}

mp_session *mp_active_session()
{
    const player_context *const ctx = mp_active_context_ptr();
    if( ctx == nullptr ) {
        // The world phase: no player is active, so no client is addressed.
        return nullptr;
    }
    for( const std::unique_ptr<mp_session> &s : g_sessions ) {
        if( s && s->context_index == ctx->slot_index ) {
            return s.get();
        }
    }
    return nullptr;
}

void mp_sessions_reset_for_test()
{
    g_sessions.clear();
}

#endif // MP_SERVER
```

- [ ] **Step 4: Превратить `mp_server_screen` в фасад над активной сессией**

В `src/mp/mp_server_screen.cpp` удалить `g_grid`, `g_transport`, `g_cursor_*` и
переписать каждую функцию так, чтобы она работала с `mp_active_session()`:

```cpp
mp_screen_grid &mp_server_grid()
{
    static mp_screen_grid fallback( 80, 24 );
    // Before any client is attached (startup, tests) the game still asks for a
    // grid to size stdscr from. The fallback keeps that path alive without
    // pretending a session exists.
    mp_session *const s = mp_active_session();
    return s ? s->grid : fallback;
}

void mp_server_blit_row( const int x0, const int y, const std::vector<mp_cell> &cells )
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        return;
    }
    for( size_t i = 0; i < cells.size(); ++i ) {
        s->grid.set( x0 + static_cast<int>( i ), y, cells[i] );
    }
}
```

и так далее для `mp_server_set_terminal_size`, `mp_server_peer_connected`,
`mp_server_set_cursor`, `mp_server_flush_screen`. `mp_server_set_transport` и
`mp_server_shared_transport` удаляются: транспорт теперь принадлежит сессии.

- [ ] **Step 5: То же для `mp_server_input`**

`g_pending_resize` удаляется; `dispatch` пишет в активную сессию, и вся работа
с проводом идёт через неё:

```cpp
void dispatch( const mp_control &msg )
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        // No client is addressed: the world phase never reads the wire, and a
        // test without a session has nowhere to put this.
        return;
    }
    switch( msg.kind ) {
        case mp_control_kind::input:
            if( player_context *ctx = mp_active_context_ptr() ) {
                ctx->input_queue.push_back( msg.event );
            }
            break;
        case mp_control_kind::resize:
            if( msg.resize.width <= MP_MAX_TERMINAL_WIDTH &&
                msg.resize.height <= MP_MAX_TERMINAL_HEIGHT ) {
                s->pending_resize = msg.resize;
            }
            break;
        case mp_control_kind::bye:
            // Closing this client's transport is what makes its own recv
            // return; the other clients' waits are untouched.
            if( s->transport ) {
                s->transport->close();
            }
            break;
        default:
            break;
    }
}
```

`mp_server_take_resize_request`, `mp_server_resize_pending`,
`mp_server_wait_for_input` и `mp_server_pump` — то же самое: `mp_active_session()`
вместо `mp_server_shared_transport()`, ранний выход при `nullptr`.

`mp_server_handshake` получает новую сигнатуру (см. **Interfaces**) и вместо
`mp_server_set_terminal_size( w, h )` делает `session.grid.resize( w, h )` и
`session.name = hello.hello.name;`.

- [ ] **Step 6: Добавить в сборку и прогнать**

`Makefile` — новый файл подхватывается глобом `$(wildcard $(SRC_DIR)/mp/*.cpp)`
под `MULTIPLAYER`; правок не нужно. В `CMakeLists.txt` проверить, что глоб `src/mp/*.cpp`
покрывает файл.

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-t9/
```
Expected: PASS.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/mp/mp_session.h src/mp/mp_session.cpp src/mp/mp_server_screen.h \
        src/mp/mp_server_screen.cpp src/mp/mp_server_input.h src/mp/mp_server_input.cpp \
        tests/mp_server_io_test.cpp CMakeLists.txt
git commit -m "feat(mp): a session object per client instead of process-wide state

The screen and input modules held process-wide state, which was only ever
correct because there was exactly one client -- their own headers said so. Each
client now owns its grid, cursor, resize request, transport and name, addressed
through the active context's slot index rather than a pointer, so player_context
stays free of networking types in the builds that have no networking.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 10: Эстафета в серверном бэкенде

**Files:**
- Modify: `src/mp/mp_curses_server.cpp`, `src/mp/mp_server_input.cpp`, `src/do_turn.cpp`

**Interfaces:**
- Consumes: `mp_sched` (задача 6), `mp_turn` (задачи 7-8), `mp_session` (задача 9).
- Produces: ничего нового; это подключение.

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_server_io_test.cpp`:

```cpp
TEST_CASE( "waiting_for_input_yields_the_baton", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sessions_reset_for_test();
    mp_loopback_pair wire = mp_make_loopback_pair( 5 );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->transport = wire.a;
    mp_sessions()[0]->context_index = 0;
    mp_activate_player( 0 );

    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    on_out_of_scope clear_sched( []() {
        mp_set_active_sched( nullptr );
    } );

    std::atomic<bool> world_ran{ false };
    std::thread player( [&]() {
        sched.wait_for_turn( 0 );
        // get_input_event() must give the baton up while it blocks on the wire:
        // otherwise a player thinking in a menu stops everybody else, which is
        // the whole thing the scheduler exists to prevent.
        input_manager::get_input_event( keyboard_mode::keychar );
        sched.release_to( 0, mp_sched::world );
    } );

    sched.yield_to( mp_sched::world, 0 );
    // Reaching here means the player thread released the baton from inside its
    // input wait, before any key was sent.
    world_ran = true;
    CHECK( world_ran.load() );

    // Let the player finish.
    mp_control key;
    key.kind = mp_control_kind::input;
    key.event = input_event( static_cast<int>( ' ' ), input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( key );
    wire.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );
    sched.shut_down();
    player.join();
    mp_sessions_reset_for_test();
}
```

- [ ] **Step 2: Прогнать — падает зависанием или ассертом**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && timeout 60 tests/mpserver-cata_test "waiting_for_input_yields_the_baton" --user-dir=/tmp/cata-mp-t10/`
Expected: FAIL — таймаут: `get_input_event` держит эстафету и блокируется на проводе.

- [ ] **Step 3: Отдавать и забирать эстафету вокруг ожидания**

В `src/mp/mp_curses_server.cpp`, в `input_manager::get_input_event`, обернуть
`mp_server_wait_for_input`:

```cpp
        const size_t slot = mp_active_context_ptr() ? mp_active_context().slot_index : 0;
        mp_assert_holds( slot );

        const int timeout = mp_active_context_ptr() ?
                            mp_active_context().input_timeout_ms : -1;

        input_event ev;
        if( mp_sched *const sched = mp_active_sched() ) {
            // This is the only place in the game where the baton changes hands.
            // The screen goes out first (inside wait_for_input), because the
            // player must see what the game drew before anyone waits on them;
            // then the baton is released, and this thread blocks on its own
            // socket holding nothing. A slow wire cannot stall the simulation,
            // because whoever holds the baton is somebody else.
            mp_set_waiting_for_input( slot, true );
            sched->release_to( slot, mp_sched::world );
            ev = mp_server_wait_for_input( timeout );
            sched->wait_for_turn( slot );
            mp_set_waiting_for_input( slot, false );
            if( sched->stopping() ) {
                ev = input_event();
                ev.type = input_event_t::timeout;
                return ev;
            }
        } else {
            ev = mp_server_wait_for_input( timeout );
        }
```

В `src/do_turn.cpp` к условию цикла действий `player_act` (`do_turn.cpp:634`)
добавить завершение сессии — второй конец того же пути размотки. `get_input_event`
при остановке вернул timeout-событие **без эстафеты**, и цикл, который продолжился
бы, зашёл бы в `get_input_event` снова — прямо на `mp_assert_holds`:

```cpp
            while( ( ava.get_moves() > 0 || uquit == QUIT_WATCH ) && !mp_sched_stopping() ) {
```

В одиночной игре `mp_sched_stopping()` всегда `false`, поэтому поведение не меняется.
`#include "mp/mp_sched.h"` в `do_turn.cpp`.

Порядок в шаге 3 сохраняется (`wait_for_turn` → `mp_set_waiting_for_input( slot, false )`):
он безгоночный, потому что флаг пишется только под эстафетой. Сигнал «кто хочет
эстафету обратно» живёт в `mp_sched::waiting_for_baton` (задача 6), а не в этом флаге.

- [ ] **Step 4: Перестроить оркестратор на возобновляемый ход**

В `src/do_turn.cpp` оркестратор получает ветку с планировщиком. Ход при N > 1
возобновляемый: `world_turn_pre` открывает его один раз, а пока барьер не выполнен,
`do_turn()` возвращается, не закрывая ход. Одиночная ветка (sched == nullptr)
остаётся ровно такой, как в задаче 4 step 4.

```cpp
bool game::do_turn()
{
    if( is_game_over() ) {
        return turn_handler::cleanup_at_end();
    }

    if( mp_sched *const sched = mp_active_sched() ) {
        // With more than one player the turn is resumable: world_turn_pre opens
        // it once, and while the barrier is unsatisfied do_turn returns without
        // closing it -- the calendar stays frozen until every living player has
        // acted or been dropped by the hurry countdown.
        mp_activate_world();
        if( mp_session_end_requested() ) {
            // A player quit or died out of the game. Their thread had nowhere
            // to return player_act's `true` to, so it left it here.
            return turn_handler::cleanup_at_end();
        }
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
        while( !mp_barrier_satisfied() ) {
            mp_tick_hurry();
            bool moved = false;
            for( size_t i = 0; i < player_slot_count(); ++i ) {
                const mp_player_state state = mp_player_state_of( i );
                // waiting_for_baton alone is not enough: a player who already
                // acted this turn releases and immediately re-parks on
                // wait_for_turn for the next one, so they report it too -- and
                // yielding to them again would spin the world. Hand the baton
                // only to someone who still owes the turn: acting (not yet
                // released this turn) or holding (returning from an input wait).
                if( sched->waiting_for_baton( i ) &&
                    ( state == mp_player_state::acting || state == mp_player_state::holding ) ) {
                    sched->yield_to( mp_sched::world, i );
                    moved = true;
                }
            }
            if( moved ) {
                // The world ran somebody, so it is accountable again: any stall
                // from here on is a real one and the watchdog should say so.
                sched->set_world_waiting( false );
            }
            if( !moved && !sched->wait_for_request( MP_POLL_MS ) ) {
                // Waiting turn: nobody owes the turn and nobody became ready.
                // The world does not tick and do_turn returns; the turn stays
                // open. The declared wait tells the watchdog that the world is
                // holding the baton on purpose, so a partner reading a menu
                // does not log as a stalled participant.
                sched->set_world_waiting( true );
                return false;
            }
            if( mp_session_end_requested() ) {
                // Somebody quit from inside their own turn: stop polling for a
                // barrier nobody is going to satisfy.
                sched->set_world_waiting( false );
                return turn_handler::cleanup_at_end();
            }
        }
        sched->set_world_waiting( false );
        mp_activate_world();
        world_turn_post();

        // Standing still is what being dropped means, and it has to be charged
        // before moves are handed back: they accumulate, and a dropped player
        // spends none of them.
        mp_forfeit_dropped_moves();

        // Moves come back after monmove, exactly as in single player. The baton
        // goes back to each player for this because the morale paths can draw.
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
        debug_menu::debug_capture::tick_if_active();
        return false;
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
            return true;
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
    debug_menu::debug_capture::tick_if_active();
    return false;
}
```

Тело потока игрока (создаётся в задаче 11) вызывает `avatar_turn_pre` и `player_act`
под своим контекстом; в ветке с планировщиком сам `do_turn` их не зовёт.

- [ ] **Step 5: Прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
timeout 120 tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-t10b/
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-t10-full/
```
Expected: PASS всё.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add src/mp/mp_curses_server.cpp src/mp/mp_server_input.cpp src/do_turn.cpp \
        tests/mp_server_io_test.cpp
git commit -m "feat(mp): the input wait is where the baton changes hands

get_input_event flushes the screen, releases the baton and only then blocks on
its own socket -- so a player reading a menu is a player holding nothing, and
the partner keeps playing. The orchestrator drives the players until the barrier
is satisfied, ticking the hurry countdown as it goes.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 11: Мультиклиентский сервер

**Files:**
- Modify: `src/mp/mp_server_main.h`, `src/mp/mp_server_main.cpp`, `src/main.cpp`

**Interfaces:**
- Consumes: `mp_listener` (`src/mp/mp_socket.h`), `mp_session`, `mp_sched`, `mp_sync_context_count`.
- Produces:

```cpp
// Accepts `players` clients, shakes hands with each, and builds a session and a
// context for every one. Returns 0 when the session is ready to play, non-zero
// on a failure the operator must see.
//
// The roster is fixed: the server waits for exactly this many clients, and once
// play starts nobody joins and nobody leaves. Joining mid-session is Phase 5.
int mp_server_main( int port, const std::string &world_name, int players );

// Assigns each session the slot whose avatar carries its hello name, then starts
// one thread per player. Called after the world has loaded, because the avatars
// do not exist until then. Returns false with `err` set when a name matches no
// avatar in the save.
bool mp_server_start_session( std::string &err );

// Stops every player thread and clears the sessions.
void mp_server_end_session();
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_session_test.cpp`:

```cpp
#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp/mp_context.h"
#include "mp/mp_server_main.h"
#include "mp/mp_session.h"

TEST_CASE( "sessions_are_matched_to_slots_by_name", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    // Deliberately in the opposite order to the slots: the match is by name,
    // not by who connected first, so that reconnecting in a different order
    // still puts everyone back in their own body.
    mp_sessions()[0]->name = "Bo";
    mp_sessions()[1]->name = "Ada";

    std::string err;
    REQUIRE( mp_server_start_session( err ) );
    CHECK( err.empty() );
    CHECK( mp_sessions()[0]->context_index == 1 );
    CHECK( mp_sessions()[1]->context_index == 0 );

    mp_server_end_session();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "an_unknown_name_is_refused_with_a_reason", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->player_at( 0 ).name = "Ada";

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->name = "Nobody";

    std::string err;
    CHECK_FALSE( mp_server_start_session( err ) );
    CHECK( err.find( "Nobody" ) != std::string::npos );

    mp_server_end_session();
}

#endif // MP_SERVER
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- 2>&1 | grep -m3 "mp_server_start_session"`
Expected: FAIL — `'mp_server_start_session' was not declared in this scope`.

- [ ] **Step 3: Добавить `--players` в разбор аргументов**

В `src/main.cpp`, рядом с `--server` (:431-441):

```cpp
            {
                "--players", "<count>",
                "How many clients the multiplayer server waits for before starting. "
                "The roster is fixed: nobody joins or leaves once play begins.",
                [&result]( int, const char **params ) -> int {
                    // A malformed or absent count leaves it at one, which is the
                    // single-client behaviour the earlier phases shipped.
                    result.players = std::max( 1, std::atoi( params[0] ) );
                    return 1;
                }
            },
```

и поле `int players = 1;` в `cli_opts` рядом с `server_port` (:274).

Вызов `mp_server_main( cli.server_port, cli.world, cli.players )`.

- [ ] **Step 4: Принимать N клиентов**

В `src/mp/mp_server_main.cpp` заменить единичный `accept` циклом:

```cpp
    mp_server_log( "waiting for " + std::to_string( players ) + " client" +
                   ( players == 1 ? "" : "s" ) );
    for( int joined = 0; joined < players; ++joined ) {
        // A server waits for its roster as long as it takes. Connection
        // timeouts and grace periods are Phase 5, together with the rest of
        // the session lifecycle.
        std::shared_ptr<mp_transport> transport = listener->accept( -1 );
        if( !transport ) {
            mp_server_log( "accepting a client failed" );
            return 1;
        }
        mp_server_log( "connection from " + transport->peer_description() + "; shaking hands" );

        auto session = std::make_unique<mp_session>();
        session->transport = transport;
        std::string reject_reason;
        if( !mp_server_handshake( *transport, *session, reject_reason ) ) {
            mp_server_log( "handshake rejected: " + reject_reason );
            return 1;
        }
        mp_sessions().push_back( std::move( session ) );
        mp_server_log( std::to_string( joined + 1 ) + " of " + std::to_string( players ) +
                       " connected" );
    }
```

- [ ] **Step 5: Раздать слоты и поднять потоки**

```cpp
bool mp_server_start_session( std::string &err )
{
    err.clear();
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        bool matched = false;
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            if( g->player_at( i ).name == session->name ) {
                session->context_index = i;
                matched = true;
                break;
            }
        }
        if( !matched ) {
            // Refusing with the name in the message is the difference between
            // "it did not work" and "you typed Bob and the save has Bo".
            err = "no character named \"" + session->name + "\" in this world";
            return false;
        }
    }
    return true;
}
```

Подъём потоков — в `mp_server_main` после загрузки мира:

```cpp
    // Nothing of a previous session's turn survives into this one: the barrier
    // flags, the open-turn flag and the end request are all about a session
    // that no longer exists.
    mp_turn_reset_all();
    g_sched = std::make_unique<mp_sched>( mp_sessions().size() );
    mp_set_active_sched( g_sched.get() );
    mp_sched_start_watchdog( *g_sched, 30000 );
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        g_player_threads.emplace_back( [i]() {
            while( true ) {
                mp_active_sched()->wait_for_turn( i );
                if( mp_active_sched()->stopping() ) {
                    return;
                }
                mp_assert_holds( i );
                mp_activate_player( i );
                avatar &ava = g->player_at( i );
                g->avatar_turn_pre( ava );
                if( g->player_act( ava ) ) {
                    // The game is over or this player quit. A thread body has
                    // nowhere to return that to, so it is left where the
                    // orchestrator picks it up and turns it into do_turn()'s
                    // `true` for main().
                    mp_request_session_end();
                }
                mp_active_sched()->release_to( i, mp_sched::world );
            }
        } );
    }
```

`mp_server_end_session()`:

```cpp
void mp_server_end_session()
{
    // Close the wires FIRST. shut_down() only wakes threads parked on the
    // baton; a player sitting in a menu is parked on recv() with no timeout,
    // and joining them before their socket goes away hangs the server forever
    // -- on the one path that matters, because ending a session while somebody
    // has a menu open is the normal case, not the exotic one. A closed
    // transport makes mp_server_wait_for_input return a timeout event, the
    // player_act loop sees mp_sched_stopping() and unwinds.
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session->transport ) {
            session->transport->close();
        }
    }
    if( g_sched ) {
        g_sched->shut_down();
    }
    for( std::thread &t : g_player_threads ) {
        t.join();
    }
    g_player_threads.clear();
    mp_set_active_sched( nullptr );
    g_sched.reset();
    mp_sessions().clear();
    mp_turn_reset_all();
}
```

- [ ] **Step 6: Обрыв любого клиента заканчивает сессию**

`mp_server_peer_connected()` сейчас смотрит на один транспорт. С фиксированным
составом уход любого клиента заканчивает партию, поэтому проверка становится
проходом по всем сессиям, а не по активной:

```cpp
bool mp_server_peer_connected()
{
    if( mp_sessions().empty() ) {
        return false;
    }
    for( const std::unique_ptr<mp_session> &s : mp_sessions() ) {
        if( !s || !s->transport || !s->transport->connected() ) {
            // The roster is fixed: one player leaving ends the session for
            // everybody. Keeping the others playing would mean parking the
            // absent context and handing their character to the AI, which is
            // Phase 5's job, not a half-version of it here.
            return false;
        }
    }
    return true;
}
```

В `src/mp/mp_curses_server.cpp` ветка `if( !mp_server_peer_connected() )` уже зовёт
`mp_server_request_exit()`; дописать в её лог, кто именно отвалился:

```cpp
            std::string who = "a client";
            for( const std::unique_ptr<mp_session> &s : mp_sessions() ) {
                if( !s || !s->transport || !s->transport->connected() ) {
                    who = "\"" + ( s ? s->name : std::string( "?" ) ) + "\"";
                    break;
                }
            }
            mp_server_log( who + " disconnected; ending the session" );
```

- [ ] **Step 7: Прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
timeout 120 tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-t11/
```
Expected: PASS.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp src/mp/mp_server_screen.cpp \
        src/mp/mp_curses_server.cpp src/main.cpp tests/mp_session_test.cpp
git commit -m "feat(mp): the server accepts a fixed roster of N clients

--players says how many to wait for; each gets a session and a context, and
slots are handed out by the character name in the hello rather than by who
connected first, so reconnecting in a different order still puts everyone back
in their own body. One thread per player, started once the world has loaded --
the avatars do not exist before that.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 12: Немодальный `debugmsg`

**Files:**
- Modify: `src/debug.cpp`, `src/debug.h`
- Create: тест в `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: `add_msg`, `DebugLog`, `mp_for_each_context`.
- Produces: изменение поведения существующего `debugmsg` под `MP_SERVER`; новых символов нет.

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_session_test.cpp`:

```cpp
TEST_CASE( "a_server_debugmsg_reaches_every_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // In test_mode debugmsg is already non-blocking, so "does it block" is not
    // observable from a test. What IS observable is the behaviour that replaces
    // the modal window: the error must land in the message log. A build that
    // still opened a window would put it nowhere.
    //
    // The log is process-wide (src/messages.h has no per-player split), so one
    // add_msg reaches everybody and a second would be a duplicate. Per-player
    // message logs are not part of this phase.
    Messages::clear_messages();
    mp_activate_world();
    debugmsg( "a server debugmsg must reach the players" );

    const std::vector<std::pair<std::string, std::string>> log = Messages::recent_messages( 5 );
    int hits = 0;
    for( const auto &entry : log ) {
        hits += entry.second.find( "must reach the players" ) != std::string::npos ? 1 : 0;
    }
    CHECK( hits == 1 );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

Немодальность проверяется не тестом, а воротами человека (задача 14): сервер,
уронивший `debugmsg` в фазе мира, повиснет для всех, и это видно за секунды.

- [ ] **Step 2: Прогнать — падает**

Run: `timeout 60 tests/mpserver-cata_test "a_server_debugmsg_reaches_every_player" --user-dir=/tmp/cata-mp-t12/`
Expected: FAIL — сообщение не попало ни в один лог: `debugmsg` в серверной сборке
пока ничего не рассылает.

- [ ] **Step 3: Сделать `debugmsg` немодальным в серверной сборке**

В `src/debug.cpp`, в точке, где поднимается интерактивное окно
(`query_popup` / `debug_error_prompt`), добавить ветку:

```cpp
#if defined(MP_SERVER)
    // A modal error is an instant deadlock on a server: it can fire during the
    // world phase, when no player context is active and there is physically
    // nowhere to draw a window. Log it, put it in the message log, carry on.
    //
    // One add_msg, not one per context: the message log is process-wide, so
    // every player already reads this one, and a pass over the contexts would
    // only produce N copies of the same line.
    DebugLog( D_ERROR, D_MAIN ) << "debugmsg: " << text;
    add_msg( m_bad, _( "Error: %s" ), text );
    return;
#endif
```

- [ ] **Step 4: Прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
timeout 60 tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-t12b/
```
Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/debug.cpp src/debug.h tests/mp_session_test.cpp
git commit -m "fix(mp): debugmsg never blocks on the server

A modal error window is an instant session deadlock: debugmsg can fire during
the world phase, when no player context is active and there is nowhere to draw.
The server logs it and broadcasts it to every player's message log instead.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 13: Сессионный харнесс, два игрока в разных меню, TSAN

**Files:**
- Create: `tests/mp_session_harness.h`, `tests/mp_session_harness.cpp`
- Modify: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: всё предыдущее.
- Produces:

```cpp
// A whole server session inside one process: N clients on loopback transports,
// scripted keystrokes, and access to every client's screen for comparison.
//
// The latency is not optional. Without it, code that only works at zero
// round-trip time gets written and passes, and the first real session finds it.
class mp_session_harness
{
    public:
        explicit mp_session_harness( size_t players, int latency_ms = 5 );
        ~mp_session_harness();

        // Sends one keystroke to the server as if player `slot` had pressed it.
        void press( size_t slot, int key );

        // Runs turns until `predicate` holds or `max_turns` pass. Returns false
        // on running out of turns, so a test fails with a message rather than
        // hanging.
        bool run_until( const std::function<bool()> &predicate, int max_turns = 100 );

        // The last screen the server sent this client, as the client applied it.
        const mp_screen_grid &screen( size_t slot ) const;

        // Drains every client's socket and applies the deltas, so screen() is
        // current. run_until() calls it after each turn.
        void pump_clients();
};

// True when two grids hold the same cells. mp_screen_grid has no comparison of
// its own -- it is a wire buffer, not a value type -- and the leak this whole
// phase guards against (one player seeing another's screen) is exactly a
// cell-for-cell equality question.
bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b );
```

- [ ] **Step 1: Написать харнесс**

`tests/mp_session_harness.h` — блок **Interfaces**, плюс приватные члены:

```cpp
    private:
        std::vector<std::shared_ptr<mp_transport>> client_wires_;
        // The client side is free functions over a grid and a state struct
        // (src/mp/mp_client_screen.h), not an object, so the harness holds the
        // two the same way the real client does.
        std::vector<mp_screen_grid> client_grids_;
        std::vector<mp_client_state> client_states_;
        std::unique_ptr<mp_sched> sched_;
        std::vector<std::thread> threads_;
```

`tests/mp_session_harness.cpp`:

```cpp
#if defined(MP_SERVER)

#include "mp_session_harness.h"

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_sched.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"

mp_session_harness::mp_session_harness( const size_t players, const int latency_ms )
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    // The first do_turn() of a session is the one-time "new_game" pass
    // (weather.on_game_start), which does not advance the calendar. Pin it here
    // so turn counting in the tests is deterministic no matter which tests ran
    // before this one in the same process.
    g->new_game = true;
    game_test_helper::reset_player_slots();
    mp_sessions_reset_for_test();

    g->player_at( 0 ).name = "P0";
    for( size_t i = 1; i < players; ++i ) {
        avatar &extra = g->add_player_slot();
        extra.set_body();
        extra.name = "P" + std::to_string( i );
    }
    mp_sync_context_count();
    // Fresh session: no leftover barrier state from a previous harness. The
    // input-wait flags, the turn-open flag and the end-of-session request belong
    // to a session, not to a turn, so mp_turn_reset() deliberately leaves them --
    // reset_all is the session/test boundary.
    mp_turn_reset_all();

    for( size_t i = 0; i < players; ++i ) {
        // The latency is not decoration. Without it, code that only works at
        // zero round-trip time passes here and fails on the first real wire.
        mp_loopback_pair wire = mp_make_loopback_pair( latency_ms );
        auto session = std::make_unique<mp_session>();
        session->transport = wire.a;
        session->context_index = i;
        session->name = g->player_at( i ).name;
        session->grid.resize( 80, 24 );
        mp_sessions().push_back( std::move( session ) );
        client_wires_.push_back( wire.b );
        client_grids_.emplace_back( 80, 24 );
        client_states_.emplace_back();
    }

    sched_ = std::make_unique<mp_sched>( players );
    mp_set_active_sched( sched_.get() );
    for( size_t i = 0; i < players; ++i ) {
        threads_.emplace_back( [i]() {
            while( true ) {
                mp_active_sched()->wait_for_turn( i );
                if( mp_active_sched()->stopping() ) {
                    return;
                }
                mp_assert_holds( i );
                mp_activate_player( i );
                avatar &ava = g->player_at( i );
                g->avatar_turn_pre( ava );
                g->player_act( ava );
                mp_active_sched()->release_to( i, mp_sched::world );
            }
        } );
    }
}

mp_session_harness::~mp_session_harness()
{
    // Close the wires FIRST. shut_down() only wakes threads parked on the baton;
    // a player sitting in a menu is parked on recv() with no timeout, and joining
    // them before their socket goes away hangs the harness -- and every Task 13
    // test ends with a player in a menu. A closed transport makes
    // mp_server_wait_for_input return a timeout event (the loopback recv polls
    // and sees the closed peer within a millisecond), and the player_act loop
    // sees mp_sched_stopping() and unwinds.
    for( std::shared_ptr<mp_transport> &wire : client_wires_ ) {
        wire->close();
    }
    sched_->shut_down();
    for( std::thread &t : threads_ ) {
        t.join();
    }
    mp_set_active_sched( nullptr );
    mp_sessions_reset_for_test();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

void mp_session_harness::press( const size_t slot, const int key )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    msg.event = input_event( key, input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_session_harness::pump_clients()
{
    for( size_t i = 0; i < client_wires_.size(); ++i ) {
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[i]->recv( 0 ) ) {
            mp_client_apply_message( *msg, client_grids_[i], client_states_[i] );
        }
    }
}

bool mp_session_harness::run_until( const std::function<bool()> &predicate, const int max_turns )
{
    for( int turn = 0; turn < max_turns; ++turn ) {
        g->do_turn();
        pump_clients();
        if( predicate() ) {
            return true;
        }
    }
    // Returning false rather than looping forever is the point: a barrier bug
    // shows up as a failed assertion with a message, not as a hung CI job.
    return false;
}

const mp_screen_grid &mp_session_harness::screen( const size_t slot ) const
{
    return client_grids_[slot];
}

bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b )
{
    if( a.width() != b.width() || a.height() != b.height() ) {
        return false;
    }
    for( int y = 0; y < a.height(); ++y ) {
        for( int x = 0; x < a.width(); ++x ) {
            if( a.at( x, y ) != b.at( x, y ) ) {
                return false;
            }
        }
    }
    return true;
}

#endif // MP_SERVER
```

- [ ] **Step 2: Написать падающие тесты — тяжёлые меню и незамороженный партнёр**

```cpp
// The six menus the design names as the heavy ones: each is a different UI
// path, and a global leaked by any of them shows up as one player seeing the
// other's screen. The keys are the DEFAULTMODE defaults.
TEST_CASE( "two_players_sit_in_different_heavy_menus", "[mp]" )
{
    struct menu_case {
        const char *name;
        int first;
        int second;
    };
    const std::vector<menu_case> menus = {
        { "inventory vs character sheet", 'i', '@' },
        { "crafting vs map",              '&', 'm' },
        { "vehicle vs construction",      '^', '*' },
    };

    for( const menu_case &c : menus ) {
        CAPTURE( c.name );
        mp_session_harness harness( 2 );

        // Neither key costs time, so both players hold nothing and both stay
        // live -- which is the whole point of the per-player context. Run until
        // both menus are open and rendered, then compare the screens.
        harness.press( 0, c.first );
        harness.press( 1, c.second );
        REQUIRE( harness.run_until( [&]() {
            return !mp_grids_equal( harness.screen( 0 ), harness.screen( 1 ) );
        }, 5 ) );

        // The main class of bug this design can have is a leaked global showing
        // one player the other's screen. Comparing the grids catches it.
        CHECK_FALSE( mp_grids_equal( harness.screen( 0 ), harness.screen( 1 ) ) );
    }
}

TEST_CASE( "a_player_in_a_menu_does_not_freeze_their_partner", "[mp]" )
{
    mp_session_harness harness( 2 );

    // Player 1 parks in a menu with moves left: they hold the barrier. The
    // world may not outrun a thinking player. Fresh avatars start with zero
    // moves, so the first do_turn is a full, closed turn in which neither
    // player_act reads any input; the calendar advances when the SECOND do_turn
    // opens a turn and player 1 parks in the menu. Every do_turn after that is
    // a waiting turn that returns without advancing -- ten of them leave the
    // calendar exactly one turn past turn_before.
    harness.press( 1, 'i' );
    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE( harness.run_until( []() {
        return false;
    }, 10 ) == false );
    CHECK( to_turn<int>( calendar::turn ) == turn_before + 1 );

    // Player 0 hurries them. The countdown must be running before we expire it:
    // the key reaches the server asynchronously through the wire, so wait for
    // the countdown to start, then advance it past expiry.
    harness.press( 0, '\'' );
    REQUIRE( harness.run_until( []() {
        return mp_hurry_remaining_ms() > 0;
    }, 5 ) );
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    // After the countdown the world moves on and the partner's menu is still
    // open -- untouched, just not waited on.
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 10 ) );
}
```

- [ ] **Step 3: Прогнать — не собирается, затем падает**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- 2>&1 | grep -m3 "mp_session_harness"`
Expected: FAIL.

- [ ] **Step 4: Добавить харнесс в сборку и довести тесты до зелёного**

`tests/*.cpp` подхватывается глобом (`Makefile:1130`), правок сборки не нужно.

Три отказа ожидаемы и лечатся так:

- **Тест виснет в `run_until`.** Игрок не отдал эстафету. Смотреть, дошёл ли его
  поток до `get_input_event`: `avatar_turn_pre` мог съесть весь ход на активности,
  и тогда `player_act` не вызывается вовсе. Ставить moves явно перед `press`.
- **Оба экрана совпали.** Либо ни один не перерисовался (клавиша не дошла — проверить
  `pump_clients()` после `do_turn()`), либо контекст протёк. Второе — настоящая
  находка: сверить `mp_active_session()` внутри `curses_drawwindow`.
- **`mp_assert_holds` сработал.** Кто-то побежал, не дождавшись `wait_for_turn`.
  Это то, ради чего ассерт и стоит; чинить место, а не ассерт.

- [ ] **Step 5: Прогнать под ThreadSanitizer — ворота CI**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 TSAN=1 BUILD_PREFIX=mpserver-tsan-
timeout 600 tests/mpserver-tsan-cata_test "[mp]" --user-dir=/tmp/cata-mp-t13-tsan/
```
Expected: PASS, ноль отчётов TSAN. Ровно один контекст исполняется в каждый момент,
поэтому TSAN обязан молчать; любое срабатывание — настоящая пропущенная передача
эстафеты, а не ложная тревога.

- [ ] **Step 6: Прогнать полный регресс обычной сборки**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-mp-t13-full/`
Expected: PASS.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add tests/mp_session_harness.h tests/mp_session_harness.cpp tests/mp_session_test.cpp
git commit -m "test(mp): a session harness, two players in two menus, under TSAN

The workhorse: a whole server session in one process, N clients on loopback with
latency baked in so nothing that depends on zero RTT can pass. The two tests
that matter are that two players in different menus see different screens, and
that one of them thinking does not stop the other once the countdown expires.
Run under ThreadSanitizer, which must stay silent because exactly one context
runs at a time.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 14: Замер свопа, правки родительского спека, ворота

**Files:**
- Create: `tests/mp_swap_benchmark_test.cpp`
- Modify: `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`, `doc/MULTIPLAYER.md`

**Interfaces:**
- Consumes: `mp_activate_player`.
- Produces: измеренное число для риска 5 Ф2 и обновлённый родительский спек.

- [ ] **Step 1: Написать бенчмарк**

```cpp
TEST_CASE( "swap_cost_is_measured", "[.][mp][benchmark]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // Phase 2 left this open as risk 5: the avatar swap is three moves of a
    // whole avatar, and the split turn puts it on the hot path -- at least 2N
    // swaps per tick. If this is expensive, the fix is an accessor instead of a
    // content swap, which the Phase 2 spec already costed.
    BENCHMARK( "mp_activate_player round trip" ) {
        mp_activate_player( 1 );
        mp_activate_player( 0 );
    };

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

- [ ] **Step 2: Снять число**

Run: `tests/cata_test "swap_cost_is_measured" --user-dir=/tmp/cata-mp-t14/ -\!`
Expected: результат в наносекундах. Записать его в спек Ф3a §6.4 и, если своп
дороже 50 мкс на пару, завести отдельную задачу на аксессор (§2 Ф2, «Отвергнуто»).

- [ ] **Step 3: Внести четыре правки в родительский спек**

`docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`:

- **§11** — строку Ф3 заменить двумя: Ф3a (планировщик, 4-5 недель, ворота — живая
  партия вдвоём по TCP) и Ф3b (вход в сессию, 2-3 недели);
- **§5.4** — абзац про отдельный поток ввода-вывода заменить на «поток игрока сам себе
  ввод-вывод, пока не держит эстафету; отдельного потока нет»;
- **§7.3** — пометить, что ленивый таймаут реализован в Ф3a вместе с барьером,
  а не в Ф4;
- **§8.1** — добавить, что модель хранения (`extra_players` внутри одного сейва
  против одного сейва на персонажа) пересматривается в Ф3b.

- [ ] **Step 4: Обновить `doc/MULTIPLAYER.md`**

Описать `--players`, фиксированный состав, подготовку второго аватара дебаг-тогглом,
клавишу «тороплю» и что делать в простое.

- [ ] **Step 5: Ворота человека**

Собрать серверную и клиентскую цели, подготовить мир с двумя персонажами,
запустить сервер и подключиться с двух машин:

```bash
make -j$(nproc) CCACHE=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
./mpserver-cataclysm-tiles --server 4321 --world "coop" --players 2
# на каждой машине:
./cataclysm-tiles --connect <host>:4321
```

Играть вечер. Записать в спек Ф3a §10 две величины, ради которых ворота и
существуют: **доля ходов, потраченных на движение**, и **сколько времени партнёр
реально проводит в ожидании**. По ним принимается решение по фазе планирования ходов.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add tests/mp_swap_benchmark_test.cpp docs/superpowers/specs/ doc/MULTIPLAYER.md
git commit -m "docs(mp): close phase 3a -- swap measured, parent spec updated

The swap benchmark closes phase 2's open risk 5. The parent spec gets the four
edits phase 3a's design called for: the phase split, the I/O thread that turned
out to be a mechanism without a job, the lazy timeout moving forward into 3a,
and the save model going up for review in 3b.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Заметки для реализующего

**1. Разрез `do_turn` делается в три коммита, а не в один.** Задачи 2-4 каждая
оставляют `do_turn()` поведенчески идентичным: сначала выносятся мировые половины,
потом персональные, и только потом функция пересобирается в оркестратор. Полный
регресс гоняется после каждой. Если после задачи 4 упало что-то, чего не падало
после задачи 3, виноват порядок в оркестраторе, а не вынос.

**2. `is_game_over()` и `QUIT_WATCH`.** `player_act` возвращает `true` на обоих;
оркестратор пробрасывает это в `main()` без изменений. При N > 1 первый же игрок,
вернувший `true`, заканчивает сессию — это фиксированный состав из Global Constraints,
а не упрощение.

**3. Почему `mp_turn` не в `player_context`.** Барьерная бухгалтерия — состояние
хода, а не дисплея игрока. Сбрасывается каждый открытый ход только то, что
принадлежит ходу: снятые по «тороплю» и сам отсчёт. Флаг ожидания ввода
(`g_waiting_for_input`) принадлежит бэкенду ввода и живёт, пока игрок реально
сидит на ожидании; в контексте он пережил бы сброс и стал бы вторым источником
правды, а сброс его в `mp_turn_reset()` превратил бы запаркованного игрока в
«действующего», которого `mp_tick_hurry()` не снимает, — вечная заморозка.

**4. Порядок в `mp_player_state_of` значим.** «Занят» проверяется раньше «есть moves»:
игрок с активностью и полными moves — *отходил*, а не *действует*. Обратный порядок
вернул бы блокировку, ради снятия которой правило и введено.

**5. `mp_action_is_free` — белый список, и это не оптимизация.** Классифицировать все
153 `action_id` дорого и опасно: ошибка в сторону «бесплатно» даёт действие вне хода.
Умолчание `return false` делает ошибку безвредной. Добавлять в список по одной строке
по мере того, как что-то раздражает на воротах.

**6. Фолбэк-сетка в `mp_server_grid()` нужна.** До подключения клиента (старт, тесты)
игра всё равно спрашивает размер, чтобы создать `stdscr`. Отдавать там `nullptr`
означало бы разбросать проверки по бэкенду.

**7. Тесты `[coop]` против `[mp]`.** `[mp]` — механика мультиплеерного слоя
(контекст, эстафета, барьер, протокол). `[coop]` — поведение игры при нескольких
аватарах (разрез хода, слоты, якорь, тезер). Разделение позволяет гонять разрез
в обычной сборке, не таща туда сеть.

**8. Что делать, если TSAN сработал.** Не глушить. Ровно один участник исполняется
в каждый момент, поэтому отчёт означает настоящую пропущенную передачу эстафеты:
где-то код побежал, не дождавшись `wait_for_turn`, либо продолжил после
`release_to`. Искать по стеку из отчёта, а не по подозрению.

---

## Значения, разрешённые при планировании

| Что | Значение |
|---|---|
| Тестовый харнесс | Catch2, `#include "cata_catch.h"`; теги `[mp]` и `[coop]`; `tests/cata_test` (обычная) / `tests/mpserver-cata_test` (серверная) |
| Сброс слотов в тестах | `game_test_helper::reset_player_slots()` (`tests/game_test_helper.h`); `g->add_player_slot()` — публичный |
| RAII теста | `restore_active_state` (`tests/game_test_helper.h`); `restore_on_out_of_scope` (`cata_scope_helpers.h:32`) |
| Подготовка мира в тесте | `clear_avatar()`, `build_test_map( ter_id( "t_pavement" ) )` (`tests/map_helpers.h`); `g->ensure_gamemode_for_test()` |
| Контекст в тесте | `mp_ensure_player_context()` — тестовый харнесс не зовёт `game::setup()` |
| Транспорт в тестах | `mp_make_loopback_pair( latency_ms )` (`src/mp/mp_loopback_transport.h`) |
| Экран клиента | `mp_client_screen` (`src/mp/mp_client_screen.h`), Ф1 |
| Сетка сервера | `mp_screen_grid` (`src/mp/mp_cell.h`); бэклог `MP_SCREEN_BACKLOG_BYTES` (`mp_server_screen.h:20`) |
| Границы терминала | `EVEN_MINIMUM_TERM_WIDTH/HEIGHT` (`game_constants.h`), `MP_MAX_TERMINAL_WIDTH/HEIGHT` (`mp_server_input.h:19-20`) |
| Протокол | `mp_hello` с полем `name` (`src/mp/mp_protocol.h:37-43`); `MP_PROTOCOL_VERSION` |
| Якорь и тезер | `mp_bubble_anchor()`, `mp_tether_check()` (`src/mp/mp_tether.h`) |
| Слоты | `game::player_slot_count()`, `player_at()`, `active_slot()`, `set_active_slot()`, `add_player_slot()`, `remove_extra_player_slots()` (`src/game.h:449-466`) |
| Контексты | `game::contexts` — `std::deque<player_context>` (`src/game.h:448`), адреса стабильны |
| Стек UI | `ui_manager::set_active_stack()` / `get_active_stack()` (`src/ui_manager.h:315-317`); `world_stack` (`ui_manager.cpp:34`) |
| Загрузка мира | `main_menu::queued_world_to_load` (`src/main.cpp:692`) — автозагрузка мимо меню (`main_menu.cpp:646-661`) |
| Сборка: источники | `src/mp/*.cpp` подхватывается глобом под `MULTIPLAYER` (`Makefile:1119`); файлы для всех билдов добавляются явно (`Makefile:1116`) |
| Сборка: TSAN | `make ... TSAN=1 BUILD_PREFIX=<prefix>-` |
| Режим теста | `extern bool test_mode` (`cached_options.h:44`) |
