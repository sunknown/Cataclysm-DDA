# MVP-0: проба детерминизма — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Измерить, детерминирована ли симуляция мира Cataclysm-DDA при фиксированном сиде, и во сколько обходится починка найденных расхождений.

**Architecture:** Счётчик вызовов RNG добавляется в `src/rng.cpp` — это единственное изменение в игровом коде, и оно поведенчески инертно. Всё остальное живёт в `tests/` как диагностика: хеш состояния мира, лог по ходам, сценарий на 500 ходов и скрипт сравнения логов. Сценарий гоняет **настоящий** `game::do_turn()`, а не его имитацию: спящий аватар пропускает весь блок ввода (`src/do_turn.cpp:631`), поэтому цикл крутится headless без единого нажатия клавиши.

**Tech Stack:** C++17, Catch2 (`tests/cata_catch.h`), GNU Make, Python 3 для сравнения логов.

## Global Constraints

- Одиночный игровой путь не меняется по поведению. Единственная правка в `src/` — инкремент счётчика в `rng_get_engine()`.
- Код пробы живёт в `tests/`, не в `src/mp/`. Слой `src/mp/` появляется на Ф0, не здесь.
- Хеш состояния соответствует §7.2 спека: номер хода, счётчик вызовов RNG, состояние RNG, затем по всем существам в стабильном порядке — позиция, здоровье, moves.
- Тест помечается тегом `[.]`, чтобы не попадать в обычные прогоны CI: `tests/Makefile:76` исключает `[.]` из батчей.
- Файлы тестов подхватываются сборкой автоматически (`tests/Makefile:7` — `wildcard *.cpp`, `tests/CMakeLists.txt:4` — `file(GLOB ...)`). Правки в файлы сборки не нужны.
- Сборка: `make -j$(nproc) TILES=0 TESTS=1`. Бинарь: `tests/cata_test`, запускается из корня репозитория.
- Спек: `docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md`, раздел 9.1.

### Два сознательных отступления от формулировок спека

**Имя функции.** Спек §9.1 называет функцию `mp_state_hash()`. В MVP-0 она называется `sample_world_state()` и возвращает структуру, а не одно число, потому что для диагностики нужны отдельные компоненты — по ним видно, что именно разошлось. При переезде в `src/mp/` на Ф0 она получит имя `mp_state_hash()` и одночисловой вариант поверх поля `combined`.

**Фиксированный сейв.** Спек §9.1 говорит о фиксированном сейве. План вместо этого генерирует карту через `build_test_map( ter_id( "t_pavement" ) )` с фиксированным сидом и расставляет существ по жёстко заданным координатам. Причина: файла-фикстуры сейва в репозитории нет, а сгенерированная при фиксированном сиде карта воспроизводима строже, чем сейв, и не требует поддерживать бинарный артефакт в git. Если проба покажет расхождение уже на этапе генерации карты, это тоже результат и записывается в отчёт.

---

## File Structure

| Файл | Ответственность |
|---|---|
| `src/rng.h` (правка) | Объявление `rng_call_count()` и `rng_reset_call_count()` |
| `src/rng.cpp` (правка) | Счётчик вызовов `rng_get_engine()` |
| `tests/determinism_probe.h` (создать) | Тип `determinism_sample`, объявления `sample_world_state()` и `write_sample_log()` |
| `tests/determinism_probe.cpp` (создать) | FNV-1a, снятие состояния мира, запись CSV-лога |
| `tests/determinism_probe_test.cpp` (создать) | Юнит-тесты хеша и сам сценарий на 500 ходов |
| `tools/determinism_probe/compare_logs.py` (создать) | Сравнение двух логов, отчёт о первом расхождении |
| `docs/superpowers/reports/2026-07-31-mvp0-determinism-findings.md` (создать в Task 6) | Результат пробы и оценка стоимости починки — артефакт ворот |

---

## Task 1: Счётчик вызовов RNG

Счётчик считает обращения к `rng_get_engine()`. Каждая функция из `rng.h` обращается к движку ровно один раз за логический бросок (`src/rng.cpp:17`, `:26`, `:36`), поэтому счётчик измеряет именно число логических использований RNG — то, что нужно для диагностики «кто съел лишнее число».

**Files:**
- Modify: `src/rng.h:28-33`
- Modify: `src/rng.cpp:207-213`
- Test: `tests/determinism_probe_test.cpp`

