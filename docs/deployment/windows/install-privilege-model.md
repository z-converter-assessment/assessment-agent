# Windows 설치 권한 모델 (현재 동작 스냅샷)

코드를 수정하지 않고, 현재 구현이 실제로 어떻게 동작하는지 사실만 기록한다.
출처: `windows-agent/src/installer.c`, `windows-agent/deploy/install.bat`,
`windows-agent/src/service.c`. 버전별 바이너리 매핑은 [rollout.md](rollout.md),
원격 접근은 [access.md](access.md), OS 매트릭스는
[`deploy/SUPPORTED_OS.md`](../../../deploy/SUPPORTED_OS.md) 참조.

## 설계 원칙 (의도)

가이딩 룰: 고객이 vendor 프로그램을 root로 24/7 돌리게 하지 않는다. 설치는
일회성 권한만 요구하고, 상시 런타임은 비특권으로 돈다.

Linux는 이 원칙을 양쪽 init에서 충족한다.

| 호스트 | 설치 권한 | 런타임 계정 | 부팅 기동 |
|---|---|---|---|
| Linux + systemd | 없음 (순수 user) | invoking user | `loginctl enable-linger` (best-effort) |
| Linux SysV/EL6 | root 1회 (init.d 등록) | `assessment-agent` user (su/RUNAS) | chkconfig / update-rc.d |

## Windows 실제 동작 — OS 세대에서 갈린다

Windows는 원칙이 NT10(Server 2016+)에서만 성립한다. 핵심은 installer.c의
버전 게이트다.

```
installer.c check_windows_version()  // line 117
    if (v.dwMajorVersion < 10) -> "requires 10 / Server 2016+", return 0;

installer.c installer_run_install()  // line 636
    if (!check_windows_version()) return 1;   // 게이트를 강제 — 거부 시 즉시 중단
```

통과한 modern은 user-level scheduled task 경로를 탄다(installer.c line 266):

```
schtasks /Create /SC ONSTART /RU <user> /RL LIMITED /F
    /SC ONSTART : 부팅 시 기동 (로그온 무관)
    /RU <user>  : S4U — 비밀번호 저장 없이 무인 실행. 생성에 admin 1회 필요
    /RL LIMITED : 런타임은 비특권 (승격 없음)
```

major < 10 (win7 NT6.1 / 2012 NT6.2~6.3 / 2003 NT5.2)은 이 경로가 막힌다.
이들은 `deploy/install.bat`으로 떨어진다:

- install.bat은 `net session`으로 Administrator를 요구한다.
- `"%EXE%" install`이 게이트에서 실패하므로, 스크립트는 수동 `sc create`
  (시스템 서비스) 명령을 안내한다.
- `sc create`는 `obj=` 미지정 -> LocalSystem(SYSTEM) 계정으로 등록/실행된다.
  서비스 진입점은 `service.c::service_main` (SCM).

### 결과 권한 비교

| 타깃 | 설치 경로 | 설치 권한 | 런타임 계정 | 원칙 부합 |
|---|---|---|---|---|
| Windows modern (NT10+) | installer.c / schtasks S4U | admin 1회 | 비특권 (RL LIMITED) | 부합 |
| Windows win7 / 2012 (NT6.x) | install.bat + sc create | admin | LocalSystem (SYSTEM) | 벗어남 |
| Windows 2003 (NT5.2) | install.bat + sc create | admin | LocalSystem (SYSTEM) | 벗어남 |

modern과 legacy 계열의 결정적 차이는 설치 권한(둘 다 admin 1회)이 아니라
런타임 계정이다: modern은 비특권 LIMITED, win7/2012/2003은 SYSTEM이다.

## 알려진 제약 / 미해결 (코드 수정 대상 아님 — 기록만)

1. version gate 과제한 의심: win7(NT6.1)/2012(NT6.2~6.3)는 schtasks S4U
   (`/RU /RL`)를 지원하는데도 `major < 10` 게이트가 막아 시스템 서비스 경로로
   강등된다. 즉 user-level이 기술적으로 가능한 OS까지 SYSTEM 런타임이 된다.
   의도된 정책인지 점검 필요. 2003은 PowerShell 부재 + S4U 미지원이라 시스템
   서비스가 불가피한 예외다.

