/**
 * @file worker.c
 * @brief task.install consumer (Windows) — stub.
 *
 * Step 1 (이 파일): 빈 구현. worker_init 이 항상 NULL 을 반환 → main.c 는
 *                  worker 를 비활성으로 보고 v1 publish-only 동작 유지.
 *                  컴파일/링크 검증 목적.
 *
 * 실제 구현은 후속 commit 에서:
 *   - download.c (libcurl HTTPS + sha256 streaming)
 *   - exec.c     (CreateProcessW + Job Object + minimal env)
 *   - worker.c   (state machine — Linux worker.c 의 Windows 포팅)
 *   - publish.c  (long-lived AMQP connection — basic_get / basic_publish / basic_ack)
 */

#include "worker.h"

#include <stdio.h>
#include <stdlib.h>

struct worker_ctx_s {
	int dummy;
};

worker_ctx_t *worker_init(const worker_config_t *cfg)
{
	(void)cfg;
	fprintf(stderr, "[worker] not implemented yet — worker disabled\n");
	return NULL;
}

int worker_tick(worker_ctx_t *ctx)
{
	(void)ctx;
	return 0;
}

void worker_keepalive(worker_ctx_t *ctx)
{
	(void)ctx;
}

void worker_begin_drain(worker_ctx_t *ctx)
{
	(void)ctx;
}

void worker_force_child_term(worker_ctx_t *ctx, int hard)
{
	(void)ctx;
	(void)hard;
}

int worker_idle(const worker_ctx_t *ctx)
{
	(void)ctx;
	return 1;
}

int worker_has_live_child(const worker_ctx_t *ctx)
{
	(void)ctx;
	return 0;
}

void worker_shutdown(worker_ctx_t *ctx)
{
	(void)ctx;
}
