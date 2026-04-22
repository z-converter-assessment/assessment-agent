#!/usr/bin/env bash
#
# tests/fault_rabbitmq.sh
# -----------------------
# 에이전트를 루프 모드로 기동한 상태에서 RabbitMQ 컨테이너를
# 재시작했을 때, 후속 iteration이 재시도 백오프를 거쳐 자력으로
# 재연결·재발행하는지 확인한다.
#
# 사용:
#   make
#   bash scripts/rabbitmq-up.sh       # 컨테이너 이름: rabbitmq
#   bash tests/fault_rabbitmq.sh
#
# 타임라인:
#   t=0       에이전트 기동 (AGENT_INTERVAL_SEC=5)
#   t=10      docker restart (브로커 다운 → 복귀)
#   t=10~30   브로커 부팅 동안 agent는 실패 → 백오프 재시도 반복
#   t=30+     브로커 복귀 후 다시 publish 성공
#
# 검증 포인트:
#   - 재시작 구간에 "retrying in" 로그가 1회 이상 찍힘 (백오프 동작)
#   - 재시작 전/후 각각 최소 1회씩 "published inventory" 가 찍힘
#   - 에이전트 프로세스가 죽지 않고 살아 있음

set -eu

cd "$(dirname "$0")/.."
# shellcheck source=tests/lib.sh
source tests/lib.sh

CONTAINER="${RABBITMQ_CONTAINER:-rabbitmq}"
BIN="./assessment-agent"
LOGDIR="/tmp/assessment-agent-tests"
LOG="$LOGDIR/fault_rabbitmq.log"
mkdir -p "$LOGDIR"
: > "$LOG"

if [ ! -x "$BIN" ]; then
    echo "[fault_rabbitmq] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2
    exit 1
fi

if ! docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "[fault_rabbitmq] docker 컨테이너 '$CONTAINER'가 실행 중이 아닙니다." >&2
    exit 1
fi

if ! wait_broker_ready 10; then
    echo "[fault_rabbitmq] 시작 시점에 management API 접근 실패." >&2
    exit 1
fi

AGENT_INTERVAL_SEC=5 AGENT_HOSTNAME_OVERRIDE="fault-test" "$BIN" > /dev/null 2> "$LOG" &
agent_pid=$!
trap 'kill $agent_pid > /dev/null 2> /dev/null || true' EXIT

echo "[fault_rabbitmq] 에이전트 시작 (pid=$agent_pid). 10s 동안 초기 발행 관찰"
sleep 10

published_before=$(grep -c 'published inventory' "$LOG" || true)
echo "[fault_rabbitmq] 재시작 전 published 로그: $published_before 건"

echo "[fault_rabbitmq] '$CONTAINER' 재시작"
docker restart "$CONTAINER" > /dev/null

echo "[fault_rabbitmq] 재기동 후 30s 관찰 (백오프 재시도 + 복귀 확인)"
sleep 30

if ! kill -0 "$agent_pid" > /dev/null 2> /dev/null; then
    echo "[fault_rabbitmq] 에이전트 프로세스가 종료되었습니다. (재연결 실패)" >&2
    echo "--- log ---" >&2
    cat "$LOG" >&2
    exit 1
fi

kill "$agent_pid"
wait "$agent_pid" 2> /dev/null || true
trap - EXIT

published_total=$(grep -c 'published inventory' "$LOG" || true)
retry_count=$(grep -c 'retrying in' "$LOG" || true)
fail_count=$(grep -c 'iteration failed' "$LOG" || true)
published_after=$((published_total - published_before))

echo "[fault_rabbitmq] published total=$published_total (before=$published_before, after=$published_after)"
echo "[fault_rabbitmq] iteration failed=$fail_count, retrying=$retry_count"
echo "--- log (tail) ---"
tail -n 40 "$LOG"

rc=0
if [ "$published_before" -lt 1 ]; then
    echo "[fault_rabbitmq] 재시작 전 published 로그가 없습니다." >&2
    rc=1
fi
if [ "$published_after" -lt 1 ]; then
    echo "[fault_rabbitmq] 재시작 후 published 로그가 없습니다 (재연결 실패)." >&2
    rc=1
fi
if [ "$retry_count" -lt 1 ]; then
    echo "[fault_rabbitmq] 백오프 재시도 로그가 감지되지 않았습니다." >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "[fault_rabbitmq] PASS"
fi
exit "$rc"
