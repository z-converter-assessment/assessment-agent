# Assessment Agent — Implementation Context

## Project Position

**Phase 1 — Data Collection / Inventory Acquisition** of the AI-based Assessment service.

The full service collects customer-server resources, runs AI analysis (avg / p95 / peak, over/under-provisioning detection) and produces sizing recommendations as a PDF report. **This agent owns the collection step.**

Pipeline: `agent (this repo) → RabbitMQ → assessment-engine (consumer + web)`

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
- **Build**: `make` → single `./assessment-agent` binary
- **Execution mode**:
  - one-shot (`AGENT_INTERVAL_SEC=0`) — for cron / systemd timer
  - loop mode (positive interval) — single long-running process with exponential backoff on publish failure

### Source Layout

```
src/
  main.c        # env parsing, collection/publish loop, entry point
  collect.c/.h  # /proc, /sys, syscall-based collectors
  publish.c/.h  # rabbitmq-c connection, TLS, basic_publish
  util.c/.h     # env file loader, string helpers
include/        # public headers for the source files above
docs/
  payload-schema.md   # message schema (canonical contract with the engine)
tests/
  test_schema.sh      # smoke test — single-publish payload validation
  run_multi.sh        # N agents publishing in parallel
  fault_agent.sh      # SIGKILL resilience
  fault_rabbitmq.sh   # broker restart resilience
  run_all.sh
scripts/
  rabbitmq-up.sh      # local dev broker (Docker)
  rabbitmq-down.sh
```

### Core Principles

1. **No-execution.** The agent only reads. It must never modify anything on the target host (no writes outside its own log file, no package operations, no service control).
2. **Stateless.** The agent does not retain previous tick / counter values across cycles. It emits raw cumulative counters from `/proc`. All delta, percentage and rate computations happen in the engine.
3. **Raw values only.** No `avg`, no `pct`, no `iops_per_second`. The 1-minute metric message is a snapshot of `/proc` raw fields.
4. **Single normalized schema.** OS-specific command differences are absorbed inside the agent (collector branches by `os_id` / kernel). The wire format is **identical** regardless of OS.
5. **Optional features are not errors.** Missing `ethtool`, `lvs`, `docker`, etc. produce `null` / empty arrays. They do not generate `server.error` messages.

---

## Message Types & Routing

Exchange: `assessment` (**direct**, durable). Delivery mode: persistent (2). Vhost: `/assessment`.

| Type | Routing key | Trigger | Description |
|------|-------------|---------|-------------|
| `inventory` | `server.inventory` | startup + on detected static change | OS, kernel, CPU model, total memory, disks, mounts, IPs |
| `metrics`   | `server.metrics`   | every `AGENT_INTERVAL_SEC` (default 60) | raw `/proc` counters: `cpu_stat`, `mem_*_kb`, `swap_*_kb`, `load_{1,5,15}m`, `disk_io[]`, `net_io[]`, `mounts[]` |
| `error`     | `server.error`     | collection / publish failure (see policy below) | `error_code`, `error_message`, `failed_component` |

**Error publish policy** (must match the team agreement to keep the queue useful):

- Optional tool / feature absent (`ethtool`, `lvs`, `docker`, IMDS timeout, non-root cron read) → **not an error**. Fill `null` / empty array.
- Core source unreadable (`/proc/meminfo`, `/etc/os-release` missing, disk full preventing publish buffer) → **emit error**, single message.
- Inside the publish retry loop → **do not emit**. Emit one summary message after retry exhaustion or on recovery, including `retry_count`, `first_failed_at`, optional `recovered_at`.

`server.error` is for agent-side failures only. Consumer-side failures (parse / DB / TTL) are handled by RabbitMQ DLX → `assessment.dlx` → `server.{type}.dead`. Different alert channels for each.

Common metadata (every message): `message_type`, `machine_id`, `agent_version`, `collected_at` (ISO 8601 UTC), `hostname`, `message_id` (UUID v4).

