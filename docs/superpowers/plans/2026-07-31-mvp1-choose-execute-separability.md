# MVP-1: разведка разрезаемости choose/execute — план

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Установить, выполним ли центральный инвариант дизайна — «`apply_intent` не открывает UI» — для десяти самых трудных действий игры, и получить решение по воротам до вложений в рефактор.

**Architecture:** Это разведка, а не реализация. Ни одной строки в `src/` не меняется. Для каждого из десяти действий выполняется одна и та же процедура: найти путь исполнения, механически перечислить все обращения к UI внутри него, классифицировать каждое как «выбор до исполнения» или «диалог посреди исполнения», вынести вердикт и набросать структуру намерения. Результат — один отчёт, который либо подтверждает §5.3 спека, либо отменяет его.

**Tech Stack:** grep, чтение кода, Markdown.

## Global Constraints

- Ни одного изменения в `src/`, `tests/` или файлах сборки. Единственный продукт — отчёт.
- Спек: `docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md`, разделы 5.3 и 9.1.
- Инвариант под проверкой, дословно из §5.3: «`apply_intent` не имеет права открыть ни одно окно и ни о чём спросить игрока. Всё, что требует решения игрока, принимается в фазе выбора и полностью записывается в `mp_intent`.»
- Отчёт пишется в `docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md`, по одному разделу на действие, плюс сводка и решение по воротам.
- Вердикт по каждому действию — ровно одно из трёх: **режется**, **режется с оговоркой**, **не режется**. Промежуточных формулировок нет; расплывчатый вердикт делает ворота бессмысленными.

---

## Определения, общие для всех задач

### Набор UI-функций

Обращением к UI считается вызов любой из следующих функций. Этот список используется **дословно** во всех десяти задачах, иначе результаты несопоставимы:

```
query_yn
query_int
uilist
popup
popup_getkey
string_input_popup
choose_adjacent
choose_adjacent_highlight
look_around
game_menus::inv
target_handler::mode_
ui_manager::redraw
cancel_activity_query
cancel_activity_or_ignore_query
```

Команда сбора для файла:

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" <файл>
```

### Классификация каждого обращения

- **P (pre)** — обращение происходит до того, как потрачены moves и изменён мир. Переносится в фазу выбора без последствий.
- **M (mid)** — обращение происходит после начала мутации мира, и его результат влияет на дальнейшее исполнение. Это прямое нарушение инварианта.
- **A (activity)** — обращение живёт внутри `activity_actor::do_turn` или `finish`, то есть в многоходовой активности, а не в исполнении одного намерения. Требует отдельного разбора: активности исполняются на всех пирах и не могут спрашивать локального игрока.
- **D (dead)** — обращение недостижимо в режиме тестов или обёрнуто в `if( !test_mode )`, то есть уже сейчас необязательно.

### Правило вердикта

- **режется** — все обращения P или D.
- **режется с оговоркой** — есть A, но ни одного M, и для каждого A назван конкретный способ убрать вопрос из активности (предрешить в намерении, заменить на детерминированное правило, вынести в отдельное намерение).
- **не режется** — есть хотя бы одно M, для которого не найден способ переноса в фазу выбора.

### Шаблон раздела отчёта

Каждая из задач 1-10 дописывает в отчёт раздел строго такого вида:

```markdown
## <Номер>. <Название действия>

**Точка входа:** `<файл:строка>`
**Путь исполнения:** <перечисление файлов и функций по порядку вызова>

| Обращение | Файл:строка | Класс | Обоснование |
|---|---|---|---|
| query_yn | src/foo.cpp:123 | M | Спрашивает подтверждение после того, как предмет уже изъят из инвентаря |

**Вердикт:** режется / режется с оговоркой / не режется

**Набросок намерения:**

    struct intent_<действие> {
        // поля с типами
    };

