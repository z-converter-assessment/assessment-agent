# Supported Operating Systems

Targets for `assessment-agent` (Linux) and `windows-agent/` (Windows).
Architecture: **x86_64 only**.

Each platform ships in two ABI profiles built from one source tree — a
**modern** profile (default) and a **legacy** profile for pre-2010 OSes. The
code is identical; only build flags and vendored TLS library versions differ.

## Linux (supported)

| Family | Versions | ID (`/etc/os-release`) | Profile |
|---|---|---|---|
| Ubuntu LTS | 18.04, 20.04, 22.04, 24.04 | `ubuntu` | modern |
| Debian | 10, 11, 12, 13 | `debian` | modern |
| RHEL | 7, 8, 9 | `rhel` | modern |
| CentOS / CentOS Stream | 7, 8, 9 (Stream) | `centos` | modern |
| Rocky Linux | 8, 9 | `rocky` | modern |
| AlmaLinux | 8, 9 | `almalinux` | modern |
| Oracle Linux | 7, 8, 9 | `ol` / `oracle` | modern |
| Amazon Linux | 2, 2023 | `amzn` | modern |
| SLES / openSUSE Leap | 12 (SP3+), 15 (SP1+) | `sles` / `opensuse-leap` | modern |
| Tencent OS | 4.0, 4.2 | `tencentos` | modern |
| **RHEL / CentOS / Oracle Linux 6** | **6** | *(no os-release — `/etc/redhat-release`)* | **legacy** |

ABI ceilings, enforced by `make verify` / `make verify-legacy` on the build host:
- **modern**: glibc 2.17 / kernel 3.10 (manylinux2014, CentOS 7 baseline) →
  `make release` → `dist/assessment-agent-linux-x86_64`
- **legacy**: glibc 2.12 / kernel 2.6.32 (manylinux2010, CentOS 6 baseline) →
  `make release-legacy` → `dist/assessment-agent-linux-x86_64-glibc2.12`

EL6 is SysV-init (no systemd) and predates `/etc/os-release`; both are handled
automatically (see "OS detection logic" below). See
[`../docs/centos6-bringup.md`](../docs/centos6-bringup.md).

## Windows (supported, Phase 1 — collect only)

Three build profiles from one source tree (`make PROFILE=…`):

| Family | Versions | NT | Profile | Binary | Verified |
|---|---|---|---|---|---|
| Windows Server | 2016, 2019, 2022, 2025 | 10.0 | `modern` | `assessment-agent.exe` | yes (CI + smoke) |
| Windows | 10, 11 | 10.0 | `modern` | `assessment-agent.exe` | yes |
| Windows Server | 2008 R2, 2012, 2012 R2 | 6.1–6.3 | `win7` | `assessment-agent-win7.exe` | **build only** |
| Windows | 7, 8, 8.1 | 6.1–6.3 | `win7` | `assessment-agent-win7.exe` | **build only** |
| **Windows Server** | **2003 / XP x64** | **5.2** | `legacy` | `assessment-agent-legacy.exe` | **build only + TLS PoC** |

- `modern` → OpenSSL 3.x, `_WIN32_WINNT=0x0A00`. The only profile validated
  end-to-end (CI build + installer smoke).
- `win7` → `make PROFILE=win7`, OpenSSL 3.x (still supports Win7),
  `_WIN32_WINNT=0x0601`. Built in CI; **NT 6.x runtime not yet validated on
  real hardware.**
- `legacy` → `make PROFILE=legacy` (alias `LEGACY=1`), OpenSSL 1.0.2u,
  `_WIN32_WINNT=0x0502`, needs an older MinGW toolchain. `GetTickCount64` /
  `BCryptGenRandom` are resolved at runtime so the NT 5.2 import table stays
  valid. Gated on the broker TLS PoC (see below).

`win7` / `legacy` build jobs run in CI as `continue-on-error` (the legacy
toolchain/OpenSSL 1.0.2 may not build on the windows-latest runner) and attach
to releases best-effort. The `task.install` worker is **not** in the Windows
Phase 1 build — collection (inventory + metrics + error) only. Server 2008 /
Vista (NT 6.0) is a grey zone (OpenSSL 3.x is unsupported there); cover it via
the `legacy` profile if needed. See
[`../windows-agent/docs/win2003-bringup.md`](../windows-agent/docs/win2003-bringup.md).

> **Server 2003 caveat:** TLS reaches the broker only if the broker offers
> TLS 1.2 + a cipher OpenSSL 1.0.2 supports. Validate with the `s_client` PoC
> in the Windows bring-up doc **before** committing to a 2003 rollout.

## Explicitly unsupported

| OS | Reason |
|---|---|
| SUSE 11 | pre-systemd, separate glibc baseline — not covered by either profile |
| RHEL 5 | glibc < 2.12, kernel < 2.6.32 — below the legacy ceiling |
| Any 32-bit architecture | builds are x86_64 only |
| Containers (Docker / k8s pods) | the agent assumes it observes the host `/proc` namespace |

If the OS is unsupported, `deploy/install.sh` refuses with a single-line
error message and exits non-zero. Same on Windows for unsupported builds.

## OS detection logic

- Linux: `deploy/lib/detect-os.sh` parses `/etc/os-release` (matching `$ID` +
  `$VERSION_ID` / major). When `/etc/os-release` is **absent** (pre-systemd
  EL6), it falls back to `/etc/redhat-release` / `/etc/centos-release` /
  `/etc/oracle-release` and accepts a `release 6` token.
- Linux init: `deploy/install.sh` installs a systemd unit when systemd is
  present, otherwise the SysV init script `deploy/sysv/assessment-agent`
  (registered via `chkconfig` / `update-rc.d`).
- Windows: the modern installer checks
  `[Environment]::OSVersion.Version.Major >= 10`. The legacy build targets NT
  5.2 and installs via `deploy/install.bat` (or
  `assessment-agent-legacy.exe install`) since Server 2003 has no PowerShell.
