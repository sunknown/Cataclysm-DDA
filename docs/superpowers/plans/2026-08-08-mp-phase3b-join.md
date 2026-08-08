# Фаза 3b: вход в сессию — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** двое подключаются к серверу с голым миром, каждый у себя создаёт персонажа или продолжает за своего, и партия начинается — без дебаг-меню и без подготовки мира вручную.

**Architecture:** `game::load( save_t )` и `game::save()` режутся на мировую часть (один раз) и персональную (по разу на слот, под контекстом своего игрока). Массив `extra_players` умирает, персонажи становятся отдельными файлами мира — и кооп-мир становится буквально обычным миром с несколькими персонажами. Каждый клиент получает `mp_join_menu` в своём контексте; клиент держит у себя карточку персонажа как опознавательный жетон, сервер остаётся единственным источником правды. Персональная часть `game::start_game()` выносится в `place_new_character`, размещение идёт в два прохода вокруг опорного персонажа.

**Tech Stack:** C++17, существующие `player_context` (Ф2), `mp_sched`/`mp_session`/сессионный харнесс (Ф3a), `avatar::create`, `save_t`, `scenario`, Catch2, Makefile + CMake.

## Global Constraints

- **Спеки:** `docs/superpowers/specs/2026-08-08-mp-phase3b-join-design.md` (основной), `2026-08-08-mp-phase3a-scheduler-design.md` (предшественник), `2026-08-04-cdda-p2p-coop-design.md` (родитель, §8.1, §8.2, §11).
- **Ф3a должна быть выполнена.** Этот план опирается на `mp_sched`, `mp_session`, `mp_sync_context_count()`, `mp_activate_player( idx )`, разрез `do_turn` и сессионный харнесс. Без них задачи 6-8 не собираются.
- **Скоуп Ф3b.** Вход и выход посреди партии, возврат после обрыва, наблюдатель после смерти, конфиг-файл сервера — **Ф5**. Создание и выбор мира, моды, спец-режимы — не вводятся, мир задаётся `--world`. Кооп-сценариев ровно один.
- **Сервер авторитетен всегда.** Карточка клиента — жетон, не источник правды: несовпадение вырождается в предложение создать нового персонажа, никогда в ошибку и никогда в создание персонажа по данным клиента.
- **`game::unserialize` в тестах не вызывать.** Он заменяет разделяемое состояние тест-игры (карты, календарь, мир) и течёт в последующие тесты; однажды это уже дало SIGSEGV в постороннем крафт-тесте. Формат проверяется чтением ключей в свежие `avatar`; настоящий круговой рейс мира с N персонажами — ворота человека (задача 9), а не автотест.
- **Hotseat к этой фазе уже удалён** (Ф3a, задача 1), и его тесты почищены. Ничего в этом плане не должно на них опираться. Единственный след, который Ф3b обязана закрыть, — формат `extra_players` в сейвах: он перестаёт и писаться, и читаться (задачи 1 и 2). Старые сейвы этого формата теряют дополнительных персонажей, это принято.
- **`uistate.json` — файл на мир, не на персонажа** (`PATH_INFO::world_base_save_path() / "uistate.json"`). Писать его один раз в мировом проходе; N записей означали бы «последний слот победил».
- **Порядок создания слотов.** `game::setup()` очищает `player_slots` (`game.cpp:716-725`), поэтому N пустых слотов создаются **после** `setup()` и `load_world_state()`, никогда раньше.
- **Сборка тестов (обычная):** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`. Запуск: `tests/cata_test "[mp],[coop]" --user-dir=/tmp/cata-mp-test/`.
- **Сборка тестов (серверная):** `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`.
- **Полный набор — только на свежей user-dir.** Переиспользованная копит `past_games_info` и роняет `stats_tracker`.
- **Регресс:** весь набор обычной сборки зелёный после каждой задачи.
- **Стиль:** `make astyle`, комментарии по-английски, лимит 100 колонок.
- **Коммиты:** после каждой задачи; `feat(mp):`, `refactor(mp):`, `test(mp):`, `docs:`.
- **Заглушки запрещены.** Каждый символ появляется с окончательным поведением.
- **Значения, разрешённые при планировании** — в конце документа.

---

## Обзор файлов

**Создаются:**

| Файл | Ответственность |
|---|---|
| `src/mp/mp_join_menu.h`, `.cpp` | Меню входа: создание персонажа, продолжение, настройки, выход |
| `src/mp/mp_character_card.h`, `.cpp` | Карточка персонажа: структура, чтение и запись в клиентской userdir |
| `src/mp/mp_placement.h`, `.cpp` | `place_new_character`, спираль поиска свободного тайла, два прохода размещения |
| `data/json/scenarios_coop.json` | Кооперативный сценарий отдельным файлом |
| `tests/mp_save_split_test.cpp` | Формат: `extra_players` не пишется, персонаж сериализуется отдельно |
| `tests/mp_placement_test.cpp` | Размещение: тезер, проходимость, столкновения, пустой мир |
| `tests/mp_character_card_test.cpp` | Карточка: круговой рейс, несовпадение, отсутствие |
| `tests/mp_join_test.cpp` | Меню входа на сессионном харнессе (серверная сборка) |
| `docs/superpowers/reports/2026-08-08-multi-character-world-audit.md` | Что именно ломается при нескольких персонажах в мире (задача 9) |

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/game_io.cpp` | `game::load( save_t )` → `load_world_state()` + `load_character()`; `game::save()` → `save_world_data()` + `save_character_data()` + оркестратор по слотам |
| `src/game.h` | Объявления четырёх новых членов плюс `place_new_character` |
| `src/game.cpp` | Персональная часть `start_game()` выносится; `start_game()` остаётся путём пустого мира |
| `src/savegame.cpp` | Запись `extra_players` (:151-154) удаляется; чтение (:326-329) заменяется строкой в лог |
| `src/mp/mp_protocol.h`, `.cpp` | `mp_hello.save_id`; новое сообщение `character_card` |
| `src/mp/mp_server_main.cpp` | Загрузка мира, N пустых слотов, меню входа в потоках, два прохода размещения |
| `src/mp/mp_client_main.cpp` | Чтение карточки перед `hello`, запись присланной карточки |
| `src/newcharacter.cpp` | Вкладка `CHARCREATOR_SCENARIO` (:2753) скрыта в серверной сборке; сценарий форсируется |
| `tests/mp_session_harness.h`, `.cpp` | Режим «сессия начинается с меню входа» |
| `Makefile`, `CMakeLists.txt` | `mp_placement.cpp` и `mp_character_card.cpp` в обычную сборку |

**Почему так.** Разрезы загрузки и сохранения остаются в `game_io.cpp` — это игровая логика, а не мультиплеерный слой, и одиночная игра ходит по ним. `mp_placement` вынесен отдельно, потому что нужен и Ф5 (новый персонаж после смерти). `mp_character_card` собирается всегда: клиентская сборка — обычная, и карточку читает она.

---

### Task 1: Разрез сохранения

**Files:**
- Modify: `src/game_io.cpp` (`game::save()`, ~строка 737), `src/game.h`, `src/savegame.cpp:151-154`
- Create: `tests/mp_save_split_test.cpp`

**Interfaces:**
- Consumes: `save_player_data()`, `save_achievements()`, `save_factions_missions_npcs()`, `save_external_options_record()`, `save_dimension_data()`, `save_maps()` (все уже есть в `game_io.cpp`), `mp_activate_player` (Ф2), `mp_sync_context_count` (Ф3a).
- Produces:

```cpp
        /**
         * The part of a save that belongs to the world: achievements, factions,
         * missions, NPCs, the external options record, dimension data, maps, and
         * the world's uistate. Written once per save, whoever is active.
         */
        bool save_world_data();
        /**
         * The part of a save that belongs to one character: their save file,
         * auto-pickup rules, auto-notes, safemode rules and zones, plus the
         * playtime file and registering them in the world's save list.
         *
         * Writes whichever slot is active, so the caller activates the context
         * first. Deliberately does not write uistate.json: that is one file per
         * world, and writing it once per character would mean the last slot won.
         */
        bool save_character_data();
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_save_split_test.cpp`:

