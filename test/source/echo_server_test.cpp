#include "blocking_server.hpp"
#include "epoll_server.hpp"
#include "fd.hpp"
#include "net.hpp"
#include "poll_server.hpp"
#include "select_server.hpp"
#include "server_concepts.hpp"
#include "uring_server.hpp"

#include <gtest/gtest.h>
#include <liburing.h>

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct EdgeEpollServer : C1M::EpollServer {
  EdgeEpollServer() : EpollServer({}, C1M::Trigger::Edge) {}
};

// A blocking client socket.
C1M::Fd makeClient() {
  C1M::Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  EXPECT_TRUE(fd);

  // A server that never answers should fail the test, not hang it.
  const timeval timeout{.tv_sec = 5, .tv_usec = 0};
  ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return fd;
}

void connectTo(const C1M::Fd &client, uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  EXPECT_EQ(::connect(client.get(), C1M::asSockaddr(address), sizeof(address)),
            0);
}

bool sendText(int fd, std::string_view text) {
  return C1M::sendAll(fd, std::as_bytes(std::span{text}));
}

// Read until `size` bytes have arrived or the connection ends.
std::string receiveText(int fd, size_t size) {
  std::string text(size, '\0');
  size_t received = 0;
  while (received < size) {
    const ssize_t count = ::recv(fd, &text[received], size - received, 0);
    if (count <= 0) {
      break;
    }
    received += static_cast<size_t>(count);
  }
  text.resize(received);
  return text;
}

