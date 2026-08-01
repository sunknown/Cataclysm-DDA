#pragma once
#ifndef CATA_SRC_MP_MP_TETHER_H
#define CATA_SRC_MP_MP_TETHER_H

#include "coordinates.h"

// The point the reality bubble is anchored to in a co-op session: the integer
// midpoint of the live player slots, with z following slot 0 (section 5.7 of
// the design spec). Computed with integer arithmetic only.
tripoint_abs_ms mp_bubble_anchor();

// How far a player may stray from the bubble anchor. Section 5.7 of the
// design spec: the bubble is 132x132 tiles, so +/-66 from the anchor, and
// monsters/fields at the bubble edge are not simulated correctly; 24 tiles of
// margin are reserved, leaving 42.
enum class tether_state {
    ok,
    warning,
    blocked,
};

// Classifies @p proposed (a candidate absolute position, usually the tile a
// player is about to step onto) against the tether limits.
tether_state mp_tether_check( const tripoint_abs_ms &proposed );

#endif // CATA_SRC_MP_MP_TETHER_H
