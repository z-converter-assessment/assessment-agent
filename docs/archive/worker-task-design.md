# Worker (task.install) 설계 노트

> **상태**: 설계 단계 (구현 시작 전).
> 이 문서는 다음 작업 세션에서 바로 이어받기 위한 핸드오프 노트입니다.
> 정식 스키마가 확정되면 `docs/payload-schema.md`에 통합합니다.

---

## 1. 배경

기존 `assessment-agent`는 **producer 전용** (인벤토리·메트릭 수집 후 RabbitMQ로 발행).
이번 작업은 동일 agent에 **consumer 역할**을 추가한다.

서버(`assessment-portal`)가 큐로 "이 패키지를 설치해라" 명령을 발행하면,
agent가 메시지를 받아 다음 흐름을 수행한다.

1. tar 파일 다운로드 (HTTPS)
2. 압축 해제
3. 내부의 `install.sh` 실행
4. 임시 파일 정리
5. 결과를 별도 큐로 회신

---

## 2. 결정 사항 (사용자 확정)

| 항목 | 결정 |
|---|---|
| **위치** | `assessment-agent` 안에 통합. `assessment-worker`는 서버 측 portal이라 무관 |
| **No-Execution 원칙** | **명시적 폐기**. CLAUDE.md / README의 "수집·전송만" 원칙은 이 기능 도입과 함께 수정 |
| **호스트 배치** | agent와 worker 동일 호스트에서 동시 동작 (단일 바이너리 통합) |
| **install.sh 권한** | 일반 유저 권한. root/sudo 없음 → **user-level 설치만 가능한 작업으로 한정** |
| **메시지 토폴로지** | 새 exchange + 새 큐 (기존 `assessment` exchange 재사용 안 함) |
| **다운로드 보안 수위** | HTTPS + sha256 체크섬 + 도메인 화이트리스트 (3중) |

---

## 3. 메시지 토폴로지

### 수신 (서버 → agent)

```
Exchange: assessment.tasks        (direct, durable, 신설)
  └── routing key: task.install
        └── queue: agent.tasks    (durable, 신설)
              ※ 큐 분할 방식은 4절 멱등성 결정 후 확정
```

### 송신 (agent → 서버, 결과 회신)

```
Exchange: assessment.tasks        (동일 exchange 재사용)
  └── routing key: task.result
        └── queue: worker.result  (durable, 신설; 서버 측 portal이 consume)
```

> 결과 큐도 같은 exchange에 두는 이유: task 관련 흐름이 한 exchange에 묶여
> 권한·DLX 정책을 단일 set으로 관리. 기존 `assessment` exchange(인벤토리·메트릭)와는
> **완전 분리** — 인벤토리 흐름이 task 흐름 장애에 영향받지 않게.

### 권한 (RabbitMQ vhost 사용자)

기존 `agent-publisher` 자격증명은 **그대로 둔다** (producer 측은 변경 없음).
worker 기능용 새 자격증명 신설:

| 사용자 | configure | write | read |
|---|---|---|---|
| `agent-worker` | `^$` | `^assessment\.tasks$` | `^agent\.tasks$` |

즉 worker는 task 결과를 동일 exchange로 write할 수 있고, `agent.tasks` 큐만 consume.
기존 `assessment` exchange는 손대지 못함 → 인벤토리/메트릭 흐름 분리 유지.

---

## 4. 페이로드 스키마

### 4.1 수신 메시지: `task.install`

```json
{
  "message_type": "task.install",
  "task_id": "uuid-v4",
  "machine_id": "대상 머신 식별자",
  "issued_at": "ISO 8601 UTC",
  "download": {
    "url": "https://files.example.com/packages/foo-1.2.3.tar.gz",
    "sha256": "abc123...64글자 hex",
    "size_bytes": 12345678
  },
  "install": {
    "script": "install.sh",
    "args": [],
    "timeout_sec": 600
  }
}
```