// Send without blocking until the socket has refused more for a tenth of a
// row, or everything is sent. Returns how many bytes were taken.
size_t sendUntilStuck(int fd, std::string_view text) {
  constexpr int kQuietMilliseconds = 100;
  size_t sent = 0;
  while (sent < text.size()) {
    const ssize_t count = ::send(fd, &text[sent], text.size() - sent,
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    pollfd writable{.fd = fd, .events = POLLOUT, .revents = 0};
    if (::poll(&writable, 1, kQuietMilliseconds) == 0) {
      break;
    }
  }
  return sent;
}

std::string pattern(size_t size) {
  // Prime, so the pattern does not line up with any buffer size.
  constexpr size_t kPeriod = 251;
  std::string text(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    text[i] = static_cast<char>(i % kPeriod);
  }
  return text;
}

// Docker's default seccomp profile, among others, refuses io_uring.
bool uringAvailable() {
  io_uring ring{};
  if (::io_uring_queue_init(1, &ring, 0) < 0) {
    return false;
  }
  ::io_uring_queue_exit(&ring);
  return true;
}

// Runs one server of each kind on its own thread for the length of a test.
template <C1M::EchoServer Server> class EchoServerTest : public testing::Test {
protected:
  void SetUp() override {
    if (std::is_same_v<Server, C1M::UringServer> && !uringAvailable()) {
      GTEST_SKIP() << "io_uring is not available here. Under Docker it needs "
                      "--security-opt seccomp=unconfined.";
    }
    mLoop = std::thread{[this] { mServer.run(); }};
  }

  void TearDown() override { stopServer(); }

  void stopServer() {
    mServer.stop();
    if (mLoop.joinable()) {
      mLoop.join();
    }
  }

  [[nodiscard]] C1M::Fd connect() const {
    C1M::Fd client = makeClient();
    connectTo(client, mServer.port());
    return client;
  }

  // A small receive buffer on the client makes the server's sends come up
  // short. It has to be set before connecting.
  [[nodiscard]] C1M::Fd connectWithReceiveBuffer(int bytes) const {
    C1M::Fd client = makeClient();
    ::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    connectTo(client, mServer.port());
    return client;
  }

private:
  Server mServer;
  std::thread mLoop;
};

using Servers =
    testing::Types<C1M::BlockingServer, C1M::SelectServer, C1M::PollServer,
                   C1M::EpollServer, EdgeEpollServer, C1M::UringServer>;

struct ServerNames {
  template <typename Server> static std::string GetName(int /*index*/) {
    if (std::is_same_v<Server, C1M::BlockingServer>) {
      return "Threads";
    }
    if (std::is_same_v<Server, C1M::SelectServer>) {
      return "Select";
    }
    if (std::is_same_v<Server, C1M::PollServer>) {
      return "Poll";
    }
    if (std::is_same_v<Server, C1M::EpollServer>) {
      return "EpollLevel";
    }
    if (std::is_same_v<Server, C1M::UringServer>) {
      return "Uring";
    }
    return "EpollEdge";
  }
};

TYPED_TEST_SUITE(EchoServerTest, Servers, ServerNames);

TYPED_TEST(EchoServerTest, EchoesMessage) {
  const C1M::Fd client = this->connect();
  ASSERT_TRUE(sendText(client.get(), "hello"));
  EXPECT_EQ(receiveText(client.get(), 5), "hello");
}

TYPED_TEST(EchoServerTest, EchoesSeveralMessagesOnOneConnection) {
  const C1M::Fd client = this->connect();
  for (const std::string_view message : {"one", "two", "three"}) {
    ASSERT_TRUE(sendText(client.get(), message));
    EXPECT_EQ(receiveText(client.get(), message.size()), message);
  }
}

TYPED_TEST(EchoServerTest, ServesManyConnectionsAtOnce) {
  constexpr size_t kClients = 200;
  std::vector<C1M::Fd> clients;
  for (size_t i = 0; i < kClients; ++i) {
    clients.push_back(this->connect());
  }
  for (size_t i = 0; i < kClients; ++i) {
    ASSERT_TRUE(sendText(clients[i].get(), "client " + std::to_string(i)));
  }
  for (size_t i = 0; i < kClients; ++i) {
    const std::string expected = "client " + std::to_string(i);
    EXPECT_EQ(receiveText(clients[i].get(), expected.size()), expected);
  }
}

// A client that sends without reading fills every buffer between the two
// programs. A server that has a reply it cannot send has to stop reading, or
// it would buffer without limit; once the client reads again, everything it
// sent has to come back.
TYPED_TEST(EchoServerTest, StopsReadingFromClientThatDoesNotRead) {
  constexpr int kReceiveBuffer = 4096;
  constexpr size_t kPayloadSize = size_t{32} * 1024 * 1024;
  const C1M::Fd client = this->connectWithReceiveBuffer(kReceiveBuffer);
  const std::string payload = pattern(kPayloadSize);

  const size_t sent = sendUntilStuck(client.get(), payload);
  ASSERT_GT(sent, 0U);
  ASSERT_LT(sent, payload.size());

  // Not EXPECT_EQ: a failure would print megabytes.
  const std::string echoed = receiveText(client.get(), sent);
  EXPECT_EQ(echoed.size(), sent);
  EXPECT_TRUE(echoed == payload.substr(0, sent));
}

TYPED_TEST(EchoServerTest, KeepsServingAfterClientsDisconnect) {
  constexpr int kClients = 20;
  for (int i = 0; i < kClients; ++i) {
    const C1M::Fd client = this->connect();
    // Closing with the echo unread makes the kernel send a reset.
    ASSERT_TRUE(sendText(client.get(), "bye"));
  }
  const C1M::Fd client = this->connect();
  ASSERT_TRUE(sendText(client.get(), "still here"));
  EXPECT_EQ(receiveText(client.get(), 10), "still here");
}

TYPED_TEST(EchoServerTest, ClosesConnectionAfterClientFinishes) {
  const C1M::Fd client = this->connect();
  const std::string payload = pattern(size_t{16} * 1024);
  ASSERT_TRUE(sendText(client.get(), payload));
  ASSERT_EQ(::shutdown(client.get(), SHUT_WR), 0);

  // Asking for one byte more than was sent: the read only ends early if the
  // server closes its side.
  EXPECT_TRUE(receiveText(client.get(), payload.size() + 1) == payload);
}

TYPED_TEST(EchoServerTest, StopClosesOpenConnections) {
  const C1M::Fd client = this->connect();
  ASSERT_TRUE(sendText(client.get(), "hello"));
  ASSERT_EQ(receiveText(client.get(), 5), "hello");

  this->stopServer();
  EXPECT_EQ(receiveText(client.get(), 1), "");
}

} // namespace
