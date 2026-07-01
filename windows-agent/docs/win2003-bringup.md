# Windows Server 2003 (NT 5.2) bring-up checklist

Operator runbook for the **legacy32** Windows agent build. The single source
tree builds binaries for several NT generations:

| Profile | NT version | Arch | Windows | OpenSSL | Binary |
|---|---|---|---|---|---|
| modern (default) | 10.0 | x86_64 | Server 2016 / 2019 / 2022 / 2025, Win10/11 | 3.x | `assessment-agent-win2016-x64.exe` |
| win7 | 6.1–6.3 | x86_64 | Server 2008R2 / 2012 / 2012R2, Win7–8.1 | 3.x | `assessment-agent-win2008r2-x64.exe` |
| **legacy32** | 5.2 | **i686 (32-bit)** | **Server 2003 x86 / XP** | 1.0.2u | `assessment-agent-win2003-x86.exe` |

> **Server 2003 is x86 only here.** The mass-market 2003 install is 32-bit (x86);
> the x64 Edition was rare and is **not** built — `legacy32` is the sole 2003
> target. A 64-bit binary would not load on a 32-bit host anyway. Confirm with
> `systeminfo | findstr /C:"System Type"` (expect `x86-based PC`).

> **legacy32 needs a vendor patch.** rabbitmq-c v0.10's `win32/threads` uses
> SRWLock (Vista+), which NT5.2 lacks → link fails with `undefined reference to
> InitializeSRWLock`. Overwrite `vendor/rabbitmq-c/librabbitmq/win32/threads.{h,c}`
> with the CRITICAL_SECTION version from `agent-fleet/win-legacy-patch` after
> `vendor-fetch`, before `vendor-build`. See that directory's README.

The code is **identical** between profiles — `GetTickCount64` and
`BCryptGenRandom` are resolved at runtime (`monotonic_ms()` /
`compat_rand_bytes()` in `src/util.c`), so the NT 5.2 build never emits those
as static imports. Only build flags and the OpenSSL/curl versions differ.

---

## 0. TLS handshake PoC — DO THIS FIRST (highest-risk unknown)

The agent connects to RabbitMQ over **AMQPS, TLS 1.2+**. OpenSSL 1.0.2 *can*
do TLS 1.2 + AES-GCM, but if the broker is configured TLS1.3-only, or only
offers cipher suites 1.0.2 lacks, **Server 2003 can never connect** — and no
amount of build work fixes that. Verify before building anything:

```cmd
REM On the 2003 image, with an OpenSSL 1.0.2 win32 build:
openssl s_client -connect <broker-host>:5671 -tls1_2
```

Confirm in the output:
- handshake completes (you reach the interactive prompt / cert chain prints)
- `Protocol  : TLSv1.2`
- `Cipher    : <suite>` is a suite OpenSSL 1.0.2 supports (e.g.
  `ECDHE-RSA-AES256-GCM-SHA384`)

If it fails: the broker's `ssl_options` (`versions` / `ciphers`) is owned by
another team — escalate and ask them to allow TLS 1.2 + a 1.0.2-compatible
cipher for the 2003 fleet. **Do not proceed to the build until this passes.**
If they cannot/ will not, drop 2003 — the legacy build is wasted otherwise.

---

## 1. Build the legacy32 binary

Requires an **older MinGW-w64 (i686) toolchain** that still targets NT 5.2
(recent MinGW dropped XP/2003 startup support) and an OpenSSL 1.0.2 +
1.0.2-compatible curl source. Server 2003 is x86 only here (the rare x64 Edition
is not built):

```bash
# 32-bit (i686) — Server 2003 x86, the sole 2003 target.
# Cross-built from an x64 host with the i686-w64-mingw32 toolchain; override
# CC/AR/WINDRES/OPENSSL_CROSS if you build inside a native mingw32 shell.
make PROFILE=legacy32 vendor-fetch    # clones OpenSSL_1_0_2u + curl
# Apply the NT5.2 rabbitmq threads patch (SRWLock -> CRITICAL_SECTION) BEFORE
# vendor-build, or the link fails with `undefined reference to InitializeSRWLock`:
cp ../../agent-fleet/win-legacy-patch/rabbitmq-threads.h \
   vendor/rabbitmq-c/librabbitmq/win32/threads.h
cp ../../agent-fleet/win-legacy-patch/rabbitmq-threads.c \
   vendor/rabbitmq-c/librabbitmq/win32/threads.c
make PROFILE=legacy32 vendor-build    # static 32-bit libs (OpenSSL target: mingw)
make PROFILE=legacy32 release         # builds assessment-agent-win2003-x86.exe + verify
```

> When cross-building, rabbitmq-c also needs `-lwinpthread` (give it via
> `EXTRA_SYS_LIBS="-lgdi32 -lssp -lwinpthread"` on the `release` line), and
> `make clean` between profiles so i686/x86_64 objects do not mix.

`make PROFILE=legacy32 verify` enforces the NT 5.2 import whitelist — **no
`bcrypt.dll`, no `api-ms-win-*` umbrella DLLs** (2003 lacks them). If verify
lists either, something re-introduced a static import; fix before shipping.

Known build notes:
- OpenSSL 1.0.2 `Configure` has no `no-tests` flag (handled by
  `OPENSSL_CONFIG_OPTS` in the Makefile).
- Older curl may use `CMAKE_USE_OPENSSL` instead of `CURL_USE_OPENSSL`; adjust
  `vendor-build-curl` flags for the pinned curl version if cmake errors.
- If a 1.1.1 build is later proven to handshake (step 0), override with
  `make PROFILE=legacy32 OPENSSL_VERSION=OpenSSL_1_1_1w ...` — no code change needed.

## 2. Install on the 2003 host (no PowerShell)

Copy `assessment-agent-win2003-x86.exe` plus `deploy\install.bat` to the host,
then from an **elevated Command Prompt**:

```cmd
install.bat                 :: register + start the service
install.bat --image-prep    :: register but do not start (golden image)
```

`install.bat` just forwards to `assessment-agent-win2003-x86.exe install`
(`CreateServiceW` + auto-restart policy via `ChangeServiceConfig2W`, both
present on NT 5.2). If that fails, the script prints a raw `sc.exe create`
fallback for diagnosis.

Provide the env file the same way as modern hosts (broker host/port/vhost,
credentials, CA pem path for AMQPS).

## 3. Smoke checks

- `sc query assessment-agent` → `STATE: 4 RUNNING`
- Broker management UI shows the connection + `server.metrics` arriving every
  `AGENT_INTERVAL_SEC`.
- A published message carries a non-null `boot_time`.
  - **Caveat (accepted):** on NT 5.2 the GetTickCount fallback wraps every
    49.7 days, so if the host's uptime already exceeds 49.7 days when the agent
    starts, the absolute `boot_time` is offset-wrong. Because it is captured
    once at process start and the engine only detects *changes*,
    counter-reset detection is **not** broken — the value is stable for the
    process lifetime. Vista/2008+ resolve `GetTickCount64` and stay exact.
- `machine_id` is populated (random bytes come from `compat_rand_bytes` →
  CryptGenRandom on 2003, not bcrypt).

## 4. Other version-gated APIs

If `make PROFILE=legacy32` link or load fails on 2003 with an unresolved import other
than the two already handled, it means another NT6+/NT10-only API is statically
imported. Find it with `objdump -T` / dependency walker, and wrap it in the
same dynamic-`GetProcAddress` pattern (see `RtlGetVersion` in `installer.c` and
`monotonic_ms()` in `util.c`).
