# MVP-1: разрезаемость choose/execute — результаты разведки

Дата: 2026-07-31
Спек: docs/superpowers/specs/2026-07-31-cdda-coop-multiplayer-design.md, §5.3 и §9.1

Проверяемый инвариант: `apply_intent` не имеет права открыть ни одно окно и ни о чём
спросить игрока. Всё, что требует решения игрока, принимается в фазе выбора и
полностью записывается в `mp_intent`.

Классы обращений к UI: P — до мутации мира, M — посреди исполнения, A — внутри
многоходовой активности, D — недостижимо в test_mode.

## 1. Стрельба

**Точка входа:** `src/handle_action.cpp:2821` (`case ACTION_FIRE:` → `fire();`, локальная функция `fire()` определена в `src/handle_action.cpp:1800`).

**Путь исполнения:**

1. `handle_action.cpp:1800` `static void fire()` — проверки годности (паралич, страх, brawler/gunshy, руки), затем при наличии огнестрельного оружия в руках — `handle_action.cpp:1850` `avatar_action::fire_wielded_weapon( you )`. Единое правило исключения для этого шага (см. правку по Finding 8 ниже): любая ветка, достижимая только когда в руках нет пригодного для стрельбы оружия (`fire_turret_manual`, `handle_action.cpp:1856`; «Draw what?» `uilist`, `handle_action.cpp:1870`), не относится к пути «стрельба оружием в руках» и в таблицу ниже не включена — это предварительное/альтернативное действие, а не часть решения о выстреле.
2. `avatar_action.cpp:816` `avatar_action::fire_wielded_weapon` — проверки (gunmod, не-оружие, несовместимый боеприпас), без обращений к UI, затем `avatar_action.cpp:840` `you.assign_activity( aim_activity_actor::use_wielded() )` — **фаза выбора заканчивается здесь и передаёт управление активности**, mutации мира и трат moves ещё не было.
3. `activity_actor.cpp:475` `aim_activity_actor::start` — вызывает `check_gun_ability_to_shoot` (может чинить простую неисправность оружия и тратить moves через `who.mod_moves()`, но это не вызов из набора UI-функций).
4. `activity_actor.cpp:536` `aim_activity_actor::do_turn` — вызывается движком активности каждый ход. Сначала, если оружие `RELOAD_AND_SHOOT` (лук/арбалет) ещё не заряжено, на `activity_actor.cpp:559` вызывает `load_RAS_weapon()` (`activity_actor.cpp:709`), которая на `activity_actor.cpp:733` может вызвать `you.select_ammo( used_gun )` — то есть открыть меню выбора стрелы/болта **до** `mode_fire`. Затем на `activity_actor.cpp:571` вызывает `target_handler::mode_fire( you, *this )`.
5. `ranged.cpp:490` `target_handler::mode_fire` — создаёт `target_ui` с `mode = Fire` и вызывает `ranged.cpp:2846` `target_ui::run()` — интерактивный цикл прицеливания/выбора цели/подтверждений (весь путь ниже находится внутри этого вызова, то есть внутри `do_turn` активности).
6. Внутри `target_ui::run()`: `action_aim()` (`ranged.cpp:3941`) и `action_aim_and_shoot()` (`ranged.cpp:3970`) вызывают `do_aim()` (`ranged.cpp:1853`), которая **тратит moves по одному (`you.mod_moves(-1)`, строка 1860) и снижает recoil на каждой итерации цикла**, пока не будет достигнут порог или не кончатся moves хода.
7. Если игрок на ветке `SWITCH_AMMO` (`ranged.cpp:3019`) вызывает `action_switch_ammo()` (`ranged.cpp:3914`) на оружии **без** `RELOAD_AND_SHOOT`, функция возвращает `false` (ветка `else`, `ranged.cpp:3932-3935`), цикл завершается с `ExitCode::Reload` (`ranged.cpp:3113`), выставляя `activity->aborted = true` и `activity->reload_requested = true` (`ranged.cpp:3115-3116`) — прицеливание отменяется в пользу обычной перезарядки.
8. По выходу из цикла: при `ExitCode::Fire` → `activity_actor.cpp:590` `aim_activity_actor::finish` детерминированно вызывает `who.fire_gun( here, fin_trajectory.back(), gun.qty, *gun, reload_loc )` (`activity_actor.cpp:612`), которая мутирует мир (патрон, урон, шум) без единого обращения к UI (проверено: в `Character::fire_gun`, `ranged.cpp:1105-…`, нет ни одного вызова из списка). При `aborted == true` (в том числе после шага 7) та же `finish()` сначала вызывает `unload_RAS_weapon()` (`activity_actor.cpp:601`, определение `activity_actor.cpp:744`), которая при `should_unload_RAS` и оставшемся боеприпасе вызывает `you.unload(loc, true)` → `Character::unload` (`character_inventory.cpp:2458`), и если у оружия есть более одной цели для разрядки — гейт `opts.size() > 1` (`character_inventory.cpp:2523`) — открывает `uilist( "Unload what?", … )` (`character_inventory.cpp:2524`); затем, если `reload_requested`, вызывает `g->reload_wielded( true )` (`activity_actor.cpp:605`) → `game::reload_wielded` (`game.cpp:7169`) → `game::reload` (`game.cpp:7052`), которая для не-RAS оружия идёт через `favorite_ammo_or_select` (`game.cpp:7120`) → `u.select_ammo(loc, prompt, empty)` (`game.cpp:7049`) — тоже открывает меню выбора боеприпаса.
9. Активность может быть прервана и напрямую, минуя `finish()`: `Character::cancel_activity()` (`character.cpp:5307`, вызов `activity.canceled(*this)` на `character.cpp:5309`) → `player_activity::canceled()` (`player_activity.cpp:392`) → `aim_activity_actor::canceled()` (`activity_actor.cpp:634`), которая тоже вызывает `unload_RAS_weapon()` (`activity_actor.cpp:636`) с тем же потенциальным `uilist`.

