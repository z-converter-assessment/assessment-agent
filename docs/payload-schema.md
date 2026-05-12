# Payload Schema 정의

> Agent → RabbitMQ → assessment-engine 간 메시지 규격
> Exchange: `assessment` (**direct**, durable) / Vhost: `/assessment` / Delivery mode: persistent (2)

---

## 변경 이력

- **v3.2 (2026-05-11)**
  - **Worker (task.install) 도입**. agent가 portal로부터 `task.install` 메시지를 받아 HTTPS tar 다운로드 → 압축 해제 → user-level `install.sh` 실행 → `task.result` 회신
  - 신규 exchange `assessment.tasks` (direct) + 머신별 큐 `agent.tasks.<machine_id>` + DLX `assessment.tasks.dlx` → `assessment.tasks.dead`
  - 신규 권한 사용자: `agent-worker` (agent용), `portal-task-issuer` (portal 발행 + 머신별 큐 declare), `portal-task-consumer` (portal result consume)
  - CM2 모델 — agent는 두 connection 사용 (collector용 `agent-publisher` + worker용 `agent-worker`)
  - "No-Execution" 원칙 폐기. 단, 실행은 비특권 user + clearenv + setrlimit + setsid + sandbox extraction directory로 격리
- **v3.1 (2026-05-11)**
  - 공통 메타데이터에 `boot_time`, `agent_started_at` 추가 — 모든 메시지에 실림. 카운터 리셋 감지를 매 metric 단위로 가능하게 함
  - `boot_time`은 프로세스 시작 시 1회 캐시 (`/proc/uptime` + `CLOCK_REALTIME` 합성). NTP 보정으로 인한 매 read 흔들림 제거. **차분 로직의 유일한 권위 소스**
  - `agent_started_at`은 프로세스 시작 시각. 관측·디버깅용 — 차분 로직에서 사용 금지
  - `inventory`에서 `boot_time` 필드 제거 (공통 메타로 이전)
  - `inventory` 발행 시점에 **1시간 ±15% jitter 주기 재발행** 추가. 환경변수 `AGENT_INVENTORY_REFRESH_SEC` (기본 3600, 0=비활성)
- **v3 (2026-05-06 제안 — 팀 합의 대기)**
  - `inventory`에 `services[]` 추가 — systemd 활성 unit 목록 (서버 식별 용도)
  - `inventory`에 `listen_ports[]` 추가 — TCP listen + UDP 비-connected 소켓 + 매핑 프로세스 (서버 식별 용도). proto 값은 `tcp` / `tcp6` / `udp` / `udp6`
  - `inventory.disks[]` 필터링 정책 명문화 — `loop` / `ram` / `sr` / `fd` 디바이스 제외 (lsblk·`/sys/block` 폴백 양쪽에 동일 적용)
  - `inventory.mounts[]` / `metrics.mounts[]` dedup 정책 명문화 — `/proc/self/mountinfo`의 `(major:minor)`로 bind mount 중복 제거. 첫 등장 마운트포인트만 유지
  - "필터링 정책" 섹션 신설 — agent(OS 무관 노이즈 제거) ↔ engine(분석 의도 제외) 책임 분리 명시
  - 서비스/포트 raw 값은 OS별로 이름이 다름(`httpd`↔`apache2`, `mysqld`↔`mysql`↔`mariadb`). **정규화는 engine 책임** — agent는 raw unit name·port·comm을 그대로 emit
- **v2.1 (2026-04-28)**
  - `metrics`의 raw kB 필드(`mem_free_kb`, `mem_buffers_kb`, `mem_cached_kb`, `swap_total_kb`, `swap_free_kb`)와 `inventory.swap_total_kb`가 소스 read 실패 시 `null`로 발행됨. 0(실제 값) 과 "못 읽음" 의 모호성 제거
  - `disks[]` 수집이 `lsblk -J` 미설치/미지원(util-linux < 2.27, 예: CentOS 7) 시 `/sys/block` 스캔으로 자동 폴백. 폴백 사용 시 `type`은 항상 `"disk"`
  - `wait_confirm` ACK 대기가 wall-clock deadline 으로 통일되며 `RABBITMQ_CONFIRM_TIMEOUT_SEC`(기본 5초)로 캡됨
  - `detect_cloud_vendor`가 더 이상 `sys_vendor=Xen`을 AWS로 매핑하지 않음. 모던 Nitro는 `Amazon EC2` 매칭
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
| `boot_time` | string\|null | ISO 8601 UTC. 시스템 부팅 시각. 프로세스 시작 시 `/proc/uptime` + `CLOCK_REALTIME`으로 1회 캐시(NTP 보정 흔들림 제거). **카운터 리셋 판정의 유일한 권위 소스** — 변경 감지(`prev != curr`)만 사용, 절대값 비교 금지. `/proc/uptime` 미접근 시 `null`. `task.result`는 항상 `null` (worker가 collect.c 캐시와 분리됨) |
| `agent_started_at` | string\|null | ISO 8601 UTC. agent 프로세스 기동 시각. 프로세스 시작 시 1회 캡처. **관측·디버깅용** — 차분/리셋 로직에서 사용 금지. `task.result`는 항상 `null` (위와 동일 사유) |

> 컨슈머는 **`machine_id`를 기준으로 upsert** 한다. `hostname`은 표시용 보조 정보.
>
> `boot_time` ↔ `agent_started_at` 책임 분리:
> - 두 메시지 간 `boot_time` 변경 → 호스트 재부팅. 누적 카운터 리셋 → **차분 스킵 (warm-up 1샘플)**
> - 두 메시지 간 `agent_started_at`만 변경 → agent만 재시작. 커널 카운터는 그대로 → **차분 그대로 OK**
> - 절대값 비교(`now() - boot_time < 5min` 등)는 서버·agent 시계 어긋남에 취약하므로 금지

---

## 메시지 타입별 라우팅

| 타입 | routing key | 발송 시점 | 큐 |
|------|-------------|-----------|----|
| `inventory` | `server.inventory` | 에이전트 기동 시 + 정적 정보 변경 감지 시 + **주기 재발행 (`AGENT_INVENTORY_REFRESH_SEC` ±15% jitter, 기본 3600s, 0=비활성)** | `server.inventory` |
| `metrics` | `server.metrics` | 주기적 (기본 60초) | `server.metrics` |
| `error` | `server.error` | 에이전트 측 수집/발행 실패 시 | `server.error` |

> **주기 재발행 idempotency**: 동일 인벤토리가 1시간마다 도착하므로 컨슈머는 내용 해시(또는 변경 컬럼) 비교 후 변화가 없으면 DB write·이벤트 발행을 생략해야 한다. 그렇지 않으면 인벤토리 재발행이 곧 DB 부하 증가로 이어진다.

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
| `swap_total_kb` | int\|null | `/proc/meminfo` → `SwapTotal` | 전체 스왑 (kB). 0=swap 미설정, `null`=`/proc/meminfo` read 실패 |
| `disks` | array | `lsblk -dn -b -o NAME,MAJ:MIN,SIZE,TYPE -J` (1차) → `/sys/block` 스캔 (폴백) | 블록 디바이스 목록. `loop`/`ram`/`sr`/`fd` 제외. 폴백 시 `type="disk"` 고정 |
| `mounts` | array | `/proc/self/mountinfo` + `statfs(2)` | 마운트별 디스크 사용량 (raw bytes). 가상 fstype 제외 + `(major:minor)`로 bind mount dedup |
| `services` | array\|null | `systemctl list-units --type=service --state=running --no-pager --plain --no-legend` | 활성 systemd 서비스 unit. systemd 미존재(systemctl 부재·exit≠0)는 **`null` 발행** — 빈 배열(서비스 0개)과 명시적으로 구분. unit 이름은 OS별로 다르며 정규화는 엔진 책임 |
| `listen_ports` | array | `/proc/net/tcp{,6}` (state=`0A` LISTEN) + `/proc/net/udp{,6}` (no peer) + `/proc/<pid>/fd/*` 소켓 inode 매칭 | TCP listen + UDP 비-connected 소켓. `pid`/`comm`은 권한·프로세스 종료로 미해상 시 `null` |
| `ip_internal` | array | `getifaddrs(3)` | 내부 IP 주소 목록 (loopback 제외) |
| `ip_external` | array\|null | 클라우드 메타데이터 API | 외부 IP. 메타데이터 미접근 시 `null` |

