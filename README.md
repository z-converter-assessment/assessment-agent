# assessment-agent

AI 기반 Assessment 서비스의 **데이터 수집 에이전트**.

고객 서버(상용 클라우드 VM)에서 리소스 메트릭을 자동 수집하여 RabbitMQ를 통해 중앙 분석 서버(assessment-worker)로 전송합니다.
수집된 데이터는 분석 엔진에서 avg/p95/peak 계산, Over/Under Provisioning 판단, 적정 스펙 추천에 사용됩니다.

---

## 타겟 환경

- **아키텍처**: x86_64 only
- **최소 커널**: 3.10 (CentOS 7)

| 클라우드 | 주요 OS |
|----------|---------|
| AWS EC2 | Amazon Linux 2/2023, Ubuntu 20.04+, RHEL 8+, CentOS 7 |
| Azure VM | Ubuntu 20.04+, RHEL 8+, CentOS 7, Debian 11+ |
| GCP CE | Ubuntu 20.04+, Debian 11+, CentOS 7, RHEL 8+ |

---

## 기술 스택

- **언어**: C
- **AMQP 클라이언트**: [rabbitmq-c](https://github.com/alanxz/rabbitmq-c) (librabbitmq)
- **JSON 직렬화**: [cJSON](https://github.com/DaveGamble/cJSON)
- **메시지 브로커**: RabbitMQ 3 (Docker 개발용 / 운영 시 내부망 브로커)
- **데이터 수집**: `/proc`, `/sys`, syscall 기반 (외부 명령어 의존 없음)
- **네트워크 전제**: **내부망**. Agent → RabbitMQ 브로커 내부망 경로 접근.

---

## 프로젝트 구조

```
.
├── src/
│   ├── main.c          # 엔트리포인트, 환경변수 파싱, 수집·발행 루프
│   ├── collect.c/.h    # /proc, /sys, syscall 기반 수집기
│   ├── publish.c/.h    # rabbitmq-c 연결 및 basic_publish
│   └── util.c/.h       # run_cmd, load_env_file, trim 유틸
├── include/            # 헤더 파일
├── docs/
│   └── payload-schema.md  # 메시지 스키마 상세 정의
├── tests/
│   ├── lib.sh          # 테스트 공통 헬퍼
│   ├── test_schema.sh  # 스키마 검증 (smoke test)
│   ├── run_multi.sh    # N개 에이전트 병렬 발행
│   ├── fault_agent.sh  # 에이전트 강제 종료 복원력
│   ├── fault_rabbitmq.sh # 브로커 재시작 복원력
│   └── run_all.sh      # 전체 테스트 순차 실행
├── scripts/
│   ├── rabbitmq-up.sh
│   └── rabbitmq-down.sh
├── Makefile
├── .env.example
└── README.md
```

> Python 프로토타입(`agent.py`, `consumer.py`, `requirements.txt`)은 초기 동작 검증용이며 추후 제거될 예정입니다.

---

## 아키텍처

```
          ┌──────────────────── 내부망 ────────────────────┐
          │                                                │
[고객 서버]                    [브로커]                    [분석 Portal]
 Agent (Producer)   →    RabbitMQ (AMQP)     →    Consumer (assessment-worker)
 - /proc 기반 수집           - Exchange: assessment (topic)  - inventory → DB 서버 등록
 - JSON 직렬화               - Queues:                       - metrics → 시계열 저장/분석
 - Publish (durable)         -   server.inventory            - error → 알림 처리
                             -   server.metrics
                             -   server.error
          │                                                │
          └────────────────────────────────────────────────┘
```

**No-Execution 원칙:** Agent는 수집·전송만 수행하며 대상 서버에 어떤 변경도 가하지 않습니다.

---

## 메시지 타입

| 타입 | Routing Key | 발송 시점 | 설명 |
|------|-------------|-----------|------|
| `inventory` | `server.inventory` | 에이전트 기동 시 1회 | 서버 정적 정보 (OS, CPU, 메모리, 디스크 구성) |
| `metrics` | `server.metrics` | 주기적 (1분) | CPU%, 메모리, I/O, 네트워크, 로드평균 |
| `error` | `server.error` | 수집/발행 실패 시 | 에러 코드, 메시지, 실패 컴포넌트 |

상세 스키마 및 JSON 예시: [`docs/payload-schema.md`](docs/payload-schema.md)

### 수집 소스

| Source | Purpose |
|--------|---------|
| `/proc/stat` | CPU user/system/iowait (delta) |
| `/proc/meminfo` | Memory total/used/available, swap |
| `/proc/diskstats` | Disk IOPS/throughput (delta) |
| `/proc/partitions` + `/sys/block/*/size` | Disk inventory |
| `/proc/net/dev` | Network rx/tx (delta) |
| `/proc/cpuinfo` | CPU model |
| `/proc/loadavg` | Load average |
| `/etc/os-release` | OS id/version |
| `/etc/machine-id` | Server unique ID |
| `statfs(2)` | Disk usage per mount |
| `getifaddrs(3)` | Internal IPs |
| `uname(2)` | Kernel version |
| `sysconf(_SC_NPROCESSORS_ONLN)` | CPU core count |

---

## 설정

`.env` 파일에서 브로커 접속 정보와 수집 주기를 설정합니다.

| 변수 | 기본값 | 설명 |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | 브로커 호스트 (내부망 도메인/IP) |
| `RABBITMQ_PORT` | `5672` | AMQP 포트 |
| `RABBITMQ_USER` / `RABBITMQ_PASS` | `admin` / `admin` | 자격증명 |
| `RABBITMQ_EXCHANGE` | `assessment` | Topic Exchange |
| `AGENT_INTERVAL_SEC` | `60` | 수집 간격(초). `0`이면 1회 실행 후 종료 |
| `AGENT_EXTERNAL_IP` | — | 설정 시 외부 IP 조회 생략 |
| `AGENT_HOSTNAME_OVERRIDE` | — | 실제 hostname 대신 이 값 사용 (테스트용) |

쉘 환경변수가 `.env`보다 우선합니다.

---

## 빌드

### 의존성 설치 (Ubuntu / Debian 계열)

```bash
sudo apt-get install -y build-essential pkg-config librabbitmq-dev libcjson-dev
```

### 빌드

```bash
make
# 결과: ./assessment-agent 바이너리 생성
```

---

## 실행

### 개발 환경 RabbitMQ 구동

```bash
sudo docker run -d --name rabbitmq -p 5672:5672 -p 15672:15672 -e RABBITMQ_DEFAULT_USER=admin -e RABBITMQ_DEFAULT_PASS=admin rabbitmq:3-management
```

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

루프 모드에서 발행 실패 시 지수 백오프로 재시도한 뒤 정상화되면 interval 주기로 복귀합니다.

---

## 테스트

의존성: `curl`, `python3`, `docker`(브로커 재시작 테스트 시)

```bash
# 전체 테스트
DOCKER_CMD="sudo docker" bash tests/run_all.sh

# 개별 실행
bash tests/test_schema.sh      # smoke test (~5s): 스키마 검증
bash tests/run_multi.sh        # 다중 에이전트 병렬 발행
bash tests/fault_agent.sh      # 에이전트 강제 종료 복원력
DOCKER_CMD="sudo docker" bash tests/fault_rabbitmq.sh  # 브로커 재시작 복원력
```

| 테스트 | 검증 내용 |
|---|---|
| `test_schema.sh` | 페이로드 필드 타입·값 정합성 |
| `run_multi.sh` | N개 동시 발행, 고유 hostname 구분, 스키마 |
| `fault_agent.sh` | 일부 에이전트 SIGKILL 후 잔여 에이전트 지속 발행 |
| `fault_rabbitmq.sh` | 브로커 재시작 후 백오프 재시도 및 자력 재연결 |

---

## 로드맵

| # | Task | Status |
|---|---|---|
| 1 | C 재구현 (rabbitmq-c + cJSON) | ✅ Done |
| 2 | 장애/복원력 테스트 | ✅ Done |
| 3 | 다중 에이전트 + 스키마 검증 | ✅ Done |
| 4 | /proc 기반 수집 전환 + 3종 메시지 타입 + topic exchange | Pending |
| 5 | Library vendoring (rabbitmq-c, cJSON) | Pending |
| 6 | Deployment (cron / systemd timer) | Pending |

---

## 학습 레퍼런스

**RabbitMQ / AMQP**
- RabbitMQ 공식 튜토리얼 — https://www.rabbitmq.com/tutorials
- RabbitMQ in Depth (Gavin M. Roy, Manning) — 운영 관점 책
- AMQP 0-9-1 레퍼런스 — https://www.rabbitmq.com/amqp-0-9-1-reference.html
- Quorum Queues — https://www.rabbitmq.com/quorum-queues.html
- rabbitmq-c (C 클라이언트) — https://github.com/alanxz/rabbitmq-c

**배포 (cron / systemd)**
- systemd 공식 매뉴얼 — `man systemd.service`, `man systemd.timer`
- Arch Wiki systemd — https://wiki.archlinux.org/title/Systemd
- crontab(5) 매뉴얼 — `man 5 crontab`

**OpenStack 메타데이터**
- Nova metadata service — https://docs.openstack.org/nova/latest/user/metadata.html
- openstacksdk 공식 문서 — https://docs.openstack.org/openstacksdk/latest/
- cloud-init 문서 — https://cloudinit.readthedocs.io

**데이터 저장 (PostgreSQL + Redis)**
- PostgreSQL 파티셔닝 — https://www.postgresql.org/docs/current/ddl-partitioning.html
- PostgreSQL BRIN 인덱스 — https://www.postgresql.org/docs/current/brin-intro.html
- TimescaleDB 튜토리얼 — https://docs.timescale.com/getting-started
- Redis 데이터 타입 가이드 — https://redis.io/docs/latest/develop/data-types/
- Use the Index, Luke! — https://use-the-index-luke.com

**관측성**
- Prometheus 공식 문서 — https://prometheus.io/docs
- Google SRE Book (무료) — https://sre.google/books/

**보안**
- RabbitMQ TLS 설정 — https://www.rabbitmq.com/ssl.html
- RabbitMQ 접근제어 (vhost/user) — https://www.rabbitmq.com/access-control.html
- HashiCorp Vault 튜토리얼 — https://developer.hashicorp.com/vault/tutorials

**메시지 스키마**
- JSON Schema 공식 — https://json-schema.org/learn
- Protocol Buffers — https://protobuf.dev
- Apache Avro — https://avro.apache.org/docs/current/

**분산 시스템 기본기 (중장기)**
- Designing Data-Intensive Applications (Martin Kleppmann, O'Reilly)
- The Twelve-Factor App — https://12factor.net
