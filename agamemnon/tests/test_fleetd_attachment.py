"""Real JSONL subprocess and Unix socket tests; no provider or broker access."""

from __future__ import annotations

import copy
import importlib
import importlib.util
import json
import os
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import pytest

REF = "a" * 32 + ".json"
PRIVATE = "private fixture input must never enter telemetry"
GATEWAY = r"""
import json, sys
delivery = json.loads(sys.argv[1])
for line in sys.stdin:
    request = json.loads(line)
    with open(sys.argv[2], "a") as log:
        log.write(json.dumps(request) + "\n")
    reply = {"schema": "hi/keystone/fleet-attach/v1", "type": "response",
             "requestId": request["requestId"], "ok": True}
    op = request["operation"]
    if op == "pull":
        reply.update(delivery)
    elif op == "publish":
        reply.update(streamSequence=7, duplicate=False)
    elif op == "ack":
        reply["confirmation"] = "broker"
    else:
        reply["confirmation"] = "transport"
    print(json.dumps(reply), flush=True)
"""


def api() -> Any:
    assert importlib.util.find_spec("agamemnon.fleetd") is not None, "Fleet bridge is absent"
    return importlib.import_module("agamemnon.fleetd")


def command(operation: str = "input") -> dict[str, Any]:
    return {
        "schema": "hi/fleet/v1",
        "commandId": "command-1",
        "idempotencyKey": "key-1",
        "workerId": "worker-1",
        "generation": 1,
        "targetKind": "sessions",
        "targetId": "session-1",
        "sessionId": "session-1",
        "taskId": "task-1",
        "agentId": "agent-1",
        "executionId": "execution-1",
        "workspace": "/work/task-1",
        "stage": "implementation",
        "operation": operation,
        "payload": {"inputRef": REF} if operation == "input" else {},
    }


def durable(value: dict[str, Any]) -> dict[str, Any]:
    record = {
        key: value[key]
        for key in (
            "workerId",
            "generation",
            "sessionId",
            "taskId",
            "agentId",
            "executionId",
            "workspace",
            "stage",
            "commandId",
        )
    }
    record.update(
        id="session-1",
        kind="sessions",
        schema="hi/fleet/v1",
        status="admitted",
        claimStatus="reserved",
        domain="pipeline",
        hmasRole="task-agent",
    )
    return {"command": copy.deepcopy(value), "status": "pending", "record": record}


class Authority:
    def __init__(self, value: dict[str, Any]) -> None:
        self.value = durable(value)
        self.fail = False
        self.calls = 0
        self.confirmations: list[dict[str, Any]] = []
        self.confirm_fail = False

    def get_command(self, command_id: str, timeout: float) -> dict[str, Any]:
        self.calls += 1
        assert command_id == "command-1"
        if self.fail:
            raise api().BridgeError("durable_read_failed")
        return copy.deepcopy(self.value)

    def get_record(self, kind: str, target_id: str, timeout: float) -> dict[str, Any]:
        assert kind == "sessions" and target_id == "session-1"
        return copy.deepcopy(self.value["record"])

    def confirm_fact(self, fact: dict[str, Any], timeout: float) -> dict[str, Any]:
        if self.confirm_fail:
            raise api().BridgeError("owner_confirmation_unavailable")
        self.confirmations.append(copy.deepcopy(fact))
        return {
            "acknowledged": True,
            "eventId": fact["eventId"],
            "record": copy.deepcopy(self.value["record"]),
        }


