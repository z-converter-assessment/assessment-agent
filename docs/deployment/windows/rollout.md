# Windows agent 롤아웃 (버전별 바이너리 배포)

빌드한 Windows 바이너리를 버전에 맞게 VM에 배포·실행하는 방법. 접근 기술은 [access.md](access.md).

## 버전 -> 바이너리 매핑

agent는 NT 세대별로 4개의 Windows 바이너리를 만든다(빌드 프로파일은 windows-agent/Makefile).
배포 시 VM의 Windows 버전에 맞는 것을 골라야 한다 — 잘못 매칭하면 PE가 로드되지 않는다.

| Windows | 빌드 프로파일 | 바이너리 | NT | WinRM 배포 |
|---|---|---|---|---|
| Server 2016 / 2019 / 2022 / 2025 | modern | assessment-agent-win2016-x64.exe | 10.0 | 가능 |
| Server 2012 / 2012R2 | win7 | assessment-agent-win2008r2-x64.exe | 6.2 / 6.3 | 가능 |
| Server 2003 (x86) | legacy32 | assessment-agent-win2003-x86.exe | 5.2 / i386 | 불가(WinRM 미지원) |

modern은 OpenSSL 3.x + bcrypt, win7도 OpenSSL 3.x, legacy32는 OpenSSL 1.0.2u + gdi32
(bcrypt 없음). 검증된 것은 modern(2016~2025)·win7(2012/2012R2)이고, legacy32(2003 x86)는
build-only. 2003 x64 Edition은 드물어 빌드하지 않는다(x86이 유일한 2003 타겟).

## 배포 흐름 (폐쇄망, ADR-0007)

VM은 외부 인터넷 직접 접근이 안 되므로 bastion이 중계한다.

```
GitHub Release ──(bastion이 다운로드)──> agent/ansible/files/binaries/
                                              │
                                bastion: ansible deploy.yml (windows play)
                                              │ WinRM 5985
                                              ▼
   roles: agent_binary(win_copy .exe) -> agent_env(.env seed) -> agent_service
                                              │
                              <exe> install  (self-installer 서브커맨드)
                                              │
                          schtasks /SC ONSTART /RU <user> /RL LIMITED  (부팅 시작)
                                              ▼
                          per-user scheduled task = `<exe> run` (collector+worker)
```

배포 위치·서비스명 (`group_vars/windows.yml`):

- 바이너리: `C:\Program Files\AssessmentAgent\assessment-agent.exe`
- worker state: `C:\ProgramData\assessment-agent\worker`
- worker tmp: `C:\Windows\Temp`
- 서비스명: AssessmentAgent

## 현재 ansible 한계 — per-version override 필요

`group_vars/windows.yml`은 `agent_binary_filename: assessment-agent.exe`(modern)를 모든 Windows에
하드코딩한다. 그대로면 2012/2012R2(win7 필요)·2003(legacy32 필요)에 modern을 깔아 로드 실패한다.

버전별 배포를 하려면 OS group_vars에 바이너리 override를 추가해야 한다. 예:

```yaml
# group_vars/windows2012.yml (및 windows2012r2.yml)
agent_binary_filename: assessment-agent-win2008r2-x64.exe
agent_binary_dest: 'C:\Program Files\AssessmentAgent\assessment-agent-win2008r2-x64.exe'

# group_vars/windows2003.yml
agent_binary_filename: assessment-agent-win2003-x86.exe
```

inventory(`inventory.yml`, gitignore)에 VM을 OS group(windows2022, windows2012, ...)으로
묶어야 group_vars가 매칭된다. inventory는 OpenStack 동적 생성(scripts/gen_inventory.py 계열)으로
채운다.

빌드 산출물은 이미 `agent/ansible/files/binaries/`에 4종 배치 완료(modern·win7·legacy·legacy32).

## 실측 현황 (엔진 인벤토리 기준)

엔진(192.168.3.149:8000)에 보고 중인 Windows agent를 IP로 매핑한 결과:

| VM | build | 에이전트 상태 |
|---|---|---|
| 2012 / 2016 / 2019 / 2022 | 9200 / 14393 / 17763 / 20348 | 온라인, 보고 중 |
| 2012R2 / 2025 | 9600 / 26100 | 보고 이력 있으나 stale |
| 2003 / 2008 | - | 엔진 미등록 = 에이전트 미작동 |

2003/2008은 WinRM이 안 떠서 이 경로로 배포된 적이 없고, 빌드도 미검증 프로파일이다.

## 전체 재생성으로 제어 확보 (계획, 즉시 아님)

현재 떠 있는 Windows는 과거 배포·재등록이 섞여 상태가 지저분하다(빌드별 composite_id 중복 =
재등록 흔적, stale 항목 혼재). 깨끗한 baseline에서 버전별 바이너리를 통제된 상태로 검증하려면
기존 Windows를 전부 내리고 재생성하는 편이 낫다.

대략적 순서 (bastion에서):

1. terraform: windows VM 일괄 destroy (windows.tf의 windows_vm 자원), 또는 `windows_vm_enabled=false` 후 apply
2. `windows_os_map`을 검증 대상 버전 전체로 확장(현재 default는 2022 x7 + 2008 x1) +
   `windows_vm_enabled=true`로 재생성 — cloudbase-init이 WinRM을 새로 연다
3. inventory 재생성 + 버전별 바이너리 override(위 "per-version override") 반영
4. ansible deploy.yml(windows play) 실행 -> 버전 맞는 .exe를 win_copy + install
5. 엔진 인벤토리에서 버전별 보고 확인, 2022는 install 태스크로 exit 2 -> success 보정까지 검증

주의: 재생성은 boot-from-volume 40~60GB 복사라 VM당 생성 timeout 30m가 걸려 있다. 평가판
이미지라 cloudbase-init의 slmgr /rearm이 라이선스 강제 셧다운을 막는다(rearm 한도 6회).
