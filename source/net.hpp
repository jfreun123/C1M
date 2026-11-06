#pragma once

#include "fd.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <span>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <system_error>
#include <tuple>

namespace C1M {

struct ServerConfig {
  uint16_t port = 0;       // 0 lets the kernel pick one; read it with port()
  int backlog = SOMAXCONN; // accept queue length requested from listen()
};

// Bytes requested from the kernel by a single recv.
constexpr size_t kReadBufferSize = size_t{16} * 1024;

[[noreturn]] inline void throwErrno(const char *what) {
  throw std::system_error(errno, std::generic_category(), what);
}

inline const sockaddr *asSockaddr(const sockaddr_in &address) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<const sockaddr *>(&address);
}

inline sockaddr *asSockaddr(sockaddr_in &address) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<sockaddr *>(&address);
}

// An eventfd lets another thread interrupt an event loop that is waiting for
// sockets to become ready: the loop watches it like any other descriptor.
inline Fd makeWakeFd() {
  Fd fd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  if (!fd) {
    throwErrno("eventfd");
  }
  return fd;
}

inline void wake(int wakeFd) noexcept {
  const uint64_t one = 1;
  std::ignore = ::write(wakeFd, &one, sizeof(one));
}

// One send on a non-blocking socket. Returns the bytes the kernel took, which
// is 0 when the send buffer is full, or -1 when the connection is broken.
inline ssize_t sendSome(int fd, std::span<const std::byte> data) noexcept {
  for (;;) {
    const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent >= 0) {
      return sent;
    }
    if (errno == EAGAIN) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

// Send everything on a blocking socket. Returns false when the connection is
// broken.
inline bool sendAll(int fd, std::span<const std::byte> data) noexcept {
  while (!data.empty()) {
    const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno != EINTR) {
      return false;
    }
    if (sent > 0) {
      data = data.subspan(static_cast<size_t>(sent));
    }
  }
  return true;
}

// A listening TCP socket bound to every local address.
class Listener {
public:
  enum class Mode : uint8_t { Blocking, NonBlocking };

  // Accepted sockets are created in the same mode as the listener.
  Listener(const ServerConfig &config, Mode mode)
      : mSocketFlags(SOCK_CLOEXEC |
                     (mode == Mode::NonBlocking ? SOCK_NONBLOCK : 0)),
        mFd(::socket(AF_INET, SOCK_STREAM | mSocketFlags, 0)) {
    if (!mFd) {
      throwErrno("socket");
    }
    const int enable = 1;
    if (::setsockopt(mFd.get(), SOL_SOCKET, SO_REUSEADDR, &enable,
                     sizeof(enable)) < 0) {
      throwErrno("setsockopt");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config.port);
    if (::bind(mFd.get(), asSockaddr(address), sizeof(address)) < 0) {
      throwErrno("bind");
    }
    if (::listen(mFd.get(), config.backlog) < 0) {
      throwErrno("listen");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(mFd.get(), asSockaddr(address), &length) < 0) {
      throwErrno("getsockname");
    }
    mPort = ntohs(address.sin_port);
  }

  [[nodiscard]] int fd() const noexcept { return mFd.get(); }
  [[nodiscard]] uint16_t port() const noexcept { return mPort; }

  // Take the next connection off the accept queue. Returns an empty Fd when
  // there is nothing to take: a non-blocking listener's queue is empty, or a
  // blocking listener has been shut down. Any other failure throws; running
  // out of file descriptors is the one to expect.
  [[nodiscard]] Fd accept() const {
    for (;;) {
      const int fd = ::accept4(mFd.get(), nullptr, nullptr, mSocketFlags);
      if (fd >= 0) {
        return Fd{fd};
      }
      // ECONNABORTED: the client gave up while it sat in the queue.
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      // EINVAL: shutdown() was called on the listener.
      if (errno == EAGAIN || errno == EINVAL) {
        return Fd{};
      }
      throwErrno("accept");
    }
  }

private:
  int mSocketFlags;
  Fd mFd;
  uint16_t mPort = 0;
};

} // namespace C1M
