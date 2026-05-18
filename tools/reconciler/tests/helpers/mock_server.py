#!/usr/bin/env python3
"""
Minimal mock HTTP server for Myrmidons test suite.

Reads response configuration from:
  1. Optional routes config file (--routes flag or second CLI arg):
       JSON array: [{"method": "GET", "path": "/api/v1/agents", "status": 200, "body": {...}}, ...]
       JSON object: {"routes": [...], "default_status": 200, "default_body": {}}
     Routes are matched in order; first match wins.
     Paths support a trailing wildcard segment: "/api/v1/agents/*" matches
     "/api/v1/agents/foo" and "/api/v1/agents/abc123".

     One-shot routes ("once": true):
       Add "once": true to a route to consume it after its first match.
       Lifecycle:
         - On the first request that matches the route's method+path, the
           server returns the route's status/body AND removes the route
           from the in-memory routes list.
         - Subsequent requests with the same method+path no longer see
           that route; they fall through to the next matching route (in
           definition order) or to default_status/default_body.
         - One-shot consumption is per-process and is NOT reset on its
           own — it only resets when the server is restarted (which
           reloads the routes config from disk).
       Use case: stateful request sequencing across multiple calls to
       the same endpoint — e.g. the first GET returns a populated list
       and subsequent GETs return an empty list, simulating a resource
       that is drained after being read.
       Example routes config:
         [
           {"method": "GET", "path": "/api/v1/agents",
            "status": 200, "body": [{"id": "a1"}], "once": true},
           {"method": "GET", "path": "/api/v1/agents",
            "status": 200, "body": []}
         ]
       The first GET /api/v1/agents returns [{"id": "a1"}]; every later
       GET /api/v1/agents returns [].
  2. Environment variables (fallback):
     MOCK_STATUS  — HTTP status code to return (default: 200)
     MOCK_BODY    — Response body JSON string (default: {})

Usage:
  MOCK_STATUS=200 MOCK_BODY='[...]' python3 mock_server.py <PORT>
  python3 mock_server.py <PORT> --routes <routes_config.json>
  python3 mock_server.py <PORT> <routes_config.json>   # legacy positional form
"""
import fnmatch
import os
import sys
import json
from http.server import HTTPServer, BaseHTTPRequestHandler

STATUS = int(os.environ.get("MOCK_STATUS", "200"))
BODY = os.environ.get("MOCK_BODY", "{}")
ROUTES_CONFIG = None


def _load_routes(config_path):
    """Load and normalise a routes config file.

    Accepts two formats:
    - Flat array: [{"method": ..., "path": ..., "status": ..., "body": ...}, ...]
    - Object: {"routes": [...], "default_status": ..., "default_body": ...}

    Returns a dict with "routes", optional "default_status", optional "default_body".
    """
    try:
        with open(config_path, "r") as f:
            raw = json.load(f)
    except FileNotFoundError:
        print(f"Error loading routes config: file not found: {config_path}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error loading routes config: invalid JSON: {e}", file=sys.stderr)
        sys.exit(1)

    if isinstance(raw, list):
        return {"routes": raw}
    if isinstance(raw, dict):
        return raw
    print("Error loading routes config: expected a JSON array or object", file=sys.stderr)
    sys.exit(1)


def _parse_args():
    """Return (port, routes_config_path_or_None)."""
    args = sys.argv[1:]
    port = 18080
    routes_path = None

    i = 0
    positional = []
    while i < len(args):
        if args[i] == "--routes":
            if i + 1 >= len(args):
                print("Error: --routes requires a file path argument", file=sys.stderr)
                sys.exit(1)
            routes_path = args[i + 1]
            i += 2
        else:
            positional.append(args[i])
            i += 1

    if positional:
        try:
            port = int(positional[0])
        except ValueError:
            print(f"Error: invalid port number: {positional[0]}", file=sys.stderr)
            sys.exit(1)
        # Legacy form: second positional arg is the routes config file
        if len(positional) > 1 and routes_path is None:
            routes_path = positional[1]

    return port, routes_path


def _path_matches(pattern, path):
    """Return True if *path* matches *pattern*.

    Supports a single trailing wildcard segment:
      /api/v1/agents/*  matches  /api/v1/agents/foo
    Exact matches are checked first.
    """
    if pattern == path:
        return True
    # Use fnmatch for simple glob-style wildcard support
    return fnmatch.fnmatch(path, pattern)


# ---------------------------------------------------------------------------
# Parse arguments and load optional routes config
# ---------------------------------------------------------------------------

_port, _routes_path = _parse_args()
if _routes_path is not None:
    ROUTES_CONFIG = _load_routes(_routes_path)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress request logging

    def do_GET(self):
        self._respond("GET")

    def do_POST(self):
        self._respond("POST")

    def do_PATCH(self):
        self._respond("PATCH")

    def do_DELETE(self):
        self._respond("DELETE")

    def _respond(self, method):
        status, body = self._get_response(method)
        # body may be a dict/list (from JSON routes config) — serialise it
        if not isinstance(body, str):
            body = json.dumps(body)
        encoded = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _get_response(self, method):
        """Get response status and body for the given method and path.

        If routes config is available, match by method+path (wildcard-aware).
        Routes with ``"once": true`` are consumed (removed) after the first
        match and subsequent requests fall through to the next route or the
        default_body, enabling stateful request sequencing (e.g. first GET
        returns data, subsequent GETs return empty list).
        Otherwise fall back to env vars.
        """
        if ROUTES_CONFIG is None:
            return STATUS, BODY

        routes = ROUTES_CONFIG.get("routes", [])
        # Try to find a matching route (first match wins)
        for i, route in enumerate(routes):
            route_method = route.get("method", "").upper()
            route_path = route.get("path", "")
            if route_method == method and _path_matches(route_path, self.path):
                status = route.get("status", 200)
                body = route.get("body", "{}")
                if route.get("once", False):
                    routes.pop(i)
                return status, body

        # Fall back to default_status/default_body from config, or env vars
        default_status = ROUTES_CONFIG.get("default_status", STATUS)
        default_body = ROUTES_CONFIG.get("default_body", BODY)
        return default_status, default_body


if __name__ == "__main__":
    httpd = HTTPServer(("127.0.0.1", _port), Handler)
    httpd.serve_forever()
