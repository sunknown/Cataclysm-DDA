# Фаза 4: «ходите без меня», разгон активностей, длительности отсчётов — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Игрок может уйти от клавиатуры, не морозя партнёра, а сессия, в которой не играет никто, не перематывает мир; длительности отсчётов задаются флагами сервера.

**Architecture:** «Ушёл» — пятое состояние игрока в `mp_turn`, хранимое, а не выводимое: барьер его не ждёт, ходы ему не копятся, а ход не закрывается, пока ушли все живые. Свободный прогон остаётся тем, чем был, — барьер закрывается сразу; единственная новая защита — боевой пол, когда никто не играет руками, а кого-то видит враждебное существо. Возвращение — действие, тратящее ход, а не любое нажатие.

**Tech Stack:** C++17, Makefile-сборки (обычная и серверная с `MP_SERVER=1`), Catch2 v2, сессионный харнесс `tests/mp_session_harness.h`.

**Spec:** `docs/superpowers/specs/2026-11-17-mp-phase4-away-design.md`

## Global Constraints

- **Спеки:** `2026-11-17-mp-phase4-away-design.md` (основной), `2026-08-04-cdda-p2p-coop-design.md` (§7.4, §7.5, §8.5, §11), `2026-08-08-mp-phase3a-scheduler-design.md` (§7 — барьер и отсчёты, не переписывается).
- **Ф3a обязана остаться зелёной.** Барьер, drop по отсчёту, сжигание ходов у drop, строка «waiting:» и все их тесты — существующий контракт. Задача 2 трогает `mp_turn_paced()`; тест `a_player_left_behind_by_a_hurry_does_not_let_the_world_run_free` (`tests/mp_session_test.cpp:341`) обязан пройти без правок.
- **`away` переживает `mp_turn_reset()`** и снимается `mp_turn_reset_all()`. Иначе режим живёт один тик; это тот же класс ошибки, что и флаги ввода (`src/mp/mp_turn.h:60-77`).
- **Флаги ввода `mp_turn_reset()` не трогает** — сохраняется как есть.
- **`mp_player_state_of()` проверяет `away` первым.** Ушедший не держит барьер ни при активности, ни при ходах, ни при открытом меню.
- **Ушедший не копит ходы:** `mp_forfeit_dropped_moves()` обнуляет их и ему (как `not_waited`).
- **Свободные действия `away` не снимают;** снимают действие, тратящее ход, и повторное нажатие клавиши.
- **Свободный прогон включается только без враждебных в поле зрения.** Пока монстр видит кого-то и никто не играет руками — ход не короче `MP_HURRY_COMBAT_MS`.
- **Всё, что делает сервер, — не решает за игрока:** сервер не закрывает его меню, не играет за него, не возвращает его из away сам.
- **Сборка тестов (обычная):** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`. Запуск: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4/`.
- **Сборка тестов (серверная):** `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`. Запуск: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/`.
- **Тесты с харнессом живут в `tests/mp_session_test.cpp`** — файл целиком под `#if defined(MP_SERVER)`, то есть только серверная сборка.
- **Полный регресс — только на свежей user-dir, и сравнивается с эталоном:** `python3 tools/run_tests_parallel.py -j $(nproc) --seed 1`. Гейт — **дифф** против прошлого прогона (пять известных падений), а не абсолютная зелень. Раннер намеренно не коммитится.
- **`game::unserialize` в тестах не вызывать** (Ф3b, Global Constraints).
- **Стиль:** `make astyle`, комментарии по-английски, лимит 100 колонок.
- **Коммиты:** после каждой задачи; `feat(mp):`, `test(mp):`, `docs(mp):`. Без `Co-Authored-By`.

## Review Focus

Пять классов входа, которых спек не называет, а человек встретит:

