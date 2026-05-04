# Contributing to ProjectAgamemnon

Thank you for your interest in contributing to ProjectAgamemnon! This is the planning,
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
- [Pixi](https://pixi.sh/) for environment management
- [Just](https://just.systems/) as the command runner
- C++20 compiler (GCC 12+ or Clang 15+)

### Environment Setup

```bash
# Clone the repository
git clone https://github.com/HomericIntelligence/ProjectAgamemnon.git
cd ProjectAgamemnon

# Activate the Pixi environment (installs CMake, Ninja, clang-tools, gcovr)
pixi shell

# Build the project
just build

# Run tests to verify setup
just test
```

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

- **REST API routes** — New endpoints under `/v1/` for task, agent, or workflow management
- **Store implementations** — Persistent or in-memory data store improvements
- **NATS client features** — Subject routing, consumer groups, message handling
- **Tests** — GoogleTest unit and integration tests
- **Dockerfile improvements** — Build optimization, security hardening
- **Documentation** — README updates, code comments for complex logic

## Development Workflow

### 1. Find or Create an Issue

Before starting work:

- Browse [existing issues](https://github.com/HomericIntelligence/ProjectAgamemnon/issues)
- Comment on an issue to claim it before starting work
- Create a new issue if one doesn't exist for your contribution

### 2. Branch Naming Convention

Create a feature branch from `main`:

```bash
git checkout main
git pull origin main
git checkout -b <issue-number>-<short-description>

# Examples:
git checkout -b 42-add-workflow-endpoint
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
git commit -m "feat(routes): add /v1/workflows endpoint

Implements REST API for workflow creation and status queries.
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

Thank you for contributing to ProjectAgamemnon!
