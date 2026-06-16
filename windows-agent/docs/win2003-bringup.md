# Windows Server 2003 (NT 5.2) bring-up checklist

Operator runbook for the **legacy** Windows agent build. The single source
tree builds binaries for several NT generations:

| Profile | NT version | Arch | Windows | OpenSSL | Binary |
|---|---|---|---|---|---|
| modern (default) | 6.1–10.0 | x86_64 | Server 2008R2 / 2012 / 2012R2 / 2016 / 2019 / 2022, Win7–11 | 3.x | `assessment-agent.exe` |
| **legacy** (`LEGACY=1`) | 5.2 | x86_64 | **Server 2003 x64 Edition / XP x64** | 1.0.2u | `assessment-agent-legacy.exe` |
| **legacy32** | 5.2 | **i686 (32-bit)** | **Server 2003 x86 (the common SKU) / XP** | 1.0.2u | `assessment-agent-legacy-x86.exe` |

> **Pick by architecture, not just OS version.** The mass-market Server 2003
> install is **32-bit (x86)** — its x64 Edition was rare. So the default 2003
> target is **legacy32**; use **legacy** only for confirmed x64 Edition hosts.
> A 64-bit binary will not load on a 32-bit host at all. Check the host with
> `systeminfo | findstr /C:"System Type"` (`x86-based PC` → legacy32).

> Server 2008 / Vista (NT 6.0) is a grey zone: GetTickCount64/bcrypt exist, but
> OpenSSL 3.x does not officially support it. If you must cover it, build it
> from the legacy profile and validate TLS the same way as 2003.

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

## 1. Build the legacy binary

Requires an **older MinGW-w64 toolchain** that still targets NT 5.2 (recent
MinGW dropped XP/2003 startup support) and an OpenSSL 1.0.2 + 1.0.2-compatible
curl source. Pick the arch that matches your fleet (see the table above —
**32-bit is the common 2003 SKU**):

```bash
# 32-bit (i686) — Server 2003 x86, the default 2003 target.
# Cross-built from an x64 host with the i686-w64-mingw32 toolchain; override
# CC/AR/WINDRES/OPENSSL_CROSS if you build inside a native mingw32 shell.
make PROFILE=legacy32 vendor-fetch    # clones OpenSSL_1_0_2u + curl
make PROFILE=legacy32 vendor-build    # static 32-bit libs (OpenSSL target: mingw)
make PROFILE=legacy32 release         # builds assessment-agent-legacy-x86.exe + verify

# 64-bit (x86_64) — only for confirmed Server 2003 / XP x64 Edition hosts.
make LEGACY=1 vendor-fetch       # clones OpenSSL_1_0_2u + curl
make LEGACY=1 vendor-build       # static libs (OpenSSL 1.0.2: no `no-tests`)
make LEGACY=1 release            # builds assessment-agent-legacy.exe + verify
```

> legacy and legacy32 **share the `vendor/` tree** at different ABIs. Run
> `make vendor-clean` when switching between the x64 and x86 builds, or the
> link step mixes architectures and fails.

`make PROFILE=legacy32 verify` (and `make LEGACY=1 verify`) enforce the NT 5.2
import whitelist — **no `bcrypt.dll`, no `api-ms-win-*` umbrella DLLs** (2003
lacks them). If verify lists either, something re-introduced a static import;
fix before shipping.

Known per-profile build notes:
- OpenSSL 1.0.2 `Configure` has no `no-tests` flag (handled by
  `OPENSSL_CONFIG_OPTS` in the Makefile).
- Older curl may use `CMAKE_USE_OPENSSL` instead of `CURL_USE_OPENSSL`; adjust
  `vendor-build-curl` flags for the pinned curl version if cmake errors.
- If a 1.1.1 build is later proven to handshake (step 0), override with
  `make LEGACY=1 OPENSSL_VERSION=OpenSSL_1_1_1w ...` — no code change needed.

## 2. Install on the 2003 host (no PowerShell)

Copy the binary for your host's architecture — `assessment-agent-legacy-x86.exe`
for 32-bit Server 2003 (the common case), `assessment-agent-legacy.exe` for x64
Edition — plus `deploy\install.bat` to the host, then from an **elevated
Command Prompt**:

```cmd
install.bat                 :: register + start the service
install.bat --image-prep    :: register but do not start (golden image)
```

`install.bat` just forwards to `assessment-agent-legacy.exe install`
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

If `make LEGACY=1` link or load fails on 2003 with an unresolved import other
than the two already handled, it means another NT6+/NT10-only API is statically
imported. Find it with `objdump -T` / dependency walker, and wrap it in the
same dynamic-`GetProcAddress` pattern (see `RtlGetVersion` in `installer.c` and
`monotonic_ms()` in `util.c`).
