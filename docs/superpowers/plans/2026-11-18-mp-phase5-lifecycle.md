# Фаза 5: жизненный цикл сессии — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Сессия переживает обрыв и смерть одного игрока, позволяет заменить погибшего
персонажа и принять нового клиента в освободившийся слот; параметры сервера читаются из
JSON-файла с приоритетом CLI > файл > defaults.

**Architecture:** Слот получает два независимых признака — подключён и жив; `disconnected`
становится третьим состоянием `mp_turn` рядом с `away`, барьер его не ждёт и ходы ему
сжигаются. Смерть и обрыв больше не зовут `mp_request_session_end()`: сессия завершается
только когда не осталось подключённых клиентов. Замена и вход посреди партии используют
существующие `mp_run_join_menu()` и `place_new_character()`. Конфиг-файл читает те же
сеттеры, что и флаги, и не заводит второй путь.

**Tech Stack:** C++17, Makefile-сборки (обычная и серверная с `MP_SERVER=1`), Catch2 v2,
сессионный харнесс `tests/mp_session_harness.h`.

**Spec:** `docs/superpowers/specs/2026-11-18-mp-phase5-lifecycle-design.md`

## Global Constraints

- **Спеки:** `2026-11-18-mp-phase5-lifecycle-design.md` (основной),
  `2026-08-04-cdda-p2p-coop-design.md` §8.3–8.5, §11; ADR 0001–0003; `GLOSSARY.md`.
- **Ветка:** работа идёт на `mp-phase-5`, созданной от `mp` (`58ce25134c`). **`master` в `mp`
  не мержить никогда** — явный запрет пользователя; `mp` считается зафиксированной.
- **Ф4 не ломать.** `away`, боевой пол, длительности отсчётов, строка `waiting:` и все их
  тесты — существующий контракт.
- **`away` и `disconnected` — разные состояния.** `away` снимается игроком; `disconnected`
  снимается только возвратом клиента. Оба не держат барьер и не копят ходы.
- **Смерть не завершает сессию.** Погибший получает меню создания персонажа; труп остаётся;
  замена ставится `place_new_character()` рядом с группой.
- **Сессия заканчивается только при нуле подключённых клиентов.** Смерть всех персонажей
  при живых клиентах — не конец.
- **Новый клиент посреди партии** занимает только слот `dead && disconnected`; слотов сверх
  `--players` не появляется.
- **Тезер:** отключённый avatar — живое тело и учитывается якорем; мёртвые пропускаются.
- **Конфиг:** `<userdir>/config/mp/server.json`, ключи `port`, `world`, `players`,
  `hurry-ms`, `hurry-combat-ms`; CLI > файл > defaults. Битый JSON/неизвестный ключ/тип —
  отказ стартовать. `hurry-*` вне `[1000, 600000]` — отказ с логом, defaults.
- **Сборка тестов (обычная):** `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`.
  Запуск: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5/`.
- **Сборка тестов (серверная):**
  `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`.
  Запуск: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`.
- **Новые исходники** добавлять и в `Makefile`, и в `CMakeLists.txt`.
- **Полный регресс** — только на свежей user-dir, дифф против эталона известных падений:
  `python3 tools/run_tests_parallel.py -j $(nproc) --seed 1`. Раннер не коммитить.
- **`game::unserialize` в тестах не вызывать.**
- **Стиль:** `make astyle`, комментарии по-английски, лимит 100 колонок.
- **Коммиты:** `feat(mp):`, `test(mp):`, `docs(mp):`, без `Co-Authored-By`.

## Review Focus

1. **Последний клиент ушёл, когда все персонажи мертвы.** Сессия обязана завершиться, а не
   зависнуть в ожидании несуществующего игрока. Тест — задача 3.
2. **Отключённого убили до возврата.** При реконнекте — меню создания замены, а не управление
   трупом и не «призрак». Тест — задача 5.
3. **Смерть или обрыв во время открытого меню создания.** Барьер не должен зависнуть, поток
   не должен запарковаться навсегда. Тест — задачи 4 и 5.
4. **Новый клиент без свободного слота.** Внятный отказ, ростер не растёт, остальные играют.
   Тест — задача 6.