> v3.1부터 `boot_time`은 inventory 본문이 아니라 **공통 메타데이터**에 실린다 (모든 메시지에 동일하게).

`disks[]` 항목 구조:
```
{ "name": "vda", "major": 252, "minor": 0, "size_bytes": 32212254720, "type": "disk" }
```
> `major`/`minor`는 mounts dedup·diskstats 매칭의 기준 키. lsblk `MAJ:MIN` 컬럼 또는 `/sys/block/<name>/dev`에서 추출.

`mounts[]` 항목 구조 (inventory):
```
{ "mount": "/", "major": 252, "minor": 1, "total_bytes": 32212254720, "free_bytes": 19327352832, "avail_bytes": 18253611008, "fstype": "ext4" }
```

`services[]` 항목 구조:
```
{ "unit": "nginx.service", "sub": "running" }
```
> `unit`은 raw 그대로 emit. 분류·정규화(`web`/`db`/`cache` 등)는 엔진 책임. `sub` 추가 필드 여지(예: `since`, `cgroup`)를 위해 객체 형태 유지.

`listen_ports[]` 항목 구조:
```
{ "proto": "tcp", "addr": "0.0.0.0", "port": 80, "uid": 33, "pid": 1234, "comm": "nginx" }
```
> `proto`는 `"tcp"` / `"tcp6"` / `"udp"` / `"udp6"`. `addr`은 dotted-quad(v4) 또는 RFC 5952 압축 표기(v6). `pid`/`comm`은 unprivileged read 권한이 부족하거나 매칭 inode 소유 프로세스가 종료된 경우 `null`. `uid`는 항상 채움(소켓 owner — 보조 식별 신호).
>
> **TCP**는 state가 `0A`(LISTEN)인 소켓만. **UDP**는 LISTEN 상태가 없으므로 remote endpoint가 `0:0`(connect()되지 않은 소켓)인 항목을 emit한다 — 바인딩된 서버 소켓과 ephemeral 클라이언트 소켓이 함께 들어올 수 있으므로 엔진에서 well-known 포트(<1024)나 등록 포트(<32768) 기준으로 후처리 권장.

### 예시

