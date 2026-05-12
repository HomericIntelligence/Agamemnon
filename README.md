# ProjectAgamemnon

Planning, coordination, and agentic orchestration for the HomericIntelligence distributed agent mesh.

Part of [Odysseus](https://github.com/HomericIntelligence/Odysseus) — the HomericIntelligence meta-repo.

## What This Is

Agamemnon is the central coordinator in the HomericIntelligence pipeline:

```
User <-> Odysseus <-> Nestor <-> Agamemnon <-> agentic pipeline loop -> completion
```

It receives researched briefs from ProjectNestor and manages the full planning and execution
pipeline using a HMAS 4-layer agentic hierarchy. Agamemnon enqueues work for Myrmidons to
pull when ready (pull-based, `MaxAckPending=1`). It does **not** do research (that is Nestor's
role) and does not provide a user interface (that is Odysseus's role).

See [AGENTS.md](AGENTS.md) for multi-agent handoff protocols.

## Architecture

All inter-component communication flows through **ProjectKeystone** (invisible transport layer).
Agamemnon publishes and subscribes to the following NATS subjects:

| Subject | Direction | Consumers |
|---|---|---|
| `hi.tasks.>` | pub/sub | Odysseus (reads task state updates) |
| `hi.pipeline.>` | pub/sub | Odysseus (reads pipeline state) |
| `hi.myrmidon.{type}.>` | PULL consumers | Myrmidons pull work from here |
| `hi.logs.agamemnon.*` | pub | Observability |
| `hi.agents.*` | pub/sub | Agent lifecycle events |

Local intra-host transport uses BlazingMQ + C++20 MessageBus. Cross-host transport uses
NATS JetStream via nats.c v3.9.1 over Tailscale.

## API Documentation

The REST API exposes 25 endpoints across agents, teams, tasks, workflows, and chaos injection.

- **[docs/api/README.md](docs/api/README.md)** — Human-readable endpoint reference with request/response tables
- **[docs/api/openapi.yaml](docs/api/openapi.yaml)** — OpenAPI 3.1 specification (machine-readable)
- **[docs/api/nats-events.md](docs/api/nats-events.md)** — NATS subject reference for all published/subscribed events

Validate the spec locally:

```bash
just docs-validate
```

## Prerequisites

| Tool | Minimum Version | Notes |
|---|---|---|
| CMake | 3.20 | |
| Ninja | 1.11 | |
| GCC or Clang | GCC 12+ / Clang 15+ | C++20 support required, no compiler extensions |
| Conan | 2.0 | Package manager for cpp-httplib, nlohmann_json, gtest |
| OpenSSL | 3.0 | Runtime: `libssl3`; build: `libssl-dev` |
| pixi | any | Recommended; provides a reproducible dev environment |

## Building

```bash
git clone https://github.com/HomericIntelligence/ProjectAgamemnon.git
cd ProjectAgamemnon

# Recommended: use pixi for a reproducible environment
pixi install

# Install Conan dependencies
just deps

# Configure + build (debug)
just build

# Run tests
just test

# Run the server
NATS_URL=nats://localhost:4222 PORT=8080 ./build/debug/ProjectAgamemnon_server
```

## Environment Variables

| Variable | Default | Description |
|---|---|---|
| `NATS_URL` | `nats://localhost:4222` | NATS server connection URL |
| `PORT` | `8080` | TCP port the HTTP server listens on |
| `AGAMEMNON_LOG_LEVEL` | `INFO` | Orchestration daemon logging verbosity |
| `AGAMEMNON_POLL_INTERVAL` | `1.0` | Orchestration daemon routing-loop poll interval (seconds) |
| `AGAMEMNON_SHUTDOWN_TIMEOUT` | `30.0` | Orchestration daemon graceful-shutdown wait (seconds) |
| `RATE_LIMIT_RPS` | `60` | Per-client steady-state request rate limit (requests/sec) |
| `RATE_LIMIT_BURST` | `120` | Per-client burst capacity for the token bucket |
| `SERVER_THREAD_COUNT` | `8` | HTTP worker thread pool size |
| `SERVER_READ_TIMEOUT_SEC` | `10` | Per-request socket read timeout (seconds) |
| `SERVER_WRITE_TIMEOUT_SEC` | `10` | Per-request socket write timeout (seconds) |
| `SERVER_REQUEST_SIZE_LIMIT_MB` | `4` | Maximum request body size before 413 is returned |
| `NATS_STREAM_MAX_BYTES_MB` | (stream default) | JetStream max byte budget per Agamemnon-owned stream |
| `NATS_STREAM_MAX_AGE_SEC` | (stream default) | JetStream max retention age per Agamemnon-owned stream |

> **Upgrading from ProjectKeystone?** The `KEYSTONE_LOG_LEVEL`,
> `KEYSTONE_POLL_INTERVAL`, and `KEYSTONE_SHUTDOWN_TIMEOUT` variables were
> renamed to their `AGAMEMNON_*` equivalents. Setting the legacy names against
> the server-side daemon is a silent no-op. See
> [docs/migration-from-keystone.md](docs/migration-from-keystone.md) for the
> full rename table and remediation steps.

## API Endpoints

### Health

| Method | Path | Description |
|---|---|---|
| `GET` | `/health` | Liveness check (returns `{status: ok, service: ProjectAgamemnon}`) |
| `GET` | `/v1/health` | Versioned health check (returns `{status: ok}`) |
| `GET` | `/v1/version` | Service version (returns `{version, name}`) |

### Agents

| Method | Path | Description |
|---|---|---|
| `GET` | `/v1/agents` | List all agents |
| `POST` | `/v1/agents` | Create an agent (set `host: docker` for docker-hosted agents) |
| `GET` | `/v1/agents/by-name/<name>` | Get agent by name |
| `GET` | `/v1/agents/<id>` | Get agent by ID |
| `POST` | `/v1/agents/<id>/start` | Start agent |
| `POST` | `/v1/agents/<id>/stop` | Stop agent |
| `PATCH` | `/v1/agents/<id>` | Update agent |
| `DELETE` | `/v1/agents/<id>` | Delete agent |

### Teams

| Method | Path | Description |
|---|---|---|
| `GET` | `/v1/teams` | List all teams |
| `POST` | `/v1/teams` | Create a team |
| `GET` | `/v1/teams/<id>` | Get team by ID |
| `PUT` | `/v1/teams/<id>` | Replace team |
| `DELETE` | `/v1/teams/<id>` | Delete team |

### Tasks

| Method | Path | Description |
|---|---|---|
| `GET` | `/v1/tasks` | List all tasks across all teams |
| `GET` | `/v1/teams/<team_id>/tasks` | List tasks for a team |
| `POST` | `/v1/teams/<team_id>/tasks` | Create a task |
| `GET` | `/v1/teams/<team_id>/tasks/<task_id>` | Get task by ID |
| `PUT` | `/v1/teams/<team_id>/tasks/<task_id>` | Replace task |
| `PATCH` | `/v1/teams/<team_id>/tasks/<task_id>` | Update task fields |

### Chaos (fault injection — for ProjectCharybdis)

| Method | Path | Description |
|---|---|---|
| `GET` | `/v1/chaos` | List all active faults |
| `POST` | `/v1/chaos/<type>` | Inject a fault |
| `DELETE` | `/v1/chaos/<id>` | Remove a fault |

## Docker

```bash
# Build the image
docker build -t projectagamemnon .

# Run the server
docker run -p 8080:8080 \
  -e NATS_URL=nats://your-nats-host:4222 \
  projectagamemnon
```

The image uses a non-root `agamemnon` user and includes a health check against `GET /v1/health`
(interval 10s, timeout 3s, 3 retries). The exposed port is `8080`.

## Development

```bash
just lint          # Run clang-tidy
just format        # Run clang-format (in-place)
just format-check  # Check formatting without modifying files
just coverage      # Build + run coverage report
just clean         # Remove build/ and install/
```

Pre-commit hooks enforce formatting and [conventional commits](https://www.conventionalcommits.org/).
Run `pixi run pre-commit install` once after cloning to activate them. Never bypass hooks with
`--no-verify`.

## Project Structure

```
.
├── src/          C++ source (server_main.cpp, routes.cpp, store.cpp, nats_client.cpp, …)
├── include/      Public headers (projectagamemnon/)
├── test/         GoogleTest unit and integration tests
├── clients/      Client libraries (python/)
├── cmake/        CMake modules and profiles
├── conan/        Conan profiles
├── scripts/      lint.sh, format.sh, coverage.sh
├── Dockerfile    Multi-stage build (ubuntu:24.04 builder → runtime)
├── justfile      Developer task runner
└── pixi.toml     Reproducible dev environment
```

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.3 | Embedded HTTP server |
| [nats.c](https://github.com/nats-io/nats.c) | 3.9.1 | NATS JetStream client |
| [nlohmann_json](https://github.com/nlohmann/json) | 3.11.3 | JSON serialization |
| [GoogleTest](https://github.com/google/googletest) | 1.14.0 | Unit and integration testing |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for branch naming, commit message conventions, and
the pull request process. Please read [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) before
participating. To report a security vulnerability, follow [SECURITY.md](SECURITY.md).

## Roadmap

See [ROADMAP.md](ROADMAP.md) for what is implemented today versus what is planned, with
status and acceptance criteria for each deferred feature.

## License

MIT

## Data & Privacy

ProjectAgamemnon processes infrastructure metadata only (agent IDs, Tailscale host
identifiers, task state). It does not collect personal data. See
[SECURITY.md](SECURITY.md) for the full data and privacy policy.
