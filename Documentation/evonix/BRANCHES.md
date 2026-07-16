# Branch and tag policy

EVONIX keeps published development lines because they document device-tested
milestones and isolated feature work. They are not interchangeable release
channels.

## Branch lineage

```text
evonix-rodin-hyperos-main
├── evonix-feature-evonix-info
│   ├── evonix-feature-boot-helper-umh
│   └── evonix-feature-manager-control-proc-v1
└── evonix-cos-upstream-20260707
    └── evonix-cos-permissive
        └── evonix-rodin-optimizer
            └── evonix-bbr3-port
                └── main (repository documentation and maintenance)
```

## Published branches

| Branch | Audited tip before publication | Purpose |
| --- | --- | --- |
| `main` | descendant of `3847963653c7` | Maintained GitHub default; latest BBRv3 line plus repository documentation. |
| `evonix-bbr3-port` | `3847963653c7` | BBRv3/FQ networking on top of the Rodin optimizer line. |
| `evonix-rodin-optimizer` | `9e6aede8fcf6` | Rodin workload, input, QoS, I/O, charging, and thermal controller. |
| `evonix-cos-permissive` | `eccacca3d11e` | ColorOS compatibility line with deliberately permissive SELinux. |
| `evonix-cos-upstream-20260707` | `4124193dbc22` | AOSP LTS integration checkpoint before the ColorOS series. |
| `evonix-rodin-hyperos-main` | `c3941b1cde6f` | Foundational Rodin/HyperOS device baseline. |
| `evonix-feature-evonix-info` | `813cd145de1e` | Kernel identity and charging API feature line. |
| `evonix-feature-boot-helper-umh` | `52fb6bd9ef56` | Experimental userspace boot-helper feature line. |
| `evonix-feature-manager-control-proc-v1` | `22a0813db78d` | Experimental manager proc/misc control feature line. |

The three `evonix-feature-*` branches are preserved experiments and are not
implicitly included in `main`. Compare trees before porting one of them.

## Tags

Custom tags use these naming families:

- `EVX_*` for ColorOS compatibility milestones.
- `evonix-*-build-ok` for compile-validated checkpoints.
- `evonix-*-phone-tested-ok` for device-tested checkpoints.
- `rodin-*` for Rodin-specific compatibility milestones.
- `archive/*evonix*` for preserved historical states.

A tag records the evidence available when it was created; it is not a promise
that the tagged tree is secure or supported today. Published tags are immutable.

## Maintenance rules

- Advance `main` with reviewed, signed-off commits.
- Never rewrite a published branch or tag to improve old commit messages.
- Integrate AOSP updates on a dedicated branch and validate the merge before
  advancing `main`.
- Use a new branch for experiments that intentionally diverge in security or
  device behavior.
- Record build and device-test evidence in the commit or pull request before
  creating a new milestone tag.
