// RUN: %check_clang_tidy %s cata-no-avatar-in-simulation %t -- --load=%cata_plugin -config="{CheckOptions: [{key: cata-no-avatar-in-simulation.SimulationFiles, value: 'unrelated.cpp;%basename_t.tmp.cpp'}]}" --
// %check_clang_tidy cannot assert silence: FileCheck refuses the empty output.
// RUN: clang-tidy %s --checks='-*,cata-no-avatar-in-simulation' --load=%cata_plugin -- -std=c++17 2>&1 | FileCheck --allow-empty --check-prefix=CHECK-ELSEWHERE %s

class avatar;
class Character;

avatar &get_avatar();
avatar &get_avatar_legacy();
Character &get_player_character();

namespace other
{
avatar &get_avatar();
} // namespace other

void f()
{
    get_avatar();
    // CHECK-MESSAGES: [[@LINE-1]]:5: warning: 'get_avatar' is whichever player is active, and simulation code also runs in the world phase, where that is always slot 0.  Use a multi-player accessor such as game::nearest_player, or get_avatar_legacy() to keep the single-player meaning on purpose. [cata-no-avatar-in-simulation]
    get_player_character();
    // CHECK-MESSAGES: [[@LINE-1]]:5: warning: 'get_player_character' is whichever player is active, and simulation code also runs in the world phase, where that is always slot 0.  Use a multi-player accessor such as game::nearest_player, or get_avatar_legacy() to keep the single-player meaning on purpose. [cata-no-avatar-in-simulation]
    avatar &( *getter )() = get_avatar;
    // CHECK-MESSAGES: [[@LINE-1]]:29: warning: 'get_avatar' is whichever player is active, and simulation code also runs in the world phase, where that is always slot 0.  Use a multi-player accessor such as game::nearest_player, or get_avatar_legacy() to keep the single-player meaning on purpose. [cata-no-avatar-in-simulation]
    getter();
    get_avatar_legacy();
    other::get_avatar();
}

// Outside the configured simulation files the same calls are fine.
// CHECK-ELSEWHERE-NOT: warning:
