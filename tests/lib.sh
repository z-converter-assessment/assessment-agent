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

# 큐에서 count건 소비하고 각 페이로드의 스키마를 검증 (v3).
# 사용:  validate_messages <count> [<queue>]
#   queue 미지정 시 RABBITMQ_QUEUE 사용 (기본 server.metrics).
# 메시지의 message_type 필드로 inventory/metrics/error 스키마를 자동 적용.
validate_messages() {
    local count="${1:-1}"
    local queue="${2:-$RABBITMQ_QUEUE}"
    curl -sS -u "$RABBITMQ_USER:$RABBITMQ_PASS" -X POST \
        -H 'Content-Type: application/json' \
        --data "$(printf '{"count":%s,"ackmode":"ack_requeue_false","encoding":"auto"}' "$count")" \
        "$(_mgmt_url "/queues/$(vhost_encoded)/$queue/get")" \
        | python3 -c '
import sys, json

def is_str(v):    return isinstance(v, str) and len(v) > 0
def is_int(v):    return isinstance(v, int) and not isinstance(v, bool)
def is_intlike(v):return isinstance(v, (int, float)) and not isinstance(v, bool)
def is_list(v):   return isinstance(v, list)
def is_obj(v):    return isinstance(v, dict)
def is_kbnull(v): return v is None or (is_intlike(v) and v >= 0)

# 공통 메타데이터 (모든 메시지)
COMMON = [
    ("message_type",  is_str),
    ("machine_id",    is_str),
    ("agent_version", is_str),
    ("collected_at",  is_str),
    ("hostname",      is_str),
    ("message_id",    is_str),
]

INVENTORY = COMMON + [
    ("kernel_version", is_str),
    ("cpu_cores",      lambda v: is_int(v) and v > 0),
    ("mem_total_kb",   lambda v: is_int(v) and v > 0),
    ("swap_total_kb",  is_kbnull),
    ("disks",          is_list),
    ("mounts",         is_list),
    ("services",       lambda v: v is None or is_list(v)),  # v3: array|null
    ("listen_ports",   is_list),                              # v3
    ("ip_internal",    is_list),
    ("boot_time",      is_str),
]

METRICS = COMMON + [
    ("cpu_stat",         is_obj),
    ("mem_total_kb",     lambda v: is_int(v) and v > 0),
    ("mem_free_kb",      is_kbnull),
    ("mem_available_kb", is_kbnull),
    ("load_1m",          is_intlike),
    ("load_5m",          is_intlike),
    ("load_15m",         is_intlike),
    ("disk_io",          is_list),
    ("mounts",           is_list),
    ("net_io",           is_list),
]

ERROR = COMMON + [
    ("error_code",       is_str),
    ("error_message",    lambda v: isinstance(v, str)),
    ("failed_component", is_str),
]

# v3 추가 검증: 항목별 필드
def validate_v3_arrays(p, errors):
    # disks[]: name, major, minor (옵셔널), size_bytes, type
    for i, d in enumerate(p.get("disks", [])):
        loc = "disks[%d]" % i
        if not is_str(d.get("name")):           errors.append(loc + ".name")
        if not is_intlike(d.get("size_bytes")): errors.append(loc + ".size_bytes")
        if not is_str(d.get("type")):           errors.append(loc + ".type")
    # mounts[]: mount, major, minor, total/free/avail bytes (+ inventory: fstype)
    for i, m in enumerate(p.get("mounts", [])):
        loc = "mounts[%d]" % i
        if not is_str(m.get("mount")):           errors.append(loc + ".mount")
        if not is_intlike(m.get("total_bytes")): errors.append(loc + ".total_bytes")
        if not is_intlike(m.get("free_bytes")):  errors.append(loc + ".free_bytes")
        if not is_intlike(m.get("avail_bytes")): errors.append(loc + ".avail_bytes")
    # listen_ports[]: proto, addr, port, uid, pid|null, comm|null (v3)
    for i, lp in enumerate(p.get("listen_ports") or []):
        loc = "listen_ports[%d]" % i
        if lp.get("proto") not in ("tcp", "tcp6", "udp", "udp6"):
            errors.append(loc + ".proto")
        if not is_str(lp.get("addr")):              errors.append(loc + ".addr")
        if not (is_int(lp.get("port")) and 0 < lp.get("port") < 65536):
            errors.append(loc + ".port")
        if not is_int(lp.get("uid")):               errors.append(loc + ".uid")
    # services[]: unit, sub  (or null)
    services = p.get("services")
    if isinstance(services, list):
        for i, s in enumerate(services):
            loc = "services[%d]" % i
            if not is_str(s.get("unit")): errors.append(loc + ".unit")
            if not is_str(s.get("sub")):  errors.append(loc + ".sub")

SCHEMAS = {"inventory": INVENTORY, "metrics": METRICS, "error": ERROR}

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
    mt = payload.get("message_type", "?")
    schema = SCHEMAS.get(mt)
    if schema is None:
        print("  " + idx + " FAIL  알 수 없는 message_type=" + str(mt))
        fail += 1
        continue
    errors = [p for p, check in schema if not check(payload.get(p))]
    if mt in ("inventory", "metrics"):
        validate_v3_arrays(payload, errors)
    if errors:
        print("  " + idx + " FAIL  " + mt + " " + host + ": " + ", ".join(errors))
        fail += 1
    else:
        print("  " + idx + " OK    " + mt + " " + host)

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
