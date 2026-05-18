#!/usr/bin/env bash
#
# tests/run_multi.sh
# ------------------
# N개의 에이전트를 병렬로 1회 실행. 각 프로세스는 AGENT_HOSTNAME_OVERRIDE로
# 서로 다른 hostname을 사용해 서버별 구분이 가능한지 검증한다.
#
# 사용:
#   make && <external broker reachable at $RABBITMQ_HOST>
#   bash tests/run_multi.sh [N]    # 기본 N=10
#
# 검증 항목:
#   1. N개 에이전트 모두 exit 0
#   2. 큐 메시지 수 == N
#   3. 고유 hostname 수 == N
#   4. 각 메시지 페이로드 스키마 정합성

set -eu

cd "$(dirname "$0")/.."
source tests/lib.sh

N="${1:-10}"
BIN="./assessment-agent"
LOGDIR="/tmp/assessment-agent-tests"
mkdir -p "$LOGDIR"

if [ ! -x "$BIN" ]; then
    log "[run_multi] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2
    exit 1
fi

if ! wait_broker_ready 5; then
    log "[run_multi] RabbitMQ management API 접근 실패 (${RABBITMQ_HOST}:${RABBITMQ_MGMT_PORT})." >&2
    exit 1
fi

log "[run_multi] 사전 큐 purge"
purge_queue

log "[run_multi] ${N}개 에이전트 병렬 실행..."
pids=()
for i in $(seq 1 "$N"); do
    err="$LOGDIR/agent-$i.err"
    : > "$err"
    AGENT_INTERVAL_SEC=0 AGENT_HOSTNAME_OVERRIDE="multi-test-$i" \
        "$BIN" > /dev/null 2> "$err" &
    pids+=($!)
done

# --- [1] 모든 에이전트 종료 대기 ---
rc=0
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        log "[run_multi] agent-$((i+1)) 실패 (exit non-zero)" >&2
        cat "$LOGDIR/agent-$((i+1)).err" >&2
        rc=1
    fi
done
[ "$rc" -ne 0 ] && exit 1

# --- [2] 큐 메시지 수 검증 ---
log "[run_multi] 큐 메시지 수 확인 중..."
count=$(wait_for_queue_count "$N" 15)
log "[run_multi] 큐 메시지 수: $count / $N"
if [ "$count" != "$N" ]; then
    log "[run_multi] FAIL: 메시지 수 불일치 (실제=${count}, 기대=${N})" >&2
    exit 1
fi

# --- [3] 고유 message_id 검증 (peek, 소비하지 않음) ---
msg_ids=$(consume_message_ids "$N" | sort -u)
unique_ids=$(printf '%s\n' "$msg_ids" | grep -c . || true)
log "[run_multi] 고유 message_id 수: $unique_ids / $N"
if [ "$unique_ids" -ne "$N" ]; then
    log "[run_multi] FAIL: message_id 중복 또는 누락" >&2
    printf '%s\n' "$msg_ids" | sed 's/^/  /' >&2
    exit 1
fi

# --- [4] 고유 hostname 검증 (소비) ---
hostnames=$(consume_hostnames "$N" | sort -u)
unique=$(printf '%s\n' "$hostnames" | grep -c . || true)
log "[run_multi] 고유 hostname 수: $unique / $N"
printf '%s\n' "$hostnames" | sed 's/^/  /'
if [ "$unique" -ne "$N" ]; then
    log "[run_multi] FAIL: 고유 hostname 수 불일치" >&2
    exit 1
fi

# --- [5] 스키마 검증 ---
# consume_hostnames가 이미 소비했으므로, 검증용 메시지를 다시 발행
log "[run_multi] 스키마 검증용 재발행 (1개)..."
AGENT_INTERVAL_SEC=0 AGENT_HOSTNAME_OVERRIDE="schema-check" \
    "$BIN" > /dev/null 2>&1

log "[run_multi] 페이로드 스키마 검증:"
if ! validate_messages 1; then
    log "[run_multi] FAIL: 스키마 검증 오류" >&2
    exit 1
fi

log "[run_multi] PASS"