5. **Битый конфиг и CLI без порта.** Сервер либо стартует с полным набором значений, либо
   отказывает с сообщением; частично применённого конфига нет. Тест — задача 7.

---

## Обзор файлов

**Изменяются:**

| Файл | Что именно |
|---|---|
| `src/mp/mp_turn.h`, `.cpp` | `mp_player_state::disconnected`; `mp_set_disconnected`, `mp_player_is_disconnected`; барьер, сжигание ходов, reset |
| `src/mp/mp_server_main.h`, `.cpp` | подключённость слотов; конец по числу клиентов; reconnect; смерть и замена; приём после старта |
| `src/mp/mp_curses_server.cpp` | обрыв помечает слот, а не завершает сессию; сообщение в журнал |
| `src/do_turn.cpp` | смерть больше не завершает сессию; мёртвый слот не держит барьер |
| `src/mp/mp_join_menu.h`, `.cpp` | меню замены без Continue; отказ при отсутствии свободного слота |
| `src/main.cpp` | необязательные аргументы `--server/--world/--players`; чтение `server.json` |
| `Makefile`, `CMakeLists.txt` | новый `src/mp/mp_server_config.cpp` |
| `doc/MULTIPLAYER.md` | обрыв, возврат, смерть/замена, вход посреди партии, конфиг |
| `tests/mp_turn_test.cpp`, `tests/mp_session_test.cpp` | новые состояния, сценарии харнесса |
| `tests/mp_server_config_test.cpp` | парсинг и приоритет конфига |

**Создаются:**

- `src/mp/mp_server_config.h`, `src/mp/mp_server_config.cpp`
- `tests/mp_server_config_test.cpp`

---

### Task 1: Состояние `disconnected` в `mp_turn`

**Files:**
- Modify: `src/mp/mp_turn.h`, `src/mp/mp_turn.cpp`
- Test: `tests/mp_turn_test.cpp`

**Interfaces:**
- Consumes: `mp_player_state_of()`, `mp_barrier_satisfied()`, `mp_forfeit_dropped_moves()`,
  `mp_turn_reset()`, `mp_turn_reset_all()`.
- Produces:
  - `enum class mp_player_state` получает значение `disconnected`.
  - `void mp_set_disconnected( size_t slot, bool disconnected );`
  - `bool mp_player_is_disconnected( size_t slot );`

- [ ] **Step 1: Написать падающие тесты**

Добавить в `tests/mp_turn_test.cpp`, в стиле соседей:

```cpp
TEST_CASE( "a_disconnected_player_does_not_hold_the_barrier", "[mp]" )
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

    mp_set_disconnected( 0, true );

    CHECK( mp_player_state_of( 0 ) == mp_player_state::disconnected );
    CHECK( mp_barrier_satisfied() );
    CHECK( mp_player_is_disconnected( 0 ) );
}

TEST_CASE( "disconnected_survives_the_turn_reset_and_dies_at_the_session_start", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_set_disconnected( 0, true );
    mp_turn_reset();
    CHECK( mp_player_is_disconnected( 0 ) );

    mp_turn_reset_all();
    CHECK_FALSE( mp_player_is_disconnected( 0 ) );
}

TEST_CASE( "a_disconnected_player_banks_no_moves", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_disconnected( 0, true );

    mp_forfeit_dropped_moves();
    CHECK( u.get_moves() == 0 );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 && tests/cata_test "[mp]" --user-dir=/tmp/cata-mp-p5/`

Expected: ошибка компиляции на `mp_player_state::disconnected`, `mp_set_disconnected`,
`mp_player_is_disconnected`.

- [ ] **Step 3: Реализовать**

`mp_turn.h`: значение `disconnected` в `mp_player_state` после `away`; объявления с
комментариями, что это серверное состояние, а не выбор игрока, и что оно переживает
`mp_turn_reset()` и снимается только возвратом клиента или `mp_turn_reset_all()`.

`mp_turn.cpp`:

