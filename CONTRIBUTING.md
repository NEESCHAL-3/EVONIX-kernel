# Contributing to EVONIX

EVONIX follows the Linux kernel's patch discipline while using GitHub for review
and Kleaf for builds. Preserve published history: do not force-push `main`, a
published feature branch, or a milestone tag.

## Choose the correct base

- Use `main` for fixes and work intended for the maintained kernel.
- Use a historical feature branch only when maintaining that isolated line.
- Do not merge unrelated experiments into `main`; open a focused branch and
  explain the intended device and ROM behavior.

The branch relationships are recorded in
[Documentation/evonix/BRANCHES.md](Documentation/evonix/BRANCHES.md).

## Patch requirements

1. Keep each commit buildable and focused on one logical change.
2. Use an imperative kernel-style subject, normally `subsystem: summary`.
3. Explain why the change is needed, its runtime impact, and any compatibility
   or security trade-off in the commit body.
4. Certify the Developer Certificate of Origin with `git commit -s`.
5. Do not add generated output, binaries, logs, credentials, signing material,
   or device data.
6. Add or update documentation when changing a user-visible interface, default,
   branch contract, or build command.

GitHub-only commits do not need a Gerrit `Change-Id`. Patches also intended for
Android Gerrit must follow the target tree's submission rules.

## Validation

From `common/`, run the inexpensive checks first:

```bash
git diff --check origin/main...HEAD
scripts/checkpatch.pl --strict --codespell --git origin/main..HEAD
```

From the Android kernel workspace root, run the arm64 build:

```bash
tools/bazel run --config=fast //common:kernel_aarch64_dist -- \
  --dist_dir="$PWD/out/evonix"
```

Changes to runtime policy, charging, storage, networking, or vendor interfaces
also require device testing. Record the ROM, kernel commit, test scenario, and
result in the pull request. A successful compile is not a substitute for boot
and runtime validation.

## Pull requests

Describe the problem and the chosen design, list affected interfaces, and
include the exact checks run. Call out changes to SELinux behavior, module ABI
validation, procfs/sysfs permissions, thermal limits, charging policy, or boot
flow explicitly. Keep reviewable changes separate from formatting churn.