**Interfaces:**
- Consumes: ничего.
- Produces:
  - `uint64_t rng_call_count();`
  - `void rng_reset_call_count();`

- [ ] **Step 1: Написать падающий тест**

Создать `tests/determinism_probe_test.cpp`:

```cpp
#include <cstdint>

#include "cata_catch.h"
#include "rng.h"

TEST_CASE( "rng_call_counter_increments", "[.][determinism_probe]" )
{
    rng_reset_call_count();
    REQUIRE( rng_call_count() == 0 );

    rng( 0, 100 );
    CHECK( rng_call_count() == 1 );

    rng( 0, 100 );
    rng_bits();
    CHECK( rng_call_count() == 3 );

    rng_reset_call_count();
    CHECK( rng_call_count() == 0 );
}
```

- [ ] **Step 2: Прогнать тест и убедиться, что он не собирается**

```bash
make -j$(nproc) TILES=0 TESTS=1
```

Ожидается: ошибка компиляции — `rng_call_count` и `rng_reset_call_count` не объявлены в `rng.h`.

- [ ] **Step 3: Объявить счётчик в заголовке**

В `src/rng.h`, сразу после объявления `rng_get_engine()` (строка 32), добавить:

```cpp
// Number of times rng_get_engine() has been called since the last reset.
// Every logical random draw touches the engine exactly once, so this counts
// logical RNG uses. Used by the determinism probe to locate the exact turn on
// which two runs consumed a different number of random values.
uint64_t rng_call_count();
void rng_reset_call_count();
```

В список включений `src/rng.h` (строки 5-14) добавить `#include <cstdint>`, сохранив алфавитный порядок — он идёт после `#include <array>`.

- [ ] **Step 4: Реализовать счётчик**

В `src/rng.cpp` заменить функцию `rng_get_engine()` (строки 207-212) на:

```cpp
static uint64_t rng_calls = 0;

uint64_t rng_call_count()
{
    return rng_calls;
}

void rng_reset_call_count()
{
    rng_calls = 0;
}

cata_default_random_engine &rng_get_engine()
{
    ++rng_calls;
    // NOLINTNEXTLINE(cata-determinism)
    static cata_default_random_engine eng( rng_get_first_seed() );
    return eng;
}
```

В список включений `src/rng.cpp` (строки 3-5) добавить `#include <cstdint>` перед `#include <cmath>`.

- [ ] **Step 5: Прогнать тест и убедиться, что он проходит**

```bash
make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "[determinism_probe]" --user-dir test_user_dir_probe
```

Ожидается: PASS.

- [ ] **Step 6: Убедиться, что одиночная игра не задета**

```bash
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_regress
```

Ожидается: столько же пройденных тестов, сколько до правки. Счётчик поведенчески инертен — он только инкрементируется и никем ещё не читается.

- [ ] **Step 7: Коммит**

```bash
git add src/rng.h src/rng.cpp tests/determinism_probe_test.cpp
git commit -m "feat(rng): count calls to rng_get_engine for determinism probe"
```

---

## Task 2: Хеш состояния мира

**Files:**
- Create: `tests/determinism_probe.h`
- Create: `tests/determinism_probe.cpp`
- Test: `tests/determinism_probe_test.cpp`

**Interfaces:**
- Consumes: `rng_call_count()` из Task 1.
- Produces:
  - `struct determinism_sample { int turn; uint64_t rng_calls; uint64_t rng_state; uint64_t creature_hash; uint64_t player_hash; uint64_t combined; };`
  - `determinism_sample sample_world_state();`
  - `uint64_t fnv1a_u64( uint64_t hash, uint64_t value );`

Порядок обхода существ берётся из `game::all_monsters()` (`src/game.h:555`), который построен над `creature_tracker::get_monsters_list()` (`src/creature_tracker.h:148`) — это `std::vector`, то есть порядок воспроизводим. Обход `monsters_by_location` (`:150`, `unordered_map`) в хеше не используется намеренно: именно он под подозрением, и хеш обязан быть измерительным прибором, а не соучастником.

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/determinism_probe_test.cpp`:

```cpp
#include "determinism_probe.h"

