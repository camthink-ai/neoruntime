# Open-Source Split & Pre-Publish Checklist

This repository (`camthink-ai/ne503-aipc`) is an open-source snapshot of the
internal NE503 AIPC workspace. This checklist is the release gate: run it in
the **source workspace** (or against this snapshot) before publishing, to catch
secrets, proprietary vendor assets, generated binaries, model files, and
customer data.

## 1. Credentials & Secrets

Scan for hardcoded credentials across all tracked files (not just docs):

```bash
# High-signal credential patterns
git grep -n -iE '(api[_-]?key|secret|password|token)\s*[:=]\s*["'"'"'][A-Za-z0-9_\-]{16,}' -- ':!*.md'

# Tokens that look like JWT / real keys (long base64/hex runs)
git grep -nE '(eyJ[A-Za-z0-9_-]+\.){2,}' -- ':!*.md'

# Private keys and certificates (should be absent or clearly test-only)
git ls-files | grep -iE '\.(pem|key|p12|jks|pfx|cer|crt)$'
```

**Current state of this snapshot (verified at open-source prep time):**

- ✅ `web/src/mock/loginMock.ts` — contains `mock-admin-password` /
  `mock-user-password` placeholders only (dev mock; token is generated
  client-side with a `Basic` header for the mock API). **Keep**, but confirm
  the mock never mirrors a production credential.
- ✅ `configs/platform-api.yaml:61` — `password: "password"` is the *documented
  default* admin password, not a leaked secret. The default is intentional;
  confirm the runtime forces a change on first login (or document it in
  `docs/getting-started/QUICK_START.md`).
- ✅ No private keys / certs / keystores are tracked.
- ⚠️ Re-run the `git grep` scans above at publish time — new commits may add
  real secrets. Any hit is a **blocker**.

## 2. Proprietary Vendor Assets & Generated Binaries

```bash
# Committed binary/archive blobs
git ls-files | grep -iE '\.(bin|so|a|dll|dylib|exe|o|tar|gz|zip|7z|img|raw|npy)$'

# Large files that are usually build artifacts or datasets
git ls-files -z | xargs -0 -n1 ls -l | sort -k5 -rn | head -20
```

**Current state of this snapshot:**

- ⚠️ **`mcu_board_prj/firmware/ne503_ota_package_v0.1.6.bin` and
  `...v0.1.7.bin`** (≈100 KB each) are committed firmware OTA packages.
  **Action required before release:** confirm these are redistributable under
  the project license; if they are internal/proprietary firmware, either
  remove them from the public tree or ship them separately. This is a
  **decision point, not auto-resolved**.
- ✅ Build artifacts (`build/`, `platform/camera-daemon/build/`, `*.pb.go`,
  `*.so`) are already excluded via `.gitignore` and are **not** tracked.
- ✅ No model weights / HEF blobs / dataset files are tracked.

## 3. Internal References & Paths

```bash
# Internal shares, CI hosts, personal paths
git grep -n -E '(/home/[A-Za-z0-9_]+|/data/[A-Za-z0-9_]+|share/|\.internal|jenkins|gitlab\.(corp|local))' -- ':!go.sum' ':!*.md'

# Chinese internal jargon / TODO markers that shouldn't ship
git grep -n -E '(TODO|FIXME|XXX|内部|台架|待定)' -- '*.go' '*.py' '*.sh' '*.ts' '*.tsx' | head
```

**Current state of this snapshot:**

- ✅ The docs audit removed the last stale internal path references
  (e.g. `/home/share/gyro-attitude-sse.md` in
  `platform/platform-api/handlers/monitor.go` → `docs/references/gyro-attitude-sse.md`).
- ⚠️ Device paths like `/data/aipc` and `/run/aipc` are **runtime paths on
  Hailo-15**, not host leaks — they are part of the documented product layout
  and should stay.
- ⚠️ Any remaining internal-host references found by the scans above are
  blockers unless clearly part of the product contract.

## 4. License & Legal

- [ ] `LICENSE` present and chosen deliberately (currently a permissive
      one-line reference — confirm the intended SPDX identifier).
- [ ] `CONTRIBUTING.md` and `SECURITY.md` exist (✅ both present).
- [ ] Third-party dependencies' licenses compatible with the project license
      (check `go.mod`, `go.sum`, `web/package.json`).
- [ ] Hailo / vendor SDK headers and binaries — confirm redistribution terms
      (HAL sources in `hal_v2/`, camera-daemon vendor code).
- [ ] Corporate branding / internal commit author info scrubbed from commit
      history if required.

## 5. Docs Hygiene (completed in the docs audit)

The following were fixed during the pre-release docs audit — no further action
needed unless docs change again:

- ✅ **Correctness errors fixed:** SDK env name (`aarch64` → `armv8a`), phantom
  `build_multi_platform.sh` references, phantom `pack-factory` target, stale
  "seccomp pending" claim.
- ✅ **Deduplication:** CT-Disc protocol consolidated to a single canonical
  spec (`docs/references/CT_DISC_PROTOCOL.md`, v1.2.0); the duplicate root
  copy and the v1.1.0 draft were removed.
- ✅ **Coverage:** `docs/README.md` now indexes every doc; the gyro SSE
  endpoint is documented in `docs/api/swagger.yaml`,
  `docs/services/platform-api.md`, and `docs/references/gyro-attitude-sse.md`.
- ✅ **Language:** the only substantive Chinese doc
  (`docs/references/gyro-attitude-sse.md`) was translated to English (filename kept —
  referenced by code).

## 6. Final Gate (run last, in the publish directory)

```bash
# 1. No secrets
git grep -n -iE '(api[_-]?key|secret|password|token)\s*[:=]\s*["'"'"'][A-Za-z0-9_\-]{16,}' -- ':!*.md'   # expect 0 hits

# 2. No phantom build targets / stale scripts
grep -rn 'build_multi_platform.sh\|pack-factory\|/opt/aipc' docs/ scripts/ configs/                       # expect 0 hits

# 3. Docs index is complete
ls docs/**/*.md docs/*.md | wc -l && grep -c '](' docs/README.md                                          # counts should match

# 4. No duplicate specs
ls docs/references/CT_DISC_PROTOCOL.md && test ! -e docs/CT_DISC_PROTOCOL.md                              # expect pass

# 5. No Chinese-only technical docs remain
grep -rlP '[\x{4e00}-\x{9fff}]' docs/ --include='*.md'                                                    # expect only deliberate exceptions
```

**Publish block if:** any secret scan hits, the firmware binaries are not
confirmed redistributable, or the default admin password is not documented as
change-on-first-login.

## Ownership

Run this checklist at every release cut, not just the initial publish. When in
doubt, exclude the asset and publish it separately.
