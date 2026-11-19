# Краш mouse view при смене активного игрока: per-player live_view — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (выбран
> пользователем 2026-11-19: нативно + один финальный ревьюер на весь дифф). Шаги отмечены
> чекбоксами (`- [ ]`).

**Goal:** убрать единственный `game::live_view`, из-за которого подсказка mouse view одного
слота регистрирует `ui_adaptor` в чужом ui-стеке и следующий ввод другого слота его
уничтожает (`cata_assert( owner == active_stack )` → SIGABRT); хранить подсказку в
`player_context`, как `wait_popup`, и гасить её при teardown контекста.

**Architecture:** `player_context` получает `std::unique_ptr<live_view> liveview` и оставляет
себе полный тип через forward declaration, как уже сделано для `static_popup`. Доступ —
`live_view &mp_active_live_view()` в `mp_context.cpp` (ленивое создание, по образцу
`wait_popup`); `game::liveview_ptr/liveview` и мёртвый `live_view::init()` удаляются.
Деструктор `player_context` гасит `liveview` и `wait_popup` под собственным `ui_stack`
(иначе `~ui_adaptor` падает на ассерте владения) и возвращает прежний стек, а если это был
свой стек — world stack. `mp_sync_context_count()` гасит `liveview` до
`cata_assert( dying.ui_stack.empty() )`, рядом с `wait_popup`.

**Tech Stack:** C++17, Catch2 2.13.10, `ui_adaptor`/`ui_stack_t`, серверная сборка
`MP_SERVER=1`, `mp_session_harness` (loopback-сессии, скриптованный ввод, клиентские сетки).

**Spec:** отдельного дизайн-документа нет; требования — §Global Constraints этого плана и
согласованная с пользователем сводка 2026-11-19 (причина, фикс, teardown, тесты, ручной
прогон). Наблюдение из ручного прогона: подсказка висела после TIMEOUT; клавиша второго
игрока её уничтожала и роняла процесс.

## Global Constraints

- **Ветка:** `fix/mp-liveview` от `mp`. В `mp` вливать только после ручного прогона
  пользователя (сценарий в Task 2); мерж — отдельным действием после его «прошло».
- **Вне захода:** `game::overmap_data.ui` — тот же класс владения, но `[INFERENCE]`; отдельная
  задача, запись в леджере и финальном сообщении. В этом плане не трогать.
- **Артефакты:** всё временное — в `.data/` (user-dir прогонов, логи), не в `/tmp`.
- **Сборки:** серверная `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`;
  обычная тестовая `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`; клиентская
  `make -j$(nproc) CCACHE=1 TILES=0 MULTIPLAYER=1 BUILD_PREFIX=mpclient-`. `RELEASE=0` —
  ассерты живые, не отключать. Новых файлов нет, Makefile/CMakeLists не менять.
- **Тесты:** серверный гейт — `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test --seed 1 -j $(nproc) "[mp]"`;
  обычный — `tests/cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/<свежий>/`.
  «Зелёный» — это дифф к снятому до изменения baseline, а не абсолютный ноль падений.
- **Стиль:** 100 колонок, комментарии по-английски; `astyle` недоступен — проверка глазами.
- **Коммиты:** `test(mp):`/`fix(mp):`, без `Co-Authored-By`. Леджер и `.data/` не коммитить;
  `AGENTS.md` не коммитить (локальные заметки, untracked).
- **Заголовки и `make clean`.** `.d`-файлы этой сборки перечисляют только `.cpp` и PCH, а не
  заголовки, поэтому правка `src/game.h` (раскладка `game`) не пересобирает потребителей:
  бинарь из старых объектов падает в `faction_manager::create_if_needed` (`this=0x0`). После
  любой правки заголовка — `make clean` перед первой сборкой; далее у server/plain/client
  свои `*obj`, так что одного clean на все три достаточно, пока заголовки не менялись.
- **Ассерты `owner == active_stack` остаются громкими.** Ослаблять `~ui_adaptor` или
  `cata_assert` в `mp_sync_context_count()` запрещено.

## Review Focus

Входы/режимы, которые спека подразумевает, а тесты Task 1 покрывают не все; каждый пункт
проверяется ревьюером осознанно:

1. `g.reset()` при выходе (`exit_handler`) при открытой подсказке: контексты уничтожаются под
   произвольным (возможно, уже не активным) стеком — деструктор обязан переключить стек.
   Тест: `a_destroyed_context_drops_its_mouse_view_where_it_registered`.
