# API Versioning Policy

## Stable API: /v1

All endpoints under `/v1/` are stable. Consumers can rely on these endpoints not having
breaking changes within the same major version.

## Version Header

Every response includes:

```text
X-API-Version: <semver>
```

The value always matches the server's release version (i.e., the `version` field returned by
`GET /v1/version`).

## Version Endpoint

`GET /v1/version` returns:

```json
{"version": "<semver>", "name": "Agamemnon"}
```

The `version` field is always identical to `X-API-Version`.

## Breaking vs. Non-Breaking Changes

**Non-breaking (allowed in `/v1`):**

- Adding optional response fields
- Adding new endpoints
- Adding optional query parameters
- Relaxing validation constraints

**Breaking (requires a new prefix, e.g. `/v2`):**

- Removing or renaming response fields
- Changing HTTP status codes for existing endpoints
- Removing endpoints
- Tightening validation that rejects previously-valid requests
- Changing authentication requirements

## Deprecation Policy

1. A deprecation notice is added to `CHANGELOG.md` when an endpoint or field is marked for removal.
2. Deprecated endpoints remain functional for a minimum of **2 releases** after the notice.
3. Responses from deprecated endpoints include the following RFC 8594 headers:
   - `Deprecation: true` — indicates the endpoint is deprecated
   - `Sunset: <HTTP-date>` — indicates when the endpoint will be removed
4. Responses from deprecated endpoints include `"deprecated": true` in the JSON body.
5. The removal is announced again in `CHANGELOG.md` at the release it takes effect.

### Setting Deprecation Headers in Code

Use the `set_deprecation_headers()` helper function to apply deprecation headers to a response:

```cpp
set_deprecation_headers(res, "Fri, 01 Jan 2027 00:00:00 GMT");
```

This will set both the `Deprecation` and `Sunset` headers on the response.

## Future Major Versions

When `/v2` is introduced:

- Both `/v1` and `/v2` are served concurrently.
- The `/v1` sunset date is announced in `CHANGELOG.md` with at least 2 release cycles of notice.
- After sunset, `/v1` endpoints return `410 Gone`.

## Version Consistency

The version string in `include/agamemnon/version.hpp` (`kVersion`) and the `VERSION`
field in `CMakeLists.txt` must be kept in sync manually. A CI check in
`scripts/check-version-consistency.sh` enforces this.
