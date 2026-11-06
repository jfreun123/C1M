#pragma once

#include "fd.hpp"
#include "net.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace C1M {

// One client of a non-blocking echo server. Every readiness-based server
// (select, poll, epoll) drives its connections through this class, so the
// servers differ only in how they learn that a socket is ready.
class EchoConnection {
public:
  enum class Status : uint8_t { Open, Closed };

  EchoConnection() noexcept = default;
  explicit EchoConnection(Fd fd) noexcept : mFd(std::move(fd)) {}

  [[nodiscard]] int fd() const noexcept { return mFd.get(); }

  // True while part of a reply is waiting for room in the socket's send
  // buffer. A blocked connection needs to hear when the socket is writable;
  // any other connection needs to hear when it is readable.
  [[nodiscard]] bool blocked() const noexcept { return !mPending.empty(); }

  // Make progress after the kernel reported the socket ready. `drain` keeps
  // reading until the socket is empty, which edge-triggered notification
  // requires: it does not report again for bytes that were left behind.
  Status onReady(std::span<std::byte> buffer, bool drain) {
    if (blocked()) {
      if (!flush()) {
        return Status::Closed;
      }
      // Nothing more is read while a reply is pending, so a client that
      // stops reading eventually stops being read from.
      if (blocked() || !drain) {
        return Status::Open;
      }
    }
    return echo(buffer, drain);
  }

private:
  // Send as much of the pending reply as the socket takes. Returns false when
  // the connection is broken.
  bool flush() {
    const auto unsent = std::span<const std::byte>{mPending}.subspan(mFlushed);
    const ssize_t sent = sendSome(mFd.get(), unsent);
    if (sent < 0) {
      return false;
    }
    mFlushed += static_cast<size_t>(sent);
    if (mFlushed == mPending.size()) {
      // Free the buffer instead of clear()ing it, so an idle connection holds
      // no memory beyond this object.
      std::vector<std::byte>{}.swap(mPending);
      mFlushed = 0;
    }
    return true;
  }

  Status echo(std::span<std::byte> buffer, bool drain) {
    for (;;) {
      const ssize_t received =
          ::recv(mFd.get(), buffer.data(), buffer.size(), 0);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received < 0 && errno == EAGAIN) {
        return Status::Open;
      }
      if (received <= 0) {
        return Status::Closed; // 0 is the peer's FIN
      }

      const auto data = buffer.first(static_cast<size_t>(received));
      const ssize_t sent = sendSome(mFd.get(), data);
      if (sent < 0) {
        return Status::Closed;
      }
      const auto rest = data.subspan(static_cast<size_t>(sent));
      if (!rest.empty()) {
        mPending.assign(rest.begin(), rest.end());
        return Status::Open;
      }
      if (!drain) {
        return Status::Open;
      }
    }
  }

  Fd mFd;
  std::vector<std::byte> mPending; // reply bytes the socket would not take
  size_t mFlushed = 0;             // how much of mPending has been sent since
};

} // namespace C1M
