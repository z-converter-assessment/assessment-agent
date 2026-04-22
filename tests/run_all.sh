#!/usr/bin/env bash
#
# tests/run_all.sh
# ----------------
# 테스트 스크립트 3종을 순서대로 실행한다.
#   1) run_multi       — 다중 에이전트 병렬 발행 + hostname 다양성
#   2) fault_agent     — 에이전트 일부 강제 종료, 잔여 동작 확인
#   3) fault_rabbitmq  — 브로커 재시작, 자력 재연결 확인
#
# fault_rabbitmq는 브로커 재기동을 수반하므로 마지막 순서에 둔다.
# 각 테스트 사이에 브로커 준비 대기 + 큐 purge 를 수행해 영향이
# 새어 나가지 않도록 한다.
#
# 사용:
#   make
#   bash scripts/rabbitmq-up.sh
#   bash tests/run_all.sh

set -eu

cd "$(dirname "$0")/.."
# shellcheck source=tests/lib.sh
source tests/lib.sh

SCRIPTS=(
    "tests/run_multi.sh"
    "tests/fault_agent.sh"
    "tests/fault_rabbitmq.sh"
)

failed=()
for s in "${SCRIPTS[@]}"; do
    echo
    echo "=========================================================="
    echo " RUN: $s"
    echo "=========================================================="

    if ! wait_broker_ready 30; then
        echo "[run_all] 브로커가 준비되지 않아 '$s' 를 건너뜁니다." >&2
        failed+=("$s (broker not ready)")
        continue
    fi
    purge_queue

    if bash "$s"; then
        echo "[run_all] OK: $s"
    else
        echo "[run_all] FAIL: $s" >&2
        failed+=("$s")
    fi
done

echo
echo "=========================================================="
if [ "${#failed[@]}" -ne 0 ]; then
    echo " SUMMARY: FAILED"
    for f in "${failed[@]}"; do
        echo "  - $f"
    done
    exit 1
fi

echo " SUMMARY: ALL PASS"
