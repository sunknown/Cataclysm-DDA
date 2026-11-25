#if defined( MULTIPLAYER )

#include "mp/mp_interactive.h"

namespace
{
// The resolver is the only caller and runs on one thread at a time, so plain
// state is enough; nesting is counted because mp_apply_plan may be reached from
// mp_resolve_round or directly, and a test may do either.
int g_depth = 0;
bool g_menu_cancelled = false;
} // namespace

namespace mp_interactive
{

scope::scope()
{
    if( g_depth == 0 ) {
        // A fresh resolution starts with no refusal carried over from a
        // previous one.
        g_menu_cancelled = false;
    }
    ++g_depth;
}

scope::~scope()
{
    --g_depth;
}

bool allowed()
{
    return g_depth == 0;
}

bool confirm()
{
    return true;
}

void note_menu_cancelled()
{
    if( g_depth > 0 ) {
        g_menu_cancelled = true;
    }
}

bool menu_cancelled()
{
    return g_menu_cancelled;
}

void clear_menu_cancelled()
{
    g_menu_cancelled = false;
}

} // namespace mp_interactive

#endif // MULTIPLAYER
