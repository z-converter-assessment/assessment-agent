# assessment-agent

AI 기반 Assessment 서비스의 **데이터 수집 에이전트**.

고객 서버(상용 클라우드 VM)에서 정적 인벤토리와 리소스 메트릭을 자동 수집하여 RabbitMQ를 통해 중앙 분석 서버(`assessment-engine`)로 전송합니다.
수집된 데이터는 분석 엔진에서 avg / p95 / peak 계산, Over/Under Provisioning 판단, 적정 스펙 추천에 사용됩니다.

---

## 타겟 환경

- **아키텍처**: x86_64 only
- **최소 커널**: 3.10 (CentOS 7)
- **현재 스코프**: Ubuntu LTS (20.04 / 22.04 / 24.04) 우선. 그 외 배포판은 best-effort 처리 (`null` 허용)
- **배포 단위**: 클라우드 VM (컨테이너 미지원 — 호스트 `/proc` 가시성 전제)

| 클라우드 | 주요 OS |
|----------|---------|
| AWS EC2 | Amazon Linux 2/2023, Ubuntu 20.04+, RHEL 8+, CentOS 7 |
| Azure VM | Ubuntu 20.04+, RHEL 8+, CentOS 7, Debian 11+ |
| GCP CE | Ubuntu 20.04+, Debian 11+, CentOS 7, RHEL 8+ |

---

## 기술 스택

