# MP follow-ups: spin, full-suite order, diagnostics — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (выбор
> пользователя по прошлым веткам: нативно + один финальный ревьюер). Шаги отмечаются
> чекбоксами (`- [ ]`).

**Goal:** закрыть отложенные пункты после фиксов live-view и watchdog: измерить и убрать
лишние передачи батона при отключённом/простаивающем слоте, найти причину order-dependent
SIGSEGV полного прогона, и закрыть мелкие диагностические/тестовые долги.

**Architecture:** одна ветка `fix/mp-followups`, задачи по возрастанию риска: сначала
детерминированные мелкие фиксы (F3-F9), затем две задачи «сначала измерение/вердикт, потом
фикс» (F1 — спин батона, F2 — порядок тестов). Ни одна задача не меняет пороги и семантику
watchdog-а, согласованные в `fix/mp-watchdog`; F2 не «чинится» перестановкой шардов.

**Tech Stack:** C++17, Catch2 2.13.10, `std::thread`, `mp_sched`, `mp_session_harness`,
серверная сборка `MP_SERVER=1`.

**Spec:** `docs/superpowers/specs/2026-11-19-mp-followups-design.md` (F1-F9, acceptance).

## Global Constraints

- **Ветка:** `fix/mp-followups` от `mp` (`3411708395`). В `mp` — только после ручного
  вердикта пользователя.
- **Артефакты:** всё временное — в `.data/`; ничего в `/tmp` и ничего не коммитить из `.data/`.
- **Сборки:** сервер `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`;
  не-серверный `... TILES=0 TESTS=1 MULTIPLAYER=1 BUILD_PREFIX=mpclient-`; обычный
  `... TILES=0 TESTS=1`. `RELEASE=0`, ассерты живые.
- **Заголовки:** после любой правки `.h` — `make clean` перед первой сборкой (в этом дереве
  `.d` не перечисляют заголовки; иначе смешение объектов и SIGSEGV в `faction_manager`).
- **Тесты:** серверный гейт `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test
  -o .data/<run> --seed 1 -j $(nproc) "[mp]"`; не-серверный — `tests/mpclient-cata_test
  "[mp],[coop]" --rng-seed 1 --user-dir=.data/<run>/`; полный — `run_tests_parallel.py
  -o .data/<run> --seed 1`. «Зелёный» — дифф к baseline, а не абсолютный ноль.
- **Watchdog не ослаблять:** порог, «один отчёт на stall», подавление объявленного ожидания —
  не трогать. F9 только распространяет инвариант «флаг покрывает ожидание» на join-фазу.
- **F2 без квартирования:** перестановка тестов по шардам запрещена как фикс; допустимо
  восстановление общего состояния тестом.
- **Стиль:** 100 колонок, комментарии по-английски; `astyle` недоступен — глазами.
- **Коммиты:** `fix(mp):`/`test(mp):`/`docs(mp):`, без `Co-Authored-By`.

## Review Focus

1. **F1: вердикт по измерению, а не по гипотезе.** Отчёт должен назвать, кто получает
   батон (отключённый слот или простаивающий игрок), и почему частота такая; «норма» —
   допустимый исход, но тогда закреплена тестом.
2. **F2: причина, а не перестановка.** Бисекция обязана назвать тест/состояние, портящее
   overmap; «пересчитали шарды» — не фикс.
3. **F3: порядок зеркала не ломает ожидание.** Перестройка `yield_to` (unlock → log → relock)
   не должна терять пробуждение и не должна писать под `mutex_`.
4. **F8: владение `overmap_data.ui`.** Если адаптор общий, он должен умереть под своим
   стеком; если инвариант «один овермап», он зафиксирован тестом.
5. **Тесты не флакают под `-j $(nproc)`:** F5/F6 проверяются повторными прогонами под
   нагрузкой, а не одним зелёным.

---

### Task 1: Гигиена watchdog-а (F3, F4, F9)