2. Конец сессии/уменьшение числа слотов при открытой подсказке: `mp_sync_context_count()`
   должен погасить её до `cata_assert( dying.ui_stack.empty() )`. Покрыто тем же деструктором
   и существующим shrink-путём; отдельного теста нет — ревьюер проверяет порядок строк.
3. Отключение клиента, чья подсказка открыта, и ввод партнёра (основной сценарий).
   Тест: `another_players_key_does_not_close_a_mouse_view`.
4. Смена активного слота (батон) при подсказках у обоих: каждая привязана к своему стеку.
   Тест: `each_players_mouse_view_lives_in_that_players_ui_stack`.
5. Одиночный игрок (`contexts.size() == 1`, `mp_activate_world()` не переключает стек):
   mouse view продолжает работать — ручной прогон Task 2, п.1.

---

### Task 1: Reproducer, per-player storage and teardown

**Files:**
- Modify: `tests/mp_session_harness.h` (declaration `mouse_move`), `tests/mp_session_harness.cpp`
  (definition, after `press`).
- Modify: `tests/mp_session_test.cpp` (новый TEST_CASE после
  `a_player_in_a_menu_does_not_freeze_their_partner`).
- Modify: `tests/mp_context_test.cpp` (два новых TEST_CASE в конце файла).
- Modify: `src/mp/mp_context.h` (forward declaration, член, аксессор), `src/mp/mp_context.cpp`
  (include, деструктор, аксессор, shrink-путь).
- Modify: `src/game.h:1357-1358` (удалить `liveview_ptr`/`liveview`), `src/game.cpp:463,2516,2519,2527,6102`
  (удалить инициализацию, перевести вызовы), `src/handle_action.cpp:411`, `src/live_view.h:16`,
  `src/live_view.cpp:30-33`.

**Interfaces:**
- Consumes: `player_context` (`src/mp/mp_context.h`), `ui_manager::get_active_stack()`,
  `ui_manager::set_active_stack()`, `ui_manager::activate_world_stack()`, `live_view::show/hide/is_enabled`.
- Produces:
  - `std::unique_ptr<live_view> player_context::liveview;` (`// NOLINT(cata-serialize)`).
  - `live_view &mp_active_live_view();` — non-null, lazily creates the active context's view.
  - `void mp_session_harness::mouse_move( size_t slot, const point &pos );` — sends a real
    `input_event( MouseInput::Move, input_event_t::mouse )` with `mouse_pos = pos` over the
    slot's wire, the same path a client's mouse takes.

- [ ] **Step 1: Снять baseline `[mp]` на неизменённом дереве.**

Run: `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test -o .data/par-liveview-base --seed 1 -j $(nproc) "[mp]"`
Expected: записать в леджер число cases/assertions и список падений (на `mp` HEAD он был
215 cases / 4236 assertions, 0 падений). Этот список — единственный допустимый остаток на
гейте Step 7.

- [ ] **Step 2: Добавить `mouse_move` в harness.**

`tests/mp_session_harness.h`: объявление после `press(...)`, `#include "point.h"` среди
include (по алфавиту). `tests/mp_session_harness.cpp`: определение рядом с `press` — тот же
`mp_control`/`mp_wrap`, но `msg.event = input_event( MouseInput::Move, input_event_t::mouse )`
и `msg.event.mouse_pos = pos;`; добавить `#include "input_enums.h"` в cpp.

- [ ] **Step 3: Написать интеграционный RED-тест.**

```cpp
TEST_CASE( "another_players_key_does_not_close_a_mouse_view", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // P1 moves the mouse over the map. A TIMEOUT does not hide the hint, so it
    // stays up while they sit at the input wait.
    harness.mouse_move( 1, point( MP_HARNESS_COLS / 2, MP_HARNESS_ROWS / 2 ) );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), "Mouse view" );
    } ) );
    CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), "Mouse view" ) );

    // P1's terminal goes away with the hint still up...
    harness.drop_client( 1 );

    // ...and P0 presses a key. P0's hint is null, and P1's adaptor belongs to
    // P1's stack; one shared live_view would destroy it here and abort.
    const int turn_before = to_turn<int>( calendar::turn );
    harness.press( 0, '.' );
    REQUIRE( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 20 ) );
    CHECK( harness.client_connected( 0 ) );
}
```

