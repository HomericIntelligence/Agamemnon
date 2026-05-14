# Releasing

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
|---|---|
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
