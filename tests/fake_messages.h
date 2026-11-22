#pragma once
#ifndef CATA_TESTS_FAKE_MESSAGES_H
#define CATA_TESTS_FAKE_MESSAGES_H

#include <vector>

#include "messages.h"

// Phase 4b-1: the test binary links tests/fake_messages.cpp instead of the real
// messages.cpp, so the server's snapshot has no deque to read. Tests that drive
// the body builder set the lines it should see here.
void set_fake_sidebar_snapshot( std::vector<Messages::log_message> lines );

#endif // CATA_TESTS_FAKE_MESSAGES_H
