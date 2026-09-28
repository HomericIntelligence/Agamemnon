"""Attach one admitted Keystone binding to an existing private Fleet worker."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from . import (
    AttachmentBridge,
    BridgeError,
    DeliveryJournal,
    DurableClient,
    GatewayProcess,
    PrivateSpool,
    UnixWorker,
)
from .observations import ObservationSink


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker-id", required=True)
    parser.add_argument("--generation", type=int, required=True)
    for name in ("spool-dir", "journal-dir", "worker-state-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--agamemnon-url", required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--heartbeat-interval", type=float, default=5)
    parser.add_argument(
        "--max-deliveries",
        type=int,
        default=1,
        help="Bound this attachment run; default one existing delivery",
    )
    parser.add_argument(
        "--events-only",
        action="store_true",
        help="Designated per-worker collector: forward metadata without pulling work",
    )
    parser.add_argument(
        "--observations-fd",
        type=int,
        help="Dedicated inherited private pipe write FD; observation output defaults off",
    )
    parser.add_argument("gateway", nargs=argparse.REMAINDER, help="Trusted gateway argv after --")
    args = parser.parse_args()
    gateway = None
    journal = None
    observations = None
    try:
        if not 1 <= args.max_deliveries <= 10000:
            raise BridgeError("invalid_delivery_limit")
        if args.observations_fd is not None:
            observations = ObservationSink(args.observations_fd)
            # The CLI takes ownership; the library sink itself only owns its duplicate.
            os.close(args.observations_fd)
        argv = args.gateway[1:] if args.gateway[:1] == ["--"] else args.gateway
        authority = DurableClient(args.agamemnon_url, token=os.environ.get("AGAMEMNON_API_KEY"))
        spool = PrivateSpool(args.spool_dir)
        journal = DeliveryJournal(args.journal_dir)
        gateway = GatewayProcess(argv, observations=observations)
        bridge = AttachmentBridge(
            gateway=gateway,
            worker=UnixWorker(args.worker_state_dir),
            authority=authority,
            spool=spool,
            journal=journal,
            worker_id=args.worker_id,
            generation=args.generation,
            timeout=args.timeout,
            heartbeat_interval=args.heartbeat_interval,
        )
        if args.events_only:
            print(json.dumps({"forwardedEvents": bridge.forward_events()}))
        else:
            for _ in range(args.max_deliveries):
                result = bridge.run_once()
                print(json.dumps(result), flush=True)
                if result["status"] == "empty":
                    break
        return 0
    except BridgeError as error:
        print(json.dumps({"error": error.code}), flush=True)
        return 1
    except Exception:
        print(json.dumps({"error": "attachment_failed"}), flush=True)
        return 1
    finally:
        if gateway is not None:
            gateway.close()
        if observations is not None:
            observations.close()
            print(json.dumps({"observations": observations.status()}), flush=True)
        elif gateway is not None:
            print(json.dumps({"observations": gateway.observation_status()}), flush=True)
        if journal is not None:
            journal.close()


if __name__ == "__main__":
    raise SystemExit(main())
