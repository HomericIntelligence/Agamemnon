"""Observation output uses real subprocess pipes and never acquires work authority."""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import pytest

PRIVATE_INPUT_FIXTURE = "private payload never belongs in the observation output"
GATEWAY = r"""
import json, os, sys
frames = json.loads(sys.argv[1])
for line in sys.stdin:
    request = json.loads(line)
    with open(sys.argv[2], 'a') as log:
        log.write(json.dumps(request) + '\n')
    response = {'schema':'hi/keystone/fleet-attach/v1', 'type':'response',
                'requestId':request['requestId'], 'ok':True, 'empty':True}
    data = ''.join(json.dumps(frame) + '\n' for frame in frames + [response]).encode()
    # Split JSON tokens and frame boundaries across genuine subprocess writes.
    for offset in range(0, len(data), 13):
        os.write(1, data[offset:offset+13])
"""


def frame(sequence: int = 1) -> dict[str, Any]:
    return {
        "schema": "hi/keystone/fleet-attach/v1",
        "type": "observation",
        "payload": PRIVATE_INPUT_FIXTURE,
        "observation": {
            "schema": "hi/fleet/observation/v1",
            "eventId": f"epoch:{sequence}",
            "sourceId": "keystone:worker-1:agent-1:epoch",
            "sourceSequence": sequence,
            "observedAt": "2026-09-10T12:00:00.000Z",
            "source": "Agamemnon",
            "target": "Hephaestus",
            "workerId": "worker-1",
            "generation": 1,
            "transport": "nats-jetstream",
            "subject": "hi.myrmidon.pipeline.task-agent.task.task-1",
            "consumerId": "agent-1",
            "stream": "homeric-myrmidon",
            "messageId": "command-1",
            "messageKind": "start",
            "taskId": "task-1",
            "operation": "deliver",
            "result": "received",
            "bytes": 137,
            "payload": {"text": PRIVATE_INPUT_FIXTURE},
            "token": PRIVATE_INPUT_FIXTURE,
            "error": PRIVATE_INPUT_FIXTURE,
        },
    }


def modules() -> tuple[Any, Any]:
    return (
        importlib.import_module("agamemnon.fleetd.observations"),
        importlib.import_module("agamemnon.fleetd.transport"),
    )


@pytest.fixture
def private_root() -> Any:
    root = Path.home() / ".codex" / "fleetd-tests"
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix="fd-", dir=root) as temporary:
        yield Path(temporary).resolve()


def gateway(transport: Any, sink: Any, values: list[Any], path: Path) -> Any:
    return transport.GatewayProcess(
        [sys.executable, "-u", "-c", GATEWAY, json.dumps(values), str(path)],
        observations=sink,
    )


def drain(descriptor: int) -> bytes:
    os.set_blocking(descriptor, False)
    result = bytearray()
    while True:
        try:
            data = os.read(descriptor, 65536)
        except BlockingIOError:
            break
        if not data:
            break
        result.extend(data)
    return bytes(result)


def test_fragmented_interleaved_gateway_frames_keep_sequence_and_redact(tmp_path: Path) -> None:
    observation, transport = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer)
    child = gateway(transport, sink, [frame(1), frame(2)], tmp_path / "requests")
    try:
        assert child.request("pull", 2)["empty"] is True
        received = [json.loads(line) for line in drain(reader).splitlines()]
        assert [item["observation"]["sourceSequence"] for item in received] == [1, 2]
        assert all(
            item["observation"]["sourceId"] == frame()["observation"]["sourceId"]
            for item in received
        )
        assert PRIVATE_INPUT_FIXTURE not in json.dumps(received)
        assert received[0]["observation"]["bytes"] == 137
        assert sink.status()["written"] == 2
        assert [
            json.loads(line)["operation"]
            for line in (tmp_path / "requests").read_text().splitlines()
        ] == ["pull"]
    finally:
        child.close()
        sink.close()
        os.close(reader)
        os.close(writer)


