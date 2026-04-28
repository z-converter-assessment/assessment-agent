#!/usr/bin/env bash
#
# tests/lib.sh — 테스트 공통 헬퍼. 직접 실행 금지, source 로 로드.
#
# 제공 함수:
#   log MSG                       타임스탬프 포함 출력
#   queue_messages                현재 큐 메시지 수 (큐 없으면 0)
#   purge_queue                   큐 전체 삭제
#   wait_broker_ready [timeout]   management API 응답까지 대기 (기본 30s)
#   wait_for_queue_count N [t]    큐 메시지가 정확히 N개가 될 때까지 대기 (기본 15s)
#   wait_for_queue_min N [t]      큐 메시지가 N개 이상이 될 때까지 대기 (기본 15s)
#   consume_hostnames N           큐에서 N건 소비 후 hostname 줄 단위 출력
#   validate_messages N           큐에서 N건 소비 후 각 페이로드 스키마 검증
#
# 환경변수:
#   DOCKER_CMD   docker 실행 커맨드 (기본 "docker", sudo 필요 시 "sudo docker")
#
# 필요 툴: curl, python3

RABBITMQ_HOST="${RABBITMQ_HOST:-localhost}"
RABBITMQ_MGMT_PORT="${RABBITMQ_MGMT_PORT:-15672}"
RABBITMQ_USER="${RABBITMQ_USER:-admin}"
RABBITMQ_PASS="${RABBITMQ_PASS:-admin}"
RABBITMQ_QUEUE="${RABBITMQ_QUEUE:-server.metrics}"
RABBITMQ_VHOST="${RABBITMQ_VHOST:-/}"
DOCKER="${DOCKER_CMD:-docker}"

vhost_encoded() { local v="$RABBITMQ_VHOST"; printf '%s' "${v//\//%2F}"; }
_mgmt_url()     { printf 'http://%s:%s/api%s' "$RABBITMQ_HOST" "$RABBITMQ_MGMT_PORT" "$1"; }

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# 현재 큐 메시지 수를 stdout에 출력. 큐 미존재·API 오류 시 0.
queue_messages() {
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$RABBITMQ_QUEUE")" \
        | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    print(d.get("messages", 0) if isinstance(d, dict) else 0)
except Exception:
    print(0)
'
}

purge_queue() {
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X DELETE \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/contents")" > /dev/null
}

# 최대 timeout초 동안 management API가 응답할 때까지 폴링.
wait_broker_ready() {
    local timeout="${1:-30}"
    local deadline=$(( $(date +%s) + timeout ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if curl -fsS -u "$RABBITMQ_USER:$RABBITMQ_PASS" \
                "$(_mgmt_url /overview)" > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# 큐 메시지 수가 정확히 expected개가 될 때까지 최대 timeout초 폴링.
# stdout에 최종 count 출력. 성공=0, 타임아웃=1.
wait_for_queue_count() {
    local expected="$1" timeout="${2:-15}"
    local deadline=$(( $(date +%s) + timeout ))
    local count=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        count=$(queue_messages)
        if [ "$count" = "$expected" ]; then
            echo "$count"; return 0
        fi
        sleep 1
    done
    echo "$count"
    return 1
}

# 큐 메시지 수가 min 이상이 될 때까지 최대 timeout초 폴링.
wait_for_queue_min() {
    local min="$1" timeout="${2:-15}"
    local deadline=$(( $(date +%s) + timeout ))
    local count=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        count=$(queue_messages)
        if [ "${count:-0}" -ge "$min" ] 2>/dev/null; then
            echo "$count"; return 0
        fi
        sleep 1
    done
    echo "$count"
    return 1
}

# 큐에서 count건 소비하며 각 메시지의 hostname을 줄 단위로 출력.
consume_hostnames() {
    local count="${1:-100}"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X POST \
        -H 'Content-Type: application/json' \
        --data "$(printf '{"count":%s,"ackmode":"ack_requeue_false","encoding":"auto"}' "$count")" \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/get")" \
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

# 큐에서 count건 소비하고 각 페이로드의 스키마를 검증.
# 결과를 stdout에 출력. 실패 건이 있으면 exit 1.
validate_messages() {
    local count="${1:-1}"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X POST \
        -H 'Content-Type: application/json' \
        --data "$(printf '{"count":%s,"ackmode":"ack_requeue_false","encoding":"auto"}' "$count")" \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/get")" \
        | python3 -c '
import sys, json

# (field_path, validator) — 모든 항목이 통과해야 OK
SCHEMA = [
    ("hostname",          lambda v: isinstance(v, str) and len(v) > 0),
    ("nproc",             lambda v: isinstance(v, str) and v.strip().isdigit()),
    ("free.mem_total_mb", lambda v: isinstance(v, (int, float)) and v > 0),
    ("lsblk_raw",         lambda v: isinstance(v, list)),
    ("ip_raw.internal",   lambda v: isinstance(v, list)),
    ("ip_raw.external",   lambda v: isinstance(v, list)),
]

def get_nested(d, dotpath):
    for key in dotpath.split("."):
        if not isinstance(d, dict):
            return None
        d = d.get(key)
    return d

try:
    msgs = json.load(sys.stdin)
except Exception as e:
    sys.stderr.write("  ERROR: API 응답 파싱 실패: " + str(e) + "\n")
    sys.exit(1)

if not isinstance(msgs, list):
    sys.stderr.write("  ERROR: API가 리스트를 반환하지 않음\n")
    sys.exit(1)

fail = 0
for i, m in enumerate(msgs):
    idx = "[" + str(i + 1) + "]"
    try:
        payload = json.loads(m.get("payload", ""))
    except Exception:
        print("  " + idx + " FAIL  payload가 유효한 JSON이 아님")
        fail += 1
        continue
    host = payload.get("hostname", "<unknown>")
    errors = [p for p, check in SCHEMA if not check(get_nested(payload, p))]
    mid = m.get("properties", {}).get("message_id", "")
    if not mid:
        errors.append("properties.message_id missing")
    if errors:
        print("  " + idx + " FAIL  " + host + ": " + ", ".join(errors))
        fail += 1
    else:
        print("  " + idx + " OK    " + host + "  (id=" + mid + ")")

sys.exit(1 if fail else 0)
'
}

# 큐에서 count건을 peek(requeue)하며 message_id를 줄 단위로 출력.
consume_message_ids() {
    local count="${1:-100}"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X POST \
        -H 'Content-Type: application/json' \
        --data "$(printf '{"count":%s,"ackmode":"ack_requeue_true","encoding":"auto"}' "$count")" \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$RABBITMQ_QUEUE/get")" \
        | python3 -c '
import sys, json
try:
    msgs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
if not isinstance(msgs, list):
    sys.exit(0)
for m in msgs:
    mid = m.get("properties", {}).get("message_id", "")
    print(mid if mid else "<missing>")
'
}
