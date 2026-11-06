// Opens many connections to an echo server, sends requests across them at a
// fixed rate, and reports what happened. One thread, driven by epoll.

// c1m
#include "args.hpp"
#include "fd.hpp"
#include "net.hpp"

// std
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::seconds;

constexpr std::string_view kUsage =
    "usage: load_gen --port N [--host IPV4] [--connections N]\n"
    "                [--connect-concurrency N] [--connect-timeout SECONDS]\n"
    "                [--rate REQUESTS_PER_SECOND] [--duration SECONDS]\n"
    "                [--size BYTES] [--hold SECONDS]\n"
    "                [--source IPV4|IPV4-IPV4[,...]]\n";

struct Options {
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  size_t connections = 1000;
  size_t connectConcurrency = 256; // connects in flight at any one time
  unsigned connectTimeout = 10;    // seconds before a connect counts as failed
  size_t rate = 1000;              // requests per second, over all connections
  unsigned duration = 5;           // seconds of sending requests
  size_t size = 64;                // bytes per request
  unsigned hold = 0;               // seconds to stay connected afterwards
  std::vector<in_addr> sources;    // addresses to connect from, in turn
};

// Enough addresses for four billion connections, and a mistyped range stops
// short of allocating that many.
constexpr size_t kMaxSources = size_t{1} << 16;

// One IPv4 address, or two joined by `-` for every address between them.
std::optional<std::vector<in_addr>> parseAddresses(std::string_view item) {
  const auto parse = [](std::string_view text) -> std::optional<uint32_t> {
    in_addr address{};
    if (::inet_pton(AF_INET, std::string{text}.c_str(), &address) != 1) {
      return std::nullopt;
    }
    return ntohl(address.s_addr);
  };
  const size_t dash = item.find('-');
  const auto first = parse(item.substr(0, dash));
  const auto last =
      dash == std::string_view::npos ? first : parse(item.substr(dash + 1));
  if (!first || !last || *last < *first || *last - *first >= kMaxSources) {
    return std::nullopt;
  }
  std::vector<in_addr> addresses;
  for (uint64_t host = *first; host <= *last; ++host) {
    addresses.push_back(in_addr{.s_addr = htonl(static_cast<uint32_t>(host))});
  }
  return addresses;
}

// A comma-separated list of what parseAddresses() takes.
std::optional<std::vector<in_addr>> parseSources(std::string_view list) {
  std::vector<in_addr> sources;
  for (size_t start = 0; start <= list.size();) {
    const size_t comma = std::min(list.find(',', start), list.size());
    const auto addresses = parseAddresses(list.substr(start, comma - start));
    if (!addresses || sources.size() + addresses->size() > kMaxSources) {
      return std::nullopt;
    }
    sources.insert(sources.end(), addresses->begin(), addresses->end());
    start = comma + 1;
  }
  return sources;
}

std::optional<Options> parseOptions(std::span<const char *const> args) {
  const auto flags = C1M::parseFlags(args.subspan(1));
  if (!flags) {
    return std::nullopt;
  }
  Options options;
  for (const auto &[name, value] : *flags) {
    bool valid = false;
    if (name == "--host") {
      options.host = value;
      valid = true;
    } else if (name == "--port") {
      valid = C1M::assignNumber(options.port, value);
    } else if (name == "--connections") {
      valid = C1M::assignNumber(options.connections, value);
    } else if (name == "--connect-concurrency") {
      valid = C1M::assignNumber(options.connectConcurrency, value) &&
              options.connectConcurrency > 0;
    } else if (name == "--connect-timeout") {
      valid = C1M::assignNumber(options.connectTimeout, value);
    } else if (name == "--rate") {
      valid = C1M::assignNumber(options.rate, value);
    } else if (name == "--duration") {
      valid = C1M::assignNumber(options.duration, value);
    } else if (name == "--size") {
      valid = C1M::assignNumber(options.size, value) && options.size > 0;
    } else if (name == "--hold") {
      valid = C1M::assignNumber(options.hold, value);
    } else if (name == "--source") {
      auto sources = parseSources(value);
      valid = sources.has_value();
      if (valid) {
        options.sources = std::move(*sources);
      }
    }
    if (!valid) {
      return std::nullopt;
    }
  }
  if (options.port == 0) {
    return std::nullopt;
  }
  return options;
}

