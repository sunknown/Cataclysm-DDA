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

Поскольку файл брифа для Шага 2 назван неверно (реальная точка входа — не `butchery.cpp`, а `game.cpp`), UI-обращения, найденные прямо в теле `game::butcher()`, ниже отнесены к **грep**, а не к **косв.**: это не косвенность (вызов функции, которая внутри себя достаёт до UI), а исправление файлового охвата базового прохода на файл, где на самом деле лежит точка входа. Настоящая (в терминах A1) трассировка на один уровень косвенности — вглубь, в функции, вызываемые из пути, а не вверх, к самому вызывающему — выполнена отдельно для `activity_actor.cpp` (тела `do_turn`/`initiate_butchery`/`finish`/`canceled`, не входящие в `butchery.cpp`) и подробно — для цепочки `destroy_the_carcass()` → …, см. п. 8 и таблицу; она размечена как **косв.**

**Путь исполнения:**

1. `game.cpp:6741` `game::butcher()` — ранние проверки (управление машиной, запечатанная местность, отсутствие трупов/предметов/инструментов) дают только сообщения и `return`, мутации мира нет.
2. `game.cpp:6844-6849`: если рядом враждебное существо — `query_yn( "You see %s nearby!  Start butchering anyway?" )` на `game.cpp:6846` — до какой-либо мутации мира и до выбора конкретного предмета. Класс P. Найдено чтением тела `game::butcher()` — единственной функции, реально являющейся точкой входа; см. пояснение выше про исправленный охват базового прохода.
3. `game.cpp:6876-6932`: `uilist kmenu` — выбор конкретного трупа/предмета для разделки, разборки или переработки, плюс пакетные пункты «Разделать всё» (`MULTIBUTCHER`)/«Разобрать всё» (`MULTIDISASSEMBLE_ONE/ALL`)/«Переработать всё» (`MULTISALVAGE`); `kmenu.query()` — `game.cpp:6932`. Тоже до мутации, класс P. Найдено тем же чтением тела `game::butcher()`.
4. По результату меню код расходится на 4 ветки (`game.cpp:6971-7024`, switch по `butcher_select`/`indexer_index`). Только `BUTCHER_CORPSE` и `MULTIBUTCHER` (частный случай `BUTCHER_OTHER`) ведут к `butchery_activity_actor` и относятся к «разделке» в смысле брифа и `butchery.cpp`. Остальные ветки, включая другой случай того же `BUTCHER_OTHER`, уходят в самостоятельные действия, доступные из того же меню по совпадению: `MULTISALVAGE` → `u.assign_activity( longsalvage_activity_actor(...) )` (`game.cpp:6974-6975`, отдельная активность переработки лома, не разделка); `BUTCHER_DISASSEMBLE` → `Character::disassemble()` (`crafting.cpp:4075`, тот же механизм, что и `ACTION_DISASSEMBLE`, концептуально соседствует с Task 2); `BUTCHER_SALVAGE` → `salvage_actor::try_to_cut_up()` (`game.cpp:7022`, отдельный `iuse`-actor, синхронный, не входит ни в один из 10 действий MVP-1); `MULTIDISASSEMBLE_ONE/ALL` → `u.disassemble_all()` (`crafting.cpp:4176`, та же крафтовая машина). Все четыре исключены из таблицы и вердикта ниже как чужие действия, а не часть разделки. Также вне пути — `MultiButchery`/`multi_butchery_activity_actor` (авто-разделка по зонам лута, `handle_action.cpp:1703`) — отдельная точка входа, недостижимая из `ACTION_BUTCHER`.
5. `BUTCHER_CORPSE`/`MULTIBUTCHER` вызывают `butcher_submenu()` (`butchery.cpp:1180`; вызовы — `game.cpp:7001` и `:6978` соответственно) — выбор конкретного `butcher_type` (полная/быстрая разделка, свежевание, потрошение, четвертование, вскрытие и т.д.). Внутри — `uilist smenu` (`butchery.cpp:1322`, `.query()` — `:1418`). Всё ещё до мутации, класс P. Это единственное совпадение в файле `butchery.cpp`, найденное дословным grep из брифа, которое относится к фазе выбора, а не к активности.
6. При выбранном типе — `u.assign_activity( butchery_activity_actor( bd ) )` (`game.cpp:6985` для `MULTIBUTCHER`, `:7005` для одиночного трупа). **Фаза выбора заканчивается здесь**, мутации мира ещё не было.
7. `butchery_activity_actor::do_turn` (`activity_actor.cpp:13287`) — вызывается движком активности каждый ход. На первом ходе обработки очередного трупа (`this_bd->progress == 0_seconds`, `:13296`) вызывается `initiate_butchery()` (`:13224`): (а) `calculate_butchery_data()` (`:13205`, без UI) считает требования и время разделки; (б) `this_bd.corpse->spill_contents( here, ... )` (`:13232`) — **уже мутация мира**: высыпает содержимое трупа (карманы/проглоченное) на карту; (в) только после этого — `set_up_butchery()` (`butchery.cpp:234`, вызов на `activity_actor.cpp:13234`). Внутри `set_up_butchery` находятся все A-обращения из таблицы ниже: три `query_yn` о десакрализации/каннибализме (`butchery.cpp:201/206/211`, через `check_anger_empathetic_npcs_with_cannibalism`, вызывается из `set_up_butchery` на `:350` и `:364`) и `popup` о нехватке инструментов (`:293`). Если `set_up_butchery` вернёт `false`, `do_turn` просто пропускает этот труп (`bd.pop_back()`) — но `spill_contents` к этому моменту уже необратимо произошёл.
8. Последующие ходы `do_turn` только увеличивают `this_bd->progress` (`:13328`), либо, по завершении, вызывают `destroy_the_carcass()` (`butchery.cpp:1059`, вызов — `activity_actor.cpp:13323`). Ни `destroy_the_carcass()`, ни `butchery_drops_harvest()` (`butchery.cpp:630`), ни `butchery_quarter()` (`:1042`) не содержат ни одного обращения из списка UI-функций **буквально** — но у `destroy_the_carcass()` есть предупреждающий комментарий прямо на месте её вызова (`activity_actor.cpp:13324-13326`): «destroy_the_carcass might spill acid, which gives the player the option to cancel this activity». Комментарий подтверждается полной трассировкой (проверена по каждому звену): `destroy_the_carcass()` для трупа с кислотной кровью вызывает `here.add_splatter( type_blood, corpse_pos, … )` (`butchery.cpp:1094` в ветке «FATAL FAILURE» — срабатывает для любого `butcher_type` при провале `butchery_drops_harvest`; `:1133` в ветке `FIELD_DRESS`; `type_blood = corpse->bloodType()`, `:1068`, равен `fd_acid` тогда и только тогда, когда `corpse->has_flag(mon_flag_ACID_BLOOD)`, `mtype.cpp:450-453`) → `map::add_splatter()` (`map.cpp:7397`) → `map::mod_field_intensity()` (`:7127`) → `map::set_field_intensity()` (`:7146`; если на клетке ещё нет этого поля — ветка `:7157-7158`) → `map::add_field()` (`:7272`, параметр `hit_player` по умолчанию `true`, `map.h:1669`) → при `hit_player && p == player_character.pos_bub()` (`map.cpp:7320-7322`, то есть именно тогда, когда труп лежит на той же клетке, что и игрок, — а `pos` в `game::butcher()` без mouse-target совпадает с `u.pos_bub()`, так что это типичный случай) — синхронно вызывает `creature_in_field( player_character )` (`:7324`) → `map::creature_in_field()` (`map_field.cpp:1840`) → `map::player_in_field()` (`:1466`); для `fd_acid` (`:1491`) — `burn_body_part()` (`:148`) → `you.deal_damage( nullptr, bp, damage_instance(damage_acid, …) )` (`:155-156`). Отсюда два независимых, оба синхронных, пути к листингу UI-функций: (а) `Creature::deal_damage()` (`creature.cpp:1490`) считает боль и вызывает `mod_pain()` (`:1529`) → `Character::mod_pain()` (`character_health.cpp:3364`) → `Creature::mod_pain()`/`set_pain()` → `Character::set_pain()` (`:3389`, переопределение) → при изменении воспринимаемой боли — `react_to_felt_pain()` (`:3396`) → если `uistate.distraction_pain && is_avatar() && intensity >= 2` (`:284`) — `g->cancel_activity_or_ignore_query( distraction_type::pain, … )` (`:285`); (б) `Character::deal_damage()` (`:2673`) после `Creature::deal_damage()` (`:2683`) и `apply_random_wound()` (`:2789`) безусловно вызывает `on_hurt( source )` (`:2791`, `source == nullptr` для урона от поля) → при `disturb && uistate.distraction_attack && !is_npc() && !has_effect(effect_narcosis)` (`:2912/2916`) и `source == nullptr` — `g->cancel_activity_or_ignore_query( distraction_type::attacked, "You were hurt!" )` (`:2926`). Обе точки — реальные вызовы из списка UI-функций, оба достижимы **синхронно, внутри одного и того же вызова `destroy_the_carcass()`**, то есть внутри `do_turn` — и оба **после** того, как `destroy_the_carcass` уже безвозвратно уничтожила труп/сгенерировала лут (мутация уже случилась внутри той же функции, до вызова `add_splatter`). Класс — A (живёт внутри `do_turn`, по тому же правилу, что и находки п.7), а не M — но это самая глубокая находка в разделе; см. таблицу и «Что мешает».
9. `butchery_activity_actor::finish()` (`activity_actor.cpp:13339`) — пусто, только `act.set_to_null()`, без UI. `::canceled()` (`:13344`) — только записывает текущий `progress` в переменную предмета; в отличие, например, от `aim_activity_actor::canceled()` из Task 1 (которая при прерывании дозывала `unload_RAS_weapon()` с потенциальным `uilist`), `butchery_activity_actor::canceled()` не вызывает `set_up_butchery` или что-либо ещё из списка UI-функций.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `query_yn` («You see %s nearby!  Start butchering anyway?») | `src/game.cpp:6846` | грep* | P | Проверка соседства враждебного существа — первое, что делает `game::butcher()` после того, как список трупов/предметов уже собран, но до выбора конкретного трупа/предмета и до `assign_activity`. Мутации мира не было. |
| `uilist kmenu` (выбор трупа/предмета для разделки/разборки/переработки, плюс пакетные пункты) | `src/game.cpp:6876` (объявление), `.query()` — `:6932` | грep* | P | Тот же вызов, до какой-либо мутации мира и до `assign_activity`. |
| `uilist smenu` (`butcher_submenu`, выбор `butcher_type`: полная/быстрая разделка, свежевание, потрошение, четвертование, вскрытие) | `src/butchery.cpp:1322` (объявление), `.query()` — `:1418` | грep | P | Вызывается из `game::butcher()` (`game.cpp:6978`/`:7001`) **до** `assign_activity( butchery_activity_actor(...) )` — часть фазы выбора, мутации мира ещё не было. |
| `query_yn` («Really desecrate the mortal remains… others may take issue», сочувствие + NPC рядом) | `src/butchery.cpp:201` | грep | A | `check_anger_empathetic_npcs_with_cannibalism()`, вызывается из `set_up_butchery()` (`butchery.cpp:350` для DISSECT-ветки, `:364` для остальных, кроме DISMEMBER), которая вызывается из `initiate_butchery()` (`activity_actor.cpp:13234`), вызываемой из `do_turn()` на первом ходу обработки трупа. К этому моменту `spill_contents()` (`activity_actor.cpp:13232`) уже отработал. |
| `query_yn` («Really desecrate the mortal remains…», только сочувствие) | `src/butchery.cpp:206` | грep | A | Тот же вызов, соседняя ветка той же функции. |
| `query_yn` («Really take this thing apart?  …others may take issue», только NPC рядом) | `src/butchery.cpp:211` | грep | A | Тот же вызов, третья ветка той же функции. |
| `popup` («You can't butcher this; you are missing some tools.») | `src/butchery.cpp:293` | грep | A | Внутри `set_up_butchery()`, тот же путь вызова, что и три строки выше — `initiate_butchery()` из первого хода `do_turn()`. Проверка `butchery_requirement->can_make_with_inventory(...)` (`:280-281`) детерминирована и опирается только на `crafting_inventory()` и данные трупа/`butcher_type`, уже известные к этому моменту. |
| `cancel_activity_or_ignore_query( distraction_type::pain, "Ouch, something hurts!" )` | `src/character_health.cpp:285` (`Character::react_to_felt_pain`) | косв. | A | Цепочка (проверена по каждому звену, см. п.8 «Путь исполнения»): `destroy_the_carcass()` (`butchery.cpp:1059`, вызов из `do_turn` на `activity_actor.cpp:13323`) → `here.add_splatter(type_blood, …)` (`butchery.cpp:1094`/`:1133`, только когда труп с `mon_flag_ACID_BLOOD`) → `map::add_splatter`→`mod_field_intensity`→`set_field_intensity`→`add_field` (`map.cpp:7397/7127/7146/7272`) → `creature_in_field(player_character)` при `hit_player && p == player.pos_bub()` (`map.cpp:7320-7324`) → `player_in_field`→`burn_body_part` (`map_field.cpp:1466/148`) → `you.deal_damage(nullptr, bp, damage_instance(damage_acid,…))` (`:155-156`) → `Creature::deal_damage` считает боль и вызывает `mod_pain` (`creature.cpp:1514/1529`) → …→ `Character::set_pain` (`character_health.cpp:3389`) → `react_to_felt_pain` (`:3396`) → эта строка. Полностью синхронно, внутри одного вызова `destroy_the_carcass()` из `do_turn`. |
| `cancel_activity_or_ignore_query( distraction_type::attacked, "You were hurt!" )` | `src/character_health.cpp:2926` (`Character::on_hurt`, ветка `source == nullptr`) | косв. | A | Та же цепочка, что и строка выше, до `you.deal_damage(...)` (`map_field.cpp:155-156`) включительно; далее `Character::deal_damage` (`character_health.cpp:2673`) безусловно вызывает `on_hurt(source)` (`:2791`, `source == nullptr` для урона от поля) → при `disturb && uistate.distraction_attack && !is_npc() && !has_effect(effect_narcosis)` (`:2912/2916`) и `source == nullptr` — эта строка (`:2926`). Тоже полностью синхронно, внутри того же вызова `destroy_the_carcass()`. |

Итог по проходам: базовый grep по `butchery.cpp` (дословно из брифа, без фильтрации) даёт **9** совпадений, а не 5: `:48` (`#include "uilist.h"` — не вызов, объявление подключения заголовка), `:201/206/211` (`query_yn`, класс A), `:282` (переменная `popup_output`, не вызов функции `popup`), `:286` (та же переменная, конкатенация строки — не вызов), `:290` (та же переменная — не вызов), `:293` (собственно вызов `popup( popup_output )`, класс A), `:1322` (`uilist smenu`, класс P). Явно отброшены как не-вызовы (совпадение только по подстроке): `:48`, `:282`, `:286`, `:290` — ни один не является обращением к перечисленным функциям. Реальных вызовов среди 9 строк — 5, как и в исходной версии этого раздела: 3×`query_yn` + 1×`popup`, класс A, и 1×`uilist smenu`, класс P. Других совпадений в файле нет (весь файл прочитан целиком, включая тела `butchery_drops_harvest`/`destroy_the_carcass`/`butchery_quarter`).

Файл настоящей точки входа (`game.cpp`, а не `butchery.cpp` брифа) при чтении тела `game::butcher()` даёт ещё 2 совпадения — `query_yn` (`:6846`) и `uilist kmenu` (`:6876`/`:6932`), оба класса P; они отмечены **грep\*** (см. пояснение под «Точкой входа» выше: это не косвенность, а исправление файлового охвата) и посчитаны на равных с базовым проходом по `butchery.cpp`.

Настоящая трассировка на один уровень косвенности вглубь, в функции, вызываемые из пути (не входящие в `butchery.cpp`): тела `do_turn`/`initiate_butchery`/`finish`/`canceled` в `activity_actor.cpp` прочитаны целиком — новых UI-обращений сверх уже учтённого вызова `set_up_butchery()` не найдено. Отдельно, по прямому указанию из code review (предупреждающий комментарий на `activity_actor.cpp:13324-13326`), прослежена **вся** цепочка вызовов `destroy_the_carcass()` — намного глубже одного уровня, до конкретных строк в `map.cpp`, `map_field.cpp`, `creature.cpp` и `character_health.cpp` (см. п.8 и две строки таблицы выше) — она добавила ещё 2 места, оба класса A, отмечены **косв.**

Итого по всем проходам: **0 M**, **6 A** (4 из базового прохода по `butchery.cpp`, 2 — глубокая косвенная трассировка цепочки `destroy_the_carcass`), **3 P** (1 из базового прохода по `butchery.cpp`, 2 — из исправленного охвата на `game.cpp`).

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

    // Предрешает риск самоожога кислотной кровью при уничтожении трупа
    // (destroy_the_carcass -> add_splatter -> ... -> on_hurt/react_to_felt_pain,
    // см. п.8 "Путь исполнения" и две строки таблицы про character_health.cpp:285/2926).
    // corpse.bloodType() == fd_acid — детерминированное свойство мтайпа трупа,
    // известное уже в момент выбора трупа в kmenu/butcher_submenu.
    bool acid_blood_risk_prompted;  // corpse.bloodType() == fd_acid, посчитано в выборе
    bool acid_blood_risk_accepted;  // ответ игрока (значим только если prompted)
};

