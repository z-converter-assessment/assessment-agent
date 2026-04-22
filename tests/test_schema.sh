#!/usr/bin/env bash
#
# tests/test_schema.sh
# --------------------
# 에이전트를 1회 실행하고 페이로드 스키마를 검증한다.
# 빠른 smoke test (~5s). CI에서도 단독 실행 가능.
#
# 사용:
#   make && bash scripts/rabbitmq-up.sh
#   bash tests/test_schema.sh
#
# 검증 항목:
#   - hostname  : 비어있지 않은 문자열
#   - nproc     : 숫자 문자열
#   - free.mem_total_mb : 양수
#   - lsblk_raw : 배열 (각 요소에 name, size 포함)
#   - ip_raw.internal : 배열
#   - ip_raw.external : 배열

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

purge_queue

log "[test_schema] 에이전트 1회 실행..."
AGENT_INTERVAL_SEC=0 "$BIN" 2>&1 | sed 's/^/  /'

count=$(wait_for_queue_count 1 10)
if [ "$count" != "1" ]; then
    log "[test_schema] FAIL: 큐에 메시지가 없습니다." >&2; exit 1
fi

log "[test_schema] 페이로드 스키마 검증:"
if ! validate_messages 1; then
    log "[test_schema] FAIL: 스키마 오류." >&2; exit 1
fi

log "[test_schema] PASS"