- [ ] **Step 4: Собрать серверный тестовый бинарь и увидеть RED.**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
Run: `tests/mpserver-cata_test "another_players_key_does_not_close_a_mouse_view" --rng-seed 1 --user-dir=.data/liveview-red/`
Expected: exit 134 (SIGABRT), в stderr
`ui_manager.cpp:111: Assertion 'owner == active_stack' failed.` — то есть тест падает именно
на чужом `ui_adaptor`, а не на таймауте/ассерте теста. Записать хвост вывода в леджер.

- [ ] **Step 5: Написать два unit-теста (ожидаемый RED — ошибка компиляции нового API).**

В конец `tests/mp_context_test.cpp`, `#include "live_view.h"` рядом с `popup.h`:

```cpp
TEST_CASE( "each_players_mouse_view_lives_in_that_players_ui_stack", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->add_player_slot().set_body();
    mp_sync_context_count();

    mp_activate_player( 0 );
    mp_active_live_view().show( tripoint::zero );
    REQUIRE( mp_active_live_view().is_enabled() );

    // The hint is per player: showing slot 0's must not enable slot 1's.
    mp_activate_player( 1 );
    CHECK_FALSE( mp_active_live_view().is_enabled() );
    mp_active_live_view().show( tripoint::zero );
    REQUIRE( mp_active_live_view().is_enabled() );

    // Hiding slot 1's view must not touch slot 0's.
    mp_active_live_view().hide();
    mp_activate_player( 0 );
    CHECK( mp_active_live_view().is_enabled() );
    mp_active_live_view().hide();

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "a_destroyed_context_drops_its_mouse_view_where_it_registered", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t other;
    ui_manager::set_active_stack( &other );
    {
        player_context ctx;
        ui_manager::set_active_stack( &ctx.ui_stack );
        ctx.liveview = std::make_unique<live_view>();
        ctx.liveview->show( tripoint::zero );
        REQUIRE( ctx.ui_stack.size() == 1 );
        // Destroy the context under somebody else's stack, as g.reset() does.
        // ~ui_adaptor asserts its owner is the active stack, so the context
        // destructor has to point the stack at itself for the drop.
        ui_manager::set_active_stack( &other );
    }
    CHECK( other.empty() );
}
```

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
Expected: ошибка компиляции тестов: `mp_active_live_view` не объявлена, у `player_context`
нет члена `liveview`.

- [ ] **Step 6: Реализовать фикс.**

`src/mp/mp_context.h`: `class live_view;` рядом с остальными forward declaration; член после
`wait_popup`:

```cpp
    // This player's mouse-view hint. The ui_adaptor it raises has to be created
    // and destroyed under this player's own ui stack (~ui_adaptor asserts
    // owner == active_stack), which one `game`-wide live_view could not honour:
    // one player's key destroyed the hint another player had opened.
    std::unique_ptr<live_view> liveview; // NOLINT(cata-serialize)
```

Объявление аксессора рядом с `mp_active_context_ptr()`:
`live_view &mp_active_live_view(); // the active player's, created on first use`

`src/mp/mp_context.cpp`: `#include "live_view.h"` (по алфавиту). Заменить
`player_context::~player_context() = default;` на определение, которое сохраняет
`ui_manager::get_active_stack()`, ставит `&ui_stack`, сбрасывает `liveview` и `wait_popup`,
затем возвращает прежний стек, а если прежний — свой, вызывает
`ui_manager::activate_world_stack()` (свой стек уже мёртв). Добавить:

```cpp
live_view &mp_active_live_view()
{
    player_context &ctx = mp_active_context();
    if( !ctx.liveview ) {
        ctx.liveview = std::make_unique<live_view>();
    }
    return *ctx.liveview;
}
```

В `mp_sync_context_count()` после `dying.wait_popup.reset();` добавить
`dying.liveview.reset();` (до `cata_assert( dying.ui_stack.empty() )`).

`src/game.h`: удалить оба объявления `liveview_ptr`/`liveview`.
`src/game.cpp`: удалить `liveview( *liveview_ptr ),` из списка инициализации; в
`handle_mouseview` — `mp_active_live_view().show(...)` / `.hide()`; в
`draw_look_around_cursor` — `mp_active_live_view().is_enabled()`; удалить
`#include "live_view.h"`.
`src/handle_action.cpp`: `mp_active_live_view().is_enabled()`; удалить `#include "live_view.h"`.
`src/live_view.h`/`src/live_view.cpp`: удалить `void init();` и `live_view::init()`.

