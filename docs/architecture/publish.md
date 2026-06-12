# 발행 파이프라인 — main.c · publish.c

> 에이전트 라이프사이클(메인 루프)과 AMQP 발행 계층의 구현 문서.
> 메시지 스키마는 [`../payload-schema.md`](../payload-schema.md), MQ 토폴로지·권한 모델은 `.claude/CLAUDE.md` "RabbitMQ Topology" 절 단일 진실.

## 두 개의 연결 — CM2 모델

역할마다 AMQP 연결을 분리한다. 자격증명도 다르다 (`agent-publisher` / `agent-worker`).

| | collector (inventory·metrics·error) | worker (task.install·task.result) |
|---|---|---|
| 함수 | `publish_message()` | `publish_conn_*` 계열 |
| 수명 | **발행 1회 = 연결 1회** (open→publish→close) | long-lived, tick마다 heartbeat pump |
| 이유 | producer 코드 경로 최소화. 60초 간격이 연결 비용을 상각 | `basic_get` 폴링·ack에 채널 연속성 필요 |
| 재연결 | 다음 발행이 곧 재연결 | 명시적 reconnect + 지수 백오프 ([`worker.md`](worker.md)) |

공통 사항:

- **publisher confirm 필수.** `confirm.select` 후 발행하고 broker ACK를 기다린다. `wait_confirm()`은 select timeout이 아니라 **wall-clock deadline**(`RABBITMQ_CONFIRM_TIMEOUT_SEC`, 기본 5초)으로 관리한다 — ACK 전에 무관한 프레임이 오면 `continue`되는데, 프레임마다 타이머가 리셋되면 총 대기가 무한정 늘어나기 때문.
- **passive exchange declare.** 발행 전 exchange 존재만 확인한다. 큐·바인딩 선언은 엔진(consumer) 소유 — 에이전트 권한 모델상 configure 권한이 `^$`인 것과 일치.
- delivery_mode 2(persistent) + `message_id`(UUID v4) + `content-type: application/json`.
- TLS: `RABBITMQ_TLS_ENABLED` 시 `amqp_ssl_socket` + CA/peer/hostname verify + optional mTLS(cert/key). **bool env는 `parse_bool`로 읽는다** — `atoi("true") == 0`이라 atoi를 쓰면 TLS가 조용히 꺼지는 보안 회귀가 된다 (main.c `make_publish_config` 주석).

### worker 연결의 fd 위생

worker 연결 소켓에는 열자마자 `FD_CLOEXEC`를 건다. 없으면 install.sh가 fork를 거쳐 broker TLS 소켓 사본을 들고 가 프레임을 읽고 쓸 수 있다. 구버전 librabbitmq(CentOS 7 EPEL 0.8)는 SSL 연결에서 `amqp_get_sockfd`가 -1을 주므로, 이때는 경고 로그를 남기고 worker child의 `close_inherited_fds()` 일괄 청소가 유일한 방어가 된다.

`publish_conn_pump()`는 1ms timeout의 `wait_frame_noblock` 호출이다. timeout이 `{0,0}`이면 librabbitmq가 heartbeat 송신 분기에 진입하기 전에 short-circuit하므로 1ms가 필요하다. transport 오류(HEARTBEAT_TIMEOUT/CONNECTION_CLOSED/SOCKET_ERROR/TCP_ERROR)를 감지하면 -1을 반환해 호출자가 연결을 dead로 표시한다.

## main.c 라이프사이클

```
파싱(.env → shell env가 우선) → 시그널 설정 → machine_id 해석
→ inventory 1회 발행 → [one-shot이면 metrics 1회 후 종료]
→ worker 초기화(optional) → 루프 → drain → 종료
```

### 프로세스 초기화 결정 사항