| Обращение | Файл:строка | Класс | Обоснование |
|---|---|---|---|
| `target_handler::mode_fire` (вызов) | `src/activity_actor.cpp:571` | A | Вызывается из `aim_activity_actor::do_turn` — то есть на каждом ходу многоходовой активности `ACT_AIM`, а не один раз в фазе выбора. Запускает весь интерактивный UI прицеливания. |
| `you.select_ammo(used_gun)` → `game_menus::inv::select_ammo` (автовыбор стрелы/болта при незаряженном RELOAD_AND_SHOOT) | вызов `src/activity_actor.cpp:733` внутри `load_RAS_weapon()`, вызываемой из `do_turn` на `src/activity_actor.cpp:559` | A | Найдено трассировкой начала `do_turn`, не буквальным grep по `avatar_action.cpp`/`ranged.cpp` (файл `activity_actor.cpp` не входил в исходную команду сбора). Срабатывает автоматически на первом ходу прицеливания луком/арбалетом без боеприпаса — внутри активности, раньше даже `mode_fire`. |
| `ui_manager::redraw` | `src/ranged.cpp:2975` | A | Перерисовка внутри игрового цикла `target_ui::run()`, который выполняется внутри `do_turn` активности прицеливания. |
| `query_yn` («Really attack yourself?») | `src/ranged.cpp:3584` | A | Вызывается из `confirm_non_enemy_target()`, которую вызывают ветки `FIRE` (`ranged.cpp:3030`) и `AIMED_SHOT`/`CAREFUL_SHOT`/`PRECISE_SHOT` (`ranged.cpp:3072`) цикла `run()` — внутри активности `ACT_AIM`. (Исправлено после ревью: в этой конкретной итерации `do_aim` ещё не вызывался — на ветке `FIRE` он не вызывается вовсе, на ветке `AIMED_SHOT` подтверждение по коду на `ranged.cpp:3068-3070` явно запрашивается **до** `action_aim_and_shoot()`. Класс A не меняется — вызов всё равно живёт внутри `do_turn`, — но старое обоснование «после мутации recoil в этом ходу» было неточным и снято.) |
| `query_yn` («Really attack %s?», не враждебный NPC) | `src/ranged.cpp:3593` | A | Та же функция `confirm_non_enemy_target()`, тот же контекст вызова — внутри активности. |
| `query_yn` (дружественные существа на линии огня) | `src/ranged.cpp:3631` | A | `prompt_friendlies_in_lof()`, вызывается на `ranged.cpp:2954` в начале каждой итерации цикла `run()` внутри `do_turn` — после того как предыдущий ход уже потратил moves на прицеливание. |
| `uilist` (Select preferences / смена режима огня) | `src/ranged.cpp:3812` | A | `action_switch_mode()`, вызывается по ветке `SWITCH_MODE` цикла `run()` (`ranged.cpp:3015`) — внутри активности. |
| `avatar::select_ammo` → `game_menus::inv::select_ammo` (выбор боеприпаса для RELOAD_AND_SHOOT, напр. лук) | `src/ranged.cpp:3927` (вызов), определение `src/game_inventory.cpp:3281` | A | Найдено не буквальным grep (вызов идёт как `you->select_ammo(gun)`, а не текстом `game_menus::inv`), а трассировкой ветки `SWITCH_AMMO` (`ranged.cpp:3019-3023`) цикла `run()`. `game_menus::inv::select_ammo` — меню инвентаря (внутри само использует `popup` при пустом инвентаре, `game_inventory.cpp:3299`), открывается посреди прицеливания, то есть внутри активности. |
| `u.select_ammo(loc, prompt, empty)` → `game_menus::inv::select_ammo` (перезарядка обычного оружия после отказа от прицеливания через `SWITCH_AMMO`) | вызов `src/activity_actor.cpp:605` (`g->reload_wielded(true)`) → `src/game.cpp:7169` → `src/game.cpp:7120` → `src/game.cpp:7049` | A | Новое (найдено по ревью). На оружии без `RELOAD_AND_SHOOT` `action_switch_ammo()` возвращает `false` (`ranged.cpp:3935`), цикл выходит с `ExitCode::Reload`, и `finish()` при `reload_requested` вызывает `game::reload_wielded`, которая открывает то же меню выбора боеприпаса — но уже не для текущего выстрела, а для отдельного действия «перезарядка». Внутри активности `ACT_AIM` (вызов происходит из её `finish()`), поэтому класс A. |
| `uilist( "Unload what?", … )` через `unload_RAS_weapon()` | вызовы `src/activity_actor.cpp:601` (из `finish()`, при отмене) и `src/activity_actor.cpp:636` (из `canceled()`, при прямом прерывании активности) → `Character::unload` (`character_inventory.cpp:2458`) → `uilist` (`character_inventory.cpp:2524`, гейт `opts.size() > 1` на `:2523`) | A | Новое (найдено по ревью). Срабатывает только при отмене прицеливания на `RELOAD_AND_SHOOT` оружии с оставшимся боеприпасом и более чем одной целью разрядки (сам ствол плюс разряжаемый гуномод/непустой brass catcher). Внутри активности, на пути отмены выстрела. |

Как решалось, что вызов «на пути»: путь исполнения ACTION_FIRE прослежен по цепочке вызовов из п. «Путь исполнения» выше, включая файлы, не входившие в исходную команду сбора брифа (`activity_actor.cpp`, `character_inventory.cpp`, `game.cpp`) — они добавлены трассировкой, а не грепом, и это явно указано в соответствующих строках таблицы. Включены только вызовы, достижимые из `fire_wielded_weapon` → `aim_activity_actor` (`start`/`do_turn`/`finish`/`canceled`) → `target_ui::run()` в режиме `TargetMode::Fire`, включая ветки отмены/перезарядки/разрядки, которые эта же активность запускает. Единообразно исключены (см. правку по Finding 8): любые ветки, реализующие *другое* действие, доступное только при отсутствии пригодного для стрельбы оружия в руках (`fire_turret_manual`, «Draw what?» `uilist`) — обе исключены по одному и тому же критерию, а не по разным, как было в предыдущей версии раздела. Также исключены прочие режимы `target_ui` (Throw, Reach, TurretManual, Turrets, Spell, SelectOnly), используемые другими действиями.

**Вердикт:** режется с оговоркой

**Набросок намерения:**

```cpp
struct intent_aim {
    item_location weapon;
    tripoint_abs_ms target;           // финальная точка прицеливания (fin_trajectory.back())
    gun_mode_id mode_id;              // РЕАЛЬНЫЙ ключ режима огня (std::map<gun_mode_id, gun_mode>);
                                       // int-индекс из брифа хрупок, если набор режимов (gunmod-ы)
                                       // отличается между пирами — см. ranged.cpp:3868-3873
    int moves_spent_aiming;           // сколько moves фаза выбора потратила на прицеливание — решено локально игроком
    double final_recoil;              // recoil на момент решения стрелять, чтобы execute не пересчитывал do_aim
    item_location reload_loc;         // боеприпас для RELOAD_AND_SHOOT — и при первичном авто-выборе
                                       // (load_RAS_weapon), и при ручном SWITCH_AMMO
    bool target_confirmed;            // результат confirm_non_enemy_target(), принят игроком в choose-фазе
    std::vector<weak_ptr_fast<Creature>> accepted_losses;
                                       // ИСПРАВЛЕНО после ревью: тип должен совпадать с реальным полем
                                       // aim_activity_actor::acceptable_losses (activity_actor_definitions.h:524),
                                       // а не character_id — list_friendlies_in_lof() (ranged.cpp:3634-3657)
                                       // явно включает дружественных МОНСТРОВ, а не только NPC
                                       // ( !cr->is_npc() && a == Creature::Attitude::FRIENDLY ), у которых
                                       // нет character_id; нужен creature-идентифицирующий тип (или
                                       // сериализуемый суррогат — позиции из traj на момент запроса)
};

struct intent_fire {
    item_location weapon;
    tripoint_abs_ms target;
    gun_mode_id mode_id;
    int shots;                        // gun.qty на момент решения стрелять
    item_location reload_loc;
};

// Отдельное намерение для веток отмены (шаги 7-9 пути исполнения): игрок решает бросить
// текущее прицеливание вместо выстрела. Не расширяет intent_fire/intent_aim — это
// осознанно другое действие (перезарядка/разрядка), см. "Что мешает" ниже.
struct intent_abort_aim {
    item_location weapon;
    bool unload_requested;            // аналог should_unload_RAS
    item_location unload_target;      // какой ствол/гуномод разряжать, если целей > 1 (Character::unload)
    bool reload_requested;            // аналог aim_activity_actor::reload_requested
    item_location reload_ammo;        // выбранный боеприпас для последующей перезарядки, если reload_requested
};
```

Поля `intent_aim` шире примера из брифа: `aim_moves_spent` из брифа переименован в `moves_spent_aiming` и дополнен `final_recoil`, потому что recoil — это накопленный результат интерактивного цикла `do_aim`, а не только счётчик потраченных moves; без него `apply_intent` не сможет детерминированно воспроизвести итог прицеливания (разброс/попадание). `int mode_index` из брифа заменён на `gun_mode_id mode_id` — реальный ключ, которым код индексирует режимы (`ranged.cpp:3873` `relevant->gun_set_mode( it.first )`, где `it.first` — `gun_mode_id`, не число); целочисленный индекс в `std::map<gun_mode_id, gun_mode>` хрупок, если у пиров разный набор гуномодов (иной порядок обхода карты). `target_confirmed` — прямой аналог решения `confirm_non_enemy_target()`. `accepted_losses` **исправлен по ревью**: изначально был указан как `std::vector<character_id>`, что неверно — реальное поле `acceptable_losses` (`activity_actor_definitions.h:524`) хранит `weak_ptr_fast<Creature>`, и `list_friendlies_in_lof()` целенаправленно включает дружественных монстров (`ranged.cpp:3651`: `!cr->is_npc() && a == Creature::Attitude::FRIENDLY`), у которых нет `character_id`; поле должно использовать creature-идентифицирующий тип. `reload_loc` покрывает оба источника выбора боеприпаса для RELOAD_AND_SHOOT — автоматический (`load_RAS_weapon`, шаг 4 пути) и ручной (`SWITCH_AMMO`, уже был в версии до ревью).

