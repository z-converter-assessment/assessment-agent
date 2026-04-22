/**
 * @file main.c
 * @brief 에이전트 엔트리포인트. 환경변수 파싱 + 수집·발행 루프.
 *
 * 실행 모드:
 *   - `AGENT_INTERVAL_SEC <= 0` : 1회 수집·발행 후 종료(cron 용).
 *   - `AGENT_INTERVAL_SEC  > 0` : 루프 모드, interval초마다 재수집.
 *
 * 설정은 env만 사용. .env 파일이 있으면 읽되, 쉘 환경이 우선한다.
 */

#define _POSIX_C_SOURCE 200809L

#include "collect.h"
#include "publish.h"
#include "util.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief 에이전트 1 사이클: 수집 → 직렬화 → 발행.
 *
 * 루프 모드에서 에러를 삼키고 계속 돌 수 있도록 예외를 던지지 않고
 * 반환값으로만 결과를 표현한다.
 *
 * @return 0 성공, -1 실패.
 */
static int run_once(void)
{
	cJSON *inv = collect_inventory();
	if (!inv) {
		fprintf(stderr, "[agent] collect failed\n");
		return -1;
	}

	char *body = cJSON_PrintUnformatted(inv);

	/* 로깅용 hostname (발행 실패 시에도 어떤 호스트인지 남기기 위함). */
	const char *hostname = "<unknown>";
	cJSON *h = cJSON_GetObjectItemCaseSensitive(inv, "hostname");
	if (cJSON_IsString(h))
		hostname = h->valuestring;

	publish_config_t cfg = {
		.host        = getenv_default("RABBITMQ_HOST", "localhost"),
		.port        = atoi(getenv_default("RABBITMQ_PORT", "5672")),
		.user        = getenv_default("RABBITMQ_USER", "admin"),
		.password    = getenv_default("RABBITMQ_PASS", "admin"),
		.exchange    = getenv_default("RABBITMQ_EXCHANGE", "assessment"),
		.queue       = getenv_default("RABBITMQ_QUEUE", "server.metrics"),
		.routing_key = getenv_default("RABBITMQ_ROUTING_KEY", "metrics"),
	};
	/* atoi 실패 시 0이 나오므로 AMQP 표준 포트로 재설정. */
	if (cfg.port <= 0)
		cfg.port = 5672;

	int rc = publish_message(&cfg, body, strlen(body));
	if (rc == 0)
		fprintf(stderr, "[agent] published inventory for %s\n", hostname);

	free(body);
	cJSON_Delete(inv);
	return rc;
}

/**
 * @brief 프로세스 진입점.
 *
 * @return 0 정상(루프 모드는 반환 없음), 비-0 1회 실행 모드 실패.
 */
int main(void)
{
	/* cwd 기준 .env 탐색. 없으면 쉘 환경만 사용. */
	load_env_file(".env");

	int interval = atoi(getenv_default("AGENT_INTERVAL_SEC", "60"));
	if (interval <= 0)
		return run_once() == 0 ? 0 : 1;

	fprintf(stderr, "[agent] loop mode: interval=%ds (Ctrl+C to exit)\n", interval);
	for (;;) {
		/* 1 사이클 실패 시 지수 백오프로 즉시 재시도. 성공하거나
		 * interval 에 도달할 때까지 반복한다. 단발 브로커 단절을
		 * 다음 수집 주기까지 기다리지 않고 복구하기 위함. */
		unsigned int backoff = 1;
		while (run_once() != 0) {
			if (backoff > (unsigned int)interval)
				backoff = (unsigned int)interval;
			fprintf(stderr, "[agent] iteration failed, retrying in %us\n", backoff);
			sleep(backoff);
			backoff *= 2;
		}
		sleep((unsigned int)interval);
	}
}