constexpr size_t kMedian = 50;
constexpr size_t kTail = 99;
constexpr size_t kMaxPercent = 100;

std::string summarize(std::vector<Clock::duration> samples) {
  if (samples.empty()) {
    return "no samples";
  }
  std::ranges::sort(samples);
  const auto percentile = [&](size_t percent) {
    const size_t index = (samples.size() - 1) * percent / kMaxPercent;
    return duration_cast<std::chrono::microseconds>(samples[index]).count();
  };
  return std::format("p50 {} us, p99 {} us, max {} us", percentile(kMedian),
                     percentile(kTail), percentile(kMaxPercent));
}

double toSeconds(Clock::duration duration) {
  return duration_cast<std::chrono::duration<double>>(duration).count();
}

class LoadGenerator {
public:
  explicit LoadGenerator(const Options &options)
      : mOptions(options), mEpoll(::epoll_create1(EPOLL_CLOEXEC)),
        mClients(options.connections), mPayload(options.size, std::byte{'x'}) {
    if (!mEpoll) {
      C1M::throwErrno("epoll_create1");
    }
    mTarget.sin_family = AF_INET;
    mTarget.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, mOptions.host.c_str(), &mTarget.sin_addr) != 1) {
      throw std::invalid_argument("not an IPv4 address: " + mOptions.host);
    }
  }

  // Returns once every connection has either been established or failed.
  void connectAll() {
    const auto start = Clock::now();
    size_t next = 0;
    while (mEstablished + mFailed < mClients.size()) {
      while (mInFlight < mOptions.connectConcurrency &&
             next < mClients.size()) {
        startConnect(next++);
      }
      pump(Clock::now() + kTick);
      expireConnects();
    }
    mConnectElapsed = Clock::now() - start;
  }

  // Sends requests round-robin over the connections at the configured rate.
  // The schedule does not wait for replies, so a slow server cannot slow the
  // test down. Latency is measured from the moment a request is really sent.
  // How far behind schedule that was is reported separately, because that
  // part is this program waking up late and not the server.
  void sendRequests() {
    if (mEstablished == 0 || mOptions.rate == 0 || mOptions.duration == 0) {
      return;
    }
    const Clock::duration interval =
        duration_cast<Clock::duration>(seconds{1}) / mOptions.rate;
    const auto start = Clock::now();
    const auto end = start + seconds{mOptions.duration};
    auto due = start;
    size_t cursor = 0;
    while (Clock::now() < end) {
      // Bounded, so replies still get read when sending is behind schedule.
      for (size_t burst = 0; burst < kMaxBurst && due <= Clock::now();
           ++burst) {
        request(cursor, due);
        cursor = (cursor + 1) % mClients.size();
        due += interval;
      }
      pump(std::min(due, end));
    }

    const auto deadline = Clock::now() + kStragglerGrace;
    while (mAwaiting > 0 && Clock::now() < deadline) {
      pump(deadline);
    }
    mRequestElapsed = Clock::now() - start;
  }

  // Stay connected, doing nothing, so the server can be inspected.
  void hold() {
    const auto end = Clock::now() + seconds{mOptions.hold};
    while (Clock::now() < end) {
      pump(end);
    }
  }

  // Prints the report. Returns true if nothing went wrong.
  [[nodiscard]] bool report() const {
    std::cout << std::format(
        "connect:   {} requested, {} established, {} failed in {:.3f} s\n",
        mClients.size(), mEstablished, mFailed, toSeconds(mConnectElapsed));
    for (const auto &[error, count] : mFailures) {
      std::cout << std::format("           {} x {}\n", count,
                               std::strerror(error));
    }
    std::cout << std::format("           time to connect: {}\n",
                             summarize(mConnectTimes));

    const size_t unanswered = mSent - mLatencies.size();
    std::cout << std::format(
        "requests:  {} sent, {} answered, {} unanswered, {} skipped in {:.2f} "
        "s\n",
        mSent, mLatencies.size(), unanswered, mSkipped,
        toSeconds(mRequestElapsed));
    std::cout << std::format("           latency: {}\n", summarize(mLatencies));
    std::cout << std::format("           sent behind schedule by: {}\n",
                             summarize(mLateness));
    std::cout << std::format("dropped:   {} connections closed by the server\n",
                             mDropped);
    return mFailed == 0 && unanswered == 0 && mDropped == 0;
  }

