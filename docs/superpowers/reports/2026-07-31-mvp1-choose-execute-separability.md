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

1. `handle_action.cpp:1800` `static void fire()` — проверки годности (паралич, страх, brawler/gunshy, руки), затем при наличии огнестрельного оружия в руках — `handle_action.cpp:1850` `avatar_action::fire_wielded_weapon( you )`. Если оружия нет — fallback на ручную турель (`avatar_action::fire_turret_manual`, не относится к стрельбе с оружием в руках, в путь не включаю) либо `uilist` на `handle_action.cpp:1870` («Draw what?» — извлечь оружие из кобуры, отдельное действие, предшествующее стрельбе, а не часть неё).
2. `avatar_action.cpp:816` `avatar_action::fire_wielded_weapon` — проверки (gunmod, не-оружие, несовместимый боеприпас), без обращений к UI, затем `avatar_action.cpp:840` `you.assign_activity( aim_activity_actor::use_wielded() )` — **фаза выбора заканчивается здесь и передаёт управление активности**, mutации мира и трат moves ещё не было.
3. `activity_actor.cpp:475` `aim_activity_actor::start` — вызывает `check_gun_ability_to_shoot` (может чинить простую неисправность оружия и тратить moves через `who.mod_moves()`, но это не вызов из набора UI-функций).
4. `activity_actor.cpp:536` `aim_activity_actor::do_turn` — вызывается движком активности каждый ход; на `activity_actor.cpp:571` вызывает `target_handler::mode_fire( you, *this )`.
5. `ranged.cpp:490` `target_handler::mode_fire` — создаёт `target_ui` с `mode = Fire` и вызывает `ranged.cpp:2846` `target_ui::run()` — интерактивный цикл прицеливания/выбора цели/подтверждений (весь путь ниже находится внутри этого вызова, то есть внутри `do_turn` активности).
6. Внутри `target_ui::run()`: `action_aim()` (`ranged.cpp:3941`) и `action_aim_and_shoot()` (`ranged.cpp:3970`) вызывают `do_aim()` (`ranged.cpp:1853`), которая **тратит moves по одному (`you.mod_moves(-1)`, строка 1860) и снижает recoil на каждой итерации цикла**, пока не будет достигнут порог или не кончатся moves хода.
7. По выходу из цикла с `ExitCode::Fire` → `activity_actor.cpp:590` `aim_activity_actor::finish` — детерминированно вызывает `who.fire_gun( here, fin_trajectory.back(), gun.qty, *gun, reload_loc )` (`activity_actor.cpp:612`), которая мутирует мир (патрон, урон, шум) без единого обращения к UI (проверено: в `Character::fire_gun`, `ranged.cpp:1105-…`, нет ни одного вызова из списка).

| Обращение | Файл:строка | Класс | Обоснование |
|---|---|---|---|
| `uilist` («Draw what?») | `src/handle_action.cpp:1870` | P | Срабатывает только когда в руках нет оружия для стрельбы; выбор оружия происходит до любой траты moves и до входа в `fire_wielded_weapon`. Отдельное от собственно выстрела действие. |
| `target_handler::mode_fire` (вызов) | `src/activity_actor.cpp:571` | A | Вызывается из `aim_activity_actor::do_turn` — то есть на каждом ходу многоходовой активности `ACT_AIM`, а не один раз в фазе выбора. Запускает весь интерактивный UI прицеливания. |
| `ui_manager::redraw` | `src/ranged.cpp:2975` | A | Перерисовка внутри игрового цикла `target_ui::run()`, который выполняется внутри `do_turn` активности прицеливания. |
| `query_yn` («Really attack yourself?») | `src/ranged.cpp:3584` | A | Вызывается из `confirm_non_enemy_target()`, которую вызывают ветки `FIRE` (`ranged.cpp:3030`) и `AIMED_SHOT`/`CAREFUL_SHOT`/`PRECISE_SHOT` (`ranged.cpp:3072`) цикла `run()` — внутри активности, после того как recoil/позиция цели в этом ходу уже мутировались через `do_aim`. |
| `query_yn` («Really attack %s?», не враждебный NPC) | `src/ranged.cpp:3593` | A | Та же функция `confirm_non_enemy_target()`, тот же контекст вызова — внутри активности. |
| `query_yn` (дружественные существа на линии огня) | `src/ranged.cpp:3631` | A | `prompt_friendlies_in_lof()`, вызывается на `ranged.cpp:2954` в начале каждой итерации цикла `run()` внутри `do_turn` — после того как предыдущий ход уже потратил moves на прицеливание. |
| `uilist` (Select preferences / смена режима огня) | `src/ranged.cpp:3812` | A | `action_switch_mode()`, вызывается по ветке `SWITCH_MODE` цикла `run()` (`ranged.cpp:3015`) — внутри активности. |
| `avatar::select_ammo` → `game_menus::inv::select_ammo` (выбор боеприпаса для RELOAD_AND_SHOOT, напр. лук) | `src/ranged.cpp:3927` (вызов), определение `src/game_inventory.cpp:3281` | A | Найдено не буквальным grep (вызов идёт как `you->select_ammo(gun)`, а не текстом `game_menus::inv`), а трассировкой ветки `SWITCH_AMMO` (`ranged.cpp:3019-3023`) цикла `run()`. `game_menus::inv::select_ammo` — меню инвентаря (внутри само использует `popup` при пустом инвентаре, `game_inventory.cpp:3299`), открывается посреди прицеливания, то есть внутри активности. |

