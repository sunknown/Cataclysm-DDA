# MVP-2: hotseat-кооп на два аватара — план

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Два полноценных аватара в одном reality bubble на одном экране, с переключением управления, тезером и корректной реакцией мира на обоих — без сети, без намерений и без требований к детерминизму.

**Architecture:** Аватар перестаёт быть единственным: `game` получает вектор слотов, а `get_avatar()` начинает возвращать активный слот вместо жёстко зашитого объекта. Член `game::u` остаётся ссылкой на слот 0 и означает «первый игрок, устаревший доступ» — это позволяет не трогать сотни его использований внутри `game.cpp` и осушать их постепенно. Ход по-прежнему крутится настоящим кодом, но цикл для hotseat живёт отдельно от одиночного `game::do_turn()`, под флагом сборки.

**Tech Stack:** C++17, Catch2 (`tests/cata_catch.h`), GNU Make, CMake.

## Global Constraints

- Одиночный игровой путь не меняется по поведению. `game::do_turn()` не переписывается; hotseat получает собственный цикл.
- Весь код режима живёт в `src/mp/` и собирается только при `MULTIPLAYER=1`. Дефолтная сборка его не содержит.
- Спек: `docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md`, разделы 5.4, 5.5, 5.7, 9.1 и фазы Ф2, Ф4, Ф6.
- Сборка: `make -j$(nproc) TILES=0 TESTS=1` для одиночной, `make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1` для режима. Бинарь тестов `tests/cata_test` запускается из корня репозитория.
- 266 существующих тестовых файлов обязаны оставаться зелёными в обеих сборках на всём протяжении работ. Это главный критерий того, что одиночная игра не задета.
- Новые тесты режима помечаются тегом `[hotseat]` и не помечаются `[.]` — они должны гоняться в CI сборки с `MULTIPLAYER=1`.

## Отступление от спека, требующее фиксации

Спек §9.1 говорит, что MVP-2 требует фаз Ф2, Ф4 и Ф6, и не требует Ф1. Это верно, но неполно: отдельный цикл хода должен где-то жить, а значит MVP-2 дополнительно втягивает **строительные леса Ф0** — флаг сборки и каталог `src/mp/`. Транспорт, намерения и хеш состояния из Ф0 при этом не нужны. Task 1 реализует ровно эту подчасть.

## Ключевое архитектурное решение: судьба `game::u`

Сейчас `game` держит `pimpl<avatar> u_ptr` (`src/game.h:1158`) и ссылку `avatar &u` (`src/game.h:1176`), связанные в конструкторе (`src/game.cpp:461`). Ссылку нельзя перепривязать, поэтому переключение управления через `u` невозможно.

Замена контентом по образцу `avatar::control_npc` (`src/avatar.cpp:154`) тоже не годится: она материализует одного персонажа за раз, а hotseat требует, чтобы оба аватара существовали в мире одновременно — иначе монстры не смогут видеть и атаковать обоих в один ход.

Принятое решение:

- `game::u` **остаётся** ссылкой и навсегда означает слот 0. Все её использования внутри `game.cpp` продолжают компилироваться и означают «первый игрок».
- `get_avatar()`, `get_player_character()` и `get_player_view()` (`src/game.cpp:11523`, `:11527`, `:11533`) начинают возвращать **активный** слот.
- Вне `game.cpp` прямых обращений `g->u` всего 14 — они разбираются поимённо в Task 2.

Следствие, которое надо знать: после Task 2 внутри `game.cpp` `u` и `get_avatar()` перестают быть синонимами. Каждое место, где это важно, чинится на Ф6 (Task 9), остальные означают «слот 0» осознанно.

---

## File Structure

| Файл | Ответственность |
|---|---|
| `Makefile` (правка) | Флаг `MULTIPLAYER=1` |
| `CMakeLists.txt` (правка) | Опция `MULTIPLAYER` |
| `src/mp/mp_mode.h` / `.cpp` (создать) | Активен ли режим, вход в hotseat-цикл |
| `src/mp/mp_hotseat_loop.h` / `.cpp` (создать) | Цикл хода на два слота |
| `src/mp/mp_tether.h` / `.cpp` (создать) | Anchor бабла и проверка тезера |
| `src/game.h` / `.cpp` (правка) | Вектор слотов, активный слот, `get_players()`, `nearest_player()`, `any_player_sees()` |
| `src/savegame.cpp` (правка) | Сериализация нескольких игроков с обратной совместимостью |
| `tests/hotseat_test.cpp` (создать) | Тесты слотов, переключения, тезера |
| `tests/hotseat_sim_test.cpp` (создать) | Тесты реакции мира на обоих игроков |

