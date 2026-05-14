# Assessment Agent — Implementation Context

## Project Position

**Phase 1 — Data Collection / Inventory Acquisition** of the AI-based Assessment service.

The full service collects customer-server resources, runs AI analysis (avg / p95 / peak, over/under-provisioning detection) and produces sizing recommendations as a PDF report. **This agent owns the collection step.**

Beyond collection, the agent also accepts `task.install` commands from the portal — HTTPS-downloaded tarballs containing a user-level `install.sh` — and reports execution results. This is the **worker** role. Collection (producer) and worker (consumer + executor) share a single binary and a single process; the parent loop publishes metrics on a 1-minute tick and polls a per-machine task queue on the same tick, forking a child for each install. The original "no-execution" principle has been deliberately retired — see "Worker (task.install)" below for the boundaries.

Pipelines:
- Inventory/metrics/error: `agent (this repo) → RabbitMQ → assessment-engine (consumer + web)`
- Task install: `assessment-portal → RabbitMQ → agent (this repo, worker role)`

---

## Target Environment

- **Architecture**: x86_64 only
- **Minimum kernel**: 3.10 (CentOS 7)
- **Deployment unit**: Linux VM on a major public cloud (AWS EC2, Azure VM, GCP CE)
- **Current scope**: Ubuntu LTS (20.04 / 22.04 / 24.04) first. Other distros are best-effort and may emit `null` fields.
- **Containers**: not supported as a deployment target. The agent assumes it observes the host VM `/proc` namespace.

---

## Network Assumption

- Internal network only. Agent and broker live on the same private network.
- Agent → broker is **AMQPS only** (TLS 1.2+, port 5671) in production.
- External-IP discovery uses the cloud metadata service (link-local 169.254.169.254). It is **optional**: failure becomes `null`, never blocks publishing.

---

## Implementation

- **Language**: C
- **AMQP client**: `rabbitmq-c` (librabbitmq) with TLS via OpenSSL
- **JSON serialization**: `cJSON`
- **HTTPS download (worker)**: `libcurl` with OpenSSL backend
- **tar extract (worker)**: `libarchive`
- **Build**: `make` → single `./assessment-agent` binary. `make USE_VENDORED=1` links statically against `vendor/{cJSON,rabbitmq-c,curl,libarchive}` (after `make vendor-fetch && make vendor-build`). OpenSSL is shared across rabbitmq-c, libcurl, and the sha256 verification path.
- **Execution mode**:
  - one-shot (`AGENT_INTERVAL_SEC=0`) — for cron / systemd timer. Worker disabled in one-shot.
  - loop mode (positive interval) — single long-running process. Each tick: poll metrics → publish → if idle and a task message exists, fork a worker child for it → reap any finished child → sleep.
- **Optional runtime commands** (silent fallback when missing — startup logs availability to stderr):
  - `lsblk` — disk listing; falls back to `/sys/block` scan
  - `curl` — IMDS / external IP; missing → `null`
  - `dbus-uuidgen` — machine_id fallback link in the resolution chain

### Source Layout

```
src/
  main.c        # env parsing, collection/publish + worker loop, entry point
  collect.c/.h  # /proc, /sys, syscall-based collectors
  publish.c/.h  # rabbitmq-c connection, TLS, basic_publish, basic_get, deadline-bounded confirm
  util.c/.h     # env file loader, string helpers
  worker.c/.h   # task.install consumer state machine — basic_get poll, fork/reap, idempotency markers
  download.c/.h # libcurl HTTPS download with sha256 streaming + host whitelist + size cap
  extract.c/.h  # libarchive tar extract with entry-type whitelist + permission stripping
  exec.c/.h     # fork/execve install.sh with clearenv + setrlimit + setsid + 4KB tail capture
include/        # public headers for the source files above
docs/
  payload-schema.md   # message schema (canonical contract with the engine)
tests/
  unit/
    tinytest.h        # header-only single-TU test harness
    test_main.c       # parser/helper unit tests (compile-includes collect.c)
  test_schema.sh      # smoke test — single-publish payload validation
  run_multi.sh        # N agents publishing in parallel
  fault_agent.sh      # SIGKILL resilience
  fault_rabbitmq.sh   # broker restart resilience
  run_all.sh
scripts/
  rabbitmq-up.sh      # local dev broker (Docker)
  rabbitmq-down.sh
deploy/
  systemd/
    assessment-agent.service  # systemd unit with User= and TimeoutStopSec= aligned with worker drain
vendor/                # gitignored; populated by `make vendor-fetch`
  cJSON/               # static lib build target
  rabbitmq-c/          # static lib build target
  curl/                # static lib build target (worker download)
  libarchive/          # static lib build target (worker extract)
```

