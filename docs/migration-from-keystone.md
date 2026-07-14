# Migration Guide: `KEYSTONE_*` → `AGAMEMNON_*` Environment Variables

Agamemnon was originally developed under the name **ProjectKeystone**.
During the rename, the orchestration daemon's runtime configuration variables
were renamed from `KEYSTONE_*` to `AGAMEMNON_*`. Deployments or `.env` files
that still set the old names will **silently fall back to defaults** in the
server-side daemon, which can mask production misconfiguration.

This document lists every renamed variable, describes the current behaviour of
each consumer, and gives copy-paste-ready remediation steps.

## Rename Table

| Old name (`KEYSTONE_*`) | New name (`AGAMEMNON_*`) | Default | Consumed by |
|---|---|---|---|
| `KEYSTONE_LOG_LEVEL` | `AGAMEMNON_LOG_LEVEL` | `INFO` | `agamemnon.orchestration.config.Settings`, `agamemnon_client.config.Settings` |
| `KEYSTONE_POLL_INTERVAL` | `AGAMEMNON_POLL_INTERVAL` | `1.0` (seconds) | `agamemnon.orchestration.config.Settings`, `agamemnon_client.config.Settings` |
| `KEYSTONE_SHUTDOWN_TIMEOUT` | `AGAMEMNON_SHUTDOWN_TIMEOUT` | `30.0` (seconds) | `agamemnon.orchestration.config.Settings`, `agamemnon_client.config.Settings` |

## Behaviour by Consumer

There are two independent configuration loaders in this repository. They have
different fallback behaviour for the legacy names — operators must update
**both** sides:

### 1. Server-side daemon (`agamemnon.orchestration.config`)

The orchestration daemon shipped inside Agamemnon **only** reads the
new `AGAMEMNON_*` names. Setting `KEYSTONE_LOG_LEVEL=DEBUG` against this
daemon is a no-op — the daemon will silently use the `INFO` default.

```python
# agamemnon/orchestration/config.py
log_level: str = field(
    default_factory=lambda: os.environ.get("AGAMEMNON_LOG_LEVEL", "INFO")
)
```

### 2. Python client library (`agamemnon_client.config`)

The published `HomericIntelligence-Agamemnon` Python client includes a
compatibility shim. If only the legacy variable is set, the client will read
it and emit a `DeprecationWarning`:

```python
# clients/python/src/agamemnon_client/config.py
def _get_with_fallback(new_name: str, old_name: str, default: str) -> str:
    if new_name in os.environ:
        return os.environ[new_name]
    if old_name in os.environ:
        warnings.warn(
            f"{old_name} is deprecated; use {new_name} instead.",
            DeprecationWarning,
            stacklevel=2,
        )
        return os.environ[old_name]
    return default
```

The shim is provided for end-user convenience while consumers migrate. It
will be removed in a future release; do **not** rely on it for new
deployments.

## Remediation

### Step 1 — Audit your deployment

Search every `.env` file, systemd unit, Docker Compose override, Kubernetes
manifest, and Ansible role for the legacy prefix:

```bash
grep -RIn 'KEYSTONE_' \
    .env* \
    /etc/agamemnon/ \
    deploy/ \
    k8s/ \
    ansible/ \
    docker-compose*.yml \
    2>/dev/null
```

### Step 2 — Rename in place

Replace each occurrence with the new name. The values themselves do not
change — only the variable names.

```bash
# .env (before)
KEYSTONE_LOG_LEVEL=DEBUG
KEYSTONE_POLL_INTERVAL=0.5
KEYSTONE_SHUTDOWN_TIMEOUT=60

# .env (after)
AGAMEMNON_LOG_LEVEL=DEBUG
AGAMEMNON_POLL_INTERVAL=0.5
AGAMEMNON_SHUTDOWN_TIMEOUT=60
```

For docker-compose:

```yaml
services:
  agamemnon:
    environment:
      AGAMEMNON_LOG_LEVEL: DEBUG
      AGAMEMNON_POLL_INTERVAL: "0.5"
      AGAMEMNON_SHUTDOWN_TIMEOUT: "60"
```

### Step 3 — Verify after deploy

After restarting the daemon, confirm the new values are in effect. The Python
client surfaces a `DeprecationWarning` if it picked up a legacy name — treat
those warnings as actionable findings, not noise:

```python
import warnings
warnings.simplefilter("error", DeprecationWarning)  # fail fast in tests
from agamemnon_client.config import load_settings
load_settings()
```

For the server-side daemon, check the structured logs at startup and confirm
the effective `log_level` matches what you intended.

## Why This Matters

The original rename PR (#26) did not include a deprecation shim on the
server side or a migration note. Issue #118 was filed to close that gap.
Without this guide, operators upgrading across the rename boundary will see
their explicit log-level / poll-interval / shutdown-timeout overrides
silently revert to defaults — a classic silent-failure regression.

## See Also

- Issue [#118](https://github.com/HomericIntelligence/Agamemnon/issues/118) — the tracking issue for this guide
- Issue [#26](https://github.com/HomericIntelligence/Agamemnon/issues/26) — the original Keystone → Agamemnon rename
- [README.md](../README.md#environment-variables) — full environment variable reference
- [RELEASING.md](../RELEASING.md) — release cadence for the Python client (where the shim lives)
