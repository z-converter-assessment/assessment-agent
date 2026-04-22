# Agent + RabbitMQ — Implementation Context

## Project Position

**Phase 1 — Data Collection / Inventory Acquisition** of the AI-based Assessment service.
The Agent collects resource metrics from customer servers and publishes them to RabbitMQ. The central Assessment Portal (assessment-worker) consumes the queue and stores/analyzes the data.

---

## Network Assumption

- **Internal network only.** The Agent and assessment-worker sit on the same internal network; the Agent reaches the RabbitMQ broker via that path.
- No public internet dependency for core operation.
- `api.ipify.org` external-IP lookup is retained for mixed/edge environments but degrades gracefully to an empty list — never on the critical path.
- **Agent ↔ Worker communication is AMQP only.** No HTTP REST endpoint exists on the worker side.

---

## Implementation

- **Language**: C (`rabbitmq-c` for AMQP, `cJSON` for JSON serialization)
- **Build**: `make` → single `./assessment-agent` binary
- **Execution**: one-shot (`AGENT_INTERVAL_SEC=0`) or loop mode (positive interval with exponential-backoff retry on failure)

### Source Layout

```
src/
  main.c        # env parsing, loop, entry point
  collect.c/.h  # hostname / nproc / mem / lsblk / ip collectors
  publish.c/.h  # rabbitmq-c connection and basic_publish
  util.c/.h     # run_cmd, load_env_file, trim helpers
```

### No-Execution Principle

The Agent only collects and publishes. It must not modify anything on the target server.

---

## Payload Schema

```json
{
  "hostname": "server01",
  "nproc": "4",
  "free": { "mem_total_mb": 16384 },
  "lsblk_raw": [{ "name": "vda", "size": "30G" }],
  "ip_raw": { "internal": ["10.0.0.10"], "external": ["1.2.3.4"] }
}
```

| Field | Method |
|---|---|
| `hostname` | `gethostname(2)` |
| `nproc` | `nproc` |
| `free.mem_total_mb` | parse `free -m` |
| `lsblk_raw` | parse `lsblk --json` |
| `ip_raw.internal` | parse `ip -o -4 addr show` (skip loopback) |
| `ip_raw.external` | `AGENT_EXTERNAL_IP` env var, else `api.ipify.org`; empty array on fully internal hosts |

Any schema change must be coordinated with the **assessment-worker** repository.

---

## RabbitMQ Configuration

```bash
sudo docker run -d --name rabbitmq -p 5672:5672 -p 15672:15672 -e RABBITMQ_DEFAULT_USER=admin -e RABBITMQ_DEFAULT_PASS=admin rabbitmq:3-management
```

| Item | Value |
|---|---|
| Exchange | `assessment` (direct, durable) |
| Queue | `server.metrics` (durable) |
| Routing Key | `metrics` |
| Delivery mode | persistent (2) |

---

## Consumer (assessment-worker)

- Subscribes to `server.metrics` queue
- Parses JSON payload and stores in analysis DB
- Triggers analysis pipeline after storage
- Lives in the `assessment-worker` repository

---

## Work Sequence

| # | Task | Status |
|---|---|---|
| 1 | C reimplementation | ✅ Done |
| 2 | Fault / resilience tests | ✅ Done |
| 3 | Multi-agent tests + schema validation | ✅ Done |
| 4 | Payload metadata fields (`collected_at`, `agent_version`, etc.) | Pending |
| 5 | Deployment (cron / systemd timer unit files) | Pending |
| 6 | OpenStack metadata collection | Pending (lowest priority) |

---

## Branch Strategy

```
main        # production-ready; no direct push
develop     # integration branch; merge via PR only
feature/xxx # feature work
fix/xxx     # bug fixes
chore/xxx   # config / tooling changes
```

Workflow: branch from `develop` → PR back to `develop` → 1 approval required.

---

## Commit Convention

| Type | When to use |
|---|---|
| `feat` | new feature |
| `fix` | bug fix |
| `chore` | config, dependency, tooling |
| `docs` | documentation only |
| `refactor` | code restructure, no behavior change |
| `test` | add or update tests |
| `style` | formatting only |
