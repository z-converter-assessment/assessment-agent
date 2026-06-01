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
- **메시지 브로커**: RabbitMQ 3 (외부에서 운영, 내부망 AMQPS 5671). **본 repo는 브로커 자체를 프로비저닝하지 않음** — 에이전트가 접속할 브로커는 별도 운영 단계에서 준비
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
├── src/                    # Linux C 소스
│   ├── main.c              # 엔트리포인트, 환경변수 파싱, 수집·발행 루프 + 워커 디스패치
│   ├── collect.c/.h        # /proc, /sys, syscall 기반 수집기 (mac_addresses[] 포함)
│   ├── publish.c/.h        # rabbitmq-c 연결 (TLS 포함) + worker용 long-lived conn
│   ├── worker.c/.h         # task.install 컨슈머 상태기계
│   ├── download.c/.h       # libcurl HTTPS + sha256 스트리밍
│   ├── extract.c/.h        # libarchive tar (entry-type 화이트리스트)
│   ├── exec.c/.h           # fork + execve install.sh (clearenv + setrlimit + setsid)
│   └── util.c/.h           # 환경변수 로더, 문자열·시간·UUID 헬퍼
├── include/                # 헤더
├── docs/
│   ├── payload-schema.md   # 메시지 스키마 (engine·windows-agent 공용 계약)
│   ├── worker-task-design.md
│   └── inventory-refresh-and-reset-detection.md
├── deploy/                 # 배포 (server-side: install.sh 한 방)
│   ├── install.sh          # 메인 설치 진입점 (POSIX sh)
│   ├── SUPPORTED_OS.md     # 지원 OS 매트릭스
│   ├── lib/
│   │   ├── detect-os.sh    # /etc/os-release 매칭
│   │   └── env-setup.sh    # 멱등 env 채우기 (빈 키만 prompt)
│   └── systemd/
│       ├── assessment-agent.service
│       └── agent.env.example
├── scripts/
│   └── image-prep.sh       # 골든 이미지 sealing 전 machine-id 리셋
├── tests/
│   ├── lib.sh
│   ├── test_schema.sh      # 스키마 검증 (smoke test)
│   ├── run_multi.sh        # N개 에이전트 병렬 발행
│   ├── fault_agent.sh      # 에이전트 강제 종료 복원력
│   ├── fault_rabbitmq.sh   # 브로커 재시작 복원력 (외부 브로커 가정)
│   └── run_all.sh
├── windows-agent/          # Windows 에이전트 (Phase 1 — 수집만)
│   ├── Makefile            # MinGW-w64 + 정적 vendor 빌드
│   ├── src/, include/      # Win32 API 기반 수집기 + SCM dispatcher
│   ├── deploy/             # install.ps1, env-setup.ps1, agent.env.example
│   └── scripts/image-prep.ps1
├── dist/                   # release 산출물 (gitignored)
├── vendor/                 # 정적 라이브러리 소스 (gitignored)
├── Makefile
├── .env.example
└── README.md
```

> 통합 운영 가이드: 이 repo 는 **에이전트만** 담당합니다. RabbitMQ 브로커 자체 (server 프로비저닝·토폴로지 부트스트랩) 는 별도 인프라 컴포넌트에서 처리합니다.

> **Docker는 *빌드* 단계 한정**입니다. 운영 서버에서 `install.sh` / `install.ps1` 가 도는 동안 도커는 등장하지 않습니다 (systemd / Windows Service 가 native 호스트 프로세스로 에이전트를 띄움). 도커는 사용자 노트북에서 amd64 Linux 바이너리를 만들 때 manylinux2014 컨테이너로만 쓰입니다.

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

| 타입 | Routing Key | 방향 | 설명 |
|------|-------------|------|------|
| `inventory`    | `server.inventory`              | agent → engine  | OS, 커널, CPU 모델, 메모리·스왑 총량, 디스크, 마운트, IP, MAC |
| `metrics`      | `server.metrics`                | agent → engine  | `cpu_stat`(raw tick), `mem_*_kb`, `swap_*_kb`, `load_{1,5,15}m`, `disk_io[]`, `mounts[]`, `net_io[]` (모두 raw) |
| `error`        | `server.error`                  | agent → engine  | `error_code`, `error_message`, `failed_component`, 재시도 요약 |
| `task.install` | `task.install.<composite_id>`   | portal → agent  | `install.type ∈ {shell, direct_exec, msi}` + `download.url/sha256/size_bytes`. agent OS 가 처리 불가능한 type 은 `unsupported_install_type` 으로 즉시 reject |
| `task.result`  | `task.result`                   | agent → portal  | `status`, `failure_reason`(enum), `exit_code`, `stdout_tail`, `stderr_tail`, `duration_ms` |

DLX(`assessment.dlx`)는 컨슈머 측 NAK / TTL 만료 / 큐 길이 초과 시 자동 라우팅 (`server.*.dead`).

공통 메타데이터: `message_type`, `machine_id`, `composite_id`(sha256), `os_family`(`linux`/`windows`), `agent_version`, `collected_at`(ISO 8601 UTC), `hostname`, `message_id`(UUID v4), `boot_time`, `agent_started_at`. **`composite_id` = `sha256(machine_id + "\n" + sorted MACs)` 가 호스트 식별 주키** (이미지 클론으로 `machine_id` 중복되는 환경 대응).

상세 스키마 및 JSON 예시: [`docs/payload-schema.md`](docs/payload-schema.md)

### install.type 매핑

| install.type   | Linux worker         | Windows worker (v2)                       |
|----------------|----------------------|-------------------------------------------|
| `shell`        | tar 추출 후 install.sh 실행 | reject → `unsupported_install_type` |
| `direct_exec`  | reject → `unsupported_install_type` | 다운로드 파일을 `CreateProcessW` 로 직접 실행 |
| `msi`          | reject → `unsupported_install_type` | `msiexec /i {path} /quiet /norestart` (exit 0 / 3010 = success) |

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

### Vendored 정적 빌드 (release 경로)

운영 호스트에 어떤 dev 패키지도 깔지 않고 단일 바이너리로 배포하는 경로. **`make verify` 가 manylinux2014 ABI 컴플라이언스 (glibc 2.17 / 화이트리스트된 동적 의존성 6개 / 금지 syscall 0) 를 강제**합니다.

#### 방법 1: 컨테이너 빌드 (권장 — host에 추가 설치 0)

```bash
./scripts/build-linux.sh
```

이 스크립트가 자동으로:
1. Docker daemon 점검
2. `manylinux2014_x86_64` 컨테이너 안에서 `scripts/build-prep.sh` 호출 → yum 으로 perl-IPC-Cmd 등 설치
3. `make vendor-fetch && make vendor-build && make USE_VENDORED=1 release`
4. `dist/` 와 `vendor/` 를 호스트 사용자로 chown

호스트는 Docker 만 있으면 됩니다 (macOS / Linux 무관). 결과: `dist/assessment-agent-linux-x86_64` + `dist/SHA256SUMS`.

> Apple Silicon (ARM) 에서는 amd64 이미지가 emulation 으로 돌아 ~10x 느립니다. **release 산출물은 native amd64 build host (CI / EC2 / VM)** 에서 만드세요.

#### 방법 2: native amd64 Linux 빌드

```bash
sudo bash scripts/build-prep.sh   # apt 또는 yum/dnf 자동 감지, 의존성 설치
make vendor-fetch && make vendor-build && make USE_VENDORED=1 release
```

`build-prep.sh` 지원 OS: Ubuntu / Debian (apt), RHEL / CentOS / Rocky / AlmaLinux / Oracle / Amazon Linux (yum 또는 dnf).

`make release` 는 자동으로 `make verify` 를 호출하므로, GLIBC_2.18+ 심볼이 하나라도 들어가면 빌드가 실패합니다.

정적 vendor 라이브러리 (Makefile에 버전 핀):
- cJSON `v1.7.18`, rabbitmq-c `v0.15.0`, libcurl `8.10.1`, libarchive `v3.7.7`, OpenSSL `3.0.15`, zlib `v1.3.1`

`vendor/` 와 `dist/` 모두 `.gitignore` 됩니다. 정리는 `make vendor-clean`.

### 동적 의존성 화이트리스트 (`make verify` 가 강제)

런타임에 동적 링크 허용되는 라이브러리는 다음 6개 + linux-vdso + ld-linux 만:
- `libc`, `libpthread`, `libdl`, `libm`, `libresolv`, `librt`

OpenSSL/zlib/libcurl/cJSON/rabbitmq-c/libarchive 는 정적 링크되어 ldd 출력에 나오지 않아야 합니다.

---

## 실행

### 운영 배포 (server 측, 한 방)

운영 호스트에 복사하는 파일은 **`assessment-agent-linux-x86_64` ELF 한 개**.
install.sh / detect-os.sh / env-setup.sh / systemd unit / agent.env.example
은 모두 ELF 내부에 `ld -r -b binary` 로 박혀 있고 install 서브커맨드가
`/tmp/agent-installer-XXXXXX/` 에 풀어 실행합니다.

```bash
sudo ./assessment-agent-linux-x86_64 install
```

서브커맨드 동작 (idempotent — 재실행 안전):

1. root 권한 확인
2. `mkdtemp` 로 `/tmp/agent-installer-XXXXXX/` (0700) 만들고 임베디드 sh /
   systemd unit / agent.env.example 풀기 — deploy/ 구조 흉내
3. `INSTALLER_SELF_PATH=/proc/self/exe` + `SKIP_SHA256=1` (자기 자신이라
   SHA256 비교 대상 없음) 환경에서 `install.sh` 실행
4. install.sh 가 평소처럼: OS 매칭 → user/group 생성 → 바이너리 +
   systemd unit 배치 → env-setup 빈 키만 prompt → `systemctl enable --now`
5. 종료 시 `/tmp/agent-installer-XXXXXX/` 삭제

골든 VM 이미지 생성:

```bash
sudo ./assessment-agent-linux-x86_64 install --image-prep   # service enable 만, start X
sudo ./assessment-agent-linux-x86_64 prep-image             # /etc/machine-id 클리어 + cloud-init / random-seed 정리
```

### 제거

```bash
sudo ./assessment-agent-linux-x86_64 uninstall              # service stop/disable + binary/unit 제거 (env / state 보존)
sudo ./assessment-agent-linux-x86_64 uninstall --purge      # 위 + /etc/assessment-agent + /var/lib/agent-worker + user/group 제거
```

> 이전 방식 (`sudo ./deploy/install.sh` 직접 호출) 도 호환을 위해 그대로
> 동작합니다 — repo clone 한 dev / CI 환경에서 여전히 사용 가능. 운영
> 배포 단계에서는 single-binary 서브커맨드 경로가 권장.

### 로컬 개발 (외부 브로커 필요)

이 repo 는 브로커를 프로비저닝하지 않습니다. 로컬에서 에이전트를 돌려보려면 다른 곳에서 RabbitMQ 인스턴스를 띄우고 `RABBITMQ_HOST`/`PORT`/`USER`/`PASS` 환경변수로 가리키세요.

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
| 4 | `/proc` 기반 수집 + 3종 메시지 타입 + direct exchange | ✅ Done |
| 5 | v2 스키마 적용 (raw values, `machine_id` 공통, `cpu_stat`/`disk_io[]`/`net_io[]`/`mounts[]`) | ✅ Done (v2.1: kB nullable + lsblk 폴백) |
| 6 | TLS (AMQPS · mTLS) / vhost / agent-publisher credentials env | ✅ Done |
| 7 | Library vendoring (rabbitmq-c, cJSON) — `make USE_VENDORED=1` 정적 빌드 | ✅ Done |
| 8 | 배포 (cron / systemd timer unit) | Pending |
