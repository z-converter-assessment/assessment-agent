# assessment-agent — Windows agent

Linux agent 와 동일한 contract 로 동작하는 Windows 포팅. inventory / metrics / error
publish + task.install consumer (v2 — `direct_exec` / `msi`) 모두 지원.

루트 [`../README.md`](../README.md) 의 메시지 contract / 토폴로지 / payload schema
는 양 OS 공통. 이 문서는 **Windows specific** 운영 가이드.

---

## 지원 환경

- **OS**: Windows 10 (1809+) / Windows Server 2016 이상 (NT 10.0+) — `modern` 프로파일
- **아키텍처**: x86_64 (`modern` / `win7` / `legacy`), **x86 32-bit (`legacy32`)**
  - 32-bit 은 NT 5.2 전용 — Server 2003 의 일반 SKU(x86)는 `legacy32` 로만 설치 가능.
    `legacy`(x64) 바이너리는 32-bit 호스트에 아예 로드되지 않는다.
- **권한**: Service 등록은 Administrator. Service runtime 계정은 LocalSystem (기본 —
  `install.ps1` 가 `New-Service` 로 등록 시 default)

구세대(Server 2012 R2 / Windows 7 → `win7`, Server 2003 / XP → `legacy`·`legacy32`)
는 SCM API / TLS / SHA256 표면 차이로 빌드는 가능해도 runtime 호환 보장 안 함 —
프로파일·검증 절차는 `docs/win2003-bringup.md` 참조.

---

## 빌드

### 운영자 (Windows 머신에서, admin PowerShell)

```powershell
PS> cd path\to\assessment-agent
PS> .\windows-agent\scripts\build-windows.ps1
```

내부 동작 (`build-prep.ps1` → `make vendor-build` → `make release`):

1. MSYS2 / mingw64 toolchain 자동 설치 (10분, 첫 실행만)
2. vendor 6종 정적 빌드 — `OpenSSL 3.0.15`, `zlib 1.3.1`, `cJSON 1.7.18`,
   `rabbitmq-c 0.15.0`, `libcurl 8.10.1`
3. `assessment-agent.exe` 빌드 + verify (DLL 화이트리스트 검사) + dist 패키징

결과물:
```
windows-agent\dist\assessment-agent.exe
windows-agent\dist\SHA256SUMS
```

### CI (GitHub Actions)

`.github/workflows/release.yml` 의 `Windows (MSYS2 / mingw64)` job —
`windows-latest` runner + `msys2/setup-msys2@v2` MINGW64. push trigger (develop /
main / tag v*) + pull_request trigger 에서 자동 빌드 + artifact upload.

tag `v*` push 시 GitHub Release 에 binary + SHA256SUMS 자동 첨부.

### Vendor 호환 fix (참고)

vendor 빌드는 MSYS2 mingw64 의 최신 환경에서 작동하도록 `Makefile` 에 명시적
disable 옵션 다수:

- `OpenSSL`: `--libdir=lib` (host triplet 별 디렉토리 분기 회피)
- `rabbitmq-c`: `-Wno-error=incompatible-pointer-types` (GCC 14+ + ioctlsocket 미스매치) + 출력 `liblibrabbitmq.<N>.a` → `librabbitmq.a` 별칭 cp
- `zlib`: `Makefile.gcc` ARFLAGS 중복 회피
- `libcurl`: `-DCURL_STATICLIB` + `CMAKE_DISABLE_FIND_PACKAGE_LibPSL/Libssh2/LibSSH/Libidn2/LibIDN2/Brotli/Zstd/NGHTTP2/nghttp2` (MSYS2 auto-detection 차단)
- cmake 4.x 호환: `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`

상세는 `windows-agent/Makefile` 참고.

---

## 배포

운영 호스트에 복사하는 파일은 **`assessment-agent.exe` 한 개**. install /
uninstall / 골든 이미지 준비까지 모두 exe 의 서브커맨드. PowerShell 스크립트
없음, agent.env.example 도 exe 내부에 RT_RCDATA 로 박힘.

### 신규 설치 (Server 측, 한 방)

```powershell
# 1. CI 산출물 dist\assessment-agent.exe 만 서버에 복사
# 2. admin PowerShell 에서:
PS> .\assessment-agent.exe install
```

`install` 동작 (idempotent — 재실행 안전):

1. Administrator 권한 확인 (`TokenIsElevated`)
2. Windows 버전 확인 (`RtlGetVersion` ≥ 10)
3. 기존 service stop (있으면) — 30초 polling 후 exe 교체
4. `C:\Program Files\assessment-agent\` + `C:\ProgramData\assessment-agent\` +
   `C:\ProgramData\assessment-agent\worker\{results,done,running}\` 생성
   (`SHCreateDirectoryExW`)
5. 자기 자신 → `C:\Program Files\assessment-agent\assessment-agent.exe` 복사
6. `agent.env` seed (임베디드 example) + 빈 키만 대화형 prompt.
   시크릿(`RABBITMQ_PASS`, `RABBITMQ_WORKER_PASS`) 은 `agent.env.local` 에
   분리, 입력 시 콘솔 echo off (`SetConsoleMode & ~ENABLE_ECHO_INPUT`)
7. data dir ACL tighten (`SetNamedSecurityInfoW`) — SYSTEM + Administrators 만
8. `assessment-agent` service 등록 (`CreateServiceW`, StartupType Automatic)
   + 자동 재시작 정책 (`ChangeServiceConfig2W` / 5s, 10s, 30s backoff, 1 day reset)
9. service 시작 + RUNNING 도달 확인

### 골든 이미지 준비 (VM 템플릿)

```powershell
# 1. service 등록만, 시작 X
PS> .\assessment-agent.exe install --image-prep

