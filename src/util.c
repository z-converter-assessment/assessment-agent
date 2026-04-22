/**
 * @file util.c
 * @brief util.h 구현. 외부 도메인(librabbitmq, cJSON)에 의존하지 않는다.
 */

#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/**
 * @brief popen으로 자식 프로세스 stdout을 읽어 동적 버퍼에 모은다.
 *
 * `lsblk --json` 등 출력이 수십 KB까지 커질 수 있으므로 정적 버퍼 대신
 * 지수적으로 realloc 하며 읽는다.
 */
char *run_cmd(const char *cmd)
{
	FILE *fp = popen(cmd, "r");
	if (!fp)
		return NULL;

	size_t cap = 4096;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		pclose(fp);
		return NULL;
	}

	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
		/* capacity 부족 시 2배씩 확장, 마지막 '\0' 자리도 확보 */
		if (len + n + 1 > cap) {
			while (len + n + 1 > cap)
				cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				pclose(fp);
				return NULL;
			}
			buf = nb;
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	buf[len] = '\0';

	/* popen은 종료 상태를 wait(2) 포맷으로 반환.
	 * 자식이 비정상 종료하거나 non-zero exit이면 실패로 처리. */
	int status = pclose(fp);
	if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

const char *getenv_default(const char *name, const char *fallback)
{
	const char *v = getenv(name);
	return (v && *v) ? v : fallback;
}

char *trim_inplace(char *s)
{
	if (!s)
		return s;

	/* 앞쪽 공백 skip 후 앞으로 당긴다. */
	char *start = s;
	while (*start && isspace((unsigned char)*start))
		start++;
	if (start != s)
		memmove(s, start, strlen(start) + 1);

	/* 뒤쪽 공백을 역방향으로 null 처리. */
	size_t l = strlen(s);
	while (l > 0 && isspace((unsigned char)s[l - 1]))
		s[--l] = '\0';
	return s;
}

/**
 * @brief python-dotenv와 유사한 방식으로 .env를 파싱한다.
 *
 * 의도적으로 동작 차이를 최소화했다:
 *   - KEY, VALUE 양쪽 공백 트림.
 *   - VALUE 양끝 "..." 또는 '...' 한 쌍만 제거(내부 이스케이프 미지원).
 *   - 기존 환경변수는 덮어쓰지 않음(setenv overwrite=0).
 *   - 잘못된 줄은 조용히 무시.
 */
void load_env_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		/* 빈 줄 / 주석 skip */
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = p;
		char *val = eq + 1;

		/* key rstrip */
		char *key_end = eq - 1;
		while (key_end >= key && (*key_end == ' ' || *key_end == '\t')) {
			*key_end = '\0';
			key_end--;
		}
		if (*key == '\0')
			continue;

		/* value rstrip */
		size_t vl = strlen(val);
		while (vl > 0 &&
		       (val[vl - 1] == '\n' || val[vl - 1] == '\r' ||
		        val[vl - 1] == ' ' || val[vl - 1] == '\t')) {
			val[--vl] = '\0';
		}

		/* 바깥 따옴표 한 쌍 제거 */
		if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
		                (val[0] == '\'' && val[vl - 1] == '\''))) {
			val[vl - 1] = '\0';
			val++;
		}

		/* 쉘 환경 우선. overwrite=0. */
		setenv(key, val, 0);
	}
	fclose(f);
}