Не включено ни в один из intent-ов (сознательно, не по забывчивости): `Character::preferred_aiming_mode`, который `action_switch_mode()` тоже пишет (`ranged.cpp:3881`, `you->preferred_aiming_mode = aim_mode->action`). Это персистентная UI-настройка персонажа (какой aim-type предлагать по умолчанию в следующий раз), а не часть исхода текущего выстрела — `final_recoil`/`moves_spent_aiming` уже полностью описывают, что случилось с *этим* выстрелом. Но поле мутирует состояние персонажа и должно быть синхронизировано между пирами каким-то общим механизмом синхронизации настроек персонажа — если такого механизма нет, это отдельный (не UI-вызовный) пробел, который стоит передать дальше по плану, а не решать здесь.

**Что мешает и как обойти:** Прицеливание — интерактивный цикл (`target_ui::run()` внутри `aim_activity_actor::do_turn`), который тратит moves по одному через `do_aim()` (`ranged.cpp:1853-1875`) и может растянуться на несколько ходов персонажа (комментарий в коде: «Time spent on aiming is determined on the go by the player», `activity_actor.cpp:485`). Поэтому стрельба не может быть одним намерением уровня «нажал — выстрелил»: она обязана распасться на «прицелиться» (интерактивно, целиком локально у выбирающего игрока, никогда не выполняется внутри `apply_intent`, поскольку не мутирует общий мир — recoil и moves персонажа до выстрела можно трактовать как приватное состояние клиента, финализируемое в момент отправки намерения) и «выстрелить» (детерминированно: `Character::fire_gun` не содержит ни одного вызова из списка UI-функций и полностью зависит только от полей, которые уже перечислены в `intent_fire`). Все 10 обнаруженных A-обращений устраняются так:
- `target_handler::mode_fire` / `ui_manager::redraw` — вся интерактивная петля прицеливания выполняется целиком в choose-фазе на клиенте инициатора; в `apply_intent` она просто не запускается.
- `select_ammo` при автозарядке RELOAD_AND_SHOOT (`load_RAS_weapon`) и при ручной `SWITCH_AMMO` — оба сворачиваются в choose-фазу и передаются полем `reload_loc`; `apply_intent` получает уже выбранный боеприпас, а не выбирает его.
- `query_yn` (self-attack, non-hostile NPC) — решение фиксируется один раз в choose-фазе (тем же диалогом, что и сейчас, но до отправки намерения) и передаётся полем `target_confirmed`; execute никогда не спрашивает повторно.
- `query_yn` (friendlies in line of fire) — решение фиксируется в choose-фазе и передаётся списком `accepted_losses` (с исправленным типом, см. выше).
- `uilist` (switch mode) — настройка, влияющая только на то, какой `mode_id` попадёт в итоговый `intent_fire`/`intent_aim`; меню выполняется в choose-фазе до отправки намерения.
- `select_ammo` при перезарядке после отказа от прицеливания (`SWITCH_AMMO` на не-RAS оружии → `ExitCode::Reload` → `g->reload_wielded`) и `uilist("Unload what?")` при разрядке (`unload_RAS_weapon`, из `finish()`-abort и из `canceled()`) — эти два **не** являются частью выстрела: игрок явно отказывается от текущего прицеливания. Оба сворачиваются в choose-фазу отдельного намерения `intent_abort_aim`, которое вообще не порождает `intent_fire`/`intent_aim` в этот ход — это осознанно другое действие (перезарядка/разрядка оружия), с собственным choose/execute-разбором, аналогичным любому другому предмет-ориентированному действию, а не патч к правилам стрельбы.

Остаточный риск (причина «с оговоркой», а не «режется»): recoil/`final_recoil` и `moves_spent_aiming` вычисляются клиентом асинхронно, в течение нескольких ходов, пока другие пиры могут одновременно применять свои намерения к тем же существам на линии огня. Если цель успеет умереть или сдвинуться между началом прицеливания и применением `intent_fire`, `apply_intent` должен либо детерминированно переигрывать промах (используя сохранённую точку `target`, а не идентификатор существа), либо намерение должно быть отклонено валидацией — это решение уже не про вызовы UI, а про правило валидации устаревших намерений, которое должно быть явно специфицировано отдельно.

## 2. Крафт

**Точка входа:** `src/handle_action.cpp:2915` (`case ACTION_CRAFT:` → `player_character.craft();`), далее `Character::craft` (`src/crafting.cpp:418`).

**Путь исполнения:**

1. `crafting.cpp:418` `Character::craft()` — вызывает `select_crafter_and_crafting_recipe()` (`crafting_gui.cpp:2861`), интерактивный цикл выбора рецепта (см. ниже), затем при выбранном рецепте — `crafter->make_craft(rec->ident(), batch_size, loc)` (`crafting.cpp:431`).
2. `crafting.cpp:756` `Character::make_craft` → `make_craft_with_command(id, batch, /*is_long=*/false, loc)` (`crafting.cpp:768`) — создаёт `*last_craft = craft_command(...)` **заново** (`crafting.cpp:777`) и вызывает `last_craft->execute()` (`crafting.cpp:778`). Это первый, «холодный» вызов `craft_command::execute()`: `has_cached_selections()` гарантированно `false`, потому что объект только что создан.
3. `craft_command::execute(bool)` (`craft_command.cpp:141`) — файл **не входит** в список брифа (`grep` в Step 2 брифа собирает только `crafting.cpp`/`crafting_gui.cpp`), поэтому все обращения здесь найдены трассировкой, а не буквальным grep. Последовательно: проверка достаточности зарядов (`craft_command.cpp:166`), проверка проф. навыков (`:171`), догнивших компонентов (`:182`), избранных предметов (`:191`), затем цикл `crafter->select_item_component(...)` (`crafting.cpp:2923`, `craft_command.cpp:209`) и — для рецептов без шагов — `crafter->select_tool_component(...)` (`crafting.cpp:3358`, `craft_command.cpp:222`); для пошаговых рецептов — `select_step_tool_allocs()` (`craft_command.cpp:243`, использует тот же `select_tool_component` внутри `select_group`). Если у рецепта есть «невнимательные» шаги и крафтер — аватар, вызывается `show_craft_planning_modal(...)` (`craft_command.cpp:252`, определение `crafting.cpp:1050`). В конце — `crafter->start_craft(*this, loc, plans)` (`craft_command.cpp:258`).
4. `crafting.cpp:1907` `Character::start_craft` — сперва `command.create_in_progress_craft()` (`craft_command.cpp:598`, вызов `crafting.cpp:1915`), которая **реально списывает** выбранные компоненты (`sane_consume_items`/`consume_items`, `craft_command.cpp:658-669`) и списывает первую порцию зарядов инструмента (`craft_consume_step_tools` на «пробном» предмете, `craft_command.cpp:648-656`) — **это точка начала мутации мира** на данном пути. После возврата `craft` уже реальный предмет с реально изъятыми компонентами. Далее `start_craft` вызывает `place_craft_or_disassembly(*this, craft, loc, ...)` (`crafting.cpp:1936`, определение `crafting.cpp:902`) — **после** списания компонентов — и только затем `assign_activity(craft_activity_actor(...))` (`crafting.cpp:1944`).
5. `craft_activity_actor::do_turn` (`activity_actor.cpp:6250`) — вызывается движком активности каждый ход, пока `item_counter` не достигнет 100%. Внутри: списание инструментальных зарядов по 5%-вым шагам (`craft.craft_consume_step_tools`, не UI), обработка «невнимательных» шагов через `craft_actualize_scheduled` (`crafting.cpp:1883`, диспетчер по `item_wakeup_kind`), проверка провала (`craft.handle_craft_failure`, вызов `activity_actor.cpp:6545`, определение `item::handle_craft_failure`, `crafting.cpp:2351`) и, по достижении 100% для рецепта с `is_long`/практикой, **повторный вызов `crafter.last_craft->execute(location)`** (`activity_actor.cpp:6528`) — то есть **весь путь шагов 3-4 выше выполняется заново изнутри `do_turn`** для следующего батча/повторной практики, уже не как «холодный», а как «горячий» вызов с закэшированными (или протухшими) выборами.
6. Файл `activity_actor.cpp` также не входит в список брифа — обращения на строках 6515/6534 (внутри `do_turn`) найдены трассировкой.

