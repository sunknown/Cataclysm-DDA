#if defined(MP_SERVER)

#include "mp/mp_session.h"

#include "mp/mp_context.h"

namespace
{
std::vector<std::unique_ptr<mp_session>> g_sessions;
} // namespace

std::vector<std::unique_ptr<mp_session>> &mp_sessions()
{
    return g_sessions;
}

mp_session *mp_active_session()
{
    if( mp_in_world_phase() ) {
        // The world phase: no player is active, so no client is addressed.
        // The active context pointer is preserved during this phase for code
        // that reads it (serialization, debug UI), so we can't rely on it.
        return nullptr;
    }
    const player_context *const ctx = mp_active_context_ptr();
    if( ctx == nullptr ) {
        return nullptr;
    }
    for( const std::unique_ptr<mp_session> &s : g_sessions ) {
        if( s && s->context_index == ctx->slot_index ) {
            return s.get();
        }
    }
    return nullptr;
}

void mp_sessions_reset_for_test()
{
    g_sessions.clear();
}

#endif // MP_SERVER
