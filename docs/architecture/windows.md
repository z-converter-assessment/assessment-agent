# Windows 에이전트 — Linux 대비 차이 중심

> `windows-agent/`는 Linux 에이전트와 **같은 wire contract**([`../payload-schema.md`](../payload-schema.md))를 emit하는 독립 포팅이다.
> 빌드·설치·운영 절차는 [`../../windows-agent/README.md`](../../windows-agent/README.md) 단일 진실. 본 문서는 소스 구조가 Linux와 어디서 갈리는지만 다룬다.

## 소스 구조 — 모듈 단위 1:1 대응

```
windows-agent/src/
  main.c       서비스/콘솔 모드 분기 + 동일한 수집 루프
  service.c    (Windows 전용) SCM dispatcher — Linux에 대응물 없음
  collect.c    Win32 수집기 — /proc 대응 API 매핑
  publish.c    librabbitmq — Linux와 거의 동일 (winsock 초기화 차이)
  worker.c     task.install consumer — fork 대신 thread
  exec.c       CreateProcess + Job Object 격리
  download.c   libcurl — Linux와 동일 구조
  installer.c  self-installer — 서비스 등록·ACL·대화형 env seed
  util.c       공통 헬퍼
```

## 수집 — /proc → Win32 API 매핑

collect.c 헤더의 매핑 표 그대로:

| Linux 소스 | Windows 대응 |
|---|---|
| `/etc/machine-id` | `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` |
| `/etc/os-release` | `HKLM\...\Windows NT\CurrentVersion` |
| `/proc/cpuinfo` | CPUID brand string (`0x80000002..04`) |
| `/proc/meminfo` | `GlobalMemoryStatusEx` |
| `/proc/stat` | `GetSystemTimes` |
| `/proc/diskstats` | `DeviceIoControl(IOCTL_DISK_PERFORMANCE)` |
| `/proc/net/dev` | `GetIfTable2` |
| `/proc/net/{tcp,udp}` | `GetExtendedTcpTable` / `GetExtendedUdpTable` |
| `/proc/self/mountinfo` | `GetLogicalDriveStringsW` + `GetVolumeInformationW` |
| `getifaddrs` | `GetAdaptersAddresses` (MAC도 여기서) |
| `systemctl list-units` | `EnumServicesStatusExW` (SCM) |

단위 호환 변환 — 스키마가 Linux 기준이므로 Windows 쪽이 맞춘다:

- `cpu_stat`: FILETIME 100ns 단위 ÷ 100,000 → 10ms tick (Linux HZ=100 호환)
- `sectors_*`: BytesRead/Written ÷ 512 (diskstats 호환)

스키마 호환 null/zero (엔진은 이 규칙을 전제로 처리 — payload-schema v3.3):

| 필드 | Windows 값 | 이유 |
|---|---|---|
| `metrics.load_{1,5,15}m` | null | Windows에 load average 개념 없음 |
| `cpu_stat.{nice,iowait,irq,softirq,steal}` | 0 | 대응 카운터 없음 |
| `listen_ports[].uid` | null | POSIX uid 없음 |

`composite_id`는 같은 알고리즘(sha256(machine_id + "\n" + sorted MACs))을 같은 정규화 규칙으로 계산한다 — 양 OS가 같은 입력이면 같은 값.

## 프로세스 모델 — fork → thread, rlimit → Job Object

| 관심사 | Linux | Windows |
|---|---|---|
| 데몬화 | systemd user unit / SysV | SCM 서비스 (`service.c` dispatcher). `--console`로 포그라운드 모드 |
| 종료 신호 | SIGTERM → `g_stop` | `SERVICE_CONTROL_STOP`/`SHUTDOWN` → `InterlockedExchange` stop 플래그. 콘솔 모드는 `SetConsoleCtrlHandler` |
| task 실행 단위 | `fork()` child | `_beginthreadex` install thread (`WaitForSingleObject(thread, 0)`으로 reap 폴링) |
| install 프로세스 격리 | `clearenv`+`setrlimit`+pgid | `CreateProcessA(CREATE_SUSPENDED)` → `AssignProcessToJobObject` → `ResumeThread`. Job Object에 `KILL_ON_JOB_CLOSE` + `ProcessMemoryLimit` + `ActiveProcessLimit` |
| 환경 격리 | PATH/LANG/HOME/USER + TASK_ID/MACHINE_ID | PATH/TEMP/USERPROFILE/SystemRoot + TASK_ID/MACHINE_ID (minimal env block) |
| stdout/stderr 캡처 | pipe + select 200ms + 4KB ring | `CreatePipe` + `PeekNamedPipe` 200ms 폴링 + 4KB ring |
| timeout 에스컬레이션 | SIGTERM → +5s SIGKILL | `TerminateJobObject` → +5s `TerminateProcess` |

## install.type 분기 — OS별 역할 분담

| type | Linux | Windows |
|---|---|---|
| `shell` | tar 전개 후 `./install.sh` 실행 | `unsupported_install_type` result |
| `direct_exec` | `unsupported_install_type` result | target_file 직접 실행 (args 전달) |
| `msi` | `unsupported_install_type` result | `msiexec /i "<file>" /quiet /norestart` — exit 0과 3010(재부팅 필요) 모두 성공 |

자기 OS가 아닌 type은 양쪽 모두 즉시 result 발행 + ack — 잘못 라우팅된 task가 DLQ에 쌓이지 않는다.

## Linux 대비 의도적 단순화 (worker.c 헤더 "v1 단순화")

추후 보강이 필요한 항목으로 소스에 명시돼 있다:

- 기동 시 `/results` 스캔 후 재발행 (replay) 미구현
- `/running` 마커(mid-install 크래시 감지) 미구현
- drain의 정밀 +5s kill 타이머는 Job Object timeout으로 흡수

즉 Windows worker의 멱등성은 `/done` 마커(I1) 단층이고, Linux의 3중 마커 복구(`running`/`results`/`done` — [`worker.md`](worker.md))보다 보장이 약하다. 재구현 시 이 갭을 인지할 것.

## 배포 모델 차이

운영 호스트에 가는 파일은 `assessment-agent.exe` 하나. install/uninstall/prep-image 전부 exe 서브커맨드이고, `agent.env.example`도 RT_RCDATA 리소스로 내장된다 (Linux의 `ld -r -b binary` 임베드와 같은 발상 — [`build-release.md`](build-release.md)).

- 설치는 admin 1회 (`TokenIsElevated` 확인) — 서비스 등록(`CreateServiceW`, 자동 시작 + 5s/10s/30s 재시작 백오프), 디렉토리 ACL을 SYSTEM+Administrators로 제한, 시크릿은 `agent.env.local` 분리 + 입력 시 콘솔 echo off.
- 골든 이미지: `prep-image`가 MachineGuid 재발급, `--sysprep`이면 sysprep generalize까지. Linux의 `/etc/machine-id` 초기화와 같은 목적 (machine_id 클론 충돌 완화 — 식별 자체는 composite_id가 담당).