Как решалось, что вызов «на пути»: включены все вызовы, достижимые из `Character::craft()` через `make_craft` → `craft_command::execute()` → `start_craft()` → `craft_activity_actor::{start,do_turn,finish,canceled}`, включая рекурсивный вход `do_turn` → `craft_command::execute()` (шаг 5) и через него — назад в `create_in_progress_craft()`/`place_craft_or_disassembly()`/`select_item_component`/`select_tool_component`/`select_requirements`. Исключены: `Character::recraft()` и `Character::long_craft()` как точки входа других экшенов (`ACTION_RECRAFT`, `ACTION_LONGCRAFT`) — они используют тот же `craft_command`, но не достижимы из `ACTION_CRAFT`; `Character::disassemble()`/`ACTION_DISASSEMBLE` — отдельный экшен, хотя в файле `crafting.cpp` физически рядом.

**Шаг 3 брифа (выбор компонентов):** выбор компонентов (`select_item_component`/`select_tool_component`) **не полностью предрешён и не сериализуем как есть** — это класс P на первом («холодном») проходе, но `craft_activity_actor::do_turn` **доспрашивает** тем же самым кодом при продолжении длинного/практического крафта (шаг 5 выше) и при провале на середине крафта через `Character::can_continue_craft` (`crafting.cpp:2649/2659`, вызывается из `item::handle_craft_failure` на `crafting.cpp:2407`, которую `do_turn` вызывает на `activity_actor.cpp:6545` при достижении `next_failure_point`). Способ предрешения для «холодного» пути есть (см. набросок намерения); для «доспрашивания» способа не нашлось — см. «Что мешает».

