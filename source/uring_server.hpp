#pragma once

#include "fd.hpp"
#include "net.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <liburing.h>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <system_error>
#include <utility>
#include <vector>

namespace C1M {

// One thread, completions from io_uring. The other single-threaded servers
// ask the kernel which sockets are ready and then do the I/O themselves. This
// one asks the kernel to do the I/O and is told when it is done.
//
// Each connection has exactly one operation in flight: a receive, then a send
// of what was received, then the next receive. Nothing is read while a reply
// is still being sent.
class UringServer {
public:
  explicit UringServer(const ServerConfig &config = {})
      : mListener(config, Listener::Mode::Blocking), mWake(makeWakeFd()) {}

  [[nodiscard]] uint16_t port() const noexcept { return mListener.port(); }

  void run() {
    Loop loop{mListener, mWake};
    loop.run();
  }

  void stop() noexcept { wake(mWake.get()); }

private:
  // Everything that exists only while run() does.
  class Loop {
  public:
    Loop(const Listener &listener, const Fd &wakeFd)
        : mListener(listener.fd()), mBuffers(kBuffers * kReadBufferSize) {
      if (const int error = ::io_uring_queue_init(kEntries, &mRing, 0);
          error < 0) {
        throw std::system_error(-error, std::generic_category(),
                                "io_uring_queue_init");
      }
      int error = 0;
      mBufferRing =
          ::io_uring_setup_buf_ring(&mRing, kBuffers, kBufferGroup, 0, &error);
      if (mBufferRing == nullptr) {
        ::io_uring_queue_exit(&mRing);
        throw std::system_error(-error, std::generic_category(),
                                "io_uring_setup_buf_ring");
      }
      for (uint16_t id = 0; id < kBuffers; ++id) {
        provide(id);
      }

      submitAccept();
      io_uring_sqe *entry = sqe();
      ::io_uring_prep_poll_add(entry, wakeFd.get(), POLLIN);
      tag(entry, Operation::Wake, wakeFd.get());
    }

    Loop(const Loop &) = delete;
    Loop &operator=(const Loop &) = delete;
    Loop(Loop &&) = delete;
    Loop &operator=(Loop &&) = delete;

    ~Loop() {
      // A socket with an operation in flight stays open after close() until
      // the ring lets go of it, so the clients are told here.
      for (const Connection &connection : mConnections) {
        if (connection.fd) {
          ::shutdown(connection.fd.get(), SHUT_RDWR);
        }
      }
      ::io_uring_free_buf_ring(&mRing, mBufferRing, kBuffers, kBufferGroup);
      ::io_uring_queue_exit(&mRing);
    }

    void run() {
      std::array<io_uring_cqe *, kBatch> completions{};
      for (;;) {
        // One system call hands the kernel every operation queued since the
        // last one and waits until at least one has completed.
        const int result = ::io_uring_submit_and_wait(&mRing, 1);
        if (result < 0 && result != -EINTR && result != -EBUSY) {
          throw std::system_error(-result, std::generic_category(),
                                  "io_uring_submit_and_wait");
        }
        const unsigned count =
            ::io_uring_peek_batch_cqe(&mRing, completions.data(), kBatch);
        for (const io_uring_cqe *completion :
             std::span{completions}.first(count)) {
          if (!handle(*completion)) {
            return;
          }
        }
        ::io_uring_cq_advance(&mRing, count);

        // The buffers these were waiting for came back during the batch.
        for (const int fd : std::exchange(mStarved, {})) {
          submitReceive(fd);
        }
      }
    }

  private:
    enum class Operation : uint8_t { Accept, Wake, Receive, Send, Flush };

    struct Connection {
      Fd fd;
      std::vector<std::byte> pending; // reply bytes the socket would not take
      size_t flushed = 0;             // how much of `pending` has been sent
      uint32_t received = 0;          // bytes waiting in the shared buffer
      uint16_t buffer = 0;            // which shared buffer they are in
    };

    static constexpr unsigned kEntries = 4096;
    static constexpr unsigned kBatch = 256;
    static constexpr int kBufferGroup = 0;
    // Has to be a power of two.
    static constexpr unsigned kBuffers = 256;
    static constexpr unsigned kOperationShift = 32;

    // An operation carries one number from submission to completion. This
    // one says what the operation was and which descriptor it was for.
    static void tag(io_uring_sqe *entry, Operation operation, int fd) {
      ::io_uring_sqe_set_data64(
          entry,
          (uint64_t{static_cast<uint8_t>(operation)} << kOperationShift) |
              static_cast<uint32_t>(fd));
    }

    // Returns false when it is time to stop.
    bool handle(const io_uring_cqe &completion) {
      const uint64_t data = ::io_uring_cqe_get_data64(&completion);
      const auto operation = static_cast<Operation>(data >> kOperationShift);
      const auto fd = static_cast<int>(static_cast<uint32_t>(data));
      switch (operation) {
      case Operation::Wake:
        return false;
      case Operation::Accept:
        onAccept(completion);
        break;
      case Operation::Receive:
        onReceive(fd, completion);
        break;
      case Operation::Send:
        onSend(fd, completion);
        break;
      case Operation::Flush:
        onFlush(fd, completion);
        break;
      }
      return true;
    }

    void onAccept(const io_uring_cqe &completion) {
      // One submission keeps producing connections until the kernel says
      // otherwise by leaving this flag out.
      if ((completion.flags & IORING_CQE_F_MORE) == 0) {
        submitAccept();
      }
      if (completion.res < 0) {
        // ECONNABORTED: the client gave up while it sat in the queue.
        if (completion.res == -ECONNABORTED || completion.res == -EINTR) {
          return;
        }
        throw std::system_error(-completion.res, std::generic_category(),
                                "accept");
      }
      Fd fd{completion.res};
      const auto slot = static_cast<size_t>(fd.get());
      if (slot >= mConnections.size()) {
        mConnections.resize(slot + 1);
      }
      mConnections[slot].fd = std::move(fd);
      submitReceive(completion.res);
    }