### Core Principles

1. **Restricted execution.** Beyond pure read of `/proc` and `/sys`, the agent now executes `install.sh` scripts supplied by `task.install` messages from the portal. Execution runs as the agent's non-privileged user (no root, no sudo) with a hardened environment (`clearenv` + `setrlimit` + `setsid` + a /tmp extraction sandbox). No other side effects — the agent does not manage packages, services, or its own binary outside the explicit task channel.
2. **Stateless metric counters.** The agent does not retain previous tick / counter values across cycles. It emits raw cumulative counters from `/proc`. All delta, percentage and rate computations happen in the engine.
3. **Raw values only.** No `avg`, no `pct`, no `iops_per_second`. The 1-minute metric message is a snapshot of `/proc` raw fields.
4. **Single normalized schema.** OS-specific command differences are absorbed inside the agent (collector branches by `os_id` / kernel). The wire format is **identical** regardless of OS.
5. **Optional features are not errors.** Missing `ethtool`, `lvs`, `docker`, etc. produce `null` / empty arrays. They do not generate `server.error` messages.
6. **Task durability over speed.** Worker prefers correctness over latency: at-most-one install at a time per host (M1), one-minute task pickup poll (T1), result persisted to disk before broker ack so a parent crash never silently loses a completed install (R2 + I1).

---

## Message Types & Routing

Exchange: `assessment` (**direct**, durable). Delivery mode: persistent (2). Vhost: `/assessment`.

| Type | Routing key | Trigger | Description |
|------|-------------|---------|-------------|
| `inventory` | `server.inventory` | startup + on detected static change + periodic refresh (`AGENT_INVENTORY_REFRESH_SEC` ±15% jitter, default 3600s, 0 disables) | OS, kernel, CPU model, total memory, disks, mounts, IPs |
| `metrics`   | `server.metrics`   | every `AGENT_INTERVAL_SEC` (default 60) | raw `/proc` counters: `cpu_stat`, `mem_*_kb`, `swap_*_kb`, `load_{1,5,15}m`, `disk_io[]`, `net_io[]`, `mounts[]` |
| `error`     | `server.error`     | collection / publish failure (see policy below) | `error_code`, `error_message`, `failed_component` |

**Inventory periodic refresh** is a recovery safety net for the engine. When the engine loses inventory state (restart, cache eviction, lost message), the next refresh restores it without an agent restart. The engine MUST treat repeat inventories as idempotent (hash compare → skip DB write/event when unchanged) so the periodic traffic does not amplify into DB load.

**Error publish policy** (must match the team agreement to keep the queue useful):

- Optional tool / feature absent (`ethtool`, `lvs`, `docker`, IMDS timeout, non-root cron read) → **not an error**. Fill `null` / empty array.
- Core source unreadable (`/proc/meminfo`, `/etc/os-release` missing, disk full preventing publish buffer) → **emit error**, single message.
- Inside the publish retry loop → **do not emit**. Emit one summary message after retry exhaustion or on recovery, including `retry_count`, `first_failed_at`, optional `recovered_at`.

`server.error` is for agent-side failures only. Consumer-side failures (parse / DB / TTL) are handled by RabbitMQ DLX → `assessment.dlx` → `server.{type}.dead`. Different alert channels for each.

Common metadata (every message): `message_type`, `machine_id`, `agent_version`, `collected_at` (ISO 8601 UTC), `hostname`, `message_id` (UUID v4), `boot_time` (ISO 8601 UTC, nullable), `agent_started_at` (ISO 8601 UTC).

`machine_id` is the canonical server identifier (read from `/etc/machine-id`). When the file is empty (containers, custom images), the agent falls back to `dbus-uuidgen --get`, then to the cloud IMDS instance-id. `hostname` is captured for human readability but **must not** be used as a primary key by the engine.

`boot_time` and `agent_started_at` carry distinct responsibilities and **must not be confused**:

- `boot_time` — system boot wall-clock, captured once at process start from `/proc/uptime` + `CLOCK_REALTIME` and cached for the process lifetime. Re-reading `/proc/stat`'s `btime` would jitter by 1–2s per NTP correction within a single boot, so caching is mandatory. This field is the **sole authoritative source** for counter-reset detection on the engine. The engine compares `prev.boot_time != curr.boot_time` (change detection only — never absolute-time comparisons like `now() - boot_time < 5min`, which break under clock skew). When changed, the engine MUST skip the differential for that sample (warm-up).
- `agent_started_at` — agent process start wall-clock, captured once at process start. **Observability/debugging only**. The reset-detection path must not key off this field; an agent-only restart leaves kernel counters intact and the differential is still valid.

Full schema and JSON examples: [`docs/payload-schema.md`](../docs/payload-schema.md).

---

## Data Sources

### Static (`inventory` — phases 1 and 2)

| Source | Field | Note |
|--------|-------|------|
| `/etc/machine-id` (+ `dbus-uuidgen` + IMDS fallback) | `machine_id` | Common metadata. Required. |
| `/etc/os-release` | `os_id`, `os_version`, `os_codename` | All systemd-based targets ✅ |
| `uname(2)` | `kernel_version` | All targets ✅ |
| `sysconf(_SC_NPROCESSORS_ONLN)` | `cpu_cores` | All targets ✅ |
| `/proc/cpuinfo` | `cpu_model` | All targets ✅ (x86_64) |
| `/proc/meminfo` | `mem_total_kb`, `swap_total_kb` | Raw kB |
| `lsblk -dn -b -o NAME,SIZE,TYPE -J` (1차) → `/sys/block` 스캔 (폴백) | `disks[]` | lsblk preferred for type info; sysfs fallback for hosts without `lsblk -J` (util-linux < 2.27) |
| `statfs(2)` per fstab entry | `mounts[]` | Raw bytes |
| `getifaddrs(3)` | `ip_internal[]` | All targets ✅ |
| Cloud metadata API (AWS IMDSv2 / Azure / GCP) | `ip_external[]` | Optional. 1s timeout, non-blocking |

> `boot_time` is no longer part of the `inventory` body. It moved to common metadata (every message) for per-metric counter-reset detection — see "Message Types & Routing" above.

### Dynamic (`metrics` — phase 4)

| Source | Field | Note |
|--------|-------|------|
| `/proc/stat` first row | `cpu_stat: { user, nice, system, idle, iowait, irq, softirq, steal }` | Raw cumulative ticks |
| `/proc/meminfo` | `mem_total_kb`, `mem_free_kb`, `mem_available_kb`, `mem_buffers_kb`, `mem_cached_kb`, `swap_total_kb`, `swap_free_kb` | Raw kB |
| `/proc/loadavg` | `load_1m`, `load_5m`, `load_15m` | |
| `/proc/diskstats` (first 14 columns) | `disk_io[].{ device, reads_completed, writes_completed, sectors_read, sectors_written }` | Per device, raw cumulative |
| `statfs(2)` | `mounts[].{ mount, total_bytes, free_bytes, avail_bytes }` | Raw bytes (no `usage_pct` here — engine computes) |
| `/proc/net/dev` | `net_io[].{ interface, rx_bytes, tx_bytes, rx_packets, tx_packets, rx_errors, tx_errors }` | Per interface, raw cumulative. `lo` excluded. |

### Compatibility Notes

| Concern | Handling |
|---|---|
| `MemAvailable` absent on CentOS 7.0~7.1 (kernel < 3.14) | Fall back to `MemFree + Buffers + Cached`. Fallback failure → `mem_available_kb=null`. |
| `/proc/meminfo` line missing / parse failure | The corresponding `*_kb` field is emitted as `null` (not 0) so 실제 0과 read 실패가 구분됨. |
| `lsblk` missing or `-J` unsupported (CentOS 7 util-linux 2.23) | Auto fallback to `/sys/block` directory scan. `type` is always reported as `"disk"` since sysfs does not classify rotational/SSD/LVM. |
| `/proc/diskstats` column count varies by kernel (14 / 18 / 20) | Use the first 14 columns. Trailing columns are ignored. |
| Cumulative counters reset on host reboot | Engine compares with previous row; if `current < previous`, treat the interval as 0 and re-sync. `boot_time` change in inventory marks a series cut. |
| 32-bit counter wrap on long uptime | Same handling: negative delta → skip interval. |
| LVM not installed | `lvs` / `pvs` / `vgs` absent → empty arrays in inventory. Not an error. |
| Cloud IMDS unreachable (on-prem image, network policy) | `ip_external` becomes `null`. Not an error. |
| `sys_vendor` reports legacy "Xen" string | No longer mapped to AWS. Hosts fall through to `/etc/machine-id` and `dbus-uuidgen` for ID resolution; `ip_external` becomes `null`. |

