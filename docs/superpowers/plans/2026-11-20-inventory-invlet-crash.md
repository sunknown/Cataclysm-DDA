# Inventory invlet crash при удалённом вводе — план диагностики и фикса

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** сервер не падает в `inventory_entry::get_invlet()` при удалённом вводе в инвентаре,
и причина (а не место падения) устранена в коде с регрессионным тестом.

**Architecture:** сначала воспроизведение и вердикт — какой именно `item_location` протух и
что его инвалидировало (смена активного контекста между ходами, тик мира при открытом меню,
или переиспользование записи между открытиями); только потом фикс в источнике
инвалидации и тест на последовательность клавиш. Никаких «просто проверок на null» в
`get_invlet()` до вердикта: это скроет симптом и оставит протухший `item_location` в списке.

**Tech Stack:** C++17, Catch2 2.13.10, `mp_session_harness`, серверная сборка `MP_SERVER=1`,
gdb, tmux для ручного воспроизведения.

**Spec:** отдельной спеки под этот дефект нет; поведенческий контекст —
`docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md` §8.3–8.5 (меню не тратят ход,
ввод идёт через сервер) и ADR 0001–0003. Если при диагностике выяснится, что нарушен
инвариант «у удалённого игрока в меню тот же `avatar`/инвентарь, что и в его ходе», это
считается дефектом спеки, а не плана — записать в отчёт и вынести на решение человеку.

## Доказательства (на момент написания плана)

- Два падения сервера на `mp` `df427a66a8`, бинарник пересобран 16:23, мир `coop-e2e`,
  два клиента в игре, ключи отправлены через `tmux send-keys` в окно Bo.
- Символизированный стек (оба раза идентичен):

```
inventory_entry::get_invlet()               src/inventory_ui.cpp:636   any_item()->invlet
  <- inventory_column::find_by_invlet       src/inventory_ui.cpp:702
  <- inventory_selector::find_entry_by_invlet  :2428
  <- inventory_selector::process_input      :3528
  <- inventory_selector::get_input          :3499
  <- inventory_pick_selector::execute       :3887
  <- game_menus::inv::common                src/game_inventory.cpp:295
  <- game::do_regular_action                src/handle_action.cpp:2735
  <- game::handle_action                    :3507
  <- game::player_act                       src/do_turn.cpp:663
  <- mp_run_player_thread                   src/mp/mp_server_main.cpp:1152
```

- `inventory_entry::is_item()` — это `!locations.empty()` (`src/inventory_ui.h:133`), а
  `any_item()` возвращает `locations.front()` и разыменовывается в `get_invlet()`
  (`:164`, `:636`). То есть падение возможно только при непустом списке локаций, чей
  предмет уже мёртв/невалиден.
- Последовательности (все клавиши — в окно Bo, паузы 3–6 с):
  - упало: `o` (Continue), `i`, `a`, `Escape`, `i`, `a`, `Escape` и далее ещё один `Escape`;
    в другом прогоне — `i`, `a`, `Escape`, `i`, затем клавиша;
  - НЕ упало на свежей сессии: `i`; `i`→`a`; `i`→`Escape`→`i`; `i`→`a`→`Escape`.
  Триггер накопительный, а не одно нажатие.
- Логи: `.data/e2e-save3/srv-p5.log`, `srv-repro.log`, `srv-repro3.log`
  (`STACK TRACE` + `crash.log` рядом с `config/`).

## Global Constraints

- **Ветка:** `fix/mp-inventory-invlet-crash` от `mp` (`df427a66a8`). В `mp` — только после
  вердикта пользователя.
- **Артефакты:** всё временное — в `.data/`; ничего не коммитить из `.data/`.
- **Сборки:** сервер `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1
  BUILD_PREFIX=mpserver-`; не-серверный `... TILES=0 TESTS=1 MULTIPLAYER=1
  BUILD_PREFIX=mpclient-`; обычный `... TILES=0 TESTS=1`. `RELEASE=0`, ассерты живые.
- **Заголовки:** после правки `.h` — `make clean` перед первой сборкой (`.d` не перечисляют
  заголовки).
- **Тесты:** серверный гейт `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test
  -o .data/<run> --seed 1 -j $(nproc) "[mp]"`; не-серверный `tests/mpclient-cata_test
  "[mp],[coop]" --rng-seed 1 --user-dir=.data/<run>/`; полный `run_tests_parallel.py
  -o .data/<run> --seed 1`. «Зелёный» — дифф к baseline.
