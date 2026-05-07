#define CPPHTTPLIB_NO_EXCEPTIONS
#include <cstdlib>
#include <iostream>

#include "httplib.h"

int main() {
  const char* port_env = std::getenv("PORT");
  int port = port_env ? std::atoi(port_env) : 8080;

  httplib::Client client("localhost", port);
  client.set_connection_timeout(2);
  client.set_read_timeout(2);

  auto res = client.Get("/v1/health");
  if (res && res->status == 200) {
    return 0;
  }

  if (res) {
    std::cerr << "healthcheck: HTTP " << res->status << "\n";
  } else {
    std::cerr << "healthcheck: connection failed\n";
  }
  return 1;
}
