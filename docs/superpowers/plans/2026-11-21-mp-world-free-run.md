# Свободный прогон мира после «тороплю» и обрыва — план диагностики и фикса

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** мир не прокручивает ходы со скоростью CPU, когда единственный оставшийся за столом
игрок снят с барьера «тороплю», а его партнёр отключился.

**Architecture:** сначала вердикт — какой предикат разрешает ходу закрываться без игрока за
клавиатурой; затем точечная правка темпа и регрессионный тест на харнессе с укороченным
отсчётом; затем гейты и повтор живого сценария.

**Tech Stack:** C++17, Catch2 2.13.10, `mp_session_harness`, серверная сборка `MP_SERVER=1`,
tmux для живого прогона.

**Spec:** `docs/superpowers/specs/2026-08-08-mp-phase3a-scheduler-design.md` §7.3 (правило
темпа) и `docs/superpowers/specs/2026-11-18-mp-phase5-lifecycle-design.md` §5.1 (обрыв не
завершает сессию).

## Доказательства (на момент написания плана)

- Живой прогон 2026-11-20 (`.superpowers/sdd/2026-11-20-inventory-invlet-crash/task-4-report.md`,
  «Concern 2»): сразу после `"Genie Roe" disconnected …; the session continues` календарь прошёл
  8:00:27 → 8:06:43 примерно за 19 с (~20 ходов/с), с автосейвом; трасса планировщика —
  `turn-start → poll-begin → poll-end → park` каждые 50 мс без передач эстафеты. Следующее
  нажатие остановило прогон. Свежий обрыв без предшествующего «тороплю» (run D) прогона не дал.
- Механика снятия: `mp_turn_reset()` (`src/mp/mp_turn.cpp:283`) снимает `g_dropped_by_countdown[i]`
  только если игрок **не** на ожидании ввода. Игрок в открытом меню остаётся снятым, пока
  сидит в нём — это записано в §7.3 («одно нажатие не должно покупать ровно один тик»).
- Темп задаёт `mp_turn_paced()` (`src/mp/mp_turn.cpp:363-396`). Он пропускает только `away`
  (`:375`), а **отключённое место** доходит до `:383`: у него нет активности и дальней цели,
  поэтому функция возвращает `false`, и пол отсчёта не включается вовсе. Сценарий сходится:
  партнёр отключён, оставшийся игрок снят → барьер удовлетворён, темпа нет → мир крутится.
- Спека требует пол прямо: §7.3, «Если играть некому — все живые сняты или заняты
  активностью, — ход длится не меньше длины отсчёта (4 с в бою, 15 с иначе): иначе мир крутил
  бы ходы со скоростью CPU вокруг думающего игрока».

## Global Constraints

- **Ветка:** `fix/mp-world-free-run` от `mp` (`72f859f14c`). В `mp` — после вердикта пользователя.
- **Артефакты:** всё временное — в `.data/`; ничего из `.data/` не коммитить.
- **Сборки:** сервер `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1
  BUILD_PREFIX=mpserver-`; не-серверный `... TILES=0 TESTS=1 MULTIPLAYER=1
  BUILD_PREFIX=mpclient-`; обычный `... TILES=0 TESTS=1`. `RELEASE=0`, ассерты живые.
  После правки `.h` — `make clean` для этого префикса (`.d` не перечисляют заголовки).
- **Тесты:** серверный гейт `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/<run>/`;
  не-серверный `tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/<run>/`;
  полный `python3 tools/run_tests_parallel.py -o .data/<run> --seed 1`. «Зелёный» — дифф к
  baseline (239 случаев `[mp]`, 0 падений).
- **Ф4/Ф5 не ломать:** обрыв по-прежнему не завершает сессию; барьер и длины отсчётов не
  меняются — меняется только условие, при котором ход получает пол.
- **Стиль:** 100 колонок, комментарии по-английски; коммиты `fix(mp):`/`test(mp):`/`docs(mp):`,
  без `Co-Authored-By`.

## Review Focus

1. Обрыв **без** предшествующего «тороплю»: партнёр держит барьер, прогона нет — правка не
   должна это изменить.
2. Оба живых места сняты: ход длится пол, мир не стоит и не летит.
3. Игрок «отошёл» (`away`): пол не включается — иначе партнёр с активностью ждёт 15 с на ходу.
4. Мёртвое место не участвует ни в барьере, ни в темпе.
5. Бой: пол — боевая длина, даже если оставленный позади игрок читает.
6. Нажатие клавиши оставленным позади игроком возвращает обычный темп.

