# Releasing

> **Operator note:** When releasing a version that drops the legacy
> `KEYSTONE_*` deprecation shim from `agamemnon_client.config`, call this out
> explicitly in the release notes and link
> [docs/migration-from-keystone.md](docs/migration-from-keystone.md). The
> server-side daemon (`agamemnon.orchestration.config`) has never accepted the
> legacy names — operators who skipped the rename will see silent default
> regressions unless they read the migration guide.

## Python Releases

Releases are triggered by pushing a `v*` tag (e.g. `v0.1.0`). The
`python-client-release.yml` workflow builds and publishes both the client and
orchestration packages to PyPI using **OIDC Trusted Publishing** — no API token
is stored in the repo.

The two packages (`HomericIntelligence-Agamemnon` and
`HomericIntelligence-Agamemnon-Orchestration`) are released together from the
same `v*` tag, using a single shared PyPI OIDC publisher entry.

### Python Client (`HomericIntelligence-Agamemnon`)

### Prerequisites (one-time setup)

Both of the following must be configured before the first tag push or the
publish step will fail with a 403.

#### 1. GitHub `pypi` environment

The `pypi` Actions environment must exist and be scoped to `v*` tags:

- Repo → **Settings → Environments → New environment**
- Name: `pypi` (lowercase, exact)
- Under *Deployment branches and tags* → *Add rule*: type **Tag**, pattern `v*`

> Already configured: environment ID `14476785067`, tag policy `v*` (tag ID
> `47723547`).

#### 2. PyPI pending publishers

Register OIDC publishers at <https://pypi.org/manage/account/publishing/>
**before** pushing the first `v*` tag (the packages do not need to exist yet):

**For `HomericIntelligence-Agamemnon` (client):**

| Field | Value |
| --- | --- |
| PyPI Project Name | `HomericIntelligence-Agamemnon` |
| Owner | `HomericIntelligence` |
| Repository name | `ProjectAgamemnon` |
| Workflow name | `python-client-release.yml` |
| Environment name | `pypi` |

**For `HomericIntelligence-Agamemnon-Orchestration` (orchestration):**

| Field | Value |
| --- | --- |
| PyPI Project Name | `HomericIntelligence-Agamemnon-Orchestration` |
| Owner | `HomericIntelligence` |
| Repository name | `ProjectAgamemnon` |
| Workflow name | `python-client-release.yml` |
| Environment name | `pypi` |

Each entry's five values must match the workflow file exactly or the OIDC
exchange will be rejected. Both packages use the same workflow and environment.

#### 2a. Registration log

Pending publishers have been registered on pypi.org against the `pypi`
environment in this repo. Re-register only if any of the five values in
the §2 tables changes (workflow rename, repo rename, env rename, etc.).

| PyPI project                                  | Workflow                    | Environment | Registered |
| --------------------------------------------- | --------------------------- | ----------- | ---------- |
| `HomericIntelligence-Agamemnon`               | `python-client-release.yml` | `pypi`      | 2026-06-04 |
| `HomericIntelligence-Agamemnon-Orchestration` | `python-client-release.yml` | `pypi`      | 2026-06-04 |

If the workflow filename is ever renamed, both rows above must be
re-registered on pypi.org **before** the next `v*` tag push or the OIDC
token exchange will 403.

#### 3. GPG signing key

`just release` runs `git commit -S` and `git tag -s`, both of which require a
usable GPG secret key. The readiness check verifies a key is present *before*
any files are mutated.

```bash
# Verify you have a secret key:
gpg --list-secret-keys

# If none, generate one (or import an existing key) and tell git to use it:
gpg --full-generate-key
git config --global user.signingkey <KEYID>
git config --global commit.gpgsign true
git config --global tag.gpgsign true
```

### Cutting a release

```bash
# Ensure you are on main with a clean working tree:
git checkout main && git pull

# Bump version, commit, tag, and push in one step:
just release 0.1.0
```

