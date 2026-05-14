# 인벤토리 주기 재발행 + 카운터 리셋 감지 (메타데이터 확장) 설계 노트

> **상태**: 설계 단계 (구현 시작 전).
> 차회 세션에서 두 가지 미정 사항(9절)을 확정한 뒤 구현 진입.
> 정식 확정 후엔 `docs/payload-schema.md`에 통합.

---

## 1. 배경 — 풀려는 두 가지 문제

### (a) 서버 측 인벤토리 상태 유실 시 회복 경로 부재

현재 inventory 메시지 발행 시점:
- agent 기동 시 1회
- 정적 정보 변경 감지 시

서버(`assessment-portal`)가 재시작·캐시 만료·메시지 유실로 인벤토리를 잃으면,
**agent를 재기동해야만** 회복된다. 운영 부담 큼.

### (b) 누적 카운터 리셋과 agent 재시작 구별 불가

서버는 두 시점의 cumulative 카운터 차분으로 CPU 사용률·네트워크 처리량을 계산.
서버가 재부팅되면 `/proc/stat`, `/proc/net/dev` 누적값이 0으로 초기화되어
차분이 음수·비정상값으로 튄다.

agent만 재시작된 경우는 커널 카운터가 그대로라 차분이 정상이지만,
서버 측에서는 두 케이스를 **구별할 수단이 없다**.

---

## 2. 결정 사항 (사용자 확정)

| 항목 | 결정 |
|---|---|
| **인벤토리 주기 재발행** | 도입. **1시간 주기**, ±15% jitter |
| **변경 시 즉시 발행** | 그대로 유지 (주기 발행과 보완 관계) |
| **`boot_time` 위치** | inventory → **공통 메타데이터로 이동**. 모든 메시지에 실린다 |
| **`agent_started_at` 신설** | 공통 메타데이터에 추가 |
| **`boot_time` 책임** | 카운터 리셋 판정의 **유일한 권위 소스** |
| **`agent_started_at` 책임** | 관측·디버깅용 컨텍스트. 차분 로직은 참조하지 않음 (단, 9절 ①번 결과에 따라 변동 가능) |

---

## 3. 인벤토리 주기 재발행

### 3.1 주기 = 1시간 (±15% jitter)

1시간 선택 근거:
- AWS SSM Inventory 30분 / Puppet 30분 / Chef 30분 / Datadog host metadata 4시간 → 디팩토 범위 30분~수시간
- 인벤토리는 정적 정보(OS, CPU, 메모리 총량). 변경 발행 경로가 메인이고 주기 발행은 안전망 → 자주 칠 이유 약함
- 30분 대비 트래픽 절반
- 평균 회복 시간 ~30분, 최악 60분 — 운영팀이 대시보드로 인지·조치하는 데 충분

### 3.2 Jitter ±15%

같은 분에 부팅된 다수 agent가 동일 주기로 발행하면 thundering herd.
jitter 적용 시 발행 시점이 60분 ± 9분 구간에 분산됨.

구현 메모:
- agent 시작 시 한 번 `rand()`로 초기 오프셋 계산
- 매 주기 발행 후 다음 주기를 `3600s × (1 + uniform(-0.15, +0.15))`로 재계산
- 환경변수 `AGENT_INVENTORY_REFRESH_SEC` (기본 3600)로 override 가능. 0이면 주기 발행 비활성

### 3.3 변경 시 즉시 발행은 유지

주기 발행이 있어도 정적 변경(예: 디스크 추가) 시 1시간을 기다리면 안 된다.
기존 변경 감지 발행 경로는 **그대로**.

### 3.4 서버 측 idempotency (확인 필요 — 8절)

동일 인벤토리가 반복 도착해도 서버 측 DB write/이벤트 발행이 일어나지 않아야 한다.
이미 "변경 시 발행" 로직이 있으므로 서버 측에 해시 비교가 있을 가능성 큼.
**`assessment-portal` 측 코드를 확인해서 idempotency가 보장되는지 검증** 필요.
보장 안 되면 portal 측에 해시 비교를 먼저 넣어야 트래픽 증가가 곧 DB write 증가로 이어지지 않는다.

### 3.5 SLO 확인 (확인 필요 — 8절)

다음 항목이 만족되어야 1시간이 유효:
- 서버 측 인벤토리 캐시 TTL이 1시간보다 길거나 없음 (TTL의 1/2~1/3보다 짧은 주기 필요)
- 명시적 RTO(인벤토리 유실 회복 목표 시간) 60분 이상

이 두 항목이 어긋나면 30분으로 단축.

---

## 4. 공통 메타데이터 확장

현재 공통 메타데이터: `message_type`, `machine_id`, `agent_version`, `collected_at`, `hostname`, `message_id`.

**추가**:

| 필드 | 타입 | 의미 |
|---|---|---|
| `boot_time` | ISO 8601 UTC (또는 epoch seconds) | 시스템 부팅 시각. `/proc/stat`의 `btime` 1회 캐시(권장; 9절 ②) |
| `agent_started_at` | ISO 8601 UTC | agent 프로세스 기동 시각. 메모리에 캐시 |

**inventory 메시지에서 `boot_time` 제거** (5절 마이그레이션 단계 거친 뒤).

### 4.1 두 필드의 의미적 분리 (반드시 문서화)

| 필드 | 책임 | 차분 로직에서 사용? |
|---|---|---|
| `boot_time` | 카운터 리셋 판정의 권위 소스 | **사용 (유일)** |
| `agent_started_at` | "이 데이터를 보낸 agent 인스턴스가 언제 떴는지" 관측·디버깅 | **사용 안 함** (Case B 예외 — 9절 ①) |

이 분리를 안 박아두면 "둘 중 뭘 봐야 하지?" 혼란이 반드시 옴.

---

## 5. 카운터 리셋 감지 로직 (서버 측)

### 5.1 판정 매트릭스

| `boot_time` 변화 | 차분 부호 | 판정 | 처리 |
|---|---|---|---|
| 동일 | 양수 | 정상 | 차분 그대로 사용 |
| 동일 | 음수 | 카운터 wraparound (32bit 한계) | 별도 처리 가능 |
| 변경 | — | 서버 재부팅 | **차분 스킵** (warm-up 1샘플) |

기존 단순 로직("음수 차분 = 리셋")으로는 wraparound와 reboot가 섞여 들어왔을 가능성.
이제는 두 케이스가 정밀하게 분리됨 — 리뷰 시 어필 포인트.

### 5.2 agent-only 재시작 구별

| 케이스 | `boot_time` | `agent_started_at` | 카운터 |
|---|---|---|---|
| 정상 | 동일 | 동일 | 차분 OK |
| agent만 재시작 | 동일 | 변경 | 차분 OK (커널 카운터 유지) |
| 서버 리부팅 | 변경 | 변경 | 차분 스킵 |
| 카운터 wraparound | 동일 | 동일 | 음수면 별도 처리 |

→ `boot_time`만으로 리셋 판정. `agent_started_at`은 관측용.

### 5.3 비교 방식 — 변경 감지만, 절대값 비교 금지

- ✅ `prev.boot_time != curr.boot_time` (변경 감지)
- ❌ `now() - boot_time < 5min` 같은 절대값 비교 (서버·agent 시계 어긋남에 취약)

`agent_started_at`도 동일 — 시간차 계산용으로 쓰지 말 것.

### 5.4 첫 샘플 정책 — 누락 + 별도 카운터

`boot_time` 변경 직후 첫 metric은 차분 불가.

| 방식 | 장점 | 단점 |
|---|---|---|
| **누락 (null/skip)** ✅ 권장 | 정확성 보존 | 그래프에 1점 hole |
| 0으로 처리 | hole 없음 | 평균/합계 왜곡 |
| 첫 cumulative를 베이스라인으로 잡고 다음부터 | hole 없음, 정확 | 첫 1샘플 지연 |

**권장**: 차분 컬럼은 null로 기록 + 운영 가시성용 카운터 `metric_reset_total{server_id}` 발행.
운영팀이 리셋 빈도 모니터링 가능.

서버 자체가 재시작되어 이전 샘플을 잃은 경우도 동일 — 첫 샘플 차분 스킵.

---

## 6. `boot_time` 값 안정성 함정 (가장 중요)

`/proc/stat`의 `btime`은 매번 `wall_clock_now - uptime`으로 재계산되는 값.
**같은 부팅인데 NTP 보정으로 1~2초 흔들릴 수 있음.**

매 샘플마다 새로 읽으면 → 차분 계산 시 "리셋 발생!" 오탐이 생긴다.

### 해결책 (택 1)

| 방식 | 설명 |
|---|---|
| **① agent 시작 시 1회 읽고 메모리 캐시** ✅ 강력 추천 | uptime은 monotonic이라 흔들림 없음. 가장 안전. |
| ② 매번 읽되 서버 측에서 ±10초 이내 차이는 같은 부팅으로 판정 | 임계값 튜닝 부담. 차선책. |

→ 9절 ②번에서 ① 적용 가능 여부 확인.

### 컨테이너 namespace 함정 (참고)

`/proc/stat`이 호스트 namespace를 보는지 컨테이너 namespace를 보는지에 따라 값이 다름.
**카운터(/proc/stat, /proc/net/dev)와 `boot_time`은 동일 source에서 읽어야 함**.
어긋나면 의미 없음.

본 프로젝트는 컨테이너 미지원이라 현 시점 영향 없음.

---

## 7. 차분을 누가 계산하는가 (가장 큰 분기점)

이 결정이 `agent_started_at`의 책임 무게와 첫 샘플 정책을 좌우.