1. **Ушли все.** Живых за клавиатурой нет, ход не закрывается, мир стоит. Это задумано, но выглядит как зависание: возвращение одного обязано двигать мир дальше без перезапуска сессии. Тест — задача 2.
2. **Все заняты активностью, а рядом монстр.** Разгон без боевого пола — это смерть за один кадр. Тест — задача 2 (оба случая: все заняты и один ушёл).
3. **Ушедший умирает.** Смерть должна закончить сессию обычным путём с его именем в причине, а не оставить «away»-место в мёртвой сессии. Тест — задача 3.
4. **Мусорные длительности отсчётов.** `--hurry-ms 0` не должен ронять барьер в мгновенный drop и не должен превращать отсчёт в бесконечность: значения вне диапазона отвергаются с сообщением сервера. Тест — задача 5.
5. **Клавиша «ушёл» без партии.** В одиночной игре действие зарегистрировано, но обязано ничего не делать. Тест — задача 3.

---

## Обзор файлов

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/mp/mp_turn.h`, `.cpp` | `mp_player_state::away`; флаг `g_away`; `mp_set_away`, `mp_player_is_away`, `mp_return_from_away`, `mp_turn_all_away`; `mp_turn_paced()` с боевым полом; `mp_forfeit_dropped_moves()` с away; `mp_set_hurry_lengths` и переменные длительностей вместо констант в отсчётах |
| `src/do_turn.cpp` | Условие цикла хода (:761) — `|| mp_turn_all_away()` |
| `src/action.h`, `src/action.cpp` | `ACTION_COOP_AWAY` и его имя `coop_away` |
| `src/handle_action.cpp` | Ветка `ACTION_COOP_AWAY`; снятие away на действии, тратящем ход |
| `src/game.cpp` | `ctxt.register_action( "coop_away" )` рядом с `coop_hurry` (:2723) |
| `src/display.cpp` | `coop_player_text()` — список ушедших рядом с ожидаемыми |
| `src/main.cpp` | Флаги `--hurry-ms`, `--hurry-combat-ms`; передача в `run_server_session()`; вызов `mp_set_hurry_lengths()` перед циклом хода |
| `data/raw/keybindings.json` | `coop_away` на `:` |
| `tests/mp_turn_test.cpp` | Состояния, барьер, ходы, предикаты |
| `tests/mp_session_test.cpp` | Харнесс: остановка на всех ушедших, боевой пол, разгон, клавиша, смерть |
| `doc/MULTIPLAYER.md` | Клавиша, флаги, поведение «все ушли» |
| `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md` | §7.4, §8.5, ревизия в шапке |

Новых файлов нет: `mp_turn` уже владеет этим состоянием по §5.2 родительского спека, а тесты — по месту.

---

### Task 1: Состояние `away` в `mp_turn`

**Files:**
- Modify: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`
- Test: `tests/mp_turn_test.cpp`

**Interfaces:**
- Consumes: `mp_player_state_of()`, `mp_barrier_satisfied()`, `mp_forfeit_dropped_moves()`, `mp_turn_reset()`, `mp_turn_reset_all()` — существующие, сигнатуры не меняются.
- Produces:
  - `enum class mp_player_state` получает значение `away`.
  - `void mp_set_away( size_t slot, bool away );`
  - `bool mp_player_is_away( size_t slot );`
  - `void mp_return_from_away( size_t slot );`

- [ ] **Step 1: Написать падающие тесты**

В конец `tests/mp_turn_test.cpp`, в стиле соседей (`restore_active_state`, `mp_ensure_player_context`, `clear_avatar`, `mp_turn_reset_all`):

```cpp
TEST_CASE( "an_away_player_does_not_hold_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    mp_set_waiting_for_input( 0, true );
    REQUIRE( mp_player_state_of( 0 ) == mp_player_state::holding );

    mp_set_away( 0, true );

    CHECK( mp_player_state_of( 0 ) == mp_player_state::away );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "away_survives_the_turn_reset_and_dies_at_the_session_start", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_set_away( 0, true );
    mp_turn_reset();
    CHECK( mp_player_is_away( 0 ) );

    mp_turn_reset_all();
    CHECK_FALSE( mp_player_is_away( 0 ) );
}

TEST_CASE( "an_away_player_banks_no_moves", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_away( 0, true );

    mp_forfeit_dropped_moves();
    CHECK( u.get_moves() == 0 );
}

TEST_CASE( "returning_from_away_makes_the_player_hold_again", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_away( 0, true );
    mp_return_from_away( 0 );

    CHECK_FALSE( mp_player_is_away( 0 ) );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acting );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-p4/`