# 2. MachineGuid 재발급 (machine_id 충돌 방지)
PS> .\assessment-agent.exe prep-image
#    또는 더 철저히 (sysprep /generalize /oobe /shutdown):
PS> .\assessment-agent.exe prep-image --sysprep
```

**중요**: prep-image 안 하면 clone 된 VM 들이 모두 같은 `MachineGuid` 를 들고
나옴. engine 은 `composite_id`(= `sha256(machine_id + sorted MACs)`) 로 호스트
식별하므로 MAC 만 달라도 분기 가능하지만, `MachineGuid` 단독 중복은 운영 가시성
저해. `--sysprep` 옵션이 가장 안전 (SID·hostname·license 까지 generalize).

### 업그레이드

새 `assessment-agent.exe` 로 `install` 다시 실행. 기존 service stop → exe
교체 → restart. agent.env / agent.env.local 은 보존되고, 빈 키만 다시 prompt.

### 제거

```powershell
PS> .\assessment-agent.exe uninstall
```

service stop + delete. on-disk state (`C:\Program Files\assessment-agent\`,
`C:\ProgramData\assessment-agent\`) 는 보존 — 재설치 시 설정 유지.
완전 제거가 필요하면 두 디렉토리를 수동 삭제.

---

## 환경 변수 (`agent.env` / `agent.env.local`)

`agent.env` 는 평문 설정, `agent.env.local` 은 시크릿 (`RABBITMQ_PASS`,
`RABBITMQ_WORKER_PASS`). ACL 로 SYSTEM + Administrators 만 접근.

핵심 변수 (전체 목록은 `deploy/agent.env.example` 참고):

| 변수 | 기본값 | 설명 |
|---|---|---|
| `RABBITMQ_HOST` | `localhost` | broker hostname |
| `RABBITMQ_PORT` | `5671` | AMQPS in production |
| `RABBITMQ_VHOST` | `/assessment` | dedicated vhost |
| `RABBITMQ_USER` / `_PASS` | — | collector publisher (`agent-publisher`) |
| `RABBITMQ_WORKER_USER` / `_PASS` | — | task.install consumer (`agent-worker`). 미설정 시 worker 비활성 |
| `RABBITMQ_TLS_ENABLED` | `true` | AMQPS |
| `AGENT_INTERVAL_SEC` | `60` | metrics 주기 |
| `AGENT_INVENTORY_REFRESH_SEC` | `3600` | inventory 재발행 주기 (jitter ±15%) |
| `AGENT_DRAIN_GRACE_SEC` | `600` | Service Stop 후 install 정상 종료 최대 대기 |
| `AGENT_DRAIN_TERM_SEC` | `30` | grace 만료 후 soft term → hard kill 사이 |
| `AGENT_DRAIN_PUBLISH_SEC` | `180` | install 끝난 후 result publish 최대 대기 |
| `WORKER_DOWNLOAD_ALLOWED_HOSTS` | — | task.install 다운로드 host 화이트리스트 (CSV, 정확 매치) |
| `WORKER_TASK_EXCHANGE` | `assessment.tasks` | engine / portal 토폴로지와 동기 |
| `WORKER_STATE_DIR` | `C:\ProgramData\assessment-agent\worker` | results / done / running 마커 위치 |
| `WORKER_INSTALL_MEM_LIMIT_MB` | `2048` | Job Object ProcessMemoryLimit |
| `WORKER_INSTALL_ACTIVE_PROC_LIMIT` | `32` | Job Object ActiveProcessLimit |
| `AGENT_EXTERNAL_IP` | — | cloud metadata IMDS 우회 (수동 외부 IP override) |

shell 환경변수가 `.env` 파일 값을 override.

---

## Service 운영

```powershell
# 상태 / 제어
PS> Get-Service assessment-agent
PS> Start-Service assessment-agent
PS> Stop-Service  assessment-agent

# 로그 (Event Log)
PS> Get-EventLog -LogName Application -Source assessment-agent -Newest 50

# 또는 PowerShell 7+
PS> Get-WinEvent -LogName Application -MaxEvents 50 |
    Where-Object { $_.ProviderName -match 'assessment-agent' }

# 자동 재시작 정책 확인
PS> sc.exe qfailure assessment-agent