**Шаг 4 брифа (`craft_command` как готовое намерение):** класс `craft_command` (`src/craft_command.h:92`) **не имеет** `serialize`/`deserialize` — ни объявления, ни определения (проверено чтением `craft_command.h` и `craft_command.cpp` целиком). Крафт **не получает намерение бесплатно**. Однако два из трёх приватных полей-накопителей результата выбора **уже сериализуемы** сами по себе: `comp_selection<CompType>::serialize/deserialize` (`craft_command.cpp:91-110`) и `step_tool_alloc::serialize/deserialize` (`craft_command.cpp:117-133`). Это значит, что `item_selections`/`tool_selections`/`step_tool_allocs` можно вынести в намерение почти без переделки типов — сериализуема «начинка», а не обёртка.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `popup`/`query_yn`/`string_input_popup`/`uilist`/`ui_manager::redraw`/`ui_manager::redraw_invalidated` — весь браузер рецептов | `crafting_gui.cpp:322,324,368-373,2270-2285,2601-2618,2771,2877,2987,3085,3094,3132-3181` (внутри `select_crafter_and_crafting_recipe`, `crafting_gui.cpp:2861`; `2877` — `ui_manager::redraw_invalidated()` в самом теле цикла `while(!impl.is_done())`) | грep | P | Модальный цикл выбора рецепта (поиск/фильтр/сравнение/пометка прочитанным, перерисовка цикла на `:2877`) выполняется целиком **до** вызова `make_craft`, никакой мутации мира ещё не было. |
| `popup` («Вы больше не можете это скрафтить!») | `crafting.cpp:469` (`Character::making_would_work`, определение `crafting.cpp:457`) | грep | A (чисто) | `making_would_work` на пути `ACTION_CRAFT` вызывается только из `craft_activity_actor::do_turn` при продолжении длинного/практического крафта (`activity_actor.cpp:6527`) — внутри активности. (Второй вызывающий, `Character::recraft()`, относится к другому экшену `ACTION_RECRAFT` и в путь не входит.) |
| `query_yn` («…нет тары для жидкости, всё равно продолжить?») | `crafting.cpp:544` (`check_eligible_containers_for_crafting`) | грep | P и A (два пути) | Вызывается двумя разными путями: (а) из обработчика `CONFIRM` в браузере рецептов (`crafting_gui.cpp:2622`) — P, до мутации; (б) из `Character::making_would_work` (`crafting.cpp:457`, см. строку выше), вызываемой из `craft_activity_actor::do_turn` при продолжении длинного/практического крафта (`activity_actor.cpp:6527`) — A, внутри активности. Учитывается класс A, так как этот путь достижим из активности. |
| `uilist` («Что делать с %s?» — надеть/бросить/убрать/уронить) | `crafting.cpp:961` (`place_craft_or_disassembly`, вызывается из `start_craft`, `crafting.cpp:1936`) | грep | **M** (основной путь) / A (повторный путь) | Основной путь: вызывается из `start_craft()` **после** того, как `create_in_progress_craft()` (`craft_command.cpp:598`, вызов `crafting.cpp:1915`) уже реально списал компоненты крафта (`craft_command.cpp:658-669`) — мутация мира началась, а ответ определяет, что происходит с уже надетым/удерживаемым предметом и куда денется предмет крафта. Это прямое нарушение инварианта, вне какой-либо активности (до `assign_activity`, `crafting.cpp:1944`). Тот же вызов повторно достижим из `do_turn` на шаге продолжения батча (`activity_actor.cpp:6528` → тот же `start_craft`) — там он уже класс A. |
| `cancel_activity_or_ignore_query` (отвлечение «шаг завершён», `distraction_type::craft_step_complete`) | `crafting.cpp:1027` (`fire_step_complete_distraction`) | грep | A | Достижимо из `do_turn` через `craft_actualize_scheduled` (`crafting.cpp:1883`, диспетчер-шим по `item_wakeup_kind`) → `craft_actualize_ready` (`crafting.cpp:1664/1670/1690`) → `fire_step_complete_distraction`. Срабатывает только когда `!was_on_craft` — то есть когда do_turn обрабатывает предмет крафта **другого** персонажа (напр. NPC-помощника), а функция при этом опрашивает `get_avatar()` — существующее для одиночной игры допущение «есть один локальный игрок», прямо релевантное MP-разбору. |
| `uilist` («Шаг: …\nЧто делать?» — ждать/заняться другим/таймер) | `crafting.cpp:1105` (`show_craft_planning_modal`) | грep | P (первый проход) / A (продолжение) | Вызывается один раз из `craft_command::execute()` при первом старте рецепта с «невнимательными» шагами (`craft_command.cpp:252`, `from_step=0`, до `assign_activity`) — P. Но `do_turn` при продолжении батча вызывает `last_craft->execute(location)` заново (`activity_actor.cpp:6528`) **без** ограничения по `from_step`, то есть модал показывается заново на **каждый** батч длинного/практического крафта — внутри активности. |
| `uilist` («Установить будильник на когда?») | `crafting.cpp:1128` (та же `show_craft_planning_modal`) | грep | P / A | Тот же вызов и тот же путь дублирования, что и предыдущая строка — вложенный под-диалог того же модала. |
| `cancel_activity_or_ignore_query` («Ваш таймер сработал: …», `distraction_type::noise`) | `crafting.cpp:1346` (`craft_actualize_alarm`) | грep | A | Прямо достижимо из `do_turn`: ветка «отложенная запись» (`activity_actor.cpp`, между установкой `passive_started_at` и проверкой `ready_at`) вызывает `craft_actualize_scheduled(craft, item_wakeup_kind::alarm, …)` → `craft_actualize_alarm`, если будильник уже просрочен на момент входа в пассивный шаг. В отличие от строки выше, здесь нет проверки «чужой ли это крафт» — сработает и для собственного крафта аватара, если у него `activity.id()` не null. |
| `popup` («Не хватает компонентов, чтобы продолжить крафт!») | `crafting.cpp:2687` (`can_continue_craft`) | грep | A | `can_continue_craft` вызывается из `item::handle_craft_failure` (`crafting.cpp:2407`), которую `do_turn` вызывает при достижении `craft.get_next_failure_point()` (обычный, не крайний случай многоходового крафта). |
| `query_yn` («Списать недостающие компоненты и продолжить крафт?») | `crafting.cpp:2697` (`can_continue_craft`) | грep | A | Тот же путь, что и строка выше — тот же вызов `can_continue_craft`. |
| `query_yn` («Часть компонентов сгнила, всё равно продолжить?») | `crafting.cpp:2702` (`can_continue_craft`) | грep | A | Тот же путь. |
| `query_yn` («Часть компонентов избранная, всё равно продолжить?») | `crafting.cpp:2711` (`can_continue_craft`) | грep | A | Тот же путь. |
| `popup` («Не хватает нужных инструментов, чтобы продолжить крафт!») | `crafting.cpp:2791` (`can_continue_craft`) | грep | A | Тот же путь; ветка нехватки инструментов, а не предметов. |
| `uilist` («Какой набор компонентов использовать?») | `crafting.cpp:2899` (`Character::select_requirements`) | грep | P / A | Единственный вызывающий — `requirement_data::select_alternative` (`requirements.cpp:1795`), вызываемая из `craft_command.cpp:201-202` внутри блока `if( need_selections )`. P на «холодном» первом вызове `execute()`. A — когда `execute()` вызывается заново из `do_turn` (`activity_actor.cpp:6528`) и `need_selections` снова становится `true` из-за протухшего кэша (`craft_command.cpp:151-161`). (Через `can_continue_craft` эта функция НЕ вызывается — там повторный выбор идёт напрямую через `select_item_component`/`select_tool_component`, без `select_requirements`.) |
| `uilist` («Использовать какой предмет?», выбор компонента) | `crafting.cpp:3052` (`Character::select_item_component`) | грep | P / A | P на первом проходе (`craft_command.cpp:209`); A — при повторном выборе внутри `can_continue_craft` (`crafting.cpp:2730`, для компонентов, уничтоженных провалом или ставших недоступны) и при повторном `execute()` из `do_turn`. |
| `uilist` («Использовать какой инструмент?») | `crafting.cpp:3417` (`Character::select_tool_component`) | грep | P / A | P на первом проходе (`craft_command.cpp:222`, `craft_command.cpp:521` внутри `select_step_tool_allocs`); A — при повторном выборе внутри `can_continue_craft` (`crafting.cpp:2855`) и при повторном `execute()` из `do_turn`. |
| `query_yn` («Не хватает зарядов, чтобы завершить %s. Всё равно начать крафт?») | `craft_command.cpp:166` | косв. | P / A | P на «холодном» вызове из `make_craft_with_command` (шаг 2 пути). A — если при повторном `execute()` из `do_turn` (шаг 5) кэшированный выбор компонентов протух и код вновь входит в ветку `need_selections` (см. `craft_command.cpp:151-161`). |
| `popup` («Нет нужных умений/квалификаций, чтобы это скрафтить!») | `craft_command.cpp:171` | косв. | P / A | Тот же механизм дублирования, что и строка выше. |
| `query_yn` («Крафт использует гнилые компоненты. Всё равно начать?») | `craft_command.cpp:182` | косв. | P / A | Тот же механизм. |
| `query_yn` («Крафт использует избранные компоненты. Всё равно начать?») | `craft_command.cpp:191` | косв. | P / A | Тот же механизм. |
| `query_yn` (`query_continue`, «Часть ранее использованных компонентов пропала. Продолжить?») | `craft_command.cpp:302` | косв. | **чистый A** | В отличие от строк выше, эта ветка *недостижима* на «холодном» первом вызове (`has_cached_selections()` гарантированно `false` для только что созданного `craft_command`, шаг 2 пути) — она существует исключительно для случая, когда `execute()` вызывается повторно с уже заполненным `item_selections`/`tool_selections`, то есть исключительно из `do_turn` (шаг 5). |
| `query_yn` («%s не пустой. Всё равно продолжить?», разлив жидкого компонента) | `craft_command.cpp:364` и `:387` (`continue_prompt_liquids`, вызывается из `create_in_progress_craft`, `craft_command.cpp:622-626`, только для `is_avatar()`) | косв. | P / A | P на первом проходе (до фактического необратимого списания компонентов в этом же вызове `create_in_progress_craft`); A — тот же вызов внутри `create_in_progress_craft`, вызванной повторно из `do_turn` (шаг 5). |
| `query_yn` («Продолжать практиковаться до роста мастерства?») | `activity_actor.cpp:6515` (`craft_activity_actor::do_turn`) | косв. | A | Прямо внутри `do_turn`, без единого уровня косвенности — классический случай A по определению из `conventions.md`. |
| `query_yn` («Мастерство выросло. Прекратить практику?») | `activity_actor.cpp:6534` (`craft_activity_actor::do_turn`) | косв. | A | Тоже прямо внутри `do_turn`. |

Итог по проходам: базовый grep по `crafting.cpp`/`crafting_gui.cpp` дал 16 различающихся мест вызова (включая сгруппированный браузер рецептов), из которых 1 — чистый P, 1 — M (с дополнительной A-веткой того же кода), 14 — A (часть из них также имеют P-ветку на первом проходе, но для вердикта учитывается достижимость как A). Проход трассировки на один уровень косвенности (файлы `craft_command.cpp` и `activity_actor.cpp`, не входящие в список брифа) добавил 8 мест, все — класса A, из них одно (`craft_command.cpp:302`) достижимо **исключительно** как A. Итого: **1 M**, **22 A**-достижимых обращений (считая по числу строк таблицы с классом A/M, без двойного счёта дублирующихся P/A строк).

**Вердикт:** не режется

**Набросок намерения:**