Expected: ошибка компиляции на `mp_player_state::away`, `mp_set_away`, `mp_player_is_away`, `mp_return_from_away`.

- [ ] **Step 3: Реализовать**

`mp_turn.h`: значение `away` в `enum class mp_player_state` — первым в перечислении, с комментарием, что оно хранимое и проверяется раньше остальных; три объявления с комментариями в стиле файла (что значит, кто зовёт, почему переживает сброс хода).

`mp_turn.cpp`:
- `std::vector<bool> g_away` рядом с `g_dropped_by_countdown`; `ensure_sized()` растит и его.
- `mp_player_state_of()`: `if( g_away[slot] ) return mp_player_state::away;` **до** проверки активности и ходов.
- `mp_barrier_satisfied()`: `away` удовлетворяет барьер наравне с `acted` и `not_waited`.
- `mp_forfeit_dropped_moves()`: обнулять ходы при `state == not_waited` **и** при `state == away`.
- `mp_turn_reset()`: `g_away` не трогать (комментарий, почему — как у флагов ввода).
- `mp_turn_reset_all()`: `g_away.assign( g->player_slot_count(), false )`.
- `mp_set_away()`, `mp_player_is_away()`, `mp_return_from_away()` (последний — просто `false` в слот; отдельная функция нужна, чтобы у вызывающего не было доступа к внутреннему массиву).

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4/`
Expected: PASS, включая прежние тесты барьера.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp tests/mp_turn_test.cpp
git commit -m "feat(mp): an away player stops holding the barrier"
```

---

### Task 2: «Все ушли — ход не закрывается», боевой пол и доказательство разгона

**Files:**
- Modify: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`, `src/do_turn.cpp:761`
- Test: `tests/mp_turn_test.cpp`, `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: Task 1 (`away`, `mp_set_away`), `mp_set_turn_open()`, `anyone_is_seen_by_a_hostile()` (внутренняя, остаётся).
- Produces: `bool mp_turn_all_away();` — истинно, когда все **живые** слоты в away.

- [ ] **Step 1: Написать падающие юнит-тесты**

```cpp
TEST_CASE( "the_turn_stays_open_while_every_living_player_is_away", "[mp]" )
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

    CHECK_FALSE( mp_turn_all_away() );
    mp_set_away( 0, true );
    CHECK_FALSE( mp_turn_all_away() );
    mp_set_away( 1, true );
    CHECK( mp_turn_all_away() );
    mp_set_away( 0, false );
    CHECK_FALSE( mp_turn_all_away() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "a_hostile_in_sight_paces_a_turn_nobody_is_playing_by_hand", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    clear_map();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    // Both busy with an activity, so nobody is playing by hand: without a
    // hostile that is exactly the free run the phase exists for.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    b.assign_activity( wait_activity_actor( 10_minutes ) );
    mp_set_turn_open( true );
    CHECK_FALSE( mp_turn_paced() );

    mp_set_turn_open( false );
    spawn_test_monster( "mon_zombie", g->player_at( 0 ).pos_bub() + tripoint::east );
    mp_set_turn_open( true );
    CHECK( mp_turn_paced() );

    // Same rule with one player away instead of busy: the floor does not care
    // who is absent, only that a fight is on and nobody is playing by hand.
    mp_set_away( 1, true );
    b.activity = player_activity();
    CHECK( mp_turn_paced() );

    // A player back at the keyboard paces the world by themselves, hostile or
    // not: their actions are the clock.
    mp_set_away( 1, false );
    b.set_moves( 100 );
    mp_set_waiting_for_input( 1, true );
    CHECK_FALSE( mp_turn_paced() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}
```