TEST_CASE( "fnv1a_is_order_sensitive", "[.][determinism_probe]" )
{
    const uint64_t seed = 1469598103934665603ULL;
    CHECK( fnv1a_u64( fnv1a_u64( seed, 1 ), 2 ) != fnv1a_u64( fnv1a_u64( seed, 2 ), 1 ) );
    CHECK( fnv1a_u64( seed, 7 ) == fnv1a_u64( seed, 7 ) );
}

TEST_CASE( "sample_world_state_reports_rng_counter", "[.][determinism_probe]" )
{
    rng_reset_call_count();
    rng( 0, 10 );
    rng( 0, 10 );

    const determinism_sample s = sample_world_state();
    CHECK( s.rng_calls == 2 );
    CHECK( s.turn == to_turn<int>( calendar::turn ) );
    CHECK( s.combined != 0 );
}
```

Дописать в блок включений этого файла:

```cpp
#include "calendar.h"
```

- [ ] **Step 2: Прогнать тест и убедиться, что он не собирается**

```bash
make -j$(nproc) TILES=0 TESTS=1
```

Ожидается: ошибка компиляции — нет файла `determinism_probe.h`.

- [ ] **Step 3: Создать заголовок**

`tests/determinism_probe.h`:

```cpp
#pragma once
#ifndef CATA_TESTS_DETERMINISM_PROBE_H
#define CATA_TESTS_DETERMINISM_PROBE_H

#include <cstdint>
#include <string>
#include <vector>

// One per-turn observation of world state, as defined by section 7.2 of
// docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md
struct determinism_sample {
    int turn = 0;
    uint64_t rng_calls = 0;
    uint64_t rng_state = 0;
    uint64_t creature_hash = 0;
    uint64_t player_hash = 0;
    uint64_t combined = 0;
};

// FNV-1a over the eight bytes of value. Order sensitive by construction.
uint64_t fnv1a_u64( uint64_t hash, uint64_t value );

// Snapshot the current world. Creatures are visited via game::all_monsters(),
// which iterates a std::vector and is therefore reproducible; the unordered
// monsters_by_location map is deliberately not used.
determinism_sample sample_world_state();

// Write samples as CSV with a header line, for tools/determinism_probe/compare_logs.py
void write_sample_log( const std::string &path, const std::vector<determinism_sample> &samples );

#endif // CATA_TESTS_DETERMINISM_PROBE_H
```

- [ ] **Step 4: Реализовать хеш и снятие состояния**

`tests/determinism_probe.cpp`:

```cpp
#include "determinism_probe.h"

#include <fstream>
#include <sstream>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "coordinates.h"
#include "creature.h"
#include "game.h"
#include "monster.h"
#include "npc.h"
#include "rng.h"

static constexpr uint64_t fnv_offset_basis = 1469598103934665603ULL;
static constexpr uint64_t fnv_prime = 1099511628211ULL;

uint64_t fnv1a_u64( uint64_t hash, uint64_t value )
{
    for( int i = 0; i < 8; ++i ) {
        hash ^= ( value >> ( i * 8 ) ) & 0xFFULL;
        hash *= fnv_prime;
    }
    return hash;
}

// Signed values are folded through a two's-complement-stable conversion so the
// hash does not depend on implementation-defined signed shifts.
static uint64_t as_u64( int v )
{
    return static_cast<uint64_t>( static_cast<int64_t>( v ) );
}

static uint64_t hash_creature( uint64_t h, const Creature &c )
{
    const tripoint_abs_ms p = c.pos_abs();
    h = fnv1a_u64( h, as_u64( p.x() ) );
    h = fnv1a_u64( h, as_u64( p.y() ) );
    h = fnv1a_u64( h, as_u64( p.z() ) );
    h = fnv1a_u64( h, as_u64( c.get_hp() ) );
    h = fnv1a_u64( h, as_u64( c.get_moves() ) );
    return h;
}

// std::minstd_rand0 streams its entire state as a single decimal integer.
static uint64_t current_rng_state()
{
    std::ostringstream ss;
    ss << rng_get_engine();
    return std::stoull( ss.str() );
}