@pytest.fixture
def environment() -> Any:
    # Keep the socket below the macOS sockaddr_un path limit.
    test_root = Path.home() / ".codex" / "fleetd-tests"
    test_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix="fd-", dir=test_root) as temporary:
        root = Path(temporary).resolve()
        for name in ("spool", "journal", "worker", "workspace"):
            (root / name).mkdir(mode=0o700)
        private = {
            "schema": "hi/fleet/private-input/v1",
            "commandId": "command-1",
            "workerId": "worker-1",
            "generation": 1,
            "sessionId": "session-1",
            "kind": "input",
            "text": PRIVATE,
        }
        (root / "spool" / REF).write_text(json.dumps(private))
        (root / "spool" / REF).chmod(0o600)
        seen: list[dict[str, Any]] = []
        responses: list[Any] = []

        class Handler(socketserver.StreamRequestHandler):
            def handle(self) -> None:
                message = json.loads(self.rfile.readline())
                seen.append(message)
                if responses:
                    response = responses.pop(0)
                    if callable(response):
                        response = response()
                    if response is None:
                        return  # A lost worker reply has an uncertain execution outcome.
                elif message.get("operation") == "events":
                    response = {"events": [], "cursor": message["after"]}
                else:
                    response = {
                        "schema": "hi/fleet/v1",
                        "eventId": f"worker-1:1:{message['commandId']}:completed",
                        **{
                            key: message[key]
                            for key in (
                                "commandId",
                                "workerId",
                                "generation",
                                "targetKind",
                                "targetId",
                            )
                        },
                        "status": "completed",
                        "receipt": {"providerTurnId": "turn-1"},
                        "text": PRIVATE,
                    }
                self.wfile.write((json.dumps(response) + "\n").encode())

        with socketserver.UnixStreamServer(str(root / "worker" / "worker.sock"), Handler) as server:
            (root / "worker" / "worker.sock").chmod(0o600)
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
            thread.start()
            try:
                yield root, seen, responses
            finally:
                server.shutdown()
                thread.join(timeout=2)


def test_private_spool_rejects_shared_system_scratch(tmp_path: Path) -> None:
    tmp_path.chmod(0o700)
    with pytest.raises(api().BridgeError, match="private_spool_in_shared_scratch"):
        api().PrivateSpool(tmp_path.resolve())


def build(environment: Any, value: dict[str, Any] | None = None) -> Any:
    module = api()
    root, _, _ = environment
    value = value or command()
    if value.get("workspace") == "/work/task-1":
        value["workspace"] = str(root / "workspace")
    subject = "hi.fleet.control.worker-1"
    if value["operation"] == "start" and value.get("taskId"):
        subject = "hi.myrmidon.pipeline.task-agent.task." + value["taskId"]
    delivery = {
        "deliveryId": "delivery-1",
        "subject": subject,
        "payload": json.dumps(value),
        "streamSequence": 1,
        "numDelivered": 1,
    }
    gateway = module.GatewayProcess(
        [sys.executable, "-u", "-c", GATEWAY, json.dumps(delivery), str(root / "gateway.log")]
    )
    journal = module.DeliveryJournal(root / "journal")
    authority = Authority(value)
    bridge = module.AttachmentBridge(
        gateway=gateway,
        worker=module.UnixWorker(root / "worker"),
        authority=authority,
        spool=module.PrivateSpool(root / "spool"),
        journal=journal,
        worker_id="worker-1",
        generation=1,
        timeout=2,
        heartbeat_interval=0.2,
    )
    return bridge, authority, gateway, journal


def test_private_delivery_uses_real_subprocess_and_socket(environment: Any) -> None:
    root, seen, _ = environment
    bridge, authority, gateway, journal = build(environment)
    try:
        result = bridge.run_once()
        assert result["status"] == "delivered"
        assert authority.calls == 1
        assert seen[0]["payload"]["text"] == PRIVATE
        assert seen[0]["payload"]["agentId"] == "agent-1"
        output = (root / "gateway.log").read_text()
        assert PRIVATE not in output
        assert PRIVATE not in (root / "journal" / "deliveries.jsonl").read_text()
        frames = [json.loads(line) for line in output.splitlines()]
        assert [frame["operation"] for frame in frames] == ["pull", "inProgress", "publish", "ack"]
        receipt = json.loads(frames[2]["payload"])
        assert receipt["status"] == "completed"
        assert "claimStatus" not in receipt and "text" not in receipt
    finally:
        gateway.close()
        journal.close()


@pytest.mark.parametrize(
    "mutation", ["unavailable", "generation", "owner", "intent", "claim", "current"]
)
def test_failed_durable_validation_sends_nothing(environment: Any, mutation: str) -> None:
    root, seen, _ = environment
    bridge, authority, gateway, journal = build(environment)
    if mutation == "unavailable":
        authority.fail = True
    elif mutation == "generation":
        authority.value["record"]["generation"] = 2
    elif mutation == "owner":
        authority.value["record"]["workerId"] = "worker-2"
    elif mutation == "intent":
        authority.value["command"]["operation"] = "cancel"
    elif mutation == "claim":
        authority.value["record"]["claimStatus"] = "released"
    else:
        authority.value["record"]["commandId"] = "new-command"
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
        assert seen == []
        assert '"operation": "ack"' not in (root / "gateway.log").read_text()
    finally:
        gateway.close()
        journal.close()