(`wait_activity_actor` — `src/activity_actor_definitions.h:4011`; `spawn_test_monster` уже подключён в этом файле.)

- [ ] **Step 2: Написать падающие тесты харнесса**

В `tests/mp_session_test.cpp`:

```cpp
TEST_CASE( "a_session_where_everyone_is_away_does_not_advance_the_world", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    mp_set_away( 0, true );
    mp_set_away( 1, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // Ten polls take about a second. With every living player away there is
    // nobody to wait for and nobody to pace: the turn must not close at all.
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, 10 ) );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    // One comes back and waits out a turn; the world moves again.
    mp_set_away( 0, false );
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, '.' );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

TEST_CASE( "an_away_player_does_not_slow_a_busy_partner", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    mp_set_away( 0, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // The away player is not a left-behind one: they cost the partner nothing
    // and, alone, nothing paces the world either.
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );
}
```

- [ ] **Step 3: Написать падающий тест разгона**

```cpp
TEST_CASE( "two_busy_players_do_not_hold_the_world", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // The phase's criterion, pinned: a long activity is a player telling the
    // game what to do, and the world is free to run while both do it.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    const int turn_before = to_turn<int>( calendar::turn );

    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );

    get_map() = map(); // the harness's map is ours again after the activity run
}
```

- [ ] **Step 4: Прогнать — падает**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/` (после серверной сборки)
Expected: FAIL — `mp_turn_all_away` не существует; «все ушли» продвигает календарь; боевой пол не держит.

- [ ] **Step 5: Реализовать**

`mp_turn.cpp`:
- `mp_turn_all_away()`: перебрать слоты, пропустить мёртвых (`is_dead_state()`), вернуть `false` на первом живом не-away; `true`, если живых нет или все ушли.
- `mp_turn_paced()` — порядок проверок:

```cpp
// A player at the keyboard is the clock: their actions pace the world, and
// nothing here needs a floor.
bool anyone_left_behind = false;
for( size_t i = 0; i < g->player_slot_count(); ++i ) {
    avatar &ava = g->player_at( i );
    if( ava.is_dead_state() )       { continue; }
    if( mp_player_left_behind( i ) ) { anyone_left_behind = true; continue; }
    if( mp_player_is_away( i ) )    { continue; }
    if( !ava.activity && !ava.has_distant_destination() ) return false;
}
const auto age = std::chrono::steady_clock::now() - g_turn_opened_at;
// A fight is not a fast-forward, whoever is busy.
if( anyone_is_seen_by_a_hostile() ) return age < std::chrono::milliseconds( MP_HURRY_COMBAT_MS );
// A left-behind player is still reading; an away one asked not to be waited
// for, and must not spend their partner's activity pace waiting on a countdown
// aimed at nobody.
if( anyone_left_behind )          return age < std::chrono::milliseconds( MP_HURRY_MS );
return false;
```

`do_turn.cpp:761`: `while( !mp_barrier_satisfied() || mp_turn_paced() || mp_turn_all_away() ) {`.

Проверить, что ветка «никто не должен хода» (`moved == false`) уходит в `wait_for_request( MP_POLL_MS, owes_turn )` и возвращает `false`, то есть ход остаётся открытым, — это уже так (:790-810).

- [ ] **Step 6: Прогнать оба набора**

Run: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4/` и `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/`
Expected: PASS. Отдельно проверить, что `a_player_left_behind_by_a_hurry_does_not_let_the_world_run_free` (`tests/mp_session_test.cpp:341`) не правился и зелёный.

- [ ] **Step 7: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp src/do_turn.cpp \
        tests/mp_turn_test.cpp tests/mp_session_test.cpp
git commit -m "feat(mp): a session nobody is playing does not move the world"
```

---

### Task 3: Клавиша, действие и возвращение

**Files:**
- Modify: `src/action.h` (`ACTION_COOP_HURRY` рядом, :363), `src/action.cpp` (`:312`), `src/handle_action.cpp` (`:3466-3476`), `src/game.cpp` (`:2723`), `data/raw/keybindings.json` (`coop_hurry`, :3307), `src/mp/mp_turn.cpp` (`mp_action_is_free`)
- Test: `tests/mp_turn_test.cpp`, `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: Task 1 (`mp_set_away`, `mp_player_is_away`, `mp_return_from_away`), `mp_action_is_free( action_id )`.
- Produces: действие `coop_away` (клавиша `:` по умолчанию), свободное.

- [ ] **Step 1: Написать падающие тесты**

`tests/mp_turn_test.cpp`:

```cpp
TEST_CASE( "the_away_toggle_is_free", "[mp]" )
{
    CHECK( mp_action_is_free( ACTION_COOP_AWAY ) );
}
```

`tests/mp_session_test.cpp`:

```cpp
TEST_CASE( "the_away_key_toggles_and_a_costly_action_brings_a_player_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );

    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_away( 0 );
    } ) );

    // Reading is not coming back: a free action must not put a player who is
    // away back into the barrier, or the partner freezes while they read.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );

    // A turn action does bring them back.
    harness.press( 0, '.' );
    CHECK( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    } ) );
}

TEST_CASE( "the_away_key_does_nothing_without_a_party", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, ':' );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_player_is_away( 0 ) );
}
```

- [ ] **Step 2: Прогнать — не собирается, затем падает**

Run: серверная сборка + `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/`
Expected: `ACTION_COOP_AWAY` не объявлен; после объявления — «ушёл» не включается клавишей.

- [ ] **Step 3: Реализовать**

- `src/action.h`: `ACTION_COOP_AWAY` рядом с `ACTION_COOP_HURRY`.
- `src/action.cpp`: `case ACTION_COOP_AWAY: return "coop_away";`.
- `data/raw/keybindings.json`: запись после `coop_hurry` — `"name": "Go without me"`, `"category": "DEFAULTMODE"`, `"id": "coop_away"`, `"bindings": [ { "input_method": "keyboard_char", "key": ":" } ]`.
- `src/game.cpp:2723`: `ctxt.register_action( "coop_away" );`.
- `src/mp/mp_turn.cpp`, `mp_action_is_free()`: `case ACTION_COOP_AWAY:` рядом с `ACTION_COOP_HURRY`.
- `src/handle_action.cpp`, в блоке `#if defined(MULTIPLAYER)`, сразу после ветки `ACTION_COOP_HURRY`:

```cpp
    if( act == ACTION_COOP_AWAY ) {
        const size_t slot = g->active_slot();
        mp_set_away( slot, !mp_player_is_away( slot ) );
        return false;
    }
```

и **после** проверки «уже действовал» (чтобы отказ не возвращал из away), перед `gamemode->pre_action( act )`:

```cpp
    if( !mp_action_is_free( act ) ) {
        mp_return_from_away( g->active_slot() );
    }
```

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/`
Expected: PASS.

- [ ] **Step 5: Тест смерти ушедшего**

```cpp
TEST_CASE( "a_death_while_away_still_ends_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    mp_set_away( 1, true );
    avatar &dead = g->player_at( 1 );
    const std::string name = dead.get_name();
    dead.set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( dead.is_dead_state() );

    CHECK( g->do_turn() );
    CHECK( mp_session_end_reason() == name + " died" );

    // Same teardown as a_death_in_a_session_ends_it_and_saves_the_world.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}
```

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4s/`
Expected: PASS без правок кода — смерть уже заканчивает сессию (`do_turn.cpp:730-740`). Если падает — чинить в этой задаче, а не откладывать.

- [ ] **Step 6: Коммит**

```bash
make astyle
git add src/action.h src/action.cpp src/handle_action.cpp src/game.cpp \
        src/mp/mp_turn.cpp data/raw/keybindings.json \
        tests/mp_turn_test.cpp tests/mp_session_test.cpp
git commit -m "feat(mp): a key to step away, and a turn action to come back"
```

---

### Task 4: Строка в панели

**Files:**
- Modify: `src/display.cpp:1343-1369`
- Test: `tests/mp_turn_test.cpp`

**Interfaces:**
- Consumes: `mp_player_is_away( slot )`, `mp_player_holds_barrier( slot )`, `mp_hurry_remaining_ms()`.
- Produces: строка вида `waiting: B (12) away: C`; ушедший в список ожидаемых не попадает.

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "the_sidebar_names_who_is_away", "[mp]" )
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

    const std::string away_name = g->player_at( 1 ).get_name();
    mp_set_away( 1, true );
    g->player_at( 0 ).set_moves( 100 );
    mp_set_waiting_for_input( 0, true );

    const std::string text = display::coop_player_text( g->player_at( 1 ) );

    // Both halves matter: the away player is reported, and is NOT reported as
    // somebody the world is waiting on.
    CHECK( text.find( "away: " + away_name ) != std::string::npos );
    CHECK( text.find( away_name + "," ) == std::string::npos );
    CHECK( text.find( "waiting: " + away_name ) == std::string::npos );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}
