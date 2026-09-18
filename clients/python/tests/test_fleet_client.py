"""Fleet client wire contracts, using an HTTP transport boundary."""

import json
import unittest
from hashlib import sha256

import httpx

from agamemnon_client import (
    AgamemnonAPIError,
    AgamemnonClient,
    AgamemnonConfig,
    AgamemnonConnectionError,
)

SNAPSHOT_DIGEST = "c7a335c3ba3d5e724413879073b1b8d62125e23d1559a31790467998a3898565"


class FleetClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.requests = []
        self.response_body = {"items": [], "total": 0}
        self.response_status = 200
        self.transport_error = None

        def respond(request):
            self.requests.append(request)
            if self.transport_error is not None:
                raise self.transport_error("controlled transport failure", request=request)
            return httpx.Response(self.response_status, json=self.response_body)

        self.client = AgamemnonClient(AgamemnonConfig(api_key="test-key"))
        await self.client.aclose()
        self.client._client = httpx.AsyncClient(
            transport=httpx.MockTransport(respond), base_url="http://localhost:8080"
        )

    async def asyncTearDown(self):
        await self.client.aclose()

    async def test_collection_uses_existing_authentication(self):
        self.assertTrue(callable(getattr(self.client, "fleet_list", None)))
        result = await self.client.fleet_list("sessions")
        self.assertEqual(result, {"items": [], "total": 0})
        self.assertEqual(self.requests[0].url.path, "/v1/fleet/sessions")
        self.assertEqual(self.requests[0].headers["authorization"], "Bearer test-key")

    async def test_project_view_health_and_rebuild_are_separate_from_task_commands(self):
        self.assertTrue(callable(getattr(self.client, "fleet_projects", None)))
        self.assertTrue(callable(getattr(self.client, "fleet_reconcile_projects", None)))
        await self.client.fleet_projects()
        await self.client.fleet_reconcile_projects()
        self.assertEqual(self.requests[0].method, "GET")
        self.assertEqual(self.requests[0].url.path, "/v1/fleet/projects")
        self.assertEqual(self.requests[1].method, "POST")
        self.assertEqual(self.requests[1].url.path, "/v1/fleet/projects/reconcile")
        self.assertEqual(self.requests[1].headers["authorization"], "Bearer test-key")

    async def test_command_preserves_private_reference_and_idempotency(self):
        self.assertTrue(callable(getattr(self.client, "fleet_command", None)))
        body = {
            "commandId": "command-1",
            "idempotencyKey": "command-1",
            "generation": 1,
            "payload": {"inputRef": "a" * 32 + ".json"},
        }
        await self.client.fleet_command("sessions", "session-1", "input", body)
        self.assertEqual(self.requests[0].url.path, "/v1/fleet/sessions/session-1/input")
        self.assertEqual(json.loads(self.requests[0].content), body)

    async def test_invalid_path_components_are_rejected_before_http(self):
        self.assertTrue(callable(getattr(self.client, "fleet_get", None)))
        for kind, identifier in [("../agents", "id"), ("sessions", "../agents")]:
            with self.assertRaises(ValueError):
                await self.client.fleet_get(kind, identifier)
        self.assertEqual(self.requests, [])

    async def test_activity_and_control_cursors_have_distinct_endpoints(self):
        self.assertTrue(callable(getattr(self.client, "fleet_observe", None)))
        fact = {"schema": "hi/fleet/v1", "kind": "activity", "eventId": "event-1"}
        await self.client.fleet_observe(fact)
        await self.client.fleet_events(after=42)
        self.assertEqual(self.requests[0].method, "POST")
        self.assertEqual(self.requests[1].url.query, b"after=42")

    async def test_manual_resolution_uses_separate_operator_credential(self):
        self.assertTrue(callable(getattr(self.client, "fleet_resolve", None)))
        decision = {"decisionId": "decision-1", "generation": 1, "outcome": "completed"}
        await self.client.fleet_resolve("sessions", "session-1", decision, "operator-key")
        self.assertEqual(self.requests[0].url.path, "/v1/fleet/sessions/session-1/resolve")
        self.assertEqual(self.requests[0].headers["x-fleet-resolution-key"], "operator-key")
        self.assertEqual(self.requests[0].headers["authorization"], "Bearer test-key")
        self.assertEqual(json.loads(self.requests[0].content), decision)

    async def test_build_submission_preserves_snapshot_parent_and_idempotency(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_submit", None)))
        body = {
            "schema": "hi/fleet/build-submit/v1",
            "idempotencyKey": "build-request-1",
            "workspaceId": "hephaestus-source",
            "recipeId": "hephaestus-test-unit-v1",
            "parameters": {},
            "parent": {
                "executionId": "parent-execution",
                "generation": 1,
                "sessionId": "parent-session",
                "targetId": "parent-session",
                "targetKind": "sessions",
            },
            "snapshot": {
                "baseCommit": "bb6566042635539f36f62c2c521ce0d6d28f9c42",
                "bytes": 91,
                "manifestDigest": SNAPSHOT_DIGEST,
                "members": 2,
                "policyDigest": "112214116cc11538ae089e29797a4b769a9b2a69c4b130193c30a5aa3f458434",
                "reference": "snapshot-interop-1",
            },
        }
        original = json.loads(json.dumps(body))
        self.response_status = 201
        self.response_body = {"record": {"id": "build-1"}, "replayed": False}
        result = await self.client.fleet_build_submit(body)
        self.assertEqual(result, self.response_body)
        self.assertEqual(body, original)
        self.assertEqual(len(self.requests), 1)
        self.assertEqual(self.requests[0].method, "POST")
        self.assertEqual(self.requests[0].url.path, "/v1/fleet/build-jobs/submit")
        self.assertEqual(json.loads(self.requests[0].content), original)
        self.assertEqual(self.requests[0].headers["authorization"], "Bearer test-key")
        self.assertNotIn("x-fleet-build-key", self.requests[0].headers)

    async def test_build_status_cancel_and_delivery_use_fixed_existing_routes(self):
        body = {"schema": "hi/fleet/build-cancel/v1", "commandId": "cancel-1", "attempt": 1}
        for name, method, suffix, arguments in [
            ("fleet_build_status", "GET", "", ("build-1",)),
            ("fleet_build_cancel", "POST", "/cancel", ("build-1", body)),
            ("fleet_build_deliver", "POST", "/deliver", ("build-1", {"attempt": 1})),
        ]:
            with self.subTest(operation=name):
                operation = getattr(self.client, name, None)
                self.assertTrue(callable(operation))
                result = await operation(*arguments)
                request = self.requests[-1]
                self.assertEqual(result, self.response_body)
                self.assertEqual(request.method, method)
                self.assertEqual(request.url.path, f"/v1/fleet/build-jobs/build-1{suffix}")
                self.assertEqual(request.headers["authorization"], "Bearer test-key")
                self.assertNotIn("x-fleet-build-key", request.headers)
                if method == "POST":
                    self.assertEqual(json.loads(request.content), arguments[-1])
                else:
                    self.assertEqual(request.content, b"")
        self.assertEqual(len(self.requests), 3)

    async def test_build_supervisor_auth_is_additional_and_request_scoped(self):
        for name, suffix, body in [
            ("fleet_build_claim_run", "claim-run", {"claimId": "claim-1", "attempt": 1}),
            ("fleet_build_fact", "facts", {"eventId": "event-1", "outcome": "cancelled"}),
        ]:
            with self.subTest(operation=name):
                operation = getattr(self.client, name, None)
                self.assertTrue(callable(operation))
                result = await operation("build-1", body, "supervisor-key")
                request = self.requests[-1]
                self.assertEqual(result, self.response_body)
                self.assertEqual(request.method, "POST")
                self.assertEqual(request.url.path, f"/v1/fleet/build-jobs/build-1/{suffix}")
                self.assertEqual(request.headers["authorization"], "Bearer test-key")
                self.assertEqual(request.headers["x-fleet-build-key"], "supervisor-key")
                self.assertEqual(json.loads(request.content), body)
        await self.client.fleet_list("build-jobs")
        self.assertNotIn("x-fleet-build-key", self.requests[-1].headers)
        self.assertEqual(len(self.requests), 3)

    async def test_build_log_default_query_and_utf8_reply_are_preserved(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_logs", None)))
        data = "compiled λ\n"
        self.response_body = {
            "schema": "hi/fleet/build-logs/v1",
            "buildId": "build-1",
            "attempt": 1,
            "snapshotDigest": SNAPSHOT_DIGEST,
            "stream": "stdout",
            "after": 0,
            "next": len(data.encode()),
            "data": data,
            "chunkDigest": sha256(data.encode()).hexdigest(),
            "complete": False,
            "truncated": False,
            "manifest": None,
        }
        result = await self.client.fleet_build_logs("build-1")
        self.assertEqual(result, self.response_body)
        request = self.requests[0]
        self.assertEqual(request.method, "GET")
        self.assertEqual(request.url.path, "/v1/fleet/build-jobs/build-1/logs")
        self.assertEqual(
            dict(request.url.params), {"stream": "stdout", "after": "0", "limit": "65536"}
        )
        self.assertEqual(request.headers["authorization"], "Bearer test-key")
        self.assertNotIn("x-fleet-build-key", request.headers)
        self.assertEqual(len(self.requests), 1)

    async def test_build_log_bounds_preserve_largest_cursor_and_smallest_page(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_logs", None)))
        await self.client.fleet_build_logs("build-1", stream="stderr", after=2**63 - 1, limit=1)
        self.assertEqual(
            dict(self.requests[0].url.params),
            {"stream": "stderr", "after": str(2**63 - 1), "limit": "1"},
        )

    async def test_build_log_invalid_bounds_fail_before_http(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_logs", None)))
        cases = (
            [{"after": value} for value in (-1, 2**63, True, False, 0.0, "0", None)]
            + [{"limit": value} for value in (0, 65537, True, False, 1.0, "1", None)]
            + [{"stream": value} for value in ("both", "../stdout", "STDOUT", "", None)]
        )
        for parameters in cases:
            with self.subTest(parameters=parameters), self.assertRaises(ValueError):
                await self.client.fleet_build_logs("build-1", **parameters)
        self.assertEqual(self.requests, [])

    async def test_build_identifiers_cannot_change_the_http_path(self):
        for name, arguments in [
            ("fleet_build_status", ()),
            ("fleet_build_cancel", ({},)),
            ("fleet_build_deliver", ({},)),
            ("fleet_build_claim_run", ({}, "supervisor-key")),
            ("fleet_build_fact", ({}, "supervisor-key")),
            ("fleet_build_logs", ()),
        ]:
            operation = getattr(self.client, name, None)
            self.assertTrue(callable(operation), name)
            for identifier in ("", "../jobs", "jobs?x=1", "jobs%2fother", "x" * 129, None, 1):
                with (
                    self.subTest(operation=name, identifier=identifier),
                    self.assertRaises(ValueError),
                ):
                    await operation(identifier, *arguments)
        self.assertEqual(self.requests, [])

    async def test_build_server_failures_are_preserved_without_retry(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_submit", None)))
        for code in (409, 503):
            with self.subTest(code=code):
                self.response_status = code
                self.response_body = {"error": "controlled controller refusal"}
                count = len(self.requests)
                with self.assertRaises(AgamemnonAPIError) as caught:
                    await self.client.fleet_build_submit({"idempotencyKey": "stable-request"})
                self.assertEqual(caught.exception.status_code, code)
                self.assertEqual(caught.exception.message, "controlled controller refusal")
                self.assertEqual(len(self.requests), count + 1)

    async def test_build_transport_failures_keep_existing_error_type_without_retry(self):
        self.assertTrue(callable(getattr(self.client, "fleet_build_status", None)))
        for error in (httpx.ConnectError, httpx.ReadTimeout):
            with self.subTest(error=error):
                self.transport_error = error
                count = len(self.requests)
                with self.assertRaises(AgamemnonConnectionError):
                    await self.client.fleet_build_status("build-1")
                self.assertEqual(len(self.requests), count + 1)