**Что мешает и как обойти:** <текст, обязателен для вердиктов кроме «режется»>
```

---

## Task 0: Создать каркас отчёта

**Files:**
- Create: `docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md`

- [ ] **Step 1: Создать файл с шапкой и пустой сводкой**

```markdown
# MVP-1: разрезаемость choose/execute — результаты разведки

Дата: 2026-07-31
Спек: docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md, §5.3 и §9.1

Проверяемый инвариант: `apply_intent` не имеет права открыть ни одно окно и ни о чём
спросить игрока. Всё, что требует решения игрока, принимается в фазе выбора и
полностью записывается в `mp_intent`.

Классы обращений к UI: P — до мутации мира, M — посреди исполнения, A — внутри
многоходовой активности, D — недостижимо в test_mode.

## Сводка

| # | Действие | Вердикт | M-обращений | A-обращений |
|---|---|---|---|---|

## Решение по воротам

_Заполняется в Task 11._
```

- [ ] **Step 2: Коммит**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: scaffold MVP-1 separability report"
```

---

## Task 1: Стрельба

Самое подозрительное действие целиком: прицеливание — это интерактивный цикл, который тратит moves по ходу.

**Точка входа:** `src/handle_action.cpp:2821` (`case ACTION_FIRE`), далее `avatar_action::fire_wielded_weapon` (`src/avatar_action.cpp:816`) и `avatar_action::can_fire_weapon` (`src/avatar_action.cpp:787`).

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2821,2878p' src/handle_action.cpp
sed -n '787,900p' src/avatar_action.cpp
grep -rn "class target_handler\|target_ui\|::mode_fire\|::mode_throw" src/ranged.h src/ranged.cpp | head -20
```

Записать в отчёт цепочку файлов и функций по порядку вызова.

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/avatar_action.cpp src/ranged.cpp
```

Отфильтровать до тех, что лежат на пути исполнения стрельбы.

- [ ] **Step 3: Классифицировать каждое обращение**

Ключевой вопрос по каждому: тратятся ли moves и мутирует ли мир **до** этого обращения. Если да — класс M.

Особое внимание: тратит ли прицеливание moves внутри интерактивного цикла. Если да, стрельба не может быть одним намерением и должна распасться на «прицелиться в точку» и «выстрелить», где вторая часть детерминирована.

- [ ] **Step 4: Записать раздел отчёта по шаблону**

Набросок намерения должен содержать конкретные типы, например:

```cpp
struct intent_fire {
    item_location weapon;
    tripoint_abs_ms target;
    int mode_index;      // индекс режима огня, а не указатель на режим
    int aim_moves_spent; // сколько moves уйдёт на прицеливание, решено заранее
};
```

- [ ] **Step 5: Коммит**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for ranged fire"
```

---

## Task 2: Крафт

**Точка входа:** `src/handle_action.cpp:2915` (`case ACTION_CRAFT`), далее `Character::craft` (`src/crafting.cpp:418`).

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2915,2935p' src/handle_action.cpp
sed -n '418,520p' src/crafting.cpp
grep -n "craft_command\|select_crafting_recipe\|recipe_id goto_recipe" src/crafting.cpp src/crafting_gui.cpp | head -20
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/crafting.cpp src/crafting_gui.cpp
```

- [ ] **Step 3: Отдельно разобрать выбор компонентов**

Крафт выбирает конкретные предметы под каждый компонент рецепта. Установить: происходит ли выбор целиком до начала активности, или активность может доспрашивать по ходу.

```bash
grep -n "comp_selection\|select_item_component\|select_tool_component" src/crafting.cpp | head -20
```

Если выбор компонентов полностью предрешён и сериализуем — это класс P и крафт режется. Если активность доспрашивает — класс A, и надо назвать способ предрешения.

- [ ] **Step 4: Разобрать `craft_command` как готовое намерение**

