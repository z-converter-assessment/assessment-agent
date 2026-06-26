# Windows 접근 방식 (WinRM)

agent 바이너리를 Windows VM에 배포·실행할 때 쓰는 원격 접근 기술과 그 활성화 경로.
출처: assessment-infra (terraform `agent/terraform/windows.tf`, ansible `agent/ansible/group_vars/windows.yml`, ADR-0007).

## 결론

SSH가 아니라 WinRM(Windows Remote Management)이다.

- 프로토콜: WinRM over HTTP
- 포트: 5985 (HTTP). HTTPS 5986은 미사용
- 인증: Basic auth + AllowUnencrypted=true (평문 전송)
- 계정: Administrator
- 인증서 검증: 무시 (server_cert_validation=ignore)
- 구동 주체: Ansible (bastion에서 실행) — `deploy.yml`의 "Deploy assessment-agent (Windows)" 플레이

ansible 연결 변수 (`group_vars/windows.yml`):

```yaml
ansible_connection: winrm
ansible_port: 5985
ansible_winrm_transport: basic
ansible_winrm_server_cert_validation: ignore
ansible_user: Administrator
ansible_password: "{{ vault_windows_admin_password }}"
```

## WinRM이 켜지는 경로 (cloudbase-init)

VM 부팅 시 cloudbase-init이 terraform user_data(`win_user_data`)의 PowerShell을 1회 실행해
WinRM을 활성화한다. 즉 이미지 자체가 아니라 첫 부팅 프로비저닝으로 열린다.

user_data 핵심 (terraform `windows.tf`):

```
#ps1_sysnative
net user Administrator "<windows_admin_password>"          # 고정 비밀번호 주입
Enable-PSRemoting -Force -SkipNetworkProfileCheck          # WinRM/PSRemoting 활성
winrm set winrm/config/service '@{AllowUnencrypted="true"}'
winrm set winrm/config/service/auth '@{Basic="true"}'
netsh advfirewall firewall add rule name="WinRM-HTTP-In-TCP" \
      protocol=TCP dir=in localport=5985 action=allow       # 5985 인바운드 허용
cscript //nologo C:\Windows\System32\slmgr.vbs /rearm       # 평가판 라이선스 타이머 리셋
```

결과적으로 접근은 두 게이트를 통과해야 한다: OpenStack Security Group(agent_sg)의 5985 +
게스트 OS 방화벽 규칙. 둘 다 AND.

## 자격증명과 제어 지점 (bastion)

- Administrator 비밀번호 = `vault_windows_admin_password`. 평문은 어디에도 없고,
  ansible-vault(AES256)로 암호화된 `group_vars/all/vault.yml`에만 있다.
- 복호화 키 `~/.vault-pass`는 bastion 로컬 전용(mode 0400). 다른 호스트엔 없다.
- 같은 비밀번호 값이 두 곳에 쓰인다: terraform이 cloudbase-init로 VM에 주입(net user),
  ansible이 vault에서 읽어 WinRM 접속.

따라서 Windows 배포는 bastion에서만 자력 실행 가능하다(vault-pass + clouds.yaml + key가
bastion에 모여 있음). agent-subnet에 떠 있는 다른 호스트는 5985에 네트워크 도달은 돼도
Administrator 자격증명이 없어 인증 단계에서 막힌다.

## 보안 주의

AllowUnencrypted=true + Basic = 비밀번호·페이로드가 평문으로 흐른다. 폐쇄 사설망(agent-subnet)
한정 전제라 허용된 구성이다. 외부 노출 환경이라면 HTTPS 5986 + 인증서로 전환해야 한다(현재 미적용).

## 버전별 WinRM 가용성 (실측)

이 경로(WinRM 5985)로 닿는 Windows와 아닌 것:

| Windows | WinRM 5985 | 비고 |
|---|---|---|
| Server 2012 / 2012R2 | 도달 OK | cloudbase-init/PSRemoting 정상 |
| Server 2016 / 2019 / 2022 / 2025 | 도달 OK | 정상 |
| Server 2008 | 미지원 | NT6.0 / PowerShell 2.0 — cloudbase-init·WinRM 미동작 가능 (windows2008.yml 경고) |
| Server 2003 | 미지원 | NT5.2 — WinRM 부재 |

2008/2003은 이 경로로 배포 불가 → 별도 접근(이미지에 바이너리 베이크, 또는 수동 콘솔/RDP)이
필요하다. 마침 이 둘은 agent 빌드에서도 미검증 legacy 프로파일이다(SUPPORTED_OS.md).

관련 문서: [rollout.md](rollout.md) — 버전별 바이너리 매핑·배포 흐름·재생성 계획.