**Files:**
- Modify: `src/mp/mp_sched.cpp` (`yield_to`), `src/mp/mp_sched.h` (комментарий, если нужно)
- Modify: `src/do_turn.cpp` (мёртвый блок), `src/mp/mp_server_main.cpp` (join-фаза)
- Test: существующие `tests/mp_sched_test.cpp`, `tests/mp_session_test.cpp` (без новых тестов;
  поведение не меняется)

**Interfaces:**
- Consumes: `mp_sched::yield_to`, `record_trace_locked`, `log_trace_entry`,
  `set_world_waiting`, `wait_for_request`.
- Produces: без новых символов; семантика `yield_to` и `world_waiting` без изменений.

- [ ] **Step 1: F3 — зеркало `yield_to` на передаче.**

В `yield_to` после `record_trace_locked` и `notify_all` отпустить `lock`, вызвать
`log_trace_entry( entry )`, затем снова `lock.lock()` и войти в `cv_.wait` с прежним
предикатом (`holder_ == from || stopping_`). `parked_` остаётся увеличенным до ожидания, так
что `shut_down()` не проскочит. Комментарий: зеркало должно назвать слот, которому отдали
батон, даже если он не вернётся.

- [ ] **Step 2: F4 — удалить мёртвый блок.**

В `do_turn` убрать `if( moved ) { sched->set_world_waiting( false ); }` вместе с комментарием;
флаг теперь ставится и снимается вокруг `wait_for_request`. Если хочется страховки — вместо
блока поставить `cata_assert( !sched->world_waiting() )` перед `poll-end` (в debug-сборке).

- [ ] **Step 3: F9 — join-фаза.**

В `mp_server_main.cpp` в ветке `anyone_choosing`: `set_world_waiting( true )` перед
`wait_for_request( MP_POLL_MS, choosing )`, `set_world_waiting( false )` сразу после; ветку
`if( moved || released )` с `false` удалить как ставшую мёртвой.

