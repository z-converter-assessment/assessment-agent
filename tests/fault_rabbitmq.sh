#!/usr/bin/env bash
#
# tests/fault_rabbitmq.sh
# -----------------------
# 에이전트를 루프 모드로 기동한 상태에서 RabbitMQ 컨테이너를 재시작.
# 에이전트가 백오프 재시도를 거쳐 자력으로 재연결·재발행하는지 확인한다.
#
# 사용:
#   make && bash scripts/rabbitmq-up.sh
#   DOCKER_CMD="sudo docker" bash tests/fault_rabbitmq.sh
#
# 검증 항목:
#   1. 재시작 전 최소 1회 publish 성공
#   2. 재시작 구간에 백오프 재시도 로그("retrying in") 발생
#   3. 재시작 후 에이전트가 자력으로 재연결해 최소 1회 publish 성공
#   4. 에이전트 프로세스가 재시작 전·후 내내 살아 있음

set -eu

cd "$(dirname "$0")/.."
source tests/lib.sh

CONTAINER="${RABBITMQ_CONTAINER:-rabbitmq}"
BIN="./assessment-agent"
LOGDIR="/tmp/assessment-agent-tests"
LOG="$LOGDIR/fault_rabbitmq.log"
mkdir -p "$LOGDIR"
: > "$LOG"

if [ ! -x "$BIN" ]; then
    log "[fault_rabbitmq] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2; exit 1
fi

if ! $DOCKER ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    log "[fault_rabbitmq] docker 컨테이너 '$CONTAINER'가 실행 중이 아닙니다." >&2; exit 1
fi

if ! wait_broker_ready 10; then
    log "[fault_rabbitmq] 시작 시점에 management API 접근 실패." >&2; exit 1
fi

AGENT_INTERVAL_SEC=5 AGENT_HOSTNAME_OVERRIDE="fault-rmq-test" \
    "$BIN" > /dev/null 2> "$LOG" &
agent_pid=$!
trap 'kill $agent_pid > /dev/null 2>&1 || true' EXIT

log "[fault_rabbitmq] 에이전트 시작 (pid=${agent_pid}). 브로커 재시작 전 발행 대기..."

# --- [1] 재시작 전 최소 1회 publish 확인 (최대 20s) ---
deadline=$(( $(date +%s) + 20 ))
published_before=0
while [ "$(date +%s)" -lt "$deadline" ]; do
    published_before=$(grep -c 'published inventory' "$LOG" || true)
    [ "$published_before" -ge 1 ] && break
    sleep 1
done
log "[fault_rabbitmq] 재시작 전 published: ${published_before}건"

if [ "$published_before" -lt 1 ]; then
    log "[fault_rabbitmq] FAIL: 브로커 재시작 전 publish 로그가 없습니다." >&2
    cat "$LOG" >&2; exit 1
fi

# --- [2] 브로커 재시작 ---
log "[fault_rabbitmq] '$CONTAINER' 재시작 중..."
$DOCKER restart "$CONTAINER" > /dev/null
log "[fault_rabbitmq] 재시작 완료. 브로커 복구 대기..."

# 에이전트가 살아 있는지 먼저 확인
if ! kill -0 "$agent_pid" 2>/dev/null; then
    log "[fault_rabbitmq] FAIL: 브로커 재시작 직후 에이전트 프로세스가 사라졌습니다." >&2
    cat "$LOG" >&2; exit 1
fi

# 브로커가 다시 올라올 때까지 대기 (최대 60s)
if ! wait_broker_ready 60; then
    log "[fault_rabbitmq] FAIL: 브로커가 재시작 후 60s 내에 복구되지 않았습니다." >&2; exit 1
fi
log "[fault_rabbitmq] 브로커 복구 확인."

# --- [3] 재연결 후 publish 확인 (최대 60s) ---
log "[fault_rabbitmq] 에이전트 재연결·재발행 대기..."
deadline=$(( $(date +%s) + 60 ))
published_after=0
while [ "$(date +%s)" -lt "$deadline" ]; do
    total=$(grep -c 'published inventory' "$LOG" || true)
    published_after=$(( total - published_before ))
    [ "$published_after" -ge 1 ] && break
    sleep 2
done
log "[fault_rabbitmq] 재시작 후 published: ${published_after}건"

# --- 에이전트 종료 ---
if ! kill -0 "$agent_pid" 2>/dev/null; then
    log "[fault_rabbitmq] FAIL: 에이전트 프로세스가 예기치 않게 종료되었습니다." >&2
    cat "$LOG" >&2; exit 1
fi
kill "$agent_pid"
wait "$agent_pid" 2>/dev/null || true
trap - EXIT

retry_count=$(grep -c 'retrying in' "$LOG" || true)
log "[fault_rabbitmq] 백오프 재시도 로그: ${retry_count}건"
echo "--- log (tail) ---"
tail -n 30 "$LOG"

# --- 결과 판정 ---
rc=0
[ "$published_after" -lt 1 ] && { log "[fault_rabbitmq] FAIL: 재시작 후 publish 없음 (재연결 실패)." >&2; rc=1; }
[ "$retry_count"     -lt 1 ] && { log "[fault_rabbitmq] FAIL: 백오프 재시도 로그가 없습니다." >&2; rc=1; }

[ "$rc" -eq 0 ] && log "[fault_rabbitmq] PASS"
exit "$rc"