---

### Task 1: Детерминированное воспроизведение и вердикт

**Files:**
- Test: `tests/mp_session_test.cpp` (новый кейс)
- Отчёт: `docs/superpowers/reports/2026-11-21-mp-world-free-run.md`
- Логи: `.data/world-run/` (не коммитить)

**Interfaces:**
- Consumes: `mp_session_harness` (`press`, `run_until`, `drop_client`, `screen`),
  `mp_hurry_remaining_ms( mp_hurry_source )`, `mp_advance_hurry_for_test( ms, source )`,
  `mp_player_state_of( slot )`, `mp_player_is_disconnected( slot )`,
  `mp_set_hurry_lengths( manual_ms, combat_ms )` (`src/mp/mp_turn.h`), `to_turn<int>()`.
- Produces: кейс `a_left_behind_player_paces_the_world_after_the_partner_disconnects` и
  вердикт с таблицей состояний.

- [ ] **Step 1: Тест-репро.**

Кейс по образцу `inventory_reopen_cycles_do_not_crash` (`tests/mp_session_test.cpp:1877`):

```cpp
TEST_CASE( "a_left_behind_player_paces_the_world_after_the_partner_disconnects", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // The harness constructor calls mp_turn_reset_all(), which puts the shipped
    // countdown lengths back: shorten them after it, not before.
    REQUIRE( mp_set_hurry_lengths( 1000, 1000 ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    const auto inventory_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    };
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( inventory_is_open ) );

    // Seat 1 hurries; the expiry drops seat 0 from the barrier for as long as it
    // sits in the menu it was dropped in.
    harness.press( 1, '`' );
    REQUIRE( harness.run_until( []() {
        return mp_hurry_remaining_ms( mp_hurry_source::manual ) > 0;
    } ) );
    mp_advance_hurry_for_test( MP_HURRY_MS + 1, mp_hurry_source::manual );
    REQUIRE( harness.run_until( []() {
        return mp_player_state_of( 0 ) == mp_player_state::not_waited;
    } ) );

    // The partner's client leaves; the session continues by design.
    harness.drop_client( 1 );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_disconnected( 1 );
    } ) );

    // The world may keep moving, but a turn nobody plays in lasts the countdown
    // it was dropped by: two turns must not flash past in milliseconds.
    const int before = to_turn<int>( calendar::turn );
    const auto started = std::chrono::steady_clock::now();
    REQUIRE( harness.run_until( [before]() {
        return to_turn<int>( calendar::turn ) >= before + 2;
    }, 20 ) );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started );
    CHECK( elapsed >= std::chrono::milliseconds( 1500 ) );
}
```

- [ ] **Step 2: Прогнать и записать RED.**

Run: `tests/mpserver-cata_test
"a_left_behind_player_paces_the_world_after_the_partner_disconnects" --rng-seed 1
--user-dir=.data/world-run/red/`
Expected: FAIL на `elapsed >= 1500ms` (обрыв в миллисекундах). Записать `elapsed` в отчёт.

- [ ] **Step 3: Снять состояние в момент решения.**

Временный `DebugLog` в `mp_turn_paced()` перед `return false` на `:383` печатает слот,
`mp_player_left_behind(i)`, `mp_player_is_disconnected(i)`, `activity`/`has_distant_destination()`
и возраст хода; прогнать кейс, записать строки, лог удалить. Ожидаемо: отключённый слот 1
печатается как «игрок за клавиатурой», хотя `mp_player_left_behind(0)` истинно.

- [ ] **Step 4: Записать вердикт.**

`docs/superpowers/reports/2026-11-21-mp-world-free-run.md`: симптом (живой лог, цифры),
механизм (снятие живёт, пока игрок в меню; пол обязан работать, но `mp_turn_paced()` считает
отключённое место игроком за клавиатурой и выключает пол), почему run D был чист (без снятия
барьер не удовлетворён и пол не нужен), что именно правится в Task 2.

- [ ] **Step 5: Коммит.**

```bash
git add tests/mp_session_test.cpp docs/superpowers/reports/2026-11-21-mp-world-free-run.md
git commit -m "test(mp): pin the free run after a drop and a disconnect"
```

---

### Task 2: Пол темпа не выключается отключённым местом

**Files:**
- Modify: `src/mp/mp_turn.cpp:363-396` (`mp_turn_paced`), `src/mp/mp_turn.h:80-94` (контракт)
- Test: кейс из Task 1

**Interfaces:**
- Consumes: `mp_player_is_away( slot )`, `mp_player_is_disconnected( slot )` (`src/mp/mp_turn.h`).
- Produces: пункт контракта: «место без игрока — `away` или `disconnected` — не считается
  игроком за клавиатурой ни для барьера, ни для темпа».

- [ ] **Step 1: RED.** Убедиться, что кейс Task 1 падает на текущем коде; зафиксировать вывод.

- [ ] **Step 2: Правка.**

В цикле `mp_turn_paced()` расширить пропуск:

```cpp
        // Away first, before the left-behind rule: an away player asked not to
        // be waited on ... A disconnected seat is the same absence from the other
        // side: nobody is at that keyboard, and counting it as a player at the
        // keyboard switched the countdown floor off for a partner who was still
        // reading after a hurry drop -- the free run.
        if( mp_player_is_away( i ) || mp_player_is_disconnected( i ) ) {
            continue;
        }