- [ ] **Step 4: Прогоны.**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/fu-t1/`
Expected: PASS, включая оба теста watchdog-а.

- [ ] **Step 5: Проверка порядка зеркала вручную (короткая сессия).**

Run: `MP_SCHED_TRACE=1 ./mpserver-cataclysm-server --server 4321 --world coop-manual --players 2 --userdir .data/mp-srv`
+ два клиента, «продолжить», 10–15 с, закрыть оба.
Expected: в `config/debug.log` строка `yield_to` для поколения N встречается раньше
`release_to` поколения N+1 (не наоборот); если слот не вернул батон (обрыв), его `yield_to`
всё равно есть.

- [ ] **Step 6: Коммит.**

```bash
git add src/mp/mp_sched.cpp src/mp/mp_sched.h src/do_turn.cpp src/mp/mp_server_main.cpp
git commit -m "fix(mp): tighten the baton trace and the declared waits"
```

---

### Task 2: Надёжность тестов и покрытие деструктора (F5, F6)

**Files:**
- Modify: `tests/mp_sched_test.cpp` (пороги/комментарии)
- Modify: `tests/mp_context_test.cpp` (новый тест)

**Interfaces:**
- Consumes: `mp_sched_start_watchdog`, `watchdog_report_count`, `player_context::~player_context`,
  `ui_manager::get_active_stack`.
- Produces: без production-кода.

- [ ] **Step 1: F5 — расширить окно/порог.**

В `rapid_handoffs_do_not_report_a_stall`: порог `300` мс вместо `10`, цикл `1000` мс вместо
`300`; в комментарии — почему (два пропущенных 100-мс опроса не должны быть возможны на
быстрой машине, но запас нужен под `-j $(nproc)`). В
`the_watchdog_leaves_quietly_when_the_session_stops` добавить строку: порог 1000 мс обязан
быть больше периода watchdog-а 100 мс, иначе тело после `shut_down()` успеет отчитаться.

- [ ] **Step 2: F6 — тест на ветку «свой стек активен».**

```cpp
TEST_CASE( "a_context_destroyed_under_its_own_stack_restores_the_world_stack", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t *dead = nullptr;
    {
        player_context ctx;
        dead = &ctx.ui_stack;
        ui_manager::set_active_stack( &ctx.ui_stack );
        ctx.liveview = std::make_unique<live_view>();
        ctx.liveview->show( tripoint::zero );
        REQUIRE( ctx.ui_stack.size() == 1 );
        // ~player_context runs with its own stack active.
    }
    CHECK( ui_manager::get_active_stack() != nullptr );
    CHECK( ui_manager::get_active_stack() != dead );
}
```

`dead` указывает на уже уничтоженный вектор; сравнивать его значение можно (не разыменовывая)
— тест ловит именно «вернулись на мёртвый стек». Если появится
`ui_manager::get_world_stack()`, проверять его напрямую вместо `!= dead`.

- [ ] **Step 3: Прогоны под нагрузкой.**

Run: `make ... BUILD_PREFIX=mpserver-` и `for i in $(seq 10); do tests/mpserver-cata_test "rapid_handoffs_do_not_report_a_stall,the_watchdog_leaves_quietly_when_the_session_stops,a_context_destroyed_under_its_own_stack_restores_the_world_stack" --rng-seed $i --user-dir=.data/fu-t2-$i/; done`
Expected: 10/10 зелёные.

- [ ] **Step 4: Коммит.**

```bash
git add tests/mp_sched_test.cpp tests/mp_context_test.cpp
git commit -m "test(mp): give the watchdog tests slack and cover the context fallback"
```

---

### Task 3: Устаревшая подсказка mouse view при реконнекте (F7)

**Files:**
- Modify: `src/mp/mp_server_main.cpp` (приём/замена клиента на место), `src/mp/mp_context.h/.cpp`
  (если нужен маленький аксессор для сброса)
- Test: `tests/mp_session_test.cpp`

**Interfaces:**
- Consumes: `player_context::liveview`, `mp_activate_player`, `mp_session_harness::reconnect_client`.
- Produces: подсказка места погашена к моменту, когда новый клиент получает первый экран.

- [ ] **Step 1: RED-тест.**

`mouse_move(1, …)` → `run_until` подсказка на экране слота 1 → `drop_client(1)` →
`reconnect_client(1, save_id)` (save_id взять у `g->player_at(1).get_save_id()` до обрыва) →
`run_until` клиент снова на карте → `CHECK_FALSE( mp_grid_contains( screen(1), "Mouse view" ) )`.
До фикса ожидается FAIL (подсказка в кадре после реконнекта) или отсутствие строки, если
кадр не перерисован — тогда тест должен ждать кадр с картой, а не первый же.

- [ ] **Step 2: Фикс.**

Там, где место передаётся новому клиенту (`mp_server_reconnected()` / установка клиента на
место), активировать это место (`mp_activate_player(slot)`, как уже делается для
`drop_main_ui`), погасить `contexts[slot].liveview.reset()` под его стеком и вернуть прежний
контекст. Не трогать чужие подсказки.

- [ ] **Step 3: Прогон и коммит.**

Run: `tests/mpserver-cata_test "the_stale_hint..." --rng-seed 1 --user-dir=.data/fu-t3/` → PASS;
затем `[mp]`-гейт.
```bash
git add src/mp/mp_server_main.cpp tests/mp_session_test.cpp
git commit -m "fix(mp): drop a seat's mouse-view hint when the seat is taken over"
```

---

### Task 4: `overmap_data.ui` — аудит владения (F8)

**Files:**
- Test: `tests/mp_session_test.cpp`
- Modify (если подтвердится): `src/overmap_ui.h/.cpp`, `src/mp/mp_context.h/.cpp`,
  `src/game.h` (перенос поля/аксессор)

**Interfaces:**
- Consumes: `overmap_ui::overmap_draw_data_t::ui`, `player_context`, `ui_adaptor` owner assert.
- Produces (если нужен фикс): владелец адаптора — `player_context`, доступ через аксессор по
  образцу `mp_active_live_view()`.

- [ ] **Step 1: RED-тест: два овермапа.**

Оба клиента жмут `m` (овермап), `run_until` на обоих экранах есть маркер овермапа; один
закрывает (`Esc`), второй двигает курсор/жмёт клавиши; затем наоборот. Ожидаемый RED:
`Assertion 'owner == active_stack' failed` или падение при teardown с открытым овермапом.
Если оба сценария зелёные — зафиксировать инвариант тестом и записать в отчёт, что фикс не
нужен (это валидный исход аудита).

- [ ] **Step 2: Решение по владению.**

Если адаптор общий: перенести `shared_ptr<ui_adaptor>` в `player_context` (как `liveview`),
в `overmap_ui.cpp` заменить `g->overmap_data.ui` на аксессор активного контекста; teardown —
в `~player_context` и `mp_sync_context_count()` рядом с `liveview`. Если овермап по инварианту
один — оставить код и закрепить тестом.

- [ ] **Step 3: Гейт и коммит.**

Run: `[mp]` + ручной сценарий из теста (два клиента, `m`, закрытие одного).
```bash
git commit -m "fix(mp): keep the overmap ui adaptor on its owner's stack"
```

---

### Task 5: Спин батона при отключённом/простаивающем слоте (F1)

**Files:**
- Test: `tests/mp_session_test.cpp` (измерение + регрессия)
- Modify (по вердикту): `src/do_turn.cpp`, `src/mp/mp_turn.cpp`, `src/mp/mp_sched.cpp`

**Interfaces:**
- Consumes: `mp_sched::holder_generation()`, `mp_session_harness::drop_client/run_until`,
  `mp_player_left_behind`, `mp_player_is_away_at_input_wait`.
- Produces: вердикт в отчёте/леджере; при спине — ограничение частоты передач и тест.

- [ ] **Step 1: Измерение.**

Тест-измеритель (временно, до выбора порога):

```cpp
TEST_CASE( "measure_idle_handoffs", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.drop_client( 1 );
    harness.run_until( []() { return false; }, 5 );   // дать сессии устояться
    mp_sched *const sched = mp_active_sched();
    REQUIRE( sched != nullptr );
    const size_t g0 = sched->holder_generation();
    harness.run_until( []() { return false; }, 20 );
    const size_t delta = sched->holder_generation() - g0;
    WARN( "idle handoffs over 20 turns: " << delta );
}
```

Записать delta, число тиков старого watchdog-правила и кто был держателем (по фазе трассы
нельзя — ring недоступен извне; смотреть `debug.log` с `MP_SCHED_TRACE=1` в ручном прогоне).
Run: `tests/mpserver-cata_test "measure_idle_handoffs" --rng-seed 1 --user-dir=.data/fu-t5/`.

- [ ] **Step 2: Вердикт.**

- **Спиннинг** (отключённый слот или холостой цикл получает батон чаще, чем раз в таймаут
  ввода): назвать место (`do_turn` owes_turn / `mp_player_left_behind` / реacquire в
  `mp_server_input`/`mp_curses_server`) и чинить.
- **Норма** (передачи — это ввод/таймауты): записать в леджер и заменить измеритель на
  страж с верхней границей.

- [ ] **Step 3: Фикс/страж и RED.**

Для спиннинга: тест `an_idle_session_does_not_spin_the_baton` с порогом из измерения;
до фикса — FAIL, после — PASS. Для нормы: тот же тест с верхней границей, RED не требуется
(поведение не меняется), но прогон 10× под нагрузкой.

- [ ] **Step 4: Гейт и коммит.**

Run: `[mp]`; ручной сценарий «Bo закрыт, Ada бездействует 60 с» из отчёта:
`grep -c "mp sched trace:"` и скорость роста поколения; CPU сервера — `top`/`pidstat`.
```bash
git commit -m "fix(mp): stop handing the baton to an idle seat in a loop"
```

---

### Task 6: Order-dependent падения полного прогона (F2)

**Files:**
- Test/repro: `.data/` (фильтр шарда, логи)
- Modify (по причине): `tests/game_test_helper.h`, `tests/mp_save_split_test.cpp`, `src/overmap*`
  или `src/savegame.cpp`
- Отчёт: `docs/superpowers/reports/2026-11-19-mp-suite-order.md`

**Interfaces:**
- Consumes: точный фильтр shard-04 из `.data/par-wd-full/shard-04.log`, `game::save`,
  `overmap::serialize`.
- Produces: причина (тест или production) и фикс/изоляция; отчёт с командой воспроизведения.

- [ ] **Step 1: Воспроизвести.**

Взять `Filters:` из `.data/par-wd-full/shard-04.log` (последний прогон) в файл
`.data/shard04-spec.txt`, затем:
`tests/cata_test "$(cat .data/shard04-spec.txt)" --rng-seed 1 --user-dir=.data/fu-t6/`
Expected: exit 139, `mp_save_split_test.cpp:375`, backtrace `overmap::serialize` (как записано
в леджере watchdog-плана).

- [ ] **Step 2: Бисекция.**

Делить список тестов пополам, сохраняя пару «anchor + save-split»; каждый прогон ≤2 мин.
Остановиться на минимальном наборе и назвать тест, после которого `game::save()` падает.
Проверить гипотезы: остатки `overmap_buffer` (несуществующие/перемещённые овермапы),
`g->world_seed`/`overmap` кэш, `calendar`/`weather` — что именно портится.

- [ ] **Step 3: Фикс или изоляция.**

Если портит тест — восстановить состояние в его cleanup (`overmap_buffer.clear()`,
`g->load_map`/`clear_map`, сброс `overmap_data`). Если production-баг — чинить там. Записать
в отчёт. Квартирование (перестановка шардов) запрещено.

- [ ] **Step 4: Гейт.**

Run: `tests/cata_test "$(cat .data/shard04-spec.txt)" --rng-seed 1 --user-dir=.data/fu-t6-green/`
Expected: 0 failed.
Run: `python3 tools/run_tests_parallel.py -o .data/fu-full --seed 1 -j $(nproc)`
Expected: нет падений сверх записанного baseline (baseline снять до изменений этой задачи на
текущем плане шардов).

- [ ] **Step 5: Коммит.**

```bash
git add tests/... docs/superpowers/reports/2026-11-19-mp-suite-order.md
git commit -m "test(mp): restore the world between the order-dependent tests"
```

---

### Task 7: Ворота и передача

**Files:** без production-кода (леджер, отчёт).

- [ ] **Step 1: Все три сборки + `[mp]`-гейты.**

Run: серверный `[mp]` (runner), `mpclient-` `[mp],[coop]`, plain `[mp],[coop]`.
Expected: 0 failed в каждом; числа записать в леджер.

- [ ] **Step 2: Полный прогон.**

Run: `python3 tools/run_tests_parallel.py -o .data/fu-final --seed 1 -j $(nproc)`
Expected: дифф к baseline; новые падения — стоп.

- [ ] **Step 3: Ручной сценарий и передача.**

Дать пользователю команды: два клиента, Bo закрыт, Ada бездействует 60 с (F1); мышь → обрыв
→ реконнект (F7); два овермапа (F8). В `mp` не вливать до его вердикта.

---

## Self-review плана

- Spec coverage: F1→Task 5, F2→Task 6, F3/F4/F9→Task 1, F5/F6→Task 2, F7→Task 3, F8→Task 4,
  гейты/ручной→Task 7. F9 (join-фаза) — в Task 1 Step 3.
- Step scan: каждая задача заканчивается проверяемым результатом; для F1/F2 дан порядок
  «измерение → вердикт → фикс», тела не выписаны, потому что зависят от измерения.
- Type consistency: имена `holder_generation()`, `trace_snapshot()`,
  `reconnect_client(slot, save_id)` совпадают с текущим кодом/тестами.
- Review Focus: пять пунктов, у каждого есть задача/проверка.
- Proportion: план описывает решения и проверки, не переписывает код.