```bash
grep -n "class craft_command" src/crafting.h src/craft_command.h 2>/dev/null
grep -rn "void serialize\|void deserialize" src/craft_command.h 2>/dev/null
```

Если `craft_command` уже сериализуем, крафт получает намерение бесплатно. Это надо проверить, а не предположить.

- [ ] **Step 5: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for crafting"
```

---

## Task 3: Разделка

**Точка входа:** `src/handle_action.cpp:2703` (`case ACTION_BUTCHER`), далее `src/butchery.cpp`.

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2703,2773p' src/handle_action.cpp
grep -nE "^void |^bool |^static " src/butchery.cpp | head -30
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/butchery.cpp
```

- [ ] **Step 3: Проверить предупреждения посреди процесса**

Разделка известна тем, что переспрашивает при нехватке инструмента, при риске испортить шкуру и при прерывании. Установить по каждому вопросу, происходит ли он до траты moves.

- [ ] **Step 4: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for butchery"
```

---

## Task 4: Вождение

**Точка входа:** `src/handle_action.cpp:2947` (`case ACTION_CONTROL_VEHICLE`).

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2947,3010p' src/handle_action.cpp
grep -n "void vehicle::use_controls\|::pldrive\|handle_potential_theft" src/vehicle_use.cpp src/game.cpp src/avatar_action.cpp | head
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/vehicle_use.cpp
```

- [ ] **Step 3: Отдельно проверить `handle_potential_theft`**

Эта функция вызывается прямо из `game::do_turn` (`src/do_turn.cpp:562`) и задаёт вопрос. Установить, задаётся ли он в момент, когда мир уже мутировал, и что произойдёт при двух игроках в чужой машине.

- [ ] **Step 4: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for vehicle control"
```

---

## Task 5: Строительство

**Точка входа:** `src/handle_action.cpp:2935` (`case ACTION_CONSTRUCT`), далее `src/construction.cpp`.

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2935,2947p' src/handle_action.cpp
grep -nE "^void |^bool |^static " src/construction.cpp | head -30
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/construction.cpp
```

- [ ] **Step 3: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for construction"
```

---

## Task 6: Чтение

**Точка входа:** `src/handle_action.cpp:2773` (`case ACTION_READ`).

Чтение интересно тем, что вовлекает нескольких персонажей: читающий может учить слушателей, а слушатели — это в мультиплеере другие игроки.

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2773,2821p' src/handle_action.cpp
grep -n "::read(\|get_book_reader\|read_activity_actor" src/character.cpp src/activity_actor.cpp | head -15
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/character.cpp | head -40
```

Отфильтровать до пути чтения.

- [ ] **Step 3: Отдельно записать вопрос о слушателях**

Установить, спрашивает ли игра, кого учить, и в какой момент. Если спрашивает посреди активности — это класс A и в мультиплеере требует отдельного намерения от каждого слушателя.

- [ ] **Step 4: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for reading"
```

---

## Task 7: Торговля

Торговля — двусторонний интерактивный процесс. Самый вероятный кандидат на вердикт «не режется» в исходной форме.

**Точка входа:** `src/npctalk.cpp`, ветка торговли в диалоге.

- [ ] **Step 1: Собрать путь исполнения**

```bash
grep -rn "trade(\|trade_ui\|npc_trading" src/npctalk.cpp src/npctrade.cpp src/trade_ui.cpp 2>/dev/null | head -20
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/npctrade.cpp src/trade_ui.cpp 2>/dev/null
```

- [ ] **Step 3: Оценить форму намерения для сделки**

Если торговый интерфейс целиком локален, а мир мутирует одним махом в конце, то намерение — это итоговый список перемещаемых предметов и сумма. Проверить, так ли это, и записать вердикт.

- [ ] **Step 4: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for trading"
```

---

## Task 8: Установка бионики и частей транспорта

**Точки входа:** установка КБМ в `src/bionics.cpp`, установка частей в `src/veh_interact.cpp`.