---

## Task 1: Флаг сборки и каркас режима

**Files:**
- Modify: `Makefile`
- Modify: `CMakeLists.txt`
- Create: `src/mp/mp_mode.h`, `src/mp/mp_mode.cpp`
- Test: `tests/hotseat_test.cpp`

**Interfaces:**
- Produces:
  - `bool mp_mode_active();`
  - `void mp_mode_set_active( bool );`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/hotseat_test.cpp`:

```cpp
#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include "mp/mp_mode.h"

TEST_CASE( "mp_mode_defaults_to_inactive", "[hotseat]" )
{
    mp_mode_set_active( false );
    CHECK_FALSE( mp_mode_active() );
    mp_mode_set_active( true );
    CHECK( mp_mode_active() );
    mp_mode_set_active( false );
}
#endif
```

- [ ] **Step 2: Прогнать и убедиться, что сборка с флагом падает**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: ошибка — нет `mp/mp_mode.h`.

- [ ] **Step 3: Добавить флаг в Makefile**

Рядом с обработкой прочих необязательных флагов (искать `ifeq ($(SOUND), 1)`) добавить:

```make
ifeq ($(MULTIPLAYER), 1)
  DEFINES += -DMULTIPLAYER
endif
```

Убедиться, что `src/mp/*.cpp` попадает в список исходников. Проверить, как собирается `SOURCES`:

```bash
grep -n "SOURCES :=\|SOURCES =\|wildcard src/" Makefile | head
```

Если сборка использует `wildcard src/*.cpp`, добавить отдельную строку для `src/mp/*.cpp`, включаемую только под флагом, чтобы в дефолтной сборке каталог не участвовал.

- [ ] **Step 4: Добавить опцию в CMake**

В `CMakeLists.txt` рядом с прочими опциями добавить `option(MULTIPLAYER "Build co-op multiplayer mode" OFF)`, а при включении — добавить `src/mp` в исходники и определить `MULTIPLAYER`.

- [ ] **Step 5: Создать модуль режима**

`src/mp/mp_mode.h`:

```cpp
#pragma once
#ifndef CATA_SRC_MP_MP_MODE_H
#define CATA_SRC_MP_MP_MODE_H

// Whether the current session is a co-op session. False in every single
// player session, including in a build that has multiplayer compiled in.
bool mp_mode_active();
void mp_mode_set_active( bool active );

#endif // CATA_SRC_MP_MP_MODE_H
```

`src/mp/mp_mode.cpp`:

```cpp
#include "mp/mp_mode.h"

static bool mp_active = false;

bool mp_mode_active()
{
    return mp_active;
}

void mp_mode_set_active( bool active )
{
    mp_active = active;
}
```

- [ ] **Step 6: Прогнать обе сборки**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo
```

Ожидается: с флагом тест `[hotseat]` проходит; без флага он не существует, а обычные тесты зелёные.

- [ ] **Step 7: Коммит**

```bash
git add Makefile CMakeLists.txt src/mp/mp_mode.h src/mp/mp_mode.cpp tests/hotseat_test.cpp
git commit -m "build: add MULTIPLAYER flag and mp mode scaffold"
```

---

## Task 2: Вектор слотов игроков

Вектор существует в обеих сборках и в одиночной игре всегда имеет размер 1. Поведение одиночной игры не меняется.

**Files:**
- Modify: `src/game.h:1158`, `src/game.h:1176`
- Modify: `src/game.cpp:461`, `src/game.cpp:11523-11535`
- Test: `tests/hotseat_test.cpp`

**Interfaces:**
- Produces:
  - `size_t game::player_slot_count() const;`
  - `size_t game::active_slot() const;`
  - `void game::set_active_slot( size_t index );`
  - `avatar &game::player_at( size_t index );`

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "single_player_has_exactly_one_slot", "[hotseat]" )
{
    REQUIRE( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK( &g->player_at( 0 ) == &get_avatar() );
}
```

Добавить в блок включений `tests/hotseat_test.cpp`:

```cpp
#include "avatar.h"
#include "game.h"
```

- [ ] **Step 2: Прогнать и убедиться, что не собирается**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: нет членов `player_slot_count`, `active_slot`, `player_at`.

- [ ] **Step 3: Ввести вектор слотов**

В `src/game.h` рядом с `pimpl<avatar> u_ptr;` (строка 1158) добавить:

```cpp
        // Player slots. Always at least one; slot 0 points at the pimpl-owned
        // avatar that game::u refers to and is not owned here. Ownership of
        // any additional avatar lives in owned_extra_players, which is why the
        // two vectors are separate rather than one vector of smart pointers:
        // std::unique_ptr bakes its deleter into the type, so a non-owning
        // slot 0 cannot share a vector with owning slots.
        std::vector<avatar *> player_slots;                        // NOLINT(cata-serialize)
        std::vector<std::unique_ptr<avatar>> owned_extra_players;  // NOLINT(cata-serialize)
        size_t active_slot_index = 0;                              // NOLINT(cata-serialize)
```

В публичную секцию `game` добавить:

```cpp
        size_t player_slot_count() const;
        size_t active_slot() const;
        void set_active_slot( size_t index );
        avatar &player_at( size_t index );
```

`std::unique_ptr` с неполным типом `avatar` требует внеклассового деструктора — он уже есть (`src/game.h:200`, `src/game.cpp:490`), поэтому дополнительных действий не нужно.

- [ ] **Step 4: Заполнить слот 0 в конструкторе**

В `game::game()` (`src/game.cpp`, тело конструктора после списка инициализации, рядом с `current_map.set( &m );`) добавить:

```cpp
    // Slot 0 aliases the pimpl-owned avatar that game::u refers to.
    // Ownership stays with u_ptr; this vector only observes.
    player_slots.push_back( &u );
```

- [ ] **Step 5: Реализовать аксессоры**

В `src/game.cpp`:

```cpp
size_t game::player_slot_count() const
{
    return player_slots.size();
}

size_t game::active_slot() const
{
    return active_slot_index;
}

void game::set_active_slot( size_t index )
{
    cata_assert( index < player_slots.size() );
    active_slot_index = index;
}

avatar &game::player_at( size_t index )
{
    cata_assert( index < player_slots.size() );
    return *player_slots[index];
}
```

Обход в этих аксессорах идёт по `player_slots` — вектору наблюдателей. `owned_extra_players` держит владение и никогда не обходится напрямую.

- [ ] **Step 6: Переключить глобальные аксессоры на активный слот**

Заменить тела трёх функций в `src/game.cpp:11523-11535`:

```cpp
Character &get_player_character()
{
    return g->player_at( g->active_slot() );
}

viewer &get_player_view()
{
    return g->player_at( g->active_slot() );
}

avatar &get_avatar()
{
    return g->player_at( g->active_slot() );
}
```

- [ ] **Step 7: Разобрать 14 прямых обращений `g->u`**

```bash
grep -rn "g->u\b" src --include=*.cpp --include=*.h
```

Для каждого решить: означает ли оно «первый игрок» (оставить) или «текущий игрок» (заменить на `get_avatar()`). Записать решение по каждому в сообщение коммита.

- [ ] **Step 8: Прогнать обе сборки полностью**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_a
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_b
```

Ожидается: одинаковое число пройденных тестов в обеих сборках, совпадающее с числом до начала задачи.

- [ ] **Step 9: Коммит**

```bash
git add src/game.h src/game.cpp tests/hotseat_test.cpp
git commit -m "refactor(game): introduce player slot vector, size one in single player"
```

---

## Task 3: Второй слот и переключение управления

**Files:**
- Modify: `src/game.h`, `src/game.cpp`
- Test: `tests/hotseat_test.cpp`

**Interfaces:**
- Consumes: `player_at()`, `set_active_slot()` из Task 2.
- Produces: `avatar &game::add_player_slot();`

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "second_slot_is_a_distinct_avatar", "[hotseat]" )
{
    REQUIRE( g->player_slot_count() == 1 );
    avatar &first = g->player_at( 0 );

    avatar &second = g->add_player_slot();
    REQUIRE( g->player_slot_count() == 2 );
    CHECK( &second != &first );

    g->set_active_slot( 1 );
    CHECK( &get_avatar() == &second );

    g->set_active_slot( 0 );
    CHECK( &get_avatar() == &first );
}
```

- [ ] **Step 2: Прогнать и убедиться, что не собирается**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

- [ ] **Step 3: Реализовать добавление слота**

В `src/game.cpp`:

```cpp
avatar &game::add_player_slot()
{
    owned_extra_players.push_back( std::make_unique<avatar>() );
    player_slots.push_back( owned_extra_players.back().get() );
    return *player_slots.back();
}
```

`owned_extra_players` растёт только добавлением в конец, поэтому указатели в `player_slots` не инвалидируются: `std::unique_ptr` перемещается при реаллокации вектора, но сам объект `avatar` остаётся на месте.

Объявить в публичной секции `src/game.h`.

- [ ] **Step 4: Прогнать тест**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
```

- [ ] **Step 5: Проверить, что второй аватар имеет собственную память карты**

Дописать тест:

```cpp
TEST_CASE( "second_slot_has_own_map_memory", "[hotseat]" )
{
    while( g->player_slot_count() > 1 ) {
        // Ensure a clean starting point regardless of test order.
        g->set_active_slot( 0 );
        break;
    }
    avatar &second = g->add_player_slot();
    second.clear_map_memory();
    CHECK( &second != &g->player_at( 0 ) );
}
```

`avatar::clear_map_memory()` объявлен в `src/avatar.h:114`, `player_map_memory` — в `src/avatar.h:439`, то есть память карты действительно принадлежит аватару, а не разделяется.

- [ ] **Step 6: Коммит**

```bash
git add src/game.h src/game.cpp tests/hotseat_test.cpp
git commit -m "feat(game): support additional player slots with independent avatars"
```

---

## Task 4: Сериализация нескольких игроков

**Files:**
- Modify: `src/savegame.cpp:147`, `src/savegame.cpp:314`
- Test: `tests/hotseat_test.cpp`

**Interfaces:**
- Consumes: `player_slot_count()`, `player_at()` из Task 2.
- Produces: ключ `"players"` в сейве, при сохранении обратной совместимости с ключом `"player"`.

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "extra_players_survive_a_save_round_trip", "[hotseat]" )
{
    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    const size_t expected = g->player_slot_count();

    std::ostringstream out;
    g->serialize_json( out );

    // The written document must carry the extra slot, and must keep slot 0
    // under the legacy key so a single player build can still open it.
    const std::string doc = out.str();
    CHECK( doc.find( "\"player\"" ) != std::string::npos );
    CHECK( doc.find( "extra_players" ) != std::string::npos );
    CHECK( doc.find( "SlotOneProbe" ) != std::string::npos );
    CHECK( expected == 2 );
}
```

`game::serialize_json( std::ostream & )` объявлен в `src/savegame.cpp:96` и пишет весь документ, поэтому тест проверяет настоящий путь сохранения, а не его копию. Добавить в включения `<sstream>` и `<string>`.

- [ ] **Step 2: Прочитать текущие точки сериализации**

```bash
sed -n '140,155p' src/savegame.cpp
sed -n '308,320p' src/savegame.cpp
```

- [ ] **Step 3: Писать массив слотов**

Заменить `json.member( "player", u );` (`src/savegame.cpp:147`) на:

```cpp
    // Slot 0 is written under the legacy "player" key so that a co-op save
    // remains loadable by a single player build, which reads only that key.
    json.member( "player", g->player_at( 0 ) );
    if( g->player_slot_count() > 1 ) {
        json.member( "extra_players" );
        json.start_array();
        for( size_t i = 1; i < g->player_slot_count(); ++i ) {
            json.write( g->player_at( i ) );
        }
        json.end_array();
    }
```

- [ ] **Step 4: Читать массив слотов**

После `data.read( "player", u );` (`src/savegame.cpp:314`) добавить:

```cpp
    if( data.has_member( "extra_players" ) ) {
        for( JsonValue jv : data.get_array( "extra_players" ) ) {
            jv.read( g->add_player_slot() );
        }
    }
```

Обе формы подтверждены соседним кодом того же файла: запись массива — `json.start_array()` / `json.write()` / `json.end_array()` (`src/savegame.cpp:154-163`), чтение через итерацию `JsonValue` по `get_array()` — `src/savegame.cpp:416` и `:318`.

- [ ] **Step 5: Проверить круглый рейс с двумя слотами**

Дописать тест, сохраняющий мир с двумя слотами и загружающий его обратно, проверяющий, что число слотов и имена персонажей совпали.

- [ ] **Step 6: Проверить, что одиночная сборка читает кооп-сейв**

```bash
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo
```

Одиночная сборка игнорирует ключ `extra_players` и видит одного персонажа. Это осознанное поведение, а не потеря данных: файл сохраняет второго игрока нетронутым, если одиночная сборка его не перезапишет. Записать это ограничение в отчёт Task 10.

- [ ] **Step 7: Коммит**

```bash
git add src/savegame.cpp tests/hotseat_test.cpp
git commit -m "feat(save): persist additional player slots alongside legacy key"
```

---

## Task 5: API доступа к игрокам для симуляции

**Files:**
- Modify: `src/game.h`, `src/game.cpp`
- Test: `tests/hotseat_sim_test.cpp`

**Interfaces:**
- Produces:
  - `std::vector<avatar *> game::get_players();`
  - `Character *game::nearest_player( const tripoint_abs_ms &pos );`
  - `bool game::any_player_sees( const tripoint_abs_ms &pos );`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/hotseat_sim_test.cpp`:

```cpp
#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <vector>

#include "avatar.h"
#include "coordinates.h"
#include "game.h"

TEST_CASE( "get_players_returns_every_slot", "[hotseat]" )
{
    const size_t before = g->player_slot_count();
    REQUIRE( g->get_players().size() == before );

    g->add_player_slot();
    CHECK( g->get_players().size() == before + 1 );
}

TEST_CASE( "nearest_player_picks_the_closer_slot", "[hotseat]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    map &here = get_map();

    // Slot positions are set explicitly so the assertion does not depend on
    // wherever the test harness left the avatars.
    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    a.setpos( here, tripoint_bub_ms( 40, 50, 0 ) );
    b.setpos( here, tripoint_bub_ms( 80, 50, 0 ) );

    CHECK( g->nearest_player( here.get_abs( tripoint_bub_ms( 45, 50, 0 ) ) ) == &a );
    CHECK( g->nearest_player( here.get_abs( tripoint_bub_ms( 75, 50, 0 ) ) ) == &b );
}
#endif
```

Форма `setpos( here, tripoint_bub_ms( x, y, z ) )` взята из `tests/behavior_test.cpp:1383`. Добавить в включения `"map.h"`, `"map_helpers_tests.h"`, `"player_helpers.h"` и `"type_id.h"`.

- [ ] **Step 2: Прогнать и убедиться, что не собирается**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

- [ ] **Step 3: Реализовать API**

```cpp
std::vector<avatar *> game::get_players()
{
    return player_slots;
}

Character *game::nearest_player( const tripoint_abs_ms &pos )
{
    Character *best = nullptr;
    int best_dist = std::numeric_limits<int>::max();
    // Slots are visited in index order, so ties resolve to the lower slot
    // deterministically.
    for( avatar *slot : player_slots ) {
        if( slot->is_dead_state() ) {
            continue;
        }
        const int d = rl_dist( slot->pos_abs(), pos );
        if( d < best_dist ) {
            best_dist = d;
            best = slot;
        }
    }
    return best;
}

bool game::any_player_sees( const tripoint_abs_ms &pos )
{
    map &here = get_map();
    // Creature::sees takes bubble coordinates (src/creature.h:407), so the
    // absolute position is converted once rather than per slot.
    const tripoint_bub_ms local = here.get_bub( pos );
    for( avatar *slot : player_slots ) {
        if( !slot->is_dead_state() && slot->sees( here, local ) ) {
            return true;
        }
    }
    return false;
}
```

Сигнатуры подтверждены: `Creature::sees( const map &here, const tripoint_bub_ms &t, bool is_avatar = false, ... )` — `src/creature.h:407`.

- [ ] **Step 4: Прогнать тест**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
```

- [ ] **Step 5: Коммит**

```bash
git add src/game.h src/game.cpp tests/hotseat_sim_test.cpp
git commit -m "feat(game): add multi-player accessors for simulation code"
```

---

## Task 6: Цикл хода hotseat

Одиночный `game::do_turn()` не трогается. Hotseat получает свой цикл, который дренирует moves обоих слотов в фиксированном порядке индексов, как требует §5.5 спека.

**Files:**
- Create: `src/mp/mp_hotseat_loop.h`, `src/mp/mp_hotseat_loop.cpp`
- Test: `tests/hotseat_sim_test.cpp`

**Interfaces:**
- Consumes: `player_at()`, `set_active_slot()`, `player_slot_count()`.
- Produces: `bool mp_hotseat_do_turn();`

- [ ] **Step 1: Прочитать одиночный цикл целиком перед копированием структуры**

```bash
sed -n '522,805p' src/do_turn.cpp
```

Цикл hotseat повторяет тот же порядок этапов, но блок обработки ввода (`src/do_turn.cpp:631-704`) заменяется на обход слотов.

- [ ] **Step 2: Написать падающий тест**

```cpp
TEST_CASE( "hotseat_turn_advances_calendar_once", "[hotseat]" )
{
    const int before = to_turn<int>( calendar::turn );
    mp_hotseat_do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}
```

Добавить включения `"calendar.h"` и `"mp/mp_hotseat_loop.h"`.

- [ ] **Step 3: Реализовать цикл**

`src/mp/mp_hotseat_loop.cpp` повторяет последовательность `game::do_turn()`, заменив блок ввода на:

```cpp
    // Slots act in index order, never in speed order, so the sequence is
    // reproducible and matches section 5.5 of the design spec.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->set_active_slot( i );
        avatar &who = g->player_at( i );
        while( who.get_moves() > 0 && who.activity ) {
            who.activity.do_turn( who );
        }
        while( who.get_moves() > 0 ) {
            if( !g->handle_action() ) {
                break;
            }
            who.action_taken();
        }
    }
    g->set_active_slot( 0 );
```

`game::handle_action()` объявлен публично и действует на `get_avatar()`, который после Task 2 возвращает активный слот — поэтому смена активного слота переключает, за кого читается ввод, без правки самого `handle_action`.

- [ ] **Step 4: Прогнать тест**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "hotseat_turn_advances_calendar_once" --user-dir test_user_dir_hs
```

- [ ] **Step 5: Проверить, что оба слота тратят moves за один ход**

Дописать тест, выставляющий обоим слотам ненулевые moves, прогоняющий один ход и проверяющий, что moves потрачены у обоих.

- [ ] **Step 6: Коммит**

```bash
git add src/mp/mp_hotseat_loop.h src/mp/mp_hotseat_loop.cpp tests/hotseat_sim_test.cpp
git commit -m "feat(mp): add hotseat turn loop draining both slots in index order"
```

---

## Task 7: Anchor бабла на середине между игроками

**Files:**
- Create: `src/mp/mp_tether.h`, `src/mp/mp_tether.cpp`
- Modify: `src/game.cpp:10132` (`game::vertical_shift`)
- Test: `tests/hotseat_sim_test.cpp`

**Interfaces:**
- Produces:
  - `tripoint_abs_ms mp_bubble_anchor();`

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "anchor_is_integer_midpoint", "[hotseat]" )
{
    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    a.setpos( get_map(), tripoint_bub_ms( 40, 40, 0 ) );
    b.setpos( get_map(), tripoint_bub_ms( 61, 40, 0 ) );

    const tripoint_abs_ms anchor = mp_bubble_anchor();
    // Odd spans round down, deterministically and without floating point.
    CHECK( anchor.x() == ( a.pos_abs().x() + b.pos_abs().x() ) / 2 );
    CHECK( anchor.z() == a.pos_abs().z() );
}
```

- [ ] **Step 2: Прогнать и убедиться, что не собирается**

- [ ] **Step 3: Реализовать anchor**

```cpp
tripoint_abs_ms mp_bubble_anchor()
{
    // Integer arithmetic only: floating point midpoints would differ between
    // builds, which section 7.1 of the spec forbids in simulation paths.
    int64_t sx = 0;
    int64_t sy = 0;
    int count = 0;
    for( avatar *who : g->get_players() ) {
        if( who->is_dead_state() ) {
            continue;
        }
        sx += who->pos_abs().x();
        sy += who->pos_abs().y();
        ++count;
    }
    if( count == 0 ) {
        return g->player_at( 0 ).pos_abs();
    }
    // Anchor z follows slot 0, per section 5.7 of the spec.
    return tripoint_abs_ms( static_cast<int>( sx / count ),
                            static_cast<int>( sy / count ),
                            g->player_at( 0 ).pos_abs().z() );
}
```

- [ ] **Step 4: Центрировать бабл на anchor в цикле hotseat**

В `src/mp/mp_hotseat_loop.cpp` перед обходом слотов вызвать `game::update_map( int &x, int &y, bool )` (`src/game.h:704`) с координатами anchor. Эта перегрузка уже принимает координаты, поэтому новый API карте не нужен.

- [ ] **Step 5: Расцепить `vertical_shift`**

`game::vertical_shift` (`src/game.cpp:10132`) сейчас делает две вещи: двигает `u` и вызывает `map::vertical_shift`. Выделить сдвиг карты в отдельный метод, чтобы цикл hotseat мог сдвинуть бабл, не двигая ни одного персонажа. Одиночный `vertical_shift` после этого вызывает новый метод и сохраняет прежнее поведение — проверяется прогоном 266 тестов.

- [ ] **Step 6: Прогнать обе сборки**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo
```

- [ ] **Step 7: Коммит**

```bash
git add src/mp/mp_tether.h src/mp/mp_tether.cpp src/mp/mp_hotseat_loop.cpp src/game.h src/game.cpp tests/hotseat_sim_test.cpp
git commit -m "feat(mp): centre the reality bubble on the integer midpoint of players"
```

---

## Task 8: Тезер

Пределы из §5.7 спека: мягкий 32 тайла от anchor, жёсткий 42.

**Files:**
- Modify: `src/mp/mp_tether.h`, `src/mp/mp_tether.cpp`
- Test: `tests/hotseat_sim_test.cpp`

**Interfaces:**
- Produces:
  - `enum class tether_state { ok, warning, blocked };`
  - `tether_state mp_tether_check( const tripoint_abs_ms &proposed );`

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "tether_thresholds_match_the_spec", "[hotseat]" )
{
    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    a.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );
    b.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );

    const tripoint_abs_ms anchor = mp_bubble_anchor();
    CHECK( mp_tether_check( anchor ) == tether_state::ok );
    CHECK( mp_tether_check( anchor + tripoint_rel_ms( 31, 0, 0 ) ) == tether_state::ok );
    CHECK( mp_tether_check( anchor + tripoint_rel_ms( 33, 0, 0 ) ) == tether_state::warning );
    CHECK( mp_tether_check( anchor + tripoint_rel_ms( 43, 0, 0 ) ) == tether_state::blocked );
}
```

- [ ] **Step 2: Прогнать и убедиться, что не собирается**

- [ ] **Step 3: Реализовать проверку**

```cpp
// Section 5.7 of the design spec: the bubble is 132x132 tiles, so +/-66 from
// the anchor. Twenty four tiles of margin are reserved because monsters and
// fields at the bubble edge are not simulated correctly, leaving 42.
static constexpr int tether_soft_limit = 32;
static constexpr int tether_hard_limit = 42;

tether_state mp_tether_check( const tripoint_abs_ms &proposed )
{
    const int d = rl_dist( mp_bubble_anchor(), proposed );
    if( d > tether_hard_limit ) {
        return tether_state::blocked;
    }
    if( d > tether_soft_limit ) {
        return tether_state::warning;
    }
    return tether_state::ok;
}
```

- [ ] **Step 4: Подключить проверку к движению**

В цикле hotseat перед применением движения активного слота проверить `mp_tether_check` для целевой клетки. При `blocked` движение отменяется с сообщением, при `warning` выводится предупреждение и указатель направления к напарнику. Движение перехватывается в цикле, а не в `avatar_action::move`, чтобы одиночный путь остался нетронутым.

- [ ] **Step 5: Прогнать тесты и закоммитить**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
git add src/mp/mp_tether.h src/mp/mp_tether.cpp src/mp/mp_hotseat_loop.cpp tests/hotseat_sim_test.cpp
git commit -m "feat(mp): enforce tether limits around the bubble anchor"
```

---

## Task 9: Миграция боевого ядра симуляции (Ф6)

Это фаза Ф6 из спека: перевести симуляционные вызовы «тот самый игрок» на «любой или ближайший игрок». Объём по grep — около 190 вызовов всего, из них боевое ядро около 70.

**Files:**
- Modify: `src/monattack.cpp`, `src/monmove.cpp`, `src/monster.cpp`, `src/npcmove.cpp`, `src/npc.cpp`, `src/game.cpp`
- Test: `tests/hotseat_sim_test.cpp`

- [ ] **Step 1: Собрать актуальный список вызовов**

```bash
for f in monattack.cpp monmove.cpp monster.cpp npcmove.cpp npc.cpp game.cpp map.cpp creature.cpp weather.cpp overmapbuffer.cpp mapgen.cpp character.cpp; do
  echo "== $f"
  grep -n "get_avatar()\|get_player_character()\|get_player_view()" src/$f
done > /tmp/player_sites.txt
wc -l /tmp/player_sites.txt
```

- [ ] **Step 2: Классифицировать каждый вызов**

Три класса, записать прямо в файл списка:

- **U (ui)** — сообщение, отрисовка, запрос. Означает локального игрока. Оставить.
- **N (nearest)** — расстояние, путь, цель атаки. Заменить на `nearest_player()`.
- **A (any)** — видимость, срабатывание триггера, спавн. Заменить на `any_player_sees()` или обход `get_players()`.

- [ ] **Step 3: Написать падающий тест на агрессию к обоим игрокам**

```cpp
TEST_CASE( "monster_targets_the_closer_player", "[hotseat]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    a.setpos( get_map(), tripoint_bub_ms( 40, 50, 0 ) );
    b.setpos( get_map(), tripoint_bub_ms( 60, 50, 0 ) );

    monster &z = spawn_test_monster( "mon_zombie", tripoint_bub_ms( 58, 50, 0 ) );
    z.plan();

    // Before the migration the monster ignores slot 1 entirely and walks
    // toward slot 0, which is twenty tiles further away.
    REQUIRE( g->nearest_player( z.pos_abs() ) == &b );
    CHECK( rl_dist( z.get_dest(), b.pos_abs() ) < rl_dist( z.get_dest(), a.pos_abs() ) );
}
```

`monster::get_dest()` объявлен в `src/monster.h:239` и возвращает `tripoint_abs_ms`, поэтому сравнение расстояний выше корректно как написано.

- [ ] **Step 4: Прогнать тест и убедиться, что он падает**

Ожидается: монстр идёт к слоту 0, потому что планирование опирается на `get_avatar()`.

- [ ] **Step 5: Мигрировать класс N в `monmove.cpp` и `monster.cpp`**

Заменить вызовы, классифицированные как N, на `g->nearest_player( pos_abs() )`, проверяя `nullptr`.

- [ ] **Step 6: Прогнать тест и убедиться, что он проходит**

- [ ] **Step 7: Мигрировать класс N в `monattack.cpp`**

- [ ] **Step 8: Мигрировать класс A**

- [ ] **Step 9: Пометить непереведённое лит-гардом**

Для оставшихся вызовов в симуляционных путях ввести `get_avatar_legacy()` — тонкую обёртку над `get_avatar()` — и заменить их на неё, чтобы новые вхождения `get_avatar()` в симуляции были заметны на ревью. Записать в отчёт Task 10 точное число оставшихся.

- [ ] **Step 10: Прогнать обе сборки полностью и закоммитить**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_a
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_b
git add src tests
git commit -m "refactor(sim): target and perceive every player slot, not just slot zero"
```

---

## Task 10: Игровая проверка и отчёт по воротам

**Files:**
- Create: `docs/superpowers/reports/2026-07-31-mvp2-hotseat-findings.md`

- [ ] **Step 1: Сыграть сессию**

Собрать с `MULTIPLAYER=1`, создать мир, добавить второй слот, сыграть не менее часа: бой с обоими персонажами, крафт одним при охране вторым, расхождение до жёсткого предела, спуск одного на другой z-уровень.

- [ ] **Step 2: Записать наблюдения по модели хода**

Ответить письменно: комфортен ли порядок «слот 0 полностью, затем слот 1»; заметна ли потеря темпа; работает ли тезер по ощущениям или мешает.

Это прямая проверка §5.5 и §5.6 спека до вложений в сеть.

- [ ] **Step 3: Записать список найденных дефектов «мир видит только слот 0»**

Каждый случай, когда мир отреагировал только на первого игрока, — это недомигрированный вызов из Task 9. Перечислить с воспроизведением.

- [ ] **Step 4: Написать отчёт**

Разделы, все обязательны:

1. Что собиралось и как запускалось.
2. Сколько вызовов мигрировано в Task 9 и сколько осталось под `get_avatar_legacy()`.
3. Наблюдения по модели хода — ответ на Step 2.
4. Найденные дефекты «мир видит только слот 0».
5. Ограничение сейвов: одиночная сборка игнорирует `extra_players`.
6. Оценка Ф4 и Ф6 по факту против заложенных в спеке 1-2 и 3-4 недель.
7. Решение по воротам: переходить к сетевой части, либо пересматривать §5.5 и §5.6, если модель хода оказалась неиграбельной.

- [ ] **Step 5: Коммит**

```bash
git add docs/superpowers/reports/2026-07-31-mvp2-hotseat-findings.md
git commit -m "docs: record MVP-2 hotseat findings and gate decision"
```