`just release` runs the readiness check automatically before mutating anything.
To preview readiness without releasing, run `./scripts/check-release-readiness.sh VERSION` directly.

The workflow runs automatically and publishes both packages. Verify with:

```bash
# Verify client package
pip index versions HomericIntelligence-Agamemnon
pip install HomericIntelligence-Agamemnon==0.1.0 --dry-run
python -c "import agamemnon_client; print(agamemnon_client.__version__)"

# Verify orchestration package
pip index versions HomericIntelligence-Agamemnon-Orchestration
pip install HomericIntelligence-Agamemnon-Orchestration==0.1.0 --dry-run
python -c "import agamemnon.orchestration; print(agamemnon.orchestration.__version__)"
```

## Operator runbook: data deletion

This section is the actionable companion to the Data & Privacy section in
`SECURITY.md`. Use it when an operator needs to purge agent/task records
(for example, after rotating Tailscale identifiers or decommissioning a host).
The full deletion path crosses two systems: GitHub Issues/Projects (the
backing store) and NATS JetStream (the transport).

1. **Identify the records to delete.** Cross-reference the agent ID(s) or
   task ID(s) using the API:
   - `GET /v1/agents/by-name/{name}` to resolve a name to an agent ID
   - `GET /v1/teams/{team_id}/tasks` to enumerate tasks for a team
2. **Delete via the REST API** so Agamemnon emits the matching `hi.agents.*`
   / `hi.tasks.*` deletion events:
   - `DELETE /v1/agents/{id}` for each agent
   - There is no direct task-delete REST endpoint; transition the task to
     `completed` or close the underlying GitHub Issue (next step).
3. **Close or delete the backing GitHub Issues/Projects items.** Closure is
   reversible (audit trail preserved); deletion is permanent. Choose deletion
   only if regulatory deletion is required.
4. **Purge the NATS JetStream history** so cached messages do not retain the
   identifiers. From a host with NATS CLI access:

   ```bash
   nats stream purge hi-tasks    --subject "hi.tasks.>"
   nats stream purge hi-pipeline --subject "hi.pipeline.>"
   # Repeat per myrmidon stream as needed:
   nats stream purge hi-myrmidon-codegen --subject "hi.myrmidon.codegen.>"
   ```

   Replace stream names with whatever your deployment uses (Agamemnon does
   not own the stream names; consult your `Myrmidons`/`ProjectKeystone` config).
5. **Rotate logs.** Operator stdout/stderr logs may have captured identifiers
   in transit. Apply your platform's log retention/rotation policy.
6. **Verify.** Re-issue `GET /v1/agents` / `GET /v1/teams/{team_id}/tasks` and
   confirm the records are gone. Run `nats stream info <stream>` and check
   that `messages = 0` for the purged subjects.

For the legal/regulatory framing of when each step is required, see the
*Data & Privacy* section in `SECURITY.md`.

## Refreshing the Dockerfile base-image digest

Both the builder (`ubuntu:24.04`) and runtime (`debian:12-slim`) stages of the
`Dockerfile` are pinned to a `@sha256:<digest>` for reproducible, Trivy-scannable
builds. When the upstream image is rebuilt with security patches, the digest must
be refreshed manually — there is no automated bump.

To refresh:

```bash
# Pull the floating tag locally
docker pull debian:12-slim
docker pull ubuntu:24.04

# Read the current digest the registry resolved to
docker inspect --format '{{index .RepoDigests 0}}' debian:12-slim
docker inspect --format '{{index .RepoDigests 0}}' ubuntu:24.04
```

Update the `FROM` lines in `Dockerfile` to use the new digest, rebuild locally
(`docker build -t projectagamemnon .`), and verify Trivy passes
(`trivy image projectagamemnon`). Open a PR titled
`chore(docker): refresh base image digests` and reference the upstream Debian/
Ubuntu security advisory that prompted the refresh.
