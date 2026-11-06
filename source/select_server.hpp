#pragma once

#include "echo_connection.hpp"
#include "fd.hpp"
#include "net.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <utility>
#include <vector>

namespace C1M {

// One thread, non-blocking sockets, readiness from select().
class SelectServer {
public:
  explicit SelectServer(const ServerConfig &config = {})
      : mListener(config, Listener::Mode::NonBlocking), mWake(makeWakeFd()) {}

  [[nodiscard]] uint16_t port() const noexcept { return mListener.port(); }

  void run() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<std::byte, kReadBufferSize> buffer;
    for (;;) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
      Sets sets;
      const int highest = fill(sets);
      if (::select(highest + 1, &sets.readable, &sets.writable, nullptr,
                   nullptr) < 0) {
        if (errno == EINTR) {
          continue;
        }
        throwErrno("select");
      }
      if (FD_ISSET(mWake.get(), &sets.readable)) {
        break;
      }
      serveReady(sets, buffer);
      if (FD_ISSET(mListener.fd(), &sets.readable)) {
        acceptPending();
      }
    }
    mConnections.clear();
  }

  void stop() noexcept { wake(mWake.get()); }

private:
  struct Sets {
    fd_set readable;
    fd_set writable;
  };

  // select() overwrites the sets it is given with its answer, so both are
  // rebuilt from every connection before every call. Returns the highest
  // descriptor in either set, which select() needs to be told.
  int fill(Sets &sets) const {
    FD_ZERO(&sets.readable);
    FD_ZERO(&sets.writable);
    FD_SET(mListener.fd(), &sets.readable);
    FD_SET(mWake.get(), &sets.readable);
    int highest = std::max(mListener.fd(), mWake.get());
    for (const EchoConnection &connection : mConnections) {
      FD_SET(connection.fd(),
             connection.blocked() ? &sets.writable : &sets.readable);
      highest = std::max(highest, connection.fd());
    }
    return highest;
  }

  // select() returns how many descriptors are ready, not which, so every
  // connection is checked. Backwards, so a closed connection can be replaced
  // by the last one, which has already been served.
  void serveReady(const Sets &sets, std::span<std::byte> buffer) {
    for (size_t i = mConnections.size(); i-- > 0;) {
      const int fd = mConnections[i].fd();
      if (!FD_ISSET(fd, &sets.readable) && !FD_ISSET(fd, &sets.writable)) {
        continue;
      }
      if (mConnections[i].onReady(buffer, false) ==
          EchoConnection::Status::Closed) {
        std::swap(mConnections[i], mConnections.back());
        mConnections.pop_back();
      }
    }
  }

  void acceptPending() {
    while (Fd fd = mListener.accept()) {
      // An fd_set is a fixed array of FD_SETSIZE bits indexed by descriptor
      // number. FD_SET on a larger number writes past the end of it.
      if (fd.get() >= FD_SETSIZE) {
        throw std::runtime_error("select: descriptor " +
                                 std::to_string(fd.get()) +
                                 " does not fit in an fd_set (FD_SETSIZE is " +
                                 std::to_string(FD_SETSIZE) + ")");
      }
      mConnections.emplace_back(std::move(fd));
    }
  }

  Listener mListener;
  Fd mWake;
  std::vector<EchoConnection> mConnections;
};

} // namespace C1M
