# Building EVONIX

EVONIX uses the Android Common Kernel Kleaf workspace. The GitHub repository is
the `common/` project; its sibling build rules, tools, and prebuilts come from the
AOSP kernel manifest.

## Requirements

- A 64-bit Linux host with Git, Python 3, `repo`, and standard build utilities.
- At least 30 GiB of free disk space; more is recommended for multiple outputs.
- Network access to Android Googlesource and GitHub for the initial sync.
- An unlocked test device, a known-good boot image, and a recovery method for
  device validation.

Kleaf uses the toolchains pinned by the manifest. Do not replace them with a
host compiler unless a change explicitly requires that experiment.

## Prepare the workspace

```bash
mkdir evonix-workspace
cd evonix-workspace

repo init \
  -u https://android.googlesource.com/kernel/manifest \
  -b common-android15-6.6-lts
repo sync -c -j"$(nproc)"

cd common
git remote add evonix https://github.com/NEESCHAL-3/EVONIX-kernel.git
git fetch evonix main
git switch -C main --track evonix/main
cd ..
```

If the `evonix` remote already exists, update its URL with `git remote set-url`
instead of adding it again. To inspect a preserved line, fetch and switch to the
specific branch named in `BRANCHES.md`.

## Build the arm64 distribution

Run this command from the workspace root, not from `common/`:

```bash
tools/bazel run --config=fast //common:kernel_aarch64_dist -- \
  --dist_dir="$PWD/out/evonix"
```

The target applies `common/arch/arm64/configs/evonix.config`. The output
directory contains the raw and compressed kernels, boot images, modules, header
archives, symbol lists, KMI check markers, and the SPDX SBOM.

## Verify the result

At minimum, confirm that the expected files exist and record their checksums:

```bash
test -s out/evonix/Image
test -s out/evonix/boot.img
test -s out/evonix/kernel_sbom.spdx.json
sha256sum out/evonix/Image out/evonix/boot.img
```

For source changes, also run from `common/`:

```bash
git diff --check
scripts/checkpatch.pl --strict --codespell --git origin/main..HEAD
```

Kleaf can emit benign duplicate-type-ID warnings while generating ABI data. A
build is successful only when Bazel exits with status zero and the distribution
step completes.

## Device validation

Do not flash an unverified image without a recovery path. Record the device ROM,
boot slot, commit, build command, image checksum, and observed runtime behavior.
Networking, thermal, charging, storage, scheduler, and compatibility changes
need targeted runtime checks in addition to a successful compile.