    void onReceive(int fd, const io_uring_cqe &completion) {
      const bool hasBuffer = (completion.flags & IORING_CQE_F_BUFFER) != 0;
      const auto buffer =
          static_cast<uint16_t>(completion.flags >> IORING_CQE_BUFFER_SHIFT);
      if (completion.res <= 0) {
        if (hasBuffer) {
          provide(buffer);
        }
        if (completion.res == -ENOBUFS) {
          // Every shared buffer is holding a reply that has not been sent.
          mStarved.push_back(fd);
        } else if (completion.res == -EINTR || completion.res == -EAGAIN) {
          submitReceive(fd);
        } else {
          close(fd); // 0 is the peer's FIN
        }
        return;
      }

      Connection &connection = mConnections[static_cast<size_t>(fd)];
      connection.buffer = buffer;
      connection.received = static_cast<uint32_t>(completion.res);
      // MSG_DONTWAIT: if the socket will not take the whole reply now, the
      // rest is copied out so that the shared buffer can go back.
      io_uring_sqe *entry = sqe();
      ::io_uring_prep_send(entry, fd, shared(buffer).data(),
                           connection.received, MSG_DONTWAIT | MSG_NOSIGNAL);
      tag(entry, Operation::Send, fd);
    }

    void onSend(int fd, const io_uring_cqe &completion) {
      const int result = completion.res;
      Connection &connection = mConnections[static_cast<size_t>(fd)];
      const auto data = shared(connection.buffer).first(connection.received);
      if (result < 0 && result != -EAGAIN) {
        provide(connection.buffer);
        close(fd);
        return;
      }
      const auto rest = data.subspan(static_cast<size_t>(std::max(result, 0)));
      if (rest.empty()) {
        provide(connection.buffer);
        submitReceive(fd);
        return;
      }
      connection.pending.assign(rest.begin(), rest.end());
      provide(connection.buffer);
      submitFlush(fd);
    }

    void onFlush(int fd, const io_uring_cqe &completion) {
      const int result = completion.res;
      Connection &connection = mConnections[static_cast<size_t>(fd)];
      if (result == -EINTR || result == -EAGAIN) {
        submitFlush(fd);
        return;
      }
      if (result < 0) {
        close(fd);
        return;
      }
      connection.flushed += static_cast<size_t>(result);
      if (connection.flushed < connection.pending.size()) {
        submitFlush(fd);
        return;
      }
      // Free the buffer instead of clear()ing it, so an idle connection
      // holds no memory beyond its slot.
      std::vector<std::byte>{}.swap(connection.pending);
      connection.flushed = 0;
      submitReceive(fd);
    }

    void submitAccept() {
      io_uring_sqe *entry = sqe();
      ::io_uring_prep_multishot_accept(entry, mListener, nullptr, nullptr,
                                       SOCK_CLOEXEC);
      tag(entry, Operation::Accept, mListener);
    }

    // No buffer is named. The kernel takes one from the shared ring when data
    // arrives, so a connection that is waiting for data holds none.
    void submitReceive(int fd) {
      io_uring_sqe *entry = sqe();
      ::io_uring_prep_recv(entry, fd, nullptr, kReadBufferSize, 0);
      entry->flags |= IOSQE_BUFFER_SELECT;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      entry->buf_group = kBufferGroup;
      tag(entry, Operation::Receive, fd);
    }

    // Unlike the first send, this one waits for the socket to have room.
    void submitFlush(int fd) {
      const Connection &connection = mConnections[static_cast<size_t>(fd)];
      const auto unsent =
          std::span<const std::byte>{connection.pending}.subspan(
              connection.flushed);
      io_uring_sqe *entry = sqe();
      ::io_uring_prep_send(entry, fd, unsent.data(), unsent.size(),
                           MSG_NOSIGNAL);
      tag(entry, Operation::Flush, fd);
    }

    // The next free slot in the submission queue. Nothing reaches the kernel
    // until run() submits the queue, unless the queue is full.
    io_uring_sqe *sqe() {
      io_uring_sqe *entry = ::io_uring_get_sqe(&mRing);
      if (entry == nullptr) {
        ::io_uring_submit(&mRing);
        entry = ::io_uring_get_sqe(&mRing);
      }
      if (entry == nullptr) {
        throw std::system_error(EBUSY, std::generic_category(),
                                "io_uring_get_sqe");
      }
      return entry;
    }

    [[nodiscard]] std::span<std::byte> shared(uint16_t buffer) {
      return std::span{mBuffers}.subspan(size_t{buffer} * kReadBufferSize,
                                         kReadBufferSize);
    }

    // Hand a shared buffer to the kernel for a later receive to use.
    void provide(uint16_t buffer) {
      ::io_uring_buf_ring_add(mBufferRing, shared(buffer).data(),
                              kReadBufferSize, buffer,
                              ::io_uring_buf_ring_mask(kBuffers), 0);
      ::io_uring_buf_ring_advance(mBufferRing, 1);
    }

    // Only called from a completion, when the connection has nothing in
    // flight.
    void close(int fd) { mConnections[static_cast<size_t>(fd)] = Connection{}; }

    int mListener;
    io_uring mRing{};
    io_uring_buf_ring *mBufferRing = nullptr;
    std::vector<std::byte> mBuffers;      // kBuffers of kReadBufferSize each
    std::vector<Connection> mConnections; // indexed by descriptor number
    std::vector<int> mStarved; // connections whose receive found no buffer
  };

  Listener mListener;
  Fd mWake;
};

} // namespace C1M
