/**
 * @file publish.h
 * @brief RabbitMQ publisher (direct exchange, durable, persistent).
 *
 * Topology contract:
 *   - Exchange : `assessment` (direct, durable)
 *   - Routing keys: `server.inventory`, `server.metrics`, `server.error`
 *   - Vhost   : `/` for local dev, `/assessment` in production
 *
 * The agent does not declare queues or bindings. The consumer
 * (`assessment-engine`) owns topology declaration via its `topology-admin`
 * user. The agent is `agent-publisher` with publish-only permission.
 */

#ifndef ASSESSMENT_AGENT_PUBLISH_H
#define ASSESSMENT_AGENT_PUBLISH_H

#include <stddef.h>

/**
 * @brief Broker connection settings.
 *
 * All string fields must outlive any @ref publish_message call that uses
 * them. The struct is plain-old-data and is not freed by this module.
 */
typedef struct {
	const char *host;       /**< Broker hostname or IP (internal network). */
	int         port;       /**< 5672 for AMQP, 5671 for AMQPS. */
	const char *vhost;      /**< AMQP vhost. `/` for dev, `/assessment` in prod. */
	const char *user;       /**< SASL PLAIN user (`agent-publisher` in prod). */
	const char *password;   /**< SASL PLAIN password. */
	const char *exchange;   /**< Direct exchange name (default `assessment`). */

	/**
	 * AMQP heartbeat interval in seconds.
	 * RabbitMQ recommends 60 (typical 10–60; 0 disables).
	 * https://www.rabbitmq.com/heartbeats.html
	 */
	int         heartbeat_sec;

	/* TLS (AMQPS). When @c tls_enabled is non-zero, port should be 5671. */
	int         tls_enabled;
	const char *tls_ca_path;     /**< Path to internal CA pem (required if TLS). */
	int         tls_verify_peer;
	int         tls_verify_hostname;
	const char *tls_cert_path;   /**< Optional mTLS client cert pem. */
	const char *tls_key_path;    /**< Optional mTLS client key pem. */
} publish_config_t;

/**
 * @brief Publish a single JSON message to the broker.
 *
 * Internally:
 *   open socket → optional TLS handshake → login → channel.open
 *     → confirm.select → exchange.declare(passive)
 *     → basic.publish(persistent, JSON) → wait broker ACK → close
 *
 * `exchange.declare` is called passively (only verifies existence). Queue
 * declaration is the consumer's responsibility.
 *
 * @param cfg         Connection settings.
 * @param routing_key One of `server.inventory` / `server.metrics` / `server.error`.
 * @param body        UTF-8 JSON serialized payload.
 * @param body_len    Byte length of @p body.
 * @return 0 on success, negative on failure. Diagnostic written to stderr.
 */
int publish_message(const publish_config_t *cfg,
                    const char *routing_key,
                    const char *body,
                    size_t      body_len);

#endif