private:
  enum class State : uint8_t { Unused, Connecting, Idle, Awaiting, Closed };

  struct Client {
    C1M::Fd fd;
    State state = State::Unused;
    size_t awaiting = 0;     // bytes of the reply still to come
    Clock::time_point since; // when the connect or the request started
  };

  static constexpr auto kTick = std::chrono::milliseconds{100};
  static constexpr auto kStragglerGrace = seconds{1};
  static constexpr size_t kMaxBurst = 1024;
  static constexpr int kMaxEvents = 1024;

  // The port is left for connect() to pick: chosen here, it would have to be
  // unique among every socket on the address, whatever each one connects to,
  // and the search for one gets slow as they run out.
  static bool bindSource(int fd, in_addr source) {
    const int enable = 1;
    if (::setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &enable,
                     sizeof(enable)) < 0) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr = source;
    return ::bind(fd, C1M::asSockaddr(address), sizeof(address)) == 0;
  }

  void startConnect(size_t index) {
    Client &client = mClients[index];
    client.since = Clock::now();
    client.fd.reset(
        ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!client.fd) {
      fail(client, errno);
      return;
    }
    // Source addresses are taken in turn.
    if (!mOptions.sources.empty() &&
        !bindSource(client.fd.get(),
                    mOptions.sources[index % mOptions.sources.size()])) {
      fail(client, errno);
      return;
    }
    // Registered before connect() so that no event can be missed.
    epoll_event event{};
    event.events = EPOLLOUT;
    event.data.u64 = index; // NOLINT(cppcoreguidelines-pro-type-union-access)
    if (::epoll_ctl(mEpoll.get(), EPOLL_CTL_ADD, client.fd.get(), &event) < 0) {
      C1M::throwErrno("epoll_ctl");
    }

    client.state = State::Connecting;
    ++mInFlight;
    mConnecting.push_back(index);
    if (::connect(client.fd.get(), C1M::asSockaddr(mTarget), sizeof(mTarget)) ==
        0) {
      establish(index);
    } else if (errno != EINPROGRESS) {
      fail(client, errno);
    }
  }

  // A connect in progress reports writable when it has finished either way;
  // SO_ERROR says which.
  void finishConnect(size_t index) {
    Client &client = mClients[index];
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(client.fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) <
        0) {
      error = errno;
    }
    if (error == 0) {
      establish(index);
    } else {
      fail(client, error);
    }
  }

  void establish(size_t index) {
    Client &client = mClients[index];
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = index; // NOLINT(cppcoreguidelines-pro-type-union-access)
    if (::epoll_ctl(mEpoll.get(), EPOLL_CTL_MOD, client.fd.get(), &event) < 0) {
      C1M::throwErrno("epoll_ctl");
    }
    client.state = State::Idle;
    --mInFlight;
    mConnectTimes.push_back(Clock::now() - client.since);
    ++mEstablished;
  }

  void fail(Client &client, int error) {
    if (client.state == State::Connecting) {
      --mInFlight;
    }
    client.fd.reset();
    client.state = State::Closed;
    ++mFailures[error];
    ++mFailed;
  }

  // Fail the connects that have been in flight for too long. Entries that
  // have already finished are discarded on the way.
  void expireConnects() {
    const auto cutoff = Clock::now() - seconds{mOptions.connectTimeout};
    while (!mConnecting.empty()) {
      Client &client = mClients[mConnecting.front()];
      if (client.state == State::Connecting) {
        if (client.since > cutoff) {
          break;
        }
        fail(client, ETIMEDOUT);
      }
      mConnecting.pop_front();
    }
  }

  void request(size_t index, Clock::time_point due) {
    Client &client = mClients[index];
    if (client.state != State::Idle) {
      // Still waiting for its previous reply, or gone.
      ++mSkipped;
      return;
    }
    if (C1M::sendSome(client.fd.get(), mPayload) !=
        static_cast<ssize_t>(mPayload.size())) {
      drop(client);
      return;
    }
    client.state = State::Awaiting;
    client.awaiting = mPayload.size();
    client.since = Clock::now();
    mLateness.push_back(client.since - due);
    ++mSent;
    ++mAwaiting;
  }

  void receive(Client &client) {
    const ssize_t received =
        ::recv(client.fd.get(), mBuffer.data(), mBuffer.size(), 0);
    if (received < 0 && (errno == EAGAIN || errno == EINTR)) {
      return;
    }
    if (received <= 0) {
      drop(client);
      return;
    }
    if (client.state != State::Awaiting) {
      return;
    }
    client.awaiting -= std::min(client.awaiting, static_cast<size_t>(received));
    if (client.awaiting == 0) {
      mLatencies.push_back(Clock::now() - client.since);
      client.state = State::Idle;
      --mAwaiting;
    }
  }

  // The server closed or reset an established connection.
  void drop(Client &client) {
    if (client.state == State::Awaiting) {
      --mAwaiting;
    }
    client.fd.reset();
    client.state = State::Closed;
    ++mDropped;
  }

  // Handle socket events until `deadline`, or until one batch of them has
  // been handled, whichever comes first.
  void pump(Clock::time_point deadline) {
    const auto wait =
        std::max(deadline - Clock::now(), Clock::duration::zero());
    const auto whole = duration_cast<seconds>(wait);
    const timespec timeout{
        .tv_sec = static_cast<time_t>(whole.count()),
        .tv_nsec = static_cast<long>(
            duration_cast<std::chrono::nanoseconds>(wait - whole).count())};
    // epoll_wait() takes its timeout in milliseconds, which is too coarse to
    // pace requests with.
    const int ready = ::epoll_pwait2(mEpoll.get(), mEvents.data(), kMaxEvents,
                                     &timeout, nullptr);
    if (ready < 0) {
      if (errno == EINTR) {
        return;
      }
      C1M::throwErrno("epoll_pwait2");
    }
    for (const epoll_event &event :
         std::span{mEvents}.first(static_cast<size_t>(ready))) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
      const auto index = static_cast<size_t>(event.data.u64);
      Client &client = mClients[index];
      if (client.state == State::Connecting) {
        finishConnect(index);
      } else if (client.state == State::Idle ||
                 client.state == State::Awaiting) {
        receive(client);
      }
    }
  }

  Options mOptions;
  C1M::Fd mEpoll;
  sockaddr_in mTarget{};
  std::vector<Client> mClients;
  std::vector<std::byte> mPayload;
  std::deque<size_t> mConnecting; // every connect started, oldest first
  std::array<epoll_event, kMaxEvents> mEvents{};
  std::array<std::byte, C1M::kReadBufferSize> mBuffer{};

  size_t mInFlight = 0;
  size_t mEstablished = 0;
  size_t mFailed = 0;
  size_t mSent = 0;
  size_t mAwaiting = 0;
  size_t mSkipped = 0;
  size_t mDropped = 0;
  std::map<int, size_t> mFailures; // errno -> how many connects failed with it
  std::vector<Clock::duration> mConnectTimes;
  std::vector<Clock::duration> mLatencies;
  std::vector<Clock::duration> mLateness;
  Clock::duration mConnectElapsed{};
  Clock::duration mRequestElapsed{};
};

} // namespace

int main(int argc, const char **argv) {
  try {
    const auto options =
        parseOptions(std::span{argv, static_cast<size_t>(argc)});
    if (!options) {
      std::cerr << kUsage;
      return 2;
    }
    LoadGenerator generator{*options};
    generator.connectAll();
    generator.sendRequests();
    generator.hold();
    return generator.report() ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "load_gen: " << e.what() << "\n";
    return 1;
  }
}
