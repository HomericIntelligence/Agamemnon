"""Structured JSON logging for ProjectKeystone using stdlib only."""

from __future__ import annotations

import json
import logging
import sys
import threading
from datetime import datetime, timezone
from typing import Any

# Pre-computed baseline attributes of a LogRecord to detect extra fields.
_BASELINE_ATTRS: frozenset[str] = frozenset(
    logging.LogRecord("", 0, "", 0, "", (), None).__dict__
)

# Reserved top-level field names that would conflict with the log schema.
_RESERVED_FIELDS = frozenset({"timestamp", "level", "logger", "message", "exception"})


class JsonFormatter(logging.Formatter):
    """Formats log records as single-line JSON objects."""

    def format(self, record: logging.LogRecord) -> str:
        output: dict[str, Any] = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
        }

        # Promote extra fields injected by the caller.
        for key, value in record.__dict__.items():
            if key not in _BASELINE_ATTRS:
                dest_key = f"ctx_{key}" if key in _RESERVED_FIELDS else key
                output[dest_key] = value

        if record.exc_info:
            output["exception"] = self.formatException(record.exc_info)

        return json.dumps(output, default=str)


class KeystoneLogger:
    """Thread-safe logger that binds context fields to every log record."""

    def __init__(self, logger: logging.Logger, context: dict[str, Any]) -> None:
        self._logger = logger
        self._context: dict[str, Any] = dict(context)
        self._lock = threading.Lock()

    def _log(self, level: int, msg: str, **kwargs: Any) -> None:
        with self._lock:
            ctx = dict(self._context)
        extra = {**kwargs, **ctx}
        self._logger.log(level, msg, extra=extra)

    def debug(self, msg: str, **kwargs: Any) -> None:
        """Log a DEBUG message with optional context fields."""
        self._log(logging.DEBUG, msg, **kwargs)

    def info(self, msg: str, **kwargs: Any) -> None:
        """Log an INFO message with optional context fields."""
        self._log(logging.INFO, msg, **kwargs)

    def warning(self, msg: str, **kwargs: Any) -> None:
        """Log a WARNING message with optional context fields."""
        self._log(logging.WARNING, msg, **kwargs)

    def error(self, msg: str, **kwargs: Any) -> None:
        """Log an ERROR message with optional context fields."""
        self._log(logging.ERROR, msg, **kwargs)

    def exception(self, msg: str, **kwargs: Any) -> None:
        """Log an ERROR message with exception info."""
        extra = {**kwargs}
        with self._lock:
            extra.update(self._context)
        self._logger.exception(msg, extra=extra)


def configure_logging(level: int = logging.INFO) -> None:
    """Configure the root logger with JSON output on stderr.

    Safe to call multiple times — will not add duplicate handlers.
    """
    root = logging.getLogger()
    root.setLevel(level)

    # Avoid adding a second StreamHandler if one already exists.
    for handler in root.handlers:
        if isinstance(handler, logging.StreamHandler) and handler.stream is sys.stderr:
            handler.setLevel(level)
            return

    handler = logging.StreamHandler(sys.stderr)
    handler.setLevel(level)
    handler.setFormatter(JsonFormatter())
    root.addHandler(handler)


def get_logger(*, component: str = "", **context: Any) -> KeystoneLogger:
    """Return a KeystoneLogger bound to the given component and context.

    Args:
        component: Appended to the ``keystone.`` logger name prefix.
        **context: Key-value pairs pre-bound to every log record.

    Returns:
        A :class:`KeystoneLogger` wrapping ``logging.getLogger(name)``.
    """
    name = f"keystone.{component}" if component else "keystone"
    return KeystoneLogger(logging.getLogger(name), context)
