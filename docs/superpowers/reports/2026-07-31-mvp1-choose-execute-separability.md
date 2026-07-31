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

## Сводка

| # | Действие | Вердикт | M-обращений | A-обращений |
|---|---|---|---|---|

## Решение по воротам

_Заполняется в Task 11._
