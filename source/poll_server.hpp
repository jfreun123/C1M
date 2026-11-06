#pragma once

#include "echo_connection.hpp"
#include "fd.hpp"
#include "net.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <poll.h>
#include <utility>
#include <vector>

namespace C1M {

// One thread, non-blocking sockets, readiness from poll().
class PollServer {
public:
  explicit PollServer(const ServerConfig &config = {})
      : mListener(config, Listener::Mode::NonBlocking), mWake(makeWakeFd()),
        mPollFds{{.fd = mListener.fd(), .events = POLLIN, .revents = 0},
                 {.fd = mWake.get(), .events = POLLIN, .revents = 0}} {}

  [[nodiscard]] uint16_t port() const noexcept { return mListener.port(); }

  void run() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<std::byte, kReadBufferSize> buffer;
    for (;;) {
      // Unlike select(), the interest list survives the call: poll() answers
      // in `revents` and leaves `events` alone. The kernel still reads the
      // whole array on every call.
      if (::poll(mPollFds.data(), mPollFds.size(), -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        throwErrno("poll");
      }
      if (mPollFds[kWake].revents != 0) {
        break;
      }

      // Every entry is checked to find the ready ones. Backwards, so a closed
      // connection can be replaced by the last one, which has already been
      // served.
      for (size_t i = mConnections.size(); i-- > 0;) {
        pollfd &entry = mPollFds[i + kFirstConnection];
        if (entry.revents == 0) {
          continue;
        }
        if (mConnections[i].onReady(buffer, false) ==
            EchoConnection::Status::Closed) {
          std::swap(mConnections[i], mConnections.back());
          mConnections.pop_back();
          std::swap(entry, mPollFds.back());
          mPollFds.pop_back();
          continue;
        }
        entry.events = interest(mConnections[i]);
      }
      if (mPollFds[kListener].revents != 0) {
        acceptPending();
      }
    }
    mConnections.clear();
    mPollFds.resize(kFirstConnection);
  }

  void stop() noexcept { wake(mWake.get()); }

private:
  // mPollFds starts with the listener and the wake fd. After those, entry
  // i + kFirstConnection belongs to mConnections[i].
  static constexpr size_t kListener = 0;
  static constexpr size_t kWake = 1;
  static constexpr size_t kFirstConnection = 2;

  static short interest(const EchoConnection &connection) noexcept {
    return static_cast<short>(connection.blocked() ? POLLOUT : POLLIN);
  }

  void acceptPending() {
    while (Fd fd = mListener.accept()) {
      mPollFds.push_back({.fd = fd.get(), .events = POLLIN, .revents = 0});
      mConnections.emplace_back(std::move(fd));
    }
  }

  Listener mListener;
  Fd mWake;
  std::vector<pollfd> mPollFds;
  std::vector<EchoConnection> mConnections;
};

} // namespace C1M
