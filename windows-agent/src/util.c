/**
 * @file util.c
 * @brief util.h implementation (Windows). No AMQP / cJSON dependencies.
 *
 * POSIX 헬퍼 (popen, gmtime_r, /proc/sys/kernel/random/uuid 등) 를 Win32 등가
 * (_popen, gmtime_s, CoCreateGuid 등) 로 대체. 페이로드 계약은 Linux와 동일.
 */

#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <objbase.h>   /* CoCreateGuid */

/* ---------- env helpers ---------- */
const char *getenv_default(const char *name, const char *fallback)
{
	const char *v = getenv(name);
	return (v && *v) ? v : fallback;
}

int parse_bool(const char *s, int fallback)
{
	if (!s || !*s) return fallback;
	if (!_stricmp(s, "1")    || !_stricmp(s, "true") ||
	    !_stricmp(s, "yes")  || !_stricmp(s, "on")   ||
	    !_stricmp(s, "y")    || !_stricmp(s, "t"))
		return 1;
	if (!_stricmp(s, "0")    || !_stricmp(s, "false") ||
	    !_stricmp(s, "no")   || !_stricmp(s, "off")   ||
	    !_stricmp(s, "n")    || !_stricmp(s, "f"))
		return 0;
	return -1;
}

int getenv_bool(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) return fallback;
	int parsed = parse_bool(v, -1);
	if (parsed < 0) {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" not a recognized boolean "
		                "(use true/false/1/0/yes/no/on/off); using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	return parsed;
}

int getenv_int_or(const char *name, int fallback)
{
	const char *v = getenv(name);
	if (!v || !*v) return fallback;
	char *end = NULL;
	errno = 0;
	long n = strtol(v, &end, 10);
	if (errno != 0 || end == v || *end != '\0') {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" not a valid integer; using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	if (n < INT_MIN || n > INT_MAX) {
		fprintf(stderr, "[agent] WARN: env %s=\"%s\" out of int range; using default %d\n",
		        name, v, fallback);
		return fallback;
	}
	return (int)n;
}

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
		if (*p == '\0' || *p == '\n' || *p == '#')
			continue;

		char *eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		char *key = p;
		char *val = eq + 1;

		char *key_end = eq - 1;
		while (key_end >= key && (*key_end == ' ' || *key_end == '\t')) {
			*key_end = '\0';
			key_end--;
		}
		if (*key == '\0')
			continue;

		size_t vl = strlen(val);
		while (vl > 0 &&
		       (val[vl - 1] == '\n' || val[vl - 1] == '\r' ||
		        val[vl - 1] == ' ' || val[vl - 1] == '\t')) {
			val[--vl] = '\0';
		}

		if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
		                (val[0] == '\'' && val[vl - 1] == '\''))) {
			val[vl - 1] = '\0';
			val++;
		}

		/* "do not overwrite" — shell env wins (matches POSIX setenv flag 0). */
		if (getenv(key) == NULL)
			_putenv_s(key, val);
	}
	fclose(f);
}

/* ---------- time / uuid ---------- */
char *iso8601_utc(time_t t, char *buf, size_t len)
{
	struct tm tm_buf;
	gmtime_s(&tm_buf, &t);
	strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
	return buf;
}

char *uuid_v4(char *buf, size_t len)
{
	GUID g;
	if (CoCreateGuid(&g) == S_OK) {
		snprintf(buf, len,
		         "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		         (unsigned long)g.Data1, g.Data2, g.Data3,
		         g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
		         g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
		return buf;
	}
	/* Synthetic fallback (not RFC-4122 but unique enough for diagnostics). */
	char hostname[64] = "unknown";
	DWORD sz = (DWORD)sizeof hostname;
	GetComputerNameA(hostname, &sz);
	SYSTEMTIME st;
	GetSystemTime(&st);
	snprintf(buf, len, "%s-%lu-%04d%02d%02dT%02d%02d%02d.%03d",
	         hostname, (unsigned long)GetCurrentProcessId(),
	         st.wYear, st.wMonth, st.wDay,
	         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

int jitter_seconds(int base_sec, double frac)
{
	if (base_sec <= 0) return base_sec;
	if (frac < 0)      frac = 0;
	if (frac >= 1.0)   frac = 0.999;

	double u = ((double)rand() / (double)RAND_MAX) * (2.0 * frac) - frac;
	double v = (double)base_sec * (1.0 + u);
	return (int)v;
}

/* ---------- boot time ----------
 *
 * Linux 에이전트는 `/proc/uptime` + `CLOCK_REALTIME` 로 부팅 wall-clock 합성.
 * Windows 등가: GetTickCount64() (ms since boot, monotonic) + 현재 wall-clock.
 * 페이로드 v3.1 의 `boot_time` 은 프로세스 시작 시 1회 캐시되며, 본 함수는
 * main.c 에서 한 번 호출되어 그 값을 캐시한다.
 */
char *get_boot_time_iso8601(char *buf, size_t len)
{
	ULONGLONG uptime_ms = GetTickCount64();

	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	ULARGE_INTEGER now_ft;
	now_ft.LowPart  = ft.dwLowDateTime;
	now_ft.HighPart = ft.dwHighDateTime;

	/* FILETIME epoch = 1601-01-01 UTC. Unix epoch = 1970-01-01 UTC.
	 * Difference = 11644473600 seconds = 116444736000000000 100ns ticks. */
	ULONGLONG ft_unix_100ns = now_ft.QuadPart - 116444736000000000ULL;
	time_t now_sec = (time_t)(ft_unix_100ns / 10000000ULL);
	time_t boot_sec = now_sec - (time_t)(uptime_ms / 1000ULL);

	return iso8601_utc(boot_sec, buf, len);
}