- `std::vector<bool> g_disconnected` рядом с `g_away`; `ensure_sized()` растит и его.
- `mp_player_state_of()`: после проверки `away` проверить `disconnected`.
- `mp_barrier_satisfied()`: `disconnected` удовлетворяет барьер наравне с `acted`/`away`.
- `mp_forfeit_dropped_moves()`: обнулять ходы при `away` и `disconnected`.
- `mp_turn_reset()`: `g_disconnected` не трогать, как и `g_away`.
- `mp_turn_reset_all()`: `g_disconnected.assign( g->player_slot_count(), false )`.
- `mp_set_disconnected()` / `mp_player_is_disconnected()`.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5/`

Expected: PASS, включая все тесты Ф4.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_turn.h src/mp/mp_turn.cpp tests/mp_turn_test.cpp
git commit -m "feat(mp): a disconnected player stops holding the barrier"
```

---

### Task 2: Подключённость слотов и предикат конца сессии

**Files:**
- Modify: `src/mp/mp_server_main.h`, `src/mp/mp_server_main.cpp`
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: `mp_sessions()`, `mp_session::transport`, `mp_turn_reset_all()`.
- Produces:
  - `size_t mp_server_connected_client_count();`
  - `bool mp_server_any_client_connected();`
  - `void mp_server_end_if_no_clients();`
  - `bool mp_server_seat_is_free_for_new_client( size_t slot );`

- [ ] **Step 1: Написать падающие тесты**

В `tests/mp_session_test.cpp`:

```cpp
TEST_CASE( "the_session_does_not_end_while_a_client_is_connected", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    CHECK( mp_server_connected_client_count() == 2 );
    CHECK( mp_server_any_client_connected() );
    CHECK_FALSE( mp_session_end_requested() );
}

TEST_CASE( "the_session_ends_when_the_last_client_leaves", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    harness.drop_client( 0 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );
}

TEST_CASE( "a_dead_character_does_not_keep_the_session_alive", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.drop_client( 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );
}
```

`drop_client()` уже есть в харнессе и закрывает транспорт слота так же, как обрыв.

- [ ] **Step 2: Прогнать — не собирается**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: ошибка компиляции на новых функциях.

- [ ] **Step 3: Реализовать**

`mp_server_main.h`: объявления выше с комментариями, что «подключён» — это живой транспорт
клиента, а не живой персонаж.

`mp_server_main.cpp`:

- `mp_server_connected_client_count()` считает сессии с `session && session->transport &&
  session->transport->connected()`.
- `mp_server_any_client_connected()` — `count > 0`.
- `mp_server_end_if_no_clients()`: если клиентов нет — `mp_request_session_end( "no players
  connected" )`; иначе ничего.
- `mp_server_seat_is_free_for_new_client( slot )`: `slot < player_slot_count()` и
  `g->player_at( slot ).is_dead_state()` и слот не подключён.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp tests/mp_session_test.cpp \
        tests/mp_session_harness.h
git commit -m "feat(mp): count connected clients instead of living characters"
```

---

### Task 3: Обрыв помечает слот и не завершает сессию

**Files:**
- Modify: `src/mp/mp_curses_server.cpp`, `src/mp/mp_server_main.h`, `src/mp/mp_server_main.cpp`
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: Task 1 (`mp_set_disconnected`), Task 2 (`mp_server_end_if_no_clients`),
  `mp_session::name`, `mp_active_slot()`.
- Produces:
  - `void mp_server_mark_disconnected( size_t slot );`
  - запись в общий журнал сообщений `Player %d (%s) disconnected`.

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "a_partner_disconnect_does_not_end_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();

    harness.drop_client( 1 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_player_is_disconnected( 1 ) );
    CHECK( harness.message_log_contains( "Player 2 (" + name + ") disconnected" ) );

    // The partner keeps playing.
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, '.' );
    CHECK( harness.run_until( []() {
        return to_turn<int>( calendar::turn ) > 0;
    } ) );
}
```

`message_log_contains()` добавляется в `tests/mp_session_harness.h`: проходит по
`Messages::recent_messages( 1000 )` и ищет подстроку в тексте любого сообщения.

- [ ] **Step 2: Прогнать — падает**

