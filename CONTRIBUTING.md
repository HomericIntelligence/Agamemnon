# Contributing to Agamemnon

Thank you for your interest in contributing to Agamemnon! This is the planning,
coordination, and agentic orchestration service for the
[HomericIntelligence](https://github.com/HomericIntelligence) distributed agent mesh.

For an overview of the full ecosystem, see the
[Odysseus](https://github.com/HomericIntelligence/Odysseus) meta-repo.

## Quick Links

- [Development Setup](#development-setup)
- [What You Can Contribute](#what-you-can-contribute)
- [Development Workflow](#development-workflow)
- [Building and Testing](#building-and-testing)
- [Pull Request Process](#pull-request-process)
- [Code Review](#code-review)

## Development Setup

### Prerequisites

- [Git](https://git-scm.com/)
- [GitHub CLI](https://cli.github.com/) (`gh`)
- [uv](https://docs.astral.sh/uv/) for the build toolchain (CMake, Ninja, Conan, gcovr, pre-commit)
- [Just](https://just.systems/) as the command runner
- A C++20 system compiler (GCC 12+ or Clang 15+) and dev headers, from apt:
  `sudo apt-get install -y build-essential libssl-dev libcurl4-openssl-dev clang-tidy clang-format`

### Environment Setup

```bash
# Clone the repository
git clone https://github.com/HomericIntelligence/Agamemnon.git
cd Agamemnon

# Install the build toolchain (CMake, Ninja, Conan, gcovr) as locked wheels
uv sync

# Build the project (uses the system gcc/c++ via the conan profile)
just build

# Run tests to verify setup
just test
```

> **GTest ABI compatibility (important).** The Conan-managed GoogleTest is built
> from source (`--build=missing`) with the compiler declared in
> `conan/profiles/debug` (`tools.build:compiler_executables` → `/usr/bin/gcc`,
> `/usr/bin/g++`) — the same system compiler the project itself uses. Keep the
> conan profile's `compiler.version` in step with your installed GCC so the
> project and its dependencies share one `libstdc++` ABI. Building with a
> compiler older than GCC 12 / Clang 15 is unsupported.

### nats.c version updates

When bumping the `nats.c` `FetchContent` pin in `CMakeLists.txt`, you must also
update the matching version in `.github/cpp-fetchcontent-deps.cdx.json` in the
**same PR**. The Grype CVE scan in CI consumes that SBOM fragment, so the two
files must agree or scan results will reflect the wrong version. There is no
automated enforcement — reviewers should reject any PR that touches one without
the other.

### Install Pre-commit Hooks

```bash
# Install hooks (clang-format, conventional commits, trailing whitespace)
pre-commit install
```

### Verify Your Setup

```bash
# List all available recipes
just --list

# Check formatting compliance
just format-check

# Run the full CI pipeline locally
just ci
```

## What You Can Contribute

See [ROADMAP.md](ROADMAP.md) for the full list of planned features and their acceptance
criteria. Good first targets are the deferred features with clear acceptance criteria.

- **REST API routes** — New endpoints under `/v1/` for task or agent management
- **Store implementations** — Persistent or in-memory data store improvements
- **NATS client features** — Subject routing, consumer groups, message handling
- **Tests** — GoogleTest unit and integration tests
- **Dockerfile improvements** — Build optimization, security hardening
- **Documentation** — README updates, code comments for complex logic

## Development Workflow

### 1. Find or Create an Issue

Before starting work:

- Browse [existing issues](https://github.com/HomericIntelligence/Agamemnon/issues)
- Comment on an issue to claim it before starting work
- Create a new issue if one doesn't exist for your contribution

Use the provided issue templates (bug report, feature request) when opening new issues — they pre-fill the required fields.

### 2. Branch Naming Convention

Create a feature branch from `main`:

```bash
git checkout main
git pull origin main
git checkout -b <issue-number>-<short-description>

# Examples:
git checkout -b 42-add-docker-agent-endpoint
git checkout -b 15-fix-store-race-condition
```

**Branch naming rules:**

- Start with the issue number
- Use lowercase letters and hyphens
- Keep descriptions short but descriptive

### 3. Commit Message Format

We follow [Conventional Commits](https://www.conventionalcommits.org/):

```text
<type>(<scope>): <subject>

<body>

<footer>
```

**Types:**

| Type       | Description                |
|------------|----------------------------|
| `feat`     | New feature                |
| `fix`      | Bug fix                    |
| `docs`     | Documentation only         |
| `style`    | Formatting, no code change |
| `refactor` | Code restructuring         |
| `test`     | Adding/updating tests      |
| `chore`    | Maintenance tasks          |

**Example:**

```bash
git commit -m "feat(routes): add /v1/agents/docker endpoint

Adds Docker agent registration, publishes creation event to NATS.
Uses the existing store for persistence.

Closes #42"
```

## Building and Testing

### Build

```bash
# Debug build (default)
just build

# The build uses CMake with Ninja generator and CMakePresets.json
```

> **GTest ABI compatibility:** Conan builds GTest from source
> (`--build=missing`) using the compiler declared in `conan/profiles/debug`
> (`tools.build:compiler_executables` → `/usr/bin/gcc`, `/usr/bin/g++`) — the
> same system compiler `just build` uses for the project. Linking against a
> different `libstdc++` produces a silent link or runtime ABI failure, so keep
> the conan profile's `compiler.version` aligned with your installed system GCC
> (install a supported one via `apt-get install -y build-essential`).

### Test

```bash
# Run all tests via CTest + GoogleTest
just test

# Generate coverage report (gcovr)
just coverage
```

### Lint and Format

```bash
# Run clang-tidy
just lint

# Check formatting (clang-format v17)
just format-check

# Auto-format all source files
just format
```

### C++ Conventions

- **Standard**: C++20
- **Formatting**: clang-format v17 (enforced by pre-commit hook)
- **Build generator**: Ninja via CMakePresets.json
- **Dependencies**: Managed via CMake FetchContent (cpp-httplib, nlohmann_json, GoogleTest)
- **Sanitizers**: Use ASAN/TSAN for debugging memory and threading issues

## Pull Request Process

### Before You Start

1. Ensure an issue exists for your work
2. Create a branch from `main` using the naming convention
3. Implement your changes
4. Run `just ci` locally to verify build, test, lint, and format checks pass

### Creating Your Pull Request

```bash
git push -u origin <branch-name>
gh pr create --title "[Type] Brief description" --body "Closes #<issue-number>"
```

**PR Requirements:**

- PR must be linked to a GitHub issue
- PR title should be clear and descriptive
- All CI checks must pass (build, test, lint, format)

### Never Push Directly to Main

The `main` branch is protected. All changes must go through pull requests.

## Code Review

### What Reviewers Look For

- **Correctness** — Does the code do what it claims?
- **Test coverage** — Are new features and edge cases tested?
- **Memory safety** — No buffer overflows, dangling pointers, or data races
- **API consistency** — Do new endpoints follow existing REST patterns?
- **Formatting** — Does `just format-check` pass?
- **Security** — Is all HTTP input validated? Are there injection risks?

### Responding to Review Comments

- Keep responses short (1 line preferred)
- Start with "Fixed -" to indicate resolution

## Markdown Standards

All documentation files must follow these standards:

- Code blocks must have a language tag (`cpp`, `bash`, `yaml`, `text`, etc.)
- Code blocks must be surrounded by blank lines
- Lists must be surrounded by blank lines
- Headings must be surrounded by blank lines

## API Versioning

See [docs/api-versioning.md](docs/api-versioning.md) for the full API versioning and
backwards-compatibility policy.

Key rules:

- `/v1/` endpoints are stable — no breaking changes within the same major version
- Breaking changes require a new prefix (`/v2/`)
- All responses carry `X-API-Version: <semver>`
- Deprecated endpoints require a 2-release notice in `CHANGELOG.md` before removal

## Reporting Issues

### Bug Reports

Include: clear title, steps to reproduce, expected vs actual behavior, compiler/OS details.

### Security Issues

**Do not open public issues for security vulnerabilities.**
See [SECURITY.md](SECURITY.md) for the responsible disclosure process.

### Data & Privacy

For questions about what data this service stores or requests to delete records,
see the [Data & Privacy](SECURITY.md#data--privacy) section of SECURITY.md.

## Code of Conduct

Please review our [Code of Conduct](CODE_OF_CONDUCT.md) before contributing.

---

Thank you for contributing to Agamemnon!
