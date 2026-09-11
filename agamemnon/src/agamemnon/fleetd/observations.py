"""Bounded, nonblocking, observation-only output to an operator-owned pipe."""

from __future__ import annotations

import fcntl
import os
import re
import stat
from collections import deque
from datetime import datetime

from .common import BridgeError, Json, encode, identifier, require

ATTACH_SCHEMA = "hi/keystone/fleet-attach/v1"
OPERATIONS = {"publish", "deliver", "redeliver", "ack", "nak", "inProgress"}
IDENTIFIERS = ("messageId", "correlationId", "taskId", "executionId", "agentId")
SUBJECT = re.compile(r"hi\.[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*\Z")
SOURCE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,511}\Z")


def sanitize(frame: Json) -> bytes:
    """Re-select the gateway's public metadata; never pass arbitrary nested fields."""
    require(
        frame.get("schema") == ATTACH_SCHEMA and frame.get("type") == "observation",
        "invalid_observation",
    )
    value = frame.get("observation")
    if not isinstance(value, dict):
        raise BridgeError("invalid_observation")
    require(value.get("schema") == "hi/fleet/observation/v1", "invalid_observation")
    require(value.get("operation") in OPERATIONS, "invalid_observation")
    require(
        value.get("result") in {"received", "confirmed", "unknown", "sent"}, "invalid_observation"
    )
    require(value.get("transport") == "nats-jetstream", "invalid_observation")
    for key in ("source", "target"):
        require(value.get(key) in {"Agamemnon", "Hephaestus", "Keystone"}, "invalid_observation")
    observed_at = value.get("observedAt")
    if not isinstance(observed_at, str) or len(observed_at) > 40:
        raise BridgeError("invalid_observation")
    try:
        timestamp = datetime.fromisoformat(observed_at.replace("Z", "+00:00"))
        require(timestamp.tzinfo is not None, "invalid_observation")
    except ValueError:
        raise BridgeError("invalid_observation") from None
    output = {
        key: value[key]
        for key in (
            "schema",
            "observedAt",
            "source",
            "target",
            "transport",
            "operation",
            "result",
        )
    }
    for key in ("eventId", "workerId", "consumerId", "stream"):
        output[key] = identifier(value.get(key))
    source_id = value.get("sourceId")
    require(
        isinstance(source_id, str) and SOURCE_ID.fullmatch(source_id) is not None,
        "invalid_observation",
    )
    output["sourceId"] = source_id
    for key in ("generation", "sourceSequence"):
        number = value.get(key)
        require(type(number) is int and 0 < number <= 2**53 - 1, "invalid_observation")
        output[key] = number
    subject = value.get("subject")
    require(
        isinstance(subject, str) and len(subject) <= 256 and SUBJECT.fullmatch(subject) is not None,
        "invalid_observation",
    )
    output["subject"] = subject
    for key in IDENTIFIERS:
        if key in value:
            output[key] = identifier(value[key])
    if "messageKind" in value:
        output["messageKind"] = identifier(value["messageKind"])
    if "bytes" in value and value["operation"] in {"publish", "deliver", "redeliver"}:
        require(
            type(value["bytes"]) is int and 0 <= value["bytes"] <= 2**53 - 1, "invalid_observation"
        )
        output["bytes"] = value["bytes"]
    data = encode({"schema": ATTACH_SCHEMA, "type": "observation", "observation": output}) + b"\n"
    require(len(data) <= 16 * 1024, "invalid_observation")
    return data


class ObservationSink:
    """Single-writer pipe with a bounded queue; writing grants no transport authority."""

    def __init__(self, descriptor: int, *, max_frames: int = 64, max_bytes: int = 65536) -> None:
        require(1 <= max_frames <= 64 and 1024 <= max_bytes <= 65536, "observation_queue_limit")
        self.fd: int | None = None
        try:
            require(
                type(descriptor) is int and descriptor >= 3,
                "observation_sink_requires_private_pipe",
            )
            info = os.fstat(descriptor)
            mode = fcntl.fcntl(descriptor, fcntl.F_GETFL) & os.O_ACCMODE
            # Anonymous pipes have no filesystem name; macOS reports their kernel
            # mode as 0660. Named FIFOs still require owner-only permissions.
            require(
                stat.S_ISFIFO(info.st_mode)
                and info.st_uid == os.getuid()
                and (info.st_nlink == 0 or not info.st_mode & 0o077)
                and mode == os.O_WRONLY,
                "observation_sink_requires_private_pipe",
            )
            self.fd = os.dup(descriptor)
            os.set_blocking(self.fd, False)
        except (OSError, ValueError):
            if self.fd is not None:
                os.close(self.fd)
            raise BridgeError("observation_sink_requires_private_pipe") from None
        self.max_frames = max_frames
        self.max_bytes = max_bytes
        self.queue: deque[bytes] = deque()
        self.offset = 0
        self.queued_bytes = 0
        self.seen = 0
        self.written = 0
        self.dropped = 0
        self.invalid = 0
        self.state = "open"

    def offer(self, frame: Json) -> None:
        self.seen += 1
        try:
            data = sanitize(frame)
        except (BridgeError, TypeError, ValueError):
            self.dropped += 1
            self.invalid += 1
            return
        if self.state != "open":
            self.dropped += 1
            return
        self.flush()
        if (
            self.state != "open"
            or len(self.queue) >= self.max_frames
            or self.queued_bytes + len(data) > self.max_bytes
        ):
            self.dropped += 1
            return
        self.queue.append(data)
        self.queued_bytes += len(data)
        self.flush()

    def _discard(self) -> None:
        self.dropped += len(self.queue)
        self.queue.clear()
        self.offset = 0
        self.queued_bytes = 0

    def flush(self) -> None:
        # No waits, sleeps, recursive gateway calls, or background unbounded writer.
        for _ in range(8):
            if self.state != "open" or not self.queue or self.fd is None:
                return
            try:
                count = os.write(self.fd, self.queue[0][self.offset :])
                if count <= 0:
                    raise OSError("closed output")
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                self.state = "unavailable"
                self._discard()
                return
            self.offset += count
            self.queued_bytes -= count
            if self.offset == len(self.queue[0]):
                self.queue.popleft()
                self.offset = 0
                self.written += 1

    def status(self) -> Json:
        return {
            "enabled": True,
            "seen": self.seen,
            "written": self.written,
            "dropped": self.dropped,
            "invalid": self.invalid,
            "queued": len(self.queue),
            "queuedBytes": self.queued_bytes,
            "maxBytes": self.max_bytes,
            "maxFrames": self.max_frames,
            "state": self.state,
            "coverage": "partial"
            if self.dropped or self.queue or self.state == "unavailable"
            else "observed",
            "historyComplete": False,
        }

    def close(self) -> None:
        if self.fd is None:
            return
        self.flush()
        self._discard()
        os.close(self.fd)
        self.fd = None
        if self.state == "open":
            self.state = "closed"
