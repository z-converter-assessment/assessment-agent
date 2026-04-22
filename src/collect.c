/**
 * @file collect.c
 * @brief 각 필드별 수집기 + 최종 payload 조립.
 *
 * 수집 방식은 Python 프로토타입과 1:1 대응:
 *   - hostname      : gethostname(2)
 *   - nproc         : `nproc`                (문자열 그대로 보존)
 *   - mem_total_mb  : `free -m` 파싱
 *   - lsblk_raw     : `lsblk --json` (name/size만 추출)
 *   - ip.internal   : `ip -o -4 addr show` 파싱 (loopback 제외)
 *   - ip.external   : AGENT_EXTERNAL_IP env 우선, 없으면 curl ipify
 *
 * 외부 명령 실패는 run_cmd()가 NULL로 반환하며, 전체 수집 실패로
 * 처리한다. 단 external ip는 네트워크 장애가 흔해 빈 배열 fallback.
 */

#define _POSIX_C_SOURCE 200809L

#include "collect.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief hostname(2) 결과를 cJSON 문자열로 반환.
 *
 * AGENT_HOSTNAME_OVERRIDE 환경변수가 설정돼 있으면 그 값을 사용한다.
 * 다중 에이전트 테스트처럼 한 호스트에서 여러 프로세스를 띄우면서
 * 서로 다른 서버로 식별시켜야 할 때 유용하다.
 *
 * @return 새 cJSON 노드(실패 시 NULL).
 */
static cJSON *collect_hostname_json(void)
{
	const char *override = getenv("AGENT_HOSTNAME_OVERRIDE");
	if (override && *override)
		return cJSON_CreateString(override);

	/* popen("hostname") 대신 syscall 사용. 결과 동일, fork 비용 절약. */
	char buf[HOST_NAME_MAX + 1];
	if (gethostname(buf, sizeof buf) != 0)
		return NULL;
	buf[sizeof buf - 1] = '\0';
	return cJSON_CreateString(buf);
}

/**
 * @brief `nproc` 결과를 문자열 그대로 보존해 반환.
 *
 * 스키마에서 `nproc`이 문자열(Python 구현 호환)이므로 정수 변환을 하지 않는다.
 */
static cJSON *collect_nproc_json(void)
{
	char *out = run_cmd("nproc");
	if (!out)
		return NULL;
	trim_inplace(out);
	cJSON *j = cJSON_CreateString(out);
	free(out);
	return j;
}

/**
 * @brief `free -m` 에서 Mem: 행의 total MB만 추출.
 *
 * 예시 행: `Mem:   16384   ...`  → 두 번째 토큰이 total.
 * @return `{ "mem_total_mb": <int> }` 객체, 실패 시 NULL.
 */
static cJSON *collect_free_json(void)
{
	char *out = run_cmd("free -m");
	if (!out)
		return NULL;

	long mem_total = -1;
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "Mem:", 4) != 0)
			continue;
		char *tok_save = NULL;
		char *tok = strtok_r(line, " \t", &tok_save); /* "Mem:" */
		tok = strtok_r(NULL, " \t", &tok_save);       /* total */
		if (tok)
			mem_total = strtol(tok, NULL, 10);
		break;
	}
	free(out);
	if (mem_total < 0)
		return NULL;

	cJSON *obj = cJSON_CreateObject();
	if (!obj)
		return NULL;
	cJSON_AddNumberToObject(obj, "mem_total_mb", (double)mem_total);
	return obj;
}

/**
 * @brief `lsblk --json` 출력을 파싱해 [{name,size}, ...] 배열로 반환.
 *
 * 파이썬 구현과 동일하게 최상위 blockdevices 만 추출한다. children
 * (파티션, LVM 등)은 현재 스키마에 포함하지 않는다. 필요 시 본 함수에서 재귀.
 */
static cJSON *collect_lsblk_json(void)
{
	char *out = run_cmd("lsblk --json -o NAME,SIZE");
	if (!out)
		return NULL;

	cJSON *parsed = cJSON_Parse(out);
	free(out);
	if (!parsed)
		return NULL;

	cJSON *result = cJSON_CreateArray();
	if (!result) {
		cJSON_Delete(parsed);
		return NULL;
	}

	cJSON *devices = cJSON_GetObjectItemCaseSensitive(parsed, "blockdevices");
	cJSON *dev = NULL;
	cJSON_ArrayForEach(dev, devices) {
		cJSON *name = cJSON_GetObjectItemCaseSensitive(dev, "name");
		cJSON *size = cJSON_GetObjectItemCaseSensitive(dev, "size");
		if (!cJSON_IsString(name) || !cJSON_IsString(size))
			continue;
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", name->valuestring);
		cJSON_AddStringToObject(item, "size", size->valuestring);
		cJSON_AddItemToArray(result, item);
	}

	cJSON_Delete(parsed);
	return result;
}

