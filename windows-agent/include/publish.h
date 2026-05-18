/**
 * @file publish.h
 * @brief RabbitMQ publisher (Windows). Same wire contract as Linux agent.
 *
 * Topology contract:
 *   - Exchange : `assessment` (direct, durable)
 *   - Routing keys: `server.inventory`, `server.metrics`, `server.error`
 *   - Vhost   : `/` for local dev, `/assessment` in production
 *
 * v1에서는 worker가 빠지므로 publish_conn_* (long-lived) API 없음.
 * publish_message() 한 함수로 open-publish-close 패턴만 제공.
 */

#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>

typedef struct {
	const char *host;
	int         port;       /**< 5672 AMQP, 5671 AMQPS */
	const char *vhost;
	const char *user;
	const char *password;
	const char *exchange;

	int         heartbeat_sec;

	int         tls_enabled;
	const char *tls_ca_path;
	int         tls_verify_peer;
	int         tls_verify_hostname;
	const char *tls_cert_path;
	const char *tls_key_path;
} publish_config_t;

/**
 * @brief Publish a single JSON message to the broker.
 *
 * Internally: open socket → optional TLS → login → channel.open
 *             → confirm.select → exchange.declare(passive)
 *             → basic.publish(persistent, JSON) → wait broker ACK → close
 *
 * @return 0 on success, negative on failure. Diagnostic to stderr.
 */
int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t      body_len);

#endif
