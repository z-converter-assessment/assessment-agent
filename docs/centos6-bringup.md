# CentOS 6 (and RHEL 6 / Oracle Linux 6) bring-up checklist

Operator runbook for the **legacy** Linux agent build. One source tree, two
ABI profiles produced from two different build containers:

| Profile | Build container | glibc ceiling | Kernel | Targets | Artifact |
|---|---|---|---|---|---|
| modern (default) | manylinux2014 | 2.17 | 3.10 | RHEL/CentOS 7+, Ubuntu 18.04+, Debian 10+, Amazon Linux 2/2023, SLES 12/15, Tencent 4 | `dist/assessment-agent-linux-x86_64` |
| **legacy** | manylinux2010 | **2.12** | 2.6.32 | **CentOS / RHEL / Oracle Linux 6** | `dist/assessment-agent-linux-x86_64-glibc2.12` |

The **code is identical**; CentOS 6 differences are handled at runtime
(`detect-os.sh` redhat-release fallback, `install.sh` SysV branch,
`collect.c` MemAvailable reproduction). Only the build container's glibc and
the `make verify` ceiling differ.

---

## 1. Build the legacy binary

The legacy binary MUST be compiled inside a **manylinux2010** container so it
links against glibc 2.12. Building it on a newer host and renaming will NOT
lower the ABI — the container is what sets the floor.

```bash
# From a native amd64 Linux host with Docker:
docker run --rm -v "$(pwd)":/src -w /src quay.io/pypa/manylinux2010_x86_64 \
  bash -c 'make vendor-fetch && make vendor-build && make USE_VENDORED=1 release-legacy'
```

`release-legacy` runs `verify-legacy`, which enforces the glibc 2.12 ceiling:
- **forbids GLIBC_2.13+** symbols (`GLIBC_2\.(1[3-9]|[2-9][0-9])`)
- same dynamic-dep whitelist (only libc/libpthread/libdl/libm/libresolv/librt
  stay dynamic; OpenSSL/curl/rabbitmq-c/libarchive are vendored static)
- forbidden-API set extended for the older ceiling (e.g. `secure_getenv`,
  glibc-2.17+ helpers absent on 2.12)

If `verify-legacy` FAILs with a GLIBC_2.13+ symbol, something in the vendored
libs pulled a newer glibc API — rebuild that vendor lib inside the 2010
container, do not bump the ceiling.

> The modern artifact is built separately in the manylinux2014 container with
> `make USE_VENDORED=1 release`. The two coexist in `dist/`.

## 2. Install on a CentOS 6 host

CentOS 6 is pre-systemd (SysV init) and has no `/etc/os-release`. Both are
handled automatically:

- `deploy/lib/detect-os.sh` falls back to `/etc/redhat-release` and accepts
  `release 6` (CentOS / RHEL / Oracle Linux 6).
- `deploy/install.sh` detects the absence of systemd and installs the SysV
  init script `deploy/sysv/assessment-agent` to `/etc/init.d/assessment-agent`,
  registering it with `chkconfig`.

```bash
# Ship the glibc2.12 binary as dist/assessment-agent-linux-x86_64 (the name
# install.sh expects) or point DIST_BIN at it, then:
sudo ./deploy/install.sh
```

Verify the service:
```bash
service assessment-agent status
chkconfig --list assessment-agent     # on for runlevels 2-5
tail -f /var/log/assessment-agent.log # SysV hosts have no journald
```

Graceful stop honors the worker-drain budget (`service assessment-agent stop`
waits up to 900s for an in-flight install, mirroring the systemd
`TimeoutStopSec=15min`).

## 3. Data-accuracy note — `mem_available_kb` is NOT a coarse approximation

CentOS 6's kernel (2.6.32 < 3.14) does not export `MemAvailable`. The agent
reproduces the kernel's own `si_mem_available()` formula in user space
(`derive_mem_available_kb()` in `collect.c`): MemFree − low-watermark sum
(from `/proc/zoneinfo`) + reclaimable pagecache (`Active(file)`+`Inactive(file)`)
+ reclaimable slab (`SReclaimable`), each discounted. This is the **same
algorithm** the kernel uses, so the value matches kernels that do export it —
the engine's memory utilization (`1 − available/total`) is consistent with all
other OSes and the old under-provisioning bias is gone. The coarse
`MemFree+Buffers+Cached` is only a last-resort fallback if `/proc/zoneinfo` is
unreadable.

No engine-side change is required for CentOS 6 memory data.

## 4. Smoke checks

- `service assessment-agent status` → running
- Broker UI: connection up, `server.metrics` every `AGENT_INTERVAL_SEC`,
  `server.inventory` on start
- `machine_id` populated. CentOS 6 has no `/etc/machine-id`; the agent falls
  back to `dbus-uuidgen --get` (ensure `dbus` is installed) → then cloud IMDS.
  The value is stable across restarts.
- `mem_available_kb` present and non-null in a metrics message (step 3).
- AMQPS: the agent connects over TLS 1.2 to port 5671 with the vendored
  OpenSSL — confirm the broker accepts TLS 1.2 from this host.

## 5. Scope

RHEL 6 and Oracle Linux 6 share the exact glibc-2.12 / kernel-2.6.32 / SysV
baseline and are covered by the same legacy binary and the same
`/etc/redhat-release` detection. SUSE 11 (also pre-systemd) is **not** covered
by this profile — it would need its own baseline.