def test_full_sink_is_bounded_and_does_not_block_or_ack_work(tmp_path: Path) -> None:
    observation, transport = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer, max_frames=2, max_bytes=8192)
    child = gateway(transport, sink, [frame(1), frame(2), frame(3)], tmp_path / "requests")
    try:
        while True:
            try:
                os.write(writer, b"x" * 4096)
            except BlockingIOError:
                break
        started = time.monotonic()
        assert child.request("pull", 2)["empty"] is True
        assert time.monotonic() - started < 1
        status = sink.status()
        assert (status["seen"], status["written"], status["queued"], status["dropped"]) == (
            3,
            0,
            2,
            1,
        )
        assert status["queuedBytes"] <= 8192
        assert [
            json.loads(line)["operation"]
            for line in (tmp_path / "requests").read_text().splitlines()
        ] == ["pull"]
        sink.close()
        assert sink.status()["dropped"] == 3
        assert sink.status()["historyComplete"] is False
    finally:
        child.close()
        sink.close()
        os.close(reader)
        os.close(writer)


def test_closed_reader_does_not_change_control_response(tmp_path: Path) -> None:
    observation, transport = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer)
    os.close(reader)
    child = gateway(transport, sink, [frame()], tmp_path / "requests")
    try:
        assert child.request("inProgress", 2, deliveryId="delivery-1")["ok"] is True
        assert sink.status()["dropped"] == 1
        assert sink.status()["state"] == "unavailable"
        assert json.loads((tmp_path / "requests").read_text())["operation"] == "inProgress"
    finally:
        child.close()
        sink.close()
        os.close(writer)


def test_partial_writes_preserve_jsonl_boundaries(monkeypatch: Any) -> None:
    observation, _ = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer)
    actual_write = os.write

    def fragmented(descriptor: int, data: bytes) -> int:
        return actual_write(descriptor, data[:17])

    monkeypatch.setattr(observation.os, "write", fragmented)
    try:
        sink.offer(frame(1))
        sink.offer(frame(2))
        result = bytearray(drain(reader))
        for _ in range(30):
            sink.flush()
            result.extend(drain(reader))
            if sink.status()["queued"] == 0:
                break
        received = [json.loads(line) for line in result.splitlines()]
        assert [item["observation"]["sourceSequence"] for item in received] == [1, 2]
        assert sink.status()["written"] == 2
        assert sink.status()["dropped"] == 0
    finally:
        sink.close()
        os.close(reader)
        os.close(writer)


@pytest.mark.parametrize(
    "field,value",
    [("sourceSequence", True), ("sourceId", PRIVATE_INPUT_FIXTURE), ("observedAt", PRIVATE_INPUT_FIXTURE), ("operation", PRIVATE_INPUT_FIXTURE)],
)
def test_invalid_observation_is_dropped_without_private_content(field: str, value: Any) -> None:
    observation, _ = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer)
    try:
        unsafe = frame()
        unsafe["observation"][field] = value
        sink.offer(unsafe)
        assert drain(reader) == b""
        assert sink.status()["dropped"] == 1
        assert PRIVATE_INPUT_FIXTURE not in json.dumps(sink.status())
    finally:
        sink.close()
        os.close(reader)
        os.close(writer)


def test_observation_output_defaults_off(tmp_path: Path) -> None:
    _, transport = modules()
    child = gateway(transport, None, [frame()], tmp_path / "requests")
    try:
        assert child.request("pull", 2)["empty"] is True
        assert child.observation_status()["enabled"] is False
        assert child.observation_status()["seen"] == 1
        assert child.observation_status()["historyComplete"] is False
    finally:
        child.close()


def test_observation_sink_rejects_regular_file_and_standard_output(tmp_path: Path) -> None:
    observation, _ = modules()
    with (tmp_path / "not-a-pipe").open("wb") as output:
        with pytest.raises(Exception, match="observation_sink_requires_private_pipe"):
            observation.ObservationSink(output.fileno())
    with pytest.raises(Exception, match="observation_sink_requires_private_pipe"):
        observation.ObservationSink(1)


