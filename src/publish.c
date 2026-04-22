/**
 * @file publish.c
 * @brief librabbitmq(rabbitmq-c) 기반 단일 메시지 발행 구현.
 *
 * publisher confirms 활성화: basic_publish 후 broker ACK/NACK을 대기한다.
 * message_id는 /proc/sys/kernel/random/uuid 기반으로 생성한다.
 *
 * 라이프사이클:
 * @verbatim
 * amqp_new_connection
 *   → amqp_tcp_socket_new / amqp_socket_open
 *     → amqp_login
 *       → amqp_channel_open
 *         → amqp_confirm_select            ← publisher confirms 활성화
 *           → amqp_exchange_declare / queue_declare / queue_bind
 *             → amqp_basic_publish
 *             → wait ACK frame             ← broker 확인
 *           → amqp_channel_close
 *         → amqp_connection_close
 *   → amqp_destroy_connection
 * @endverbatim
 */

#define _POSIX_C_SOURCE 200809L

#include "publish.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define CONFIRM_TIMEOUT_SEC 5

static int check_rpc(amqp_rpc_reply_t r, const char *ctx)
{
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL:
		return 0;
	case AMQP_RESPONSE_NONE:
		fprintf(stderr, "[publish] %s: missing RPC reply\n", ctx);
		return -1;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		fprintf(stderr, "[publish] %s: %s\n", ctx,
		        amqp_error_string2(r.library_error));
		return -1;
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		fprintf(stderr, "[publish] %s: server exception (class=%u method=%u)\n",
		        ctx, r.reply.id >> 16, r.reply.id & 0xFFFF);
		return -1;
	}
	return -1;
}

/**
 * @brief 메시지 고유 ID 생성.
 *
 * /proc/sys/kernel/random/uuid에서 UUID를 읽는다.
 * 실패 시 hostname-pid-usec 조합으로 fallback.
 */
static void gen_message_id(char *buf, size_t len)
{
	FILE *f = fopen("/proc/sys/kernel/random/uuid", "r");
	if (f) {
		if (fgets(buf, (int)len, f)) {
			size_t l = strlen(buf);
			if (l > 0 && buf[l - 1] == '\n')
				buf[l - 1] = '\0';
			fclose(f);
			return;
		}
		fclose(f);
	}

	char hostname[64] = "unknown";
	gethostname(hostname, sizeof hostname);
	struct timeval tv;
	gettimeofday(&tv, NULL);
	snprintf(buf, len, "%s-%d-%ld%06ld",
	         hostname, (int)getpid(), (long)tv.tv_sec, (long)tv.tv_usec);
}

/**
 * @brief publisher confirms 모드에서 broker ACK/NACK 대기.
 *
 * CONFIRM_TIMEOUT_SEC 내에 ACK이 오지 않으면 실패로 처리한다.
 * @return 0 ACK, -1 NACK 또는 타임아웃.
 */
static int wait_confirm(amqp_connection_state_t conn)
{
	struct timeval tv = { .tv_sec = CONFIRM_TIMEOUT_SEC, .tv_usec = 0 };
	amqp_frame_t frame;

	for (;;) {
		amqp_maybe_release_buffers(conn);
		int status = amqp_simple_wait_frame_noblock(conn, &frame, &tv);
		if (status == AMQP_STATUS_TIMEOUT) {
			fprintf(stderr, "[publish] confirm: timed out waiting for broker ACK\n");
			return -1;
		}
		if (status != AMQP_STATUS_OK) {
			fprintf(stderr, "[publish] confirm: frame error: %s\n",
			        amqp_error_string2(status));
			return -1;
		}
		if (frame.frame_type != AMQP_FRAME_METHOD || frame.channel != 1)
			continue;

		if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)
			return 0;

		if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
			fprintf(stderr, "[publish] confirm: broker NACK'd the message\n");
			return -1;
		}
	}
}

int publish_message(const publish_config_t *cfg, const char *body, size_t body_len)
{
	int rc = -1;
	amqp_connection_state_t conn = amqp_new_connection();
	if (!conn) {
		fprintf(stderr, "[publish] amqp_new_connection failed\n");
		return -1;
	}

	amqp_socket_t *socket = amqp_tcp_socket_new(conn);
	if (!socket) {
		fprintf(stderr, "[publish] amqp_tcp_socket_new failed\n");
		goto out_destroy;
	}

	if (amqp_socket_open(socket, cfg->host, cfg->port)) {
		fprintf(stderr, "[publish] amqp_socket_open(%s:%d) failed\n",
		        cfg->host, cfg->port);
		goto out_destroy;
	}

	amqp_rpc_reply_t login = amqp_login(
		conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
		cfg->user, cfg->password);
	if (check_rpc(login, "login") != 0)
		goto out_destroy;

	amqp_channel_open(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "channel.open") != 0)
		goto out_close_conn;

	/* publisher confirms 활성화 */
	amqp_confirm_select(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "confirm.select") != 0)
		goto out_close_channel;

	amqp_exchange_declare(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes("direct"),
		0, 1, 0, 0, amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "exchange.declare") != 0)
		goto out_close_channel;

	amqp_queue_declare(conn, 1,
		amqp_cstring_bytes(cfg->queue),
		0, 1, 0, 0, amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "queue.declare") != 0)
		goto out_close_channel;

	amqp_queue_bind(conn, 1,
		amqp_cstring_bytes(cfg->queue),
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(cfg->routing_key),
		amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "queue.bind") != 0)
		goto out_close_channel;

	char msg_id[128];
	gen_message_id(msg_id, sizeof msg_id);

	amqp_basic_properties_t props;
	memset(&props, 0, sizeof props);
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG
	             | AMQP_BASIC_DELIVERY_MODE_FLAG
	             | AMQP_BASIC_MESSAGE_ID_FLAG;
	props.content_type = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2; /* persistent */
	props.message_id    = amqp_cstring_bytes(msg_id);

	amqp_bytes_t body_bytes = { .len = body_len, .bytes = (void *)body };

	int pub = amqp_basic_publish(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(cfg->routing_key),
		0, 0, &props, body_bytes);
	if (pub != AMQP_STATUS_OK) {
		fprintf(stderr, "[publish] basic.publish: %s\n",
		        amqp_error_string2(pub));
		goto out_close_channel;
	}

	/* broker ACK 대기 */
	if (wait_confirm(conn) != 0)
		goto out_close_channel;

	rc = 0;

out_close_channel:
	amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
out_close_conn:
	amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
out_destroy:
	amqp_destroy_connection(conn);
	return rc;
}
