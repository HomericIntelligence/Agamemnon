"""Supported durable reads, Keystone JSONL, and private worker socket exchange."""

from __future__ import annotations

import http.client
import io
import ipaddress
import os
import selectors
import socket
import ssl
import stat
import subprocess
import sys
import threading
import uuid
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from .common import (
    MAX_FRAME,
    BridgeError,
    Deadline,
    Json,
    decode,
    encode,
    identifier,
    private_directory,
    require,
)
from .observations import ObservationSink

ATTACH_SCHEMA = "hi/keystone/fleet-attach/v1"
_RESOLVE = """
import json, socket, sys
addresses = socket.getaddrinfo(sys.argv[1], int(sys.argv[2]), type=socket.SOCK_STREAM)
data = json.dumps({'addresses': addresses})
if len(data) > 16384:
    raise SystemExit(2)
sys.stdout.write(data)
"""


def _addresses(host: str, port: int, deadline: Deadline) -> list[Any]:
    """Keep uncancellable name resolution in one owned, bounded child process."""
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        address = None
    if address is not None and "%" not in host:
        family = socket.AF_INET if address.version == 4 else socket.AF_INET6
        endpoint = (host, port) if address.version == 4 else (host, port, 0, 0)
        return [(family, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", endpoint)]
    deadline.remaining()
    process = subprocess.Popen(
        [sys.executable, "-I", "-S", "-c", _RESOLVE, host, str(port)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    try:
        try:
            data, _ = process.communicate(timeout=deadline.remaining())
        except subprocess.TimeoutExpired:
            raise BridgeError("deadline_exceeded") from None
        deadline.remaining()
        require(process.returncode == 0, "durable_read_failed")
        addresses = decode(data, 16384).get("addresses")
        if not isinstance(addresses, list) or not 0 < len(addresses) <= 128:
            raise BridgeError("durable_read_failed")
        return addresses
    finally:
        # Reaping is cleanup only; it cannot extend successful request eligibility.
        try:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=1)
        finally:
            if process.stdout is not None:
                process.stdout.close()


def _connect(host: str, port: int, deadline: Deadline) -> socket.socket:
    """Apply the remaining budget to each address, closing every unsuccessful socket."""
    for family, kind, protocol, _name, address in _addresses(host, port, deadline):
        connection = None
        try:
            connection = socket.socket(family, kind, protocol)
            connection.settimeout(deadline.remaining())
            connection.connect(tuple(address))
            deadline.remaining()
            return connection
        except OSError:
            if connection is not None:
                connection.close()
        except BaseException:
            if connection is not None:
                connection.close()
            raise
    raise OSError("authority connection failed")


class _DeadlineReader(io.RawIOBase):
    """Apply the same budget to every receive within buffered HTTP parsing."""

    def __init__(self, sock: socket.socket, deadline: Deadline) -> None:
        self.sock, self.deadline = sock, deadline
        self.source = sock.makefile("rb", buffering=0)

    def readable(self) -> bool:
        return True

    def readinto(self, buffer: Any) -> int | None:
        self.sock.settimeout(self.deadline.remaining())
        count = self.source.readinto(buffer)
        self.deadline.remaining()
        return count

    def close(self) -> None:
        try:
            self.source.close()
        finally:
            super().close()


class _HTTPSConnection(http.client.HTTPSConnection):
    """Type the standard-library context without replacing its verification settings."""

    _context: ssl.SSLContext


class DurableClient:
    """Read supported Agamemnon resources; never list or claim work through REST."""

    def __init__(self, base_url: str, *, token: str | None = None) -> None:
        parsed = urlsplit(base_url)
        require(
            parsed.scheme in {"https", "http"}
            and parsed.hostname is not None
            and not parsed.username
            and not parsed.password
            and not parsed.query
            and not parsed.fragment
            and parsed.path in {"", "/"},
            "invalid_authority_url",
        )
        require(
            parsed.scheme == "https" or parsed.hostname in {"127.0.0.1", "::1", "localhost"},
            "authority_requires_tls",
        )
        self.url = parsed
        self.token = token

    def _request(self, method: str, path: str, timeout: float, body: Json | None = None) -> Json:
        deadline = Deadline(timeout)

        class Response(http.client.HTTPResponse):
            def __init__(
                self,
                sock: socket.socket,
                debuglevel: int = 0,
                method: str | None = None,
                url: str | None = None,
            ) -> None:
                super().__init__(sock, debuglevel, method, url)
                self.fp.close()
                self.fp = io.BufferedReader(_DeadlineReader(sock, deadline))

        factory = _HTTPSConnection if self.url.scheme == "https" else http.client.HTTPConnection
        connection = factory(self.url.hostname or "", self.url.port, timeout=deadline.remaining())
        connection.response_class = Response
        response = None
        try:
            headers = {"Accept": "application/json"}
            if self.token:
                headers["Authorization"] = "Bearer " + self.token
            payload = None if body is None else encode(body)
            if payload is not None:
                require(len(payload) <= 16 * 1024, "fact_limit")
                headers["Content-Type"] = "application/json"
                headers["Content-Length"] = str(len(payload))
            connection.sock = _connect(connection.host, connection.port, deadline)
            if isinstance(connection, _HTTPSConnection):
                connection.sock.settimeout(deadline.remaining())
                connection.sock = connection._context.wrap_socket(
                    connection.sock, server_hostname=connection.host, do_handshake_on_connect=False
                )
                connection.sock.settimeout(deadline.remaining())
                connection.sock.do_handshake()
                deadline.remaining()
            connection.putrequest(method, path)
            for name, value in headers.items():
                connection.putheader(name, value)
            assert connection.sock is not None
            connection.sock.settimeout(deadline.remaining())
            connection.endheaders()
            if payload is not None:
                connection.sock.settimeout(deadline.remaining())
                connection.send(payload)
            deadline.remaining()
            response = connection.getresponse()
            require(response.status == 200, "durable_read_failed")  # No redirect following.
            data = bytearray()
            while len(data) <= 512 * 1024:
                deadline.remaining()
                chunk = response.read1(min(8192, 512 * 1024 + 1 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
            result = decode(bytes(data), 512 * 1024)
            deadline.remaining()
            return result
        except (OSError, ValueError, http.client.HTTPException):
            raise BridgeError("durable_read_failed") from None
        finally:
            if response is not None:
                response.close()
            connection.close()

    def get_command(self, command_id: str, timeout: float) -> Json:
        return self._request(
            "GET", "/v1/fleet/commands/" + identifier(command_id, route=True), timeout
        )

    def get_record(self, kind: str, target_id: str, timeout: float) -> Json:
        require(kind in {"sessions", "workers"}, "unsupported_target")
        return self._request(
            "GET", "/v1/fleet/" + kind + "/" + identifier(target_id, route=True), timeout
        )

    def confirm_fact(self, fact: Json, timeout: float) -> Json:
        """Confirm the same published fact through the supported idempotent owner handler."""
        return self._request("POST", "/v1/fleet/events", timeout, fact)


class GatewayProcess:
    """One serialized authenticated attachment; transport observations grant no authority."""

    def __init__(self, argv: list[str], *, observations: ObservationSink | None = None) -> None:
        require(bool(argv) and all(isinstance(item, str) and item for item in argv), "gateway_argv")
        try:
            self.process = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                bufsize=0,
            )
        except OSError:
            raise BridgeError("gateway_start_failed") from None
        assert self.process.stdin is not None and self.process.stdout is not None
        os.set_blocking(self.process.stdin.fileno(), False)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.lock = threading.Lock()
        self.buffer = bytearray()
        self.observations = observations
        self.observations_seen = 0

    def request(self, operation: str, timeout: float, **fields: object) -> Json:
        deadline = Deadline(timeout)
        require(self.lock.acquire(timeout=deadline.remaining()), "gateway_busy")
        try:
            require(operation in {"pull", "publish", "ack", "inProgress"}, "gateway_operation")
            request_id = uuid.uuid4().hex
            data = (
                encode(
                    {
                        "schema": ATTACH_SCHEMA,
                        "requestId": request_id,
                        "operation": operation,
                        **fields,
                    }
                )
                + b"\n"
            )
            require(len(data) <= MAX_FRAME, "frame_limit")
            assert self.process.stdin is not None and self.process.stdout is not None
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdin, selectors.EVENT_WRITE)
                offset = 0
                while offset < len(data):
                    require(bool(selector.select(deadline.remaining())), "gateway_deadline")
                    offset += os.write(self.process.stdin.fileno(), data[offset:])
                selector.unregister(self.process.stdin)
                selector.register(self.process.stdout, selectors.EVENT_READ)
                for _ in range(1024):
                    while b"\n" not in self.buffer:
                        require(bool(selector.select(deadline.remaining())), "gateway_deadline")
                        chunk = os.read(self.process.stdout.fileno(), 8192)
                        require(bool(chunk), "gateway_disconnected")
                        self.buffer.extend(chunk)
                        require(len(self.buffer) <= MAX_FRAME, "frame_limit")
                    line, _, remainder = self.buffer.partition(b"\n")
                    self.buffer = bytearray(remainder)
                    frame = decode(bytes(line))
                    require(frame.get("schema") == ATTACH_SCHEMA, "gateway_schema")
                    if frame.get("type") == "observation":
                        self.observations_seen += 1
                        if self.observations is not None:
                            self.observations.offer(frame)
                        continue
                    require(
                        frame.get("type") == "response" and frame.get("requestId") == request_id,
                        "gateway_response_mismatch",
                    )
                    require(frame.get("ok") is True, "gateway_operation_failed")
                    if self.observations is not None:
                        self.observations.flush()
                    return frame
            raise BridgeError("gateway_observation_limit")
        except (OSError, ValueError):
            raise BridgeError("gateway_unavailable") from None
        finally:
            self.lock.release()

    def observation_status(self) -> Json:
        if self.observations is not None:
            return self.observations.status()
        return {
            "enabled": False,
            "seen": self.observations_seen,
            "written": 0,
            "dropped": self.observations_seen,
            "queued": 0,
            "coverage": "disabled",
            "historyComplete": False,
        }

    def close(self) -> None:
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        if self.process.stdout is not None:
            self.process.stdout.close()


class UnixWorker:
    """Exchange one JSONL frame with the already supervised worker; never start it."""

    def __init__(self, state_dir: Path) -> None:
        self.directory = state_dir

    def exchange(self, message: Json, timeout: float) -> Json:
        deadline = Deadline(timeout)
        directory = private_directory(self.directory, os.getuid())
        try:
            info = os.stat("worker.sock", dir_fd=directory, follow_symlinks=False)
            require(
                stat.S_ISSOCK(info.st_mode)
                and info.st_uid == os.getuid()
                and not info.st_mode & 0o077,
                "worker_socket_owner_or_mode",
            )
            data = encode(message) + b"\n"
            require(len(data) <= MAX_FRAME, "frame_limit")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(deadline.remaining())
                connection.connect(str(self.directory / "worker.sock"))
                connection.settimeout(deadline.remaining())
                connection.sendall(data)
                response = bytearray()
                while b"\n" not in response:
                    connection.settimeout(deadline.remaining())
                    chunk = connection.recv(min(8192, MAX_FRAME + 1 - len(response)))
                    require(bool(chunk), "worker_disconnected")
                    response.extend(chunk)
                    require(len(response) <= MAX_FRAME, "frame_limit")
                require(
                    response.endswith(b"\n") and response.count(b"\n") == 1, "worker_invalid_frame"
                )
                return decode(bytes(response))
        except OSError:
            raise BridgeError("worker_unavailable") from None
        finally:
            os.close(directory)
