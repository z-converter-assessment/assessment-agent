# Payload Schema 정의

> Agent → RabbitMQ → assessment-engine 간 메시지 규격
> Exchange: `assessment` (**direct**, durable) / Vhost: `/assessment` / Delivery mode: persistent (2)

---

## 변경 이력

- **v2 (2026-04-24 합의)**
  - `machine_id`를 공통 메타데이터로 승격 (모든 메시지 타입에 포함)
  - 에이전트는 raw 값만 전송. delta·비율·IOPS-per-second 등 2차 가공은 분석 엔진이 담당
  - `metrics` 메시지의 모든 환산값을 `/proc` raw 값으로 대체
  - `cpu_user_pct/cpu_system_pct/cpu_iowait_pct` → `cpu_stat` (raw tick 8필드)
  - `mem_used_mb/mem_available_mb/swap_used_mb` → `mem_*_kb` / `swap_*_kb` (raw kB)
  - `disk_read_iops/disk_write_iops/disk_read_bytes/disk_write_bytes` → `disk_io[]` (디바이스별 누적)
  - `net_rx_bytes/net_tx_bytes` → `net_io[]` (인터페이스별 누적)
  - `disk_usage` → `mounts` (이름 충돌 해소, raw bytes)
  - `load_1m` → `load_1m` + `load_5m` + `load_15m`
  - `error.timestamp` 제거 — `collected_at`으로 통일

---

## 공통 메타데이터

모든 메시지 타입(`inventory` / `metrics` / `error`)에 포함된다.

| 필드 | 타입 | 설명 |
|------|------|------|
| `message_type` | string | `"inventory"` / `"metrics"` / `"error"` |
| `machine_id` | string | `/etc/machine-id` (없으면 `dbus-uuidgen --get` → IMDS instance-id fallback). 서버 불변 식별자 |
| `agent_version` | string | 빌드 시 define (예: `"1.0.0"`) |
| `collected_at` | string | ISO 8601 UTC (예: `"2026-04-23T14:30:00Z"`). `error` 메시지에서는 에러 발생 시각 |
| `hostname` | string | 보조 식별자 (운영 중 변경 가능) |
| `message_id` | string | UUID v4 |

> 컨슈머는 **`machine_id`를 기준으로 upsert** 한다. `hostname`은 표시용 보조 정보.

---

## 메시지 타입별 라우팅

| 타입 | routing key | 발송 시점 | 큐 |
|------|-------------|-----------|----|
| `inventory` | `server.inventory` | 에이전트 기동 시 + 정적 정보 변경 감지 시 | `server.inventory` |
| `metrics` | `server.metrics` | 주기적 (기본 60초) | `server.metrics` |
| `error` | `server.error` | 에이전트 측 수집/발행 실패 시 | `server.error` |

DLX(`assessment.dlx`)는 컨슈머 측 NAK / TTL 만료 / `x-max-length` 초과 시 자동 라우팅:
- `server.inventory` → `server.inventory.dead`
- `server.metrics` → `server.metrics.dead`
- `server.error` → `server.error.dead`

`server.error`(에이전트 자가 보고)와 `server.*.dead`(컨슈머 측 처리 실패)는 **별개의 알림 채널**이다.

---

## 1. inventory

에이전트 기동 시, 그리고 정적 정보 변경 감지 시 발송. 잘 변하지 않는 서버 스펙 정보.

### 필드 정의

