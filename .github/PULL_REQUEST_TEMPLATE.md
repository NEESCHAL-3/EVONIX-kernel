## Summary

Describe the problem, the design, and why this belongs in the selected branch.

## Impact

- Device/ROM:
- User-visible interfaces:
- Security or compatibility trade-offs:

## Validation

- [ ] `git diff --check`
- [ ] `scripts/checkpatch.pl --strict --codespell --git <base>..HEAD`
- [ ] Relevant Kleaf target builds
- [ ] Device boot tested, or clearly marked not tested
- [ ] Runtime behavior tested for affected subsystems

List exact commands, commit, image checksum, and test results:

## Review notes

Call out changes to SELinux, module ABI validation, procfs/sysfs permissions,
thermal limits, charging, storage, networking, or boot flow.
