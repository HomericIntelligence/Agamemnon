"""NATS event listener with input validation and graceful shutdown support."""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Protocol

import nats.errors
import nats.js.errors

from pydantic import ValidationError

from agamemnon.orchestration.models import TaskEvent
from agamemnon.orchestration.validation import validate_id

logger = logging.getLogger(__name__)


class TaskClaimer(Protocol):
    """Protocol for objects that can handle advance_dag_tracked calls."""

    def advance_dag_tracked(self, team_id: str) -> None:
        ...


class NATSListener:
    """Listens for NATS task events and dispatches advance_dag calls.

    Subjects follow the pattern ``hi.tasks.{team_id}.{task_id}.{event}``.
    Both ``team_id`` and ``task_id`` are validated before dispatch to prevent
    path traversal or injection from malformed NATS messages.

    Event payloads are parsed through :class:`~agamemnon.orchestration.models.TaskEvent` for
    type-safe, validated deserialization. Both flat (``{"status": "completed"}``)
    and Hermes-nested (``{"data": {"status": "completed"}}``) formats are handled.

    Once :meth:`begin_shutdown` is called, new events are logged and dropped
    without being dispatched — allowing in-flight operations started before
    shutdown to complete undisturbed.
    """

    _SUBJECT_PART_COUNT = 5

    def __init__(self, task_claimer: TaskClaimer) -> None:
        self._task_claimer = task_claimer
        self._shutting_down: bool = False

    @property
    def shutting_down(self) -> bool:
        """Return True if this listener is in shutdown mode."""
        return self._shutting_down

    def begin_shutdown(self) -> None:
        """Stop accepting new NATS events.

        After this call, :meth:`_on_task_event` will log and drop any incoming
        events rather than dispatching them to advance_dag.
        """
        self._shutting_down = True
        logger.info("nats_listener_shutdown_started")

    async def start(
        self,
        nc: Any,
        subject: str = "hi.tasks.>",
        *,
        stream: str = "homeric-tasks",
        max_retries: int = 5,
        retry_base_delay: float = 1.0,
    ) -> None:
        """Subscribe to a JetStream subject, retrying with backoff on stream-not-found.

        Args:
            nc: An active ``nats.aio.client.Client`` connection.
            subject: The NATS subject pattern to subscribe to.
            stream: The JetStream stream name (used in error messages).
            max_retries: Number of retry attempts when the stream is not found.
            retry_base_delay: Initial delay (seconds) before the first retry;
                doubles on each subsequent attempt.

        Raises:
            ConnectionError: If the NATS server is unreachable or authentication fails.
            RuntimeError: If the stream is not found after all retries are exhausted,
                or if JetStream is not available on the server.
        """
        js = nc.jetstream()
        last_exc: Exception | None = None
        delay = retry_base_delay

        for attempt in range(1, max_retries + 1):
            try:
                await js.subscribe(subject)
                logger.info(
                    "nats_listener_subscribed",
                    extra={"subject": subject, "stream": stream},
                )
                return
            except nats.js.errors.NotFoundError as exc:
                last_exc = exc
                logger.warning(
                    "nats_stream_not_found",
                    extra={
                        "stream": stream,
                        "subject": subject,
                        "attempt": attempt,
                        "max_retries": max_retries,
                        "retry_delay_seconds": delay,
                        "error": str(exc),
                    },
                )
                if attempt < max_retries:
                    await asyncio.sleep(delay)
                    delay *= 2
            except nats.errors.NoServersError as exc:
                logger.error(
                    "nats_no_servers",
                    extra={"subject": subject, "stream": stream, "error": str(exc)},
                )
                raise ConnectionError(
                    f"NATS server unreachable — cannot subscribe to '{subject}': {exc}"
                ) from exc
            except (
                nats.errors.AuthorizationError,
                nats.errors.InvalidUserCredentialsError,
            ) as exc:
                logger.error(
                    "nats_auth_error",
                    extra={"subject": subject, "stream": stream, "error": str(exc)},
                )
                raise ConnectionError(
                    f"NATS authentication failed — cannot subscribe to '{subject}': {exc}"
                ) from exc
            except nats.js.errors.ServiceUnavailableError as exc:
                logger.error(
                    "nats_jetstream_unavailable",
                    extra={"subject": subject, "stream": stream, "error": str(exc)},
                )
                raise RuntimeError(
                    "JetStream is not enabled on the connected NATS server"
                ) from exc

        logger.error(
            "nats_stream_not_found_fatal",
            extra={
                "stream": stream,
                "subject": subject,
                "attempts": max_retries,
                "error": str(last_exc),
            },
        )
        raise RuntimeError(
            f"NATS stream for task events not found — ensure stream '{stream}' is "
            f"configured (tried {max_retries} time(s)): {last_exc}"
        ) from last_exc

    async def _on_task_event(
        self,
        subject: str,
        team_id: str,
        task_id: str,
        raw_payload: bytes | None = None,
    ) -> None:
        """Handle an incoming NATS task event, validating IDs and payload before dispatch.

        Validates both ``team_id`` and ``task_id`` extracted from the NATS subject.
        If a raw payload is provided, it is parsed through
        :class:`~agamemnon.orchestration.models.TaskEvent` for type-safe deserialization.
        Malformed IDs or invalid JSON produce a warning log and the event is dropped
        without raising.

        Args:
            subject: The full NATS subject string (used for log context).
            team_id: The team ID extracted from the subject parts.
            task_id: The task ID extracted from the subject parts.
            raw_payload: Optional raw message bytes to parse as a TaskEvent.
        """
        if self._shutting_down:
            logger.info(
                "nats_event_dropped_during_shutdown",
                extra={"team_id": team_id, "subject": subject},
            )
            return

        try:
            validate_id(team_id, "team_id")
            validate_id(task_id, "task_id")
        except ValueError as exc:
            logger.warning(
                "nats_event_dropped_invalid_id",
                extra={"subject": subject, "error": str(exc)},
            )
            return

        event: TaskEvent | None = None
        if raw_payload is not None:
            try:
                payload_dict: Any = json.loads(raw_payload.decode())
            except Exception as exc:
                logger.warning(
                    "nats_event_dropped_invalid_json",
                    extra={"subject": subject, "error": str(exc)},
                )
                return

            try:
                event = TaskEvent.model_validate(payload_dict)
            except ValidationError as exc:
                logger.warning(
                    "nats_event_dropped_invalid_payload",
                    extra={"subject": subject, "error": str(exc)},
                )
                return

        logger.debug(
            "nats_event_received",
            extra={
                "team_id": team_id,
                "task_id": task_id,
                "subject": subject,
                "effective_status": event.effective_status if event else None,
            },
        )
        self._task_claimer.advance_dag_tracked(team_id)

    @staticmethod
    def _parse_subject(subject: str) -> tuple[str, str]:
        """Parse NATS subject 'hi.tasks.{team_id}.{task_id}.{event}' into (team_id, task_id).

        Raises ValueError if the subject does not have the expected 5-part structure.
        """
        parts = subject.split(".")
        if len(parts) != NATSListener._SUBJECT_PART_COUNT:
            raise ValueError(
                f"Expected {NATSListener._SUBJECT_PART_COUNT} subject parts, got {len(parts)}: {subject!r}"
            )
        return parts[2], parts[3]  # team_id, task_id

    async def stop(self) -> None:
        """Drain the NATS connection and release resources."""
        logger.info("nats_listener_stopped")