| 필드 | 타입 | 소스 | 설명 |
|------|------|------|------|
| `os_id` | string | `/etc/os-release` → `ID` | 배포판 식별자 (예: `ubuntu`, `centos`, `amzn`) |
| `os_version` | string | `/etc/os-release` → `VERSION_ID` | 배포판 버전 (예: `22.04`, `7`) |
| `os_codename` | string\|null | `/etc/os-release` → `VERSION_CODENAME` | 코드네임 (예: `jammy`) |
| `kernel_version` | string | `uname(2)` | `release` 필드 (예: `5.15.0-101-generic`) |
| `cpu_cores` | int | `sysconf(_SC_NPROCESSORS_ONLN)` | 온라인 CPU 코어 수 |
| `cpu_model` | string\|null | `/proc/cpuinfo` → `model name` | 트림된 CPU 모델명 |
| `mem_total_kb` | int | `/proc/meminfo` → `MemTotal` | 전체 물리 메모리 (kB) |
| `swap_total_kb` | int | `/proc/meminfo` → `SwapTotal` | 전체 스왑 (kB). 0이면 swap 미설정 |
| `disks` | array | `lsblk -dn -b -o NAME,SIZE,TYPE -J` | 블록 디바이스 목록 |
| `mounts` | array | `statfs(2)` per fstab entry | 마운트별 디스크 사용량 (raw bytes) |
| `ip_internal` | array | `getifaddrs(3)` | 내부 IP 주소 목록 (loopback 제외) |
| `ip_external` | array\|null | 클라우드 메타데이터 API | 외부 IP. 메타데이터 미접근 시 `null` |
| `boot_time` | string | `/proc/uptime` 또는 `stat /proc/1` | ISO 8601 UTC. 시계열 단절 감지에 사용 |

`disks[]` 항목 구조:
```
{ "name": "vda", "size_bytes": 32212254720, "type": "disk" }
```

`mounts[]` 항목 구조 (inventory):
```
{ "mount": "/", "total_bytes": 32212254720, "free_bytes": 19327352832, "avail_bytes": 18253611008, "fstype": "ext4" }
```

### 예시

```json
{
  "message_type":  "inventory",
  "machine_id":    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version": "1.0.0",
  "collected_at":  "2026-04-23T14:30:00Z",
  "hostname":      "web-server-01",
  "message_id":    "550e8400-e29b-41d4-a716-446655440000",

  "os_id":          "ubuntu",
  "os_version":     "22.04",
  "os_codename":    "jammy",
  "kernel_version": "5.15.0-101-generic",
  "cpu_cores":      4,
  "cpu_model":      "Intel(R) Xeon(R) Platinum 8375C CPU @ 2.90GHz",
  "mem_total_kb":   16777216,
  "swap_total_kb":  2097152,
  "disks": [
    { "name": "vda", "size_bytes": 32212254720,  "type": "disk" },
    { "name": "vdb", "size_bytes": 107374182400, "type": "disk" }
  ],
  "mounts": [
    { "mount": "/",     "total_bytes": 32212254720,  "free_bytes": 19327352832, "avail_bytes": 18253611008, "fstype": "ext4" },
    { "mount": "/data", "total_bytes": 107374182400, "free_bytes": 53687091200, "avail_bytes": 53687091200, "fstype": "xfs" }
  ],
  "ip_internal": ["10.0.1.15", "172.16.0.3"],
  "ip_external": ["54.123.45.67"],
  "boot_time":   "2026-04-20T09:12:33Z"
}
```

---

## 2. metrics

기본 60초 주기 수집. 분석 엔진이 두 시점의 차로 delta·% 계산. 에이전트는 `/proc`의 누적 카운터를 그대로 전송한다 (stateless).

### 필드 정의

| 필드 | 타입 | 소스 | 설명 |
|------|------|------|------|
| `cpu_stat` | object | `/proc/stat` 첫 행 | 누적 tick 8필드. 단위 = jiffies |
| `mem_total_kb` | int | `/proc/meminfo` → `MemTotal` | (kB) |
| `mem_free_kb` | int | `/proc/meminfo` → `MemFree` | (kB) |
| `mem_available_kb` | int\|null | `/proc/meminfo` → `MemAvailable` | (kB). 커널 < 3.14 시 `MemFree+Buffers+Cached`로 fallback |
| `mem_buffers_kb` | int | `/proc/meminfo` → `Buffers` | (kB) |
| `mem_cached_kb` | int | `/proc/meminfo` → `Cached` | (kB) |
| `swap_total_kb` | int | `/proc/meminfo` → `SwapTotal` | (kB) |
| `swap_free_kb` | int | `/proc/meminfo` → `SwapFree` | (kB) |
| `load_1m` | float | `/proc/loadavg` | 1분 로드 평균 |
| `load_5m` | float | `/proc/loadavg` | 5분 로드 평균 |
| `load_15m` | float | `/proc/loadavg` | 15분 로드 평균 |
| `disk_io` | array | `/proc/diskstats` (앞 14컬럼) | 디바이스별 누적 카운터 |
| `mounts` | array | `statfs(2)` | 마운트별 raw bytes |
| `net_io` | array | `/proc/net/dev` | 인터페이스별 누적 카운터 (loopback `lo` 제외) |