- **Ф4/Ф5 не ломать:** меню не тратят ход, барьер и таймауты не меняются.
- **Стиль:** 100 колонок, комментарии по-английски; `astyle` локально недоступен — глазами.
- **Коммиты:** `fix(mp):`/`test(mp):`/`docs(mp):`, без `Co-Authored-By`.
- **Нельзя:** чинить перестановкой шардов, `null`-проверкой только в `get_invlet()` до
  вердикта, отключением меню, «повторить и забыть».

## Review Focus

1. **MP-специфичность.** Воспроизводится ли то же на одном месте (`mp_session_harness(1)`) и
   на двух? Если только на двух — инвалидация идёт через смену активного контекста, и это
   главный подозреваемый.
2. **Время жизни `item_location`.** Меню может висеть, пока мир делает ход за другого
   игрока: какие локации в записи (инвентарь персонажа, карта, vehicle) и что их убивает.
3. **Причина, а не место.** Фикс должен убирать протухание (или не давать записи его
   получить), а не глотать `null` в одном геттере.
4. **Воспроизводимость теста.** Тест обязан падать до фикса и проходить после на
   последовательности клавиш, а не «иногда».
5. **Соседние меню.** Тот же класс записи используется в других селекторах (`wear`, `drop`,
   `use`); фикс не должен чинить только `i`.

---

### Task 1: Детерминированное воспроизведение и захват состояния

**Files:**
- Test: `tests/mp_session_test.cpp` (новый кейс) или `tests/mp_inventory_test.cpp` (если
  удобнее отдельным файлом — тесты собираются `wildcard *.cpp`)
- Modify (диагностика, не коммитить): `src/inventory_ui.cpp` — временные логи
- Отчёт: `.data/inv-crash/` (логи, gdb-дамп)

**Interfaces:**
- Consumes: `mp_session_harness` (`press`, `run_until`, `screen`), `mp_menu_map`-подобные
  хелперы в `tests/mp_session_test.cpp`.
- Produces: воспроизводящий кейс с фиксированными клавишами и стабильным падением.

- [ ] **Step 1: Тест-репро на харнессе.**

Написать кейс `inventory_reopen_cycles_do_not_crash` по образцу
`two_players_can_hold_the_overmap_open_at_once`: два места, оба в игре, затем для места 0
последовательность `i`, `a`, `Escape`, `i`, `a`, `Escape`, `i`, `Escape` через `press` с
`run_until`, которая дожидается смены экрана (маркер инвентаря — строка `Weapon held` или
`Items worn` из `inventory_ui`). Ожидаемо: падение процесса (SIGSEGV) или зелёный прогон —
зафиксировать факт.

- [ ] **Step 2: Прогнать и записать.**

Run: `tests/mpserver-cata_test "inventory_reopen_cycles_do_not_crash" --rng-seed 1 --user-dir=.data/inv-crash/`
Expected: либо SIGSEGV в `get_invlet`, либо зелёный. Если зелёный — расширять число циклов и
добавить ходы мира между открытиями (`run_until` с движением другого места), пока падение не
станет устойчивым; записать, что именно добавило воспроизводимость.

- [ ] **Step 3: Захват состояния в gdb.**

Если харнесс не воспроизводит, воспроизвести на реальном сервере под gdb:

```bash
gdb -batch -ex run -ex 'bt' -ex 'frame 5' -ex 'info locals' \
    --args ./mpserver-cataclysm-server --server 4403 --world coop-e2e --players 2 \
    --userdir .data/e2e-save3/srv
```

и повторить клавиши из tmux. Нужно получить: адрес `this` для `inventory_entry`, содержимое
`locations` (пусто/нет), `locations.front()`'s `where()` и указатель на предмет, а также
`is_item()`/`is_selectable()`.

- [ ] **Step 4: Записать вывод.**

В отчёт: команда, полный стек, дамп записи, экран в момент падения, последовательность клавиш,
двух- или одно-седельный случай. Без этого Task 2 не начинать.

- [ ] **Step 5: Коммит теста.**

```bash
git add tests/mp_session_test.cpp
git commit -m "test(mp): pin the inventory reopen crash"
```

---

### Task 2: Вердикт — какой `item_location` протух и почему

**Files:** без production-кода; отчёт `.data/inv-crash/verdict.md` (в git — только итог в
`docs/superpowers/reports/2026-11-20-inventory-invlet-crash.md`).

**Interfaces:**
- Consumes: дамп из Task 1.
- Produces: имя колонки/записи, тип локации, событие инвалидации, и проверяемая гипотеза.

- [ ] **Step 1: Определить запись.**

По дампу назвать: колонка (`inventory_column`), категория, предмет, `item_location::where()`
(character/map/vehicle), и жив ли `Character*` (слот 0/1, `g->u` vs `player_at`).

- [ ] **Step 2: Проверить гипотезы по одной.**