```

- [ ] **Step 2: Прогнать — падает**

Run: `tests/cata_test "the_sidebar_names_who_is_away" --user-dir=/tmp/cata-mp-p4/`
Expected: FAIL — away не появляется в строке.

- [ ] **Step 3: Реализовать**

`display::coop_player_text()`: собрать второй список — имена слотов с `mp_player_is_away( i )` — и дописать `away: A, B` к результату; когда ожидаемых нет, а ушедшие есть, вернуть `P<n>: <имя>` плюс ` — away: A, B`. Формат — одна строка; перевод через `_( )`, как у соседних строк. Ничего не рисовать, если партии нет (`mp_coop_active()`).

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4/`
Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/display.cpp tests/mp_turn_test.cpp
git commit -m "feat(mp): the sidebar says who stepped away"
```

---

### Task 5: Длительности отсчётов флагами сервера

**Files:**
- Modify: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`, `src/main.cpp:265-300` (`cli_opts`), `:432-460` (флаги), `:607-636` (`run_server_session`), `:644` (`run_game` — вызов)
- Test: `tests/mp_turn_test.cpp`

**Interfaces:**
- Consumes: `mp_request_hurry()`, `mp_hurry_remaining_ms( source )`, `MP_HURRY_MS`, `MP_HURRY_COMBAT_MS`.
- Produces: `bool mp_set_hurry_lengths( int manual_ms, int combat_ms );` — `false` и запись в `DebugLog( D_ERROR, D_MAIN )`, если любое значение вне `[1000, 600000]`; тогда длины остаются прежними. `mp_turn_reset_all()` возвращает длины к значениям по умолчанию.

