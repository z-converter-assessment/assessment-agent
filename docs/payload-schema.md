# Payload Schema 정의

> Agent → RabbitMQ → Worker 간 메시지 규격
> Exchange: `assessment` (topic) / Delivery mode: persistent (2)

---

## 공통 메타데이터

모든 메시지 타입에 포함되는 필드.

| 필드 | 타입 | 설명 |
|------|------|------|
| `message_type` | string | `"inventory"` / `"metrics"` / `"error"` |
| `agent_version` | string | 빌드 시 define (e.g. `"1.0.0"`) |
| `collected_at` | string | ISO 8601 UTC (e.g. `"2026-04-23T14:30:00Z"`) |
| `hostname` | string | 서버 식별자 |
| `message_id` | string | UUID v4 |

---

## 메시지 타입별 라우팅

| 타입 | routing key | 발송 시점 | 설명 |
|------|-------------|-----------|------|
| inventory | `server.inventory` | 에이전트 기동 시 1회 | OS, 커널, CPU 모델, 총 메모리, 디스크 구성 등 정적 정보 |
| metrics | `server.metrics` | 주기적 (1분) | CPU%, 메모리, I/O, 네트워크, 로드평균 등 실시간 성능 |
| error | `server.error` | 수집/발행 실패 시 | 에러 코드, 메시지, 실패 컴포넌트 |

---

## 1. inventory

에이전트 기동 시 1회 발송. 잘 변하지 않는 서버 스펙 정보.

### 필드 정의

| 필드 | 타입 | 소스 | 설명 |
|------|------|------|------|
| `machine_id` | string | `/etc/machine-id` | OS 설치 시 생성되는 고유 ID (서버 불변 식별자) |
| `os_id` | string | `/etc/os-release` → `ID` | 배포판 식별자 (e.g. `ubuntu`, `centos`, `amzn`) |
| `os_version` | string | `/etc/os-release` → `VERSION_ID` | 배포판 버전 (e.g. `22.04`, `7`) |
| `kernel_version` | string | `uname(2)` | `X.Y.Z` 부분만 추출 |
| `cpu_cores` | int | `sysconf(_SC_NPROCESSORS_ONLN)` | 온라인 CPU 코어 수 |
| `cpu_model` | string | `/proc/cpuinfo` → `model name` | CPU 모델명 (x86_64) |
| `mem_total_mb` | int | `/proc/meminfo` → `MemTotal` | 전체 물리 메모리 (MB) |
| `disks` | array | `/proc/partitions` + `/sys/block/*/size` | 블록 디바이스 목록 |
| `disk_usage` | array | `statfs(2)` | 마운트별 디스크 사용량 |
| `ip_internal` | array | `getifaddrs(3)` | 내부 IP 주소 목록 |
| `ip_external` | array | env 또는 cloud metadata API | 외부 IP 주소 목록 (optional) |

### 예시

```json
{
  "message_type": "inventory",
  "agent_version": "1.0.0",
  "collected_at": "2026-04-23T14:30:00Z",
  "hostname": "web-server-01",
  "message_id": "550e8400-e29b-41d4-a716-446655440000",

  "machine_id": "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "os_id": "ubuntu",
  "os_version": "22.04",
  "kernel_version": "5.15.0",
  "cpu_cores": 4,
  "cpu_model": "Intel(R) Xeon(R) Platinum 8375C CPU @ 2.90GHz",
  "mem_total_mb": 16384,
  "disks": [
    { "name": "vda", "size_bytes": 32212254720 },
    { "name": "vdb", "size_bytes": 107374182400 }
  ],
  "disk_usage": [
    { "mount": "/", "total_mb": 30720, "used_mb": 12800, "avail_mb": 17920 },
    { "mount": "/data", "total_mb": 102400, "used_mb": 51200, "avail_mb": 51200 }
  ],
  "ip_internal": ["10.0.1.15", "172.16.0.3"],
  "ip_external": ["54.123.45.67"]
}
```

---

## 2. metrics

1분 주기 수집. 분석 엔진에서 avg / p95 / peak 계산 및 Over/Under Provisioning 판단에 사용.

### 필드 정의

