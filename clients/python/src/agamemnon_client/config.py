"""Orchestration-layer settings loaded from environment variables."""

from __future__ import annotations

import os
import warnings
from dataclasses import dataclass


@dataclass
class Settings:
    """Runtime settings for the Agamemnon orchestration layer."""

    log_level: str
    poll_interval: float
    shutdown_timeout: float


def _get_with_fallback(new_name: str, old_name: str, default: str) -> str:
    """Return the value of *new_name*, falling back to *old_name* with a deprecation warning."""
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


def load_settings() -> Settings:
    """Load orchestration settings from environment variables."""
    return Settings(
        log_level=_get_with_fallback("AGAMEMNON_LOG_LEVEL", "KEYSTONE_LOG_LEVEL", "INFO"),
        poll_interval=float(
            _get_with_fallback("AGAMEMNON_POLL_INTERVAL", "KEYSTONE_POLL_INTERVAL", "1.0")
        ),
        shutdown_timeout=float(
            _get_with_fallback(
                "AGAMEMNON_SHUTDOWN_TIMEOUT", "KEYSTONE_SHUTDOWN_TIMEOUT", "30.0"
            )
        ),
    )