@pytest.mark.parametrize(
    "mutation", ["traversal", "symlink", "mode", "scope", "oversize", "duplicate"]
)
def test_private_spool_rejects_unsafe_inputs(environment: Any, mutation: str) -> None:
    root, seen, _ = environment
    value = command()
    path = root / "spool" / REF
    if mutation == "traversal":
        value["payload"]["inputRef"] = "../" + REF
    elif mutation == "symlink":
        moved = root / "outside.json"
        path.rename(moved)
        path.symlink_to(moved)
    elif mutation == "mode":
        path.chmod(0o644)
    elif mutation == "scope":
        body = json.loads(path.read_text())
        body["sessionId"] = "other-session"
        path.write_text(json.dumps(body))
    elif mutation == "oversize":
        path.write_text("x" * (128 * 1024 + 1))
    else:
        path.write_text('{"schema":"x","schema":"y"}')
    bridge, _, gateway, journal = build(environment, value)
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
        assert seen == []
    finally:
        gateway.close()
        journal.close()


def test_wrong_owner_spool_is_rejected(environment: Any) -> None:
    root, _, _ = environment
    with pytest.raises(api().BridgeError):
        api().PrivateSpool(root / "spool", owner_uid=os.getuid() + 1)


def test_unknown_worker_outcome_never_replays(environment: Any) -> None:
    root, seen, responses = environment
    responses.append(None)
    bridge, _, gateway, journal = build(environment)
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
    finally:
        gateway.close()
        journal.close()
    bridge, _, gateway, journal = build(environment)
    try:
        with pytest.raises(api().BridgeError, match="outcome_unknown"):
            bridge.run_once()
        assert len(seen) == 1
        assert PRIVATE not in (root / "journal" / "deliveries.jsonl").read_text()
    finally:
        gateway.close()
        journal.close()


def test_confirmed_duplicate_republishes_receipt_without_private_body(environment: Any) -> None:
    root, seen, _ = environment
    bridge, _, gateway, journal = build(environment)
    try:
        bridge.run_once()
    finally:
        gateway.close()
        journal.close()
    (root / "spool" / REF).unlink()
    bridge, _, gateway, journal = build(environment)
    try:
        bridge.run_once()
        assert len(seen) == 1
    finally:
        gateway.close()
        journal.close()


def test_start_is_not_implicitly_a_turn(environment: Any) -> None:
    _, seen, _ = environment
    bridge, _, gateway, journal = build(environment, command("start"))
    try:
        bridge.run_once()
        assert [item["operation"] for item in seen] == ["start"]
        assert "text" not in seen[0]["payload"]
    finally:
        gateway.close()
        journal.close()


def test_conflicting_nested_identity_is_rejected(environment: Any) -> None:
    _, seen, _ = environment
    value = command("start")
    value["payload"]["agentId"] = "other-agent"
    bridge, _, gateway, journal = build(environment, value)
    try:
        with pytest.raises(api().BridgeError, match="identity_conflict"):
            bridge.run_once()
        assert seen == []
    finally:
        gateway.close()
        journal.close()