- [ ] **Step 1: Собрать пути исполнения**

```bash
grep -n "install_bionics\|::install_bionic" src/bionics.cpp src/character.cpp | head
grep -nE "^void veh_interact::|^bool veh_interact::" src/veh_interact.cpp | head -20
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/bionics.cpp src/veh_interact.cpp
```

- [ ] **Step 3: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for installation"
```

---

## Task 9: Взлом

**Точка входа:** `src/iexamine_actors.cpp:235` (`iexamine::try_start_hacking`).

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '225,250p' src/iexamine_actors.cpp
grep -n "try_start_hacking\|can_hack\|hacking_activity" src/iexamine.cpp src/activity_actor.cpp | head -15
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/iexamine.cpp src/iexamine_actors.cpp | head -40
```

- [ ] **Step 3: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for hacking"
```

---

## Task 10: Погрузка и разгрузка контейнеров

**Точка входа:** `src/handle_action.cpp:2878` (`case ACTION_UNLOAD_CONTAINER`), плюс расширенный инвентарь в `src/advanced_inv.cpp`.

Расширенный инвентарь важен отдельно: он содержит `test_mode ? amount = possible_max : query_int(...)` (`src/advanced_inv.cpp:2351`) — то есть уже сейчас имеет ветку без вопроса. Это готовый пример того, как выглядит разрезание.

- [ ] **Step 1: Собрать путь исполнения**

```bash
sed -n '2878,2915p' src/handle_action.cpp
sed -n '2340,2360p' src/advanced_inv.cpp
```

- [ ] **Step 2: Перечислить обращения к UI**

```bash
grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/advanced_inv.cpp
```

- [ ] **Step 3: Посчитать, сколько обращений уже обёрнуты в `test_mode`**

```bash
grep -c "test_mode" src/advanced_inv.cpp
```

Каждое такое место — уже наполовину разрезанное действие, и это снижает оценку стоимости Ф1.

- [ ] **Step 4: Записать раздел отчёта и закоммитить**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 separability verdict for container loading"
```

---

## Task 11: Сводка и решение по воротам

**Files:**
- Modify: `docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md`

- [ ] **Step 1: Заполнить таблицу сводки**

По одной строке на действие: номер, название, вердикт, число M-обращений, число A-обращений.

- [ ] **Step 2: Оценить объём Ф1 по собранным числам**

```bash
grep -rcE "query_yn|query_int|uilist|popup|string_input_popup|game_menus::inv" src/*.cpp | grep -v ":0$" | sort -t: -k2 -rn | head -20
```

Дать оценку Ф1 в неделях, опираясь на: общее число M-обращений по десяти действиям, экстраполяцию на остальной слой действий и число мест, уже обёрнутых в `test_mode`.

Если оценка расходится с заложенными в спеке 5-8 неделями больше чем вдвое, это записывается явно и спек правится.

- [ ] **Step 3: Написать решение по воротам**

Ровно одно из двух, с обоснованием:

- **Продолжать по спеку.** Допустимо, если ни одно действие не получило вердикт «не режется», либо получившие его действия можно временно исключить из мультиплеера без потери играбельности — и тогда назвать поимённо, какие именно и что игрок теряет.
- **Пересматривать дизайн.** Если есть действия с вердиктом «не режется», без которых кооп бессмыслен. Тогда §5.3, а следом §6 и §7 спека переписываются под стриминг состояния мира, и оценка проекта пересчитывается с учётом плюс 12-20 недель из §11 спека.

- [ ] **Step 4: Записать список непроверенного**

Разведка охватила десять действий из нескольких десятков. Перечислить явно, что не проверялось, чтобы вердикт не читался шире, чем он есть.

- [ ] **Step 5: Коммит**

```bash
git add docs/superpowers/reports/2026-07-31-mvp1-choose-execute-separability.md
git commit -m "docs: MVP-1 summary and gate decision"
```
