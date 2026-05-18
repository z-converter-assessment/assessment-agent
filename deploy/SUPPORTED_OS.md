# Supported Operating Systems

Targets for `assessment-agent` (Linux) and `windows-agent/` (Windows).
Architecture: **x86_64 only**.

## Linux (supported)

| Family | Versions | ID (`/etc/os-release`) |
|---|---|---|
| Ubuntu LTS | 18.04, 20.04, 22.04, 24.04 | `ubuntu` |
| Debian | 10, 11, 12, 13 | `debian` |
| RHEL | 7, 8, 9 | `rhel` |
| CentOS / CentOS Stream | 7, 8, 9 (Stream) | `centos` |
| Rocky Linux | 8, 9 | `rocky` |
| AlmaLinux | 8, 9 | `almalinux` |
| Oracle Linux | 7, 8, 9 | `ol` / `oracle` |
| Amazon Linux | 2, 2023 | `amzn` |
| SLES / openSUSE Leap | 12 (SP3+), 15 (SP1+) | `sles` / `opensuse-leap` |
| Tencent OS | 4.0, 4.2 | `tencentos` |

ABI ceiling: **glibc 2.17 / kernel 3.10** (CentOS 7 baseline). Enforced by
`make verify` on the build host.

## Windows (supported, Phase 1 — collect only)

| Family | Versions |
|---|---|
| Windows Server | 2016, 2019, 2022, 2025 |
| Windows | 10, 11 |
| PowerShell | 5.1+ (installer requirement) |

`task.install` worker is **not** in the Windows Phase 1 build — collection
(inventory + metrics + error) only.

## Explicitly unsupported

| OS | Reason |
|---|---|
| CentOS 6 | kernel < 3.10, glibc < 2.17, no systemd |
| SUSE 11 | same — pre-systemd |
| RHEL 5 / 6 | same |
| Any 32-bit architecture | builds are x86_64 only |
| Containers (Docker / k8s pods) | the agent assumes it observes the host `/proc` namespace |

If the OS is unsupported, `deploy/install.sh` refuses with a single-line
error message and exits non-zero. Same on Windows for unsupported builds.

## OS detection logic

- Linux: `/etc/os-release` is parsed by `deploy/lib/detect-os.sh`. Matching
  is by `$ID` + `$VERSION_ID` (or `$VERSION_ID` major).
- Windows: `install.ps1` checks `[Environment]::OSVersion.Version.Major >= 10`
  (Server 2016 / Windows 10 = NT 10.0).