def test_named_fifo_requires_private_permissions(tmp_path: Path) -> None:
    observation, _ = modules()
    path = tmp_path / "flow.fifo"
    os.mkfifo(path, 0o600)
    reader = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    writer = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    try:
        sink = observation.ObservationSink(writer)
        sink.offer(frame())
        assert json.loads(drain(reader))["observation"]["sourceSequence"] == 1
        sink.close()
        path.chmod(0o644)
        with pytest.raises(Exception, match="observation_sink_requires_private_pipe"):
            observation.ObservationSink(writer)
    finally:
        os.close(reader)
        os.close(writer)


def test_compound_source_identity_retains_full_gateway_namespace() -> None:
    observation, _ = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer)
    try:
        value = frame()
        source_id = "keystone:" + "w" * 128 + ":" + "c" * 128 + ":" + "e" * 32
        value["observation"]["sourceId"] = source_id
        sink.offer(value)
        assert json.loads(drain(reader))["observation"]["sourceId"] == source_id
        value["observation"]["sourceId"] = "x" * 513
        sink.offer(value)
        assert drain(reader) == b""
        assert sink.status()["dropped"] == 1
    finally:
        sink.close()
        os.close(reader)
        os.close(writer)


def test_cli_inherited_observation_fd_reports_delivery_coverage(private_root: Path) -> None:
    root = private_root
    for name in ("spool", "journal", "worker"):
        (root / name).mkdir(mode=0o700)
    reader, writer = os.pipe()
    try:
        result = subprocess.run(
            [
                sys.executable,
                "-m",
                "agamemnon.fleetd",
                "--worker-id",
                "worker-1",
                "--generation",
                "1",
                "--spool-dir",
                str(root / "spool"),
                "--journal-dir",
                str(root / "journal"),
                "--worker-state-dir",
                str(root / "worker"),
                "--agamemnon-url",
                "http://127.0.0.1:1",
                "--observations-fd",
                str(writer),
                "--",
                sys.executable,
                "-u",
                "-c",
                GATEWAY,
                json.dumps([frame()]),
                str(root / "requests"),
            ],
            pass_fds=(writer,),
            capture_output=True,
            text=True,
            timeout=4,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        received = [json.loads(line) for line in drain(reader).splitlines()]
        assert len(received) == 1
        status = [json.loads(line) for line in result.stdout.splitlines()]
        assert status[0]["status"] == "empty"
        assert status[-1]["observations"]["written"] == 1
        assert status[-1]["observations"]["state"] == "closed"
        assert status[-1]["observations"]["historyComplete"] is False
        assert PRIVATE_INPUT_FIXTURE not in json.dumps(received) + result.stdout + result.stderr
    finally:
        os.close(reader)
        os.close(writer)


def test_partial_frame_survives_backpressure_without_merging_next_frame(monkeypatch: Any) -> None:
    observation, _ = modules()
    reader, writer = os.pipe()
    sink = observation.ObservationSink(writer, max_frames=1)
    actual_write = os.write
    blocked = False

    def fragmented(descriptor: int, data: bytes) -> int:
        if blocked:
            raise BlockingIOError
        return actual_write(descriptor, data[:17])

    monkeypatch.setattr(observation.os, "write", fragmented)
    try:
        sink.offer(frame(1))
        blocked = True
        sink.offer(frame(2))
        assert sink.status()["dropped"] == 1
        blocked = False
        result = bytearray(drain(reader))
        for _ in range(20):
            sink.flush()
            result.extend(drain(reader))
        sink.offer(frame(3))
        for _ in range(20):
            sink.flush()
            result.extend(drain(reader))
        received = [json.loads(line) for line in result.splitlines()]
        assert [item["observation"]["sourceSequence"] for item in received] == [1, 3]
        assert sink.status()["written"] == 2
    finally:
        sink.close()
        os.close(reader)
        os.close(writer)
