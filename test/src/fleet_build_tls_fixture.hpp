#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fleet_build_tls_test {

inline void require(bool condition) {
  if (!condition) throw std::runtime_error("synthetic artifact fixture setup failed");
}

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

inline Key make_key() {
  const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
  require(context != nullptr);
  require(EVP_PKEY_keygen_init(context.get()) == 1);
  require(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1);
  EVP_PKEY* generated = nullptr;
  const auto result = EVP_PKEY_keygen(context.get(), &generated);
  Key key(generated, EVP_PKEY_free);
  require(result == 1 && key != nullptr);
  return key;
}

inline void extension(X509* certificate, int name, const char* value) {
  const std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> field(
      X509V3_EXT_conf_nid(nullptr, nullptr, name, value), X509_EXTENSION_free);
  require(field != nullptr && X509_add_ext(certificate, field.get(), -1) == 1);
}

inline Certificate make_certificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key,
                                    bool wrong_ip = false, bool expired = false) {
  Certificate certificate(X509_new(), X509_free);
  require(certificate != nullptr);
  require(X509_set_version(certificate.get(), 2) == 1);
  require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), issuer ? 2 : 1) == 1);
  require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), expired ? -7200 : -86400) !=
          nullptr);
  require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), expired ? -3600 : 604800) !=
          nullptr);
  require(X509_set_pubkey(certificate.get(), key) == 1);
  auto* name = X509_get_subject_name(certificate.get());
  require(name != nullptr);
  const auto* common_name = issuer ? "127.0.0.1" : "Fleet artifact synthetic CA";
  require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char*>(common_name), -1, -1,
                                     0) == 1);
  require(X509_set_issuer_name(certificate.get(), issuer ? X509_get_subject_name(issuer) : name) ==
          1);
  extension(certificate.get(), NID_basic_constraints,
            issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
  extension(certificate.get(), NID_key_usage,
            issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
  if (issuer) {
    extension(certificate.get(), NID_ext_key_usage, "serverAuth");
    extension(certificate.get(), NID_subject_alt_name, wrong_ip ? "IP:127.0.0.2" : "IP:127.0.0.1");
  }
  require(X509_sign(certificate.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0);
  require(X509_verify(certificate.get(), issuer_key ? issuer_key : key) == 1);
  require(X509_cmp_current_time(X509_get0_notBefore(certificate.get())) < 0);
  const auto after = X509_cmp_current_time(X509_get0_notAfter(certificate.get()));
  require(expired ? after < 0 : after > 0);
  if (issuer) require(X509_check_ip_asc(certificate.get(), "127.0.0.1", 0) == (wrong_ip ? 0 : 1));
  return certificate;
}

inline std::string pem(X509* certificate) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
  require(output != nullptr && PEM_write_bio_X509(output.get(), certificate) == 1);
  char* bytes = nullptr;
  const auto size = BIO_get_mem_data(output.get(), &bytes);
  require(size > 0 && bytes != nullptr);
  return {bytes, static_cast<std::size_t>(size)};
}

// Only synthetic public trust bytes leave this function. Private keys stay in memory.
inline std::string certificate_pem() {
  const auto key = make_key();
  const auto certificate = make_certificate(key.get(), nullptr, nullptr);
  return pem(certificate.get());
}

enum class Mode { Trusted, WrongTrust, WrongIp, Expired, Plaintext };

inline std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

struct Request {
  std::string target;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> parameters;

  bool has_header(const std::string& name) const { return headers.contains(lowercase(name)); }
  std::string get_header_value(const std::string& name) const {
    const auto found = headers.find(lowercase(name));
    return found == headers.end() ? "" : found->second;
  }
  std::string get_param_value(const std::string& name) const {
    const auto found = parameters.find(name);
    return found == parameters.end() ? "" : found->second;
  }
};

struct Response {
  int status = 200;
  std::string body;
  std::chrono::milliseconds delay{0};
};

class Socket {
 public:
  explicit Socket(int value) : value_(value) {}
  ~Socket() {
    if (value_ >= 0) ::close(value_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int get() const { return value_; }

 private:
  int value_;
};

// A single, bounded HTTP/1.1 GET per connection. This is a test boundary, not an
// artifact service. No httplib SSL macro or shared Request ABI is changed.
class Server {
 public:
  using Handler = std::function<Response(const Request&)>;
  using Clock = std::chrono::steady_clock;

  explicit Server(Handler handler, Mode mode = Mode::Trusted)
      : handler_(std::move(handler)),
        mode_(mode),
        listener_(::socket(AF_INET, SOCK_STREAM, 0)),
        context_(SSL_CTX_new(TLS_server_method()), SSL_CTX_free) {
    require(listener_.get() >= 0 && context_ != nullptr);
    const auto ca_key = make_key();
    const auto ca = make_certificate(ca_key.get(), nullptr, nullptr);
    const auto leaf_key = make_key();
    const auto leaf = make_certificate(leaf_key.get(), ca.get(), ca_key.get(),
                                       mode == Mode::WrongIp, mode == Mode::Expired);
    trust_ = pem(ca.get());
    if (mode == Mode::WrongTrust) {
      const auto other_key = make_key();
      const auto other_ca = make_certificate(other_key.get(), nullptr, nullptr);
      require(X509_cmp(ca.get(), other_ca.get()) != 0);
      trust_ = pem(other_ca.get());
    }
    require(SSL_CTX_use_certificate(context_.get(), leaf.get()) == 1);
    require(SSL_CTX_use_PrivateKey(context_.get(), leaf_key.get()) == 1);
    require(SSL_CTX_check_private_key(context_.get()) == 1);
    require(SSL_CTX_set_min_proto_version(context_.get(), TLS1_2_VERSION) == 1);
    nonblocking(listener_.get());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    require(::listen(listener_.get(), 8) == 0);
    socklen_t length = sizeof(address);
    require(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port_ = ntohs(address.sin_port);
    require(port_ > 0);
    // Binding/listening precedes publication of the port; no readiness sleep.
    worker_ = std::thread([this] { serve(); });
  }

  ~Server() { stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void stop() {
    stopped_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  int port() const { return port_; }
  const std::string& ca_pem() const { return trust_; }
  std::size_t connections() const { return connections_.load(); }
  std::size_t handshakes() const { return handshakes_.load(); }
  std::string error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

 private:
  static void nonblocking(int fd) {
    const auto flags = ::fcntl(fd, F_GETFL);
    const auto fd_flags = ::fcntl(fd, F_GETFD);
    require(flags >= 0 && fd_flags >= 0);
    require(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    require(::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0);
  }

  bool ready(int fd, short events, Clock::time_point deadline) {
    while (!stopped_.load() && Clock::now() < deadline) {
      pollfd descriptor{fd, events, 0};
      // 20 ms caps shutdown latency even when a peer stalls during handshake/I/O.
      const auto result = ::poll(&descriptor, 1, 20);
      if (result > 0) return (descriptor.revents & (events | POLLHUP | POLLERR)) != 0;
      if (result < 0 && errno != EINTR) throw std::runtime_error("artifact fixture poll failed");
    }
    return false;
  }

  bool retry(SSL* ssl, int result, int fd, Clock::time_point deadline) {
    const auto error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_WANT_READ) return ready(fd, POLLIN, deadline);
    if (error == SSL_ERROR_WANT_WRITE) return ready(fd, POLLOUT, deadline);
    // Certificate rejection, client deadline and oversized-response abort are
    // expected peer failures. Tests separately demand successful HTTP for controls.
    return false;
  }

  static Request parse(const std::string& bytes) {
    Request request;
    std::istringstream input(bytes);
    std::string method, version, line;
    require(static_cast<bool>(input >> method >> request.target >> version));
    require(method == "GET" && version == "HTTP/1.1");
    std::getline(input, line);
    while (std::getline(input, line) && line != "\r") {
      const auto colon = line.find(':');
      require(colon != std::string::npos && !line.empty() && line.back() == '\r');
      auto value = line.substr(colon + 1, line.size() - colon - 2);
      const auto first = value.find_first_not_of(" \t");
      value = first == std::string::npos ? "" : value.substr(first);
      require(request.headers.emplace(lowercase(line.substr(0, colon)), value).second);
    }
    const auto question = request.target.find('?');
    if (question != std::string::npos) {
      std::istringstream query(request.target.substr(question + 1));
      while (std::getline(query, line, '&')) {
        const auto equal = line.find('=');
        require(equal != std::string::npos);
        // Production emits only literal identifiers/digests/decimal quantities.
        // Retain raw bytes so incorrect escaping cannot accidentally satisfy a test.
        require(request.parameters.emplace(line.substr(0, equal), line.substr(equal + 1)).second);
      }
    }
    return request;
  }

  void connection(int fd) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    const std::unique_ptr<SSL, decltype(&SSL_free)> ssl(
        mode_ == Mode::Plaintext ? nullptr : SSL_new(context_.get()), SSL_free);
    if (mode_ != Mode::Plaintext) {
      require(ssl != nullptr && SSL_set_fd(ssl.get(), fd) == 1);
      int result = 0;
      do {
        ERR_clear_error();
        result = SSL_accept(ssl.get());
      } while (result != 1 && retry(ssl.get(), result, fd, deadline));
      if (result != 1) return;
      ++handshakes_;
    }
    std::string bytes;
    std::array<char, 2048> buffer{};
    while (!stopped_.load() && Clock::now() < deadline && bytes.size() < 16384) {
      const auto available = std::min(buffer.size(), std::size_t{16384} - bytes.size());
      ERR_clear_error();
      const auto count = ssl ? SSL_read(ssl.get(), buffer.data(), static_cast<int>(available))
                             : static_cast<int>(::recv(fd, buffer.data(), available, 0));
      if (count > 0) {
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
        // A TLS ClientHello at the plaintext endpoint is not an accepted HTTP
        // request. Return an HTTP response so curl proves it rejects the protocol.
        if (!ssl && !bytes.starts_with("GET ") && !std::string_view("GET ").starts_with(bytes))
          break;
        if (bytes.find("\r\n\r\n") != std::string::npos) break;
      } else if (ssl ? !retry(ssl.get(), count, fd, deadline)
                     : !(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
                         ready(fd, POLLIN, deadline))) {
        return;
      }
    }
    Response response{400, "plaintext test endpoint", std::chrono::milliseconds(0)};
    if (bytes.starts_with("GET ") && bytes.find("\r\n\r\n") != std::string::npos) {
      response = handler_(parse(bytes));
    } else if (ssl) {
      return;
    }
    if (response.delay.count() > 0) {
      require(response.delay <= std::chrono::milliseconds(1500));
      std::unique_lock lock(mutex_);
      if (wake_.wait_until(lock, std::min(deadline, Clock::now() + response.delay),
                           [this] { return stopped_.load(); }))
        return;
    }
    const auto wire = "HTTP/1.1 " + std::to_string(response.status) +
                      " Test\r\n"
                      "Content-Type: application/json\r\nConnection: close\r\n"
                      "Location: /must-not-follow\r\nContent-Length: " +
                      std::to_string(response.body.size()) + "\r\n\r\n" + response.body;
    std::size_t sent = 0;
    while (sent < wire.size() && !stopped_.load() && Clock::now() < deadline) {
      ERR_clear_error();
      const auto count =
          ssl ? SSL_write(ssl.get(), wire.data() + sent, static_cast<int>(wire.size() - sent))
              : static_cast<int>(::send(fd, wire.data() + sent, wire.size() - sent, 0));
      if (count > 0)
        sent += static_cast<std::size_t>(count);
      else if (ssl ? !retry(ssl.get(), count, fd, deadline)
                   : !(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
                       ready(fd, POLLOUT, deadline)))
        return;
    }
    // A nonblocking best-effort close_notify does not wait for the peer.
    if (ssl && !stopped_.load()) (void)SSL_shutdown(ssl.get());
  }

  void serve() noexcept {
    try {
      // Mask SIGPIPE only in this owned thread, before OpenSSL/socket writes.
      // The mask and any pending signal disappear with the thread; no global handler.
      sigset_t blocked;
      require(::sigemptyset(&blocked) == 0 && ::sigaddset(&blocked, SIGPIPE) == 0);
      require(::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) == 0);
      while (!stopped_.load()) {
        if (!ready(listener_.get(), POLLIN, Clock::now() + std::chrono::milliseconds(100)))
          continue;
        const Socket accepted(::accept(listener_.get(), nullptr, nullptr));
        if (accepted.get() < 0) {
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
          throw std::runtime_error("artifact fixture accept failed");
        }
        nonblocking(accepted.get());
        ++connections_;
        connection(accepted.get());
      }
    } catch (const std::exception& exception) {
      std::lock_guard lock(mutex_);
      error_ = exception.what();
    } catch (...) {
      std::lock_guard lock(mutex_);
      error_ = "artifact fixture thread failed";
    }
  }

  Handler handler_;
  Mode mode_;
  Socket listener_;
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
  std::string trust_;
  int port_ = 0;
  std::atomic<bool> stopped_{false};
  std::atomic<std::size_t> connections_{0};
  std::atomic<std::size_t> handshakes_{0};
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::string error_;
  std::thread worker_;
};

}  // namespace fleet_build_tls_test
