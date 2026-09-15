"""Resolve scoped private inputs without copying them into control metadata."""

from __future__ import annotations

import os
import re
import stat
import tempfile
from pathlib import Path

from .common import ASSIGNMENT, MAX_PRIVATE, BridgeError, Json, decode, private_directory, require

REFERENCE = re.compile(r"[0-9a-f]{32}\.json\Z")


class PrivateSpool:
    """Read only owner-written, regular input objects in one private directory."""

    def __init__(self, directory: Path, *, owner_uid: int | None = None) -> None:
        self.directory = directory
        self.owner_uid = os.getuid() if owner_uid is None else owner_uid
        descriptor = self._open_directory()
        os.close(descriptor)

    def _open_directory(self) -> int:
        descriptor = private_directory(self.directory, self.owner_uid)
        try:
            path = self.directory.resolve(strict=True)
            shared = (
                Path("/tmp"),
                Path("/var/tmp"),
                Path("/private/var/folders"),
                Path(tempfile.gettempdir()),
            )
            require(
                not any(path.is_relative_to(root.resolve()) for root in shared),
                "private_spool_in_shared_scratch",
            )
            return descriptor
        except Exception:
            os.close(descriptor)
            raise

    def _read(self, reference: str) -> Json:
        require(
            isinstance(reference, str) and REFERENCE.fullmatch(reference) is not None,
            "invalid_private_reference",
        )
        directory = self._open_directory()
        descriptor = -1
        try:
            descriptor = os.open(
                reference, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory
            )
            before = os.fstat(descriptor)
            require(
                stat.S_ISREG(before.st_mode) and before.st_nlink == 1, "private_input_not_regular"
            )
            require(
                before.st_uid == self.owner_uid and not before.st_mode & 0o077,
                "private_input_owner_or_mode",
            )
            require(before.st_size <= MAX_PRIVATE, "private_input_limit")
            data = bytearray()
            while len(data) <= MAX_PRIVATE:
                part = os.read(descriptor, min(8192, MAX_PRIVATE + 1 - len(data)))
                if not part:
                    break
                data.extend(part)
            after = os.fstat(descriptor)
            require(
                (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns),
                "private_input_changed",
            )
            return decode(bytes(data), MAX_PRIVATE)
        except OSError:
            raise BridgeError("private_input_unavailable") from None
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            os.close(directory)

    def materialize(self, command: Json) -> Json:
        """Build one worker command; never synthesize an initial or follow-up turn."""
        payload = dict(command["payload"])
        for key in ASSIGNMENT:
            if key in command:
                require(key not in payload or payload[key] == command[key], "identity_conflict")
                payload[key] = command[key]
        fields = set(payload) - set(ASSIGNMENT)
        operation = command["operation"]
        if operation in {"input", "respond"}:
            self._check_workspace(payload.get("workspace"))
        if operation in {"start", "resume", "interrupt", "cancel", "drain"}:
            require(not fields, "unexpected_private_input")
        else:
            if operation == "input":
                require(fields in ({"promptRef"}, {"inputRef"}), "input_reference_required")
                reference_key = next(iter(fields))
                kind = "prompt" if reference_key == "promptRef" else "input"
            else:
                require(
                    operation == "respond" and fields == {"responseRef", "requestId"},
                    "response_reference_required",
                )
                reference_key, kind = "responseRef", "response"
            body = self._read(payload[reference_key])
            scope = {
                "schema": "hi/fleet/private-input/v1",
                "commandId": command["commandId"],
                "workerId": command["workerId"],
                "generation": command["generation"],
                "sessionId": command["targetId"],
                "kind": kind,
            }
            require(
                all(body.get(key) == value for key, value in scope.items()),
                "private_input_scope_mismatch",
            )
            require(type(body.get("generation")) is int, "private_input_scope_mismatch")
            payload.pop(reference_key)
            if kind == "response":
                require(set(body) == set(scope) | {"requestId", "response"}, "private_input_fields")
                request_id = body["requestId"]
                require(
                    type(request_id) in (str, int) and str(request_id) == str(payload["requestId"]),
                    "private_request_mismatch",
                )
                require(isinstance(body["response"], dict), "private_response_invalid")
                payload["requestId"], payload["response"] = request_id, body["response"]
            else:
                require(set(body) == set(scope) | {"text"}, "private_input_fields")
                require(
                    isinstance(body["text"], str) and bool(body["text"]), "private_text_invalid"
                )
                payload["text"] = body["text"]
        return {**command, "payload": payload}

    def _check_workspace(self, value: object) -> None:
        if not isinstance(value, str) or not value:
            raise BridgeError("private_workspace_invalid")
        try:
            workspace = Path(value)
            canonical = workspace.resolve(strict=True)
            require(
                workspace.is_absolute() and canonical == workspace and canonical.is_dir(),
                "private_workspace_invalid",
            )
            spool = self.directory.resolve(strict=True)
            require(
                not canonical.is_relative_to(spool) and not spool.is_relative_to(canonical),
                "private_spool_workspace_overlap",
            )
        except OSError:
            raise BridgeError("private_workspace_invalid") from None
