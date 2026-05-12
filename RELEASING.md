# Releasing

> **Operator note:** When releasing a version that drops the legacy
> `KEYSTONE_*` deprecation shim from `agamemnon_client.config`, call this out
> explicitly in the release notes and link
> [docs/migration-from-keystone.md](docs/migration-from-keystone.md). The
> server-side daemon (`agamemnon.orchestration.config`) has never accepted the
> legacy names — operators who skipped the rename will see silent default
> regressions unless they read the migration guide.

## Python Client (`HomericIntelligence-Agamemnon`)

Releases are triggered by pushing a `v*` tag (e.g. `v0.1.0`). The
`python-client-release.yml` workflow builds the wheel/sdist and publishes to
PyPI using **OIDC Trusted Publishing** — no API token is stored in the repo.

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

#### 2. PyPI pending publisher

Register the OIDC publisher at <https://pypi.org/manage/account/publishing/>
**before** pushing the first `v*` tag (the package does not need to exist yet):

| Field | Value |
| --- | --- |
| PyPI Project Name | `HomericIntelligence-Agamemnon` |
| Owner | `HomericIntelligence` |
| Repository name | `ProjectAgamemnon` |
| Workflow name | `python-client-release.yml` |
| Environment name | `pypi` |

These five values must match the workflow file exactly or the OIDC exchange
will be rejected.

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

The workflow runs automatically and publishes the package. Verify with:

```bash
pip index versions HomericIntelligence-Agamemnon
pip install HomericIntelligence-Agamemnon==0.1.0 --dry-run
python -c "import agamemnon_client; print(agamemnon_client.__version__)"
```

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
