#if defined(MP_SERVER)

#include "mp/mp_session.h"

#include "mp/mp_context.h"
#include "mp/mp_server_input.h"

namespace
{
std::vector<std::unique_ptr<mp_session>> g_sessions;
} // namespace

std::vector<std::unique_ptr<mp_session>> &mp_sessions()
{
    return g_sessions;
}

mp_session *mp_session_of_slot( const size_t slot )
{
    for( const std::unique_ptr<mp_session> &session : g_sessions ) {
        if( session && session->context_index == slot ) {
            return session.get();
        }
    }
    return nullptr;
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
    return mp_session_of_slot( ctx->slot_index );
}

void mp_sessions_reset_for_test()
{
    // The readers go before the sessions do. A std::thread destroyed joinable
    // terminates the process, and the reader holds this session's address, so
    // every reader must be joined while its session is still alive.
    mp_server_stop_readers();
    g_sessions.clear();
}

mp_session::~mp_session()
{
    // Normally already stopped by the caller that erased the session; this is
    // the backstop that makes the guarantee structural rather than a rule every
    // new erase path has to remember.
    mp_server_stop_reader( *this );
}

#endif // MP_SERVER