---

## RabbitMQ Topology (Target Production)

```
Exchange: assessment             (direct, durable)
  ├── server.inventory        → queue: server.inventory      (durable, DLX-bound)
  ├── server.metrics          → queue: server.metrics        (durable, DLX-bound, x-message-ttl=72h, x-max-length=1M)
  └── server.error            → queue: server.error          (durable, DLX-bound)

DLX: assessment.dlx              (direct, durable)
  ├── server.inventory        → queue: server.inventory.dead
  ├── server.metrics          → queue: server.metrics.dead
  └── server.error            → queue: server.error.dead

Exchange: assessment.tasks       (direct, durable)
  ├── task.install.<m1>       → queue: agent.tasks.<m1>      (durable, DLX-bound, x-message-ttl=1h, x-max-length=100, x-overflow=reject-publish)
  ├── task.install.<m2>       → queue: agent.tasks.<m2>      (...)
  └── task.result             → queue: worker.result          (portal consumes)

DLX: assessment.tasks.dlx        (direct, durable)
  └── task.install.*          → queue: assessment.tasks.dead (durable, x-message-ttl=7d)
```

The agent publishes to `assessment` (inventory/metrics/error) and `assessment.tasks` (task.result), and consumes from its own `agent.tasks.<machine_id>` queue via `basic_get`. Exchange + DLX declarations are owned by the engine (run once with `topology-admin`). The per-machine `agent.tasks.<machine_id>` queue is declared by the **portal** when it first registers a new machine.

### Permissions Model (least privilege)

| Vhost user | configure | write | read | Used by |
|---|---|---|---|---|
| `agent-publisher` | `^$` | `^assessment$` | `^$` | this agent (collector) |
| `agent-worker` | `^$` | `^assessment\.tasks$` | `^agent\.tasks\..*$` | this agent (worker) — task.result publish + own queue consume |
| `worker-consumer` | `^$` | `^$` | `^server\.(inventory\|metrics\|error)$` | engine consumer |
| `dlq-handler` | `^$` | `^$` | `^server\.(inventory\|metrics\|error)\.dead$` | engine DLQ inspection |
| `portal-task-issuer` | `^agent\.tasks\..*$` | `^assessment\.tasks$` | `^$` | portal — publishes task.install + declares per-machine queue |
| `portal-task-consumer` | `^$` | `^$` | `^worker\.result$` | portal — consumes task.result |
| `topology-admin` | `^(assessment(\.tasks)?(\.dlx)?\|server\..*\|worker\.result\|assessment\.tasks\.dead)$` | `^(assessment(\.tasks)?(\.dlx)?\|server\..*\|worker\.result\|assessment\.tasks\.dead)$` | `^(server\..*\|agent\.tasks\..*\|worker\.result)$` | one-shot bootstrap |
| `monitor-readonly` | `^$` | `^$` | `^$` | management UI (`monitoring` tag) |

The agent ships with two credentials: `agent-publisher` (collector connection) and `agent-worker` (worker connection — CM2 model: separate AMQP connections per role). `agent-worker` is a single shared credential across the fleet, so the read scope is the wildcard `^agent\.tasks\..*$` — a malicious agent could theoretically subscribe to another machine's queue, but machine_ids are UUIDs from `/etc/machine-id` so guessing is impractical. For stricter isolation, fleet provisioning may issue per-machine credentials.

### TLS

- AMQPS port **5671**. Plain 5672 is disabled at the broker.
- TLS 1.2+. Hostname verification on. Internal-CA pem distributed to the agent at deploy time (`/etc/assessment-agent/ca.pem`).
- Optional mTLS (`client.pem` + `client.key` per agent host) — recommended for production.

### Local Development Broker