determinism_sample sample_world_state()
{
    determinism_sample s;
    s.turn = to_turn<int>( calendar::turn );
    s.rng_calls = rng_call_count();

    // current_rng_state() must be read after rng_calls, because streaming the
    // engine does not touch it but reading the counter first keeps the two
    // fields describing the same instant regardless of future changes.
    s.rng_state = current_rng_state();

    uint64_t ch = fnv_offset_basis;
    for( monster &m : g->all_monsters() ) {
        ch = hash_creature( ch, m );
    }
    for( npc &guy : g->all_npcs() ) {
        ch = hash_creature( ch, guy );
    }
    s.creature_hash = ch;

    uint64_t ph = fnv_offset_basis;
    ph = hash_creature( ph, get_avatar() );
    ph = fnv1a_u64( ph, as_u64( get_avatar().get_stamina() ) );
    s.player_hash = ph;

    uint64_t combined = fnv_offset_basis;
    combined = fnv1a_u64( combined, as_u64( s.turn ) );
    combined = fnv1a_u64( combined, s.rng_calls );
    combined = fnv1a_u64( combined, s.rng_state );
    combined = fnv1a_u64( combined, s.creature_hash );
    combined = fnv1a_u64( combined, s.player_hash );
    s.combined = combined;

    return s;
}

void write_sample_log( const std::string &path, const std::vector<determinism_sample> &samples )
{
    std::ofstream out( path, std::ios::trunc );
    out << "turn,rng_calls,rng_state,creature_hash,player_hash,combined\n";
    for( const determinism_sample &s : samples ) {
        out << s.turn << ',' << s.rng_calls << ',' << s.rng_state << ','
            << s.creature_hash << ',' << s.player_hash << ',' << s.combined << '\n';
    }
}
```

- [ ] **Step 5: Прогнать тест и убедиться, что он проходит**

```bash
make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "[determinism_probe]" --user-dir test_user_dir_probe
```

Ожидается: PASS, три теста.

- [ ] **Step 6: Коммит**

```bash
git add tests/determinism_probe.h tests/determinism_probe.cpp tests/determinism_probe_test.cpp
git commit -m "test: add world state hash for determinism probe"
```

---

## Task 3: Проверка записи лога

**Files:**
- Test: `tests/determinism_probe_test.cpp`

**Interfaces:**
- Consumes: `write_sample_log()`, `determinism_sample` из Task 2.
- Produces: ничего нового.

- [ ] **Step 1: Написать падающий тест**

Дописать в `tests/determinism_probe_test.cpp`:

```cpp
TEST_CASE( "write_sample_log_round_trips", "[.][determinism_probe]" )
{
    const std::string path = "determinism_probe_roundtrip.csv";
    std::vector<determinism_sample> samples;
    samples.push_back( determinism_sample{ 1, 2, 3, 4, 5, 6 } );
    samples.push_back( determinism_sample{ 7, 8, 9, 10, 11, 12 } );

    write_sample_log( path, samples );

    std::ifstream in( path );
    REQUIRE( in.good() );
    std::string header;
    std::getline( in, header );
    CHECK( header == "turn,rng_calls,rng_state,creature_hash,player_hash,combined" );

    std::string first;
    std::getline( in, first );
    CHECK( first == "1,2,3,4,5,6" );

    std::string second;
    std::getline( in, second );
    CHECK( second == "7,8,9,10,11,12" );

    in.close();
    std::remove( path.c_str() );
}
```

Дописать в блок включений этого файла:

```cpp
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
```

- [ ] **Step 2: Прогнать тест**

```bash
make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "[determinism_probe]" --user-dir test_user_dir_probe
```

Ожидается: PASS. Формат уже реализован в Task 2, поэтому тест проходит сразу — он фиксирует контракт с Python-скриптом из Task 5, чтобы изменение формата ломало тест, а не молча ломало сравнение.

- [ ] **Step 3: Коммит**

```bash
git add tests/determinism_probe_test.cpp
git commit -m "test: pin determinism probe log format"
```

---

## Task 4: Сценарий на 500 ходов

Аватар усыпляется, поэтому весь блок обработки ввода в `game::do_turn()` пропускается (`src/do_turn.cpp:631`: `if( !u.has_effect( effect_sleep ) || uquit == QUIT_WATCH )`). В режиме тестов UI обезврежен: `ui_manager::redraw()` возвращается сразу (`src/ui_manager.cpp:380`), `query_popup` не блокирует (`src/popup.cpp:288`), `handle_key_blocking_activity()` выходит немедленно (`src/do_turn.cpp:235`). Поэтому настоящий цикл хода крутится headless.

Погода намеренно не фиксируется: она использует RNG и плавающую точку и является частью того, что проба обязана измерить.

**Files:**
- Modify: `tests/determinism_probe_test.cpp`

**Interfaces:**
- Consumes: `sample_world_state()`, `write_sample_log()` из Task 2.
- Produces: CSV-лог по пути из переменной окружения `CDDA_DETERMINISM_LOG`, по умолчанию `determinism_probe.csv`.

- [ ] **Step 1: Написать дымовой тест на один ход**

Дописать в `tests/determinism_probe_test.cpp`:

```cpp
TEST_CASE( "do_turn_runs_headless_for_sleeping_avatar", "[.][determinism_probe]" )
{
    const override_option no_autosave( "AUTOSAVE", "false" );

    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &u = get_avatar();
    u.add_effect( efftype_id( "sleep" ), 10_hours );
    REQUIRE( u.has_effect( efftype_id( "sleep" ) ) );

    const int before = to_turn<int>( calendar::turn );
    g->do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}
