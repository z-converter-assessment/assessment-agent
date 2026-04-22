#!/usr/bin/env bash
#
# tests/lib.sh
# ------------
# 테스트 스크립트 공통 헬퍼. 직접 실행용이 아니라 `source`로 로드한다.
#
# 제공 함수:
#   queue_messages         현재 RABBITMQ_QUEUE 의 메시지 수를 출력
#   purge_queue            큐 메시지 전부 삭제
#   wait_broker_ready N    최대 N초 동안 management API가 응답할 때까지 대기
#   consume_hostnames N    큐에서 최대 N건을 소비하며 각 메시지의 hostname을 줄 단위로 출력
#
# 필요 툴: curl, python3 (JSON 파싱에 사용)

RABBITMQ_HOST="${RABBITMQ_HOST:-localhost}"
RABBITMQ_MGMT_PORT="${RABBITMQ_MGMT_PORT:-15672}"
RABBITMQ_USER="${RABBITMQ_USER:-admin}"
RABBITMQ_PASS="${RABBITMQ_PASS:-admin}"
RABBITMQ_QUEUE="${RABBITMQ_QUEUE:-server.metrics}"
RABBITMQ_VHOST="${RABBITMQ_VHOST:-/}"

# "/"를 %2F로 인코딩 (management API 요구)
vhost_encoded() {
    local v="$RABBITMQ_VHOST"
    printf '%s' "${v//\//%2F}"
}

_mgmt_url() {
    printf 'http://%s:%s/api%s' "$RABBITMQ_HOST" "$RABBITMQ_MGMT_PORT" "$1"
}

queue_messages() {
    local path="/queues/$(vhost_encoded)/$RABBITMQ_QUEUE"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" "$(_mgmt_url "$path")" \
        | python3 -c 'import sys, json; d=json.load(sys.stdin); print(d.get("messages", "?"))'
}

purge_queue() {
    local path="/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/contents"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X DELETE "$(_mgmt_url "$path")" > /dev/null
}

consume_hostnames() {
    local count="${1:-100}"
    local path="/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/get"
    local body
    body=$(printf '{"count":%s,"ackmode":"ack_requeue_false","encoding":"auto"}' "$count")
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X POST \
        -H 'Content-Type: application/json' \
        --data "$body" \
        "$(_mgmt_url "$path")" \
        | python3 -c '
import sys, json
try:
    msgs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
if not isinstance(msgs, list):
    sys.exit(0)
for m in msgs:
    try:
        p = json.loads(m.get("payload", ""))
        h = p.get("hostname")
        if isinstance(h, str):
            print(h)
    except Exception:
        pass
'
}

wait_broker_ready() {
    local timeout="${1:-30}"
    local deadline=$(( $(date +%s) + timeout ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if curl -fsS -u "$RABBITMQ_USER:$RABBITMQ_PASS" "$(_mgmt_url /overview)" > /dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}