```bash
sudo docker run -d --name rabbitmq \
  -p 5672:5672 -p 15672:15672 \
  -e RABBITMQ_DEFAULT_USER=admin -e RABBITMQ_DEFAULT_PASS=admin \
  rabbitmq:3-management
```

Local dev uses plain AMQP and the default vhost. Production must not.

---

## Configuration (.env)

| Var | Default | Description |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | broker hostname |
| `RABBITMQ_PORT` | `5671` | AMQPS in production |
| `RABBITMQ_VHOST` | `/assessment` | dedicated vhost (production) |
| `RABBITMQ_USER` | — | `agent-publisher` in production (collector connection) |
| `RABBITMQ_PASS` | — | injected from secret store |
| `RABBITMQ_WORKER_USER` | — | `agent-worker` in production (worker connection). When unset, worker is disabled |
| `RABBITMQ_WORKER_PASS` | — | injected from secret store |
| `RABBITMQ_TLS_ENABLED` | `true` | disable only for local dev |
| `RABBITMQ_TLS_CA_PATH` | `/etc/assessment-agent/ca.pem` | CA pem |
| `RABBITMQ_TLS_VERIFY_PEER` | `true` | |
| `RABBITMQ_TLS_VERIFY_HOSTNAME` | `true` | |
| `RABBITMQ_TLS_CERT_PATH` | — | mTLS client cert (optional) |
| `RABBITMQ_TLS_KEY_PATH` | — | mTLS client key (optional) |
| `RABBITMQ_HEARTBEAT_SEC` | `60` | AMQP heartbeat interval |
| `RABBITMQ_CONFIRM_TIMEOUT_SEC` | `5` | wall-clock deadline for the publisher-confirm ACK |
| `RABBITMQ_EXCHANGE` | `assessment` | direct exchange |
| `RABBITMQ_ROUTING_KEY_INVENTORY` | `server.inventory` | contract — must match engine |
| `RABBITMQ_ROUTING_KEY_METRICS` | `server.metrics` | contract — must match engine |
| `RABBITMQ_ROUTING_KEY_ERROR` | `server.error` | contract — must match engine |
| `AGENT_INTERVAL_SEC` | `60` | `0` means one-shot |
| `AGENT_INVENTORY_REFRESH_SEC` | `3600` | inventory periodic re-publish base interval. Actual delay is `value × (1 + uniform(-0.15, +0.15))`. `0` disables the periodic refresh (startup + change-trigger only) |
| `AGENT_HOSTNAME_OVERRIDE` | — | testing aid |
| `AGENT_EXTERNAL_IP` | — | skip IMDS lookup if set |
| `AGENT_DRAIN_GRACE_SEC` | `600` | on SIGTERM, max seconds to wait for in-flight install to finish before sending SIGTERM to it |
| `AGENT_DRAIN_TERM_SEC` | `30` | additional seconds after SIGTERM before SIGKILL |
| `AGENT_DRAIN_PUBLISH_SEC` | `180` | after install completes, max seconds to wait for result publish before giving up (file replays on next startup). Should exceed `WORKER_RECONNECT_BACKOFF_MAX`(60) × ~3 to allow several reconnect attempts |
| `WORKER_INSTALL_NOFILE` | `4096` | `RLIMIT_NOFILE` (raw fd count) for install.sh. `<= 0` substitutes 4096 |
| `WORKER_DOWNLOAD_ALLOWED_HOSTS` | — | comma-separated hostname whitelist for `task.install` downloads. Case-insensitive exact match (no wildcards). Empty/unset = **all hosts blocked** = worker effectively disabled |
| `WORKER_STATE_DIR` | `/var/lib/agent-worker` | base directory for `/results` (pending publish), `/done` (idempotency markers, 7-day retention) |
| `WORKER_TMP_DIR` | `/tmp` | base directory for extraction sandboxes (`agent-task-<task_id>/`) |
| `WORKER_DONE_RETENTION_SEC` | `604800` | retention for `/done/<task_id>.json` idempotency markers (default 7 days) |
| `WORKER_DISK_RESERVE_MB` | `50` | required free space margin on `WORKER_TMP_DIR` above `download.size_bytes` before download starts |
| `WORKER_INSTALL_MEM_LIMIT_MB` | `2048` | `RLIMIT_AS` applied to `install.sh` (address-space cap) |
| `WORKER_INSTALL_FSIZE_LIMIT_MB` | `5120` | `RLIMIT_FSIZE` applied to `install.sh` (max file size) |
| `WORKER_TASK_QUEUE_PREFIX` | `agent.tasks` | queue name prefix; agent consumes `<prefix>.<machine_id>` |
| `WORKER_TASK_EXCHANGE` | `assessment.tasks` | exchange to which task.result is published |
| `WORKER_TASK_RESULT_KEY` | `task.result` | routing key for task.result |

