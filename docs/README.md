# 문서 인덱스

본 디렉토리는 assessment-agent의 영구 문서. 규약·원칙·금지 사항은 `.claude/CLAUDE.md`, 운영자용 소개·설정은 루트 `README.md`.

## 어떤 문서를 언제 보는가

| 상황 | 위치 |
|---|---|
| 메시지 규격 (엔진과의 계약 — 필드·타입·버전 이력) | [`payload-schema.md`](payload-schema.md) |
| 수집기가 값을 어디서 어떻게 읽는가 | [`architecture/collection.md`](architecture/collection.md) |
| 메인 루프·AMQP 발행·재시도·drain | [`architecture/publish.md`](architecture/publish.md) |
| task.install 수신·격리 실행·멱등성·복구 | [`architecture/worker.md`](architecture/worker.md) |
| Windows 포팅이 Linux와 갈리는 지점 | [`architecture/windows.md`](architecture/windows.md) |
| 빌드·정적 링크·ABI 검증·self-installer·권한 모델 | [`architecture/build-release.md`](architecture/build-release.md) |
| inventory 주기 재발행과 counter-reset 감지 규약 | [`inventory-refresh-and-reset-detection.md`](inventory-refresh-and-reset-detection.md) |
| CentOS 6 (glibc 2.12) 지원 작업 기록 | [`centos6-bringup.md`](centos6-bringup.md) |
| 과거 설계 노트 (역사 자료 — 현행 문서가 우선) | [`archive/`](archive/) |

## 디렉토리

```
docs/
├── README.md                  ← 본 파일 (인덱스)
├── payload-schema.md          계약 단일 진실 — agent ↔ engine 메시지 규격 (v3.x 이력 포함)
├── architecture/              구현 deep dive (영구·갱신 — 코드 변경 시 동시 갱신)
│   ├── collection.md          collect.c — /proc·/sys·syscall 수집, 공통 메타데이터, composite_id
│   ├── publish.md             main.c·publish.c — CM2 이중 연결, confirm, 백오프, drain 4단계
│   ├── worker.md              worker·download·extract·exec — 상태 머신, 3중 마커 내구성, EC1 격리
│   ├── windows.md             windows-agent — Win32 매핑, thread/Job Object, install.type 분기
│   └── build-release.md       Makefile·installer.c·deploy/ — vendoring, verify ABI 게이트, 권한 모델
├── inventory-refresh-and-reset-detection.md   엔진과 합의된 운영 규약
├── centos6-bringup.md         legacy(glibc 2.12) 지원 기록
└── archive/                   superseded 설계 노트 — 인용 금지, 맥락 참고용
    └── worker-task-design.md  worker 도입 전 의사결정 기록 (B-f·M1·T1·K3·R2·I1·S2·U1·EC1)
```

## 라이프사이클 규약

- `payload-schema.md` — 계약. 변경은 버전 항목 추가로만 (이력 보존). 엔진 쪽 `docs/architecture/agent.md`와 동시 갱신 의무.
- `architecture/` — 영구·갱신. 코드 변경 시 동시 업데이트.
- `archive/` — 불변. 새 결정은 현행 문서에 쓰고, 옛 문서는 상단에 superseded 표기만 추가.