| 필드 | 타입 | 의미 |
|---|---|---|
| `message_type` | string | 고정 `"task.install"`. 향후 다른 task 타입 추가 대비 |
| `task_id` | string (UUID v4) | 작업 고유 ID. 결과 회신·중복 검출·로그 추적 키 |
| `machine_id` | string | 타겟 머신. agent는 자기 `/etc/machine-id`와 비교, 불일치 시 무시 (ack 후 drop) |
| `issued_at` | string (ISO 8601 UTC) | 발행 시각. 만료(예: 1h 이상) 메시지 거부 정책에 사용 |
| `download.url` | string | HTTPS URL. host는 화이트리스트 검증 |
| `download.sha256` | string (hex 64) | 다운로드 파일 무결성 검증 |
| `download.size_bytes` | integer | 예상 크기. 다운로드 중 초과 시 중단 (디스크 보호) |
| `install.script` | string | tar 내부에서 실행할 스크립트 경로. 보통 `install.sh` |
| `install.args` | array of string | 스크립트 인자. 빈 배열 가능 |
| `install.timeout_sec` | integer | 스크립트 실행 타임아웃. 초과 시 SIGTERM → SIGKILL |

### 4.2 송신 메시지: `task.result`

```json
{
  "message_type": "task.result",
  "task_id": "수신 메시지와 동일",
  "machine_id": "자기 식별자",
  "status": "success",
  "failure_reason": null,
  "exit_code": 0,
  "duration_ms": 12345,
  "stdout_tail": "마지막 4KB",
  "stderr_tail": "마지막 4KB",
  "completed_at": "ISO 8601 UTC"
}
```

| 필드 | 타입 | 비고 |
|---|---|---|
| `status` | `"success"` \| `"failure"` | |
| `failure_reason` | string \| null | 실패 시 분류 enum (아래) |
| `exit_code` | integer \| null | install.sh 종료 코드. 스크립트 실행 전 실패 시 null |
| `duration_ms` | integer | 다운로드+검증+실행 합계 |
| `stdout_tail` / `stderr_tail` | string | 끝에서 4KB만 캡처. 전체 로그는 보내지 않음 |

**`failure_reason` enum**:

- `url_not_allowed` — 화이트리스트 위반
- `download_failed` — HTTP 4xx/5xx, 네트워크 오류, size 초과
- `sha256_mismatch` — 체크섬 불일치
- `extract_failed` — tar 파싱 실패, path traversal 감지
- `script_not_found` — 압축 해제 후 install.script 경로 없음
- `script_failed` — exit_code != 0
- `script_timeout` — timeout_sec 초과로 강제 종료
- `message_expired` — issued_at이 너무 오래됨
- `internal_error` — 그 외 (디스크 풀, 권한 등)

---

## 5. 처리 흐름

```
                       [assessment-portal (서버)]
                                │ publish(task.install)
                                ▼
   ┌──────────────────[assessment.tasks exchange]──────────────────┐
   │                                                                │
   │   routing: task.install  ──>  queue: agent.tasks               │
   │                                                                │
   └────────────────────────────────────┬───────────────────────────┘
                                        │ consume (manual ack)
                                        ▼
                              ┌──────────────────┐
                              │     agent        │
                              │   (this repo)    │
                              └──────────────────┘
                                        │
                                        ▼
   ┌────────────────────────── 처리 단계 ──────────────────────────┐
   │                                                                │
   │  1. machine_id 일치 검증          (불일치 → ack drop, 결과 X)  │
   │  2. issued_at 만료 검증           (만료 → reject, message_expired) │
   │  3. download.url host 화이트리스트 (위반 → reject, url_not_allowed) │
   │  4. /tmp/agent-task-<task_id>/ 생성                            │
   │  5. libcurl HTTPS 다운로드        (TLS verify on, size_bytes 상한) │
   │  6. sha256 검증                   (불일치 → sha256_mismatch)   │
   │  7. tar 압축 해제                 (path traversal 차단)         │
   │  8. install.script fork/exec      (CWD = 압축 해제 디렉토리)    │
   │     timeout_sec 내 미종료 → SIGTERM, +5s → SIGKILL              │
   │  9. exit_code, stdout/stderr tail 4KB 캡처                     │
   │ 10. /tmp/agent-task-<task_id>/ 전체 삭제                       │
   │ 11. task.result publish (publisher confirm 대기)                │
   │ 12. ack (단계 1 제외하고는 결과 발행 후 ack)                    │
   │                                                                │
   └────────────────────────────────────────────────────────────────┘
```

---

## 6. 보안 가드 상세

