"""Configuration settings for the Agamemnon orchestration daemon."""

from __future__ import annotations

import os
from dataclasses import dataclass, field

VALID_LOG_LEVELS: tuple[str, ...] = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")


def _validate_log_level(value: str) -> str:
    """Normalize and validate a log level string.

    Accepts the standard Python logging level names case-insensitively
    and returns the canonical uppercase form. Raises ``ValueError`` with
    a clear message listing accepted values for any other input so that
    misconfiguration fails loudly at startup rather than silently falling
    back to ``WARNING`` inside ``logging.basicConfig``.
    """
    if not isinstance(value, str):
        raise ValueError(
            f"AGAMEMNON_LOG_LEVEL must be a string, got {type(value).__name__}. "
            f"Accepted values: {', '.join(VALID_LOG_LEVELS)}."
        )
    normalized = value.strip().upper()
    if normalized not in VALID_LOG_LEVELS:
        raise ValueError(
            f"Invalid log level {value!r}. "
            f"Accepted values: {', '.join(VALID_LOG_LEVELS)}."
        )
    return normalized


@dataclass
class Settings:
    """Runtime configuration for the Agamemnon orchestration daemon.

    All values can be overridden via environment variables.
    """

    log_level: str = field(default_factory=lambda: os.environ.get("AGAMEMNON_LOG_LEVEL", "INFO"))
    poll_interval: float = field(
        default_factory=lambda: float(os.environ.get("AGAMEMNON_POLL_INTERVAL", "1.0"))
    )
    shutdown_timeout: float = field(
        default_factory=lambda: float(os.environ.get("AGAMEMNON_SHUTDOWN_TIMEOUT", "30.0"))
    )

    def __post_init__(self) -> None:
        self.log_level = _validate_log_level(self.log_level)

    def __setattr__(self, name: str, value: object) -> None:
        if name == "log_level" and isinstance(value, str):
            value = _validate_log_level(value)
        super().__setattr__(name, value)


def load_settings() -> Settings:
    """Load settings from environment variables with defaults."""
    return Settings()
