#pragma once

#include <unistd.h>
#include <utility>

namespace C1M {

// Owns a file descriptor and closes it on destruction.
class Fd {
public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept : mFd(fd) {}

  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;

  Fd(Fd &&other) noexcept : mFd(std::exchange(other.mFd, -1)) {}
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.mFd, -1));
    }
    return *this;
  }

  ~Fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return mFd; }
  [[nodiscard]] explicit operator bool() const noexcept { return mFd >= 0; }

  void reset(int fd = -1) noexcept {
    if (mFd >= 0) {
      ::close(mFd);
    }
    mFd = fd;
  }

private:
  int mFd = -1;
};

} // namespace C1M