// Для MULTIBUTCHER (game.cpp:6977) намерение — просто список intent_butcher,
// по одному на труп; do_turn уже и так обрабатывает bd как стек одного трупа
// за раз, так что порядок элементов списка сохраняет текущее поведение.
struct intent_butcher_batch {
    std::vector<intent_butcher> items;
};
```

**Что мешает и как обойти:** Обхода требуют 6 строк класса A: 4 — из `set_up_butchery()`, вызываемой на первом ходу `do_turn()` через `initiate_butchery()` (разобраны первыми двумя абзацами ниже); ещё 2 — из цепочки `destroy_the_carcass()` → …→ `on_hurt`/`react_to_felt_pain` (`character_health.cpp:285/2926`, разобраны третьим абзацем).

Три `query_yn` (`butchery.cpp:201/206/211`) — один смысловой вопрос («продолжить, зная моральную цену?») с текстом, зависящим от двух булевых флагов: `you.empathizes_with_monster(corpse.id)` (черта персонажа, не меняется между выбором и первым ходом активности) и `nearby_empathetic_npc` (скан видимых NPC в момент вызова, `butchery.cpp:190-197`). Обе величины вычислимы в фазе выбора той же функцией, на тех же входных данных, что и сейчас — единственная разница в том, что скан NPC будет выполнен на один игровой тик раньше, чем сегодня. Это тот же класс допущения «мир таков, каким он был на момент отправки намерения», который уже принят конвенцией для остальных предрешённых A-находок в Task 1/2 (в отличие от `can_continue_craft` из Task 2, где решение зависит от результата RNG-броска, случившегося внутри уже идущей активности, и поэтому принципиально не может быть известно заранее). Ремедиум: посчитать оба флага и, если нужно, задать вопрос в фазе выбора; результат — `moral_quandary_prompted`/`moral_quandary_accepted` в намерении; `set_up_butchery()` в `apply_intent` просто читает готовый ответ вместо вызова `query_yn`, и при отказе ведёт себя как сегодня при `false` (труп пропускается, `bd.pop_back()`).

`popup` о нехватке инструментов (`butchery.cpp:293`) проверяет `butchery_requirement->can_make_with_inventory(...)` — тот же `requirement_id`, что вычисляет `calculate_butchery_data()` (`activity_actor.cpp:13211-13214`) из `corpse.harvest`, `corpse.size` и `this_bd.b_type`, и та же `you.crafting_inventory()`, что доступна в фазе выбора. Ремедиум — «заменить на детерминированное правило, вычисленное раньше»: выполнить ту же проверку `can_make_with_inventory` в фазе выбора, до создания намерения; при провале — показать то же сообщение сразу и не создавать `intent_butcher` вовсе (симметрично тому, как уже сегодня `game::butcher()` отказывается открывать `kmenu`, если во всём стеке предметов нет ни одного трупа/подходящего инструмента, `game.cpp:6826-6837`). Полю в намерении это не соответствует — это гейт на отправку намерения, а не данные, нужные `apply_intent`.

Две строки `cancel_activity_or_ignore_query` (`character_health.cpp:285` — `distraction_type::pain`, `:2926` — `distraction_type::attacked`) достижимы, когда у трупа `bloodType() == fd_acid` (т.е. `mon_flag_ACID_BLOOD`, `mtype.cpp:450-453`) и клетка, куда попадёт брызг, совпадает с клеткой игрока. Первое условие детерминировано и известно уже на этапе выбора трупа в `kmenu`/`butcher_submenu`, до создания намерения. Второе — нет: прямой вызов `here.add_splatter(type_blood, corpse_pos, …)` (`:1094`/`:1133`) бьёт именно по `corpse_pos`, но тот же труп ещё и вызывает `here.add_splatter_trail(type_blood, corpse_pos, random_entry(here.points_in_radius(corpse_pos, corpse->size+1)))` (`butchery.cpp:1099`/`:1138`) → `map::add_splatter_trail` (`map.cpp:7416-7428`) прогоняет `add_splatter` по каждой точке случайно выбранной (`random_entry`) линии длиной вплоть до `corpse->size + 1` клеток — то есть труп, лежащий НЕ на клетке игрока, а в нескольких клетках от неё, тоже может обрызгать игрока кислотой, и то, какая именно клетка на линии будет задета, решает RNG в момент вызова, а не что-то известное заранее. На вердикт и ремедиум это не влияет: обход опирается на `bloodType()` (условие «стоит ли вообще спрашивать»), а не на то, по какой конкретно клетке в итоге попадёт брызг — RNG-исход самого попадания не требует своего вопроса игроку, только сам факт «этот тип трупа при разделке может обжечь» решается заранее. Ремедиум: если `corpse.bloodType() == fd_acid`, спросить в фазе выбора («У этого трупа кислотная кровь — разделка может вас обжечь. Продолжить?»), записать `acid_blood_risk_prompted`/`acid_blood_risk_accepted`. Если принято — обернуть вызов `destroy_the_carcass()` внутри `apply_intent`/`do_turn` скобкой из **существующих** методов `player_activity` (`ignore_distraction`/`allow_distractions`/`is_distraction_ignored`, используемых сегодня интерактивно через опцию «IGNORE» в самом `cancel_activity_or_ignore_query`: `game.cpp:1531` — ветка `IGNORE`, `u.activity.ignore_distraction(type)` — `:1532`, распространение на backlog — `:1533-1535`): `act.ignore_distraction(distraction_type::pain); act.ignore_distraction(distraction_type::attacked); destroy_the_carcass(...); act.allow_distractions();` — выставляется детерминированно вместо выбора игрока в моменте, и снимается сразу после вызова (подробнее — следующий абзац). Сам урон/поле кислоты при этом не убирается — убирается только запрос, что и требуется инвариантом.

Уточнение к этому ремедиуму (найдено при повторной проверке — предыдущая формулировка переоценивала стоимость точного обхода): `ignore_distraction(type)` (`player_activity.cpp:479-482`) сам по себе — залипающий флаг до явной отмены, но `player_activity` уже предоставляет и отмену: `allow_distractions()` (`player_activity.cpp:484-487`) одним вызовом полностью очищает набор `ignored_distractions`, а `is_distraction_ignored(type)` (`:474-477`) позволяет проверить его состояние. Это значит, что более точный обход — подавить оба типа отвлечения только на время ОДНОГО вызова `destroy_the_carcass()`, а не на всю оставшуюся активность — не требует изменения общего боевого/средового кода: достаточно локальной скобки внутри `do_turn`/`apply_intent` вокруг этого конкретного вызова — `act.ignore_distraction(distraction_type::pain); act.ignore_distraction(distraction_type::attacked); destroy_the_carcass(...); act.allow_distractions();` — целиком в пределах кода разделки, без правок в `map.cpp`/`character_health.cpp`. Единственная точность, которой эта скобка не даёт: `allow_distractions()` очищает **весь** набор `ignored_distractions`, а не только эти два типа, так что если бы что-то ещё в этой же активности (гипотетически, не сегодня — в текущем `do_turn` разделки больше ничего `ignore_distraction` не вызывает) успело поставить свой собственный игнор между `ignore_distraction` и `allow_distractions()`, он тоже был бы сброшен; для конкретно разделки это не проблема (только эта пара мест ставит игнор), но как общий приём — не идеально универсальная скобка. Простейший вариант (`ignore_distraction` до вызова, без последующего `allow_distractions()`) из первой версии этого раздела был не тем, что реально требуется: правильный, дешёвый и локальный обход — именно скобка `ignore/…/allow`, и он не оставляет протечки сигнала безопасности за пределы одного вызова `destroy_the_carcass()`. Обход **найден, назван и не требует правок вне кода разделки**.

Отдельно, не влияет на вердикт, но стоит отметить: мутация (`spill_contents`, `activity_actor.cpp:13232`) в текущем коде происходит **до** `set_up_butchery()`, то есть до всех четырёх ремедиируемых вопросов — если бы вопросы остались внутри `do_turn`, это уже было бы нарушением независимо от классификации A. Поскольку весь блок предрешается в фазе выбора, `apply_intent` может вызывать `spill_contents()` после того, как решение уже известно, и порядок перестаёт иметь значение для инварианта — но это не единственная причина класть его в намерение: сам факт «живёт внутри `do_turn`» уже требует отдельного разбора по правилу `conventions.md`, независимо от относительного порядка с мутацией.

Не входит в намерение и не разбирается здесь (обосновано в п.4 «Путь исполнения» выше): `MULTISALVAGE`, `BUTCHER_DISASSEMBLE`, `BUTCHER_SALVAGE`, `MULTIDISASSEMBLE_ONE/ALL` — четыре соседних действия из того же начального `kmenu`, не являющиеся разделкой и не входящие в 10 действий MVP-1 (кроме разборки, которая частично пересекается по механизму с Task 2, но не тождественна её точке входа `ACTION_CRAFT`).

## 4. Вождение

**Точка входа:** `src/handle_action.cpp:2947` (`case ACTION_CONTROL_VEHICLE:`) → при отсутствии верховой посадки/черты `WAYFARER` вызывает `control_vehicle( mouse_target )` (`handle_action.cpp:2953`) → `game::control_vehicle` (`src/game.cpp:5123`).

**Путь исполнения:**

1. `game.cpp:5123` `game::control_vehicle()` — если у цели нет машины и нет `p`, пробует `remoteveh()` (дистанционное управление) и открывает `remote_veh->interact_with(...)` (`game.cpp:5130`, определение `vehicle_use.cpp:2992`) — до какой-либо мутации.
2. Иначе ищет `optional_vpart_position` в целевой точке (`game.cpp:5142`), считает `controls_ok`/`reins_ok`. Ветка «уже за рулём» (`veh->player_in_control(here, u)`, `vehicle.cpp:282`, чистая проверка без UI): если `controls_ok` — `veh->interact_with(&here, u.pos_bub())` (`game.cpp:5155`); если только вожжи — снимает `controlling_vehicle` напрямую (`game.cpp:5157-5159`), без UI.
3. Если управление доступно, но машина заперта — `veh->interact_with(&here, target_pos)` (`game.cpp:5166`) и `return` («предлагает хотвайринг», комментарий на `game.cpp:5167`).
4. Иначе — `veh->handle_potential_theft(u)` (`game.cpp:5169`, определение `vehicle.cpp:5104`, сигнатура `vehicle.h:1100`). Эта функция **не является чистой проверкой** (см. `vehicle.cpp:5104-5165` целиком). Реальная логика: (а) `is_owned_by_player` (`vehicle.cpp:5108-5110`) — если истинно, `return true` (`:5112-5114`), без мутации; (б) иначе, если `!has_owner() || ( witnesses.empty() && ( has_old_owner() || you.is_npc() ) )` (`vehicle.cpp:5122`, **точное условие**, исправлено по ревью раунда 2, п.5 — было упрощённо «владелец не задан или свидетелей нет») — код входит во внутренний блок, но мутация внутри него **дополнительно обусловлена**: только если `!has_owner() || ( theft_time && calendar::turn - *theft_time > 15_minutes )` (`vehicle.cpp:5123-5126`) реально выполняются `set_owner`/`remove_old_owner` (`:5127-5128`); если этот внутренний гейт ложен (например, есть текущий владелец и `old_owner`-маркер младше 15 минут), ветка `5122` всё равно возвращает `true` (`:5135`), но **без мутации**. Итог: **не более одной** из двух возможных мутаций (`:5127-5128` либо `:5150-5162` ниже) вообще срабатывает за один вызов, и часто не срабатывает ни одна — «ровно одна из двух» (формулировка раунда 1) исправлена по ревью раунда 2, п.6; (в) если ни (а), ни (б) — доходит до `query_yn` (`vehicle.cpp:5143`) и, если ответ «да», выполняет мутацию `set_old_owner`/злит NPC-свидетелей (`:5150-5162`). Итог для целей этого раздела не меняется: во ВСЕХ случаях, где `handle_potential_theft` возвращает `true` через путь (б) или принятый путь (в), **мир уже мог измениться** к моменту возврата в `control_vehicle()`.
5. Если в руках оружие, конфликтующее с вождением — `query_yn` («…wield … Put it away?», `game.cpp:5179`) либо `query_yn` («…with both hands…», `game.cpp:5189`). **Оба этих вопроса задаются ПОСЛЕ шага 4**, то есть после того, как `handle_potential_theft` уже мог мутировать мир (см. п.4). При отказе — бэйр `return` (`game.cpp:5186` и `game.cpp:5196` соответственно): если мутация кражи произошла, она остаётся в силе, хотя управление игрок не получил. При согласии — `u.unwield()` (`game.cpp:5182`/`game.cpp:5192`) → `Character::unwield()` (`character_inventory.cpp:1529`) → `dispose_item(...)` (вызов `character_inventory.cpp:1547`, определение `character_inventory.cpp:872`) — которая вне `test_mode` (`character_inventory.cpp:874`) показывает **ещё один** `uilist` («куда деть оружие»: убрать в инвентарь/бросить/надеть/в кобуру — `character_inventory.cpp:878`, `.query()` — `:953`) — тоже после возможной мутации кражи. Классификация этого вызова (D или M) — предмет отдельной, явно не решённой самостоятельно оговорки, см. «Политика по неоднозначным случаям» после таблицы.
6. Мутация состояния «за рулём» начинается только здесь: если `veh->engine_on` — `u.controlling_vehicle = true` (`game.cpp:5201`); иначе `veh->start_engines(here, &u, true)` (`game.cpp:5204`, определение `vehicle_use.cpp:816`), которая **ставит активность** `start_engines_activity_actor` (`vehicle_use.cpp:860-861`).
7. Ветка «несколько точек управления рядом, `p` не задан» (`game.cpp:5221-5263`): `choose_adjacent(_("Control vehicle where?"))` (`game.cpp:5240`) — до мутации; затем `handle_potential_theft(u)` (`game.cpp:5259`, та же функция и та же логика, что и в п.4) и **сразу за ней** `veh->interact_with(&here, *vehicle_position)` (`game.cpp:5262`) — открытие общего меню происходит **после** того, как `handle_potential_theft` уже могла (но не обязательно) мутировать мир.
8. `vehicle::interact_with()` (`vehicle_use.cpp:2992`) строит и показывает `veh_menu` в цикле `do { menu.reset(); build_interact_menu(...); } while(menu.query());` (`vehicle_use.cpp:3000-3004`). `veh_menu::query()` (`src/veh_utils.cpp:427`) создаёт `uilist menu;` (`veh_utils.cpp:436`), вызывает `menu.query()` (`:478`), затем при `chosen._check_theft` — `veh.handle_potential_theft(get_player_character())` (`:487`, **третий** вызывающий `vehicle.cpp:5143`, помимо `game.cpp:5169`/`:5259`), затем `chosen._on_submit()` (`:496`), и возвращает `chosen._keep_menu_open` (`:506`) — **если `true`, цикл на `vehicle_use.cpp:3004` заходит на новую итерацию**, заново вызывая и `build_interact_menu`, и `menu.query()` (а внутри нeё — снова потенциально `handle_potential_theft`).
9. **Найдено по ревью раунда 2 (п.2); список сайтов исправлен по ревью раунда 3 (п.1):** в `build_interact_menu` (`vehicle_use.cpp:2303`) ровно **3** пункта вызывают `.keep_menu_open()` и реально относятся к ЭТОМУ меню: включение/выключение оборудования (`vehicle_use.cpp:290`, внутри `add_electronic_toggle`, вызываемой из `build_electronics_menu` через лямбду `add_toggle`) и камера (`:323`, напрямую внутри `build_electronics_menu`) — обе достижимы, поскольку `build_interact_menu` вызывает `build_electronics_menu(*here, menu)` на `:2506`, передавая ей ТОТ ЖЕ объект `menu`; и «Remember/Forget vehicle position» (`:2352-2357`, напрямую внутри `build_interact_menu`). Все три реально мутируют в своём `on_submit`: `e.enabled = state` (`:297`), `camera_on = !camera_on` (`:332`), `toggle_tracking()` (`:2357` → `vehicle_use.cpp:566-576`: `overmap_buffer.add_vehicle`/`remove_vehicle`, флаг `tracking_on`). Ранее список ошибочно включал ещё три сайта `.keep_menu_open()` (`:198`, `:217`, `:418`) — они существуют, но принадлежат `vehicle::control_doors` (`vehicle_use.cpp:153-262`, собственный отдельный `veh_menu` и собственный `do{...}while(menu.query())`) и `vehicle::control_engines` (`:404-460`, тоже отдельный `veh_menu` и цикл) — обе функции открываются как ОТДЕЛЬНЫЕ, вложенные меню при выборе соответствующего пункта в основном меню (`on_submit` на `:314` и `:2495`), а не являются пунктами самого `build_interact_menu`; тот же паттерн, вероятно, повторяется и в них, но это отдельные экземпляры `uilist`/цикла вне охвата этой строки. Итог: на **второй и любой последующей** итерации цикла из п.8 (после того как первая итерация выбрала любой из этих 3 пунктов) — и `uilist` открытия меню, и вложенная проверка кражи на `veh_utils.cpp:487` (если следующий выбранный пункт её не пропускает) выполняются **после** этой мутации, **независимо от того, через какую из четырёх веток (`game.cpp:5130/5155/5166/5262`) был изначально открыт `interact_with()`**.
10. Из пунктов `build_interact_menu`, к состоянию, которое меняет именно `ACTION_CONTROL_VEHICLE` (`controlling_vehicle`/`engine_on`/`is_locked`), относятся: «Hotwire» (`vehicle_use.cpp:2385`, on_submit ставит активность `hotwire_car_activity_actor`, `:2393-2394`), «Stop controlling»/«Stop driving»/«Turn off/on the engine»/«Let go of controls» (`vehicle_use.cpp:2427-2479`). Внутри двух engine-off веток on_submit вызывает `disable_smart_controller_if_needed()` (`vehicle_use.cpp:2449`/`:2461`, определение `vehicle.cpp:6357`) **до** `stop_engines`/`controlling_vehicle=false` в том же on_submit; если опция `FORCE_SMART_CONTROLLER_OFF_ON_ENGINE_STOP == "ask"` (значение по умолчанию — `options.cpp:1642`) — вызывает `query_yn(...)` (`vehicle.cpp:6374`). Эти два пункта используют `.skip_theft_check()` (`vehicle_use.cpp:2429,2440,2457,2476`), поэтому проверка кражи из п.8 для НИХ конкретно не выполняется.
11. `start_engines_activity_actor::do_turn` отсутствует (не переопределён); `::finish()` (`activity_actor.cpp:11993`) детерминированно запускает моторы, без UI. `hotwire_car_activity_actor::do_turn` (`activity_actor.cpp:1213`) и `::finish()` (`activity_actor.cpp:1228`) — тоже без UI. Обе активности прочитаны целиком.
12. **Отдельная точка, вне `control_vehicle()`:** `game::do_turn()` (`src/do_turn.cpp:560-564`), пока `u.in_vehicle && u.controlling_vehicle`. См. «Особый фокус» ниже.

**Другие входы в `interact_with()` за пределами `ACTION_CONTROL_VEHICLE` (восстановлено по ревью раунда 2, п.7 — было ошибочно удалено при правке раунда 1):** `vehicle::interact_with()` — не эксклюзивная точка входа этого действия. Проверено: `game.cpp:5587` (внутри обработчика `ACTION_EXAMINE` для клетки с машиной — `vp->vehicle().interact_with(&here, examp, with_pickup)`) и `iuse.cpp:7499`/`:7501` (использование предмета дистанционного управления — `veh->interact_with(&here, ...)`). Это подтверждает аргумент об исключении шести базовых grep-совпадений (см. ниже): весь `veh_menu`/`build_interact_menu` — общий диспетчер, обслуживающий несколько разных действий, не только вождение.

Также вне пути (не относится к «взять управление», хотя тоже вызывает `handle_potential_theft`): вход в машину через дверь при движении — `avatar_action.cpp:577` (внутри `avatar_action::move`) и `avatar_action.cpp:690` (внутри `avatar_action::swim`); толчок машины мехом — `game.cpp:7705`; движение в клетку с чужой машиной — `game.cpp:7729`. Все четыре достигаются из `ACTION_MOVE_*`/`ACTION_SWIM`, а не из `ACTION_CONTROL_VEHICLE` — как Task 3 исключил соседние пункты `kmenu` разделки, эти вызовы принадлежат другому действию (перемещению).

**Базовый (грep) проход:** дословный `grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/vehicle_use.cpp` даёт **10** строк: `:55` (`#include "uilist.h"` — не вызов), `:928` (`query_int`, внутри `vehicle::reload_seeds`), `:1491` (`uilist detergent_selector`, внутри `vehicle::use_washing_machine`), `:1681` (комментарий — не вызов), `:1685` (`choose_adjacent_highlight`, внутри **`place_carried_furniture`**, статическая функция `vehicle_use.cpp:1659-1696`), `:1704` (`choose_adjacent_highlight`, внутри **`pickup_furniture_for_carry`**, статическая функция `vehicle_use.cpp:1697-1739`), `:1790` и `:1803` (комментарии — не вызовы), `:1804` (`choose_adjacent_highlight`, внутри `vehicle::use_harness`), `:2906` (`game_menus::inv::veh_tool_attach`, внутри `build_interact_menu`, пункт «Attach a tool»). Из 10 строк 4 — не вызовы (`:55,1681,1790,1803`), явно отброшены. Из оставшихся 6 вызовов **ни один не лежит на пути `ACTION_CONTROL_VEHICLE`**: `place_carried_furniture`/`pickup_furniture_for_carry` (обе — статические помощники `vehicle::use_tiedown_furniture`, `vehicle_use.cpp:1740`, упряжь для мебели) и `use_monster_capture` (`vehicle_use.cpp:1642-1657`, у неё самой — ноль обращений к UI из списка) — все реализуют вождению не относящиеся действия того же общего меню `veh_menu` (доступного, как показано выше, и через `ACTION_EXAMINE`, и через предмет дистанционного управления, не только через `control_vehicle()`) — тот же тип исключения, что Task 3 применил к соседним пунктам `kmenu` разделки. Итог базового прохода: **0 P/M/A-строк на пути**, 6 исключены как побочные действия того же меню, 4 — не вызовы.

