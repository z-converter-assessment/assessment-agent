/**
 * @file util.h
 * @brief 프로세스/환경 공통 유틸리티 헤더.
 *
 * 셸 명령 실행, .env 로딩, 환경변수 기본값, 문자열 트림 등
 * 수집(collect) 및 발행(publish) 양쪽에서 공유하는 소형 헬퍼를 모아 둔다.
 * 도메인 로직(AMQP, 인벤토리 스키마)은 포함하지 않는다.
 */

#ifndef ASSESSMENT_AGENT_UTIL_H
#define ASSESSMENT_AGENT_UTIL_H

#include <stddef.h>

/**
 * @brief 셸 명령을 실행하고 stdout을 동적 문자열로 수집한다.
 *
 * 출력은 새로 할당된 null-terminated 문자열로 반환되며, 호출자가
 * free()로 해제해야 한다. `lsblk --json` 처럼 수십 KB에 달하는
 * 출력도 지원하도록 버퍼를 지수적으로 확장한다.
 *
 * @param cmd 실행할 셸 명령 문자열. /bin/sh -c 에 전달된다.
 * @return 성공 시 출력 문자열 포인터(malloc), 실패 시 NULL.
 *         실패 조건: popen 실패, 자식 비정상 종료, 종료 상태 != 0,
 *         메모리 부족.
 */
char *run_cmd(const char *cmd);

/**
 * @brief .env 스타일 파일을 읽어 프로세스 환경변수로 주입한다.
 *
 * 이미 설정된 변수는 덮어쓰지 않는다(쉘 환경이 우선). 파일이 존재하지
 * 않으면 조용히 반환한다.
 *
 * 지원 형식:
 *   - `KEY=VALUE`
 *   - `KEY="VALUE"`, `KEY='VALUE'` (바깥 한 쌍 따옴표만 제거)
 *   - `# 주석`
 *
 * @param path .env 파일 경로. 상대 경로면 cwd 기준.
 */
void load_env_file(const char *path);

/**
 * @brief getenv()의 얇은 래퍼. 미설정/빈 값이면 fallback을 돌려준다.
 *
 * @param name     조회할 환경변수 이름.
 * @param fallback name이 없거나 빈 문자열일 때 반환할 기본값.
 * @return environ이 유지하는 문자열 또는 fallback 그대로.
 */
const char *getenv_default(const char *name, const char *fallback);

/**
 * @brief 문자열 앞뒤 공백을 in-place로 제거한다.
 *
 * `\n`, `\r`, `\t`, ' ' 등 isspace(3) 기준의 모든 whitespace를 다듬는다.
 * 같은 버퍼 안에서 내용을 이동시키므로 추가 할당은 없다.
 *
 * @param s 대상 문자열 포인터. NULL이면 그대로 NULL 반환.
 * @return 정리된 문자열의 시작 포인터(= s).
 */
char *trim_inplace(char *s);

#endif