`cpu_stat` 객체 구조:
```
{
  "user":    int,    "nice":    int,
  "system":  int,    "idle":    int,
  "iowait":  int,    "irq":     int,
  "softirq": int,    "steal":   int
}
```

`disk_io[]` 항목 구조:
```
{
  "device": "vda",
  "reads_completed":  1234567,
  "writes_completed": 456789,
  "sectors_read":     9876543,
  "sectors_written":  4321098
}
```
> `/proc/diskstats` sector = 512 bytes. `bytes_per_sec` 환산은 분석 엔진 책임.

`mounts[]` 항목 구조 (metrics — `usage_pct` 없음):
```
{ "mount": "/", "total_bytes": 32212254720, "free_bytes": 16106127360, "avail_bytes": 15032385536 }
```

`net_io[]` 항목 구조:
```
{
  "interface": "eth0",
  "rx_bytes":   123456789012,  "tx_bytes":   456789012345,
  "rx_packets": 1234567,        "tx_packets": 2345678,
  "rx_errors":  0,              "tx_errors":  0
}
```

### 예시

```json
{
  "message_type":  "metrics",
  "machine_id":    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version": "1.0.0",
  "collected_at":  "2026-04-23T14:31:00Z",
  "hostname":      "web-server-01",
  "message_id":    "550e8400-e29b-41d4-a716-446655440001",

  "cpu_stat": {
    "user":    123456789, "nice":    0,
    "system":  23456789,  "idle":    876543210,
    "iowait":  12345678,  "irq":     0,
    "softirq": 123456,    "steal":   56789
  },
  "mem_total_kb":     16777216,
  "mem_free_kb":      4194304,
  "mem_available_kb": 8388608,
  "mem_buffers_kb":   524288,
  "mem_cached_kb":    3145728,
  "swap_total_kb":    2097152,
  "swap_free_kb":     2097152,

  "load_1m":  1.25,
  "load_5m":  1.10,
  "load_15m": 0.95,

  "disk_io": [
    { "device": "vda",
      "reads_completed":  1234567, "writes_completed": 456789,
      "sectors_read":     9876543, "sectors_written":  4321098 }
  ],
  "mounts": [
    { "mount": "/",     "total_bytes": 32212254720,  "free_bytes": 16106127360, "avail_bytes": 15032385536 },
    { "mount": "/data", "total_bytes": 107374182400, "free_bytes": 53687091200, "avail_bytes": 53687091200 }
  ],
  "net_io": [
    { "interface": "eth0",
      "rx_bytes": 123456789012, "tx_bytes": 456789012345,
      "rx_packets": 1234567,     "tx_packets": 2345678,
      "rx_errors": 0,            "tx_errors": 0 }
  ]
}
```

---

## 3. error

에이전트 측 수집/발행 실패 시 발송. 컨슈머 측 처리 실패는 RabbitMQ DLX로 자동 라우팅되며 이 메시지 타입을 사용하지 않는다.

### 발행 정책

- **선택적 도구·기능 부재는 에러가 아니다.** `ethtool`, `lvs`, `docker`, IMDS 미응답, 비-root 권한으로 인한 cron 미접근 등은 페이로드 필드를 `null` / 빈 배열로 두고 발행하지 않는다.
- **재시도 루프 내부에서는 에러를 발행하지 않는다.** 재시도 종료 또는 복구 시점에 요약 1건만 발행 (`retry_count`, `first_failed_at`, 가능하면 `recovered_at` 포함).
- 핵심 소스 read 실패(`/proc/meminfo` 권한, `/etc/os-release` 부재, 임시 파일 생성 불가 등)는 에러로 발행한다.

### 필드 정의