- `signal(SIGPIPE, SIG_IGN)` — librabbitmq는 write에 MSG_NOSIGNAL을 안 쓴다. broker가 publish 도중 TCP reset하면 SIGPIPE 기본 동작으로 프로세스가 죽는다. SIG_IGN이면 write가 EPIPE를 반환하고 기존 재시도 로직이 회복한다. (install.sh로 내려보낼 때는 SIG_DFL로 복원 — [`worker.md`](worker.md) EC1.)
- `umask(077)` — worker state 파일(`done/`·`running/`·`results/`)이 task_id·stdout tail을 담으므로 owner-only.
- jitter 시드는 `time ^ getpid()` — 같은 분에 부팅된 fleet이 서로 다른 난수열을 갖게.

### 메인 루프 (loop 모드, `AGENT_INTERVAL_SEC > 0`)

tick마다: metrics 수집·발행 → inventory refresh 기한 도달 시 재발행 → `worker_tick()` → sleep.

- **inventory 주기 재발행** — `AGENT_INVENTORY_REFRESH_SEC`(기본 3600, 0=비활성)에 ±15% jitter. 같은 오케스트레이터로 동시 부팅된 fleet의 동시 발행을 1시간 기준 약 18분 창으로 분산한다. 엔진이 상태를 잃어도(재시작·캐시 소실) 에이전트 재시작 없이 복구되는 안전망.
- **chunked sleep (25초 단위)** — 통째로 `sleep(60)`하면 worker 연결의 heartbeat가 굶는다. heartbeat 60초 기준 25초 조각은 broker의 2×heartbeat 허용 창 안에 안전하게 들어간다. 조각 사이마다 `worker_keepalive()` 호출, 단 sleep에서 깬 직후 `g_stop`을 먼저 재확인한다 — half-open 소켓에서 keepalive가 블록되어 종료가 지연되는 것을 막기 위해.

### 발행 재시도 — publish_with_retry

지수 백오프(1초 시작, 2배씩, 상한 = `AGENT_INTERVAL_SEC`)로 무한 재시도. 루프 안에서는 `server.error`를 발행하지 않는다 — 죽은 broker에 에러를 발행하는 셈이라서. 대신:

- 첫 실패 시각을 `first_failed_at`에 1회 기록.
- 회복 시(재시도 >0) `PUBLISH_RECOVERED` error 메시지 1건으로 요약 — `retry_count`·`first_failed_at`·`recovered_at` 포함. 운영자가 단절 구간을 사후에 본다.
- `g_stop`(SIGTERM)이면 재시도를 끊고 -1.

### one-shot 모드 (`AGENT_INTERVAL_SEC=0`)

cron/timer용. inventory + metrics 각 1회 발행 후 종료. worker는 비활성 — 큐 폴링 주기를 보장할 수 없는 실행 모델이라서.

### worker 초기화는 옵션

`RABBITMQ_WORKER_USER`가 비면 worker 전체가 꺼진다. 켜진 경우 큐 이름은 `agent.tasks.<composite_id>` — portal의 routing key `task.install.<composite_id>`와 정확히 일치해야 한다 (payload contract v4). `worker_init` 실패는 치명 아님 — collector 단독으로 계속.

### 종료 — drain 4단계

SIGTERM 후 진행 중인 install을 살리는 단계적 종료 (상세 상태 전이는 [`worker.md`](worker.md)):

| 단계 | 기한 (env) | 동작 |
|---|---|---|
| 1. grace | `AGENT_DRAIN_GRACE_SEC` (600) | in-flight install의 자연 종료 대기 |
| 2. term | `AGENT_DRAIN_TERM_SEC` (30) | child 프로세스 그룹에 SIGTERM |
| 3. kill | (term 경과 후) | SIGKILL |
| 4. publish-stuck | `AGENT_DRAIN_PUBLISH_SEC` (180) | install은 끝났는데 result 발행만 실패가 지속되면 포기 — `/results` 파일이 다음 기동에서 재발행된다 |

4단계 기본값 180초는 worker 재연결 백오프 상한(60초)의 약 3배 — drain 중에도 재연결 시도가 2~3회 들어갈 시간을 준다. systemd `TimeoutStopSec`은 이 합보다 커야 한다 (`deploy/systemd/assessment-agent.service`).