Run: `tests/mpserver-cata_test "a_partner_disconnect_does_not_end_the_session" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: FAIL — сессия завершается, `disconnected` не выставляется.

- [ ] **Step 3: Реализовать**

`mp_server_main.cpp`: `mp_server_mark_disconnected( slot )` — найти сессию слота, закрыть
транспорт, `mp_set_disconnected( slot, true )`, записать в журнал через `Messages::add_msg`
(текст через `_( )`), затем `mp_server_end_if_no_clients()`.

`mp_curses_server.cpp`: в ветке `!mp_server_peer_connected()` во время партии не звать
`mp_request_session_end()`, а вызвать `mp_server_mark_disconnected( mp_active_slot() )`;
сохранить защиту от повторной обработки. Логировать в консоль сервера факт обрыва, но
сессию не останавливать, пока есть другие клиенты.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS; тест Ф4 на смерть/обрыв, который ожидал конец сессии, обновлён под новый
контракт.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_curses_server.cpp src/mp/mp_server_main.h src/mp/mp_server_main.cpp \
        tests/mp_session_test.cpp
git commit -m "feat(mp): a disconnect marks the seat and leaves the session running"
```

---

### Task 4: Возврат клиента в тот же слот

**Files:**
- Modify: `src/mp/mp_server_main.cpp`, `src/mp/mp_server_main.h`
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: Task 1 (`mp_set_disconnected`), Task 2 (connected count), `mp_character_card`,
  `mp_session::card_save_id`.
- Produces:
  - `std::optional<size_t> mp_server_seat_for_card( const std::string &save_id );`
  - `void mp_server_reconnected( size_t slot );`
  - запись в журнал `Player %d (%s) reconnected`.

- [ ] **Step 1: Написать падающий тест**

```cpp
TEST_CASE( "a_reconnecting_client_gets_the_same_character_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();
    const std::string save_id = g->player_at( 1 ).get_save_id();

    harness.drop_client( 1 );
    harness.run_until( [&]() {
        return mp_player_is_disconnected( 1 );
    } );
    CHECK( harness.reconnect_client( 1, save_id ) );

    CHECK_FALSE( mp_player_is_disconnected( 1 ) );
    CHECK( g->player_at( 1 ).get_name() == name );
    CHECK( harness.message_log_contains( "Player 2 (" + name + ") reconnected" ) );
}
```

`reconnect_client( slot, save_id )` добавляется в харнесс: новый транспорт для того же
слота с картой `save_id`, без меню создания персонажа.

- [ ] **Step 2: Прогнать — падает**

Run: `tests/mpserver-cata_test "a_reconnecting_client_gets_the_same_character_back" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: FAIL — слот не возвращается, `disconnected` не снимается.

- [ ] **Step 3: Реализовать**

`mp_server_main.cpp`:

- `mp_server_seat_for_card( save_id )` ищет отключённый слот с тем же `card_save_id`.
- `mp_server_reconnected( slot )`: `mp_set_disconnected( slot, false )`, лог
  `Player %d (%s) reconnected`, разбудить поток слота.
- В обработке нового подключения после старта партии: если карта совпала с отключённым
  слотом — вернуть его; до Task 6 все прочие подключения отклоняются с причиной `no free
  seat`.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp tests/mp_session_test.cpp \
        tests/mp_session_harness.h
git commit -m "feat(mp): a reconnecting client resumes its own character"
```

---

### Task 5: Смерть, меню замены и размещение

**Files:**
- Modify: `src/do_turn.cpp`, `src/mp/mp_server_main.cpp`, `src/mp/mp_server_main.h`,
  `src/mp/mp_join_menu.cpp`, `src/mp/mp_join_menu.h`
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: `place_new_character()`, `mp_run_join_menu()`, `mp_find_free_tile_near()`,
  Task 1 (`mp_set_disconnected`), Task 2 (connected count).
- Produces:
  - `bool mp_player_needs_replacement( size_t slot );`
  - `void mp_server_notify_death( size_t slot );`
  - `void mp_run_replacement_menu( size_t slot );`

- [ ] **Step 1: Написать падающие тесты**

