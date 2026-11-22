#if defined(MP_SERVER)

#include "mp/mp_session_ui.h"

#include "mp/mp_protocol.h"
#include "mp/mp_session.h"

void mp_session_ui_enter( mp_session &s )
{
    s.ui_active = true;
}

void mp_session_ui_leave( mp_session &s )
{
    s.ui_active = false;
    s.describe_pending.reset();
    s.describe_last = {};
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
