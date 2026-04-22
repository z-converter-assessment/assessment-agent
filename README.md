# assessment-agent

서버 Assessment 서비스의 Agent입니다.
고객 서버에서 메트릭 데이터를 수집하여 **내부망으로 연결된 분석 서버(assessment-worker)** 로 전송합니다.

Agent ↔ 분석 서버 사이의 통신 엔드포인트는 **RabbitMQ(AMQP)** 이며, Agent는 큐에 메시지를 발행하고 분석 서버는 RabbitMQ consumer로 이를 수신합니다.

---

## 기술 스택

- **언어**: C
- **AMQP 클라이언트**: [rabbitmq-c](https://github.com/alanxz/rabbitmq-c) (librabbitmq)
- **JSON 직렬화**: [cJSON](https://github.com/DaveGamble/cJSON)
- **메시지 브로커**: RabbitMQ 3 (개발 편의상 Docker로 구동 — 실제 운영 환경에서는 분석 서버 쪽 내부망 브로커에 연결)
- **대상 OS**: Linux (Ubuntu / CentOS 계열)
- **네트워크 전제**: **내부망**. 분석 서버와 고객 서버는 동일한 내부 네트워크에 연결되어 있으며 Agent는 RabbitMQ 브로커에 내부망 경로로 접근합니다. 외부 인터넷 접근은 전제하지 않습니다. 단, `api.ipify.org` 등 외부 IP 조회 로직은 혼합/예외 환경을 대비해 유지합니다.

---

## 프로젝트 구조

```
.
├── src/
│   ├── main.c          # 엔트리포인트, 환경변수 파싱, 수집·발행 루프
│   ├── collect.c/.h    # hostname / nproc / mem / lsblk / ip 수집
│   ├── publish.c/.h    # rabbitmq-c 연결 및 basic_publish
│   └── util.c/.h       # run_cmd, load_env_file, trim 유틸
├── include/            # 헤더 파일
├── tests/
│   ├── lib.sh          # 테스트 공통 헬퍼 (queue 조회, 스키마 검증 등)
│   ├── test_schema.sh  # 단일 에이전트 + 페이로드 스키마 검증 (smoke test)
│   ├── run_multi.sh    # N개 에이전트 병렬 발행 + hostname 다양성
│   ├── fault_agent.sh  # 에이전트 강제 종료 후 잔여 동작 확인
│   ├── fault_rabbitmq.sh # 브로커 재시작 후 자력 재연결 확인
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
 - 인벤토리 수집             - Exchange: assessment         - 메시지 수신
 - JSON 직렬화               - Queue:    server.metrics     - DB 저장
 - Publish (durable)         - Routing:  metrics            - 분석 파이프라인
          │                                                │
          └────────────────────────────────────────────────┘
```

**No-Execution 원칙:** Agent는 수집·전송만 수행하며 대상 서버에 어떤 변경도 가하지 않습니다.

---

## 수집 데이터 스펙

```json
{
  "hostname": "server01",
  "nproc": "4",
  "free": {"mem_total_mb": 16384},
  "lsblk_raw": [{"name": "vda", "size": "30G"}],
  "ip_raw": {"internal": ["10.0.0.10"], "external": ["1.2.3.4"]}
}
```

| 필드 | 수집 방법 |
|---|---|
| `hostname` | `hostname` |
| `nproc` | `nproc` |
| `free.mem_total_mb` | `free -m` 파싱 |
| `lsblk_raw` | `lsblk --json` 파싱 |
| `ip_raw.internal` | `ip -o -4 addr show` 파싱 (loopback 제외) |
| `ip_raw.external` | `AGENT_EXTERNAL_IP` 환경변수 또는 `api.ipify.org` 조회 (내부망 전용 환경에서는 빈 배열) |

---

## 설정

`.env` 파일에서 브로커 접속 정보와 수집 주기를 설정합니다.

| 변수 | 기본값 | 설명 |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | 브로커 호스트 (내부망 도메인/IP) |
| `RABBITMQ_PORT` | `5672` | AMQP 포트 |
| `RABBITMQ_USER` / `RABBITMQ_PASS` | `admin` / `admin` | 자격증명 |
| `RABBITMQ_EXCHANGE` | `assessment` | 직렬(direct) Exchange |
| `RABBITMQ_QUEUE` | `server.metrics` | 큐 이름 |
| `RABBITMQ_ROUTING_KEY` | `metrics` | 라우팅 키 |
| `AGENT_INTERVAL_SEC` | `60` | 수집 간격(초). `0`이면 1회 실행 후 종료 |
| `AGENT_EXTERNAL_IP` | — | 설정 시 외부 조회 생략 |
| `AGENT_HOSTNAME_OVERRIDE` | — | 설정 시 실제 hostname 대신 이 값을 payload에 사용 (다중 에이전트 테스트용) |

쉘 환경변수가 `.env`보다 우선합니다.

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

### ✅ 1. C 재구현
단일 바이너리 배포 및 런타임 의존성 최소화. `rabbitmq-c` + `cJSON` 조합, Python 프로토타입과 동일한 스키마·환경변수 유지.

### ✅ 2. 장애 시나리오 테스트
브로커 재시작 후 지수 백오프 재시도·자력 재연결, 에이전트 SIGKILL 후 잔여 에이전트 지속 발행 검증.

### ✅ 3. 다중 에이전트 테스트
10개 에이전트 동시 publish + 고유 hostname 구분 + 페이로드 스키마 자동 검증.

### 4. 페이로드 메타데이터 보강
현재 payload에는 시간·버전 정보가 없어 시계열 분석 시 애로가 생깁니다.
- `collected_at`: ISO8601 타임스탬프 (UTC)
- `agent_version`: Agent 자체 버전
- `schema_version`: payload 스키마 버전 (하위호환 관리)
- `duration_ms`: 수집에 걸린 시간 (성능 이슈 감지)
- `collection_errors`: 부분 실패한 필드 기록

### 5. 배포
- **cron 또는 systemd timer** 기반 one-shot 실행을 우선합니다. 데몬(systemd service)은 Portal 수신 측에만 적용합니다.
- 단일 바이너리 + `.env` 배포 방식 확정, Ansible/cloud-init으로 일괄 설치 자동화

### 6. OpenStack 메타데이터 수집
운영 환경이 OpenStack이므로 호스트 OS 수준 메트릭을 넘어 플랫폼 계층에서 더 정밀한 정보를 얻을 수 있습니다.
- **Nova metadata service** (`http://169.254.169.254/openstack/latest/meta_data.json`): instance ID, flavor, availability zone, project ID
- **Flavor 정보**: vCPU, RAM, ephemeral disk 스펙 (현재 `nproc`/`free`와 교차검증 가능)
- **Cinder 볼륨 메타데이터**: `lsblk`로 보이는 디스크의 실제 볼륨 타입(HDD/SSD)
- **Neutron 네트워크 정보**: security group, floating IP 매핑

### 7. 학습 레퍼런스

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
