"""Keystone daemon — routes messages between publishers and subscribers."""

from __future__ import annotations

import argparse
import asyncio
import logging as stdlib_logging
import signal
import sys
import time
from typing import Any

from agamemnon.orchestration.config import Settings, load_settings
from agamemnon.orchestration.logging import KeystoneLogger, configure_logging, get_logger
from agamemnon.orchestration.nats_listener import NATSListener
from agamemnon.orchestration.task_claimer import TaskClaimer

logger: KeystoneLogger = get_logger(component="daemon")

_shutdown_event: asyncio.Event | None = None


def handle_sigterm(signum: int, frame: Any) -> None:
    """Handle SIGTERM by raising SystemExit so cleanup runs."""
    raise SystemExit(0)


def _handle_signal(signum: int, listener: NATSListener) -> None:
    """Set the shutdown event and stop the listener from accepting new events."""
    logger.info("shutdown_signal_received", signal=signum)
    listener.begin_shutdown()
    if _shutdown_event is not None:
        _shutdown_event.set()


def run_routing_loop(poll_interval: float = 1.0) -> None:
    """Run the main message-routing loop until interrupted."""
    logger.info("routing_loop_started", poll_interval=poll_interval)
    try:
        while True:
            time.sleep(poll_interval)
    except (KeyboardInterrupt, SystemExit):
        logger.info("routing_loop_stopped")


def assign_task(team_id: str, task_id: str, agent_id: str) -> None:
    """Record a task assignment in the structured log."""
    logger.info(
        "task_assigned",
        team_id=team_id,
        task_id=task_id,
        agent_id=agent_id,
    )


async def run(settings: Settings) -> int:
    """Run the daemon until a shutdown signal is received.

    Shutdown sequence:
    1. Stop accepting new NATS events (begin_shutdown).
    2. Wait for in-flight advance_dag calls to complete (drain with timeout).
    3. Drain the NATS connection (listener.stop).
    """
    global _shutdown_event
    _shutdown_event = asyncio.Event()

    async def _noop_get_tasks(team_id: str) -> list[dict[str, Any]]:
        return []

    async def _noop_claim_task(team_id: str, task_id: str) -> bool:
        return False

    task_claimer = TaskClaimer(
        get_tasks=_noop_get_tasks,
        claim_task=_noop_claim_task,
    )
    listener = NATSListener(task_claimer)

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, _handle_signal, sig, listener)

    logger.info("daemon_started")
    try:
        await _shutdown_event.wait()
    finally:
        drained = await task_claimer.drain(settings.shutdown_timeout)
        if not drained:
            logger.warning("drain_incomplete_forcing_shutdown")
        try:
            await listener.stop()
        except Exception as exc:  # noqa: BLE001
            logger.error("nats_stop_error", error=str(exc))

    logger.info("daemon_stopped")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point for the Keystone daemon."""
    parser = argparse.ArgumentParser(description="ProjectKeystone transport daemon")
    parser.add_argument(
        "--log-level",
        default=None,
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="Logging verbosity level (default: from KEYSTONE_LOG_LEVEL env var or INFO)",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.0,
        help="Routing loop poll interval in seconds",
    )
    parser.add_argument(
        "--shutdown-timeout",
        type=float,
        default=None,
        help="Seconds to wait for in-flight operations during shutdown",
    )
    args = parser.parse_args(argv)

    settings = load_settings()
    if args.log_level is not None:
        settings.log_level = args.log_level
    if args.shutdown_timeout is not None:
        settings.shutdown_timeout = args.shutdown_timeout

    configure_logging(level=getattr(stdlib_logging, settings.log_level))
    signal.signal(signal.SIGTERM, handle_sigterm)

    logger.info("daemon_starting", log_level=settings.log_level)
    try:
        return asyncio.run(run(settings))
    except Exception as exc:  # noqa: BLE001
        logger.exception("daemon_error", error=str(exc))
        return 1


if __name__ == "__main__":
    sys.exit(main())
