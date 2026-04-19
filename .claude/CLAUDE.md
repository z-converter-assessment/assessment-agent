# Agent + RabbitMQ Communication Implementation Context

## Project Position

This task corresponds to **Phase 1 — Data Collection / Inventory Acquisition** of the AI-based Assessment service.
The Agent collects resource metrics from customer servers and transmits the data to the central Assessment Portal (analysis server) through RabbitMQ, building the data pipeline.

---

## Implementation Scope

| Component | Description |
|---|---|
| **Agent** | Server resource collection script (directly implemented) |
| **RabbitMQ** | Runs standalone on Docker, acts as message broker between Agent and Portal |
| **Producer** | Agent publishes messages to the RabbitMQ queue after collection |
| **Consumer** | Portal (analysis server) receives queue messages |

> **No-Execution Principle**: The Agent only performs collection and transmission; it does NOT perform execution/modification operations.

---

## Collected Data Specification (Agent Output Format)

```json
{
  "hostname": "server01",
  "nproc": "4",
  "free": {
    "mem_total_mb": 16384
  },
  "lsblk_raw": [
    {"name": "vda", "size": "30G"},
    {"name": "vdb", "size": "150G"}
  ],
  "ip_raw": {
    "internal": ["10.0.0.10"],
    "external": ["1.2.3.4"]
  }
}
```

### Collection Field Mapping

| Field | Collection Method | Description |
|---|---|---|
| `hostname` | `hostname` command | Server identifier |
| `nproc` | `nproc` command | Number of CPU cores |
| `free.mem_total_mb` | parse `free -m` | Total memory (MB) |
| `lsblk_raw` | parse `lsblk --json` | Disk list and sizes |
| `ip_raw.internal` | parse `ip addr` | Internal IP address list |
| `ip_raw.external` | external lookup or config | External IP address |

> Future extended collection items: CPU usage (avg/p95/peak), Memory usage, Disk I/O, Network I/O (statistics after 1–2 weeks of collection)

---

## RabbitMQ Configuration (Docker)

### How to Run

```bash
docker run -d \
  --name rabbitmq \
  -p 5672:5672 \
  -p 15672:15672 \
  -e RABBITMQ_DEFAULT_USER=admin \
  -e RABBITMQ_DEFAULT_PASS=admin \
  rabbitmq:3-management
```

| Port | Purpose |
|---|---|
| `5672` | AMQP protocol (Agent / Consumer connection) |
| `15672` | RabbitMQ Management UI |

### Queue Design

| Item | Value |
|---|---|
| Exchange | `assessment` (direct) |
| Queue | `server.metrics` |
| Routing Key | `metrics` |
| Message Format | JSON (Agent Output format) |
| Durability | durable=True (prevent message loss) |

---

## Communication Flow

```
[Customer Server]                [Broker]                [Analysis Portal]
  Agent (Producer)  →  RabbitMQ (Docker)  →  Consumer (FastAPI or Script)
  - Run collection           - Queue: server.metrics     - Receive message
  - JSON serialization       - Durable queue             - Store in DB (MariaDB)
  - Publish                                              - Trigger analysis pipeline
```

---

## Agent Implementation Specification

### Language / Dependencies

- Language: **Python 3**
- Library: `pika` (RabbitMQ AMQP client)
- Execution: one-shot or cron/scheduler repeated execution (interval TBD)

### Main Function Structure (Pseudocode)

```python
def collect_inventory() -> dict:
    # Collect hostname, nproc, free, lsblk, ip
    ...

def publish_to_rabbitmq(data: dict):
    # pika connection → channel → queue declaration → basic_publish
    ...

if __name__ == "__main__":
    data = collect_inventory()
    publish_to_rabbitmq(data)
```

### Execution Environment Assumptions

- Customer server: Linux (Ubuntu / CentOS family)
- Python 3.6+ assumed installed
- RabbitMQ broker address injected via environment variable or config file

---

## Consumer Implementation Specification (Analysis Portal Side)

- Subscribe to RabbitMQ `server.metrics` queue
- Parse received messages as JSON and store in analysis DB (MariaDB)
- After storing, trigger the analysis pipeline (asynchronous or separate scheduler)

---

## Future Integration Points

- **Analysis Engine**: Calculate avg/p95/peak based on stored metrics, determine over/under-provisioning
- **AI Recommendation**: LLM-based optimal vCPU / Memory / Disk recommendations
- **Automated Report Generation**: Jinja2 / HTML / Markdown-based customer PDF reports

---

## Work Sequence (Current Implementation Goals)

1. Run RabbitMQ on Docker and verify connection
2. Agent — implement collection function (`collect_inventory`)
3. Agent — implement RabbitMQ Producer (`publish_to_rabbitmq`)
4. Consumer — implement queue reception and output verification (pre-portal integration stage)
5. Integration test: Agent execution → RabbitMQ → Consumer reception verification

---

## 브랜치 전략

```
main          # 배포용. 직접 push 금지
develop       # 개발 통합. PR로만 머지
feature/xxx   # 기능 개발
fix/xxx       # 버그 수정
chore/xxx     # 설정 변경
```

**작업 흐름**

```
develop에서 브랜치 생성
→ 작업 완료 후 develop으로 PR
→ 1명 이상 리뷰 승인 후 머지
```

---

## 커밋 컨벤션

| 타입 | 설명 | 예시 |
|---|---|---|
| `feat` | 새로운 기능 추가 | `feat: 메트릭 수집 엔드포인트 추가` |
| `fix` | 버그 수정 | `fix: Redis 연결 타임아웃 오류 수정` |
| `chore` | 설정, 패키지 변경 | `chore: requirements.txt 업데이트` |
| `docs` | 문서 수정 | `docs: README 로컬 실행 방법 추가` |
| `refactor` | 리팩토링 | `refactor: 메트릭 파싱 로직 함수 분리` |
| `test` | 테스트 코드 | `test: 수집 API 유닛 테스트 추가` |
| `style` | 포맷 등 스타일 변경 | `style: 들여쓰기 정리` |