```cpp
// Намерение "начать крафт" — соответствует ОДНОМУ, "холодному" проходу
// craft_command::execute() → start_craft(), от Character::craft() до
// assign_activity(craft_activity_actor). НЕ покрывает продолжение батча —
// см. "Что мешает" ниже, отдельного чистого намерения для него нет.
struct intent_start_craft {
    recipe_id rec;
    int batch_size;
    bool is_long;                                    // craft_command::longcraft
    std::optional<tripoint_bub_ms> workbench_loc;     // nullopt = крафт из инвентаря/с рук

    // Результат select_item_component()/select_tool_component()/
    // select_step_tool_allocs(), предрешённый в фазе выбора. Оба вложенных
    // типа УЖЕ сериализуемы (comp_selection<T>::serialize/deserialize,
    // craft_command.cpp:91-133; step_tool_alloc::serialize/deserialize,
    // craft_command.cpp:117-133) — сам craft_command serialize/deserialize
    // не имеет НИГДЕ (проверено), но эта "начинка" уже готова.
    std::vector<comp_selection<item_comp>> item_selections;
    std::vector<comp_selection<tool_comp>> tool_selections;
    std::vector<std::vector<step_tool_alloc>> step_tool_allocs;
    // select_requirements (crafting.cpp:2899) возвращает `const requirement_data *`
    // в `alternatives[menu.ret]` — сырой указатель НЕЛЬЗЯ класть в намерение,
    // он не переживёт сериализацию/сеть. Но и `menu.ret` сам по себе не годится
    // как индекс "в общем списке альтернатив рецепта": `alternatives` в этой
    // сигнатуре — не deduped_requirements().alternatives() целиком, а результат
    // feasible_alternatives() (requirements.cpp:1794), то есть уже ОТФИЛЬТРОВАННОЕ
    // по текущему инвентарю подмножество, переданное в select_requirements
    // (requirements.cpp:1795). Индекс в этом подмножестве нестабилен — он
    // зависит от того, что было в инвентаре в момент вызова, а не от рецепта.
    // Нужен индекс в ПОЛНОМ, стабильном deduped_requirements().alternatives()
    // (requirements.h:476) — то есть на месте вызова нужно сопоставить указатель,
    // выбранный в feasible_alternatives(), с его позицией в исходном alternatives(),
    // и сохранить именно её:
    size_t chosen_requirement_alternative_index; // индекс в alternatives(), НЕ в
                                                  // отфильтрованном feasible_alternatives()

    // Ответы на диалоги craft_command::execute()/create_in_progress_craft(),
    // предрешённые один раз при отправке намерения:
    bool confirm_insufficient_charges;       // craft_command.cpp:166
    bool confirm_use_rotten;                 // craft_command.cpp:182
    bool confirm_use_favorite;                // craft_command.cpp:191
    bool confirm_nonempty_liquid_containers;  // craft_command.cpp:364/387

    // Разрешение конфликта "чем занять руки" — ИСПРАВЛЯЕТ найденную M:
    // сейчас uilist (crafting.cpp:961) показывается ПОСЛЕ списания
    // компонентов; все условия, от которых зависит выбор
    // (has_two_arms_lifting, наличие верстака рядом, has_wield_conflicts),
    // известны ДО списания — перенос решения в фазу выбора ничего не теряет.
    enum class wield_conflict_choice { not_applicable, wield_craft, drop_craft, stash, drop };
    wield_conflict_choice on_wield_conflict;

    // Планы для "невнимательных" шагов, предрешённые для ПЕРВОГО прохода
    // по рецепту (show_craft_planning_modal, crafting.cpp:1105/1128).
    // attention_plan не имеет СОБСТВЕННОГО serialize/deserialize, но его поля
    // уже читаются/пишутся построчно как часть состояния предмета-крафта
    // (savegame_json.cpp:2843 — запись цикла по step_plans, :3096 — чтение) —
    // это существующий путь сохранения, а не новый код; для намерения нужно
    // просто вынести те же два поля, а не изобретать сериализацию с нуля.
    std::vector<attention_plan> step_plans;
};
```

**Что мешает и как обойти:** У крафта две отдельные проблемы. Вердикт «не режется» опирается на вторую (она однозначно подпадает под правило вердикта); первая — реальная находка уровня M, для которой обход есть, но которую правило из `conventions.md`/`controller-addenda.md` дословно не разбирает (см. ниже, почему на неё нельзя опереться как на «механический» вывод).

Первая, найденная, но не покрытая таблицей правил: `place_craft_or_disassembly` открывает `uilist` «Что делать с %s?» (`crafting.cpp:961`) **после** того, как `create_in_progress_craft()` уже физически списал компоненты (`craft_command.cpp:658-669`) и часть зарядов инструмента, но **до** того, как активность вообще создана (`assign_activity`, `crafting.cpp:1944`) — то есть это не активность, а прямое нарушение инварианта в синхронном вызове, классический M. Обходной путь для этой конкретной строки прост и назван (`on_wield_conflict` в наброске намерения выше, поле `wield_conflict_choice`): все условия, определяющие, нужно ли вообще показывать это меню, известны **до** списания компонентов (положение относительно верстака, обе ли руки заняты, конфликтует ли текущий предмет в руках) — можно и нужно спросить это раньше и просто исполнить готовое решение в `execute`-фазе. Формально `controller-addenda.md` (A3) говорит, что «режется с оговоркой» требует нулевого числа M-строк — то есть эта находка **исключает** этот вердикт независимо от найденного обхода. А базовое правило `conventions.md` определяет «не режется» как «есть M, для которого обход не найден» — но обход для этой конкретной M-строки найден. Таким образом, случай «M есть, обход найден» дословной таблицей правил не покрыт ни в одну, ни в другую сторону; я не подставляю сюда собственную интерпретацию и просто фиксирую разрыв. Вердикт этого раздела опирается не на эту находку, а на следующую, которая правилом покрыта однозначно.

Вторая, решающая для вердикта: `craft_activity_actor::do_turn` при продолжении длинного/практического крафта **не просто просит подтверждение** — он **заново вызывает всю фазу выбора** (`crafter.last_craft->execute(location)`, `activity_actor.cpp:6528`), тем самым делая доступной изнутри активности буквально всю UI-поверхность `craft_command::execute()`/`create_in_progress_craft()`, включая выбор компонентов/инструментов (`uilist`, `crafting.cpp:3052`/`3417`) и подтверждения гнили/избранности/недостатка зарядов (`craft_command.cpp:166-191`). Часть из этого предрешается по тому же шаблону, что и M-находка выше (простые булевы политики: «крафтить дальше практику автоматически», «останавливаться молча при нехватке» и т.п. — см. `intent_craft_continuation_policy` ниже), и для этой части способ есть. Но остаётся необрешаемый остаток: `Character::can_continue_craft` (`crafting.cpp:2649/2659`, вызывается из `item::handle_craft_failure` при провале крафта на середине, `crafting.cpp:2407`, вызов из `do_turn` — `activity_actor.cpp:6545`) решает, **какие именно** компоненты нужно доизбрать и предложить взамен, основываясь на (а) результате RNG-броска провала, случившегося во время `do_turn` этого самого хода, и (б) реальном, актуальном на этот ход состоянии карты/инвентаря (`check_item_components_missing`/`check_tool_components_missing`, `craft_command.cpp:152-153`) — которое в многопользовательской игре могло измениться из-за действий других игроков между началом крафта и этим ходом. Ни то, ни другое не известно и не может быть детерминированно вычислено в момент отправки исходного намерения на старте крафта: это не вопрос «какой ответ дал бы игрок», а вопрос «что вообще произошло в мире с тех пор», который в принципе нельзя закодировать в `mp_intent`, отправленный раньше. Единственные пути, которые превращают это в детерминированное правило (жёстко резервировать все N батчей компонентов заранее — меняет игровой баланс/ощущение и не решает вопрос RNG-провала; или автоматически прерывать крафт без вопроса при любой нехватке, без права продолжить с заменой) — это не перенос выбора игрока, а изменение самого игрового поведения, то есть продуктовое решение, а не механическая переклассификация. Согласно `controller-addenda.md`: «If even one A row has no remedy, … the verdict is не режется» — про эти конкретные строки (`crafting.cpp:2687/2697/2702/2711/2791`, `crafting.cpp:3052`/`3417` в контексте `can_continue_craft`) обход не найден. Это единственное основание, дословно покрытое правилом вердикта, и его одного достаточно для «не режется» — независимо от того, как трактовать находку M выше.

Отдельно, но с тем же диагнозом «единого локального игрока»: `fire_step_complete_distraction` (`crafting.cpp:1027`) и `craft_actualize_alarm` (`crafting.cpp:1346`) опрашивают `get_avatar()` — единственного локального игрока — из логики, привязанной к предмету крафта, а не к конкретному подключённому клиенту. Это не решается добавлением поля в намерение: нужно сначала определить, *к какому клиенту* вообще адресован этот вопрос в многопользовательской партии, что выходит за рамки классификации P/M/A/D и относится к более общей задаче адресации UI-событий по пирам.

