"""Bounded framing and errors that cannot reveal private input."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import time
from pathlib import Path
from typing import Any

Json = dict[str, Any]
MAX_FRAME = 1024 * 1024
MAX_PRIVATE = 128 * 1024
IDENTITY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}\Z")
ROUTE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}\Z")
ASSIGNMENT = ("workspace", "agentId", "taskId", "sessionId", "executionId", "stage", "issueRefs")


class BridgeError(Exception):
    """A fixed error code, never an upstream exception or input fragment."""

    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


def require(condition: bool, code: str) -> None:
    if not condition:
        raise BridgeError(code)


def identifier(value: Any, *, route: bool = False) -> str:
    pattern = ROUTE_ID if route else IDENTITY
    require(isinstance(value, str) and pattern.fullmatch(value) is not None, "invalid_identity")
    return str(value)


def encode(value: Json) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    except (TypeError, ValueError):
        raise BridgeError("invalid_json") from None


def digest(value: Json) -> str:
    return hashlib.sha256(encode(value)).hexdigest()


def _pairs(values: list[tuple[str, Any]]) -> Json:
    result: Json = {}
    for key, value in values:
        require(key not in result, "duplicate_json_key")
        result[key] = value
    return result


def _constant(_value: str) -> Any:
    raise BridgeError("invalid_json")


def decode(data: bytes, limit: int = MAX_FRAME) -> Json:
    require(len(data) <= limit, "frame_limit")
    try:
        value = json.loads(data, object_pairs_hook=_pairs, parse_constant=_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise BridgeError("invalid_json") from None
    require(isinstance(value, dict), "invalid_object")
    return dict(value)


def private_directory(path: Path, owner_uid: int) -> int:
    """Open an existing canonical owner-only directory without following a link."""
    try:
        require(path.absolute() == path.resolve(strict=True), "private_directory_symlink")
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        info = os.fstat(descriptor)
        if info.st_uid != owner_uid or info.st_mode & 0o077 or not stat.S_ISDIR(info.st_mode):
            os.close(descriptor)
            raise BridgeError("private_directory_owner_or_mode")
        return descriptor
    except OSError:
        raise BridgeError("private_directory_unavailable") from None


class Deadline:
    def __init__(self, timeout: float) -> None:
        require(0 < timeout <= 300, "invalid_deadline")
        self.end = time.monotonic() + timeout

    def remaining(self) -> float:
        value = self.end - time.monotonic()
        require(value > 0, "deadline_exceeded")
        return value
