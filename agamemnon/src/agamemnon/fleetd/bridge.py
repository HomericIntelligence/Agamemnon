"""Validate durable admission, deliver private commands, and return worker facts."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from concurrent.futures import TimeoutError as FutureTimeout
from datetime import datetime
from typing import Protocol

from .common import (
    ASSIGNMENT,
    BridgeError,
    Deadline,
    Json,
    decode,
    digest,
    encode,
    identifier,
    require,
)
from .journal import DeliveryJournal
from .spool import PrivateSpool
from .transport import GatewayProcess, UnixWorker


class Authority(Protocol):
    def get_command(self, command_id: str, timeout: float) -> Json: ...

    def get_record(self, kind: str, target_id: str, timeout: float) -> Json: ...

    def confirm_fact(self, fact: Json, timeout: float) -> Json: ...


_OPERATIONS = {"start", "input", "respond", "interrupt", "cancel", "resume", "drain"}
_RECEIPT_FIELDS = {
    "sessionId",
    "providerThreadId",
    "providerTurnId",
    "requestId",
    "draining",
    "stage",
    "waitingReason",
    "error",
}
_ERROR_CODES = {
    "idempotency_conflict",
    "outcome_unknown",
    "command_id_conflict",
    "provider_uncertain",
    "session_not_found",
    "session_exists",
    "session_not_ready",
    "session_not_resumable",
    "worker_draining",
    "worker_capacity",
    "workspace_owned",
    "workspace_outside_root",
    "workspace_not_found",
    "workspace_not_directory",
    "request_not_owned",
    "no_active_turn",
    "invalid_input",
    "invalid_response",
    "invalid_request_id",
    "thread_identity_unknown",
    "unsupported_approval_decision",
    "invalid_answers",
    "invalid_request",
    "worker_failure",
}
_ACTIVITIES = {
    "model_working",
    "tool_running",
    "waiting_approval",
    "waiting_input",
    "idle",
    "disconnected",
    "unknown",
}
_WAITING = {
    None,
    "background_cleanup_unconfirmed",
    "provider_stop_confirmation",
    "provider_uncertain",
    "provider_error",
    "provider_disconnected",
    "restart_requires_resume",
    "item/commandExecution/requestApproval",
    "item/fileChange/requestApproval",
    "item/tool/requestUserInput",
}


class AttachmentBridge:
    """Drive one admitted binding; the controller remains the only task authority."""

    def __init__(
        self,
        *,
        gateway: GatewayProcess,
        worker: UnixWorker,
        authority: Authority,
        spool: PrivateSpool,
        journal: DeliveryJournal,
        worker_id: str,
        generation: int,
        timeout: float = 60,
        heartbeat_interval: float = 5,
    ) -> None:
        self.worker_id = identifier(worker_id, route=True)
        require(type(generation) is int and generation > 0, "invalid_generation")
        require(0 < heartbeat_interval < timeout <= 300, "invalid_deadline")
        self.generation, self.timeout, self.heartbeat_interval = (
            generation,
            timeout,
            heartbeat_interval,
        )
        self.gateway, self.worker, self.authority = gateway, worker, authority
        self.spool, self.journal = spool, journal
        journal.bind(worker_id, generation)

    def _command(self, delivery: Json) -> Json:
        require(isinstance(delivery.get("payload"), str), "invalid_delivery")
        command = decode(delivery["payload"].encode(), 128 * 1024)
        fields = {
            "schema",
            "commandId",
            "idempotencyKey",
            "workerId",
            "generation",
            "targetKind",
            "targetId",
            "operation",
            "payload",
            "correlationId",
            *ASSIGNMENT,
        }
        require(set(command) <= fields and command.get("schema") == "hi/fleet/v1", "command_schema")
        for key in ("commandId", "workerId", "targetId"):
            identifier(command.get(key), route=True)
        identifier(command.get("idempotencyKey"))
        require(
            command["workerId"] == self.worker_id
            and type(command.get("generation")) is int
            and command["generation"] == self.generation,
            "command_owner_or_generation",
        )
        require(
            command.get("targetKind") in {"sessions", "workers"}
            and command.get("operation") in _OPERATIONS
            and isinstance(command.get("payload"), dict),
            "command_contract",
        )
        require(
            (command["targetKind"] == "workers") == (command["operation"] == "drain"),
            "unsupported_target_operation",
        )
        if command["targetKind"] == "workers":
            require(command["targetId"] == self.worker_id, "wrong_worker_target")
        else:
            require(
                command.get("sessionId", command["targetId"]) == command["targetId"],
                "session_identity_conflict",
            )
        return command

    def _authorize(self, command: Json, delivery: Json, deadline: Deadline, replay: bool) -> None:
        state = self.authority.get_command(command["commandId"], deadline.remaining())
        require(state.get("command") == command, "durable_intent_mismatch")
        record = state.get("record")
        if not isinstance(record, dict):
            raise BridgeError("durable_record_invalid")
        self._record_owner(record, command["targetKind"], command["targetId"])
        for key in ASSIGNMENT:
            if key in command:
                require(record.get(key) == command[key], "durable_assignment_mismatch")
        if command["targetKind"] == "sessions" and not replay:
            require(record.get("claimStatus") in {"reserved", "claimed"}, "durable_claim_inactive")
        elif command["targetKind"] == "workers" and not replay:
            require(record.get("status") == "draining", "durable_worker_not_draining")
        require(
            state.get("status") in {"pending", "accepted", "completed", "failed"},
            "durable_command_status",
        )
        if not replay:
            require(
                state["status"] == "pending" and record.get("commandId") == command["commandId"],
                "durable_command_requires_reconcile",
            )
        subject = "hi.fleet.control." + self.worker_id
        if command["operation"] == "start" and command.get("taskId"):
            # A task-bearing start must arrive on the canonical role-task subject.
            subject = "hi.myrmidon.{}.{}.task.{}".format(
                identifier(record.get("domain"), route=True),
                identifier(record.get("hmasRole"), route=True),
                identifier(command["taskId"], route=True),
            )
        require(delivery.get("subject") == subject, "delivery_subject_mismatch")

    def _record_owner(self, record: Json, kind: str, target_id: str) -> None:
        worker = record.get("id") if kind == "workers" else record.get("workerId")
        require(
            record.get("schema") == "hi/fleet/v1"
            and record.get("id") == target_id
            and record.get("kind") == kind
            and worker == self.worker_id
            and type(record.get("generation")) is int
            and record["generation"] == self.generation,
            "durable_owner_or_generation",
        )

    def _receipt(self, command: Json, result: Json) -> Json:
        for key in ("schema", "commandId", "workerId", "generation", "targetKind", "targetId"):
            require(result.get(key) == command[key], "worker_receipt_identity")
        require(type(result.get("generation")) is int, "worker_receipt_identity")
        event_id = identifier(result.get("eventId"))
        require(
            result.get("status") in {"accepted", "completed", "failed"}, "worker_receipt_status"
        )
        receipt = result.get("receipt")
        if not isinstance(receipt, dict):
            raise BridgeError("worker_receipt_invalid")
        safe: Json = {}
        for key, value in receipt.items():
            if key not in _RECEIPT_FIELDS:
                continue
            if key == "draining":
                require(type(value) is bool, "worker_receipt_invalid")
            elif key == "error":
                value = (
                    value if isinstance(value, str) and value in _ERROR_CODES else "worker_failure"
                )
            elif key == "waitingReason":
                value = (
                    value
                    if isinstance(value, (str, type(None))) and value in _WAITING
                    else "unknown"
                )
            elif value is not None:
                if key == "requestId" and type(value) is int:
                    value = str(value)
                identifier(value)
            safe[key] = value
        fact = {
            key: command[key]
            for key in ("schema", "commandId", "workerId", "generation", "targetKind", "targetId")
        }
        fact.update(eventId=event_id, status=result["status"], receipt=safe)
        require(len(encode(fact)) <= 16 * 1024, "worker_receipt_limit")
        return fact

    def _publish(self, fact: Json, deadline: Deadline) -> None:
        response = self.gateway.request(
            "publish",
            deadline.remaining(),
            subject="hi.fleet.events." + self.worker_id,
            payload=encode(fact).decode(),
            messageId=fact["eventId"],
        )
        require(
            type(response.get("streamSequence")) is int and response["streamSequence"] > 0,
            "publish_confirmation_missing",
        )
        # Core NATS subscribers cannot replay an absent controller. Until that receiver
        # has durable delivery, confirm through its supported idempotent fact handler.
        confirmed = self.authority.confirm_fact(fact, deadline.remaining())
        require(
            confirmed.get("acknowledged") is True
            and confirmed.get("eventId") == fact["eventId"]
            and isinstance(confirmed.get("record"), dict),
            "owner_confirmation_missing",
        )
        self._record_owner(confirmed["record"], fact["targetKind"], fact["targetId"])

    def _exchange(self, command: Json, delivery_id: str, deadline: Deadline) -> Json:
        # The worker owns its subprocess. This thread only waits on a bounded socket request.
        with ThreadPoolExecutor(max_workers=1) as executor:
            pending = executor.submit(self.worker.exchange, command, deadline.remaining())
            while True:
                try:
                    return pending.result(
                        timeout=min(self.heartbeat_interval, deadline.remaining())
                    )
                except FutureTimeout:
                    self.gateway.request("inProgress", deadline.remaining(), deliveryId=delivery_id)

    def run_once(self) -> Json:
        """Pull one existing Keystone delivery. Failure leaves it unacknowledged."""
        deadline = Deadline(self.timeout)
        delivery = self.gateway.request("pull", deadline.remaining())
        if delivery.get("empty") is True:
            return {"status": "empty"}
        delivery_id = identifier(delivery.get("deliveryId"))
        command = self._command(delivery)
        command_id, command_digest = command["commandId"], digest(command)
        previous = self.journal.commands.get(command_id)
        key_owner = self.journal.keys.get(command["idempotencyKey"])
        require(key_owner in (None, command_id), "idempotency_conflict")
        if previous:
            require(previous["digest"] == command_digest, "command_identity_conflict")
        self._authorize(command, delivery, deadline, replay=previous is not None)
        if previous and "receipt" not in previous:
            raise BridgeError("outcome_unknown_requires_reconcile")
        self.gateway.request("inProgress", deadline.remaining(), deliveryId=delivery_id)
        if previous:
            fact = previous["receipt"]["fact"]
        else:
            materialized = self.spool.materialize(command)
            self.journal.append(
                "intent",
                {
                    "commandId": command_id,
                    "key": command["idempotencyKey"],
                    "digest": command_digest,
                    "materializedDigest": digest(materialized),
                },
            )
            try:
                result = self._exchange(materialized, delivery_id, deadline)
                fact = self._receipt(command, result)
                self.journal.append("receipt", {"commandId": command_id, "fact": fact})
            except BridgeError:
                self.journal.append("uncertain", {"commandId": command_id})
                raise
        self._publish(fact, deadline)
        self.journal.append("published", {"commandId": command_id, "eventId": fact["eventId"]})
        response = self.gateway.request("ack", deadline.remaining(), deliveryId=delivery_id)
        require(response.get("confirmation") == "broker", "delivery_ack_unconfirmed")
        self.journal.append("acknowledged", {"commandId": command_id, "eventId": fact["eventId"]})
        return {"status": "delivered", "commandId": command_id, "eventId": fact["eventId"]}

    def forward_events(self) -> int:
        """Publish a bounded worker event page; never derive issue completion from it."""
        deadline = Deadline(self.timeout)
        response = self.worker.exchange(
            {"operation": "events", "after": self.journal.cursor, "limit": 500},
            deadline.remaining(),
        )
        events = response.get("events")
        if not isinstance(events, list):
            raise BridgeError("worker_events_invalid")
        require(len(events) <= 500, "worker_events_invalid")
        count = 0
        for event in events:
            require(isinstance(event, dict), "worker_events_invalid")
            require(
                type(event.get("seq")) is int
                and event["seq"] == self.journal.cursor + 1
                and event.get("sourceSequence") == event["seq"],
                "worker_event_gap",
            )
            fact = self._activity(event, deadline)
            pending = self.journal.pending_events.get(event["seq"])
            if pending is None:
                self.journal.append(
                    "event", {"sequence": event["seq"], "fact": fact, "digest": digest(fact)}
                )
            else:
                require(pending["digest"] == digest(fact), "worker_event_identity_conflict")
                fact = pending["fact"]
            self._publish(fact, deadline)
            self.journal.append("cursor", {"sequence": event["seq"]})
            count += 1
        require(
            type(response.get("cursor")) is int and response["cursor"] == self.journal.cursor,
            "worker_event_cursor_mismatch",
        )
        return count

    def _activity(self, event: Json, deadline: Deadline) -> Json:
        require(
            event.get("schema") == "hi/fleet/v1"
            and event.get("workerId") == self.worker_id
            and type(event.get("generation")) is int
            and event["generation"] == self.generation
            and event.get("kind") == "activity"
            and event.get("targetKind") == "sessions",
            "worker_activity_identity",
        )
        target = identifier(event.get("targetId"), route=True)
        record = self.authority.get_record("sessions", target, deadline.remaining())
        self._record_owner(record, "sessions", target)
        body = event.get("event")
        if not isinstance(body, dict):
            raise BridgeError("worker_activity_invalid")
        require(
            isinstance(body.get("activity"), str) and body["activity"] in _ACTIVITIES,
            "worker_activity_invalid",
        )
        for field, expected in (
            ("workerId", self.worker_id),
            ("generation", self.generation),
            ("sessionId", target),
        ):
            require(field not in body or body[field] == expected, "worker_activity_identity")
        safe: Json = {"activity": body["activity"]}
        observed = body.get("observedAt")
        if not isinstance(observed, str):
            raise BridgeError("worker_activity_time")
        require(
            len(observed) <= 40 and "T" in observed,
            "worker_activity_time",
        )
        try:
            parsed = datetime.fromisoformat(observed.replace("Z", "+00:00"))
        except ValueError:
            raise BridgeError("worker_activity_time") from None
        require(parsed.tzinfo is not None, "worker_activity_time")
        safe["observedAt"] = observed
        for field in ("agentId", "taskId", "executionId", "sessionId", "stage"):
            if field in record and field in body:
                require(body[field] == record[field], "worker_activity_assignment")
                safe[field] = record[field]
        for field in ("providerThreadId", "providerTurnId", "requestId"):
            if body.get(field) is not None:
                safe[field] = identifier(str(body[field]))
        reason = body.get("waitingReason")
        safe["waitingReason"] = (
            reason if isinstance(reason, (str, type(None))) and reason in _WAITING else "unknown"
        )
        if body.get("outcome") in {"completed", "failed", "interrupted", "cancelled", "unknown"}:
            safe["outcome"] = body["outcome"]
        if body.get("backgroundCleanup") == "confirmed_empty":
            safe["backgroundCleanup"] = "confirmed_empty"
        if body.get("commandId") is not None:
            safe["commandId"] = identifier(body["commandId"], route=True)
        fact = {
            "schema": "hi/fleet/v1",
            "eventId": identifier(event.get("eventId")),
            "workerId": self.worker_id,
            "generation": self.generation,
            "kind": "activity",
            "sourceSequence": event["sourceSequence"],
            "seq": event["seq"],
            "targetKind": "sessions",
            "targetId": target,
            "event": safe,
        }
        require(len(encode(fact)) <= 16 * 1024, "worker_activity_limit")
        return fact
