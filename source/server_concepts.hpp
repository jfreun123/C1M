#pragma once

#include <concepts>
#include <cstdint>

namespace C1M {

// An EchoServer is listening as soon as it is constructed, so connections can
// queue before run() is called. run() serves them until stop() is called,
// then closes every open connection and returns. stop() is safe to call from
// another thread or from a signal handler.
template <typename S>
concept EchoServer = requires(S server, const S constServer) {
  { constServer.port() } -> std::same_as<uint16_t>;
  server.run();
  server.stop();
};

} // namespace C1M
