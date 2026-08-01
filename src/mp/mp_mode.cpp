#include "mp/mp_mode.h"

static bool mp_active = false;

bool mp_mode_active()
{
    return mp_active;
}

void mp_mode_set_active( bool active )
{
    mp_active = active;
}