```cpp
TEST_CASE( "a_death_opens_a_replacement_instead_of_ending_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    avatar &dead = g->player_at( 1 );
    const tripoint_bub_ms corpse = dead.pos_bub();

    dead.set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK_FALSE( mp_session_end_requested() );
    CHECK( dead.is_dead_state() );
    CHECK_FALSE( get_map().i_at( corpse ).empty() );

    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&]() {
        return !g->player_at( 1 ).is_dead_state();
    }, 60 ) );
    CHECK( g->player_at( 1 ).pos_bub() != corpse );
}

TEST_CASE( "the_session_does_not_end_when_the_last_character_dies", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_session_end_requested() );
}

TEST_CASE( "a_dead_disconnected_avatar_reconnects_into_creation", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string save_id = g->player_at( 1 ).get_save_id();
    harness.drop_client( 1 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
    CHECK( harness.reconnect_client( 1, save_id ) );
    harness.choose_random_character( 1 );
    CHECK( harness.run_until( []() {
        return !g->player_at( 1 ).is_dead_state();
    }, 60 ) );
}

TEST_CASE( "a_disconnect_while_creating_a_replacement_frees_the_seat", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    harness.drop_client( 1 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
}
```

- [ ] **Step 2: Прогнать — падает**

Run: `tests/mpserver-cata_test "a_death_opens_a_replacement_instead_of_ending_the_session" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: FAIL — сессия завершается, замены нет.

- [ ] **Step 3: Реализовать**

- `src/do_turn.cpp`: убрать `mp_request_session_end( ... died ... )` из цикла смерти;
  вместо этого `mp_server_notify_death( slot )` и продолжить. Мёртвый слот не держит
  барьер (Task 1/состояние `dead`).
- `mp_server_main.cpp`: `mp_server_notify_death( slot )` помечает слот и будит его поток.
- `mp_run_player_thread`: ветка `ended` больше не зовёт `mp_request_session_end()` для
  смерти: мёртвого она отдаёт `mp_server_notify_death()`, а добровольный выход — тому же
  пути, что обрыв (`mp_server_mark_disconnected`). Если `mp_player_needs_replacement( slot )`,
  вызвать `mp_run_replacement_menu( slot )`; меню — тот же `mp_run_join_menu()` без Continue.
- `mp_run_replacement_menu`: после выбора `place_new_character( g->player_at( slot ),
  mp_find_free_tile_near( anchor ) )`; ошибка размещения — сообщение игроку и повтор меню.
- Сессия не завершается; `mp_server_end_if_no_clients()` не вызывается из смерти.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS; труп остаётся, замена появляется рядом.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/do_turn.cpp src/mp/mp_server_main.h src/mp/mp_server_main.cpp \
        src/mp/mp_join_menu.h src/mp/mp_join_menu.cpp tests/mp_session_test.cpp
git commit -m "feat(mp): death opens a replacement character instead of ending the session"
```

---

### Task 6: Вход посреди партии в слот `dead && disconnected`

**Files:**
- Modify: `src/mp/mp_server_main.cpp`, `src/mp/mp_server_main.h`
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: Task 2 (`mp_server_seat_is_free_for_new_client`), Task 4 (seat by card),
  `mp_server_seat_client()`, `mp_run_join_menu()`.
- Produces:
  - `std::optional<size_t> mp_server_free_seat_for_new_client();`
  - `bool mp_server_accept_new_client( std::unique_ptr<mp_session> session, std::string &err );`

- [ ] **Step 1: Написать падающие тесты**

```cpp
TEST_CASE( "a_new_client_can_take_a_dead_disconnected_seat", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.drop_client( 1 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    CHECK( harness.run_until( []() {
        return g->player_at( 1 ).get_name() != "";
    } ) );
    CHECK( harness.seat_occupied( 1 ) );
}

TEST_CASE( "a_new_client_is_refused_when_no_seat_is_free", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    std::string err;
    CHECK_FALSE( harness.try_admit_new_client( "Cid", err ) );
    CHECK( err.find( "no free seat" ) != std::string::npos );
}
```

`try_admit_new_client( username, err )` добавляется в харнесс: соединение без свободного
слота, возвращает false и причину отказа.

- [ ] **Step 2: Прогнать — не собирается**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: ошибка компиляции на новых функциях харнесса.

- [ ] **Step 3: Реализовать**

- `mp_server_free_seat_for_new_client()`: первый слот, для которого
  `mp_server_seat_is_free_for_new_client()`.
