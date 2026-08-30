// The rokkd serve loop, factored out so both tools/rokkd.cpp and the tests can
// drive it. Binds a Unix domain socket and dispatches protocol messages to a
// SearchService until `stop` is set.
#pragma once

#include <atomic>
#include <string>

#include "search_service.hpp"

namespace rokkdoxx::svc {

// Returns 0 on clean shutdown, non-zero on bind/listen failure. Removes the
// socket file on exit. Poll `stop` between accepts (accept has a 200 ms timeout).
int serve(SearchService& service, const std::string& socket_path, std::atomic<bool>& stop);

// Default socket path: $XDG_RUNTIME_DIR/rokkd.sock or /tmp/rokkd-<uid>.sock.
std::string default_socket_path();

}  // namespace rokkdoxx::svc