`machine_id` is the canonical server identifier (read from `/etc/machine-id`). When the file is empty (containers, custom images), the agent falls back to `dbus-uuidgen --get`, then to the cloud IMDS instance-id. `hostname` is captured for human readability but **must not** be used as a primary key by the engine.

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
| `lsblk -dn -b -o NAME,SIZE,TYPE -J` | `disks[]` | External command (no `/proc` equivalent for disk listing) |
| `statfs(2)` per fstab entry | `mounts[]` | Raw bytes |
| `getifaddrs(3)` | `ip_internal[]` | All targets ✅ |
| Cloud metadata API (AWS IMDSv2 / Azure / GCP) | `ip_external[]` | Optional. 1s timeout, non-blocking |
| `/proc/uptime` or `stat /proc/1` | `boot_time` | For timeline reconstruction |

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
| `MemAvailable` absent on CentOS 7.0~7.1 (kernel < 3.14) | Fall back to `MemFree + Buffers + Cached`. Document the fallback in the kB field. |
| `/proc/diskstats` column count varies by kernel (14 / 18 / 20) | Use the first 14 columns. Trailing columns are ignored. |
| Cumulative counters reset on host reboot | Engine compares with previous row; if `current < previous`, treat the interval as 0 and re-sync. `boot_time` change in inventory marks a series cut. |
| 32-bit counter wrap on long uptime | Same handling: negative delta → skip interval. |
| LVM not installed | `lvs` / `pvs` / `vgs` absent → empty arrays in inventory. Not an error. |
| Cloud IMDS unreachable (on-prem image, network policy) | `ip_external` becomes `null`. Not an error. |

---

## RabbitMQ Topology (Target Production)

```
Exchange: assessment       (direct, durable)
  ├── server.inventory  → queue: server.inventory      (durable, DLX-bound)
  ├── server.metrics    → queue: server.metrics        (durable, DLX-bound, x-message-ttl=72h, x-max-length=1M)
  └── server.error      → queue: server.error          (durable, DLX-bound)

DLX: assessment.dlx        (direct, durable)
  ├── server.inventory  → queue: server.inventory.dead
  ├── server.metrics    → queue: server.metrics.dead
  └── server.error      → queue: server.error.dead
```

The agent only publishes. Queue / exchange / DLX declaration is owned by the engine (run once with the `topology-admin` user).

### Permissions Model (least privilege)

| Vhost user | configure | write | read | Used by |
|---|---|---|---|---|
| `agent-publisher` | `^$` | `^assessment$` | `^$` | this agent |
| `worker-consumer` | `^$` | `^$` | `^server\.(inventory\|metrics\|error)$` | engine consumer |
| `dlq-handler` | `^$` | `^$` | `^server\.(inventory\|metrics\|error)\.dead$` | engine DLQ inspection |
| `topology-admin` | `^(assessment(\.dlx)?\|server\..*)$` | `^(assessment(\.dlx)?\|server\..*)$` | `^server\..*$` | one-shot bootstrap |
| `monitor-readonly` | `^$` | `^$` | `^$` | management UI (`monitoring` tag) |

The agent ships with `agent-publisher` credentials only. Compromise scope is bounded to publishing arbitrary payloads — it cannot consume or reconfigure topology.

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
| `RABBITMQ_USER` | — | `agent-publisher` in production |
| `RABBITMQ_PASS` | — | injected from secret store |
| `RABBITMQ_TLS_ENABLED` | `true` | disable only for local dev |
| `RABBITMQ_TLS_CA_PATH` | `/etc/assessment-agent/ca.pem` | CA pem |
| `RABBITMQ_TLS_VERIFY_PEER` | `true` | |
| `RABBITMQ_TLS_VERIFY_HOSTNAME` | `true` | |
| `RABBITMQ_TLS_CERT_PATH` | — | mTLS client cert (optional) |
| `RABBITMQ_TLS_KEY_PATH` | — | mTLS client key (optional) |
| `RABBITMQ_EXCHANGE` | `assessment` | direct exchange |
| `RABBITMQ_ROUTING_KEY_INVENTORY` | `server.inventory` | contract — must match engine |
| `RABBITMQ_ROUTING_KEY_METRICS` | `server.metrics` | contract — must match engine |
| `RABBITMQ_ROUTING_KEY_ERROR` | `server.error` | contract — must match engine |
| `AGENT_INTERVAL_SEC` | `60` | `0` means one-shot |
| `AGENT_HOSTNAME_OVERRIDE` | — | testing aid |
| `AGENT_EXTERNAL_IP` | — | skip IMDS lookup if set |

Shell environment variables override values in `.env`.

---

## Work Sequence

| # | Task | Status |
|---|---|---|
| 1 | C reimplementation (rabbitmq-c + cJSON) | ✅ Done |
| 2 | Fault / resilience tests | ✅ Done |
| 3 | Multi-agent run + schema validation | ✅ Done |
| 4 | Migrate to `/proc`-only collectors + 3 message types + direct exchange | In progress |
| 5 | Adopt v2 schema (raw values, `machine_id` common, `cpu_stat` / `disk_io[]` / `net_io[]` / `mounts[]`) | In progress |
| 6 | TLS / vhost / 5-user permission model | Pending |
| 7 | Library vendoring (rabbitmq-c, cJSON) | Pending |
| 8 | Deployment artifacts (cron + systemd timer) | Pending |

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
