## Summary

<!-- What and why, in 2-4 sentences. -->

## Change type

- [ ] feat
- [ ] fix
- [ ] refactor
- [ ] docs
- [ ] test
- [ ] chore / ci / build

## Checklist

- [ ] `make check` passes locally (or I noted why below)
- [ ] Web / Python checks pass if those areas changed
- [ ] Tests added/updated for new behavior
- [ ] If API routes changed (`platform/platform-api/server/main.go`), `docs/api/swagger.yaml` updated in the same PR (`Swagger sync` CI job must stay green)
- [ ] No secrets, internal IPs, or proprietary vendor code introduced
- [ ] Commit messages follow the conventional-commits policy

## Verification

<!-- How you tested this end-to-end. Commands, devices, outputs. -->

## Risk / rollback

<!-- What could break and how to roll back. -->