```

Обновить абзац контракта в `src/mp/mp_turn.h` (`:80-94`): «away **или disconnected** …
не игрок за клавиатурой; пол считается по оставленным позади и занятым».

- [ ] **Step 3: GREEN.**

Run: `tests/mpserver-cata_test
"a_left_behind_player_paces_the_world_after_the_partner_disconnects" --rng-seed 1
--user-dir=.data/world-run/green/`
Expected: PASS; повторить `--rng-seed 1..5`.

- [ ] **Step 4: Гейты.**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/world-run/mp/`
Run: `tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/world-run/cl/`
Expected: 0 failed в обоих (сервер — 240 случаев).

- [ ] **Step 5: Коммит.**

```bash
git add src/mp/mp_turn.cpp src/mp/mp_turn.h
git commit -m "fix(mp): pace the world when the only player left is dropped"
```

---

### Task 3: Живой прогон сценария run C и полный прогон

**Files:** без кода; отчёт `.data/world-run/live/report.md`.

- [ ] **Step 1: Живой прогон.**

Сервер на копии мира (`.data/inv-crash/live/e2e-final`, порт 4404), два клиента через
`Continue as`. Повторить 12 циклов `i` / «тороплю» / ожидание 16 с / `.` в окне Bo, затем
закрыть клиент Ada. Наблюдать календарь 20 с: ожидается не больше 2–3 ходов (пол 15 с), а не
сотни; следующее нажатие Bo возвращает обычный темп. Логи и экраны — в `.data/world-run/live/`.

- [ ] **Step 2: Полный прогон.**

Run: `python3 tools/run_tests_parallel.py -o .data/world-run/full --seed 1 -j $(nproc)`
Expected: нет падений сверх baseline (известные seed-флейки).

- [ ] **Step 3: Отчёт и передача.**

Записать результат живого прогона, цифры календаря до/после, и передать пользователю:
сливать в `mp` или нет.

## Self-review плана

- **Coverage:** воспроизведение → вердикт → фикс → гейты → живой прогон; у каждого шага
  проверяемый результат.
- **Step scan:** ни один шаг не говорит «поправить что-нибудь»; правка — одна ветка условия,
  тест — один кейс.
- **Type consistency:** имена `mp_turn_paced`, `mp_player_left_behind`, `mp_player_is_away`,
  `mp_player_is_disconnected`, `mp_set_hurry_lengths`, `mp_player_state_of`,
  `mp_hurry_remaining_ms`, `mp_advance_hurry_for_test`, `drop_client`, `run_until` совпадают
  с текущим кодом (`src/mp/mp_turn.{h,cpp}`, `tests/mp_session_harness.h`).
- **Review Focus:** шесть пунктов, у каждого есть шаг (обрыв без снятия — Task 2 Step 4;
  оба сняты и `away` — существующие `an_idle_session_does_not_spin_the_baton` и гейт;
  бой и мёртвое место — `mp_turn_paced` уже их различает, правка их не трогает; возврат темпа
  — Task 3 Step 1).
- **Proportion:** план описывает решения и проверки, не переписывает код.
