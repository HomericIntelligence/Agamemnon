#include "agamemnon/nats_client.hpp"

#include "agamemnon/metrics.hpp"

// NOLINTNEXTLINE(misc-include-cleaner) — nats.h brings in its own transitive includes
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace agamemnon {

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

// ── effective_retry_base_ms ───────────────────────────────────────────────────

// static
int NatsClient::effective_retry_base_ms() noexcept {
  const char* env = std::getenv("AGAMEMNON_NATS_RETRY_BASE_MS");
  if (!env) return kBaseRetryMs;
  try {
    int val = std::stoi(env);
    return (val >= 0) ? val : kBaseRetryMs;
  } catch (...) {
    return kBaseRetryMs;
  }
}

// ── Lifetime ─────────────────────────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url) : url_(url) {}

NatsClient::NatsClient(const std::string& url, CircuitBreaker::Config cb_cfg,
                       std::size_t dlq_capacity)
    : url_(url), breaker_(cb_cfg), dlq_(dlq_capacity) {}

NatsClient::~NatsClient() { close(); }

// ── connect ───────────────────────────────────────────────────────────────────

bool NatsClient::connect() {
  natsOptions* opts = nullptr;
  natsStatus s = natsOptions_Create(&opts);
  const char* endpoint = url_.c_str();
  if (s == NATS_OK) s = natsOptions_SetServers(opts, &endpoint, 1);
  // Allow nats.c internal reconnect attempts before we declare failure.
  if (s == NATS_OK) s = natsOptions_SetMaxReconnect(opts, 5);
  if (s == NATS_OK) s = natsOptions_SetReconnectWait(opts, 500);

  natsConnection* c = nullptr;
  if (s == NATS_OK) s = natsConnection_Connect(&c, opts);
  natsOptions_Destroy(opts);

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
bool NatsClient::publish_durable(const std::string& subject, const std::string& payload,
                                 const std::string& message_id) {
  if (!connected_ || !js_ || message_id.empty()) return false;
  std::string stream;
  if (subject.starts_with("hi.pipeline."))
    stream = "homeric-pipeline";
  else if (subject.starts_with("hi.myrmidon."))
    stream = "homeric-myrmidon";
  else if (subject.starts_with("hi.tasks."))
    stream = "homeric-tasks";
  else
    return false;
  jsStreamInfo* info = nullptr;
  if (js_GetStreamInfo(&info, to_js(js_), stream.c_str(), nullptr, nullptr) != NATS_OK)
    return false;
  const bool valid = info->Config->Storage == js_FileStorage &&
                     info->Config->Retention == js_LimitsPolicy &&
                     info->Config->Duplicates >= 120000000000LL && info->Config->MaxAge == 0 &&
                     info->Config->Discard == js_DiscardNew &&
                     (info->Config->MaxMsgsPerSubject <= 0 || info->Config->DiscardNewPerSubject);
  jsStreamInfo_Destroy(info);
  if (!valid) return false;
  jsPubOptions options;
  jsPubOptions_Init(&options);
  options.MsgId = message_id.c_str();
  options.ExpectStream = stream.c_str();
  options.MaxWait = 2000;
  jsPubAck* ack = nullptr;
  auto result = js_Publish(&ack, to_js(js_), subject.c_str(), payload.data(),
                           static_cast<int>(payload.size()), &options, nullptr);
  const bool persisted = result == NATS_OK && ack != nullptr;
  if (ack) jsPubAck_Destroy(ack);
  return persisted;
}
bool NatsClient::subscribe_durable(const std::string& stream, const std::string& subject,
                                   const std::string& durable, MessageCallback cb,
                                   int retry_delay_ms) {
  if (!connected_ || !js_ || retry_delay_ms < 1 || retry_delay_ms > 10000) return false;
  jsStreamInfo* stream_info = nullptr;
  if (js_GetStreamInfo(&stream_info, to_js(js_), stream.c_str(), nullptr, nullptr) != NATS_OK)
    return false;
  const bool stream_valid = stream_info->Config->Storage == js_FileStorage &&
                            stream_info->Config->Retention == js_LimitsPolicy &&
                            stream_info->Config->Duplicates >= 120000000000LL &&
                            stream_info->Config->MaxAge == 0 &&
                            stream_info->Config->Discard == js_DiscardNew &&
                            (stream_info->Config->MaxMsgsPerSubject <= 0 ||
                             stream_info->Config->DiscardNewPerSubject);
  jsStreamInfo_Destroy(stream_info);
  if (!stream_valid) return false;
  jsConsumerInfo* info = nullptr;
  auto found =
      js_GetConsumerInfo(&info, to_js(js_), stream.c_str(), durable.c_str(), nullptr, nullptr);
  if (found == NATS_NOT_FOUND) {
    jsConsumerConfig config;
    jsConsumerConfig_Init(&config);
    config.Durable = durable.c_str();
    config.FilterSubject = subject.c_str();
    config.DeliverPolicy = js_DeliverAll;
    config.AckPolicy = js_AckExplicit;
    config.AckWait = 30000000000LL;
    config.MaxDeliver = -1;
    config.MaxAckPending = 1;
    found = js_AddConsumer(&info, to_js(js_), stream.c_str(), &config, nullptr, nullptr);
  }
  if (found != NATS_OK || !info) return false;
  const auto* config = info->Config;
  const bool compatible =
      config->AckPolicy == js_AckExplicit && config->DeliverPolicy == js_DeliverAll &&
      config->AckWait == 30000000000LL && config->MaxDeliver == -1 && config->MaxAckPending == 1 &&
      config->BackOffLen == 0 && config->FilterSubject && subject == config->FilterSubject &&
      (!config->DeliverSubject || !*config->DeliverSubject) && config->InactiveThreshold == 0;
  jsConsumerInfo_Destroy(info);
  if (!compatible) return false;
  jsSubOptions options;
  jsSubOptions_Init(&options);
  options.Stream = stream.c_str();
  options.Consumer = durable.c_str();
  options.ManualAck = true;
  natsSubscription* subscription = nullptr;
  if (js_PullSubscribe(&subscription, to_js(js_), subject.c_str(), durable.c_str(), nullptr,
                       &options, nullptr) != NATS_OK)
    return false;
  consumers_.emplace_back([this, subscription, cb = std::move(cb), stream, durable,
                           retry_delay_ms](std::stop_token stop) {
    while (!stop.stop_requested()) {
      natsMsgList messages{};
      auto status = natsSubscription_Fetch(&messages, subscription, 1, 100, nullptr);
      if (status == NATS_TIMEOUT) continue;
      if (status != NATS_OK) {
        std::cerr << "[nats] durable fetch failed for " << durable << ": "
                  << natsStatus_GetText(status) << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
        continue;
      }
      for (int index = 0; index < messages.Count; ++index) {
        auto* message = messages.Msgs[index];
        jsMsgMetaData* metadata = nullptr;
        if (natsMsg_GetMetaData(&metadata, message) != NATS_OK) continue;
        const std::string source = natsMsg_GetSubject(message);
        const std::string data(natsMsg_GetData(message), natsMsg_GetDataLength(message));
        std::string failure;
        // Slow GitHub processing extends the ACK deadline, but this is not a
        // distributed controller lease. Deployment remains a single writer.
        std::jthread heartbeat([message](std::stop_token heartbeat_stop) {
          int ticks = 0;
          while (!heartbeat_stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (++ticks % 50 == 0) natsMsg_InProgress(message, nullptr);
          }
        });
        try {
          if (metadata->NumDelivered > 3)
            failure = "processing_retry_exhausted";
          else
            cb(source, data);
        } catch (const std::invalid_argument&) {
          failure = "invalid_input";
        } catch (...) {
          failure = "processing_failed";
        }
        heartbeat.request_stop();
        heartbeat.join();
        if (failure.empty()) {
          if (natsMsg_AckSync(message, nullptr, nullptr) != NATS_OK) {
            std::cerr << "[nats] durable ACK unconfirmed for " << durable << "\n";
            natsMsg_NakWithDelay(message, retry_delay_ms, nullptr);
          }
        } else if (failure == "invalid_input" || metadata->NumDelivered >= 3) {
          std::string encoded;
          static constexpr char hex[] = "0123456789abcdef";
          const auto kept = std::min<std::size_t>(data.size(), 65536);
          for (std::size_t i = 0; i < kept; ++i) {
            auto byte = static_cast<unsigned char>(data[i]);
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
          }
          const auto id = durable + ":" + std::to_string(metadata->Sequence.Stream);
          nlohmann::json record = {
              {"schema", "hi/quarantine/v1"},    {"eventId", id},
              {"status", "quarantined"},         {"reason", failure},
              {"sourceStream", stream},          {"sourceSequence", metadata->Sequence.Stream},
              {"sourceSubject", source},         {"deliveries", metadata->NumDelivered},
              {"payloadHex", encoded},           {"sourceBytes", data.size()},
              {"truncated", kept != data.size()}};
          if (publish_durable("hi.pipeline.quarantine." + durable, record.dump(), id)) {
            if (natsMsg_Term(message, nullptr) != NATS_OK)
              std::cerr << "[nats] quarantine termination unconfirmed for " << durable << "\n";
          } else {
            std::cerr << "[nats] quarantine unconfirmed for " << durable
                      << "; source remains unacknowledged and quarantine delivery will retry\n";
            natsMsg_NakWithDelay(message, retry_delay_ms, nullptr);
          }
        } else {
          natsMsg_NakWithDelay(message, retry_delay_ms, nullptr);
        }
        jsMsgMetaData_Destroy(metadata);
      }
      natsMsgList_Destroy(&messages);
    }
    // Bound consumers survive attachment destruction and process restart.
    natsSubscription_Destroy(subscription);
  });
  return true;
}

void NatsClient::close() {
  for (auto& consumer : consumers_) consumer.request_stop();
  consumers_.clear();
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

void NatsClient::ensure_streams(bool durable_work) {
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
    if (durable_work &&
        (std::string(sd.name) == "homeric-pipeline" || std::string(sd.name) == "homeric-myrmidon" ||
         std::string(sd.name) == "homeric-tasks")) {
      cfg.MaxAge = 0;
      cfg.Discard = js_DiscardNew;
      cfg.Duplicates = 120000000000LL;
    }
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

  int delay_ms = effective_retry_base_ms();
  for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
    auto s = static_cast<natsStatus>(do_publish_once(subject, payload));
    if (s == NATS_OK) {
      breaker_.record_success();
      return true;
    }

    std::cerr << "[nats] publish error on " << subject << " (attempt " << attempt << "/"
              << kMaxRetries << "): " << natsStatus_GetText(s) << "\n";

    if (!is_infra_error(s) || attempt == kMaxRetries) {
      // Non-retryable error or last attempt exhausted.
      breaker_.record_failure();
      dlq_.push(subject, payload, attempt);
      std::cerr << "[nats] ERROR: publish to " << subject
                << " failed after all retries — message dead-lettered\n";
      return false;
    }

    // Exponential backoff before next retry.
    // delay_ms is tunable via AGAMEMNON_NATS_RETRY_BASE_MS to reduce httplib
    // request-thread blocking (#290).
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
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

  int delay_ms = effective_retry_base_ms();
  for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
    auto s = static_cast<natsStatus>(do_publish_once(subject, payload_str));
    if (s == NATS_OK) {
      breaker_.record_success();
      return;
    }

    std::cerr << "[nats] publish error on " << subject << " (attempt " << attempt << "/"
              << kMaxRetries << "): " << natsStatus_GetText(s) << "\n";

    if (!is_infra_error(s) || attempt == kMaxRetries) {
      breaker_.record_failure();
      dlq_.push(subject, payload_str, attempt, level, service);
      std::cerr << "[nats] ERROR: publish to " << subject
                << " failed after all retries — message dead-lettered\n";
      return;
    }

    // delay_ms is tunable via AGAMEMNON_NATS_RETRY_BASE_MS (#290).
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
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

// Called once by nats.c when the subscription is destroyed (after the last
// message callback has returned).  Releases the CallbackContext so that
// subscribe() can be called more than once without leaking.
extern "C" void nats_sub_complete(void* closure) {
  delete static_cast<CallbackContext*>(closure);  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // anonymous namespace

bool NatsClient::subscribe(const std::string& subject, MessageCallback cb) {
  if (!connected_ || !conn_) return false;

  // Heap-allocate the context; ownership is transferred to the subscription.
  // nats_sub_complete() deletes it when the subscription is destroyed.
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

  // Register teardown callback so ctx is deleted when the subscription is destroyed,
  // not leaked if subscribe() is ever called more than once (#202).
  natsSubscription_SetOnCompleteCB(sub, nats_sub_complete, ctx);
  return true;
}

}  // namespace agamemnon
