#pragma once
#ifndef CATA_SRC_MP_MP_INTERACTIVE_H
#define CATA_SRC_MP_MP_INTERACTIVE_H

#if defined( MULTIPLAYER )

// The resolver runs a finished round on the world thread. No code path it
// invokes may block for input there: the baton belongs to the world, so a
// prompt would wait for a player who can never be asked, and a menu would park
// a thread nobody is going to grant. This module is the one place that says
// "interactive input is not available right now" and every UI that could ask
// for it consults it:
//
//  - query_yn()/query_ynq() answer confirmation prompts themselves (an
//    affirmative answer is what "the plan was committed" means for a step into
//    a dangerous tile or a vehicle its owner would object to);
//  - uilist menus refuse to open and record that a plan needed an interactive
//    choice, which the resolver turns into a fizzled plan with a party-visible
//    error;
//  - any remaining modal popup is dismissed instead of waiting for a key.
//
// The mode is scoped by mp_interactive::scope, an RAII guard around the
// resolver: it cannot leak, and a single-player or ordinary in-game prompt
// outside that scope takes its normal interactive path.
namespace mp_interactive
{

// Marks the current thread's call stack non-interactive for its lifetime.
// Nested scopes are allowed (mp_apply_plan may run under mp_resolve_round, or
// directly from a test); the mode ends when the outermost one does.
class scope
{
    public:
        scope();
        ~scope();
        scope( const scope & ) = delete;
        scope &operator=( const scope & ) = delete;
};

// Whether a prompt or a menu may wait for a human here.
bool allowed();

// The answer a confirmation prompt takes inside a non-interactive scope. Only
// meaningful when allowed() is false; the affirmative is the one that lets a
// committed action do what it says.
bool confirm();

// Records that a menu refused to open because a plan was executing. The
// resolver clears it before each action and turns it into a fizzled plan.
void note_menu_cancelled();

// Whether a menu was refused since the last clear.
bool menu_cancelled();

void clear_menu_cancelled();

} // namespace mp_interactive

#else // MULTIPLAYER

// Singleplayer and the non-multiplayer builds never enter the mode, so the
// hooks compile to the ordinary interactive answer.
namespace mp_interactive
{
class scope
{
    public:
        scope() = default;
};
inline bool allowed()
{
    return true;
}
inline bool confirm()
{
    return true;
}
inline void note_menu_cancelled() {}
inline bool menu_cancelled()
{
    return false;
}
inline void clear_menu_cancelled() {}
} // namespace mp_interactive

#endif // MULTIPLAYER

#endif // CATA_SRC_MP_MP_INTERACTIVE_H