```cpp
// Политика продолжения батча/практики — покрывает ту часть шага 5 do_turn,
// для которой обход найден (см. текст выше). НЕ покрывает can_continue_craft
// (жёлтый список выше) — там обхода нет.
struct intent_craft_continuation_policy {
    bool auto_continue_long_craft;      // сейчас подразумевается is_long == true
    bool auto_continue_practice;        // activity_actor.cpp:6515 "Keep practicing…"
    bool stop_silently_on_shortfall;    // НЕ полная замена can_continue_craft: покрывает
                                         // только случай "заранее решили не продолжать",
                                         // а не "какие компоненты предложить взамен"
    std::vector<attention_plan> step_plans_per_batch; // повторяется на каждый батч,
                                                        // а не только на первый проход
};
```

Не включено ни в одно намерение (сознательно): точный набор компонентов/инструментов, которые `can_continue_craft` предложит взамен после провала — по причине, изложенной выше, это в принципе не вычислимо на момент отправки исходного намерения.

## 3. Разделка

**Точка входа:** `src/handle_action.cpp:2703` (`case ACTION_BUTCHER:` → `butcher( mouse_target )`). Бриф называет файл `src/butchery.cpp`, но сам вызываемый метод — `game::butcher`, определён в `src/game.cpp:6741`; `src/butchery.cpp` содержит только вспомогательные функции, которые `game::butcher` и активность разделки вызывают ниже по стеку.

**Путь исполнения:**