/**
 * @brief `ip -o -4 addr show` 에서 IPv4 주소만 뽑아 배열로 반환.
 *
 * 예시 행: `2: enp0s3    inet 10.0.0.10/24 brd ...` 에서
 * "inet " 다음의 "10.0.0.10/24"를 잘라 '/' 앞까지만 취한다.
 * loopback(127.x.x.x)은 제외.
 */
static cJSON *collect_internal_ips_json(void)
{
	char *out = run_cmd("ip -o -4 addr show");
	if (!out)
		return NULL;

	cJSON *arr = cJSON_CreateArray();
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *needle = strstr(line, "inet ");
		if (!needle)
			continue;
		needle += 5;
		char ip[64] = { 0 };
		size_t i = 0;
		while (*needle && *needle != '/' && *needle != ' ' &&
		       i + 1 < sizeof ip) {
			ip[i++] = *needle++;
		}
		ip[i] = '\0';
		if (i == 0)
			continue;
		if (strncmp(ip, "127.", 4) == 0)
			continue;
		cJSON_AddItemToArray(arr, cJSON_CreateString(ip));
	}
	free(out);
	return arr;
}

/**
 * @brief 외부 IP 배열을 구성.
 *
 * - AGENT_EXTERNAL_IP 환경변수가 있으면 콤마 분리 후 그대로 사용.
 * - 없으면 `curl -s --max-time 3 https://api.ipify.org` 호출.
 *
 * 내부망 환경이 기본이므로, 둘 다 실패해도 빈 배열을 돌려준다.
 * 전체 수집 실패로 간주하지 않는다.
 */
static cJSON *collect_external_ips_json(void)
{
	cJSON *arr = cJSON_CreateArray();

	const char *configured = getenv("AGENT_EXTERNAL_IP");
	if (configured && *configured) {
		char *copy = strdup(configured);
		if (!copy)
			return arr;
		char *save = NULL;
		for (char *tok = strtok_r(copy, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save)) {
			trim_inplace(tok);
			if (*tok)
				cJSON_AddItemToArray(arr, cJSON_CreateString(tok));
		}
		free(copy);
		return arr;
	}

	/* curl 미설치 / 네트워크 단절은 NULL 반환으로 조용히 넘어간다. */
	char *out = run_cmd("curl -s --max-time 3 https://api.ipify.org");
	if (out) {
		trim_inplace(out);
		if (*out)
			cJSON_AddItemToArray(arr, cJSON_CreateString(out));
		free(out);
	}
	return arr;
}

cJSON *collect_inventory(void)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	cJSON *hostname = collect_hostname_json();
	cJSON *nproc = collect_nproc_json();
	cJSON *freej = collect_free_json();
	cJSON *lsblk = collect_lsblk_json();
	cJSON *ip_internal = collect_internal_ips_json();
	cJSON *ip_external = collect_external_ips_json();

	/* 핵심 필드 중 하나라도 실패면 전체 실패.
	 * ip_external은 위에서 빈 배열 fallback을 이미 보장. */
	if (!hostname || !nproc || !freej || !lsblk || !ip_internal || !ip_external) {
		cJSON_Delete(hostname);
		cJSON_Delete(nproc);
		cJSON_Delete(freej);
		cJSON_Delete(lsblk);
		cJSON_Delete(ip_internal);
		cJSON_Delete(ip_external);
		cJSON_Delete(root);
		return NULL;
	}

	cJSON_AddItemToObject(root, "hostname", hostname);
	cJSON_AddItemToObject(root, "nproc", nproc);
	cJSON_AddItemToObject(root, "free", freej);
	cJSON_AddItemToObject(root, "lsblk_raw", lsblk);

	cJSON *ip_raw = cJSON_CreateObject();
	cJSON_AddItemToObject(ip_raw, "internal", ip_internal);
	cJSON_AddItemToObject(ip_raw, "external", ip_external);
	cJSON_AddItemToObject(root, "ip_raw", ip_raw);

	return root;
}
