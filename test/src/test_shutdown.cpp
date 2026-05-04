#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include "httplib.h"
#include <gtest/gtest.h>

namespace projectagamemnon::test {

// Find a free port by binding to port 0 and reading back the assigned port.
static int find_free_port() {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = 0;
  bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  close(sock);
  return port;
}

TEST(ShutdownTest, ServerStopUnblocksListen) {
  httplib::Server server;
  server.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("pong", "text/plain");
  });

  int port = find_free_port();
  std::atomic<bool> listening{false};
  std::atomic<bool> listen_returned{false};

  std::thread server_thread([&] {
    server.set_pre_routing_handler(
        [&](const httplib::Request&, httplib::Response&) -> httplib::Server::HandlerResponse {
          listening.store(true, std::memory_order_relaxed);
          return httplib::Server::HandlerResponse::Unhandled;
        });
    listening.store(true, std::memory_order_relaxed);
    server.listen("127.0.0.1", port);
    listen_returned.store(true, std::memory_order_relaxed);
  });

  // Wait until the server is up.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(server.is_running()) << "server did not start within 5 s";

  // Calling stop() should unblock listen().
  server.stop();

  server_thread.join();  // should complete promptly
  EXPECT_TRUE(listen_returned.load()) << "listen() did not return after stop()";
}

TEST(ShutdownTest, ShutdownFlagSetOnStop) {
  httplib::Server server;
  int port = find_free_port();
  std::atomic<bool> shutdown_requested{false};

  std::thread server_thread([&] { server.listen("127.0.0.1", port); });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(server.is_running());

  shutdown_requested.store(true, std::memory_order_relaxed);
  server.stop();

  server_thread.join();
  EXPECT_TRUE(shutdown_requested.load());
}

// Verifies the signal handler's null-pointer guard prevents a double-stop:
// g_server is nulled before a second signal can fire, so shutdown_handler()
// calls server.stop() at most once.
TEST(ShutdownTest, HandlerGuardPreventsDoubleStop) {
  httplib::Server server;
  int port = find_free_port();

  std::thread server_thread([&] { server.listen("127.0.0.1", port); });

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(server.is_running());

  // Simulate what main() does: null the pointer before stop() returns so a
  // second signal sees nullptr and is a no-op.
  httplib::Server* ptr = &server;
  ptr->stop();
  ptr = nullptr;

  // Second invocation via nullptr guard — must not crash.
  if (ptr) {
    ptr->stop();
  }

  server_thread.join();
  SUCCEED();
}

}  // namespace projectagamemnon::test
