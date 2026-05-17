#include "projectagamemnon/nats_client.hpp"

#include "projectagamemnon/metrics.hpp"

// NOLINTNEXTLINE(misc-include-cleaner) — nats.h brings in its own transitive includes
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon {

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline natsConnection* to_conn(void* p) { return static_cast<natsConnection*>(p); }
static inline jsCtx* to_js(void* p) { return static_cast<jsCtx*>(p); }

// Infra-class errors that warrant a retry (transient broker unavailability).
// Protocol rejections (e.g. NATS_NOT_PERMITTED) should not be retried.
static bool is_infra_error(natsStatus s) noexcept {
  switch (s) {
    case NATS_CONNECTION_CLOSED:
    case NATS_CONNECTION_DISCONNECTED:
    case NATS_IO_ERROR:
    case NATS_NO_SERVER:
    case NATS_TIMEOUT:
      return true;
    default:
      return false;
  }
}

// Get path to circuit breaker state file
static std::string get_cb_state_path() {
  const char* state_dir = std::getenv("AGAMEMNON_STATE_DIR");
  std::string dir = state_dir ? state_dir : "/tmp";
  return dir + "/agamemnon_cb.json";
}

// Check if circuit breaker persistence is enabled
static bool cb_persistence_enabled() {
  const char* enabled = std::getenv("AGAMEMNON_CB_PERSIST");
  return enabled && std::string(enabled) == "true";
}

// Persist circuit breaker state to JSON file
static void save_cb_state(const CircuitBreaker& breaker) {
  if (!cb_persistence_enabled()) return;

  try {
    nlohmann::json state_json;
    state_json["state"] = static_cast<int>(breaker.state());
    state_json["failure_count"] = breaker.failure_count();
    state_json["open_until"] = breaker.failure_count();  // Simplified: use failure_count as proxy

    std::string path = get_cb_state_path();
    std::ofstream ofs(path);
    if (ofs) {
      ofs << state_json.dump();
    }
  } catch (...) {
    // Silently ignore I/O errors
  }
}

// Restore circuit breaker state from JSON file (not used yet, but available)
// Note: Full restoration would require modifying CircuitBreaker to support
// setting state directly. For now, this is a reference implementation.
static void load_cb_state(const std::string& /*path*/) {
  // Future: implement when CircuitBreaker supports state injection
}

// ── Lifetime ─────────────────────────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url) : url_(url) {
  // Try to restore circuit breaker state from previous run
  if (cb_persistence_enabled()) {
    // Placeholder: full restoration requires CircuitBreaker::set_state() support
    // For now, we just prepare to save state on transitions
  }
}

NatsClient::NatsClient(const std::string& url, CircuitBreaker::Config cb_cfg,
                       std::size_t dlq_capacity)
    : url_(url), breaker_(cb_cfg), dlq_(dlq_capacity) {
  // Try to restore circuit breaker state from previous run
  if (cb_persistence_enabled()) {
    // Placeholder: full restoration requires CircuitBreaker::set_state() support
    // For now, we just prepare to save state on transitions
  }
}

NatsClient::~NatsClient() { close(); }

// ── connect ───────────────────────────────────────────────────────────────────

bool NatsClient::connect() {
  natsOptions* opts = nullptr;
  natsOptions_Create(&opts);
  // Allow nats.c internal reconnect attempts before we declare failure.
  natsOptions_SetMaxReconnect(opts, 5);
  natsOptions_SetReconnectWait(opts, 500);  // 500 ms between internal reconnect attempts

  natsConnection* c = nullptr;
  natsStatus s = natsConnection_Connect(&c, opts);
  natsOptions_Destroy(opts);

  if (s != NATS_OK) {
    // Fall back to simple URL connect (natsOptions_SetServers variant)
    s = natsConnection_ConnectTo(&c, url_.c_str());
  }

  if (s != NATS_OK) {
    std::cerr << "[nats] WARNING: could not connect to " << url_ << " — " << natsStatus_GetText(s)
              << " (NATS events will be skipped)\n";
    connected_ = false;
    if (metrics_) metrics_->set_nats_connected(false);
    return false;
  }
  conn_ = c;
  connected_ = true;
  if (metrics_) metrics_->set_nats_connected(true);

  // Obtain a JetStream context
  jsCtx* js = nullptr;
  jsOptions jso;
  jsOptions_Init(&jso);
  s = natsConnection_JetStream(&js, c, &jso);
  if (s != NATS_OK) {
    std::cerr << "[nats] WARNING: JetStream context failed — " << natsStatus_GetText(s) << "\n";
    // Still "connected" for plain-core NATS
  } else {
    js_ = js;
  }
  return true;
}

