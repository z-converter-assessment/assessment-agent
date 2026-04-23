# Agent + RabbitMQ — Implementation Context

## Project Position

**Phase 1 — Data Collection / Inventory Acquisition** of the AI-based Assessment service.

전체 서비스: 고객 서버 자원을 자동 수집 → AI 분석(avg/p95/peak, Over/Under Provisioning 판단) → 적정 스펙 추천 → PDF 리포트 자동 생성.
이 에이전트는 **수집 단계**를 담당한다.

---

## Target Environment

- **아키텍처**: x86_64 only
- **최소 커널**: 3.10 (CentOS 7)
- **타겟**: 상용 클라우드(AWS EC2, Azure VM, GCP CE) 위의 Linux VM

| 클라우드 | 주요 OS |
|----------|---------|
| AWS EC2 | Amazon Linux 2/2023, Ubuntu 20.04+, RHEL 8+, CentOS 7 |
| Azure VM | Ubuntu 20.04+, RHEL 8+, CentOS 7, Debian 11+ |
| GCP CE | Ubuntu 20.04+, Debian 11+, CentOS 7, RHEL 8+ |

---

## Network Assumption

- **Internal network only.** Agent와 assessment-worker는 동일 내부망.
- Agent ↔ Worker 통신은 **AMQP only**.
- `api.ipify.org` 외부 IP 조회는 혼합/예외 환경 대비용. 실패 시 빈 배열 — critical path 아님.

---

## Implementation

- **Language**: C (`rabbitmq-c` for AMQP, `cJSON` for JSON serialization)
- **Build**: `make` → single `./assessment-agent` binary
- **Execution**: one-shot (`AGENT_INTERVAL_SEC=0`) or loop mode (positive interval with exponential-backoff retry on failure)
- **Data collection**: `/proc`, `/sys`, syscall 기반. 외부 명령어(`nproc`, `free`, `lsblk`, `ip`) 의존 제거 예정.

### Source Layout

```
src/
  main.c        # env parsing, loop, entry point
  collect.c/.h  # /proc, /sys, syscall 기반 수집기
  publish.c/.h  # rabbitmq-c connection and basic_publish
  util.c/.h     # run_cmd, load_env_file, trim helpers
docs/
  payload-schema.md  # 메시지 스키마 상세 정의
```

### No-Execution Principle

The Agent only collects and publishes. It must not modify anything on the target server.

---

## Message Types & Routing

Exchange: `assessment` (topic, durable) / Delivery mode: persistent (2)

| Type | Routing Key | Trigger | Description |
|------|-------------|---------|-------------|
| `inventory` | `server.inventory` | 에이전트 기동 시 1회 | 서버 정적 정보 (OS, CPU, 메모리, 디스크 구성) |
| `metrics` | `server.metrics` | 주기적 (1분) | CPU%, 메모리, I/O, 네트워크, 로드평균 |
| `error` | `server.error` | 수집/발행 실패 시 | 에러 코드, 메시지, 실패 컴포넌트 |

공통 메타데이터 (모든 메시지): `message_type`, `agent_version`, `collected_at` (ISO 8601), `hostname`, `message_id` (UUID)

상세 스키마 및 JSON 예시: [`docs/payload-schema.md`](../docs/payload-schema.md)

---

## Data Sources & Compatibility

| Source | Purpose | Compatibility Note |
|--------|---------|-------------------|
| `/proc/stat` | CPU user/system/iowait (delta) | All targets ✅ |
| `/proc/meminfo` | Memory total/used/available, swap | CentOS 7.0~7.1: `MemAvailable` fallback 필요 |
| `/proc/diskstats` | Disk IOPS/throughput (delta) | 커널별 14/18/20 컬럼 → 앞 14개만 사용 |
| `/proc/partitions` + `/sys/block/*/size` | Disk inventory | All targets ✅ |
| `/proc/net/dev` | Network rx/tx (delta) | All targets ✅ |
| `/proc/cpuinfo` | CPU model (x86_64) | All targets ✅ |
| `/proc/loadavg` | Load average | All targets ✅ |
| `/etc/os-release` | OS id/version | `ID` 값 배포판마다 다름 |
| `/etc/machine-id` | Server unique ID | All systemd-based targets ✅ |
| `statfs(2)` | Disk usage per mount | All targets ✅ |
| `getifaddrs(3)` | Internal IPs | All targets ✅ |
| `uname(2)` | Kernel version | All targets ✅ |
| `sysconf(_SC_NPROCESSORS_ONLN)` | CPU core count | All targets ✅ |

---

## RabbitMQ Configuration

```bash
sudo docker run -d --name rabbitmq -p 5672:5672 -p 15672:15672 -e RABBITMQ_DEFAULT_USER=admin -e RABBITMQ_DEFAULT_PASS=admin rabbitmq:3-management
```

| Item | Value |
|---|---|
| Exchange | `assessment` (topic, durable) |
| Queues | `server.inventory`, `server.metrics`, `server.error` (all durable) |
| Routing Keys | `server.inventory`, `server.metrics`, `server.error` |
| Delivery mode | persistent (2) |

---

## Consumer (assessment-worker)

- Subscribes to `server.inventory`, `server.metrics`, `server.error` queues
- inventory → DB에 서버 등록
- metrics → 시계열 저장 → avg/p95/peak 분석 → Over/Under Provisioning 판단
- error → 알림 처리
- Lives in the `assessment-worker` repository

---

## Work Sequence

| # | Task | Status |
|---|---|---|
| 1 | C reimplementation | ✅ Done |
| 2 | Fault / resilience tests | ✅ Done |
| 3 | Multi-agent tests + schema validation | ✅ Done |
| 4 | /proc 기반 수집 전환 + 3종 메시지 타입 + topic exchange | Pending |
| 5 | Library vendoring (rabbitmq-c, cJSON) | Pending |
| 6 | Deployment (cron / systemd timer unit files) | Pending |

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
