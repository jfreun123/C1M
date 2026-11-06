#pragma once

#include "fd.hpp"
#include "net.hpp"

#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unordered_set>
#include <utility>

namespace C1M {

// Blocking sockets, one thread per connection: the design the C10k problem
// was written about.
class BlockingServer {
public:
  explicit BlockingServer(const ServerConfig &config = {})
      : mListener(config, Listener::Mode::Blocking) {}

  [[nodiscard]] uint16_t port() const noexcept { return mListener.port(); }

  void run() {
    std::exception_ptr failure;
    try {
      while (Fd fd = mListener.accept()) {
        spawn(std::move(fd));
      }
    } catch (...) {
      failure = std::current_exception();
    }
    disconnectAll();
    if (failure) {
      std::rethrow_exception(failure);
    }
  }

  void stop() noexcept {
    // A thread blocked in accept() has no wake fd to watch. Shutting the
    // listener down is what makes accept() return.
    ::shutdown(mListener.fd(), SHUT_RD);
  }

private:
  // Shared with every connection thread, so a thread that is still exiting
  // never touches a server that has already been destroyed.
  struct Connections {
    std::mutex mutex;
    std::condition_variable allClosed;
    std::unordered_set<int> open;
  };

  void spawn(Fd fd) {
    const int descriptor = fd.get();
    {
      const std::lock_guard lock{mConnections->mutex};
      mConnections->open.insert(descriptor);
    }
    try {
      std::thread{serve, std::move(fd), mConnections}.detach();
    } catch (...) {
      const std::lock_guard lock{mConnections->mutex};
      mConnections->open.erase(descriptor);
      throw;
    }
  }

  static void serve(Fd fd, const std::shared_ptr<Connections> &connections) {
    // Left uninitialized on purpose: zeroing it would touch four pages of
    // every thread's stack.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<std::byte, kReadBufferSize> buffer;
    for (;;) {
      const ssize_t received =
          ::recv(fd.get(), buffer.data(), buffer.size(), 0);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        break; // 0 is the peer's FIN
      }
      const auto data = std::span{buffer}.first(static_cast<size_t>(received));
      if (!sendAll(fd.get(), data)) {
        break;
      }
    }
    {
      // Unregister before `fd` closes, so disconnectAll() never shuts down a
      // descriptor number that has been reused.
      const std::lock_guard lock{connections->mutex};
      connections->open.erase(fd.get());
    }
    connections->allClosed.notify_all();
  }

  void disconnectAll() {
    std::unique_lock lock{mConnections->mutex};
    for (const int fd : mConnections->open) {
      // Makes the thread's blocked recv() return 0.
      ::shutdown(fd, SHUT_RDWR);
    }
    mConnections->allClosed.wait(lock,
                                 [&] { return mConnections->open.empty(); });
  }

  Listener mListener;
  std::shared_ptr<Connections> mConnections = std::make_shared<Connections>();
};

} // namespace C1M