2. 2003 런타임 SYSTEM: 위 제약의 불가피한 결과. 비특권 무인 부팅 수단이 없어
   LocalSystem이 유일한 현실적 선택.

3. legacy32(win2003-x86) CI 미검증: release.yml에 matrix를 추가했으나
   windows-latest MSYS2의 i686 toolchain이 NT5.2 startup을 내는지 미검증
   (experimental). 빌드 실패해도 나머지 릴리즈는 막지 않는다.

4. 2003 TLS 선결 조건: 브로커가 TLS1.3-only이거나 OpenSSL 1.0.2가 못 거는
   cipher만 제공하면 2003은 빌드와 무관하게 접속 불가
   (`docs/win2003-bringup.md` 0번 단계가 빌드보다 먼저 PoC하라고 명시).

5. install-smoke CI는 modern만 검증한다. win7/2003의 install.bat + 시스템
   서비스 경로는 자동 검증되지 않는다.

## 빌드 산출물 5종 현황 (참고)

2003 x64 Edition은 드물어 빌드하지 않는다 — x86(legacy32)이 유일한 2003 타깃이다.

| 빌드 | 타깃 최저 OS | 아키 | 바이너리 | OpenSSL |
|---|---|---|---|---|
| Linux modern | CentOS7 / glibc2.17 | x86_64 | assessment-agent-linux-x86_64 | 시스템 |
| Linux legacy | CentOS6 / glibc2.12 | x86_64 | assessment-agent-linux-x86_64-glibc2.12 | 시스템 |
| Win modern | Server 2016 | x86_64 | assessment-agent-win2016-x64.exe | 3.x |
| Win win7 | Server 2008R2 | x86_64 | assessment-agent-win2008r2-x64.exe | 3.x |
| Win legacy32 | Server 2003 x86 | i686 | assessment-agent-win2003-x86.exe | 1.0.2u |

릴리즈 에셋은 위 바이너리 + SHA256SUMS뿐이다. 설치 스크립트는 별도 에셋이
아니라 바이너리에 embed(Linux) 되거나 C 코드(Windows installer.c)에 내장된다.
릴리즈 트리거는 `v*` 태그 push 한 가지다(브랜치 push/PR은 빌드+smoke 검증만).

### 검증 상태 (2026-06-26 기준)

| 빌드 | 현재 코드 로컬 빌드 | CI | 실호스트 |
|---|---|---|---|
| Linux modern | OK (코드 반영 + SHA 정합) | 과거 smoke 통과 | - |
| Linux legacy (glibc2.12) | 로컬 빌드만 | 미검증 (experimental) | - |
| Win modern (win2016-x64) | OK (agentdbg=0) | 미실행 | - |
| Win win7 (win2008r2-x64) | OK (agentdbg=0) | 미실행 | - |
| Win legacy32 (win2003-x86) | OK (SRWLock 패치 적용, agentdbg=0) | 미실행 | TLS1.2 PoC + 런타임 미해결 |

로컬 x86_64/i686 cross-compile로 Windows 3종(modern/win7/legacy32)을 현재 코드로
빌드 검증했다. 잡아야 했던 cross 환경 함정: (1) WINDRES를 vendor-build에 주면
OpenSSL `--cross-compile-prefix`와 이중 prefix 충돌 -> release 단계에만 부여,
(2) 프로파일 전환 시 `make clean`으로 OBJ ABI 혼합 방지, (3) rabbitmq-c가
winpthread 필요 -> `EXTRA_SYS_LIBS=-lwinpthread`, (4) legacy32는 rabbitmq-c
win32/threads의 SRWLock(Vista+)을 NT5.2에 없어 `agent-fleet/win-legacy-patch`의
CRITICAL_SECTION 패치로 교체.

미해결: legacy32 빌드는 위 패치가 레포 밖에 있어 레포/CI 단독으로는 재현되지
않는다(CI legacy32 job은 패치 없이 SRWLock 링크 실패). 또 패치 적용 빌드도
win2003 실기 런타임(서비스 기동/크래시)은 별도 과제다
([`win2003-bringup.md`](../../../windows-agent/docs/win2003-bringup.md),
`agent-fleet/win-legacy-patch/README.md`). 권위있는 빌드 경로는 여전히 CI
(windows-latest + MSYS2)이며, 현재 코드 커밋은 아직 push되지 않아 CI 미실행이다.
