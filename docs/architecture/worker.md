# Worker — task.install 수신·격리 실행·result 회신

> worker.c(상태 머신) · download.c(취득) · extract.c(전개) · exec.c(실행)의 구현 문서.
> 메시지 스키마·큐 토폴로지·권한은 [`../payload-schema.md`](../payload-schema.md)와 `.claude/CLAUDE.md` 단일 진실.
> 설계 당시 의사결정 기록은 [`../archive/worker-task-design.md`](../archive/worker-task-design.md) (역사 자료).

핵심 설계 결정 요약: 단일 바이너리·단일 프로세스·task당 fork(B-f), 호스트당 동시 1건(M1), 1분 tick `basic_get` 폴링(T1), 실행 종료 후 ack(K3), result 파일 선기록(R2), `/done` 마커 멱등성(I1), drain(S2), 비특권 동일 유저 실행(U1), 환경 격리(EC1).

## 상태 머신과 tick

```
IDLE  ── basic_get으로 task 수신, fork ──▶ BUSY
BUSY  ── child reap + result 발행 완료 ──▶ IDLE
DRAIN ── SIGTERM 후. 신규 수신 없음, in-flight만 마무리
```

`worker_tick()`은 메인 루프가 1분마다 부르며, 순서가 곧 복구 전략이다:

1. **reap 먼저** (`reap_child_only`) — AMQP 상태와 무관하게 `waitpid(WNOHANG)`. broker가 죽어도 OS child를 거두므로 drain이 좀비 때문에 영원히 돌지 않는다.
2. **재연결** (`reconnect_if_dead`) — 연결이 dead면 지수 백오프(1→2→…→60초 상한) 안에서 재수립. 재연결되면 이전 채널의 delivery_tag는 무효이므로 0으로 버린다 — broker가 미ack 메시지를 redeliver하고, `/running` 마커가 그 redelivery를 막는다.
3. **heartbeat pump** (`publish_conn_pump`) — transport 사망 감지 시 dead 마킹.
4. **미발행 result 처리** (`publish_reaped_result`) — 직전 tick의 발행 실패분 포함. `child_pid == 0`인데 `inflight_task_id`가 남아 있으면 전부 여기서 재시도한다.
5. **신규 task 시도** (`try_pick_new_task`) — drain 아님 + child 없음 + 미발행 result 없음일 때만.

## 내구성 — 디스크가 진실, broker는 전달자

상태 디렉토리(`WORKER_STATE_DIR`, 기본 `/var/lib/agent-worker`, 0700/0600):

| 디렉토리 | 의미 | 수명 |
|---|---|---|
| `running/<task_id>.json` | install이 지금 진행 중 (fork **직전**에 기록) | child reap + result 발행 완료 시 삭제 |
| `results/<task_id>.json` | child가 쓴 result. **발행 전까지의 진실** | 발행 성공 시 `done/`으로 rename |
| `done/<task_id>.json` | 완료 마커 = 멱등성 게이트 (I1) | `WORKER_DONE_RETENTION_SEC` (기본 7일) 후 purge |

모든 기록은 `write_file_atomic()` — tmp에 쓰고 fsync, rename, **부모 디렉토리까지 fsync**. 디렉토리 fsync가 없으면 rename 자체가 전원 단절에서 증발해 redelivery가 같은 install을 재실행한다.

**순서 불변식** (synth failure 경로 포함): `/done` 선기록 → 발행 → ack. `/done` 기록이 실패하면(ENOSPC·RO mount) 발행하지 않는다 — ack만 되고 마커가 없으면 미래의 redelivery가 재실행되는 구멍이 열리기 때문.

### 기동 시 복구 시퀀스 (`worker_init`)

1. `purge_stale_workspaces` — `/tmp/agent-task-*` 잔해 삭제.
2. `replay_pending_results` — `/results`에 남은 파일(이전 인스턴스가 발행 못 한 것)을 발행 후 `/done`으로 이동. 파일명은 task_id 검증을 통과해야만 처리 — `.tmp` 잔해·수작업 파일은 건드리지 않는다.
3. `recover_stale_running` — `/running` 마커가 남았으면 이전 인스턴스가 install 도중 죽은 것. `/done`·`/results`에 이미 결과가 있으면 그것이 진실이므로 마커만 지우고, 없으면 `internal_error` synth result를 `/done` 선기록 후 best-effort 발행.
4. `purge_expired_done` — 보존 기한 지난 마커 삭제.

### 실패·회복 매트릭스가 코드와 닿는 곳

| 시나리오 | 처리 코드 경로 |
|---|---|
| child가 result 파일 없이 죽음 (signal/OOM) | `publish_reaped_result` → synth `internal_error` |
| result 발행 실패 (broker 단절) | `inflight_task_id` 유지 → 다음 tick 재시도, 끝내 못 하면 다음 기동 replay |
| 발행 성공 후 ack 실패 | **성공으로 처리** — result는 이미 발행됐고 `/done`이 durable. 연결만 dead 마킹. redelivery는 I1이 `already_done`으로 응답 |
| `/results→/done` rename 실패 | ack하지 않음 — broker가 들고 있다가 redeliver, 다음 기동 replay가 정리 |
| 진행 중 redelivery (consumer_timeout) | `/running` 마커 감지 → ack만 하고 drop (원 부모가 result를 발행할 것) |
| fork 실패 (EAGAIN) | synth 실패 발행. synth가 `/done`을 못 쓰면 `/running`을 남겨 다음 기동 복구에 위임 |
| 잘못된 task (JSON 파싱 불가·task_id 비정상) | ack 후 drop — 재전송해도 같은 결과인 메시지를 큐에 남기지 않는다 |

## child 파이프라인 (`child_run_task`)

fork 직후 가장 먼저:

- `setpgid(0,0)` — 자기 프로세스 그룹 리더가 된다. 부모의 drain 에스컬레이션 `kill(-child_pid, …)`의 표적이 되기 위함 (부모도 race 방지용으로 `setpgid(pid,pid)`를 중복 호출 — 둘 중 하나만 성공하면 됨).
- `close_inherited_fds()` — broker TLS 소켓 등 부모 fd 일괄 차단. `FD_CLOEXEC`는 execve 시점에만 발동하므로, libcurl/libarchive가 그 전에 fork할 수 있는 child 단계 진입 즉시 닫는다.

검증 게이트 (전부 fork 후, 다운로드 전):

- `task_id_valid()` — UUID v4 문법(또는 32-hex)만 허용. 디렉토리 탈출·셸 메타문자 차단의 1차 방어.
- `machine_id` 불일치 → `internal_error` result (portal이 라우팅 오류를 인지하도록 침묵하지 않음).
- `install.type != "shell"` → `unsupported_install_type` result — Windows용 `direct_exec`/`msi`가 잘못 라우팅돼도 DLQ에 쌓이지 않는다.

이후 `/tmp/agent-task-<task_id>/` workspace에서 **download → extract → exec** 순서로 진행하고, 어떤 결과든 result JSON을 `results/`에 원자적으로 기록한 뒤 workspace를 지우고 `_exit(0)`.

### 1. download.c — 신뢰 경계의 입구

검사 순서: size 상한(16GB sanity cap — portal 오타·악의적 INT64_MAX로 인한 오버플로 차단) → 스킴 검사 → **호스트 화이트리스트**(`WORKER_DOWNLOAD_ALLOWED_HOSTS`, 대소문자 무시 정확 일치, 비어 있으면 전부 차단 = worker 사실상 비활성) → 디스크 여유 pre-flight(`statvfs` 오버플로 가드, fail-closed).

전송 자체의 방어:

- 목적지 파일은 `unlink` 후 `O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC`, 0600 — 심링크 follow·동시 생성 race 차단.
- libcurl: **redirect 금지**(RD2), HTTPS는 peer+hostname verify, connect timeout 10초, 60초간 1KB/s 미만이면 중단.
- HTTP도 허용한다 (v3.4) — 사내망 ZDM처럼 TLS 인증서 없는 신뢰 채널 지원. 무결성은 sha256 스트리밍 검증이 흡수한다. 전제: sha256 자체는 AMQPS로 보호된 task.install 메시지로 도착.
- write callback이 파일 기록과 `EVP_DigestUpdate`를 동시에 수행(tee)하며 `size_bytes` 초과 시 전송을 중단시킨다. 종료 후 기록 바이트 수가 기대치와 **정확히 같아야** 하고, sha256이 일치해야 한다. 모든 실패 분기는 부분 파일을 unlink.

### 2. extract.c — TP1 전개 정책

엔트리를 **쓰기 전에** 검사한다. 첫 위반에서 전체 중단:

- 경로 안전성: 절대경로·`..` 세그먼트 거부 (`extract_path_safe`) + libarchive `SECURE_NODOTDOT`·`SECURE_SYMLINKS` 이중 방어.
- 타입 화이트리스트: 일반 파일·디렉토리만. symlink/hardlink/device/FIFO/socket 거부 — filetype이 regular로 위장해도 `archive_entry_hardlink/symlink` 필드를 별도 검사.
- 권한 `mode &= 0777` (setuid/setgid/sticky 박탈), owner는 agent 프로세스 uid/gid로 강제, ACL·xattr·fflags 제거.
- 포맷: tar + gzip/bzip2/xz/zstd 필터.

### 3. exec.c — EC1 실행 격리

부모(=worker child)가 pipe 2개를 열고 fork, grandchild가 bootstrap 후 `execve("./install.sh")`:

- `chdir(전개 디렉토리)`, stdin=`/dev/null`, stdout/stderr→pipe.
- `clearenv()` 후 화이트리스트만 재구성: `PATH`/`LANG`/`HOME=/tmp`/`USER=agent` + `TASK_ID`/`MACHINE_ID`.
- `setrlimit`: `RLIMIT_AS`(메모리)·`RLIMIT_FSIZE`(파일 크기)는 MB→bytes, `RLIMIT_NOFILE`(fd 수)·`RLIMIT_CPU`(=timeout초)는 raw 값.
- `signal(SIGPIPE, SIG_DFL)` 복원 — SIG_IGN은 execve를 넘어 상속되므로, 복원하지 않으면 `yes | foo` 류 파이프라인을 쓰는 install 스크립트가 행에 걸린다.
- bootstrap 실패는 `_exit(124)` — 126(not executable)/127(not found)/128+N(signal)과 겹치지 않는 코드라 부모가 "환경 구성 실패"와 "스크립트 자체 실패"를 구분한다.
- **grandchild는 setsid하지 않는다.** 새 세션을 만들면 agent의 drain 에스컬레이션(`kill(-worker_child_pgid)`)이 install.sh에 닿지 않아 orphan으로 살아남는다. 같은 이유로 timeout kill은 단일 프로세스 `kill(pid)`다.

부모는 200ms select 루프로 양 pipe를 비동기 드레인하며 (EOF난 fd는 set에서 제거해 5Hz 공회전 방지), 4KB **ring buffer**로 마지막 4KB tail만 보존한다(비인쇄 바이트는 `?`로 치환). timeout 도달 시 SIGTERM, +5초 후 SIGKILL — `RLIMIT_CPU`가 CPU-bound 폭주를 별도로 캡한다.

결과 매핑: exit 0 → `EXEC_OK` / 124 → `internal_error` / 그 외 exit → `script_failed` / signal 종료는 term_sent 여부에 따라 `script_timeout` 또는 `script_failed`.