```cpp
#include "cata_catch.h"

#include <sstream>
#include <string>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "json_loader.h"
#include "mp/mp_context.h"

// The save format is the load-bearing change of this phase: a co-op world stops
// being "one save with extra avatars nested inside" and becomes an ordinary
// world with several character saves. These tests pin the format, not the file
// system -- game::unserialize must never be fed a document from a test (see
// Global Constraints).

TEST_CASE( "the_save_document_no_longer_nests_extra_players", "[coop]" )
{
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    mp_sync_context_count();
    REQUIRE( g->player_slot_count() == 2 );

    std::ostringstream out;
    g->serialize_json( out );
    const std::string doc = out.str();

    // Slot 0 still writes under the legacy "player" key, so a single-player
    // build opens the file and sees one character.
    CHECK( doc.find( "\"player\"" ) != std::string::npos );
    // The nested array is gone: with it, "load one character" would still drag
    // somebody else's avatar along, and the load split below cannot be honest.
    CHECK( doc.find( "extra_players" ) == std::string::npos );
    CHECK( doc.find( "SlotOneProbe" ) == std::string::npos );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "each_slot_serializes_as_a_standalone_character", "[coop]" )
{
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    const std::string slot0 = g->player_at( 0 ).name;
    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    mp_sync_context_count();

    // Serializing under each context is what the per-slot save loop does: the
    // document written while slot i is active carries slot i and nobody else.
    std::string docs[2];
    for( size_t i = 0; i < 2; ++i ) {
        mp_activate_player( i );
        std::ostringstream out;
        g->serialize_json( out );
        docs[i] = out.str();
    }
    mp_activate_player( 0 );

    avatar reloaded[2];
    for( size_t i = 0; i < 2; ++i ) {
        JsonObject root = json_loader::from_string( docs[i] );
        root.read( "player", reloaded[i] );
        root.allow_omitted_members();
    }
    CHECK( reloaded[0].name == slot0 );
    CHECK( reloaded[1].name == "SlotOneProbe" );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

- [ ] **Step 2: Прогнать — падает**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "the_save_document_no_longer_nests_extra_players" --user-dir=/tmp/cata-3b-t1/`
Expected: FAIL — `extra_players` найден в документе: запись ещё на месте.

- [ ] **Step 3: Удалить запись `extra_players` и тест, который её требовал**

В `src/savegame.cpp:151-154` удалить блок:

```cpp
    if( g->player_slot_count() > 1 ) {
        json.member( "extra_players" );
        ...
    }
```

Затем убрать тест, который эту запись требовал:

```bash
grep -rn "extra_players_survive_a_save_round_trip\|single_player_build_sees_only_slot_zero" tests/
```

`extra_players_survive_a_save_round_trip` проверял, что массив пишется, — он прямо
противоречит этой задаче и удаляется; его роль занимают два теста шага 1.
`single_player_build_sees_only_slot_zero` проверял, что одиночная сборка видит слот 0
несмотря на массив, — теперь массива нет вовсе, и утверждение стало пустым: тоже
удаляется. Если Ф3a их уже вычистила, оба `grep` молчат и шаг сводится к правке
`savegame.cpp`.

- [ ] **Step 4: Объявить и вынести две функции**

Объявления из блока **Interfaces** — в `src/game.h` рядом с `bool save();`.

`game::save_world_data()` берёт из тела `game::save()` (`game_io.cpp:754-770`):
`save_achievements()`, `save_factions_missions_npcs()`, `save_external_options_record()`,
`save_dimension_data()`, `save_maps()` и запись `uistate.json`.

`game::save_character_data()` берёт: `save_player_data()`,
`get_auto_pickup().save_character()`, `get_auto_notes_settings().save( true )`,
`get_safemode().save_character()`, `zone_manager::get_manager().save_zones()`,
`world_generator->active_world->add_save( save_t::from_save_id( u.get_save_id() ) )`
и запись `.pt`-файла.

- [ ] **Step 5: `game::save()` становится оркестратором**

```cpp
bool game::save()
{
    if( save_is_dirty ) {
        popup( _( "The game is in an unsupported state after using debug tools and cannot be saved." ) );
        return false;
    }
    std::chrono::seconds time_since_load =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - time_of_last_load );
    std::chrono::seconds total_time_played = time_played_at_last_load + time_since_load;
    events().send<event_type::game_save>( time_since_load, total_time_played );

    try {
        if( !save_world_data() ) {
            debugmsg( "game not saved" );
            return false;
        }
        // Every character gets its own file. The context swap is what makes this
        // work: save_character_data writes `u`, and `u` is whichever slot the
        // swap put there.
        const size_t restore = active_slot();
        for( size_t i = 0; i < player_slot_count(); ++i ) {
            mp_activate_player( i );
#if defined(MP_SERVER)
            mp_server_log( "saving \"" + u.name + "\"" );
#endif
            if( !save_character_data() ) {
                mp_activate_player( restore );
                debugmsg( "game not saved" );
                return false;
            }
        }
        mp_activate_player( restore );

        world_generator->last_world_name = world_generator->active_world->world_name;
        world_generator->last_character_name = player_at( 0 ).name;
        world_generator->save_last_world_info();
        return true;
    } catch( std::ios::failure & ) {
        popup( _( "Failed to save game data" ) );
        return false;
    }
}
```

`.pt`-файл и `add_save` переезжают в `save_character_data()`, поэтому каждый персонаж
регистрируется в `world_saves` сам.

- [ ] **Step 6: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-3b-t1b/
tests/cata_test --user-dir=/tmp/cata-3b-t1-full/
```
Expected: PASS оба.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/game.h src/game_io.cpp src/savegame.cpp tests/mp_save_split_test.cpp
git commit -m "refactor(mp): one save file per character instead of a nested array

game::save splits into a world pass and a per-character pass, and the loop over
slots uses the Phase 2 context swap for what it was built for: save_character_data
writes u, and u is whichever slot is active. The extra_players array stops being
written, which is what makes a co-op world literally an ordinary world with
several characters -- and what lets the load split that follows be honest.
uistate.json stays in the world pass: it is one file per world, so writing it per
character would mean the last slot won.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Разрез загрузки

**Files:**
- Modify: `src/game_io.cpp:325-510`, `src/game.h`, `src/savegame.cpp:326-329`
- Modify: `tests/mp_save_split_test.cpp`

**Interfaces:**
- Consumes: `load_master()`, `load_dimension_data()`, `init_autosave()` (`game_io.cpp`), `mp_activate_player`.
- Produces:

```cpp
        /**
         * The part of a load that belongs to the world: the master save
         * (factions), dimension data, the autosave timer, the gamemode, the
         * world's uistate and the calendar caches. Runs once, before any
         * character exists.
         */
        bool load_world_state();
        /**
         * Loads one character save into slot `slot`: replaces that slot's avatar,
         * then its map memory, diary, memorial and per-character settings.
         *
         * Must run with `slot` active -- unserialize() reads `u`, and `u` is
         * whichever slot the context swap put there. Asserts it.
         */
        bool load_character( const save_t &name, size_t slot );
```

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_save_split_test.cpp`:

```cpp
TEST_CASE( "loading_a_character_needs_that_slot_active", "[coop]" )
{
    // load_character's contract, stated as a test because violating it is
    // silent: unserialize reads `u`, so loading into the wrong slot leaves two
    // players sharing one character rather than failing.
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();
    avatar &second = g->add_player_slot();
    second.set_body();
    mp_sync_context_count();

    mp_activate_player( 1 );
    CHECK( g->active_slot() == 1 );
    CHECK( &g->player_at( 1 ) == &get_avatar() );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "load_world_state\|load_character"`
Expected: FAIL — `'class game' has no member named 'load_world_state'`.

- [ ] **Step 3: Удалить чтение `extra_players`**

В `src/savegame.cpp:326-329` удалить блок:

```cpp
    if( data.has_member( "extra_players" ) ) {
        for( JsonValue jv : data.get_array( "extra_players" ) ) {
            jv.read( g->add_player_slot() );
        }
    }
```

Заменять его на предупреждение не нужно: формат писал только дебаг-тоггл, релизных
сейвов в нём нет, и решение — просто игнорировать. Одна строка всё же остаётся, чтобы
старый сейв не породил жалобу на непрочитанный член в дебаг-сборке:

```cpp
    // Written by the pre-3b format, where a co-op party lived inside slot zero's
    // file. Characters are their own saves now, so this key is not read; named
    // here only so the reader does not report it as unvisited.
    data.allow_omitted_members();
```

- [ ] **Step 4: Вынести `load_world_state` и `load_character`**

`load_world_state()` берёт из `game::load( save_t )` записи `_( "Master save" )`
и `_( "Dimension data" )`, а из `_( "Finalizing" )` — `init_autosave()`, `gamemode`,
чтение `uistate.json`, установку кэшей календаря (`set_eternal_season`,
`set_season_length`, `set_eternal_night`, `set_eternal_day`), `reload_npcs()`,
`validate_npc_followers()`, `validate_mounted_npcs()`, `validate_camps()`,
`validate_linked_vehicles()`.

`load_character( name, slot )` берёт: `_( "Character save" )` (с `u = avatar()`
и `unserialize`), `_( "Map memory" )`, `_( "Diary" )`, `_( "Memorial" )`, а из
`_( "Finalizing" )` — `get_auto_pickup().load_character()`,
`get_auto_notes_settings().load( true )`, `get_safemode().load_character()`,
`zone_manager::load_zones()`, `u.recalc_sight_limits()`, назначение ID,
`update_map( u )`, `set_owner` по инвентарю, `u.reset()`,
`recalculate_enchantment_cache()`, `activate_passive`,
`effect_on_conditions::load_existing_character( u )`.

Первой строкой:

