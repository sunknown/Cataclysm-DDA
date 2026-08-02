# Debug-переключатель hotseat-коопа — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Дать возможность включить hotseat-режим прямо во время игры через debug-меню, чтобы Task 10 родительского плана (плейтест не менее часа) стало физически выполнимо.

**Architecture:** Тестируемая логика (создание второго слота + переключение флага режима) живёт в `debug_menu.h`/`debug_menu.cpp` как отдельная функция без UI. UI-обёртка строит `uilist` выбора шаблона персонажа и регистрируется в двух существующих таблицах debug-меню (категория «Player» и глобальный `all_actions()` для поиска). Игровой цикл в `main.cpp` ветвится между `game::do_turn()` и уже существующим `mp_hotseat_do_turn()` по флагу `mp_mode_active()`.

**Tech Stack:** C++17, Catch2 (`tests/cata_catch.h`), GNU Make.

## Global Constraints

- Одиночный игровой путь не меняется по поведению. Весь новый код — под `#if defined(MULTIPLAYER)`, включая объявления в заголовках (`debug_menu.h`), а не только определения — иначе линковка default-сборки сломается на неразрешённых символах `mp_mode_active`/`mp_mode_set_active`/`mp_hotseat_do_turn` (эти символы существуют только когда `src/mp/*.cpp` попадает в сборку, см. Makefile).
- Сборка: `make -j$(nproc) TILES=0 TESTS=1` для одиночной, `make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1` для режима. **`make clean` обязателен перед каждым переключением флага MULTIPLAYER** — `make` не отслеживает изменение `-D`-флагов, только текст исходников, и инкрементальная сборка после переключения флага может незаметно подсунуть устаревшие объектники другой конфигурации (задокументированный урок из Task 9).
- Established-baseline после Task 9: default-сборка полного набора тестов — **1828 test cases, 28988647 assertions**; MULTIPLAYER-сборка, фильтр `[hotseat],[npc],[monster]` — **197 test cases, 62160 assertions**; `tests/cata_test --list-tests "[hotseat]"` — **13** тест-кейсов до этого плана (после него — 14, см. Task 1).
- Второй слот создаётся не более одного раза за игровую сессию — при повторном включении режима после выключения новый слот не создаётся.

---

## Task 1: Тестируемая логика переключения

**Files:**
- Modify: `src/debug_menu.h` (добавить объявление после `map_reveal`, строка 71)
- Modify: `src/debug_menu.cpp` (добавить определение, добавить `#include "mp/mp_mode.h"` и `#include "player_difficulty.h"`)
- Test: `tests/hotseat_test.cpp`

**Interfaces:**
- Produces:
  - `void debug_menu::toggle_hotseat( const std::string &template_name );` — пустая строка `template_name` означает «голый аватар». Если режим уже включён, аргумент игнорируется и режим выключается.

- [ ] **Step 1: Написать падающий тест**

Добавить в конец `tests/hotseat_test.cpp` (после существующего `TEST_CASE( "single_player_build_sees_only_slot_zero", ... )`):

```cpp
#if defined(MULTIPLAYER)
#include "debug_menu.h"
#include "mp/mp_mode.h"

TEST_CASE( "hotseat_debug_toggle_creates_slot_and_enables_mode", "[hotseat]" )
{
    game_test_helper::reset_player_slots();
    mp_mode_set_active( false );

    debug_menu::toggle_hotseat( "" );
    CHECK( g->player_slot_count() == 2 );
    CHECK( mp_mode_active() );

    // Toggling again while already active must not create a third slot.
    debug_menu::toggle_hotseat( "" );
    CHECK( g->player_slot_count() == 2 );
    CHECK_FALSE( mp_mode_active() );

    // Re-enabling after a disable must not create a third slot either.
    debug_menu::toggle_hotseat( "" );
    CHECK( g->player_slot_count() == 2 );
    CHECK( mp_mode_active() );

    mp_mode_set_active( false );
    game_test_helper::reset_player_slots();
}
#endif
```

- [ ] **Step 2: Прогнать и убедиться, что сборка падает**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: ошибка компиляции — нет `debug_menu::toggle_hotseat`.

- [ ] **Step 3: Добавить объявление в заголовок**

В `src/debug_menu.h` после строки `void map_reveal( int reveal_level_int, const std::optional<tripoint_abs_omt> &p = std::nullopt );` (строка 71) добавить:

```cpp
#if defined(MULTIPLAYER)
// Creates the second hotseat player slot (once per session, from a saved
// character template if template_name names one, or bare if empty) and
// enables hotseat mode. If hotseat mode is already active, disables it
// instead and template_name is ignored. Safe to call repeatedly: a second
// slot is never created more than once per session.
void toggle_hotseat( const std::string &template_name );
#endif
```

- [ ] **Step 4: Реализовать функцию**

В `src/debug_menu.cpp` добавить рядом с прочими `#include` (после `#include "path_info.h"`, строка 114):

```cpp
#include "mp/mp_mode.h"
#include "player_difficulty.h"
```

