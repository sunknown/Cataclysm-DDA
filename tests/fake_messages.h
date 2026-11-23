#pragma once
#ifndef CATA_TESTS_FAKE_MESSAGES_H
#define CATA_TESTS_FAKE_MESSAGES_H

#include <string>
#include <vector>

#include "messages.h"

// Phase 4b-1: the test binary links tests/fake_messages.cpp instead of the real
// messages.cpp, so the server's snapshot has no deque to read. Tests that drive
// the body builder set the lines it should see here.
void set_fake_sidebar_snapshot( std::vector<Messages::log_message> lines );

// The pair the Character hook records. The test binary has no real store, so
// the hook's arguments are captured here for assertions.
struct FakeMessagePair {
    std::string player;
    std::string npc;
    std::string actor_id;
    std::string actor_display;
};

const std::vector<FakeMessagePair> &fake_message_pairs();
void clear_fake_message_pairs();

// Viewer-aware snapshot stub for the body builder. A viewer without an entry
// falls back to set_fake_sidebar_snapshot's lines.
void set_fake_sidebar_snapshot_for( const std::string &viewer_id,
                                    std::vector<Messages::log_message> lines );

#endif // CATA_TESTS_FAKE_MESSAGES_H