- [ ] **Step 1: Написать падающие тесты**

```cpp
TEST_CASE( "the_hurry_lengths_can_be_set_for_a_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    REQUIRE( mp_set_hurry_lengths( 3000, 1000 ) );
    mp_request_hurry();
    const int left = mp_hurry_remaining_ms( mp_hurry_source::manual );
    CHECK( left > 0 );
    CHECK( left <= 3000 );

    // The defaults come back with the session reset, not with the turn reset:
    // the lengths are the operator's, not the turn's.
    mp_turn_reset_all();
    mp_request_hurry();
    CHECK( mp_hurry_remaining_ms( mp_hurry_source::manual ) <= MP_HURRY_MS );
}

TEST_CASE( "a_nonsense_hurry_length_is_refused", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    CHECK_FALSE( mp_set_hurry_lengths( 0, 1000 ) );
    CHECK_FALSE( mp_set_hurry_lengths( 1000, -1 ) );
    CHECK_FALSE( mp_set_hurry_lengths( 1000, 600001 ) );
    mp_request_hurry();
    CHECK( mp_hurry_remaining_ms( mp_hurry_source::manual ) > 3000 );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-p4/`
Expected: ошибка компиляции на `mp_set_hurry_lengths`.

- [ ] **Step 3: Реализовать**

