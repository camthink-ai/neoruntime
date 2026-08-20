# Contributing to NeoRuntime

Thanks for helping improve the NeoRuntime platform.

## Repository Scope

This repository contains platform services, HAL v2 implementations, configuration
templates, deployment units, tools, tests, and the web console. SDKs and sample
applications are maintained in sibling repositories.

## Development Checks

Run the narrowest checks that cover your change:

```bash
make proto
make test-unit
make web
```

For C++ or HAL v2 changes, also run the relevant CMake target:

```bash
make hal-v2 HAL_PLATFORM=stub
make camera-daemon
```

## Branch Policy

- `develop` is the integration branch: start every contribution from the latest
  `develop` and open PRs against `develop`.
- `main` is the stable release branch: it only receives changes through release
  PRs from `develop`, or short-lived hotfix branches cut from `main`.
- Functional changes must be developed on a separate branch and submitted by PR.
  Do not commit feature, bug-fix, refactor, performance, build, or runtime
  behavior changes directly to `main` or `develop`.
- After a hotfix merges into `main`, merge `main` back into `develop` so the
  two branches do not drift.
- Use short, descriptive branch names:
  - `feat/<area>-<summary>` for new functionality.
  - `fix/<area>-<summary>` for bug fixes.
  - `refactor/<area>-<summary>` for internal restructuring.
  - `docs/<summary>` for documentation-only changes.
  - `chore/<summary>` for repository maintenance.
- Keep one branch focused on one topic. Split unrelated behavior, docs, and
  cleanup work into separate branches when they can be reviewed independently.

Example:

```bash
git fetch origin
git switch develop
git pull --ff-only origin develop
git switch -c feat/media-config-export
```

## Commit Messages

Use Conventional Commits. The repository enforces this with commitlint.

Format:

```text
<type>(<scope>): <subject>

<body>
```

Allowed types:

- `feat`: new user-facing or platform functionality.
- `fix`: bug fix.
- `docs`: documentation-only change.
- `style`: formatting-only change, no behavior change.
- `refactor`: code restructuring without behavior change.
- `perf`: performance improvement.
- `test`: tests.
- `chore`: maintenance, scripts, metadata, or auxiliary changes.
- `ci`: CI/CD workflow changes.
- `build`: build system or dependency changes.
- `revert`: revert a previous commit.

Rules:

- Keep the subject lowercase, imperative, and under 72 characters.
- Include a body for functional changes. Explain what changed, why it changed,
  and any compatibility, deployment, migration, or verification notes.
- Keep commits reviewable. A commit should represent one coherent change.
- Avoid vague subjects such as `update`, `fix bug`, or `misc changes`.

Examples:

```text
feat(media): add config export and import endpoints

Expose a versioned media config envelope so device settings can be cloned
between NeoRuntime units. The import path snapshots current files before applying
changes and restarts camera-daemon to replay persisted runtime config.

Verified with make test-unit and make camera-daemon HAL_PLATFORM=stub.
```

```text
fix(web): keep stream enabled while encoder is rebuilding

Use has_encoder instead of transient stream status so disabling one stream does
not briefly turn off the remaining stream toggles while the backend rebuilds
encoders.
```

## Pull Requests

- Keep changes focused and explain user-visible behavior.
- Do not commit secrets, customer data, model files, vendor SDKs, generated
  binaries, logs, build directories, or device-specific credentials.
- Prefer environment variables or gitignored local files for deployment
  secrets.
- Include tests or a clear manual verification note when automated coverage is
  not practical.