| 필드 | 타입 | 설명 |
|------|------|------|
| `error_code` | string | 분류 코드 (예: `COLLECT_MEMINFO_FAILED`, `PUBLISH_RETRY_EXHAUSTED`) |
| `error_message` | string | 사람이 읽을 수 있는 상세 |
| `failed_component` | string | `"collect"` 또는 `"publish"` |
| `retry_count` | int\|null | 재시도 요약 시점에만 |
| `first_failed_at` | string\|null | 재시도 요약 시점에만 (ISO 8601) |
| `recovered_at` | string\|null | 복구 보고 시점에만 (ISO 8601) |

### 예시 (수집 실패)

```json
{
  "message_type":  "error",
  "machine_id":    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version": "1.0.0",
  "collected_at":  "2026-04-23T14:31:00.123Z",
  "hostname":      "web-server-01",
  "message_id":    "550e8400-e29b-41d4-a716-446655440002",

  "error_code":       "COLLECT_MEMINFO_FAILED",
  "error_message":    "failed to open /proc/meminfo: Permission denied",
  "failed_component": "collect"
}
```

### 예시 (발행 재시도 누적 후 복구)

```json
{
  "message_type":  "error",
  "machine_id":    "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version": "1.0.0",
  "collected_at":  "2026-04-23T14:36:42Z",
  "hostname":      "web-server-01",
  "message_id":    "550e8400-e29b-41d4-a716-446655440003",

  "error_code":       "PUBLISH_RECOVERED",
  "error_message":    "broker reconnected after 5 retries",
  "failed_component": "publish",
  "retry_count":      5,
  "first_failed_at":  "2026-04-23T14:30:14Z",
  "recovered_at":     "2026-04-23T14:36:42Z"
}
```

---

## 호환성 참고

| 이슈 | 영향 범위 | 대응 |
|------|-----------|------|
| `MemAvailable` 필드 부재 | CentOS 7.0~7.1 (커널 < 3.14) | `MemFree + Buffers + Cached`로 fallback (값은 `mem_available_kb`에 채움) |
| `/proc/diskstats` 컬럼 수 가변 | 커널별 14 / 18 / 20 | 앞 14컬럼만 사용 |
| 재부팅으로 누적 카운터 리셋 | 모든 환경 | 컨슈머에서 `이전값 > 현재값` 시 0 처리 + `boot_time` 변화로 시계열 단절 표시 |
| 32-bit 카운터 wrap | 장기 uptime | 동일하게 음수 delta는 skip |
| `/etc/machine-id` 빈 파일 | 컨테이너 빌드된 이미지 | `dbus-uuidgen --get` → IMDS instance-id fallback |
| `/etc/os-release` 부재 | 구형 CentOS 6 등 (지원 외) | `os_id`, `os_version`, `os_codename`을 모두 `null`로 발행 |

### 타겟 환경

- 아키텍처: x86_64 only
- 최소 커널: 3.10 (CentOS 7)
- 현재 스코프: Ubuntu LTS 우선 (20.04 / 22.04 / 24.04). 그 외 OS는 best-effort + `null`

| 클라우드 | 주요 OS |
|----------|---------|
| AWS EC2 | Amazon Linux 2/2023, Ubuntu 20.04+, RHEL 8+, CentOS 7 |
| Azure VM | Ubuntu 20.04+, RHEL 8+, CentOS 7, Debian 11+ |
| GCP CE | Ubuntu 20.04+, Debian 11+, CentOS 7, RHEL 8+ |

### 수집 소스별 호환성

| 소스 | CentOS 7 | RHEL 8+ | Ubuntu 20.04+ | Amazon Linux 2/2023 | Debian 11+ |
|------|----------|---------|---------------|---------------------|------------|
| `/proc/stat` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/meminfo` | ⚠️ 7.0~7.1 `MemAvailable` 없음 | ✅ | ✅ | ✅ | ✅ |
| `/proc/diskstats` (14컬럼만 사용) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/net/dev` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/loadavg` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/cpuinfo` (x86_64) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/etc/os-release` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/etc/machine-id` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `statfs(2)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `getifaddrs(3)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `uname(2)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sysconf(_SC_NPROCESSORS_ONLN)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `lsblk` (외부 명령) | ✅ | ✅ | ✅ | ✅ | ✅ |
