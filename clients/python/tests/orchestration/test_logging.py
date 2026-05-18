"""Tests for agamemnon/orchestration/logging.py."""

from __future__ import annotations

import json
import logging
import sys
import threading
from datetime import datetime
from io import StringIO
from unittest.mock import patch

from agamemnon.orchestration.logging import (
    JsonFormatter,
    AgamemnonLogger,
    configure_logging,
    get_logger,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_record(
    msg: str = "hello",
    level: int = logging.INFO,
    name: str = "test",
    args: tuple = (),
    **extra: object,
) -> logging.LogRecord:
    record = logging.LogRecord(name, level, "", 0, msg, args, None)
    for key, value in extra.items():
        setattr(record, key, value)
    return record


def _format(record: logging.LogRecord) -> dict:
    return json.loads(JsonFormatter().format(record))


# ---------------------------------------------------------------------------
# TestJsonFormatter
# ---------------------------------------------------------------------------


class TestJsonFormatter:
    def test_output_is_valid_json(self) -> None:
        raw = JsonFormatter().format(_make_record())
        assert json.loads(raw) is not None

    def test_required_fields_present(self) -> None:
        data = _format(_make_record(name="agamemnon.test"))
        assert "timestamp" in data
        assert "level" in data
        assert "logger" in data
        assert "message" in data

    def test_logger_name_matches_record(self) -> None:
        data = _format(_make_record(name="agamemnon.daemon"))
        assert data["logger"] == "agamemnon.daemon"

    def test_level_matches_record(self) -> None:
        data = _format(_make_record(level=logging.WARNING))
        assert data["level"] == "WARNING"

    def test_extra_fields_promoted_to_top_level(self) -> None:
        data = _format(_make_record(team_id="t1", task_id="task-99", agent_id="a3"))
        assert data["team_id"] == "t1"
        assert data["task_id"] == "task-99"
        assert data["agent_id"] == "a3"

    def test_uses_get_message_not_msg(self) -> None:
        record = _make_record("hello %s", args=("world",))
        data = _format(record)
        assert data["message"] == "hello world"

    def test_timestamp_is_iso8601_utc(self) -> None:
        data = _format(_make_record())
        ts = datetime.fromisoformat(data["timestamp"])
        assert ts.tzinfo is not None
        assert ts.utcoffset().total_seconds() == 0  # type: ignore[union-attr]

    def test_reserved_field_collision_prefixed(self) -> None:
        data = _format(_make_record(level_custom=5))  # 'level' is reserved
        # level_custom is NOT reserved, so it appears as-is
        assert "level_custom" in data
        # Verify actual collision: inject a field named 'level'
        record = _make_record()
        setattr(record, "level", "INJECTED")
        out = _format(record)
        assert out.get("ctx_level") == "INJECTED"
        assert out["level"] != "INJECTED"

    def test_non_serializable_value_uses_str_fallback(self) -> None:
        class Unserializable:
            def __repr__(self) -> str:
                return "<Unserializable>"

        data = _format(_make_record(custom=Unserializable()))
        assert "custom" in data

    def test_exc_info_adds_exception_field(self) -> None:
        try:
            raise ValueError("boom")
        except ValueError:
            record = logging.LogRecord("test", logging.ERROR, "", 0, "err", (), sys.exc_info())
        data = _format(record)
        assert "exception" in data
        assert "ValueError" in data["exception"]

    def test_include_location_adds_pathname_lineno_funcname(self) -> None:
        """JsonFormatter with include_location=True must include location fields (line 40-42)."""
        record = _make_record(name="test")
        record.pathname = "/path/to/file.py"
        record.lineno = 42
        record.funcName = "test_func"

        formatter = JsonFormatter(include_location=True)
        data = json.loads(formatter.format(record))

        assert data["pathname"] == "/path/to/file.py"
        assert data["lineno"] == 42
        assert data["funcName"] == "test_func"

    def test_exclude_location_by_default(self) -> None:
        """JsonFormatter without include_location must exclude location fields."""
        record = _make_record(name="test")
        record.pathname = "/path/to/file.py"
        record.lineno = 42
        record.funcName = "test_func"

        formatter = JsonFormatter(include_location=False)
        data = json.loads(formatter.format(record))

        assert "pathname" not in data
        assert "lineno" not in data
        assert "funcName" not in data


# ---------------------------------------------------------------------------
# TestGetLogger
# ---------------------------------------------------------------------------


class TestGetLogger:
    def test_returns_agamemnon_logger(self) -> None:
        lg = get_logger(component="test")
        assert isinstance(lg, AgamemnonLogger)

    def test_component_sets_logger_name(self) -> None:
        lg = get_logger(component="daemon")
        assert lg._logger.name == "agamemnon.daemon"

    def test_no_component_uses_agamemnon_name(self) -> None:
        lg = get_logger()
        assert lg._logger.name == "agamemnon"

    def test_context_bound_at_construction(self) -> None:
        records: list[logging.LogRecord] = []

        class CapturingHandler(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record)

        handler = CapturingHandler()
        lg = get_logger(component="ctx_test", team_id="team-42")
        lg._logger.addHandler(handler)
        lg._logger.setLevel(logging.DEBUG)
        lg._logger.propagate = False

        lg.info("event")
        assert len(records) == 1
        assert records[0].__dict__.get("team_id") == "team-42"

        # Cleanup
        lg._logger.removeHandler(handler)
        lg._logger.propagate = True


# ---------------------------------------------------------------------------
# TestAgamemnonLoggerAdapter
# ---------------------------------------------------------------------------


class TestAgamemnonLoggerAdapter:
    def _capturing_logger(self, component: str) -> tuple[AgamemnonLogger, list[logging.LogRecord]]:
        records: list[logging.LogRecord] = []

        class Cap(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record)

        cap = Cap()
        lg = get_logger(component=component)
        lg._logger.addHandler(cap)
        lg._logger.setLevel(logging.DEBUG)
        lg._logger.propagate = False
        return lg, records

    def test_process_does_not_mutate_caller_extra(self) -> None:
        lg, records = self._capturing_logger("no_mutate")
        lg.info("first", foo="bar")
        lg.info("second", foo="baz")
        assert records[0].__dict__["foo"] == "bar"
        assert records[1].__dict__["foo"] == "baz"
        assert len(records) == 2
        lg._logger.handlers.clear()

    def test_per_call_extra_merged_with_context(self) -> None:
        lg = get_logger(component="merge_test", team_id="t1")
        records: list[logging.LogRecord] = []

        class Cap(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                records.append(record)

        cap = Cap()
        lg._logger.addHandler(cap)
        lg._logger.setLevel(logging.DEBUG)
        lg._logger.propagate = False

        lg.info("event", task_id="task-1")
        assert records[0].__dict__["team_id"] == "t1"
        assert records[0].__dict__["task_id"] == "task-1"
        lg._logger.handlers.clear()

    def test_thread_safe_context_read(self) -> None:
        lg = get_logger(component="threadsafe", team_id="t1")
        records: list[logging.LogRecord] = []
        errors: list[Exception] = []
        lock = threading.Lock()

        class Cap(logging.Handler):
            def emit(self, record: logging.LogRecord) -> None:
                with lock:
                    records.append(record)

        cap = Cap()
        lg._logger.addHandler(cap)
        lg._logger.setLevel(logging.DEBUG)
        lg._logger.propagate = False

        def worker() -> None:
            try:
                for _ in range(10):
                    lg.info("ping", task_id="x")
            except Exception as exc:
                with lock:
                    errors.append(exc)

        threads = [threading.Thread(target=worker) for _ in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors
        assert len(records) == 50
        lg._logger.handlers.clear()

    def test_debug_method_logs_at_debug_level(self) -> None:
        """AgamemnonLogger.debug() must log at DEBUG level (line 72)."""
        lg, records = self._capturing_logger("debug_test")
        lg.debug("debug_msg", key="value")
        assert len(records) == 1
        assert records[0].levelno == logging.DEBUG
        assert records[0].getMessage() == "debug_msg"
        lg._logger.handlers.clear()

    def test_warning_method_logs_at_warning_level(self) -> None:
        """AgamemnonLogger.warning() must log at WARNING level (line 80)."""
        lg, records = self._capturing_logger("warning_test")
        lg.warning("warn_msg", key="value")
        assert len(records) == 1
        assert records[0].levelno == logging.WARNING
        assert records[0].getMessage() == "warn_msg"
        lg._logger.handlers.clear()

    def test_error_method_logs_at_error_level(self) -> None:
        """AgamemnonLogger.error() must log at ERROR level (line 84)."""
        lg, records = self._capturing_logger("error_test")
        lg.error("error_msg", key="value")
        assert len(records) == 1
        assert records[0].levelno == logging.ERROR
        assert records[0].getMessage() == "error_msg"
        lg._logger.handlers.clear()

    def test_bind_creates_new_logger_with_merged_context(self) -> None:
        """AgamemnonLogger.bind() returns a new logger with merged context (line 99-101)."""
        lg1 = get_logger(component="bind_test", team_id="t1")
        lg2 = lg1.bind(task_id="task-1", agent_id="a1")

        # lg1 should still have only team_id
        assert lg1._context == {"team_id": "t1"}
        # lg2 should have merged context
        assert lg2._context == {"team_id": "t1", "task_id": "task-1", "agent_id": "a1"}

    def test_bind_overrides_existing_fields(self) -> None:
        """AgamemnonLogger.bind() overrides existing fields in context."""
        lg1 = get_logger(component="bind_override", team_id="t1", task_id="old-task")
        lg2 = lg1.bind(task_id="new-task")

        assert lg1._context["task_id"] == "old-task"
        assert lg2._context["task_id"] == "new-task"
        assert lg2._context["team_id"] == "t1"

    def test_bind_does_not_mutate_original(self) -> None:
        """AgamemnonLogger.bind() must not mutate the original logger's context."""
        lg1 = get_logger(component="bind_immutable", team_id="t1")
        original_context = dict(lg1._context)

        lg2 = lg1.bind(task_id="task-1")

        # Original should be unchanged
        assert lg1._context == original_context
        # New one should have the binding
        assert "task_id" in lg2._context


# ---------------------------------------------------------------------------
# TestConfigureLogging
# ---------------------------------------------------------------------------


class TestConfigureLogging:
    def setup_method(self) -> None:
        # Clear root handlers before each test.
        root = logging.getLogger()
        root.handlers = [
            h
            for h in root.handlers
            if not (isinstance(h, logging.StreamHandler) and h.stream is sys.stderr)
        ]

    def test_no_duplicate_handlers_on_repeated_calls(self) -> None:
        configure_logging()
        configure_logging()
        root = logging.getLogger()
        stderr_handlers = [
            h
            for h in root.handlers
            if isinstance(h, logging.StreamHandler) and h.stream is sys.stderr
        ]
        assert len(stderr_handlers) == 1

    def test_log_level_respected_debug(self) -> None:
        buf = StringIO()
        with patch("sys.stderr", buf):
            configure_logging(level=logging.DEBUG)
            logging.getLogger("agamemnon.level_test").debug("dbg_msg")
        assert "dbg_msg" in buf.getvalue()

    def test_log_level_respected_warning_filters_debug(self) -> None:
        buf = StringIO()
        with patch("sys.stderr", buf):
            configure_logging(level=logging.WARNING)
            logging.getLogger("agamemnon.level_test2").debug("should_not_appear")
        assert "should_not_appear" not in buf.getvalue()

    def test_output_goes_to_stderr(self) -> None:
        root = logging.getLogger()
        configure_logging()
        handler = next(
            h for h in root.handlers
            if isinstance(h, logging.StreamHandler) and h.stream is sys.stderr
        )
        assert handler.stream is sys.stderr