Добавить определение (рядом с другими небольшими debug-функциями, например после `control_npc_menu()`, строка ~1957):

```cpp
#if defined(MULTIPLAYER)
void toggle_hotseat( const std::string &template_name )
{
    if( mp_mode_active() ) {
        mp_mode_set_active( false );
        add_msg( m_info, _( "Hotseat co-op disabled." ) );
        return;
    }

    if( g->player_slot_count() == 1 ) {
        avatar &second = g->add_player_slot();
        if( !template_name.empty() ) {
            pool_type pool;
            if( !second.load_template( template_name, pool ) ) {
                popup( _( "Could not load template \"%s\"; slot 2 stays a bare avatar." ),
                       template_name );
            }
        }
    }

    mp_mode_set_active( true );
    add_msg( m_good, _( "Hotseat co-op enabled." ) );
}
#endif
```

- [ ] **Step 5: Прогнать тест**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat]" --user-dir test_user_dir_hs
```

Ожидается: `tests/cata_test --list-tests "[hotseat]"` показывает 14 тест-кейсов; новый тест проходит; остальные 13 остаются зелёными.

- [ ] **Step 6: Прогнать полный набор в обеих сборках**

```bash
tests/cata_test "[hotseat],[npc],[monster]" --rng-seed 1 --user-dir test_user_dir_hs2
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo
```

Ожидается: MULTIPLAYER-фильтр `[hotseat],[npc],[monster]` — **198 test cases / 62166 assertions** (established baseline 197/62160 плюс новый тест: он несёт 6 `CHECK`/`CHECK_FALSE`, добавляет ровно 1 тест-кейс и 6 assertions — если числа разошлись сильнее, разбираться, а не считать зелёным). Default-сборка — 1828 test cases / 28988647 assertions, без изменений (новый код целиком под `#if defined(MULTIPLAYER)`, в default-сборке не компилируется).

- [ ] **Step 7: Коммит**

```bash
git add src/debug_menu.h src/debug_menu.cpp tests/hotseat_test.cpp
git commit -m "feat(mp): add testable hotseat toggle logic to debug_menu"
```

---

## Task 2: Регистрация в debug-меню

**Files:**
- Modify: `src/debug_menu_types.h` (новое значение enum)
- Modify: `src/debug_menu.cpp` (строка в `enum_to_string`, запись в `player_uilist()`, запись в `all_actions()`, UI-обёртка с выбором шаблона)

**Interfaces:**
- Consumes: `debug_menu::toggle_hotseat( const std::string & )` из Task 1.
- Produces: пункт «Toggle hotseat co-op» в категории «Player» debug-меню и в глобальном поиске действий.

- [ ] **Step 1: Добавить значение в enum**

В `src/debug_menu_types.h`, перед завершающим `last,` (строка 118), добавить:

```cpp
#if defined(MULTIPLAYER)
    TOGGLE_HOTSEAT,
#endif
```

- [ ] **Step 2: Прогнать и убедиться, что сборка падает**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
```

Ожидается: предупреждение/ошибка о неполном `switch` в `enum_to_string` (строка ~200 `src/debug_menu.cpp`) — сборка использует `-Werror`, так что это станет ошибкой компиляции.

- [ ] **Step 3: Добавить строковое представление**

В `src/debug_menu.cpp`, в функции `enum_to_string<debug_menu::debug_menu_index>`, перед `case debug_menu::debug_menu_index::last:` (строка 311) добавить:

```cpp
#if defined(MULTIPLAYER)
        case debug_menu::debug_menu_index::TOGGLE_HOTSEAT: return "TOGGLE_HOTSEAT";
#endif
```

- [ ] **Step 4: Написать UI-обёртку с выбором шаблона**

В `src/debug_menu.cpp`, рядом с определением `toggle_hotseat()` из Task 1, добавить статическую функцию-обёртку:

```cpp
#if defined(MULTIPLAYER)
static void toggle_hotseat_menu()
{
    if( mp_mode_active() || g->player_slot_count() > 1 ) {
        toggle_hotseat( "" );
        return;
    }

    std::vector<std::string> templates;
    for( std::string path : get_files_from_path( ".template", PATH_INFO::templatedir(), false,
            true ) ) {
        path.erase( path.find( ".template" ), std::string::npos );
        path.erase( 0, path.find_last_of( "\\/" ) + 1 );
        templates.push_back( path );
    }
    std::sort( templates.begin(), templates.end(), localized_compare );

    uilist smenu;
    smenu.text = _( "Start slot 2 as:" );
    smenu.addentry( 0, true, MENU_AUTOASSIGN, _( "Bare avatar (no gear)" ) );
    for( size_t i = 0; i < templates.size(); ++i ) {
        smenu.addentry( static_cast<int>( i ) + 1, true, MENU_AUTOASSIGN, templates[i] );
    }
    smenu.query();

    if( smenu.ret < 0 ) {
        return;
    }
    toggle_hotseat( smenu.ret == 0 ? "" : templates[smenu.ret - 1] );
}
#endif
```

`MENU_AUTOASSIGN` и `localized_compare` уже используются в этом файле для аналогичных списков (например `src/debug_menu.cpp:815`, `:1819`), `translation.h` уже включён (`src/debug_menu.cpp:131`) — дополнительных `#include` не требуется.