Все обращения к UI, реально лежащие на пути `ACTION_CONTROL_VEHICLE`, найдены трассировкой — по `game::control_vehicle()`, `vehicle::handle_potential_theft()`, `veh_menu::query()` (включая цикл повторного входа, п.8-9 выше), `Character::unwield()`→`dispose_item()` и `disable_smart_controller_if_needed()`.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `choose_adjacent` («Control vehicle where?») | `src/game.cpp:5240` | косв. | P | До определения `veh` и до `handle_potential_theft` — никакой мутации ещё не было. |
| `query_yn` («This vehicle belongs to: %s… continue?»), прямые вызовы | `src/vehicle.cpp:5143`; вызовы — `game.cpp:5169`, `game.cpp:5259` | косв. | P | Оба вызывающих места — самые первые содержательные действия в своей ветке `control_vehicle()`; предшествуют собственной мутации `handle_potential_theft` (`vehicle.cpp:5127-5128`/`:5150-5162`), задавая вопрос раньше её последствий. |
| `query_yn` (та же строка `vehicle.cpp:5143`), вызов через `veh_menu` | `src/veh_utils.cpp:487` | косв. | **P** (1-я итерация, входы `game.cpp:5130/5155/5166`) / **M** (1-я итерация, вход `game.cpp:5262`; ЛЮБАЯ итерация ≥2, любой вход) | **Расщеплено по ревью раунда 2, п.1** (в раунде 1 ошибочно значилась целиком P). 1-я итерация через `5130`/`5155`/`5166` — ничего не мутировало раньше. 1-я итерация через `5262` — `game.cpp:5259` уже мог мутировать (см. таблицу выше). Итерация ≥2 (после выбора любого из 3 пунктов с `.keep_menu_open()`, действительно принадлежащих этому меню — `vehicle_use.cpp:290,323,2355`, список исправлен по ревью раунда 3; все три реально мутируют) — мутация первой итерации уже произошла, независимо от того, каким из 4 путей была открыта самая первая итерация меню. |
| `uilist` (открытие `veh_menu`) | `src/veh_utils.cpp:436` (объявление), `.query()` — `:478` | косв. | **P** (1-я итерация, входы `5130/5155/5166`) / **M** (1-я итерация, вход `5262`; ЛЮБАЯ итерация ≥2, любой вход) | Тот же механизм и то же расщепление, что и в строке выше (и раунд-1-находка, и находка ревью раунда 2, п.2) — это ОДИН И ТОТ ЖЕ `uilist`, который просто вызывается на каждой итерации цикла `vehicle_use.cpp:3000-3004`. |
| `query_yn` («…have to wield a %s.\n\nPut it away?») | `src/game.cpp:5179` | косв. | **M** | `handle_potential_theft(u)` на `game.cpp:5169` уже вернул `true` и мог уже мутировать мир — этот вопрос задаётся ПОСЛЕ. При отказе — `return` (`game.cpp:5186`): мутация кражи (если была) остаётся применённой, хотя управление не получено. |
| `query_yn` («…wield a %s with both hands…Put it away?») | `src/game.cpp:5189` | косв. | **M** | Тот же механизм, что и в строке выше; отказ — `return` на `game.cpp:5196`. |
| `uilist` («Dispose of %s» — убрать в инвентарь/бросить/надеть/в кобуру) | `src/character_inventory.cpp:878` (объявление), `.query()` — `:953`; вызовы — `character_inventory.cpp:1547` внутри `Character::unwield()` (`:1529`), вызываемого из `game.cpp:5182`/`:5192` | косв. (2 уровня) | **M** (рабочая классификация; см. «Политика по неоднозначным случаям» ниже — вопрос не решён односторонне) | Достигается только после того, как игрок УЖЕ согласился убрать оружие (строка выше), т.е. дальше по тому же пост-мутационному пути. Формально обёрнута в `if( test_mode ) return false;` (`character_inventory.cpp:874`) — дословно совпадает с определением D из `conventions.md`. |
| `query_yn` («Smart controller engine may turn on engine back. Disable it?») | `src/vehicle.cpp:6374` (внутри `disable_smart_controller_if_needed`, вызовы — `vehicle_use.cpp:2449`/`:2461`) | косв. (2 уровня) | P | `.skip_theft_check()` на этих пунктах (`vehicle_use.cpp:2429,2440,2457,2476`) отключает проверку кражи из строки «открытие veh_menu» для НИХ конкретно; задаётся до `stop_engines`/`controlling_vehicle=false` — до собственной мутации этого пункта. Опция по умолчанию — `"ask"` (`options.cpp:1642`). |

Итог по проходам: базовый grep по `vehicle_use.cpp` — 10 строк, 4 не вызовы, 6 вызовов, 0 на пути. Трассировка дала **8 строк таблицы** (исправлено по ревью раунда 3, п.2 — было ошибочно написано «6», хотя перечислялось 7 пунктов, а сама таблица уже тогда содержала 8 строк): `choose_adjacent` (P), theft-warning прямые вызовы (P), theft-warning через `veh_menu` (смешанная P/M), открытие `veh_menu` (смешанная P/M, тот же код что и предыдущая строка по механизму), weapon-hold-один-рукой `query_yn` (M), weapon-hold-обе-руки `query_yn` (M), `dispose_item uilist` (M/рабочая), smart-controller `query_yn` (P). Из них **безусловно P** — 3 строки (`choose_adjacent`, theft-warning прямые вызовы, smart-controller); **безусловно M** — 3 строки (оба weapon-hold `query_yn`, `dispose_item`); **условно P/M в зависимости от контекста вызова** — 2 строки (theft-warning-через-menu, открытие `veh_menu`), каждая из которых становится M в двух отдельных сценариях (вход через `game.cpp:5262`, или любая итерация цикла ≥2). **Итого М-триггерящих сценариев: 3 (безусловные) + 2×2 (условные) = 7**, распределённых по 5 из 8 строк таблицы (было в раунде 1: 4 M из 8 строк — раунд 2 добавил находку по п.1 ревью и переклассифицировал 2 строки из «целиком P» в «P/M смешанные»). **0 A**.