// ── close ─────────────────────────────────────────────────────────────────────

void NatsClient::close() {
  if (js_) {
    jsCtx_Destroy(to_js(js_));
    js_ = nullptr;
  }
  if (conn_) {
    natsConnection_Close(to_conn(conn_));
    natsConnection_Destroy(to_conn(conn_));
    conn_ = nullptr;
  }
  connected_ = false;
  if (metrics_) metrics_->set_nats_connected(false);
}

// ── ensure_streams ────────────────────────────────────────────────────────────

void NatsClient::ensure_streams() {
  if (!connected_ || !js_) return;

  auto env_int64 = [](const char* name, int64_t def) -> int64_t {
    const char* v = std::getenv(name);
    return v ? static_cast<int64_t>(std::stoll(v)) : def;
  };
  const int64_t max_bytes = env_int64("NATS_STREAM_MAX_BYTES_MB", 50) * 1024LL * 1024LL;
  const int64_t max_age = env_int64("NATS_STREAM_MAX_AGE_SEC", 3600) * 1000000000LL;  // nanoseconds

  struct StreamDef {
    const char* name;
    const char* subject;
  };
  static const StreamDef kStreams[] = {
      {"homeric-agents", "hi.agents.>"},     {"homeric-tasks", "hi.tasks.>"},
      {"homeric-myrmidon", "hi.myrmidon.>"}, {"homeric-research", "hi.research.>"},
      {"homeric-pipeline", "hi.pipeline.>"}, {"homeric-logs", "hi.logs.>"},
  };

  for (const auto& sd : kStreams) {
    jsStreamConfig cfg;
    jsStreamConfig_Init(&cfg);
    cfg.Name = sd.name;
    const char* subjects[] = {sd.subject};
    cfg.Subjects = subjects;
    cfg.SubjectsLen = 1;
    cfg.Storage = js_FileStorage;
    cfg.Retention = js_LimitsPolicy;
    cfg.MaxBytes = max_bytes;
    cfg.MaxAge = static_cast<uint64_t>(max_age);
    cfg.MaxMsgs = -1;

    jsStreamInfo* info = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    // js_AddStream signature: (info**, ctx*, cfg*, opts*, errCode*)
    natsStatus s = js_AddStream(&info, to_js(js_), &cfg, nullptr, &jerr);
    if (s == NATS_OK) {
      jsStreamInfo_Destroy(info);
    } else if (jerr == JSStreamNameExistErr) {
      // Already exists — that's fine.
    } else {
      std::cerr << "[nats] WARNING: could not create stream " << sd.name << " — "
                << natsStatus_GetText(s) << " jerr=" << jerr << "\n";
    }
  }
}

// ── do_publish_once ───────────────────────────────────────────────────────────

int NatsClient::do_publish_once(const std::string& subject, const std::string& payload) {
  natsStatus s;
  if (js_) {
    jsPubAck* ack = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    s = js_Publish(&ack, to_js(js_), subject.c_str(), payload.data(),
                   static_cast<int>(payload.size()), nullptr, &jerr);
    if (ack) jsPubAck_Destroy(ack);
  } else {
    s = natsConnection_Publish(to_conn(conn_), subject.c_str(), payload.data(),
                               static_cast<int>(payload.size()));
  }
  return static_cast<int>(s);
}

// ── publish ───────────────────────────────────────────────────────────────────

bool NatsClient::publish(const std::string& subject, const std::string& payload) {
  if (!connected_ || !conn_) return false;

  if (!breaker_.allow_attempt()) {
    std::cerr << "[nats] ERROR: circuit OPEN — dropping publish to " << subject << "\n";
    dlq_.push(subject, payload, 0);
    return false;
  }

  int delay_ms = kBaseRetryMs;
  for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
    auto s = static_cast<natsStatus>(do_publish_once(subject, payload));
    if (s == NATS_OK) {
      breaker_.record_success();
      save_cb_state(breaker_);  // Persist on state transition to Closed
      return true;
    }

    std::cerr << "[nats] publish error on " << subject << " (attempt " << attempt << "/"
              << kMaxRetries << "): " << natsStatus_GetText(s) << "\n";

    if (!is_infra_error(s) || attempt == kMaxRetries) {
      // Non-retryable error or last attempt exhausted.
      breaker_.record_failure();
      save_cb_state(breaker_);  // Persist on state transition
      dlq_.push(subject, payload, attempt);
      std::cerr << "[nats] ERROR: publish to " << subject
                << " failed after all retries — message dead-lettered\n";
      return false;
    }

    // Exponential backoff before next retry.
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    delay_ms *= 2;
  }

  // Unreachable, but satisfies compiler.
  return false;
}

