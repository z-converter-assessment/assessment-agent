# assessment-agent

서버 Assessment 서비스의 Agent입니다.
고객 서버에서 메트릭 데이터를 수집하여 API 서버로 전송합니다.

---

## 기술 스택

---

## 프로젝트 구조

---

## 브랜치 전략

```
main          # 배포용. 직접 push 금지
develop       # 개발 통합. PR로만 머지
feature/xxx   # 기능 개발
fix/xxx       # 버그 수정
chore/xxx     # 설정 변경
```

**작업 흐름**

```
develop에서 브랜치 생성
→ 작업 완료 후 develop으로 PR
→ 1명 이상 리뷰 승인 후 머지
```

---

## 커밋 컨벤션

| 타입 | 설명 | 예시 |
|---|---|---|
| `feat` | 새로운 기능 추가 | `feat: 메트릭 수집 엔드포인트 추가` |
| `fix` | 버그 수정 | `fix: Redis 연결 타임아웃 오류 수정` |
| `chore` | 설정, 패키지 변경 | `chore: requirements.txt 업데이트` |
| `docs` | 문서 수정 | `docs: README 로컬 실행 방법 추가` |
| `refactor` | 리팩토링 | `refactor: 메트릭 파싱 로직 함수 분리` |
| `test` | 테스트 코드 | `test: 수집 API 유닛 테스트 추가` |
| `style` | 포맷 등 스타일 변경 | `style: 들여쓰기 정리` |

---

## 팀원

| 이름 | 역할 | 담당 |
|---|---|---|
|김태원| 인프라 | 환경 구성, CI/CD |
|이수빈| 백엔드 | API 엔드포인트 |
|김종찬| 백엔드 | MQ + Worker |
|박윤호| Agent 개발 | 서버 메트릭 정보 수집 및 전송 |
