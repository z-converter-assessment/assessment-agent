#!/usr/bin/env bash
#
# tests/test_schema.sh
# --------------------
# v3 smoke test: 에이전트를 1회 실행하고 inventory + metrics 페이로드의
# v3 스키마 준수를 검증한다. (~5s)
#
# 사용:
#   make && <external broker reachable at $RABBITMQ_HOST>
#   bash tests/test_schema.sh
#
# 큐 가정: server.inventory, server.metrics 가 모두 binding 되어 있어야 함
# (외부 broker; topology 부트스트랩은 engine 측에서 수행).
#
# 검증 항목:
#   - inventory : 공통 메타데이터 + v3 신규 필드(services, listen_ports, disks/mounts major)
#   - metrics   : 공통 메타데이터 + cpu_stat/mem/load/disk_io/mounts/net_io

set -eu

cd "$(dirname "$0")/.."
source tests/lib.sh

BIN="./assessment-agent"

if [ ! -x "$BIN" ]; then
    log "[test_schema] '$BIN' 바이너리가 없습니다. make 먼저 실행하세요." >&2; exit 1
fi

if ! wait_broker_ready 5; then
    log "[test_schema] RabbitMQ management API 접근 실패." >&2; exit 1
fi

# v3: 두 큐를 모두 검증
INV_QUEUE="${RABBITMQ_INVENTORY_QUEUE:-server.inventory}"
MET_QUEUE="${RABBITMQ_METRICS_QUEUE:-server.metrics}"

# purge both
RABBITMQ_QUEUE="$INV_QUEUE" purge_queue
RABBITMQ_QUEUE="$MET_QUEUE" purge_queue

log "[test_schema] 에이전트 1회 실행..."
AGENT_INTERVAL_SEC=0 "$BIN" 2>&1 | sed 's/^/  /'

# inventory: 1건 기다림
inv_count=$(RABBITMQ_QUEUE="$INV_QUEUE" wait_for_queue_min 1 10)
if [ "${inv_count:-0}" -lt 1 ]; then
    log "[test_schema] FAIL: $INV_QUEUE 에 메시지 없음." >&2; exit 1
fi
# metrics: 1건 기다림
met_count=$(RABBITMQ_QUEUE="$MET_QUEUE" wait_for_queue_min 1 10)
if [ "${met_count:-0}" -lt 1 ]; then
    log "[test_schema] FAIL: $MET_QUEUE 에 메시지 없음." >&2; exit 1
fi

log "[test_schema] inventory 검증:"
if ! validate_messages 1 "$INV_QUEUE"; then
    log "[test_schema] FAIL: inventory 스키마 오류." >&2; exit 1
fi

log "[test_schema] metrics 검증:"
if ! validate_messages 1 "$MET_QUEUE"; then
    log "[test_schema] FAIL: metrics 스키마 오류." >&2; exit 1
fi

log "[test_schema] PASS (v3)"