// ── publish_log ───────────────────────────────────────────────────────────────

void NatsClient::publish_log(const std::string& subject, const std::string& level,
                             const std::string& message, const nlohmann::json& metadata) {
  // Build ADR-005 structured log payload.
  auto now = std::chrono::system_clock::now();
  double ts = std::chrono::duration<double>(now.time_since_epoch()).count();

  const std::string service = "agamemnon";
  nlohmann::json payload = {
      {"timestamp", ts},    {"service", service},   {"level", level},
      {"message", message}, {"metadata", metadata},
  };

  std::string payload_str = payload.dump();

  // Fire-and-forget: ignore publish return value so NATS errors never affect
  // the caller's request handling path. However, we store level/service with DLQ entries.
  if (!connected_ || !conn_) {
    dlq_.push(subject, payload_str, 0, level, service);
    return;
  }

  if (!breaker_.allow_attempt()) {
    std::cerr << "[nats] ERROR: circuit OPEN — dropping publish to " << subject << "\n";
    dlq_.push(subject, payload_str, 0, level, service);
    return;
  }

  int delay_ms = kBaseRetryMs;
  for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
    auto s = static_cast<natsStatus>(do_publish_once(subject, payload_str));
    if (s == NATS_OK) {
      breaker_.record_success();
      save_cb_state(breaker_);
      return;
    }

    std::cerr << "[nats] publish error on " << subject << " (attempt " << attempt << "/"
              << kMaxRetries << "): " << natsStatus_GetText(s) << "\n";

    if (!is_infra_error(s) || attempt == kMaxRetries) {
      breaker_.record_failure();
      save_cb_state(breaker_);
      dlq_.push(subject, payload_str, attempt, level, service);
      std::cerr << "[nats] ERROR: publish to " << subject
                << " failed after all retries — message dead-lettered\n";
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    delay_ms *= 2;
  }
}

// ── subscribe ─────────────────────────────────────────────────────────────────

namespace {

struct CallbackContext {
  NatsClient::MessageCallback cb;
  MetricsRegistry* metrics = nullptr;
};

// nats.c requires a C-style callback signature.
extern "C" void nats_msg_handler(natsConnection* /*nc*/, natsSubscription* /*sub*/, natsMsg* msg,
                                 void* closure) {
  if (!closure || !msg) return;
  auto* ctx = static_cast<CallbackContext*>(closure);
  std::string subject(natsMsg_GetSubject(msg));
  const char* data = static_cast<const char*>(natsMsg_GetData(msg));
  int datLen = natsMsg_GetDataLength(msg);
  std::string payload(data ? data : "", data ? static_cast<std::size_t>(datLen) : 0);
  if (ctx->metrics) ctx->metrics->record_nats_receive(subject);
  ctx->cb(subject, payload);
  natsMsg_Destroy(msg);
}

}  // anonymous namespace

bool NatsClient::subscribe(const std::string& subject, MessageCallback cb) {
  if (!connected_ || !conn_) return false;

  // Heap-allocate the context; it lives for the lifetime of the subscription.
  // For this server the subscription lives for the lifetime of the process.
  auto* ctx =
      new CallbackContext{std::move(cb), metrics_};  // NOLINT(cppcoreguidelines-owning-memory)

  natsSubscription* sub = nullptr;
  natsStatus s =
      natsConnection_Subscribe(&sub, to_conn(conn_), subject.c_str(), nats_msg_handler, ctx);
  if (s != NATS_OK) {
    std::cerr << "[nats] subscribe error on " << subject << ": " << natsStatus_GetText(s) << "\n";
    delete ctx;  // NOLINT(cppcoreguidelines-owning-memory) — reclaiming from failed C API transfer
    return false;
  }
  return true;
}

}  // namespace projectagamemnon