```json
{
  "message_type":     "inventory",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T14:30:00Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440000",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

  "os_id":          "ubuntu",
  "os_version":     "22.04",
  "os_codename":    "jammy",
  "kernel_version": "5.15.0-101-generic",
  "cpu_cores":      4,
  "cpu_model":      "Intel(R) Xeon(R) Platinum 8375C CPU @ 2.90GHz",
  "mem_total_kb":   16777216,
  "swap_total_kb":  2097152,
  "disks": [
    { "name": "vda", "major": 252, "minor": 0,  "size_bytes": 32212254720,  "type": "disk" },
    { "name": "vdb", "major": 252, "minor": 16, "size_bytes": 107374182400, "type": "disk" }
  ],
  "mounts": [
    { "mount": "/",     "major": 252, "minor": 1,  "total_bytes": 32212254720,  "free_bytes": 19327352832, "avail_bytes": 18253611008, "fstype": "ext4" },
    { "mount": "/data", "major": 252, "minor": 16, "total_bytes": 107374182400, "free_bytes": 53687091200, "avail_bytes": 53687091200, "fstype": "xfs" }
  ],
  "services": [
    { "unit": "ssh.service",        "sub": "running" },
    { "unit": "nginx.service",      "sub": "running" },
    { "unit": "postgresql.service", "sub": "running" }
  ],
  "listen_ports": [
    { "proto": "tcp",  "addr": "0.0.0.0", "port": 22,   "uid": 0,   "pid": 901,  "comm": "sshd" },
    { "proto": "tcp",  "addr": "0.0.0.0", "port": 80,   "uid": 33,  "pid": 1234, "comm": "nginx" },
    { "proto": "tcp6", "addr": "::",      "port": 5432, "uid": 113, "pid": 1789, "comm": "postgres" },
    { "proto": "udp",  "addr": "0.0.0.0", "port": 53,   "uid": 101, "pid": 612,  "comm": "systemd-resolve" },
    { "proto": "udp",  "addr": "0.0.0.0", "port": 123,  "uid": 0,   "pid": 555,  "comm": "ntpd" }
  ],
  "ip_internal": ["10.0.1.15", "172.16.0.3"],
  "ip_external": ["54.123.45.67"]
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
| `mem_free_kb` | int\|null | `/proc/meminfo` → `MemFree` | (kB). `null`=라인 부재/파싱 실패 |
| `mem_available_kb` | int\|null | `/proc/meminfo` → `MemAvailable` | (kB). 커널 < 3.14 시 `MemFree+Buffers+Cached`로 fallback. fallback도 실패하면 `null` |
| `mem_buffers_kb` | int\|null | `/proc/meminfo` → `Buffers` | (kB). `null`=라인 부재/파싱 실패 |
| `mem_cached_kb` | int\|null | `/proc/meminfo` → `Cached` | (kB). `null`=라인 부재/파싱 실패 |
| `swap_total_kb` | int\|null | `/proc/meminfo` → `SwapTotal` | (kB). 0=swap 미설정, `null`=read 실패 |
| `swap_free_kb` | int\|null | `/proc/meminfo` → `SwapFree` | (kB). `null`=read 실패 |
| `load_1m` | float | `/proc/loadavg` | 1분 로드 평균 |
| `load_5m` | float | `/proc/loadavg` | 5분 로드 평균 |
| `load_15m` | float | `/proc/loadavg` | 15분 로드 평균 |
| `disk_io` | array | `/proc/diskstats` (앞 14컬럼) | 디바이스별 누적 카운터. `loop`/`ram`/`sr`/`fd` 제외 |
| `mounts` | array | `/proc/self/mountinfo` + `statfs(2)` | 마운트별 raw bytes. 가상 fstype 제외 + `(major:minor)`로 dedup |
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
  "major": 252, "minor": 0,
  "reads_completed":  1234567,
  "writes_completed": 456789,
  "sectors_read":     9876543,
  "sectors_written":  4321098
}
```
> `/proc/diskstats` sector = 512 bytes. `bytes_per_sec` 환산은 분석 엔진 책임.
> `(major:minor)`는 inventory `disks[]` / `mounts[]`와의 조인 키.