- `mp_turn.cpp`: `int g_hurry_ms = MP_HURRY_MS; int g_hurry_combat_ms = MP_HURRY_COMBAT_MS;`, `length_ms_of( source )` **и** `mp_turn_paced()` (два её читателя из задачи 2) читают их вместо констант, сами константы остаются значениями по умолчанию; `mp_set_hurry_lengths()` с проверкой диапазона; `mp_turn_reset_all()` возвращает значения по умолчанию.
- `main.cpp`: в `cli_opts` — `std::optional<int> hurry_ms; std::optional<int> hurry_combat_ms;`; флаги `--hurry-ms` и `--hurry-combat-ms` в общий список (описание: «How long, in milliseconds, the party waits for a player who is reading, 1000-600000»); `run_server_session` получает эти два значения и сразу после `mp_server_open_world()` зовёт `mp_set_hurry_lengths()`, логируя отказ в `mp_server_log()`.
- Значения по умолчанию не передаются: `std::nullopt` значит «оставить как есть».

- [ ] **Step 4: Прогнать — зелено, и глазами проверить флаг**

Run: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4/`, затем серверная сборка и `./mpserver-cataclysm-server --server 4321 --world coop --players 1 --hurry-ms 5000` — в консоли сервера длина не жалуется, `--hurry-ms 0` печатает отказ.
Expected: PASS; отказ виден в логе.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp src/main.cpp tests/mp_turn_test.cpp
git commit -m "feat(mp): the countdown lengths come from the command line"
```

---

### Task 6: Документация и ворота

**Files:**
- Modify: `doc/MULTIPLAYER.md`, `docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md`, `docs/superpowers/specs/2026-11-17-mp-phase4-away-design.md` (строка статуса)

**Interfaces:**
- Consumes: весь предыдущий код.
- Produces: пройденные ворота и обновлённые документы.

- [ ] **Step 1: `doc/MULTIPLAYER.md`**

В раздел про ходы/«тороплю» (:`242-273`) добавить: клавиша `:` — «Go without me», что она значит (партнёр не ждёт, ходы не копятся, панель показывает `away:`), что возвращает (действие, тратящее ход, или повторное нажатие), что происходит, когда ушли все (мир стоит), и боевой пол. В раздел запуска сервера — флаги `--hurry-ms`/`--hurry-combat-ms` с диапазоном и значениями по умолчанию.

- [ ] **Step 2: Родительский спек**

- §7.4: возвращает **действие, тратящее ход**, а не любое нажатие (причина: свободное действие вернуло бы в барьер читающего); «все ушли — мир стоит»; боевой пол для ушедшего.
- §7.5: свободный прогон уже реализован резюмируемым ходом в Ф3a; Ф4 добавляет только боевой пол и запрет на прогон, когда ушли все; доказательство — тест `two_busy_players_do_not_hold_the_world`.
- §8.5: длины отсчётов задаются флагами сейчас, файл конфигурации остаётся Ф5 и читает те же сеттеры.
- В шапку — ревизия `2026-11-17` со списком правок и ссылкой на спеку Ф4.

- [x] **Step 3: Ворота автотестов**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp],[coop]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4-gate/

make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p4-gate-s/

python3 tools/run_tests_parallel.py -j $(nproc) --seed 1
```

Expected: оба `[mp]`-набора зелёные; полный прогон **диффится** с эталоном прошлой волны — те же пять известных падений, ничего нового. Раннер не коммитить.

- [ ] **Step 4: Ворота человека**

Две машины, обычный серверный сценарий из `doc/MULTIPLAYER.md`:

1. Один игрок нажимает `:` и уходит; второй играет — партнёр не ждёт, в панели видно `away: <имя>`.
2. Ушедший возвращается действием — ход снова его.
3. Оба ушли — мир стоит (календарь на экране не идёт); кто-то вернулся — идёт.
4. Оба ложатся спать (или долгий крафт) — до утра проходят секунды, и это видно по календарю.
5. Монстр рядом с ушедшим — мир не проскакивает бой за один кадр.

Результат 2026-11-18: шаги 1–4 подтверждены локально в tmux (сервер + два curses-клиента на одной машине, окна `client1`/`client2`); шаг 5 вживую не воспроизведён и покрыт только автоматическим тестом. Полные ворота с двух машин остаются открытыми. Подробности — в шапке спека Ф4.

- [ ] **Step 5: Коммит**

```bash
git add doc/MULTIPLAYER.md docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md \
        docs/superpowers/specs/2026-11-17-mp-phase4-away-design.md