### Case A: 서버가 차분 계산 (현행으로 추정 — raw cumulative 발행)

- 서버가 이전 샘플 + 그때의 `boot_time`을 함께 저장해야 함
- 이미 차분 계산을 위해 이전 샘플은 들고 있을 것 — 스키마에 `boot_time` 컬럼 추가만 하면 됨
- 서버 자체가 재시작되면 이전 `boot_time` 유실 → 첫 샘플 무조건 차분 스킵 (warm-up 1샘플)
- `agent_started_at`은 **관측용**으로 머무름

### Case B: agent가 차분 계산 (rate 값을 발행)

- agent 재시작 시 직전 cumulative 값이 메모리에서 사라짐 → 첫 샘플 차분 불가
- 서버는 `agent_started_at` 변경을 보고 "이 샘플은 신뢰하지 마"를 판단해야 함
- 이 경우 `agent_started_at`이 **운영 신호로 격상**됨

### 현재 추정

`docs/payload-schema.md`와 CLAUDE.md의 핵심 원칙 3 ("Raw values only. avg/pct/rate 안 보냄. delta·비율 계산은 분석 엔진 책임") 에 따라 **Case A**.

→ 그래도 9절 ①에서 명시적으로 확인 후 진행.

---

## 8. 마이그레이션 — 구버전 agent 호환

`boot_time`을 inventory → 공통 메타로 옮기는 동안 구버전 agent가 살아있다.

### 단계적 전환

| 단계 | 신버전 agent | 서버 |
|---|---|---|
| 1. 호환기 | inventory `boot_time` + 공통 메타 `boot_time` **양쪽 모두 발행** | 공통 메타 우선, 없으면 inventory에서 fallback |
| 2. 전환 완료 확인 | — | 모든 호스트가 공통 메타로 발행 중인지 확인 |
| 3. 정리 | inventory에서 `boot_time` 제거. 공통 메타에만 발행 | inventory `boot_time` 필드 deprecate |
| 4. 스키마 정리 | — | inventory 스키마에서 필드 제거 |

이 순서를 어기면 일부 호스트가 카운터 리셋 판정에서 빠지는 회귀 발생.

---

## 9. 미정 / 차회 결정 필요

### ① 차분 계산은 어디서? (Case A vs B)

**확인 방법**: `assessment-portal` 측 consumer/handler 코드와 metric_snapshots 테이블 스키마 확인.
- avg/pct/rate 컬럼이 metric_snapshots에 저장 → Case A (raw 받아서 서버에서 차분)
- agent가 이미 rate 값을 발행 → Case B

CLAUDE.md 원칙상 Case A가 유력하지만 **코드로 한 번 확인 필수**.

### ② `boot_time` 1회 캐시 가능한 구조인가?

현재 main.c가 loop 모드에서 매 tick마다 collector를 재호출하는 구조라면,
collector 함수 내부에서 static 캐시 변수로 1회만 읽도록 변경 필요.

`src/collect.c`의 `boot_time` 수집 부분 확인 후 결정.

### ③ `boot_time` 직렬화 포맷

- ISO 8601 UTC 문자열 (`"2026-05-11T01:23:45Z"`) — 기존 `collected_at`과 일관
- epoch seconds (정수) — 비교 부담 적음, 페이로드 약간 작음

기존 메타데이터 컨벤션에 맞춰 ISO 8601 권장. 그러나 NTP 흔들림 핵심 변수라 epoch 정수도 고려 여지.

### ④ inventory idempotency 검증 (3.4)

`assessment-portal`의 consumer가 동일 inventory 반복 수신 시 noop인지 확인.
아니면 portal 측 PR이 선행되어야 함.

### ⑤ `metric_reset_total` 알림 카운터 발행 채널

5.4의 운영 카운터를 별도 메시지로 발행할지, 기존 `server.error` 채널 재사용할지.

---

## 10. 차회 작업 시작점

이 브랜치(`feature/worker-install`)에 worker 기능과 함께 진행.
→ 단, 변경 자체는 worker와 무관하므로 **별도 커밋으로 분리** 한다 (커밋 단위 feature 분리).

순서:
1. 9절 ①·② 코드 확인 (5분)
2. 사용자와 ③·④·⑤ 짧은 합의 (10분)
3. `src/collect.c`에 `boot_time` 1회 캐시 + getter 추가
4. 공통 메타 직렬화 부분에 `boot_time` / `agent_started_at` 추가 (`src/main.c` 또는 publish 경로)
5. inventory 발행 루프에 1시간 ±15% jitter 타이머 추가 (`src/main.c`)
6. 단위 테스트 추가 (jitter 범위, `boot_time` 캐시)
7. `docs/payload-schema.md`에 공통 메타 두 필드 추가
8. CLAUDE.md / README의 inventory 발행 시점 설명 갱신

서버(`assessment-portal`) 측 변경은 별도 PR — 본 repo의 작업 범위 아님.