- `mp_server_accept_new_client()`: принять handshake, выбрать свободный слот; если нет —
  отправить `bye` с причиной `no free seat` и вернуть false; иначе
  `mp_server_seat_client( slot, std::move( session ) )`, поток слота запускает join-меню
  без Continue, затем `place_new_character()` рядом с группой.
- Слушатель не закрывается после `g_join_phase = false`; цикл accept продолжается, пока
  сессия жива.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_server_main.h src/mp/mp_server_main.cpp tests/mp_session_test.cpp \
        tests/mp_session_harness.h
git commit -m "feat(mp): a newcomer fills a dead disconnected seat"
```

---

### Task 7: JSON-конфиг сервера и необязательные CLI-аргументы

**Files:**
- Create: `src/mp/mp_server_config.h`, `src/mp/mp_server_config.cpp`
- Modify: `src/main.cpp`, `Makefile`, `CMakeLists.txt`
- Test: `tests/mp_server_config_test.cpp`

**Interfaces:**
- Consumes: `mp_set_hurry_lengths()`.
- Produces:
  - `struct mp_server_config` с `std::optional<int> port`, `std::optional<std::string> world`,
    `std::optional<int> players`, `std::optional<int> hurry_ms`,
    `std::optional<int> hurry_combat_ms`.
  - `struct cli_server_overrides` с теми же пятью `std::optional` полями.
  - `bool mp_load_server_config( const std::string &path, mp_server_config &out,
    std::string &err );` — отсутствующий файл не ошибка: `out` пуст, `true`.
  - `bool mp_apply_cli_server_config( const mp_server_config &file,
    const cli_server_overrides &cli, int &port, std::string &world, int &players,
    std::string &err );` — CLI сильнее файла; отсутствие порта/мира после слияния — `false`
    и `err`.

- [ ] **Step 1: Написать падающие тесты**

`tests/mp_server_config_test.cpp`:

```cpp
TEST_CASE( "server_config_reads_every_key", "[mp]" )
{
    mp_server_config cfg;
    std::string err;
    REQUIRE( mp_load_server_config( "tests/data/mp/server-config-ok.json", cfg, err ) );
    CHECK( cfg.port.value() == 4321 );
    CHECK( cfg.world.value() == "coop" );
    CHECK( cfg.players.value() == 2 );
    CHECK( cfg.hurry_ms.value() == 5000 );
    CHECK( cfg.hurry_combat_ms.value() == 2000 );
}

TEST_CASE( "server_config_refuses_unknown_keys_and_bad_json", "[mp]" )
{
    mp_server_config cfg;
    std::string err;
    CHECK_FALSE( mp_load_server_config( "tests/data/mp/server-config-unknown.json", cfg, err ) );
    CHECK_FALSE( err.empty() );
    CHECK_FALSE( mp_load_server_config( "tests/data/mp/server-config-bad.json", cfg, err ) );
}

TEST_CASE( "cli_overrides_the_config_file", "[mp]" )
{
    mp_server_config file;
    file.port = 4000;
    file.world = "file-world";
    file.players = 3;
    cli_server_overrides cli;
    cli.port = 5000;
    cli.world = "cli-world";
    int port = 0;
    std::string world;
    int players = 0;
    std::string err;
    REQUIRE( mp_apply_cli_server_config( file, cli, port, world, players, err ) );
    CHECK( port == 5000 );
    CHECK( world == "cli-world" );
    CHECK( players == 3 );
}

TEST_CASE( "server_config_refuses_missing_port_or_world", "[mp]" )
{
    mp_server_config file;
    cli_server_overrides cli;
    int port = 0;
    std::string world;
    int players = 1;
    std::string err;
    CHECK_FALSE( mp_apply_cli_server_config( file, cli, port, world, players, err ) );
    CHECK_FALSE( err.empty() );
}
```

- [ ] **Step 2: Прогнать — не собирается**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "[mp]" --user-dir=/tmp/cata-mp-p5s/`

Expected: ошибка компиляции на `mp_server_config`.

- [ ] **Step 3: Реализовать**

- Парсер JSON: только перечисленные ключи, строгая типизация; битый JSON/неизвестный ключ/
  неверный тип → `err`, `false`.
