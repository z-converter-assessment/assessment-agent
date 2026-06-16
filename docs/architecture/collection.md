# 수집 아키텍처 — collect.c

> inventory / metrics / error 페이로드를 만드는 수집 계층의 구현 문서.
> 와이어 포맷(필드·타입·nullable)은 [`../payload-schema.md`](../payload-schema.md) 단일 진실.
> 본 문서는 "그 값을 어디서 어떻게 읽어 오는가"를 다룬다.

## 설계 원칙이 코드로 나타나는 방식

| 원칙 (CLAUDE.md) | 구현 |
|---|---|
| Stateless | 이전 tick 값을 보관하는 전역 상태가 없다. `/proc` 누적 카운터를 그대로 emit — delta·rate 계산은 엔진 몫 |
| Raw values only | 수집기 어디에도 `pct`·`avg` 계산이 없다. 예외는 단위 표기뿐 (sectors→bytes 같은 변환도 안 함) |
| Optional은 에러가 아님 | 읽기 실패 시 `null` 또는 빈 배열. "읽기 실패(null)"와 "실제 0"을 구분한다 — `add_kb_or_null()` |
| 단일 정규화 스키마 | OS별 차이는 수집기 내부 폴백으로 흡수 (lsblk→sysfs, MemAvailable 유도). 와이어 포맷은 동일 |

수집 실패의 두 등급:

- **optional 소스 실패** — 해당 필드만 `null`/빈 배열. 메시지는 정상 발행.
- **core 소스 실패** (`/proc/meminfo`, `/proc/stat` 등) — `collect_*_payload()`가 NULL 반환 → main.c가 `server.error` 1건 emit 후 계속 진행.

## 공통 메타데이터

`add_common_metadata()`가 모든 메시지 타입에 같은 머리를 붙인다:
`message_type` / `machine_id` / `composite_id` / `os_family` / `agent_version` / `collected_at` / `hostname` / `message_id`(UUID v4) / `boot_time` / `agent_started_at`.

### boot_time — 프로세스 수명 캐시 (필수 불변)

`/proc/stat`의 `btime` 줄에서 epoch를 읽어 **프로세스 시작 시 1회 캐시**한다 (`cached_boot_time_iso`). 커널이 NTP 보정을 btime에 반영하므로, 매 cycle 재독하면 1~2초씩 흔들려 엔진의 counter-reset 감지가 오발한다. 읽기 실패도 sticky로 캐시해 메시지마다 재시도하지 않는다 (실패 시 JSON null).

`agent_started_at`은 첫 호출 시각 1회 캐시. 관측/디버깅 전용 — reset 감지는 `boot_time`만 쓴다 ([`../inventory-refresh-and-reset-detection.md`](../inventory-refresh-and-reset-detection.md) §4.1).

### composite_id — 클론 충돌 보강 식별자

```
composite_id = sha256_hex( machine_id + "\n" + mac1 + "\n" + mac2 + ... )
```

- MAC 목록은 `collect_mac_addresses()` 결과를 그대로 재사용 (lowercase·sorted·dedup·필터). Linux/Windows 에이전트가 같은 알고리즘으로 같은 값을 내야 하기 때문.
- 프로세스 수명 1회 계산 후 캐시 (`cached_composite_id` — collect.h public, main.c가 worker 큐 이름 빌드에 사용).
- 모든 NIC이 가상이면 `sha256(machine_id + "\n")`으로 식별성이 떨어진다. 이 엣지 케이스는 한계로 인정하고, composite_id 충돌 감지는 엔진 책임.

### machine_id 해석 체인

`resolve_machine_id()` — 위에서부터 시도, 전부 실패하면 agent는 exit 2:

1. `/etc/machine-id` (32-hex 검증)
2. `/var/lib/dbus/machine-id`
3. `dbus-uuidgen --get`
4. 클라우드 IMDS instance-id — `/sys/class/dmi/id/sys_vendor`로 벤더 감지(Amazon/Microsoft/Google) 후 벤더별 메타데이터 API 호출 (AWS는 IMDSv2 토큰 선행, 모두 1초 timeout). legacy "Xen" 문자열은 AWS로 간주하지 않는다 — 비-AWS Xen IaaS 오인 방지.

## inventory 수집기 카탈로그

| 필드 | 소스 | 구현 노트 |
|---|---|---|
| `os_id`·`os_version`·`os_codename` | `/etc/os-release` | KEY=VALUE 파서, 따옴표 제거. 파일 없으면 셋 다 null |
| `kernel_version` | `uname(2)` | |
| `cpu_cores` | `sysconf(_SC_NPROCESSORS_ONLN)` | |
| `cpu_model` | `/proc/cpuinfo` 첫 `model name` | |
| `mem_total_kb`·`swap_total_kb` | `/proc/meminfo` | MemTotal 실패는 core 실패 |
| `disks[]` | `lsblk -dn -b -e 7,11 -o NAME,MAJ:MIN,SIZE,TYPE -J` → 실패 시 `/sys/block` 스캔 | 아래 "디스크 수집" |
| `mounts[]` | `/proc/self/mountinfo` + `statvfs(2)` | 아래 "마운트 수집" |
| `services` | `systemctl list-units --type=service --state=running` | systemd 없으면 **null 리터럴** (빈 배열과 구분). raw unit 이름만 emit — 카테고리 분류는 엔진 책임 |
| `listen_ports[]` | `/proc/net/{tcp,udp}{,6}` + `/proc/<pid>/fd` 소켓 inode 맵 | 아래 "listen_ports" |
| `ip_internal[]` | `getifaddrs(3)` | loopback 제외, IPv4만. netmask에서 prefix를 계산해 CIDR 문자열(`"10.0.1.15/24"`)로 emit |
| `mac_addresses[]` | `/sys/class/net/<iface>/address` | lo·docker·br-·veth·virbr 제외, `type != 1`(비-Ethernet) 제외, all-zero 제외, lowercase·sort·dedup |
| `ip_external` | env `AGENT_EXTERNAL_IP` override → 클라우드 IMDS | 벤더 감지 실패·IMDS 불가 시 null 리터럴 |

