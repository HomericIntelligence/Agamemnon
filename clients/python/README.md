# agamemnon-client

[![PyPI version](https://badge.fury.io/py/HomericIntelligence-Agamemnon.svg)](https://pypi.org/project/HomericIntelligence-Agamemnon/)

Async Python client for the [Agamemnon](https://github.com/HomericIntelligence/Agamemnon) REST API.

## Installation

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

## License

MIT
