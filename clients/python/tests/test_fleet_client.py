"""Fleet client wire contracts, using an HTTP transport boundary."""

import json
import unittest

import httpx

from agamemnon_client import AgamemnonClient, AgamemnonConfig


class FleetClientTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.requests = []

        def respond(request):
            self.requests.append(request)
            return httpx.Response(200, json={"items": [], "total": 0})

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
