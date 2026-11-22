#pragma once
#ifndef CATA_SRC_LOG_SNAPSHOT_H
#define CATA_SRC_LOG_SNAPSHOT_H

#include <cstddef>
#include <string>
#include <vector>

#include "color.h"
#include "messages.h"

// One candidate line for the 4b-1 native log snapshot, already resolved from
// the message store. Keeping this a plain record lets the rules be tested
// without the real deque: the test binary links tests/fake_messages.cpp, not
// src/messages.cpp.
struct log_source_entry {
    std::string text;
    nc_color color;
    bool recent = true;
    bool hidden = false;
    bool expired = false;
};

// Newest first. Stops at the first expired entry, drops cooldown-hidden entries
// only when from_top is false, resolves inline tags for recent entries, and
// keeps at most max_messages. Mirrors Messages::display_messages.
std::vector<Messages::log_message> log_snapshot_build(
    const std::vector<log_source_entry> &newest_first, size_t max_messages, bool from_top );

#endif // CATA_SRC_LOG_SNAPSHOT_H
