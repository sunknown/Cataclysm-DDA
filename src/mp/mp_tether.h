#pragma once
#ifndef CATA_SRC_MP_MP_TETHER_H
#define CATA_SRC_MP_MP_TETHER_H

#include "coordinates.h"

// The point the reality bubble is anchored to in a co-op session: the integer
// midpoint of the live player slots, with z following slot 0 (section 5.7 of
// the design spec). Computed with integer arithmetic only.
tripoint_abs_ms mp_bubble_anchor();

#endif // CATA_SRC_MP_MP_TETHER_H