- [ ] **Step 7: Пересобрать и увидеть GREEN трёх тестов.**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
Run: `tests/mpserver-cata_test "another_players_key_does_not_close_a_mouse_view,each_players_mouse_view_lives_in_that_players_ui_stack,a_destroyed_context_drops_its_mouse_view_where_it_registered" --rng-seed 1 --user-dir=.data/liveview-green/`
Expected: 3 cases, 0 failed (все assertions прошли).

- [ ] **Step 8: Серверный `[mp]`-гейт.**

Run: `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test -o .data/par-liveview --seed 1 -j $(nproc) "[mp]"`
Expected: падения совпадают с baseline из Step 1 (baseline был 0 падений → здесь тоже 0);
cases = baseline + 3. Любое новое падение — стоп, `systematic-debugging`, не гейт.

- [ ] **Step 9: Коммит.**

```bash
git add src/mp/mp_context.h src/mp/mp_context.cpp src/game.h src/game.cpp \
        src/handle_action.cpp src/live_view.h src/live_view.cpp \
        tests/mp_session_harness.h tests/mp_session_harness.cpp \
        tests/mp_session_test.cpp tests/mp_context_test.cpp
git commit -m "fix(mp): keep a player's mouse view in their own ui stack"
```

---

### Task 2: Сборки, обычный гейт, ручной сценарий

**Files:**
- Modify (локально, без коммита): `AGENTS.md` — предусловие ручного прогона.
- Ledger: `.superpowers/sdd/2026-11-19-mp-liveview-ownership/progress.md`.

**Interfaces:**
- Consumes: коммит Task 1.
- Produces: доказательство «все три сборки зелёные» + инструкция ручного прогона пользователю.

- [ ] **Step 1: Обычная тестовая сборка и `[mp],[coop]`.**

Run: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1`  (полная сборка: `obj/` пуст после `make clean`)
Run: `tests/cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/liveview-plain/`
Expected: 0 failed. Это сборка без `MULTIPLAYER`, поэтому это sanity-прогон незагейченных
`[mp]`-тестов (фаза 5 меряла 164-case набор на `MULTIPLAYER=1`).

- [ ] **Step 2: Клиентская сборка.**

Run: `make -j$(nproc) CCACHE=1 TILES=0 MULTIPLAYER=1 BUILD_PREFIX=mpclient-`
Expected: успешная сборка (server-only код не тронут, но `src/mp/` компилируется целиком).

- [ ] **Step 3: Записать в `AGENTS.md` предусловие ручного сценария.**

Добавить раздел `## Manual MP gate: mouse view ownership` с пунктами:
1. Одиночный клиент: навести мышь на карту — подсказка `< Mouse view >` появляется и
   закрывается любой клавишей.
2. Два клиента: мышь над картой у Bo — подсказка у Bo; клавиша/движение Ada её не трогают.
3. Не отпуская подсказку, закрыть терминал Bo; Ada продолжает играть, сервер жив.
4. Выход из игры при открытой подсказке (последний клиент уходит) не роняет процесс.
Файл не коммитить.

- [ ] **Step 4: Финальный self-check леджера.**

Run: `git log --oneline fix/mp-liveview -3 && git status --short`
Expected: два коммита поверх `mp` — план (`docs(mp): plan …`) и фикс (`fix(mp): …`), плюс
финальный коммит правок плана по факту прогонов; незакоммичены только `AGENTS.md` и
`docs/superpowers/plans/2026-11-19-mp-baton-hold-diagnosis.md` (как и до работы).

- [ ] **Step 5: Передать пользователю.**

Сообщить: ветка, коммит, результаты гейтов, ручной сценарий, отдельная задача про
`game::overmap_data.ui` (`[INFERENCE]`). В `mp` не вливать до «прошло».

---

## Self-review плана

- Spec coverage: причина (общий `live_view`) → Task 1; teardown в двух путях → Task 1 Steps 5-6;
  тесты RED→GREEN → Task 1 Steps 3-7; гейты → Task 1 Step 8, Task 2 Steps 1-2; ручной прогон и
  мерж после него → Task 2 Steps 3-5; `overmap_data.ui` вне захода → Global Constraints.
- Step scan: каждый шаг — одно действие с проверяемым результатом; тела даны только там, где
  сигнатура и тест их не определяют (деструктор, аксессор, harness).
- Type consistency: `player_context::liveview`, `mp_active_live_view()`,
  `mp_session_harness::mouse_move( size_t, const point & )` совпадают во всех задачах.
- Review Focus: пять пунктов, у четырёх есть тест, пятый — ручной прогон Task 2.
- Proportion: план короче кода, который описывает; код-блоков три.
