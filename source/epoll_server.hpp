#pragma once

#include "echo_connection.hpp"
#include "fd.hpp"
#include "net.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <sys/epoll.h>
#include <utility>
#include <vector>

namespace C1M {

enum class Trigger : uint8_t { Level, Edge };

// One thread, non-blocking sockets, readiness from epoll.
class EpollServer {
public:
  explicit EpollServer(const ServerConfig &config = {},
                       Trigger trigger = Trigger::Level)
      : mListener(config, Listener::Mode::NonBlocking), mWake(makeWakeFd()),
        mEpoll(::epoll_create1(EPOLL_CLOEXEC)), mTrigger(trigger) {
    if (!mEpoll) {
      throwErrno("epoll_create1");
    }
    watch(EPOLL_CTL_ADD, mListener.fd(), EPOLLIN);
    watch(EPOLL_CTL_ADD, mWake.get(), EPOLLIN);
  }

  [[nodiscard]] uint16_t port() const noexcept { return mListener.port(); }

  void run() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<std::byte, kReadBufferSize> buffer;
    std::array<epoll_event, kMaxEvents> events{};
    for (;;) {
      // The interest list lives in the kernel, so nothing is passed in, and
      // only descriptors that are ready come back.
      const int ready =
          ::epoll_wait(mEpoll.get(), events.data(), kMaxEvents, -1);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throwErrno("epoll_wait");
      }
      for (const epoll_event &event :
           std::span{events}.first(static_cast<size_t>(ready))) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        const int fd = event.data.fd;
        if (fd == mWake.get()) {
          mConnections.clear();
          return;
        }
        if (fd == mListener.fd()) {
          acceptPending();
        } else {
          serve(fd, buffer);
        }
      }
    }
  }

  void stop() noexcept { wake(mWake.get()); }

private:
  static constexpr int kMaxEvents = 256;

  // What the kernel should report for a connection.
  [[nodiscard]] uint32_t interest(const EchoConnection &connection) const {
    if (mTrigger == Trigger::Edge) {
      // Registered once and never changed. Each event means "something
      // changed since the last one", so serve() has to use the socket until
      // it returns EAGAIN.
      return EPOLLIN | EPOLLOUT | EPOLLET;
    }
    return connection.blocked() ? EPOLLOUT : EPOLLIN;
  }

  // Same parameter order as epoll_ctl().
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void watch(int operation, int fd, uint32_t events) const {
    epoll_event event{};
    event.events = events;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    event.data.fd = fd;
    if (::epoll_ctl(mEpoll.get(), operation, fd, &event) < 0) {
      throwErrno("epoll_ctl");
    }
  }

  void acceptPending() {
    while (Fd fd = mListener.accept()) {
      // The kernel hands out the lowest free descriptor number, so the
      // numbers stay dense enough to index a vector with.
      const auto slot = static_cast<size_t>(fd.get());
      if (slot >= mConnections.size()) {
        mConnections.resize(slot + 1);
      }
      mConnections[slot] = EchoConnection{std::move(fd)};
      watch(EPOLL_CTL_ADD, mConnections[slot].fd(),
            interest(mConnections[slot]));
    }
  }

  void serve(int fd, std::span<std::byte> buffer) {
    EchoConnection &connection = mConnections[static_cast<size_t>(fd)];
    const bool wasBlocked = connection.blocked();
    if (connection.onReady(buffer, mTrigger == Trigger::Edge) ==
        EchoConnection::Status::Closed) {
      // Closing the descriptor also removes it from the interest list.
      connection = EchoConnection{};
      return;
    }
    if (mTrigger == Trigger::Level && connection.blocked() != wasBlocked) {
      watch(EPOLL_CTL_MOD, fd, interest(connection));
    }
  }

  Listener mListener;
  Fd mWake;
  Fd mEpoll;
  Trigger mTrigger;
  std::vector<EchoConnection> mConnections; // indexed by descriptor number
};

} // namespace C1M