Как решалось, что вызов «на пути»: путь исполнения ACTION_FIRE прослежен по цепочке вызовов из п. Путь исполнения выше; включены только вызовы, достижимые из `fire_wielded_weapon` → `aim_activity_actor` → `target_ui::run()` в режиме `TargetMode::Fire`. Исключены вызовы, относящиеся к другим действиям, до которых код добирается только если оружие для стрельбы отсутствует (`fire_turret_manual`, «Draw what?» — кроме отдельной P-строки выше) или к другим режимам `target_ui` (Throw, Reach, TurretManual, Turrets, Spell, SelectOnly), которые используются другими действиями (метание, рукопашный охват, ручная турель, каст спелла), а не стрельбой из оружия в руках.

**Вердикт:** режется с оговоркой

**Набросок намерения:**

```cpp
struct intent_aim {
    item_location weapon;
    tripoint_abs_ms target;           // финальная точка прицеливания (fin_trajectory.back())
    int gun_mode_index;               // индекс режима огня, а не указатель
    int moves_spent_aiming;           // сколько moves фаза выбора потратила на прицеливание — решено локально игроком
    double final_recoil;              // recoil на момент решения стрелять, чтобы execute не пересчитывал do_aim
    item_location reload_loc;         // выбранный боеприпас для RELOAD_AND_SHOOT, если менялся во время прицеливания
    bool target_confirmed;            // результат confirm_non_enemy_target(), принят игроком в choose-фазе
    std::vector<character_id> accepted_losses; // кого игрок согласился рискнуть задеть (aka acceptable_losses)
};

struct intent_fire {
    item_location weapon;
    tripoint_abs_ms target;
    int gun_mode_index;
    int shots;                        // gun.qty на момент решения стрелять
    item_location reload_loc;
};
```

Поля `intent_aim` шире примера из брифа: `aim_moves_spent` из брифа переименован в `moves_spent_aiming` и дополнен `final_recoil`, потому что recoil — это накопленный результат интерактивного цикла `do_aim`, а не только счётчик потраченных moves; без него `apply_intent` не сможет детерминированно воспроизвести итог прицеливания (разброс/попадание). `target_confirmed` и `accepted_losses` — прямые аналоги существующих полей `aim_activity_actor::acceptable_losses` (`activity_actor_definitions.h:524`) и решений `confirm_non_enemy_target()`/`prompt_friendlies_in_lof()`; без них execute пришлось бы либо переспрашивать (нарушение инварианта), либо тихо считать «да», что меняет игровой баланс. `reload_loc` перенесён из решения о смене боеприпаса на лету (`SWITCH_AMMO`).

**Что мешает и как обойти:** Прицеливание — интерактивный цикл (`target_ui::run()` внутри `aim_activity_actor::do_turn`), который тратит moves по одному через `do_aim()` (`ranged.cpp:1853-1875`) и может растянуться на несколько ходов персонажа (комментарий в коде: «Time spent on aiming is determined on the go by the player», `activity_actor.cpp:485`). Поэтому стрельба не может быть одним намерением уровня «нажал — выстрелил»: она обязана распасться на «прицелиться» (интерактивно, целиком локально у выбирающего игрока, никогда не выполняется внутри `apply_intent`, поскольку не мутирует общий мир — recoil и moves персонажа до выстрела можно трактовать как приватное состояние клиента, финализируемое в момент отправки намерения) и «выстрелить» (детерминированно: `Character::fire_gun` не содержит ни одного вызова из списка UI-функций и полностью зависит только от полей, которые уже перечислены в `intent_fire`). Обнаруженные A-обращения устраняются так:
- `target_handler::mode_fire` / `ui_manager::redraw` — вся интерактивная петля прицеливания выполняется целиком в choose-фазе на клиенте инициатора; в `apply_intent` она просто не запускается.
- `query_yn` (self-attack, non-hostile NPC) — решение фиксируется один раз в choose-фазе (тем же диалогом, что и сейчас, но до отправки намерения) и передаётся полем `target_confirmed`; execute никогда не спрашивает повторно.
- `query_yn` (friendlies in line of fire) — решение фиксируется в choose-фазе и передаётся списком `accepted_losses`, который уже существует в коде как `acceptable_losses` — этот список ровно то, что нужно сериализовать.
- `uilist` (switch mode) и `select_ammo` (switch ammo) — это настройки, влияющие только на то, какие значения попадут в итоговый `intent_fire`/`intent_aim` (индекс режима, `reload_loc`); сами меню выполняются в choose-фазе до отправки намерения, execute получает уже готовые индексы.

Остаточный риск (причина «с оговоркой», а не «режется»): recoil/`final_recoil` и `moves_spent_aiming` вычисляются клиентом асинхронно, в течение нескольких ходов, пока другие пиры могут одновременно применять свои намерения к тем же существам на линии огня. Если цель успеет умереть или сдвинуться между началом прицеливания и применением `intent_fire`, `apply_intent` должен либо детерминированно переигрывать промах (используя сохранённую точку `target`, а не идентификатор существа), либо намерение должно быть отклонено валидацией — это решение уже не про вызовы UI, а про правило валидации устаревших намерений, которое должно быть явно специфицировано отдельно.

## Сводка

| # | Действие | Вердикт | M-обращений | A-обращений |
|---|---|---|---|---|

## Решение по воротам

_Заполняется в Task 11._
