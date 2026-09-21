"""Build requests exercise bounded real HTTP; no controller or work is started."""

import asyncio
import gzip
import json
import math
import os
import unittest
from unittest.mock import patch

from agamemnon_client import (
    AgamemnonAPIError,
    AgamemnonClient,
    AgamemnonConfig,
    AgamemnonConnectionError,
)


class FleetBuildTransportTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.mode = "valid"
        self.requests = []
        self.handlers = []
        self.writers = set()
        self.started = asyncio.Event()
        self.peer_closed = asyncio.Event()
        self.chunks_sent = 0
        self.server = await asyncio.start_server(self.accept, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.client = self.make_client()

    def make_client(self, timeout=3):
        return AgamemnonClient(
            AgamemnonConfig(
                host="127.0.0.1", port=self.port, api_key="controlled-main-key", timeout=timeout
            ),
            trust_env=False,
        )

    async def asyncTearDown(self):
        await self.client.aclose()
        self.server.close()
        await self.server.wait_closed()
        for writer in list(self.writers):
            writer.close()
        for task in self.handlers:
            if not task.done():
                task.cancel()
        results = await asyncio.gather(*self.handlers, return_exceptions=True)
        for result in results:
            if isinstance(result, BaseException) and not isinstance(result, asyncio.CancelledError):
                raise result

    def accept(self, reader, writer):
        self.writers.add(writer)
        self.handlers.append(asyncio.create_task(self.respond(reader, writer)))

    async def respond(self, reader, writer):
        try:
            header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 3)
            lines = header.decode("ascii").split("\r\n")
            headers = {
                name.lower(): value
                for name, value in (line.split(": ", 1) for line in lines[1:] if line)
            }
            body = await reader.readexactly(int(headers.get("content-length", "0")))
            self.requests.append((lines[0], headers, body))
            mode = self.mode
            payload = json.dumps({"ok": True}).encode()
            status = b"200 OK"
            extra = b""
            if mode == "oversized":
                payload = json.dumps({"data": "x" * (512 * 1024)}).encode()
            elif mode == "valid-large":
                payload = json.dumps({"data": "x" * (400 * 1024)}).encode()
            elif mode == "trickle":
                payload = b'{"data":"' + b"x" * 30 + b'"}'
            elif mode == "encoded":
                payload = gzip.compress(payload)
                extra = b"Content-Encoding: gzip\r\n"
            elif mode == "redirect":
                status = b"307 Temporary Redirect"
                extra = f"Location: http://127.0.0.1:{self.port}/unexpected\r\n".encode()
            elif mode == "refusal":
                status = b"409 Conflict"
                payload = b'{"error":"controlled stale generation"}'
            writer.write(
                b"HTTP/1.1 "
                + status
                + b"\r\nContent-Type: application/json\r\n"
                + f"Content-Length: {len(payload)}\r\n".encode()
                + extra
                + b"Connection: close\r\n\r\n"
            )
            await writer.drain()
            self.started.set()
            if mode == "hold":
                self.assertEqual(await reader.read(1), b"")
                self.peer_closed.set()
                return
            step = 1 if mode == "trickle" else 32 * 1024
            for index in range(0, len(payload), step):
                writer.write(payload[index : index + step])
                await writer.drain()
                self.chunks_sent += 1
                if mode == "trickle":
                    await asyncio.sleep(0.05)
        except (ConnectionError, asyncio.IncompleteReadError):
            self.peer_closed.set()
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass
            self.writers.discard(writer)

    def build_calls(self):
        return (
            ("fleet_build_submit", ({"idempotencyKey": "stable"},)),
            ("fleet_build_status", ("build-1",)),
            ("fleet_build_cancel", ("build-1", {"commandId": "stop-1"})),
            ("fleet_build_deliver", ("build-1", {"commandId": "start-1"})),
            ("fleet_build_claim_run", ("build-1", {"claimId": "claim-1"}, "controlled-worker-key")),
            ("fleet_build_fact", ("build-1", {"eventId": "event-1"}, "controlled-worker-key")),
            ("fleet_build_logs", ("build-1",)),
        )

    async def test_every_build_operation_rejects_oversized_response_without_retry(self):
        self.mode = "oversized"
        for name, arguments in self.build_calls():
            before = len(self.requests)
            with self.subTest(operation=name):
                with self.assertRaises(AgamemnonConnectionError):
                    await getattr(self.client, name)(*arguments)
                self.assertEqual(len(self.requests), before + 1)

    async def test_total_deadline_applies_while_response_keeps_making_progress(self):
        await self.client.aclose()
        self.client = self.make_client(timeout=0.5)
        self.mode = "trickle"
        with self.assertRaises(AgamemnonConnectionError) as caught:
            await self.client.fleet_build_claim_run("build-1", {"claimId": "claim-1"}, "worker-key")
        # A connect/read timeout is a different failure and cannot satisfy this oracle.
        self.assertIsInstance(caught.exception.__cause__, asyncio.TimeoutError)
        self.assertGreaterEqual(self.chunks_sent, 2)
        self.assertEqual(len(self.requests), 1)

    async def test_cancellation_closes_pending_response_and_preserves_cancellation(self):
        self.mode = "hold"
        task = asyncio.create_task(self.client.fleet_build_status("build-1"))
        try:
            await asyncio.wait_for(self.started.wait(), 3)
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            await asyncio.wait_for(self.peer_closed.wait(), 3)
            self.assertEqual(len(self.requests), 1)
        finally:
            if not task.done():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def test_encoded_response_is_rejected_before_automatic_decompression(self):
        self.mode = "encoded"
        with self.assertRaises(AgamemnonConnectionError):
            await self.client.fleet_build_status("build-1")
        self.assertEqual(len(self.requests), 1)

    async def test_redirect_is_not_successful_and_is_not_followed(self):
        self.mode = "redirect"
        with self.assertRaises(AgamemnonAPIError) as caught:
            await self.client.fleet_build_status("build-1")
        self.assertEqual(caught.exception.status_code, 307)
        self.assertEqual(len(self.requests), 1)

    async def test_large_valid_json_and_authentication_survive_streaming(self):
        self.mode = "valid-large"
        result = await self.client.fleet_build_claim_run(
            "build-1", {"claimId": "claim-1"}, "controlled-worker-key"
        )
        self.assertEqual(result, {"data": "x" * (400 * 1024)})
        self.mode = "valid"
        self.assertEqual(await self.client.fleet_build_status("build-1"), {"ok": True})
        self.assertEqual(len(self.requests), 2)
        for _, headers, _ in self.requests:
            self.assertEqual(headers["authorization"], "Bearer controlled-main-key")
        self.assertEqual(self.requests[0][1]["x-fleet-build-key"], "controlled-worker-key")
        self.assertNotIn("x-fleet-build-key", self.requests[1][1])

    async def test_refusal_preserves_status_and_message_without_retry(self):
        self.mode = "refusal"
        with self.assertRaises(AgamemnonAPIError) as caught:
            await self.client.fleet_build_cancel("build-1", {"commandId": "stop-1"})
        self.assertEqual(caught.exception.status_code, 409)
        self.assertEqual(caught.exception.message, "controlled stale generation")
        self.assertEqual(len(self.requests), 1)

    async def test_explicit_proxy_opt_out_keeps_loopback_credentials_on_direct_path(self):
        proxy = f"http://127.0.0.1:{self.port}"
        config = AgamemnonConfig(host="127.0.0.1", port=self.port, api_key="controlled-main-key")
        with patch.dict(os.environ, {"HTTP_PROXY": proxy, "ALL_PROXY": proxy, "NO_PROXY": ""}):
            # The same controlled peer can distinguish proxy absolute-form requests.
            async with AgamemnonClient(config) as legacy:
                self.assertEqual(await legacy.fleet_build_status("build-1"), {"ok": True})
            self.assertTrue(self.requests[-1][0].startswith("GET http://127.0.0.1:"))
            try:
                direct = AgamemnonClient(config, trust_env=False)
            except TypeError:
                self.fail("The public client must support an explicit environment-proxy opt-out")
            async with direct:
                self.assertEqual(await direct.fleet_build_status("build-1"), {"ok": True})
            self.assertEqual(self.requests[-1][0], "GET /v1/fleet/build-jobs/build-1 HTTP/1.1")
        self.assertEqual(len(self.requests), 2)

    async def test_nonfinite_build_deadline_is_refused_before_http(self):
        for timeout in (math.inf, math.nan):
            with self.subTest(timeout=timeout):
                async with self.make_client(timeout=timeout) as client:
                    with self.assertRaises(ValueError):
                        await client.fleet_build_status("build-1")
        self.assertEqual(self.requests, [])