**Особый фокус — `handle_potential_theft` из `game::do_turn`:** `do_turn.cpp:560-564`:
```
if( u.in_vehicle && u.controlling_vehicle ) {
    vehicle *veh = veh_pointer_or_null( m.veh_at( u.pos_bub() ) );
    if( veh && !veh->handle_potential_theft( u, true ) ) {     // do_turn.cpp:562
        veh->handle_potential_theft( u, false, false );        // do_turn.cpp:563
    }
}
```
Guard `if( check_only ) return false;` (`vehicle.cpp:5138`) стоит ПОСЛЕ мутирующего блока `5122-5136` (который сам, как показано в п.4 «Путь исполнения», мутирует не всегда — см. вложенный гейт `:5123-5126`). Поэтому вызов с `check_only=true` (`:562`) может мутировать, если исполнение попадает в ветку `5122` И проходит вложенный гейт `5123-5126`; в этом случае он возвращает `true` РАНЬШЕ, чем вызов на `:563` вообще проверяется — вторая строка в этот ход не выполняется. Вызов на `:563` (`prompt=false`) мутирует только в отдельном сценарии — владелец есть и живые свидетели есть прямо сейчас (та ветка, где `:562` НЕ мутировал, вернув `false` по guard'у `check_only`). **Не более одной** из двух строк мутирует в конкретный ход (исправлено по ревью раунда 2, п.6 — было «ровно одна из двух»; возможно и такое, что НИ ОДНА не мутирует — например, `is_owned_by_player` истинно, или ветка `5122` пройдена без прохождения вложенного гейта `5123-5126`). Вывод «это не обращение к UI» не меняется: ни один из двух вызовов не доходит до `query_yn` — оба детерминированы, без диалога, при любом исходе.

Что произойдёт при двух игроках в чужой машине (вопрос брифа): весь механизм жёстко завязан на единственный `avatar &u = get_avatar()`, и в `do_turn.cpp:560`, и внутри `handle_potential_theft` (`get_avatar()` на `vehicle.cpp:5109`). Пассажир-человек не проверяется вообще (проверяется только `controlling_vehicle`); свидетель-человек не получает ни уведомления, ни собственного вопроса (`if( elem->is_npc() )` на `vehicle.cpp:5153` пропускает не-NPC). Для двух одновременно управляющих людей `do_turn()` как синглтон-функция от одного `u` структурно не имеет способа опросить обоих — нужен перебор по всем управляющим персонажам, а не чтение одного глобального `u`.

**Политика по неоднозначным случаям (введена по ревью раунда 2, п.8):** в этом разделе встретились два места, где буквальный текст `conventions.md`/`controller-addenda.md` не даёт однозначного ответа: (1) классификация `dispose_item` (D по букве определения — обёрнута в `if(test_mode)`, — либо M по разумному прочтению, поскольку `test_mode` в реальной игре всегда `false`, и это защита тестов от зависания на интерактивном вводе, а не признак необязательности функции); (2) вердикт при «M есть, но обход найден для всех M» (ни один из трёх вердиктов не описан буквально для этого случая, см. ниже). **Принятая политика: флагировать оба конфликта для решающего (Task 11), а не разрешать их самостоятельно.** Для (1) в таблице выше и в наброске намерения используется **рабочая классификация M** (чтобы таблица и intent-набросок были пригодны к использованию), но это явно помечено как невынесенное решение, а не как факт: при буквальном чтении `conventions.md` эта строка — D, и это единственная строка в разделе, где применимость D vs M реально спорна (все прочие обёрнутые-в-`test_mode` конструкции в этом разделе не встречались). Для (2) — см. ниже, вердикт выбран, но явно помечен как вынужденный выбор при неполном правиле, а не как механический вывод.

**Вердикт:** не режется

**Как получен этот вердикт (переписано по ревью раунда 2, п.3 — раунд-1 формулировка была неточной и неверно ссылалась на Task 2; диапазон исправлен по ревью раунда 3, п.3 — было ошибочно «от 3 до 7»):** таблица содержит от **6 до 7** M-триггерящих сценариев в зависимости от того, считать ли `dispose_item` как M или D (см. «Политика» выше) — это единственная переменная величина, а не «3 vs 7». Независимо от этой политики, 4 сценария M всегда присутствуют условно (открытие `veh_menu` и вложенная проверка кражи через него, каждая — в двух отдельных сценариях: вход через `game.cpp:5262`, либо любая итерация цикла ≥2, см. «Итог по проходам» выше), а 2 (weapon-hold ×2) — всегда безусловно; `dispose_item`, если считать его M, добавляет третий безусловный сценарий (2→3), доводя итог до 7; если считать его D — итог 6. И 0 A-строк. Ни одно из трёх определений `conventions.md` не описывает эту таблицу буквально и однозначно:
- «режется» (все P/D) — не подходит независимо от трактовки `dispose_item`: даже при политике «D» для неё остаются 6 M-сценариев (weapon-hold ×2 плюс 4 условных от `veh_menu`), так что «режется» исключено в любом случае.
- «режется с оговоркой» (есть A, ни одного M, у каждого A назван обход) — не подходит буквально по ДВУМ независимым причинам: (i) M-строк больше нуля независимо от политики по `dispose_item`; (ii) A-строк нет вообще, а формулировка правила требует «есть A» — то есть даже если бы M было ровно ноль, это по-прежнему не тот случай, который правило описывает (правило описывает «удалось изолировать все нарушения внутри активностей», а не «нарушений не осталось вовсе»).
- «не режется» (есть M, для которого не найден способ переноса) — тоже не подходит буквально: обходы найдены и названы для всех обнаруженных M-строк (см. ниже), значит по букве правила спусковой крючок «обход не найден» не сработал.

**Это значит, что верного, буквально подходящего варианта из трёх нет — выбор вердикта здесь principled, но не механический.** Раунд 1 ошибочно утверждал, что «не режется» получается «методом исключения... механически», и ошибочно ссылался на Task 2 как на прецедент ТАКОГО ЖЕ рассуждения. Это неверно: Task 2 **не** обосновывал свой вердикт «не режется» через этот самый разрыв правил. У Task 2 была СВОЯ M-находка с обходом (`place_craft_or_disassembly`, `crafting.cpp:961`) и она честно зафиксировала тот же разрыв («случай «M есть, обход найден» дословной таблицей правил не покрыт»), но явно написала: «Вердикт этого раздела опирается не на эту находку, а на следующую» — и оперла итоговый вердикт на СОВЕРШЕННО ОТДЕЛЬНУЮ, безусловно попадающую под правило находку: `can_continue_craft` (`crafting.cpp:2649/2659`), у которой обхода принципиально нет (решение зависит от исхода RNG-броска, случившегося уже внутри активности, а не от чего-то известного заранее). У Task 4 такого отдельного, чисто-M-без-обхода якоря **нет** — все 6-7 найденных M-сценариев в этом разделе имеют названные обходы (см. ниже). Поэтому, в отличие от Task 2, вердикт этого раздела **целиком** держится на неразрешённом разрыве правил, а не подкреплён отдельной безусловной находкой.

Согласно принятой политике «флагировать, не решать самостоятельно» (см. выше), правильным было бы просто зафиксировать это как открытый вопрос для Task 11 без выбора конкретной буквы. Но поскольку задание требует «ровно одну из трёх строк, без хеджирования», делается вынужденный выбор: **не режется**, а не «режется с оговоркой», по следующей, явно интерпретационной (не буквальной) причине: `controller-addenda.md` (A3) формулирует условие «режется с оговоркой» через «ноль M» с явным повтором-усилением («requires zero M-class calls… If even one A row has no remedy… не режется»), и нигде не даёт того же снисхождения M-строкам, которое даёт A-строкам («обход найден» → не блокирует). Асимметрия читается как намеренная: A-находки по конструкции уже локализованы внутри управляемого, предсказуемого контекста (многоходовая активность) — этот шаблон «предрешить в фазе выбора, activity просто читает готовый ответ» уже 10 раз подтверждён работающим в Task 1. M-находки возникают СИНХРОННО, внутри одной непрерывной функции, где сегодняшний код уже смешал вопрос и мутацию в одном порядке — устранение части из них (`veh_menu` повторный вход, находка ревью раунда 2 п.2) требует не просто «спросить заранее», а забрать ЦЕЛУЮ интерактивную сессию меню в фазу выбора целиком, что структурно тяжелее, чем предрешить один булев ответ активности. Это делает «не режется» более консервативным (и потому более безопасным для решения по воротам) выбором в рамках сегодняшнего текста правила. **Исправлено по ревью раунда 3, п.4:** предыдущая версия этого абзаца ошибочно утверждала, что «другой решающий был бы вправе выбрать «режется с оговоркой»» — это прямо противоречит только что процитированному тексту A3, который требует **ноля** M-строк без исключений, а в этой таблице M-строк больше нуля при любой трактовке `dispose_item`. «Режется с оговоркой» сегодня закрыт буквальным текстом A3, а не является вторым равноправным вариантом на усмотрение решающего — открытый вопрос для Task 11 не «какую из трёх букв выбрать сейчас» (только «не режется» буквально совместим с M>0), а «стоит ли вообще пересматривать формулировку A3» (например, дать M-находкам с конкретным, не зависящим от RNG обходом то же снисхождение, что уже даётся A-находкам) — это решение о ПРАВИЛЕ, а не о том, как применить сегодняшнее правило к этой таблице.

**Набросок намерения:**

```cpp
// Основной случай ACTION_CONTROL_VEHICLE. Обходы для всех M-находок вынесены в поля
// ниже: предрешить И вопрос о краже, И вопрос про оружие, И способ уборки оружия —
// в фазе выбора, ДО того как apply_intent вообще начнёт что-либо мутировать. Порядок
// вопросов в фазе выбора не важен (это чистый UI без мутации); важен только порядок
// МУТАЦИЙ внутри apply_intent, который теперь развязан от порядка вопросов в
// сегодняшнем синхронном коде.
struct intent_control_vehicle {
    tripoint_bub_ms target_pos;         // точка с органами управления (choose_adjacent,
                                         // game.cpp:5240, класс P — без изменений)
    bool theft_prompted;                // был ли вообще вопрос о краже (условие
                                         // vehicle.cpp:5108-5148 целиком, не просто "владелец/
                                         // свидетели" — см. п.4 "Путь исполнения")
    bool theft_accepted;                // ответ на query_yn (vehicle.cpp:5143); значим только
                                         // если theft_prompted
    bool weapon_stow_prompted;          // нужно ли было спрашивать про оружие
                                         // (has_two_arms_lifting()/is_two_handed(), game.cpp:5178/5188)
    bool weapon_stow_accepted;          // ответ на один из двух query_yn (game.cpp:5179/5189)

    // dispose_item (character_inventory.cpp:878/953) — куда деть снятое оружие.
    enum class weapon_dispose_method { not_applicable, stash, drop, wear, holster };
    weapon_dispose_method dispose_method;
    item_location holster_target;           // ВСЕГДА сам надетый холстер `e`
                                             // (character_inventory.cpp:828-869), НИКОГДА
                                             // прикреплённый под-карман `it` напрямую — см. поле
                                             // ниже, исправлено по ревью раунда 3, п.5 (было
                                             // ошибочно "e ИЛИ it", что противоречило описанию
                                             // holster_pocket_index как индекса ВНУТРИ holster_target)

    // ДОБАВЛЕНО по ревью раунда 2, п.4, уточнено по ревью раунда 3, п.5: item_location одного
    // holster_target недостаточно, когда нужно выбрать конкретный прикреплённый под-карман
    // (`it`, character_inventory.cpp:841-854, каждый — отдельный пункт меню внутри цикла по
    // e.get_contents().get_added_pockets()) — реальная мутация в этом случае идёт через отдельную
    // перегрузку Character::store(item_pocket*, ...) (character_inventory.cpp:3082), а не через
    // item_location-перегрузку store(item&, item&, ...), используемую для случая "положить в сам
    // холстер целиком" (:831-839). item_pocket* — сырой указатель, не переживает сериализацию;
    // нужен стабильный суррогат — индекс кармана внутри holster_target (позиция в
    // e.get_contents().get_added_pockets(), :841, где e == holster_target).
    int holster_pocket_index = -1;          // -1 = "холстер целиком" (:831-839, доп. индекс не
                                             // нужен); >=0 = индекс конкретного прикреплённого
                                             // под-кармана (:846-854), с которым apply_intent
                                             // обязан восстановить тот же item_pocket*, что и
                                             // choose-фаза видела, прежде чем звать store(pocket, ...)
};

// Пункты общего veh_menu, меняющие именно "за рулём"-состояние. Отдельные намерения,
// не варианты intent_control_vehicle — veh_menu общий диспетчер, обслуживающий несколько
// действий (ACTION_CONTROL_VEHICLE, ACTION_EXAMINE, предмет дистанционного управления —
// см. "Другие входы" выше); эти два — единственные его relevant пункты для вождения.
struct intent_release_vehicle_controls {
    bool smart_controller_disable_prompted; // FORCE_SMART_CONTROLLER_OFF_ON_ENGINE_STOP == "ask"
                                             // (дефолт — именно "ask", options.cpp:1642) и есть
                                             // enabled smart controller без доступного
                                             // электромотора (vehicle.cpp:6357-6378)
    bool smart_controller_disable_accepted; // ответ на query_yn (vehicle.cpp:6374)
};

struct intent_hotwire_vehicle {
    tripoint_abs_ms target;             // pos_abs() + coord_translate(parts[0].mount),
                                         // vehicle_use.cpp:2392
};

// При достижении interact_with() через ветку "несколько точек управления рядом"
// (game.cpp:5262) ИЛИ при повторном заходе цикла veh_menu после мутирующего пункта
// с .keep_menu_open() (vehicle_use.cpp:290,323,2355 — реальные сайты этого меню,
// см. п.9 "Пути исполнения") — theft_prompted/
// theft_accepted для ЭТОЙ конкретной итерации нужно нести в конечное намерение,
// а не молча мутировать владение до появления/повторного показа меню, как сегодня.
struct intent_theft_predetermination {
    bool theft_prompted;
    bool theft_accepted;
};
```

Поля с суффиксом `_prompted` нужны, чтобы `apply_intent` отличал «диалога не было» от «диалог был и получил отказ» — оба случая иначе дали бы одинаковый `false` у `_accepted`.

**Что мешает и как обойти:** обход есть для каждой из найденных M-строк, и он один и тот же по форме — переставить порядок так, чтобы ВСЕ решения принимались в фазе выбора, ДО того как исполнение вообще коснётся мутации владения:

1. **`game.cpp:5179`/`:5189` (вопрос про оружие после мутации кражи).** Условия для вопроса детерминированы состоянием персонажа/оружия на момент выбора и не зависят от исхода проверки кражи. Обход: задать ОБА вопроса в фазе выбора, записать оба ответа в `intent_control_vehicle`; `apply_intent` мутирует один раз, без вопросов.
2. **`character_inventory.cpp:878/953` (`dispose_item`).** Тоже детерминировано состоянием на момент выбора; обход — выбрать способ и, при необходимости, карман-под-карман в фазе выбора, записать в `dispose_method`/`holster_target`/`holster_pocket_index`.
3. **Открытие/повторное открытие `veh_menu` (`game.cpp:5262`, ЛИБО любая итерация ≥2 после `.keep_menu_open()`-пункта).** Обход: вся интерактивная сессия меню (включая повторные проходы после toggle-пунктов) выполняется целиком на клиенте в фазе выбора (аналогично тому, как Task 1 перенёс весь `target_ui::run()`) — `apply_intent` получает уже готовое конечное намерение (`intent_release_vehicle_controls`/`intent_hotwire_vehicle`/иное для не относящихся к вождению пунктов) плюс предрешённые `theft_prompted`/`theft_accepted` для каждого шага сессии, которые требовали проверки кражи; сами toggle-мутации (`toggle_tracking` и подобные) применяются одним пакетом решений, а не переспрашиваются на каждой итерации.

Остаточный риск, не блокирующий вердикт сам по себе (аналогично Task 1/3): к моменту, когда `apply_intent` реально применяет `dispose_method`/`holster_pocket_index`, инвентарь/надетые вещи могли измениться — общий вопрос валидации устаревших намерений, не специфика вождения.

---

### Отчёт об исправлениях к разделу 4 — раунд 1 (по итогам первого ревью)

Ревью нашло 7 проблем; все проверены заново чтением кода перед правкой.

1. **`game.cpp:5179`/`:5189` — P → M.** Подтверждено: `handle_potential_theft(u)` на `game.cpp:5169` мутирует мир ДО возврата в вызывающий код; оба вопроса про оружие переклассифицированы в M.
2. **Пропущенная строка — добавлена.** `u.unwield()` → `Character::unwield` → `dispose_item` → `uilist`/`query()`. Классифицирована M (пересмотрено в раунде 2, см. ниже — вопрос D-vs-M теперь явно флагируется, а не решается односторонне).
3. **`do_turn.cpp:562`/`:563` — атрибуция мутации переставлена.** Guard `check_only` стоит после мутирующего блока; вызов `:562` тоже может мутировать (дальше уточнено в раунде 2, п.5-6).
4. **Опция по умолчанию — `"ask"`, не «не ask».** Проверено `options.cpp:1630-1642`.
5. **Неверная атрибуция функции — исправлена.** `vehicle_use.cpp:1685`/`:1704` — внутри `place_carried_furniture`/`pickup_furniture_for_carry`, не `use_monster_capture`.
6. **Счёт базового прохода — исправлен.** 10 строк, а не 6; 4 не вызовы явно перечислены.
7. **Пропущенный вызывающий — добавлен.** `veh_utils.cpp:487` — третий вызывающий `vehicle.cpp:5143` (классификация этой конкретной строки далее уточнена в раунде 2, п.1 — оказалось, что она сама неоднородна по P/M).

Дополнительно (самостоятельно, раунд 1): открытие `veh_menu` через `game.cpp:5262` реклассифицировано в M (тот же механизм, что и находка №1 ревью).

**Итог раунда 1:** было «0 M, 0 A, 5 P», стало «4 M, 0 A, 4 P», вердикт **не режется**.

### Отчёт об исправлениях к разделу 4 — раунд 2 (по итогам повторного ревью)

Повторное ревью подтвердило вердикт «не режется», но нашло внутренние несостыковки и новые находки в раунд-1-таблице; все проверены заново чтением кода перед правкой.

1. **Внутренняя несостыковка — исправлена.** Строка про открытие `veh_menu` была расщеплена P (3 входа) / M (вход через `:5262`), но родственная строка — theft `query_yn` через `veh_utils.cpp:487` — по ошибке осталась целиком P, хотя тот же самый вход через `game.cpp:5262` (после `handle_potential_theft` на `:5259`) делает её M по идентичной логике. Расщеплена так же: P для входов `5130/5155/5166`, M для входа `5262`.
2. **Пропущенная M-находка — повторный вход в цикл меню.** `veh_menu::query()` (`veh_utils.cpp:506`) возвращает `chosen._keep_menu_open`; в `build_interact_menu` 3 пункта вызывают `.keep_menu_open()` (`vehicle_use.cpp:290,323,2355`), и все три мутируют (`e.enabled=state` `:297`, `camera_on=!camera_on` `:332`, `toggle_tracking()` `:2357`→`:566-576`). Цикл `do {...} while(menu.query());` (`vehicle_use.cpp:3000-3004`) поэтому на итерации ≥2 показывает `uilist` и потенциально проверяет кражу (`veh_utils.cpp:487`) уже ПОСЛЕ мутации предыдущей итерации — независимо от того, через какую из 4 веток был открыт `interact_with()` изначально. Добавлено как расширение классификации обеих затронутых строк (не как отдельные новые строки — выбран более компактный вариант, разрешённый условием ревью). **Список сайтов исправлен по ревью раунда 3** — см. раздел «раунд 3» ниже; исходный список ошибочно включал 3 сайта из отдельных вложенных меню (`control_doors`, `control_engines`).
3. **Формулировка вывода вердикта переписана.** Раунд 1 утверждал, что «не режется» получается «механически методом исключения», и ссылался на Task 2 как на прецедент такого же рассуждения. Обе части неточны: (а) буквально ни одно из трёх определений `conventions.md` не описывает таблицу с M>0 и A=0, где у всех M есть обход, — выбор вердикта в этой ситуации не механический, а вынужденный/интерпретационный, и текст теперь прямо это признаёт; (б) Task 2 не обосновывал свой вердикт этим же разрывом — она зафиксировала АНАЛОГИЧНЫЙ разрыв для своей собственной M-находки, но явно оперла итоговый вердикт «не режется» на ОТДЕЛЬНУЮ, безусловно покрытую правилом находку (`can_continue_craft`, обхода принципиально нет из-за зависимости от исхода RNG внутри активности) — цитата исправлена.
4. **Тип поля `holster_target` уточнён.** Проверено `character_inventory.cpp:825-869`: для холстера с прикреплёнными под-карманами (`e.get_contents().has_additional_pockets()`) генерируются отдельные пункты меню на каждый под-карман (`:846-854`), и их фактическая мутация идёт через `Character::store(item_pocket*, ...)` (`:3082`), а не через `item_location`-перегрузку. `item_location` один не может отличить «холстер целиком» от «конкретный под-карман внутри него» для целей воспроизведения этого именно вызова. Добавлено поле `holster_pocket_index` (индекс в `get_added_pockets()`, `-1` = «холстер целиком»).
5. **Условие `vehicle.cpp:5122` изложено неточно — исправлено.** Было «владелец не задан или свидетелей сейчас нет»; реальное условие — `!has_owner() || ( witnesses.empty() && ( has_old_owner() || you.is_npc() ) )`, и вход в эту ветку НЕ гарантирует мутацию — есть дополнительный гейт `:5123-5126` (`!has_owner() || theft_time > 15_minutes`), без прохождения которого функция возвращает `true` без единой мутации. Текст пути исполнения и «Особого фокуса» переписаны с точным условием и без ложного «вход в ветку = мутация».
6. **«Ровно одна из двух» → «не более одной».** И `is_owned_by_player` (`vehicle.cpp:5112`), и путь через `5122` без прохождения `5123-5126`, не мутируют ничего — так что возможен ход, когда НИ ОДИН из двух вызовов `do_turn` не мутирует. Исправлено везде, где встречалась старая формулировка.
7. **Удалённые в раунде 1 сведения восстановлены.** Правка раунда 1 случайно убрала упоминание о том, что `interact_with()` вызывается и из `ACTION_EXAMINE` (`game.cpp:5587`), и из предмета дистанционного управления (`iuse.cpp:7499`/`:7501`) — оба указателя перепроверены (`sed -n`) и восстановлены отдельным абзацем сразу после «Путь исполнения», подпирающим аргумент об исключении шести базовых grep-совпадений как побочных действий общего меню.
8. **Политика по неоднозначным случаям — явно объявлена.** Раунд 1 без объявления политики классифицировал `dispose_item` как M вопреки буквальному совпадению с определением D (`if(test_mode) return false;`), но ОТДЕЛЬНО отказался самостоятельно разрешать разрыв правил для вердикта М+обход, просто зафиксировав его. Это было непоследовательно. Принятая теперь единая политика: **флагировать оба конфликта для решающего (Task 11), не разрешать самостоятельно** — для `dispose_item` в таблице используется рабочая классификация M (нужна для наброска намерения), но явно помечена как нерешённый вопрос, а не факт; для вердикта — сделан вынужденный выбор «не режется» с явным указанием, что «режется с оговоркой» тоже был бы защитим при другой (тоже интерпретационной) трактовке того же текста правила, и что это не механический вывод.

**Итог раунда 2:** таблица теперь содержит от 3 (безусловных) до 7 (считая условные P/M-сценарии) M-триггерящих мест вместо 4 в раунде 1; вердикт остаётся **не режется**, но обоснование переписано как явно интерпретационный, а не механический выбор, с прямо названной альтернативой («режется с оговоркой») и явно скорректированной ссылкой на прецедент Task 2. **(Обе выделенные формулировки — «3 безусловных» и «альтернатива режется с оговоркой» — сами оказались неточны; исправлены ниже, раунд 3, п.3 и п.4.)**

### Отчёт об исправлениях к разделу 4 — раунд 3 (по итогам третьего ревью)

Третье ревью подтвердило, что вердикт «не режется» стабилен и все 8 находок первых двух раундов адресованы, но нашло 5 новых ошибок — все в прозе, добавленной в раунде 2; ни одна не меняет вердикт. Все проверены заново чтением кода перед правкой.

1. **Неверная атрибуция сайтов `.keep_menu_open()` — исправлена.** Список «6 пунктов (`:198,217,290,323,418,2355`)» был неверен: `:198` и `:217` принадлежат `vehicle::control_doors` (`vehicle_use.cpp:153-262`, проверено — собственный `veh_menu menu(...)` на своей строке и собственный `do{...}while(menu.query())`, заканчивающийся на `:262`), а `:418` — `vehicle::control_engines` (`:404-460`, тот же паттерн). Обе функции — ОТДЕЛЬНЫЕ, вложенные `veh_menu`-сессии, открываемые как `on_submit` пунктов ВНУТРИ `build_interact_menu` (`control_doors(here)` на `:314`, `control_engines(*here)` на `:2495`), а не пункты самого `build_interact_menu`. Реальных сайтов — **3**: `:290` (внутри `add_electronic_toggle`, вызываемой из `build_electronics_menu` через лямбду) и `:323` (напрямую внутри `build_electronics_menu`) — обе достижимы через вызов `build_electronics_menu(*here, menu)` на `:2506` из `build_interact_menu`; и `:2355` (напрямую внутри `build_interact_menu`). Все три подтверждённо мутируют: `e.enabled = state` (`:297`), `camera_on = !camera_on` (`:332`), `toggle_tracking()` (уже было проверено в раунде 2). Список исправлен во всех обнаруженных вхождениях (путь исполнения п.9, таблица — 2 строки, набросок намерения, собственный текст раунда-2-отчёта выше); хедж-фраза «5 остальных не проверены построчно» убрана, так как все 3 реальных сайта теперь подтверждённо мутируют, хеджировать больше нечего.
2. **Арифметическая ошибка в счёте — исправлена.** Было «Трассировка дала 6 отдельных кодовых мест», хотя перечислялось 7 пунктов текста, а сама таблица содержит (и содержала уже тогда) 8 строк. Исправлено на «8 строк таблицы» с явным перечислением всех восьми.
3. **Перепутанная причина диапазона — исправлена.** Было «от 3 до 7 M-триггерящих сценариев (в зависимости от того, считать ли `dispose_item` как M или D)» — неверно: политика по `dispose_item` двигает счёт **7→6** (не 3→7), что уже подтверждалось соседним предложением («даже при политике D... остаются 6»). Ось «3 vs 7» — отдельная, не связанная с `dispose_item`: 2-3 безусловных места (weapon-hold ×2, плюс `dispose_item`, если M) против 4 дополнительных условных сценариев от `veh_menu`. Текст переписан: диапазон явно назван «6 до 7», вызванный ТОЛЬКО политикой по `dispose_item`; ось «безусловные vs условные» объяснена отдельно и корректно.
4. **Противоречие с A3 — исправлено.** Финальное предложение обоснования вердикта утверждало, что «другой решающий был бы вправе выбрать «режется с оговоркой»» — это прямо противоречит процитированному двумя предложениями раньше тексту A3, который требует **ноля** M-строк без исключений для этого вердикта; в этой таблице M больше нуля при любой политике по `dispose_item`. «Режется с оговоркой» сегодня закрыт буквальным текстом A3, а не является вторым равноправным вариантом. Предложение переписано: открытый вопрос для Task 11 — не «какую из трёх букв выбрать» (только «не режется» буквально совместимо с M>0), а «стоит ли пересматривать саму формулировку A3» — решение о правиле, а не о применении правила к этой таблице.
5. **Устаревшее описание поля — исправлено.** `holster_target` был описан как «сам холстер `e`, ИЛИ конкретный под-карман `it`», что противоречило описанию `holster_pocket_index` как «индекс кармана ВНУТРИ `holster_target`» (это работает только если `holster_target` всегда `e`, а не иногда `it`). Описание `holster_target` исправлено: он ВСЕГДА `e`; выбор конкретного под-кармана внутри него делает `holster_pocket_index`.

**Итог раунда 3:** классификация P/M и вердикт «не режется» не изменились — все 5 находок этого раунда исправляют прозу (списки сайтов, арифметику суммирования, формулировку про A3, описание поля намерения), а не переоткрывают классификацию обращений.

## 5. Строительство

**Точка входа:** `src/handle_action.cpp:2935` (`case ACTION_CONSTRUCT:`) → при `!player_character.in_vehicle` вызывает `construction_menu( false )` (`handle_action.cpp:2939`, определение — `src/construction.cpp:521`).

**Путь исполнения:**

1. `construction.cpp:521` `construction_menu()` — целиком UI: список конструкций, фильтр, превью, выбор пункта. По `CONFIRM` (`:1109`), если `player_can_build(...)` (`:1191`) и пройдены проверки видимости/территории (см. п.2 ниже) — вызывает `place_construction( { constructs[select] } )` (`:1131`) и завершает меню (`exit = true`, `:1133`). Мутации мира в самом `construction_menu()` нет.
2. Внутри ветки `CONFIRM`, до `place_construction`, — `g->warn_player_maybe_anger_local_faction( true )` (`construction.cpp:1124`). Это не проверка без побочных эффектов: при отказе игрока продолжать функция сама портит отношения с фракцией (`game.cpp:5729-5731`) — но это происходит строго до `place_construction`, то есть до собственной мутации строительства. См. таблицу и отдельное примечание после неё.
3. `place_construction()` (`construction.cpp:1357`) — `choose_adjacent( "Construct where?", ... )` (`:1385`) выбирает точку, всё ещё без мутации. Затем цикл по группам компонентов (`:1438-1445`) вызывает `player_character.consume_items( it, 1, is_crafting_component, return_false<itype_id>, true )` (`:1439`) для каждой требуемой группы материалов — этот вызов, если у игрока и рядом на карте одновременно есть подходящий материал, сам открывает `uilist` (см. п.4). Затем `here.partial_con_set( pnt, pc )` (`:1448`) — это первая безусловная мутация мира в этой активности (запись частичной конструкции на тайл). Сразу после неё — цикл по группам инструментов (`:1449-1451`), вызывающий `player_character.consume_tools( it )` (`:1450`), который тоже может открыть `uilist` (см. п.4) — уже гарантированно после `partial_con_set`. Далее — инвалидация кэшей и `player_character.assign_activity( build_construction_activity_actor(...) )` (`:1456`). **Фаза выбора в сегодняшнем коде формально не заканчивается на входе в `place_construction()` — часть UI из списка находится ВНУТРИ неё, вперемешку с первыми мутациями.**
4. Индирекция (см. таблицу): `consume_items( const std::vector<item_comp>&, ... )` (`crafting.cpp:3327`) вызывает `select_item_component()` (`crafting.cpp:2923`), которая при неоднозначности источника (и на карте, и у игрока есть подходящий материал) открывает `uilist cmenu` («Use which component?», объявление `crafting.cpp:3052`, `.query()` — `:3162`); аналогично `consume_tools( const std::vector<tool_comp>&, ... )` (`crafting.cpp:3938`) вызывает `select_tool_component()` (`crafting.cpp:3358`), которая при неоднозначности источника инструмента открывает `uilist tmenu` («Use which tool?», объявление `crafting.cpp:3417`, `.query()` — `:3460`).
5. `build_construction_activity_actor::do_turn()` (`activity_actor.cpp:8899`) — вызывается движком активности каждый ход. Каждый ход: `pc->id->do_turn_special( construction_bub, you )` (`:8944`) — вызывает JSON-заданную per-turn функцию конкретной конструкции (`do_turn_shovel`/`do_turn_deconstruct`/`do_turn_exhume`/пустышку `check_nothing`-аналог, в зависимости от `post_special`/`pre_special` в данных). Когда счётчик прогресса достигает 100% — `complete_construction( act, you )` (`:8951`), тоже вызывается прямо из `do_turn()`. Отдельного `finish()` у `build_construction_activity_actor` **нет** — завершение целиком происходит внутри `do_turn()`.
6. `complete_construction()` (`construction.cpp:1457`) — начисляет опыт, раздаёт опыт помощникам, убирает `partial_con`, перемещает предметы с тайла, меняет террейн/фёрнитуру, спавнит побочные продукты, чистит активность (`act.set_to_null()`, `:1543`) — и **после** этого вызывает все `built.post_specials` (`:1548`) — это и есть `done_*`-функции (`done_vehicle`, `done_wiring`, `done_grave`, `done_digormine_stair` и т.д.), заданные в JSON конструкции. Если в `backlog` уже стоял `ACT_MULTIPLE_CONSTRUCTION` — назначает `multi_build_construction_activity_actor()` (`:1550-1552`), тоже из `do_turn()`.
7. `multi_build_construction_activity_actor` — отдельная, зонная активность «построить всё в зоне»; её `multi_activity_do()` (`activity_item_handling.cpp:3823`) при наличии частичной конструкции на тайле просто переставляет `build_construction_activity_actor` (`:3837`), иначе вызывает статический хелпер `construction_activity()` (`activity_item_handling.cpp:3161`), который дублирует логику потребления компонентов/инструментов из `place_construction()` (включая те же `consume_items`/`consume_tools`, то есть тот же потенциальный `uilist`) и тоже завершается `assign_activity( build_construction_activity_actor(...) )` (`:3194`). Файл прочитан целиком — новых UI-функций из списка нет. Эта активность назначается только через отдельный, зонный вход (не через `ACTION_CONSTRUCT`), кроме продолжения через `backlog` в п.6.

**Отдельно — возобновление прерванного строительства (по прямому указанию брифа):** обнаружение незаконченной конструкции на тайле обрабатывается не активностью, а `iexamine::part_con()` (`src/iexamine.cpp:4776`), доступной через `ACTION_EXAMINE` — отдельную точку входа, не через `ACTION_CONSTRUCT`. Внутри — `query_yn( "Unfinished task: %s, %d%% complete here, continue construction?" )` (`iexamine.cpp:4787`) и, при отказе, `query_yn( "Cancel construction?" )` (`:4789`). Оба вызова происходят **до** повторного `you.assign_activity( build_construction_activity_actor(...) )` (`:4794`) — то есть до какой-либо мутации в рамках этого конкретного возобновления. Внутри самой активности (`do_turn`, п.5 выше) при возобновлении ничего заново не спрашивается — `do_turn()` просто продолжает считать прогресс с сохранённого `pc->counter`. Значит для действия «продолжить прерванную конструкцию» нужна собственная пара «выбор/исполнение» (выбор = ответ на эти два `query_yn`), но она не привязана к `ACTION_CONSTRUCT` и не входит в разбираемый здесь путь исполнения.

**Базовый (грep) проход** — дословный `grep -nE "query_yn|query_int|uilist|popup|popup_getkey|string_input_popup|choose_adjacent|choose_adjacent_highlight|look_around|game_menus::inv|target_handler::mode_|ui_manager::redraw|cancel_activity_query|cancel_activity_or_ignore_query" src/construction.cpp` даёт 20 строк. Из них 2 — не вызовы (`:40`, `:70`, оба `#include`), и часть остальных — это сеттеры/геттеры на локальных объектах `popup`/`string_input_popup_imgui` (`:1075-1078`, `:1080`, `:1836`), которые не считаются отдельными UI-обращениями сверх самого объявления/`.query()` того же объекта (тот же подход, что и в разделе про Task 3 к `uilist smenu`/`kmenu`). Реальных, отдельных UI-обращений — 8: `popup` (`:532`), `ui_manager::redraw()` (`:1063`), `string_input_popup_imgui` (поиск-фильтр, объявление `:1074`/`.query()` `:1079`), `popup` (`:1136`), `choose_adjacent` (`:1385`), `string_input_popup_imgui` (имя машины, объявление `:1835`/`.query()` `:1837`), `query_yn` (`:1882`), `query_yn` (`:2033`), `who.query_yn` (`:2232`), `who.query_yn` (`:2271`) — то есть 10 отдельных строк таблицы (уточнение счёта: 8 «функций», но `string_input_popup_imgui` даёт 2 отдельные строки за счёт двух разных использований в разных функциях, а `query_yn`/`popup` встречаются по несколько раз в разных местах — итоговых строк в таблице ниже для базового прохода **10**).

**Индирекция (один уровень вглубь, по A1):** для каждой функции, вызываемой на пути (п.2-4, 6 выше), проверено, обращается ли она сама к списку UI-функций. Найдено 4 места:
- `g->warn_player_maybe_anger_local_faction( true )` (`construction.cpp:1124`) → `query_yn(...)` (`game.cpp:5706`).
- `iuse::handle_ground_graffiti(...)` внутри `construct::done_grave()` (`construction.cpp:1804`) → `string_input_popup_imgui` (объявление `iuse.cpp:4991`, `.query()` — `:4995`).
- `player_character.consume_items( it, 1, is_crafting_component, return_false<itype_id>, true )` (`construction.cpp:1439`) → `Character::consume_items(vector)` (`crafting.cpp:3327`) → `select_item_component()` (`crafting.cpp:2923`) → `uilist cmenu` («Use which component?», объявление `:3052`, `.query()` — `:3162`).
- `player_character.consume_tools( it )` (`construction.cpp:1450`) → `Character::consume_tools(vector)` (`crafting.cpp:3938`) → `select_tool_component()` (`crafting.cpp:3358`) → `uilist tmenu` («Use which tool?», объявление `:3417`, `.query()` — `:3460`).

Других мест на один уровень вглубь не найдено: `player_can_build`/`player_can_see_to_build`/`load_available_constructions`/`valid_constructions_near_player` (только чтение состояния, без UI); все `check_*`-предикаты конструкции (`construction.cpp:1559-1751`, файл прочитан целиком) — чистые предикаты по карте/трансформам, без UI; `done_trunk_plank`→`iuse::cut_log_into_planks()` (`iuse.cpp:4566`) — только назначает `chop_planks_activity_actor`, без UI; сама эта активность (`activity_actor.cpp:9938-9965`, `do_turn`/`finish` прочитаны целиком) — без UI; `place_appliance()` (`veh_appliance.cpp:86-140`, тело прочитано целиком) — без UI, хотя остальной файл `veh_appliance.cpp` содержит несколько обращений из списка (`:424`, `:525`, `:568`, `:570`, `:737`) — все они принадлежат другим функциям того же файла (взаимодействие с уже построенным устройством через `ACTION_EXAMINE`), не `place_appliance()`, и не лежат на пути `ACTION_CONSTRUCT`; `multi_build_construction_activity_actor::multi_activity_do()`/`construction_activity()` (`activity_item_handling.cpp:3823`/`:3161`, оба прочитаны целиком) — используют те же `consume_items`/`consume_tools`, что уже учтены выше, новых мест нет.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `popup` («You can not construct anything here.») | `src/construction.cpp:532` | грep | P | Первая строка `construction_menu()`, до построения UI списка и до любой мутации. |
| `ui_manager::redraw()` | `src/construction.cpp:1063` | грep | P | Перерисовка меню конструкций внутри цикла ввода, до `CONFIRM`, до `place_construction`. |
| `string_input_popup_imgui` (поиск/фильтр списка), объявление `:1074`, `.query()` — `:1079` | `src/construction.cpp:1074`/`:1079` | грep | P | Фильтрация видимого списка конструкций внутри `construction_menu()`, до выбора пункта и до `place_construction`. |
| `popup` («You can't build that!») | `src/construction.cpp:1136` | грep | P | Ветка `CONFIRM`, когда `player_can_build` вернул `false` — сообщение об ошибке, `place_construction` не вызывается вовсе. |
| `choose_adjacent` («Construct where?») | `src/construction.cpp:1385` | грep | P | Первая содержательная строка `place_construction()` — до `consume_items`/`partial_con_set`, мутации ещё не было. |
| `string_input_popup_imgui` (имя машины), объявление `:1835`, `.query()` — `:1837` | `src/construction.cpp:1835`/`:1837` | грep | A | Первая строка `construct::done_vehicle()` — `post_special`, вызывается из `complete_construction()` (`:1548`), вызываемой из `do_turn()` (`activity_actor.cpp:8951`). К этому моменту террейн/предметы уже мутированы `complete_construction()` (смена террейна, спавн байпродуктов, `act.set_to_null()`, все — до `post_specials`). |
| `query_yn` («Also reveal all nearby wirings?») | `src/construction.cpp:1882` | грep | A | Внутри `construct::done_wiring()`, тот же `post_special`-механизм, что и строка выше — после того, как `place_appliance()` уже создал устройство на тайле в той же функции. |
| `query_yn` («The rock feels much warmer than normal.  Proceed?») | `src/construction.cpp:2033` | грep | A | Внутри `construct::done_digormine_stair()`, тот же `post_special`-механизм. Ответ определяет исход (яма vs открытый проём) уже после того, как в этой же функции потрачены калории/вода/сон персонажа (`:2028-2030`, до проверки). |
| `who.query_yn` («%s\nConfirm deconstruct?») | `src/construction.cpp:2232` | грep | A | Внутри `construct::do_turn_deconstruct()` — вызывается не через `post_specials`, а напрямую через `pc->id->do_turn_special()` (`activity_actor.cpp:8944`), то есть буквально на каждом первом ходу `do_turn()` для этой конструкции (`counter == 0`, `:2227`). При отказе — `here.partial_con_remove()`+`who.cancel_activity()` (`:2248-2249`), т.е. активность уже была назначена и частично мутировала мир (`partial_con_set` в `place_construction`) до этого вопроса. |
| `who.query_yn` («Would you really touch the sacred resting place of the dead?») | `src/construction.cpp:2271` | грep | A | Внутри `construct::do_turn_exhume()`, тоже напрямую через `pc->id->do_turn_special()` из `do_turn()`, на каждом ходу, пока не появится соответствующая мораль (`!who.has_morale(morale_gravedigger)`, `:2269`). |
| `query_yn` («You're in the territory of %s… Continue anyway?») | `src/game.cpp:5706`, вызов — `construction.cpp:1124` | косв. | P | Вызывается внутри `construction_menu()` строго до `place_construction()` — до собственной мутации строительства. Побочный эффект отказа/согласия — изменение отношений с фракцией (`game.cpp:5713-5715`) — происходит атомарно с самим вопросом, а не после отдельной, уже начатой мутации стройки; см. примечание после таблицы. |
| `string_input_popup_imgui` (текст на надгробии), объявление `iuse.cpp:4991`, `.query()` — `:4995`; вызов — `construction.cpp:1804` (`construct::done_grave()`) | `src/iuse.cpp:4991`/`:4995` | косв. | A | `done_grave()` — тоже `post_special`, вызывается из `complete_construction()` из `do_turn()`; к этому моменту `here.destroy_furn(p, true)` (`construction.cpp:1811`) в этой же функции ещё не произошло (вызывается последней строкой), но `complete_construction()` уже сменил террейн/фёрнитуру и очистил активность до вызова `post_specials`. |
| `uilist cmenu` («Use which component?»), объявление `crafting.cpp:3052`, `.query()` — `:3162`; вызов — `construction.cpp:1439` через `consume_items`→`select_item_component` | `src/crafting.cpp:3052`/`:3162` | косв. | M | Вызывается внутри цикла по группам компонентов в `place_construction()` (`:1438-1445`), **до** `here.partial_con_set` (`:1448`), но каждая итерация цикла реально изымает предметы из мира сразу внутри `Character::consume_items(map&, const comp_selection<item_comp>&, ...)` (`crafting.cpp:3221`, реальное изъятие — `m.use_charges` на `:3248` / `m.use_amount` на `:3252`). Для конструкции с 2+ требуемыми группами материалов вторая и последующие итерации задают этот вопрос **после** того, как материалы первой группы уже необратимо изъяты — то есть после начала мутации мира, и ответ определяет, какие именно предметы будут изъяты дальше. Не гейтировано `test_mode`. |
| `uilist tmenu` («Use which tool?»), объявление `crafting.cpp:3417`, `.query()` — `:3460`; вызов — `construction.cpp:1450` через `consume_tools`→`select_tool_component` | `src/crafting.cpp:3417`/`:3460` | косв. | M | Вызывается в цикле по группам инструментов в `place_construction()` (`:1449-1451`) — безусловно **после** `here.partial_con_set( pnt, pc )` (`:1448`), которая уже записала частичную конструкцию на тайл. Мировая мутация к этому моменту уже началась независимо от того, сколько групп инструментов требуется; ответ определяет, чей запас заряда инструмента будет потрачен. Не гейтировано `test_mode`. |

Итог по проходам: базовый grep по `construction.cpp` даёт **10** строк реальных обращений (5 P, 5 A) из 20 совпадений regex (2 — `#include`, остальные — сеттеры/`.cancelled()` на тех же двух объектах `popup`/`string_input_popup_imgui`, уже учтённых объявлением+`.query()`). Индирекция (один уровень вглубь) добавляет **4** строки: 1 P, 1 A, **2 M**. Итого по всем проходам: **6 P, 6 A, 2 M, 0 D**.

**Примечание к строке `game.cpp:5706` (P):** в отличие от прочих P-строк, эта не является чистой проверкой без последствий — отказ/согласие сразу меняет `likes_u`/`trusts_u`/`respects_u` владельца лагеря (`game.cpp:5713-5715`). Это не нарушает инвариант для фазы выбора (диалог+мутация репутации происходят до какой-либо мутации строительства и не зависят от неё), но означает, что при переносе в намерение нужно записать не просто «был ли задан вопрос», а его результат как решение, которое `apply_intent` обязан воспроизвести (изменить репутацию фракции) — это не бесплатная UI-проверка, а совмещённый в текущем коде «вопрос+детерминированный побочный эффект», который переносится в фазу выбора целиком.

**Вердикт:** не режется

Основание — механическое, без интерпретационных зазоров (в отличие от Task 4): в таблице есть 2 строки класса M (`crafting.cpp:3162`, `crafting.cpp:3460`, обе — из индирекции через `place_construction()`). Правило `conventions.md` формулирует «не режется» напрямую через «есть хотя бы одно M»; `controller-addenda.md` (A3) требует **ноль** M-обращений для «режется с оговоркой» без исключений — количество A-строк и наличие для них обходов на выбор между «режется с оговоркой» и «не режется» не влияет, если M > 0. Обе M-находки реальны (не за `test_mode`), обе достижимы при обычной игре (конструкция с ≥2 группами материалов, либо любая конструкция, требующая инструмент, при наличии у игрока одновременно личного и ближнего источника материала/заряда) — ни одна не является гипотетическим краевым случаем.

**Набросок намерения:**

```cpp
// place_construction() сегодня смешивает выбор (choose_adjacent, выбор
// источника компонента/инструмента при неоднозначности) с первой мутацией
// (partial_con_set) в одной синхронной функции. Чтобы убрать оба M
// (crafting.cpp:3162/:3460), весь выбор источника материалов/инструментов
// должен переехать в фазу выбора целиком — это НЕ один булев ответ, как в
// A-находках, а набор решений по числу требуемых групп компонентов/
// инструментов конкретной конструкции (JSON-зависимо, не константа).
struct intent_construct {
    tripoint_bub_ms location;              // choose_adjacent, construction.cpp:1385, класс P
    construction_id chosen_construction;   // выбор пункта меню, construction_menu(), класс P

    // Предрешает uilist "Use which component?" (crafting.cpp:3162) для КАЖДОЙ
    // группы con.requirements->get_components(), где источник неоднозначен
    // (и на карте, и у игрока есть материал одновременно). usage_from —
    // реальный enum из comp_selection<item_comp> (character.h), не отдельная
    // самодельная перечислимая обёртка.
    std::vector<comp_selection<item_comp>> component_selection;

    // Предрешает uilist "Use which tool?" (crafting.cpp:3460) для каждой
    // группы con.requirements->get_tools(), тем же типом comp_selection,
    // что реально хранит select_tool_component().
    std::vector<comp_selection<tool_comp>> tool_selection;

    // Предрешает query_yn о территории фракции (game.cpp:5706, P, но с
    // побочным эффектом — см. примечание к таблице). Нужно "_prompted",
    // чтобы отличить "вопроса не было" от "был и получил отказ" — оба
    // случая иначе дают одинаковый false у "_accepted".
    bool faction_territory_prompted;
    bool faction_territory_accepted;
};

// Поля для A-находок (do_turn_special на каждый ход, done_* при завершении).
// Не отдельная структура "на будущее" — они обязаны попасть в тот же
// intent_construct, потому что apply_intent не может звать query_yn ни на
// каком ходу активности.
struct intent_construct_activity_hooks {
    // do_turn_deconstruct (construction.cpp:2232): предрешается тем же
    // текстом (potential_deconstruct_items), который известен уже в момент
    // выбора точки строительства/сноса — это не зависит от исхода мутации.
    bool deconstruct_confirmed;             // значим только для конструкций
                                             // категории DECONSTRUCT

    // do_turn_exhume (construction.cpp:2271): зависит только от
    // has_trait(trait_SPIRITUAL) — черта не меняется во время активности.
    bool grave_desecration_accepted;        // значим только для эксгумации

    // done_digormine_stair (construction.cpp:2033): t_lava под тайлом на
    // момент старта неизвестен игроку (не вскрыт) — заменяется
    // детерминированным правилом, а не предрешённым ответом (см. "Что
    // мешает"), поэтому здесь НЕ булево поле игрока, а константа поведения.
    bool lava_tunnel_policy_continue;       // true = "да, вопреки жаре"
                                             // (текущий текст default-ответа
                                             // не хранит игрок — это дизайн-
                                             // решение, см. ниже)

    // done_vehicle (construction.cpp:1837): произвольный текст, вводимый
    // игроком, не зависит от исхода мутации.
    std::string vehicle_name;               // пусто = "Car" (текущий дефолт)

    // done_wiring (construction.cpp:1882): чистое предпочтение игрока, не
    // зависит от исхода.
    bool reveal_nearby_wirings;

    // done_grave (construction.cpp:1804, через iuse::handle_ground_graffiti):
    // произвольный текст; запрашивается в выборе, только если у персонажа
    // qual_CUT на момент выбора (construction.cpp:1803) — детерминировано.
    std::optional<std::string> grave_inscription;
};
```

**Что мешает и как обойти:** вердикт определён двумя M-строками; обход для них назван, но ЭТО НЕ МЕНЯЕТ ВЕРДИКТ (M блокирует «режется с оговоркой» независимо от наличия обхода, см. «Вердикт» выше и прецедент Task 4).

1. **`crafting.cpp:3162`/`:3460` (выбор источника компонента/инструмента).** Оба спрашивают «откуда взять материал/заряд» и оба детерминированы состоянием мира на момент старта строительства (что лежит у игрока, что — на тайлах рядом) — то же самое состояние, которое `place_construction()` читает сегодня прямо перед вопросом. Обход: перенести весь цикл выбора источника (`select_item_component`/`select_tool_component` для каждой требуемой группы) в фазу выбора, записать результат как `component_selection`/`tool_selection` в `intent_construct`; `apply_intent` вызывает уже существующие `Character::consume_items(comp_selection<item_comp>, ...)`/`consume_tools(comp_selection<tool_comp>, ...)` — перегрузки, принимающие готовый `comp_selection`, без похода в `select_*_component()` и без `uilist`, — эти перегрузки уже существуют и используются сегодня как второй шаг внутри `select_item_component`/`select_tool_component`, так что `apply_intent` не придумывает новый путь мутации, а просто пропускает интерактивный первый шаг.
2. **`construction.cpp:2232` (подтверждение сноса).** Текст вопроса строится из `potential_deconstruct_items()` для террейна/фёрнитуры на выбранном тайле — известен уже в `place_construction()`, до `assign_activity`. Обход: задать вопрос в фазе выбора для конструкций категории `DECONSTRUCT`, записать `deconstruct_confirmed`; `do_turn_deconstruct()` читает готовый ответ вместо `query_yn` на первом ходу.
3. **`construction.cpp:2271` (эксгумация).** Зависит только от `has_trait(trait_SPIRITUAL)` — не меняется за время активности. Обход: посчитать в фазе выбора, записать `grave_desecration_accepted`.
4. **`construction.cpp:2033` (лава под тайлом при копке).** В отличие от пп.2-3, это НЕ предрешаемый вопрос — наличие `t_lava` под тайлом на старте копки, как правило, ещё не открыто игроку (не вскрытая клетка), так что «спросить заранее» подменяло бы реальную неопределённость поддельным выбором вслепую. Подходящий по конвенции обход — третий из трёх названных в `conventions.md» вариантов, «заменить на детерминированное правило»: зафиксировать политику (например, всегда продолжать копку, как если бы игрок сегодня отвечал «да», либо всегда останавливаться безопасно, как при «нет») вместо интерактивного вопроса; любой вариант убирает диалог из `do_turn()`, но меняет наблюдаемое поведение относительно сегодняшнего интерактивного выбора — отсюда `lava_tunnel_policy_continue` в намеченной структуре как поле дизайн-политики, а не поле, заполняемое игроком за ходом выбора.
5. **`construction.cpp:1837`/`:1882`/`iuse.cpp:4991` (имя машины / раскрыть проводку / надпись на могиле).** Все три — независимые от исхода мутации ввод/предпочтение игрока, знание о которых доступно уже на момент выбора точки строительства (в первых двух случаях — сразу; для надписи — только если у персонажа `qual_CUT`, тоже известно заранее). Обход — простое перемещение в фазу выбора, без изменения семантики.
6. **`game.cpp:5706` (территория фракции, класс P с побочным эффектом).** Не требует обхода для вердикта (уже P), но обход всё равно назван в наброске намерения (`faction_territory_prompted`/`_accepted`) для полноты — тот же самый паттерн, что применён к аналогичным находкам в Task 4 (`theft_prompted`/`_accepted`).

## 6. Чтение

**Точка входа:** `src/handle_action.cpp:2773` (`case ACTION_READ`) → `read()` (локальная статическая функция, определена в `src/handle_action.cpp:1747`).

**Путь исполнения:**

1. `handle_action.cpp:1747` `static void read()` — вызывает `game_menus::inv::read(player_character)` (`handle_action.cpp:1751`) для выбора книги из инвентаря/окружения. Единое правило исключения для этого шага: если книга не выбрана (пользователь нажал ESC или инвентарь пуст), функция выводит "Never mind." (`handle_action.cpp:1778`) и возвращается — это предварительное действие, а не часть решения о чтении, в таблицу ниже не включено.

2. `game_inventory.cpp:1633` `game_menus::inv::read(Character &you)` — обёртка над `inv_internal()` (`game_inventory.cpp:1638`), которая создаёт `inventory_pick_selector` и вызывает его `execute()`. Внутри `inv_internal()` при пустом инвентаре вызывается `popup( msg, PF_GET_KEY )` (`game_inventory.cpp:204`) — до мутации, класс P.

3. `inventory_ui.cpp:3877` `inventory_pick_selector::execute()` — интерактивный цикл выбора предмета (показывает инвентарь, обрабатывает ввод мыши/клавиатуры, возвращает выбранный `item_location`). Содержит вызовы `ui_manager::redraw()` (`inventory_ui.cpp:3884`, `:3895`, `:3917`) внутри цикла `while(true)` — до мутации, класс P.

4. Возврат к `handle_action.cpp:1753` — если книга выбрана, код определяет тип книги (spellbook с `learn_spell`, efile, обычная книга, martial arts manual) и вызывает соответствующий путь. Для spellbook вызывается `the_book.get_use("learn_spell")->call(...)` (`handle_action.cpp:1758`) — отдельное действие, не входит в 10 действий MVP-1 (разбирается отдельно). Для efile — `player_character.read(loc, parent_loc)` (`handle_action.cpp:1768`) с получением физического носителя через `parent_loc.obtain(player_character)` (`handle_action.cpp:1762`), затем `player_character.read(loc)` (`handle_action.cpp:1774`). Для обычной книги — `loc.obtain(player_character)` (`handle_action.cpp:1773`) и `player_character.read(loc)` (`handle_action.cpp:1774`). Все эти ветки сходятся на одном вызове `avatar::read()`.

5. `avatar.cpp:486` `bool avatar::read(item_location &book, item_location ereader)` — основная логика чтения:
   - Проверка `get_book_reader(*book, fail_messages)` (`avatar.cpp:494`) — определяет, кто может читать (аватар или NPC-чтец). Если `reader == nullptr`, выводятся причины отказа (`avatar.cpp:497-500`) и функция возвращается — это детерминированная проверка, без UI.
   - Для непрочитанных книг (первый раз): краткая "просмотрка" через `assign_activity(read_activity_actor(time_taken, book, ereader, false))` (`avatar.cpp:522`) — **фаза выбора заканчивается здесь**, мутации мира ещё не было.
   - Для guidebook (`itype_guidebook`): мгновенный эффект без активности (`avatar.cpp:526-537`) — `get_hint()` выводит подсказку, `mod_moves(-100)` тратит ходы, `assign_activity` не вызывается. Это одноходовое действие, не многоходовая активность.
   - Для обычных книг и martial arts manuals: **показ uilist** (`avatar.cpp:586`) с выбором:
     - "Read once" (один раз)
     - "Read until you gain a level" (до повышения уровня)
     - Список NPC-слушателей (`learners`), которые могут учиться
     - Список "reading for fun" (`fun_learners`)
     - Список "not participating" (`nonlearners`) с причинами
     - Для martial arts manuals: отдельный `uilist` (`avatar.cpp:673`) с выбором "Train once" / "Train until tired or success"
   - После выбора: `assign_activity(read_activity_actor(time_taken, book, ereader, continuous, learner_id))` (`avatar.cpp:748`) — **фаза выбора заканчивается здесь**, мутации мира ещё не было.

6. `activity_actor.cpp:2117` `read_activity_actor::start(player_activity &act, Character &who)` — инициализация активности: проверка `cancel_if_book_invalid()` (`:2119`), определение типа книги (`bktype`, `:2125-2126`), добавление книги в `act.targets` (`:2130`), списание заряда ereader (`:2137`, если используется). **Ни одного вызова UI-функций.**

7. `activity_actor.cpp:2144` `read_activity_actor::do_turn(player_activity &act, Character &who)` — вызывается движком активности каждый ход:
   - Проверка читаемости книги (`cancel_if_book_invalid`, `:2146`)
   - Определение типа книги (`:2150-2153`)
   - Проверка освещённости (`:2155-2161`) — если темно и нет флага `READ_IN_DARKNESS`/`CAN_USE_IN_DARK`, активность отменяется (`act.set_to_null()`)
   - Расход энергии для martial arts (`:2163-2165`) — `burn_energy_all(-1)` с вероятностью 1/3
   - Периодический debug-лог (`:2168-2171`)
   - Проверка наличия ereader (`:2173-2179`) — если e-book больше не найден, активность отменяется
   - Проверка зарядов ereader (`:2181-2189`) — если зарядов недостаточно, активность отменяется
   - Периодический расход зарядов (`:2191-2200`) — `ereader->consume_tool_uses(1, ...)` раз в 5 минут
   - **Ни одного вызова UI-функций** — вся логика детерминированная.

8. `activity_actor.cpp:2234` `read_activity_actor::player_read(avatar &you)` — обработка чтения для аватара (вызывается из `finish()` на `:2516`):
   - Идентификация книги (`:2236-2239`) — если не прочитана, вызывает `you.identify(*book)` и возвращается
   - Проверка `get_book_reader()` (`:2242-2249`) — если reader == nullptr, выводятся причины отказа
   - Сбор списка слушателей (`get_crafting_helpers()`, `:2258`) — детерминированно, на основе `get_book_mastery()`, `fun_to_read()`, `has_morale_to_read()`
   - Для каждого слушателя: вызов `read_book(*learner, islotbook, skill_level, penalty)` (`:2317`), который обновляет `SkillLevel` через `skill_level.readBook(min_ex, max_ex, level)` (`:2231`)
   - Проверка `continuous` и `learner_id` для определения, продолжать ли чтение (`:2333/2356`) — если уровень слушателя достиг целевого, `continuous = false`
   - Вывод сообщений о прогрессе (`:2326-2353`)
   - **Ни одного вызова UI-функций.**

9. `activity_actor.cpp:2383` `read_activity_actor::player_readma(avatar &you)` — martial arts manual (вызывается из `finish()` на `:2516`):
   - Идентификация книги (`:2387-2389`)
   - Проверка `one_in(difficulty)` для обучения (`:2406`) — если повезло, вызывает `mart_iter->second.call(&you, *book, you.pos_bub())` (`:2408`) и возвращается
   - Иначе: случайные сообщения о прогрессе (`:2411-2431`)
   - **Ни одного вызова UI-функций.**

10. `activity_actor.cpp:2435` `read_activity_actor::npc_read(npc &learner)` — чтение NPC (вызывается из `finish()` на `:2530`):
    - Аналогично `player_read`, но без идентификации и с проверкой `display_messages` (`:2444`) — сообщения выводятся только если NPC принадлежит фракции игрока и игрок видит NPC
    - Вызов `read_book(learner, islotbook, skill_level, 1.0)` (`:2466`)
    - Проверка `continuous` и обновление `continuous = false` при достижении целевого уровня (`:2480`)
    - **Ни одного вызова UI-функций.**

11. `activity_actor.cpp:2505` `read_activity_actor::finish(player_activity &act, Character &who)` — завершение активности:
    - Проверка `cancel_if_book_invalid()` (`:2507`)
    - Для аватара: вызов `player_readma()` или `player_read()` (`:2516`), если требуется завершить активность (`should_null == true`)
    - Для NPC: вызов `npc_read()` (`:2530`), если требуется завершить активность
    - Если `continuous == true`: пересчёт времени чтения (`time_taken = who.as_avatar()->time_to_read(*book, *reader)`, `:2559`), проверка `handle_study_zone_resume()` (`:2566`), перезапуск активности с новыми `moves_total`/`moves_left` (`:2571-2573`)
    - Иначе: сообщение "You finish reading." (`:2576`), проверка `handle_study_zone_resume()` (`:2579`), `act.set_to_null()` (`:2584`)
    - **Ни одного вызова UI-функций.**

12. `read_activity_actor::canceled()` — **не определён** (отсутствует в `activity_actor_definitions.h:970-1024`). Используется базовый `activity_actor::canceled()`, который просто сбрасывает активность в null через `act.set_to_null()`.

Как решалось, что вызов «на пути»: путь исполнения ACTION_READ прослежен по цепочке вызовов из п. «Путь исполнения» выше, включая файлы, не входившие в исходную команду сбора брифа (`game_inventory.cpp`, `inventory_ui.cpp`, `avatar.cpp`) — они добавлены трассировкой, а не грепом, и это явно указано в соответствующих строках таблицы. Включены все вызовы, достижимые из `handle_action.cpp:1747` `read()` через `game_menus::inv::read()` → `inv_internal()` → `inventory_pick_selector::execute()` → `avatar::read()` → `read_activity_actor::{start,do_turn,finish}`, включая все ветки (spellbook, efile, обычная книга, martial arts manual, непрочитанная книга, guidebook). Исключены: `ACTION_LEARN_SPELL`/`learn_spell_actor::use` — отдельный экшен для spellbooks, не входит в 10 действий MVP-1; `multi_read_activity_actor` (изучение нескольких книг подряд) — отдельная точка входа, недостижимая из `ACTION_READ`.

**Шаг 3 брифа (вопрос о слушателях):** игра спрашивает "кого учить" **в фазе выбора**, а не внутри активности. Конкретно в `avatar::read()`: (а) `avatar.cpp:544-579` — построение списков `learners`, `fun_learners`, `nonlearners` (детерминированно, на основе `get_book_mastery()`, `fun_to_read()`, `has_morale_to_read()`); (б) `avatar.cpp:585-664` — показ `uilist menu` с вариантами выбора; (в) `avatar.cpp:656` — `menu.query(true)` — запрос к игроку; (г) `avatar.cpp:748` — `assign_activity(read_activity_actor(..., continuous, learner_id))` — передача `learner_id` в активность. Классификация: Class P (до мутации мира, до создания активности). Следствие для мультиплеера: в MP-сессии вопрос "кого учить" должен задаваться каждому игроку отдельно в фазе выбора. Ответ (выбранный `learner_id`) передаётся в намерении как поле `learner_id`. Внутри активности (`do_turn`/`finish`) код опирается только на переданный `learner_id`, не спрашивая ничего у игрока.

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `game_menus::inv::read(player_character)` | `src/handle_action.cpp:1751` | грep | P | Выбор книги из инвентаря/окружения — часть фазы выбора, до `assign_activity`. Мутации мира не было. |
| `popup("...nothing to read...")` | `src/game_inventory.cpp:204` (внутри `inv_internal`) | косв. | P | Показывается, если у персонажа нет книг. До мутации, до `assign_activity`. |
| `ui_manager::redraw()` (цикл выбора предмета) | `src/inventory_ui.cpp:3884,3895,3917` (внутри `inventory_pick_selector::execute()`) | косв. | P | Перерисовка инвентаря в цикле выбора. До мутации, до `assign_activity`. |
| `uilist menu` (выбор режима чтения и слушателя) | `src/avatar.cpp:586` | грep | P | Главное меню чтения: "Read once", "Read until level up", список NPC-слушателей. До `assign_activity`. |
| `menu.query(true)` | `src/avatar.cpp:656` | грep | P | Запрос к игроку о выборе режима/слушателя. Классический пример P: вопрос задаётся до активности. |
| `uilist menu` (martial arts manual) | `src/avatar.cpp:673` | грep | P | Отдельное меню для martial arts manuals: "Train once" / "Train until tired or success". До `assign_activity`. |

Итог по проходам: базовый grep по `character.cpp`/`activity_actor.cpp` (файлы из брифа) не дал ни одного совпадения на пути чтения — все найденные вызовы относятся к другим действиям (`mend_item`, `select_item_components`, `crush_item`, `pick_style` в `character.cpp`; `mode_fire`, `choose_adjacent_highlight`, `popup` для других активностей в `activity_actor.cpp`). Трассировка на один уровень косвенности (файлы `handle_action.cpp`, `game_inventory.cpp`, `inventory_ui.cpp`, `avatar.cpp`, не входящие в список брифа) добавила 6 вызовов, все — Class P. **0 M, 0 A, 6 P.**

**Вердикт:** режется

**Набросок намерения:**

```cpp
struct intent_read {
    item_location book;                 // выбранная книга (из game_menus::inv::read)
    std::optional<item_location> ereader; // ereader для e-books (nullopt для обычных книг)

    bool continuous;                    // "Read until level up" / "Train until success"
    int learner_id;                     // ID выбранного слушателя (-1 = только аватар)

    // Для martial arts manuals:
    bool train_once;                    // false = continuous (если is_martialarts)

    // Детерминированные вычисления, известные в момент выбора:
    time_duration time_taken;           // время чтения, вычисленное time_to_read() (avatar.cpp:511)
    book_type bktype;                   // normal / martial_art (определяется по book->type->use_methods.count("MA_MANUAL"))

    // Для continuous-режима:
    skill_id target_skill;              // навык, который будет качаться (islotbook->skill)
    int target_level;                   // уровень, до которого будем читать (islotbook->level)
};
```

Поля `intent_read` покрывают: выбор книги (`book`, `ereader`) — из `game_menus::inv::read()`; режим чтения (`continuous`, `learner_id`, `train_once`) — из `uilist menu` в `avatar::read()`; время и тип (`time_taken`, `bktype`) — детерминированные вычисления на момент выбора; цель обучения (`target_skill`, `target_level`) — для continuous-режима, чтобы `apply_intent` мог проверить, когда остановиться.

Не включено (и не требуется): список слушателей (`learners`, `fun_learners`, `nonlearners`) — вычисляется заново в `do_turn`/`player_read()` на актуальном состоянии карты (NPC могли двигаться). Это не часть намерения, а состояние мира. Результат `get_book_reader()` — определяется из `book` и текущего состояния персонажа, пересчитывается в `apply_intent`.

**Что мешает и как обойти:** Проблем нет. Чтение — один из самых чистых случаев для разделения choose/execute:

1. **Фаза выбора (P):** Весь UI-взаимодействие происходит до `assign_activity()`: выбор книги (`game_menus::inv::read()`), выбор режима и слушателя (`uilist menu` в `avatar::read()`). Все ответы фиксируются и передаются в намерении.

2. **Фаза исполнения (без UI):** `read_activity_actor::{start,do_turn,finish}` не содержат ни одного вызова из списка UI-функций: `do_turn` только расходует moves и ereader charges; `finish` применяет результаты (`player_read()`/`npc_read()`) без вопросов; `canceled()` отсутствует (используется базовый, который не спрашивает ничего).

3. **Мультиплеер:** Вопрос "кого учить" задаётся каждому игроку отдельно в фазе выбора. Ответ (`learner_id`) передаётся в намерении. Внутри активности код опирается только на переданный `learner_id`, не требуя дополнительных решений от других игроков.

Риски (не влияющие на вердикт): `continuous`-режим с `learner_id` — если слушатель (NPC) покидает группу или меняет состояние между отправкой намерения и выполнением, активность может завершиться досрочно (`player_read()` проверяет `learner_id` на `:2333/2356`). Это не UI-проблема, а логика отмены активности. Ereader charges — если ereader разрядился или был потерян между отправкой и выполнением, `do_turn` отменит активность (`activity_actor.cpp:2173-2189`). Это детерминированная проверка, не требующая UI.


## 7. Торговля

**Точка входа:** `src/game.cpp:5433` (`case ACTION_TRADE:` → `npc_trading::trade( who, 0, _( "Trade" ) );`) и `src/npctalk_funcs.cpp:200,230,1132` (ветки диалога «Reward», «Trade», «Pay:»). Единое определение — `src/npctrade.cpp:287` `bool npc_trading::trade( npc &np, int cost, const std::string &deal )`.

**Путь исполнения:**

1. `npctrade.cpp:287` `npc_trading::trade()` — подготовка: `np.shop_restock()` (`:289`), `np.drop_invalid_inventory()` (`:292`). Затем создаётся `std::unique_ptr<trade_ui>` (`:294`) и вызывается `tradeui->perform_trade()` (`:295`). **Фаза выбора заканчивается здесь и передаёт управление интерактивному циклу**, мутации мира ещё не было.

2. `trade_ui.cpp:193` `trade_ui::perform_trade()` — интерактивный цикл `while( !_exit )`, внутри которого на каждом шаге вызывается `_panes[_cpane]->execute()` (`:199`) и обрабатываются события из очереди через `_process( ev )` (`:204`). Цикл работает, пока не придёт событие `TRADECANCEL`, `TRADEOK` или `SWITCH`. При `TRADEOK` вызывается `_confirm_trade()` (`:286`), который может открыть `popup()` или `query_yn()`. При успешном завершении (`_traded == true`) возвращается `trade_result_t` со списками выбранных предметов и расчётами.

3. `trade_selector::execute()` (наследник `inventory_drop_selector`, реализован в `inventory_ui.cpp:5353`) — собственный цикл обработки ввода игрока в рамках одной панели торгового окна. Содержит вызов `ui_manager::redraw_invalidated()` (`:5362`) внутри цикла — это перерисовка, а не вопрос. Обработка ввода идёт через `_ctxt_trade.handle_input()` (`:5363`), который может сгенерировать события `SWITCH`, `TRADEOK`, `TRADECANCEL`, `AUTO_BALANCE`, `BANK_BALANCE`, либо передать управление в `inventory_drop_selector::on_input()` (`:5382`) для стандартных действий инвентаря (выбор предмета, фильтр и т.д.).

4. `_confirm_trade()` (`trade_ui.cpp:301`) — проверка условий сделки: если NPC не готов принять сделку, показывается `popup()` (`:307`/`:309`/`:314`). Если NPC не может запомнить долг, вызывается `query_yn( "I'm never going to be able to pay you back..." )` (`:317`). Иначе — финальное подтверждение `query_yn( "Looks like a deal! Accept this trade?" )` (`:323`). Все вызовы происходят **до** мутации мира, в рамках интерактивного цикла выбора.

5. После возврата из `perform_trade()`, если `trade_result.traded == true`, выполняются фактические перемещения предметов:
   - `npc_trading::transfer_items( trade_result.items_you, ... )` (`npctrade.cpp:305`) — перемещение предметов игрока в эскроу.
   - `npc_trading::transfer_items( trade_result.items_trader, ... )` (`:307`) — перемещение предметов NPC к игроку.
   - `distribute_items_to_npc_zones( escrow, np )` (`:310`, для shopkeeper) или цикл `np.i_add()` (`:312-314`) — размещение полученных предметов у NPC.
   - Обработка `trade_charges`/`trade_amount` на тайлах карты (`:317-335`).
   - Обновление баланса: `player_character.cash -= trade_result.delta_bank` (`:339`), `update_npc_owed()` (`:340`), `player_character.practice()` (`:341`).

**Классификация всех найденных обращений к UI:**

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `ui_manager::redraw_invalidated()` (основной цикл перерисовки) | `src/inventory_ui.cpp:5362` (внутри `trade_selector::execute()`) | грep | P | Перерисовка внутри интерактивного цикла выбора. До мутации, до `perform_trade()`. |
| `ui_manager::redraw()` (запрос строки фильтра) | `src/inventory_ui.cpp:3233` (внутри `inventory_selector::query_string()`, вызываемой из `query_count()`) | косв. | P | Вызывается, когда игрок вводит фильтр в торговом окне. До мутации. |
| `string_input_popup` (запрос строки фильтра) | `src/inventory_ui.cpp:3214` (внутри `inventory_selector::query_string()`) | косв. | P | Создаётся при вводе фильтра. До мутации. |
| `popup( "%s cannot be traded." )` | `src/trade_ui.cpp:242` (внутри `autobalance()`) | грep | P | Показывается, если автобаланс пытается добавить нес tradeable предмет. До мутации. |
| `popup( "Your promises of digital payment..." )` | `src/trade_ui.cpp:258` (внутри `bank_balance()`) | грep | P | Показывается, если опция CAPITALISM выключена. До мутации. |
| `popup( "You'll need to offer me more than that." )` | `src/trade_ui.cpp:307` (внутри `_confirm_trade()`) | грep | P | Показывается, если NPC не готов принять сделку и не может extend credit. До мутации. |
| `popup( "Sorry, I'm only willing to extend you %s in credit." )` | `src/trade_ui.cpp:309` (внутри `_confirm_trade()`) | грep | P | Показывается, если NPC не готов принять сделку, но может extend credit. До мутации. |
| `popup( "%s doesn't have the appropriate pockets..." )` | `src/trade_ui.cpp:314` (внутри `_confirm_trade()`) | грep | P | Показывается, если у NPC нет карманов для принимаемых предметов. До мутации. |
| `query_yn( "I'm never going to be able to pay you back..." )` | `src/trade_ui.cpp:317` (внутри `_confirm_trade()`) | грep | P | Вопрос о том, соглашается ли игрок на долг, который NPC не может запомнить. До мутации. |
| `query_yn( "Looks like a deal! Accept this trade?" )` | `src/trade_ui.cpp:323` (внутри `_confirm_trade()`) | грep | P | Финальное подтверждение сделки. До мутации. |

Как решалось, что вызов «на пути»: путь исполнения прослежен от `npc_trading::trade()` через `trade_ui::perform_trade()` → `trade_selector::execute()` → `_process( ev )` → `_confirm_trade()`, а также через индикредность `inventory_selector::query_string()` (вызываемую из `query_count()` при вводе фильтра). Все вызовы происходят внутри интерактивного цикла `perform_trade()`, который завершается ДО любого перемещения предметов. Фактическая мутация мира происходит после возврата из `perform_trade()` в `npc_trading::trade()` (`:297-343`), и ни один из вызовов в этом участке не содержит UI-функций (проверено: `transfer_items`, `distribute_items_to_npc_zones`, `update_npc_owed`, `player_character.practice` — все детерминированные, без вопросов).

**Вердикт:** режется

**Набросок намерения:**

```cpp
struct intent_trade {
    item_location trader;                 // NPC-продавец (character_id или equivalent)
    bool trade_accepted;                  // результат query_yn("Looks like a deal!") — ключевое поле:
                                          // если false, сделка не состоится, и apply_intent не должен
                                          // мутировать мир. Предрешается в choose-фазе.

    // Списки обмениваемых предметов, собранные в choose-фазе:
    struct trade_item {
        item_location location;           // исходная локация предмета (откуда берётся)
        int quantity;                     // количество (для charge-based предметов — заряды)
    };
    std::vector<trade_item> items_from_player;   // что игрок отдаёт NPC
    std::vector<trade_item> items_from_trader;   // что NPC отдаёт игроку

    // Расчёты, известные в момент подтверждения:
    int value_player_items;               // суммарная стоимость предметов игрока
    int value_trader_items;               // суммарная стоимость предметов NPC
    int balance;                          // разница (player - trader), <0 = игрок доплачивает
    int delta_bank;                       // изменение банковского счёта (withdrawal/deposit)
    int cost;                             // начальная стоимость услуги (если cost > 0, это не barter)

    // Для NPC, которые не обмениваются бесплатно:
    bool will_exchange_freely;            // np.will_exchange_items_freely() — известен заранее
    int max_credit_extended;              // максимальный кредит NPC — известен заранее
    int max_willing_to_owe;               // максимум, готовый запомнить долг — известен заранее
    bool debt_over_limit_confirmed;       // результат query_yn("I'm never going to be able...") —
                                          // предрешается в choose-фазе, если долг превышает лимит
};
```

Поля `items_from_player` и `items_from_trader` используют `item_location` с указанием исходной локации, потому что `transfer_items()` в `apply_intent` обязан восстановить те же перемещения, что были выбраны в choose-фазе. Поле `trade_accepted` — критическое: если игрок отказался от подтверждения (`query_yn` вернул false), намерение должно быть отброшено без применения. Поля `max_credit_extended` и `max_willing_to_owe` нужны, чтобы `apply_intent` мог детерминированно воспроизвести логику `_confirm_trade()` без вызова UI — но поскольку решение уже предрешено в choose-фазе (`trade_accepted`), эти поля фактически не используются в execute, а служат для валидации намерения (если состояние мира изменилось так, что сделка больше невозможна, intent должен быть отклонён).

Не включено (сознательно): `npc.shop_restock()` и `np.drop_invalid_inventory()` — это обслуживание NPC перед торговлей, не часть исхода сделки. `distribute_items_to_npc_zones()` — детерминированная логика размещения, не требует UI. `player_character.practice(skill_speech, ...)` — детерминированный расчёт, не требует UI.

**Что мешает и как обойти:** Проблем нет. Торговля — один из самых чистых случаев для разделения choose/execute:

1. **Фаза выбора (P):** Весь UI-взаимодействие происходит внутри `perform_trade()` до мутации мира: выбор предметов обоими сторонами, автобаланс, банковский баланс, подтверждения долга и финальное "Looks like a deal?". Все ответы фиксируются и передаются в намерении.

2. **Фаза исполнения (без UI):** После `perform_trade()` код в `npc_trading::trade()` (`:297-343`) выполняет только детерминированные перемещения предметов и расчёты — ни одного вызова из списка UI-функций. Нет activities, нет `assign_activity`, нет многоходовых процессов.

3. **Мультиплеер:** Вопрос "accept this trade?" задаётся каждому игроку отдельно в фазе выбора. Ответ (`trade_accepted`) передаётся в намерении. Внутри execute код опирается только на переданные списки предметов и расчёты, не требуя дополнительных решений от других игроков.

Риски (не влияющие на вердикт): `npc.will_exchange_items_freely()` и кредитные лимиты — если NPC потерял статус shopkeeper или изменил отношение к игроку между отправкой намерения и выполнением, `apply_intent` должен отклонить intent (проверка `npc_will_accept_trade` в `_confirm_trade` воспроизводится детерминированно). Это не UI-проблема, а логика валидации.


## 8. Установка бионики и частей транспорта

**Точка входа (бионика):** `src/iuse_actor.cpp:4578` (`install_bionic_actor::use`, вызов `can_install_bionics` → `install_bionics`).
**Точка входа (транспорт):** `src/veh_interact.cpp:949` (`veh_interact::do_install`, интерактивный выбор детали).

**Путь исполнения:**

*Бионика:*
1. `iuse_actor.cpp:4578` `install_bionic_actor::use(Character*, item&)` — вызывает `p->can_install_bionics(*it.type, *p, false)` (`iuse_actor.cpp:4581`), затем при успешной проверке — `p->install_bionics(*it.type, *p, false)` (`iuse_actor.cpp:4586`).
2. `bionics.cpp:2460` `Character::can_install_bionics` — содержит обращения к UI: `query_yn` о конфликтующих мутациях (`:2496`), `popup` о нехватке слотов (`:2512`), `query_yn` о шансе успеха (`:2518`, `:2524`). Все до вызова `install_bionics`, то есть до мутации мира. Класс P.
3. `bionics.cpp:2547` `Character::install_bionics` — **не содержит** обращений к UI. Вызывает `assign_activity( bionic_operation_activity_actor(...) )` (`bionics.cpp:2584`). Фаза выбора заканчивается здесь.
4. `activity_actor.cpp:1617` `bionic_operation_activity_actor::do_turn` — вызывается движком активности каждый ход. Внутри: расчёт времени, проверка автодока, вызов `perform_install`/`perform_uninstall`. **Ни одного обращения к UI-функциям из списка.**
5. `activity_actor.cpp:1766` `bionic_operation_activity_actor::finish` — сообщения об успехе/неудаче, удаление эффекта `effect_under_operation`. **Ни одного обращения к UI-функциям из списка.**

*Дополнительные точки входа в бионику:*
- `iexamine.cpp:5932` `game_menus::inv::install_bionic(you, you, patient)` → `autodoc_internal` (`game_inventory.cpp:2833`) → `inventory_pick_selector::execute()` (`inventory_ui.cpp:3877`) с `ui_manager::redraw` в цикле — P, до мутации. Затем `can_install_bionics`/`install_bionics` — как выше.
- `npctalk_funcs.cpp:583` `bionic_install_common` → `patient.can_install_bionics(it, p, false, 20)` → `patient.install_bionics(it, p, false, 20)` — те же функции, те же классы.

*Транспорт:*
1. `veh_interact.cpp:949` `veh_interact::do_install(map&)` — интерактивный цикл выбора детали. Содержит: `string_input_popup_imgui popup(34)` для фильтра (`:1009`), `query_yn` о потере летучести (`:1042`), `query_yn` о складывании (`:1051`), `ui_manager::redraw()` в цикле (`:1004`). Все — до `assign_activity`. Класс P.
2. `veh_interact.cpp:162` `veh_interact::assign_activity(map&)` — вычисляет время, вызывает `player_character.assign_activity( vehicle_activity_actor( sel_cmd, time, veh, ... ) )` (`:209`). **Ни одного обращения к UI.** Фаза выбора заканчивается здесь.
3. `activity_actor.cpp:11343` `vehicle_activity_actor::complete_vehicle(player_activity&, Character&)` — для `VEHICLE_INSTALL`:
   - Потребление компонентов/инструментов (без UI).
   - `veh.install_part(...)` (`:11411`) — без UI.
   - Если `vp_new.info().variants.size() > 1` (`:11419`) — вызывает `veh_shape::change_part_shape(...)` (`:11420`) → `veh_menu::query()` (`veh_utils.cpp:427`) → `uilist menu` (`veh_utils.cpp:436`). **Класс A** (живёт внутри `finish` активности).
   - Если деталь имеет световые флаги (`:11427-11429`) — вызывает `orient_part(...)` (`:11430`) → `popup(...)` (`veh_interact.cpp:231`) + `g->look_around()` (`:235`). **Класс A**.
   - `here.board_vehicle(...)` если `VPFLAG_BOARDABLE` — без UI.
   - `you.practice(...)` — без UI.
4. `activity_actor.cpp:11305` `vehicle_activity_actor::finish(...)` — после `complete_vehicle`, для не-NPC: при наличии vehicle_part на позиции вызывает `g->exam_vehicle(vp->vehicle(), mount_loc)` (`:11333`) или `g->exam_appliance(...)` (`:11331`). Это **возврат в интерактивное меню транспорта**, то есть новый цикл с P-обращениями. Класс A (вызывается из `finish`).
5. `vehicle_activity_actor::do_turn` (`activity_actor_definitions.h:2865`) — пустой (`{};`), без UI.

**Шаг 2 брифа (grep по указанным файлам):**

| Обращение | Файл:строка | Проход | Класс | Обоснование |
|---|---|---|---|---|
| `query_yn` («Installing this bionic will remove the conflicting traits…») | `src/bionics.cpp:2496` | грep | P | В `can_install_bionics`, вызываемой из `install_bionic_actor::use` (`iuse_actor.cpp:4581`) и из `iexamine.cpp:5962`/`npctalk_funcs.cpp:583` — до `install_bionics`, до мутации. |
| `popup` («Not enough space for bionic installation!») | `src/bionics.cpp:2512` | грep | P | Там же, в `can_install_bionics`, до мутации. |
| `query_yn` («Are you sure you wish to install the selected bionic?») | `src/bionics.cpp:2518` | грep | P | Там же, в `can_install_bionics`, до мутации. |
| `query_yn` («WARNING: %i percent chance of failure…») | `src/bionics.cpp:2524` | грep | P | Там же, в `can_install_bionics`, до мутации. |
| `ui_manager::redraw` (в цикле `inventory_pick_selector::execute`) | `src/inventory_ui.cpp:3884` | косв. | P | Найдено трассировкой: `iexamine.cpp:5932` → `game_menus::inv::install_bionic` (`game_inventory.cpp:3058`) → `autodoc_internal` (`game_inventory.cpp:2867`) → `inventory_pick_selector::execute()` (`inventory_ui.cpp:3877`), внутри цикла `while(true)` на `:3883` — `ui_manager::redraw()` на `:3884`. До `can_install_bionics`/`install_bionics`, до мутации. |
| `popup` («You don't have any bionics to install.») | `src/game_inventory.cpp:2883` | косв. | P | Та же трассировка: `autodoc_internal` при пустом инвентаре биоников. До мутации. |
| `string_input_popup_imgui popup(34)` («Search for part») | `src/veh_interact.cpp:1009` | грep | P | В `do_install`, интерактивный цикл выбора детали, до `assign_activity`. |
| `query_yn` («Installing this part will mean that this vehicle is no longer flightworthy.») | `src/veh_interact.cpp:1042` | грep | P | В `do_install`, до `assign_activity`. |
| `query_yn` («Installing this part will make the vehicle unfoldable. Continue?») | `src/veh_interact.cpp:1051` | грep | P | В `do_install`, до `assign_activity`. |
| `ui_manager::redraw` (в цикле `do_install`) | `src/veh_interact.cpp:1004` | грep | P | В `do_install`, каждый ход цикла выбора детали. До `assign_activity`. |
| `ui_manager::redraw` (в цикле `do_main_loop`) | `src/veh_interact.cpp:529` | грep | P | В `do_main_loop` (`veh_interact.cpp:513`), вызываемом из `run` (`:254`) перед `assign_activity` (`:255`). До мутации. |
| `uilist` (через `veh_menu::query` в `change_part_shape`) | вызов `src/activity_actor.cpp:11420` → `veh_shape.cpp:146` → `veh_utils.cpp:427` (`veh_menu::query`) → `uilist menu` (`:436`) | косв. | A | Найдено трассировкой: `complete_vehicle` при `variants.size() > 1`. Вызывается из `finish` активности, то есть внутри многоходовой деятельности. |
| `popup` («Press space, choose a facing direction…») | вызов `src/activity_actor.cpp:11430` → `veh_interact.cpp:231` | косв. | A | Найдено трассировкой: `orient_part` при световых флагах детали. Вызывается из `finish` активности. |
| `look_around` | вызов `src/activity_actor.cpp:11430` → `veh_interact.cpp:235` | косв. | A | Та же трассировка: `orient_part` вызывает `g->look_around()` для выбора направления. Внутри активности. |
| `exam_vehicle` / `exam_appliance` (возврат в меню после активности) | `src/activity_actor.cpp:11333` / `:11331` | косв. | A | Вызывается из `finish` для не-NPC, если на позиции осталась деталь. Возвращает игрока в интерактивное меню транспорта — тот же цикл с P-обращениями. |
| `uilist` («Consume what?», выбор компонента при установке детали) | вызов `src/activity_actor.cpp:11387` (`consume_items`) → `crafting.cpp:3327` (`Character::consume_items`) → `crafting.cpp:2923` (`select_item_component`) → `uilist cmenu` (`crafting.cpp:3052`) | косв. | A | Найдено трассировкой: `complete_vehicle` при `VEHICLE_INSTALL` вызывает `consume_items` для потребления компонентов. Если у компонента несколько подходящих предметов в инвентаре/на карте, вызывается `uilist` для выбора. Вызывается из `finish` активности. |

Итог по проходам: базовый grep по `bionics.cpp`/`veh_interact.cpp` дал 11 различающихся мест вызова (5 в `bionics.cpp`, 6 в `veh_interact.cpp`), все — класса P. Проход трассировки на один уровень косвенности (файлы `inventory_ui.cpp`, `game_inventory.cpp`, `activity_actor.cpp`, `crafting.cpp`, `veh_shape.cpp`, `veh_utils.cpp`, не входящие в список брифа) добавил 7 мест: 1 P (в `inventory_ui.cpp`/`game_inventory.cpp` — выбор бионика через autodoc), 6 A (в `activity_actor.cpp`/`crafting.cpp` — `change_part_shape`/`orient_part`/`exam_vehicle`/`uilist(consume_items)` в `complete_vehicle`/`finish`). Итого: **0 M**, **6 A**-достижимых обращений.

**Вердикт:** режется с оговоркой

**Набросок намерения:**

```cpp
// Намерение «установить бионик» — покрывает путь от выбора бионика
// в инвентаре до assign_activity(bionic_operation_activity_actor).
// Сам activity_actor не содержит UI-вызовов, поэтому execute-фаза
// полностью детерминированна.
struct intent_install_bionic {
    item_location bionic_item;      // выбранный бионик (CBM)
    bool installing;                // true = установка, false = удаление (can_uninstall_bionic)
    bionic_id installed_bionic;     // ID устанавливаемого/удаляемого бионика
    bionic_uid uninstalled_bionic;  // UID удаляемого бионика (для uninstall)
    bool operation_autodoc;         // автодок или ручной
    int operation_skill;            // навык установщика
    int operation_difficulty;       // сложность операции
    bionic_id upgraded_from;        // ID бионика, который будет заменён при апгрейде (upgraded_bionic)
    std::string installer_name;     // имя установщика для сообщений

    // Результаты can_install_bionics, предрешённые в фазе выбора:
    bool confirm_conflicting_muts;  // ответ на query_yn о конфликтующих мутациях (bionics.cpp:2496)
    bool confirm_success_chance;    // ответ на query_yn о шансе успеха (bionics.cpp:2518/2524)

    // Разрешение конфликта «нехватка слотов» — если can_install_bionics
    // вернула false с popup («Not enough space…», bionics.cpp:2512),
    // намерение не создаётся вовсе. Если вернула true, значит слоты есть,
    // и это поле не нужно.
};

// Намерение «установить деталь транспорта» — покрывает путь от выбора детали
// в veh_interact::do_install до assign_activity(vehicle_activity_actor).
// Содержит все предрешённые вопросы (летучесть, складывание, вариант, ориентация, компоненты).
struct intent_install_vehicle_part {
    vpart_id part_id;               // выбранная деталь
    int part_index;                 // индекс детали в автомобиле (для позиционирования)
    point_rel_ms mount_location;    // куда устанавливать
    vpart_id variant_chosen;        // выбранный косметический вариант (если variants.size() > 1)
                                     // — предрешается в do_install через veh_menu::query
    units::angle direction;         // направление установки (если деталь имеет световые флаги)
                                     // — предрешается в do_install через orient_part/look_around

    // Результаты query_yn из do_install, предрешённые в фазе выбора:
    bool confirm_flightworthy_loss; // ответ на query_yn о потере летучести (veh_interact.cpp:1042)
    bool confirm_unfoldable;        // ответ на query_yn о складывании (veh_interact.cpp:1051)

    // Результаты select_item_component/select_tool_component, предрешённые в фазе выбора:
    // — предрешаются в do_install через аналогичный выбор компонентов/инструментов
    // — поля comp_selection<item_comp> и comp_selection<tool_comp> уже сериализуемы
    //   (crafting.cpp:91-133), как показано в Task 5
    std::vector<comp_selection<item_comp>> component_selections;
    std::vector<comp_selection<tool_comp>> tool_selections;

    // Разрешение «возврата в меню» после активности (activity_actor.cpp:11333):
    // Это не часть намерения установки, а побочный эффект finish().
    // В apply_intent после успешной установки достаточно показать сообщение
    // «You install a X into the Y» и вернуть управление — повторный вход
    // в меню необязателен (см. "Что мешает" ниже).
};
```

**Что мешает и как обойти:**

Для бионики: путь чистый — `can_install_bionics` содержит все P-обращения (подтверждение конфликта мутаций, нехватка слотов, шанс успеха), `install_bionics` не имеет UI и создаёт активность, а `bionic_operation_activity_actor::{do_turn,finish}` не содержат ни одного вызова из списка. Вердикт «режется» без оговорок.

Для транспорта: шесть A-обращений в `complete_vehicle`/`finish` устраняются так:
1. `veh_shape::change_part_shape` → `uilist` (veh_utils.cpp:436, вызов из activity_actor.cpp:11420) — выбор косметического варианта. Игрок выбирает вариант в интерактивном цикле `do_install` до `assign_activity`. Поле `variant_chosen` в `intent_install_vehicle_part` передаёт выбор в execute-фазу.
2. `orient_part` → `popup` + `look_around` (veh_interact.cpp:231/235, вызов из activity_actor.cpp:11430) — выбор направления для светящихся деталей. Игрок выбирает направление в том же цикле `do_install`. Поле `direction` в намерении передаёт выбор.
3. `exam_vehicle`/`exam_appliance` (activity_actor.cpp:11333) — не является частью установки как таковой: это побочный эффект `finish()`, который просто возвращает игрока в меню транспорта для дальнейших действий. В `apply_intent` этот вызов не нужен — достаточно показать сообщение об успехе и завершить активность.
4. `uilist` в `select_item_component` (crafting.cpp:3052, вызов из activity_actor.cpp:11387 через `consume_items`) — выбор компонента при установке детали. Игрок выбирает компоненты в фазе выбора (аналогично `intent_start_craft` из Task 5). Поля `component_selections`/`tool_selections` в намерении передают уже выбранные компоненты. После этого `consume_items` вызывается с конкретными предметами, без `uilist`.

Все четыре обхода названы конкретно, M-обращений не обнаружено (все UI-вызовы в `complete_vehicle`/`finish` имеют класс A, а не M, поскольку живут внутри активности). Вердикт «режется с оговоркой» обоснован.


## Сводка

| # | Действие | Вердикт | M-обращений | A-обращений |
|---|---|---|---|---|
| 7 | Торговля | режется | 0 | 0 |

## Решение по воротам

_Заполняется в Task 11._
