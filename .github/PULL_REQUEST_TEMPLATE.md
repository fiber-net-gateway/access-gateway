## Summary

<!-- Describe what this PR changes and why. -->

## Scope

- [ ] Data Plane (`native/access-server/`)
- [ ] Control Plane API / Workers (`server/`)
- [ ] Management Console UI (`web/`)
- [ ] Deployment / CI (`deploy/`, `compose*.yaml`, `.github/`)
- [ ] Documentation (`docs/`, `README*`)

## Impact Checklist

- [ ] Wire contract or rnacos Data IDs changed (documented & tested across native + console)
- [ ] Database migration added (`server/migrations/`)
- [ ] Secret handling / redaction reviewed (no credentials, tokens, or private keys logged or exposed)
- [ ] Lifecycle states remain distinct (`draft` / `published` / `active` / `unknown`)
- [ ] Hot-path allocation / EventLoop ownership / shutdown ordering reviewed (if native)
- [ ] Before/after screenshots attached (if UI)

## Validation Run

<!-- List the exact commands executed and their results. -->

```bash
# Example:
# npm run typecheck && npm test && npm run format:check && npm run build
# npm run test:native
```