Shell environment variables override values in `.env`.

---

## Worker (task.install)

The worker consumes `task.install` messages from a per-machine queue, executes a packaged `install.sh`, and publishes a `task.result` summary. It runs inside the same process as the collector — there is no separate worker binary, daemon, or thread.

### Architecture

```
                                                 ┌──────────────────────┐
                                                 │ assessment-portal    │
                                                 │ (publisher / consumer)
                                                 └──────────┬───────────┘
                                                            │ publishes task.install.<machine_id>
                                                            ▼
        ┌──────────────────────[assessment.tasks (direct exchange)]──────────────────────┐
        │                                                                                 │
        │   task.install.<m1> → agent.tasks.<m1>   (TTL 1h, max 100, reject-publish, DLX) │
        │   task.install.<m2> → agent.tasks.<m2>   (...)                                  │
        │   task.result       → worker.result      (portal consumes)                      │
        │                                                                                 │
        └────────────────┬────────────────────────────────────────────────────────────────┘
                         │ basic_get (1-min poll)
                         ▼
            ┌─────────────────────────────────────────────┐
            │  agent parent process (single thread)        │
            │   tick: collect/publish metrics →            │
            │         if idle: basic_get(agent.tasks.<m>)  │
            │         if got task: fork() child            │
            │         reap any finished child →            │
            │           read /var/lib/agent-worker/results/│
            │           publish task.result + ack + move   │
            │             file to /var/lib/agent-worker/done/│
            │         sleep AGENT_INTERVAL_SEC             │
            └────────────────────┬────────────────────────┘
                                 │ fork (M1: one child at a time)
                                 ▼
                ┌────────────────────────────────────────┐
                │ worker child (one task)                 │
                │  1. machine_id sanity check             │
                │  2. /done/<task_id>.json present?       │
                │       yes → write "already_done" result │
                │             file, exit                  │
                │  3. host whitelist + HTTPS download +   │
                │       sha256 streaming + size cap +     │
                │       statvfs disk check                │
                │  4. libarchive extract (TP1 policy)     │
                │  5. fork/execve install.sh under EC1    │
                │     environment, capture 4KB tails      │
                │  6. write /var/lib/agent-worker/results/│
                │       <task_id>.json (success/failure)  │
                │  7. rm -rf /tmp/agent-task-<task_id>/   │
                │  8. _exit(0)                            │
                └─────────────────────────────────────────┘
```

### Design Decisions (see [`docs/worker-task-design.md`](../docs/worker-task-design.md))

| Area | Decision |
|---|---|
| Execution model | **B-f**: single binary, single process, fork-per-task |
| Concurrency | **M1**: at most one in-flight install per host |
| Task pickup | **T1**: `basic_get` polled at each 1-minute tick (no `basic_consume`) |
| Ack timing | **K3**: ack after `install.sh` terminates (success or failure both ack) |
| Result delivery | **R2**: child writes result file; parent reads, publishes, acks, moves to `/done/` |
| Idempotency | **I1**: `/done/<task_id>.json` marker → redelivered tasks publish `already_done` result and skip install |
| SIGTERM | **S2**: drain in-flight install (no new tasks, finish current). systemd `TimeoutStopSec` must accommodate |
| Install user | **U1**: same non-privileged user as the agent process (no setuid, no sudo) |
| install.sh env | **EC1**: `clearenv` + whitelisted PATH/HOME/USER/LANG + TASK_ID/MACHINE_ID, `setrlimit` caps, `setsid`, stdin /dev/null, stdout/stderr captured as 4KB tail, CWD = extraction directory |
| Download | libcurl HTTPS only, **redirects disabled** (RD2), sha256 streaming (OpenSSL EVP), `size_bytes` byte cap, host whitelist (W1) |
| Extract | libarchive, regular files + directories only (no symlink/hardlink/device/FIFO/socket), `mode &= 0777` (strip setuid/setgid/sticky), owner = agent user (TP1) |
| AMQP connections | **CM2**: two separate connections — `agent-publisher` for collector, `agent-worker` for worker (task.result publish + own queue consume) |
| sha256 trust | **P1**: portal-sourced sha256 trusted for now. Portal compromise = arbitrary code execution on every fleet host. Documented + audited |

