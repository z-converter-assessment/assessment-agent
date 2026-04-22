#!/usr/bin/env bash
#
# tests/fault_agent.sh
# --------------------
# N개의 에이전트를 루프 모드로 띄우고 그 중 하나를 SIGKILL로 죽였을 때
# 나머지 에이전트가 계속 정상 발행하는지 확인한다.
#
# 사용:
#   make
#   bash scripts/rabbitmq-up.sh
#   bash tests/fault_agent.sh [N]     # 기본 N=3
#
# 관찰 포인트:
#   - kill 대상 외의 에이전트는 kill 전후 모두 "published inventory" 로그가 남아야 한다.
#   - 큐 메시지 수가 kill 이후에도 증가해야 한다.

set -eu

cd "$(dirname "$0")/.."
# shellcheck source=tests/lib.sh
source tests/lib.sh

N="${1:-3}"
BIN="./assessment-agent"
LOGDIR="/tmp/assessment-agent-tests"
mkdir -p "$LOGDIR"

if [ "$N" -lt 2 ]; then
    echo "[fault_agent] N은 2 이상이어야 합니다." >&2
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo "[fault_agent] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2
    exit 1
fi

if ! wait_broker_ready 5; then
    echo "[fault_agent] RabbitMQ management API 접근 실패." >&2
    exit 1
fi

pids=()
logs=()
for i in $(seq 1 "$N"); do
    log="$LOGDIR/agent-$i.err"
    : > "$log"
    AGENT_INTERVAL_SEC=3 "$BIN" > /dev/null 2> "$log" &
    pids+=($!)
    logs+=("$log")
done

trap '
    for p in "${pids[@]}"; do
        kill "$p" > /dev/null 2> /dev/null || true
    done
' EXIT

echo "[fault_agent] $N 개 에이전트 시작. 10s 후 agent#1 SIGKILL"
sleep 10

before=$(queue_messages)
kill -9 "${pids[0]}"
echo "[fault_agent] kill ${pids[0]} 완료. 15s 동안 잔여 에이전트 관찰"
sleep 15
after=$(queue_messages)

for i in $(seq 1 $((N-1))); do
    kill "${pids[i]}" > /dev/null 2> /dev/null || true
done
wait 2> /dev/null || true
trap - EXIT

echo "[fault_agent] kill 전 큐: $before, kill 후 큐: $after"
echo "[fault_agent] 에이전트별 'published inventory' 카운트:"
dead_cnt=$(grep -c 'published inventory' "${logs[0]}" || true)
echo "  agent1 (killed): $dead_cnt"
survivor_total=0
for i in $(seq 1 $((N-1))); do
    c=$(grep -c 'published inventory' "${logs[i]}" || true)
    echo "  agent$((i+1))       : $c"
    survivor_total=$((survivor_total + c))
done

# 생존 에이전트가 kill 이후에도 발행했다면 큐가 증가해 있어야 한다
if [ "$after" -le "$before" ]; then
    echo "[fault_agent] kill 이후 큐가 증가하지 않았습니다. 잔여 에이전트 동작 여부 확인 필요." >&2
    exit 1
fi

if [ "$survivor_total" -lt $((N-1)) ]; then
    echo "[fault_agent] 잔여 에이전트의 누적 발행 횟수가 부족합니다." >&2
    exit 1
fi

echo "[fault_agent] PASS"