- **언어**: C
- **AMQP 클라이언트**: [rabbitmq-c](https://github.com/alanxz/rabbitmq-c) (librabbitmq) + OpenSSL
- **JSON 직렬화**: [cJSON](https://github.com/DaveGamble/cJSON)
- **메시지 브로커**: RabbitMQ 3 (Docker 개발용 / 운영 시 내부망 브로커, AMQPS 5671)
- **데이터 수집**: `/proc`, `/sys`, syscall 기반. 외부 명령은 `/proc`만으로 어려운 항목(`lsblk`, `lvs`, `ethtool` 등)에 한해 사용
- **네트워크 전제**: 내부망. Agent → Broker AMQPS-only

---

## 핵심 설계 원칙

1. **No-Execution.** 에이전트는 수집·전송만 수행하며 대상 서버에 어떠한 변경도 가하지 않습니다.
2. **Stateless.** 직전 tick / 카운터를 보관하지 않습니다. `/proc` 누적값을 그대로 publish 합니다.
3. **Raw values only.** `avg`, `pct`, `iops_per_second` 등 2차 가공값은 보내지 않습니다. delta·비율 계산은 분석 엔진 책임입니다.
4. **Single normalized schema.** OS·버전 차이는 에이전트 내부 collector가 분기 처리하고, 전송 페이로드는 단일 스키마입니다.
5. **Optional features are not errors.** `ethtool`, `lvs`, `docker`, IMDS 등 선택적 항목 부재는 `null` / 빈 배열로 처리하고 `server.error`를 발행하지 않습니다.

---

## 프로젝트 구조

```
.
├── src/
│   ├── main.c          # 엔트리포인트, 환경변수 파싱, 수집·발행 루프
│   ├── collect.c/.h    # /proc, /sys, syscall 기반 수집기
│   ├── publish.c/.h    # rabbitmq-c 연결 (TLS 포함) 및 basic_publish
│   └── util.c/.h       # 환경변수 로더, 문자열 헬퍼
├── include/            # 헤더 파일
├── docs/
│   └── payload-schema.md   # 메시지 스키마 (engine과의 contract)
├── tests/
│   ├── lib.sh
│   ├── test_schema.sh      # 스키마 검증 (smoke test)
│   ├── run_multi.sh        # N개 에이전트 병렬 발행
│   ├── fault_agent.sh      # 에이전트 강제 종료 복원력
│   ├── fault_rabbitmq.sh   # 브로커 재시작 복원력
│   └── run_all.sh
├── scripts/
│   ├── rabbitmq-up.sh
│   └── rabbitmq-down.sh
├── Makefile
├── .env.example
└── README.md
```

---

## 아키텍처

```
          ┌──────────────────── 내부망 ────────────────────┐
          │                                                │
[고객 서버]                    [브로커]                    [분석 Engine]
 Agent (Producer)   →    RabbitMQ (AMQPS 5671)   →    assessment-engine
 - /proc / /sys / syscall      - vhost: /assessment            - consumer + web
 - JSON 직렬화                 - Exchange: assessment (direct) - DB upsert (machine_id)
 - Publish (durable, persistent) - Queues: server.{inv,met,err}  - 시계열 분석
                               - DLX: assessment.dlx           - PDF 리포트 생성
          │                                                │
          └────────────────────────────────────────────────┘
```

운영 단계 보안: AMQPS(TLS 1.2+) + `/assessment` vhost + 5종 역할별 user (`agent-publisher` / `worker-consumer` / `dlq-handler` / `topology-admin` / `monitor-readonly`).

---

## 메시지 타입

Exchange: `assessment` (direct, durable). Delivery mode: persistent (2).

| 타입 | Routing Key | 발송 시점 | 설명 |
|------|-------------|-----------|------|
| `inventory` | `server.inventory` | 기동 시 + 정적 정보 변경 감지 시 | OS, 커널, CPU 모델, 메모리·스왑 총량, 디스크 구성, 마운트, IP |
| `metrics` | `server.metrics` | 주기적 (기본 60초) | `cpu_stat`(raw tick), `mem_*_kb`, `swap_*_kb`, `load_{1,5,15}m`, `disk_io[]`, `mounts[]`, `net_io[]` (모두 raw) |
| `error` | `server.error` | 수집/발행 실패 시 (선택적 도구 부재는 발행 안 함) | `error_code`, `error_message`, `failed_component`, 재시도 요약 |

DLX(`assessment.dlx`)는 컨슈머 측 NAK / TTL 만료 / 큐 길이 초과 시 자동 라우팅 (`server.*.dead`).

공통 메타데이터: `message_type`, `machine_id`, `agent_version`, `collected_at`(ISO 8601 UTC), `hostname`, `message_id`(UUID v4). **`machine_id`가 모든 메시지의 서버 식별자**.

상세 스키마 및 JSON 예시: [`docs/payload-schema.md`](docs/payload-schema.md)

### 수집 소스 (요약)

| Source | Purpose |
|--------|---------|
| `/proc/stat` | CPU 누적 tick (`cpu_stat`) |
| `/proc/meminfo` | Memory total / free / available / buffers / cached, swap total / free (kB) |
| `/proc/diskstats` (앞 14컬럼) | 디바이스별 누적 I/O 카운터 (`disk_io[]`) |
| `/proc/net/dev` | 인터페이스별 누적 RX/TX (`net_io[]`) |
| `/proc/loadavg` | 1m / 5m / 15m 로드 평균 |
| `/proc/cpuinfo` | CPU 모델 |
| `/proc/uptime` | 부팅 시각 (시계열 단절 감지) |
| `/etc/os-release` | OS id / version / codename |
| `/etc/machine-id` | 서버 불변 식별자 (fallback: `dbus-uuidgen` → IMDS instance-id) |
| `lsblk -dn -b -J` | 디스크 목록 |
| `statfs(2)` | 마운트별 사용량 (raw bytes) |
| `getifaddrs(3)` | 내부 IP |
| Cloud metadata API | 외부 IP (optional) |
| `uname(2)` | 커널 버전 |
| `sysconf(_SC_NPROCESSORS_ONLN)` | CPU 코어 수 |

---

## 설정 (.env)

| 변수 | 기본값 | 설명 |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | 브로커 호스트 |
| `RABBITMQ_PORT` | `5671` | 운영기 AMQPS. 로컬 dev는 5672 |
| `RABBITMQ_VHOST` | `/assessment` | 운영기 전용 vhost |
| `RABBITMQ_USER` / `RABBITMQ_PASS` | — | 운영기 `agent-publisher` / Vault·KMS 주입 |
| `RABBITMQ_TLS_ENABLED` | `true` | 로컬 dev에서만 false |
| `RABBITMQ_TLS_CA_PATH` | `/etc/assessment-agent/ca.pem` | 내부 CA pem |
| `RABBITMQ_TLS_VERIFY_PEER` / `RABBITMQ_TLS_VERIFY_HOSTNAME` | `true` / `true` | |
| `RABBITMQ_TLS_CERT_PATH` / `RABBITMQ_TLS_KEY_PATH` | — | mTLS 적용 시 |
| `RABBITMQ_EXCHANGE` | `assessment` | direct exchange |
| `RABBITMQ_ROUTING_KEY_INVENTORY` | `server.inventory` | engine과 contract |
| `RABBITMQ_ROUTING_KEY_METRICS` | `server.metrics` | engine과 contract |
| `RABBITMQ_ROUTING_KEY_ERROR` | `server.error` | engine과 contract |
| `RABBITMQ_HEARTBEAT_SEC` | `60` | AMQP heartbeat 간격 |
| `RABBITMQ_CONFIRM_TIMEOUT_SEC` | `5` | publisher confirm ACK 대기 wall-clock 한도 |
| `AGENT_INTERVAL_SEC` | `60` | `0` = one-shot |
| `AGENT_EXTERNAL_IP` | — | 설정 시 IMDS 호출 생략 |
| `AGENT_HOSTNAME_OVERRIDE` | — | 테스트용 |

쉘 환경변수가 `.env`보다 우선합니다.

---

## 빌드

### 빌드 의존성 (Ubuntu / Debian)

```bash
sudo apt-get install -y build-essential pkg-config librabbitmq-dev libcjson-dev libssl-dev
```

### 런타임 명령 의존성

에이전트는 일부 정보를 외부 명령으로 채집합니다. 부재 시 `null` / 빈 배열로 silent fallback되며, 시작 시 stderr에 명령 가용성을 한 줄씩 로깅합니다.

| 명령 | 용도 | 부재 시 동작 |
|---|---|---|
| `lsblk` (util-linux ≥ 2.27) | inventory `disks[]` | `/sys/block` 폴백 (type은 항상 `"disk"`) |
| `curl` | machine_id IMDS · external IP | 다음 fallback 시도 또는 `null` |
| `dbus-uuidgen` | machine_id 폴백 (체인의 한 단계) | 다음 fallback 시도 |

CentOS 7의 util-linux 2.23은 `lsblk -J`(JSON 출력)를 지원하지 않습니다. 이 경우 자동으로 `/sys/block` 스캔으로 전환됩니다.

### 빌드

```bash
make
# 결과: ./assessment-agent 바이너리 생성
```

### Vendored 정적 빌드 (선택)

운영 호스트에 `librabbitmq-dev` / `libcjson-dev`를 깔지 않고 단일 바이너리로 배포하려는 경우.

```bash
make vendor-fetch    # cJSON, rabbitmq-c 소스를 vendor/ 아래로 git clone (1회)
make vendor-build    # cmake로 정적 라이브러리 빌드
make USE_VENDORED=1  # 정적 라이브러리 링크하여 에이전트 빌드
```

`vendor/`는 `.gitignore`에 있어 커밋되지 않습니다. 정리는 `make vendor-clean`. OpenSSL은 동적 링크이므로 운영 호스트에 `libssl` 런타임이 필요합니다.

---

## 실행

### 로컬 개발 RabbitMQ 구동

```bash
sudo docker run -d --name rabbitmq \
  -p 5672:5672 -p 15672:15672 \
  -e RABBITMQ_DEFAULT_USER=admin -e RABBITMQ_DEFAULT_PASS=admin \
  rabbitmq:3-management
```

> 로컬 dev는 평문 5672 + default vhost. 운영기는 AMQPS 5671 + `/assessment` vhost + 역할별 user를 사용합니다.

### 에이전트 실행

```bash
# 1회 수집 후 종료 (cron / systemd timer 용)
AGENT_INTERVAL_SEC=0 ./assessment-agent

# 루프 모드 (60초 간격 반복)
AGENT_INTERVAL_SEC=60 ./assessment-agent

# .env 파일 사용
cp .env.example .env  # 필요 시 편집
./assessment-agent
```

루프 모드에서 발행 실패 시 지수 백오프로 재시도합니다. 재시도 루프 내부에서는 `server.error`를 발행하지 않으며, 재시도 종료 또는 복구 시점에 요약 1건만 발행합니다.

---

## 테스트

### 단위 테스트 (브로커·네트워크 불필요)

순수 파서/헬퍼 함수에 대한 C 단위 테스트. 파일 한 개·외부 프레임워크 없이 동작.

```bash
make test-unit
# tests/unit/run_unit 빌드 후 실행 → "N passed, 0 failed"
```

커버 함수: `trim_inplace` / `getenv_default` / `iso8601_utc` / `iso8601_utc_ms` / `read_file_all` / `load_env_file` / `is_machine_id` / `meminfo_get_kb` / `read_os_release_field` / `or_empty_array` / `add_kb_or_null`.

### 통합 / 회복력 테스트

의존성: `curl`, `python3`, `docker`(브로커 재시작 테스트 시)

```bash
# 전체 실행
DOCKER_CMD="sudo docker" bash tests/run_all.sh

# 개별 실행
bash tests/test_schema.sh                                # 스키마 smoke (~5s)
bash tests/run_multi.sh                                  # 다중 에이전트 병렬 발행
bash tests/fault_agent.sh                                # SIGKILL 복원력
DOCKER_CMD="sudo docker" bash tests/fault_rabbitmq.sh    # 브로커 재시작 복원력
```

| 테스트 | 검증 내용 |
|---|---|
| `test_schema.sh` | 페이로드 필드 타입·값 정합성 |
| `run_multi.sh` | N개 동시 발행, 고유 `machine_id` 구분, 스키마 |
| `fault_agent.sh` | 일부 에이전트 SIGKILL 후 잔여 에이전트 지속 발행 |
| `fault_rabbitmq.sh` | 브로커 재시작 후 백오프 재시도 및 자력 재연결 |

---

## 로드맵

| # | Task | Status |
|---|---|---|
| 1 | C 재구현 (rabbitmq-c + cJSON) | ✅ Done |
| 2 | 장애/복원력 테스트 | ✅ Done |
| 3 | 다중 에이전트 + 스키마 검증 | ✅ Done |
| 4 | `/proc` 기반 수집 + 3종 메시지 타입 + direct exchange | In progress |
| 5 | v2 스키마 적용 (raw values, `machine_id` 공통, `cpu_stat`/`disk_io[]`/`net_io[]`/`mounts[]`) | In progress |
| 6 | TLS / vhost / 5종 user 권한 모델 | Pending |
| 7 | Library vendoring (rabbitmq-c, cJSON) | 🔧 infra ready (`make vendor-fetch && make vendor-build && make USE_VENDORED=1`) — 운영 호스트에서 검증 필요 |
| 8 | 배포 (cron / systemd timer unit) | Pending |