| 필드 | 타입 | 소스 | 설명 |
|------|------|------|------|
| `cpu_user_pct` | float | `/proc/stat` delta | 사용자 공간 CPU 사용률 (%) |
| `cpu_system_pct` | float | `/proc/stat` delta | 커널 공간 CPU 사용률 (%) |
| `cpu_iowait_pct` | float | `/proc/stat` delta | I/O 대기 CPU 비율 (%) — 디스크 병목 지표 |
| `mem_used_mb` | int | `/proc/meminfo` | 사용 중 메모리 (MB) |
| `mem_available_mb` | int | `/proc/meminfo` | 가용 메모리 (MB) — 커널 3.14 미만 시 `MemFree+Buffers+Cached` fallback |
| `swap_used_mb` | int | `/proc/meminfo` | 스왑 사용량 (MB) — 0 초과 시 메모리 부족 신호 |
| `disk_read_iops` | int | `/proc/diskstats` delta | 초당 읽기 I/O 횟수 |
| `disk_write_iops` | int | `/proc/diskstats` delta | 초당 쓰기 I/O 횟수 |
| `disk_read_bytes` | long | `/proc/diskstats` delta | 초당 읽기 바이트 |
| `disk_write_bytes` | long | `/proc/diskstats` delta | 초당 쓰기 바이트 |
| `disk_usage` | array | `statfs(2)` | 마운트별 디스크 사용률 |
| `net_rx_bytes` | long | `/proc/net/dev` delta | 초당 수신 바이트 |
| `net_tx_bytes` | long | `/proc/net/dev` delta | 초당 송신 바이트 |
| `load_1m` | float | `/proc/loadavg` | 1분 로드 평균 |

### 예시

```json
{
  "message_type": "metrics",
  "agent_version": "1.0.0",
  "collected_at": "2026-04-23T14:31:00Z",
  "hostname": "web-server-01",
  "message_id": "550e8400-e29b-41d4-a716-446655440001",

  "cpu_user_pct": 23.5,
  "cpu_system_pct": 4.2,
  "cpu_iowait_pct": 1.8,
  "mem_used_mb": 12288,
  "mem_available_mb": 4096,
  "swap_used_mb": 0,
  "disk_read_iops": 150,
  "disk_write_iops": 85,
  "disk_read_bytes": 6291456,
  "disk_write_bytes": 3145728,
  "disk_usage": [
    { "mount": "/", "usage_pct": 80.0 },
    { "mount": "/data", "usage_pct": 20.0 }
  ],
  "net_rx_bytes": 1048576,
  "net_tx_bytes": 524288,
  "load_1m": 1.25
}
```

---

## 3. error

수집 또는 발행 실패 시 발송. Worker 측에서 알림 처리용.

### 필드 정의

| 필드 | 타입 | 소스 | 설명 |
|------|------|------|------|
| `error_code` | string | 에이전트 내부 | 에러 분류 코드 (e.g. `COLLECT_MEMINFO_FAILED`) |
| `error_message` | string | 에이전트 내부 | 사람이 읽을 수 있는 에러 상세 |
| `failed_component` | string | 에이전트 내부 | `"collect"` 또는 `"publish"` |
| `timestamp` | string | `clock_gettime(2)` | 에러 발생 시각 ISO 8601 (밀리초 포함) |

### 예시

```json
{
  "message_type": "error",
  "agent_version": "1.0.0",
  "collected_at": "2026-04-23T14:31:00Z",
  "hostname": "web-server-01",
  "message_id": "550e8400-e29b-41d4-a716-446655440002",

  "error_code": "COLLECT_MEMINFO_FAILED",
  "error_message": "failed to open /proc/meminfo: Permission denied",
  "failed_component": "collect",
  "timestamp": "2026-04-23T14:31:00.123Z"
}
```

---

## 호환성 참고

| 이슈 | 영향 범위 | 대응 |
|------|-----------|------|
| `MemAvailable` 필드 부재 | CentOS 7.0~7.1 (커널 3.10 초기) | `MemFree + Buffers + Cached`로 fallback |
| `/proc/diskstats` 컬럼 수 가변 | 커널별 14/18/20개 | 앞쪽 14개 필드만 사용 |
| `/etc/os-release` ID 값 | 배포판마다 다름 | `ID` + `VERSION_ID` 조합으로 통일 |

### 타겟 환경

- 아키텍처: x86_64 only
- 최소 커널: 3.10 (CentOS 7)

| 클라우드 | 주요 OS |
|----------|---------|
| AWS EC2 | Amazon Linux 2/2023, Ubuntu 20.04+, RHEL 8+, CentOS 7 |
| Azure VM | Ubuntu 20.04+, RHEL 8+, CentOS 7, Debian 11+ |
| GCP CE | Ubuntu 20.04+, Debian 11+, CentOS 7, RHEL 8+ |

### 수집 소스별 호환성

| 소스 | CentOS 7 | RHEL 8+ | Ubuntu 20.04+ | Amazon Linux 2/2023 | Debian 11+ |
|------|----------|---------|---------------|---------------------|------------|
| `/proc/stat` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/meminfo` | ⚠️ 7.0~7.1 MemAvailable 없음 | ✅ | ✅ | ✅ | ✅ |
| `/proc/diskstats` (14컬럼만 사용) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/net/dev` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/loadavg` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/proc/cpuinfo` (x86_64) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/etc/os-release` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `statfs(2)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `getifaddrs(3)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `uname(2)` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sysconf(_SC_NPROCESSORS_ONLN)` | ✅ | ✅ | ✅ | ✅ | ✅ |
