"""Keystone daemon — routes messages between publishers and subscribers."""

from __future__ import annotations

import argparse
import logging as stdlib_logging
import signal
import sys
import time
from typing import Any

from keystone.logging import KeystoneLogger, configure_logging, get_logger

logger: KeystoneLogger = get_logger(component="daemon")


def handle_sigterm(signum: int, frame: Any) -> None:
    """Handle SIGTERM by raising SystemExit so cleanup runs."""
    raise SystemExit(0)


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


def main(argv: list[str] | None = None) -> int:
    """Entry point for the Keystone daemon."""
    parser = argparse.ArgumentParser(description="ProjectKeystone transport daemon")
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="Logging verbosity level",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.0,
        help="Routing loop poll interval in seconds",
    )
    args = parser.parse_args(argv)

    configure_logging(level=getattr(stdlib_logging, args.log_level))
    signal.signal(signal.SIGTERM, handle_sigterm)

    logger.info("daemon_starting", log_level=args.log_level)
    try:
        run_routing_loop(poll_interval=args.poll_interval)
    except Exception as exc:  # noqa: BLE001
        logger.exception("daemon_error", error=str(exc))
        return 1
    finally:
        logger.info("daemon_stopped")

    return 0


if __name__ == "__main__":
    sys.exit(main())
