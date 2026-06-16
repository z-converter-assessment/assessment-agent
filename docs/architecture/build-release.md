# 빌드·릴리스·설치 — Makefile · installer.c · deploy/

> "어떤 호스트에서도 도는 단일 바이너리"를 만드는 빌드 체계와, 그 바이너리가 자기 자신을 설치하는 구조.
> OS 지원 매트릭스는 [`../../deploy/SUPPORTED_OS.md`](../../deploy/SUPPORTED_OS.md), Windows 빌드는 [`../../windows-agent/README.md`](../../windows-agent/README.md) 단일 진실.

## 빌드 2경로

| | dev (기본) | release (`USE_VENDORED=1`) |
|---|---|---|
| 라이브러리 | pkg-config로 시스템 라이브러리 동적 링크 | `vendor/` 정적 링크 |
| 용도 | 개발·단위 테스트 | 배포 산출물 |
| 남는 동적 의존 | 시스템 상황에 따름 | **glibc 계열만** (libc/libpthread/libdl/libm/libresolv/librt) — `make verify`가 강제 |

vendored 6종 (버전 고정): cJSON 1.7.18 · rabbitmq-c 0.15.0 · curl 8.10.1 · libarchive 3.7.7 · OpenSSL 3.0.15 · zlib 1.3.1.

```sh
# 컨테이너 빌드 (권장 — 호스트에 Docker만 있으면 됨)
./scripts/build-linux.sh

# 네이티브 amd64 Linux 빌드 호스트
sudo bash scripts/build-prep.sh         # apt/yum 자동 감지 후 toolchain 설치
make vendor-fetch && make vendor-build  # OpenSSL·zlib 먼저, 나머지가 그걸 링크
make USE_VENDORED=1 release
```

빌드 순서가 중요하다: OpenSSL + zlib을 먼저 빌드하고, curl/libarchive/rabbitmq-c의 cmake에 `OPENSSL_ROOT_DIR` + `OPENSSL_USE_STATIC_LIBS`를 줘서 **호스트 배포판의 libssl을 줍지 않게** 한다 — ABI 이식성의 핵심. 결과 바이너리는 PIE + full RELRO + BIND_NOW.

릴리스 산출물은 반드시 **네이티브 amd64 Linux**에서 — ARM Mac의 QEMU 에뮬레이션 빌드는 개발용으로만.

## 스크립트 임베드 — dist 디렉토리 없는 단일 파일 배포

deploy 스크립트 7종(install/uninstall/image-prep.sh, lib/{detect-os,env-setup}.sh, systemd unit, agent.env.example)을 `ld -r -b binary`로 오브젝트화해 바이너리에 박는다:

- `build/embed/`에 flat 이름으로 스테이징 → 심볼이 `_binary_install_sh_start`처럼 경로 없이 떨어진다.
- `objcopy --rename-section .data=.rodata`로 읽기 전용 섹션으로 이동 — 런타임 변조 불가.
- installer.c가 이 블롭을 `/tmp/agent-installer-XXXXXX/`(0700)에 풀고 `/bin/sh`로 실행한다.

## verify — ABI 컴플라이언스 게이트

`make release`는 `verify`를 통과해야 산출물을 만든다. 3중 검사:

1. **GLIBC 심볼 상한** — `objdump -T`로 프로파일 상한 초과 심볼 검출. modern은 manylinux2014(glibc 2.17, CentOS 7 기준)라 `GLIBC_2.18+` 금지.
2. **동적 의존 화이트리스트** — `ldd` 출력이 glibc 계열 + linux-vdso만인지.
3. **금지 API** — 상한 이후 도입된 wrapper(`getrandom`/`statx`/`memfd_create`/`renameat2`/`copy_file_range`/`pidfd_*`) 미사용.