git commit -m "docs(mp): close phase 4 -- away, free run, countdown flags"
```

---

## Заметки для реализующего

**1. `away` — не drop.** У drop есть причина (отсчёт истёк) и правило «живёт, пока игрок сидит в том же меню». У away причины нет и правило другое: живёт, пока игрок сам не вернулся. Не сливать их в один флаг: `mp_turn_reset()` обращается с ними по-разному, и это единственное место, где разница видна.

**2. Боевой пол — не отсчёт и никого не drop-ает.** Он только не даёт ходу закрыться быстрее своей длины. Игрок, которого видит монстр, остаётся ушедшим: сервер за него не играет и меню ему не закрывает.

**3. Свободный прогон не новый.** Он уже есть в Ф3a и держится на том, что барьер удовлетворён у занятого игрока. Задача 2 его не включает, а ограничивает: боевой пол и «все ушли». Если тест разгона зелёный ещё до правок — это ожидаемо, и правки нужны только для двух ограничений.

**4. Панель — одна строка.** Панель узкая; не заводить вторую строку ради away. Если имена не влезают, режет их существующий механизм панели, а не этот код.

**5. Клавиша `:` свободна в `DEFAULTMODE`,** но занята в других контекстах (`CHOOSE_LOCATION`, `EDITMAP_SWAP_POINTS`). Это нормально: контексты не пересекаются, а игрок может переназначить.

**6. Флаги не должны ломать отсчёт.** Ноль/отрицательное/огромное отвергается до старта сессии, а не зажимается: зажатое значение — это тихая подмена решения оператора.

**7. `mp_set_away()` из теста зовут между ходами.** Харнесс держит батон у тестового потока между вызовами `run_until()`, и только там тест трогает состояние, которое читает мировой поток, — так же, как существующие тесты зовут `set_moves()` и `mp_request_hurry()`. Внутри `run_until()`-предиката away не выставлять.

---

## Значения, разрешённые при планировании

| Что | Значение |
|---|---|
| Клавиша по умолчанию | `:` (`keyboard_char`), действие `coop_away`, `DEFAULTMODE` |
| Длительности по умолчанию | `MP_HURRY_MS = 15000`, `MP_HURRY_COMBAT_MS = 4000` (`src/mp/mp_turn.h:10,12`) |
| Допустимый диапазон флагов | `[1000, 600000]` мс; иначе отказ с логом |
| Состояние «ушёл» | `mp_player_state::away`, флаг `g_away` в `mp_turn.cpp` |
| Возврат | `mp_return_from_away( slot )` — из `handle_action.cpp` на несвободном действии и из `ACTION_COOP_AWAY` |
| Предикат остановки | `mp_turn_all_away()` в условии `do_turn.cpp:761` |
| Харнесс | `tests/mp_session_harness.h`, режим `playing`; длинная активность — `wait_activity_actor( 10_minutes )` |
| Смерть в тесте | `set_part_hp_cur( bodypart_id( "torso" ), 0 )` (`tests/mp_session_test.cpp:438`) |
| Барьер и отсчёты в тестах | `mp_request_hurry()`, `mp_advance_hurry_for_test( ms, источник )` |
| Панель | `display::coop_player_text( const Character & )` (`src/display.h:212`) |
| Инструмент полного прогона | `tools/run_tests_parallel.py` (не коммитится), `--seed 1` |