- [ ] **Step 5: Зарегистрировать в категории «Player»**

В `src/debug_menu.cpp`, в `player_uilist()` (строка 615), в `uilist_initializer` после записи `CONTROL_NPC` (строка 629) добавить:

```cpp
#if defined(MULTIPLAYER)
        { uilist_entry( debug_menu_index::TOGGLE_HOTSEAT, true, 'H', _( "Toggle hotseat co-op" ) ) },
#endif
```

- [ ] **Step 6: Зарегистрировать в `all_actions()`**

В `src/debug_menu.cpp`, в таблице `all_actions()` (строка 4130), после записи `CONTROL_NPC` (строка ~4232) добавить:

```cpp
#if defined(MULTIPLAYER)
        {
            debug_menu_index::TOGGLE_HOTSEAT, translate_marker( "Toggle hotseat co-op" ), "hotseat multiplayer coop toggle", "Player", []()
            {
                toggle_hotseat_menu();
            }
        },
#endif
```

- [ ] **Step 7: Прогнать обе сборки**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat],[npc],[monster]" --rng-seed 1 --user-dir test_user_dir_hs3
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo2
```

Ожидается: оба набора зелёные, числа совпадают с Task 1 Step 6 (**198 test cases / 62166 assertions** для MULTIPLAYER-фильтра, **1828 / 28988647** для default) — этот таск не меняет тестируемое поведение, только регистрирует UI поверх уже протестированной логики.

- [ ] **Step 8: Коммит**

```bash
git add src/debug_menu_types.h src/debug_menu.cpp
git commit -m "feat(mp): register hotseat toggle in the debug menu"
```

---

## Task 3: Ветвление игрового цикла

**Files:**
- Modify: `src/main.cpp:877`

**Interfaces:**
- Consumes: `bool mp_mode_active()` (`src/mp/mp_mode.h`), `bool mp_hotseat_do_turn()` (`src/mp/mp_hotseat_loop.h`) — уже существуют с Tasks 1 и 6 родительского плана.

- [ ] **Step 1: Прочитать текущий цикл**

```bash
sed -n '868,878p' src/main.cpp
```

Текущий код (строка 877):

```cpp
        while( !g->do_turn() ) {}
```

- [ ] **Step 2: Добавить включения**

В `src/main.cpp`, рядом с `#include "game.h"` (строка 46), добавить:

```cpp
#if defined(MULTIPLAYER)
#include "mp/mp_hotseat_loop.h"
#include "mp/mp_mode.h"
#endif
```

- [ ] **Step 3: Заменить цикл**

Заменить `while( !g->do_turn() ) {}` (строка 877) на:

```cpp
        while( true ) {
            bool done;
#if defined(MULTIPLAYER)
            done = mp_mode_active() ? mp_hotseat_do_turn() : g->do_turn();
#else
            done = g->do_turn();
#endif
            if( done ) {
                break;
            }
        }
```

- [ ] **Step 4: Прогнать обе сборки полностью**

```bash
make -j$(nproc) TILES=0 TESTS=1 MULTIPLAYER=1
tests/cata_test "[hotseat],[npc],[monster]" --rng-seed 1 --user-dir test_user_dir_hs4
make clean && make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_solo3
```

Ожидается: оба набора зелёные, числа совпадают с Task 2 Step 7 (**198 test cases / 62166 assertions** MULTIPLAYER, **1828 / 28988647** default) — изменение находится в `main()`, который не покрыт юнит-тестами, так что единственная проверка здесь — что ничего не сломалось на уровне сборки и существующего поведения; ручная проверка — Step 5.

- [ ] **Step 5: Ручная smoke-проверка**

```bash
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --user-dir test_user_dir_manual
```

Запустить обычную одиночную игру (`./cataclysm-tiles` или `./cataclysm` из корня репозитория, собранного на Step 4 без MULTIPLAYER) и убедиться, что игра запускается и ход обрабатывается как обычно — цикл в `main.cpp` при выключенном MULTIPLAYER компилируется в тот же `g->do_turn()`, что и раньше, но стоит визуально подтвердить перед тем, как считать Task 10 родительского плана открытым.

- [ ] **Step 6: Коммит**

```bash
git add src/main.cpp
git commit -m "feat(mp): branch the main turn loop into hotseat mode when active"
```

---

## После завершения

Это разблокирует Task 10 родительского плана (`docs/superpowers/plans/2026-07-31-mvp2-hotseat-coop.md`): собрать с `MULTIPLAYER=1`, запустить игру, открыть debug-меню (`~` по умолчанию) → Player → Toggle hotseat co-op, выбрать шаблон или голого аватара, играть.
