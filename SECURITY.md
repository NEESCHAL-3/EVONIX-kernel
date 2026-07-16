# Security policy

## Supported code

Security fixes target `main`. The `evonix-bbr3-port` branch is the immediate
source lineage for `main`; other feature branches and milestone tags are
preserved for history and are not independently supported.

## Development security posture

The maintained lineage currently makes compatibility choices that reduce the
security guarantees of a stock Android GKI kernel:

- SELinux enforcement is forced off.
- Module version and symbol-version mismatches are allowed.
- Several ColorOS compatibility interfaces are intentionally writable by
  Android userspace.

These choices are visible and intentional, but they make the kernel unsuitable
for production, high-assurance, or security-sensitive use. Do not report the
documented existence of these behaviors as a new vulnerability. Reports about
an unexpected privilege boundary bypass, memory-safety flaw, information leak,
or unsafe interaction within those interfaces are still welcome.

## Reporting a vulnerability

Use a private
[GitHub security advisory](https://github.com/NEESCHAL-3/EVONIX-kernel/security/advisories/new).
Do not open a public issue for an unpatched vulnerability.

Include the affected commit or branch, impact, reproduction steps, relevant
configuration, and a proposed fix when available. Do not include personal device
data, proprietary vendor material, credentials, or signing keys.

There is no guaranteed response or disclosure timeline. Please allow reasonable
time to reproduce and fix a report before public disclosure.