`verify-legacy`는 manylinux2010(glibc 2.12, CentOS 6 기준) 프로파일 — `GLIBC_2.13+` 금지에 `secure_getenv`/`getauxval`까지 추가 금지. legacy 바이너리는 같은 소스·같은 정적 링크로 **빌드 호스트의 glibc만 낮춘** 산출물이다 (ABI 하한은 컴파일 플래그가 아니라 빌드 호스트 glibc의 속성). CentOS/RHEL/Oracle Linux 6의 SysV 경로로 배포된다 — [`../centos6-bringup.md`](../centos6-bringup.md).

```
dist/
  assessment-agent-linux-x86_64              # modern (glibc ≥ 2.17)
  assessment-agent-linux-x86_64-glibc2.12    # legacy (release-legacy)
  SHA256SUMS                                 # install.sh가 검증
```

## self-installer — `assessment-agent install`

서브커맨드 (main.c → installer.c):

| 커맨드 | 동작 |
|---|---|
| `install [--image-prep]` | 임베디드 번들 스테이징 → `install.sh` 실행. `--image-prep`이면 서비스 등록만 하고 시작 안 함 |
| `uninstall [--purge]` | 대칭 해제. `--purge`는 state까지 삭제 |
| `prep-image` | `/etc/machine-id` 초기화 — 골든 이미지 스냅샷 직전용 |

installer.c는 권한 게이트를 두지 않는다 — **어떤 권한이 필요한지는 스테이징된 sh 스크립트가 스스로 결정**한다. `INSTALLER_SELF_PATH=/proc/self/exe`를 넘겨 install.sh가 dist/ 디렉토리 없이 실행 중인 바이너리 자체를 설치 대상으로 쓰게 하고, `SKIP_SHA256=1`로 자기 자신 검증을 생략한다. 부모 환경은 상속되므로 운영자가 셸에 미리 export한 `RABBITMQ_*`가 install.sh의 env seed로 흘러든다 (env preset 또는 no-TTY면 비대화형 설치).

## 권한 모델 — "고객이 벤더 프로그램을 root로 상시 구동하지 않는다"

설치에 1회성 권한만 쓰고, 24/7 런타임은 비특권. 분기는 `deploy/lib/detect-os.sh`가 결정:

| 호스트 | 설치 권한 | 런타임 | 부팅 시작 | 경로 |
|---|---|---|---|---|
| systemd | **불필요** (순수 user) | `systemctl --user` | `loginctl enable-linger` (best-effort) | `~/.local/bin`, `${XDG_CONFIG_HOME}/assessment-agent`, user unit |
| SysV / EL6 | root 1회 (`/etc/init.d` 등록) | `assessment-agent` 시스템 유저 (`su` RUNAS) | chkconfig / update-rc.d | `/usr/local/bin`, `/etc/assessment-agent`, `/var/lib/agent-worker` |
| Windows | admin 1회 (서비스 등록) | 서비스 | SCM Automatic | `C:\Program Files\assessment-agent` |

user-level 설치의 잔존 리스크(에이전트 유저 소유 `~/.local/bin` 바이너리를 task.install이 덮어쓸 수 있음)는 `.claude/CLAUDE.md` "Out of Scope" 절에 트레이드오프로 기록돼 있다 — 방어선은 파일 소유권이 아니라 portal 신뢰 경계(P1) + 바이너리 서명.

## 테스트 진입점

| 대상 | 명령 | 비고 |
|---|---|---|
| 단위 (파서·헬퍼) | `make test-unit` | broker·네트워크 불필요. pkg-config dev 라이브러리 사용 — 정적 링크 표면이 아니라 파싱 로직 검증 |
| 스키마 스모크 | `tests/test_schema.sh` | 1회 발행 페이로드 검증 |
| 멀티 에이전트 | `tests/run_multi.sh` | N개 병렬 발행 |
| 장애 내성 | `tests/fault_agent.sh` / `tests/fault_rabbitmq.sh` | SIGKILL / broker 재시작 |
