// c1m
#include "args.hpp"
#include "blocking_server.hpp"
#include "epoll_server.hpp"
#include "net.hpp"
#include "poll_server.hpp"
#include "select_server.hpp"
#include "server_concepts.hpp"
#include "uring_server.hpp"

// std
#include <cerrno>
#include <csignal>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::string_view kUsage =
    "usage: echo_server <threads|select|poll|epoll|epoll-et|io_uring> "
    "[--port N] [--backlog N]\n";

struct Options {
  std::string_view model;
  C1M::ServerConfig config;
};

std::optional<Options> parseOptions(std::span<const char *const> args) {
  if (args.size() < 2) {
    return std::nullopt;
  }
  Options options{.model = args[1], .config = {}};
  const auto flags = C1M::parseFlags(args.subspan(2));
  if (!flags) {
    return std::nullopt;
  }
  for (const auto &[name, value] : *flags) {
    bool valid = false;
    if (name == "--port") {
      valid = C1M::assignNumber(options.config.port, value);
    } else if (name == "--backlog") {
      valid = C1M::assignNumber(options.config.backlog, value);
    }
    if (!valid) {
      return std::nullopt;
    }
  }
  return options;
}

// Calls stop() on a server when SIGINT or SIGTERM arrives, for as long as
// this object exists. A handler is used instead of a thread waiting in
// sigwait() so that the event-loop servers really are single-threaded
// processes.
template <C1M::EchoServer Server> class StopOnSignal {
public:
  explicit StopOnSignal(Server &server) {
    sServer = &server;
    std::signal(SIGINT, handle);
    std::signal(SIGTERM, handle);
  }

  StopOnSignal(const StopOnSignal &) = delete;
  StopOnSignal &operator=(const StopOnSignal &) = delete;
  StopOnSignal(StopOnSignal &&) = delete;
  StopOnSignal &operator=(StopOnSignal &&) = delete;

  ~StopOnSignal() {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    sServer = nullptr;
  }

private:
  static void handle(int /*signal*/) {
    // stop() makes one system call, which may overwrite the errno of whatever
    // the signal interrupted.
    const int saved = errno;
    sServer->stop();
    errno = saved;
  }

  // A signal handler has no other way to reach the server.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static inline Server *sServer = nullptr;
};

// Run the server until SIGINT or SIGTERM arrives.
template <C1M::EchoServer Server, typename... Args>
void serve(const Options &options, Args &&...args) {
  Server server{options.config, std::forward<Args>(args)...};
  std::cout << "echo_server: " << options.model << " listening on port "
            << server.port() << " (pid " << ::getpid() << ")" << std::endl;
  const StopOnSignal<Server> stopOnSignal{server};
  server.run();
}

bool run(const Options &options) {
  if (options.model == "threads") {
    serve<C1M::BlockingServer>(options);
  } else if (options.model == "select") {
    serve<C1M::SelectServer>(options);
  } else if (options.model == "poll") {
    serve<C1M::PollServer>(options);
  } else if (options.model == "epoll") {
    serve<C1M::EpollServer>(options, C1M::Trigger::Level);
  } else if (options.model == "epoll-et") {
    serve<C1M::EpollServer>(options, C1M::Trigger::Edge);
  } else if (options.model == "io_uring") {
    serve<C1M::UringServer>(options);
  } else {
    return false;
  }
  return true;
}

} // namespace

int main(int argc, const char **argv) {
  try {
    const auto options =
        parseOptions(std::span{argv, static_cast<size_t>(argc)});
    if (!options || !run(*options)) {
      std::cerr << kUsage;
      return 2;
    }
  } catch (const std::exception &e) {
    std::cerr << "echo_server: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
