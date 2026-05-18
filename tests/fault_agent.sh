#!/usr/bin/env bash
#
# tests/fault_agent.sh
# --------------------
# N개의 에이전트를 루프 모드로 기동 후, agent#1을 SIGKILL로 강제 종료.
# 나머지 에이전트가 계속 정상 발행하는지 확인한다.
#
# 사용:
#   make && <external broker reachable at $RABBITMQ_HOST>
#   bash tests/fault_agent.sh [N]    # 기본 N=3
#
# 검증 항목:
#   1. 종료된 에이전트(agent1)의 PID가 실제로 사라짐
#   2. 잔여 에이전트가 kill 이후에도 각각 최소 MIN_PUBLISHES회 이상 발행
#   3. kill 이후 큐 메시지 수가 증가

set -eu

cd "$(dirname "$0")/.."
source tests/lib.sh

N="${1:-3}"
BIN="./assessment-agent"
LOGDIR="/tmp/assessment-agent-tests"
INTERVAL=3      # 에이전트 수집 간격 (초)
OBS_BEFORE=10   # kill 전 관찰 시간 (초) — 적어도 2회 발행 기대
OBS_AFTER=18    # kill 후 관찰 시간 (초) — 잔여 에이전트가 ≥4회 발행 기대
# MIN_PUBLISHES: OBS_AFTER 동안 interval마다 1회 발행, 여유 2회를 뺀 하한
MIN_PUBLISHES=$(( OBS_AFTER / INTERVAL - 2 ))
mkdir -p "$LOGDIR"

if [ "$N" -lt 2 ]; then
    log "[fault_agent] N은 2 이상이어야 합니다." >&2; exit 1
fi
if [ ! -x "$BIN" ]; then
    log "[fault_agent] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2; exit 1
fi
if ! wait_broker_ready 5; then
    log "[fault_agent] RabbitMQ management API 접근 실패." >&2; exit 1
fi

pids=()
logs=()
for i in $(seq 1 "$N"); do
    log_f="$LOGDIR/agent-$i.err"
    : > "$log_f"
    AGENT_INTERVAL_SEC=$INTERVAL "$BIN" > /dev/null 2> "$log_f" &
    pids+=($!)
    logs+=("$log_f")
done

trap '
    for p in "${pids[@]}"; do
        kill "$p" > /dev/null 2>&1 || true
    done
' EXIT

log "[fault_agent] ${N}개 에이전트 기동 완료. ${OBS_BEFORE}s 초기 발행 관찰..."
sleep "$OBS_BEFORE"

before=$(queue_messages)
kill -9 "${pids[0]}"
log "[fault_agent] agent1 (pid=${pids[0]}) SIGKILL 완료. ${OBS_AFTER}s 잔여 관찰..."

# --- [1] agent1 PID 소멸 확인 ---
deadline=$(( $(date +%s) + 5 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    kill -0 "${pids[0]}" 2>/dev/null || break
    sleep 1
done
if kill -0 "${pids[0]}" 2>/dev/null; then
    log "[fault_agent] FAIL: agent1이 SIGKILL 후에도 살아 있습니다." >&2; exit 1
fi
log "[fault_agent] agent1 종료 확인."

# --- [2] 잔여 에이전트 관찰 ---
sleep "$OBS_AFTER"

# kill 후 큐 메시지가 늘었는지 (management API lag 대비 최대 10s 대기)
after=$(wait_for_queue_min $(( before + 1 )) 10)

for i in $(seq 1 $((N-1))); do
    kill "${pids[$i]}" > /dev/null 2>&1 || true
done
wait 2>/dev/null || true
trap - EXIT

# --- 결과 출력 ---
log "[fault_agent] 큐: kill 전=${before}, kill 후=${after}"
log "[fault_agent] 에이전트별 'published inventory' 카운트:"
dead_cnt=$(grep -c 'published inventory' "${logs[0]}" || true)
log "  agent1 (killed): ${dead_cnt}회"

survivor_ok=0
for i in $(seq 1 $((N-1))); do
    c=$(grep -c 'published inventory' "${logs[$i]}" || true)
    status="OK"
    [ "$c" -lt "$MIN_PUBLISHES" ] && status="적음(기대≥${MIN_PUBLISHES})"
    log "  agent$((i+1)):         ${c}회  [${status}]"
    [ "$c" -ge "$MIN_PUBLISHES" ] && survivor_ok=$(( survivor_ok + 1 ))
done

rc=0

if [ "$after" -le "$before" ] 2>/dev/null; then
    log "[fault_agent] FAIL: kill 이후 큐가 증가하지 않았습니다." >&2
    rc=1
fi

if [ "$survivor_ok" -lt $((N-1)) ]; then
    log "[fault_agent] FAIL: 잔여 에이전트 일부가 충분히 발행하지 않았습니다." >&2
    rc=1
fi

[ "$rc" -eq 0 ] && log "[fault_agent] PASS"
exit "$rc"