1. `game.cpp:6741` `game::butcher()` — ранние проверки (управление машиной, запечатанная местность, отсутствие трупов/предметов/инструментов) дают только сообщения и `return`, мутации мира нет.
2. `game.cpp:6844-6849`: если рядом враждебное существо — `query_yn( "You see %s nearby!  Start butchering anyway?" )` на `game.cpp:6846` — до какой-либо мутации мира и до выбора конкретного предмета. Класс P. Найдено трассировкой: `game.cpp` не входит в грep-список брифа (ограничен `butchery.cpp`).
3. `game.cpp:6876-6932`: `uilist kmenu` — выбор конкретного трупа/предмета для разделки, разборки или переработки, плюс пакетные пункты «Разделать всё» (`MULTIBUTCHER`)/«Разобрать всё» (`MULTIDISASSEMBLE_ONE/ALL`)/«Переработать всё» (`MULTISALVAGE`); `kmenu.query()` — `game.cpp:6932`. Тоже до мутации, класс P. Найдено той же трассировкой.
4. По результату меню код расходится на 4 ветки (`game.cpp:6971-7024`, switch по `butcher_select`/`indexer_index`). Только `BUTCHER_CORPSE` и `MULTIBUTCHER` (частный случай `BUTCHER_OTHER`) ведут к `butchery_activity_actor` и относятся к «разделке» в смысле брифа и `butchery.cpp`. Две другие ветки уходят в самостоятельные действия, доступные из того же меню по совпадению: `BUTCHER_DISASSEMBLE` → `Character::disassemble()` (`crafting.cpp:4075`, тот же механизм, что и `ACTION_DISASSEMBLE`, концептуально соседствует с Task 2); `BUTCHER_SALVAGE` → `salvage_actor::try_to_cut_up()` (`game.cpp:7022`, отдельный `iuse`-actor, синхронный, не входит ни в один из 10 действий MVP-1); `MULTIDISASSEMBLE_ONE/ALL` → `u.disassemble_all()` (`crafting.cpp:4176`, та же крафтовая машина). Все три исключены из таблицы и вердикта ниже как чужие действия, а не часть разделки. Также вне пути — `MultiButchery`/`multi_butchery_activity_actor` (авто-разделка по зонам лута, `handle_action.cpp:1703`) — отдельная точка входа, недостижимая из `ACTION_BUTCHER`.
5. `BUTCHER_CORPSE`/`MULTIBUTCHER` вызывают `butcher_submenu()` (`butchery.cpp:1180`; вызовы — `game.cpp:7000` и `:6978` соответственно) — выбор конкретного `butcher_type` (полная/быстрая разделка, свежевание, потрошение, четвертование, вскрытие и т.д.). Внутри — `uilist smenu` (`butchery.cpp:1322`, `.query()` — `:1418`). Всё ещё до мутации, класс P. Это единственное совпадение в файле `butchery.cpp`, найденное дословным grep из брифа, которое относится к фазе выбора, а не к активности.
6. При выбранном типе — `u.assign_activity( butchery_activity_actor( bd ) )` (`game.cpp:6985` для `MULTIBUTCHER`, `:7005` для одиночного трупа). **Фаза выбора заканчивается здесь**, мутации мира ещё не было.
7. `butchery_activity_actor::do_turn` (`activity_actor.cpp:13287`) — вызывается движком активности каждый ход. На первом ходе обработки очередного трупа (`this_bd->progress == 0_seconds`, `:13296`) вызывается `initiate_butchery()` (`:13224`): (а) `calculate_butchery_data()` (`:13205`, без UI) считает требования и время разделки; (б) `this_bd.corpse->spill_contents( here, ... )` (`:13232`) — **уже мутация мира**: высыпает содержимое трупа (карманы/проглоченное) на карту; (в) только после этого — `set_up_butchery()` (`butchery.cpp:234`, вызов на `activity_actor.cpp:13234`). Внутри `set_up_butchery` находятся все A-обращения из таблицы ниже: три `query_yn` о десакрализации/каннибализме (`butchery.cpp:201/206/211`, через `check_anger_empathetic_npcs_with_cannibalism`, вызывается из `set_up_butchery` на `:350` и `:364`) и `popup` о нехватке инструментов (`:293`). Если `set_up_butchery` вернёт `false`, `do_turn` просто пропускает этот труп (`bd.pop_back()`) — но `spill_contents` к этому моменту уже необратимо произошёл.
8. Последующие ходы `do_turn` только увеличивают `this_bd->progress` (`:13328`), либо, по завершении, вызывают `destroy_the_carcass()` (`butchery.cpp:1059`, вызов — `activity_actor.cpp:13323`). Прочитаны целиком вместе с `butchery_drops_harvest()` (`butchery.cpp:630`) и `butchery_quarter()` (`:1042`) — ни одного обращения из списка UI-функций (подтверждено тем же дословным grep по всему `butchery.cpp`: все 5 совпадений в файле уже перечислены в п.5 и п.7).
9. `butchery_activity_actor::finish()` (`activity_actor.cpp:13339`) — пусто, только `act.set_to_null()`, без UI. `::canceled()` (`:13344`) — только записывает текущий `progress` в переменную предмета; в отличие, например, от `aim_activity_actor::canceled()` из Task 1 (которая при прерывании дозывала `unload_RAS_weapon()` с потенциальным `uilist`), `butchery_activity_actor::canceled()` не вызывает `set_up_butchery` или что-либо ещё из списка UI-функций.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `query_yn` («You see %s nearby!  Start butchering anyway?») | `src/game.cpp:6846` | косв. | P | Проверка соседства враждебного существа — первое, что делает `game::butcher()` после того, как список трупов/предметов уже собран, но до выбора конкретного трупа/предмета и до `assign_activity`. Мутации мира не было. |
| `uilist kmenu` (выбор трупа/предмета для разделки/разборки/переработки, плюс пакетные пункты) | `src/game.cpp:6876` (объявление), `.query()` — `:6932` | косв. | P | Тот же вызов, до какой-либо мутации мира и до `assign_activity`. |
| `uilist smenu` (`butcher_submenu`, выбор `butcher_type`: полная/быстрая разделка, свежевание, потрошение, четвертование, вскрытие) | `src/butchery.cpp:1322` (объявление), `.query()` — `:1418` | грep | P | Вызывается из `game::butcher()` (`game.cpp:6978`/`:7000`) **до** `assign_activity( butchery_activity_actor(...) )` — часть фазы выбора, мутации мира ещё не было. |
| `query_yn` («Really desecrate the mortal remains… others may take issue», сочувствие + NPC рядом) | `src/butchery.cpp:201` | грep | A | `check_anger_empathetic_npcs_with_cannibalism()`, вызывается из `set_up_butchery()` (`butchery.cpp:350` для DISSECT-ветки, `:364` для остальных, кроме DISMEMBER), которая вызывается из `initiate_butchery()` (`activity_actor.cpp:13234`), вызываемой из `do_turn()` на первом ходу обработки трупа. К этому моменту `spill_contents()` (`activity_actor.cpp:13232`) уже отработал. |
| `query_yn` («Really desecrate the mortal remains…», только сочувствие) | `src/butchery.cpp:206` | грep | A | Тот же вызов, соседняя ветка той же функции. |
| `query_yn` («Really take this thing apart?  …others may take issue», только NPC рядом) | `src/butchery.cpp:211` | грep | A | Тот же вызов, третья ветка той же функции. |
| `popup` («You can't butcher this; you are missing some tools.») | `src/butchery.cpp:293` | грep | A | Внутри `set_up_butchery()`, тот же путь вызова, что и три строки выше — `initiate_butchery()` из первого хода `do_turn()`. Проверка `butchery_requirement->can_make_with_inventory(...)` (`:280-281`) детерминирована и опирается только на `crafting_inventory()` и данные трупа/`butcher_type`, уже известные к этому моменту. |

Итог по проходам: базовый grep по `butchery.cpp` (дословно из брифа) дал 5 совпадений — 3×`query_yn` (`:201/206/211`) и 1×`popup` (`:293`), все класс A, плюс `uilist smenu` (`:1322`/`:1418`), класс P; других совпадений в файле нет (проверено — весь файл прочитан вместе с телами `butchery_drops_harvest`/`destroy_the_carcass`/`butchery_quarter`, которые вызываются из `do_turn` после первого хода). Трассировка на один уровень косвенности — вверх по стеку, в `game.cpp` (вызывающий `game::butcher`, файл не входит в грep-список брифа) — добавила 2 места: `query_yn` про враждебное существо (`:6846`) и `uilist kmenu` (`:6876`/`:6932`), оба класса P. Итого: **0 M**, **4 A** (все из базового прохода), **3 P** (2 из трассировки, 1 из базового).

**Вердикт:** режется с оговоркой

**Набросок намерения:**

```cpp
// butchery_data (butchery.h:28) уже почти намерение: содержит item_location
// corpse и butcher_type b_type — единственные два поля, которые реально
// выбираются игроком (в uilist kmenu и uilist smenu, обе — P, до мутации).
// Остальные поля butchery_data (req, req_speed_bonus, time_to_butcher,
// progress) детерминированно пересчитываются в calculate_butchery_data()
// из этих двух plus текущей crafting_inventory() — дублировать их в
// намерении не нужно, apply_intent пересчитает их сам, без UI.
struct intent_butcher {
    item_location corpse;
    butcher_type b_type;

    // Предрешает три query_yn из check_anger_empathetic_npcs_with_cannibalism
    // (butchery.cpp:201/206/211). Вычисляется в фазе выбора ТОЙ ЖЕ логикой
    // (you.empathizes_with_monster(corpse.id) + скан g->all_npcs() на
    // "видит игрока и сочувствует" (butchery.cpp:190-197)), на тех же данных,
    // которые сейчас читаются в do_turn — просто на один шаг раньше, в
    // момент выбора трупа и типа разделки, а не на первом ходу активности.
    bool moral_quandary_prompted;   // нужно ли было вообще спрашивать
    bool moral_quandary_accepted;   // ответ игрока (значим только если prompted)
};

// Для MULTIBUTCHER (game.cpp:6977) намерение — просто список intent_butcher,
// по одному на труп; do_turn уже и так обрабатывает bd как стек одного трупа
// за раз, так что порядок элементов списка сохраняет текущее поведение.
struct intent_butcher_batch {
    std::vector<intent_butcher> items;
};
```

**Что мешает и как обойти:** Обхода требуют 4 строки, все класса A, все — из одной и той же функции `set_up_butchery()`, вызываемой на первом ходу `do_turn()` через `initiate_butchery()`.

Три `query_yn` (`butchery.cpp:201/206/211`) — один смысловой вопрос («продолжить, зная моральную цену?») с текстом, зависящим от двух булевых флагов: `you.empathizes_with_monster(corpse.id)` (черта персонажа, не меняется между выбором и первым ходом активности) и `nearby_empathetic_npc` (скан видимых NPC в момент вызова, `butchery.cpp:190-197`). Обе величины вычислимы в фазе выбора той же функцией, на тех же входных данных, что и сейчас — единственная разница в том, что скан NPC будет выполнен на один игровой тик раньше, чем сегодня. Это тот же класс допущения «мир таков, каким он был на момент отправки намерения», который уже принят конвенцией для остальных предрешённых A-находок в Task 1/2 (в отличие от `can_continue_craft` из Task 2, где решение зависит от результата RNG-броска, случившегося внутри уже идущей активности, и поэтому принципиально не может быть известно заранее). Ремедиум: посчитать оба флага и, если нужно, задать вопрос в фазе выбора; результат — `moral_quandary_prompted`/`moral_quandary_accepted` в намерении; `set_up_butchery()` в `apply_intent` просто читает готовый ответ вместо вызова `query_yn`, и при отказе ведёт себя как сегодня при `false` (труп пропускается, `bd.pop_back()`).

`popup` о нехватке инструментов (`butchery.cpp:293`) проверяет `butchery_requirement->can_make_with_inventory(...)` — тот же `requirement_id`, что вычисляет `calculate_butchery_data()` (`activity_actor.cpp:13211-13214`) из `corpse.harvest`, `corpse.size` и `this_bd.b_type`, и та же `you.crafting_inventory()`, что доступна в фазе выбора. Ремедиум — «заменить на детерминированное правило, вычисленное раньше»: выполнить ту же проверку `can_make_with_inventory` в фазе выбора, до создания намерения; при провале — показать то же сообщение сразу и не создавать `intent_butcher` вовсе (симметрично тому, как уже сегодня `game::butcher()` отказывается открывать `kmenu`, если во всём стеке предметов нет ни одного трупа/подходящего инструмента, `game.cpp:6826-6837`). Полю в намерении это не соответствует — это гейт на отправку намерения, а не данные, нужные `apply_intent`.

Отдельно, не влияет на вердикт, но стоит отметить: мутация (`spill_contents`, `activity_actor.cpp:13232`) в текущем коде происходит **до** `set_up_butchery()`, то есть до всех четырёх ремедиируемых вопросов — если бы вопросы остались внутри `do_turn`, это уже было бы нарушением независимо от классификации A. Поскольку весь блок предрешается в фазе выбора, `apply_intent` может вызывать `spill_contents()` после того, как решение уже известно, и порядок перестаёт иметь значение для инварианта — но это не единственная причина класть его в намерение: сам факт «живёт внутри `do_turn`» уже требует отдельного разбора по правилу `conventions.md`, независимо от относительного порядка с мутацией.

Не входит в намерение и не разбирается здесь (обосновано в п.4 «Путь исполнения» выше): `BUTCHER_DISASSEMBLE`, `BUTCHER_SALVAGE`, `MULTIDISASSEMBLE_ONE/ALL` — три соседних действия из того же начального `kmenu`, не являющиеся разделкой и не входящие в 10 действий MVP-1 (кроме разборки, которая частично пересекается по механизму с Task 2, но не тождественна её точке входа `ACTION_CRAFT`).

## Сводка

| # | Действие | Вердикт | M-обращений | A-обращений |
|---|---|---|---|---|

## Решение по воротам

_Заполняется в Task 11._
