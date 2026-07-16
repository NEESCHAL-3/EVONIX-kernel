# EVONIX architecture

EVONIX layers device-oriented policy and compatibility work on the Android 15
Linux 6.6 common kernel. The code remains a monolithic GKI build: EVONIX
components are built into the arm64 kernel through the EVONIX config fragment
and the kernel Makefiles.

## Source map

| Area | Primary paths | Responsibility |
| --- | --- | --- |
| Build configuration | `arch/arm64/configs/evonix.config`, `BUILD.bazel` | Applies EVONIX networking, storage, branding, and vendor-module options to `kernel_aarch64`. |
| Rodin controller | `drivers/misc/evonix_rodin/` | Workload state, input events, CPU QoS, load detection, charging interaction, and thermal policy. |
| ColorOS compatibility | `drivers/misc/evonix_cos/` | Power, display, storage, scheduler, AFS, QoS, and procfs interfaces expected by ColorOS userspace. |
| Storage | `block/`, `drivers/ufs/core/`, `fs/f2fs/` | Kyber selection, UFS telemetry/control compatibility, and F2FS ownership behavior. |
| Networking | `net/ipv4/`, `include/net/`, `include/uapi/linux/` | BBRv3 backport, pacing signals, diagnostics, and FQ defaults. |
| Vendor modules | `kernel/module/` | Allows target vendor modules across the intended GKI version boundary. |
| Branding | `Makefile`, `scripts/mkcompile_h`, `scripts/setlocalversion` | EVONIX release name and build identity. |

## Rodin controller

`evonix_rodin_core.c` owns the shared state and `/proc/evonix_rodin` root.
Input events and workload sampling feed the controller; the QoS and thermal
components apply policy and expose read-only diagnostic views. Keep policy
decisions in their owning component rather than duplicating state transitions
across proc handlers.

## ColorOS compatibility

The compatibility layer supplies the interfaces expected by vendor userspace
while attempting to use real kernel, power-supply, UFS, scheduler, and storage
state. These paths form an ABI even when they are not upstream Linux APIs.
Changing a name, mode, unit, or write behavior therefore requires ROM-level
testing and documentation.

Several compatibility nodes use broad Android-facing permissions. Treat every
write handler as an input-validation and privilege-boundary review point.

## Networking and storage defaults

The EVONIX fragment enables BBR as the default TCP congestion controller and FQ
as the default qdisc. The BBRv3 port also changes supporting TCP headers, rate
sampling, timers, diagnostics, and output paths; it must be reviewed and tested
as one coordinated subsystem change.

Kyber is available for multi-queue block devices, and EVONIX policy prefers it
for physical UFS queues. Storage changes require latency, throughput, suspend,
and data-integrity testing—not benchmark results alone.

## Security-sensitive compatibility choices

The current maintained lineage forces SELinux permissive and accepts module
version mismatches. These are explicit compatibility decisions, not hidden
defaults. Any future hardened line should make the policy selectable, restore
stock enforcement and module validation, and validate vendor compatibility as a
separate deliverable.

See [SECURITY.md](../../SECURITY.md) for reporting and support policy.
