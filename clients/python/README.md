# agamemnon-client

[![PyPI version](https://badge.fury.io/py/HomericIntelligence-Agamemnon.svg)](https://pypi.org/project/HomericIntelligence-Agamemnon/)

Async Python client for the [Agamemnon](https://github.com/HomericIntelligence/Agamemnon) REST API.

## Installation

Requires Python 3.10 or newer. The client requires AnyIO 4.14.2 or newer to
include the CVE-2026-63374 fix. The patched upstream release does not support
Python 3.9, so the client no longer supports that interpreter.

```bash
pip install HomericIntelligence-Agamemnon
```

## Quick start

```python
import asyncio
from agamemnon_client import AgamemnonClient, AgamemnonConfig

async def main() -> None:
    async with AgamemnonClient(AgamemnonConfig(host="localhost", port=8080)) as client:
        health = await client.health()
        print(health)

asyncio.run(main())
```

## Fleet build calls

The `fleet_build_submit`, `fleet_build_status`, `fleet_build_cancel`,
`fleet_build_deliver`, `fleet_build_claim_run`, `fleet_build_fact`, and
`fleet_build_logs` methods use the existing authenticated controller client.
Claim and fact calls additionally require the separate supervisor key.
See the [subordinate build contract](../../docs/fleet.md) for admission,
replay, cancellation and evidence semantics.

Each build call requires a finite `AgamemnonConfig.timeout` and applies that
deadline to the whole request, including streamed response reads. Responses
must use identity content encoding and fit within 512 KiB. Redirects and other
encodings are rejected; requests are not automatically retried. The response
is closed on completion, failure or cancellation.

Service bridges must pass `trust_env=False` to
`AgamemnonClient(config, trust_env=False)` to disable HTTPX environment settings,
including inherited proxies. The keyword-only option defaults to `True` for
existing callers. These methods use the configured HTTP host and port; remote
transport qualification remains an operator responsibility.

## License

MIT
