"""Append-only delivery receipts; these records grant no task authority."""

from __future__ import annotations

import fcntl
import os
import stat
from pathlib import Path

from .common import MAX_FRAME, BridgeError, Json, decode, encode, private_directory, require


class DeliveryJournal:
    """Hold one writer lock and retain uncertainty instead of replaying commands."""

    def __init__(self, directory: Path, *, max_bytes: int = 64 * 1024 * 1024) -> None:
        self.commands: dict[str, Json] = {}
        self.keys: dict[str, str] = {}
        self.cursor = 0
        self.pending_events: dict[int, Json] = {}
        self.identity: tuple[str, int] | None = None
        self.max_bytes = max_bytes
        self.descriptor = -1
        directory_fd = private_directory(directory, os.getuid())
        try:
            self.descriptor = os.open(
                "deliveries.jsonl",
                os.O_CREAT | os.O_APPEND | os.O_RDWR | os.O_NOFOLLOW,
                0o600,
                dir_fd=directory_fd,
            )
            info = os.fstat(self.descriptor)
            require(
                stat.S_ISREG(info.st_mode)
                and info.st_nlink == 1
                and info.st_uid == os.getuid()
                and not info.st_mode & 0o077,
                "journal_owner_or_mode",
            )
            fcntl.flock(self.descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            require(info.st_size <= max_bytes, "journal_limit")
            with os.fdopen(os.dup(self.descriptor), "rb") as source:
                for line in source:
                    require(line.endswith(b"\n"), "journal_incomplete")
                    self._apply(decode(line))
        except (OSError, KeyError, TypeError, ValueError):
            self.close()
            raise BridgeError("journal_unavailable") from None
        except BridgeError:
            self.close()
            raise
        finally:
            os.close(directory_fd)

    def _apply(self, record: Json) -> None:
        kind, value = record["kind"], record["value"]
        require(isinstance(value, dict), "journal_invalid")
        if kind == "identity":
            identity = (value["workerId"], value["generation"])
            require(self.identity in (None, identity), "journal_identity_conflict")
            self.identity = identity
        elif kind == "intent":
            command_id = value["commandId"]
            require(
                command_id not in self.commands and value["key"] not in self.keys,
                "journal_duplicate_intent",
            )
            self.commands[command_id] = value
            self.keys[value["key"]] = command_id
        elif kind in {"receipt", "published", "acknowledged", "uncertain"}:
            self.commands[value["commandId"]][kind] = value
        elif kind == "cursor":
            require(
                type(value["sequence"]) is int and value["sequence"] > self.cursor,
                "journal_cursor_invalid",
            )
            self.cursor = value["sequence"]
            self.pending_events.pop(self.cursor, None)
        elif kind == "event":
            require(
                type(value["sequence"]) is int
                and value["sequence"] > self.cursor
                and value["sequence"] not in self.pending_events,
                "journal_event_invalid",
            )
            self.pending_events[value["sequence"]] = value
        else:
            raise BridgeError("journal_invalid")

    def bind(self, worker_id: str, generation: int) -> None:
        require(
            self.identity in (None, (worker_id, generation)),
            "journal_generation_requires_reconcile",
        )
        if self.identity is None:
            self.append("identity", {"workerId": worker_id, "generation": generation})

    def append(self, kind: str, value: Json) -> None:
        data = encode({"kind": kind, "value": value}) + b"\n"
        require(
            len(data) <= MAX_FRAME
            and os.fstat(self.descriptor).st_size + len(data) <= self.max_bytes,
            "journal_limit",
        )
        try:
            view = memoryview(data)
            while view:
                written = os.write(self.descriptor, view)
                require(written > 0, "journal_write_failed")
                view = view[written:]
            os.fsync(self.descriptor)
            self._apply(decode(data))
        except OSError:
            raise BridgeError("journal_write_failed") from None

    def close(self) -> None:
        if self.descriptor >= 0:
            os.close(self.descriptor)
            self.descriptor = -1