```

Дописать в блок включений этого файла:

```cpp
#include "avatar.h"
#include "game.h"
#include "map_helpers_tests.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "type_id.h"
```

- [ ] **Step 2: Прогнать дымовой тест**

```bash
make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "do_turn_runs_headless_for_sleeping_avatar" --user-dir test_user_dir_probe
```

Ожидается: PASS, тест завершается без ожидания ввода.

Если тест зависает или падает, единственная допустимая замена — **не** подменять `do_turn()` самодельным циклом, а зафиксировать это как результат пробы: значит настоящий цикл хода недоступен headless, и MVP-0 переходит сразу к Task 6 с выводом «фидельность пробы ограничена, требуется отдельная работа по headless-запуску `do_turn`». Точность измерения важнее наличия зелёного теста.

- [ ] **Step 3: Написать сценарий на 500 ходов**

Дописать в `tests/determinism_probe_test.cpp`:

```cpp
TEST_CASE( "determinism_probe_500_turns", "[.][determinism_probe]" )
{
    const override_option no_autosave( "AUTOSAVE", "false" );

    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &u = get_avatar();
    u.add_effect( efftype_id( "sleep" ), 10_hours );

    // A crowd is required: a single monster has too few decisions to expose
    // ordering nondeterminism. These positions are fixed, not random.
    for( int i = 0; i < 12; ++i ) {
        const tripoint_bub_ms where( 40 + ( i % 4 ) * 3, 40 + ( i / 4 ) * 3, 0 );
        spawn_test_monster( "mon_zombie", where );
    }
    spawn_npc( { 45, 50 }, "thug" );
    spawn_npc( { 55, 50 }, "thug" );

    rng_reset_call_count();

    std::vector<determinism_sample> samples;
    samples.reserve( 500 );
    for( int i = 0; i < 500; ++i ) {
        g->do_turn();
        samples.push_back( sample_world_state() );
    }

    const char *env = std::getenv( "CDDA_DETERMINISM_LOG" );
    write_sample_log( env ? env : "determinism_probe.csv", samples );

    CHECK( samples.size() == 500 );
    // The last sample must show RNG activity, otherwise the scenario is inert
    // and the probe would report a meaningless clean result.
    CHECK( samples.back().rng_calls > 0 );
}
```

Дописать в блок включений этого файла:

```cpp
#include <cstdlib>

#include "coordinates.h"
#include "point.h"
```

- [ ] **Step 4: Прогнать сценарий**

```bash
make -j$(nproc) TILES=0 TESTS=1
tests/cata_test "determinism_probe_500_turns" --rng-seed 12345 --user-dir test_user_dir_probe
```

Ожидается: PASS, создан `determinism_probe.csv` на 501 строку (заголовок плюс 500 наблюдений).

- [ ] **Step 5: Коммит**

```bash
git add tests/determinism_probe_test.cpp
git commit -m "test: add 500-turn determinism probe scenario"
```

---

## Task 5: Инструмент сравнения логов

**Files:**
- Create: `tools/determinism_probe/compare_logs.py`

**Interfaces:**
- Consumes: CSV-формат, зафиксированный в Task 3.
- Produces: код возврата 0 при совпадении, 1 при расхождении; отчёт в stdout.

- [ ] **Step 1: Написать скрипт**

`tools/determinism_probe/compare_logs.py`:

```python
#!/usr/bin/env python3
"""Compare two determinism probe logs and report the first divergence.

Usage: compare_logs.py run_a.csv run_b.csv

Exit code 0 means the two runs matched on every turn, 1 means they diverged.
The report names the first divergent turn and which component moved first,
which is what tells you where to look: an rng_calls delta means one run
consumed a different number of random values, while a creature_hash delta at
an identical rng_calls means the same draws produced a different arrangement,
pointing at iteration order rather than at the RNG.
"""
import csv
import sys

