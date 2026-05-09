"""Configuration settings for the Keystone daemon."""

from __future__ import annotations

import os
from dataclasses import dataclass, field


@dataclass
class Settings:
    """Runtime configuration for the Keystone daemon.

    All values can be overridden via environment variables.
    """

    log_level: str = field(default_factory=lambda: os.environ.get("KEYSTONE_LOG_LEVEL", "INFO"))
    poll_interval: float = field(
        default_factory=lambda: float(os.environ.get("KEYSTONE_POLL_INTERVAL", "1.0"))
    )
    shutdown_timeout: float = field(
        default_factory=lambda: float(os.environ.get("KEYSTONE_SHUTDOWN_TIMEOUT", "30.0"))
    )


def load_settings() -> Settings:
    """Load settings from environment variables with defaults."""
    return Settings()