`mounts[]` 항목 구조 (metrics — `usage_pct`·`fstype` 없음):
```
{ "mount": "/", "major": 252, "minor": 1, "total_bytes": 32212254720, "free_bytes": 16106127360, "avail_bytes": 15032385536 }
```
> `(major:minor)`는 inventory `disks[]`/`mounts[]`와의 조인 키. fstype은 inventory에만 둠 (메트릭 주기마다 변하지 않음).

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
  "message_type":     "metrics",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T14:31:00Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440001",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

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
    { "device": "vda", "major": 252, "minor": 0,
      "reads_completed":  1234567, "writes_completed": 456789,
      "sectors_read":     9876543, "sectors_written":  4321098 }
  ],
  "mounts": [
    { "mount": "/",     "major": 252, "minor": 1,  "total_bytes": 32212254720,  "free_bytes": 16106127360, "avail_bytes": 15032385536 },
    { "mount": "/data", "major": 252, "minor": 16, "total_bytes": 107374182400, "free_bytes": 53687091200, "avail_bytes": 53687091200 }
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
  "message_type":     "error",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T14:31:00.123Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440002",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

  "error_code":       "COLLECT_MEMINFO_FAILED",
  "error_message":    "failed to open /proc/meminfo: Permission denied",
  "failed_component": "collect"
}
```

### 예시 (발행 재시도 누적 후 복구)

```json
{
  "message_type":     "error",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T14:36:42Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440003",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

  "error_code":       "PUBLISH_RECOVERED",
  "error_message":    "broker reconnected after 5 retries",
  "failed_component": "publish",
  "retry_count":      5,
  "first_failed_at":  "2026-04-23T14:30:14Z",
  "recovered_at":     "2026-04-23T14:36:42Z"
}
```

---

## 4. task.install (수신: portal → agent)

portal이 발행, agent가 consume. agent는 user-level `install.sh`를 실행하고 `task.result`로 회신.

### 라우팅

- Exchange: `assessment.tasks` (direct, durable)
- Routing key: `task.install.<target_machine_id>` — broker가 정확히 해당 머신의 큐로만 배달
- Queue: `agent.tasks.<machine_id>` (durable, `x-message-ttl=3600000` 1h, `x-max-length=100`, `x-overflow=reject-publish`, DLX bound)

> 머신별 큐는 portal이 머신 등록(첫 inventory 수신) 시점에 declare한다. agent는 declare 권한 없이 consume만.

### 필드 정의

| 필드 | 타입 | 의미 |
|---|---|---|
| `message_type` | string | 고정 `"task.install"` |
| `task_id` | string (UUID v4) | 작업 고유 ID. 결과 회신·중복 검출·로그 추적 키 |
| `machine_id` | string | 타겟 머신. agent는 자기 `/etc/machine-id`와 비교하여 불일치 시 ack drop (정상 라우팅이면 발생하지 않음) |
| `issued_at` | string (ISO 8601 UTC) | 발행 시각. **agent 측 절대값 검증 없음** (broker TTL이 만료 처리) — 운영 로깅/디버깅용 |
| `download.url` | string | HTTPS URL. host는 `WORKER_DOWNLOAD_ALLOWED_HOSTS` 화이트리스트와 case-insensitive 정확 매치 필요. 30x redirect 비활성 |
| `download.sha256` | string (hex 64) | 다운로드 파일 SHA256. OpenSSL EVP로 스트리밍 검증 |
| `download.size_bytes` | integer | 예상 크기 (byte). 다운로드 중 초과 시 abort + `failure_reason: download_failed`. `WORKER_TMP_DIR` statvfs 여유 < size + `WORKER_DISK_RESERVE_MB` 면 다운로드 시작 전 `failure_reason: insufficient_disk` |
| `install.script` | string | tar 내부에서 실행할 스크립트 경로. 보통 `install.sh` |
| `install.args` | array of string | 스크립트 인자. 빈 배열 가능 |
| `install.timeout_sec` | integer | wall-clock timeout. 초과 시 SIGTERM → 5s 후 SIGKILL. `RLIMIT_CPU`로도 같은 값 적용 |

### 예시

```json
{
  "message_type":     "task.install",
  "task_id":          "550e8400-e29b-41d4-a716-446655440010",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "issued_at":        "2026-04-23T15:00:00Z",
  "download": {
    "url":        "https://files.example.com/packages/foo-1.2.3.tar.gz",
    "sha256":     "9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca7",
    "size_bytes": 12345678
  },
  "install": {
    "script":      "install.sh",
    "args":        [],
    "timeout_sec": 600
  }
}
```

---

## 5. task.result (송신: agent → portal)

agent가 task 처리 종료 시점에 발행 (성공·실패 무관 — K3). portal이 consume.

### 라우팅

- Exchange: `assessment.tasks` (재사용)
- Routing key: `task.result`
- Queue: `worker.result` (durable, portal consume)

### 필드 정의

공통 메타데이터(`message_type`, `machine_id`, `agent_version`, `collected_at`, `hostname`, `message_id`, `boot_time`, `agent_started_at`) 그대로 포함.

| 필드 | 타입 | 의미 |
|---|---|---|
| `message_type` | string | 고정 `"task.result"` |
| `task_id` | string | 수신한 task.install의 `task_id` 그대로 |
| `status` | string | `"success"` \| `"failure"` |
| `failure_reason` | string \| null | 실패 시 분류 (아래 enum). 성공 시 `null` |
| `exit_code` | int \| null | install.sh 종료 코드. 실행 전 실패 시 `null` |
| `duration_ms` | int | 다운로드+추출+install 합계 시간 |
| `stdout_tail` | string | install.sh stdout의 끝 4KB. 미실행 시 `""` |
| `stderr_tail` | string | install.sh stderr의 끝 4KB. 미실행 시 `""` |
| `completed_at` | string | 처리 완료 시각 (ISO 8601 UTC) |

### `failure_reason` enum

| 값 | 의미 |
|---|---|
| `url_not_allowed` | `download.url`의 host가 화이트리스트 위반 |
| `download_failed` | HTTP 4xx/5xx, 네트워크 오류, redirect 수신, size_bytes 초과 |
| `sha256_mismatch` | 다운로드 완료 후 sha256 불일치 |
| `extract_failed` | tar 파싱 실패, symlink/hardlink/device/FIFO/socket 엔트리, path traversal (`..` 또는 절대경로) |
| `script_not_found` | 추출 후 `install.script` 경로 없음 |
| `script_failed` | exit_code != 0 |
| `script_timeout` | `install.timeout_sec` 초과로 강제 종료 |
| `insufficient_disk` | 다운로드 시작 전 statvfs 체크에서 여유 공간 부족 |
| `internal_error` | 그 외 (자식 비정상 종료, 디스크 풀, fork 실패 등). 부모가 합성 result로 보고할 때도 사용 |
| `already_done` | 동일 `task_id`로 이전에 처리 완료된 task가 재배달됨 (멱등성 마커 발견). agent는 install 안 함 |

### 예시 (성공)

```json
{
  "message_type":     "task.result",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T15:08:34Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440011",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

  "task_id":        "550e8400-e29b-41d4-a716-446655440010",
  "status":         "success",
  "failure_reason": null,
  "exit_code":      0,
  "duration_ms":    513412,
  "stdout_tail":    "...installed foo-1.2.3 successfully\n",
  "stderr_tail":    "",
  "completed_at":   "2026-04-23T15:08:34Z"
}
```

### 예시 (sha256 불일치 실패)

```json
{
  "message_type":     "task.result",
  "machine_id":       "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "agent_version":    "1.0.0",
  "collected_at":     "2026-04-23T15:01:12Z",
  "hostname":         "web-server-01",
  "message_id":       "550e8400-e29b-41d4-a716-446655440012",
  "boot_time":        "2026-04-20T09:12:33Z",
  "agent_started_at": "2026-04-23T14:29:55Z",

  "task_id":        "550e8400-e29b-41d4-a716-446655440010",
  "status":         "failure",
  "failure_reason": "sha256_mismatch",
  "exit_code":      null,
  "duration_ms":    8341,
  "stdout_tail":    "",
  "stderr_tail":    "",
  "completed_at":   "2026-04-23T15:01:12Z"
}
```

---

## 필터링 정책 (agent ↔ engine 책임 분리)

원칙: **agent는 OS·환경 무관 잡음을 자르고, engine은 분석 의도에 따른 제외를 한다.**
v2 raw-values 원칙(누적 카운터는 agent, delta·% 환산은 engine)과 동일한 분리 사상.

### Agent가 수집 단계에서 제외하는 것

| 대상 | 정책 | 적용 위치 |
|------|------|-----------|
| 블록 디바이스 — `loop`, `ram`, `sr`, `fd` 접두사 | 항상 제외. lsblk는 `-e 7,11` (major 7=loop, 11=sr/cdrom)로 1차 차단, `/sys/block` 폴백 경로는 이름 접두사로 동일 정책 적용 | `inventory.disks[]`, `metrics.disk_io[]` |
| 가상 파일시스템 — `proc`, `sysfs`, `cgroup`, `cgroup2`, `devpts`, `tmpfs`, `devtmpfs`, `mqueue`, `hugetlbfs`, `fusectl`, `debugfs`, `tracefs`, `securityfs`, `pstore`, `autofs`, `rpc_pipefs`, `binfmt_misc`, `configfs`, `bpf`, `ramfs`, `overlay`, `squashfs`, `nsfs` | 항상 제외 (커널 인터페이스·snap squashfs·k8s nsfs 등) | `inventory.mounts[]`, `metrics.mounts[]` |
| 네트워크 인터페이스 — `lo` | 항상 제외 | `metrics.net_io[]` |
| Bind mount 중복 | `/proc/self/mountinfo`에서 동일 `(major:minor)`는 첫 등장 마운트포인트만 유지 | `inventory.mounts[]`, `metrics.mounts[]` |

### Engine이 분석 단계에서 처리하는 것

- **이름 정규화**: `httpd`/`apache2`, `mysqld`/`mysql`/`mariadb` 같은 OS별 unit 이름을 카테고리(`web`/`db`/`...`)로 매핑. 매핑 테이블은 엔진 측에 둔다 (변경 시 에이전트 재배포 회피).
- **분석 목적 제외**: "리포트 총 용량에서 docker overlay 제외" 같이 리포트마다 룰이 다른 항목.
- **시계열 보정**: 재부팅으로 카운터 리셋, 32-bit wrap 처리.

### 변경 가이드

- 새 fstype 차단이 필요하면 → agent `skip_fs[]`에 추가 (모든 OS에서 의미가 동일한 경우만)
- 특정 보고서에서만 빼고 싶으면 → engine 단계 룰로 추가 (에이전트 변경 X)
- 정책 변경은 본 문서를 동시 갱신 — 컨슈머가 "어디서 잘렸는지"를 한 곳에서 확인 가능해야 함

---

## 호환성 참고

| 이슈 | 영향 범위 | 대응 |
|------|-----------|------|
| `MemAvailable` 필드 부재 | CentOS 7.0~7.1 (커널 < 3.14) | `MemFree + Buffers + Cached`로 fallback (값은 `mem_available_kb`에 채움). fallback 실패 시 `null` |
| `/proc/meminfo` 라인 부분 부재 / 파싱 실패 | 비정상 환경 | 해당 kB 필드를 `null`로 발행 (0이 아님 — 모호성 방지) |
| `lsblk` 미설치 / `lsblk -J` 미지원 | CentOS 7 (util-linux 2.23) | `/sys/block` 스캔으로 자동 폴백. `type`은 항상 `"disk"` |
| `systemctl` 미존재 (비-systemd) | 지원 외 (Amazon Linux 1, 일부 컨테이너) | `inventory.services`를 `null`로 발행. 에러 아님 |
| `/proc/<pid>/fd/*` 권한 부족으로 listen 소켓 → pid 매칭 실패 | unprivileged agent + 다른 uid 프로세스 | 해당 항목 `pid`/`comm`을 `null`로 두고 `uid`만 채워 발행 |
| `/proc/diskstats` 컬럼 수 가변 | 커널별 14 / 18 / 20 | 앞 14컬럼만 사용 |
| 재부팅으로 누적 카운터 리셋 | 모든 환경 | 컨슈머는 두 시점 간 공통 메타 `boot_time`이 다르면 **차분 스킵 (warm-up 1샘플)**. 동일하면 차분 그대로 사용. 음수 차분은 32-bit wrap으로 별도 처리 |
| 32-bit 카운터 wrap | 장기 uptime | 동일하게 음수 delta는 skip |
| `/etc/machine-id` 빈 파일 | 컨테이너 빌드된 이미지 | `/var/lib/dbus/machine-id` → `dbus-uuidgen --get` → IMDS instance-id fallback |
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
| `lsblk -J` (외부 명령) | ⚠️ util-linux 2.23은 `-J` 미지원, `/sys/block` 폴백 사용 | ✅ | ✅ | ✅ | ✅ |
| `/proc/self/mountinfo` (mount dedup) | ✅ (커널 ≥ 2.6.26) | ✅ | ✅ | ✅ | ✅ |
| `/proc/net/tcp`, `/proc/net/tcp6` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `systemctl list-units` | ✅ (systemd) | ✅ | ✅ | ✅ | ✅ |