# 제거
PS> Stop-Service assessment-agent
PS> sc.exe delete assessment-agent
PS> Remove-Item 'C:\Program Files\assessment-agent' -Recurse -Force
PS> Remove-Item 'C:\ProgramData\assessment-agent' -Recurse -Force
```

---

## Linux 와의 차이 (의도된)

| 항목 | Linux | Windows |
|---|---|---|
| `install.type` 처리 | `shell` (tar + install.sh) | `direct_exec` (EXE 직접) / `msi` (msiexec) |
| `shell` 수신 시 | 처리 | `unsupported_install_type` reject |
| process spawn | `fork() + execve()` | `CreateProcessW(CREATE_SUSPENDED)` + Job Object |
| 자식 종료 강제 | `kill(-pgid, SIG)` + `RLIMIT_CPU` | `TerminateJobObject` |
| 리소스 제한 | `setrlimit` (AS / FSIZE / CPU / NOFILE) | Job Object (ProcessMemoryLimit / ActiveProcessLimit) |
| State dir | `/var/lib/agent-worker/` | `%ProgramData%\assessment-agent\worker\` |
| machine_id 소스 | `/etc/machine-id` (+ `dbus-uuidgen` / IMDS fallback) | Registry `HKLM\...\Cryptography\MachineGuid` (+ IMDS fallback) |
| metrics.load_*m | `/proc/loadavg` | always `null` (Windows 에 동등 개념 없음) |
| cpu_stat 일부 | `/proc/stat` 8필드 | `nice/iowait/irq/softirq/steal` 항상 0 |
| listen_ports.uid | POSIX uid | always `null` |
| 환경 격리 | `clearenv` + whitelist | `CreateProcessW lpEnvironment` minimal block |
| dir fsync | `open(O_DIRECTORY) + fsync` | `MoveFileExA` (atomic rename) |

Wire format / payload schema 는 양 OS 동일 — `os_family` 필드로 engine 이 정제 분기.

---

## Troubleshooting

### MachineGuid 충돌 (이미지 클론)

증상: engine 의 server_inventory 에 같은 `machine_id` 가 여러 호스트로 나오는데
hostname 만 다름.

원인: 골든 이미지를 만들 때 image-prep 안 함. 클론된 VM 들이 모두 동일 GUID 를
상속.

해결:
1. (사후 처리) 각 클론 VM 에서 `scripts\image-prep.ps1` 실행 → service restart →
   재발급된 MachineGuid 로 신규 host 로 등록
2. (예방) 골든 이미지 만들기 전 반드시 `image-prep.ps1 -RunSysprep` 호출 후 snapshot

부가 안전망: agent 가 발행하는 `composite_id` = `sha256(machine_id + sorted MACs)`
이므로 MAC 만 달라도 engine 이 호스트 분기 가능. 가상 NIC 만 있는 환경에서는
이 fallback 도 실패 — 한계로 수용.

### vendor 빌드 실패

```
ld.exe: cannot find vendor/curl/build/lib/libcurl.a
```

원인 후보:
- MSYS2 pacman 패키지가 너무 최신 → vendor 호환 깨짐. `make vendor-clean` 후
  재빌드
- libcurl 의 새 선택적 의존 (psl / ssh2 / idn2 등) 이 활성화됨 → `Makefile` 의
  `CMAKE_DISABLE_FIND_PACKAGE_*` 항목이 모두 들어가 있는지 확인

```
amqp_socket.c:309: error: incompatible pointer types
```

원인: MSYS2 mingw64 의 GCC 14+ 가 `ioctlsocket(int *) vs u_long *` 미스매치를
warning → error 로 promote. `Makefile` 의 `vendor-build-rabbitmq` 에
`-Wno-error=incompatible-pointer-types` 가 있는지 확인.

### Service Stop 가 너무 오래 걸림

진행 중 install 이 있으면 `AGENT_DRAIN_GRACE_SEC=600` (10분) 까지 기다린 후
`worker_force_child_term` 이 Job Object 단위로 강제 종료. SCM 의
`TimeoutStopSec` 가 그보다 짧으면 SCM 이 강제 kill → agent 자체 종료 → Job 의
`KILL_ON_JOB_CLOSE` 가 자식 다 정리.

`drain` 중에는 service.c 의 `service_stop_pending_update` 가 2.5s 마다 SCM 에
"진행 중" 신호 (`dwCheckPoint` 증분) 를 보내므로 SCM 은 stuck 판정 안 함.

급한 종료가 필요하면 `AGENT_DRAIN_GRACE_SEC` 을 짧게 (예: `60`) 변경.

### task.install 받았는데 처리 안 됨

`RABBITMQ_WORKER_USER` / `_PASS` 가 비어있으면 worker 비활성 (collector publish-only).
agent 로그 시작 부분에 `worker disabled (RABBITMQ_WORKER_USER/PASS 미설정)`
출력 확인. 설정 후 service restart.

### 다운로드가 화이트리스트 위반

```
task.result.failure_reason = "url_not_allowed"
```

`WORKER_DOWNLOAD_ALLOWED_HOSTS` (CSV, 정확 매치) 에 해당 호스트명 추가 →
service restart. 와일드카드 / 서브도메인 매칭 안 됨.