| 가드 | 구현 메모 |
|---|---|
| **HTTPS 강제** | URL 스킴이 `https://` 아니면 즉시 거부. libcurl `CURLOPT_PROTOCOLS = CURLPROTO_HTTPS` |
| **TLS 검증** | `CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`. CA pem은 시스템 기본 또는 별도 .env로 지정 |
| **도메인 화이트리스트** | `.env` `WORKER_DOWNLOAD_ALLOWED_HOSTS=files.example.com,cdn.example.com` 콤마 구분. URL의 host가 정확히 일치해야 통과 (서브도메인 와일드카드는 단계 1에서 지원 안 함) |
| **size_bytes 상한** | libcurl `CURLOPT_PROGRESSFUNCTION`으로 다운로드 바이트 추적. 초과 시 transfer 중단 |
| **sha256 검증** | OpenSSL `EVP_DigestUpdate`로 스트리밍 해시. 다운로드 완료 후 비교 |
| **path traversal** | 압축 해제 시 각 entry 경로가 `..` 포함하거나 절대경로면 거부. 심볼릭 링크 entry도 거부 |
| **임시 디렉토리 격리** | `/tmp/agent-task-<uuid>/`. 작업 후 무조건 `rm -rf` (성공·실패 무관) |
| **timeout** | `install.timeout_sec` 초 내 미종료 시 SIGTERM, 5초 후에도 살아있으면 SIGKILL |
| **stdout/stderr 격리** | 파이프로 캡처. 4KB ring buffer 유지 (전체 로그를 메모리에 들고 있지 않기) |

---

## 7. 코드 구조 (예정)

```
src/
  main.c             # 기존 — agent 진입점, --mode=collect|worker 분기 추가
  collect.c          # 기존 — 그대로
  publish.c          # 기존 — 기능 그대로, worker 결과 발행에도 재사용 가능 여부 검토
  util.c             # 기존 — 그대로

  worker.c (신설)    # consume 루프, 메시지 파싱·검증·dispatch
  worker.h
  download.c (신설)  # libcurl HTTPS 다운로드 + sha256 + size 상한
  download.h
  extract.c (신설)   # tar 압축 해제 + path traversal 가드 (libarchive 또는 외부 tar)
  extract.h
  exec.c (신설)      # fork/exec, timeout, stdout/stderr tail 캡처
  exec.h

include/             # 위 신설 헤더들
```

빌드: `Makefile`에 libcurl + libssl(EVP_*) 링크 추가.
vendoring 모드 (`USE_VENDORED=1`)도 libcurl 정적 빌드 지원 결정 필요.

**실행 모드 결정 필요**:

- (A) 단일 바이너리 + `--mode=collect|worker` 플래그 → systemd unit 2개
- (B) 단일 바이너리 + 동일 프로세스 안에서 두 루프 동시 실행 (pthread 또는 fork)
- (C) 별도 바이너리 2개로 빌드

차회 작업 시작 시 사용자와 합의.

---

## 8. 미정 / 차회 결정 필요

1. **메시지 스키마 최종 승인** — 4절 그대로 갈지 수정할지
2. **도메인 화이트리스트 운용** — .env 콤마 구분으로 갈지, 다른 방식인지
3. **동시 작업 처리** — prefetch=1 (순차) vs 병렬 N개
4. **멱등성** — `task_id` 기반 마킹 파일로 중복 실행 차단할지, install.sh 측 책임으로 둘지
5. **실행 모드** — 7절의 (A)/(B)/(C) 중 선택
6. **agent 큐 분할 방식** — 머신별 전용 큐 (`agent.tasks.<machine_id>`) vs 공용 큐 + payload `machine_id` 필터링
7. **결과 publish 실패 시 정책** — task는 성공했는데 결과 회신만 실패한 경우 어떻게? (재시도 한도, dead-letter 처리)
8. **DLX 토폴로지** — `assessment.tasks.dlx` 신설 여부
9. **CLAUDE.md / README 수정** — No-Execution 원칙 폐기 명시, 권한 모델에 `agent-worker` 추가
10. **테스트 전략** — 통합 테스트에 가짜 tar 서버(local HTTPS) 어떻게 띄울지

---

## 9. 차회 작업 시작점

이 브랜치 (`feature/worker-install`) 에서 다음을 순서대로:

1. 8절 미정 사항 합의 (사용자와 짧은 Q&A)
2. `Makefile`에 libcurl, libssl 의존성 추가 + 헬로월드 컴파일 확인
3. `src/worker.c` 골격 (consume → 파싱 → 단계별 분기) 작성
4. `src/download.c` 단독 테스트 (curl + sha256 + size 상한)
5. `src/extract.c` 단독 테스트 (path traversal 케이스 포함)
6. `src/exec.c` 단독 테스트 (timeout, tail 캡처)
7. end-to-end smoke 테스트 (`tests/test_worker_smoke.sh`)
8. CLAUDE.md / README 갱신
