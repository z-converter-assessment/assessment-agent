#!/usr/bin/env bash
#
# tests/run_all.sh
# ----------------
# 전체 테스트를 순서대로 실행한다.
#
# 실행 순서:
#   1) test_schema    — 단일 에이전트 + 페이로드 스키마 검증 (빠른 smoke test)
#   2) run_multi      — 다중 에이전트 병렬 발행 + hostname 다양성
#   3) fault_agent    — 에이전트 일부 강제 종료, 잔여 동작 확인
#   4) fault_rabbitmq — 브로커 재시작, 자력 재연결 확인
#
# fault_rabbitmq는 브로커 재기동을 수반하므로 마지막에 둔다.
# 각 테스트 전에 브로커 준비 대기 + 큐 purge를 수행한다.
#
# 사용:
#   make
#   <external broker reachable at $RABBITMQ_HOST>
#   DOCKER_CMD="sudo docker" bash tests/run_all.sh

set -eu

cd "$(dirname "$0")/.."
source tests/lib.sh

SCRIPTS=(
    "tests/test_schema.sh"
    "tests/run_multi.sh"
    "tests/fault_agent.sh"
    "tests/fault_rabbitmq.sh"
)

passed=()
failed=()

for s in "${SCRIPTS[@]}"; do
    echo
    echo "=========================================================="
    log " RUN: $s"
    echo "=========================================================="

    if ! wait_broker_ready 30; then
        log "[run_all] 브로커 미준비 — '$s' 건너뜀" >&2
        failed+=("$s (broker not ready)")
        continue
    fi
    purge_queue

    if bash "$s"; then
        log "[run_all] OK: $s"
        passed+=("$s")
    else
        log "[run_all] FAIL: $s" >&2
        failed+=("$s")
    fi
done

echo
echo "=========================================================="
echo " SUMMARY"
echo "=========================================================="
for p in "${passed[@]}"; do echo "  PASS  $p"; done
for f in "${failed[@]}"; do echo "  FAIL  $f"; done
echo

if [ "${#failed[@]}" -ne 0 ]; then
    echo " 결과: FAILED (${#failed[@]}/${#SCRIPTS[@]})"
    exit 1
fi
echo " 결과: ALL PASS (${#SCRIPTS[@]}/${#SCRIPTS[@]})"