def test_cli_is_available_without_new_dependencies() -> None:
    result = subprocess.run(
        [sys.executable, "-m", "agamemnon.fleetd", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert "--spool-dir" in result.stdout


def test_cli_bindings_leave_shared_events_to_explicit_collector(
    environment: Any, monkeypatch: Any
) -> None:
    root, seen, _ = environment
    cli = importlib.import_module("agamemnon.fleetd.__main__")

    def invoke(binding: int, *, events_only: bool = False) -> None:
        value = command("start")
        value.update(
            commandId=f"command-{binding}",
            idempotencyKey=f"key-{binding}",
            targetId=f"session-{binding}",
            sessionId=f"session-{binding}",
            taskId=f"task-{binding}",
            agentId=f"agent-{binding}",
            executionId=f"execution-{binding}",
            workspace=f"/work/task-{binding}",
        )
        authority = Authority(value)
        authority.value["record"]["id"] = value["targetId"]
        monkeypatch.setattr(
            authority, "get_command", lambda command_id, timeout: copy.deepcopy(authority.value)
        )
        monkeypatch.setattr(cli, "DurableClient", lambda *args, **kwargs: authority)
        journal = root / f"binding-{binding}"
        journal.mkdir(mode=0o700)
        delivery = {
            "deliveryId": f"delivery-{binding}",
            "subject": f"hi.myrmidon.pipeline.task-agent.task.task-{binding}",
            "payload": json.dumps(value),
            "streamSequence": binding,
            "numDelivered": 1,
        }
        argv = [
            "agamemnon.fleetd",
            "--worker-id",
            "worker-1",
            "--generation",
            "1",
            "--spool-dir",
            str(root / "spool"),
            "--journal-dir",
            str(journal),
            "--worker-state-dir",
            str(root / "worker"),
            "--agamemnon-url",
            "http://127.0.0.1:8080",
        ]
        if events_only:
            argv.append("--events-only")
        argv.extend(
            [
                "--",
                sys.executable,
                "-u",
                "-c",
                GATEWAY,
                json.dumps(delivery),
                str(root / f"gateway-{binding}.log"),
            ]
        )
        monkeypatch.setattr(sys, "argv", argv)
        assert cli.main() == 0

    invoke(1)
    invoke(2)
    assert [request["operation"] for request in seen] == ["start", "start"]
    invoke(3, events_only=True)
    assert [request["operation"] for request in seen] == ["start", "start", "events"]


def test_response_ref_preserves_provider_request_id_type(environment: Any) -> None:
    root, seen, _ = environment
    body = json.loads((root / "spool" / REF).read_text())
    body.pop("text")
    body.update(kind="response", requestId=19, response={"decision": "accept"})
    (root / "spool" / REF).write_text(json.dumps(body))
    value = command("respond")
    value["payload"] = {"responseRef": REF, "requestId": "19"}
    bridge, _, gateway, journal = build(environment, value)
    try:
        bridge.run_once()
        assert seen[0]["payload"]["requestId"] == 19
        assert seen[0]["payload"]["response"] == {"decision": "accept"}
        assert "responseRef" not in seen[0]["payload"]
    finally:
        gateway.close()
        journal.close()


def test_publication_uncertainty_does_not_resend_private_command(environment: Any) -> None:
    _, seen, _ = environment
    bridge, _, gateway, journal = build(environment)
    original = gateway.request

    def lose_publish_reply(operation: str, timeout: float, **fields: object) -> Any:
        result = original(operation, timeout, **fields)
        if operation == "publish":
            raise api().BridgeError("gateway_disconnected")
        return result

    gateway.request = lose_publish_reply
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
    finally:
        gateway.close()
        journal.close()
    bridge, _, gateway, journal = build(environment)
    try:
        bridge.run_once()
        assert len(seen) == 1
    finally:
        gateway.close()
        journal.close()


def activity() -> dict[str, Any]:
    return {
        "schema": "hi/fleet/v1",
        "eventId": "worker-1:1:1",
        "workerId": "worker-1",
        "generation": 1,
        "seq": 1,
        "sourceSequence": 1,
        "kind": "activity",
        "targetKind": "sessions",
        "targetId": "session-1",
        "event": {
            "workerId": "worker-1",
            "generation": 1,
            "sessionId": "session-1",
            "taskId": "task-1",
            "agentId": "agent-1",
            "executionId": "execution-1",
            "stage": "implementation",
            "activity": "model_working",
            "observedAt": "2026-09-10T12:00:00.000Z",
            "waitingReason": None,
            "text": PRIVATE,
        },
    }


def test_activity_is_sanitized_and_cursor_advances_after_publication(environment: Any) -> None:
    root, _, responses = environment
    responses.append({"events": [activity()], "cursor": 1})
    bridge, _, gateway, journal = build(environment)
    try:
        assert bridge.forward_events() == 1
        assert journal.cursor == 1
        frames = [json.loads(line) for line in (root / "gateway.log").read_text().splitlines()]
        assert [frame["operation"] for frame in frames] == ["publish"]
        fact = json.loads(frames[0]["payload"])
        assert fact["event"]["activity"] == "model_working"
        assert "claimStatus" not in fact and PRIVATE not in encode_for_test(fact)
    finally:
        gateway.close()
        journal.close()


@pytest.mark.parametrize(
    "cleanup,activity_name,reason,expected_reason",
    [
        ("confirmed_empty", "idle", None, None),
        (
            "unconfirmed",
            "unknown",
            "background_cleanup_unconfirmed",
            "background_cleanup_unconfirmed",
        ),
        (PRIVATE, "unknown", PRIVATE, "unknown"),
    ],
)
def test_cleanup_confirmation_and_waiting_reason_survive_safe_forwarding(
    environment: Any, cleanup: str, activity_name: str, reason: Any, expected_reason: Any
) -> None:
    _, _, responses = environment
    event = activity()
    event["event"].update(backgroundCleanup=cleanup, activity=activity_name, waitingReason=reason)
    responses.append({"events": [event], "cursor": 1})
    bridge, authority, gateway, journal = build(environment)
    try:
        assert bridge.forward_events() == 1
        forwarded = authority.confirmations[0]["event"]
        assert forwarded.get("backgroundCleanup") == (
            "confirmed_empty" if cleanup == "confirmed_empty" else None
        )
        assert forwarded["waitingReason"] == expected_reason
        assert PRIVATE not in encode_for_test(forwarded)
    finally:
        gateway.close()
        journal.close()


@pytest.mark.parametrize(
    "relationship", ["equal", "workspace_parent", "workspace_child", "symlink", "missing"]
)
def test_private_inputs_reject_workspace_overlap_before_read(
    environment: Any, monkeypatch: Any, relationship: str
) -> None:
    root, _, _ = environment
    workspace = root / "source"
    workspace.mkdir()
    if relationship == "equal":
        workspace = root / "spool"
    elif relationship == "workspace_parent":
        workspace = root
    elif relationship == "workspace_child":
        workspace = root / "spool" / "nested-source"
        workspace.mkdir()
    elif relationship == "symlink":
        alias = root / "source-link"
        alias.symlink_to(workspace)
        workspace = alias
    else:
        workspace = root / "missing-source"
    value = command()
    value["workspace"] = str(workspace)
    spool = api().PrivateSpool(root / "spool")

    def forbidden_read(_reference: str) -> None:
        pytest.fail("An overlapping or invalid workspace reached the private body reader")

    monkeypatch.setattr(spool, "_read", forbidden_read)
    with pytest.raises(
        api().BridgeError, match="private_(spool_workspace_overlap|workspace_invalid)"
    ):
        spool.materialize(value)


def encode_for_test(value: Any) -> str:
    return json.dumps(value, sort_keys=True)


@pytest.mark.parametrize("mutation", ["gap", "owner", "timestamp", "nested_owner"])
def test_invalid_activity_cannot_advance_cursor(environment: Any, mutation: str) -> None:
    root, _, responses = environment
    event = activity()
    if mutation == "gap":
        event["seq"] = event["sourceSequence"] = 2
    elif mutation == "owner":
        event["workerId"] = "other-worker"
    elif mutation == "timestamp":
        event["event"]["observedAt"] = "PRIVATE-TIMESTAMP"
    else:
        event["event"]["workerId"] = "other-worker"
    responses.append({"events": [event], "cursor": event["seq"]})
    bridge, _, gateway, journal = build(environment)
    try:
        with pytest.raises(api().BridgeError):
            bridge.forward_events()
        assert journal.cursor == 0
        assert not (root / "gateway.log").exists()
    finally:
        gateway.close()
        journal.close()


def test_worker_timeout_is_bounded_and_renews_delivery(environment: Any) -> None:
    root, seen, responses = environment
    release = threading.Event()
    responses.append(lambda: release.wait(timeout=2) and None)
    bridge, _, gateway, journal = build(environment)
    bridge.timeout = 0.15
    bridge.heartbeat_interval = 0.025
    started = time.monotonic()
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
        assert time.monotonic() - started < 1.5
        assert len(seen) == 1
        frames = [json.loads(line) for line in (root / "gateway.log").read_text().splitlines()]
        assert sum(frame["operation"] == "inProgress" for frame in frames) >= 2
        assert not any(frame["operation"] == "ack" for frame in frames)
    finally:
        release.set()
        gateway.close()
        journal.close()


def test_journal_incomplete_tail_and_second_writer_fail_closed(environment: Any) -> None:
    root, _, _ = environment
    first = api().DeliveryJournal(root / "journal")
    try:
        with pytest.raises(api().BridgeError):
            api().DeliveryJournal(root / "journal")
    finally:
        first.close()
    with (root / "journal" / "deliveries.jsonl").open("ab") as stream:
        stream.write(b'{"kind":')
    with pytest.raises(api().BridgeError, match="journal_incomplete"):
        api().DeliveryJournal(root / "journal")


@pytest.mark.parametrize("status", [200, 302, 503])
def test_real_durable_http_read_and_redirect_rejection(environment: Any, status: int) -> None:
    root, seen, _ = environment
    value = command()
    value["workspace"] = str(root / "workspace")
    expected = durable(value)
    paths: list[str] = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            paths.append(self.path)
            self.send_response(status)
            self.send_header("Location", "/should-not-follow")
            self.end_headers()
            self.wfile.write(json.dumps(expected).encode())

        def do_POST(self) -> None:
            paths.append(self.path)
            fact = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            self.send_response(200)
            self.end_headers()
            self.wfile.write(
                json.dumps(
                    {"acknowledged": True, "eventId": fact["eventId"], "record": expected["record"]}
                ).encode()
            )

        def log_message(self, *_args: Any) -> None:
            return

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
        thread.start()
        bridge, _, gateway, journal = build(environment)
        bridge.authority = api().DurableClient(f"http://127.0.0.1:{server.server_port}")
        try:
            if status == 200:
                bridge.run_once()
                assert len(seen) == 1
            else:
                with pytest.raises(api().BridgeError, match="durable_read_failed"):
                    bridge.run_once()
                assert seen == []
            expected_paths = ["/v1/fleet/commands/command-1"]
            if status == 200:
                expected_paths.append("/v1/fleet/events")
            assert paths == expected_paths
        finally:
            gateway.close()
            journal.close()
            server.shutdown()
            thread.join(timeout=2)


def test_broker_confirmation_without_owner_does_not_ack_or_reexecute(environment: Any) -> None:
    root, seen, _ = environment
    bridge, authority, gateway, journal = build(environment)
    authority.confirm_fail = True
    try:
        with pytest.raises(api().BridgeError):
            bridge.run_once()
        frames = [json.loads(line) for line in (root / "gateway.log").read_text().splitlines()]
        assert frames[-1]["operation"] == "publish"
        assert "receipt" in journal.commands["command-1"]
        assert "acknowledged" not in journal.commands["command-1"]
    finally:
        gateway.close()
        journal.close()
    bridge, authority, gateway, journal = build(environment)
    try:
        bridge.run_once()
        assert len(seen) == 1 and len(authority.confirmations) == 1
    finally:
        gateway.close()
        journal.close()


def test_activity_cursor_waits_for_owner_confirmation(environment: Any) -> None:
    _, _, responses = environment
    responses.append({"events": [activity()], "cursor": 1})
    bridge, authority, gateway, journal = build(environment)
    authority.confirm_fail = True
    try:
        with pytest.raises(api().BridgeError):
            bridge.forward_events()
        assert journal.cursor == 0
        responses.append({"events": [activity()], "cursor": 1})
        authority.confirm_fail = False
        assert bridge.forward_events() == 1
        assert journal.cursor == 1
    finally:
        gateway.close()
        journal.close()


def test_cached_receipt_reconciliation_cannot_execute_after_claim_release(environment: Any) -> None:
    _, seen, _ = environment
    bridge, _, gateway, journal = build(environment)
    try:
        bridge.run_once()
    finally:
        gateway.close()
        journal.close()
    bridge, authority, gateway, journal = build(environment)
    authority.value["record"]["claimStatus"] = "released"
    authority.value["record"]["status"] = "cancelled"
    authority.value["status"] = "completed"
    try:
        bridge.run_once()
        assert len(seen) == 1
    finally:
        gateway.close()
        journal.close()