### 디스크 수집 — lsblk 우선, sysfs 폴백

lsblk는 partition/disk/LVM type과 byte 단위 size를 주므로 1차. util-linux가 `-J`를 모르는 구버전(CentOS 7의 2.23)이거나 lsblk 자체가 없으면 `/sys/block` 스캔으로 폴백 — 이 경우 `type`은 항상 `"disk"`, size는 `sectors × 512`.

배제 정책은 `is_excluded_block_dev()` 한 곳에 중앙화 (loop/ram/sr/fd) — lsblk 경로·sysfs 경로·`disk_io[]`가 같은 정책을 공유한다. lsblk JSON 키가 버전에 따라 `MAJ:MIN`(pre-2.33) / `maj:min`(2.33+)으로 갈리는 것도 흡수.

`major`/`minor`를 함께 emit하는 이유: 엔진이 `disks[]`·`mounts[]`·`disk_io[]`를 디바이스명 문자열 매칭 없이 join하게 하기 위함.

### 마운트 수집 — mountinfo 파싱과 dedup

`/proc/self/mountinfo`를 한 줄씩 단일 패스 토큰 스캔으로 파싱한다 (`parse_mountinfo_line`). 위치 3(maj:min)·5(mountpoint)·`-` 뒤 첫 필드(fstype)만 캡처 — optional field(`shared:N` 등)가 몇 개든 안전.

- 가상 fs 배제: `is_excluded_fstype()` 한 곳에 중앙화 (proc/sysfs/tmpfs/overlay/squashfs/nsfs/9p/virtiofs 등) — inventory와 metrics의 `mounts[]`가 항상 같은 정책.
- **(major,minor) 기준 dedup** — bind mount(같은 디바이스, 여러 mountpoint)는 첫 등장만 남긴다.
- 용량은 `statvfs(2)` raw bytes (`f_blocks/f_bfree/f_bavail × f_frsize`). `usage_pct`는 없다 — 엔진 계산.
- fstype은 inventory에만 포함. metrics에서는 빠진다 — tick마다 변하지 않는 값을 60초 메시지에서 빼 wire 트래픽을 줄임.

### listen_ports — 소켓 inode → 프로세스 역추적

1. `/proc/<pid>/fd/*` 심링크를 전수 순회해 `socket:[inode]` → (pid, comm) 맵을 만든다 (`build_socket_owner_map`). 비특권 agent라 타 uid 프로세스의 fd 디렉토리는 EACCES — 조용히 skip하고 해당 소켓의 `pid`/`comm`은 null로 남는다. `uid`는 `/proc/net/tcp`에서 직접 나오므로 항상 채워진다.
2. `/proc/net/tcp{,6}`에서 state `0A`(LISTEN), `/proc/net/udp{,6}`에서 remote가 all-zero(unconnected = 서버형)인 소켓을 추린다.
3. 커널이 `%08X` host-order로 찍는 hex 주소를 디코딩한다 — IPv4는 LSB-first 재조립(x86_64 전제), IPv6는 4×uint32 byte-swap 후 `inet_ntop`.

## metrics 수집기 카탈로그

| 필드 | 소스 | 구현 노트 |
|---|---|---|
| `cpu_stat{user,nice,system,idle,iowait,irq,softirq,steal}` | `/proc/stat` 첫 행 | 누적 tick 그대로. 4필드 미만 파싱이면 core 실패 |
| `mem_*_kb`·`swap_*_kb` | `/proc/meminfo` | 누락 줄은 null (0 아님) |
| `mem_available_kb` | MemAvailable → 유도식 → 근사식 | 아래 "MemAvailable 3단 폴백" |
| `load_{1,5,15}m` | `/proc/loadavg` | |
| `disk_io[]` | `/proc/diskstats` 앞 14컬럼 | 커널별 14/18/20컬럼 차이를 앞 14개만 읽어 흡수. `/sys/block/<dev>` 존재 검사로 파티션 행 제거 |
| `mounts[]` | inventory와 동일 경로 (fstype만 제외) | |
| `net_io[]` | `/proc/net/dev` | `lo` 제외. bytes/packets/errors 6필드만 (`%*d`로 중간 컬럼 skip) |

### MemAvailable 3단 폴백

kernel < 3.14 (CentOS 6, CentOS 7.0~7.1)는 `/proc/meminfo`에 MemAvailable이 없다. 순서대로:

1. `/proc/meminfo`의 `MemAvailable` 그대로.
2. **커널 si_mem_available() 동일 알고리즘을 유저스페이스에서 재현** (`derive_mem_available_kb`) — `/proc/zoneinfo`의 zone별 `low` watermark 합 + reclaimable pagecache/slab. MemAvailable을 export하는 커널과 같은 값이 나온다.
3. 그것도 실패하면 (zoneinfo 불가 등) 근사식 `MemFree + Buffers + Cached` — 가용량을 과대 보고하는 거친 추정이므로 최후 수단.

## error 페이로드

`build_error_payload()` — 공통 메타데이터 + `error_code`/`error_message`/`failed_component`, 재시도 요약용 `retry_count`/`first_failed_at`/`recovered_at`(optional). error만 `collected_at`이 밀리초 정밀도다 — 장애 타임라인 재구성용. 발행 정책(언제 emit하고 언제 침묵하는지)은 [`publish.md`](publish.md)와 `.claude/CLAUDE.md` "Error publish policy" 참조.