- `mp_apply_cli_server_config`: для каждого поля CLI-значение сильнее файла; отсутствие порта
  и мира после слияния → `err`, `false`.
- `src/main.cpp`: `--server`, `--world` и `--players` получают `num_args = 0`; обработчик
  смотрит на следующий токен `argv[0]`, и если он не начинается с `-`, разбирает его и
  возвращает 1 (иначе 0 и значение берётся из файла). Это сохраняет `--server 4321` и
  разрешает голый `--server`; `cli_opts` хранит `std::optional<int>` для port/players и
  `std::optional<std::string>` для world. Путь конфига — `PATH_INFO::config_dir() +
  "/mp/server.json"`; читать файл до `mp_server_main`; `run_server_session` получает
  итоговые port/world/players/hurry.
- `Makefile`/`CMakeLists.txt`: добавить `mp_server_config.cpp`.
- Тестовые JSON-файлы: `tests/data/mp/server-config-ok.json`,
  `server-config-unknown.json`, `server-config-bad.json`.

- [ ] **Step 4: Прогнать — зелено**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5s/`

Expected: PASS; отдельно проверить `./mpserver-cataclysm-server --help` и запуск с
`--server` без порта при наличии `server.json`.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add src/mp/mp_server_config.h src/mp/mp_server_config.cpp src/main.cpp \
        Makefile CMakeLists.txt tests/mp_server_config_test.cpp tests/data/mp/
git commit -m "feat(mp): server options come from a JSON file as well as the command line"
```

---

### Task 8: Документация и ворота

**Files:**
- Modify: `doc/MULTIPLAYER.md`, `docs/superpowers/specs/2026-11-18-mp-phase5-lifecycle-design.md`
- Test: все наборы

**Interfaces:**
- Consumes: весь предыдущий код.

- [ ] **Step 1: `doc/MULTIPLAYER.md`**

Добавить: обрыв не завершает сессию, `Player N (name) disconnected/reconnected`; смерть и
меню замены, труп остаётся; вход посреди партии только в слот `мёртв + отключён`; конец
сессии по числу подключённых; JSON-конфиг и приоритет.

- [ ] **Step 2: Статус спека**

В шапке `2026-11-18-mp-phase5-lifecycle-design.md` заменить «план ещё не написан» на
«реализовано; автоматические ворота …» после прохождения.

- [ ] **Step 3: Ворота автотестов**

```bash
make -j$(nproc) CCACHE=1 TILES=0 TESTS=1
tests/cata_test "[mp],[coop]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5-gate/

make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=/tmp/cata-mp-p5-gate-s/

python3 tools/run_tests_parallel.py -j $(nproc) --seed 1
```

Expected: оба `[mp]`-набора зелёные; полный прогон — те же известные падения, ничего нового.
Раннер не коммитить.

- [ ] **Step 4: Ворота человека**

Один сервер и два клиента (две машины или два tmux-окна), сценарий:

1. Один клиент закрывается — второй продолжает играть, в журнале `Player N (name) disconnected`.
2. Клиент возвращается — тот же персонаж, `reconnected`.
3. Персонаж умирает — меню создания замены, труп на месте, замена рядом.
4. Последний клиент уходит — сессия завершается.

- [ ] **Step 5: Коммит**

```bash
make astyle
git add doc/MULTIPLAYER.md docs/superpowers/specs/2026-11-18-mp-phase5-lifecycle-design.md
git commit -m "docs(mp): close phase 5 -- lifecycle and server config"
```

---

## Заметки для реализующего

1. **`disconnected` — не `away`.** `away` — игроковое решение, снимается клавишей;
   `disconnected` — серверное, снимается возвратом клиента. Не сливать.
2. **Смерть не должна звать `mp_request_session_end()`.** Единственный путь конца сессии —
   `mp_server_end_if_no_clients()`.
3. **Создание замены — не новый слот.** Слот остаётся тем же; меняется только avatar.
4. **Новый клиент не создаёт слот.** Он занимает только `dead && disconnected`; иначе отказ.
5. **JSON-парсер не должен терпеть неизвестные ключи.** Опечатка в конфиге должна быть
   видна оператору, а не молча проигнорирована.