### Failure / Recovery Matrix

| Scenario | Behavior |
|---|---|
| Child crashes before writing result file | Parent reaps with `WIFSIGNALED`; publishes synthesized `failure_reason: internal_error` result, acks, moves marker to `/done/` |
| Parent crashes between child completion and broker ack | Broker re-delivers task on next reconnect. Parent's startup scans `/var/lib/agent-worker/results/` and publishes any pending file; redelivered task hits `/done/` marker and skips install (I1) |
| Parent SIGTERM while child running | Parent enters drain mode (no new `basic_get`), waits for child reap, publishes result, then exits (S2) |
| Download size exceeds `size_bytes` mid-transfer | libcurl progress callback aborts; result published with `failure_reason: download_failed` |
| sha256 mismatch | Result `failure_reason: sha256_mismatch` |
| tar contains symlink/hardlink/device/setuid file | Result `failure_reason: extract_failed` (entry filtered before extraction begins) |
| `install.sh` exceeds `install.timeout_sec` | Parent (worker child) wall-clock loop sends SIGTERM(grandchild) at deadline, +5s SIGKILL. `RLIMIT_CPU` also caps CPU time. Result `failure_reason: script_timeout` |
| Disk insufficient before download | Result `failure_reason: insufficient_disk` |
| Broker rejects task.result publish (network blip) | `publish_with_retry` (existing collector retry logic) reused. Result file remains in `/results/` until publish succeeds — durable across retries |

### Out of Scope

- Agent self-upgrade via `task.install` — agent binary lives in root-owned `/usr/local/bin`, so user-level install.sh **cannot replace it**. Self-upgrade goes through a separate deploy channel.
- Privileged installs (system packages, systemd unit registration, kernel modules) — blocked by U1 user-level execution. Such installs must be performed out-of-band.
- Multiple concurrent installs on the same host — blocked by M1. The MQ queue handles backpressure.
- Automatic retry of failed downloads — F1: no agent-side retry. Portal re-issues with a new `task_id` if retry is desired.

---

## Work Sequence

| # | Task | Status |
|---|---|---|
| 1 | C reimplementation (rabbitmq-c + cJSON) | ✅ Done |
| 2 | Fault / resilience tests | ✅ Done |
| 3 | Multi-agent run + schema validation | ✅ Done |
| 4 | Migrate to `/proc`-only collectors + 3 message types + direct exchange | ✅ Done |
| 5 | Adopt v2 schema (raw values, `machine_id` common, `cpu_stat` / `disk_io[]` / `net_io[]` / `mounts[]`) | ✅ Done (v2.1: kB nullable + lsblk fallback) |
| 6 | TLS (AMQPS / mTLS) / vhost / agent-publisher credentials env | ✅ Done |
| 7 | Library vendoring (rabbitmq-c, cJSON) — `make USE_VENDORED=1` static build | ✅ Done |
| 8 | Inventory periodic refresh + boot_time/agent_started_at in common metadata | ✅ Done (v3.1) |
| 9 | Worker (task.install) — fork-based consumer, libcurl/libarchive, CM2 dual-connection, idempotency, drain escalation | ✅ Done (v3.2) |
| 10 | Deployment artifacts (systemd unit + .env template with worker drain TimeoutStopSec) | ✅ Done |

---

## Branch Strategy

```
main         # production-ready; no direct push
develop      # integration; merge via PR only
feature/xxx  # feature work
fix/xxx      # bug fixes
chore/xxx    # config / tooling
docs/xxx     # documentation only
```

Workflow: branch from `develop` → PR back to `develop` → 1 approval required.

---

## Commit Convention (description in Korean)

| Type | When |
|---|---|
| `feat` | new feature |
| `fix` | bug fix |
| `chore` | config, dependency, tooling |
| `docs` | documentation only |
| `refactor` | code restructure, no behavior change |
| `test` | add or update tests |
| `style` | formatting only |