```cpp
bool game::load_character( const save_t &name, const size_t slot )
{
    // unserialize() reads `u`; `u` is whichever slot the context swap put there.
    // Loading into the wrong slot is silent and produces two players sharing one
    // character, so it is asserted rather than documented.
    cata_assert( active_slot() == slot );
```

`game::load( save_t )` остаётся для одиночной игры и становится
`load_world_state()` плюс `load_character( name, 0 )` — сигнатура и вызывающие
(`main_menu::load_game`, `game_io.cpp:315`) не меняются.

- [ ] **Step 5: Прогнать тесты и регресс**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-3b-t2/
tests/cata_test --user-dir=/tmp/cata-3b-t2-full/
```
Expected: PASS оба. Одиночная загрузка ходит по новому разрезу, значит регресс её и проверяет.

- [ ] **Step 6: Ручная проверка одиночной загрузки**

Автотест не грузит настоящий мир (Global Constraints), поэтому здесь один ручной прогон:

```bash
make -j$(nproc) CCACHE=1 TILES=0
./cataclysm --world "Falconer"
```
Expected: существующий одиночный мир открывается и играется как раньше.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/game.h src/game_io.cpp src/savegame.cpp tests/mp_save_split_test.cpp
git commit -m "refactor(mp): split loading into a world pass and a per-character pass

game::load mixed three levels: the world (factions, dimension data, autosave,
gamemode), one character (their save, map memory, diary, memorial) and
finalization. Split, the per-character half can run once per slot under that
slot's context -- with an assert, because loading into the wrong slot is silent
and leaves two players sharing one character. The legacy nested array stops
being read.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Размещение персонажей

**Files:**
- Create: `src/mp/mp_placement.h`, `.cpp`, `tests/mp_placement_test.cpp`
- Modify: `src/game.cpp:808-900` (`game::start_game()`), `src/game.h`, `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `start_location::place_player`, `find_player_initial_location`, `prepare_map` (`src/start_location.h`), `avatar::add_profession_items`, `migrate_items_to_storage`, `assign_npc_id`, `map::passable`, `creatures::creature_at`, `MP_TETHER_SOFT_LIMIT` (`src/mp/mp_tether.h`).
- Produces:

```cpp
// Establishes a freshly created character in a world that already exists.
//
// With `near` unset this runs the scenario's own start location and the
// world-building path -- only ever correct for the first character in an empty
// world, because game::start_game()'s world steps (start_calendar, seed,
// init_region_layout, and MAPBUFFER.clear on its retry path) would reset the
// calendar and wipe the map of a loaded world.
//
// With `near` set every world step is skipped and the character is placed on a
// free tile next to that point.
//
// Also what Phase 5 needs for a replacement character after a death.
bool place_new_character( avatar &ava, std::optional<tripoint_abs_ms> near );

// A free tile as close to `target` as possible: passable, not occupied by any
// creature, and within the tether's soft limit. Returns nullopt when the search
// exhausts that radius, which the caller reports rather than papering over.
//
// The occupancy check must catch characters placed moments ago by the same
// pass: placement happens one player at a time, and a just-placed avatar is on
// its tile before it appears in any creature cache.
std::optional<tripoint_abs_ms> mp_find_free_tile_near( const tripoint_abs_ms &target );

// The two-pass placement of a whole roster, run once after every player has
// chosen (never before: slots are handed out by connection order, so "slot 0"
// is not necessarily anybody's position).
//
// Pass 1 gives every continuing character the position from its own save; the
// first of them becomes the anchor. Pass 2 places every new character next to
// the anchor -- or, when there is no anchor because the world was empty, sends
// the first new character down the full start_game() path and makes it the
// anchor.
//
// `is_new[i]` says whether slot i was created rather than continued. Fills
// `moved` with the slots that had to be pulled to the anchor, so the caller can
// tell those players before the session starts.
bool mp_place_roster( const std::vector<bool> &is_new, std::vector<size_t> &moved );
```

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_placement_test.cpp`:

```cpp
#include "cata_catch.h"

#include <optional>
#include <vector>

#include "avatar.h"
#include "coordinates.h"
#include "game.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "mp/mp_context.h"
#include "mp/mp_placement.h"
#include "mp/mp_tether.h"

TEST_CASE( "a_free_tile_near_a_target_is_passable_and_unoccupied", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &u = g->player_at( 0 );
    const tripoint_abs_ms target = u.pos_abs();

    const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( target );
    REQUIRE( spot.has_value() );

    map &here = get_map();
    CHECK( here.passable( here.get_bub( *spot ) ) );
    // Not the target itself: somebody is standing there, and two avatars on one
    // tile is the failure this function exists to prevent.
    CHECK( *spot != target );
    CHECK( rl_dist( *spot, target ) <= MP_TETHER_SOFT_LIMIT );
}