FIELDS = ["rng_calls", "rng_state", "creature_hash", "player_hash", "combined"]


def load(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    a = load(sys.argv[1])
    b = load(sys.argv[2])

    if len(a) != len(b):
        print(f"DIVERGED: length {len(a)} vs {len(b)}")
        return 1

    for i, (ra, rb) in enumerate(zip(a, b)):
        if ra["combined"] == rb["combined"]:
            continue

        print(f"DIVERGED at turn {ra['turn']} (sample index {i})")
        for field in FIELDS:
            mark = "  " if ra[field] == rb[field] else "->"
            print(f"{mark} {field}: {ra[field]} vs {rb[field]}")

        if ra["rng_calls"] != rb["rng_calls"]:
            delta = int(rb["rng_calls"]) - int(ra["rng_calls"])
            print(f"\nRNG call count differs by {delta}. One run drew a different")
            print("number of random values. Look for a conditional RNG call whose")
            print("condition depends on iteration order or on floating point.")
        else:
            print("\nRNG call count is identical, so both runs drew the same values")
            print("in the same quantity but arranged the world differently. This is")
            print("the signature of nondeterministic iteration order — the prime")
            print("suspect is monsters_by_location in src/creature_tracker.h:150.")
        return 1

    print(f"IDENTICAL across {len(a)} turns")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Сделать скрипт исполняемым и проверить на искусственных данных**

```bash
chmod +x tools/determinism_probe/compare_logs.py
printf 'turn,rng_calls,rng_state,creature_hash,player_hash,combined\n1,5,7,9,11,13\n' > /tmp/a.csv
printf 'turn,rng_calls,rng_state,creature_hash,player_hash,combined\n1,5,7,99,11,77\n' > /tmp/b.csv
tools/determinism_probe/compare_logs.py /tmp/a.csv /tmp/b.csv; echo "exit=$?"
```

Ожидается: сообщение `DIVERGED at turn 1`, пометка `->` у `creature_hash` и `combined`, диагноз про порядок итерации, `exit=1`.

- [ ] **Step 3: Проверить совпадающий случай**

```bash
tools/determinism_probe/compare_logs.py /tmp/a.csv /tmp/a.csv; echo "exit=$?"
```

Ожидается: `IDENTICAL across 1 turns`, `exit=0`.

- [ ] **Step 4: Проверить, что скрипт проходит линтер проекта**

```bash
flake8 tools/determinism_probe/compare_logs.py
```

Ожидается: пустой вывод. Проект использует flake8 через цель `python-check` (`Makefile:1648`).

- [ ] **Step 5: Коммит**

```bash
git add tools/determinism_probe/compare_logs.py
git commit -m "tools: add determinism probe log comparison"
```

---

## Task 6: Прогон пробы и отчёт по воротам

Это деливерабл ворот. Задача не в том, чтобы получить зелёный тест, а в том, чтобы измерить факт расхождения **и стоимость его починки**.

**Files:**
- Create: `docs/superpowers/reports/2026-07-31-mvp0-determinism-findings.md`

**Interfaces:**
- Consumes: всё из Task 1-5.
- Produces: письменное заключение с решением «продолжать по спеку» или «переписывать §5.3, §6, §7 под стриминг состояния».

- [ ] **Step 1: Два прогона на одной машине с одним сидом**

```bash
CDDA_DETERMINISM_LOG=/tmp/run_a.csv tests/cata_test "determinism_probe_500_turns" \
    --rng-seed 12345 --user-dir test_user_dir_a
CDDA_DETERMINISM_LOG=/tmp/run_b.csv tests/cata_test "determinism_probe_500_turns" \
    --rng-seed 12345 --user-dir test_user_dir_b
tools/determinism_probe/compare_logs.py /tmp/run_a.csv /tmp/run_b.csv
```

Записать вывод дословно. Ожидание из спека §9.1 — расхождение. Совпадение тоже является результатом и означает, что подозрение на `monsters_by_location` не подтвердилось в этом сценарии.

- [ ] **Step 2: Прогон с другим сидом**

```bash
CDDA_DETERMINISM_LOG=/tmp/run_c.csv tests/cata_test "determinism_probe_500_turns" \
    --rng-seed 999 --user-dir test_user_dir_c
CDDA_DETERMINISM_LOG=/tmp/run_d.csv tests/cata_test "determinism_probe_500_turns" \
    --rng-seed 999 --user-dir test_user_dir_d
tools/determinism_probe/compare_logs.py /tmp/run_c.csv /tmp/run_d.csv
```

Один сид может случайно не задеть проблемный путь. Два сида дают уверенность, что чистый результат — это не везение.

- [ ] **Step 3: Прогон на второй машине**

Собрать репозиторий на второй машине с другим процессором и по возможности другим компилятором, прогнать с тем же сидом 12345, скопировать лог и сравнить с `/tmp/run_a.csv`.

```bash
tools/determinism_probe/compare_logs.py /tmp/run_a.csv /tmp/run_remote.csv
```

Это измеряет риск §10.2 спека — расхождение плавающей точки между сборками — на двадцать недель раньше, чем он всплыл бы на Ф7. Если второй машины нет, допустимая замена — пересобрать на той же машине другим компилятором (`make CXX=clang++` против `make CXX=g++`) и сравнить; записать в отчёте, что это ослабленная версия проверки.

- [ ] **Step 4: Локализовать причину расхождения**

Если расхождение найдено, взять номер хода из отчёта скрипта и определить конкретное место:

```bash
grep -rn "monsters_by_location" src/*.cpp src/*.h
grep -rn "for.*:.*get_monsters_by_location\|for.*:.*monsters_by_location" src/
```

Затем прогнать сценарий под отладчиком или с временной печатью порядка обхода на проблемном ходу и зафиксировать, какой именно обход даёт разный порядок.

- [ ] **Step 5: Оценить стоимость починки**

Для каждого найденного источника расхождения записать: файл и строку, что именно недетерминировано, предлагаемую замену (сортировка по ключу, переход на `monsters_list`, `stable_sort`), число затронутых мест по grep и оценку в днях.

Это и есть главный результат MVP-0. Ворота проходятся не фактом расхождения, а тем, что стоимость починки известна.

- [ ] **Step 6: Написать отчёт**

Создать `docs/superpowers/reports/2026-07-31-mvp0-determinism-findings.md` со следующими разделами, все обязательны:

1. **Что прогонялось** — сценарий, число ходов, сиды, машины и компиляторы.
2. **Результат одного и того же сида на одной машине** — дословный вывод скрипта.
3. **Результат другого сида** — дословный вывод.
4. **Результат между машинами или компиляторами** — дословный вывод, и явная пометка, если проверка была ослабленной.
5. **Найденные источники недетерминизма** — таблица: файл и строка, природа проблемы, предлагаемая замена, число затронутых мест, оценка в днях.
6. **Суммарная стоимость починки** — в днях, с разбивкой.
7. **Решение по воротам** — одно из двух: продолжать по спеку и переходить к MVP-1, либо детерминизм недостижим за разумную цену, и §5.3, §6, §7 спека переписываются под стриминг состояния.
8. **Что проба не покрыла** — честный список: транспорт, крафт, стрельба, длинные активности, вождение и всё, чего не было в сценарии. Чистый результат пробы не означает, что детерминирована вся игра.

- [ ] **Step 7: Коммит**

```bash
git add docs/superpowers/reports/2026-07-31-mvp0-determinism-findings.md
git commit -m "docs: record MVP-0 determinism probe findings and gate decision"
```

---

## Проверка после выполнения всех задач

- [ ] Обычный прогон тестов не задевает пробу (она под тегом `[.]`):

```bash
tests/cata_test "~[slow] ~[.]" --rng-seed 1 --order lex --user-dir test_user_dir_final
```

Ожидается: столько же пройденных тестов, сколько до начала работ, и ни одного из `[determinism_probe]`.

- [ ] Единственное изменение в `src/` — счётчик RNG:

```bash
git diff --stat <хеш коммита до Task 1>..HEAD -- src/
```

Ожидается: затронуты только `src/rng.h` и `src/rng.cpp`.
