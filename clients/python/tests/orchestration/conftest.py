"""Shared pytest fixtures and configuration for Agamemnon orchestration tests.

asyncio_mode = "auto" is configured in pyproject.toml [tool.pytest.ini_options],
so @pytest.mark.asyncio decorators are not required on individual async tests.
"""

from __future__ import annotations

from unittest.mock import AsyncMock, MagicMock

import pytest

from tests.orchestration.helpers import make_agent, make_task

__all__ = ["make_agent", "make_task"]


@pytest.fixture
def mock_http_client() -> MagicMock:
    """Return a mock HTTP client with an async ``get`` method."""
    http = MagicMock()
    http.get = AsyncMock()
    return http


@pytest.fixture
def mock_agamemnon_client() -> MagicMock:
    """Return a MagicMock standing in for an AgamemnonClient."""
    client = MagicMock()
    client.get_tasks = AsyncMock(return_value=[])
    client.claim_task = AsyncMock(return_value=True)
    return client
