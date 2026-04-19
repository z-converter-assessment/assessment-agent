# assessment-agent

서버 Assessment 서비스의 Agent입니다.
고객 서버에서 메트릭 데이터를 수집하여 API 서버로 전송합니다.

현재는 프로토 타입으로 agent의 정상적인 작동을 보기위해 `consumer.py`와 rabbitMQ를 함께 추가하였고 실제 구현에선 제외할 부분입니다.

---

## 기술 스택

- **언어**: Python 3.6+ (개발 시 3.13 사용)
- **메시지 브로커**: RabbitMQ 3 (개발 편의상 Docker로 구동 — 이 레포에선 차후 삭제 예정)
- **AMQP 클라이언트**: [pika](https://pika.readthedocs.io)
- **설정 로딩**: [python-dotenv](https://pypi.org/project/python-dotenv/)
- **대상 OS**: Linux (Ubuntu / CentOS 계열)

---

## 프로젝트 구조

```
.
├── agent.py              # 수집 + Publisher
├── consumer.py           # 큐 구독 검증용 Consumer, 삭제 예정
├── requirements.txt
├── .env.example
├── scripts/              # 삭제 예정
│   ├── rabbitmq-up.sh
│   └── rabbitmq-down.sh
└── README.md
```

---

## 아키텍처

```
[고객 서버]                   [브로커]                     [분석 Portal]
 Agent (Producer)   →   RabbitMQ (Docker)   →   Consumer
 - 인벤토리 수집           - Exchange: assessment       - 메시지 수신
 - JSON 직렬화             - Queue:    server.metrics   - (향후) DB 저장
 - Publish (durable)       - Routing:  metrics          - (향후) 분석 파이프라인
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
| `ip_raw.external` | `AGENT_EXTERNAL_IP` 환경변수 또는 `api.ipify.org` 조회 |

---

## 설치

```
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
```

## 설정

`.env` 파일에서 브로커 접속 정보와 수집 주기를 설정합니다.

| 변수 | 기본값 | 설명 |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | 브로커 호스트 |
| `RABBITMQ_PORT` | `5672` | AMQP 포트 |
| `RABBITMQ_USER` / `RABBITMQ_PASS` | `admin` / `admin` | 자격증명 |
| `RABBITMQ_EXCHANGE` | `assessment` | 직렬(direct) Exchange |
| `RABBITMQ_QUEUE` | `server.metrics` | 큐 이름 |
| `RABBITMQ_ROUTING_KEY` | `metrics` | 라우팅 키 |
| `AGENT_INTERVAL_SEC` | `60` | 수집 간격(초). `0`이면 1회 실행 후 종료 |
| `AGENT_EXTERNAL_IP` | — | 설정 시 외부 조회 생략 |

쉘 환경변수가 `.env`보다 우선합니다.

## 실행

**1. RabbitMQ 기동**
```
bash scripts/rabbitmq-up.sh
```
Management UI: http://localhost:15672 (admin/admin)

**2. Consumer (터미널 1)**
```
.venv/bin/python3 consumer.py
```

**3. Agent (터미널 2)**
```
.venv/bin/python3 agent.py
```

**정리**
```
bash scripts/rabbitmq-down.sh
```

---

## 로드맵

### 1. OpenStack 기반 추가 수집 항목
운영 환경이 OpenStack이므로 호스트 OS 수준 메트릭을 넘어 플랫폼 계층에서 더 정밀한 정보를 얻을 수 있습니다.
- **Nova metadata service** (`http://169.254.169.254/openstack/latest/meta_data.json`): instance ID, flavor, availability zone, project ID
- **cloud-init user-data / vendor-data**: 초기 구성 정보
- **Flavor 정보**: vCPU, RAM, ephemeral disk 스펙 (현재 `nproc`/`free`와 교차검증 가능)
- **Cinder 볼륨 메타데이터**: `lsblk`로 보이는 디스크가 실제로 어떤 볼륨 타입(HDD/SSD)인지
- **Neutron 네트워크 정보**: security group, floating IP 매핑

### 2. 장애·이상 상황 테스트
Agent가 production에서 안정적으로 돌려면 실패 시나리오 검증이 필요합니다.
- **브로커 다운**: RabbitMQ 컨테이너 정지 상태에서 루프 계속 동작 여부 (현재 예외 catch는 하지만 재시도 백오프 미구현)
- **네트워크 순단**: iptables로 5672 포트 일시 차단 → 연결 재수립 여부
- **수집 커맨드 실패**: `lsblk`/`ip` 미설치 또는 권한 오류 시 부분 실패 허용 여부 결정
- **외부 IP 조회 타임아웃**: `api.ipify.org` 접근 불가 시 동작 (현재는 빈 배열로 fallback)
- **큐 미러링/HA**: RabbitMQ 노드 장애 시 메시지 손실 방지 (quorum queue 고려)

### 3. 다중 에이전트 확장 테스트
여러 서버에서 Agent를 동시에 돌렸을 때 데이터가 충돌 없이 구분되는지 확인해야 합니다.
- **부하 테스트**: Agent 10/50/100대 동시 publish 시 Consumer 처리 속도
- **서버 식별**: `hostname` 충돌 방지 (동일 이름 VM이 여러 AZ에 있을 수 있음) — instance UUID 병행 권장
- **Consumer 쪽 집계**: 서버별 최신 메시지만 유지할지, 시계열로 쌓을지 결정
- **배포 방식**: Ansible/cloud-init으로 Agent 일괄 설치 및 systemd 등록

### 4. 메타데이터 보강
현재 payload에는 시간·버전 정보가 없어 나중에 시계열 분석 시 애로가 생깁니다.
- `collected_at`: ISO8601 타임스탬프 (UTC)
- `agent_version`: Agent 자체 버전
- `schema_version`: payload 스키마 버전 (하위호환 관리)
- `duration_ms`: 수집에 걸린 시간 (성능 이슈 감지)
- `collection_errors`: 부분 실패한 필드 기록

### 5. 향후 구현 방향
프로젝트 진행 중 실제로 결정·구현해야 할 항목입니다.
- **배포 방식**: **cron 또는 systemd timer 기반 one-shot 실행**을 우선합니다. 데몬(systemd service)은 Portal 수신 측에만 적용합니다.
- **관측성**: Agent 자체 동작 상태를 볼 수 있도록 Prometheus exporter 노출, JSON 구조화 로그 출력.
- **보안**: AMQP TLS 적용, vhost/사용자 분리로 Agent별 권한 최소화, 자격증명은 Vault/K8s Secret으로 관리.
- **데이터 저장 (PostgreSQL + Redis 전제)**: PostgreSQL에 파티셔닝 + BRIN 인덱스로 시계열을 다루고, Redis는 "서버별 최신 상태 캐시"로만 사용합니다. 규모가 커져 `time_bucket` 집계나 자동 압축이 필요해지면 그때 TimescaleDB extension을 기존 PG 위에 얹으면 됩니다.

### 6. 추가 학습 주제
지식 보강이 필요한 영역입니다.
- **RabbitMQ 운영**: exchange 타입별 특성, dead-letter queue, quorum vs classic queue, prefetch/ack 패턴
- **AMQP 프로토콜**: connection vs channel 생명주기, heartbeat, publisher confirms
- **메시지 스키마 관리**: JSON Schema / Protobuf / Avro 비교, 스키마 버저닝 전략
- **OpenStack SDK**: `openstacksdk` 파이썬 라이브러리로 메타데이터 프로그래매틱 조회

### 7. 학습 레퍼런스
로드맵 토픽별 공식 문서·레퍼런스입니다.

**RabbitMQ / AMQP**
- RabbitMQ 공식 튜토리얼 — https://www.rabbitmq.com/tutorials (Python 예제로 1~7강)
- RabbitMQ in Depth (Gavin M. Roy, Manning) — 운영 관점 책
- AMQP 0-9-1 레퍼런스 — https://www.rabbitmq.com/amqp-0-9-1-reference.html
- Quorum Queues — https://www.rabbitmq.com/quorum-queues.html
- Pika 공식 문서 — https://pika.readthedocs.io

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
- prometheus_client (Python) — https://github.com/prometheus/client_python
- structlog (Python 구조화 로깅) — https://www.structlog.org
- Google SRE Book (무료) — https://sre.google/books/

**보안**
- RabbitMQ TLS 설정 — https://www.rabbitmq.com/ssl.html
- RabbitMQ 접근제어 (vhost/user) — https://www.rabbitmq.com/access-control.html
- HashiCorp Vault 튜토리얼 — https://developer.hashicorp.com/vault/tutorials

**메시지 스키마**
- JSON Schema 공식 — https://json-schema.org/learn
- Protocol Buffers — https://protobuf.dev/getting-started/pythontutorial/
- Apache Avro — https://avro.apache.org/docs/current/

**분산 시스템 기본기 (중장기)**
- Designing Data-Intensive Applications (Martin Kleppmann, O'Reilly)
- The Twelve-Factor App — https://12factor.net
