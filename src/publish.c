/**
 * @file publish.c
 * @brief librabbitmq(rabbitmq-c) 기반 단일 메시지 발행 구현.
 *
 * AMQP RPC 호출은 세 가지 실패 경로를 가진다:
 *   - 라이브러리 예외(네트워크/프로토콜 오류)
 *   - 서버 예외(권한/정책 거부 등)
 *   - 응답 없음
 *
 * 라이프사이클:
 * @verbatim
 * amqp_new_connection
 *   → amqp_tcp_socket_new / amqp_socket_open
 *     → amqp_login
 *       → amqp_channel_open
 *         → amqp_exchange_declare / queue_declare / queue_bind
 *           → amqp_basic_publish
 *         → amqp_channel_close
 *       → amqp_connection_close
 *   → amqp_destroy_connection
 * @endverbatim
 * goto 라벨로 부분 실패 시에도 열려 있는 자원만 정확히 닫는다.
 */

#include "publish.h"

#include <amqp.h>
#include <amqp_framing.h>
#include <amqp_tcp_socket.h>

#include <stdio.h>
#include <string.h>

/**
 * @brief amqp_rpc_reply_t 판독 헬퍼.
 *
 * @param r   AMQP RPC 응답.
 * @param ctx 호출 위치 식별용 짧은 라벨(예: "login", "channel.open").
 * @return 0 정상, -1 실패. 실패 시 stderr에 이유 출력.
 */
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
		/* class/method ID만으로 대략적 원인 식별 가능.
		 * 세부 메시지 파싱은 필요 시 channel.close 프레임을 따라가야 함. */
		fprintf(stderr, "[publish] %s: server exception (class=%u method=%u)\n",
		        ctx, r.reply.id >> 16, r.reply.id & 0xFFFF);
		return -1;
	}
	return -1;
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

	/* vhost="/", channel_max=0(무제한), frame_max=131072, heartbeat=0.
	 * librabbitmq 공식 예제와 동일한 보수적 기본값. */
	amqp_rpc_reply_t login = amqp_login(
		conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
		cfg->user, cfg->password);
	if (check_rpc(login, "login") != 0)
		goto out_destroy;

	amqp_channel_open(conn, 1);
	if (check_rpc(amqp_get_rpc_reply(conn), "channel.open") != 0)
		goto out_close_conn;

	/* direct + durable: Python 프로토타입과 동일 큐 설계. */
	amqp_exchange_declare(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes("direct"),
		0, /* passive */ 1, /* durable */
		0, /* auto_delete */ 0, /* internal */
		amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "exchange.declare") != 0)
		goto out_close_channel;

	amqp_queue_declare(conn, 1,
		amqp_cstring_bytes(cfg->queue),
		0, /* passive */ 1, /* durable */
		0, /* exclusive */ 0, /* auto_delete */
		amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "queue.declare") != 0)
		goto out_close_channel;

	amqp_queue_bind(conn, 1,
		amqp_cstring_bytes(cfg->queue),
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(cfg->routing_key),
		amqp_empty_table);
	if (check_rpc(amqp_get_rpc_reply(conn), "queue.bind") != 0)
		goto out_close_channel;

	/* basic.properties: content_type으로 consumer 디코드 힌트,
	 * delivery_mode=2로 브로커 재시작 시에도 메시지 유지. */
	amqp_basic_properties_t props;
	memset(&props, 0, sizeof props);
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2; /* persistent */

	/* amqp_bytes_t는 non-const void*를 요구하지만 내부에서 읽기만 한다. */
	amqp_bytes_t body_bytes;
	body_bytes.len = body_len;
	body_bytes.bytes = (void *)body;

	int pub = amqp_basic_publish(conn, 1,
		amqp_cstring_bytes(cfg->exchange),
		amqp_cstring_bytes(cfg->routing_key),
		0, /* mandatory */ 0, /* immediate */
		&props, body_bytes);
	if (pub != AMQP_STATUS_OK) {
		fprintf(stderr, "[publish] basic.publish: %s\n",
		        amqp_error_string2(pub));
		goto out_close_channel;
	}

	rc = 0;

out_close_channel:
	amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS);
out_close_conn:
	amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
out_destroy:
	amqp_destroy_connection(conn);
	return rc;
}
