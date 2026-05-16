# Prometheus metrics — `GET /metrics`

ProjectAgamemnon exposes Prometheus-format metrics on the unauthenticated
`GET /metrics` endpoint. The response is plain `text/plain; version=0.0.4`
suitable for scraping by Prometheus, VictoriaMetrics, or compatible TSDBs.

## Endpoint

| Field | Value |
| --- | --- |
| Method | `GET` |
| Path | `/metrics` |
| Auth | None |
| Response type | `text/plain` (Prometheus exposition format) |
| Status | `200` always (unless the server is down) |

## Exported metrics

All metrics are prefixed with `hi_` (HomericIntelligence). Source of truth:
[`src/metrics.cpp`](../src/metrics.cpp).

| Metric | Type | Help |
| --- | --- | --- |
| `hi_http_requests_total` | counter | Total HTTP requests handled |
| `hi_http_request_duration_seconds` | histogram | HTTP request latency in seconds |
| `hi_http_errors_total` | counter | Total HTTP 4xx/5xx responses |
| `hi_tasks_total` | gauge | Current number of tasks in the store |
| `hi_task_state_transitions_total` | counter | Total task state transitions |
| `hi_agents_total` | gauge | Current number of agents in the store |
| `hi_nats_messages_published_total` | counter | Total NATS messages published |
| `hi_nats_messages_received_total` | counter | Total NATS messages received |
| `hi_nats_connected` | gauge | `1` if connected to NATS, `0` otherwise |
| `hi_process_start_time_seconds` | gauge | Unix timestamp of process start |
| `hi_build_info` | gauge | Build metadata (value is always `1`; labels carry version/commit) |

## Sample Prometheus scrape config

```yaml
scrape_configs:
  - job_name: agamemnon
    metrics_path: /metrics
    static_configs:
      - targets: ['agamemnon.tailnet:8080']
        labels:
          service: agamemnon
          mesh: homericintelligence
```

For a Tailscale-internal deployment, replace `agamemnon.tailnet` with the
magic-DNS hostname or the `100.x.x.x` peer address.
