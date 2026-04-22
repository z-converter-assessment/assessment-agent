#!/usr/bin/env bash
#
# tests/run_multi.sh
# ------------------
# N개의 에이전트 프로세스를 병렬로 1회씩 실행시킨다. 각 프로세스는
# AGENT_HOSTNAME_OVERRIDE 로 서로 다른 hostname ("multi-test-1" ...
# "multi-test-N") 을 사용하므로, 분석 서버가 서버별로 구분해 저장
# 가능한지 검증할 수 있다.
#
# 사용:
#   make
#   bash scripts/rabbitmq-up.sh
#   bash tests/run_multi.sh [N]            # 기본 N=10
#
# 검증:
#   - N개가 모두 정상 종료(exit 0)
#   - 큐에서 소비한 메시지 수 == N
#   - 메시지의 고유 hostname 수 == N

set -eu

cd "$(dirname "$0")/.."
# shellcheck source=tests/lib.sh
source tests/lib.sh

N="${1:-10}"
BIN="./assessment-agent"

if [ ! -x "$BIN" ]; then
    echo "[run_multi] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2
    exit 1
fi

if ! wait_broker_ready 5; then
    echo "[run_multi] RabbitMQ management API($RABBITMQ_HOST:$RABBITMQ_MGMT_PORT) 접근 실패." >&2
    exit 1
fi

echo "[run_multi] 사전 큐 purge"
purge_queue

mkdir -p /tmp/assessment-agent-tests
echo "[run_multi] $N 개 에이전트 병렬 실행 (hostname=multi-test-{1..$N})..."
pids=()
for i in $(seq 1 "$N"); do
    err="/tmp/assessment-agent-tests/agent-$i.err"
    : > "$err"
    AGENT_INTERVAL_SEC=0 AGENT_HOSTNAME_OVERRIDE="multi-test-$i" "$BIN" > /dev/null 2> "$err" &
    pids+=($!)
done

rc=0
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
        rc=1
    fi
done

if [ "$rc" -ne 0 ]; then
    echo "[run_multi] 일부 에이전트 프로세스가 실패했습니다." >&2
    echo "[run_multi] stderr 확인: /tmp/assessment-agent-tests/agent-*.err" >&2
    exit 1
fi

sleep 1
count=$(queue_messages)
echo "[run_multi] 발행 후 큐 메시지 수: $count (기대: $N)"

if [ "$count" != "$N" ]; then
    echo "[run_multi] 큐 메시지 수가 기대와 다릅니다." >&2
    exit 1
fi

hostnames=$(consume_hostnames "$N" | sort -u)
unique=$(printf '%s\n' "$hostnames" | grep -c . || true)
echo "[run_multi] 고유 hostname 수: $unique"
echo "$hostnames" | sed 's/^/  /'

if [ "$unique" -ne "$N" ]; then
    echo "[run_multi] 고유 hostname 수가 기대(N=$N)와 다릅니다." >&2
    exit 1
fi

echo "[run_multi] PASS"
