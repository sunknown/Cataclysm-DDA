#if defined(MP_SERVER)

#include "mp/mp_session_ui.h"

#include "mp/mp_protocol.h"
#include "mp/mp_server_input.h"
#include "mp/mp_session.h"

void mp_session_ui_enter( mp_session &s )
{
    s.ui_active = true;
}

void mp_session_ui_leave( mp_session &s )
{
    s.ui_active = false;
    // The hover state is shared with the world thread, which dispatches a hover
    // for this seat while this one may be leaving: clear it through the input
    // module's helper, under the same lock every other access takes.
    mp_server_describe_clear( s );
}

bool mp_session_ui_active( const mp_session &s )
{
    return s.ui_active;
}

bool mp_session_can_describe( const mp_session &s )
{
    return ( s.caps & MP_CAP_DESCRIBE ) != 0 && s.ui_active;
}

#endif // MP_SERVER
