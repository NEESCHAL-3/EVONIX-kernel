# EVONIX Kernel

EVONIX is an Android 15 kernel based on the AOSP `android15-6.6-lts` common
kernel and tuned for the Poco X7 Pro (`rodin`). The project combines device
compatibility work with a Rodin-specific performance controller, ColorOS
compatibility interfaces, storage tuning, and a BBRv3/FQ networking stack.

> [!WARNING]
> The current `main` lineage is a development kernel. It deliberately keeps
> SELinux permissive and relaxes kernel-module version checks for vendor-module
> compatibility. Do not treat it as a production-security configuration. Read
> [SECURITY.md](SECURITY.md) before flashing or redistributing a build.

## Project profile

| Item | Value |
| --- | --- |
| Device | Poco X7 Pro (`rodin`) |
| Android kernel base | AOSP `android15-6.6-lts` |
| Kernel release | Linux 6.6.139 with EVONIX branding |
| Maintained branch | `main` |
| Build system | Kleaf / Bazel |
| License | GPL-2.0-only; see [COPYING](COPYING) |

## Highlights

- Google BBRv3 backport with BBR and FQ selected by the EVONIX config fragment.
- Event-driven Rodin workload, input, CPU QoS, I/O, charging, and thermal policy.
- Real-backed ColorOS compatibility nodes for power, display, storage, scheduler,
  AFS, QoS, and procfs consumers.
- Kyber support and EVONIX policy for physical UFS queues.
- Vendor Wi-Fi/Bluetooth module compatibility across the target GKI integration.
- Reproducible arm64 Kleaf build with strict KMI checking and an SPDX SBOM.

The implementation map and security-sensitive design choices are documented in
[Documentation/evonix/ARCHITECTURE.md](Documentation/evonix/ARCHITECTURE.md).

## Build

This repository is the `common/` project inside an Android kernel `repo`
workspace. A standalone clone does not contain Kleaf, the prebuilts, or the
other projects needed for a complete build.

After preparing the workspace as described in
[Documentation/evonix/BUILDING.md](Documentation/evonix/BUILDING.md), build the
arm64 distribution from the workspace root:

```bash
tools/bazel run --config=fast //common:kernel_aarch64_dist -- \
  --dist_dir="$PWD/out/evonix"
```

The distribution includes `Image`, compressed images, boot images, GKI modules,
headers, symbol data, and `kernel_sbom.spdx.json`.

## Branches and milestones

Published development lines are intentionally preserved. `main` follows the
latest BBRv3 line, while feature branches and milestone tags retain build- and
device-tested checkpoints. See
[Documentation/evonix/BRANCHES.md](Documentation/evonix/BRANCHES.md) before
starting work from a historical branch.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change. Patches should
be narrowly scoped, signed off, checked with `scripts/checkpatch.pl`, and
validated with the relevant Kleaf target. Generated files, flashable packages,
private keys, and local build logs do not belong in source control.

## Disclaimer

Flashing a custom kernel can prevent a device from booting and can cause data
loss. Keep a known-good boot image and a recovery path. You are responsible for
device-specific verification and compliance with the licenses of any bundled or
redistributed components.