Кандидаты, каждый проверяется отдельным минимальным экспериментом и записывается в отчёт:
1. смена активного контекста между ходами при открытом меню инвалидирует `Character*`-локации;
2. ход мира (другого места) уничтожает предмет карты, попавший в список;
3. запись переиспользуется между открытиями селектора (статические/кэш-структуры);
4. баг не MP: воспроизводится на `mp_session_harness(1)`.

- [ ] **Step 3: Записать вердикт.**

Отчёт `docs/superpowers/reports/2026-11-20-inventory-invlet-crash.md`: стек, дамп, какая
гипотеза подтвердилась, что именно инвалидирует локацию, почему это видно только на удалённом
вводе/повторных открытиях. Если вердикт «вне MP» — так и написать.

- [ ] **Step 4: Коммит отчёта.**

```bash
git add docs/superpowers/reports/2026-11-20-inventory-invlet-crash.md
git commit -m "docs(mp): inventory invlet crash verdict"
```

---

### Task 3: Фикс в источнике и регрессия

**Files:**
- Modify: назовёт вердикт (ожидаемо `src/inventory_ui.cpp` / `src/game_inventory.cpp` /
  `src/mp/mp_context.cpp`)
- Test: кейс из Task 1

**Interfaces:**
- Consumes: вердикт Task 2.
- Produces: устранённое протухание; тест зелёный; семантика меню не меняется.

- [ ] **Step 1: RED.**

Убедиться, что кейс Task 1 падает на текущем коде ровно тем стеком; зафиксировать вывод.

- [ ] **Step 2: Минимальный фикс в источнике.**

Правка ровно там, где локация становится невалидной (обновление записей при смене
контекста/тике мира/переоткрытии). Никаких `null`-проверок в геттерах, если вердикт не
говорит, что «запись с мёртвой локацией легальна и должна отбрасываться»; в этом случае
отбрасывание делается в месте построения списка, с комментарием и тестом.

- [ ] **Step 3: GREEN.**

Run: `tests/mpserver-cata_test "inventory_reopen_cycles_do_not_crash" --rng-seed 1 --user-dir=.data/inv-green/`
Expected: PASS; повторить 10× под `--rng-seed 1..10`.

- [ ] **Step 4: Гейты.**

Run: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/inv-mp/`
Run: `tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/inv-cl/`
Expected: 0 failed в обоих.

- [ ] **Step 5: Коммит.**

```bash
git add <файлы из вердикта> tests/mp_session_test.cpp
git commit -m "fix(mp): ..."
```

---

### Task 4: Проверка на живом сервере и возврат к фазе 5

**Files:** без кода.

- [ ] **Step 1: Ручная последовательность.**

Поднять сервер на `coop-e2e` (порт 4403, `.data/e2e-save3`), оба места через `Continue as`,
повторить упавшую последовательность 3 раза по 4 цикла `i`/`a`/`Escape`; сервер жив.

- [ ] **Step 2: Сценарии фазы 5.**

На той же сессии: обрыв одного клиента (сессия продолжается), затем смерть персонажа и
меню замены, затем вход третьего клиента в освободившееся место. Записать результат каждого
в отчёт; при отказе — стоп и отдельный дефект.

- [ ] **Step 3: Полный прогон.**

Run: `python3 tools/run_tests_parallel.py -o .data/inv-final --seed 1 -j $(nproc)`
Expected: нет падений сверх baseline.

- [ ] **Step 4: Передача.**

Дать пользователю вердикт: сливать в `mp` или нет; обновить статус фазы 5 в
`docs/superpowers/specs/2026-11-18-mp-phase5-lifecycle-design.md` только после его прогона.

---

## Self-review плана

- **Coverage:** воспроизведение → вердикт → фикс → гейты → ручная проверка и возврат к
  фазе 5; каждая часть имеет отдельный task и проверяемый результат.
- **Step scan:** ни один шаг не решает «что-то поправить»; фикс отложен до вердикта, потому
  что тело зависит от него. Проверки — команды и наблюдаемый результат.
- **Type consistency:** имена `inventory_entry::get_invlet`, `find_by_invlet`,
  `process_input`, `inventory_pick_selector::execute`, `mp_session_harness::press/run_until`
  совпадают с текущим кодом.
- **Review Focus:** пять пунктов, у каждого есть шаг (MP-специфичность — Task 2 Step 2.4,
  время жизни — Task 2 Step 1–2, причина — Task 3 Step 2, воспроизводимость — Task 3 Step 3,
  соседние меню — Task 2 Step 1 плюс проверка в Task 3 Step 2).
- **Proportion:** план описывает решения и проверки, не переписывает код.