TEST_CASE( "two_placements_in_a_row_do_not_collide", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    avatar &anchor = g->player_at( 0 );
    const tripoint_abs_ms target = anchor.pos_abs();

    avatar &a = g->add_player_slot();
    a.set_body();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    REQUIRE( place_new_character( a, target ) );
    REQUIRE( place_new_character( b, target ) );

    // The second placement runs while the first avatar is already standing on
    // its tile but is not yet in any creature cache -- the case a naive
    // occupancy check misses.
    CHECK( a.pos_abs() != b.pos_abs() );
    CHECK( a.pos_abs() != target );
    CHECK( b.pos_abs() != target );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "a_new_character_next_to_an_anchor_gets_its_profession_items", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    const tripoint_abs_ms target = g->player_at( 0 ).pos_abs();
    avatar &fresh = g->add_player_slot();
    fresh.set_body();
    mp_sync_context_count();
    REQUIRE( fresh.create( character_type::RANDOM ) );

    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE( place_new_character( fresh, target ) );

    // The personal half of start_game ran...
    CHECK( fresh.getID().is_valid() );
    CHECK_FALSE( fresh.inv_dump().empty() );
    // ...and none of its world steps did. start_calendar() here would reset the
    // clock of a world that has been played for weeks.
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_anchor_is_the_first_continuing_slot_not_slot_zero", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    // Slot 0 is a brand new character; slot 1 continued from a save. Slots are
    // handed out by connection order, so this ordering is ordinary, and
    // anchoring on slot 0 would anchor on a character with no position yet.
    avatar &continuing = g->add_player_slot();
    continuing.set_body();
    mp_sync_context_count();
    const tripoint_abs_ms saved_pos = g->player_at( 0 ).pos_abs() + tripoint::east * 5;
    continuing.setpos( get_map(), get_map().get_bub( saved_pos ) );

    std::vector<bool> is_new = { true, false };
    std::vector<size_t> moved;
    REQUIRE( mp_place_roster( is_new, moved ) );

    // The continuing character stayed put; the new one came to it.
    CHECK( continuing.pos_abs() == saved_pos );
    CHECK( rl_dist( g->player_at( 0 ).pos_abs(), saved_pos ) <= MP_TETHER_SOFT_LIMIT );
    CHECK( moved.empty() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "scattered_continuations_are_pulled_to_the_anchor_and_reported", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    avatar &far_away = g->add_player_slot();
    far_away.set_body();
    mp_sync_context_count();
    const tripoint_abs_ms anchor_pos = g->player_at( 0 ).pos_abs();
    // Far outside the bubble: played solo on the other side of the map.
    far_away.setpos( get_map(),
                     get_map().get_bub( anchor_pos + tripoint::east * 500 ) );

    std::vector<bool> is_new = { false, false };
    std::vector<size_t> moved;
    REQUIRE( mp_place_roster( is_new, moved ) );

    CHECK( rl_dist( far_away.pos_abs(), anchor_pos ) <= MP_TETHER_SOFT_LIMIT );
    // Reported, not silent: it is that player's save, and they must be told
    // before the session starts rather than discover it later.
    CHECK( moved == std::vector<size_t> { 1 } );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_placement\|place_new_character"`
Expected: FAIL — `mp/mp_placement.h: No such file or directory`.

- [ ] **Step 3: Создать `src/mp/mp_placement.h`**

Блок **Interfaces** целиком, guard `CATA_SRC_MP_MP_PLACEMENT_H`, с `#include <optional>`,
`<vector>`, `"coordinates.h"`.

- [ ] **Step 4: Реализовать спираль**

```cpp
std::optional<tripoint_abs_ms> mp_find_free_tile_near( const tripoint_abs_ms &target )
{
    map &here = get_map();
    // Radius zero first: the target itself is a legal answer when nobody is on
    // it (the anchor's own placement, or a shelter floor with room to spare).
    for( int radius = 0; radius <= MP_TETHER_SOFT_LIMIT; ++radius ) {
        for( const tripoint_abs_ms &candidate : mp_ring_around( target, radius ) ) {
            const tripoint_bub_ms bub = here.get_bub( candidate );
            if( !here.inbounds( bub ) || !here.passable( bub ) ) {
                continue;
            }
            // get_creature_at covers monsters, NPCs and avatars, which is what
            // makes a just-placed player visible to the next placement in the
            // same pass.
            if( get_creature_tracker().creature_at( bub ) != nullptr ) {
                continue;
            }
            return candidate;
        }
    }
    // Exhausted the tether's soft limit. Reported to the caller, who turns it
    // into a refusal in the join menu -- placing further out would just make
    // the tether complain on the first turn.
    return std::nullopt;
}
```

`mp_ring_around( centre, radius )` — вспомогательная функция в том же файле: тайлы
ровно на расстоянии `radius` по чебышёвской метрике на z-уровне центра, `radius == 0`
даёт сам центр.

- [ ] **Step 5: Вынести персональную часть `start_game()`**

```cpp
bool place_new_character( avatar &ava, const std::optional<tripoint_abs_ms> near )
{
    // The personal half of game::start_game(), in the order start_game runs it.
    ava.setID( assign_npc_id() );
    ava.add_profession_items();
    ava.migrate_items_to_storage( true );

    if( !near ) {
        // Empty world: this character builds it. Everything world-scoped lives
        // in game::start_game() and is reached only through here.
        return g->start_game_world_steps( ava );
    }

    const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( *near );
    if( !spot ) {
        return false;
    }
    map &here = get_map();
    ava.setpos( here, here.get_bub( *spot ) );
    const tripoint_abs_ms pos = ava.pos_abs();
    ava.visit_items( [&pos]( item * it, item * ) {
        it->preserve_location( pos );
        return VisitResponse::NEXT;
    } );
    return true;
}
```

В `src/game.cpp` из `start_game()` выделяется `bool game::start_game_world_steps( avatar & )`
со всем мировым: `seed`, `new_game`, `start_calendar()`, `weather.nextweather`,
`safe_mode`, `get_safemode().load_global()`, `init_autosave()`, `dimension_prefix`,
`overmap_buffer.init_region_layout()`, `load_master()`, поиск стартовой локации
с ретраями, `set_parameters`, `prepare_map`, map extras, `load_map`, `place_player`,
`invalidate_map_cache`/`build_map_cache`. Персональные три строки из него удаляются —
их теперь делает `place_new_character` до вызова.

`game::start_game()` остаётся как есть для одиночной игры: персональные три строки
плюс `start_game_world_steps( u )`.

- [ ] **Step 6: Реализовать два прохода**

```cpp
bool mp_place_roster( const std::vector<bool> &is_new, std::vector<size_t> &moved )
{
    cata_assert( is_new.size() == g->player_slot_count() );
    moved.clear();

    // Pass 1: continuing characters keep the position from their own save. The
    // first of them anchors everybody else.
    std::optional<size_t> anchor;
    for( size_t i = 0; i < is_new.size(); ++i ) {
        if( !is_new[i] && !anchor ) {
            anchor = i;
        }
    }

    // Pass 1b: continuations that were played solo on the far side of the map.
    // One bubble is 132x132 tiles, so they have to be brought together or the
    // tether objects before the first turn.
    if( anchor ) {
        const tripoint_abs_ms anchor_pos = g->player_at( *anchor ).pos_abs();
        for( size_t i = 0; i < is_new.size(); ++i ) {
            if( is_new[i] || i == *anchor ) {
                continue;
            }
            if( rl_dist( g->player_at( i ).pos_abs(), anchor_pos ) <= MP_TETHER_SOFT_LIMIT ) {
                continue;
            }
            const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( anchor_pos );
            if( !spot ) {
                return false;
            }
            map &here = get_map();
            g->player_at( i ).setpos( here, here.get_bub( *spot ) );
            moved.push_back( i );
        }
    }

    // Pass 2: new characters. Without an anchor the first of them builds the
    // world and becomes one.
    for( size_t i = 0; i < is_new.size(); ++i ) {
        if( !is_new[i] ) {
            continue;
        }
        mp_activate_player( i );
        avatar &ava = g->player_at( i );
        const std::optional<tripoint_abs_ms> near =
            anchor ? std::optional<tripoint_abs_ms>( g->player_at( *anchor ).pos_abs() )
            : std::nullopt;
        if( !place_new_character( ava, near ) ) {
            return false;
        }
        if( !anchor ) {
            anchor = i;
        }
    }
    mp_activate_player( 0 );
    return true;
}
```

- [ ] **Step 7: Добавить в сборку, прогнать**

`Makefile` — `SOURCES += $(SRC_DIR)/mp/mp_placement.cpp` рядом с `mp_context.cpp`;
то же в `CMakeLists.txt`.

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-3b-t3/
tests/cata_test --user-dir=/tmp/cata-3b-t3-full/
```
Expected: PASS оба.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_placement.h src/mp/mp_placement.cpp src/game.h src/game.cpp \
        tests/mp_placement_test.cpp Makefile CMakeLists.txt
git commit -m "feat(mp): place characters around an anchor, in two passes

start_game cannot run on a loaded world: start_calendar, the seed,
init_region_layout and the MAPBUFFER.clear on its retry path would reset the
clock and wipe the map. Its personal half moves into place_new_character, which
skips every world step when given a point to stand next to.

Placement runs after everyone has chosen and anchors on the first continuing
character -- not on slot 0, which is handed out by connection order and may be a
brand new character with no position at all. The spiral search checks creature
occupancy so the second placement of a pass sees the first.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Кооперативный сценарий

**Files:**
- Create: `data/json/scenarios_coop.json`
- Modify: `src/newcharacter.cpp:2753-2755`, `src/mp/mp_join_menu.h` (объявление), `src/mp/mp_placement.cpp`

**Interfaces:**
- Consumes: `set_scenario`, `get_scenario`, `scenario_id` (`src/scenario.h:192-193`), `reset_scenario` (`src/newcharacter.cpp:2358`).
- Produces:

```cpp
// The scenario every co-op character starts under, or nullptr in a build with no
// server. One scenario for now, which is why the join menu locks the choice
// instead of filtering a list: with a single entry the two are the same thing.
const scenario *mp_coop_scenario();
```

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_placement_test.cpp`:

```cpp
TEST_CASE( "the_coop_scenario_exists_and_starts_somewhere_safe", "[coop]" )
{
    const scenario *coop = mp_coop_scenario();
    REQUIRE( coop != nullptr );

    // Zero points, so nobody has to spend a budget on being in a co-op game.
    CHECK( coop->point_cost() == 0 );
    // Exactly one start location type, so the anchor's spot is predictable:
    // a shelter, which is safe and has floor to spare for a party.
    CHECK( coop->start_location_targets_count() >= 1 );
    CHECK_FALSE( coop->has_flag( flag_CHALLENGE ) );
}
```

- [ ] **Step 2: Прогнать — падает**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_coop_scenario"`
Expected: FAIL — `'mp_coop_scenario' was not declared in this scope`.

- [ ] **Step 3: Создать сценарий**

`data/json/scenarios_coop.json` — **новый файл**, а не правка `scenarios.json`:
последний апстрим тоже правит, а новый файл конфликтов при вливании не создаёт.

```json
[
  {
    "type": "scenario",
    "id": "coop_survivors",
    "name": "Co-op Survivors",
    "points": 0,
    "description": "You and the others woke up in the same evac shelter.  Whatever comes next, it comes for all of you.",
    "allowed_locs": [ "sloc_shelter_safe" ],
    "start_name": "Evac Shelter",
    "flags": [ "CITY_START" ]
  }
]
```

Стартовая локация переиспользует существующую `sloc_shelter_safe`
(`data/json/start_locations.json`): `om_terrain: "shelter"`, место безопасное
и вместительное.

- [ ] **Step 4: Реализовать `mp_coop_scenario`**

В `src/mp/mp_placement.cpp`:

```cpp
const scenario *mp_coop_scenario()
{
    static const string_id<scenario> coop_id( "coop_survivors" );
    if( !coop_id.is_valid() ) {
        // Content and code disagree -- the JSON was renamed or dropped. Loud,
        // because the alternative is characters starting under a random
        // scenario and landing across the map from each other.
        debugmsg( "the co-op scenario \"coop_survivors\" is missing from the JSON" );
        return nullptr;
    }
    return &coop_id.obj();
}
```

- [ ] **Step 5: Форсировать сценарий и скрыть вкладку**

В `src/newcharacter.cpp`, у вкладки сценария (`:2753`):

```cpp
#if defined(MP_SERVER)
    // The scenario is fixed for co-op: everyone starts in the same kind of
    // place, which is what makes the anchor's spot safe and roomy. One
    // scenario means locking the choice and filtering a list are the same
    // thing, so the tab simply does not appear.
    const bool show_scenario_tab = false;
#else
    const bool show_scenario_tab = true;
#endif
    if( show_scenario_tab && ImGui::BeginTabItem( _( "SCENARIO" ), nullptr,
            tab_selected[static_cast<int>( CHARCREATOR_SCENARIO )] ) ) {
```

Установка сценария — в `mp_join_menu` перед `avatar::create` (задача 6, шаг 4):
`reset_scenario( ava, mp_coop_scenario() )`.

- [ ] **Step 6: Прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[coop]" --user-dir=/tmp/cata-3b-t4/
tests/cata_test "[scenario],[widget]" --user-dir=/tmp/cata-3b-t4b/
```
Expected: PASS. JSON валиден, сценарий грузится, существующие сценарные тесты целы.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add data/json/scenarios_coop.json src/newcharacter.cpp src/mp/mp_placement.h \
        src/mp/mp_placement.cpp tests/mp_placement_test.cpp
git commit -m "feat(mp): a co-op scenario, in a file of its own

Zero points, one start location, an evac shelter. The scenario does not put
players on adjacent tiles -- no scenario can, they pick one location per
character and randomize the overmap tile -- but it guarantees the anchor lands
somewhere safe with floor to spare, which is what makes the placement code's job
trivial. New file rather than an edit to scenarios.json, which upstream also
edits.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 5: Карточка персонажа

**Files:**
- Create: `src/mp/mp_character_card.h`, `.cpp`, `tests/mp_character_card_test.cpp`
- Modify: `src/mp/mp_protocol.h`, `.cpp`, `src/mp/mp_client_main.cpp`, `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `JsonOut`/`JsonValue`, `PATH_INFO::config_dir_path()`, `write_to_file`, `read_from_file_optional_json`, `mp_control` (`src/mp/mp_protocol.h`).
- Produces:

```cpp
// What the client remembers about a character it plays on some server: enough to
// say "I am this survivor" on the next connect, and nothing more.
//
// Deliberately not the character's save. A save without its world is unplayable
// -- position, missions, factions and map memory all reference that world -- so
// keeping one would be a backup that cannot restore anything, plus a question
// about which copy wins. The server is authoritative; this is a token.
struct mp_character_card {
    std::string server;          // host:port, as typed into --connect
    std::string world_name;
    std::string save_id;         // matches save_t::decoded_name() on the server
    std::string character_name;  // for the menu line and the log

    bool empty() const {
        return save_id.empty();
    }
};

// Where a card for `server` lives in the client's config dir. One file per
// server, so "your characters, per server" is a directory listing.
cata_path mp_card_path( const std::string &server );

// Reads the card for `server`. An absent or unparseable file reads as an empty
// card, never as an error: the worst case is being offered character creation.
mp_character_card mp_load_character_card( const std::string &server );

// Writes `card`. Returns false when the write failed, which the caller logs --
// a lost card costs the player one menu choice next time, nothing more.
bool mp_save_character_card( const mp_character_card &card );
```

Протокол:

```cpp
// mp_hello gains one field: the save_id from the client's card, empty when it
// has none. The server looks a character up by it.
struct mp_hello {
    int proto = 0;
    std::string name;
    std::string save_id;   // new in Phase 3b
    int width = 0;
    int height = 0;
    bool colors_256 = false;
};

// New server->client control, sent the moment the character is established --
// not at session end. The card does not change during play, and sending it
// early means it survives the server crashing.
struct mp_character_card_msg {
    std::string world_name;
    std::string save_id;
    std::string character_name;
};
```

`mp_control_kind` получает `character_card`; `mp_control` — поле
`mp_character_card_msg character_card;`. `MP_PROTOCOL_VERSION` инкрементируется.

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_character_card_test.cpp`:

```cpp
#include "cata_catch.h"

#include <string>

#include "mp/mp_character_card.h"
#include "mp/mp_protocol.h"

TEST_CASE( "a_card_survives_a_round_trip", "[mp]" )
{
    mp_character_card card;
    card.server = "10.0.0.4:4321";
    card.world_name = "Falconer";
    card.save_id = "Ada Lovelace";
    card.character_name = "Ada Lovelace";

    REQUIRE( mp_save_character_card( card ) );
    const mp_character_card back = mp_load_character_card( card.server );

    CHECK( back.world_name == card.world_name );
    CHECK( back.save_id == card.save_id );
    CHECK( back.character_name == card.character_name );
    CHECK_FALSE( back.empty() );
}

TEST_CASE( "a_missing_card_reads_as_empty_not_as_an_error", "[mp]" )
{
    // Being offered character creation is the correct outcome of having no
    // card. An error here would turn "first time on this server" into a
    // failure to connect.
    const mp_character_card none = mp_load_character_card( "no.such.host:1" );
    CHECK( none.empty() );
}

TEST_CASE( "the_hello_carries_the_save_id", "[mp]" )
{
    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "Ada";
    hello.hello.save_id = "Ada Lovelace";
    hello.hello.width = 80;
    hello.hello.height = 24;

    mp_control back;
    REQUIRE( mp_control_from_json( mp_control_to_json( hello ), back ) );
    CHECK( back.hello.save_id == "Ada Lovelace" );
}

TEST_CASE( "the_character_card_message_round_trips", "[mp]" )
{
    mp_control msg;
    msg.kind = mp_control_kind::character_card;
    msg.character_card.world_name = "Falconer";
    msg.character_card.save_id = "Ada Lovelace";
    msg.character_card.character_name = "Ada Lovelace";

    mp_control back;
    REQUIRE( mp_control_from_json( mp_control_to_json( msg ), back ) );
    CHECK( back.kind == mp_control_kind::character_card );
    CHECK( back.character_card.save_id == "Ada Lovelace" );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_character_card"`
Expected: FAIL — `mp/mp_character_card.h: No such file or directory`.

- [ ] **Step 3: Создать `mp_character_card.h` / `.cpp`**

Заголовок — блок **Interfaces**. Реализация: имя файла — `base64_encode( server )`
плюс `.mpcard.json` в `PATH_INFO::config_dir_path() / "mp"`; сериализация через
`JsonOut`, чтение через `read_from_file_optional_json`.

Собирается **всегда**, не под `MULTIPLAYER`: карточку читает клиент, а клиент —
обычная сборка.

- [ ] **Step 4: Расширить протокол**

`src/mp/mp_protocol.h`: поле `save_id` в `mp_hello`, структура
`mp_character_card_msg`, значение `character_card` в `mp_control_kind`, поле
в `mp_control`, инкремент `MP_PROTOCOL_VERSION`.

`src/mp/mp_protocol.cpp`: чтение и запись новых полей в `mp_control_to_json` /
`mp_control_from_json` по образцу соседних.

- [ ] **Step 5: Подключить клиента**

`src/mp/mp_client_main.cpp` — перед отправкой `hello` (`:113-120`):

```cpp
    const mp_character_card card = mp_load_character_card( host_and_port );
    hello.hello.save_id = card.save_id;
```

и в обработке входящих контролов — новая ветка:

```cpp
        case mp_client_event::character_card: {
            // The server just told us who we are. Written immediately rather
            // than at session end, so a crash does not cost the player their
            // identity on this server.
            mp_character_card fresh;
            fresh.server = host_and_port;
            fresh.world_name = state.card.world_name;
            fresh.save_id = state.card.save_id;
            fresh.character_name = state.card.character_name;
            if( !mp_save_character_card( fresh ) ) {
                DebugLog( D_WARNING, D_MAIN ) << "mp client: could not write the character card";
            }
            break;
        }
```

`mp_client_event` получает значение `character_card`, `mp_client_state` — поле
`mp_character_card_msg card;`; `mp_client_apply_message` заполняет их.

- [ ] **Step 6: Добавить в сборку, прогнать**

`Makefile` — `SOURCES += $(SRC_DIR)/mp/mp_character_card.cpp` рядом с `mp_context.cpp`;
то же в `CMakeLists.txt`.

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp]" --user-dir=/tmp/cata-3b-t5/
tests/cata_test --user-dir=/tmp/cata-3b-t5-full/
```
Expected: PASS оба.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/mp/mp_character_card.h src/mp/mp_character_card.cpp src/mp/mp_protocol.h \
        src/mp/mp_protocol.cpp src/mp/mp_client_main.cpp src/mp/mp_client_screen.h \
        src/mp/mp_client_screen.cpp tests/mp_character_card_test.cpp Makefile CMakeLists.txt
git commit -m "feat(mp): the client remembers who it is, with a card not a save

A token -- server, world, save id, name -- so the next connect can offer
'continue as Ada' instead of a list of everybody's characters. Deliberately not
the character's save: a save without its world is unplayable, so it would be a
backup that restores nothing plus a question about which copy wins. The server
stays authoritative; a card naming a character the world does not have degrades
to character creation, never to an error.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: Меню входа

**Files:**
- Create: `src/mp/mp_join_menu.h`, `.cpp`
- Modify: `Makefile`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `avatar::create( character_type, template )` (`src/avatar.h:117`), `character_type::CUSTOM`/`RANDOM`/`TEMPLATE` (`src/enums.h:481-487`), `mp_coop_scenario()` (задача 4), `reset_scenario` (`src/newcharacter.cpp:2358`), `mp_load_character_card` (задача 5), `save_t`, `WORLD::world_saves`, `uilist`, `get_options()`.
- Produces:

```cpp
// What one player decided in the join menu.
struct mp_join_choice {
    enum class kind {
        // Created a fresh character; needs placing (task 3).
        created,
        // Continuing an existing one; its position comes from its save.
        continued,
        // Left before choosing. The seat is freed and the server listens again.
        quit,
    };
    kind decision = kind::quit;
    // Set when decision == continued: which save to load.
    std::optional<save_t> save;
};

// Runs the join menu for the active player, in their own context and their own
// thread, and returns what they chose. Streams to their client like any other
// screen.
//
// `card_save_id` is the save id their client reported in its hello; an empty
// one, or one that names no character in this world, simply means the continue
// entry is absent.
mp_join_choice mp_run_join_menu( const std::string &card_save_id );

// Draws the "waiting for the others" screen for a player who has chosen, and
// blocks until everyone has. An ordinary drawn screen -- the protocol needs
// nothing for this.
void mp_wait_for_roster( const std::vector<std::string> &still_choosing );
```

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/mp_character_card_test.cpp` (тест логики выбора, без UI):

```cpp
TEST_CASE( "the_continue_entry_appears_only_for_a_character_in_this_world", "[mp]" )
{
    std::vector<save_t> world_saves;
    world_saves.push_back( save_t::from_save_id( "Ada Lovelace" ) );

    // The card names somebody in this world: continuing is offered.
    CHECK( mp_continue_target( world_saves, "Ada Lovelace" ).has_value() );
    // The card names a stranger, or there is no card at all: only creation is
    // offered. Never an error -- a first connect must not fail.
    CHECK_FALSE( mp_continue_target( world_saves, "Grace Hopper" ).has_value() );
    CHECK_FALSE( mp_continue_target( world_saves, "" ).has_value() );
    CHECK_FALSE( mp_continue_target( {}, "Ada Lovelace" ).has_value() );
}
```

`mp_continue_target( const std::vector<save_t> &, const std::string &card_save_id )`
объявляется в `mp_join_menu.h` рядом с остальным: чистая функция, вся логика пункта
«продолжить» без UI, поэтому проверяется в обычной сборке.

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 2>&1 | grep -m3 "mp_continue_target"`
Expected: FAIL.

- [ ] **Step 3: Создать `mp_join_menu.h` и чистую часть**

```cpp
std::optional<save_t> mp_continue_target( const std::vector<save_t> &world_saves,
        const std::string &card_save_id )
{
    if( card_save_id.empty() ) {
        return std::nullopt;
    }
    for( const save_t &save : world_saves ) {
        if( save.decoded_name() == card_save_id ) {
            return save;
        }
    }
    // The card outlived the character -- deleted, or a different world reusing
    // the same server. Offering creation is the right answer; refusing the
    // connection would not be.
    return std::nullopt;
}
```

- [ ] **Step 4: Реализовать меню**

```cpp
mp_join_choice mp_run_join_menu( const std::string &card_save_id )
{
    avatar &ava = get_avatar();
    const std::vector<save_t> &saves = world_generator->active_world->world_saves;
    const std::optional<save_t> resume = mp_continue_target( saves, card_save_id );

    while( true ) {
        uilist menu( _( "Join the session" ), {} );
        enum { NEW_CUSTOM, NEW_PRESET, NEW_RANDOM, CONTINUE, SETTINGS, HELP, QUIT };
        menu.entries.emplace_back( NEW_CUSTOM, true, MENU_AUTOASSIGN,
                                   _( "New character: custom" ) );
        menu.entries.emplace_back( NEW_PRESET, true, MENU_AUTOASSIGN,
                                   _( "New character: preset template" ) );
        menu.entries.emplace_back( NEW_RANDOM, true, MENU_AUTOASSIGN,
                                   _( "New character: random" ) );
        if( resume ) {
            menu.entries.emplace_back( CONTINUE, true, MENU_AUTOASSIGN,
                                       string_format( _( "Continue as %s" ),
                                               resume->decoded_name() ) );
        }
        // Server-side options, and anyone may change them: co-op is a trusted
        // circle, and the one dangerous shared setting (sidebar width) is
        // already closed on the server.
        menu.entries.emplace_back( SETTINGS, true, MENU_AUTOASSIGN, _( "Settings" ) );
        menu.entries.emplace_back( HELP, true, MENU_AUTOASSIGN, _( "Help" ) );
        menu.entries.emplace_back( QUIT, true, 'q', _( "Quit" ) );
        menu.query();

        mp_join_choice choice;
        switch( menu.ret ) {
            case NEW_CUSTOM:
            case NEW_PRESET:
            case NEW_RANDOM: {
                // The scenario is fixed before creation so every co-op character
                // starts in the same kind of place; the scenario tab is hidden
                // in this build (task 4).
                if( const scenario *coop = mp_coop_scenario() ) {
                    reset_scenario( ava, coop );
                }
                const character_type type = menu.ret == NEW_CUSTOM ? character_type::CUSTOM
                                            : menu.ret == NEW_RANDOM ? character_type::RANDOM
                                            : character_type::TEMPLATE;
                std::string tmpl;
                if( type == character_type::TEMPLATE && !mp_pick_template( tmpl ) ) {
                    continue;
                }
                if( !ava.create( type, tmpl ) ) {
                    // Backed out of creation. Back to the menu, not into the
                    // session with a half-built character.
                    continue;
                }
                choice.decision = mp_join_choice::kind::created;
                return choice;
            }
            case CONTINUE:
                choice.decision = mp_join_choice::kind::continued;
                choice.save = resume;
                return choice;
            case SETTINGS:
                get_options().show( false );
                continue;
            case HELP:
                get_help().display_help();
                continue;
            case QUIT:
            default:
                choice.decision = mp_join_choice::kind::quit;
                return choice;
        }
    }
}
```

`mp_pick_template( std::string &out )` — список шаблонов через существующий
`main_menu::load_char_templates`-эквивалент; при пустом списке сообщение
«No templates found» и возврат false.

`mp_wait_for_roster` рисует список ещё выбирающих и крутит
`inp_mngr.get_input_event()` до сигнала от оркестратора; экран обычный, протокол
не меняется.

- [ ] **Step 5: Добавить в сборку, прогнать**

Файл подхватывается глобом `$(wildcard $(SRC_DIR)/mp/*.cpp)` под `MULTIPLAYER`;
проверить, что глоб в `CMakeLists.txt` тоже покрывает.

Run:
```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-3b-t6/
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```
Expected: PASS, серверная сборка линкуется.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add src/mp/mp_join_menu.h src/mp/mp_join_menu.cpp tests/mp_character_card_test.cpp \
        CMakeLists.txt
git commit -m "feat(mp): a join menu of its own, per client, in its own context

A new menu rather than a mode of main_menu: that one is entangled with world
selection, world creation, mods and special games, and reusing it would drag all
of them in. What is reused is what is actually needed -- avatar::create and the
template list -- so character creation needs no new code, it is the same UI
running in one player's context.

The continue entry is driven by the client's card and is pure logic, so it is
tested without a UI: a card naming a stranger, or no card, simply means the
entry is absent.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 7: Старт сессии на сервере

**Files:**
- Modify: `src/mp/mp_server_main.h`, `.cpp`, `src/main.cpp`
- Modify: `tests/mp_session_harness.h`, `.cpp`

**Interfaces:**
- Consumes: всё предыдущее плюс `mp_sched`, `mp_session`, `mp_sync_context_count` (Ф3a).
- Produces:

```cpp
// Loads the world named on the command line, seats N empty slots, runs every
// client's join menu, then places the roster. Returns false with `err` set when
// the world will not load or the roster cannot be placed.
//
// Replaces Phase 3a's "match sessions to slots by name": characters no longer
// have to exist before the server starts.
bool mp_server_open_world( const std::string &world_name, std::string &err );
```

`mp_server_start_session` из Ф3a удаляется — раздача слотов по `mp_hello.name`
заменяется меню входа.

- [ ] **Step 1: Написать падающий тест**

Создать `tests/mp_join_test.cpp`:

```cpp
#if defined(MP_SERVER)

#include "cata_catch.h"

#include "avatar.h"
#include "calendar.h"
#include "game.h"
#include "mp/mp_context.h"
#include "mp_session_harness.h"

TEST_CASE( "the_world_does_not_tick_until_everyone_has_chosen", "[mp]" )
{
    // The harness starts in join mode: two clients, neither has chosen.
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE_FALSE( harness.run_until( []() {
        return false;
    }, 3 ) );
    // One player still in the menu means the world is not running. This is the
    // fixed roster: everyone arrives together, and joining mid-session is
    // Phase 5.
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    harness.choose_random_character( 0 );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    harness.choose_random_character( 1 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 10 ) );
}

TEST_CASE( "two_fresh_characters_start_next_to_each_other", "[mp]" )
{
    mp_session_harness harness( 2, 5, mp_harness_mode::join );
    harness.choose_random_character( 0 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( []() {
        return true;
    }, 5 ) );

    // The world was empty, so the first new character built it and became the
    // anchor; the second came to it. Both inside the tether means the bubble
    // holds them both, which is the whole premise of tethered co-op.
    const tripoint_abs_ms a = g->player_at( 0 ).pos_abs();
    const tripoint_abs_ms b = g->player_at( 1 ).pos_abs();
    CHECK( a != b );
    CHECK( rl_dist( a, b ) <= MP_TETHER_SOFT_LIMIT );
}

#endif // MP_SERVER
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- 2>&1 | grep -m3 "mp_harness_mode\|choose_random_character"`
Expected: FAIL — `'mp_harness_mode' was not declared`.

- [ ] **Step 3: Расширить харнесс режимом входа**

`tests/mp_session_harness.h`:

```cpp
enum class mp_harness_mode {
    // Slots are seeded directly, as in Phase 3a: the session is already playing.
    playing,
    // Slots start empty and each client runs the join menu, as a real session
    // does. run_until() does not advance the world until all have chosen.
    join,
};
```

плюс `void choose_random_character( size_t slot );`, который отвечает на меню того
слота выбором «New character: random» — последовательностью нажатий, а не вызовом
внутренностей: харнесс проверяет путь, которым пойдёт человек.

- [ ] **Step 4: Реализовать `mp_server_open_world`**

```cpp
bool mp_server_open_world( const std::string &world_name, std::string &err )
{
    err.clear();
    WORLD *world = world_generator->get_world( world_name );
    if( world == nullptr ) {
        err = "no world named \"" + world_name + "\"";
        return false;
    }
    world_generator->set_active_world( world );
    try {
        g->setup();
    } catch( const std::exception &e ) {
        err = std::string( "setting the world up failed: " ) + e.what();
        return false;
    }
    if( !g->load_world_state() ) {
        err = "loading the world state failed";
        return false;
    }

    // Slots come after setup(): setup() clears player_slots (game.cpp:716-725),
    // so seating them earlier would lose them. An empty avatar is exactly what
    // character creation fills, and it is what gives the join menu a context to
    // draw into -- contexts are parallel to slots, and the menu runs before any
    // character exists.
    while( g->player_slot_count() < mp_sessions().size() ) {
        g->add_player_slot().set_body();
    }
    mp_sync_context_count();
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        mp_sessions()[i]->context_index = i;
    }

    // Join phase: no barrier, the scheduler simply rotates through whoever has
    // not finished. Threads are already up (Phase 3a).
    std::vector<bool> is_new( mp_sessions().size(), true );
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        mp_activate_player( i );
        const mp_join_choice choice = mp_run_join_menu( mp_sessions()[i]->card_save_id );
        switch( choice.decision ) {
            case mp_join_choice::kind::created:
                is_new[i] = true;
                break;
            case mp_join_choice::kind::continued:
                is_new[i] = false;
                if( !g->load_character( *choice.save, i ) ) {
                    err = "loading \"" + choice.save->decoded_name() + "\" failed";
                    return false;
                }
                break;
            case mp_join_choice::kind::quit:
                err = "\"" + mp_sessions()[i]->name + "\" left before choosing";
                return false;
        }
    }

    std::vector<size_t> moved;
    if( !mp_place_roster( is_new, moved ) ) {
        err = "could not find room for everybody near the group";
        return false;
    }
    for( const size_t slot : moved ) {
        mp_activate_player( slot );
        add_msg( m_info, _( "Your survivor and the others found each other." ) );
    }
    mp_activate_player( 0 );

    // The card goes out now, not at session end: it does not change during play,
    // and sending it early means it survives the server crashing.
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        mp_send_character_card( *mp_sessions()[i], world->world_name,
                                g->player_at( i ).get_save_id(), g->player_at( i ).name );
    }
    return true;
}
```

`mp_session` получает поле `std::string card_save_id;`, заполняемое из
`mp_hello.save_id` в handshake. `mp_send_character_card` — короткая функция
в `mp_server_main.cpp`, собирающая `mp_control` и отправляющая в транспорт сессии.

Меню исполняется в потоке своего игрока: цикл выше — оркестраторская сторона,
а `mp_run_join_menu` вызывается телом потока игрока (Ф3a, задача 11), которое
на время фазы входа зовёт меню вместо `player_act`.

- [ ] **Step 5: Убрать раздачу слотов по имени**

Удалить `mp_server_start_session` из `src/mp/mp_server_main.h` и `.cpp` вместе
с тестами `sessions_are_matched_to_slots_by_name` и
`an_unknown_name_is_refused_with_a_reason` в `tests/mp_session_test.cpp`:
имя персонажа больше не определяет слот, его определяет меню входа.

- [ ] **Step 6: Прогнать**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
timeout 300 tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-3b-t7/
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test --user-dir=/tmp/cata-3b-t7-full/
```
Expected: PASS всё.

- [ ] **Step 7: Прогнать под ThreadSanitizer**

Run:
```bash
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 TSAN=1 BUILD_PREFIX=mpserver-tsan-
timeout 600 tests/mpserver-tsan-cata_test "[mp]" --user-dir=/tmp/cata-3b-t7-tsan/
```
Expected: PASS, ноль отчётов. Фаза входа гоняет те же потоки, что и партия, поэтому
ворота Ф3a остаются в силе.

- [ ] **Step 8: Коммит**

```bash
make astyle
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp src/mp/mp_session.h src/main.cpp \
        tests/mp_session_harness.h tests/mp_session_harness.cpp tests/mp_session_test.cpp \
        tests/mp_join_test.cpp
git commit -m "feat(mp): the server opens a world and seats everyone through the join menu

Replaces Phase 3a's slot-by-name handout: characters no longer have to exist
before the server starts. The world loads once, N empty slots are seated after
setup (which clears them), and each client runs its own join menu -- an empty
avatar being exactly what creation fills, and what gives the menu a context to
draw into before any character exists.

There is no barrier during the join phase; the scheduler just rotates. Placement
runs once everyone has chosen, and anybody pulled to the group is told so before
the first turn rather than after.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: Ревизия «несколько персонажей в одном мире»

**Files:**
- Create: `docs/superpowers/reports/2026-08-08-multi-character-world-audit.md`
- Modify: то, что ревизия найдёт (список — результат задачи, не её вход)

**Interfaces:**
- Consumes: весь предыдущий код.
- Produces: отчёт со списком механик, ломающихся при нескольких персонажах в мире, вердиктом по каждой и правками там, где они дешёвые.

Апстрим сам предупреждает об этом (`src/main_menu.cpp:983`: «Many game features
will not work correctly with multiple characters in the same world»), и весь
кооп-дизайн стоит ровно на этом пути. Список того, что именно ломается, не
составлял никто, включая апстрим. Задача — составить его и проверить по пунктам.

- [ ] **Step 1: Найти источник предупреждения**

Run:
```bash
grep -rn "multiple characters in the same world" src/
git -C . log -S "multiple characters in the same world" --oneline | tail -5
```
Записать, когда и зачем предупреждение появилось: коммит, автор, обсуждение.

- [ ] **Step 2: Перебрать подозреваемых**

По каждому пункту — проверить в коде, кто владелец состояния: мир или персонаж.
Список подозреваемых и что искать:

| Механика | Что проверить |
|---|---|
| Миссии | `mission::process_all()`, `mission_start`/`mission_fail`; привязана ли миссия к `character_id` или к миру |
| Фракционная репутация | `faction::likes_u`, `respects_u`; общая на мир или на персонажа |
| Лагеря | `basecamp`, `validate_camps()`; чей лагерь и кто им командует |
| Память карты | `u.load_map_memory()`; отдельный файл на персонажа — проверить, что так и есть |
| Дневник | `avatar_diary`; отдельный файл на персонажа |
| Зоны | `zone_manager::save_zones` — разделение на персональные и мировые |
| Последователи NPC | `validate_npc_followers()`; за кем идёт NPC при двух аватарах |
| Транспорт | `passenger_id` в `vpart`; сохраняется ли при свопе слотов |
| Достижения и статистика | `save_achievements()`, `stats()`; на мир или на персонажа |
| `uistate` | Один файл на мир (Global Constraints) — подтвердить, что последствия косметические |

- [ ] **Step 3: Написать отчёт**

`docs/superpowers/reports/2026-08-08-multi-character-world-audit.md`: по каждому
пункту — владелец состояния, вердикт (работает / ломается косметически / ломается
по существу), и для ломающегося — стоимость правки. Отчёт, а не список задач: часть
находок уедет в Ф5 и дальше, и это нормально.

- [ ] **Step 4: Починить дешёвое, зафиксировать дорогое**

Правки, укладывающиеся в один-два файла и покрываемые тестом, делаются здесь же,
каждая своим коммитом. Всё, что тянет на фазу, идёт в отчёт как отложенное
с оценкой — и попадает в границы Ф5 или tiles-фазы, а не в этот план.

- [ ] **Step 5: Коммит отчёта**

```bash
git add docs/superpowers/reports/2026-08-08-multi-character-world-audit.md
git commit -m "docs(mp): audit what breaks with several characters in one world

Upstream warns about this path at main_menu.cpp:983 and the whole co-op design
stands on it, but nobody -- upstream included -- ever wrote down what 'many game
features will not work correctly' actually means. This is that list, with an
owner and a verdict per mechanic, and the cheap fixes applied.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 9: Ворота, правки спеков, документация

**Files:**
- Modify: `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`, `doc/MULTIPLAYER.md`

**Interfaces:**
- Consumes: работающую фазу.
- Produces: пройденные ворота и обновлённые документы.

- [ ] **Step 1: Ворота — свежий мир, два новых персонажа**

```bash
make -j$(nproc) CCACHE=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
make -j$(nproc) CCACHE=1 TILES=1
# создать пустой мир локально, без персонажей:
./cataclysm-tiles           # World -> create "coop", выйти не создавая персонажа
./mpserver-cataclysm-tiles --server 4321 --world "coop" --players 2
# на каждой машине:
./cataclysm-tiles --connect <host>:4321
```

Expected: каждый видит своё меню входа, создаёт персонажа, оба оказываются в одном
шелтере в соседних клетках, партия идёт. Первый готовый видит экран ожидания второго.

- [ ] **Step 2: Ворота — тот же мир, оба продолжают**

Выйти из партии, поднять сервер на том же мире, подключиться теми же клиентами.

Expected: у каждого пункт «Continue as <имя>» вместо создания, оба продолжают
за своих, позиции сохранились.

- [ ] **Step 3: Ворота — проверить круговой рейс сохранения**

```bash
ls save/coop/
```
Expected: два файла персонажей, а не один; в мире два персонажа, каждый
зарегистрирован в `world_saves`. Открыть мир локально обычным билдом и убедиться,
что оба персонажа видны в списке загрузки — это и есть обещание §8.1 родителя,
проверенное руками.

- [ ] **Step 4: Внести правки в родительский спек**

`docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`:

- **§8.1** — «несколько аватаров в сейве» заменить на «несколько сейвов персонажей
  в мире»; обещание «одиночный сейв можно отдать серверу и наоборот» становится
  полным в обе стороны;
- **§8.2** — «Вход» реализован в Ф3b, не в Ф5, и в форме меню на каждом клиенте,
  а не автоматического поиска по имени;
- **§13** — «Клиент бесполезен без сервера» уточнить: клиент держит карточку
  персонажа, чтобы опознать себя; играть по ней нельзя, сервер остаётся
  единственным источником правды.

Если правки Ф3a (её задача 14) ещё не внесены — внести обе группы одним коммитом.

- [ ] **Step 5: Обновить `doc/MULTIPLAYER.md`**

Описать: подготовку пустого мира, запуск сервера с `--world` и `--players`,
меню входа и его пункты, кооп-сценарий, где живёт карточка персонажа.

- [ ] **Step 6: Коммит**

```bash
git add docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md doc/MULTIPLAYER.md
git commit -m "docs(mp): close phase 3b -- gates passed, parent spec updated

A co-op world is now literally an ordinary world with several characters, which
is what the parent spec promised in 8.1 and could only half deliver while the
party lived inside slot zero's file. The parent gets the three edits 3b called
for, including the honest version of 'the client is useless without a server':
it keeps a card, and a card cannot be played.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Заметки для реализующего

**1. Порядок задач 1 и 2 не переставлять.** Задача 1 (сохранение) создаёт фикстуру,
на которой проверяется задача 2 (загрузка): пока запись `extra_players` жива, «мир
с двумя персонажами» не существует в новом формате, и разрез загрузки нечем
проверять.

**2. `game::unserialize` в тестах не вызывать** — Global Constraints, и там же
причина. Если задача требует настоящий круговой рейс мира, это ворота человека
(задача 9), а не автотест. Соблазн велик и стоит SIGSEGV в постороннем тесте.

**3. Ассерт в `load_character` несущий.** Загрузка в неверный слот молчаливо даёт
двух игроков, делящих одного персонажа. Ассерт — единственное, что превращает это
в громкую ошибку.

**4. Опорный — не слот 0.** Слоты раздаются по порядку подключения, поэтому слот 0
может оказаться новым персонажем без позиции. Вся привязка идёт к опорному
(первому продолжающему, а при пустом мире — первому новому). Тест
`the_anchor_is_the_first_continuing_slot_not_slot_zero` существует именно для того,
чтобы это не переписали обратно.

**5. Спираль обязана видеть только что размещённых.** Проход 2 ставит персонажей
по очереди; второй ищет место, когда первый уже стоит на тайле. Проверка через
`creature_tracker::creature_at` это ловит, проверка «нет ли монстра» — нет.

**6. Сценарий не даёт соседних тайлов.** Ни этот, ни любой другой: сценарий
выбирает одну локацию на персонажа и рандомизирует OMT. Он гарантирует, что место
безопасно и вместительно; вплотную ставит код размещения. Если в обзоре покажется,
что задачу 3 можно упростить за счёт задачи 4 — нельзя.

**7. Карточка не создаёт персонажей.** Сервер авторитетен: карточка только
выбирает между «продолжить» и «создать». Клиент, приславший `save_id`
несуществующего персонажа, получает меню создания, а не персонажа и не отказ.

**8. Задача 8 может найти блокер.** Ревизия «несколько персонажей в одном мире» —
единственная задача плана с открытым исходом. Если она обнаружит что-то, ломающееся
по существу (например, миссии, привязанные к миру, а не к персонажу), это меняет
границы Ф5, а возможно и раньше. Поэтому она идёт до ворот, а не после.

---

## Значения, разрешённые при планировании

| Что | Значение |
|---|---|
| Тестовый харнесс | Catch2, `#include "cata_catch.h"`; теги `[mp]` и `[coop]`; `tests/cata_test` / `tests/mpserver-cata_test` |
| Сброс слотов | `game_test_helper::reset_player_slots()` (`tests/game_test_helper.h`); `g->add_player_slot()` — публичный |
| RAII теста | `restore_active_state` (`tests/game_test_helper.h`); `restore_on_out_of_scope` (`cata_scope_helpers.h:32`) |
| Подготовка мира в тесте | `clear_avatar()`, `build_test_map( ter_id( "t_pavement" ) )` (`tests/map_helpers.h`); `g->ensure_gamemode_for_test()` |
| Контекст в тесте | `mp_ensure_player_context()`, `mp_sync_context_count()` (Ф3a), `mp_activate_player( idx )` |
| Сессионный харнесс | `tests/mp_session_harness.h` (Ф3a), расширяется `mp_harness_mode` |
| Создание персонажа | `avatar::create( character_type, tempname )` (`src/avatar.h:117`); `character_type::CUSTOM`/`RANDOM`/`TEMPLATE` (`src/enums.h:481-487`) |
| Сценарий | `set_scenario`/`get_scenario` (`src/scenario.h:192-193`); `reset_scenario( avatar &, const scenario * )` (`src/newcharacter.cpp:2358`); вкладка `CHARCREATOR_SCENARIO` (`:2753`) |
| Сейвы мира | `WORLD::world_saves` (`src/worldfactory.h:67`); `save_t::from_save_id`/`decoded_name`/`base_path` (`:32-55`); `WORLD::add_save` |
| Разрез загрузки — источник | `game::load( save_t )` (`src/game_io.cpp:325-510`) |
| Разрез сохранения — источник | `game::save()` (`src/game_io.cpp:737`); части `save_player_data` (`:604`), `save_achievements` (`:647`), `save_factions_missions_npcs` (`:574`), `save_dimension_data` (`:582`), `save_maps` (`:589`), `save_external_options_record` (`:702`) |
| Стартовая локация | `game::start_game()` (`src/game.cpp:808-900`); `start_location::find_player_initial_location`, `prepare_map`, `place_player` |
| Тезер | `MP_TETHER_SOFT_LIMIT`, `mp_bubble_anchor()`, `mp_tether_check()` (`src/mp/mp_tether.h`) |
| Существа на тайле | `get_creature_tracker().creature_at( tripoint_bub_ms )` (`src/creature_tracker.h`) |
| Клиентские пути | `PATH_INFO::config_dir_path()` (`src/path_info.h:64`); `base64_encode` (`src/base64.h`) |
| Протокол | `mp_control`, `mp_control_kind`, `mp_hello`, `MP_PROTOCOL_VERSION` (`src/mp/mp_protocol.h`) |
| Клиентская сторона | `mp_client_apply_message( payload, mp_screen_grid &, mp_client_state & )`, `mp_client_event` (`src/mp/mp_client_screen.h`) |
| Сборка: всегда | Явные строки `SOURCES +=` (`Makefile:1116`); под `MULTIPLAYER` — глоб `src/mp/*.cpp` (`:1119`) |
