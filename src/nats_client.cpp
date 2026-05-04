#include "projectagamemnon/nats_client.hpp"

// NOLINTNEXTLINE(misc-include-cleaner) — nats.h brings in its own transitive includes
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "nats.h"
#include "nlohmann/json.hpp"

namespace projectagamemnon {

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline natsConnection* to_conn(void* p) { return static_cast<natsConnection*>(p); }
static inline jsCtx* to_js(void* p) { return static_cast<jsCtx*>(p); }

// ── Lifetime ─────────────────────────────────────────────────────────────────

NatsClient::NatsClient(const std::string& url) : url_(url) {}

NatsClient::~NatsClient() { close(); }

// ── connect ───────────────────────────────────────────────────────────────────

bool NatsClient::connect() {
  natsConnection* c = nullptr;
  natsStatus s = natsConnection_ConnectTo(&c, url_.c_str());
  if (s != NATS_OK) {
    std::cerr << "[nats] WARNING: could not connect to " << url_ << " — " << natsStatus_GetText(s)
              << " (NATS events will be skipped)\n";
    connected_ = false;
    return false;
  }
  conn_ = c;
  connected_ = true;

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
}

// ── ensure_streams ────────────────────────────────────────────────────────────

void NatsClient::ensure_streams() {
  if (!connected_ || !js_) return;

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

// ── publish ───────────────────────────────────────────────────────────────────

bool NatsClient::publish(const std::string& subject, const std::string& payload) {
  if (!connected_ || !conn_) return false;

  natsStatus s;
  if (js_) {
    jsPubAck* ack = nullptr;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    // js_Publish signature: (pubAck**, ctx*, subj, data, dataLen, opts*, errCode*)
    s = js_Publish(&ack, to_js(js_), subject.c_str(), payload.data(),
                   static_cast<int>(payload.size()), nullptr, &jerr);
    if (ack) jsPubAck_Destroy(ack);
  } else {
    s = natsConnection_Publish(to_conn(conn_), subject.c_str(), payload.data(),
                               static_cast<int>(payload.size()));
  }
  if (s != NATS_OK) {
    std::cerr << "[nats] publish error on " << subject << ": " << natsStatus_GetText(s) << "\n";
    return false;
  }
  return true;
}

// ── publish_log ───────────────────────────────────────────────────────────────

void NatsClient::publish_log(const std::string& subject, const std::string& level,
                             const std::string& message, const nlohmann::json& metadata) {
  // Build ADR-005 structured log payload.
  auto now = std::chrono::system_clock::now();
  double ts = std::chrono::duration<double>(now.time_since_epoch()).count();

  nlohmann::json payload = {
      {"timestamp", ts},    {"service", "agamemnon"}, {"level", level},
      {"message", message}, {"metadata", metadata},
  };

  // Fire-and-forget: ignore publish return value so NATS errors never affect
  // the caller's request handling path.
  publish(subject, payload.dump());
}

// ── subscribe ─────────────────────────────────────────────────────────────────

namespace {

struct CallbackContext {
  NatsClient::MessageCallback cb;
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
  ctx->cb(subject, payload);
  natsMsg_Destroy(msg);
}

}  // anonymous namespace

bool NatsClient::subscribe(const std::string& subject, MessageCallback cb) {
  if (!connected_ || !conn_) return false;

  // Ownership transfers to the C library on successful subscribe; the callback
  // deletes the context when the subscription fires or is destroyed.
  auto ctx = std::make_unique<CallbackContext>(CallbackContext{std::move(cb)});
  CallbackContext* raw = ctx.release();  // NOLINT(cppcoreguidelines-owning-memory) — ownership transfer to nats.c C API

  natsSubscription* sub = nullptr;
  natsStatus s =
      natsConnection_Subscribe(&sub, to_conn(conn_), subject.c_str(), nats_msg_handler, raw);
  if (s != NATS_OK) {
    std::cerr << "[nats] subscribe error on " << subject << ": " << natsStatus_GetText(s) << "\n";
    delete raw;  // NOLINT(cppcoreguidelines-owning-memory) — reclaiming from failed C API transfer
    return false;
  }
  return true;
}

}  // namespace projectagamemnon
